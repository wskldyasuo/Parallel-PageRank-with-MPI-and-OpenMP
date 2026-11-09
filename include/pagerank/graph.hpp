#ifndef DSA5208_PAGERANK_GRAPH_HPP
#define DSA5208_PAGERANK_GRAPH_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pagerank {

using Vertex = std::uint32_t;

struct CSRGraph {
    std::vector<std::uint64_t> incoming_offsets;
    std::vector<Vertex> incoming_sources;
    std::vector<Vertex> out_degree;
    std::vector<std::int64_t> original_ids;

    std::size_t vertex_count() const { return out_degree.size(); }
    std::size_t edge_count() const { return incoming_sources.size(); }
};

struct GraphLoadResult {
    CSRGraph graph;
    std::uint64_t declared_vertices = 0;
    std::uint64_t declared_edges = 0;
    std::uint64_t raw_edges = 0;
    std::uint64_t duplicate_edges = 0;
    bool used_dense_id_mapping = false;
    double load_seconds = 0.0;
};

// Build an in-neighbor CSR graph. Duplicate directed edges are removed and
// self-loops are retained.
CSRGraph make_csr_graph(
    std::size_t vertex_count,
    const std::vector<std::pair<Vertex, Vertex>>& directed_edges,
    std::vector<std::int64_t> original_ids = {});

// Read a SNAP two-column directed edge list. Missing isolated vertices are
// reconstructed only when the header declares a dense zero-based ID range.
GraphLoadResult load_snap_graph(const std::string& path);

// Portable across the little-endian x86-64 and ARM64 systems used by this
// project. The file stores fixed-width arrays rather than C++ object layouts.
GraphLoadResult load_csr_graph(const std::string& path);
void save_csr_graph(const CSRGraph& graph, const std::string& path);

}  // namespace pagerank

#endif

