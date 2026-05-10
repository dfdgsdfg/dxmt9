#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9

# Set the working directory to the physical install location so relative
# resource lookups inside the game see the correct files.  The harness
# bootstraps a dosdevices/d: symlink that points here, so D:\... paths
# inside Wine resolve to this directory.
export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR="$exp_repo_root/experiments/apps_3rd/street-fighter-iv-benchmark"

exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" -benchmark
