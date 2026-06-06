#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_tile_ffp_coverage.py"


FIELDS = [
    "seq",
    "encoder",
    "draw_calls",
    "primitive_count",
    "triangle_estimate",
    "vertex_count",
    "tile_ffp_routed_tile_draws",
    "tile_ffp_routed_tile_primitives",
    "tile_ffp_routed_tile_vertices",
    "tile_ffp_routed_portable_draws",
    "tile_ffp_routed_portable_primitives",
    "tile_ffp_routed_portable_vertices",
    "tile_ffp_eligible_draws",
    "tile_ffp_eligible_primitives",
    "tile_ffp_eligible_vertices",
    "tile_ffp_fallback_gpu_family_draws",
    "tile_ffp_fallback_gpu_family_primitives",
    "tile_ffp_fallback_not_ffp_draws",
    "tile_ffp_fallback_not_ffp_primitives",
    "tile_ffp_fallback_precision_draws",
    "tile_ffp_fallback_precision_primitives",
    "tile_ffp_fallback_unsupported_state_draws",
    "tile_ffp_fallback_unsupported_state_primitives",
    "ffp_draws",
    "programmable_draws",
    "textured_draws",
]


class AnalyzeTileFfpCoverageTests(unittest.TestCase):
    def test_classifies_candidate_low_and_no_coverage_rows(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            encoders = root / "encoders.csv"
            report = root / "tile.md"
            csv_output = root / "tile.csv"
            with encoders.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": "60",
                    "encoder": "0",
                    "draw_calls": "42",
                    "primitive_count": "100000",
                    "triangle_estimate": "100000",
                    "vertex_count": "300000",
                    "tile_ffp_routed_tile_draws": "0",
                    "tile_ffp_routed_tile_primitives": "0",
                    "tile_ffp_routed_tile_vertices": "0",
                    "tile_ffp_routed_portable_draws": "42",
                    "tile_ffp_routed_portable_primitives": "100000",
                    "tile_ffp_routed_portable_vertices": "300000",
                    "tile_ffp_eligible_draws": "0",
                    "tile_ffp_eligible_primitives": "0",
                    "tile_ffp_eligible_vertices": "0",
                    "tile_ffp_fallback_gpu_family_draws": "0",
                    "tile_ffp_fallback_gpu_family_primitives": "0",
                    "tile_ffp_fallback_not_ffp_draws": "42",
                    "tile_ffp_fallback_not_ffp_primitives": "100000",
                    "tile_ffp_fallback_precision_draws": "0",
                    "tile_ffp_fallback_precision_primitives": "0",
                    "tile_ffp_fallback_unsupported_state_draws": "0",
                    "tile_ffp_fallback_unsupported_state_primitives": "0",
                    "ffp_draws": "0",
                    "programmable_draws": "42",
                    "textured_draws": "42",
                })
                writer.writerow({
                    "seq": "60",
                    "encoder": "1",
                    "draw_calls": "10",
                    "primitive_count": "100000",
                    "triangle_estimate": "100000",
                    "vertex_count": "300000",
                    "tile_ffp_routed_tile_draws": "0",
                    "tile_ffp_routed_tile_primitives": "0",
                    "tile_ffp_routed_tile_vertices": "0",
                    "tile_ffp_routed_portable_draws": "10",
                    "tile_ffp_routed_portable_primitives": "100000",
                    "tile_ffp_routed_portable_vertices": "300000",
                    "tile_ffp_eligible_draws": "1",
                    "tile_ffp_eligible_primitives": "1000",
                    "tile_ffp_eligible_vertices": "3000",
                    "tile_ffp_fallback_gpu_family_draws": "0",
                    "tile_ffp_fallback_gpu_family_primitives": "0",
                    "tile_ffp_fallback_not_ffp_draws": "9",
                    "tile_ffp_fallback_not_ffp_primitives": "99000",
                    "tile_ffp_fallback_precision_draws": "0",
                    "tile_ffp_fallback_precision_primitives": "0",
                    "tile_ffp_fallback_unsupported_state_draws": "0",
                    "tile_ffp_fallback_unsupported_state_primitives": "0",
                    "ffp_draws": "1",
                    "programmable_draws": "9",
                    "textured_draws": "0",
                })
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "draw_calls": "10",
                    "primitive_count": "100000",
                    "triangle_estimate": "100000",
                    "vertex_count": "300000",
                    "tile_ffp_routed_tile_draws": "0",
                    "tile_ffp_routed_tile_primitives": "0",
                    "tile_ffp_routed_tile_vertices": "0",
                    "tile_ffp_routed_portable_draws": "10",
                    "tile_ffp_routed_portable_primitives": "100000",
                    "tile_ffp_routed_portable_vertices": "300000",
                    "tile_ffp_eligible_draws": "5",
                    "tile_ffp_eligible_primitives": "50000",
                    "tile_ffp_eligible_vertices": "150000",
                    "tile_ffp_fallback_gpu_family_draws": "0",
                    "tile_ffp_fallback_gpu_family_primitives": "0",
                    "tile_ffp_fallback_not_ffp_draws": "5",
                    "tile_ffp_fallback_not_ffp_primitives": "50000",
                    "tile_ffp_fallback_precision_draws": "0",
                    "tile_ffp_fallback_precision_primitives": "0",
                    "tile_ffp_fallback_unsupported_state_draws": "0",
                    "tile_ffp_fallback_unsupported_state_primitives": "0",
                    "ffp_draws": "5",
                    "programmable_draws": "5",
                    "textured_draws": "0",
                })

            subprocess.run(
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
                check=True,
            )

            text = report.read_text(encoding="utf-8")
            self.assertIn("candidate-tile-ffp-coverage", text)
            self.assertIn("low-tile-ffp-coverage", text)
            self.assertIn("no-tile-ffp-coverage", text)

            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = {row["row"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["60/0"]["verdict"], "no-tile-ffp-coverage")
            self.assertEqual(rows["60/1"]["verdict"], "low-tile-ffp-coverage")
            self.assertEqual(rows["60/2"]["verdict"], "candidate-tile-ffp-coverage")
            self.assertEqual(rows["60/2"]["eligible_primitive_share_pct"], "50.000")


if __name__ == "__main__":
    unittest.main()
