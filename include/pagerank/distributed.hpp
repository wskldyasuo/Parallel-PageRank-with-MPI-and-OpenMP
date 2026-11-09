#ifndef DSA5208_PAGERANK_DISTRIBUTED_HPP
#define DSA5208_PAGERANK_DISTRIBUTED_HPP

#include "pagerank/graph.hpp"
#include "pagerank/pagerank.hpp"

#include <mpi.h>

#include <cstdint>
#include <vector>

namespace pagerank {

struct Partition {
    std::vector<int> counts;
    std::vector<int> displacements;
};

enum class PartitionStrategy {
    vertices,
    in_edges,
};

struct PartitionStats {
    int min_vertices = 0;
    int max_vertices = 0;
    double mean_vertices = 0.0;
    std::uint64_t min_edges = 0;
    std::uint64_t max_edges = 0;
    double mean_edges = 0.0;
    std::uint64_t max_work = 0;
    double mean_work = 0.0;
};

struct DistributedCSRGraph {
    int vertex_count = 0;
    int local_begin = 0;
    Partition partition;
    std::vector<std::uint64_t> incoming_offsets;
    std::vector<Vertex> incoming_sources;
    std::vector<Vertex> out_degree;

    int local_vertex_count() const {
        return static_cast<int>(incoming_offsets.size()) - 1;
    }
};

enum class ScoreExchange {
    full,
    demand,
};

struct DemandExchangePlan {
    std::vector<Vertex> source_slots;
    std::vector<Vertex> remote_vertices;
    std::vector<int> request_counts;
    std::vector<int> request_displacements;
    std::vector<int> serve_counts;
    std::vector<int> serve_displacements;
    std::vector<Vertex> requested_vertices;
    double build_seconds = 0.0;
    std::uint64_t remote_dependencies_total = 0;
    std::uint64_t remote_dependencies_max = 0;
    double remote_dependencies_mean = 0.0;
    int request_partners_max = 0;
    double request_partners_mean = 0.0;
    std::uint64_t score_values_per_iteration = 0;
    std::uint64_t state_bytes_max = 0;
};

enum class ParallelMode {
    mpi,
    hybrid,
};

struct DistributionResult {
    DistributedCSRGraph graph;
    double max_seconds = 0.0;
    PartitionStats stats;
};

// All ranks call this function. On rank 0, root_graph contains the complete
// graph and is released after distribution; on other ranks it may be empty.
DistributionResult distribute_csr(CSRGraph& root_graph,
                                  PartitionStrategy strategy,
                                  MPI_Comm communicator);

DemandExchangePlan build_demand_exchange_plan(
    const DistributedCSRGraph& graph, MPI_Comm communicator);

// Every rank receives the complete score vector. MPI communication is kept
// outside OpenMP regions and therefore only requires MPI_THREAD_FUNNELED.
PageRankResult run_distributed(const DistributedCSRGraph& graph,
                               const PageRankOptions& options,
                               ParallelMode mode,
                               MPI_Comm communicator);

PageRankResult run_distributed_demand(const DistributedCSRGraph& graph,
                                      const DemandExchangePlan& plan,
                                      const PageRankOptions& options,
                                      ParallelMode mode,
                                      MPI_Comm communicator);

double distributed_one_step_residual(const DistributedCSRGraph& graph,
                                     const std::vector<double>& ranks,
                                     double alpha,
                                     ParallelMode mode,
                                     MPI_Comm communicator);

double distributed_one_step_residual_demand(
    const DistributedCSRGraph& graph,
    const DemandExchangePlan& plan,
    const std::vector<double>& local_ranks,
    double alpha,
    ParallelMode mode,
    MPI_Comm communicator);

std::vector<double> gather_complete_scores(
    const DistributedCSRGraph& graph,
    const std::vector<double>& local_scores,
    MPI_Comm communicator);

}  // namespace pagerank

#endif
