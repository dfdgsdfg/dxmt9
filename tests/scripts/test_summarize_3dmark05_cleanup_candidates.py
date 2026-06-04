#!/usr/bin/env python3
"""Regression tests for 3DMark05 cleanup candidate summaries."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_3dmark05_cleanup_candidates.py"


def write_bytes(path: Path, size: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"x" * size)


class Summarize3DMark05CleanupCandidatesTests(unittest.TestCase):
    def test_groups_trace_and_output_by_run_id_and_marks_references(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            trace_root = root / "traces"
            output_root = root / "output"
            referenced = "app-d3d9-3dmark05-kept-r1"
            stale = "app-d3d9-3dmark05-stale-r1"

            write_bytes(trace_root / referenced / "frame50.gputrace" / "raw.bin", 7)
            write_bytes(output_root / referenced / "result.json", 11)
            write_bytes(trace_root / stale / "analysis" / "old.bin", 17)
            write_bytes(output_root / stale / "dxmt9.log", 19)

            references = root / "perfomance.plan.md"
            references.write_text(f"keep `{referenced}` here\n", encoding="utf-8")
            report = root / "cleanup.md"
            summary = root / "cleanup.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--trace-root",
                    str(trace_root),
                    "--output-root",
                    str(output_root),
                    "--reference-file",
                    str(references),
                    "--top",
                    "0",
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
                rows = {row["run_id"]: row for row in csv.DictReader(handle)}

            self.assertEqual(rows[referenced]["status"], "referenced")
            self.assertEqual(rows[referenced]["reference_count"], "1")
            self.assertEqual(rows[referenced]["total_size_bytes"], "18")
            self.assertEqual(rows[stale]["status"], "unreferenced")
            self.assertEqual(rows[stale]["total_size_bytes"], "36")
            text = report.read_text(encoding="utf-8")
            self.assertIn("3DMark05 Cleanup Candidates", text)
            self.assertIn(stale, text)
            self.assertIn("non-destructive", text)


if __name__ == "__main__":
    unittest.main()
