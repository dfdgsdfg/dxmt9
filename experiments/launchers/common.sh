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
  if [[ "${DXMT_EXPERIMENT_SKIP_STAGE:-}" == "1" ]]; then
    exp_log "skipping dxmt9 staging"
    return 0
  fi

  exp_require_var DXMT_EXPERIMENT_PREFIX
  exp_require_var DXMT_EXPERIMENT_PE_BUILD_DIR
  exp_require_var DXMT_EXPERIMENT_UNIX_BUILD_DIR

  local -a cmd
  cmd=(bash "$exp_repo_root/scripts/install_heroic_wine.sh"
       --prefix "$DXMT_EXPERIMENT_PREFIX"
       --pe-build-dir "$DXMT_EXPERIMENT_PE_BUILD_DIR"
       --unix-build-dir "$DXMT_EXPERIMENT_UNIX_BUILD_DIR")

  if [[ -n "${DXMT_EXPERIMENT_WINE_ROOT:-}" ]]; then
    cmd+=(--wine-root "$DXMT_EXPERIMENT_WINE_ROOT")
  fi

  exp_log "staging dxmt9 runtime into prefix $DXMT_EXPERIMENT_PREFIX"
  "${cmd[@]}"
}

exp_run_wine_binary() {
  exp_require_var DXMT_EXPERIMENT_WINE_BIN
  exp_require_var DXMT_EXPERIMENT_PREFIX
  exp_require_var DXMT_EXPERIMENT_BINARY
  exp_require_var DXMT_EXPERIMENT_LOG

  local binary=${1:-"$DXMT_EXPERIMENT_BINARY"}
  shift || true
  local dll_overrides=${DXMT_EXPERIMENT_WINE_DLLOVERRIDES:-d3d9=n,b}
  local workdir=${DXMT_EXPERIMENT_WORKDIR:-$exp_repo_root}
  local log_dir
  log_dir=$(dirname -- "$DXMT_EXPERIMENT_LOG")

  exp_log "running $binary"
  if [[ -n "${DXMT_EXPERIMENT_CX_BOTTLE:-}" ]]; then
    (
      cd "$workdir"
      CX_ROOT="${DXMT_EXPERIMENT_WINE_ROOT:-}" \
      CX_BOTTLE="$DXMT_EXPERIMENT_CX_BOTTLE" \
      DXMT_VALIDATE=1 \
      DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-debug}" \
      DXMT_LOG_PATH="$log_dir" \
      "$DXMT_EXPERIMENT_WINE_BIN" --bottle "$DXMT_EXPERIMENT_CX_BOTTLE" --wait-all --dll "$dll_overrides" "$binary" "$@"
    )
  else
    (
      cd "$workdir"
      WINEPREFIX="$DXMT_EXPERIMENT_PREFIX" \
      WINEDLLOVERRIDES="$dll_overrides" \
      DXMT_VALIDATE=1 \
      DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-debug}" \
      DXMT_LOG_PATH="$log_dir" \
      "$DXMT_EXPERIMENT_WINE_BIN" "$binary" "$@"
    )
  fi
}
