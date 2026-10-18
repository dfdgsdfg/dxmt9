#!/usr/bin/env python3
"""Qualify repeated observer-off/deferred/async workload controls.

This is an evidence gate, not a workload launcher. It accepts one immutable
JSON record per run and refuses missing measurements or unmatched provenance.
It never fills absent metrics with zero and never synthesizes an off control.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA_RUN = "dxmt9.trace.observer-control-run.v1"
SCHEMA_BUDGET = "dxmt9.trace.observer-control-budget.v1"
SCHEMA_REPORT = "dxmt9.trace.observer-control-report.v1"
MODES = ("observer-off", "deferred", "async")
PROVENANCE_FIELDS = ("workload", "build", "runtime", "renderer", "profile", "selection")
METRIC_FIELDS = (
    "wall_elapsed_ms", "process_cpu_ms", "producer_cpu_ms", "worker_cpu_ms",
    "peak_memory_bytes", "buffer_occupancy_peak", "producer_notifications",
    "bridge_calls", "source_loss", "sdk_loss", "output_loss",
)
BUDGET_FIELDS = (
    "max_process_cpu_regression_pct", "max_frame_p50_regression_pct",
    "max_frame_p95_regression_pct", "max_frame_p99_regression_pct",
    "max_producer_cpu_ms", "max_worker_cpu_ms", "max_peak_memory_bytes",
    "max_buffer_occupancy", "max_producer_notifications_per_frame",
    "max_bridge_calls_per_frame_delta", "max_source_loss", "max_sdk_loss",
    "max_output_loss",
)


def _digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def _json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def _number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    value = float(value)
    return value if math.isfinite(value) and value >= 0 else None


def _percentile(values: list[float], percentile: float) -> float:
    ordered = sorted(values)
    # Nearest-rank percentile, with rank clamped to the nonempty population.
    return ordered[max(0, math.ceil(percentile * len(ordered)) - 1)]


def _validate_budget(budget: dict[str, Any], runs: list[dict[str, Any]]) -> list[str]:
    errors: list[str] = []
    if budget.get("schema") != SCHEMA_BUDGET:
        errors.append("budget_schema_invalid")
    frozen_at = budget.get("frozen_at")
    try:
        frozen = datetime.fromisoformat(frozen_at.replace("Z", "+00:00"))
        if frozen.tzinfo is None:
            raise ValueError("timezone required")
    except (AttributeError, TypeError, ValueError):
        errors.append("budget_frozen_at_invalid")
        frozen = None
    thresholds = budget.get("thresholds")
    if not isinstance(thresholds, dict):
        errors.append("budget_thresholds_missing")
        thresholds = {}
    for field in BUDGET_FIELDS:
        value = _number(thresholds.get(field))
        if value is None:
            errors.append(f"budget_threshold_missing_or_invalid:{field}")
    if frozen is not None:
        for run in runs:
            try:
                started = datetime.fromisoformat(run["started_at"].replace("Z", "+00:00"))
                if started.tzinfo is None or frozen >= started:
                    errors.append(f"budget_not_frozen_before_run:{run.get('run_id', 'unknown')}")
            except (AttributeError, KeyError, TypeError, ValueError):
                errors.append(f"run_started_at_invalid:{run.get('run_id', 'unknown')}")
    return errors


def qualify(runs: list[tuple[str, dict[str, Any], str]], budget: dict[str, Any],
            budget_sha256: str) -> dict[str, Any]:
    """Qualify run records; tuple is (source path, record, source digest)."""
    errors: list[str] = []
    by_mode: dict[str, list[tuple[str, dict[str, Any], str]]] = {mode: [] for mode in MODES}
    identities: set[str] = set()
    for source, run, digest in runs:
        mode = run.get("mode")
        if run.get("schema") != SCHEMA_RUN:
            errors.append(f"run_schema_invalid:{source}")
        if mode not in by_mode:
            errors.append(f"run_mode_invalid:{source}")
            continue
        run_id = run.get("run_id")
        if not isinstance(run_id, str) or not run_id or run_id in identities:
            errors.append(f"run_id_missing_or_duplicate:{source}")
        else:
            identities.add(run_id)
        provenance = run.get("provenance")
        if not isinstance(provenance, dict) or any(key not in provenance for key in PROVENANCE_FIELDS):
            errors.append(f"provenance_incomplete:{source}")
        if run.get("workload_status") != "pass":
            errors.append(f"workload_not_pass:{source}")
        if run.get("correctness") is not True:
            errors.append(f"correctness_not_pass:{source}")
        if mode == "observer-off":
            if run.get("capture_status") != "disabled":
                errors.append(f"observer_off_not_disabled:{source}")
        else:
            if run.get("capture_status") != "complete":
                errors.append(f"capture_incomplete:{source}")
            receipt = run.get("trace_receipt")
            if not isinstance(receipt, dict) or receipt.get("qualified") is not True:
                errors.append(f"trace_receipt_not_qualified:{source}")
            elif not isinstance(receipt.get("sha256"), str) or len(receipt["sha256"]) != 64:
                errors.append(f"trace_receipt_digest_missing:{source}")
        metrics = run.get("metrics")
        metric_sources = run.get("metric_sources")
        if not isinstance(metrics, dict):
            errors.append(f"metrics_missing:{source}")
            metrics = {}
        if not isinstance(metric_sources, dict):
            errors.append(f"metric_sources_missing:{source}")
            metric_sources = {}
        for field in METRIC_FIELDS:
            if _number(metrics.get(field)) is None:
                errors.append(f"metric_unavailable_or_invalid:{field}:{source}")
            if not isinstance(metric_sources.get(field), str) or not metric_sources[field]:
                errors.append(f"metric_source_missing:{field}:{source}")
        frames = metrics.get("frame_time_ms")
        if (not isinstance(frames, list) or not frames
                or any(_number(value) is None or value == 0 for value in frames)):
            errors.append(f"frame_time_population_missing_or_invalid:{source}")
        else:
            if not isinstance(metric_sources.get("frame_time_ms"), str) or not metric_sources["frame_time_ms"]:
                errors.append(f"metric_source_missing:frame_time_ms:{source}")
            if metrics.get("frames") != len(frames):
                errors.append(f"frame_count_mismatch:{source}")
        by_mode[mode].append((source, run, digest))

    counts = {mode: len(by_mode[mode]) for mode in MODES}
    if any(count < 3 for count in counts.values()):
        errors.append("requires_at_least_three_runs_per_mode")
    if len(set(counts.values())) != 1:
        errors.append("mode_run_counts_mismatch")
    for mode in MODES:
        reps = [item[1].get("repetition_id") for item in by_mode[mode]]
        if any(not isinstance(rep, str) or not rep for rep in reps) or len(set(reps)) != len(reps):
            errors.append(f"repetition_id_missing_or_duplicate:{mode}")
    rep_sets = [set(item[1].get("repetition_id") for item in by_mode[mode]) for mode in MODES]
    if rep_sets and any(reps != rep_sets[0] for reps in rep_sets[1:]):
        errors.append("matched_repetition_ids_mismatch")

    reference_provenance = next((item[1].get("provenance") for item in by_mode[MODES[0]]), None)
    if isinstance(reference_provenance, dict):
        for mode in MODES:
            for source, run, _ in by_mode[mode]:
                if run.get("provenance") != reference_provenance:
                    errors.append(f"matched_provenance_mismatch:{source}")

    all_runs = [item[1] for mode in MODES for item in by_mode[mode]]
    errors.extend(_validate_budget(budget, all_runs))
    thresholds = budget.get("thresholds", {}) if isinstance(budget.get("thresholds"), dict) else {}
    report: dict[str, Any] = {
        "schema": SCHEMA_REPORT,
        "status": "unqualified",
        "qualified": False,
        "budget": {"sha256": budget_sha256, "frozen_at": budget.get("frozen_at"),
                   "thresholds": thresholds},
        "runs": [{"path": source, "sha256": digest, "mode": run.get("mode"),
                  "run_id": run.get("run_id"), "repetition_id": run.get("repetition_id"),
                  "trace_receipt": run.get("trace_receipt")}
                 for source, run, digest in runs],
        "metrics": {},
        "errors": errors,
        "limitations": [
            "This report qualifies supplied workload evidence only; it does not launch or measure workloads.",
            "Frame percentiles use the supplied complete frame_time_ms populations (nearest-rank).",
        ],
    }
    if not errors:
        medians: dict[str, dict[str, float]] = {}
        frame_stats: dict[str, dict[str, float]] = {}
        for mode in MODES:
            entries = by_mode[mode]
            medians[mode] = {
                field: statistics.median(float(item[1]["metrics"][field]) for item in entries)
                for field in METRIC_FIELDS
            }
            frames = [float(value) for item in entries
                      for value in item[1]["metrics"]["frame_time_ms"]]
            frame_stats[mode] = {f"p{p}": _percentile(frames, p / 100)
                                 for p in (50, 95, 99)}
        comparisons: dict[str, Any] = {}
        off = medians["observer-off"]
        off_frames = frame_stats["observer-off"]
        for mode in ("deferred", "async"):
            current = medians[mode]
            current_frames = frame_stats[mode]
            comparisons[mode] = {
                "process_cpu_regression_pct": _regression(current["process_cpu_ms"], off["process_cpu_ms"]),
                "frame_time_regression_pct": {
                    f"p{p}": _regression(current_frames[f"p{p}"], off_frames[f"p{p}"])
                    for p in (50, 95, 99)
                },
                "producer_cpu_ms": current["producer_cpu_ms"],
                "worker_cpu_ms": current["worker_cpu_ms"],
                "peak_memory_bytes": current["peak_memory_bytes"],
                "buffer_occupancy_peak": current["buffer_occupancy_peak"],
                "producer_notifications_per_frame": _per_frame(current["producer_notifications"], by_mode[mode]),
                "bridge_calls_per_frame_delta": (
                    _per_frame(current["bridge_calls"], by_mode[mode])
                    - _per_frame(off["bridge_calls"], by_mode["observer-off"])
                ),
                "loss": {name: current[name] for name in ("source_loss", "sdk_loss", "output_loss")},
            }
            c = comparisons[mode]
            checks = (
                (c["process_cpu_regression_pct"] <= thresholds["max_process_cpu_regression_pct"], "process_cpu_regression"),
                (all(c["frame_time_regression_pct"][f"p{p}"] <= thresholds[f"max_frame_p{p}_regression_pct"] for p in (50, 95, 99)), "frame_time_regression"),
                (c["producer_cpu_ms"] <= thresholds["max_producer_cpu_ms"], "producer_cpu_budget"),
                (c["worker_cpu_ms"] <= thresholds["max_worker_cpu_ms"], "worker_cpu_budget"),
                (c["peak_memory_bytes"] <= thresholds["max_peak_memory_bytes"], "memory_budget"),
                (c["buffer_occupancy_peak"] <= thresholds["max_buffer_occupancy"], "occupancy_budget"),
                (c["producer_notifications_per_frame"] <= thresholds["max_producer_notifications_per_frame"], "notification_budget"),
                (c["bridge_calls_per_frame_delta"] <= thresholds["max_bridge_calls_per_frame_delta"], "bridge_call_budget"),
                (c["loss"]["source_loss"] <= thresholds["max_source_loss"], "source_loss_budget"),
                (c["loss"]["sdk_loss"] <= thresholds["max_sdk_loss"], "sdk_loss_budget"),
                (c["loss"]["output_loss"] <= thresholds["max_output_loss"], "output_loss_budget"),
            )
            report.setdefault("budget_failures", []).extend(
                f"{mode}:{name}" for ok, name in checks if not ok
            )
        report["metrics"] = {"run_medians": medians, "frame_time_percentiles_ms": frame_stats,
                              "comparisons_to_observer_off": comparisons}
        if report.get("budget_failures"):
            report["errors"].extend(report["budget_failures"])
        report["qualified"] = not report["errors"]
        report["status"] = "qualified" if report["qualified"] else "unqualified"
    return report


def _regression(value: float, reference: float) -> float:
    return (value / reference - 1.0) * 100.0 if reference > 0 else math.inf


def _per_frame(value: float, rows: list[tuple[str, dict[str, Any], str]]) -> float:
    frames = sum(int(item[1]["metrics"]["frames"]) for item in rows)
    return value / frames if frames else math.inf


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--budget", required=True, type=Path,
                        help="Numeric budget JSON frozen before the first run.")
    parser.add_argument("--off", action="append", required=True, type=Path,
                        help="Observer-off run record; repeat at least three times.")
    parser.add_argument("--deferred", action="append", required=True, type=Path,
                        help="Deferred-output run record; repeat at least three times.")
    parser.add_argument("--async", dest="asynchronous", action="append", required=True, type=Path,
                        help="Async-output run record; repeat at least three times.")
    parser.add_argument("--output", required=True, type=Path, help="Qualification report JSON path.")
    args = parser.parse_args(argv)
    try:
        budget = _json(args.budget)
        paths = [(mode, path) for mode, values in (
            ("observer-off", args.off), ("deferred", args.deferred), ("async", args.asynchronous)
        ) for path in values]
        runs = []
        for expected_mode, path in paths:
            run = _json(path)
            if run.get("mode") != expected_mode:
                raise ValueError(f"{path}: mode {run.get('mode')!r} does not match --{expected_mode}")
            runs.append((str(path.resolve()), run, _digest(path)))
        report = qualify(runs, budget, _digest(args.budget))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        print(json.dumps({"status": report["status"], "qualified": report["qualified"],
                          "output": str(args.output), "errors": report["errors"]}, sort_keys=True))
        return 0 if report["qualified"] else 2
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"qualification input error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
