#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_tile_ffp_expansion.py"


FIELDS = [
    "seq",
    "encoder",
    "draw_calls",
    "primitive_count",
    "tile_ffp_eligible_primitives",
    "tile_ffp_fallback_gpu_family_primitives",
    "tile_ffp_fallback_not_ffp_primitives",
    "tile_ffp_fallback_precision_primitives",
    "tile_ffp_fallback_unsupported_state_primitives",
    "ffp_draws",
    "programmable_draws",
    "textured_draws",
]


def write_rows(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


class AnalyzeTileFfpExpansionTests(unittest.TestCase):
    def test_classifies_current_programmable_and_unsupported_expansion(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            encoders = root / "encoders.csv"
            report = root / "tile-expansion.md"
            csv_output = root / "tile-expansion.csv"
            write_rows(encoders, [
                {
                    "seq": "60",
                    "encoder": "0",
                    "draw_calls": "10",
                    "primitive_count": "100000",
                    "tile_ffp_eligible_primitives": "50000",
                    "tile_ffp_fallback_gpu_family_primitives": "0",
                    "tile_ffp_fallback_not_ffp_primitives": "50000",
                    "tile_ffp_fallback_precision_primitives": "0",
                    "tile_ffp_fallback_unsupported_state_primitives": "0",
                    "ffp_draws": "5",
                    "programmable_draws": "5",
                    "textured_draws": "0",
                },
                {
                    "seq": "60",
                    "encoder": "1",
                    "draw_calls": "42",
                    "primitive_count": "100000",
                    "tile_ffp_eligible_primitives": "0",
                    "tile_ffp_fallback_gpu_family_primitives": "0",
                    "tile_ffp_fallback_not_ffp_primitives": "100000",
                    "tile_ffp_fallback_precision_primitives": "0",
                    "tile_ffp_fallback_unsupported_state_primitives": "0",
                    "ffp_draws": "0",
                    "programmable_draws": "42",
                    "textured_draws": "42",
                },
                {
                    "seq": "60",
                    "encoder": "2",
                    "draw_calls": "30",
                    "primitive_count": "100000",
                    "tile_ffp_eligible_primitives": "0",
                    "tile_ffp_fallback_gpu_family_primitives": "0",
                    "tile_ffp_fallback_not_ffp_primitives": "0",
                    "tile_ffp_fallback_precision_primitives": "0",
                    "tile_ffp_fallback_unsupported_state_primitives": "100000",
                    "ffp_draws": "30",
                    "programmable_draws": "0",
                    "textured_draws": "30",
                },
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(encoders),
                    "--seq",
                    "60",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_output),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("needs-programmable-tile-route", text)
            self.assertIn("needs-unsupported-state-expansion", text)
            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = {row["row"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["60/0"]["verdict"], "current-tile-ffp-coverage")
            self.assertEqual(rows["60/1"]["verdict"], "needs-programmable-tile-route")
            self.assertEqual(rows["60/1"]["dominant_blocker"], "not-ffp")
            self.assertEqual(rows["60/2"]["verdict"], "needs-unsupported-state-expansion")
            self.assertEqual(rows["60/2"]["dominant_blocker"], "unsupported-state")


if __name__ == "__main__":
    unittest.main()
