#include "pagerank/graph.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace pagerank {
namespace {

constexpr char kBinaryMagic[8] = {'P', 'R', 'C', 'S', 'R', '0', '0', '1'};
constexpr std::uint32_t kBinaryVersion = 1;

struct RawEdge {
    std::int64_t source;
    std::int64_t target;
};

bool parse_count(const std::string& line, const std::string& label,
                 std::uint64_t& value) {
    const std::size_t position = line.find(label);
    if (position == std::string::npos) {
        return false;
    }
    const char* first = line.data() + position + label.size();
    const char* last = line.data() + line.size();
    while (first != last && (*first == ' ' || *first == '\t')) {
        ++first;
    }
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc();
}

bool parse_edge(const std::string& line, RawEdge& edge) {
    const char* cursor = line.data();
    const char* last = line.data() + line.size();
    while (cursor != last && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }
    if (cursor == last || *cursor == '#') {
        return false;
    }

    auto source_result = std::from_chars(cursor, last, edge.source);
    if (source_result.ec != std::errc()) {
        throw std::runtime_error("Invalid source vertex in edge-list line: " + line);
    }
    cursor = source_result.ptr;
    while (cursor != last && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }
    auto target_result = std::from_chars(cursor, last, edge.target);
    if (target_result.ec != std::errc()) {
        throw std::runtime_error("Invalid target vertex in edge-list line: " + line);
    }
    return true;
}

std::uint64_t edge_key(Vertex source, Vertex target) {
    // Target occupies the high bits so sorted keys are already in incoming-CSR order.
    return (static_cast<std::uint64_t>(target) << 32U) | source;
}

template <typename T>
void write_value(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!output) {
        throw std::runtime_error("Failed while writing binary CSR file.");
    }
}

template <typename T>
void write_vector(std::ofstream& output, const std::vector<T>& values) {
    if (!values.empty()) {
        output.write(reinterpret_cast<const char*>(values.data()),
                     static_cast<std::streamsize>(values.size() * sizeof(T)));
    }
    if (!output) {
        throw std::runtime_error("Failed while writing binary CSR array.");
    }
}

template <typename T>
void read_value(std::ifstream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!input) {
        throw std::runtime_error("Truncated binary CSR header.");
    }
}

template <typename T>
void read_vector(std::ifstream& input, std::vector<T>& values) {
    if (!values.empty()) {
        input.read(reinterpret_cast<char*>(values.data()),
                   static_cast<std::streamsize>(values.size() * sizeof(T)));
    }
    if (!input) {
        throw std::runtime_error("Truncated binary CSR array.");
    }
}

}  // namespace

CSRGraph make_csr_graph(
    std::size_t vertex_count,
    const std::vector<std::pair<Vertex, Vertex>>& directed_edges,
    std::vector<std::int64_t> original_ids) {
    if (vertex_count == 0 ||
        vertex_count > static_cast<std::size_t>(std::numeric_limits<Vertex>::max())) {
        throw std::invalid_argument("The graph must contain 1..UINT32_MAX vertices.");
    }
    if (original_ids.empty()) {
        original_ids.resize(vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) {
            original_ids[i] = static_cast<std::int64_t>(i);
        }
    }
    if (original_ids.size() != vertex_count) {
        throw std::invalid_argument("The original-ID vector must match the graph size.");
    }

    std::vector<std::uint64_t> keys;
    keys.reserve(directed_edges.size());
    for (const auto& edge : directed_edges) {
        if (edge.first >= vertex_count || edge.second >= vertex_count) {
            throw std::invalid_argument("An edge endpoint is outside the graph.");
        }
        keys.push_back(edge_key(edge.first, edge.second));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    CSRGraph graph;
    graph.incoming_offsets.assign(vertex_count + 1, 0);
    graph.incoming_sources.resize(keys.size());
    graph.out_degree.assign(vertex_count, 0);
    graph.original_ids = std::move(original_ids);

    for (const std::uint64_t key : keys) {
        const Vertex source = static_cast<Vertex>(key & 0xffffffffULL);
        const Vertex target = static_cast<Vertex>(key >> 32U);
        ++graph.incoming_offsets[static_cast<std::size_t>(target) + 1];
        ++graph.out_degree[source];
    }
    for (std::size_t i = 1; i < graph.incoming_offsets.size(); ++i) {
        graph.incoming_offsets[i] += graph.incoming_offsets[i - 1];
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        graph.incoming_sources[i] =
            static_cast<Vertex>(keys[i] & 0xffffffffULL);
    }
    return graph;
}

GraphLoadResult load_snap_graph(const std::string& path) {
    const auto start = std::chrono::steady_clock::now();
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open graph file: " + path);
    }

    std::uint64_t declared_vertices = 0;
    std::uint64_t declared_edges = 0;
    std::vector<RawEdge> raw_edges;
    std::vector<std::int64_t> vertex_ids;
    std::string line;

    while (std::getline(input, line)) {
        if (!line.empty() && line[0] == '#') {
            parse_count(line, "Nodes:", declared_vertices);
            parse_count(line, "Edges:", declared_edges);
            if (declared_edges > 0 && raw_edges.capacity() == 0) {
                raw_edges.reserve(static_cast<std::size_t>(declared_edges));
                vertex_ids.reserve(static_cast<std::size_t>(declared_edges));
            }
            continue;
        }
        RawEdge edge{};
        if (parse_edge(line, edge)) {
            raw_edges.push_back(edge);
            vertex_ids.push_back(edge.source);
            vertex_ids.push_back(edge.target);
        }
    }
    if (raw_edges.empty()) {
        throw std::runtime_error("The graph file contains no edges: " + path);
    }
    if (declared_edges != 0 && raw_edges.size() != declared_edges) {
        throw std::runtime_error("SNAP header edge count does not match the file contents.");
    }

    std::sort(vertex_ids.begin(), vertex_ids.end());
    vertex_ids.erase(std::unique(vertex_ids.begin(), vertex_ids.end()), vertex_ids.end());
    if (declared_vertices != 0 && vertex_ids.size() != declared_vertices) {
        const bool declared_dense_zero_based =
            declared_vertices <= std::numeric_limits<Vertex>::max() &&
            declared_vertices <= std::numeric_limits<std::size_t>::max() &&
            !vertex_ids.empty() && vertex_ids.front() >= 0 &&
            static_cast<std::uint64_t>(vertex_ids.back()) < declared_vertices;
        if (!declared_dense_zero_based) {
            throw std::runtime_error(
                "The declared node count differs from the IDs present in the edge list, "
                "and the missing IDs cannot be inferred as a dense zero-based range.");
        }
        vertex_ids.resize(static_cast<std::size_t>(declared_vertices));
        for (std::size_t i = 0; i < vertex_ids.size(); ++i) {
            vertex_ids[i] = static_cast<std::int64_t>(i);
        }
    }
    if (vertex_ids.size() > std::numeric_limits<Vertex>::max()) {
        throw std::runtime_error("The graph has too many vertices for 32-bit CSR indices.");
    }

    bool dense_ids = true;
    const std::int64_t first_id = vertex_ids.front();
    for (std::size_t i = 0; i < vertex_ids.size(); ++i) {
        if (vertex_ids[i] != first_id + static_cast<std::int64_t>(i)) {
            dense_ids = false;
            break;
        }
    }

    std::unordered_map<std::int64_t, Vertex> id_map;
    if (!dense_ids) {
        id_map.reserve(vertex_ids.size());
        for (std::size_t i = 0; i < vertex_ids.size(); ++i) {
            id_map.emplace(vertex_ids[i], static_cast<Vertex>(i));
        }
    }
    auto mapped_id = [&](std::int64_t original) -> Vertex {
        if (dense_ids) {
            return static_cast<Vertex>(original - first_id);
        }
        return id_map.at(original);
    };

    std::vector<std::pair<Vertex, Vertex>> mapped_edges;
    mapped_edges.reserve(raw_edges.size());
    for (const RawEdge& edge : raw_edges) {
        mapped_edges.emplace_back(mapped_id(edge.source), mapped_id(edge.target));
    }

    const std::uint64_t raw_edge_count = raw_edges.size();
    raw_edges.clear();
    raw_edges.shrink_to_fit();
    // Function-argument evaluation order is not fixed here: GCC may move
    // vertex_ids before evaluating vertex_ids.size(), leaving a zero size.
    const std::size_t vertex_count = vertex_ids.size();
    CSRGraph graph =
        make_csr_graph(vertex_count, mapped_edges, std::move(vertex_ids));

    GraphLoadResult result;
    result.declared_vertices = declared_vertices;
    result.declared_edges = declared_edges;
    result.raw_edges = raw_edge_count;
    result.duplicate_edges = raw_edge_count - graph.edge_count();
    result.used_dense_id_mapping = dense_ids;
    result.graph = std::move(graph);
    result.load_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

void save_csr_graph(const CSRGraph& graph, const std::string& path) {
    if (graph.vertex_count() == 0 ||
        graph.incoming_offsets.size() != graph.vertex_count() + 1 ||
        graph.original_ids.size() != graph.vertex_count() ||
        graph.incoming_offsets.front() != 0 ||
        graph.incoming_offsets.back() != graph.edge_count()) {
        throw std::invalid_argument("Cannot save an inconsistent CSR graph.");
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Cannot create binary CSR file: " + path);
    }
    output.write(kBinaryMagic, sizeof(kBinaryMagic));
    const std::uint32_t vertex_width = sizeof(Vertex);
    const std::uint32_t offset_width = sizeof(std::uint64_t);
    const std::uint32_t reserved = 0;
    const std::uint64_t vertices = graph.vertex_count();
    const std::uint64_t edges = graph.edge_count();
    write_value(output, kBinaryVersion);
    write_value(output, vertex_width);
    write_value(output, offset_width);
    write_value(output, reserved);
    write_value(output, vertices);
    write_value(output, edges);
    write_vector(output, graph.incoming_offsets);
    write_vector(output, graph.incoming_sources);
    write_vector(output, graph.out_degree);
    write_vector(output, graph.original_ids);
}

GraphLoadResult load_csr_graph(const std::string& path) {
    const auto start = std::chrono::steady_clock::now();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open binary CSR file: " + path);
    }
    char magic[sizeof(kBinaryMagic)]{};
    input.read(magic, sizeof(magic));
    if (!input || !std::equal(std::begin(magic), std::end(magic),
                              std::begin(kBinaryMagic))) {
        throw std::runtime_error("Invalid binary CSR magic value.");
    }
    std::uint32_t version = 0;
    std::uint32_t vertex_width = 0;
    std::uint32_t offset_width = 0;
    std::uint32_t reserved = 0;
    std::uint64_t vertices = 0;
    std::uint64_t edges = 0;
    read_value(input, version);
    read_value(input, vertex_width);
    read_value(input, offset_width);
    read_value(input, reserved);
    read_value(input, vertices);
    read_value(input, edges);
    if (version != kBinaryVersion || vertex_width != sizeof(Vertex) ||
        offset_width != sizeof(std::uint64_t) || reserved != 0 || vertices == 0 ||
        vertices > std::numeric_limits<Vertex>::max()) {
        throw std::runtime_error("Unsupported or invalid binary CSR header.");
    }
    if (vertices > std::numeric_limits<std::size_t>::max() - 1 ||
        edges > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("Binary CSR graph is too large for this system.");
    }

    CSRGraph graph;
    graph.incoming_offsets.resize(static_cast<std::size_t>(vertices) + 1);
    graph.incoming_sources.resize(static_cast<std::size_t>(edges));
    graph.out_degree.resize(static_cast<std::size_t>(vertices));
    graph.original_ids.resize(static_cast<std::size_t>(vertices));
    read_vector(input, graph.incoming_offsets);
    read_vector(input, graph.incoming_sources);
    read_vector(input, graph.out_degree);
    read_vector(input, graph.original_ids);

    if (graph.incoming_offsets.front() != 0 ||
        graph.incoming_offsets.back() != edges ||
        !std::is_sorted(graph.incoming_offsets.begin(), graph.incoming_offsets.end())) {
        throw std::runtime_error("Invalid offsets in binary CSR graph.");
    }
    for (Vertex source : graph.incoming_sources) {
        if (source >= vertices) {
            throw std::runtime_error("Invalid source index in binary CSR graph.");
        }
    }
    const std::uint64_t out_degree_sum = std::accumulate(
        graph.out_degree.begin(), graph.out_degree.end(), std::uint64_t{0});
    if (out_degree_sum != edges) {
        throw std::runtime_error("Out-degree sum does not match binary CSR edge count.");
    }

    GraphLoadResult result;
    result.declared_vertices = vertices;
    result.declared_edges = edges;
    result.raw_edges = edges;
    result.used_dense_id_mapping = true;
    result.graph = std::move(graph);
    result.load_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

}  // namespace pagerank

