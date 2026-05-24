#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

# Wild-experiment wrapper. Wine + prefix + install paths are managed by
# scripts/wine/* + experiments/CATALOGUE.toml; see specs/experiments/runtime/.
# All flags (--wine-id, --rebuild-prefix, --allow-non-vanilla, --wine-manifest)
# are passed through to run_experiment.py.

exec python3 "$repo_root/scripts/run_apps/run_experiment.py" run \
  app-d3d9-sfiv-benchmark "$@"
