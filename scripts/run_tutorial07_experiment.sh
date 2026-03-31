#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

bash "$repo_root/scripts/build_tutorial07.sh"
python3 "$repo_root/scripts/run_experiment.py" run dx-sdk-tutorial07 "$@"
