"""Contract tests for the repeated trace observer control gate."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
SPEC = importlib.util.spec_from_file_location(
    "qualify_trace_observer", ROOT / "scripts/tools/qualify_trace_observer.py"
)
assert SPEC and SPEC.loader
qualifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qualifier)


def budget():
    return {
        "schema": qualifier.SCHEMA_BUDGET,
        "frozen_at": "2026-01-01T00:00:00Z",
        "thresholds": {name: 1_000_000 for name in qualifier.BUDGET_FIELDS},
    }


def run(mode, repetition):
    measured = repetition + (0 if mode == "observer-off" else 1)
    return {
        "schema": qualifier.SCHEMA_RUN,
        "mode": mode,
        "run_id": f"{mode}-{repetition}",
        "repetition_id": f"rep-{repetition}",
        "started_at": f"2026-01-02T00:00:0{repetition}Z",
        "workload_status": "pass",
        "correctness": True,
        "capture_status": "disabled" if mode == "observer-off" else "complete",
        "trace_receipt": None if mode == "observer-off" else {
            "qualified": True, "sha256": "a" * 64,
        },
        "provenance": {
            "workload": "wine-d3d9-fixture",
            "build": {"revision": "deadbeef"},
            "runtime": {"wine_id": "sikarugir"},
            "renderer": {"mode": "framegraph"},
            "profile": "perf",
            "selection": {"categories": ["cpu", "wait"], "duration_ms": 1000},
        },
        "metrics": {
            "wall_elapsed_ms": 1000 + measured,
            "process_cpu_ms": 500 + measured,
            "producer_cpu_ms": 0 if mode == "observer-off" else 2,
            "worker_cpu_ms": 0 if mode != "async" else 3,
            "peak_memory_bytes": 10000 + measured,
            "buffer_occupancy_peak": 0 if mode != "async" else 32,
            "producer_notifications": 0 if mode != "async" else 2,
            "bridge_calls": 500,
            "source_loss": 0,
            "sdk_loss": 0,
            "output_loss": 0,
            "frames": 3,
            "frame_time_ms": [10 + measured, 11 + measured, 12 + measured],
        },
        "metric_sources": {
            field: "workload-result.json#metrics" for field in qualifier.METRIC_FIELDS
        } | {"frame_time_ms": "workload-frame-samples.json#frames"},
    }


class QualifyTraceObserverTests(unittest.TestCase):
    def records(self):
        return [(f"{mode}-{n}.json", run(mode, n), f"{n:064x}")
                for mode in qualifier.MODES for n in range(1, 4)]

    def test_matched_complete_runs_qualify_against_frozen_budgets(self):
        report = qualifier.qualify(self.records(), budget(), "b" * 64)
        self.assertTrue(report["qualified"], report["errors"])
        self.assertEqual(report["metrics"]["frame_time_percentiles_ms"]["observer-off"]["p99"], 15)

    def test_unavailable_measurement_fails_closed_without_zero_filling(self):
        records = self.records()
        records[0][1]["metrics"].pop("worker_cpu_ms")
        report = qualifier.qualify(records, budget(), "b" * 64)
        self.assertFalse(report["qualified"])
        self.assertTrue(any("metric_unavailable_or_invalid:worker_cpu_ms" in error
                            for error in report["errors"]))

    def test_budget_must_precede_runs_and_provenance_must_match(self):
        records = self.records()
        records[-1][1]["provenance"]["renderer"]["mode"] = "traditional"
        frozen_late = budget()
        frozen_late["frozen_at"] = "2026-01-03T00:00:00Z"
        report = qualifier.qualify(records, frozen_late, "b" * 64)
        self.assertFalse(report["qualified"])
        self.assertIn("matched_provenance_mismatch:async-3.json", report["errors"])
        self.assertTrue(any(error.startswith("budget_not_frozen_before_run:")
                            for error in report["errors"]))


if __name__ == "__main__":
    unittest.main()
