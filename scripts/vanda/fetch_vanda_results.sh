#!/usr/bin/env bash
set -euo pipefail

remote=${1:-${USER}@vanda.nus.edu.sg}
remote_project=${2:-DSA5208_Project3}
local_project=${3:-$(cd "$(dirname "$0")/../.." && pwd)}

mkdir -p "$local_project/results/vanda/raw" "$local_project/results/vanda/logs"
rsync -av "$remote:$remote_project/results/vanda/raw/" \
    "$local_project/results/vanda/raw/"
rsync -av "$remote:$remote_project/results/vanda/logs/" \
    "$local_project/results/vanda/logs/"
rsync -av "$remote:$remote_project/results/vanda/submitted_jobs.tsv" \
    "$local_project/results/vanda/"
