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

exp_wine_unix_dir() {
  local root=${DXMT_EXPERIMENT_WINE_ROOT:-}
  if [[ -z "$root" ]]; then
    return 0
  fi

  local arch
  case "$(uname -m)" in
    arm64|aarch64)
      arch=aarch64-unix
      ;;
    x86_64)
      arch=x86_64-unix
      ;;
    *)
      arch="$(uname -m)-unix"
      ;;
  esac

  if [[ -d "$root/lib/wine/$arch" ]]; then
    printf '%s\n' "$root/lib/wine/$arch"
  elif [[ -d "$root/lib/wine/x86_64-unix" ]]; then
    printf '%s\n' "$root/lib/wine/x86_64-unix"
  elif [[ -d "$root/lib/wine/aarch64-unix" ]]; then
    printf '%s\n' "$root/lib/wine/aarch64-unix"
  fi
}

exp_resolve_profile_defaults() {
  EXP_PROFILE_NAME=$(printf '%s' "${DXMT_EXPERIMENT_PROFILE:-${DXMT_PROFILE:-debug}}" | tr '[:upper:]' '[:lower:]')

  case "$EXP_PROFILE_NAME" in
    debug)
      EXP_DEFAULT_DXMT_VALIDATE=1
      EXP_DEFAULT_DXMT_LOG_LEVEL=debug
      EXP_DEFAULT_DXMT_PERF_COUNTERS=
      EXP_DEFAULT_DXMT_PERF_COUNTERS_PERIODIC_PRESENTS=
      EXP_DEFAULT_WINEDEBUG=
      EXP_DEFAULT_DXMT9_OFFLOAD_COMMIT_REPLAY=
      EXP_DEFAULT_DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=
      ;;
    perf)
      EXP_DEFAULT_DXMT_VALIDATE=0
      EXP_DEFAULT_DXMT_LOG_LEVEL=warn
      EXP_DEFAULT_DXMT_PERF_COUNTERS=1
      EXP_DEFAULT_DXMT_PERF_COUNTERS_PERIODIC_PRESENTS=60
      EXP_DEFAULT_WINEDEBUG=-all
      # Promoted pair (H195 / index-cache-locality idx-20 promotion proof):
      # offload absorbs the index-cache CPU tax; set either to 0 to opt out.
      EXP_DEFAULT_DXMT9_OFFLOAD_COMMIT_REPLAY=1
      EXP_DEFAULT_DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1
      ;;
    *)
      exp_log "invalid DXMT_EXPERIMENT_PROFILE: $EXP_PROFILE_NAME (expected debug or perf)"
      exit 2
      ;;
  esac
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
  cmd=(bash "$exp_repo_root/scripts/install/install_heroic_wine.sh"
       --prefix "$DXMT_EXPERIMENT_PREFIX"
       --pe-build-dir "$DXMT_EXPERIMENT_PE_BUILD_DIR"
       --unix-build-dir "$DXMT_EXPERIMENT_UNIX_BUILD_DIR")

  if [[ -n "${DXMT_EXPERIMENT_RUNTIME_PE_BUILD_DIR:-}" ]]; then
    cmd+=(--runtime-pe-build-dir "$DXMT_EXPERIMENT_RUNTIME_PE_BUILD_DIR")
  fi

  if [[ -n "${DXMT_EXPERIMENT_WOW64_PE_BUILD_DIR:-}" ]]; then
    cmd+=(--wow64-pe-build-dir "$DXMT_EXPERIMENT_WOW64_PE_BUILD_DIR")
  fi

  if [[ -n "${DXMT_EXPERIMENT_WOW64_RUNTIME_PE_BUILD_DIR:-}" ]]; then
    cmd+=(--wow64-runtime-pe-build-dir "$DXMT_EXPERIMENT_WOW64_RUNTIME_PE_BUILD_DIR")
  fi

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
  local dll_overrides=${DXMT_EXPERIMENT_WINE_DLLOVERRIDES:-d3d9,winemetal=n,b}
  local workdir=${DXMT_EXPERIMENT_WORKDIR:-$exp_repo_root}
  local log_dir
  log_dir=$(dirname -- "$DXMT_EXPERIMENT_LOG")
  local wine_unix_dir
  wine_unix_dir=$(exp_wine_unix_dir)
  local dyld_library_path=${DYLD_LIBRARY_PATH:-}
  if [[ -n "$wine_unix_dir" ]]; then
    dyld_library_path="$wine_unix_dir${dyld_library_path:+:$dyld_library_path}"
  fi
  local winemetal_so=${DXMT9_WINEMETAL_SO:-}
  if [[ -z "$winemetal_so" && -f "${DXMT_EXPERIMENT_UNIX_BUILD_DIR:-}/winemetal/unix/winemetal.so" ]]; then
    winemetal_so="$DXMT_EXPERIMENT_UNIX_BUILD_DIR/winemetal/unix/winemetal.so"
  fi
  exp_resolve_profile_defaults

  exp_log "running $binary profile=$EXP_PROFILE_NAME"
  if [[ -n "${DXMT_EXPERIMENT_CX_BOTTLE:-}" ]]; then
    (
      cd "$workdir"
      CX_ROOT="${DXMT_EXPERIMENT_WINE_ROOT:-}" \
      CX_BOTTLE="$DXMT_EXPERIMENT_CX_BOTTLE" \
      WINEDEBUG="${WINEDEBUG:-$EXP_DEFAULT_WINEDEBUG}" \
      DYLD_LIBRARY_PATH="$dyld_library_path" \
      DXMT_VALIDATE="${DXMT_VALIDATE:-$EXP_DEFAULT_DXMT_VALIDATE}" \
      DXMT_PERF_COUNTERS="${DXMT_PERF_COUNTERS:-$EXP_DEFAULT_DXMT_PERF_COUNTERS}" \
      DXMT_PERF_COUNTERS_PERIODIC_PRESENTS="${DXMT_PERF_COUNTERS_PERIODIC_PRESENTS:-$EXP_DEFAULT_DXMT_PERF_COUNTERS_PERIODIC_PRESENTS}" \
      DXMT9_OFFLOAD_COMMIT_REPLAY="${DXMT9_OFFLOAD_COMMIT_REPLAY:-$EXP_DEFAULT_DXMT9_OFFLOAD_COMMIT_REPLAY}" \
      DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE="${DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE:-$EXP_DEFAULT_DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE}" \
      DXMT9_WINEMETAL_SO="$winemetal_so" \
      DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-$EXP_DEFAULT_DXMT_LOG_LEVEL}" \
      DXMT_LOG_PATH="$log_dir" \
      "$DXMT_EXPERIMENT_WINE_BIN" --bottle "$DXMT_EXPERIMENT_CX_BOTTLE" --wait-all --dll "$dll_overrides" "$binary" "$@"
    )
  else
    (
      cd "$workdir"
      WINEPREFIX="$DXMT_EXPERIMENT_PREFIX" \
      WINEDLLOVERRIDES="$dll_overrides" \
      WINEDEBUG="${WINEDEBUG:-$EXP_DEFAULT_WINEDEBUG}" \
      DYLD_LIBRARY_PATH="$dyld_library_path" \
      DXMT_VALIDATE="${DXMT_VALIDATE:-$EXP_DEFAULT_DXMT_VALIDATE}" \
      DXMT_PERF_COUNTERS="${DXMT_PERF_COUNTERS:-$EXP_DEFAULT_DXMT_PERF_COUNTERS}" \
      DXMT_PERF_COUNTERS_PERIODIC_PRESENTS="${DXMT_PERF_COUNTERS_PERIODIC_PRESENTS:-$EXP_DEFAULT_DXMT_PERF_COUNTERS_PERIODIC_PRESENTS}" \
      DXMT9_OFFLOAD_COMMIT_REPLAY="${DXMT9_OFFLOAD_COMMIT_REPLAY:-$EXP_DEFAULT_DXMT9_OFFLOAD_COMMIT_REPLAY}" \
      DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE="${DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE:-$EXP_DEFAULT_DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE}" \
      DXMT9_WINEMETAL_SO="$winemetal_so" \
      DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-$EXP_DEFAULT_DXMT_LOG_LEVEL}" \
      DXMT_LOG_PATH="$log_dir" \
      "$DXMT_EXPERIMENT_WINE_BIN" "$binary" "$@"
    )
  fi
}

exp_run_d3d9_intent_probe() {
  local mode=$1
  shift || true

  local source_path="$exp_repo_root/experiments/apps/conf-d3d9-intent-probe/conf-d3d9-intent-probe.cpp"
  if [[ ! -f "$source_path" ]]; then
    exp_log "conf-d3d9-intent-probe source/build lane unavailable: missing $source_path"
    exp_log "not running generated/ignored binary: ${DXMT_EXPERIMENT_BINARY:-<unset>}"
    exit 2
  fi

  exp_stage_dxmt9
  export DXMT_EXPERIMENT_WORKDIR
  DXMT_EXPERIMENT_WORKDIR=$(dirname -- "$DXMT_EXPERIMENT_BINARY")
  exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" "$mode" "$@"
}
