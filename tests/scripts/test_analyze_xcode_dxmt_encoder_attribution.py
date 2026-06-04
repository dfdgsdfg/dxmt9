#!/usr/bin/env python3
"""Regression tests for Xcode/dxmt encoder attribution reports."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_xcode_dxmt_encoder_attribution.py"


def write_joined(path: Path) -> None:
    fields = [
        "seq",
        "enc",
        "gpu_ms",
        "gpu_share_pct",
        "buffer_write_mib",
        "vs_buffer_write_mib",
        "dxmt_hidden_backend_write_mib",
        "dxmt_named_tiled_buffer_mib",
        "tiled_vertex_buffer_mib",
        "tiled_primitive_block_mib",
        "dxmt_tvb_pressure_proxy_mib",
        "dxmt_tvb_named_to_proxy_ratio",
        "dxmt_vs_buffer_write_to_tvb_proxy_ratio",
        "primitive_block_tile_intersections_pct",
        "tiling_block_utilization_pct",
        "primitives_per_tile",
        "dxmt_backend_storage_class",
        "dxmt_backend_probe_hint",
        "dxmt_cpu_writer_mib",
        "dxmt_argbuf_table_bytes",
        "dxmt_argbuf_cbuf_bytes",
        "dxmt_set_vertex_bytes_bytes",
        "dxmt_transient_vertex_bytes",
        "dxmt_transient_index_bytes",
        "dxmt_draw_calls",
        "dxmt_stream_handle_changes",
        "dxmt_stream_offset_changes",
        "dxmt_stream_stride_changes",
        "dxmt_ib_handle_changes",
        "dxmt_stream_unique_handles",
        "dxmt_ib_unique_handles",
        "dxmt_stream_unique_bytes",
        "dxmt_ib_unique_bytes",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerow({
            "seq": 50,
            "enc": 2,
            "gpu_ms": 20.0,
            "gpu_share_pct": 70.0,
            "buffer_write_mib": 1000.0,
            "vs_buffer_write_mib": 950.0,
            "dxmt_hidden_backend_write_mib": 999.0,
            "dxmt_named_tiled_buffer_mib": 20.0,
            "tiled_vertex_buffer_mib": 12.0,
            "tiled_primitive_block_mib": 8.0,
            "dxmt_tvb_pressure_proxy_mib": 100.0,
            "dxmt_tvb_named_to_proxy_ratio": 0.2,
            "dxmt_vs_buffer_write_to_tvb_proxy_ratio": 9.5,
            "primitive_block_tile_intersections_pct": 0.5,
            "tiling_block_utilization_pct": 0.3,
            "primitives_per_tile": 300.0,
            "dxmt_backend_storage_class": "hidden_vertex_tiler_parameter_storage",
            "dxmt_backend_probe_hint": "primitive-backend-pressure-or-state-shape-ab",
            "dxmt_cpu_writer_mib": 0.5,
            "dxmt_argbuf_table_bytes": 1024,
            "dxmt_argbuf_cbuf_bytes": 2048,
            "dxmt_set_vertex_bytes_bytes": 512,
            "dxmt_transient_vertex_bytes": 0,
            "dxmt_transient_index_bytes": 0,
            "dxmt_draw_calls": 100,
            "dxmt_stream_handle_changes": 120,
            "dxmt_stream_offset_changes": 40,
            "dxmt_stream_stride_changes": 5,
            "dxmt_ib_handle_changes": 80,
            "dxmt_stream_unique_handles": 12,
            "dxmt_ib_unique_handles": 9,
            "dxmt_stream_unique_bytes": 1048576,
            "dxmt_ib_unique_bytes": 524288,
        })
        writer.writerow({
            "seq": 50,
            "enc": 1,
            "gpu_ms": 5.0,
            "gpu_share_pct": 10.0,
            "buffer_write_mib": 100.0,
            "vs_buffer_write_mib": 10.0,
            "dxmt_hidden_backend_write_mib": 20.0,
            "dxmt_named_tiled_buffer_mib": 5.0,
            "tiled_vertex_buffer_mib": 3.0,
            "tiled_primitive_block_mib": 2.0,
            "dxmt_tvb_pressure_proxy_mib": 5.0,
            "dxmt_tvb_named_to_proxy_ratio": 1.0,
            "dxmt_vs_buffer_write_to_tvb_proxy_ratio": 2.0,
            "primitive_block_tile_intersections_pct": 0.1,
            "tiling_block_utilization_pct": 0.1,
            "primitives_per_tile": 10.0,
            "dxmt_backend_storage_class": "explicit_dxmt_writer",
            "dxmt_backend_probe_hint": "reduce-explicit-writer-bytes",
            "dxmt_cpu_writer_mib": 80.0,
            "dxmt_argbuf_table_bytes": 1048576,
            "dxmt_argbuf_cbuf_bytes": 83886080,
            "dxmt_set_vertex_bytes_bytes": 0,
            "dxmt_transient_vertex_bytes": 0,
            "dxmt_transient_index_bytes": 0,
            "dxmt_draw_calls": 10,
            "dxmt_stream_handle_changes": 1,
            "dxmt_stream_offset_changes": 0,
            "dxmt_stream_stride_changes": 0,
            "dxmt_ib_handle_changes": 1,
            "dxmt_stream_unique_handles": 1,
            "dxmt_ib_unique_handles": 1,
            "dxmt_stream_unique_bytes": 4096,
            "dxmt_ib_unique_bytes": 4096,
        })


class AnalyzeXcodeDxmtEncoderAttributionTests(unittest.TestCase):
    def test_report_classifies_hidden_backend_and_writer_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            joined = root / "joined.csv"
            output = root / "reports" / "attribution.md"
            csv_output = root / "reports" / "attribution.csv"
            write_joined(joined)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(joined),
                    "--output",
                    str(output),
                    "--csv-output",
                    str(csv_output),
                    "--top",
                    "2",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            report = output.read_text(encoding="utf-8")
            self.assertIn("Xcode/dxmt Encoder Attribution", report)
            self.assertIn("hidden-backend-primary-state-churn-secondary", report)
            self.assertIn("dxmt-writer-primary", report)
            self.assertIn("use TVB/tiler/backend probes", report)
            self.assertIn("Backend Mechanism Signals", report)
            self.assertIn("hidden-expanded-tvb-parameter-storage", report)
            self.assertIn("primitive-backend-pressure-or-state-shape-ab", report)
            self.assertIn("Mechanism Experiment Queue", report)
            self.assertIn("--require-tvb-mechanism-proof", report)
            self.assertIn("--target-row-key 50/2", report)
            self.assertIn("<primitive-order-preserving-backend-mechanism-option>", report)

            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["row"], "50/2")
            self.assertEqual(
                rows[0]["classification"],
                "hidden-backend-primary-state-churn-secondary",
            )
            self.assertEqual(
                rows[0]["mechanism_hint"],
                "hidden-expanded-tvb-parameter-storage",
            )
            self.assertEqual(rows[0]["backend_storage_class"],
                             "hidden_vertex_tiler_parameter_storage")
            self.assertEqual(float(rows[0]["tvb_named_to_proxy_ratio"]), 0.2)
            self.assertEqual(rows[1]["classification"], "dxmt-writer-primary")
            self.assertEqual(float(rows[0]["state_churn_events_per_draw"]), 2.45)

    def test_rejects_nonpositive_top(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            joined = root / "joined.csv"
            write_joined(joined)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(joined),
                    "--output",
                    str(root / "attribution.md"),
                    "--top",
                    "0",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--top must be positive", result.stderr)


if __name__ == "__main__":
    unittest.main()
