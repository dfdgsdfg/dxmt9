#!/usr/bin/env bash
# V1 boundary-isolation audit suite.
#
# Runs each of the boundary-isolated probes once per matching pipeline
# boundary so a regression at one boundary cannot be confused with another.
# The results land in experiments/output/dx9-present-policy-ab/<tag>/<probe>/
# and per-probe summary.{json,md} carry only that boundary's counter set
# (driven by --boundary in run_dx9_present_policy_ab.py).
#
# See docs/research/boundary-benchmarks.md for the design.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
runner="$repo_root/scripts/tools/run_dx9_present_policy_ab.py"

runs="${BOUNDARY_AUDIT_RUNS:-3}"
timeout="${BOUNDARY_AUDIT_TIMEOUT:-60}"
tag="${BOUNDARY_AUDIT_TAG:-boundary-audit-$(date +%Y%m%d-%H%M%S)}"

# (boundary, probe app name) pairs. The probes must already be built
# (scripts/build_apps/build_*_probe.sh) before this suite runs.
suites=(
  "B2 perf-d3d9-bridge-empty"
  "B3 perf-d3d9-encode-replay"
  "B3 perf-d3d9-chain-parametric"
  "B4 perf-d3d9-ffp-only"
  "B4 perf-d3d9-multi-rt"
  "B4 perf-d3d9-depth-heavy"
  "B4 perf-d3d9-skeletal"
  "B6 perf-d3d9-present-loop"
)

cd "$repo_root"
for entry in "${suites[@]}"; do
  boundary="${entry%% *}"
  app="${entry##* }"
  echo "==> ${boundary} via ${app}"
  python3 "$runner" \
    --app "$app" \
    --mode default \
    --runs "$runs" \
    --timeout "$timeout" \
    --tag "${tag}-${boundary}-${app#dxmt9-perf-}" \
    --boundary "$boundary" \
    || echo "    !! ${app} failed; continuing"
  echo
done

echo "boundary audit complete."
echo "outputs under: experiments/output/dx9-present-policy-ab/${tag}-*"
