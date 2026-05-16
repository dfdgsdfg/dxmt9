import tempfile
import unittest
from pathlib import Path
import sys

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.run_apps import run_experiment


class CaptureMetricTests(unittest.TestCase):
    def write_image(self, pixels: np.ndarray) -> Path:
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        path = Path(temp_dir.name) / "image.png"
        Image.fromarray(pixels.astype(np.uint8), "RGB").save(path)
        return path

    def test_black_frame_is_classified_as_black_screen(self):
        path = self.write_image(np.zeros((4, 4, 3), dtype=np.uint8))
        metrics = run_experiment.image_luma_metrics(path)

        self.assertEqual(metrics["mean_luma"], 0.0)
        self.assertEqual(metrics["variance"], 0.0)
        self.assertTrue(run_experiment.is_black_screen(metrics))

    def test_flat_nonblack_frame_is_not_black_screen(self):
        pixels = np.full((4, 4, 3), 64, dtype=np.uint8)
        path = self.write_image(pixels)
        metrics = run_experiment.image_luma_metrics(path)

        self.assertGreater(metrics["mean_luma"], run_experiment.BLACK_LUMA_THRESHOLD)
        self.assertFalse(run_experiment.is_black_screen(metrics))

    def test_low_mean_high_variance_frame_is_not_black_screen(self):
        pixels = np.zeros((4, 4, 3), dtype=np.uint8)
        pixels[0, 0] = [255, 255, 255]
        path = self.write_image(pixels)
        metrics = run_experiment.image_luma_metrics(path)

        self.assertLessEqual(metrics["mean_luma"], run_experiment.BLACK_LUMA_THRESHOLD * 2)
        self.assertGreater(metrics["variance"], run_experiment.BLACK_VARIANCE_THRESHOLD)
        self.assertFalse(run_experiment.is_black_screen(metrics))


if __name__ == "__main__":
    unittest.main()
