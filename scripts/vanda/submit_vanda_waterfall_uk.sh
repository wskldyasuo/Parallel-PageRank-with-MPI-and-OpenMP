#!/usr/bin/env bash
set -euo pipefail

project_dir=${1:-$PWD}
build_job_id=${2:-}
manifest="$project_dir/results/vanda/waterfall_uk/submitted_jobs.tsv"
mkdir -p "$project_dir/results/vanda/waterfall_uk/raw" "$project_dir/results/vanda/waterfall_uk/logs"
qsub_args=(
    -o "$project_dir/results/vanda/waterfall_uk/logs/wf_uk.out"
    -v "PROJECT_DIR=${project_dir}"
)
if [[ -n "$build_job_id" ]]; then
    qsub_args+=(-W "depend=afterok:${build_job_id}")
fi
job_id=$(qsub "${qsub_args[@]}" \
    "$project_dir/scripts/vanda/run_vanda_waterfall_uk.pbs")
printf 'job_id\tdataset\tallocated_cpus\tlayouts\tnodes\tmemory_per_node\n' > "$manifest"
printf '%s\tuk-2002\t32\t1x1,32x1,2x16\t1\t48gb\n' "$job_id" | tee -a "$manifest"
