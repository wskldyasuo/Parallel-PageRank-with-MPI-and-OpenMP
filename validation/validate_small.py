#!/usr/bin/env python3
"""Independently validate the C++ executable against a direct linear solve."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
EXECUTABLE = ROOT / "build" / "pagerank"
GRAPH_FILE = ROOT / "tests" / "data" / "dangling_graph.txt"
ALPHA = 0.85


def direct_solution() -> np.ndarray:
    vertex_count = 4
    edges = [(0, 1), (0, 2), (1, 2), (2, 0), (2, 3)]
    out_degree = np.zeros(vertex_count, dtype=np.int64)
    for source, _ in edges:
        out_degree[source] += 1

    transition = np.zeros((vertex_count, vertex_count), dtype=np.float64)
    for source, target in edges:
        transition[target, source] += 1.0 / out_degree[source]
    for source in range(vertex_count):
        if out_degree[source] == 0:
            transition[:, source] = 1.0 / vertex_count

    system = np.eye(vertex_count) - ALPHA * transition
    teleport = np.full(vertex_count, (1.0 - ALPHA) / vertex_count)
    return np.linalg.solve(system, teleport)


def cpp_solution() -> np.ndarray:
    if not EXECUTABLE.exists():
        raise RuntimeError("Run 'make' before validation.")
    with tempfile.TemporaryDirectory(prefix="dsa5208-pagerank-") as directory:
        output = Path(directory) / "ranks.tsv"
        subprocess.run(
            [
                str(EXECUTABLE),
                "--input",
                str(GRAPH_FILE),
                "--mode",
                "serial",
                "--output",
                str(output),
                "--top-k",
                "0",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        rows = output.read_text(encoding="utf-8").splitlines()[1:]
        parsed = {int(row.split("\t")[0]): float(row.split("\t")[1]) for row in rows}
        return np.array([parsed[index] for index in range(4)], dtype=np.float64)


def main() -> None:
    expected = direct_solution()
    actual = cpp_solution()
    difference = np.abs(actual - expected)
    l1_error = float(difference.sum())
    max_error = float(difference.max())
    sum_error = abs(float(actual.sum()) - 1.0)

    print("Independent small-graph validation")
    print(f"direct ranks = {expected}")
    print(f"C++ ranks    = {actual}")
    print(f"L1 error     = {l1_error:.3e}")
    print(f"max error    = {max_error:.3e}")
    print(f"sum error    = {sum_error:.3e}")

    if l1_error > 1e-10 or max_error > 1e-10 or sum_error > 1e-10:
        raise SystemExit("Validation failed.")
    print("Validation passed.")


if __name__ == "__main__":
    main()

