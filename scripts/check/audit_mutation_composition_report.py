#!/usr/bin/env python3
"""Strict parser and conservation audit for mutation-composition windows.

This intentionally accepts the serialized observer line only when every input
used by the candidate predicate and economic gate is present.  A report with a
missing field is invalid rather than being silently interpreted as zero.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any


REPORT_PREFIX = "[dxmt9-mutation-composition]"
LINE_RE = re.compile(r"^\[dxmt9-mutation-composition\]\s+\w+:\s+(.*)$")
TOKEN_RE = re.compile(r"^([a-z][a-z0-9_]*)=([^\s]+)$")

BARRIER_FIELDS = (
    "barrier_draw",
    "barrier_process_vertices",
    "barrier_read_lock",
    "barrier_query_readback",
    "barrier_update_copy",
    "barrier_cross_thread",
    "barrier_destroy_reset",
    "barrier_capture_lease",
    "barrier_failure",
    "barrier_unknown",
)
REJECTION_FIELDS = (
    "different_resource",
    "different_generation",
    "render_tape_identity",
    "disposition",
    "range_overlap",
    "barrier",
    "failure",
    "source_order",
    "capacity",
    "invalid",
    "completion",
)
NUMERIC_FIELDS = (
    "window_presents",
    "mutations",
    "mutation_bytes",
    "mergeable_range_pairs",
    "candidate_calls",
    "candidate_bytes",
    "candidate_cpu_time_saved_ns",
    "candidate_cpu_ms_per_present",
    "wow64_writeback_ns",
    "queue_lock_ns",
    "backing_rotation_ns",
    "arena_update_ns",
    "shadow_copy_ns",
    "live_contents_copy_ns",
    "mergeable_union_bytes",
    "mergeable_overlap_bytes",
    "zero_use_generations",
    "discard_discard_dead",
    "barriers",
    *BARRIER_FIELDS,
    "pending",
    "completed",
    "failed",
    "discarded",
    "overflow",
    "provisional_rejections",
    "final_rejections",
    "provisional_completion_rejections",
    "final_completion_rejections",
    *(f"provisional_rejection_{name}" for name in REJECTION_FIELDS),
    *(f"final_rejection_{name}" for name in REJECTION_FIELDS),
    "first_use_gpu",
    "first_use_cpu",
    "first_use_distance_total",
    "first_use_distance_max",
    "invalid_or_dropped",
)
STRING_FIELDS = ("gate", "composition", "reason")
REQUIRED_FIELDS = frozenset((*NUMERIC_FIELDS, *STRING_FIELDS))


class ReportError(ValueError):
    """A report is malformed or fails a conservation/predicate invariant."""


def _number(name: str, value: str) -> int | float:
    if name == "candidate_cpu_ms_per_present":
        try:
            parsed = float(value)
        except ValueError as exc:
            raise ReportError(f"{name} is not numeric") from exc
        if not math.isfinite(parsed) or parsed < 0:
            raise ReportError(f"{name} is not a finite nonnegative number")
        return parsed
    try:
        parsed = int(value, 10)
    except ValueError as exc:
        raise ReportError(f"{name} is not an unsigned integer") from exc
    if parsed < 0:
        raise ReportError(f"{name} is negative")
    return parsed


def parse_line(line: str) -> dict[str, Any] | None:
    """Parse and validate one report line, returning its typed fields."""
    match = LINE_RE.match(line.strip())
    if not match:
        return None
    fields: dict[str, Any] = {}
    for token in match.group(1).split():
        token_match = TOKEN_RE.match(token)
        if not token_match:
            raise ReportError(f"malformed report token: {token!r}")
        name, value = token_match.groups()
        if name in fields:
            raise ReportError(f"duplicate report field: {name}")
        if name not in REQUIRED_FIELDS:
            raise ReportError(f"unknown report field: {name}")
        fields[name] = (
            _number(name, value) if name in NUMERIC_FIELDS else value
        )
    missing = REQUIRED_FIELDS - fields.keys()
    if missing:
        raise ReportError("missing report fields: " + ", ".join(sorted(missing)))
    _validate_window(fields)
    return fields


def _validate_window(report: dict[str, Any]) -> None:
    if report["window_presents"] <= 0:
        raise ReportError("window_presents must be positive")
    if report["mutations"] != (
        report["pending"]
        + report["completed"]
        + report["failed"]
        + report["discarded"]
    ):
        raise ReportError("mutation completion conservation failed")
    if report["pending"] != 0:
        raise ReportError("finalized report still has pending mutations")
    if report["mergeable_range_pairs"] != report["candidate_calls"]:
        raise ReportError("candidate_calls disagrees with mergeable_range_pairs")
    if report["candidate_calls"] > report["mutations"]:
        raise ReportError("candidate_calls exceeds mutations")
    if report["candidate_bytes"] > report["mutation_bytes"]:
        raise ReportError("candidate_bytes exceeds mutation_bytes")
    if report["first_use_gpu"] + report["first_use_cpu"] > report["mutations"]:
        raise ReportError("first-use count exceeds mutations")
    if report["overflow"] != 0:
        raise ReportError("bounded observer overflow makes the window invalid")
    if report["invalid_or_dropped"] != 0:
        raise ReportError("invalid or dropped observations make the window invalid")
    if report["first_use_distance_max"] < 0:
        raise ReportError("first-use distance max is negative")
    if report["barriers"] != sum(report[name] for name in BARRIER_FIELDS):
        raise ReportError("barrier total disagrees with typed barrier counts")
    provisional = sum(
        report[f"provisional_rejection_{name}"] for name in REJECTION_FIELDS
    )
    final = sum(report[f"final_rejection_{name}"] for name in REJECTION_FIELDS)
    if report["provisional_rejections"] != provisional:
        raise ReportError("provisional rejection total disagrees with reasons")
    if report["final_rejections"] != final:
        raise ReportError("final rejection total disagrees with reasons")
    if report["provisional_completion_rejections"] != report[
        "provisional_rejection_completion"
    ]:
        raise ReportError("provisional completion rejection total disagrees")
    if report["final_completion_rejections"] != report["final_rejection_completion"]:
        raise ReportError("final completion rejection total disagrees")
    if report["composition"] != "forbidden" or report["reason"] != "semantic-proof-required":
        raise ReportError("composition policy is not the required forbidden gate")
    expected_gate = (
        "open"
        if report["candidate_cpu_time_saved_ns"] / report["window_presents"] / 1e6 >= 0.5
        else "closed"
        if report["candidate_cpu_time_saved_ns"] / report["window_presents"] / 1e6 < 0.2
        else "inconclusive"
    )
    if report["gate"] != expected_gate:
        raise ReportError(
            f"gate {report['gate']!r} disagrees with candidate CPU time "
            f"({expected_gate!r})"
        )
    expected_ms = (
        report["candidate_cpu_time_saved_ns"] / report["window_presents"] / 1e6
    )
    if abs(report["candidate_cpu_ms_per_present"] - expected_ms) > 1e-5:
        raise ReportError("candidate CPU normalized value disagrees with raw time")


def parse_reports(path: str | Path) -> list[dict[str, Any]]:
    """Parse every observer report in a log, rejecting malformed report lines."""
    report_path = Path(path)
    if report_path.is_dir():
        candidates = sorted(report_path.glob("*dxmt9.log"))
        report_candidates = [
            candidate
            for candidate in candidates
            if REPORT_PREFIX in candidate.read_text(
                encoding="utf-8", errors="replace"
            )
        ]
        if len(report_candidates) == 1:
            report_path = report_candidates[0]
        elif report_candidates and all(
            [
                [
                    line
                    for line in candidate.read_text(
                        encoding="utf-8", errors="replace"
                    ).splitlines()
                    if REPORT_PREFIX in line
                ]
                == [
                    line
                    for line in report_candidates[0].read_text(
                        encoding="utf-8", errors="replace"
                    ).splitlines()
                    if REPORT_PREFIX in line
                ]
                for candidate in report_candidates[1:]
            ]
        ):
            # Some experiment artifacts retain both the app log and the
            # launcher log.  They are acceptable only when byte-identical;
            # otherwise the audit must not pick one silently.
            report_path = report_candidates[0]
        else:
            raise ReportError(
                f"expected one consistent report log in {report_path}, "
                f"found {len(report_candidates)}"
            )
    if not report_path.is_file():
        raise ReportError(f"report log does not exist: {report_path}")
    reports: list[dict[str, Any]] = []
    for line_number, line in enumerate(
        report_path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
    ):
        if REPORT_PREFIX not in line:
            continue
        try:
            report = parse_line(line)
        except ReportError as exc:
            raise ReportError(f"{report_path}:{line_number}: {exc}") from exc
        if report is not None:
            reports.append(report)
    if not reports:
        raise ReportError(f"no {REPORT_PREFIX} reports found in {report_path}")
    return reports


def audit_reports(reports: list[dict[str, Any]]) -> dict[str, Any]:
    """Return a weighted summary; each window contributes its Presents."""
    if not reports:
        raise ReportError("no reports to audit")
    presents = sum(report["window_presents"] for report in reports)
    raw_ns = sum(report["candidate_cpu_time_saved_ns"] for report in reports)
    weighted_ms = raw_ns / presents / 1e6
    gate = (
        "open"
        if weighted_ms >= 0.5
        else "closed"
        if weighted_ms < 0.2
        else "inconclusive"
    )
    summary = {
        "windows": len(reports),
        "window_presents": presents,
        "mutations": sum(report["mutations"] for report in reports),
        "candidate_calls": sum(report["candidate_calls"] for report in reports),
        "candidate_bytes": sum(report["candidate_bytes"] for report in reports),
        "candidate_cpu_time_saved_ns": raw_ns,
        "candidate_cpu_ms_per_present": weighted_ms,
        "gates": sorted({report["gate"] for report in reports}),
        "gate": gate,
        "composition": "forbidden",
    }
    return summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_or_log", type=Path)
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)
    try:
        summary = audit_reports(parse_reports(args.run_or_log))
    except ReportError as exc:
        print(f"mutation-composition report audit: FAIL: {exc}", file=sys.stderr)
        return 1
    if args.as_json:
        print(json.dumps(summary, sort_keys=True))
    else:
        print(
            "mutation-composition report audit: PASS: "
            f"{summary['windows']} windows, {summary['window_presents']} Presents, "
            f"weighted candidate CPU {summary['candidate_cpu_ms_per_present']:.6f} ms/Present"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
