#!/usr/bin/env python3
"""Regression tests for experiment screenshot comparison gates."""

from __future__ import annotations

import csv
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "compare_experiment_images.py"


def load_module():
    spec = importlib.util.spec_from_file_location("compare_experiment_images", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def save_rgb(path: Path, data: np.ndarray) -> None:
    Image.fromarray(data.astype(np.uint8), mode="RGB").save(path)


class CompareExperimentImagesTests(unittest.TestCase):
    def test_identical_images_are_exact_match(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            data = np.zeros((4, 4, 3), dtype=np.uint8)
            data[:, :, 1] = 128
            save_rgb(before, data)
            save_rgb(after, data)

            comparisons = module.compare_images(before, after)

            self.assertEqual(len(comparisons), 1)
            self.assertEqual(comparisons[0].changed_pixels, 0)
            self.assertEqual(comparisons[0].changed_pct, 0.0)
            self.assertEqual(comparisons[0].max_delta, 0)
            self.assertAlmostEqual(comparisons[0].ssim, 1.0)

    def test_crop_bottom_removes_bottom_overlay_delta(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            base = np.zeros((4, 4, 3), dtype=np.uint8)
            changed = base.copy()
            changed[3, :, :] = 255
            save_rgb(before, base)
            save_rgb(after, changed)

            comparisons = module.compare_images(before, after, crop_bottom_pixels=1)

            self.assertEqual(comparisons[0].area, "full")
            self.assertAlmostEqual(comparisons[0].changed_pct, 25.0)
            self.assertEqual(comparisons[1].area, "crop-bottom-1")
            self.assertEqual(comparisons[1].changed_pixels, 0)

    def test_named_roi_reports_region_delta(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            base = np.zeros((6, 6, 3), dtype=np.uint8)
            changed = base.copy()
            changed[1:3, 2:5, 0] = 255
            save_rgb(before, base)
            save_rgb(after, changed)

            comparisons = module.compare_images(
                before,
                after,
                rois=[module.parse_roi("2,1,5,3:muzzle")],
            )

            self.assertEqual(comparisons[1].area, "muzzle")
            self.assertEqual(comparisons[1].width, 3)
            self.assertEqual(comparisons[1].height, 2)
            self.assertEqual(comparisons[1].changed_pixels, 6)
            self.assertAlmostEqual(comparisons[1].changed_pct, 100.0)

    def test_cli_writes_report_summary_diff_and_fails_requested_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            report = root / "report.md"
            summary = root / "summary.csv"
            diff = root / "diff.png"
            base = np.zeros((3, 3, 3), dtype=np.uint8)
            changed = base.copy()
            changed[1, 1, 0] = 255
            save_rgb(before, base)
            save_rgb(after, changed)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--before",
                    str(before),
                    "--after",
                    str(after),
                    "--label-before",
                    "baseline",
                    "--label-after",
                    "candidate",
                    "--output",
                    str(report),
                    "--summary-output",
                    str(summary),
                    "--diff-output",
                    str(diff),
                    "--require-similar",
                    "--max-changed-pct",
                    "1.0",
                    "--min-ssim",
                    "0.99",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 1)
            self.assertIn("changed_pct", result.stderr)
            self.assertTrue(diff.is_file())
            text = report.read_text(encoding="utf-8")
            self.assertIn("# Experiment Image Comparison", text)
            self.assertIn("`baseline`", text)
            self.assertIn("Requirement Failures", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["area"], "full")
            self.assertGreater(float(rows[0]["changed_pct"]), 1.0)
            self.assertEqual(rows[0]["before_active_pixels"], "0")
            self.assertEqual(rows[0]["after_active_pixels"], "1")

    def test_cli_fails_when_exact_match_has_no_active_pixels(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            report = root / "report.md"
            black = np.zeros((4, 4, 3), dtype=np.uint8)
            save_rgb(before, black)
            save_rgb(after, black)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--before",
                    str(before),
                    "--after",
                    str(after),
                    "--output",
                    str(report),
                    "--require-similar",
                    "--max-changed-pct",
                    "0",
                    "--min-ssim",
                    "1",
                    "--min-before-active-pct",
                    "1",
                    "--min-after-active-pct",
                    "1",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 1)
            self.assertIn("before_active_pct", result.stderr)
            self.assertIn("after_active_pct", result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("`0.000000%`", text)
            self.assertIn("Requirement Failures", text)

    def test_cli_allows_explicit_lsb_tolerance(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            report = root / "report.md"
            base = np.zeros((8, 8, 3), dtype=np.uint8)
            changed = base.copy()
            changed[2:4, 2:4, :] = 1
            save_rgb(before, base)
            save_rgb(after, changed)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--before",
                    str(before),
                    "--after",
                    str(after),
                    "--output",
                    str(report),
                    "--require-similar",
                    "--max-changed-pct",
                    "10",
                    "--min-ssim",
                    "0.99",
                    "--max-delta",
                    "1",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("Max delta", text)
            self.assertIn("`1`", text)
            self.assertIn("Passed", text)

    def test_cli_writes_roi_summary_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            report = root / "report.md"
            summary = root / "summary.csv"
            base = np.zeros((5, 5, 3), dtype=np.uint8)
            changed = base.copy()
            changed[0:2, 0:2, 1] = 64
            save_rgb(before, base)
            save_rgb(after, changed)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--before",
                    str(before),
                    "--after",
                    str(after),
                    "--output",
                    str(report),
                    "--summary-output",
                    str(summary),
                    "--roi",
                    "0,0,2,2:top-left",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[1]["area"], "top-left")
            self.assertEqual(rows[1]["changed_pixels"], "4")
            self.assertEqual(rows[1]["changed_pct"], "100.000000")

    def test_cli_lsb1_policy_allows_one_lsb_blend_tolerance(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            report = root / "report.md"
            base = np.zeros((64, 64, 3), dtype=np.uint8)
            base[:, :, 1] = 32
            changed = base.copy()
            changed[4:5, 4:5, 0] += 1
            save_rgb(before, base)
            save_rgb(after, changed)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--before",
                    str(before),
                    "--after",
                    str(after),
                    "--output",
                    str(report),
                    "--policy",
                    "lsb1",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("Policy: `lsb1`", text)
            self.assertIn("max_delta=1", text)
            self.assertIn("Passed", text)

    def test_cli_exact_policy_rejects_one_lsb_delta(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            report = root / "report.md"
            base = np.zeros((8, 8, 3), dtype=np.uint8)
            changed = base.copy()
            changed[2, 2, 0] = 1
            save_rgb(before, base)
            save_rgb(after, changed)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--before",
                    str(before),
                    "--after",
                    str(after),
                    "--output",
                    str(report),
                    "--policy",
                    "exact",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 1)
            self.assertIn("changed_pct", result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("Policy: `exact`", text)
            self.assertIn("Requirement Failures", text)

    def test_cli_rejects_delta_above_lsb_tolerance(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.png"
            after = root / "after.png"
            report = root / "report.md"
            base = np.zeros((4, 4, 3), dtype=np.uint8)
            changed = base.copy()
            changed[1, 1, 0] = 2
            save_rgb(before, base)
            save_rgb(after, changed)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--before",
                    str(before),
                    "--after",
                    str(after),
                    "--output",
                    str(report),
                    "--require-similar",
                    "--max-changed-pct",
                    "100",
                    "--min-ssim",
                    "0",
                    "--max-delta",
                    "1",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 1)
            self.assertIn("max_delta", result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("max_delta 2 > 1", text)


if __name__ == "__main__":
    unittest.main()
