#include "pagerank/distributed.hpp"
#include "pagerank/graph.hpp"
#include "pagerank/pagerank.hpp"

#include <mpi.h>
#include <omp.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

namespace {

struct CommandLine {
    std::string input;
    std::string input_format = "auto";
    std::string mode = "serial";
    std::string partition = "vertices";
    std::string score_exchange = "full";
    std::string output;
    std::string metrics;
    std::string run_id;
    pagerank::PageRankOptions options;
    std::size_t top_k = 10;
    int threads = 1;
    bool verify = false;
    bool help = false;
};

struct ExchangeMetrics {
    double dependency_seconds = 0.0;
    std::uint64_t remote_dependencies_total = 0;
    std::uint64_t remote_dependencies_max = 0;
    double remote_dependencies_mean = 0.0;
    int request_partners_max = 0;
    double request_partners_mean = 0.0;
    std::uint64_t score_values_per_iteration = 0;
    std::uint64_t score_bytes_per_iteration = 0;
    std::uint64_t score_state_bytes_max = 0;
};

std::string require_value(int& index, int argc, char** argv) {
    if (++index >= argc) {
        throw std::invalid_argument("Missing value after " +
                                    std::string(argv[index - 1]));
    }
    return argv[index];
}

template <typename T, typename Convert>
T parse_exact(const std::string& text, Convert convert,
              const std::string& label) {
    std::size_t used = 0;
    const auto value = convert(text, &used);
    if (used != text.size()) {
        throw std::invalid_argument("Invalid " + label + ": " + text);
    }
    return static_cast<T>(value);
}

CommandLine parse_arguments(int argc, char** argv) {
    CommandLine command;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            command.help = true;
        } else if (argument == "--input") {
            command.input = require_value(index, argc, argv);
        } else if (argument == "--input-format") {
            command.input_format = require_value(index, argc, argv);
        } else if (argument == "--mode") {
            command.mode = require_value(index, argc, argv);
        } else if (argument == "--partition") {
            command.partition = require_value(index, argc, argv);
        } else if (argument == "--score-exchange") {
            command.score_exchange = require_value(index, argc, argv);
        } else if (argument == "--threads") {
            const long long threads = parse_exact<long long>(
                require_value(index, argc, argv),
                [](const std::string& value, std::size_t* used) {
                    return std::stoll(value, used);
                },
                "thread count");
            if (threads < 1 || threads > std::numeric_limits<int>::max()) {
                throw std::invalid_argument("Thread count is outside 1..INT_MAX.");
            }
            command.threads = static_cast<int>(threads);
        } else if (argument == "--alpha") {
            command.options.alpha = parse_exact<double>(
                require_value(index, argc, argv),
                [](const std::string& value, std::size_t* used) {
                    return std::stod(value, used);
                },
                "alpha");
        } else if (argument == "--tolerance" || argument == "--tol") {
            command.options.tolerance = parse_exact<double>(
                require_value(index, argc, argv),
                [](const std::string& value, std::size_t* used) {
                    return std::stod(value, used);
                },
                "tolerance");
        } else if (argument == "--max-iterations") {
            command.options.max_iterations = parse_exact<std::size_t>(
                require_value(index, argc, argv),
                [](const std::string& value, std::size_t* used) {
                    return std::stoull(value, used);
                },
                "iteration limit");
        } else if (argument == "--top-k") {
            command.top_k = parse_exact<std::size_t>(
                require_value(index, argc, argv),
                [](const std::string& value, std::size_t* used) {
                    return std::stoull(value, used);
                },
                "top-k");
        } else if (argument == "--output") {
            command.output = require_value(index, argc, argv);
        } else if (argument == "--metrics") {
            command.metrics = require_value(index, argc, argv);
        } else if (argument == "--run-id") {
            command.run_id = require_value(index, argc, argv);
        } else if (argument == "--verify") {
            command.verify = true;
        } else {
            throw std::invalid_argument("Unknown argument: " + argument);
        }
    }
    if (!command.help && command.input.empty()) {
        throw std::invalid_argument("--input is required.");
    }
    if (command.mode != "serial" && command.mode != "mpi" &&
        command.mode != "hybrid") {
        throw std::invalid_argument("--mode must be serial, mpi, or hybrid.");
    }
    if (command.input_format != "auto" && command.input_format != "snap" &&
        command.input_format != "csr") {
        throw std::invalid_argument("--input-format must be auto, snap, or csr.");
    }
    if (command.partition != "vertices" && command.partition != "in-edges") {
        throw std::invalid_argument(
            "--partition must be vertices or in-edges.");
    }
    if (command.score_exchange != "full" &&
        command.score_exchange != "demand") {
        throw std::invalid_argument(
            "--score-exchange must be full or demand.");
    }
    if (command.mode == "serial" && command.score_exchange != "full") {
        throw std::invalid_argument(
            "Serial mode only supports --score-exchange full.");
    }
    if (command.mode == "serial" && command.partition != "vertices") {
        throw std::invalid_argument(
            "Serial mode only supports --partition vertices.");
    }
    if (command.threads < 1 ||
        (command.mode != "hybrid" && command.threads != 1)) {
        throw std::invalid_argument(
            "Use --threads 1 for serial/MPI and a positive value for hybrid.");
    }
    return command;
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " --input GRAPH [options]\n"
        << "  --mode serial|mpi|hybrid  Execution mode (default: serial)\n"
        << "  --threads N               Threads per rank; hybrid only (default: 1)\n"
        << "  --partition vertices|in-edges  Distributed target partition\n"
        << "  --score-exchange full|demand  Per-iteration score exchange\n"
        << "  --input-format auto|snap|csr\n"
        << "  --alpha VALUE             Damping factor (default: 0.85)\n"
        << "  --tolerance VALUE         Global L1 threshold (default: 1e-12)\n"
        << "  --max-iterations N        Iteration limit (default: 1000)\n"
        << "  --verify                  Compare parallel scores with serial CSR\n"
        << "  --top-k N                 Scores printed by rank 0 (default: 10)\n"
        << "  --output FILE             Write original_id<TAB>rank\n"
        << "  --metrics FILE            Append a CSV measurement row\n"
        << "  --run-id VALUE            Identifier stored in metrics CSV\n";
}

pagerank::GraphLoadResult load_graph(const CommandLine& command) {
    std::string format = command.input_format;
    if (format == "auto") {
        format = std::filesystem::path(command.input).extension() == ".prcsr"
                     ? "csr"
                     : "snap";
    }
    return format == "csr" ? pagerank::load_csr_graph(command.input)
                           : pagerank::load_snap_graph(command.input);
}

bool all_true(bool value, MPI_Comm communicator) {
    int local = value ? 1 : 0;
    int global = 0;
    MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN, communicator);
    return global != 0;
}

double l1_distance(const std::vector<double>& left,
                   const std::vector<double>& right) {
    if (left.size() != right.size()) {
        return std::numeric_limits<double>::infinity();
    }
    double difference = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference += std::fabs(left[index] - right[index]);
    }
    return difference;
}

std::uint64_t peak_rss_bytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

std::string csv_quote(const std::string& text) {
    std::string result = "\"";
    for (const char character : text) {
        result += character;
        if (character == '"') {
            result += '"';
        }
    }
    return result + '"';
}

std::string hostname() {
    char buffer[256]{};
    return gethostname(buffer, sizeof(buffer) - 1) == 0 ? buffer : "unknown";
}

void write_ranks(const std::string& path,
                 const std::vector<std::int64_t>& original_ids,
                 const std::vector<double>& ranks) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Cannot open rank output file: " + path);
    }
    output << "original_id\trank\n" << std::setprecision(17);
    for (std::size_t vertex = 0; vertex < ranks.size(); ++vertex) {
        output << original_ids[vertex] << '\t' << ranks[vertex] << '\n';
    }
}

void write_metrics(const CommandLine& command, int mpi_ranks,
                   std::size_t vertices, std::size_t edges,
                   std::uint64_t raw_edges, std::uint64_t duplicate_edges,
                   std::size_t dangling_vertices, double load_seconds,
                   double distribution_seconds,
                   const pagerank::PartitionStats& partition_stats,
                   const pagerank::PageRankResult& result, double residual,
                   double sum_error, double serial_l1_error,
                   const ExchangeMetrics& exchange,
                   std::uint64_t peak_rss_max_bytes) {
    const bool header = !std::filesystem::exists(command.metrics) ||
                        std::filesystem::file_size(command.metrics) == 0;
    std::ofstream output(command.metrics, std::ios::app);
    if (!output) {
        throw std::runtime_error("Cannot open metrics file: " + command.metrics);
    }
    if (header) {
        output
            << "version,mode,partition,score_exchange,run_id,hostname,dataset,mpi_ranks,threads_per_rank,"
               "vertices,edges,raw_edges,duplicate_edges,dangling_vertices,"
               "alpha,tolerance,max_iterations,iterations,converged,load_seconds,"
               "distribution_seconds,solve_seconds,solve_mean_seconds,"
               "compute_max_seconds,compute_mean_seconds,"
               "allreduce_max_seconds,allreduce_mean_seconds,"
               "allgatherv_max_seconds,allgatherv_mean_seconds,"
               "score_exchange_max_seconds,score_exchange_mean_seconds,"
               "score_pack_max_seconds,score_pack_mean_seconds,"
               "dependency_seconds,remote_dependencies_total,"
               "remote_dependencies_max,remote_dependencies_mean,"
               "request_partners_max,request_partners_mean,"
               "score_values_per_iteration,score_bytes_per_iteration,"
               "score_state_bytes_max,"
               "update_max_seconds,"
               "update_mean_seconds,update_imbalance,local_vertices_min,"
               "local_vertices_max,local_vertices_mean,local_edges_min,"
               "local_edges_max,local_edges_mean,local_work_max,"
               "local_work_mean,local_work_imbalance,last_change,residual,"
               "rank_sum_error,serial_l1_error,peak_rss_bytes\n";
    }
    const double update_imbalance =
        result.update_mean_seconds > 0.0
            ? result.update_max_seconds / result.update_mean_seconds
            : 0.0;
    const double work_imbalance =
        partition_stats.mean_work > 0.0
            ? partition_stats.max_work / partition_stats.mean_work
            : 0.0;
    output << "CSR4," << command.mode << ',' << command.partition << ','
           << command.score_exchange << ','
           << csv_quote(command.run_id) << ','
           << csv_quote(hostname()) << ',' << csv_quote(command.input) << ','
           << mpi_ranks << ',' << command.threads << ',' << vertices << ','
           << edges << ',' << raw_edges << ',' << duplicate_edges << ','
           << dangling_vertices << ',' << std::setprecision(17)
           << command.options.alpha << ',' << command.options.tolerance << ','
           << command.options.max_iterations << ',' << result.iterations << ','
           << (result.converged ? 1 : 0) << ',' << load_seconds << ','
           << distribution_seconds << ',' << result.solve_seconds << ','
           << result.solve_mean_seconds << ',' << result.compute_max_seconds << ','
           << result.compute_mean_seconds << ','
           << result.allreduce_max_seconds << ','
           << result.allreduce_mean_seconds << ','
           << result.allgatherv_max_seconds << ','
           << result.allgatherv_mean_seconds << ','
           << result.score_exchange_max_seconds << ','
           << result.score_exchange_mean_seconds << ','
           << result.score_pack_max_seconds << ','
           << result.score_pack_mean_seconds << ','
           << exchange.dependency_seconds << ','
           << exchange.remote_dependencies_total << ','
           << exchange.remote_dependencies_max << ','
           << exchange.remote_dependencies_mean << ','
           << exchange.request_partners_max << ','
           << exchange.request_partners_mean << ','
           << exchange.score_values_per_iteration << ','
           << exchange.score_bytes_per_iteration << ','
           << exchange.score_state_bytes_max << ','
           << result.update_max_seconds << ',' << result.update_mean_seconds << ','
           << update_imbalance << ',' << partition_stats.min_vertices << ','
           << partition_stats.max_vertices << ','
           << partition_stats.mean_vertices << ',' << partition_stats.min_edges
           << ',' << partition_stats.max_edges << ','
           << partition_stats.mean_edges << ',' << partition_stats.max_work << ','
           << partition_stats.mean_work << ',' << work_imbalance << ','
           << result.last_change << ',' << residual << ',' << sum_error << ','
           << serial_l1_error << ',' << peak_rss_max_bytes << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    int provided = 0;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
    int rank = 0;
    int size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int exit_code = 0;
    try {
        if (!all_true(provided >= MPI_THREAD_FUNNELED, MPI_COMM_WORLD)) {
            throw std::runtime_error("MPI_THREAD_FUNNELED is required.");
        }
        CommandLine command;
        bool parsed = true;
        try {
            command = parse_arguments(argc, argv);
        } catch (const std::exception& error) {
            if (rank == 0) {
                std::cerr << "Argument error: " << error.what() << '\n';
            }
            parsed = false;
        }
        if (!all_true(parsed, MPI_COMM_WORLD)) {
            MPI_Finalize();
            return 1;
        }
        if (command.help) {
            if (rank == 0) {
                print_usage(argv[0]);
            }
            MPI_Finalize();
            return 0;
        }
        if (command.mode == "serial" && size != 1) {
            throw std::invalid_argument("Serial mode must run with exactly one MPI rank.");
        }
        omp_set_dynamic(0);
        omp_set_num_threads(command.threads);

        pagerank::GraphLoadResult loaded;
        int load_ok = 1;
        if (rank == 0) {
            try {
                loaded = load_graph(command);
            } catch (const std::exception& error) {
                std::cerr << "Input error: " << error.what() << '\n';
                load_ok = 0;
            }
        }
        MPI_Bcast(&load_ok, 1, MPI_INT, 0, MPI_COMM_WORLD);
        if (!load_ok) {
            MPI_Finalize();
            return 1;
        }

        std::size_t vertices = 0;
        std::size_t edges = 0;
        std::size_t dangling_vertices = 0;
        std::vector<std::int64_t> original_ids;
        pagerank::PageRankResult serial_reference;
        if (rank == 0) {
            vertices = loaded.graph.vertex_count();
            edges = loaded.graph.edge_count();
            dangling_vertices = static_cast<std::size_t>(std::count(
                loaded.graph.out_degree.begin(), loaded.graph.out_degree.end(), 0));
            original_ids = loaded.graph.original_ids;
            if (command.verify && command.mode != "serial") {
                serial_reference =
                    pagerank::run_serial(loaded.graph, command.options);
            }
        }
        unsigned long long graph_sizes[3] = {
            static_cast<unsigned long long>(vertices),
            static_cast<unsigned long long>(edges),
            static_cast<unsigned long long>(dangling_vertices)};
        MPI_Bcast(graph_sizes, 3, MPI_UNSIGNED_LONG_LONG, 0, MPI_COMM_WORLD);
        vertices = static_cast<std::size_t>(graph_sizes[0]);
        edges = static_cast<std::size_t>(graph_sizes[1]);
        dangling_vertices = static_cast<std::size_t>(graph_sizes[2]);

        pagerank::PageRankResult result;
        pagerank::PartitionStats partition_stats;
        pagerank::DistributedCSRGraph distributed_graph;
        pagerank::DemandExchangePlan demand_plan;
        ExchangeMetrics exchange_metrics;
        double distribution_seconds = 0.0;
        double residual = 0.0;
        if (command.mode == "serial") {
            partition_stats.min_vertices = static_cast<int>(vertices);
            partition_stats.max_vertices = static_cast<int>(vertices);
            partition_stats.mean_vertices = static_cast<double>(vertices);
            partition_stats.min_edges = edges;
            partition_stats.max_edges = edges;
            partition_stats.mean_edges = static_cast<double>(edges);
            partition_stats.max_work = vertices + edges;
            partition_stats.mean_work = static_cast<double>(vertices + edges);
            result = pagerank::run_serial(loaded.graph, command.options);
            residual = pagerank::one_step_residual(
                loaded.graph, result.ranks, command.options.alpha);
        } else {
            const auto strategy = command.partition == "in-edges"
                                      ? pagerank::PartitionStrategy::in_edges
                                      : pagerank::PartitionStrategy::vertices;
            auto distribution = pagerank::distribute_csr(
                loaded.graph, strategy, MPI_COMM_WORLD);
            distribution_seconds = distribution.max_seconds;
            partition_stats = distribution.stats;
            distributed_graph = std::move(distribution.graph);
            const auto mode = command.mode == "mpi"
                                  ? pagerank::ParallelMode::mpi
                                  : pagerank::ParallelMode::hybrid;
            if (command.score_exchange == "demand") {
                demand_plan = pagerank::build_demand_exchange_plan(
                    distributed_graph, MPI_COMM_WORLD);
                result = pagerank::run_distributed_demand(
                    distributed_graph, demand_plan, command.options, mode,
                    MPI_COMM_WORLD);
                residual = pagerank::distributed_one_step_residual_demand(
                    distributed_graph, demand_plan, result.ranks,
                    command.options.alpha, mode, MPI_COMM_WORLD);
                exchange_metrics.dependency_seconds = demand_plan.build_seconds;
                exchange_metrics.remote_dependencies_total =
                    demand_plan.remote_dependencies_total;
                exchange_metrics.remote_dependencies_max =
                    demand_plan.remote_dependencies_max;
                exchange_metrics.remote_dependencies_mean =
                    demand_plan.remote_dependencies_mean;
                exchange_metrics.request_partners_max =
                    demand_plan.request_partners_max;
                exchange_metrics.request_partners_mean =
                    demand_plan.request_partners_mean;
                exchange_metrics.score_values_per_iteration =
                    demand_plan.score_values_per_iteration;
                exchange_metrics.score_state_bytes_max =
                    demand_plan.state_bytes_max;
            } else {
                result = pagerank::run_distributed(
                    distributed_graph, command.options, mode, MPI_COMM_WORLD);
                residual = pagerank::distributed_one_step_residual(
                    distributed_graph, result.ranks, command.options.alpha,
                    mode, MPI_COMM_WORLD);
                const std::uint64_t remote_values =
                    static_cast<std::uint64_t>(vertices) *
                    static_cast<std::uint64_t>(size - 1);
                exchange_metrics.remote_dependencies_total = remote_values;
                exchange_metrics.remote_dependencies_max =
                    vertices - partition_stats.min_vertices;
                exchange_metrics.remote_dependencies_mean =
                    static_cast<double>(remote_values) / size;
                exchange_metrics.request_partners_max = size - 1;
                exchange_metrics.request_partners_mean = size - 1;
                exchange_metrics.score_values_per_iteration = remote_values;
                exchange_metrics.score_state_bytes_max =
                    (2ULL * vertices +
                     static_cast<std::uint64_t>(partition_stats.max_vertices)) *
                    sizeof(double);
            }
            exchange_metrics.score_bytes_per_iteration =
                exchange_metrics.score_values_per_iteration * sizeof(double);
        }

        double rank_sum =
            std::accumulate(result.ranks.begin(), result.ranks.end(), 0.0);
        if (result.ranks_are_local) {
            double global_sum = 0.0;
            MPI_Allreduce(&rank_sum, &global_sum, 1, MPI_DOUBLE, MPI_SUM,
                          MPI_COMM_WORLD);
            rank_sum = global_sum;
        }
        const double sum_error = std::fabs(rank_sum - 1.0);
        double serial_error = 0.0;
        const std::size_t expected_rank_count =
            result.ranks_are_local
                ? static_cast<std::size_t>(distributed_graph.local_vertex_count())
                : vertices;
        bool valid = result.ranks.size() == expected_rank_count &&
                     result.iterations > 0 &&
                     std::isfinite(result.last_change) &&
                     std::isfinite(residual) && sum_error < 1e-9;
        for (const double score : result.ranks) {
            valid = valid && std::isfinite(score) && score >= 0.0;
        }
        if (result.converged) {
            valid = valid && residual <= std::max(1e-10, 2 * command.options.tolerance);
        }
        const bool need_complete_scores =
            result.ranks_are_local &&
            (command.verify || command.top_k > 0 || !command.output.empty());
        std::vector<double> complete_scores;
        if (need_complete_scores) {
            complete_scores = pagerank::gather_complete_scores(
                distributed_graph, result.ranks, MPI_COMM_WORLD);
        } else if (rank == 0 && !result.ranks_are_local) {
            complete_scores = result.ranks;
        }
        if (command.verify && command.mode != "serial" && rank == 0) {
            serial_error = l1_distance(complete_scores, serial_reference.ranks);
            const double allowed = std::max(
                1e-10, 2 * command.options.tolerance /
                           (1 - command.options.alpha));
            valid = valid && serial_error <= allowed;
        }
        valid = all_true(valid, MPI_COMM_WORLD);

        const std::uint64_t local_peak_rss = peak_rss_bytes();
        std::uint64_t peak_rss_max = 0;
        MPI_Reduce(&local_peak_rss, &peak_rss_max, 1, MPI_UINT64_T, MPI_MAX, 0,
                   MPI_COMM_WORLD);

        if (rank == 0) {
            std::cout << std::setprecision(12)
                      << "mode                  = " << command.mode << '\n'
                      << "dataset               = " << command.input << '\n'
                      << "mpi ranks             = " << size << '\n'
                      << "threads per rank      = " << command.threads << '\n'
                      << "partition             = " << command.partition << '\n'
                      << "score exchange        = " << command.score_exchange << '\n'
                      << "vertices              = " << vertices << '\n'
                      << "edges                 = " << edges << '\n'
                      << "load seconds          = " << loaded.load_seconds << '\n'
                      << "distribution seconds  = " << distribution_seconds << '\n'
                      << "dependency seconds    = "
                      << exchange_metrics.dependency_seconds << '\n'
                      << "solve seconds         = " << result.solve_seconds << '\n'
                      << "compute seconds (max) = "
                      << result.compute_max_seconds << '\n'
                      << "Allreduce seconds (max)= "
                      << result.allreduce_max_seconds << '\n'
                      << "Allgatherv seconds (max)= "
                      << result.allgatherv_max_seconds << '\n'
                      << "score exchange seconds (max)= "
                      << result.score_exchange_max_seconds << '\n'
                      << "score pack seconds (max)= "
                      << result.score_pack_max_seconds << '\n'
                      << "score values / iteration= "
                      << exchange_metrics.score_values_per_iteration << '\n'
                      << "score state bytes (max)= "
                      << exchange_metrics.score_state_bytes_max << '\n'
                      << "update imbalance      = "
                      << (result.update_mean_seconds > 0.0
                              ? result.update_max_seconds /
                                    result.update_mean_seconds
                              : 0.0)
                      << '\n'
                      << "work imbalance        = "
                      << (partition_stats.mean_work > 0.0
                              ? partition_stats.max_work /
                                    partition_stats.mean_work
                              : 0.0)
                      << '\n'
                      << "iterations            = " << result.iterations << '\n'
                      << "converged             = "
                      << (result.converged ? "yes" : "no") << '\n'
                      << "last L1 change        = " << result.last_change << '\n'
                      << "one-step residual     = " << residual << '\n'
                      << "rank sum error        = " << sum_error << '\n';
            if (command.verify && command.mode != "serial") {
                std::cout << "serial L1 error       = " << serial_error << '\n';
            }
            std::cout << "validation            = " << (valid ? "PASS" : "FAIL")
                      << '\n';

            const std::size_t top_k =
                std::min(command.top_k, complete_scores.size());
            std::vector<std::size_t> order(complete_scores.size());
            std::iota(order.begin(), order.end(), 0);
            std::partial_sort(
                order.begin(), order.begin() + top_k, order.end(),
                [&](std::size_t left, std::size_t right) {
                    if (complete_scores[left] != complete_scores[right]) {
                        return complete_scores[left] > complete_scores[right];
                    }
                    return original_ids[left] < original_ids[right];
                });
            std::cout << "top " << top_k << " ranks:\n";
            for (std::size_t position = 0; position < top_k; ++position) {
                const std::size_t vertex = order[position];
                std::cout << "  " << position + 1
                          << ". original_id=" << original_ids[vertex]
                          << " rank=" << complete_scores[vertex] << '\n';
            }
            if (!command.output.empty()) {
                write_ranks(command.output, original_ids, complete_scores);
            }
            if (!command.metrics.empty()) {
                write_metrics(
                    command, size, vertices, edges, loaded.raw_edges,
                    loaded.duplicate_edges, dangling_vertices,
                    loaded.load_seconds, distribution_seconds, partition_stats,
                    result, residual, sum_error, serial_error, exchange_metrics,
                    peak_rss_max);
            }
        }
        exit_code = valid ? (result.converged ? 0 : 2) : 1;
    } catch (const std::exception& error) {
        std::cerr << "rank " << rank << " error: " << error.what() << '\n';
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }
    MPI_Finalize();
    return exit_code;
}
