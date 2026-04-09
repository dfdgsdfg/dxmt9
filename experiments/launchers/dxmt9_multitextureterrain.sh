#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9
export DXMT9_EXPERIMENT_WORKDIR
DXMT9_EXPERIMENT_WORKDIR=$(dirname -- "$DXMT9_EXPERIMENT_BINARY")
exp_run_wine_binary
