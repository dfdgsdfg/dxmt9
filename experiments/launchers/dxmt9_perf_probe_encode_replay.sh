#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9
export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR=$(dirname -- "$DXMT_EXPERIMENT_BINARY")

# Pass-through for ENCODE_REPLAY_* tunables. Wine inherits the parent
# environment, so simply re-exporting (with defaults) makes them visible
# to the child Win32 process. Defaults match the in-app defaults so that
# unset values continue to work.
export ENCODE_REPLAY_DRAWS_PER_FRAME=${ENCODE_REPLAY_DRAWS_PER_FRAME:-100}
export ENCODE_REPLAY_ITERATIONS=${ENCODE_REPLAY_ITERATIONS:-1000}

exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY"
