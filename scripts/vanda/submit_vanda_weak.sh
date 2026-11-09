#!/usr/bin/env bash
set -euo pipefail

project_dir=${1:-$PWD}
weak_data_dir=${2:-/scratch/$USER/DSA5208_Project3/data/weak}
prep="$project_dir/scripts/vanda/prepare_weak_data.pbs"
runner="$project_dir/scripts/vanda/run_vanda_weak.pbs"
result_root="$project_dir/results/vanda/weak"
manifest="$result_root/submitted_jobs.tsv"

mkdir -p "$result_root/raw" "$result_root/logs"
printf 'job_id\tfamily\tscale\ttotal_cpus\tnodes\tmemory_per_node\tdependency\n' > "$manifest"

prep_id=$(qsub -N weak_prepare \
    -l select=1:ncpus=4:mpiprocs=4:mem=16gb \
    -l walltime=04:00:00 \
    -o "$result_root/logs/weak_prepare.out" \
    -v "PROJECT_DIR=$project_dir,WEAK_DATA_DIR=$weak_data_dir" "$prep")
echo "Preparation job: $prep_id"

cpus=(4 8 16 32 64 128)
replicas=(1 2 4 8 16 32)
fractions=(1of32 1of16 1of8 1of4 1of2 1of1)
berk_mem=(8 8 12 16 24 32)
uk_mem=(8 8 12 20 24 32)
walltimes=(01:00:00 01:00:00 01:00:00 01:30:00 02:00:00 03:00:00)

submit_case() {
    local family=$1 scale=$2 total=$3 mem=$4 walltime=$5
    local nodes per_node name variables id
    if (( total <= 32 )); then nodes=1; per_node=$total; else nodes=$((total / 32)); per_node=32; fi
    name="wk_${family}_${total}"
    variables="PROJECT_DIR=$project_dir,WEAK_DATA_DIR=$weak_data_dir,FAMILY=$family,SCALE=$scale,TOTAL_CPUS=$total"
    args=(-N "$name" -l "select=${nodes}:ncpus=${per_node}:mpiprocs=${per_node}:mem=${mem}gb"
          -l "walltime=$walltime" -W "depend=afterok:${prep_id}"
          -o "$result_root/logs/${name}.out" -v "$variables")
    (( nodes > 1 )) && args+=(-l place=scatter)
    id=$(qsub "${args[@]}" "$runner")
    printf '%s\t%s\t%s\t%s\t%s\t%sgb\t%s\n' \
        "$id" "$family" "$scale" "$total" "$nodes" "$mem" "$prep_id" | tee -a "$manifest"
}

for index in "${!cpus[@]}"; do
    submit_case berkstan "${replicas[$index]}" "${cpus[$index]}" \
        "${berk_mem[$index]}" "${walltimes[$index]}"
    submit_case uk "${fractions[$index]}" "${cpus[$index]}" \
        "${uk_mem[$index]}" "${walltimes[$index]}"
done

echo "Manifest: $manifest"
