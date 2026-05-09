#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

exec bash "$script_dir/../run_apps/run_sfiv_benchmark_experiment.sh" --host crossover "$@"
