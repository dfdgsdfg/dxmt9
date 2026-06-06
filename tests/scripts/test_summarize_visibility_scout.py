#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_visibility_scout.py"


VIS_FIELDS = [
    "seq",
    "encoder",
    "command",
    "draw_ordinal",
    "result_index",
    "metal_draw_index",
    "primitive_type",
    "source_primitive_count",
    "submitted_primitive_count",
    "submitted_element_count",
    "indexed",
    "expanded_indexed",
    "split_chunk",
    "visible_samples",
    "rt0",
    "depth",
    "texture_mask",
    "color_write",
    "z_enable",
    "z_write",
    "z_func",
    "alpha_blend",
    "alpha_test",
    "scissor",
    "cull",
    "fill",
    "overflow",
]


class SummarizeVisibilityScoutTests(unittest.TestCase):
    def test_visibility_summary_groups_and_joins_probe_rows(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            visibility = root / "visibility.csv"
            probe = root / "probe.csv"
            output = root / "summary.md"
            csv_output = root / "summary.csv"

            with visibility.open("w", newline="", encoding="utf-8") as f:
                writer = csv.DictWriter(f, fieldnames=VIS_FIELDS)
                writer.writeheader()
                writer.writerow(
                    {
                        "seq": "60",
                        "encoder": "2",
                        "command": "70",
                        "draw_ordinal": "100",
                        "result_index": "0",
                        "metal_draw_index": "36",
                        "primitive_type": "3",
                        "source_primitive_count": "8186",
                        "submitted_primitive_count": "8186",
                        "submitted_element_count": "24558",
                        "indexed": "1",
                        "expanded_indexed": "0",
                        "split_chunk": "0",
                        "visible_samples": "0",
                        "rt0": "0x1",
                        "depth": "0x2",
                        "texture_mask": "0x3f",
                        "color_write": "15",
                        "z_enable": "1",
                        "z_write": "0",
                        "z_func": "4",
                        "alpha_blend": "0",
                        "alpha_test": "0",
                        "scissor": "0",
                        "cull": "2",
                        "fill": "0",
                        "overflow": "0",
                    }
                )
                writer.writerow(
                    {
                        "seq": "60",
                        "encoder": "2",
                        "command": "71",
                        "draw_ordinal": "101",
                        "result_index": "1",
                        "metal_draw_index": "37",
                        "primitive_type": "3",
                        "source_primitive_count": "22622",
                        "submitted_primitive_count": "22622",
                        "submitted_element_count": "67866",
                        "indexed": "1",
                        "expanded_indexed": "0",
                        "split_chunk": "0",
                        "visible_samples": "8884",
                        "rt0": "0x1",
                        "depth": "0x2",
                        "texture_mask": "0x3f",
                        "color_write": "15",
                        "z_enable": "1",
                        "z_write": "0",
                        "z_func": "4",
                        "alpha_blend": "0",
                        "alpha_test": "0",
                        "scissor": "0",
                        "cull": "2",
                        "fill": "0",
                        "overflow": "0",
                    }
                )

            with probe.open("w", newline="", encoding="utf-8") as f:
                writer = csv.DictWriter(
                    f,
                    fieldnames=[
                        "seq",
                        "encoder",
                        "encoder_draw_index",
                        "draw_ordinal",
                        "original_cache_miss32",
                        "candidate_cache_miss32",
                    ],
                )
                writer.writeheader()
                writer.writerow(
                    {
                        "seq": "60",
                        "encoder": "2",
                        "encoder_draw_index": "36",
                        "draw_ordinal": "100",
                        "original_cache_miss32": "1000",
                        "candidate_cache_miss32": "800",
                    }
                )
                writer.writerow(
                    {
                        "seq": "60",
                        "encoder": "2",
                        "encoder_draw_index": "37",
                        "draw_ordinal": "101",
                        "original_cache_miss32": "3000",
                        "candidate_cache_miss32": "2400",
                    }
                )

            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(visibility),
                    "--probe-draws",
                    str(probe),
                    "--row",
                    "60/2",
                    "--draw-indices",
                    "36..37",
                    "--output",
                    str(output),
                    "--csv-output",
                    str(csv_output),
                ],
                check=True,
                cwd=REPO_ROOT,
            )

            text = output.read_text(encoding="utf-8")
            self.assertIn("No-sample draws: `1`", text)
            self.assertIn("Requested Draw Window", text)
            self.assertIn("No-Sample Draws", text)
            self.assertIn("| 60/2 | 36 | 100 |", text)
            self.assertIn("60/2\\|depth=read\\|blend=off", text)
            self.assertIn("-800", text)

            with csv_output.open(newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["draws"], "2")
            self.assertEqual(rows[0]["zero_draws"], "1")
            self.assertEqual(rows[0]["positive_draws"], "1")
            self.assertEqual(rows[0]["miss32_delta"], "-800")
            self.assertEqual(rows[0]["zero_source_primitives"], "8186")
            self.assertEqual(rows[0]["positive_source_primitives"], "22622")
            self.assertEqual(rows[0]["zero_miss32_delta"], "-200")
            self.assertEqual(rows[0]["positive_miss32_delta"], "-600")


if __name__ == "__main__":
    unittest.main()
