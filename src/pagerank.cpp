#include "pagerank/pagerank.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace pagerank {
namespace {

double update_once(const CSRGraph& graph, const std::vector<double>& current,
                   double alpha, std::vector<double>& next) {
    const std::size_t n = graph.vertex_count();
    double dangling_mass = 0.0;
    for (std::size_t source = 0; source < n; ++source) {
        if (graph.out_degree[source] == 0) {
            dangling_mass += current[source];
        }
    }

    const double base = (1.0 - alpha + alpha * dangling_mass) /
                        static_cast<double>(n);
    double difference = 0.0;
    for (std::size_t target = 0; target < n; ++target) {
        double value = base;
        const std::uint64_t begin = graph.incoming_offsets[target];
        const std::uint64_t end = graph.incoming_offsets[target + 1];
        for (std::uint64_t edge = begin; edge < end; ++edge) {
            const Vertex source = graph.incoming_sources[edge];
            value += alpha * current[source] /
                     static_cast<double>(graph.out_degree[source]);
        }
        next[target] = value;
        difference += std::fabs(value - current[target]);
    }
    return difference;
}

void validate(const CSRGraph& graph, const PageRankOptions& options) {
    if (graph.vertex_count() == 0 ||
        graph.incoming_offsets.size() != graph.vertex_count() + 1 ||
        graph.original_ids.size() != graph.vertex_count()) {
        throw std::invalid_argument("The CSR graph is empty or inconsistent.");
    }
    if (!std::isfinite(options.alpha) || options.alpha < 0.0 ||
        options.alpha >= 1.0) {
        throw std::invalid_argument("alpha must be finite and satisfy 0 <= alpha < 1.");
    }
    if (!std::isfinite(options.tolerance) || options.tolerance <= 0.0) {
        throw std::invalid_argument("tolerance must be finite and positive.");
    }
    if (options.max_iterations == 0) {
        throw std::invalid_argument("max_iterations must be positive.");
    }
}

}  // namespace

PageRankResult run_serial(const CSRGraph& graph, const PageRankOptions& options) {
    validate(graph, options);
    const std::size_t n = graph.vertex_count();
    std::vector<double> current(n, 1.0 / static_cast<double>(n));
    std::vector<double> next(n, 0.0);

    PageRankResult result;
    const auto start = std::chrono::steady_clock::now();
    while (result.iterations < options.max_iterations) {
        result.last_change = update_once(graph, current, options.alpha, next);
        current.swap(next);
        ++result.iterations;
        if (result.last_change <= options.tolerance) {
            result.converged = true;
            break;
        }
    }
    result.solve_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    result.ranks = std::move(current);
    return result;
}

double one_step_residual(const CSRGraph& graph,
                         const std::vector<double>& ranks,
                         double alpha) {
    if (ranks.size() != graph.vertex_count()) {
        throw std::invalid_argument("The rank vector must match the graph size.");
    }
    std::vector<double> next(ranks.size(), 0.0);
    return update_once(graph, ranks, alpha, next);
}

}  // namespace pagerank

