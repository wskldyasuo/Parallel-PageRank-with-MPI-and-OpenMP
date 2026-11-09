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
    std::string input;
    std::string output;
    std::uint64_t denominator = 0;
    std::uint64_t seed = 5208;
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
            options.input = require_value(i, argc, argv);
        } else if (argument == "--output") {
            options.output = require_value(i, argc, argv);
        } else if (argument == "--denominator") {
            options.denominator = std::stoull(require_value(i, argc, argv));
        } else if (argument == "--seed") {
            options.seed = std::stoull(require_value(i, argc, argv));
        } else {
            throw std::invalid_argument("Unknown argument: " + argument);
        }
    }
    if (options.input.empty() || options.output.empty() || options.denominator == 0) {
        throw std::invalid_argument(
            "Usage: sample_induced_csr --input INPUT.prcsr --denominator POWER_OF_TWO "
            "--seed N --output OUTPUT.prcsr");
    }
    if ((options.denominator & (options.denominator - 1)) != 0) {
        throw std::invalid_argument("--denominator must be a power of two");
    }
    return options;
}

std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

bool selected(pagerank::Vertex vertex, std::uint64_t denominator, std::uint64_t seed) {
    if (denominator == 1) {
        return true;
    }
    // Power-of-two denominators make the samples exactly nested:
    // h % 32 == 0 implies h % 16 == 0, and so on.
    return (splitmix64(static_cast<std::uint64_t>(vertex) ^ seed) &
            (denominator - 1)) == 0;
}

pagerank::CSRGraph induced_subgraph(const pagerank::CSRGraph& input,
                                    std::uint64_t denominator,
                                    std::uint64_t seed) {
    const auto missing = std::numeric_limits<pagerank::Vertex>::max();
    std::vector<pagerank::Vertex> new_index(input.vertex_count(), missing);
    std::size_t selected_vertices = 0;
    for (std::size_t old = 0; old < input.vertex_count(); ++old) {
        if (selected(static_cast<pagerank::Vertex>(old), denominator, seed)) {
            if (selected_vertices >= missing) {
                throw std::runtime_error("Sample has too many vertices for uint32 IDs");
            }
            new_index[old] = static_cast<pagerank::Vertex>(selected_vertices++);
        }
    }

    pagerank::CSRGraph output;
    output.incoming_offsets.reserve(selected_vertices + 1);
    output.out_degree.assign(selected_vertices, 0);
    output.original_ids.reserve(selected_vertices);
    output.incoming_offsets.push_back(0);
    const std::uint64_t estimated_edges =
        static_cast<std::uint64_t>(input.edge_count()) / denominator / denominator;
    output.incoming_sources.reserve(static_cast<std::size_t>(estimated_edges * 11 / 10 + 1));

    for (std::size_t old_target = 0; old_target < input.vertex_count(); ++old_target) {
        if (new_index[old_target] == missing) {
            continue;
        }
        output.original_ids.push_back(input.original_ids[old_target]);
        const std::uint64_t begin = input.incoming_offsets[old_target];
        const std::uint64_t end = input.incoming_offsets[old_target + 1];
        for (std::uint64_t edge = begin; edge < end; ++edge) {
            const pagerank::Vertex old_source = input.incoming_sources[edge];
            const pagerank::Vertex new_source = new_index[old_source];
            if (new_source != missing) {
                output.incoming_sources.push_back(new_source);
                ++output.out_degree[new_source];
            }
        }
        output.incoming_offsets.push_back(output.incoming_sources.size());
    }
    return output;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_arguments(argc, argv);
        const auto start = std::chrono::steady_clock::now();
        const pagerank::CSRGraph input = pagerank::load_csr_graph(options.input).graph;
        const auto sample_start = std::chrono::steady_clock::now();
        const pagerank::CSRGraph output =
            induced_subgraph(input, options.denominator, options.seed);
        const double sample_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - sample_start).count();
        pagerank::save_csr_graph(output, options.output);
        const double total_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();

        std::size_t dangling = 0;
        for (pagerank::Vertex degree : output.out_degree) {
            dangling += degree == 0;
        }
        std::ofstream metadata(options.output + ".json");
        metadata << std::setprecision(17)
                 << "{\n"
                 << "  \"format\": \"PRCSR001\",\n"
                 << "  \"method\": \"nested-uniform-vertex-induced\",\n"
                 << "  \"input\": \"" << options.input << "\",\n"
                 << "  \"seed\": " << options.seed << ",\n"
                 << "  \"vertex_fraction\": \"1/" << options.denominator << "\",\n"
                 << "  \"vertices\": " << output.vertex_count() << ",\n"
                 << "  \"edges\": " << output.edge_count() << ",\n"
                 << "  \"dangling_vertices\": " << dangling << ",\n"
                 << "  \"file_bytes\": " << std::filesystem::file_size(options.output) << ",\n"
                 << "  \"sample_seconds\": " << sample_seconds << ",\n"
                 << "  \"total_seconds\": " << total_seconds << "\n"
                 << "}\n";

        std::cout << std::setprecision(12)
                  << "fraction         = 1/" << options.denominator << '\n'
                  << "seed             = " << options.seed << '\n'
                  << "vertices         = " << output.vertex_count() << '\n'
                  << "edges            = " << output.edge_count() << '\n'
                  << "dangling         = " << dangling << '\n'
                  << "sample seconds   = " << sample_seconds << '\n'
                  << "total seconds    = " << total_seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Induced sampler error: " << error.what() << '\n';
        return 1;
    }
}
