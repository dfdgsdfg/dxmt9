#!/usr/bin/env python3
"""Focused tests for the strict mutation-composition report audit."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "audit_mutation_composition_report",
    ROOT / "scripts/check/audit_mutation_composition_report.py",
)
assert SPEC and SPEC.loader
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


def report_line(**overrides: object) -> str:
    fields: dict[str, object] = {
        name: (0.0 if name == "candidate_cpu_ms_per_present" else 0)
        for name in AUDIT.NUMERIC_FIELDS
    }
    fields.update({"window_presents": 60, "mutations": 2, "completed": 2})
    fields.update(
        {
            "mergeable_range_pairs": 1,
            "candidate_calls": 1,
            "candidate_bytes": 4,
            "mutation_bytes": 8,
            "candidate_cpu_time_saved_ns": 120_000_000,
            "candidate_cpu_ms_per_present": 2.0,
            "provisional_rejection_different_resource": 1,
            "provisional_rejections": 1,
            "final_rejection_different_resource": 1,
            "final_rejections": 1,
            "composition": "forbidden",
            "reason": "semantic-proof-required",
            "gate": "open",
        }
    )
    fields.update(overrides)
    return "[dxmt9-mutation-composition] info: " + " ".join(
        f"{name}={fields[name]}"
        for name in sorted(AUDIT.REQUIRED_FIELDS)
    )


class MutationCompositionReportAuditTests(unittest.TestCase):
    def test_weighted_windows_use_raw_cpu_time_and_present_count(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "run_dxmt9.log"
            path.write_text(
                report_line() + "\n"
                + report_line(
                    window_presents=120,
                    candidate_cpu_time_saved_ns=0,
                    candidate_cpu_ms_per_present=0.0,
                    mutations=1,
                    completed=1,
                    mergeable_range_pairs=0,
                    candidate_calls=0,
                    candidate_bytes=0,
                    mutation_bytes=4,
                    provisional_rejections=0,
                    final_rejections=0,
                    provisional_rejection_different_resource=0,
                    final_rejection_different_resource=0,
                    gate="closed",
                )
                + "\n",
                encoding="utf-8",
            )
            summary = AUDIT.audit_reports(AUDIT.parse_reports(path))
        self.assertEqual(summary["window_presents"], 180)
        self.assertEqual(summary["candidate_cpu_time_saved_ns"], 120_000_000)
        self.assertAlmostEqual(summary["candidate_cpu_ms_per_present"], 2.0 / 3.0)
        self.assertEqual(summary["gate"], "open")

    def test_missing_decision_input_is_rejected(self) -> None:
        for field in (
            "mutation_bytes",
            "first_use_distance_total",
            "barrier_draw",
            "provisional_rejection_barrier",
            "final_rejection_completion",
        ):
            with self.subTest(field=field):
                line = " ".join(
                    token
                    for token in report_line().split()
                    if not token.startswith(field + "=")
                )
                with self.assertRaises(AUDIT.ReportError):
                    AUDIT.parse_line(line)

    def test_conservation_and_gate_are_checked(self) -> None:
        with self.assertRaises(AUDIT.ReportError):
            AUDIT.parse_line(report_line(completed=1))
        with self.assertRaises(AUDIT.ReportError):
            AUDIT.parse_line(report_line(gate="closed"))


if __name__ == "__main__":
    unittest.main()
