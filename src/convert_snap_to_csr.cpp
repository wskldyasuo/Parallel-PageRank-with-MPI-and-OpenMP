#include "pagerank/graph.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Header {
    std::uint64_t vertices = 0;
    std::uint64_t edges = 0;
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

bool parse_edge(const std::string& line, std::uint32_t& source,
                std::uint32_t& target) {
    const char* cursor = line.data();
    const char* last = line.data() + line.size();
    while (cursor != last && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }
    if (cursor == last || *cursor == '#') {
        return false;
    }
    std::uint64_t source64 = 0;
    std::uint64_t target64 = 0;
    auto parsed = std::from_chars(cursor, last, source64);
    if (parsed.ec != std::errc()) {
        throw std::runtime_error("Invalid source in line: " + line);
    }
    cursor = parsed.ptr;
    while (cursor != last && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }
    parsed = std::from_chars(cursor, last, target64);
    if (parsed.ec != std::errc() ||
        source64 > std::numeric_limits<std::uint32_t>::max() ||
        target64 > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Invalid target in line: " + line);
    }
    source = static_cast<std::uint32_t>(source64);
    target = static_cast<std::uint32_t>(target64);
    return true;
}

Header read_header(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open input: " + path);
    }
    Header header;
    std::string line;
    while (std::getline(input, line) && !line.empty() && line[0] == '#') {
        parse_count(line, "Nodes:", header.vertices);
        parse_count(line, "Edges:", header.edges);
    }
    if (header.vertices == 0 ||
        header.vertices > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "A dense zero-based '# Nodes:' header is required.");
    }
    return header;
}

template <typename Consume>
std::uint64_t visit_edges(const std::string& path, std::uint64_t vertices,
                          Consume consume) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open input: " + path);
    }
    std::string line;
    std::uint64_t count = 0;
    std::uint64_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        std::uint32_t source = 0;
        std::uint32_t target = 0;
        if (!parse_edge(line, source, target)) {
            continue;
        }
        if (source >= vertices || target >= vertices) {
            throw std::runtime_error("Endpoint outside dense range at line " +
                                     std::to_string(line_number));
        }
        consume(source, target);
        ++count;
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0] << " INPUT.txt OUTPUT.prcsr\n";
            return 1;
        }
        const std::string input_path = argv[1];
        const std::string output_path = argv[2];
        const auto total_start = std::chrono::steady_clock::now();
        const Header header = read_header(input_path);
        const std::size_t n = static_cast<std::size_t>(header.vertices);

        std::vector<std::uint64_t> indegrees(n, 0);
        std::vector<pagerank::Vertex> out_degree(n, 0);
        const auto count_start = std::chrono::steady_clock::now();
        const std::uint64_t raw_edges = visit_edges(
            input_path, header.vertices,
            [&](std::uint32_t source, std::uint32_t target) {
                ++indegrees[target];
                if (out_degree[source] ==
                    std::numeric_limits<pagerank::Vertex>::max()) {
                    throw std::runtime_error("A vertex out-degree exceeds UINT32_MAX.");
                }
                ++out_degree[source];
            });
        if (header.edges != 0 && raw_edges != header.edges) {
            throw std::runtime_error("Header edge count does not match input.");
        }

        pagerank::CSRGraph graph;
        graph.incoming_offsets.resize(n + 1, 0);
        for (std::size_t vertex = 0; vertex < n; ++vertex) {
            graph.incoming_offsets[vertex + 1] =
                graph.incoming_offsets[vertex] + indegrees[vertex];
        }
        graph.incoming_sources.resize(static_cast<std::size_t>(raw_edges));
        std::vector<std::uint64_t> cursor(graph.incoming_offsets.begin(),
                                          graph.incoming_offsets.end() - 1);
        const auto fill_start = std::chrono::steady_clock::now();
        visit_edges(input_path, header.vertices,
                    [&](std::uint32_t source, std::uint32_t target) {
                        graph.incoming_sources[cursor[target]++] = source;
                    });
        cursor.clear();
        cursor.shrink_to_fit();
        indegrees.clear();
        indegrees.shrink_to_fit();

        const auto deduplicate_start = std::chrono::steady_clock::now();
        std::fill(out_degree.begin(), out_degree.end(), 0);
        std::vector<std::uint64_t> compact_offsets(n + 1, 0);
        std::size_t write = 0;
        for (std::size_t target = 0; target < n; ++target) {
            auto first = graph.incoming_sources.begin() +
                         static_cast<std::ptrdiff_t>(graph.incoming_offsets[target]);
            auto last = graph.incoming_sources.begin() +
                        static_cast<std::ptrdiff_t>(graph.incoming_offsets[target + 1]);
            std::sort(first, last);
            const auto unique_last = std::unique(first, last);
            for (auto source = first; source != unique_last; ++source) {
                graph.incoming_sources[write++] = *source;
                ++out_degree[*source];
            }
            compact_offsets[target + 1] = write;
        }
        graph.incoming_sources.resize(write);
        graph.incoming_sources.shrink_to_fit();
        graph.incoming_offsets = std::move(compact_offsets);
        graph.out_degree = std::move(out_degree);
        graph.original_ids.resize(n);
        for (std::size_t vertex = 0; vertex < n; ++vertex) {
            graph.original_ids[vertex] = static_cast<std::int64_t>(vertex);
        }

        const auto write_start = std::chrono::steady_clock::now();
        pagerank::save_csr_graph(graph, output_path);
        const auto finish = std::chrono::steady_clock::now();
        auto seconds = [](auto begin, auto end) {
            return std::chrono::duration<double>(end - begin).count();
        };
        std::cout << "vertices             = " << graph.vertex_count() << '\n'
                  << "raw edges           = " << raw_edges << '\n'
                  << "unique edges        = " << graph.edge_count() << '\n'
                  << "duplicate edges     = " << raw_edges - graph.edge_count() << '\n'
                  << "count seconds       = " << seconds(count_start, fill_start)
                  << '\n'
                  << "fill seconds        = "
                  << seconds(fill_start, deduplicate_start) << '\n'
                  << "deduplicate seconds = "
                  << seconds(deduplicate_start, write_start) << '\n'
                  << "write seconds       = " << seconds(write_start, finish) << '\n'
                  << "total seconds       = " << seconds(total_start, finish) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Conversion error: " << error.what() << '\n';
        return 1;
    }
}
