#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

# 3DMark06 separates the graphics suite into two SM2 tests (-gt1/-gt2) and
# two HDR/SM3 tests (-hdr1/-hdr2). Keep routine runs bounded to the first SM2
# test; broader or different selections are explicit DXMT_3DMARK06_ARGS input.
default_3dmark06_selection_args="-gt1"
default_3dmark06_runner_args="-nosplash -nosysteminfo -noscreens"
default_3dmark06_args="$default_3dmark06_selection_args $default_3dmark06_runner_args"
dxmt_3dmark06_auto_enter_pid=""

append_result_file_3dmark06() {
  if [[ -n "${DXMT_3DMARK06_RESULT_FILE:-}" ]]; then
    dxmt_3dmark06_args+=("$DXMT_3DMARK06_RESULT_FILE")
  fi
}

focus_3dmark06() {
  osascript \
    -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark06" to true'
}

send_enter_3dmark06() {
  osascript \
    -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark06" to true' \
    -e 'tell application "System Events" to key code 36'
}

require_unlocked_session_3dmark06() {
  if [[ "${DXMT_3DMARK06_REQUIRE_UNLOCKED:-1}" == "0" ]]; then
    return 0
  fi
  if command -v ioreg >/dev/null 2>&1; then
    local session_state
    session_state=$(ioreg -n Root -d1 2>/dev/null || true)
    if [[ "$session_state" == *'"CGSSessionScreenIsLocked"=Yes'* ]]; then
      echo "[3dmark06] macOS session is locked; unlock the desktop or set DXMT_3DMARK06_REQUIRE_UNLOCKED=0 to bypass" >&2
      exit 2
    fi
  fi
}

schedule_3dmark06_enter() {
  (
    local delay=${DXMT_3DMARK06_ENTER_DELAY_SEC:-20}
    local enter_count=${DXMT_3DMARK06_ENTER_COUNT:-3}
    local enter_interval=${DXMT_3DMARK06_ENTER_INTERVAL_SEC:-2}
    local remaining=${DXMT_3DMARK06_FOCUS_KEEPALIVE_SEC:-120}

    echo "[3dmark06-auto-enter] delay=${delay}s count=${enter_count} interval=${enter_interval}s"
    sleep "$delay"

    if [[ ! "$enter_count" =~ ^[0-9]+$ ]]; then
      enter_count=3
    fi
    if [[ ! "$enter_interval" =~ ^[0-9]+$ ]]; then
      enter_interval=2
    fi

    local i=0
    while (( i < enter_count )); do
      echo "[3dmark06-auto-enter] enter attempt $((i + 1))/$enter_count"
      send_enter_3dmark06 || true
      i=$((i + 1))
      if (( i < enter_count )); then
        sleep "$enter_interval"
      fi
    done

    while [[ "$remaining" =~ ^[0-9]+$ ]] && (( remaining > 0 )); do
      sleep 1
      focus_3dmark06 || true
      remaining=$((remaining - 1))
    done
  ) &
  dxmt_3dmark06_auto_enter_pid=$!
}

cleanup_3dmark06() {
  if [[ -n "$dxmt_3dmark06_auto_enter_pid" ]]; then
    kill "$dxmt_3dmark06_auto_enter_pid" >/dev/null 2>&1 || true
  fi
}

default_binary="$exp_repo_root/experiments/apps_3rd/app-d3d9-3dmark06/3DMark06.exe"
binary=${DXMT_EXPERIMENT_BINARY:-$default_binary}
workdir=$(dirname -- "$binary")
read -r -a dxmt_3dmark06_args <<< "${DXMT_3DMARK06_ARGS:-$default_3dmark06_args}"
append_result_file_3dmark06

if [[ "${DXMT_3DMARK06_DRY_RUN:-0}" != "0" ]]; then
  echo "[3dmark06-dry-run] binary=$binary"
  echo "[3dmark06-dry-run] workdir=$workdir"
  echo "[3dmark06-dry-run] default_selection=GT1-only"
  echo "[3dmark06-dry-run] args=${dxmt_3dmark06_args[*]}"
  printf '[3dmark06-dry-run] command='
  printf ' %q' "${DXMT_EXPERIMENT_WINE_BIN:-wine}" "$binary" "${dxmt_3dmark06_args[@]}"
  printf '\n'
  exit 0
fi

require_unlocked_session_3dmark06
exp_stage_dxmt9

export DXMT_EXPERIMENT_WORKDIR="$workdir"

trap cleanup_3dmark06 EXIT
trap 'cleanup_3dmark06; exit 130' INT
trap 'cleanup_3dmark06; exit 143' TERM
if [[ "${DXMT_3DMARK06_AUTO_ENTER:-0}" != "0" ]]; then
  schedule_3dmark06_enter
fi

echo "[3dmark06] default_selection=GT1-only"
echo "[3dmark06] args=${dxmt_3dmark06_args[*]}"
exp_run_wine_binary "$binary" "${dxmt_3dmark06_args[@]}"
