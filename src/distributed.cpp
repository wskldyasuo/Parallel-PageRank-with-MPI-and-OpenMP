#include "pagerank/distributed.hpp"

#include <omp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace pagerank {
namespace {

Partition make_vertex_partition(int vertex_count, int ranks) {
    Partition partition;
    partition.counts.assign(ranks, 0);
    partition.displacements.assign(ranks, 0);
    const long long block =
        (static_cast<long long>(vertex_count) + ranks - 1) / ranks;
    for (int rank = 0; rank < ranks; ++rank) {
        const int begin = static_cast<int>(std::min<long long>(
            vertex_count, static_cast<long long>(rank) * block));
        const int end = static_cast<int>(
            std::min<long long>(vertex_count, begin + block));
        partition.displacements[rank] = begin;
        partition.counts[rank] = end - begin;
    }
    return partition;
}

Partition make_inedge_partition(const CSRGraph& graph, int ranks) {
    const int vertex_count = static_cast<int>(graph.vertex_count());
    std::vector<std::uint64_t> prefix(
        static_cast<std::size_t>(vertex_count) + 1, 0);
    for (int vertex = 0; vertex < vertex_count; ++vertex) {
        const std::uint64_t indegree =
            graph.incoming_offsets[vertex + 1] - graph.incoming_offsets[vertex];
        prefix[vertex + 1] = prefix[vertex] + 1 + indegree;
    }

    std::vector<int> boundaries(static_cast<std::size_t>(ranks) + 1, 0);
    boundaries.back() = vertex_count;
    const std::uint64_t total_work = prefix.back();
    for (int rank = 1; rank < ranks; ++rank) {
        const std::uint64_t target = static_cast<std::uint64_t>(
            (static_cast<long double>(total_work) * rank) / ranks);
        auto upper = std::lower_bound(prefix.begin(), prefix.end(), target);
        int boundary = static_cast<int>(upper - prefix.begin());
        if (boundary > 0 && boundary < vertex_count) {
            const std::uint64_t upper_distance = prefix[boundary] - target;
            const std::uint64_t lower_distance = target - prefix[boundary - 1];
            if (lower_distance <= upper_distance) {
                --boundary;
            }
        }
        boundaries[rank] = std::max(boundaries[rank - 1], boundary);
    }

    Partition partition;
    partition.counts.resize(ranks);
    partition.displacements.resize(ranks);
    for (int rank = 0; rank < ranks; ++rank) {
        partition.displacements[rank] = boundaries[rank];
        partition.counts[rank] = boundaries[rank + 1] - boundaries[rank];
    }
    return partition;
}

int owner_of(const Partition& partition, int vertex) {
    int low = 0;
    int high = static_cast<int>(partition.counts.size());
    while (low < high) {
        const int middle = low + (high - low) / 2;
        const int end = partition.displacements[middle] +
                        partition.counts[middle];
        if (vertex < end) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    if (low >= static_cast<int>(partition.counts.size()) ||
        vertex < partition.displacements[low]) {
        throw std::runtime_error("Cannot determine source-vertex owner.");
    }
    return low;
}

void check_options(const PageRankOptions& options) {
    if (!std::isfinite(options.alpha) || options.alpha < 0.0 ||
        options.alpha >= 1.0) {
        throw std::invalid_argument(
            "alpha must be finite and satisfy 0 <= alpha < 1.");
    }
    if (!std::isfinite(options.tolerance) || options.tolerance <= 0.0) {
        throw std::invalid_argument("tolerance must be finite and positive.");
    }
    if (options.max_iterations == 0 ||
        options.max_iterations >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(
            "max_iterations must be in the range 1..INT_MAX.");
    }
}

double dangling_mass(const DistributedCSRGraph& graph,
                     const std::vector<double>& current,
                     ParallelMode mode,
                     MPI_Comm communicator,
                     double* compute_seconds = nullptr,
                     double* allreduce_seconds = nullptr) {
    const double compute_start = MPI_Wtime();
    double local = 0.0;
    const int local_count = graph.local_vertex_count();
    if (mode == ParallelMode::hybrid) {
#pragma omp parallel for reduction(+ : local) schedule(static)
        for (int index = 0; index < local_count; ++index) {
            const int vertex = graph.local_begin + index;
            if (graph.out_degree[vertex] == 0) {
                local += current[vertex];
            }
        }
    } else {
        for (int index = 0; index < local_count; ++index) {
            const int vertex = graph.local_begin + index;
            if (graph.out_degree[vertex] == 0) {
                local += current[vertex];
            }
        }
    }
    if (compute_seconds != nullptr) {
        *compute_seconds += MPI_Wtime() - compute_start;
    }
    double global = 0.0;
    const double allreduce_start = MPI_Wtime();
    MPI_Allreduce(&local, &global, 1, MPI_DOUBLE, MPI_SUM, communicator);
    if (allreduce_seconds != nullptr) {
        *allreduce_seconds += MPI_Wtime() - allreduce_start;
    }
    return global;
}

void update_local(const DistributedCSRGraph& graph,
                  const std::vector<double>& current,
                  double alpha,
                  double dangling,
                  ParallelMode mode,
                  std::vector<double>& next) {
    const int local_count = graph.local_vertex_count();
    const double base = (1.0 - alpha + alpha * dangling) /
                        static_cast<double>(graph.vertex_count);
    auto update = [&](int local_target) {
        double value = base;
        const std::uint64_t begin = graph.incoming_offsets[local_target];
        const std::uint64_t end = graph.incoming_offsets[local_target + 1];
        for (std::uint64_t edge = begin; edge < end; ++edge) {
            const Vertex source = graph.incoming_sources[edge];
            value += alpha * current[source] /
                     static_cast<double>(graph.out_degree[source]);
        }
        next[local_target] = value;
    };
    if (mode == ParallelMode::hybrid) {
#pragma omp parallel for schedule(static)
        for (int local_target = 0; local_target < local_count; ++local_target) {
            update(local_target);
        }
    } else {
        for (int local_target = 0; local_target < local_count; ++local_target) {
            update(local_target);
        }
    }
}

double global_difference(const DistributedCSRGraph& graph,
                         const std::vector<double>& current,
                         const std::vector<double>& next,
                         ParallelMode mode,
                         MPI_Comm communicator,
                         double* compute_seconds = nullptr,
                         double* allreduce_seconds = nullptr) {
    const double compute_start = MPI_Wtime();
    double local = 0.0;
    const int local_count = graph.local_vertex_count();
    if (mode == ParallelMode::hybrid) {
#pragma omp parallel for reduction(+ : local) schedule(static)
        for (int index = 0; index < local_count; ++index) {
            local += std::fabs(next[index] - current[graph.local_begin + index]);
        }
    } else {
        for (int index = 0; index < local_count; ++index) {
            local += std::fabs(next[index] - current[graph.local_begin + index]);
        }
    }
    if (compute_seconds != nullptr) {
        *compute_seconds += MPI_Wtime() - compute_start;
    }
    double global = 0.0;
    const double allreduce_start = MPI_Wtime();
    MPI_Allreduce(&local, &global, 1, MPI_DOUBLE, MPI_SUM, communicator);
    if (allreduce_seconds != nullptr) {
        *allreduce_seconds += MPI_Wtime() - allreduce_start;
    }
    return global;
}

void gather_scores(const DistributedCSRGraph& graph,
                   const std::vector<double>& local,
                   std::vector<double>& complete,
                   MPI_Comm communicator,
                   double* allgatherv_seconds = nullptr) {
    const double allgatherv_start = MPI_Wtime();
    MPI_Allgatherv(local.data(), graph.local_vertex_count(), MPI_DOUBLE,
                   complete.data(), graph.partition.counts.data(),
                   graph.partition.displacements.data(), MPI_DOUBLE,
                   communicator);
    if (allgatherv_seconds != nullptr) {
        *allgatherv_seconds += MPI_Wtime() - allgatherv_start;
    }
}

double dangling_mass_local(const DistributedCSRGraph& graph,
                           const std::vector<double>& current,
                           ParallelMode mode,
                           MPI_Comm communicator,
                           double* compute_seconds = nullptr,
                           double* allreduce_seconds = nullptr) {
    const double compute_start = MPI_Wtime();
    double local = 0.0;
    const int local_count = graph.local_vertex_count();
    if (mode == ParallelMode::hybrid) {
#pragma omp parallel for reduction(+ : local) schedule(static)
        for (int index = 0; index < local_count; ++index) {
            if (graph.out_degree[graph.local_begin + index] == 0) {
                local += current[index];
            }
        }
    } else {
        for (int index = 0; index < local_count; ++index) {
            if (graph.out_degree[graph.local_begin + index] == 0) {
                local += current[index];
            }
        }
    }
    if (compute_seconds != nullptr) {
        *compute_seconds += MPI_Wtime() - compute_start;
    }
    double global = 0.0;
    const double allreduce_start = MPI_Wtime();
    MPI_Allreduce(&local, &global, 1, MPI_DOUBLE, MPI_SUM, communicator);
    if (allreduce_seconds != nullptr) {
        *allreduce_seconds += MPI_Wtime() - allreduce_start;
    }
    return global;
}

void exchange_demand_scores(const DistributedCSRGraph& graph,
                            const DemandExchangePlan& plan,
                            const std::vector<double>& local_scores,
                            std::vector<double>& remote_scores,
                            std::vector<double>& send_values,
                            MPI_Comm communicator,
                            double* exchange_seconds = nullptr,
                            double* pack_seconds = nullptr) {
    const double pack_start = MPI_Wtime();
    for (std::size_t index = 0; index < plan.requested_vertices.size(); ++index) {
        const int local_index = static_cast<int>(plan.requested_vertices[index]) -
                                graph.local_begin;
        send_values[index] = local_scores[local_index];
    }
    if (pack_seconds != nullptr) {
        *pack_seconds += MPI_Wtime() - pack_start;
    }
    const double start = MPI_Wtime();
    MPI_Alltoallv(send_values.data(), plan.serve_counts.data(),
                  plan.serve_displacements.data(), MPI_DOUBLE,
                  remote_scores.data(), plan.request_counts.data(),
                  plan.request_displacements.data(), MPI_DOUBLE,
                  communicator);
    if (exchange_seconds != nullptr) {
        *exchange_seconds += MPI_Wtime() - start;
    }
}

void update_local_demand(const DistributedCSRGraph& graph,
                         const DemandExchangePlan& plan,
                         const std::vector<double>& local_scores,
                         const std::vector<double>& remote_scores,
                         double alpha,
                         double dangling,
                         ParallelMode mode,
                         std::vector<double>& next) {
    const int local_count = graph.local_vertex_count();
    const double base = (1.0 - alpha + alpha * dangling) /
                        static_cast<double>(graph.vertex_count);
    auto update = [&](int local_target) {
        double value = base;
        const std::uint64_t begin = graph.incoming_offsets[local_target];
        const std::uint64_t end = graph.incoming_offsets[local_target + 1];
        for (std::uint64_t edge = begin; edge < end; ++edge) {
            const Vertex source = graph.incoming_sources[edge];
            const Vertex slot = plan.source_slots[edge];
            const double score = slot < static_cast<Vertex>(local_count)
                                     ? local_scores[slot]
                                     : remote_scores[slot - local_count];
            value += alpha * score /
                     static_cast<double>(graph.out_degree[source]);
        }
        next[local_target] = value;
    };
    if (mode == ParallelMode::hybrid) {
#pragma omp parallel for schedule(static)
        for (int local_target = 0; local_target < local_count; ++local_target) {
            update(local_target);
        }
    } else {
        for (int local_target = 0; local_target < local_count; ++local_target) {
            update(local_target);
        }
    }
}

double global_difference_local(const std::vector<double>& current,
                               const std::vector<double>& next,
                               ParallelMode mode,
                               MPI_Comm communicator,
                               double* compute_seconds = nullptr,
                               double* allreduce_seconds = nullptr) {
    const double compute_start = MPI_Wtime();
    double local = 0.0;
    const int local_count = static_cast<int>(current.size());
    if (mode == ParallelMode::hybrid) {
#pragma omp parallel for reduction(+ : local) schedule(static)
        for (int index = 0; index < local_count; ++index) {
            local += std::fabs(next[index] - current[index]);
        }
    } else {
        for (int index = 0; index < local_count; ++index) {
            local += std::fabs(next[index] - current[index]);
        }
    }
    if (compute_seconds != nullptr) {
        *compute_seconds += MPI_Wtime() - compute_start;
    }
    double global = 0.0;
    const double allreduce_start = MPI_Wtime();
    MPI_Allreduce(&local, &global, 1, MPI_DOUBLE, MPI_SUM, communicator);
    if (allreduce_seconds != nullptr) {
        *allreduce_seconds += MPI_Wtime() - allreduce_start;
    }
    return global;
}

}  // namespace

DistributionResult distribute_csr(CSRGraph& root_graph,
                                  PartitionStrategy strategy,
                                  MPI_Comm communicator) {
    int rank = 0;
    int size = 0;
    MPI_Comm_rank(communicator, &rank);
    MPI_Comm_size(communicator, &size);
    const double start = MPI_Wtime();

    int valid = 1;
    int vertex_count = 0;
    if (rank == 0) {
        const bool consistent =
            root_graph.vertex_count() > 0 &&
            root_graph.incoming_offsets.size() == root_graph.vertex_count() + 1 &&
            root_graph.incoming_offsets.front() == 0 &&
            root_graph.incoming_offsets.back() == root_graph.edge_count() &&
            root_graph.vertex_count() <=
                static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
            root_graph.edge_count() <=
                static_cast<std::size_t>(std::numeric_limits<int>::max());
        valid = consistent ? 1 : 0;
        if (consistent) {
            vertex_count = static_cast<int>(root_graph.vertex_count());
        }
    }
    MPI_Bcast(&valid, 1, MPI_INT, 0, communicator);
    if (!valid) {
        throw std::invalid_argument(
            "Distributed CSR requires a consistent graph with N,M <= INT_MAX.");
    }
    MPI_Bcast(&vertex_count, 1, MPI_INT, 0, communicator);

    DistributionResult result;
    result.graph.vertex_count = vertex_count;
    if (rank == 0) {
        result.graph.partition =
            strategy == PartitionStrategy::in_edges
                ? make_inedge_partition(root_graph, size)
                : make_vertex_partition(vertex_count, size);
    } else {
        result.graph.partition.counts.resize(size);
        result.graph.partition.displacements.resize(size);
    }
    MPI_Bcast(result.graph.partition.counts.data(), size, MPI_INT, 0,
              communicator);
    MPI_Bcast(result.graph.partition.displacements.data(), size, MPI_INT, 0,
              communicator);
    result.graph.local_begin = result.graph.partition.displacements[rank];
    const int local_count = result.graph.partition.counts[rank];

    result.graph.out_degree.resize(vertex_count);
    if (rank == 0) {
        result.graph.out_degree = root_graph.out_degree;
    }
    MPI_Bcast(result.graph.out_degree.data(), vertex_count, MPI_UINT32_T, 0,
              communicator);

    std::vector<int> indegrees;
    std::vector<int> edge_counts(size, 0);
    std::vector<int> edge_displacements(size, 0);
    if (rank == 0) {
        indegrees.resize(vertex_count);
        for (int vertex = 0; vertex < vertex_count; ++vertex) {
            indegrees[vertex] = static_cast<int>(
                root_graph.incoming_offsets[vertex + 1] -
                root_graph.incoming_offsets[vertex]);
        }
        for (int owner = 0; owner < size; ++owner) {
            const int begin = result.graph.partition.displacements[owner];
            const int end = begin + result.graph.partition.counts[owner];
            edge_displacements[owner] =
                static_cast<int>(root_graph.incoming_offsets[begin]);
            edge_counts[owner] = static_cast<int>(
                root_graph.incoming_offsets[end] -
                root_graph.incoming_offsets[begin]);
        }
    }

    std::vector<int> local_indegrees(local_count);
    MPI_Scatterv(indegrees.data(), result.graph.partition.counts.data(),
                 result.graph.partition.displacements.data(), MPI_INT,
                 local_indegrees.data(), local_count, MPI_INT, 0, communicator);

    int local_edges = 0;
    MPI_Scatter(edge_counts.data(), 1, MPI_INT, &local_edges, 1, MPI_INT, 0,
                communicator);
    result.graph.incoming_sources.resize(local_edges);
    MPI_Scatterv(root_graph.incoming_sources.data(), edge_counts.data(),
                 edge_displacements.data(), MPI_UINT32_T,
                 result.graph.incoming_sources.data(), local_edges,
                 MPI_UINT32_T, 0, communicator);

    result.graph.incoming_offsets.assign(
        static_cast<std::size_t>(local_count) + 1, 0);
    for (int index = 0; index < local_count; ++index) {
        result.graph.incoming_offsets[index + 1] =
            result.graph.incoming_offsets[index] + local_indegrees[index];
    }

    const std::uint64_t local_edge_count =
        static_cast<std::uint64_t>(local_edges);
    const std::uint64_t local_work =
        static_cast<std::uint64_t>(local_count) + local_edge_count;
    long long local_vertices_ll = local_count;
    long long min_vertices = 0;
    long long max_vertices = 0;
    unsigned long long local_edges_ull = local_edge_count;
    unsigned long long min_edges = 0;
    unsigned long long max_edges = 0;
    unsigned long long local_work_ull = local_work;
    unsigned long long max_work = 0;
    MPI_Allreduce(&local_vertices_ll, &min_vertices, 1, MPI_LONG_LONG, MPI_MIN,
                  communicator);
    MPI_Allreduce(&local_vertices_ll, &max_vertices, 1, MPI_LONG_LONG, MPI_MAX,
                  communicator);
    MPI_Allreduce(&local_edges_ull, &min_edges, 1, MPI_UNSIGNED_LONG_LONG,
                  MPI_MIN, communicator);
    MPI_Allreduce(&local_edges_ull, &max_edges, 1, MPI_UNSIGNED_LONG_LONG,
                  MPI_MAX, communicator);
    MPI_Allreduce(&local_work_ull, &max_work, 1, MPI_UNSIGNED_LONG_LONG,
                  MPI_MAX, communicator);
    result.stats.min_vertices = static_cast<int>(min_vertices);
    result.stats.max_vertices = static_cast<int>(max_vertices);
    result.stats.mean_vertices = static_cast<double>(vertex_count) / size;
    result.stats.min_edges = min_edges;
    result.stats.max_edges = max_edges;
    result.stats.mean_edges = static_cast<double>(
        rank == 0 ? root_graph.edge_count() : 0);
    MPI_Bcast(&result.stats.mean_edges, 1, MPI_DOUBLE, 0, communicator);
    result.stats.mean_edges /= size;
    result.stats.max_work = max_work;
    result.stats.mean_work =
        (static_cast<double>(vertex_count) + result.stats.mean_edges * size) /
        size;

    if (rank == 0) {
        root_graph = CSRGraph{};
    }

    const double local_seconds = MPI_Wtime() - start;
    MPI_Allreduce(&local_seconds, &result.max_seconds, 1, MPI_DOUBLE, MPI_MAX,
                  communicator);
    return result;
}

DemandExchangePlan build_demand_exchange_plan(
    const DistributedCSRGraph& graph, MPI_Comm communicator) {
    int rank = 0;
    int size = 0;
    MPI_Comm_rank(communicator, &rank);
    MPI_Comm_size(communicator, &size);
    const double start = MPI_Wtime();

    DemandExchangePlan plan;
    plan.request_counts.assign(size, 0);
    plan.request_displacements.assign(size, 0);
    plan.serve_counts.assign(size, 0);
    plan.serve_displacements.assign(size, 0);

    for (Vertex source : graph.incoming_sources) {
        if (owner_of(graph.partition, static_cast<int>(source)) != rank) {
            plan.remote_vertices.push_back(source);
        }
    }
    std::sort(plan.remote_vertices.begin(), plan.remote_vertices.end());
    plan.remote_vertices.erase(
        std::unique(plan.remote_vertices.begin(), plan.remote_vertices.end()),
        plan.remote_vertices.end());
    for (Vertex source : plan.remote_vertices) {
        ++plan.request_counts[owner_of(graph.partition,
                                       static_cast<int>(source))];
    }
    for (int peer = 1; peer < size; ++peer) {
        plan.request_displacements[peer] =
            plan.request_displacements[peer - 1] +
            plan.request_counts[peer - 1];
    }

    MPI_Alltoall(plan.request_counts.data(), 1, MPI_INT,
                 plan.serve_counts.data(), 1, MPI_INT, communicator);
    for (int peer = 1; peer < size; ++peer) {
        plan.serve_displacements[peer] =
            plan.serve_displacements[peer - 1] + plan.serve_counts[peer - 1];
    }
    const int serve_total = plan.serve_displacements.back() +
                            plan.serve_counts.back();
    plan.requested_vertices.resize(serve_total);
    MPI_Alltoallv(plan.remote_vertices.data(), plan.request_counts.data(),
                  plan.request_displacements.data(), MPI_UINT32_T,
                  plan.requested_vertices.data(), plan.serve_counts.data(),
                  plan.serve_displacements.data(), MPI_UINT32_T,
                  communicator);

    bool valid = true;
    for (Vertex source : plan.requested_vertices) {
        valid = valid && source >= static_cast<Vertex>(graph.local_begin) &&
                source < static_cast<Vertex>(graph.local_begin +
                                             graph.local_vertex_count());
    }
    int local_valid = valid ? 1 : 0;
    int global_valid = 0;
    MPI_Allreduce(&local_valid, &global_valid, 1, MPI_INT, MPI_MIN,
                  communicator);
    if (!global_valid) {
        throw std::runtime_error("Demand exchange requested a non-local score.");
    }

    std::unordered_map<Vertex, Vertex> remote_slot;
    remote_slot.reserve(plan.remote_vertices.size());
    const Vertex local_count =
        static_cast<Vertex>(graph.local_vertex_count());
    for (std::size_t index = 0; index < plan.remote_vertices.size(); ++index) {
        remote_slot.emplace(plan.remote_vertices[index],
                            local_count + static_cast<Vertex>(index));
    }
    plan.source_slots.resize(graph.incoming_sources.size());
    for (std::size_t edge = 0; edge < graph.incoming_sources.size(); ++edge) {
        const Vertex source = graph.incoming_sources[edge];
        if (source >= static_cast<Vertex>(graph.local_begin) &&
            source < static_cast<Vertex>(graph.local_begin) + local_count) {
            plan.source_slots[edge] = source - graph.local_begin;
        } else {
            plan.source_slots[edge] = remote_slot.at(source);
        }
    }

    const unsigned long long local_remote = plan.remote_vertices.size();
    unsigned long long total_remote = 0;
    unsigned long long max_remote = 0;
    MPI_Allreduce(&local_remote, &total_remote, 1, MPI_UNSIGNED_LONG_LONG,
                  MPI_SUM, communicator);
    MPI_Allreduce(&local_remote, &max_remote, 1, MPI_UNSIGNED_LONG_LONG,
                  MPI_MAX, communicator);
    plan.remote_dependencies_total = total_remote;
    plan.remote_dependencies_max = max_remote;
    plan.remote_dependencies_mean = static_cast<double>(total_remote) / size;
    plan.score_values_per_iteration = total_remote;

    int local_partners = 0;
    for (int count : plan.request_counts) {
        local_partners += count > 0 ? 1 : 0;
    }
    int partner_sum = 0;
    MPI_Allreduce(&local_partners, &plan.request_partners_max, 1, MPI_INT,
                  MPI_MAX, communicator);
    MPI_Allreduce(&local_partners, &partner_sum, 1, MPI_INT, MPI_SUM,
                  communicator);
    plan.request_partners_mean = static_cast<double>(partner_sum) / size;

    const unsigned long long local_state_bytes =
        static_cast<unsigned long long>(2 * graph.local_vertex_count() +
                                        plan.remote_vertices.size() +
                                        plan.requested_vertices.size()) *
            sizeof(double) +
        static_cast<unsigned long long>(plan.source_slots.size() +
                                        plan.remote_vertices.size() +
                                        plan.requested_vertices.size()) *
            sizeof(Vertex) +
        static_cast<unsigned long long>(4 * size) * sizeof(int);
    unsigned long long max_state_bytes = 0;
    MPI_Allreduce(&local_state_bytes, &max_state_bytes, 1,
                  MPI_UNSIGNED_LONG_LONG, MPI_MAX, communicator);
    plan.state_bytes_max = max_state_bytes;

    const double local_seconds = MPI_Wtime() - start;
    MPI_Allreduce(&local_seconds, &plan.build_seconds, 1, MPI_DOUBLE, MPI_MAX,
                  communicator);
    return plan;
}

PageRankResult run_distributed(const DistributedCSRGraph& graph,
                               const PageRankOptions& options,
                               ParallelMode mode,
                               MPI_Comm communicator) {
    check_options(options);
    if (graph.vertex_count <= 0 || graph.out_degree.size() !=
                                       static_cast<std::size_t>(graph.vertex_count)) {
        throw std::invalid_argument("The distributed graph is empty or inconsistent.");
    }

    std::vector<double> current(
        graph.vertex_count, 1.0 / static_cast<double>(graph.vertex_count));
    std::vector<double> local_next(graph.local_vertex_count(), 0.0);
    std::vector<double> complete(graph.vertex_count, 0.0);
    PageRankResult result;

    MPI_Barrier(communicator);
    const double start = MPI_Wtime();
    double local_update_seconds = 0.0;
    double local_compute_seconds = 0.0;
    double local_allreduce_seconds = 0.0;
    double local_allgatherv_seconds = 0.0;
    while (result.iterations < options.max_iterations) {
        const double dangling = dangling_mass(
            graph, current, mode, communicator, &local_compute_seconds,
            &local_allreduce_seconds);
        const double update_start = MPI_Wtime();
        update_local(graph, current, options.alpha, dangling, mode, local_next);
        const double update_elapsed = MPI_Wtime() - update_start;
        local_update_seconds += update_elapsed;
        local_compute_seconds += update_elapsed;
        result.last_change = global_difference(
            graph, current, local_next, mode, communicator,
            &local_compute_seconds, &local_allreduce_seconds);
        gather_scores(graph, local_next, complete, communicator,
                      &local_allgatherv_seconds);
        current.swap(complete);
        ++result.iterations;
        if (result.last_change <= options.tolerance) {
            result.converged = true;
            break;
        }
    }
    const double local_seconds = MPI_Wtime() - start;
    double solve_sum_seconds = 0.0;
    MPI_Allreduce(&local_seconds, &result.solve_seconds, 1, MPI_DOUBLE, MPI_MAX,
                  communicator);
    MPI_Allreduce(&local_seconds, &solve_sum_seconds, 1, MPI_DOUBLE, MPI_SUM,
                  communicator);
    int communicator_size = 1;
    MPI_Comm_size(communicator, &communicator_size);
    result.solve_mean_seconds = solve_sum_seconds / communicator_size;

    auto reduce_timing = [&](double local, double& maximum, double& mean) {
        double sum = 0.0;
        MPI_Allreduce(&local, &maximum, 1, MPI_DOUBLE, MPI_MAX, communicator);
        MPI_Allreduce(&local, &sum, 1, MPI_DOUBLE, MPI_SUM, communicator);
        mean = sum / communicator_size;
    };
    reduce_timing(local_update_seconds, result.update_max_seconds,
                  result.update_mean_seconds);
    reduce_timing(local_compute_seconds, result.compute_max_seconds,
                  result.compute_mean_seconds);
    reduce_timing(local_allreduce_seconds, result.allreduce_max_seconds,
                  result.allreduce_mean_seconds);
    reduce_timing(local_allgatherv_seconds, result.allgatherv_max_seconds,
                  result.allgatherv_mean_seconds);
    result.score_exchange_max_seconds = result.allgatherv_max_seconds;
    result.score_exchange_mean_seconds = result.allgatherv_mean_seconds;
    result.ranks = std::move(current);
    return result;
}

PageRankResult run_distributed_demand(const DistributedCSRGraph& graph,
                                      const DemandExchangePlan& plan,
                                      const PageRankOptions& options,
                                      ParallelMode mode,
                                      MPI_Comm communicator) {
    check_options(options);
    if (graph.vertex_count <= 0 || graph.out_degree.size() !=
                                       static_cast<std::size_t>(graph.vertex_count) ||
        plan.source_slots.size() != graph.incoming_sources.size()) {
        throw std::invalid_argument("The demand-distributed graph is inconsistent.");
    }

    const int local_count = graph.local_vertex_count();
    std::vector<double> current(
        local_count, 1.0 / static_cast<double>(graph.vertex_count));
    std::vector<double> next(local_count, 0.0);
    std::vector<double> remote(plan.remote_vertices.size(), 0.0);
    std::vector<double> send_values(plan.requested_vertices.size(), 0.0);
    PageRankResult result;
    result.local_begin = graph.local_begin;
    result.ranks_are_local = true;

    MPI_Barrier(communicator);
    const double start = MPI_Wtime();
    double local_update_seconds = 0.0;
    double local_compute_seconds = 0.0;
    double local_allreduce_seconds = 0.0;
    double local_exchange_seconds = 0.0;
    double local_pack_seconds = 0.0;
    while (result.iterations < options.max_iterations) {
        exchange_demand_scores(graph, plan, current, remote, send_values,
                               communicator, &local_exchange_seconds,
                               &local_pack_seconds);
        const double dangling = dangling_mass_local(
            graph, current, mode, communicator, &local_compute_seconds,
            &local_allreduce_seconds);
        const double update_start = MPI_Wtime();
        update_local_demand(graph, plan, current, remote, options.alpha,
                            dangling, mode, next);
        const double update_elapsed = MPI_Wtime() - update_start;
        local_update_seconds += update_elapsed;
        local_compute_seconds += update_elapsed;
        result.last_change = global_difference_local(
            current, next, mode, communicator, &local_compute_seconds,
            &local_allreduce_seconds);
        current.swap(next);
        ++result.iterations;
        if (result.last_change <= options.tolerance) {
            result.converged = true;
            break;
        }
    }
    const double local_seconds = MPI_Wtime() - start;
    double solve_sum_seconds = 0.0;
    MPI_Allreduce(&local_seconds, &result.solve_seconds, 1, MPI_DOUBLE, MPI_MAX,
                  communicator);
    MPI_Allreduce(&local_seconds, &solve_sum_seconds, 1, MPI_DOUBLE, MPI_SUM,
                  communicator);
    int communicator_size = 1;
    MPI_Comm_size(communicator, &communicator_size);
    result.solve_mean_seconds = solve_sum_seconds / communicator_size;

    auto reduce_timing = [&](double local, double& maximum, double& mean) {
        double sum = 0.0;
        MPI_Allreduce(&local, &maximum, 1, MPI_DOUBLE, MPI_MAX, communicator);
        MPI_Allreduce(&local, &sum, 1, MPI_DOUBLE, MPI_SUM, communicator);
        mean = sum / communicator_size;
    };
    reduce_timing(local_update_seconds, result.update_max_seconds,
                  result.update_mean_seconds);
    reduce_timing(local_compute_seconds, result.compute_max_seconds,
                  result.compute_mean_seconds);
    reduce_timing(local_allreduce_seconds, result.allreduce_max_seconds,
                  result.allreduce_mean_seconds);
    reduce_timing(local_exchange_seconds, result.score_exchange_max_seconds,
                  result.score_exchange_mean_seconds);
    reduce_timing(local_pack_seconds, result.score_pack_max_seconds,
                  result.score_pack_mean_seconds);
    result.ranks = std::move(current);
    return result;
}

double distributed_one_step_residual(const DistributedCSRGraph& graph,
                                     const std::vector<double>& ranks,
                                     double alpha,
                                     ParallelMode mode,
                                     MPI_Comm communicator) {
    std::vector<double> next(graph.local_vertex_count(), 0.0);
    const double dangling = dangling_mass(graph, ranks, mode, communicator);
    update_local(graph, ranks, alpha, dangling, mode, next);
    return global_difference(graph, ranks, next, mode, communicator);
}

double distributed_one_step_residual_demand(
    const DistributedCSRGraph& graph,
    const DemandExchangePlan& plan,
    const std::vector<double>& local_ranks,
    double alpha,
    ParallelMode mode,
    MPI_Comm communicator) {
    std::vector<double> next(graph.local_vertex_count(), 0.0);
    std::vector<double> remote(plan.remote_vertices.size(), 0.0);
    std::vector<double> send_values(plan.requested_vertices.size(), 0.0);
    exchange_demand_scores(graph, plan, local_ranks, remote, send_values,
                           communicator);
    const double dangling =
        dangling_mass_local(graph, local_ranks, mode, communicator);
    update_local_demand(graph, plan, local_ranks, remote, alpha, dangling, mode,
                        next);
    return global_difference_local(local_ranks, next, mode, communicator);
}

std::vector<double> gather_complete_scores(
    const DistributedCSRGraph& graph,
    const std::vector<double>& local_scores,
    MPI_Comm communicator) {
    int rank = 0;
    MPI_Comm_rank(communicator, &rank);
    std::vector<double> complete;
    if (rank == 0) {
        complete.resize(graph.vertex_count);
    }
    MPI_Gatherv(local_scores.data(), graph.local_vertex_count(), MPI_DOUBLE,
                complete.data(), graph.partition.counts.data(),
                graph.partition.displacements.data(), MPI_DOUBLE, 0,
                communicator);
    return complete;
}

}  // namespace pagerank
