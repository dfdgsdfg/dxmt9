#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from statistics import mean, median, pstdev


REPO_ROOT = Path(__file__).resolve().parents[2]
OUTPUT_ROOT = REPO_ROOT / "experiments" / "output"
SUMMARY_ROOT = OUTPUT_ROOT / "dx9-present-policy-ab"
DEFAULT_WINE_ROOT = (
    Path.home()
    / "Library/Application Support/heroic/tools/wine/Wine-11.6-DXMT/Contents/Resources/wine"
)

MODE_ENV = {
    "default": {},
    "async": {
        "DXMT9_PRESENT_ASYNC_ACQUIRE": "1",
    },
    "cap": {
        "DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS": "1",
    },
    "preacquire": {
        "DXMT9_PRESENT_PREACQUIRE": "1",
    },
    "preacquire-cap": {
        "DXMT9_PRESENT_PREACQUIRE": "1",
        "DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS": "1",
    },
    "async-cap": {
        "DXMT9_PRESENT_ASYNC_ACQUIRE": "1",
        "DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS": "1",
    },
    "async-cap-completion": {
        "DXMT9_PRESENT_ASYNC_ACQUIRE": "1",
        "DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS": "1",
        "DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION": "1",
    },
}

DEFAULT_APPS = ["dx-sdk-basichlsl", "dx-sdk-tutorial07"]
DEFAULT_MODES = ["default", "async", "async-cap"]
PRESENT_COUNTER_FIELDS = [
    "present_encoded",
    "present_skipped",
    "present_async_acquire_issued",
    "present_async_acquire_fallbacks",
    "present_boundary_wait_ms",
    "present_boundary_wait_max_ms",
    "present_acquire_wait_ms",
    "present_acquire_wait_max_ms",
    "present_async_acquire_wait_ms",
    "present_token_wait_ms",
    "present_token_wait_max_ms",
    "present_preacquire_requests",
    "present_preacquire_hits",
    "present_preacquire_misses",
    "present_preacquire_wait_ms",
    "present_preacquire_wait_max_ms",
    "command_buffers",
    "completion_present_wait_ms",
    "queue_writer_wait_ms",
    "queue_commit_wait_ms",
]

# Backend counter fields surfaced in summary.json for cross-policy diff.
# Sourced from kCounterTable: R-BACK-12 (uniform_*), R-BACK-15
# (render_pass_*), R-BACK-2.10 (chunk_*), R-BACK-2.27 (ring_arena_*).
# Mode-to-mode delta on these fields is what reveals whether an upstream
# encoder change leaked into the present-policy A/B run.
BACKEND_COUNTER_FIELDS = [
    "uniform_vs_consts_calls",
    "uniform_vs_consts_bytes",
    "uniform_ps_consts_calls",
    "uniform_ps_consts_bytes",
    "uniform_ffp_vs_calls",
    "uniform_ffp_vs_bytes",
    "uniform_ffp_ps_calls",
    "uniform_ffp_ps_bytes",
    "uniform_volatile_pushes",
    "render_pass_load_action_load",
    "render_pass_load_action_clear",
    "render_pass_load_action_dontcare",
    "render_pass_load_action_depth_load",
    "render_pass_load_action_depth_clear",
    "render_pass_load_action_depth_dontcare",
    "render_pass_load_action_stencil_load",
    "render_pass_load_action_stencil_clear",
    "render_pass_load_action_stencil_dontcare",
    "render_pass_store_action_store",
    "render_pass_store_action_dontcare",
    "render_pass_store_action_resolve",
    "render_pass_store_action_depth_store",
    "render_pass_store_action_depth_dontcare",
    "render_pass_store_action_stencil_store",
    "render_pass_store_action_stencil_dontcare",
    "render_pass_tile_preservation_bytes",
    "chunk_admit",
    "chunk_reject",
    # V1 boundary B2 (audit item (b)) — bridge commit latency in raw
    # nanoseconds. Surfaced here so cross-policy A/B diff can detect a
    # bridge-ABI regression (bigger marshalling struct, extra importer
    # validation) that chunk_admit alone would not catch.
    "bridge_commit_latency_ns",
    "bridge_commit_latency_max_ns",
    "bridge_commit_latency_p50_ns",
    "bridge_commit_latency_p95_ns",
    "bridge_commit_latency_p99_ns",
    # R-BACK-2.29..2.32 — sub-command-buffer chain length and total
    # mid-chunk commits. Both are zero in default policy=off; positive
    # under DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass / per-n-records.
    "sub_command_buffers",
    "chunk_subcb_count_max",
    "ring_arena_heap_fallback_count",
    "ring_arena_heap_fallback_bytes",
    "ring_arena_heap_fallback_argbuf",
    "ring_arena_heap_fallback_lambda",
    "ring_arena_heap_fallback_staging",
    "ring_arena_heap_fallback_copytemp",
    # GPU faults (M5). Always-zero in healthy runs; surfaced so the A/B
    # diff cannot silently regress to a faulting policy.
    "gpu_command_buffer_errors",
    # Per-command-buffer GPU wall time (M4). Apple Silicon timestamps
    # via MTLCommandBuffer.GPUStartTime/GPUEndTime sampled at Completed.
    # Distribution lets policy A/B detect GPU regressions independent of
    # CPU encode time.
    "gpu_command_buffer_time_ms",
    "gpu_command_buffer_time_max_ms",
    "gpu_command_buffer_time_samples",
    "gpu_command_buffer_time_p50_ms",
    "gpu_command_buffer_time_p95_ms",
    "gpu_command_buffer_time_p99_ms",
]

COUNTER_FIELDS = PRESENT_COUNTER_FIELDS + BACKEND_COUNTER_FIELDS


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Repeat dxmt9 D3D9 present-policy experiments and summarize A/B results."
    )
    parser.add_argument("--app", action="append", dest="apps", help="Catalogue app to run. Repeatable.")
    parser.add_argument(
        "--mode",
        action="append",
        dest="modes",
        choices=sorted(MODE_ENV),
        help="Policy mode to run. Repeatable.",
    )
    parser.add_argument("--runs", type=int, default=3, help="Runs per app/mode. Default: 3.")
    parser.add_argument(
        "--cv-tolerance",
        type=float,
        default=0.0,
        help=(
            "R-BENCH-1.2 determinism gate: fail when fps coefficient-of-"
            "variation (max-min)/mean exceeds this. 0.0 = report only "
            "(default). 0.02 = 2%% spread tolerance."
        ),
    )
    parser.add_argument("--timeout", type=str, default="60", help="Per-run timeout seconds. Default: 60.")
    parser.add_argument("--log-level", default="warn", help="DXMT_LOG_LEVEL. Default: warn.")
    parser.add_argument("--wine-root", type=Path, default=DEFAULT_WINE_ROOT, help="DXMT Wine root.")
    parser.add_argument("--tag", default="", help="Output tag. Default: timestamp.")
    parser.add_argument("--keep-prefixes", action="store_true", help="Keep temporary Wine prefixes.")
    return parser.parse_args()


def load_json(path: Path) -> dict[str, object]:
    if not path.exists():
        return {
            "status": "missing",
            "failures": [{"type": "missing_result"}],
            "result_path": str(path),
        }
    data = json.loads(path.read_text())
    data["result_path"] = str(path)
    return data


def numeric(value: object) -> float | None:
    if isinstance(value, (int, float)):
        return float(value)
    return None


def summarize_values(values: list[float]) -> dict[str, float] | None:
    if not values:
        return None
    avg = mean(values)
    lo = min(values)
    hi = max(values)
    # CV is reported against the mean using the spread (max-min)/mean,
    # which is the form R-BENCH-1.2 cares about: a single regression run
    # whose worst-case sample drifts by more than the tolerance fails the
    # determinism gate. pstdev is included for distribution shape but is
    # not what the gate checks against.
    cv = (hi - lo) / avg if avg > 0 else 0.0
    return {
        "mean": avg,
        "min": lo,
        "max": hi,
        "median": median(values),
        "stddev": pstdev(values) if len(values) >= 2 else 0.0,
        "cv": cv,
    }


def preserve_trace_files() -> dict[Path, bytes | None]:
    snapshots: dict[Path, bytes | None] = {}
    for path in (REPO_ROOT / "experiments" / "apps").glob("**/*.trace.txt"):
        snapshots[path] = path.read_bytes() if path.exists() else None
    return snapshots


def restore_trace_files(snapshots: dict[Path, bytes | None]) -> None:
    for path, data in snapshots.items():
        if data is None:
            if path.exists():
                path.unlink()
        else:
            path.write_bytes(data)


def run_one(
    app: str,
    mode: str,
    run_index: int,
    args: argparse.Namespace,
    tag: str,
    suite_log: Path,
) -> dict[str, object]:
    suffix = f"{tag}-{mode}-r{run_index:02d}"
    output_dir = OUTPUT_ROOT / f"{app}-{suffix}"
    cmd = [
        sys.executable,
        str(REPO_ROOT / "scripts" / "run_apps" / "run_experiment.py"),
        "run",
        app,
        "--output-suffix",
        suffix,
        "--timeout",
        args.timeout,
        "--wine-root",
        str(args.wine_root),
        "--pe-build-dir",
        str(REPO_ROOT / "build-win32-x64-builtin" / "src" / "win32"),
        "--runtime-pe-build-dir",
        str(REPO_ROOT / "build-win32-x64-builtin" / "src" / "winemetal"),
        "--unix-build-dir",
        str(REPO_ROOT / "build-x86_64-builtin" / "src"),
    ]
    if not args.keep_prefixes:
        cmd.append("--cleanup-temp-prefix")

    env = os.environ.copy()
    env.update(
        {
            "DXMT_EXPERIMENT_WINE_DLLOVERRIDES": "d3d9=b",
            "DXMT_LOG_LEVEL": args.log_level,
            "DXMT_PERF_COUNTERS": env.get("DXMT_PERF_COUNTERS", "1"),
        }
    )
    for key in MODE_ENV:
        for env_key in MODE_ENV[key]:
            env.pop(env_key, None)
    env.update(MODE_ENV[mode])

    with suite_log.open("a") as log:
        log.write(f"==> app={app} mode={mode} run={run_index}\n")
        log.flush()
        process = subprocess.run(cmd, cwd=REPO_ROOT, env=env, text=True, stdout=log, stderr=log)
        log.write(f"<== returncode={process.returncode}\n\n")

    result = load_json(output_dir / "result.json")
    result["app"] = app
    result["mode"] = mode
    result["run"] = run_index
    result["process_returncode"] = process.returncode
    result["output_dir"] = str(output_dir)
    return result


def aggregate(results: list[dict[str, object]], apps: list[str], modes: list[str]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for app in apps:
        for mode in modes:
            matching = [r for r in results if r.get("app") == app and r.get("mode") == mode]
            fps_values: list[float] = []
            elapsed_values: list[float] = []
            counters_by_field: dict[str, list[float]] = {field: [] for field in COUNTER_FIELDS}
            pass_count = 0
            for result in matching:
                if result.get("status") == "pass":
                    pass_count += 1
                perf = result.get("performance")
                if isinstance(perf, dict):
                    fps = numeric(perf.get("fps"))
                    elapsed = numeric(perf.get("process_elapsed_sec"))
                    if fps is not None:
                        fps_values.append(fps)
                    if elapsed is not None:
                        elapsed_values.append(elapsed)
                counters = result.get("dxmt9_perf_counters")
                if isinstance(counters, dict):
                    for field in COUNTER_FIELDS:
                        value = numeric(counters.get(field))
                        if value is not None:
                            counters_by_field[field].append(value)
            row: dict[str, object] = {
                "app": app,
                "mode": mode,
                "runs": len(matching),
                "pass_count": pass_count,
                "fps": summarize_values(fps_values),
                "elapsed_sec": summarize_values(elapsed_values),
                "counters": {
                    field: summarize_values(values)
                    for field, values in counters_by_field.items()
                    if values
                },
            }
            rows.append(row)
    return rows


def fmt_summary(value: object, digits: int = 2) -> str:
    if not isinstance(value, dict):
        return "-"
    mean_value = value.get("mean")
    min_value = value.get("min")
    max_value = value.get("max")
    if not all(isinstance(v, (int, float)) for v in (mean_value, min_value, max_value)):
        return "-"
    return f"{mean_value:.{digits}f} [{min_value:.{digits}f}, {max_value:.{digits}f}]"


def evaluate_cv_gate(
    rows: list[dict[str, object]], tolerance: float
) -> list[dict[str, object]]:
    """Return a list of rows whose fps cv exceeds the tolerance.

    R-BENCH-1.2 determinism gate. tolerance==0 disables enforcement (the
    cv field is still computed and reported, just not gated against).
    """
    violations: list[dict[str, object]] = []
    if tolerance <= 0.0:
        return violations
    for row in rows:
        fps = row.get("fps")
        if not isinstance(fps, dict):
            continue
        cv = fps.get("cv")
        if not isinstance(cv, (int, float)):
            continue
        if cv > tolerance:
            violations.append(
                {
                    "app": row.get("app"),
                    "mode": row.get("mode"),
                    "fps_cv": float(cv),
                    "fps_mean": fps.get("mean"),
                    "fps_min": fps.get("min"),
                    "fps_max": fps.get("max"),
                    "tolerance": tolerance,
                }
            )
    return violations


def fmt_cv(value: object, tolerance: float) -> str:
    if not isinstance(value, dict):
        return "-"
    cv = value.get("cv")
    if not isinstance(cv, (int, float)):
        return "-"
    marker = ""
    if tolerance > 0.0 and cv > tolerance:
        marker = " ⚠️"
    return f"{cv * 100:.2f}%{marker}"


def write_markdown(path: Path, payload: dict[str, object]) -> None:
    rows = payload["summary"]
    assert isinstance(rows, list)
    tolerance = payload.get("cv_tolerance")
    if not isinstance(tolerance, (int, float)):
        tolerance = 0.0
    violations = payload.get("cv_violations")
    if not isinstance(violations, list):
        violations = []
    lines = [
        "# DX9 Present Policy A/B",
        "",
        f"- generated_at: `{payload['generated_at']}`",
        f"- tag: `{payload['tag']}`",
        f"- runs_per_app_mode: `{payload['runs_per_app_mode']}`",
        f"- cv_tolerance: `{tolerance:.4f}`"
        + (" (report-only)" if tolerance == 0.0 else ""),
        "",
        "| app | mode | pass | fps mean [min,max] | fps cv | present encoded | fallbacks | boundary ms | acquire ms | token ms | pre hits | pre misses | pre wait ms | command buffers |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        assert isinstance(row, dict)
        counters = row.get("counters") if isinstance(row.get("counters"), dict) else {}
        assert isinstance(counters, dict)
        lines.append(
            "| "
            f"`{row.get('app')}` | "
            f"`{row.get('mode')}` | "
            f"`{row.get('pass_count')}/{row.get('runs')}` | "
            f"`{fmt_summary(row.get('fps'))}` | "
            f"`{fmt_cv(row.get('fps'), float(tolerance))}` | "
            f"`{fmt_summary(counters.get('present_encoded'), 1)}` | "
            f"`{fmt_summary(counters.get('present_async_acquire_fallbacks'), 1)}` | "
            f"`{fmt_summary(counters.get('present_boundary_wait_ms'), 3)}` | "
            f"`{fmt_summary(counters.get('present_acquire_wait_ms'), 3)}` | "
            f"`{fmt_summary(counters.get('present_token_wait_ms'), 3)}` | "
            f"`{fmt_summary(counters.get('present_preacquire_hits'), 1)}` | "
            f"`{fmt_summary(counters.get('present_preacquire_misses'), 1)}` | "
            f"`{fmt_summary(counters.get('present_preacquire_wait_ms'), 3)}` | "
            f"`{fmt_summary(counters.get('command_buffers'), 1)}` |"
        )
    backend_groups: list[tuple[str, list[tuple[str, str, int]]]] = [
        (
            "Uniform pushes (R-BACK-12)",
            [
                ("uniform_vs_consts_calls", "vs.calls", 0),
                ("uniform_ps_consts_calls", "ps.calls", 0),
                ("uniform_ffp_vs_calls", "ffp.vs.calls", 0),
                ("uniform_ffp_ps_calls", "ffp.ps.calls", 0),
                ("uniform_volatile_pushes", "volatile", 0),
            ],
        ),
        (
            "Render-pass actions (R-BACK-15)",
            [
                ("render_pass_load_action_load", "color.load", 0),
                ("render_pass_load_action_dontcare", "color.dontcare", 0),
                ("render_pass_store_action_store", "color.store", 0),
                ("render_pass_store_action_dontcare", "color.dontcare.s", 0),
                ("render_pass_store_action_depth_dontcare", "depth.dontcare.s", 0),
                ("render_pass_tile_preservation_bytes", "tile.bytes", 0),
            ],
        ),
        (
            "Bridge / arena (R-BACK-2.10/2.27)",
            [
                ("chunk_admit", "chunk.admit", 0),
                ("chunk_reject", "chunk.reject", 0),
                ("ring_arena_heap_fallback_count", "heap.fb.count", 0),
                ("ring_arena_heap_fallback_bytes", "heap.fb.bytes", 0),
            ],
        ),
    ]
    for title, fields in backend_groups:
        lines.append("")
        lines.append(f"## Backend metrics — {title}")
        lines.append("")
        header = "| app | mode | " + " | ".join(label for _, label, _ in fields) + " |"
        sep = "|---|---|" + "|".join(["---:"] * len(fields)) + "|"
        lines.append(header)
        lines.append(sep)
        for row in rows:
            assert isinstance(row, dict)
            counters = (
                row.get("counters") if isinstance(row.get("counters"), dict) else {}
            )
            assert isinstance(counters, dict)
            cells = " | ".join(
                f"`{fmt_summary(counters.get(key), digits)}`"
                for key, _, digits in fields
            )
            lines.append(f"| `{row.get('app')}` | `{row.get('mode')}` | {cells} |")

    if violations:
        lines.append("")
        lines.append(
            f"## ⚠️ Determinism gate violations (cv > {float(tolerance):.4f})"
        )
        lines.append("")
        lines.append("| app | mode | fps cv | fps mean | fps min | fps max |")
        lines.append("|---|---|---:|---:|---:|---:|")
        for v in violations:
            assert isinstance(v, dict)
            cv = v.get("fps_cv")
            mean_v = v.get("fps_mean")
            min_v = v.get("fps_min")
            max_v = v.get("fps_max")
            cv_s = f"{cv * 100:.2f}%" if isinstance(cv, (int, float)) else "-"
            mean_s = f"{mean_v:.2f}" if isinstance(mean_v, (int, float)) else "-"
            min_s = f"{min_v:.2f}" if isinstance(min_v, (int, float)) else "-"
            max_s = f"{max_v:.2f}" if isinstance(max_v, (int, float)) else "-"
            lines.append(
                f"| `{v.get('app')}` | `{v.get('mode')}` | `{cv_s}` | `{mean_s}` | `{min_s}` | `{max_s}` |"
            )
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    if args.runs < 1:
        raise SystemExit("--runs must be >= 1")
    apps = args.apps or DEFAULT_APPS
    modes = args.modes or DEFAULT_MODES
    tag = args.tag or datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    out_dir = SUMMARY_ROOT / tag
    out_dir.mkdir(parents=True, exist_ok=True)
    suite_log = out_dir / "suite.log"
    suite_log.write_text("")

    snapshots = preserve_trace_files()
    results: list[dict[str, object]] = []
    try:
        for run_index in range(1, args.runs + 1):
            for app in apps:
                for mode in modes:
                    print(f"==> {app} {mode} run {run_index}/{args.runs}", flush=True)
                    results.append(run_one(app, mode, run_index, args, tag, suite_log))
    finally:
        restore_trace_files(snapshots)

    summary_rows = aggregate(results, apps, modes)
    violations = evaluate_cv_gate(summary_rows, args.cv_tolerance)
    payload: dict[str, object] = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "tag": tag,
        "apps": apps,
        "modes": modes,
        "runs_per_app_mode": args.runs,
        "cv_tolerance": args.cv_tolerance,
        "cv_violations": violations,
        "results": results,
        "summary": summary_rows,
    }
    summary_json = out_dir / "summary.json"
    summary_md = out_dir / "summary.md"
    summary_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    write_markdown(summary_md, payload)
    print(f"summary_json: {summary_json}")
    print(f"summary_md: {summary_md}")
    print(f"suite_log: {suite_log}")
    if violations:
        print(
            f"determinism gate: FAIL — {len(violations)} row(s) over "
            f"cv tolerance {args.cv_tolerance:.4f}:",
            file=sys.stderr,
        )
        for v in violations:
            cv = v.get("fps_cv")
            cv_s = f"{cv * 100:.2f}%" if isinstance(cv, (int, float)) else "-"
            print(
                f"  {v.get('app')} / {v.get('mode')}: fps cv={cv_s}",
                file=sys.stderr,
            )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
