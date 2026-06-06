#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_stream_ib_backend_churn.py"


ENCODER_FIELDS = [
    "seq",
    "encoder",
    "draw_calls",
    "triangle_estimate",
    "vertex_count",
    "stream_metal_binds",
    "stream_metal_bind_handle_changes",
    "stream_metal_bind_offset_changes",
    "stream_handle_changes",
    "stream_offset_changes",
    "stream_stride_changes",
    "stream_unique_handles",
    "stream_unique_bytes",
    "ib_metal_binds",
    "ib_handle_changes",
    "ib_unique_handles",
    "ib_unique_bytes",
    "argbuf_table_bytes",
    "argbuf_cbuf_bytes",
    "set_vertex_bytes_bytes",
    "transient_vertex_bytes",
    "transient_index_bytes",
    "indexed_cache_opt_candidate_miss_delta_32",
    "pso_handle_changes",
]


STREAM_FIELDS = [
    "seq",
    "encoder",
    "stream",
    "samples",
    "metal_binds",
    "metal_bind_firsts",
    "metal_bind_handle_changes",
    "metal_bind_offset_changes",
    "unique_handles",
    "unique_handle_overflows",
    "unique_bytes",
    "unique_dynamic_handles",
    "unique_writeonly_handles",
    "unique_default_pool_handles",
    "unique_managed_pool_handles",
    "unique_systemmem_pool_handles",
    "unique_scratch_pool_handles",
    "handle_changes",
    "offset_changes",
    "stride_changes",
    "last_handle",
    "last_offset",
    "last_stride",
]


PROBE_DRAW_FIELDS = [
    "seq",
    "encoder",
    "encoder_draw_index",
    "stream0_handle",
    "index_buffer",
    "stream_extra_bindings",
]


class AnalyzeStreamIbBackendChurnTests(unittest.TestCase):
    def test_classifies_handle_churn_and_stream_breakdown(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            encoders = root / "encoders.csv"
            streams = root / "streams.csv"
            probe_draws = root / "probe-draws.csv"
            report = root / "stream-ib.md"
            csv_output = root / "stream-ib.csv"

            with encoders.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=ENCODER_FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "draw_calls": "187",
                    "triangle_estimate": "389376",
                    "vertex_count": "1168128",
                    "stream_metal_binds": "273",
                    "stream_metal_bind_handle_changes": "271",
                    "stream_metal_bind_offset_changes": "0",
                    "stream_handle_changes": "271",
                    "stream_offset_changes": "0",
                    "stream_stride_changes": "10",
                    "stream_unique_handles": "59",
                    "stream_unique_bytes": "6938808",
                    "ib_metal_binds": "187",
                    "ib_handle_changes": "160",
                    "ib_unique_handles": "34",
                    "ib_unique_bytes": "443454",
                    "argbuf_table_bytes": "5024",
                    "argbuf_cbuf_bytes": "96136",
                    "set_vertex_bytes_bytes": "2992",
                    "transient_vertex_bytes": "0",
                    "transient_index_bytes": "0",
                    "indexed_cache_opt_candidate_miss_delta_32": "-175168",
                    "pso_handle_changes": "47",
                })
                writer.writerow({
                    "seq": "60",
                    "encoder": "8",
                    "draw_calls": "5",
                    "triangle_estimate": "26",
                    "vertex_count": "78",
                    "stream_metal_binds": "5",
                    "stream_metal_bind_handle_changes": "0",
                    "stream_metal_bind_offset_changes": "4",
                    "stream_handle_changes": "0",
                    "stream_offset_changes": "4",
                    "stream_stride_changes": "1",
                    "stream_unique_handles": "1",
                    "stream_unique_bytes": "786432",
                    "ib_metal_binds": "5",
                    "ib_handle_changes": "1",
                    "ib_unique_handles": "1",
                    "ib_unique_bytes": "98304",
                    "argbuf_table_bytes": "96",
                    "argbuf_cbuf_bytes": "3016",
                    "set_vertex_bytes_bytes": "80",
                    "transient_vertex_bytes": "0",
                    "transient_index_bytes": "24",
                    "indexed_cache_opt_candidate_miss_delta_32": "0",
                    "pso_handle_changes": "2",
                })

            with streams.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=STREAM_FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "stream": "0",
                    "samples": "187",
                    "metal_binds": "161",
                    "metal_bind_firsts": "1",
                    "metal_bind_handle_changes": "160",
                    "metal_bind_offset_changes": "0",
                    "unique_handles": "34",
                    "unique_handle_overflows": "0",
                    "unique_bytes": "2344728",
                    "unique_dynamic_handles": "0",
                    "unique_writeonly_handles": "34",
                    "unique_default_pool_handles": "0",
                    "unique_managed_pool_handles": "34",
                    "unique_systemmem_pool_handles": "0",
                    "unique_scratch_pool_handles": "0",
                    "handle_changes": "160",
                    "offset_changes": "0",
                    "stride_changes": "0",
                    "last_handle": "0x1",
                    "last_offset": "0",
                    "last_stride": "24",
                })
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "stream": "1",
                    "samples": "126",
                    "metal_binds": "112",
                    "metal_bind_firsts": "1",
                    "metal_bind_handle_changes": "111",
                    "metal_bind_offset_changes": "0",
                    "unique_handles": "25",
                    "unique_handle_overflows": "0",
                    "unique_bytes": "4594080",
                    "unique_dynamic_handles": "0",
                    "unique_writeonly_handles": "25",
                    "unique_default_pool_handles": "0",
                    "unique_managed_pool_handles": "25",
                    "unique_systemmem_pool_handles": "0",
                    "unique_scratch_pool_handles": "0",
                    "handle_changes": "111",
                    "offset_changes": "0",
                    "stride_changes": "10",
                    "last_handle": "0x2",
                    "last_offset": "0",
                    "last_stride": "32",
                })

            with probe_draws.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=PROBE_DRAW_FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "encoder_draw_index": "0",
                    "stream0_handle": "0x100",
                    "index_buffer": "0x102",
                    "stream_extra_bindings": "s1:0x101@0/32",
                })
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "encoder_draw_index": "1",
                    "stream0_handle": "0x200",
                    "index_buffer": "0x202",
                    "stream_extra_bindings": "s1:0x201@0/32",
                })
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "encoder_draw_index": "2",
                    "stream0_handle": "0x200",
                    "index_buffer": "0x202",
                    "stream_extra_bindings": "s1:0x203@0/32",
                })

            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(encoders),
                    "--streams-csv",
                    str(streams),
                    "--probe-draws",
                    str(probe_draws),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_output),
                ],
                cwd=REPO_ROOT,
                check=True,
            )

            text = report.read_text(encoding="utf-8")
            self.assertIn("stream-ib-isolation-required", text)
            self.assertIn("s0:h160/o0/s0/u34;s1:h111/o0/s10/u25", text)

            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = {row["row"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["60/2"]["verdict"], "handle-churn-dominant")
            self.assertEqual(rows["60/8"]["verdict"], "low-geometry")
            self.assertEqual(rows["60/2"]["explicit_writer_bytes"], "104152")
            self.assertEqual(rows["60/2"]["draw_stream0_handle_changes"], "1")
            self.assertEqual(rows["60/2"]["draw_index_buffer_changes"], "1")
            self.assertEqual(rows["60/2"]["draw_extra_stream_binding_changes"], "2")
            self.assertEqual(rows["60/2"]["draw_extra_stream_binding_unique"], "3")
            self.assertEqual(rows["60/2"]["draw_binding_tuple_changes"], "2")
            self.assertEqual(rows["60/2"]["draw_binding_tuple_unique"], "3")
            self.assertEqual(rows["60/2"]["draw_binding_tuple_max_run"], "1")
            self.assertEqual(rows["60/2"]["draw_binding_tuple_avg_run"], "1.000")
            self.assertEqual(rows["60/2"]["draw_stream0_handle_unique"], "2")
            self.assertEqual(rows["60/2"]["draw_index_buffer_unique"], "2")
            self.assertEqual(rows["60/2"]["draw_first_extra_handle_unique"], "3")
            self.assertEqual(rows["60/2"]["draw_consecutive_pair_count"], "3")
            self.assertEqual(rows["60/2"]["draw_consecutive_pair_samples"], "3")
            self.assertEqual(rows["60/2"]["draw_consecutive_pair_ratio"], "1.000")
            self.assertEqual(rows["60/2"]["draw_stream0_ib_delta_top"], "+2x3")
            self.assertEqual(rows["60/2"]["draw_consecutive_triplet_count"], "2")
            self.assertEqual(rows["60/2"]["draw_consecutive_triplet_samples"], "3")
            self.assertEqual(rows["60/2"]["draw_consecutive_triplet_ratio"], "0.667")
            self.assertEqual(rows["60/2"]["draw_triplet_delta_top"], "+1/+2x2;+3/+2x1")
            self.assertEqual(rows["60/2"]["draw_transition_classes"], "s0+ib+extrax1;extrax1")
            self.assertIn("s0=0x100,ib=0x102,extra=s1:0x101@0/32x1",
                          rows["60/2"]["draw_top_binding_tuples"])


if __name__ == "__main__":
    unittest.main()
