#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

exec bash "$script_dir/../run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh" --host crossover "$@"
