#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_programmable_route_feasibility.py"


FIELDS = [
    "seq",
    "encoder",
    "primitive_count",
    "vertex_count",
    "texture_mask",
    "color_write",
    "alpha_blend",
    "alpha_test",
    "depth_enabled",
    "depth_write",
    "pso",
    "shader_variant",
    "vs",
    "ps",
]


def write_rows(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


class AnalyzeProgrammableRouteFeasibilityTests(unittest.TestCase):
    def test_classifies_depth_only_textured_and_color_routes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            probes = root / "probe.csv"
            report = root / "programmable.md"
            csv_output = root / "programmable.csv"
            write_rows(probes, [
                {
                    "seq": "60",
                    "encoder": "0",
                    "primitive_count": "100",
                    "vertex_count": "300",
                    "texture_mask": "0x7f",
                    "color_write": "0x0",
                    "alpha_blend": "0",
                    "alpha_test": "0",
                    "depth_enabled": "1",
                    "depth_write": "1",
                    "pso": "0x1",
                    "shader_variant": "0xa",
                    "vs": "0x10",
                    "ps": "0x20",
                },
                {
                    "seq": "60",
                    "encoder": "1",
                    "primitive_count": "100",
                    "vertex_count": "300",
                    "texture_mask": "0x7",
                    "color_write": "0xf",
                    "alpha_blend": "0",
                    "alpha_test": "0",
                    "depth_enabled": "1",
                    "depth_write": "0",
                    "pso": "0x2",
                    "shader_variant": "0xb",
                    "vs": "0x11",
                    "ps": "0x21",
                },
                {
                    "seq": "60",
                    "encoder": "2",
                    "primitive_count": "100",
                    "vertex_count": "300",
                    "texture_mask": "0x0",
                    "color_write": "0xf",
                    "alpha_blend": "0",
                    "alpha_test": "0",
                    "depth_enabled": "1",
                    "depth_write": "1",
                    "pso": "0x3",
                    "shader_variant": "0xc",
                    "vs": "0x12",
                    "ps": "0x22",
                },
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(probes),
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
            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = {row["row"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["60/0"]["verdict"], "candidate-depth-only-route")
            self.assertEqual(rows["60/1"]["verdict"], "needs-programmable-textured-route")
            self.assertEqual(rows["60/2"]["verdict"], "needs-programmable-color-route")
            self.assertIn("candidate-depth-only-route", report.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
