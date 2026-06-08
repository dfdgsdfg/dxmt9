#!/usr/bin/env bash
# CI conformance gate across both renderer backends (R-BACK-39.4).
#
# Runs the D3D9 conformance suite (scripts/tools/run_d3d9_conformance.py) once
# per renderer mode — `traditional` and `framegraph` — by exporting
# DXMT9_RENDER_MODE for each leg. Each leg's per-test verdict JSON is recorded
# separately under a mode-tagged artifact name. A failing/timeout verdict in
# EITHER leg fails this script (non-zero exit), so a mode-specific regression
# blocks merge.
#
# DXMT9_RENDER_MODE is consumed by src/dxmt9/render/backend_factory.cpp
# (resolveBackendMode): "framegraph" selects the Frame Graph backend; unset/
# ""/"0"/"traditional"/unknown selects the Traditional backend. The conformance
# runner inherits os.environ (build_env in run_d3d9_conformance.py), so exporting
# the var here propagates it through Wine into the dxmt9 runtime.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

modes=("traditional" "framegraph")

usage() {
  cat <<'EOF'
Usage:
  bash scripts/run_suites/run_d3d9_conformance_render_modes.sh [options] [-- <run_d3d9_conformance.py args>]

Options:
  --mode <name>     Run only the named render mode (traditional|framegraph).
                    Repeatable; default runs both.
  --out-dir <path>  Directory for per-mode result JSON + summary.
                    Default: experiments/output/d3d9-conformance-render-modes
  --help            Show this message.

Any arguments after `--` are forwarded verbatim to run_d3d9_conformance.py
for every mode leg (e.g. --chunk-size, --timeout, --exe, --skip-aux).

Artifacts (per leg):
  <out-dir>/conformance-<mode>.json   per-test verdicts for that render mode
  <out-dir>/conformance-<mode>.log    runner stderr/stdout for that render mode
  <out-dir>/summary.json              aggregate per-mode pass/fail/skip/timeout

Exit status is non-zero if EITHER mode leg has any failing or timeout verdict
(R-BACK-39.4: a mode-specific regression must block merge).
EOF
}

out_dir="$repo_root/experiments/output/d3d9-conformance-render-modes"
selected_modes=()
forwarded_args=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      selected_modes+=("${2:-}")
      shift 2
      ;;
    --out-dir)
      out_dir=${2:-}
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      forwarded_args=("$@")
      break
      ;;
    *)
      printf 'error: unknown argument: %s\n\n' "$1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ${#selected_modes[@]} -gt 0 ]]; then
  for m in "${selected_modes[@]}"; do
    if [[ "$m" != "traditional" && "$m" != "framegraph" ]]; then
      printf 'error: invalid --mode %s (expected traditional|framegraph)\n' "$m" >&2
      exit 1
    fi
  done
  modes=("${selected_modes[@]}")
fi

mkdir -p "$out_dir"
summary_json="$out_dir/summary.json"

declare -a mode_status=()

for mode in "${modes[@]}"; do
  result_json="$out_dir/conformance-$mode.json"
  leg_log="$out_dir/conformance-$mode.log"
  printf '==> render mode: %s\n' "$mode"

  cmd=(
    python3
    "$repo_root/scripts/tools/run_d3d9_conformance.py"
    --output "$result_json"
  )
  if [[ ${#forwarded_args[@]} -gt 0 ]]; then
    cmd+=("${forwarded_args[@]}")
  fi

  # Export the renderer selector for this leg only; the conformance runner
  # forwards os.environ into the Wine process.
  leg_rc=0
  DXMT9_RENDER_MODE="$mode" "${cmd[@]}" 2>&1 | tee "$leg_log" || leg_rc=${PIPESTATUS[0]}

  if [[ "$leg_rc" -ne 0 ]]; then
    printf 'render mode %s: runner exited %s\n' "$mode" "$leg_rc" >&2
    mode_status+=("$mode=runner-error")
    continue
  fi
  mode_status+=("$mode=ran")
done

# Aggregate per-mode verdicts and decide pass/fail. The conformance runner
# always exits 0 and only records verdicts to JSON, so the merge-blocking
# gate is computed here from the per-mode result files.
python3 - "$repo_root" "$summary_json" "$out_dir" "${modes[@]}" <<'PY'
from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

repo_root = Path(sys.argv[1])
summary_json = Path(sys.argv[2])
out_dir = Path(sys.argv[3])
modes = sys.argv[4:]

per_mode: dict[str, dict] = {}
overall_ok = True

for mode in modes:
    result_path = out_dir / f"conformance-{mode}.json"
    if not result_path.exists():
        per_mode[mode] = {
            "result_path": str(result_path),
            "present": False,
            "tally": {},
            "regressions": [],
            "ok": False,
        }
        overall_ok = False
        continue
    verdicts = json.loads(result_path.read_text())
    tally = {"pass": 0, "fail": 0, "skip": 0, "timeout": 0}
    regressions = []
    for name, verdict in verdicts.items():
        tally[verdict] = tally.get(verdict, 0) + 1
        if verdict in ("fail", "timeout"):
            regressions.append({"test": name, "verdict": verdict})
    mode_ok = not regressions and bool(verdicts)
    if not mode_ok:
        overall_ok = False
    per_mode[mode] = {
        "result_path": str(result_path),
        "present": True,
        "tally": tally,
        "regressions": sorted(regressions, key=lambda r: r["test"]),
        "ok": mode_ok,
    }

payload = {
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "requirement": "R-BACK-39.4",
    "modes": per_mode,
    "ok": overall_ok,
}
summary_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")

print("conformance render-mode summary:")
for mode in modes:
    info = per_mode[mode]
    if not info["present"]:
        print(f"  {mode:12s} MISSING result file ({info['result_path']})")
        continue
    t = info["tally"]
    print(
        f"  {mode:12s} pass={t['pass']} fail={t['fail']} "
        f"skip={t['skip']} timeout={t['timeout']} "
        f"-> {'OK' if info['ok'] else 'REGRESSION'}"
    )
    for reg in info["regressions"][:20]:
        print(f"      {reg['verdict']:8s} {reg['test']}")
print(f"  summary -> {summary_json}")

sys.exit(0 if overall_ok else 1)
PY
