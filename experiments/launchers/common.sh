#!/usr/bin/env bash
set -euo pipefail

exp_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)

exp_log() {
  printf '[experiment] %s\n' "$*" >&2
}

exp_require_var() {
  local name=$1
  if [[ -z "${!name:-}" ]]; then
    exp_log "missing required environment variable: $name"
    exit 2
  fi
}

exp_stage_dxmt9() {
  exp_require_var DXMT9_EXPERIMENT_PREFIX
  exp_require_var DXMT9_EXPERIMENT_PE_BUILD_DIR
  exp_require_var DXMT9_EXPERIMENT_UNIX_BUILD_DIR

  local -a cmd
  cmd=(bash "$exp_repo_root/scripts/install_heroic_wine.sh"
       --prefix "$DXMT9_EXPERIMENT_PREFIX"
       --pe-build-dir "$DXMT9_EXPERIMENT_PE_BUILD_DIR"
       --unix-build-dir "$DXMT9_EXPERIMENT_UNIX_BUILD_DIR")

  if [[ -n "${DXMT9_EXPERIMENT_WINE_ROOT:-}" ]]; then
    cmd+=(--wine-root "$DXMT9_EXPERIMENT_WINE_ROOT")
  fi

  exp_log "staging dxmt9 runtime into prefix $DXMT9_EXPERIMENT_PREFIX"
  "${cmd[@]}"
}

exp_run_wine_binary() {
  exp_require_var DXMT9_EXPERIMENT_WINE_BIN
  exp_require_var DXMT9_EXPERIMENT_PREFIX
  exp_require_var DXMT9_EXPERIMENT_BINARY
  exp_require_var DXMT9_EXPERIMENT_LOG

  local binary=${1:-"$DXMT9_EXPERIMENT_BINARY"}
  shift || true
  local dll_overrides=${DXMT9_EXPERIMENT_WINE_DLLOVERRIDES:-d3d9=n,b}

  exp_log "running $binary"
  WINEPREFIX="$DXMT9_EXPERIMENT_PREFIX" \
  WINEDLLOVERRIDES="$dll_overrides" \
  DXMT9_VALIDATE=1 \
  DXMT9_LOG="$DXMT9_EXPERIMENT_LOG" \
  "$DXMT9_EXPERIMENT_WINE_BIN" "$binary" "$@"
}
