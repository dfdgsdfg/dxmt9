import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts/tools/summarize_color_attachment_dumps.py"


class SummarizeColorAttachmentDumpsTests(unittest.TestCase):
    def write_dump(
        self,
        root: Path,
        name: str,
        command_index: int,
        command_draw_index: int,
        pixel_rgba: tuple[int, int, int],
    ) -> None:
        dump = root / name
        r, g, b = pixel_rgba
        dump.write_bytes(bytes([b, g, r, 0]))
        dump.with_suffix(dump.suffix + ".json").write_text(
            json.dumps(
                {
                    "handle": "0x300",
                    "seq": 1,
                    "enc": 2,
                    "afterDraw": 1,
                    "draw": 0,
                    "commandIndex": command_index,
                    "commandDrawIndex": command_draw_index,
                    "commandDrawCount": 12,
                    "texture0": "0x200",
                    "formatName": "X8R8G8B8",
                    "width": 1,
                    "height": 1,
                    "rowBytes": 4,
                }
            )
        )

    def test_summarizes_roi_bright_white_and_command_draw_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dump = root / "color-s1-e2-after-draw-d3-ci4-cmd-d5-of6.bin"
            # X8R8G8B8/BGRA memory order: black, bright blue-ish, white, muted.
            dump.write_bytes(
                bytes(
                    [
                        0,
                        0,
                        0,
                        0,
                        20,
                        230,
                        240,
                        0,
                        250,
                        251,
                        252,
                        0,
                        40,
                        50,
                        60,
                        0,
                    ]
                )
            )
            dump.with_suffix(dump.suffix + ".json").write_text(
                json.dumps(
                    {
                        "handle": "0x300",
                        "seq": 1,
                        "enc": 2,
                        "afterDraw": 1,
                        "draw": 3,
                        "commandIndex": 4,
                        "commandDrawIndex": 5,
                        "commandDrawCount": 6,
                        "texture0": "0x200",
                        "formatName": "X8R8G8B8",
                        "width": 2,
                        "height": 2,
                        "rowBytes": 8,
                    }
                )
            )
            md = root / "summary.md"
            csv_path = root / "summary.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root),
                    "--roi",
                    "0,0,2,2:all",
                    "--output",
                    str(md),
                    "--csv-output",
                    str(csv_path),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with csv_path.open() as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            row = rows[0]
            self.assertEqual(row["seq"], "1")
            self.assertEqual(row["enc"], "2")
            self.assertEqual(row["command_index"], "4")
            self.assertEqual(row["command_draw_index"], "5")
            self.assertEqual(row["command_draw_count"], "6")
            self.assertEqual(row["texture0"], "0x200")
            self.assertEqual(row["max_r"], "252")
            self.assertEqual(row["max_g"], "251")
            self.assertEqual(row["max_b"], "250")
            self.assertEqual(row["bright_pixels"], "2")
            self.assertEqual(row["white_pixels"], "1")
            self.assertEqual(row["warm_pixels"], "2")
            self.assertEqual(row["hot_x"], "0")
            self.assertEqual(row["hot_y"], "1")
            self.assertEqual(row["hot_r"], "252")
            self.assertEqual(row["hot_g"], "251")
            self.assertEqual(row["hot_b"], "250")
            self.assertEqual(row["warm_hot_x"], "0")
            self.assertEqual(row["warm_hot_y"], "1")
            self.assertEqual(row["warm_hot_r"], "252")
            self.assertEqual(row["warm_hot_g"], "251")
            self.assertEqual(row["warm_hot_b"], "250")
            self.assertIn("4:5/6", md.read_text())

    def test_sorts_command_metadata_numerically(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.write_dump(root, "late.bin", 10, 0, (10, 20, 30))
            self.write_dump(root, "early.bin", 2, 9, (40, 50, 60))
            md = root / "summary.md"
            csv_path = root / "summary.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root),
                    "--roi",
                    "0,0,1,1:all",
                    "--output",
                    str(md),
                    "--csv-output",
                    str(csv_path),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with csv_path.open() as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual([row["command_index"] for row in rows], ["2", "10"])
            text = md.read_text()
            self.assertLess(text.index("2:9/12"), text.index("10:0/12"))


if __name__ == "__main__":
    unittest.main()
