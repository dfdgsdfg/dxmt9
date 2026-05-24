#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

apps=(
  "sample-d3d9-basic-hlsl"
  "sample-d3d9-tutorial07"
  "sample-d3d9-dxut-simple"
  "sample-d3d9-irrlicht-lights"
  "sample-d3d9-water-rt"
  "sample-d3d9-multitexture-terrain"
)

usage() {
  cat <<'EOF'
Usage:
  bash scripts/run_suites/run_dx9_oracle_compare_suite.sh [options]

Options:
  --wine-root <path>      Wine runtime root passed to run_experiment.py.
  --timeout <seconds>     Override timeout for every experiment.
  --keep-prefixes         Do not delete auto-created temp prefixes.
  --app <name>            Run only the named catalogue app. Repeatable.
  --help                  Show this message.

This script runs each selected app twice:
  1. builtin Wine d3d9
  2. dxmt9

It then compares:
  experiments/output/<app>-builtin-oracle/actual.png
  experiments/output/<app>-dxmt9-compare/actual.png

Outputs:
  experiments/output/dx9-oracle-compare/summary.json
  experiments/output/dx9-oracle-compare/summary.md
EOF
}

wine_root=""
timeout_override=""
cleanup_temp_prefix=true
selected_apps=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --wine-root)
      wine_root=${2:-}
      shift 2
      ;;
    --timeout)
      timeout_override=${2:-}
      shift 2
      ;;
    --keep-prefixes)
      cleanup_temp_prefix=false
      shift
      ;;
    --app)
      selected_apps+=("${2:-}")
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      printf 'error: unknown argument: %s\n\n' "$1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ${#selected_apps[@]} -gt 0 ]]; then
  apps=("${selected_apps[@]}")
fi

suite_dir="$repo_root/experiments/output/dx9-oracle-compare"
mkdir -p "$suite_dir"
summary_json="$suite_dir/summary.json"
summary_md="$suite_dir/summary.md"
suite_log="$suite_dir/suite.log"

: > "$suite_log"

cleanup_stale_prefixes() {
  printf '==> cleaning stale temp prefixes\n' | tee -a "$suite_log"
  python3 "$repo_root/scripts/tools/cleanup_dxmt9_temp_prefixes.py" --all | tee -a "$suite_log"
  printf '\n' | tee -a "$suite_log"
}

cleanup_stale_prefixes

run_lane() {
  local app=$1
  local suffix=$2
  local overrides=$3
  local -a cmd=(
    python3
    "$repo_root/scripts/run_apps/run_experiment.py"
    run
    "$app"
    --output-suffix "$suffix"
  )
  if [[ -n "$wine_root" ]]; then
    cmd+=(--wine-root "$wine_root")
  fi
  if [[ -n "$timeout_override" ]]; then
    cmd+=(--timeout "$timeout_override")
  fi
  if [[ "$cleanup_temp_prefix" == true ]]; then
    cmd+=(--cleanup-temp-prefix)
  fi

  DXMT_EXPERIMENT_WINE_DLLOVERRIDES="$overrides" "${cmd[@]}" | tee -a "$suite_log" || true
  printf '\n' | tee -a "$suite_log"
}

for app in "${apps[@]}"; do
  printf '==> %s builtin\n' "$app" | tee -a "$suite_log"
  run_lane "$app" "builtin-oracle" "d3d9=b"
  printf '==> %s dxmt9\n' "$app" | tee -a "$suite_log"
  run_lane "$app" "dxmt9-compare" "d3d9=n,b"
done

status=$(python3 - "$repo_root" "$summary_json" "$summary_md" "${apps[@]}" <<'PY'
from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from PIL import Image

repo_root = Path(sys.argv[1])
summary_json = Path(sys.argv[2])
summary_md = Path(sys.argv[3])
apps = sys.argv[4:]
output_root = repo_root / "experiments" / "output"


def compute_ssim(actual_path: Path, reference_path: Path) -> float:
    actual = Image.open(actual_path).convert("L")
    reference = Image.open(reference_path).convert("L")
    if actual.size != reference.size:
        raise ValueError(f"image size mismatch: {actual.size} vs {reference.size}")
    x = np.asarray(actual, dtype=np.float64)
    y = np.asarray(reference, dtype=np.float64)
    mu_x = float(np.mean(x))
    mu_y = float(np.mean(y))
    sigma_x = float(np.var(x))
    sigma_y = float(np.var(y))
    sigma_xy = float(np.mean((x - mu_x) * (y - mu_y)))
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    numerator = (2.0 * mu_x * mu_y + c1) * (2.0 * sigma_xy + c2)
    denominator = (mu_x * mu_x + mu_y * mu_y + c1) * (sigma_x + sigma_y + c2)
    if denominator == 0.0:
        return 1.0
    return float(numerator / denominator)


rows: list[dict[str, object]] = []
for app in apps:
    builtin_result = output_root / f"{app}-builtin-oracle" / "result.json"
    dxmt9_result = output_root / f"{app}-dxmt9-compare" / "result.json"
    builtin_image = output_root / f"{app}-builtin-oracle" / "actual.png"
    dxmt9_image = output_root / f"{app}-dxmt9-compare" / "actual.png"

    row: dict[str, object] = {
        "name": app,
        "builtin_result_path": str(builtin_result),
        "dxmt9_result_path": str(dxmt9_result),
        "status": "missing",
        "builtin_status": "missing",
        "dxmt9_status": "missing",
        "builtin_failures": [{"type": "missing_result"}],
        "dxmt9_failures": [{"type": "missing_result"}],
        "oracle_ssim": None,
    }

    if builtin_result.exists():
        builtin_data = json.loads(builtin_result.read_text())
        row["builtin_status"] = builtin_data.get("status", "unknown")
        row["builtin_failures"] = builtin_data.get("failures", [])
    if dxmt9_result.exists():
        dxmt9_data = json.loads(dxmt9_result.read_text())
        row["dxmt9_status"] = dxmt9_data.get("status", "unknown")
        row["dxmt9_failures"] = dxmt9_data.get("failures", [])

    if builtin_image.exists() and dxmt9_image.exists():
        row["oracle_ssim"] = compute_ssim(builtin_image, dxmt9_image)
        row["status"] = "pass" if row["oracle_ssim"] >= 0.90 else "fail"

    rows.append(row)

payload = {
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "apps": rows,
    "pass_count": sum(1 for row in rows if row["status"] == "pass"),
    "fail_count": sum(1 for row in rows if row["status"] != "pass"),
}
summary_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")

lines = [
    "# DX9 Oracle Compare Suite",
    "",
    f"- generated_at: `{payload['generated_at']}`",
    f"- pass_count: `{payload['pass_count']}`",
    f"- fail_count: `{payload['fail_count']}`",
    "",
    "| app | status | builtin | dxmt9 | builtin-vs-dxmt9 ssim |",
    "| --- | --- | --- | --- | ---: |",
]
for row in rows:
    ssim = row["oracle_ssim"]
    ssim_text = "-" if ssim is None else f"{float(ssim):.4f}"
    lines.append(
        f"| `{row['name']}` | `{row['status']}` | `{row['builtin_status']}` | `{row['dxmt9_status']}` | `{ssim_text}` |"
    )
    if row["builtin_failures"]:
        lines.append(f"|  | builtin_failures |  |  | `{json.dumps(row['builtin_failures'], sort_keys=True)}` |")
    if row["dxmt9_failures"]:
        lines.append(f"|  | dxmt9_failures |  |  | `{json.dumps(row['dxmt9_failures'], sort_keys=True)}` |")

summary_md.write_text("\n".join(lines) + "\n")
print(0 if payload["fail_count"] == 0 else 1)
PY
)

printf 'suite summary:\n  %s\n  %s\n' "$summary_json" "$summary_md"
exit "$status"
