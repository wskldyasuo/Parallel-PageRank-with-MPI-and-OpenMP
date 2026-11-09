#ifndef DSA5208_PAGERANK_PAGERANK_HPP
#define DSA5208_PAGERANK_PAGERANK_HPP

#include "pagerank/graph.hpp"

#include <cstddef>
#include <vector>

namespace pagerank {

struct PageRankOptions {
    double alpha = 0.85;
    double tolerance = 1e-12;
    std::size_t max_iterations = 1000;
};

struct PageRankResult {
    std::vector<double> ranks;
    std::size_t iterations = 0;
    double last_change = 0.0;
    double solve_seconds = 0.0;
    double solve_mean_seconds = 0.0;
    double update_max_seconds = 0.0;
    double update_mean_seconds = 0.0;
    double compute_max_seconds = 0.0;
    double compute_mean_seconds = 0.0;
    double allreduce_max_seconds = 0.0;
    double allreduce_mean_seconds = 0.0;
    double allgatherv_max_seconds = 0.0;
    double allgatherv_mean_seconds = 0.0;
    double score_exchange_max_seconds = 0.0;
    double score_exchange_mean_seconds = 0.0;
    double score_pack_max_seconds = 0.0;
    double score_pack_mean_seconds = 0.0;
    int local_begin = 0;
    bool ranks_are_local = false;
    bool converged = false;
};

PageRankResult run_serial(const CSRGraph& graph, const PageRankOptions& options);
double one_step_residual(const CSRGraph& graph,
                         const std::vector<double>& ranks,
                         double alpha);

}  // namespace pagerank

#endif

