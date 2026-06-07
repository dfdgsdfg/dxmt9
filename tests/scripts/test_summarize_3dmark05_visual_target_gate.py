#!/usr/bin/env python3
"""Regression tests for 3DMark05 visual target gate summaries."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_3dmark05_visual_target_gate.py"


class Summarize3DMark05VisualTargetGateTests(unittest.TestCase):
    def write_csv(self, path: Path, fieldnames: list[str], rows: list[dict[str, str]]) -> None:
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rows)

    def test_promotes_when_source_queue_survives(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            component = root / "components.csv"
            overlap = root / "local.csv"
            queue = root / "queue.csv"
            report = root / "gate.md"
            csv_output = root / "gate.csv"
            self.write_csv(component, ["frame", "component_id", "bbox"], [
                {"frame": "1092", "component_id": "1", "bbox": "10,20,30,40"},
            ])
            self.write_csv(overlap, ["roi", "seq", "texture0", "command_index", "roi_coverage_pct", "bbox_coverage_pct"], [
                {
                    "roi": "frame1092-component1",
                    "seq": "1092",
                    "texture0": "0x20000010000007f",
                    "command_index": "202",
                    "roi_coverage_pct": "100",
                    "bbox_coverage_pct": "3",
                },
            ])
            self.write_csv(queue, ["roi", "seq", "texture0", "command_index", "max_roi_coverage_pct", "max_bbox_coverage_pct"], [
                {
                    "roi": "frame1092-component1",
                    "seq": "1092",
                    "texture0": "0x20000010000007f",
                    "command_index": "202",
                    "max_roi_coverage_pct": "100",
                    "max_bbox_coverage_pct": "3",
                },
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--component-csv",
                    str(component),
                    "--local-overlap-csv",
                    str(overlap),
                    "--source-queue-csv",
                    str(queue),
                    "--source-texture",
                    "0x20000010000007f",
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
            self.assertEqual(rows[0]["verdict"], "promote-candidate")
            self.assertIn("Source Queue Candidates", report.read_text(encoding="utf-8"))

    def test_blocks_when_only_non_source_local_rows_survive(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            component = root / "components.csv"
            overlap = root / "local.csv"
            queue = root / "queue.csv"
            report = root / "gate.md"
            csv_output = root / "gate.csv"
            self.write_csv(component, ["frame", "component_id", "bbox"], [
                {"frame": "1091", "component_id": "7", "bbox": "165,394,196,404"},
            ])
            self.write_csv(overlap, ["roi", "seq", "texture0", "command_index", "roi_coverage_pct", "bbox_coverage_pct"], [
                {
                    "roi": "frame1091-component7",
                    "seq": "1091",
                    "texture0": "0x20000010000005a",
                    "command_index": "150",
                    "roi_coverage_pct": "100",
                    "bbox_coverage_pct": "1.114",
                },
            ])
            self.write_csv(queue, ["roi", "seq", "texture0", "command_index", "max_roi_coverage_pct", "max_bbox_coverage_pct"], [])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--component-csv",
                    str(component),
                    "--local-overlap-csv",
                    str(overlap),
                    "--source-queue-csv",
                    str(queue),
                    "--source-texture",
                    "0x20000010000007f",
                    "--source-texture",
                    "0x200000100000075",
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
            self.assertEqual(rows[0]["verdict"], "blocked-local-non-source")
            self.assertIn("0x20000010000005a:1", rows[0]["evidence"])


if __name__ == "__main__":
    unittest.main()
