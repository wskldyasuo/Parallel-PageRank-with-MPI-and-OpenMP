#!/usr/bin/env bash
set -euo pipefail

project_dir=${1:-$PWD}
runner="$project_dir/scripts/vanda/run_vanda_case.pbs"
manifest="$project_dir/results/vanda/submitted_jobs.tsv"

mkdir -p "$project_dir/results/vanda/raw" "$project_dir/results/vanda/logs"
printf 'job_id\tsuite\tdataset\ttotal_cpus\tnodes\tmemory_per_node\n' > "$manifest"

memory_for() {
    local dataset=$1
    local cpus=$2
    if (( cpus == 1 )); then
        case "$dataset" in
            google|berkstan) echo 4 ;;
            enwiki|uk) echo 8 ;;
        esac
    elif (( cpus <= 16 )); then
        case "$dataset" in
            google|berkstan) echo 8 ;;
            enwiki) echo 16 ;;
            uk) echo 20 ;;
        esac
    elif (( cpus == 32 )); then
        case "$dataset" in
            google|berkstan) echo 16 ;;
            enwiki) echo 32 ;;
            uk) echo 48 ;;
        esac
    else
        case "$dataset" in
            google|berkstan) echo 12 ;;
            enwiki) echo 24 ;;
            uk) echo 32 ;;
        esac
    fi
}

walltime_for() {
    local dataset=$1
    local suite=$2
    case "$dataset:$suite" in
        google:*|berkstan:*) echo 01:30:00 ;;
        enwiki:*) echo 03:00:00 ;;
        uk:fixed32) echo 05:00:00 ;;
        uk:strong) echo 03:00:00 ;;
    esac
}

submit_job() {
    local name=$1
    local suite=$2
    local dataset=$3
    local cpus=$4
    local nodes=$5
    local per_node_cpus=$6
    local mem=$7
    local walltime=$8

    local select="select=${nodes}:ncpus=${per_node_cpus}:mpiprocs=${per_node_cpus}:mem=${mem}gb"
    local variables="PROJECT_DIR=${project_dir},SUITE=${suite},DATASET_KEY=${dataset}"
    if [[ "$suite" == strong ]]; then
        variables+=",TOTAL_CPUS=${cpus}"
    fi

    local qsub_args=(
        -N "$name"
        -l "$select"
        -l "walltime=$walltime"
        -o "$project_dir/results/vanda/logs/${name}.out"
        -v "$variables"
    )
    if (( nodes > 1 )); then
        qsub_args+=(-l place=scatter)
    fi

    local job_id
    job_id=$(qsub "${qsub_args[@]}" "$runner")
    printf '%s\t%s\t%s\t%s\t%s\t%sgb\n' \
        "$job_id" "$suite" "$dataset" "$cpus" "$nodes" "$mem" | tee -a "$manifest"
}

datasets=(google berkstan enwiki uk)

# Fixed-total decomposition: one 32-CPU node per dataset.
for dataset in "${datasets[@]}"; do
    mem=$(memory_for "$dataset" 32)
    walltime=$(walltime_for "$dataset" fixed32)
    submit_job "fx32_${dataset}" fixed32 "$dataset" 32 1 32 "$mem" "$walltime"
done

# Strong scaling: exact resources for 1, 2, 4, 8, 16, 32, 64, and 128 CPUs.
for dataset in "${datasets[@]}"; do
    for cpus in 1 2 4 8 16 32 64 128; do
        if (( cpus <= 32 )); then
            nodes=1
            per_node_cpus=$cpus
        else
            nodes=$((cpus / 32))
            per_node_cpus=32
        fi
        mem=$(memory_for "$dataset" "$cpus")
        walltime=$(walltime_for "$dataset" strong)
        submit_job "sc_${dataset}_${cpus}" strong "$dataset" "$cpus" \
            "$nodes" "$per_node_cpus" "$mem" "$walltime"
    done
done

echo "Submitted jobs are recorded in $manifest"
