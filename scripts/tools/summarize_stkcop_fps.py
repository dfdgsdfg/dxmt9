#!/usr/bin/env python3
"""Summarize benchmark-owned STALKER .result FPS without runtime instrumentation.

Time is reconstructed as sum(1 / per-frame FPS), relative to benchmark start.
Windows count frame completions in (start, end]; they are not wall-clock/Present
identities. Long-frame counts and percentiles belong to the completion window.
"""
import argparse
import configparser
import json
import math
from pathlib import Path


def summarize(path: Path, window_seconds: float = 10.0) -> dict:
    if not math.isfinite(window_seconds) or window_seconds <= 0:
        raise ValueError("window seconds must be finite and positive")
    config = configparser.ConfigParser()
    config.read_string(path.read_text(encoding="utf-8-sig"))
    rows = sorted((int(key), float(value))
                  for key, value in config["per_frame_stats"].items())
    if not rows or [key for key, _ in rows] != list(range(len(rows))):
        raise ValueError("frame IDs must be nonempty, unique and contiguous from zero")
    if any(not math.isfinite(fps) or fps <= 0 for _, fps in rows):
        raise ValueError("per-frame FPS must be finite and positive")
    durations = [1.0 / fps for _, fps in rows]
    total = math.fsum(durations)
    reported = float(config["general"]["average"])
    derived = len(rows) / total
    if not math.isfinite(reported) or not math.isclose(reported, derived, rel_tol=1e-4):
        raise ValueError("general.average disagrees with frame-count / sum(1/FPS)")
    # Bound memory/output on malformed or accidentally microscopic window sizes.
    count = math.ceil(total / window_seconds)
    if count > 10000:
        raise ValueError("too many windows (maximum 10000)")
    buckets: list[list[float]] = [[] for _ in range(count)]
    elapsed = 0.0
    for duration in durations:
        elapsed += duration
        index = min(count - 1, max(0, math.ceil(elapsed / window_seconds) - 1))
        buckets[index].append(duration * 1000.0)
    windows = []
    for index, values in enumerate(buckets):
        start = index * window_seconds
        end = min(total, start + window_seconds)
        ordered = sorted(values)
        windows.append({
            "start_s": start, "end_s": end, "frames": len(values),
            "fps": len(values) / (end - start),
            "p95_frame_ms": ordered[math.ceil(len(values) * 0.95) - 1] if values else None,
            "max_frame_ms": max(values) if values else None,
            "frames_over_50ms": sum(ms > 50 for ms in values),
            "frames_over_100ms": sum(ms > 100 for ms in values),
            "frames_over_250ms": sum(ms > 250 for ms in values),
        })
    return {"source": str(path), "time_basis": "reconstructed benchmark-relative frame completion",
            "reported_average_fps": reported, "derived_average_fps": derived,
            "duration_s": total, "frames": len(rows), "windows": windows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, nargs="+")
    parser.add_argument("--window-seconds", type=float, default=10.0)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        reports = [summarize(path, args.window_seconds) for path in args.results]
    except (OSError, ValueError, KeyError, configparser.Error) as error:
        parser.error(str(error))
    if args.json:
        print(json.dumps(reports, indent=2, allow_nan=False))
        return
    for report in reports:
        print(f"\n{report['source']}")
        print(f"{report['frames']} frames / {report['duration_s']:.3f}s"
              f" = {report['derived_average_fps']:.3f} FPS")
        print("seconds       FPS  p95 ms  max ms  >100ms >250ms")
        for row in report["windows"]:
            p95 = f"{row['p95_frame_ms']:.1f}" if row['p95_frame_ms'] is not None else "n/a"
            maximum = f"{row['max_frame_ms']:.1f}" if row['max_frame_ms'] is not None else "n/a"
            print(f"{row['start_s']:5.0f}-{row['end_s']:5.1f} "
                  f"{row['fps']:6.2f} {p95:>7} {maximum:>7} "
                  f"{row['frames_over_100ms']:7} {row['frames_over_250ms']:6}")


if __name__ == "__main__":
    main()
