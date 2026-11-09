#!/usr/bin/env bash
set -euo pipefail

project_dir=${1:-$PWD}
runner="$project_dir/scripts/vanda/run_vanda_phase.pbs"
manifest="$project_dir/results/vanda/phase/submitted_jobs.tsv"
mkdir -p "$project_dir/results/vanda/phase/raw" "$project_dir/results/vanda/phase/logs"
printf 'job_id\tsuite\tdataset\ttotal_cpus\tnodes\tmemory_per_node\n' > "$manifest"

memory_for() {
    local dataset=$1 cpus=$2
    if (( cpus <= 32 )); then
        case "$dataset" in
            google|berkstan) echo 16 ;; enwiki) echo 32 ;; uk) echo 48 ;;
        esac
    else
        case "$dataset" in
            google|berkstan) echo 12 ;; enwiki) echo 24 ;; uk) echo 32 ;;
        esac
    fi
}

walltime_for() {
    case "$1" in
        google|berkstan) echo 02:00:00 ;;
        enwiki) echo 04:00:00 ;;
        uk) echo 06:00:00 ;;
    esac
}

for dataset in google berkstan enwiki uk; do
    for cpus in 4 32 64 128; do
        if (( cpus <= 32 )); then nodes=1; per_node_cpus=$cpus
        else nodes=$((cpus / 32)); per_node_cpus=32
        fi
        mem=$(memory_for "$dataset" "$cpus")
        walltime=$(walltime_for "$dataset")
        name="ph_${dataset}_${cpus}"
        select="select=${nodes}:ncpus=${per_node_cpus}:mpiprocs=${per_node_cpus}:mem=${mem}gb"
        args=(
            -N "$name" -l "$select" -l "walltime=$walltime"
            -o "$project_dir/results/vanda/phase/logs/${name}.out"
            -v "PROJECT_DIR=${project_dir},DATASET_KEY=${dataset},TOTAL_CPUS=${cpus}"
        )
        if (( nodes > 1 )); then args+=(-l place=scatter); fi
        job_id=$(qsub "${args[@]}" "$runner")
        printf '%s\tphase\t%s\t%s\t%s\t%sgb\n' \
            "$job_id" "$dataset" "$cpus" "$nodes" "$mem" | tee -a "$manifest"
    done
done

echo "Submitted phase-timing jobs are recorded in $manifest"
