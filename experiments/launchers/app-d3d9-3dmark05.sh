#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

# Keep heuristic runs scoped to the first benchmark by default. Broader suites
# must be requested explicitly through DXMT_3DMARK05_ARGS.
default_3dmark05_selection_args="-gt1"
default_3dmark05_runner_args="-nosplash -nosysteminfo -noscreens"
default_3dmark05_args="$default_3dmark05_selection_args $default_3dmark05_runner_args"
dxmt_3dmark05_auto_enter_pid=""

validate_timeout_app-d3d9-3dmark05() {
  local name=$1
  local value=$2
  if [[ ! "$value" =~ ^[0-9]+([.][0-9]+)?$ ||
        "$value" =~ ^0+([.]0+)?$ ]]; then
    echo "$name must be positive numeric seconds" >&2
    exit 2
  fi
}

resolve_launcher_timeout_app-d3d9-3dmark05() {
  if [[ -n "${DXMT_3DMARK05_LAUNCHER_TIMEOUT:-}" ]]; then
    validate_timeout_app-d3d9-3dmark05 DXMT_3DMARK05_LAUNCHER_TIMEOUT "$DXMT_3DMARK05_LAUNCHER_TIMEOUT"
    printf '%s\n' "$DXMT_3DMARK05_LAUNCHER_TIMEOUT"
  elif [[ -n "${DXMT_3DMARK05_DIRECT_TIMEOUT:-}" ]]; then
    validate_timeout_app-d3d9-3dmark05 DXMT_3DMARK05_DIRECT_TIMEOUT "$DXMT_3DMARK05_DIRECT_TIMEOUT"
    printf '%s\n' "$DXMT_3DMARK05_DIRECT_TIMEOUT"
  else
    printf '120\n'
  fi
}

self_supervise_app-d3d9-3dmark05_if_needed() {
  if [[ "${DXMT_3DMARK05_SELF_SUPERVISED:-0}" != "0" ||
        -n "${DXMT_EXPERIMENT_NAME:-}" ||
        "${DXMT_3DMARK05_ALLOW_UNSUPERVISED:-0}" != "0" ]]; then
    return 0
  fi

  local timeout_sec
  timeout_sec=$(resolve_launcher_timeout_app-d3d9-3dmark05)

  if [[ "${DXMT_3DMARK05_DIRECT_DRY_RUN:-0}" != "0" ]]; then
    echo "launcher_timeout: ${timeout_sec}s"
    printf 'launcher_cmd: DXMT_3DMARK05_SELF_SUPERVISED=1'
    printf ' %q' "$0" "$@"
    printf '\n'
    exit 0
  fi

  python3 - "$0" "$timeout_sec" "$@" <<'PY'
import os
import signal
import subprocess
import sys

launcher = sys.argv[1]
timeout_sec = float(sys.argv[2])
launcher_args = sys.argv[3:]
cmd = [launcher, *launcher_args]


def terminate_process_group(process, timeout):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=timeout)


env = os.environ.copy()
env["DXMT_3DMARK05_SELF_SUPERVISED"] = "1"

print(f"[3dmark05-launcher-timeout] timeout={timeout_sec:g}s", flush=True)
process = subprocess.Popen(cmd, cwd=os.getcwd(), env=env, start_new_session=True)
try:
    returncode = process.wait(timeout=timeout_sec)
except subprocess.TimeoutExpired:
    print(
        f"[3dmark05-launcher-timeout] timeout after {timeout_sec:g}s; terminating process group",
        file=sys.stderr,
        flush=True,
    )
    terminate_process_group(process, 5)
    sys.exit(124)
except KeyboardInterrupt:
    print(
        "[3dmark05-launcher-timeout] interrupted; terminating process group",
        file=sys.stderr,
        flush=True,
    )
    terminate_process_group(process, 5)
    sys.exit(130)

sys.exit(returncode)
PY
  exit "$?"
}

self_supervise_app-d3d9-3dmark05_if_needed "$@"

append_result_file_app-d3d9-3dmark05() {
  if [[ -n "${DXMT_3DMARK05_RESULT_FILE:-}" ]]; then
    dxmt_3dmark05_args+=("$DXMT_3DMARK05_RESULT_FILE")
  fi
}

focus_app-d3d9-3dmark05() {
  osascript \
    -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark05" to true'
}

send_enter_app-d3d9-3dmark05() {
  osascript \
    -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark05" to true' \
    -e 'tell application "System Events" to key code 36'
}

require_unlocked_session_app-d3d9-3dmark05() {
  if [[ "${DXMT_3DMARK05_REQUIRE_UNLOCKED:-1}" == "0" ]]; then
    return 0
  fi
  if command -v ioreg >/dev/null 2>&1; then
    local session_state
    session_state=$(ioreg -n Root -d1 2>/dev/null || true)
    if [[ "$session_state" == *'"CGSSessionScreenIsLocked"=Yes'* ]]; then
      echo "[3dmark05] macOS session is locked; unlock the desktop or set DXMT_3DMARK05_REQUIRE_UNLOCKED=0 to bypass" >&2
      exit 2
    fi
  fi
}

schedule_app-d3d9-3dmark05_enter() {
  local default_focus_keepalive=${1:-120}
  (
    echo "[3dmark05-auto-enter] delay=${DXMT_3DMARK05_ENTER_DELAY_SEC:-20}s count=${DXMT_3DMARK05_ENTER_COUNT:-3} interval=${DXMT_3DMARK05_ENTER_INTERVAL_SEC:-2}s"
    sleep "${DXMT_3DMARK05_ENTER_DELAY_SEC:-20}"

    local enter_count=${DXMT_3DMARK05_ENTER_COUNT:-3}
    local enter_interval=${DXMT_3DMARK05_ENTER_INTERVAL_SEC:-2}
    if [[ ! "$enter_count" =~ ^[0-9]+$ ]]; then
      enter_count=3
    fi
    if [[ ! "$enter_interval" =~ ^[0-9]+$ ]]; then
      enter_interval=2
    fi

    local i=0
    while (( i < enter_count )); do
      echo "[3dmark05-auto-enter] enter attempt $((i + 1))/$enter_count"
      if ! send_enter_app-d3d9-3dmark05; then
        echo "[3dmark05-auto-enter] enter attempt failed"
      fi
      i=$((i + 1))
      if (( i < enter_count )); then
        sleep "$enter_interval"
      fi
    done

    local remaining=${DXMT_3DMARK05_FOCUS_KEEPALIVE_SEC:-$default_focus_keepalive}
    while [[ "$remaining" =~ ^[0-9]+$ ]] && (( remaining > 0 )); do
      sleep 1
      focus_app-d3d9-3dmark05 || true
      remaining=$((remaining - 1))
    done
  ) &
  dxmt_3dmark05_auto_enter_pid=$!
}

if [[ "${DXMT_3DMARK05_DIRECT:-0}" != "0" ]]; then
  prefix=${DXMT_3DMARK05_PREFIX:-"$exp_repo_root/experiments/prefixs/app-d3d9-3dmark05-verify"}
  wine_root=${DXMT_3DMARK05_WINE_ROOT:-"$exp_repo_root/experiments/wine/sikarugir-cx-24.0.7"}
  wine_bin=${DXMT_3DMARK05_WINE_BIN:-"$wine_root/bin/wine"}
  wine_server=${DXMT_3DMARK05_WINESERVER:-"$wine_root/bin/wineserver"}
  exe_dir="$prefix/drive_c/Program Files (x86)/Futuremark/3DMark05"
  exe="$exe_dir/3DMark05.exe"
  log_path=${DXMT_3DMARK05_LOG:-/tmp/3dmark05-direct.log}
  winemetal_so=${DXMT9_WINEMETAL_SO:-"$exp_repo_root/build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so"}

  cleanup_app_d3d9_3dmark05_direct() {
    local status=${1:-$?}
    trap - EXIT INT TERM
    if [[ -n "${dxmt_3dmark05_auto_enter_pid:-}" ]]; then
      kill "$dxmt_3dmark05_auto_enter_pid" >/dev/null 2>&1 || true
    fi
    if [[ "${DXMT_3DMARK05_KILL_SERVER_ON_EXIT:-1}" != "0" ]]; then
      WINEPREFIX="$prefix" "$wine_server" -k >/dev/null 2>&1 || true
    fi
    exit "$status"
  }

  trap 'cleanup_app_d3d9_3dmark05_direct $?' EXIT
  trap 'cleanup_app_d3d9_3dmark05_direct 130' INT
  trap 'cleanup_app_d3d9_3dmark05_direct 143' TERM

  if [[ ! -x "$wine_bin" ]]; then
    echo "missing wine binary: $wine_bin" >&2
    exit 2
  fi

  if [[ ! -f "$exe" ]]; then
    echo "missing 3DMark05 executable: $exe" >&2
    exit 2
  fi

  export DXMT_EXPERIMENT_PREFIX="$prefix"
  export DXMT_EXPERIMENT_WINE_ROOT="$wine_root"
  export DXMT_EXPERIMENT_PE_BUILD_DIR="${DXMT_3DMARK05_PE_BUILD_DIR:-${DXMT_EXPERIMENT_PE_BUILD_DIR:-$exp_repo_root/build-win32-x64-builtin/src/win32}}"
  export DXMT_EXPERIMENT_RUNTIME_PE_BUILD_DIR="${DXMT_3DMARK05_RUNTIME_PE_BUILD_DIR:-${DXMT_EXPERIMENT_RUNTIME_PE_BUILD_DIR:-$exp_repo_root/build-win32-x64-builtin/src/winemetal}}"
  export DXMT_EXPERIMENT_WOW64_PE_BUILD_DIR="${DXMT_3DMARK05_WOW64_PE_BUILD_DIR:-${DXMT_EXPERIMENT_WOW64_PE_BUILD_DIR:-$exp_repo_root/build-win32-x86-builtin/src/win32}}"
  export DXMT_EXPERIMENT_WOW64_RUNTIME_PE_BUILD_DIR="${DXMT_3DMARK05_WOW64_RUNTIME_PE_BUILD_DIR:-${DXMT_EXPERIMENT_WOW64_RUNTIME_PE_BUILD_DIR:-$exp_repo_root/build-win32-x86-builtin/src/winemetal}}"
  export DXMT_EXPERIMENT_UNIX_BUILD_DIR="${DXMT_3DMARK05_UNIX_BUILD_DIR:-${DXMT_EXPERIMENT_UNIX_BUILD_DIR:-$exp_repo_root/build-x86_64-builtin/src}}"

  if [[ "${DXMT_3DMARK05_STAGE:-1}" != "0" ]]; then
    exp_stage_dxmt9
  fi

  if [[ "${DXMT_3DMARK05_KILL_SERVER:-1}" != "0" ]]; then
    WINEPREFIX="$prefix" "$wine_server" -k >/dev/null 2>&1 || true
  fi

  mkdir -p "$(dirname -- "$log_path")"
  : > "$log_path"

  read -r -a dxmt_3dmark05_args <<< "${DXMT_3DMARK05_ARGS:-$default_3dmark05_args}"
  append_result_file_app-d3d9-3dmark05

  export DXMT_EXPERIMENT_WORKDIR
  DXMT_EXPERIMENT_WORKDIR="$exe_dir"

  echo "[3dmark05-direct] prefix=$prefix"
  echo "[3dmark05-direct] wine=$wine_bin"
  echo "[3dmark05-direct] log=$log_path"
  echo "[3dmark05-direct] default_selection=GT1-only"
  echo "[3dmark05-direct] args=${dxmt_3dmark05_args[*]}"

  require_unlocked_session_app-d3d9-3dmark05

  if [[ "${DXMT_3DMARK05_AUTO_ENTER:-1}" != "0" ]]; then
    schedule_app-d3d9-3dmark05_enter 0
  fi

  exp_resolve_profile_defaults
  echo "[3dmark05-direct] profile=$EXP_PROFILE_NAME"

  cd "$DXMT_EXPERIMENT_WORKDIR"
  WINEPREFIX="$prefix" \
  WINEDLLOVERRIDES="${DXMT_3DMARK05_DLLOVERRIDES:-d3d9,winemetal_dxmt9=n,b;d3dx9_25,d3dx9_26,d3dx9_27,d3dx9_28,d3dcompiler_43,d3dcompiler_47=n,b}" \
  WINEDEBUG="${WINEDEBUG:-$EXP_DEFAULT_WINEDEBUG}" \
  DYLD_LIBRARY_PATH="$wine_root/lib/wine/x86_64-unix${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
  DXMT_VALIDATE="${DXMT_VALIDATE:-$EXP_DEFAULT_DXMT_VALIDATE}" \
  DXMT_PERF_COUNTERS="${DXMT_PERF_COUNTERS:-$EXP_DEFAULT_DXMT_PERF_COUNTERS}" \
  DXMT_PERF_COUNTERS_PERIODIC_PRESENTS="${DXMT_PERF_COUNTERS_PERIODIC_PRESENTS:-$EXP_DEFAULT_DXMT_PERF_COUNTERS_PERIODIC_PRESENTS}" \
  DXMT9_OFFLOAD_COMMIT_REPLAY="${DXMT9_OFFLOAD_COMMIT_REPLAY:-$EXP_DEFAULT_DXMT9_OFFLOAD_COMMIT_REPLAY}" \
  DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE="${DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE:-$EXP_DEFAULT_DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE}" \
  DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK="${DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK:-1}" \
  DXMT9_WINEMETAL_SO="$winemetal_so" \
  DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-$EXP_DEFAULT_DXMT_LOG_LEVEL}" \
  DXMT_LOG_PATH="${DXMT_LOG_PATH:-$(dirname -- "$log_path")}" \
  "$wine_bin" "$exe" "${dxmt_3dmark05_args[@]}" 2>&1 | tee -a "$log_path"
  exit "${PIPESTATUS[0]}"
fi

exp_stage_dxmt9

export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR="$exp_repo_root/experiments/prefixs/app-d3d9-3dmark05/drive_c/Program Files (x86)/Futuremark/3DMark05"

require_unlocked_session_app-d3d9-3dmark05

if [[ "${DXMT_3DMARK05_AUTO_ENTER:-1}" != "0" ]]; then
  schedule_app-d3d9-3dmark05_enter 120
fi

read -r -a dxmt_3dmark05_args <<< "${DXMT_3DMARK05_ARGS:-$default_3dmark05_args}"
append_result_file_app-d3d9-3dmark05
echo "[3dmark05] default_selection=GT1-only"
echo "[3dmark05] args=${dxmt_3dmark05_args[*]}"
exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" "${dxmt_3dmark05_args[@]}"
