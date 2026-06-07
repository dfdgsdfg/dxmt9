#!/usr/bin/env python3

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "compare_attachment_dumps.py"


def write_dump(path: Path, data: bytes, *, handle: str = "0x1") -> None:
    path.write_bytes(data)
    path.with_name(path.name + ".json").write_text(
        json.dumps({
            "handle": handle,
            "seq": 60,
            "enc": 0,
            "format": 41,
            "formatName": "D24X8",
            "metalPixelFormat": 260,
            "width": 2,
            "height": 2,
            "rowBytes": 4,
            "byteCount": len(data),
        }),
        encoding="utf-8",
    )


class CompareAttachmentDumpsTests(unittest.TestCase):
    def test_exact_match_passes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            before = root / "before.bin"
            after = root / "after.bin"
            out = root / "report.md"
            out_csv = root / "summary.csv"
            write_dump(before, bytes([1, 2, 3, 4]))
            write_dump(after, bytes([1, 2, 3, 4]), handle="0x2")
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--before",
                    str(before),
                    "--after",
                    str(after),
                    "--output",
                    str(out),
                    "--summary-output",
                    str(out_csv),
                    "--require-exact",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn(str(out), result.stdout)
            with out_csv.open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["changed_bytes"], "0")
            self.assertEqual(row["metadata_status"], "compatible")

    def test_byte_difference_fails_exact_gate(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            before = root / "before.bin"
            after = root / "after.bin"
            out = root / "report.md"
            out_csv = root / "summary.csv"
            write_dump(before, bytes([1, 2, 3, 4]))
            write_dump(after, bytes([1, 9, 3, 4]))
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--before",
                    str(before),
                    "--after",
                    str(after),
                    "--output",
                    str(out),
                    "--summary-output",
                    str(out_csv),
                    "--require-exact",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 1)
            with out_csv.open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["changed_bytes"], "1")
            self.assertEqual(row["max_delta"], "7")


if __name__ == "__main__":
    unittest.main()
