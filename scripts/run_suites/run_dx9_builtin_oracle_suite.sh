#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

apps=(
  "sample-d3d9-basic-hlsl"
  "sample-d3d9-tutorial07"
  "sample-d3d9-dxut-simple"
)

usage() {
  cat <<'EOF'
Usage:
  bash scripts/run_suites/run_dx9_builtin_oracle_suite.sh [options]

Options:
  --wine-root <path>      Wine runtime root passed to run_experiment.py.
  --timeout <seconds>     Override timeout for every experiment.
  --keep-prefixes         Do not delete auto-created temp prefixes.
  --app <name>            Run only the named catalogue app. Repeatable.
  --help                  Show this message.

This script runs the shader sample apps against builtin Wine d3d9 and writes:
  experiments/output/dx9-builtin-oracle/summary.json
  experiments/output/dx9-builtin-oracle/summary.md
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

suite_dir="$repo_root/experiments/output/dx9-builtin-oracle"
mkdir -p "$suite_dir"
summary_json="$suite_dir/summary.json"
summary_md="$suite_dir/summary.md"
suite_log="$suite_dir/suite.log"

: > "$suite_log"

status=0

cleanup_stale_prefixes() {
  printf '==> cleaning stale temp prefixes\n' | tee -a "$suite_log"
  "$repo_root/scripts/run_python.sh" "$repo_root/scripts/tools/cleanup_dxmt9_temp_prefixes.py" --all | tee -a "$suite_log"
  printf '\n' | tee -a "$suite_log"
}

cleanup_stale_prefixes

for app in "${apps[@]}"; do
  printf '==> %s\n' "$app" | tee -a "$suite_log"
  cmd=(
    "$repo_root/scripts/run_python.sh"
    "$repo_root/scripts/run_apps/run_experiment.py"
    run
    "$app"
    --output-suffix builtin-oracle
  )
  if [[ -n "$wine_root" ]]; then
    cmd+=(--wine-root "$wine_root")
  fi
  if [[ -n "$timeout_override" ]]; then
    cmd+=(--timeout "$timeout_override")
  fi
  if [[ "$cleanup_temp_prefix" == false ]]; then
    cmd+=(--keep-temp-prefix)
  fi

  if ! DXMT_EXPERIMENT_WINE_DLLOVERRIDES='d3d9=b' "${cmd[@]}" | tee -a "$suite_log"; then
    status=1
  fi
  printf '\n' | tee -a "$suite_log"
done

"$repo_root/scripts/run_python.sh" - "$repo_root" "$summary_json" "$summary_md" "${apps[@]}" <<'PY'
from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

repo_root = Path(sys.argv[1])
summary_json = Path(sys.argv[2])
summary_md = Path(sys.argv[3])
apps = sys.argv[4:]
output_root = repo_root / "experiments" / "output"

rows: list[dict[str, object]] = []
for app in apps:
    result_path = output_root / f"{app}-builtin-oracle" / "result.json"
    if not result_path.exists():
        rows.append(
            {
                "name": app,
                "status": "missing",
                "ssim": None,
                "failures": [{"type": "missing_result"}],
                "result_path": str(result_path),
            }
        )
        continue
    data = json.loads(result_path.read_text())
    rows.append(
        {
            "name": app,
            "status": data.get("status", "unknown"),
            "ssim": data.get("ssim"),
            "failures": data.get("failures", []),
            "result_path": str(result_path),
        }
    )

payload = {
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "apps": rows,
    "pass_count": sum(1 for row in rows if row["status"] == "pass"),
    "fail_count": sum(1 for row in rows if row["status"] != "pass"),
}
summary_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")

lines = [
    "# DX9 Builtin Oracle Suite",
    "",
    f"- generated_at: `{payload['generated_at']}`",
    f"- pass_count: `{payload['pass_count']}`",
    f"- fail_count: `{payload['fail_count']}`",
    "",
    "| app | status | ssim | result |",
    "| --- | --- | ---: | --- |",
]
for row in rows:
    ssim = row["ssim"]
    ssim_text = "-" if ssim is None else f"{float(ssim):.4f}"
    result_path = Path(str(row["result_path"]))
    rel_result = result_path.relative_to(repo_root)
    lines.append(
        f"| `{row['name']}` | `{row['status']}` | `{ssim_text}` | `{rel_result}` |"
    )
    failures = row["failures"]
    if failures:
        lines.append(f"|  | failures |  | `{json.dumps(failures, sort_keys=True)}` |")

summary_md.write_text("\n".join(lines) + "\n")
PY

printf 'suite summary:\n  %s\n  %s\n' "$summary_json" "$summary_md"
exit "$status"
