#!/usr/bin/env python3
"""Partition CA present-request -> present-CB GPU-start latency.

The input is the set of XML tables exported from one phase-aligned Metal
System Trace.  The report distinguishes queued dxmt9 GPU work from external
GPU activity, true global GPU idle, and the small Metal driver CPU interval.
It is intentionally based on interval unions: Vertex and Fragment channels
often overlap and must not be added as independent durations.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
from dataclasses import dataclass
from pathlib import Path

from summarize_xctrace_metal_intervals import parse_duration_ms, parse_xctrace_rows


SEQ_RE = re.compile(r"(?:RenderPass|Present)\[seq=(\d+)")
CB_LABEL_RE = re.compile(r"\b(cb_seq_\d+)\b")


@dataclass(frozen=True)
class Interval:
    start: float
    end: float
    command_buffer: str
    process: str
    label: str
    seq: int | None


def parse_time_ms(value: str) -> float:
    """Parse xctrace's MM:SS.mmm.uuu formatted timestamp."""
    fields = (value or "").strip().replace(":", ".").split(".")
    if len(fields) != 4:
        return 0.0
    try:
        minute, second, millis, micros = (int(field) for field in fields)
    except ValueError:
        return 0.0
    return (minute * 60 + second) * 1000.0 + millis + micros / 1000.0


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    return ordered[low] * (high - position) + ordered[high] * (position - low)


def distribution(values: list[float]) -> tuple[float, float, float]:
    if not values:
        return (0.0, 0.0, 0.0)
    return (sum(values) / len(values), percentile(values, 0.50),
            percentile(values, 0.95))


def clipped(intervals: list[Interval], start: float, end: float) -> list[tuple[float, float]]:
    return [(max(start, interval.start), min(end, interval.end))
            for interval in intervals
            if interval.end > start and interval.start < end]


def merged(intervals: list[tuple[float, float]]) -> list[tuple[float, float]]:
    result: list[tuple[float, float]] = []
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if result and start <= result[-1][1]:
            result[-1] = (result[-1][0], max(result[-1][1], end))
        else:
            result.append((start, end))
    return result


def union_ms(intervals: list[tuple[float, float]]) -> float:
    return sum(end - start for start, end in merged(intervals))


def load_gpu_intervals(path: Path) -> list[Interval]:
    result: list[Interval] = []
    for row in parse_xctrace_rows(path):
        start = parse_time_ms(row.get("start-time", ""))
        duration = parse_duration_ms(row.get("duration", ""))
        label = row.get("formatted-label", "")
        match = SEQ_RE.search(label)
        result.append(Interval(
            start=start,
            end=start + duration,
            command_buffer=row.get("metal-command-buffer-id", ""),
            process=row.get("sentinel_2", ""),
            label=label,
            seq=int(match.group(1)) if match else None,
        ))
    return result


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--analysis-dir", type=Path, required=True)
    parser.add_argument("--process", default="3DMark05.exe")
    parser.add_argument("--output-csv", type=Path)
    parser.add_argument("--output-md", type=Path)
    return parser


def main() -> int:
    args = make_parser().parse_args()
    base = args.analysis_dir
    required = {
        "requests": base / "ca-client-present-request.xml",
        "gpu": base / "metal-gpu-intervals.xml",
        "submissions": base / "metal-application-command-buffer-submissions.xml",
        "mapping": base / "metal-gpu-submission-to-command-buffer-id.xml",
        "driver": base / "metal-driver-intervals.xml",
    }
    missing = [str(path) for path in required.values() if not path.is_file()]
    if missing:
        raise SystemExit("missing required xctrace export(s): " + ", ".join(missing))

    gpu = load_gpu_intervals(required["gpu"])
    app_gpu = [interval for interval in gpu if args.process in interval.process]

    submissions = parse_xctrace_rows(required["submissions"])
    submission_by_cb = {
        row.get("metal-command-buffer-id", ""): row
        for row in submissions
        if args.process in row.get("process", "")
    }
    mapping_rows = parse_xctrace_rows(required["mapping"])
    mapping_start_by_cb: dict[str, float] = {}
    for row in mapping_rows:
        command_buffer = row.get("metal-command-buffer-id", "")
        if command_buffer not in submission_by_cb:
            continue
        timestamp = parse_time_ms(row.get("start-time", ""))
        previous = mapping_start_by_cb.get(command_buffer)
        if previous is None or timestamp < previous:
            mapping_start_by_cb[command_buffer] = timestamp

    driver_by_label: dict[str, list[float]] = {}
    for row in parse_xctrace_rows(required["driver"]):
        if args.process not in row.get("process", ""):
            continue
        if row.get("metal-object-label") != "Command Buffer":
            continue
        match = CB_LABEL_RE.search(row.get("formatted-label", ""))
        if match:
            driver_by_label.setdefault(match.group(1), []).append(
                parse_duration_ms(row.get("duration", "")))

    requests = [
        row for row in parse_xctrace_rows(required["requests"])
        if args.process in row.get("process", "")
    ]
    requests.sort(key=lambda row: parse_time_ms(row.get("start-time", "")))
    rows: list[dict[str, float | int | str]] = []
    prior_target_cb = ""
    for request_index, request in enumerate(requests):
        request_ms = parse_time_ms(request.get("start-time", ""))
        target_cb = request.get("metal-command-buffer-id", "")
        # The first request has no preceding present target inside the trace,
        # so its prior-frame tail cannot be classified phase-aligned.
        if not prior_target_cb:
            prior_target_cb = target_cb
            continue
        target_intervals = [interval for interval in app_gpu
                            if interval.command_buffer == target_cb]
        if not target_intervals:
            prior_target_cb = target_cb
            continue
        target_gpu_start = min(interval.start for interval in target_intervals)
        if target_gpu_start < request_ms:
            prior_target_cb = target_cb
            continue
        present_intervals = [interval for interval in target_intervals
                             if "Present[seq=" in interval.label]
        seq = present_intervals[0].seq if present_intervals else target_intervals[0].seq
        if seq is None:
            prior_target_cb = target_cb
            continue

        predecessor = [interval for interval in app_gpu
                       if interval.seq == seq and interval.command_buffer != target_cb]
        prior_present = [interval for interval in app_gpu
                         if interval.command_buffer == prior_target_cb]
        older_app = [interval for interval in app_gpu
                     if interval.seq != seq and interval.command_buffer != target_cb]
        app_window = clipped(app_gpu, request_ms, target_gpu_start)
        predecessor_window = clipped(predecessor, request_ms, target_gpu_start)
        prior_present_window = clipped(prior_present, request_ms, target_gpu_start)
        all_gpu_window = clipped(gpu, request_ms, target_gpu_start)
        classified_app_window = predecessor_window + clipped(
            older_app, request_ms, target_gpu_start)
        app_busy = union_ms(app_window)
        classified_app_busy = union_ms(classified_app_window)
        all_gpu_busy = union_ms(all_gpu_window)
        latency = target_gpu_start - request_ms

        submission = submission_by_cb.get(target_cb, {})
        submission_start = parse_time_ms(submission.get("start-time", ""))
        submission_end = submission_start + parse_duration_ms(
            submission.get("duration", ""))
        mapping_start = mapping_start_by_cb.get(target_cb, 0.0)
        label_match = CB_LABEL_RE.search(submission.get("narrative", ""))
        driver_values = driver_by_label.get(label_match.group(1), []) if label_match else []

        preceding_app_ends = [
            interval.end for interval in app_gpu
            if interval.command_buffer != target_cb and interval.end <= target_gpu_start
        ]
        predecessor_cbs = {interval.command_buffer for interval in predecessor}
        rows.append({
            "request_index": request_index,
            "seq": seq,
            "target_cb": target_cb,
            "request_ms": request_ms,
            "target_gpu_start_ms": target_gpu_start,
            "request_to_gpu_start_ms": latency,
            "predecessor_cb_count": len(predecessor_cbs),
            "current_frame_predecessor_busy_ms": union_ms(predecessor_window),
            "prior_present_tail_ms": union_ms(prior_present_window),
            "all_app_gpu_active_ms": app_busy,
            # Exclusive external activity makes app + external + idle an
            # additive partition even when app and compositor channels overlap.
            "external_gpu_active_ms": max(0.0, all_gpu_busy - app_busy),
            "global_gpu_idle_ms": max(0.0, latency - all_gpu_busy),
            "unclassified_app_gpu_ms": max(0.0, app_busy - classified_app_busy),
            "last_app_handoff_ms": (
                target_gpu_start - max(preceding_app_ends)
                if preceding_app_ends else latency
            ),
            "submission_end_to_request_ms": request_ms - submission_end,
            "mapping_to_gpu_start_ms": (
                target_gpu_start - mapping_start if mapping_start else 0.0
            ),
            "driver_command_buffer_cpu_ms": sum(driver_values),
        })
        prior_target_cb = target_cb

    if not rows:
        raise SystemExit("no present requests joined to target GPU intervals")

    fields = list(rows[0])
    if args.output_csv:
        args.output_csv.parent.mkdir(parents=True, exist_ok=True)
        with args.output_csv.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    metrics = [
        ("Request -> present-CB GPU start", "request_to_gpu_start_ms"),
        ("Current-frame predecessor sub-CB busy", "current_frame_predecessor_busy_ms"),
        ("Prior-present CB tail", "prior_present_tail_ms"),
        ("All application GPU active", "all_app_gpu_active_ms"),
        ("External GPU active", "external_gpu_active_ms"),
        ("Global GPU idle", "global_gpu_idle_ms"),
        ("Unclassified application GPU", "unclassified_app_gpu_ms"),
        ("Last app GPU end -> target start", "last_app_handoff_ms"),
        ("Submission end -> CA request", "submission_end_to_request_ms"),
        ("GPU submission mapping -> target start", "mapping_to_gpu_start_ms"),
        ("Driver command-buffer CPU interval", "driver_command_buffer_cpu_ms"),
    ]
    lines = [
        "# GT2 Present-to-GPU Latency Partition",
        "",
        f"- Analysis directory: `{base}`",
        f"- Present requests: `{len(requests)}`",
        f"- Joined request/target rows: `{len(rows)}`",
        f"- Process filter: `{args.process}`",
        "",
        "Durations are interval unions across overlapping GPU channels.",
        "External GPU active is external-only time after subtracting overlap",
        "with application GPU activity, so app + external + idle is additive.",
        "Prior-present tail and current-frame predecessor busy may overlap, so",
        "they are diagnostic views rather than additive partition buckets.",
        "",
        "| Metric | mean ms | p50 ms | p95 ms |",
        "|---|---:|---:|---:|",
    ]
    for label, key in metrics:
        mean, p50, p95 = distribution([float(row[key]) for row in rows])
        lines.append(f"| {label} | {mean:.3f} | {p50:.3f} | {p95:.3f} |")
    predecessor_counts = sorted({int(row["predecessor_cb_count"]) for row in rows})
    app_share = sum(float(row["all_app_gpu_active_ms"]) for row in rows) / max(
        1e-9, sum(float(row["request_to_gpu_start_ms"]) for row in rows)) * 100.0
    lines.extend([
        "",
        f"- Current-frame predecessor CB counts observed: `{predecessor_counts}`",
        f"- Application GPU-active share of request latency: `{app_share:.2f}%`",
        "",
    ])
    report = "\n".join(lines)
    if args.output_md:
        args.output_md.parent.mkdir(parents=True, exist_ok=True)
        args.output_md.write_text(report, encoding="utf-8")
    print(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
