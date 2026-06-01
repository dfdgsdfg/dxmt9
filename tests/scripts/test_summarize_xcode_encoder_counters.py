#!/usr/bin/env python3
"""Regression tests for Xcode encoder counter summarization."""

from __future__ import annotations

import csv
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_xcode_encoder_counters.py"
MIB = 1024 * 1024


def load_xcode_summarizer():
    spec = importlib.util.spec_from_file_location("summarize_xcode_encoder_counters", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class SummarizeXcodeEncoderCountersTests(unittest.TestCase):
    def run_pso_attribution_fixture(self, pso_state_samples: int) -> subprocess.CompletedProcess[str]:
        root = Path(self.tmpdir.name)
        xcode_csv = root / "frame60-counters-xcode.csv"
        dxmt_csv = root / "3dmark05-perf-encoders.csv"
        joined_csv = root / "joined.csv"
        summary_csv = root / "summary.csv"
        report = root / "report.md"

        with xcode_csv.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=[
                "Index",
                "Encoder Label",
                "GPU Time",
                "Buffer Device Memory Bytes Written",
            ])
            writer.writeheader()
            writer.writerow({
                "Index": 0,
                "Encoder Label": "RenderPass[seq=1,enc=2,rt=0x10,depth=0x20]",
                "GPU Time": 10_000_000,
                "Buffer Device Memory Bytes Written": 256 * MIB,
            })

        with dxmt_csv.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=[
                "seq",
                "encoder",
                "draw_calls",
                "pso_state_samples",
            ])
            writer.writeheader()
            writer.writerow({
                "seq": 1,
                "encoder": 2,
                "draw_calls": 10,
                "pso_state_samples": pso_state_samples,
            })

        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                str(xcode_csv),
                "--dxmt-encoders-csv",
                str(dxmt_csv),
                "--summary-output",
                str(summary_csv),
                "--joined-output",
                str(joined_csv),
                "--report-output",
                str(report),
                "--require-top-pso-attribution",
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_joined_rows_classify_gpu_side_vs_buffer_writes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            xcode_csv = root / "frame60-counters-xcode.csv"
            dxmt_csv = root / "3dmark05-perf-encoders.csv"
            stream_csv = root / "3dmark05-perf-encoder-streams.csv"
            joined_csv = root / "joined.csv"
            summary_csv = root / "summary.csv"
            report = root / "report.md"

            xcode_fields = [
                "Index",
                "Encoder Index",
                "CommandBuffer Index",
                "CommandBuffer Label",
                "Encoder Label",
                "GPU Time",
                "Bytes Written To Device Memory",
                "Buffer Device Memory Bytes Written",
                "VS Bytes Written To Device Memory",
                "VS Buffer Device Memory Bytes Written",
                "VS Invocations",
                "FS Invocations",
                "Primitives",
                "Post Clipped Primitives",
                "Pixels Rasterized",
                "FS Tiles Processed",
                "Primitives Per Tile",
            ]
            with xcode_csv.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=xcode_fields)
                writer.writeheader()
                writer.writerow({
                    "Index": 0,
                    "Encoder Index": 0,
                    "CommandBuffer Index": 0,
                    "CommandBuffer Label": "cb_seq_1",
                    "Encoder Label": "RenderPass[seq=1,enc=2,rt=0x10,depth=0x20]",
                    "GPU Time": 10_000_000,
                    "Bytes Written To Device Memory": 300 * MIB,
                    "Buffer Device Memory Bytes Written": 256 * MIB,
                    "VS Bytes Written To Device Memory": 250 * MIB,
                    "VS Buffer Device Memory Bytes Written": 250 * MIB,
                    "VS Invocations": 1000,
                    "FS Invocations": 1000,
                    "Primitives": 500,
                    "Post Clipped Primitives": 400,
                    "Pixels Rasterized": 2000,
                    "FS Tiles Processed": 20,
                    "Primitives Per Tile": 10,
                })

            dxmt_fields = [
                "seq",
                "encoder",
                "draw_calls",
                "cull_none_draws",
                "cull_front_draws",
                "cull_back_draws",
                "depth_enabled_draws",
                "depth_write_draws",
                "scissor_enabled_draws",
                "alpha_blend_enabled_draws",
                "alpha_test_enabled_draws",
                "clip_plane_enabled_draws",
                "vertex_count",
                "pso_state_samples",
                "stream0_stride_min",
                "stream0_stride_max",
                "stream_handle_changes",
                "ib_handle_changes",
                "argbuf_table_bytes",
                "argbuf_cbuf_bytes",
                "set_vertex_bytes_bytes",
                "transient_vertex_bytes",
                "transient_index_bytes",
                "vsout_layout_last",
            ]
            with dxmt_csv.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=dxmt_fields)
                writer.writeheader()
                writer.writerow({
                    "seq": 1,
                    "encoder": 2,
                    "draw_calls": 10,
                    "cull_none_draws": 2,
                    "cull_front_draws": 0,
                    "cull_back_draws": 8,
                    "depth_enabled_draws": 10,
                    "depth_write_draws": 8,
                    "scissor_enabled_draws": 4,
                    "alpha_blend_enabled_draws": 3,
                    "alpha_test_enabled_draws": 1,
                    "clip_plane_enabled_draws": 0,
                    "vertex_count": 1000,
                    "pso_state_samples": 10,
                    "stream0_stride_min": 32,
                    "stream0_stride_max": 32,
                    "stream_handle_changes": 1,
                    "ib_handle_changes": 1,
                    "argbuf_table_bytes": 128 * 1024,
                    "argbuf_cbuf_bytes": 512 * 1024,
                    "set_vertex_bytes_bytes": 160,
                    "transient_vertex_bytes": 0,
                    "transient_index_bytes": 0,
                    "vsout_layout_last": 0x100,
                })
            with stream_csv.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=[
                    "seq",
                    "encoder",
                    "stream",
                    "samples",
                    "metal_binds",
                    "metal_bind_firsts",
                    "metal_bind_handle_changes",
                    "metal_bind_offset_changes",
                    "unique_handles",
                    "unique_bytes",
                    "handle_changes",
                    "offset_changes",
                    "stride_changes",
                    "last_handle",
                    "last_offset",
                    "last_stride",
                ])
                writer.writeheader()
                writer.writerow({
                    "seq": 1,
                    "encoder": 2,
                    "stream": 0,
                    "samples": 10,
                    "metal_binds": 2,
                    "metal_bind_firsts": 1,
                    "metal_bind_handle_changes": 1,
                    "metal_bind_offset_changes": 0,
                    "unique_handles": 1,
                    "unique_bytes": 4096,
                    "handle_changes": 1,
                    "offset_changes": 2,
                    "stride_changes": 0,
                    "last_handle": "0xabc",
                    "last_offset": 32,
                    "last_stride": 48,
                })

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(xcode_csv),
                    "--dxmt-encoders-csv",
                    str(dxmt_csv),
                    "--dxmt-streams-csv",
                    str(stream_csv),
                    "--summary-output",
                    str(summary_csv),
                    "--joined-output",
                    str(joined_csv),
                    "--report-output",
                    str(report),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with joined_csv.open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))

            self.assertEqual(row["dxmt_gpu_write_hint"], "gpu_vs_buffer_write")
            self.assertEqual(row["dxmt_write_owner_confidence"], "high")
            self.assertEqual(row["dxmt_cull_none_draws"], "2")
            self.assertEqual(row["dxmt_cull_back_draws"], "8")
            self.assertEqual(row["dxmt_depth_enabled_draws"], "10")
            self.assertEqual(row["dxmt_depth_write_draws"], "8")
            self.assertEqual(row["dxmt_scissor_enabled_draws"], "4")
            self.assertAlmostEqual(float(row["dxmt_vs_buffer_write_share"]), 250 / 256)
            self.assertGreater(float(row["dxmt_unexplained_buffer_write_ratio"]), 0.99)
            self.assertAlmostEqual(
                float(row["vs_buffer_bytes_per_post_clipped_primitive"]),
                250 * MIB / 400,
            )
            self.assertAlmostEqual(
                float(row["vs_buffer_bytes_per_primitive_tile_estimate"]),
                250 * MIB / 200,
            )
            self.assertEqual(row["dxmt_vsout_expected_stage_out_bytes_per_vertex"], "32")
            report_text = report.read_text(encoding="utf-8")
            self.assertIn("unexplained Xcode buffer write", report_text)
            self.assertIn("VS buffer bytes / post-clipped primitive", report_text)
            self.assertIn("dxmt cull none/front/back draws", report_text)
            self.assertIn("## DXMT Per-Stream Breakdown", report_text)
            self.assertIn("0xabc/32/48", report_text)

    def test_report_emits_hot_set_when_top_three_miss_gpu_share_target(self) -> None:
        summarizer = load_xcode_summarizer()
        rows = []
        for enc, gpu_ms in enumerate([40.0, 25.0, 15.0, 18.0, 2.0]):
            rows.append({
                "seq": 60,
                "enc": enc,
                "xcode_index": enc,
                "gpu_ms": gpu_ms,
                "buffer_write_mib": 100.0 + enc,
                "vs_buffer_write_mib": 99.0 + enc,
                "vs_invocations": 1000.0 + enc,
                "dxmt_indexed_vertex_reference_count": 2000 + enc,
                "dxmt_indexed_unique_vertex_estimate": 1000 + enc,
                "dxmt_indexed_vertex_cache_miss_estimate_64": 1200 + enc,
                "dxmt_vsout_expected_stage_out_bytes_per_vertex": 184,
                "dxmt_named_tiled_buffer_mib": 1.0,
                "dxmt_hidden_backend_write_mib": 98.0 + enc,
                "dxmt_cpu_writer_bytes": 1024,
                "dxmt_gpu_write_hint": "gpu_vs_buffer_write",
                "dxmt_backend_storage_class": "hidden_vertex_tiler_parameter_storage",
                "dxmt_backend_probe_hint": "primitive-backend-pressure-or-state-shape-ab",
            })

        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.md"
            summarizer.write_report(
                report,
                rows,
                "hot-fixture",
                hot_gpu_share_target=95.0,
            )
            text = report.read_text(encoding="utf-8")

        self.assertIn("- Top 3 GPU share: `80.00%`", text)
        self.assertIn("- Hot set GPU share: `98.00%` (`top 4`, target `95.00%`)", text)
        self.assertIn("## Hot Set Aggregate", text)
        self.assertIn("| Encoders | `60/0, 60/1, 60/2, 60/3` |", text)
        self.assertIn("| VS invocations / cache miss 64 |", text)
        self.assertIn(
            "use the Hot Set Aggregate for whole-frame bottleneck conclusions",
            text,
        )

    def test_zero_vsout_layout_still_counts_position(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            xcode_csv = root / "frame60-counters-xcode.csv"
            dxmt_csv = root / "3dmark05-perf-encoders.csv"
            joined_csv = root / "joined.csv"
            summary_csv = root / "summary.csv"
            report = root / "report.md"

            with xcode_csv.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=[
                    "Index",
                    "Encoder Label",
                    "GPU Time",
                    "Buffer Device Memory Bytes Written",
                    "VS Buffer Device Memory Bytes Written",
                    "VS Invocations",
                ])
                writer.writeheader()
                writer.writerow({
                    "Index": 0,
                    "Encoder Label": "RenderPass[seq=1,enc=2,rt=0x10,depth=0x20]",
                    "GPU Time": 10_000_000,
                    "Buffer Device Memory Bytes Written": 256 * MIB,
                    "VS Buffer Device Memory Bytes Written": 16 * MIB,
                    "VS Invocations": 1024,
                })

            with dxmt_csv.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=[
                    "seq",
                    "encoder",
                    "draw_calls",
                    "pso_state_samples",
                    "vsout_layout_last",
                ])
                writer.writeheader()
                writer.writerow({
                    "seq": 1,
                    "encoder": 2,
                    "draw_calls": 1,
                    "pso_state_samples": 1,
                    "vsout_layout_last": "0x0",
                })

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(xcode_csv),
                    "--dxmt-encoders-csv",
                    str(dxmt_csv),
                    "--summary-output",
                    str(summary_csv),
                    "--joined-output",
                    str(joined_csv),
                    "--report-output",
                    str(report),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with joined_csv.open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["dxmt_vsout_expected_stage_out_bytes_per_vertex"], "16")
            self.assertAlmostEqual(
                float(row["dxmt_vs_buffer_to_expected_stage_out_ratio"]),
                (16 * MIB / 1024) / 16,
            )

    def test_top_pso_attribution_gate_accepts_current_source_coverage(self) -> None:
        result = self.run_pso_attribution_fixture(pso_state_samples=10)

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_top_pso_attribution_gate_rejects_old_logs(self) -> None:
        result = self.run_pso_attribution_fixture(pso_state_samples=0)

        self.assertEqual(result.returncode, 1)
        self.assertIn("top encoder PSO/VSOut attribution coverage is too low",
                      result.stderr)


if __name__ == "__main__":
    unittest.main()
