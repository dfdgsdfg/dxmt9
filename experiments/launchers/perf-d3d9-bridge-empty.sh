#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

# V1 boundary B2 (audit item (b)) — bridge-ABI throughput probe.
# Pass BRIDGE_EMPTY_ITERATIONS through to the in-tree probe binary so the
# inner loop count can be tuned per run; the binary itself defaults to
# 100000 when the env var is unset (read-once at startup).
export BRIDGE_EMPTY_ITERATIONS=${BRIDGE_EMPTY_ITERATIONS:-100000}

exp_stage_dxmt9
export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR=$(dirname -- "$DXMT_EXPERIMENT_BINARY")
exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY"
