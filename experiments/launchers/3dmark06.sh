#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9

export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR="$exp_repo_root/experiments/prefixs/3dmark06/drive_c/Program Files (x86)/Futuremark/3DMark06"

exp_3dmark06_terminate_processes() {
  local pids
  pids=$(pgrep -f '[/]3DMark06\.exe.*Futuremark.*3DMark06' || true)
  if [[ -z "$pids" ]]; then
    exp_log "3DMark06 watchdog found no process to terminate"
    return 0
  fi

  exp_log "3DMark06 watchdog terminating process(es): ${pids//$'\n'/ }"
  kill $pids 2>/dev/null || true
  sleep 5

  local live
  live=$(pgrep -f '[/]3DMark06\.exe.*Futuremark.*3DMark06' || true)
  if [[ -n "$live" ]]; then
    exp_log "3DMark06 watchdog force-killing process(es): ${live//$'\n'/ }"
    kill -KILL $live 2>/dev/null || true
  fi
}

watchdog_pid=""
cleanup_watchdog() {
  if [[ -n "$watchdog_pid" ]]; then
    kill "$watchdog_pid" 2>/dev/null || true
  fi
}
trap cleanup_watchdog EXIT

## The 3DMark06 skinned UI accepts Enter as "Run 3DMark" after it settles.
## Let the benchmark run briefly after that, then terminate the app so this
## exploratory target does not wait on the full benchmark/result workflow.
if [[ "${DXMT_3DMARK06_AUTO_CONTROL:-1}" != "0" ]]; then
  (
    sleep "${DXMT_3DMARK06_ENTER_DELAY_SEC:-20}"
    osascript \
      -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark06" to true' \
      -e 'tell application "System Events" to key code 36' || true
    sleep "${DXMT_3DMARK06_RUN_AFTER_ENTER_SEC:-30}"
    if [[ "${DXMT_3DMARK06_AUTO_TERMINATE:-1}" != "0" ]]; then
      exp_3dmark06_terminate_processes
    fi
  ) &
  watchdog_pid=$!
fi

exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" \
  -gtall -batchall -featureall -nosplash -nosysteminfo -noscreens
