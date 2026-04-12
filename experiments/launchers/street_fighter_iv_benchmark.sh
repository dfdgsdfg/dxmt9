#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9
export DXMT9_EXPERIMENT_WORKDIR
DXMT9_EXPERIMENT_WORKDIR=$(dirname -- "$DXMT9_EXPERIMENT_BINARY")

if [[ -n "${DXMT9_EXPERIMENT_CX_BOTTLE:-}" ]]; then
  exp_require_var DXMT9_EXPERIMENT_WINE_BIN
  exp_require_var DXMT9_EXPERIMENT_BINARY
  exp_require_var DXMT9_EXPERIMENT_LOG
  dll_overrides=${DXMT9_EXPERIMENT_WINE_DLLOVERRIDES:-d3d9=b;d3dx9_41=b}
  wineserver_bin="${DXMT9_EXPERIMENT_WINE_ROOT:-}/bin/wineserver"
  (
    cd "$DXMT9_EXPERIMENT_WORKDIR"
    CX_ROOT="${DXMT9_EXPERIMENT_WINE_ROOT:-}" \
    CX_BOTTLE="$DXMT9_EXPERIMENT_CX_BOTTLE" \
    DXMT9_VALIDATE=1 \
    DXMT9_LOG="$DXMT9_EXPERIMENT_LOG" \
    "$DXMT9_EXPERIMENT_WINE_BIN" --bottle "$DXMT9_EXPERIMENT_CX_BOTTLE" --dll "$dll_overrides" --desktop sfivoracle,1280x720 "$DXMT9_EXPERIMENT_BINARY" &
    wine_pid=$!
    if [[ -x "$wineserver_bin" ]]; then
      CX_ROOT="${DXMT9_EXPERIMENT_WINE_ROOT:-}" \
      CX_BOTTLE="$DXMT9_EXPERIMENT_CX_BOTTLE" \
      "$wineserver_bin" -w || true
    fi
    wait "$wine_pid" || true
  )
else
  exp_run_wine_binary
fi
