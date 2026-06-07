#!/usr/bin/env python3
"""Regression tests for effect ROI force-white probe queue planning."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "plan_effect_roi_forcewhite_probes.py"


class PlanEffectRoiForcewhiteProbesTests(unittest.TestCase):
    def write_queue_source(self, path: Path, rows: list[dict[str, str]]) -> None:
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=[
                    "roi",
                    "texture0",
                    "texture0_width",
                    "texture0_height",
                    "texture0_format",
                    "seq",
                    "encoder",
                    "ordinal",
                    "command_index",
                    "command_draw_index",
                    "command_draw_count",
                    "encoder_draw_index",
                    "bbox_source",
                    "src_blend",
                    "dst_blend",
                    "complete",
                    "primitive_count",
                    "intersection_area",
                    "roi_coverage_pct",
                    "bbox_coverage_pct",
                ],
            )
            writer.writeheader()
            writer.writerows(rows)

    def test_cli_writes_probe_queue_with_command_draw_index(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "roi.csv"
            report = root / "queue.md"
            csv_output = root / "queue.csv"
            self.write_queue_source(
                source,
                [
                    {
                        "roi": "muzzle",
                        "texture0": "0x20000010000007f",
                        "texture0_width": "1024",
                        "texture0_height": "256",
                        "texture0_format": "31",
                        "seq": "1455",
                        "encoder": "11",
                        "ordinal": "314033",
                        "command_index": "7",
                        "command_draw_index": "2",
                        "command_draw_count": "5",
                        "encoder_draw_index": "4",
                        "bbox_source": "projected-screen",
                        "src_blend": "10",
                        "dst_blend": "2",
                        "complete": "1",
                        "primitive_count": "4",
                        "intersection_area": "1200",
                        "roi_coverage_pct": "12",
                        "bbox_coverage_pct": "3",
                    }
                ],
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(source),
                    "--roi",
                    "muzzle",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("--probe-force-texture-white-command-draw-index", text)
            self.assertIn("--probe-force-texture-white-command-index 7", text)
            self.assertIn("--probe-force-texture-white-command-draw-index 2", text)
            self.assertIn("--probe-force-texture-white-row 1455/11", text)
            self.assertIn("--encoder-breakdown-seq 1455", text)
            self.assertNotIn("--no-encoder-breakdown", text)
            self.assertNotIn("--probe-indexed-triangle-encoder-draw-min 3", text)
            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["ordinal"], "314033")
            self.assertEqual(rows[0]["command_index"], "7")
            self.assertEqual(rows[0]["command_draw_index"], "2")
            self.assertEqual(rows[0]["command_draw_count"], "5")
            self.assertEqual(rows[0]["encoder_draw_index"], "4")
            self.assertEqual(rows[0]["probe_draw_index"], "3")
            self.assertIn(
                "--probe-force-texture-white-texture0-width 1024",
                rows[0]["command"],
            )
            self.assertIn(
                "--probe-force-texture-white-command-draw-index 2",
                rows[0]["command"],
            )
            self.assertNotIn(
                "--probe-indexed-triangle-encoder-draw-min",
                rows[0]["command"],
            )

    def test_command_index_collapses_duplicate_ordinals(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "roi.csv"
            report = root / "queue.md"
            csv_output = root / "queue.csv"
            base = {
                "roi": "muzzle",
                "texture0": "0x200000100000080",
                "texture0_width": "128",
                "texture0_height": "128",
                "texture0_format": "2",
                "seq": "517",
                "encoder": "2",
                "command_index": "322",
                "command_draw_index": "4",
                "command_draw_count": "7",
                "encoder_draw_index": "1",
                "bbox_source": "screen-space-pos",
                "src_blend": "5",
                "dst_blend": "2",
                "complete": "1",
                "primitive_count": "2",
                "intersection_area": "10000",
                "roi_coverage_pct": "100",
                "bbox_coverage_pct": "1",
            }
            self.write_queue_source(
                source,
                [
                    {**base, "ordinal": "100"},
                    {**base, "ordinal": "101"},
                ],
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(source),
                    "--roi",
                    "muzzle",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["command_index"], "322")
            self.assertEqual(rows[0]["command_draw_index"], "4")
            self.assertEqual(rows[0]["command_draw_count"], "7")
            self.assertEqual(rows[0]["rows"], "2")
            self.assertEqual(rows[0]["primitive_count"], "4")
            self.assertIn("--probe-force-texture-white-command-index 322", rows[0]["command"])
            self.assertIn("--probe-force-texture-white-command-draw-index 4", rows[0]["command"])

    def test_min_bbox_coverage_filters_broad_geometry_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "roi.csv"
            csv_output = root / "queue.csv"
            base = {
                "roi": "component",
                "texture0_width": "1024",
                "texture0_height": "1024",
                "texture0_format": "31",
                "seq": "1092",
                "encoder": "4",
                "ordinal": "",
                "command_index": "202",
                "command_draw_index": "0",
                "command_draw_count": "2",
                "encoder_draw_index": "1",
                "bbox_source": "projected-screen",
                "src_blend": "10",
                "dst_blend": "2",
                "complete": "1",
                "primitive_count": "8",
                "intersection_area": "200",
                "roi_coverage_pct": "100",
            }
            self.write_queue_source(
                source,
                [
                    {
                        **base,
                        "texture0": "0x20000010000007f",
                        "bbox_coverage_pct": "0.001",
                    },
                    {
                        **base,
                        "texture0": "0x20000010000005a",
                        "command_index": "203",
                        "bbox_coverage_pct": "2.0",
                    },
                ],
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(source),
                    "--roi",
                    "component",
                    "--min-bbox-coverage-pct",
                    "1",
                    "--csv-output",
                    str(csv_output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["texture0"], "0x20000010000005a")
            self.assertEqual(rows[0]["max_bbox_coverage_pct"], "2.000")


if __name__ == "__main__":
    unittest.main()
