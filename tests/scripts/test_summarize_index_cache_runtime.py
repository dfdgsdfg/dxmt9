#!/usr/bin/env python3
"""Regression tests for reordered-index-cache runtime summaries."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_index_cache_runtime.py"


ENCODER_FIELDS = [
    "seq",
    "encoder",
    "draw_calls",
    "indexed_order_probe_draws",
    "indexed_order_probe_skipped",
    "indexed_order_probe_bytes",
    "indexed_cache_opt_candidate_draws",
    "indexed_cache_opt_candidate_skipped",
    "indexed_cache_opt_candidate_bytes",
    "indexed_cache_opt_candidate_original_miss32",
    "indexed_cache_opt_candidate_miss32",
    "reordered_index_cache_lookups",
    "reordered_index_cache_hits",
    "reordered_index_cache_rejected_hits",
    "reordered_index_cache_misses",
    "reordered_index_cache_created",
    "reordered_index_cache_created_bytes",
]

PROBE_FIELDS = [
    "seq",
    "encoder",
    "encoder_draw_index",
    "eligible",
    "applied",
    "original_cache_miss32",
    "effective_cache_miss32",
]


class SummarizeIndexCacheRuntimeTests(unittest.TestCase):
    def test_runtime_accounting_with_probe_draw_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            encoders = root / "encoders.csv"
            probes = root / "probe-draws.csv"
            report = root / "report.md"
            summary = root / "summary.csv"

            with encoders.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=ENCODER_FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": 50,
                    "encoder": 0,
                    "draw_calls": 42,
                    "indexed_order_probe_draws": 33,
                    "indexed_order_probe_skipped": 9,
                    "indexed_order_probe_bytes": 1000,
                    "reordered_index_cache_lookups": 42,
                    "reordered_index_cache_hits": 33,
                    "reordered_index_cache_rejected_hits": 9,
                    "reordered_index_cache_misses": 0,
                    "reordered_index_cache_created": 0,
                    "reordered_index_cache_created_bytes": 0,
                })
                writer.writerow({
                    "seq": 50,
                    "encoder": 1,
                    "draw_calls": 156,
                    "indexed_order_probe_draws": 69,
                    "indexed_order_probe_skipped": 87,
                    "indexed_order_probe_bytes": 2000,
                    "reordered_index_cache_lookups": 156,
                    "reordered_index_cache_hits": 69,
                    "reordered_index_cache_rejected_hits": 87,
                    "reordered_index_cache_misses": 0,
                    "reordered_index_cache_created": 0,
                    "reordered_index_cache_created_bytes": 0,
                })

            with probes.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=PROBE_FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": 50,
                    "encoder": 0,
                    "encoder_draw_index": 0,
                    "eligible": 1,
                    "applied": 1,
                    "original_cache_miss32": 100,
                    "effective_cache_miss32": 70,
                })
                writer.writerow({
                    "seq": 50,
                    "encoder": 1,
                    "encoder_draw_index": 0,
                    "eligible": 1,
                    "applied": 0,
                    "original_cache_miss32": 80,
                    "effective_cache_miss32": 80,
                })

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--run",
                    f"proof={encoders},{probes}",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            row = rows[0]
            self.assertEqual(row["cache_lookups"], "198")
            self.assertEqual(row["runtime_applied_draws"], "102")
            self.assertEqual(row["runtime_skipped_draws"], "96")
            self.assertEqual(row["probe_draw_logging"], "per-draw-present")
            self.assertEqual(row["probe_applied_delta32"], "-30")
            self.assertEqual(row["verdict"], "active-with-rejections")
            self.assertIn("`proof`", report.read_text(encoding="utf-8"))

    def test_created_cache_entries_count_as_runtime_applied_draws(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            encoders = root / "encoders.csv"
            report = root / "report.md"
            summary = root / "summary.csv"

            with encoders.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=ENCODER_FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": 2,
                    "encoder": 0,
                    "draw_calls": 43,
                    "indexed_order_probe_draws": 34,
                    "indexed_order_probe_skipped": 9,
                    "indexed_order_probe_bytes": 566814,
                    "indexed_cache_opt_candidate_draws": 22,
                    "indexed_cache_opt_candidate_bytes": 443568,
                    "indexed_cache_opt_candidate_original_miss32": 133174,
                    "indexed_cache_opt_candidate_miss32": 99469,
                    "reordered_index_cache_lookups": 43,
                    "reordered_index_cache_hits": 17,
                    "reordered_index_cache_rejected_hits": 0,
                    "reordered_index_cache_misses": 26,
                    "reordered_index_cache_created": 17,
                    "reordered_index_cache_created_bytes": 1234,
                })

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--run",
                    f"smoke={encoders}",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with summary.open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["runtime_applied_draws"], "34")
            self.assertEqual(row["runtime_skipped_draws"], "9")
            self.assertEqual(row["cache_miss_not_created"], "9")
            self.assertEqual(row["indexed_order_accounting_delta"], "0")
            self.assertEqual(row["probe_draw_logging"], "not-provided")
            self.assertEqual(row["candidate_miss_delta32"], "-33705")


if __name__ == "__main__":
    unittest.main()
