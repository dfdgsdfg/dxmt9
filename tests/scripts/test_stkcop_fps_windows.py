"""Offline FPS windows: no Wine, GPU or runtime instrumentation."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

module_path = Path(__file__).resolve().parents[2] / "scripts/tools/summarize_stkcop_fps.py"
spec = importlib.util.spec_from_file_location("stkcop_fps", module_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class WindowsTest(unittest.TestCase):
    def report(self, rows, average, width=1.0):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "day.result"
            path.write_text("[general]\naverage=" + str(average) +
                            "\n[per_frame_stats]\n" + rows)
            return module.summarize(path, width)

    def test_boundary_and_partial_window(self):
        result = self.report("0=2\n1=2\n2=2\n", 2)
        self.assertEqual(result["duration_s"], 1.5)
        self.assertEqual([w["frames"] for w in result["windows"]], [2, 1])
        self.assertEqual([w["fps"] for w in result["windows"]], [2, 2])

    def test_empty_windows_and_stall(self):
        result = self.report("0=0.25\n", 0.25)
        self.assertEqual([w["frames"] for w in result["windows"]], [0, 0, 0, 1])
        self.assertIsNone(result["windows"][0]["p95_frame_ms"])
        self.assertEqual(result["windows"][-1]["max_frame_ms"], 4000)

    def test_invalid_input(self):
        for rows, average, width in [("0=0\n", 1, 1), ("0=nan\n", 1, 1),
                                     ("1=2\n", 2, 1), ("0=2\n", 3, 1),
                                     ("0=2\n", 2, 0), ("0=2\n", 2, 1e-9)]:
            with self.subTest(rows=rows, width=width), self.assertRaises(ValueError):
                self.report(rows, average, width)


if __name__ == "__main__":
    unittest.main()
