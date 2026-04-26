#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from statistics import mean


REPO_ROOT = Path(__file__).resolve().parent.parent
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
COUNTER_FIELDS = [
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
    "command_buffers",
    "completion_present_wait_ms",
    "queue_writer_wait_ms",
    "queue_commit_wait_ms",
]


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
    return {
        "mean": mean(values),
        "min": min(values),
        "max": max(values),
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
        str(REPO_ROOT / "scripts" / "run_experiment.py"),
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


def write_markdown(path: Path, payload: dict[str, object]) -> None:
    rows = payload["summary"]
    assert isinstance(rows, list)
    lines = [
        "# DX9 Present Policy A/B",
        "",
        f"- generated_at: `{payload['generated_at']}`",
        f"- tag: `{payload['tag']}`",
        f"- runs_per_app_mode: `{payload['runs_per_app_mode']}`",
        "",
        "| app | mode | pass | fps mean [min,max] | present encoded | fallbacks | boundary ms | acquire ms | token ms | command buffers |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|",
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
            f"`{fmt_summary(counters.get('present_encoded'), 1)}` | "
            f"`{fmt_summary(counters.get('present_async_acquire_fallbacks'), 1)}` | "
            f"`{fmt_summary(counters.get('present_boundary_wait_ms'), 3)}` | "
            f"`{fmt_summary(counters.get('present_acquire_wait_ms'), 3)}` | "
            f"`{fmt_summary(counters.get('present_token_wait_ms'), 3)}` | "
            f"`{fmt_summary(counters.get('command_buffers'), 1)}` |"
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

    payload: dict[str, object] = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "tag": tag,
        "apps": apps,
        "modes": modes,
        "runs_per_app_mode": args.runs,
        "results": results,
        "summary": aggregate(results, apps, modes),
    }
    summary_json = out_dir / "summary.json"
    summary_md = out_dir / "summary.md"
    summary_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    write_markdown(summary_md, payload)
    print(f"summary_json: {summary_json}")
    print(f"summary_md: {summary_md}")
    print(f"suite_log: {suite_log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
