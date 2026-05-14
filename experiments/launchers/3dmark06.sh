#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9

export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR="$exp_repo_root/experiments/prefixs/3dmark06/drive_c/Program Files (x86)/Futuremark/3DMark06"

## Same automation status as 3DMark05 — see the comment block in
## experiments/launchers/3dmark05.sh. Operator must click "Run 3DMark"
## once after the UI appears (within ~15 s of launch). 06 also shows
## a "Please Register" dialog on first run that needs "Continue" —
## subsequent runs skip it (registry remembers the click).
exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" \
  -gtall -batchall -featureall -nosplash -nosysteminfo -noscreens
