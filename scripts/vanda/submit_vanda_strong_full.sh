#!/usr/bin/env bash
set -euo pipefail

project_dir=${1:-$PWD}
runner="$project_dir/scripts/vanda/run_vanda_strong_full.pbs"
manifest="$project_dir/results/vanda/strong_full/submitted_jobs.tsv"
mkdir -p "$project_dir/results/vanda/strong_full/raw" "$project_dir/results/vanda/strong_full/logs"
printf 'job_id\tdataset\tscope\tmax_cpus\tnodes\tmemory_per_node\n' > "$manifest"

memory_for() {
    case "$1" in
        google|berkstan) echo 16 ;;
        enwiki) echo 32 ;;
        uk) echo 48 ;;
    esac
}

walltime_for() {
    case "$1:$2" in
        google:*|berkstan:*) echo 01:00:00 ;;
        enwiki:*) echo 02:00:00 ;;
        uk:single) echo 03:00:00 ;;
        uk:64|uk:128) echo 02:00:00 ;;
    esac
}

submit_one() {
    local dataset=$1 scope=$2 nodes=$3 max_cpus=$4
    local mem walltime name select variables job_id
    mem=$(memory_for "$dataset")
    walltime=$(walltime_for "$dataset" "$scope")
    name="sf_${dataset}_${scope}"
    select="select=${nodes}:ncpus=32:mpiprocs=32:mem=${mem}gb"
    variables="PROJECT_DIR=${project_dir},DATASET_KEY=${dataset},SWEEP_SCOPE=${scope}"
    args=(-N "$name" -l "$select" -l "walltime=$walltime"
          -o "$project_dir/results/vanda/strong_full/logs/${name}.out" -v "$variables")
    (( nodes > 1 )) && args+=(-l place=scatter)
    job_id=$(qsub "${args[@]}" "$runner")
    printf '%s\t%s\t%s\t%s\t%s\t%sgb\n' \
        "$job_id" "$dataset" "$scope" "$max_cpus" "$nodes" "$mem" | tee -a "$manifest"
}

for dataset in google berkstan enwiki uk; do
    submit_one "$dataset" single 1 32
    submit_one "$dataset" 64 2 64
    submit_one "$dataset" 128 4 128
done
