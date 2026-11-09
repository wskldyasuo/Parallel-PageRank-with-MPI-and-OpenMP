#include "pagerank/graph.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::vector<std::string> inputs;
    std::string output;
    std::size_t replicas = 1;
};

std::string require_value(int& index, int argc, char** argv) {
    if (++index >= argc) {
        throw std::invalid_argument("Missing value after " + std::string(argv[index - 1]));
    }
    return argv[index];
}

Options parse_arguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--input") {
            options.inputs.push_back(require_value(i, argc, argv));
        } else if (argument == "--output") {
            options.output = require_value(i, argc, argv);
        } else if (argument == "--replicas") {
            options.replicas = std::stoull(require_value(i, argc, argv));
        } else {
            throw std::invalid_argument("Unknown argument: " + argument);
        }
    }
    if (options.inputs.empty() || options.output.empty() || options.replicas == 0) {
        throw std::invalid_argument(
            "Usage: generate_web_graph --input FILE [--input FILE ...] "
            "--replicas N --output FILE.prcsr");
    }
    return options;
}

pagerank::CSRGraph replicate_union(const std::vector<pagerank::CSRGraph>& components,
                                   std::size_t replicas) {
    std::uint64_t base_vertices = 0;
    std::uint64_t base_edges = 0;
    for (const auto& graph : components) {
        base_vertices += graph.vertex_count();
        base_edges += graph.edge_count();
    }
    const std::uint64_t vertices = base_vertices * replicas;
    const std::uint64_t edges = base_edges * replicas;
    if (vertices > std::numeric_limits<pagerank::Vertex>::max() ||
        vertices > std::numeric_limits<std::size_t>::max() - 1 ||
        edges > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("Requested synthetic graph is too large.");
    }

    pagerank::CSRGraph output;
    output.incoming_offsets.resize(static_cast<std::size_t>(vertices) + 1);
    output.incoming_sources.resize(static_cast<std::size_t>(edges));
    output.out_degree.resize(static_cast<std::size_t>(vertices));
    output.original_ids.resize(static_cast<std::size_t>(vertices));
    output.incoming_offsets[0] = 0;

    std::uint64_t vertex_offset = 0;
    std::uint64_t edge_offset = 0;
    for (std::size_t replica = 0; replica < replicas; ++replica) {
        for (const auto& graph : components) {
            for (std::size_t local = 0; local < graph.vertex_count(); ++local) {
                const std::uint64_t global = vertex_offset + local;
                output.out_degree[global] = graph.out_degree[local];
                output.original_ids[global] = static_cast<std::int64_t>(global);
                const std::uint64_t begin = graph.incoming_offsets[local];
                const std::uint64_t end = graph.incoming_offsets[local + 1];
                for (std::uint64_t edge = begin; edge < end; ++edge) {
                    output.incoming_sources[edge_offset++] =
                        static_cast<pagerank::Vertex>(
                            vertex_offset + graph.incoming_sources[edge]);
                }
                output.incoming_offsets[global + 1] = edge_offset;
            }
            vertex_offset += graph.vertex_count();
        }
    }
    if (vertex_offset != vertices || edge_offset != edges) {
        throw std::runtime_error("Internal error while assembling replicated graph.");
    }
    return output;
}

std::uint64_t file_size(const std::string& path) {
    return std::filesystem::file_size(path);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_arguments(argc, argv);
        const auto total_start = std::chrono::steady_clock::now();
        std::vector<pagerank::CSRGraph> components;
        components.reserve(options.inputs.size());

        const auto load_start = std::chrono::steady_clock::now();
        for (const std::string& input : options.inputs) {
            std::cout << "Loading " << input << "\n";
            const bool is_csr = std::filesystem::path(input).extension() == ".prcsr";
            components.push_back(is_csr ? pagerank::load_csr_graph(input).graph
                                        : pagerank::load_snap_graph(input).graph);
        }
        const double load_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - load_start).count();

        const auto assembly_start = std::chrono::steady_clock::now();
        pagerank::CSRGraph graph = replicate_union(components, options.replicas);
        const double assembly_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - assembly_start).count();

        const auto write_start = std::chrono::steady_clock::now();
        pagerank::save_csr_graph(graph, options.output);
        const double write_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - write_start).count();
        const double total_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - total_start).count();

        const std::string metadata_path = options.output + ".json";
        std::ofstream metadata(metadata_path);
        metadata << std::setprecision(17)
                 << "{\n"
                 << "  \"format\": \"PRCSR001\",\n"
                 << "  \"replicas\": " << options.replicas << ",\n"
                 << "  \"components\": " << options.inputs.size() << ",\n"
                 << "  \"inputs\": [\n";
        for (std::size_t i = 0; i < options.inputs.size(); ++i) {
            metadata << "    \"" << options.inputs[i] << "\""
                     << (i + 1 == options.inputs.size() ? "\n" : ",\n");
        }
        metadata << "  ],\n"
                 << "  \"vertices\": " << graph.vertex_count() << ",\n"
                 << "  \"edges\": " << graph.edge_count() << ",\n"
                 << "  \"file_bytes\": " << file_size(options.output) << ",\n"
                 << "  \"input_load_seconds\": " << load_seconds << ",\n"
                 << "  \"assembly_seconds\": " << assembly_seconds << ",\n"
                 << "  \"write_seconds\": " << write_seconds << ",\n"
                 << "  \"total_seconds\": " << total_seconds << "\n"
                 << "}\n";

        std::cout << std::setprecision(12)
                  << "vertices        = " << graph.vertex_count() << '\n'
                  << "edges           = " << graph.edge_count() << '\n'
                  << "file bytes      = " << file_size(options.output) << '\n'
                  << "input load sec  = " << load_seconds << '\n'
                  << "assembly sec    = " << assembly_seconds << '\n'
                  << "write sec       = " << write_seconds << '\n'
                  << "total sec       = " << total_seconds << '\n'
                  << "metadata        = " << metadata_path << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Generator error: " << error.what() << '\n';
        return 1;
    }
}

