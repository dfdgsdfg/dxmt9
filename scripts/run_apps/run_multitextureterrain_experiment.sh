#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

bash "$repo_root/scripts/build_apps/build_multitextureterrain.sh"
python3 "$repo_root/scripts/run_apps/run_experiment.py" run dxmt9-multitexture-terrain "$@"
