#!/usr/bin/env python3
"""Regression tests for primitive-conflict mini-replay analysis."""

from __future__ import annotations

import csv
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_mini_replay_primitive_conflicts.py"


def pack_floats(values: list[float]) -> bytes:
    return struct.pack("<" + "f" * len(values), *values)


def write_vsconsts(path: Path) -> None:
    constants = [0.0] * (256 * 4)
    def set_c(index: int, value: tuple[float, float, float, float]) -> None:
        base = index * 4
        constants[base:base + 4] = list(value)

    set_c(4, (1.0, 0.0, 0.0, 0.0))
    set_c(5, (0.0, 1.0, 0.0, 0.0))
    set_c(6, (0.0, 0.0, 1.0, 0.0))
    set_c(7, (0.0, 0.0, 0.0, 1.0))
    path.write_bytes(pack_floats(constants) + bytes(16 * 16 + 16 * 4))


def write_streams(root: Path) -> tuple[Path, Path]:
    # Two screen-overlapping triangles in NDC. Pixel 1,1 in a 4x4 target has
    # center NDC (-0.25, 0.25), so both triangles cover it.
    positions = [
        (-0.5, 0.5, 0.2), (0.5, 0.5, 0.2), (-0.5, -0.5, 0.2),
        (-0.5, 0.5, 0.3), (0.5, 0.5, 0.3), (-0.5, -0.5, 0.3),
    ]
    stream0 = bytearray()
    for x, y, z in positions:
        stream0.extend(pack_floats([x, y, z, 0.0, 0.0, 0.0]))
    stream0_path = root / "draw.stream0.bin"
    stream0_path.write_bytes(bytes(stream0))

    uvs = [
        (0.0, 0.0), (1.0, 0.0), (0.0, 1.0),
        (0.3, 0.0), (1.3, 0.0), (0.3, 1.0),
    ]
    stream1 = bytearray()
    for u, v in uvs:
        stream1.extend(pack_floats([0.0, 0.0, 1.0]))
        stream1.extend(pack_floats([0.0, 1.0, 0.0]))
        stream1.extend(pack_floats([u, v]))
    stream1_path = root / "draw.stream1.bin"
    stream1_path.write_bytes(bytes(stream1))
    return stream0_path, stream1_path


def write_fixture(root: Path) -> tuple[Path, Path]:
    stream0_path, stream1_path = write_streams(root)
    index_path = root / "draw.index.bin"
    index_path.write_bytes(struct.pack("<HHHHHH", 0, 1, 2, 3, 4, 5))
    vsconsts_path = root / "draw.vsconsts.bin"
    write_vsconsts(vsconsts_path)
    ffpvs_path = root / "draw.ffpvs.bin"
    ffpvs_path.write_bytes(bytes(2120))

    manifest = {
        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
        "draws": [{
            "row": "50/2",
            "seq": 50,
            "encoder": 2,
            "encoder_draw_index": 18,
            "draw_ordinal": 25582,
            "attachments": {"colors": [{"width": 4, "height": 4}], "depth": {"width": 4, "height": 4}},
            "state": {
                "primitive_type": 3,
                "index_count": 6,
                "primitive_count": 2,
                "index_type": "uint16",
                "stream0_stride": 24,
            },
            "geometry": {
                "index_file": str(index_path),
                "stream0_file": str(stream0_path),
                "streams": [
                    {"stream": 0, "file": str(stream0_path)},
                    {"stream": 1, "file": str(stream1_path)},
                ],
            },
            "uniforms": {
                "vsconsts_file": str(vsconsts_path),
                "ffpvs_file": str(ffpvs_path),
            },
        }],
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    pixels_path = root / "primitive-pixels.csv"
    with pixels_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=[
            "x",
            "y",
            "before_original_triangle",
            "after_original_triangle",
            "primitive_identity_changed",
            "color_changed",
            "before_color_rgb",
            "after_color_rgb",
            "color_delta_max",
            "color_delta_l1",
        ])
        writer.writeheader()
        writer.writerow({
            "x": "1",
            "y": "1",
            "before_original_triangle": "0",
            "after_original_triangle": "1",
            "primitive_identity_changed": "1",
            "color_changed": "1",
            "before_color_rgb": "0,0,0",
            "after_color_rgb": "5,1,0",
            "color_delta_max": "5",
            "color_delta_l1": "6",
        })
    return manifest_path, pixels_path


class PrimitiveConflictAnalysisTest(unittest.TestCase):
    def test_reports_depth_and_uv_delta(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, pixels = write_fixture(root)
            output = root / "analysis.md"
            csv_output = root / "analysis.csv"
            summary_output = root / "summary.csv"
            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--manifest",
                    str(manifest),
                    "--draw-index",
                    "0",
                    "--primitive-pixel-csv",
                    str(pixels),
                    "--output",
                    str(output),
                    "--csv-output",
                    str(csv_output),
                    "--summary-csv-output",
                    str(summary_output),
                ],
                check=True,
            )
            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["both_triangles_cover_pixel"], "1")
            self.assertAlmostEqual(float(rows[0]["depth_delta"]), 0.1, places=5)
            self.assertAlmostEqual(float(rows[0]["uv0_delta"]), 0.3, places=5)
            with summary_output.open(newline="", encoding="utf-8") as handle:
                summary_rows = list(csv.DictReader(handle))
            self.assertEqual(len(summary_rows), 1)
            self.assertEqual(summary_rows[0]["pixels"], "1")
            self.assertEqual(summary_rows[0]["color_changed_pixels"], "1")
            self.assertEqual(summary_rows[0]["max_color_delta"], "5")
            self.assertEqual(summary_rows[0]["max_color_delta_l1"], "6")
            self.assertEqual(summary_rows[0]["both_cover_pixels"], "1")
            text = output.read_text(encoding="utf-8")
            self.assertIn("Max abs depth delta", text)
            self.assertIn("Color changed pixels", text)


if __name__ == "__main__":
    unittest.main()
