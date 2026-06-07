#!/usr/bin/env python3
"""Regression tests for capture ROI image summaries."""

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
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_capture_rois.py"


def load_module():
    spec = importlib.util.spec_from_file_location("summarize_capture_rois", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def save_rgb(path: Path, data: np.ndarray) -> None:
    Image.fromarray(data.astype(np.uint8), mode="RGB").save(path)


class SummarizeCaptureRoisTests(unittest.TestCase):
    def test_summarize_image_reports_warm_and_white_roi_pixels(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = root / "frame001092.png"
            data = np.zeros((6, 8, 3), dtype=np.uint8)
            data[1:3, 2:5, :] = [255, 245, 230]
            data[4, 7, :] = [0, 255, 255]
            save_rgb(image, data)

            rows = module.summarize_image(
                image,
                [module.parse_roi("2,1,5,3:muzzle"), module.parse_roi("7,4,8,5:cyan")],
                bright_threshold=220,
                white_threshold=240,
                warm_red_threshold=180,
                warm_green_threshold=110,
                warm_blue_margin=32,
            )

            self.assertEqual(rows[0]["frame"], "1092")
            self.assertEqual(rows[0]["roi"], "muzzle")
            self.assertEqual(rows[0]["warm_pixels"], 6)
            self.assertEqual(rows[0]["white_pixels"], 0)
            self.assertEqual(rows[0]["bright_pixels"], 6)
            self.assertEqual(rows[1]["roi"], "cyan")
            self.assertEqual(rows[1]["warm_pixels"], 0)
            self.assertEqual(rows[1]["bright_pixels"], 1)

    def test_cli_writes_signal_sorted_markdown_and_csv(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            img1 = root / "frame000001.png"
            img2 = root / "frame000002.png"
            report = root / "summary.md"
            csv_path = root / "summary.csv"
            first = np.zeros((4, 4, 3), dtype=np.uint8)
            second = np.zeros((4, 4, 3), dtype=np.uint8)
            first[0, 0, :] = [255, 255, 255]
            second[0:2, 0:2, :] = [255, 200, 120]
            save_rgb(img1, first)
            save_rgb(img2, second)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root),
                    "--roi",
                    "0,0,4,4:full",
                    "--sort",
                    "signal",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_path),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("# Capture ROI Summary", text)
            self.assertLess(text.find("frame000002.png"), text.find("frame000001.png"))
            with csv_path.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["image"], "frame000002.png")
            self.assertEqual(rows[0]["warm_pixels"], "4")

    def test_cli_filters_image_names_when_scanning_directory(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            keep = root / "frame000001.png"
            derived = root / "frame000001-rois.png"
            montage = root / "scout-montage.png"
            report = root / "summary.md"
            csv_path = root / "summary.csv"
            data = np.zeros((2, 2, 3), dtype=np.uint8)
            save_rgb(keep, data)
            save_rgb(derived, data)
            save_rgb(montage, data)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root),
                    "--include-name-regex",
                    r"^frame[0-9]+\.png$",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_path),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with csv_path.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual([row["image"] for row in rows], ["frame000001.png"])

    def test_cli_writes_frame_score_summary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = root / "frame000010.png"
            report = root / "summary.md"
            csv_path = root / "summary.csv"
            score_report = root / "scores.md"
            score_csv = root / "scores.csv"
            montage = root / "scores.png"
            data = np.zeros((4, 8, 3), dtype=np.uint8)
            data[0:2, 0:2, :] = [255, 210, 150]
            data[0, 6, :] = [255, 210, 150]
            save_rgb(image, data)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root),
                    "--include-name-regex",
                    r"^frame[0-9]+\.png$",
                    "--roi",
                    "0,0,2,2:muzzle",
                    "--roi",
                    "6,0,8,2:control",
                    "--candidate-roi",
                    "muzzle",
                    "--control-roi",
                    "control",
                    "--frame-score-output",
                    str(score_report),
                    "--frame-score-csv-output",
                    str(score_csv),
                    "--frame-score-montage-output",
                    str(montage),
                    "--min-candidate-warm",
                    "1",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_path),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = score_report.read_text(encoding="utf-8")
            self.assertIn("# Capture Frame Score Summary", text)
            self.assertIn("candidate-dominates-control", text)
            with score_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["frame"], "10")
            self.assertEqual(rows[0]["best_candidate_roi"], "muzzle")
            self.assertEqual(rows[0]["candidate_warm_pixels"], "4")
            self.assertEqual(rows[0]["control_warm_pixels"], "1")
            self.assertEqual(rows[0]["verdict"], "candidate-dominates-control")
            self.assertTrue(montage.is_file())
            with Image.open(montage) as image:
                self.assertGreaterEqual(image.width, 600)
                self.assertGreaterEqual(image.height, 180)

    def test_cli_writes_filtered_warm_components(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = root / "frame000020.png"
            report = root / "summary.md"
            component_report = root / "components.md"
            component_csv = root / "components.csv"
            component_montage = root / "components.png"
            data = np.zeros((12, 12, 3), dtype=np.uint8)
            data[1:3, 1:3, :] = [255, 245, 245]
            data[6:12, 6:12, :] = [255, 210, 140]
            save_rgb(image, data)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root),
                    "--include-name-regex",
                    r"^frame[0-9]+\.png$",
                    "--output",
                    str(report),
                    "--component-output",
                    str(component_report),
                    "--component-csv-output",
                    str(component_csv),
                    "--component-montage-output",
                    str(component_montage),
                    "--component-min-area",
                    "1",
                    "--component-max-area",
                    "10",
                    "--component-min-white",
                    "1",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = component_report.read_text(encoding="utf-8")
            self.assertIn("# Capture Warm Component Summary", text)
            with component_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["frame"], "20")
            self.assertEqual(rows[0]["bbox"], "1,1,3,3")
            self.assertEqual(rows[0]["area"], "4")
            self.assertEqual(rows[0]["bbox_area"], "4")
            self.assertEqual(rows[0]["bbox_aspect_ratio"], "1.000000")
            self.assertEqual(rows[0]["white_pixels"], "4")
            self.assertTrue(component_montage.is_file())

    def test_cli_filters_warm_components_by_shape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = root / "frame000021.png"
            report = root / "summary.md"
            component_report = root / "components.md"
            component_csv = root / "components.csv"
            data = np.zeros((12, 16, 3), dtype=np.uint8)
            data[1:5, 1:5, :] = [255, 245, 245]
            data[8:9, 1:12, :] = [255, 245, 245]
            save_rgb(image, data)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root),
                    "--include-name-regex",
                    r"^frame[0-9]+\.png$",
                    "--output",
                    str(report),
                    "--component-output",
                    str(component_report),
                    "--component-csv-output",
                    str(component_csv),
                    "--component-min-area",
                    "1",
                    "--component-max-area",
                    "100",
                    "--component-max-aspect-ratio",
                    "2.0",
                    "--component-min-fill-pct",
                    "50",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with component_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["bbox"], "1,1,5,5")
            self.assertEqual(rows[0]["bbox_fill_pct"], "100.000000")
            self.assertEqual(rows[0]["bbox_aspect_ratio"], "1.000000")

    def test_invalid_roi_is_rejected(self) -> None:
        module = load_module()
        with self.assertRaises(Exception):
            module.parse_roi("0,0,0,1:bad")


if __name__ == "__main__":
    unittest.main()
