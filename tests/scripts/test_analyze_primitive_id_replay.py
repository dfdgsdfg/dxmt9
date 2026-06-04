#!/usr/bin/env python3
"""Regression tests for primitive-id replay comparison."""

from __future__ import annotations

import csv
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_primitive_id_replay.py"


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_primitive_id_replay", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def encode_id(value: int) -> list[int]:
    encoded = value + 1
    return [encoded & 0xff, (encoded >> 8) & 0xff, (encoded >> 16) & 0xff]


def save_ids(path: Path, ids: np.ndarray) -> None:
    rgb = np.zeros((ids.shape[0], ids.shape[1], 3), dtype=np.uint8)
    for y in range(ids.shape[0]):
        for x in range(ids.shape[1]):
            if ids[y, x] >= 0:
                rgb[y, x, :] = encode_id(int(ids[y, x]))
    Image.fromarray(rgb, mode="RGB").save(path)


def save_color(path: Path, changed: bool) -> None:
    rgb = np.zeros((2, 2, 3), dtype=np.uint8)
    if changed:
        rgb[0, 0, 0] = 8
    Image.fromarray(rgb, mode="RGB").save(path)


class AnalyzePrimitiveIdReplayTests(unittest.TestCase):
    def test_canonicalizes_reverse_order_to_original_triangle_ids(self) -> None:
        module = load_module()
        indices = [0, 1, 2, 3, 4, 5]
        self.assertEqual(
            module.order_to_original_triangle_map(indices, "reverse-triangles"),
            [1, 0],
        )

    def test_cli_reports_color_changed_pixel_triangle_identity(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            index_file = root / "draw.index.bin"
            index_file.write_bytes(struct.pack("<6H", 0, 1, 2, 3, 4, 5))
            before_ids = np.array([[0, 1], [-1, -1]], dtype=np.int64)
            # Candidate was submitted in reverse order: submit id 0 maps to
            # original triangle 1, submit id 1 maps to original triangle 0.
            after_ids = np.array([[0, 1], [-1, -1]], dtype=np.int64)
            before_primitive = root / "before.ppm"
            after_primitive = root / "after.ppm"
            save_ids(before_primitive, before_ids)
            save_ids(after_primitive, after_ids)
            before_color = root / "before-color.ppm"
            after_color = root / "after-color.ppm"
            save_color(before_color, False)
            save_color(after_color, True)
            report = root / "report.md"
            pixels = root / "pixels.csv"
            summary = root / "summary.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--index-file",
                    str(index_file),
                    "--before-primitive-id",
                    str(before_primitive),
                    "--after-primitive-id",
                    str(after_primitive),
                    "--before-primitive-order",
                    "original",
                    "--after-primitive-order",
                    "reverse-triangles",
                    "--before-color",
                    str(before_color),
                    "--after-color",
                    str(after_color),
                    "--output",
                    str(report),
                    "--pixel-csv-output",
                    str(pixels),
                    "--summary-csv-output",
                    str(summary),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("# Primitive ID Replay Analysis", text)
            self.assertIn("Primitive identity changed pixels", text)
            self.assertIn("Color + primitive changed pixels", text)
            with pixels.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["x"], "0")
            self.assertEqual(rows[0]["y"], "0")
            self.assertEqual(rows[0]["before_original_triangle"], "0")
            self.assertEqual(rows[0]["after_original_triangle"], "1")
            self.assertEqual(rows[0]["primitive_identity_changed"], "1")
            self.assertEqual(rows[0]["color_changed"], "1")
            self.assertEqual(rows[0]["before_color_rgb"], "0,0,0")
            self.assertEqual(rows[0]["after_color_rgb"], "8,0,0")
            self.assertEqual(rows[0]["color_delta_max"], "8")
            self.assertEqual(rows[0]["color_delta_l1"], "8")
            with summary.open(newline="", encoding="utf-8") as handle:
                summary_rows = list(csv.DictReader(handle))
            self.assertEqual(len(summary_rows), 1)
            self.assertEqual(summary_rows[0]["primitive_identity_changed_pixels"], "2")
            self.assertEqual(summary_rows[0]["color_changed_pixels"], "1")
            self.assertEqual(summary_rows[0]["color_and_primitive_changed_pixels"], "1")
            self.assertEqual(summary_rows[0]["max_color_delta"], "8")
            self.assertEqual(summary_rows[0]["max_color_delta_l1"], "8")

    def test_cli_can_emit_all_primitive_changed_pixels_even_with_color_images(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            index_file = root / "draw.index.bin"
            index_file.write_bytes(struct.pack("<6H", 0, 1, 2, 3, 4, 5))
            before_ids = np.array([[0, 1], [-1, -1]], dtype=np.int64)
            after_ids = np.array([[0, 1], [-1, -1]], dtype=np.int64)
            before_primitive = root / "before.ppm"
            after_primitive = root / "after.ppm"
            save_ids(before_primitive, before_ids)
            save_ids(after_primitive, after_ids)
            before_color = root / "before-color.ppm"
            after_color = root / "after-color.ppm"
            save_color(before_color, False)
            save_color(after_color, False)
            report = root / "report.md"
            pixels = root / "pixels.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--index-file",
                    str(index_file),
                    "--before-primitive-id",
                    str(before_primitive),
                    "--after-primitive-id",
                    str(after_primitive),
                    "--before-primitive-order",
                    "original",
                    "--after-primitive-order",
                    "reverse-triangles",
                    "--before-color",
                    str(before_color),
                    "--after-color",
                    str(after_color),
                    "--pixel-scope",
                    "primitive-changed",
                    "--output",
                    str(report),
                    "--pixel-csv-output",
                    str(pixels),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with pixels.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 2)
            self.assertTrue(all(row["primitive_identity_changed"] == "1" for row in rows))
            self.assertTrue(all(row["color_changed"] == "0" for row in rows))
            self.assertTrue(all(row["color_delta_max"] == "0" for row in rows))


if __name__ == "__main__":
    unittest.main()
