#!/usr/bin/env bash
set -euo pipefail

project_dir=${1:-$PWD}
runner="$project_dir/scripts/vanda/run_vanda_e3.pbs"
manifest="$project_dir/results/vanda/e3/submitted_jobs.tsv"

mkdir -p "$project_dir/results/vanda/e3/raw" "$project_dir/results/vanda/e3/logs"
printf 'job_id\tsuite\tdataset\ttotal_cpus\tnodes\tmemory_per_node\n' > "$manifest"

memory_for() {
    case "$1" in
        google|berkstan) echo 16 ;;
        enwiki) echo 32 ;;
        uk) echo 48 ;;
    esac
}

walltime_for() {
    case "$1" in
        google|berkstan) echo 02:00:00 ;;
        enwiki) echo 04:00:00 ;;
        uk) echo 06:00:00 ;;
    esac
}

for dataset in google berkstan enwiki uk; do
    mem=$(memory_for "$dataset")
    walltime=$(walltime_for "$dataset")
    name="e3_${dataset}"
    job_id=$(qsub \
        -N "$name" \
        -l "select=1:ncpus=32:mpiprocs=32:mem=${mem}gb" \
        -l "walltime=$walltime" \
        -o "$project_dir/results/vanda/e3/logs/${name}.out" \
        -v "PROJECT_DIR=${project_dir},DATASET_KEY=${dataset}" \
        "$runner")
    printf '%s\te3\t%s\t32\t1\t%sgb\n' \
        "$job_id" "$dataset" "$mem" | tee -a "$manifest"
done

echo "Submitted E3 jobs are recorded in $manifest"
