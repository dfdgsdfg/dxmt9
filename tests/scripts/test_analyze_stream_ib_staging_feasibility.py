#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_stream_ib_staging_feasibility.py"


PROBE_DRAW_FIELDS = [
    "seq",
    "encoder",
    "encoder_draw_index",
    "primitive_count",
    "vertex_count",
    "original_index_min",
    "original_index_max",
    "original_stream0_byte_min",
    "original_stream0_byte_max",
    "base_vertex",
    "index_buffer",
    "effective_index_source",
    "effective_index_offset",
    "effective_index_bytes",
    "stream0_handle",
    "stream0_stride",
    "stream_extra_bindings",
]

ENCODER_FIELDS = [
    "seq",
    "encoder",
    "argbuf_table_bytes",
    "argbuf_cbuf_bytes",
    "set_vertex_bytes_bytes",
    "transient_vertex_bytes",
    "transient_index_bytes",
]


class AnalyzeStreamIbStagingFeasibilityTests(unittest.TestCase):
    def test_estimates_staging_copy_cost_and_offset_tradeoff(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            probe_draws = root / "probe-draws.csv"
            encoders = root / "encoders.csv"
            report = root / "staging.md"
            csv_output = root / "staging.csv"

            with probe_draws.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=PROBE_DRAW_FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "encoder_draw_index": "0",
                    "primitive_count": "5000",
                    "vertex_count": "15000",
                    "original_index_min": "0",
                    "original_index_max": "9",
                    "original_stream0_byte_min": "0",
                    "original_stream0_byte_max": "216",
                    "base_vertex": "0",
                    "index_buffer": "0x102",
                    "effective_index_source": "staged-original",
                    "effective_index_offset": "0",
                    "effective_index_bytes": "60",
                    "stream0_handle": "0x100",
                    "stream0_stride": "24",
                    "stream_extra_bindings": "s1:0x101@0/32",
                })
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "encoder_draw_index": "1",
                    "primitive_count": "5000",
                    "vertex_count": "15000",
                    "original_index_min": "0",
                    "original_index_max": "4",
                    "original_stream0_byte_min": "0",
                    "original_stream0_byte_max": "96",
                    "base_vertex": "0",
                    "index_buffer": "0x202",
                    "effective_index_source": "original",
                    "effective_index_offset": "0",
                    "effective_index_bytes": "30",
                    "stream0_handle": "0x200",
                    "stream0_stride": "24",
                    "stream_extra_bindings": "s1:0x201@0/32",
                })

            with encoders.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=ENCODER_FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "argbuf_table_bytes": "10",
                    "argbuf_cbuf_bytes": "20",
                    "set_vertex_bytes_bytes": "30",
                    "transient_vertex_bytes": "40",
                    "transient_index_bytes": "0",
                })

            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(probe_draws),
                    "--encoder-csv",
                    str(encoders),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_output),
                ],
                cwd=REPO_ROOT,
                check=True,
            )

            text = report.read_text(encoding="utf-8")
            self.assertIn("staging-ab-preflight-required", text)

            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = {row["row"]: row for row in csv.DictReader(handle)}
            row = rows["60/2"]
            self.assertEqual(row["verdict"], "staging-ab-candidate-offset-risk")
            self.assertEqual(row["draws"], "2")
            self.assertEqual(row["triangles"], "10000")
            self.assertEqual(row["tuple_changes"], "1")
            self.assertEqual(row["tuple_unique"], "2")
            self.assertEqual(row["stream_slots"], "2")
            self.assertEqual(row["stream0_unique_handles"], "2")
            self.assertEqual(row["extra_stream_unique_handles"], "2")
            self.assertEqual(row["ib_unique_handles"], "2")
            self.assertEqual(row["stream0_copy_bytes"], "360")
            self.assertEqual(row["extra_stream_copy_bytes"], "480")
            self.assertEqual(row["ib_copy_bytes"], "90")
            self.assertEqual(row["staging_copy_bytes"], "930")
            self.assertEqual(row["existing_explicit_writer_bytes"], "100")
            self.assertEqual(row["staging_to_existing_writer_ratio"], "9.300")
            self.assertEqual(row["expected_stream_offset_changes"], "2")
            self.assertEqual(row["expected_ib_offset_changes"], "1")
            self.assertEqual(row["expected_offset_changes_per_draw"], "1.500")


if __name__ == "__main__":
    unittest.main()
