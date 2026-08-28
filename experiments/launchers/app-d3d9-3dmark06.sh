#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"
source "$script_dir/3dmark_lane_presets.sh"

dxmt_resolve_3dmark_lane \
  06 "${DXMT_3DMARK06_LANE:-gt1}" "${DXMT_3DMARK06_ARGS:-}"
dxmt_3dmark06_auto_enter_pid=""
dxmt_3dmark06_autorun_pid=""

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

# Win32-side unattended start (DXMT_3DMARK06_AUTORUN=1): wait for the main
# GUI window inside the prefix via tool-winctl, then click the Run 3DMark
# button (control id 1, IDOK). Unlike the osascript auto-enter path above,
# this needs no focus, no Accessibility permission, and works while the
# window is hidden behind macOS windows. Requires an edition whose GUI shows
# the "3DMark06 - <edition>" main window (the UL-published legacy key
# registers Advanced Edition; per-test CLI selectors stay Professional-only).
schedule_3dmark06_autorun() {
  local winctl="$exp_repo_root/experiments/apps/tool-winctl/tool-winctl-x64.exe"
  if [[ ! -f "$winctl" ]]; then
    echo "[3dmark06-autorun] missing $winctl; build it with scripts/build_apps/build_tool-winctl.sh" >&2
    return 0
  fi
  (
    local wine_bin=${DXMT_EXPERIMENT_WINE_BIN:-wine}
    local settle=${DXMT_3DMARK06_AUTORUN_SETTLE_SEC:-3}
    local attempts=${DXMT_3DMARK06_AUTORUN_ATTEMPTS:-12}
    export WINEPREFIX="$DXMT_EXPERIMENT_PREFIX"
    echo "[3dmark06-autorun] waiting for main window"
    if ! "$wine_bin" "$winctl" wait --window "3DMark06 - " --timeout-ms 120000 >/dev/null 2>&1; then
      echo "[3dmark06-autorun] main window did not appear; leaving GUI untouched" >&2
      exit 0
    fi
    # The main window's title appears before its child buttons are created,
    # and a started benchmark disables the main window. So: click, then treat
    # "main window disabled or gone" as the ground-truth success signal and
    # retry otherwise.
    local i=0
    while (( i < attempts )); do
      sleep "$settle"
      echo "[3dmark06-autorun] click attempt $((i + 1))/$attempts (Run 3DMark, control id 1)"
      "$wine_bin" "$winctl" click --window "3DMark06 - " --control-id 1 2>&1 \
        | sed 's/^/[3dmark06-autorun] /' || true
      sleep 3
      local head_line=""
      head_line=$({ "$wine_bin" "$winctl" dump --window "3DMark06 - " 2>/dev/null || true; } | head -1)
      if [[ -z "$head_line" || "$head_line" == *"enabled=0"* ]]; then
        echo "[3dmark06-autorun] benchmark started (main window ${head_line:+disabled}${head_line:-gone})"
        exit 0
      fi
      i=$((i + 1))
    done
    echo "[3dmark06-autorun] gave up after $attempts click attempts" >&2
  ) &
  dxmt_3dmark06_autorun_pid=$!
}

cleanup_3dmark06() {
  if [[ -n "$dxmt_3dmark06_auto_enter_pid" ]]; then
    kill "$dxmt_3dmark06_auto_enter_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$dxmt_3dmark06_autorun_pid" ]]; then
    kill "$dxmt_3dmark06_autorun_pid" >/dev/null 2>&1 || true
  fi
}

default_binary="$exp_repo_root/experiments/apps_3rd/app-d3d9-3dmark06/3DMark06.exe"
binary=${DXMT_EXPERIMENT_BINARY:-$default_binary}
workdir=$(dirname -- "$binary")
read -r -a dxmt_3dmark06_args <<< "$DXMT_3DMARK_RESOLVED_ARGS"
append_result_file_3dmark06

if [[ "${DXMT_3DMARK06_DRY_RUN:-0}" != "0" ]]; then
  echo "[3dmark06-dry-run] binary=$binary"
  echo "[3dmark06-dry-run] workdir=$workdir"
  dxmt_print_3dmark_lane_identity 06
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
if [[ "${DXMT_3DMARK06_AUTORUN:-0}" != "0" ]]; then
  schedule_3dmark06_autorun
fi

dxmt_print_3dmark_lane_identity 06
echo "[3dmark06] args=${dxmt_3dmark06_args[*]}"
exp_run_wine_binary "$binary" "${dxmt_3dmark06_args[@]}"
