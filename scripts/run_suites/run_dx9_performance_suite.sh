#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

apps=(
  "sample-d3d9-basic-hlsl"
  "sample-d3d9-tutorial07"
  "sample-d3d9-hdr-formats"
  "sample-d3d9-dxut-simple"
  "sample-d3d9-irrlicht-lights"
  "sample-d3d9-water-rt"
  "sample-d3d9-multitexture-terrain"
  "perf-d3d9-present-only"
  "perf-d3d9-offscreen-heavy"
  "perf-d3d9-many-draw"
)

usage() {
  cat <<'EOF'
Usage:
  bash scripts/run_suites/run_dx9_performance_suite.sh [options]

Options:
  --vanilla-wine-root <path>  Pristine Wine root used as builtin D3D9 oracle.
                              Default: ~/Library/.../Wine-11.7/.../wine
  --dxmt9-wine-root <path>    Wine root where dxmt9 will be staged.
                              Default: same vanilla Wine-11.7 (see
                              agents/rules/test_wild.rules.md — never default
                              to a Wine-*-DXMT / Wine-*-VK build).
  --timeout <seconds>         Override timeout for every experiment.
  --log-level <level>         DXMT_LOG_LEVEL for perf runs. Default: warn.
  --keep-prefixes             Do not delete auto-created temp prefixes.
  --app <name>                Run only the named catalogue app. Repeatable.
  --help                      Show this message.

This compares:
  - vanilla: Wine builtin d3d9, no dxmt9 staging
  - dxmt9:   staged dxmt9 builtin deployment

Outputs:
  experiments/output/dx9-performance-suite/summary.json
  experiments/output/dx9-performance-suite/summary.md
EOF
}

vanilla_wine_root="$HOME/Library/Application Support/heroic/tools/wine/Wine-11.7/Contents/Resources/wine"
# dxmt9 stages itself into the same vanilla Wine root by default — Wine-*-DXMT
# and Wine-*-VK builds carry custom d3d9 / wow64 patches that mask dxmt9
# regressions. See agents/rules/test_wild.rules.md.
dxmt9_wine_root="$vanilla_wine_root"
timeout_override=""
log_level="warn"
cleanup_temp_prefix=true
selected_apps=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --vanilla-wine-root)
      vanilla_wine_root=${2:-}
      shift 2
      ;;
    --dxmt9-wine-root)
      dxmt9_wine_root=${2:-}
      shift 2
      ;;
    --timeout)
      timeout_override=${2:-}
      shift 2
      ;;
    --log-level)
      log_level=${2:-}
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

suite_dir="$repo_root/experiments/output/dx9-performance-suite"
mkdir -p "$suite_dir"
summary_json="$suite_dir/summary.json"
summary_md="$suite_dir/summary.md"
suite_log="$suite_dir/suite.log"

: > "$suite_log"

cleanup_stale_prefixes() {
  printf '==> cleaning stale temp prefixes\n' | tee -a "$suite_log"
  "$repo_root/scripts/run_python.sh" "$repo_root/scripts/tools/cleanup_dxmt9_temp_prefixes.py" --all | tee -a "$suite_log"
  printf '\n' | tee -a "$suite_log"
}

run_lane() {
  local app=$1
  local lane=$2
  shift 2

  local -a cmd=(
    "$repo_root/scripts/run_python.sh"
    "$repo_root/scripts/run_apps/run_experiment.py"
    run
    "$app"
    --output-suffix "perf-$lane"
  )
  if [[ -n "$timeout_override" ]]; then
    cmd+=(--timeout "$timeout_override")
  fi
  if [[ "$cleanup_temp_prefix" == false ]]; then
    cmd+=(--keep-temp-prefix)
  fi
  cmd+=("$@")

  printf '==> %s %s\n' "$app" "$lane" | tee -a "$suite_log"
  if ! DXMT_EXPERIMENT_WINE_DLLOVERRIDES='d3d9=b' DXMT_LOG_LEVEL="$log_level" DXMT_PERF_COUNTERS="${DXMT_PERF_COUNTERS:-1}" "${cmd[@]}" | tee -a "$suite_log"; then
    printf 'warning: %s %s failed\n' "$app" "$lane" | tee -a "$suite_log"
  fi
  printf '\n' | tee -a "$suite_log"
}

cleanup_stale_prefixes

for app in "${apps[@]}"; do
  run_lane "$app" "vanilla" \
    --wine-root "$vanilla_wine_root" \
    --skip-stage \
    --stage-mingw-runtime

  run_lane "$app" "dxmt9" \
    --wine-root "$dxmt9_wine_root" \
    --pe-build-dir "$repo_root/build-win32-x64-builtin/src/win32" \
    --runtime-pe-build-dir "$repo_root/build-win32-x64-builtin/src/winemetal" \
    --unix-build-dir "$repo_root/build-x86_64-builtin/src"
done

status=$("$repo_root/scripts/run_python.sh" - "$repo_root" "$summary_json" "$summary_md" "${apps[@]}" <<'PY'
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


def read_result(app: str, lane: str) -> dict[str, object]:
    path = output_root / f"{app}-perf-{lane}" / "result.json"
    if not path.exists():
        return {
            "status": "missing",
            "failures": [{"type": "missing_result"}],
            "result_path": str(path),
            "performance": {},
        }
    data = json.loads(path.read_text())
    data["result_path"] = str(path)
    return data


rows: list[dict[str, object]] = []
for app in apps:
    vanilla = read_result(app, "vanilla")
    dxmt9 = read_result(app, "dxmt9")
    vanilla_perf = vanilla.get("performance", {}) if isinstance(vanilla.get("performance"), dict) else {}
    dxmt9_perf = dxmt9.get("performance", {}) if isinstance(dxmt9.get("performance"), dict) else {}
    dxmt9_counters = (
        dxmt9.get("dxmt9_perf_counters", {}) if isinstance(dxmt9.get("dxmt9_perf_counters"), dict) else {}
    )
    vanilla_fps = vanilla_perf.get("fps")
    dxmt9_fps = dxmt9_perf.get("fps")
    speedup = None
    if isinstance(vanilla_fps, (int, float)) and isinstance(dxmt9_fps, (int, float)) and vanilla_fps > 0:
        speedup = dxmt9_fps / vanilla_fps
    rows.append(
        {
            "name": app,
            "status": "pass"
            if vanilla.get("status") == "pass" and dxmt9.get("status") == "pass" and speedup is not None
            else "fail",
            "vanilla": vanilla,
            "dxmt9": dxmt9,
            "dxmt9_perf_counters": dxmt9_counters,
            "speedup_dxmt9_vs_vanilla": speedup,
        }
    )

payload = {
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "apps": rows,
    "pass_count": sum(1 for row in rows if row["status"] == "pass"),
    "fail_count": sum(1 for row in rows if row["status"] != "pass"),
}
summary_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")

columns = [
    "app", "status", "vanilla fps", "dxmt9 fps", "speedup",
    "present ok/skip", "boundary app/skip", "boundary wait ms",
    "boundary max ms", "dxmt9 cmdbuf", "dxmt9 mtbuf", "dxmt9 pso",
    "completion ms", "present completion ms", "draw completion ms",
    "blit completion ms", "completion max ms", "acquire ms",
    "acquire max ms", "acquire slow", "preacq hit/miss", "preacq ms",
    "preacq max ms", "writer wait ms", "commit wait ms", "sequence wait ms",
    "sync ms", "vanilla sec", "dxmt9 sec",
]
align = ["---", "---"] + ["---:"] * (len(columns) - 2)

lines = [
    "# DX9 Performance Suite",
    "",
    f"- generated_at: `{payload['generated_at']}`",
    f"- pass_count: `{payload['pass_count']}`",
    f"- fail_count: `{payload['fail_count']}`",
    "",
    "| " + " | ".join(columns) + " |",
    "| " + " | ".join(align) + " |",
]
for row in rows:
    vanilla = row["vanilla"]
    dxmt9 = row["dxmt9"]
    vp = vanilla.get("performance", {}) if isinstance(vanilla.get("performance"), dict) else {}
    dp = dxmt9.get("performance", {}) if isinstance(dxmt9.get("performance"), dict) else {}
    dc = dxmt9.get("dxmt9_perf_counters", {}) if isinstance(dxmt9.get("dxmt9_perf_counters"), dict) else {}
    vf = vp.get("fps")
    df = dp.get("fps")
    ve = vp.get("process_elapsed_sec")
    de = dp.get("process_elapsed_sec")
    speedup = row["speedup_dxmt9_vs_vanilla"]
    fmt = lambda value, digits=2: "-" if not isinstance(value, (int, float)) else f"{value:.{digits}f}"
    fmt_int = lambda value: "-" if not isinstance(value, (int, float)) else f"{int(value)}"
    lines.append(
        f"| `{row['name']}` | `{row['status']}` | `{fmt(vf)}` | `{fmt(df)}` | `{fmt(speedup, 3)}` | "
        f"`{fmt_int(dc.get('present_encoded'))}/{fmt_int(dc.get('present_skipped'))}` | "
        f"`{fmt_int(dc.get('present_boundary_applied'))}/{fmt_int(dc.get('present_boundary_skipped'))}` | "
        f"`{fmt(dc.get('present_boundary_wait_ms'), 3)}` | "
        f"`{fmt(dc.get('present_boundary_wait_max_ms'), 3)}` | "
        f"`{fmt_int(dc.get('command_buffers'))}` | `{fmt_int(dc.get('metal_buffers'))}` | "
        f"`{fmt_int(dc.get('pipeline_builds'))}` | `{fmt(dc.get('completion_wait_ms'), 3)}` | "
        f"`{fmt(dc.get('completion_present_wait_ms'), 3)}` | "
        f"`{fmt(dc.get('completion_draw_wait_ms'), 3)}` | "
        f"`{fmt(dc.get('completion_blit_wait_ms'), 3)}` | "
        f"`{fmt(dc.get('completion_wait_max_ms'), 3)}` | `{fmt(dc.get('present_acquire_wait_ms'), 3)}` | "
        f"`{fmt(dc.get('present_acquire_wait_max_ms'), 3)}` | "
        f"`{fmt_int(dc.get('present_acquire_slow_waits'))}` | "
        f"`{fmt_int(dc.get('present_preacquire_hits'))}/{fmt_int(dc.get('present_preacquire_misses'))}` | "
        f"`{fmt(dc.get('present_preacquire_wait_ms'), 3)}` | "
        f"`{fmt(dc.get('present_preacquire_wait_max_ms'), 3)}` | "
        f"`{fmt(dc.get('queue_writer_wait_ms'), 3)}` | `{fmt(dc.get('queue_commit_wait_ms'), 3)}` | "
        f"`{fmt(dc.get('queue_sequence_wait_ms'), 3)}` | `{fmt(dc.get('sync_wait_ms'), 3)}` | "
        f"`{fmt(ve)}` | `{fmt(de)}` |"
    )
    if vanilla.get("status") != "pass":
        cells = ["", "vanilla_failures"] + [""] * (len(columns) - 3) + [
            f"`{json.dumps(vanilla.get('failures', []), sort_keys=True)}`"
        ]
        lines.append(
            "| " + " | ".join(cells) + " |"
        )
    if dxmt9.get("status") != "pass":
        cells = ["", "dxmt9_failures"] + [""] * (len(columns) - 3) + [
            f"`{json.dumps(dxmt9.get('failures', []), sort_keys=True)}`"
        ]
        lines.append(
            "| " + " | ".join(cells) + " |"
        )

summary_md.write_text("\n".join(lines) + "\n")
print(0 if payload["fail_count"] == 0 else 1)
PY
)

printf 'suite summary:\n  %s\n  %s\n' "$summary_json" "$summary_md"
exit "$status"
