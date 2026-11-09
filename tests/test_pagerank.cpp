#include "pagerank/graph.hpp"
#include "pagerank/pagerank.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace {

int passed = 0;
int failed = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        ++passed;
        std::cout << "[PASS] " << name << '\n';
    } else {
        ++failed;
        std::cout << "[FAIL] " << name << '\n';
    }
}

bool near(double left, double right, double tolerance = 1e-10) {
    return std::fabs(left - right) <= tolerance;
}

}  // namespace

int main() {
    using pagerank::Vertex;
    const pagerank::PageRankOptions options{};

    const std::vector<std::pair<Vertex, Vertex>> four_vertex_edges = {
        {0, 1}, {0, 2}, {1, 2}, {2, 0}};
    const pagerank::CSRGraph four_vertex_graph =
        pagerank::make_csr_graph(4, four_vertex_edges);
    const pagerank::PageRankResult four_vertex =
        pagerank::run_serial(four_vertex_graph, options);

    check(four_vertex.converged, "four-vertex graph converges");
    check(four_vertex.iterations == 53, "four-vertex iteration count");
    check(near(four_vertex.ranks[0], 0.369323534954), "rank[0]");
    check(near(four_vertex.ranks[1], 0.204581549974), "rank[1]");
    check(near(four_vertex.ranks[2], 0.378475867453), "rank[2]");
    check(near(four_vertex.ranks[3], 0.047619047619), "isolated rank[3]");
    check(near(std::accumulate(four_vertex.ranks.begin(),
                               four_vertex.ranks.end(), 0.0), 1.0, 1e-12),
          "rank sum is one");
    check(pagerank::one_step_residual(four_vertex_graph, four_vertex.ranks, 0.85)
              <= 1e-12,
          "four-vertex residual");

    const pagerank::CSRGraph all_dangling = pagerank::make_csr_graph(5, {});
    const pagerank::PageRankResult uniform =
        pagerank::run_serial(all_dangling, options);
    check(uniform.converged, "all-dangling graph converges");
    check(uniform.iterations == 1, "all-dangling graph stops after one iteration");
    bool all_uniform = true;
    for (double rank : uniform.ranks) {
        all_uniform = all_uniform && near(rank, 0.2, 1e-15);
    }
    check(all_uniform, "all-dangling graph remains uniform");

    const pagerank::CSRGraph self_loop =
        pagerank::make_csr_graph(1, {{0, 0}, {0, 0}});
    check(self_loop.edge_count() == 1, "duplicate edge is removed");
    const pagerank::PageRankResult singleton =
        pagerank::run_serial(self_loop, options);
    check(singleton.converged && near(singleton.ranks[0], 1.0, 1e-15),
          "single self-loop has rank one");

    const pagerank::GraphLoadResult with_isolated =
        pagerank::load_snap_graph("tests/data/isolated_header_graph.txt");
    check(with_isolated.graph.vertex_count() == 4,
          "declared dense isolated vertices are reconstructed");
    check(with_isolated.graph.edge_count() == 1,
          "isolated-vertex input preserves its edge");
    check(with_isolated.graph.original_ids == std::vector<std::int64_t>({0, 1, 2, 3}),
          "reconstructed vertex IDs are dense and zero-based");

    std::cout << "\nPassed: " << passed << "\nFailed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}

