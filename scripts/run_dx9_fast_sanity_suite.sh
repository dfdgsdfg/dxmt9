#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

apps=(
  "d9vk-d3d9-clear"
  "d9vk-d3d9-buffer"
  "d9vk-d3d9-lock-matrix"
  "d9vk-d3d9-triangle"
)

usage() {
  cat <<'EOF'
Usage:
  bash scripts/run_dx9_fast_sanity_suite.sh [options]

Options:
  --wine-root <path>              Wine runtime root passed to run_experiment.py.
  --x64-pe-build-dir <path>       x64 PE build dir passed to run_experiment.py.
  --x64-runtime-pe-build-dir <path>
                                  x64 runtime PE build dir passed to run_experiment.py.
  --x86-pe-build-dir <path>       x86 PE build dir passed to run_experiment.py.
  --x86-runtime-pe-build-dir <path>
                                  x86 runtime PE build dir passed to run_experiment.py.
  --unix-build-dir <path>         Unix build dir passed to run_experiment.py.
  --timeout <seconds>             Override timeout for every experiment.
  --keep-prefixes                 Do not delete auto-created temp prefixes.
  --app <name>                    Run only the named catalogue app. Repeatable.
  --help                          Show this message.

This script:
  1. cleans stale temp prefixes
  2. cross-builds the fast sanity Win32 apps
  3. runs each app across:
     - dxmt9-x64
     - builtin-x64
     - builtin-x86

Outputs:
  experiments/output/dx9-fast-sanity/summary.json
  experiments/output/dx9-fast-sanity/summary.md
EOF
}

wine_root=""
x64_pe_build_dir=""
x64_runtime_pe_build_dir=""
x86_pe_build_dir=""
x86_runtime_pe_build_dir=""
unix_build_dir=""
timeout_override=""
cleanup_temp_prefix=true
selected_apps=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --wine-root)
      wine_root=${2:-}
      shift 2
      ;;
    --x64-pe-build-dir)
      x64_pe_build_dir=${2:-}
      shift 2
      ;;
    --x64-runtime-pe-build-dir)
      x64_runtime_pe_build_dir=${2:-}
      shift 2
      ;;
    --x86-pe-build-dir)
      x86_pe_build_dir=${2:-}
      shift 2
      ;;
    --x86-runtime-pe-build-dir)
      x86_runtime_pe_build_dir=${2:-}
      shift 2
      ;;
    --unix-build-dir)
      unix_build_dir=${2:-}
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

x64_pe_build_dir=${x64_pe_build_dir:-"$repo_root/build-win32-x64-builtin/src/win32"}
x64_runtime_pe_build_dir=${x64_runtime_pe_build_dir:-"$repo_root/build-win32-x64-builtin/src/winemetal"}
x86_pe_build_dir=${x86_pe_build_dir:-"$repo_root/build-win32-x86-builtin/src/win32"}
x86_runtime_pe_build_dir=${x86_runtime_pe_build_dir:-"$repo_root/build-win32-x86-builtin/src/winemetal"}
unix_build_dir=${unix_build_dir:-"$repo_root/build-x86_64-builtin/src"}

suite_dir="$repo_root/experiments/output/dx9-fast-sanity"
mkdir -p "$suite_dir"
summary_json="$suite_dir/summary.json"
summary_md="$suite_dir/summary.md"
suite_log="$suite_dir/suite.log"

: > "$suite_log"

status=0

cleanup_stale_prefixes() {
  printf '==> cleaning stale temp prefixes\n' | tee -a "$suite_log"
  python3 "$repo_root/scripts/cleanup_dxmt9_temp_prefixes.py" --all | tee -a "$suite_log"
  printf '\n' | tee -a "$suite_log"
}

build_apps() {
  printf '==> building fast sanity apps\n' | tee -a "$suite_log"
  bash "$repo_root/scripts/build_dx9_fast_sanity_apps.sh" | tee -a "$suite_log"
  printf '\n' | tee -a "$suite_log"
}

binary_for_app() {
  local app=$1
  local arch=$2
  case "$app" in
    d9vk-d3d9-clear)
      printf '%s/experiments/apps/D9VKD3D9Clear/d3d9-clear-%s.exe' "$repo_root" "$arch"
      ;;
    d9vk-d3d9-buffer)
      printf '%s/experiments/apps/D9VKD3D9Buffer/d3d9-buffer-%s.exe' "$repo_root" "$arch"
      ;;
    d9vk-d3d9-ffp-quirks)
      printf '%s/experiments/apps/D9VKD3D9FixedFunctionQuirks/d3d9-ffp-quirks-%s.exe' "$repo_root" "$arch"
      ;;
    d9vk-d3d9-lock-matrix)
      printf '%s/experiments/apps/D9VKD3D9LockMatrix/d3d9-lock-matrix-%s.exe' "$repo_root" "$arch"
      ;;
    d9vk-d3d9-triangle)
      printf '%s/experiments/apps/D9VKD3D9Triangle/d3d9-triangle-%s.exe' "$repo_root" "$arch"
      ;;
    *)
      return 1
      ;;
  esac
}

run_lane() {
  local app=$1
  local lane=$2
  local arch=$3
  local overrides=$4
  local pe_dir=$5
  local runtime_pe_dir=$6

  local binary
  binary=$(binary_for_app "$app" "$arch")

  local -a cmd=(
    python3
    "$repo_root/scripts/run_experiment.py"
    run
    "$app"
    --binary "$binary"
    --output-suffix "$lane"
  )
  if [[ -n "$wine_root" ]]; then
    cmd+=(--wine-root "$wine_root")
  fi
  if [[ -n "$timeout_override" ]]; then
    cmd+=(--timeout "$timeout_override")
  fi
  if [[ -n "$pe_dir" ]]; then
    cmd+=(--pe-build-dir "$pe_dir")
  fi
  if [[ -n "$runtime_pe_dir" ]]; then
    cmd+=(--runtime-pe-build-dir "$runtime_pe_dir")
  fi
  if [[ -n "$unix_build_dir" ]]; then
    cmd+=(--unix-build-dir "$unix_build_dir")
  fi
  if [[ "$cleanup_temp_prefix" == true ]]; then
    cmd+=(--cleanup-temp-prefix)
  fi

  printf '==> %s %s\n' "$app" "$lane" | tee -a "$suite_log"
  if ! DXMT_EXPERIMENT_WINE_DLLOVERRIDES="$overrides" "${cmd[@]}" | tee -a "$suite_log"; then
    status=1
  fi
  printf '\n' | tee -a "$suite_log"
}

cleanup_stale_prefixes
build_apps

for app in "${apps[@]}"; do
  run_lane "$app" "dxmt9-x64" "x64" "d3d9=n,b" "$x64_pe_build_dir" "$x64_runtime_pe_build_dir"
  run_lane "$app" "builtin-x64" "x64" "d3d9=b" "$x64_pe_build_dir" "$x64_runtime_pe_build_dir"
  run_lane "$app" "builtin-x86" "x86" "d3d9=b" "$x86_pe_build_dir" "$x86_runtime_pe_build_dir"
done

python3 - "$repo_root" "$summary_json" "$summary_md" "${apps[@]}" <<'PY'
from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

repo_root = Path(sys.argv[1])
summary_json = Path(sys.argv[2])
summary_md = Path(sys.argv[3])
apps = sys.argv[4:]
lanes = ("dxmt9-x64", "builtin-x64", "builtin-x86")
output_root = repo_root / "experiments" / "output"

rows = []
for app in apps:
    for lane in lanes:
        result_path = output_root / f"{app}-{lane}" / "result.json"
        row = {
            "app": app,
            "lane": lane,
            "status": "missing",
            "failures": [{"type": "missing_result"}],
            "result_path": str(result_path),
        }
        if result_path.exists():
            data = json.loads(result_path.read_text())
            row["status"] = data.get("status", "unknown")
            row["failures"] = data.get("failures", [])
        rows.append(row)

payload = {
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "rows": rows,
    "pass_count": sum(1 for row in rows if row["status"] == "pass"),
    "fail_count": sum(1 for row in rows if row["status"] != "pass"),
}
summary_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")

lines = [
    "# DX9 Fast Sanity Suite",
    "",
    f"- generated_at: `{payload['generated_at']}`",
    f"- pass_count: `{payload['pass_count']}`",
    f"- fail_count: `{payload['fail_count']}`",
    "",
    "| app | lane | status | result |",
    "| --- | --- | --- | --- |",
]
for row in rows:
    result_path = Path(str(row["result_path"]))
    rel_result = result_path.relative_to(repo_root)
    lines.append(f"| `{row['app']}` | `{row['lane']}` | `{row['status']}` | `{rel_result}` |")
    if row["failures"]:
      lines.append(f"|  |  | failures | `{json.dumps(row['failures'], sort_keys=True)}` |")

summary_md.write_text("\n".join(lines) + "\n")
PY

printf 'suite summary:\n  %s\n  %s\n' "$summary_json" "$summary_md"
exit "$status"
