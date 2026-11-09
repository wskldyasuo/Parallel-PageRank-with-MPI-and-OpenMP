#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 5 ]]; then
    echo "Usage: $0 DATASET [REPETITIONS] [MPI_RANKS] [HYBRID_RANKS] [THREADS]" >&2
    exit 1
fi

dataset=$1
repetitions=${2:-5}
mpi_ranks=${3:-4}
hybrid_ranks=${4:-2}
threads=${5:-2}
program=./build/pagerank
output_dir=results/local
metrics_file=${METRICS_FILE:-$output_dir/all_modes.csv}
warmups=${WARMUPS:-1}

mkdir -p "$output_dir"

for warmup in $(seq 1 "$warmups"); do
    "$program" --input "$dataset" --mode serial --top-k 0 >/dev/null
    mpirun --bind-to none -np "$mpi_ranks" "$program" \
        --input "$dataset" --mode mpi --top-k 0 >/dev/null
    mpirun --bind-to none -np "$hybrid_ranks" "$program" \
        --input "$dataset" --mode hybrid --threads "$threads" \
        --top-k 0 >/dev/null
done

for repetition in $(seq 1 "$repetitions"); do
    "$program" --input "$dataset" --mode serial \
        --run-id "serial-$repetition" --metrics "$metrics_file" --top-k 0
    mpirun --bind-to none -np "$mpi_ranks" "$program" \
        --input "$dataset" --mode mpi \
        --run-id "mpi-$repetition" --metrics "$metrics_file" --top-k 0
    mpirun --bind-to none -np "$hybrid_ranks" "$program" \
        --input "$dataset" --mode hybrid --threads "$threads" \
        --run-id "hybrid-$repetition" --metrics "$metrics_file" --top-k 0
done

echo "Wrote $metrics_file"
