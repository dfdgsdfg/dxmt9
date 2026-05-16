#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9

export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR="$exp_repo_root/experiments/prefixs/3dmark05/drive_c/Program Files (x86)/Futuremark/3DMark05"

## CLI automation flags discovered via `strings` on the binary:
##   -gtall      run all Graphics Tests
##   -batchall   run all batch (size) tests
##   -featureall run all Feature tests
##   -cpuall     run all CPU tests
##   -nosplash   disable splash window
##   -noscreens  disable loading screens
##   -nosysteminfo  skip system info collection
##
## NOTE: those flags only *select* the tests; the main UI still waits
## for "Run 3DMark". Coordinate clicks are unreliable because the
## Wine-on-macOS window geometry and accessibility metadata do not line
## up with the visible skinned UI. In practice, pressing Enter after the
## UI settles activates the default Run button reliably.
if [[ "${DXMT_3DMARK05_AUTO_ENTER:-1}" != "0" ]]; then
  (
    focus_3dmark05() {
      osascript \
        -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark05" to true'
    }

    sleep "${DXMT_3DMARK05_ENTER_DELAY_SEC:-20}"
    focus_3dmark05 || true
    osascript -e 'tell application "System Events" to key code 36' || true

    remaining=${DXMT_3DMARK05_FOCUS_KEEPALIVE_SEC:-40}
    while [[ "$remaining" =~ ^[0-9]+$ ]] && (( remaining > 0 )); do
      sleep 1
      focus_3dmark05 || true
      remaining=$((remaining - 1))
    done
  ) &
fi

exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" \
  -gtall -batchall -featureall -cpuall -nosplash -nosysteminfo -noscreens
