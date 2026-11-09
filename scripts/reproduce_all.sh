#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$project_dir"

missing_data=0
for graph in web-Google web-BerkStan enwiki-2013 uk-2002; do
    if [[ ! -f "data/processed/$graph.prcsr" ]]; then
        echo "Missing data/processed/$graph.prcsr" >&2
        missing_data=1
    fi
done
if [[ "$missing_data" == 1 ]]; then
    echo "Run 'bash scripts/prepare_data.sh all' before reproducing the notebooks." >&2
    exit 2
fi

venv_dir=${VENV_DIR:-"$project_dir/.venv"}
if [[ ! -x "$venv_dir/bin/python" ]]; then
    "$project_dir/scripts/setup_environment.sh"
fi

make -j"${BUILD_JOBS:-2}" all
make test
make test-parallel

for notebook in \
    00_environment_data_check.ipynb \
    01_local_experiments.ipynb \
    02_vanda_experiments.ipynb \
    03_load_balancing_experiment.ipynb \
    04_phase_timing_experiment.ipynb \
    05_demand_exchange_experiment.ipynb; do
    echo "Executing $notebook"
    "$venv_dir/bin/jupyter" execute "$notebook" --inplace
done

"$venv_dir/bin/python" scripts/build_report.py
echo "Reproduction complete. Final report: output/pdf/DSA5208_Project3_Yan_Wan_Wang.pdf"
