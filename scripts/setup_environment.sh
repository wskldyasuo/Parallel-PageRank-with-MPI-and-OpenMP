#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "$0")/.." && pwd)
python_executable=${PYTHON:-python3}
venv_dir=${VENV_DIR:-"$project_dir/.venv"}

"$python_executable" - <<'PY'
import sys
if sys.version_info < (3, 12):
    raise SystemExit("Python 3.12 or newer is required")
print("Using Python", sys.version.split()[0])
PY

"$python_executable" -m venv "$venv_dir"
"$venv_dir/bin/python" -m pip install --upgrade pip
"$venv_dir/bin/python" -m pip install -r "$project_dir/requirements.txt"

echo "Python environment ready: $venv_dir"
echo "Activate it with: source '$venv_dir/bin/activate'"
