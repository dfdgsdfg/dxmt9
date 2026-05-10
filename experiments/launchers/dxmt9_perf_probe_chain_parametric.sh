#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9
export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR=$(dirname -- "$DXMT_EXPERIMENT_BINARY")

# Pass-through for CHAIN_* tunables. Wine inherits the parent environment,
# so simply re-exporting (with defaults) makes them visible to the child
# Win32 process. Defaults match the in-app defaults so unset values keep
# working. The W4 A/B harness sweeps these alongside DXMT9_MID_CHUNK_*
# knobs which the encoder consumes directly — this launcher does NOT
# interpose on the cap policy.
export CHAIN_LENGTH=${CHAIN_LENGTH:-4}
export CHAIN_DRAWS_PER_PASS=${CHAIN_DRAWS_PER_PASS:-50}
export CHAIN_ITERATIONS=${CHAIN_ITERATIONS:-1000}

exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY"
