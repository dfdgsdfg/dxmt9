#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9

export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR="$exp_repo_root/experiments/prefixs/app-d3d9-3dmark05/drive_c/Program Files (x86)/Futuremark/3DMark05"

if [[ "${DXMT_3DMARK05_AUTO_ENTER:-1}" != "0" ]]; then
  (
    focus_app-d3d9-3dmark05() {
      osascript \
        -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark05" to true'
    }

    sleep "${DXMT_3DMARK05_ENTER_DELAY_SEC:-20}"
    focus_app-d3d9-3dmark05 || true
    osascript -e 'tell application "System Events" to key code 36' || true

    remaining=${DXMT_3DMARK05_FOCUS_KEEPALIVE_SEC:-120}
    while [[ "$remaining" =~ ^[0-9]+$ ]] && (( remaining > 0 )); do
      sleep 1
      focus_app-d3d9-3dmark05 || true
      remaining=$((remaining - 1))
    done
  ) &
fi

read -r -a dxmt_3dmark05_args <<< "${DXMT_3DMARK05_ARGS:--gtall -batchall -featureall -cpuall -nosplash -nosysteminfo -noscreens}"
exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" "${dxmt_3dmark05_args[@]}"
