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

# Render one profile-record field. An empty profile default (e.g. the debug
# profile's WINEDEBUG) is reported as the explicit token `unset` so the record
# never has a hole a reader could mistake for "not reported".
exp_profile_record_value() {
  if [[ -z "${1:-}" ]]; then
    printf 'unset'
  else
    printf '%s' "$1"
  fi
}

exp_resolve_profile_defaults() {
  # `perf` is the default because an experiment run is a measurement first:
  # the debug profile's Metal validation layer and debug-level logging cost
  # roughly 4x throughput (SFIV: 11.3 vs 43.0 sampled fps, 1.0 GB vs 22 MB of
  # log), which has already been misread once as a renderer regression.
  #
  # The trade-off this makes: the previous `debug` default meant every wild run
  # carried the Metal validation layer, so an API misuse was caught by whichever
  # run happened to hit it. That safety net is now opt-in. Anyone diagnosing a
  # wild failure — black screen, GPU fault, suspected API misuse — should set
  # `DXMT_EXPERIMENT_PROFILE=debug`, which is where the validation layer and
  # debug logging now live. A run that measures anything must not. Concretely,
  # the runner's `scan_log_for_failures` "Metal API Validation" / "validation
  # error" markers can now only fire on a `debug` run.
  local exp_profile_raw
  if [[ -n "${DXMT_EXPERIMENT_PROFILE:-}" ]]; then
    exp_profile_raw=$DXMT_EXPERIMENT_PROFILE
    EXP_PROFILE_SOURCE=DXMT_EXPERIMENT_PROFILE
  elif [[ -n "${DXMT_PROFILE:-}" ]]; then
    exp_profile_raw=$DXMT_PROFILE
    EXP_PROFILE_SOURCE=DXMT_PROFILE
  else
    exp_profile_raw=perf
    EXP_PROFILE_SOURCE=default
  fi
  EXP_PROFILE_NAME=$(printf '%s' "$exp_profile_raw" | tr '[:upper:]' '[:lower:]')

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

  # Record the configuration this run resolved, in the run's own output. The
  # runner captures launcher stdout/stderr into the run log, and
  # `run_experiment.py::extract_experiment_profile` parses this line back into
  # `result.json`'s `profile` object. A measurement artifact that does not say
  # what configuration produced it is how a profile mistake becomes a
  # multi-hour regression hunt.
  #
  # Values are the *effective* ones — `exp_run_wine_binary` lets a caller
  # override any single knob (`${DXMT_LOG_LEVEL:-$EXP_DEFAULT_DXMT_LOG_LEVEL}`),
  # so reporting the profile default alone could still misdescribe the run.
  exp_log "profile: name=$EXP_PROFILE_NAME source=$EXP_PROFILE_SOURCE"\
" validate=$(exp_profile_record_value "${DXMT_VALIDATE:-$EXP_DEFAULT_DXMT_VALIDATE}")"\
" log_level=$(exp_profile_record_value "${DXMT_LOG_LEVEL:-$EXP_DEFAULT_DXMT_LOG_LEVEL}")"\
" winedebug=$(exp_profile_record_value "${WINEDEBUG:-$EXP_DEFAULT_WINEDEBUG}")"\
" perf_counters=$(exp_profile_record_value "${DXMT_PERF_COUNTERS:-$EXP_DEFAULT_DXMT_PERF_COUNTERS}")"\
" offload_commit_replay=$(exp_profile_record_value "${DXMT9_OFFLOAD_COMMIT_REPLAY:-$EXP_DEFAULT_DXMT9_OFFLOAD_COMMIT_REPLAY}")"\
" optimize_opaque_depth_index_cache=$(exp_profile_record_value "${DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE:-$EXP_DEFAULT_DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE}")"
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
  local dll_overrides=${DXMT_EXPERIMENT_WINE_DLLOVERRIDES:-d3d9,winemetal_dxmt9=n,b}
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
  if [[ -z "$winemetal_so" && -f "${DXMT_EXPERIMENT_UNIX_BUILD_DIR:-}/winemetal/unix/winemetal_dxmt9.so" ]]; then
    winemetal_so="$DXMT_EXPERIMENT_UNIX_BUILD_DIR/winemetal/unix/winemetal_dxmt9.so"
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
      DXMT9_RENDER_TAPE_CAPTURE="${DXMT9_RENDER_TAPE_CAPTURE:-}" \
      DXMT9_RENDER_TAPE_PROFILE="${DXMT9_RENDER_TAPE_PROFILE:-}" \
      DXMT9_RENDER_TAPE_OUTPUT_ROOT="${DXMT9_RENDER_TAPE_OUTPUT_ROOT:-}" \
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
      DXMT9_RENDER_TAPE_CAPTURE="${DXMT9_RENDER_TAPE_CAPTURE:-}" \
      DXMT9_RENDER_TAPE_PROFILE="${DXMT9_RENDER_TAPE_PROFILE:-}" \
      DXMT9_RENDER_TAPE_OUTPUT_ROOT="${DXMT9_RENDER_TAPE_OUTPUT_ROOT:-}" \
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
