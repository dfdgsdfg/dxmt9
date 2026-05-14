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
## for the user to press "Run 3DMark". Full automation via osascript
## keystroke or cliclick was attempted but the wine-on-macOS window
## geometry is inconsistent — CGWindowList reports 332x110 while the
## visible 3DMark UI is ~1580x928 pixels (a ~2.4× ratio that doesn't
## match any obvious Retina-vs-logical convention), so we can't
## reliably compute the button's screen coordinate. Accessibility-tree
## buttons also have missing labels under custom-skinned 3DMark UIs.
##
## So this launcher leaves the test selection automated and asks the
## operator to click "Run 3DMark" once after the UI appears (within
## ~15 s of launch).
exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" \
  -gtall -batchall -featureall -cpuall -nosplash -nosysteminfo -noscreens
