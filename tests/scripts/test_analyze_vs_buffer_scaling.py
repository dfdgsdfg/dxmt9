#!/usr/bin/env python3
"""Regression tests for VS buffer scaling analysis."""

from __future__ import annotations

import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_vs_buffer_scaling.py"


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_vs_buffer_scaling", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class AnalyzeVsBufferScalingTests(unittest.TestCase):
    def test_aggregate_and_report_classify_unexplained_vs_buffer_writes(self) -> None:
        module = load_module()
        rows = [
            {
                "run": "fixture",
                "gpu_ms": "10",
                "vs_buffer_write_mib": "100",
                "buffer_write_mib": "101",
                "vs_invocations": "1000",
                "primitives": "500",
                "post_clipped_primitives": "500",
                "pixels_rasterized": "2000",
                "fs_invocations": "1800",
                "dxmt_vertex_count": "1000",
                "dxmt_vsout_expected_stage_out_bytes_per_vertex": "40",
                "dxmt_stream0_input_max_mib": "1",
                "tiled_vertex_buffer_mib": "2",
                "tiled_primitive_block_mib": "1",
                "dxmt_cpu_writer_mib": "0.25",
                "dxmt_stream_handle_changes": "4",
                "dxmt_stream_offset_changes": "1",
                "dxmt_stream_stride_changes": "0",
                "dxmt_ib_handle_changes": "3",
                "dxmt_draw_calls": "10",
                "vs_buffer_write_limiter_pct": "20",
                "vertex_stage_time_pct": "95",
                "vs_alu_limiter_pct": "3",
                "dxmt_cull_back_draws": "10",
                "dxmt_depth_enabled_draws": "10",
                "dxmt_depth_write_draws": "0",
                "dxmt_scissor_enabled_draws": "5",
                "dxmt_alpha_blend_enabled_draws": "5",
                "dxmt_textured_draws": "10",
                "dxmt_ffp_draws": "0",
                "dxmt_pretransformed_draws": "0",
            },
            {
                "run": "fixture",
                "gpu_ms": "5",
                "vs_buffer_write_mib": "50",
                "buffer_write_mib": "51",
                "vs_invocations": "500",
                "primitives": "250",
                "post_clipped_primitives": "250",
                "pixels_rasterized": "1000",
                "fs_invocations": "900",
                "dxmt_vertex_count": "500",
                "dxmt_vsout_expected_stage_out_bytes_per_vertex": "40",
                "dxmt_stream0_input_max_mib": "0.5",
                "tiled_vertex_buffer_mib": "1",
                "tiled_primitive_block_mib": "0.5",
                "dxmt_cpu_writer_mib": "0.125",
                "dxmt_stream_handle_changes": "2",
                "dxmt_stream_offset_changes": "0",
                "dxmt_stream_stride_changes": "0",
                "dxmt_ib_handle_changes": "2",
                "dxmt_draw_calls": "5",
                "vs_buffer_write_limiter_pct": "10",
                "vertex_stage_time_pct": "90",
                "vs_alu_limiter_pct": "2",
                "dxmt_cull_back_draws": "5",
                "dxmt_depth_enabled_draws": "5",
                "dxmt_depth_write_draws": "0",
                "dxmt_scissor_enabled_draws": "2",
                "dxmt_alpha_blend_enabled_draws": "2",
                "dxmt_textured_draws": "5",
                "dxmt_ffp_draws": "0",
                "dxmt_pretransformed_draws": "0",
            },
        ]

        aggregate = module.aggregate(rows, 2)
        self.assertEqual(aggregate["run"], "fixture")
        self.assertAlmostEqual(aggregate["vs_buffer_mib"], 150.0)
        self.assertAlmostEqual(aggregate["cpu_writer_mib"], 0.375)
        self.assertGreater(aggregate["vsout_ratio"], 1000.0)
        self.assertAlmostEqual(aggregate["tiled_ratio"], 33.333333, places=4)

        correlations = module.correlation_rows(rows)
        self.assertEqual(correlations[0]["pearson_r"], 1.0)
        shapes = module.shape_rows(rows)
        self.assertEqual(len(shapes), 1)
        self.assertIn("cull=back", shapes[0]["shape"])
        self.assertIn("depth=read", shapes[0]["shape"])
        self.assertAlmostEqual(shapes[0]["vs_buffer_mib"], 150.0)

        backend_candidate = dict(aggregate)
        backend_candidate["run"] = "half-vsout-fixture"
        backend_candidate["gpu_ms"] = aggregate["gpu_ms"] * 1.03
        backend_candidate["vs_buffer_mib"] = aggregate["vs_buffer_mib"] * 0.98
        backend_candidate["vs_buffer_write_mib"] = backend_candidate["vs_buffer_mib"]
        backend_candidate["vs_b_per_vs_invocation"] = aggregate["vs_b_per_vs_invocation"] * 0.98
        backend_candidate["top_row_keys"] = aggregate["top_row_keys"]
        backend_deltas = module.baseline_deltas([aggregate, backend_candidate], aggregate, 8)
        self.assertEqual(len(backend_deltas), 1)
        self.assertEqual(backend_deltas[0]["candidate_kind"], "non-reorder-backend-shape")
        self.assertEqual(backend_deltas[0]["backend_shape_gate"], "reject")
        self.assertIn("bytes/inv reduction < 5%", backend_deltas[0]["backend_shape_reason"])
        self.assertIn("GPU did not improve by >= 2%", backend_deltas[0]["backend_shape_reason"])
        self.assertAlmostEqual(backend_deltas[0]["vs_write_delta_mib"], -3.0, places=6)
        self.assertAlmostEqual(backend_deltas[0]["invocation_effect_mib"], 0.0, places=6)
        self.assertAlmostEqual(backend_deltas[0]["bytes_per_invocation_effect_mib"], -3.0, places=6)
        self.assertAlmostEqual(backend_deltas[0]["residual_mib"], 0.0, places=6)
        self.assertEqual(backend_deltas[0]["primary_mover"], "bytes_per_invocation")

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report = root / "report.md"
            aggregate_csv = root / "aggregate.csv"
            delta_csv = root / "deltas.csv"
            module.write_summary(
                report,
                [aggregate, backend_candidate],
                correlations,
                correlations,
                shapes,
                backend_deltas,
                aggregate["run"],
            )
            module.write_aggregate_csv(aggregate_csv, [aggregate, backend_candidate])
            module.write_delta_csv(delta_csv, backend_deltas)
            text = report.read_text(encoding="utf-8")
            self.assertIn("hidden Apple GPU vertex-stage", text)
            self.assertIn("## Non-Reorder Backend-Shape Gate", text)
            self.assertIn("bytes/inv reduction < 5%", text)
            self.assertIn("## VS Write Delta Attribution", text)
            self.assertIn("bytes_per_invocation", text)
            self.assertIn("## Render-State Shape Split", text)
            with aggregate_csv.open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["run"], "fixture")
            self.assertEqual(row["vs_buffer_mib"], "150.0")
            self.assertEqual(row["vs_invocations"], "1500.0")
            self.assertEqual(row["dxmt_vertex_count"], "1500.0")
            self.assertEqual(row["draw_calls"], "15.0")
            with delta_csv.open(newline="", encoding="utf-8") as handle:
                delta_row = next(csv.DictReader(handle))
            self.assertEqual(delta_row["run"], "half-vsout-fixture")
            self.assertEqual(delta_row["primary_mover"], "bytes_per_invocation")
            self.assertEqual(delta_row["backend_shape_gate"], "reject")


if __name__ == "__main__":
    unittest.main()
