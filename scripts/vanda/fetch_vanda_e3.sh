#!/usr/bin/env bash
set -euo pipefail

remote=${1:-${USER}@vanda.nus.edu.sg}
remote_project=${2:-DSA5208_Project3}
local_project=${3:-$(cd "$(dirname "$0")/../.." && pwd)}

mkdir -p "$local_project/results/vanda/e3/raw" \
    "$local_project/results/vanda/e3/logs"
rsync -av "$remote:$remote_project/results/vanda/e3/raw/" \
    "$local_project/results/vanda/e3/raw/"
rsync -av "$remote:$remote_project/results/vanda/e3/logs/" \
    "$local_project/results/vanda/e3/logs/"
rsync -av "$remote:$remote_project/results/vanda/e3/submitted_jobs.tsv" \
    "$local_project/results/vanda/e3/"
