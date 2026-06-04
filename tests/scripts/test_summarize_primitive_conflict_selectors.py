#!/usr/bin/env python3
"""Regression tests for primitive-conflict selector summaries."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_primitive_conflict_selectors.py"


FIELDS = [
    "draw_index",
    "semantic_status",
    "pixels",
    "color_changed_pixels",
    "max_color_delta",
    "max_abs_depth_delta",
    "max_uv0_delta",
    "max_projected_tex7_delta",
]


def write_input(path: Path, rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


class SummarizePrimitiveConflictSelectorsTests(unittest.TestCase):
    def test_color_only_separator_requires_final_color_oracle(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            input_csv = root / "primitive-conflict-summary.csv"
            output = root / "selector-summary.md"
            csv_output = root / "selector-summary.csv"
            write_input(input_csv, [
                {
                    "draw_index": 2,
                    "semantic_status": "pass",
                    "pixels": 3,
                    "color_changed_pixels": 0,
                    "max_color_delta": 0,
                    "max_abs_depth_delta": "0.001",
                    "max_uv0_delta": "0.018",
                    "max_projected_tex7_delta": "0.005",
                },
                {
                    "draw_index": 7,
                    "semantic_status": "pass",
                    "pixels": 72,
                    "color_changed_pixels": 0,
                    "max_color_delta": 0,
                    "max_abs_depth_delta": "0.008",
                    "max_uv0_delta": "0.052",
                    "max_projected_tex7_delta": "0.018",
                },
                {
                    "draw_index": 4,
                    "semantic_status": "fail",
                    "pixels": 15,
                    "color_changed_pixels": 3,
                    "max_color_delta": 84,
                    "max_abs_depth_delta": "0.006",
                    "max_uv0_delta": "0.022",
                    "max_projected_tex7_delta": "0.014",
                },
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--input",
                    str(input_csv),
                    "--output",
                    str(output),
                    "--csv-output",
                    str(csv_output),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = output.read_text(encoding="utf-8")
            self.assertIn("Decision: `final-color-oracle-required`", text)
            self.assertIn("only final-color metrics separate", text)
            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = {row["metric"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["max_color_delta"]["verdict"], "exact-fail-only-positive")
            self.assertEqual(rows["color_changed_pixels"]["verdict"], "exact-fail-only-positive")
            self.assertEqual(rows["max_abs_depth_delta"]["verdict"], "overlap")
            self.assertEqual(rows["owner_changed_pixels"]["verdict"], "overlap")

    def test_non_color_separator_is_reported_as_candidate_runtime_selector(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            input_csv = root / "primitive-conflict-summary.csv"
            output = root / "selector-summary.md"
            write_input(input_csv, [
                {
                    "draw_index": 2,
                    "semantic_status": "pass",
                    "pixels": 3,
                    "color_changed_pixels": 0,
                    "max_color_delta": 0,
                    "max_abs_depth_delta": "0.001",
                    "max_uv0_delta": "0.052",
                    "max_projected_tex7_delta": "0.018",
                },
                {
                    "draw_index": 7,
                    "semantic_status": "pass",
                    "pixels": 20,
                    "color_changed_pixels": 0,
                    "max_color_delta": 0,
                    "max_abs_depth_delta": "0.002",
                    "max_uv0_delta": "0.010",
                    "max_projected_tex7_delta": "0.006",
                },
                {
                    "draw_index": 4,
                    "semantic_status": "fail",
                    "pixels": 15,
                    "color_changed_pixels": 3,
                    "max_color_delta": 84,
                    "max_abs_depth_delta": "0.006",
                    "max_uv0_delta": "0.022",
                    "max_projected_tex7_delta": "0.014",
                },
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--input",
                    str(input_csv),
                    "--output",
                    str(output),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = output.read_text(encoding="utf-8")
            self.assertIn("Decision: `candidate-runtime-selector-found`", text)
            self.assertIn("non-color separating metrics: max_abs_depth_delta", text)


if __name__ == "__main__":
    unittest.main()
