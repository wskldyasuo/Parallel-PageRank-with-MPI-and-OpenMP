#!/usr/bin/env python3
"""Compare the complete C++ rank vector with an independent NetworkX result."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import tempfile
import time
from pathlib import Path

import networkx as nx
import numpy as np
import scipy


ROOT = Path(__file__).resolve().parents[1]
EXECUTABLE = ROOT / "build" / "pagerank"


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument(
        "--cpp-input",
        type=Path,
        help="Optional CSR input for C++; NetworkX still reads --input.",
    )
    parser.add_argument(
        "--cpp-id-mode",
        choices=("original", "dense-sorted"),
        default="original",
        help=(
            "Use original IDs from the C++ output, or align dense C++ IDs "
            "with the sorted original IDs from the raw edge list."
        ),
    )
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--alpha", type=float, default=0.85)
    parser.add_argument("--reference-epsilon", type=float, default=1e-13)
    parser.add_argument("--max-iterations", type=int, default=1000)
    parser.add_argument(
        "--cpp-mode", choices=("serial", "mpi", "hybrid"), default="serial"
    )
    parser.add_argument(
        "--score-exchange", choices=("full", "demand"), default="full"
    )
    parser.add_argument("--mpi-ranks", type=int, default=4)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument(
        "--partition", choices=("vertices", "in-edges"), default="vertices"
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_cpp_ranks(
    path: Path, alpha: float, max_iterations: int, cpp_mode: str,
    score_exchange: str, mpi_ranks: int, threads: int, partition: str
) -> tuple[dict[int, float], float]:
    with tempfile.TemporaryDirectory(prefix="dsa5208-networkx-") as directory:
        output = Path(directory) / "cpp_ranks.tsv"
        start = time.perf_counter()
        command = [
            str(EXECUTABLE), "--input", str(path), "--mode", cpp_mode,
            "--output", str(output), "--alpha", str(alpha),
            "--max-iterations", str(max_iterations), "--top-k", "0",
            "--partition", partition, "--score-exchange", score_exchange,
        ]
        if cpp_mode == "hybrid":
            command.extend(("--threads", str(threads)))
        if cpp_mode != "serial":
            command = [
                "mpirun", "--bind-to", "none", "-np", str(mpi_ranks),
                *command,
            ]
        subprocess.run(
            command,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        cpp_seconds = time.perf_counter() - start
        ranks: dict[int, float] = {}
        with output.open(encoding="utf-8") as stream:
            next(stream)
            for line in stream:
                original_id, rank = line.rstrip().split("\t")
                ranks[int(original_id)] = float(rank)
        return ranks, cpp_seconds


def main() -> None:
    options = arguments()
    if not EXECUTABLE.exists():
        raise SystemExit("Run 'make' before validation.")
    cpp_input = options.cpp_input or options.input

    start = time.perf_counter()
    graph = nx.read_edgelist(
        options.input,
        comments="#",
        create_using=nx.DiGraph,
        nodetype=int,
        data=False,
    )
    graph_seconds = time.perf_counter() - start
    vertex_count = graph.number_of_nodes()

    uniform = {vertex: 1.0 / vertex_count for vertex in graph}
    start = time.perf_counter()
    reference = nx.pagerank(
        graph,
        alpha=options.alpha,
        weight=None,
        personalization=uniform,
        dangling=uniform,
        nstart=uniform,
        tol=options.reference_epsilon / vertex_count,
        max_iter=options.max_iterations,
    )
    reference_seconds = time.perf_counter() - start
    cpp, cpp_seconds = read_cpp_ranks(
        cpp_input, options.alpha, options.max_iterations, options.cpp_mode,
        options.score_exchange, options.mpi_ranks, options.threads,
        options.partition
    )

    if options.cpp_id_mode == "dense-sorted":
        original_ids = sorted(reference)
        expected_dense_ids = set(range(len(original_ids)))
        if set(cpp) != expected_dense_ids:
            raise SystemExit("C++ IDs are not the expected dense zero-based range.")
        cpp = {
            original_id: cpp[dense_id]
            for dense_id, original_id in enumerate(original_ids)
        }

    if set(cpp) != set(reference):
        raise SystemExit("C++ and NetworkX vertex sets differ.")
    differences = [abs(cpp[vertex] - reference[vertex]) for vertex in reference]
    l1_error = float(sum(differences))
    max_error = float(max(differences))
    cpp_sum_error = abs(sum(cpp.values()) - 1.0)

    report = {
        "networkx_input": str(options.input.resolve()),
        "networkx_input_sha256": sha256(options.input),
        "cpp_input": str(cpp_input.resolve()),
        "cpp_input_sha256": sha256(cpp_input),
        "cpp_id_mode": options.cpp_id_mode,
        "cpp_mode": options.cpp_mode,
        "score_exchange": options.score_exchange,
        "mpi_ranks": 1 if options.cpp_mode == "serial" else options.mpi_ranks,
        "threads_per_rank": options.threads if options.cpp_mode == "hybrid" else 1,
        "partition": options.partition,
        "vertices": vertex_count,
        "edges": graph.number_of_edges(),
        "alpha": options.alpha,
        "reference_global_epsilon": options.reference_epsilon,
        "max_iterations": options.max_iterations,
        "l1_error": l1_error,
        "max_error": max_error,
        "cpp_rank_sum_error": cpp_sum_error,
        "graph_load_seconds": graph_seconds,
        "networkx_seconds": reference_seconds,
        "cpp_seconds": cpp_seconds,
        "python_version": platform.python_version(),
        "networkx_version": nx.__version__,
        "numpy_version": np.__version__,
        "scipy_version": scipy.__version__,
        "accepted": l1_error <= 1e-9 and max_error <= 1e-10 and cpp_sum_error <= 1e-10,
    }
    options.report.parent.mkdir(parents=True, exist_ok=True)
    options.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(json.dumps(report, indent=2))
    if not report["accepted"]:
        raise SystemExit("NetworkX validation failed acceptance thresholds.")


if __name__ == "__main__":
    main()

