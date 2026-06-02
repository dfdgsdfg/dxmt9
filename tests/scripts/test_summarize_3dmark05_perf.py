#!/usr/bin/env python3
"""Regression tests for 3DMark05 perf summary parsing."""

from __future__ import annotations

import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_3dmark05_perf.py"


def load_module():
    spec = importlib.util.spec_from_file_location("summarize_3dmark05_perf", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class Summarize3DMark05PerfTests(unittest.TestCase):
    def test_encoder_breakdown_fields_are_parsed_and_written(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            log_path = temp_path / "dxmt9.log"
            log_path.write_text(
                "[dxmt9-perf-encoder seq=7 encoder=3 rt=0x10 depth=0x20 "
                "draw_calls=4 pso_state_samples=8 "
                "cull_none_draws=1 cull_front_draws=0 cull_back_draws=3 "
                "fill_solid_draws=4 fill_wireframe_draws=0 "
                "depth_enabled_draws=4 depth_write_draws=3 "
                "depth_func_less_draws=1 depth_func_lessequal_draws=2 "
                "depth_func_always_draws=1 depth_func_other_draws=0 "
                "scissor_enabled_draws=2 alpha_blend_enabled_draws=1 "
                "alpha_test_enabled_draws=1 clip_plane_enabled_draws=0 "
                "stream_metal_binds=5 stream_metal_bind_firsts=1 "
                "stream_metal_bind_handle_changes=2 "
                "stream_metal_bind_offset_changes=1 "
                "stream_unique_handles=3 stream_unique_bytes=65536 "
                "stream_handle_changes=2 stream_offset_changes=3 "
                "stream_stride_changes=1 stream0_last_handle=0xabc "
                "stream0_last_offset=64 stream0_last_stride=32 "
                "ib_metal_binds=4 ib_handle_changes=1 "
                "ib_unique_handles=2 ib_unique_bytes=4096 ib_last_handle=0xdef "
                "argbuf_table_bytes=4096 argbuf_cbuf_bytes=8192 "
                "argbuf_cbuf_vs_bytes=1024 argbuf_cbuf_ffp_vs_bytes=2048 "
                "argbuf_cbuf_ps_bytes=512 argbuf_cbuf_ffp_ps_bytes=256 "
                "set_vertex_bytes_calls=4 set_vertex_bytes_bytes=96 "
                "set_vertex_bytes_slot5_calls=3 set_vertex_bytes_slot5_bytes=72 "
                "set_vertex_bytes_other_calls=1 set_vertex_bytes_other_bytes=24 "
                "transient_vertex_bytes=128 transient_vertex_user_bytes=32 "
                "transient_vertex_preupload_bytes=16 "
                "transient_vertex_decl_fallback_bytes=8 "
                "transient_vertex_expanded_main_bytes=64 "
                "transient_vertex_expanded_extra_bytes=8 "
                "transient_index_bytes=64 transient_index_user_bytes=24 "
                "transient_index_preupload_bytes=32 "
                "transient_index_shadow_fallback_bytes=8 "
                "indexed_order_optimized_draws=1 "
                "indexed_order_optimized_bytes=1234 "
                "transient_index_optimized_order_bytes=1234]\n"
                "[dxmt9-perf-encoder-stream seq=7 encoder=3 stream=0 "
                "samples=4 metal_binds=5 metal_bind_firsts=1 "
                "metal_bind_handle_changes=2 metal_bind_offset_changes=1 "
                "unique_handles=3 unique_bytes=65536 "
                "handle_changes=2 offset_changes=3 stride_changes=1 "
                "last_handle=0xabc last_offset=64 last_stride=32]\n"
                "[dxmt9-perf-indexed-probe-draw seq=7 encoder=3 "
                "encoder_draw_index=2 draw_ordinal=42 eligible=1 applied=1 "
                "optimized_eligible=1 optimized_applied=0 reorder_bytes=1234 "
                "primitive_type=4 primitive_count=4096 "
                "vertex_count=12288 texture_mask=0x7f color_write=0xf "
                "alpha_blend=1 src_blend=5 dst_blend=6 blend_op=1 "
                "separate_alpha=0 src_blend_alpha=5 dst_blend_alpha=6 "
                "blend_op_alpha=1 "
                "alpha_test=0 depth_enabled=1 depth_write=0 depth_func=4 "
                "stencil=0 clip_plane=0 scissor=1 scissor_l=16 scissor_t=32 "
                "scissor_r=512 scissor_b=384 cull=2 fill=0 base_vertex=0 "
                "start_index=128 index_type=1 index_buffer=0xdef "
                "stream0_handle=0xabc stream0_offset=64 stream0_stride=24 "
                "pso=0x111 shader_variant=0x222 vs=0x333 ps=0x444 "
                "vsout=0xfff]\n",
                encoding="utf-8",
            )

            encoders, streams = module.parse_encoder_lines(log_path)
            probe_draws = module.parse_probe_draw_lines(log_path)
            module.enrich_encoder_rows(encoders)

            self.assertEqual(len(encoders), 1)
            self.assertEqual(len(streams), 1)
            self.assertEqual(len(probe_draws), 1)
            encoder = encoders[0]
            self.assertEqual(encoder["stream_metal_binds"], 5)
            self.assertEqual(encoder["cull_none_draws"], 1)
            self.assertEqual(encoder["cull_back_draws"], 3)
            self.assertEqual(encoder["depth_enabled_draws"], 4)
            self.assertEqual(encoder["depth_write_draws"], 3)
            self.assertEqual(encoder["depth_func_lessequal_draws"], 2)
            self.assertEqual(encoder["scissor_enabled_draws"], 2)
            self.assertEqual(encoder["alpha_blend_enabled_draws"], 1)
            self.assertEqual(encoder["stream_metal_bind_firsts"], 1)
            self.assertEqual(encoder["stream_metal_bind_handle_changes"], 2)
            self.assertEqual(encoder["stream_metal_bind_offset_changes"], 1)
            self.assertEqual(encoder["stream_unique_handles"], 3)
            self.assertEqual(encoder["stream_unique_bytes"], 65536)
            self.assertEqual(encoder["stream_handle_changes"], 2)
            self.assertEqual(encoder["stream_offset_changes"], 3)
            self.assertEqual(encoder["stream_stride_changes"], 1)
            self.assertEqual(encoder["ib_metal_binds"], 4)
            self.assertEqual(encoder["ib_unique_handles"], 2)
            self.assertEqual(encoder["ib_unique_bytes"], 4096)
            self.assertEqual(encoder["ib_last_handle"], "0xdef")
            self.assertEqual(encoder["argbuf_table_bytes"], 4096)
            self.assertEqual(encoder["argbuf_cbuf_bytes"], 8192)
            self.assertEqual(encoder["argbuf_cbuf_vs_bytes"], 1024)
            self.assertEqual(encoder["argbuf_cbuf_ffp_vs_bytes"], 2048)
            self.assertEqual(encoder["argbuf_cbuf_ps_bytes"], 512)
            self.assertEqual(encoder["argbuf_cbuf_ffp_ps_bytes"], 256)
            self.assertEqual(encoder["set_vertex_bytes_calls"], 4)
            self.assertEqual(encoder["set_vertex_bytes_bytes"], 96)
            self.assertEqual(encoder["set_vertex_bytes_slot5_calls"], 3)
            self.assertEqual(encoder["set_vertex_bytes_slot5_bytes"], 72)
            self.assertEqual(encoder["set_vertex_bytes_other_calls"], 1)
            self.assertEqual(encoder["set_vertex_bytes_other_bytes"], 24)
            self.assertEqual(encoder["transient_vertex_bytes"], 128)
            self.assertEqual(encoder["transient_vertex_user_bytes"], 32)
            self.assertEqual(encoder["transient_vertex_preupload_bytes"], 16)
            self.assertEqual(encoder["transient_vertex_decl_fallback_bytes"], 8)
            self.assertEqual(encoder["transient_vertex_expanded_main_bytes"], 64)
            self.assertEqual(encoder["transient_vertex_expanded_extra_bytes"], 8)
            self.assertEqual(encoder["transient_index_bytes"], 64)
            self.assertEqual(encoder["transient_index_user_bytes"], 24)
            self.assertEqual(encoder["transient_index_preupload_bytes"], 32)
            self.assertEqual(encoder["transient_index_shadow_fallback_bytes"], 8)
            self.assertEqual(encoder["indexed_order_optimized_draws"], 1)
            self.assertEqual(encoder["indexed_order_optimized_bytes"], 1234)
            self.assertEqual(encoder["transient_index_optimized_order_bytes"], 1234)
            self.assertEqual(encoder["pso_state_samples_per_draw"], 2.0)

            stream = streams[0]
            self.assertEqual(stream["metal_binds"], 5)
            self.assertEqual(stream["metal_bind_firsts"], 1)
            self.assertEqual(stream["metal_bind_handle_changes"], 2)
            self.assertEqual(stream["metal_bind_offset_changes"], 1)
            self.assertEqual(stream["unique_handles"], 3)
            self.assertEqual(stream["unique_bytes"], 65536)
            self.assertEqual(stream["last_handle"], "0xabc")
            self.assertEqual(stream["last_offset"], 64)
            self.assertEqual(stream["last_stride"], 32)
            probe_draw = probe_draws[0]
            self.assertEqual(probe_draw["eligible"], 1)
            self.assertEqual(probe_draw["applied"], 1)
            self.assertEqual(probe_draw["optimized_eligible"], 1)
            self.assertEqual(probe_draw["optimized_applied"], 0)
            self.assertEqual(probe_draw["primitive_count"], 4096)
            self.assertEqual(probe_draw["src_blend"], 5)
            self.assertEqual(probe_draw["dst_blend"], 6)
            self.assertEqual(probe_draw["scissor_r"], 512)
            self.assertEqual(probe_draw["index_buffer"], "0xdef")
            self.assertEqual(probe_draw["stream0_handle"], "0xabc")

            encoder_csv = temp_path / "3dmark05-perf-encoders.csv"
            stream_csv = temp_path / "3dmark05-perf-encoder-streams.csv"
            probe_draw_csv = temp_path / "3dmark05-perf-indexed-probe-draws.csv"
            module.write_csv(encoder_csv, encoders, module.ENCODER_CSV_KEYS)
            module.write_csv(stream_csv, streams, module.STREAM_CSV_KEYS)
            module.write_csv(probe_draw_csv, probe_draws, module.PROBE_DRAW_CSV_KEYS)

            with encoder_csv.open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["stream_metal_binds"], "5")
            self.assertEqual(row["cull_none_draws"], "1")
            self.assertEqual(row["cull_back_draws"], "3")
            self.assertEqual(row["depth_enabled_draws"], "4")
            self.assertEqual(row["depth_write_draws"], "3")
            self.assertEqual(row["scissor_enabled_draws"], "2")
            self.assertEqual(row["stream_metal_bind_handle_changes"], "2")
            self.assertEqual(row["stream_unique_bytes"], "65536")
            self.assertEqual(row["stream_handle_changes"], "2")
            self.assertEqual(row["stream0_last_stride"], "32")
            self.assertEqual(row["ib_unique_bytes"], "4096")
            self.assertEqual(row["ib_last_handle"], "0xdef")
            self.assertEqual(row["argbuf_table_bytes"], "4096")
            self.assertEqual(row["argbuf_cbuf_bytes"], "8192")
            self.assertEqual(row["argbuf_cbuf_ffp_vs_bytes"], "2048")
            self.assertEqual(row["set_vertex_bytes_bytes"], "96")
            self.assertEqual(row["set_vertex_bytes_slot5_bytes"], "72")
            self.assertEqual(row["set_vertex_bytes_other_bytes"], "24")
            self.assertEqual(row["transient_vertex_preupload_bytes"], "16")
            self.assertEqual(row["transient_vertex_expanded_main_bytes"], "64")
            self.assertEqual(row["transient_index_bytes"], "64")
            self.assertEqual(row["transient_index_shadow_fallback_bytes"], "8")
            self.assertEqual(row["indexed_order_optimized_draws"], "1")
            self.assertEqual(row["transient_index_optimized_order_bytes"], "1234")

            with stream_csv.open(newline="", encoding="utf-8") as handle:
                stream_row = next(csv.DictReader(handle))
            self.assertEqual(stream_row["metal_binds"], "5")
            self.assertEqual(stream_row["metal_bind_offset_changes"], "1")
            self.assertEqual(stream_row["unique_bytes"], "65536")
            self.assertEqual(stream_row["handle_changes"], "2")
            self.assertEqual(stream_row["offset_changes"], "3")
            self.assertEqual(stream_row["stride_changes"], "1")

            with probe_draw_csv.open(newline="", encoding="utf-8") as handle:
                probe_row = next(csv.DictReader(handle))
            self.assertEqual(probe_row["applied"], "1")
            self.assertEqual(probe_row["optimized_applied"], "0")
            self.assertEqual(probe_row["reorder_bytes"], "1234")
            self.assertEqual(probe_row["src_blend"], "5")
            self.assertEqual(probe_row["scissor_l"], "16")
            self.assertEqual(probe_row["pso"], "0x111")

            summary_md = temp_path / "summary.md"
            module.write_markdown(
                summary_md,
                temp_path,
                {
                    "dxmt9_perf_counters": {
                        "commit_chunk_draw_run_break_state_delta": 100,
                        "commit_chunk_draw_run_break_state_delta_stream_only": 10,
                        "commit_chunk_draw_run_break_state_delta_mixed": 80,
                        "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_ib": 70,
                        "commit_chunk_draw_run_submits": 5,
                        "commit_chunk_draw_run_records": 20,
                        "commit_chunk_draw_batch_const_upload_passthrough": 12,
                        "commit_chunk_draw_submission_batch_submits": 4,
                        "commit_chunk_draw_submission_batch_records": 24,
                        "commit_chunk_draw_submission_batch_max_records": 9,
                        "commit_chunk_draw_submission_batch_size_1": 1,
                        "commit_chunk_draw_submission_batch_size_2": 1,
                        "commit_chunk_draw_submission_batch_size_3_4": 0,
                        "commit_chunk_draw_submission_batch_size_5_8": 1,
                        "commit_chunk_draw_submission_batch_size_9_16": 1,
                        "commit_chunk_draw_submission_batch_size_17_32": 0,
                        "commit_chunk_draw_submission_batch_size_33_plus": 0,
                    },
                },
                [],
                [],
                encoder_csv,
                stream_csv,
                probe_draws,
                probe_draw_csv,
            )
            summary = summary_md.read_text(encoding="utf-8")
            self.assertIn("## State-Delta Break Split", summary)
            self.assertIn("commit_chunk_draw_run_break_state_delta_mixed_pair_stream_ib", summary)
            self.assertIn("70.00%", summary)
            self.assertIn("## Draw Batching Derived", summary)
            self.assertIn("| `draw_run_records_per_submit` | `4.000` |", summary)
            self.assertIn("| `draw_submission_batch_records_per_submit` | `6.000` |", summary)
            self.assertIn("| `const_upload_passthrough_per_submission_batch` | `3.000` |", summary)
            self.assertIn("### Submission Batch Size Histogram", summary)
            self.assertIn("| `commit_chunk_draw_submission_batch_size_9_16` | `1` | `25.00%` |", summary)
            self.assertIn("- Indexed probe draw lines: `1`", summary)
            self.assertIn("## Indexed Probe Draw Samples", summary)
            self.assertIn("| `applied` | `1` |", summary)
            self.assertIn("| `optimized_eligible` | `1` |", summary)


if __name__ == "__main__":
    unittest.main()
