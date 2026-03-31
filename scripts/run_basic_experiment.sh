#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

bash "$script_dir/build_basic_hlsl.sh"
python3 "$repo_root/scripts/run_experiment.py" run dx-sdk-basichlsl "$@"
