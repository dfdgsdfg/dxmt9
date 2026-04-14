#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9
export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR=$(dirname -- "$DXMT_EXPERIMENT_BINARY")

if [[ -n "${DXMT_EXPERIMENT_CX_BOTTLE:-}" ]]; then
  exp_require_var DXMT_EXPERIMENT_WINE_BIN
  exp_require_var DXMT_EXPERIMENT_BINARY
  exp_require_var DXMT_EXPERIMENT_LOG
  dll_overrides=${DXMT_EXPERIMENT_WINE_DLLOVERRIDES:-d3d9=b;d3dx9_41=b}
  wineserver_bin="${DXMT_EXPERIMENT_WINE_ROOT:-}/bin/wineserver"
  (
    cd "$DXMT_EXPERIMENT_WORKDIR"
    log_dir=$(dirname -- "$DXMT_EXPERIMENT_LOG")
    CX_ROOT="${DXMT_EXPERIMENT_WINE_ROOT:-}" \
    CX_BOTTLE="$DXMT_EXPERIMENT_CX_BOTTLE" \
    DXMT_VALIDATE=1 \
    DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-debug}" \
    DXMT_LOG_PATH="$log_dir" \
    "$DXMT_EXPERIMENT_WINE_BIN" --bottle "$DXMT_EXPERIMENT_CX_BOTTLE" --dll "$dll_overrides" --desktop sfivoracle,1280x720 "$DXMT_EXPERIMENT_BINARY" &
    wine_pid=$!
    if [[ -x "$wineserver_bin" ]]; then
      CX_ROOT="${DXMT_EXPERIMENT_WINE_ROOT:-}" \
      CX_BOTTLE="$DXMT_EXPERIMENT_CX_BOTTLE" \
      "$wineserver_bin" -w || true
    fi
    wait "$wine_pid" || true
  )
else
  exp_run_wine_binary
fi
