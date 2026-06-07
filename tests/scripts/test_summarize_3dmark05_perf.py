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
    def test_load_result_falls_back_to_existing_summary_tables(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            (temp_path / "3dmark05-perf-summary.md").write_text(
                "# 3DMark05 Perf Summary\n\n"
                "## Run Counters\n\n"
                "| Counter | Value |\n"
                "|---|---:|\n"
                "| `present_encoded` | `1,680` |\n"
                "| `map_buffer_total_ms` | `12.500` |\n"
                "| `draw_skipped_no_pipeline` | `0` |\n\n"
                "## Correctness / Visual-Coupling Counters\n\n"
                "| Scope | Counter | Value |\n"
                "|---|---|---:|\n"
                "| run | `present_encoded` | `wrong-table` |\n\n"
                "## Bridge Launch Check\n\n"
                "| Counter | Value |\n"
                "|---|---:|\n"
                "| `bridge_factory` | `53` |\n"
                "| `bridge_draw` | `0` |\n",
                encoding="utf-8",
            )

            result = module.load_result(temp_path)

            self.assertEqual(result["status"], "partial-summary")
            self.assertEqual(result["dxmt9_perf_counters"]["present_encoded"], 1680)
            self.assertEqual(result["dxmt9_perf_counters"]["map_buffer_total_ms"], 12.5)
            self.assertEqual(result["dxmt9_perf_counters"]["draw_skipped_no_pipeline"], 0)
            self.assertEqual(result["dxmt9_bridge_counters"]["bridge_factory"], 53)
            self.assertEqual(result["dxmt9_bridge_counters"]["bridge_draw"], 0)

    def test_render_pass_reentry_encoder_role_aggregation(self) -> None:
        module = load_module()
        encoders = [
            {
                "seq": 60,
                "encoder": 6,
                "draw_calls": 10,
                "depth_enabled_draws": 10,
                "depth_write_draws": 0,
                "textured_draws": 10,
                "alpha_blend_enabled_draws": 0,
                "blend_screen_draws": 0,
                "blend_additive_draws": 0,
                "blend_alpha_composite_draws": 0,
                "color0_included": 1,
                "color0_load_action": 1,
                "color0_store_action": 1,
                "color0_clear": 0,
                "color_load_bytes": 4096,
                "color_store_bytes": 4096,
                "depth_included": 1,
                "depth_load_action": 1,
                "depth_store_action": 1,
                "depth_clear": 0,
                "depth_load_bytes": 4096,
                "depth_store_bytes": 4096,
                "primitive_count": 2048,
            },
            {
                "seq": 60,
                "encoder": 7,
                "draw_calls": 12,
                "depth_enabled_draws": 12,
                "depth_write_draws": 12,
                "textured_draws": 0,
                "alpha_blend_enabled_draws": 0,
                "blend_screen_draws": 0,
                "blend_additive_draws": 0,
                "blend_alpha_composite_draws": 0,
                "color0_included": 1,
                "color0_load_action": 1,
                "color0_store_action": 1,
                "color0_clear": 0,
                "color_load_bytes": 1024,
                "color_store_bytes": 2048,
                "depth_included": 1,
                "depth_load_action": 1,
                "depth_store_action": 1,
                "depth_clear": 0,
                "depth_load_bytes": 1024,
                "depth_store_bytes": 2048,
                "primitive_count": 4096,
            },
            {
                "seq": 60,
                "encoder": 8,
                "draw_calls": 20,
                "depth_enabled_draws": 20,
                "depth_write_draws": 0,
                "textured_draws": 20,
                "alpha_blend_enabled_draws": 15,
                "blend_screen_draws": 10,
                "blend_additive_draws": 0,
                "blend_alpha_composite_draws": 5,
                "color0_included": 1,
                "color0_load_action": 1,
                "color0_store_action": 1,
                "color0_clear": 0,
                "color_load_bytes": 4096,
                "color_store_bytes": 4096,
                "depth_included": 1,
                "depth_load_action": 1,
                "depth_store_action": 0,
                "depth_clear": 0,
                "depth_load_bytes": 4096,
                "depth_store_bytes": 0,
                "primitive_count": 8192,
            },
        ]
        reentry_rows = [
            {
                "prior_a_seq": 60,
                "prior_a_encoder": 6,
                "last_seq": 60,
                "last_encoder": 8,
                "first_seq": 60,
                "first_encoder": 8,
                "last_b_seq": 60,
                "last_b_encoder": 7,
                "first_b_seq": 60,
                "first_b_encoder": 7,
                "count": 3,
                "preservation_bytes": 12288,
                "a_color_touch_distance": 1,
                "a_depth_touch_distance": 1,
                "b_color_touch_distance": 1,
                "b_depth_touch_distance": 1,
            },
        ]

        rows = module.aggregate_render_pass_reentry_encoder_roles(reentry_rows, encoders)

        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["prior_a_role"], "textured-depth-read-opaque")
        self.assertEqual(rows[0]["b_role"], "opaque-depth-write-untextured")
        self.assertEqual(rows[0]["a_role"], "screen-blend-depth-read")
        self.assertEqual(
            rows[0]["prior_a_pass_action"],
            "c0=load/store/no-clear/8,192B; d=load/store/no-clear/8,192B",
        )
        self.assertEqual(
            rows[0]["b_pass_action"],
            "c0=load/store/no-clear/3,072B; d=load/store/no-clear/3,072B",
        )
        self.assertEqual(
            rows[0]["a_pass_action"],
            "c0=load/store/no-clear/8,192B; d=load/dontcare/no-clear/4,096B",
        )
        self.assertEqual(rows[0]["count"], 3)
        self.assertEqual(rows[0]["preservation_bytes"], 12288)
        self.assertEqual(rows[0]["prior_a_draws_avg"], 10.0)
        self.assertEqual(rows[0]["b_draws_avg"], 12.0)
        self.assertEqual(rows[0]["a_draws_avg"], 20.0)
        self.assertEqual(rows[0]["prior_a_primitives_avg"], 2048.0)
        self.assertEqual(rows[0]["b_primitives_avg"], 4096.0)
        self.assertEqual(rows[0]["a_primitives_avg"], 8192.0)

    def test_encoder_breakdown_fields_are_parsed_and_written(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            log_path = temp_path / "dxmt9.log"
            log_path.write_text(
                "[dxmt9-perf-encoder seq=7 encoder=3 rt=0x10 depth=0x20 "
                "draw_calls=4 pso_state_samples=8 "
                "color_attachment_count=1 color0_included=1 "
                "color0_load_action=1 color0_store_action=1 color0_clear=0 "
                "color_load_bytes=1024 color_store_bytes=2048 "
                "depth_included=1 depth_load_action=2 depth_store_action=1 "
                "depth_clear=1 depth_load_bytes=0 depth_store_bytes=4096 "
                "stencil_included=0 stencil_load_action=0 stencil_store_action=0 "
                "stencil_clear=0 stencil_load_bytes=0 stencil_store_bytes=0 "
                "cull_none_draws=1 cull_front_draws=0 cull_back_draws=3 "
                "fill_solid_draws=4 fill_wireframe_draws=0 "
                "depth_enabled_draws=4 depth_write_draws=3 "
                "depth_func_less_draws=1 depth_func_lessequal_draws=2 "
                "depth_func_always_draws=1 depth_func_other_draws=0 "
                "scissor_enabled_draws=2 alpha_blend_enabled_draws=1 "
                "blend_screen_draws=1 blend_additive_draws=0 "
                "blend_alpha_composite_draws=0 "
                "alpha_blend_textured_draws=1 "
                "alpha_blend_textured_primitives=42 "
                "alpha_blend_textured_vertices=126 "
                "alpha_blend_small_draws=1 "
                "alpha_blend_small_primitives=42 "
                "alpha_blend_small_vertices=126 "
                "alpha_test_enabled_draws=1 clip_plane_enabled_draws=0 "
                "tile_ffp_routed_tile_draws=0 "
                "tile_ffp_routed_tile_primitives=0 "
                "tile_ffp_routed_tile_vertices=0 "
                "tile_ffp_routed_portable_draws=4 "
                "tile_ffp_routed_portable_primitives=4096 "
                "tile_ffp_routed_portable_vertices=12288 "
                "tile_ffp_eligible_draws=1 "
                "tile_ffp_eligible_primitives=1024 "
                "tile_ffp_eligible_vertices=3072 "
                "tile_ffp_fallback_gpu_family_draws=0 "
                "tile_ffp_fallback_gpu_family_primitives=0 "
                "tile_ffp_fallback_not_ffp_draws=2 "
                "tile_ffp_fallback_not_ffp_primitives=2048 "
                "tile_ffp_fallback_precision_draws=0 "
                "tile_ffp_fallback_precision_primitives=0 "
                "tile_ffp_fallback_unsupported_state_draws=1 "
                "tile_ffp_fallback_unsupported_state_primitives=1024 "
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
                "probe_scissor_rect_draws=1 "
                "probe_scissor_rect_area_delta_pixels=256 "
                "probe_force_texture_white_draws=2 "
                "probe_fragmentless_depth_only_draws=3 "
                "probe_fragmentless_depth_only_primitives=4096 "
                "probe_fragmentless_depth_only_vertices=12288 "
                "transient_index_optimized_order_bytes=1234]\n"
                "[dxmt9-perf-encoder-stream seq=7 encoder=3 stream=0 "
                "samples=4 metal_binds=5 metal_bind_firsts=1 "
                "metal_bind_handle_changes=2 metal_bind_offset_changes=1 "
                "unique_handles=3 unique_bytes=65536 "
                "handle_changes=2 offset_changes=3 stride_changes=1 "
                "last_handle=0xabc last_offset=64 last_stride=32]\n"
                "[dxmt9-perf-indexed-probe-draw seq=7 encoder=3 "
                "encoder_draw_index=2 draw_ordinal=42 command_index=50 eligible=1 applied=1 "
                "optimized_eligible=1 optimized_applied=0 reorder_bytes=1234 "
                "scissor_rect_eligible=1 scissor_rect_applied=1 "
                "alpha_blend_probe_applied=1 depth_write_probe_applied=0 "
                "depth_func_probe_applied=1 "
                "primitive_type=4 primitive_count=4096 "
                "vertex_count=12288 texture_mask=0x7f "
                "texture0=0x101 texture1=0x202 texture2=0x303 texture3=0x404 "
                "texture4=0x505 texture5=0x606 texture6=0x707 texture7=0x808 "
                "color_write=0xf "
                "alpha_blend=1 src_blend=5 dst_blend=6 blend_op=1 "
                "separate_alpha=0 src_blend_alpha=5 dst_blend_alpha=6 "
                "blend_op_alpha=1 "
                "alpha_test=0 depth_enabled=1 depth_write=0 depth_func=4 "
                "stencil=0 clip_plane=0 scissor=1 scissor_l=16 scissor_t=32 "
                "scissor_r=512 scissor_b=384 "
                "original_scissor_l=8 original_scissor_t=16 "
                "original_scissor_r=256 original_scissor_b=192 "
                "cull=2 fill=0 base_vertex=0 "
                "start_index=128 index_type=1 index_buffer=0xdef "
                "effective_index_source=cached-reordered-hit "
                "effective_index_offset=0 effective_index_bytes=1234 "
                "stream0_handle=0xabc stream0_offset=64 stream0_stride=24 "
                "stream_extra_bindings=s1:0xbeef@128/32;s2:0xcafe@0/16 "
                "pso=0x111 shader_variant=0x222 vs=0x333 ps=0x444 "
                "vs_constants_hash=0x555 ps_constants_hash=0x666 "
                "uniform_payload_hash=0x777 "
                "vsout=0xfff]\n"
                "[dxmt9-perf-render-pass-reentry frame=9 rank=1 "
                "a_rt=0xaaa a_depth=0x111 a_samples=1 "
                "b_rt=0xbbb b_depth=0x222 b_samples=1 "
                "count=3 preservation_bytes=12288 "
                "prior_a_seq=60 prior_a_encoder=0 prior_a_pass=2 "
                "first_seq=60 first_encoder=2 first_pass=4 "
                "first_b_seq=60 first_b_encoder=1 first_b_pass=3 "
                "last_seq=60 last_encoder=8 last_pass=6 "
                "last_b_seq=60 last_b_encoder=7 last_b_pass=5 "
                "b_reads_a_color=1 b_reads_a_depth=0 "
                "a_reads_b_color=0 a_reads_b_depth=1 "
                "a_color_proof=4 a_depth_proof=5 "
                "b_color_proof=11 b_depth_proof=12 "
                "a_color_touch_distance=7 a_depth_touch_distance=8 "
                "b_color_touch_distance=4294967295 b_depth_touch_distance=12]\n"
                "[dxmt9-perf-frame frame=0 wall_ms=0.000 fps=0.000 "
                "present_encoded=1 submit_draw=1 submit_present=1 "
                "command_buffers=1 render_pass_begin=1 render_pass_end=1 "
                "draw_calls=4 draw_indexed=2 draw_triangles=42 draw_vertices=126 "
                "bind_pipeline=3 submit_draw_cpu_ms=1.000 encode_chunk_calls=1 "
                "encode_chunk_cpu_ms=1.500 encode_draw_cpu_ms=1.250 "
                "encode_draw_pipeline_lookup_cpu_ms=0.100 "
                "encode_draw_uniform_build_cpu_ms=0.200 "
                "encode_draw_binding_packet_cpu_ms=0.300 "
                "encode_draw_argbuf_cbuf_update_cpu_ms=0.400 "
                "encode_draw_stream_bind_cpu_ms=0.500 encode_draw_issue_cpu_ms=0.600 "
                "command_buffer_commit_cpu_ms=0.700 completion_wait_ms=0.000 "
                "present_acquire_wait_ms=0.000 present_boundary_wait_ms=0.000 "
                "present_token_wait_ms=0.000 gpu_command_buffer_time_ms=0.000 "
                "gpu_command_buffer_time_samples=0 render_encoder_gpu_time_ms=0.000 "
                "render_encoder_gpu_time_samples=0 gpu_command_buffer_errors=0 "
                "sub_command_buffers=1]\n"
                "[dxmt9-perf-frame frame=1 wall_ms=125.000 fps=8.000 "
                "present_encoded=1 submit_draw=2 submit_present=1 "
                "command_buffers=3 render_pass_begin=5 render_pass_end=5 "
                "draw_calls=40 draw_indexed=20 draw_triangles=420 draw_vertices=1260 "
                "bind_pipeline=30 submit_draw_cpu_ms=10.000 encode_chunk_calls=2 "
                "encode_chunk_cpu_ms=15.000 encode_draw_cpu_ms=12.500 "
                "encode_draw_pipeline_lookup_cpu_ms=1.000 "
                "encode_draw_uniform_build_cpu_ms=2.000 "
                "encode_draw_binding_packet_cpu_ms=3.000 "
                "encode_draw_argbuf_cbuf_update_cpu_ms=4.000 "
                "encode_draw_stream_bind_cpu_ms=5.000 encode_draw_issue_cpu_ms=6.000 "
                "command_buffer_commit_cpu_ms=7.000 completion_wait_ms=9.000 "
                "present_acquire_wait_ms=1.000 present_boundary_wait_ms=2.000 "
                "present_token_wait_ms=3.000 gpu_command_buffer_time_ms=11.000 "
                "gpu_command_buffer_time_samples=2 render_encoder_gpu_time_ms=10.000 "
                "render_encoder_gpu_time_samples=2 gpu_command_buffer_errors=1 "
                "sub_command_buffers=3]\n",
                encoding="utf-8",
            )

            encoders, streams = module.parse_encoder_lines(log_path)
            probe_draws = module.parse_probe_draw_lines(log_path)
            reentry_rows = module.parse_render_pass_reentry_lines(log_path)
            frame_rows = module.parse_frame_lines(log_path)
            module.enrich_encoder_rows(encoders)

            self.assertEqual(len(encoders), 1)
            self.assertEqual(len(streams), 1)
            self.assertEqual(len(probe_draws), 1)
            self.assertEqual(len(reentry_rows), 1)
            self.assertEqual(len(frame_rows), 2)
            self.assertEqual(reentry_rows[0]["a_rt"], "0xaaa")
            self.assertEqual(reentry_rows[0]["b_depth"], "0x222")
            self.assertEqual(reentry_rows[0]["preservation_bytes"], 12288)
            self.assertEqual(reentry_rows[0]["prior_a_seq"], 60)
            self.assertEqual(reentry_rows[0]["prior_a_encoder"], 0)
            self.assertEqual(reentry_rows[0]["prior_a_pass"], 2)
            self.assertEqual(reentry_rows[0]["a_color_touch_distance"], 7)
            self.assertEqual(reentry_rows[0]["b_color_touch_distance"], 4294967295)
            self.assertEqual(frame_rows[1]["wall_ms"], 125.0)
            self.assertEqual(frame_rows[1]["fps"], 8.0)
            encoder = encoders[0]
            self.assertEqual(encoder["color_attachment_count"], 1)
            self.assertEqual(encoder["color0_included"], 1)
            self.assertEqual(encoder["color0_load_action"], 1)
            self.assertEqual(encoder["color0_store_action"], 1)
            self.assertEqual(encoder["color_load_bytes"], 1024)
            self.assertEqual(encoder["color_store_bytes"], 2048)
            self.assertEqual(encoder["depth_included"], 1)
            self.assertEqual(encoder["depth_load_action"], 2)
            self.assertEqual(encoder["depth_store_action"], 1)
            self.assertEqual(encoder["depth_clear"], 1)
            self.assertEqual(encoder["depth_store_bytes"], 4096)
            self.assertEqual(encoder["stream_metal_binds"], 5)
            self.assertEqual(encoder["cull_none_draws"], 1)
            self.assertEqual(encoder["cull_back_draws"], 3)
            self.assertEqual(encoder["depth_enabled_draws"], 4)
            self.assertEqual(encoder["depth_write_draws"], 3)
            self.assertEqual(encoder["depth_func_lessequal_draws"], 2)
            self.assertEqual(encoder["scissor_enabled_draws"], 2)
            self.assertEqual(encoder["alpha_blend_enabled_draws"], 1)
            self.assertEqual(encoder["blend_screen_draws"], 1)
            self.assertEqual(encoder["blend_additive_draws"], 0)
            self.assertEqual(encoder["blend_alpha_composite_draws"], 0)
            self.assertEqual(encoder["alpha_blend_textured_draws"], 1)
            self.assertEqual(encoder["alpha_blend_textured_primitives"], 42)
            self.assertEqual(encoder["alpha_blend_textured_vertices"], 126)
            self.assertEqual(encoder["alpha_blend_small_draws"], 1)
            self.assertEqual(encoder["alpha_blend_small_primitives"], 42)
            self.assertEqual(encoder["alpha_blend_small_vertices"], 126)
            self.assertEqual(encoder["tile_ffp_routed_tile_draws"], 0)
            self.assertEqual(encoder["tile_ffp_routed_portable_draws"], 4)
            self.assertEqual(encoder["tile_ffp_routed_portable_primitives"], 4096)
            self.assertEqual(encoder["tile_ffp_eligible_draws"], 1)
            self.assertEqual(encoder["tile_ffp_eligible_primitives"], 1024)
            self.assertEqual(encoder["tile_ffp_fallback_not_ffp_draws"], 2)
            self.assertEqual(encoder["tile_ffp_fallback_not_ffp_primitives"], 2048)
            self.assertEqual(encoder["tile_ffp_fallback_unsupported_state_draws"], 1)
            self.assertEqual(encoder["tile_ffp_fallback_unsupported_state_primitives"], 1024)
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
            self.assertEqual(encoder["probe_scissor_rect_draws"], 1)
            self.assertEqual(encoder["probe_scissor_rect_area_delta_pixels"], 256)
            self.assertEqual(encoder["probe_force_texture_white_draws"], 2)
            self.assertEqual(encoder["probe_fragmentless_depth_only_draws"], 3)
            self.assertEqual(encoder["probe_fragmentless_depth_only_primitives"], 4096)
            self.assertEqual(encoder["probe_fragmentless_depth_only_vertices"], 12288)
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
            self.assertEqual(probe_draw["scissor_rect_eligible"], 1)
            self.assertEqual(probe_draw["scissor_rect_applied"], 1)
            self.assertEqual(probe_draw["alpha_blend_probe_applied"], 1)
            self.assertEqual(probe_draw["depth_write_probe_applied"], 0)
            self.assertEqual(probe_draw["depth_func_probe_applied"], 1)
            self.assertEqual(probe_draw["primitive_count"], 4096)
            self.assertEqual(probe_draw["texture0"], "0x101")
            self.assertEqual(probe_draw["texture7"], "0x808")
            self.assertEqual(probe_draw["src_blend"], 5)
            self.assertEqual(probe_draw["dst_blend"], 6)
            self.assertEqual(probe_draw["scissor_r"], 512)
            self.assertEqual(probe_draw["original_scissor_r"], 256)
            self.assertEqual(probe_draw["index_buffer"], "0xdef")
            self.assertEqual(probe_draw["effective_index_source"], "cached-reordered-hit")
            self.assertEqual(probe_draw["effective_index_offset"], 0)
            self.assertEqual(probe_draw["effective_index_bytes"], 1234)
            self.assertEqual(probe_draw["stream0_handle"], "0xabc")
            self.assertEqual(probe_draw["stream_extra_bindings"],
                             "s1:0xbeef@128/32;s2:0xcafe@0/16")
            self.assertEqual(probe_draw["vs_constants_hash"], "0x555")
            self.assertEqual(probe_draw["ps_constants_hash"], "0x666")
            self.assertEqual(probe_draw["uniform_payload_hash"], "0x777")

            encoder_csv = temp_path / "3dmark05-perf-encoders.csv"
            stream_csv = temp_path / "3dmark05-perf-encoder-streams.csv"
            probe_draw_csv = temp_path / "3dmark05-perf-indexed-probe-draws.csv"
            reentry_csv = temp_path / "3dmark05-perf-render-pass-reentry.csv"
            frame_csv = temp_path / "3dmark05-perf-frames.csv"
            module.write_csv(encoder_csv, encoders, module.ENCODER_CSV_KEYS)
            module.write_csv(stream_csv, streams, module.STREAM_CSV_KEYS)
            module.write_csv(probe_draw_csv, probe_draws, module.PROBE_DRAW_CSV_KEYS)
            module.write_csv(reentry_csv, reentry_rows, module.RENDER_PASS_REENTRY_CSV_KEYS)
            module.write_csv(frame_csv, frame_rows, module.FRAME_CSV_KEYS)

            with encoder_csv.open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["color_attachment_count"], "1")
            self.assertEqual(row["color0_load_action"], "1")
            self.assertEqual(row["color0_store_action"], "1")
            self.assertEqual(row["color_load_bytes"], "1024")
            self.assertEqual(row["color_store_bytes"], "2048")
            self.assertEqual(row["depth_load_action"], "2")
            self.assertEqual(row["depth_store_action"], "1")
            self.assertEqual(row["depth_store_bytes"], "4096")
            self.assertEqual(row["stream_metal_binds"], "5")
            self.assertEqual(row["cull_none_draws"], "1")
            self.assertEqual(row["cull_back_draws"], "3")
            self.assertEqual(row["depth_enabled_draws"], "4")
            self.assertEqual(row["depth_write_draws"], "3")
            self.assertEqual(row["scissor_enabled_draws"], "2")
            self.assertEqual(row["tile_ffp_routed_portable_draws"], "4")
            self.assertEqual(row["tile_ffp_eligible_draws"], "1")
            self.assertEqual(row["tile_ffp_fallback_not_ffp_draws"], "2")
            self.assertEqual(row["tile_ffp_fallback_unsupported_state_draws"], "1")
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
            self.assertEqual(row["probe_scissor_rect_draws"], "1")
            self.assertEqual(row["probe_force_texture_white_draws"], "2")
            self.assertEqual(row["probe_fragmentless_depth_only_draws"], "3")
            self.assertEqual(row["probe_fragmentless_depth_only_primitives"], "4096")
            self.assertEqual(row["probe_fragmentless_depth_only_vertices"], "12288")
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
            self.assertEqual(probe_row["alpha_blend_probe_applied"], "1")
            self.assertEqual(probe_row["depth_func_probe_applied"], "1")
            self.assertEqual(probe_row["reorder_bytes"], "1234")
            self.assertEqual(probe_row["texture0"], "0x101")
            self.assertEqual(probe_row["texture7"], "0x808")
            self.assertEqual(probe_row["src_blend"], "5")
            self.assertEqual(probe_row["scissor_l"], "16")
            self.assertEqual(probe_row["original_scissor_l"], "8")
            self.assertEqual(probe_row["effective_index_source"], "cached-reordered-hit")
            self.assertEqual(probe_row["effective_index_offset"], "0")
            self.assertEqual(probe_row["effective_index_bytes"], "1234")
            self.assertEqual(probe_row["stream_extra_bindings"],
                             "s1:0xbeef@128/32;s2:0xcafe@0/16")
            self.assertEqual(probe_row["pso"], "0x111")
            self.assertEqual(probe_row["vs_constants_hash"], "0x555")
            self.assertEqual(probe_row["ps_constants_hash"], "0x666")
            self.assertEqual(probe_row["uniform_payload_hash"], "0x777")

            with reentry_csv.open(newline="", encoding="utf-8") as handle:
                reentry_row = next(csv.DictReader(handle))
            self.assertEqual(reentry_row["a_rt"], "0xaaa")
            self.assertEqual(reentry_row["b_rt"], "0xbbb")
            self.assertEqual(reentry_row["count"], "3")
            self.assertEqual(reentry_row["preservation_bytes"], "12288")
            self.assertEqual(reentry_row["prior_a_seq"], "60")
            self.assertEqual(reentry_row["prior_a_encoder"], "0")
            self.assertEqual(reentry_row["prior_a_pass"], "2")
            self.assertEqual(reentry_row["first_b_encoder"], "1")
            self.assertEqual(reentry_row["last_b_encoder"], "7")
            self.assertEqual(reentry_row["b_reads_a_color"], "1")
            self.assertEqual(reentry_row["a_reads_b_depth"], "1")
            self.assertEqual(reentry_row["a_color_proof"], "4")
            self.assertEqual(reentry_row["b_depth_proof"], "12")
            self.assertEqual(reentry_row["a_color_touch_distance"], "7")
            self.assertEqual(reentry_row["b_color_touch_distance"], "4294967295")

            with frame_csv.open(newline="", encoding="utf-8") as handle:
                frame_csv_rows = list(csv.DictReader(handle))
            self.assertEqual(frame_csv_rows[1]["wall_ms"], "125.0")
            self.assertEqual(frame_csv_rows[1]["completion_wait_ms"], "9.0")

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
                        "draw_skipped_no_pipeline": 0,
                        "gpu_command_buffer_errors": 2,
                        "hazard_probe": 10,
                        "hazard_bloom": 3,
                        "hazard_exact": 2,
                        "hazard_bloom_false_positive": 1,
                        "render_pass_same_key_reentry": 9,
                        "render_pass_same_key_reentry_distance_1": 4,
                        "render_pass_same_key_reentry_distance_2": 2,
                        "render_pass_same_key_reentry_distance_3_4": 1,
                        "render_pass_same_key_reentry_distance_5_8": 1,
                        "render_pass_same_key_reentry_distance_9_16": 1,
                        "render_pass_same_key_reentry_distance_17_plus": 0,
                        "render_pass_same_key_reentry_distance_1_same_color": 1,
                        "render_pass_same_key_reentry_distance_1_same_color_preservation_bytes": 1024,
                        "render_pass_same_key_reentry_distance_1_same_depth": 2,
                        "render_pass_same_key_reentry_distance_1_same_depth_preservation_bytes": 2048,
                        "render_pass_same_key_reentry_distance_1_rt_depth_change": 1,
                        "render_pass_same_key_reentry_distance_1_rt_depth_change_preservation_bytes": 4096,
                        "render_pass_same_key_reentry_distance_1_sample_change": 0,
                        "render_pass_same_key_reentry_distance_1_sample_change_preservation_bytes": 0,
                        "render_split_rt_change": 8,
                        "render_split_hazard": 2,
                        "render_split_clear": 1,
                        "render_split_present": 4,
                        "map_buffer_total_ms": 12.5,
                        "map_buffer_mutex_wait_ms": 3.25,
                        "map_buffer_wait_ms": 0.0,
                    },
                },
                encoders,
                streams,
                encoder_csv,
                stream_csv,
                probe_draws,
                probe_draw_csv,
                reentry_rows,
                reentry_csv,
                frame_rows,
                frame_csv,
            )
            summary = summary_md.read_text(encoding="utf-8")
            self.assertIn("## Correctness / Visual-Coupling Counters", summary)
            self.assertIn("| run | `draw_skipped_no_pipeline` | `0` |", summary)
            self.assertIn("| run | `gpu_command_buffer_errors` | `2` |", summary)
            self.assertIn("| run | `hazard_bloom_false_positive` | `1` |", summary)
            self.assertIn("| run | `render_split_hazard` | `2` |", summary)
            self.assertIn("| run | `render_split_rt_change` | `8` |", summary)
            self.assertIn("| run | `render_pass_same_key_reentry_distance_1` | `4` |", summary)
            self.assertIn("| run | `map_buffer_total_ms` | `12.500` |", summary)
            self.assertIn("| run | `map_buffer_mutex_wait_ms` | `3.250` |", summary)
            self.assertIn("| encoder_sum | `blend_screen_draws` | `1` |", summary)
            self.assertIn("| encoder_sum | `tile_ffp_fallback_not_ffp_draws` | `2` |", summary)
            self.assertIn("| encoder_sum | `transient_vertex_decl_fallback_bytes` | `8` |", summary)
            self.assertIn("| encoder_sum | `probe_force_texture_white_draws` | `2` |", summary)
            self.assertIn("## Frame Sampling / Low-FPS Windows", summary)
            self.assertIn("| `sampled_frames` | `1` |", summary)
            self.assertIn("| 1 | 125.000 | 8.000 | 40 | 5/5 | 3 | 9.000 | 6.000 | 11.000 | 10.000 | 3 | 1 |", summary)
            self.assertIn("## State-Delta Break Split", summary)
            self.assertIn("### Same-Key Re-entry Distance", summary)
            self.assertIn("| `render_pass_same_key_reentry_distance_2` | `2` |", summary)
            self.assertIn("| `render_pass_same_key_reentry_distance_1` | `4` | `44.44%` |", summary)
            self.assertIn("### Same-Key Re-entry Distance-1 Shape", summary)
            self.assertIn("| same color / different depth | `1` | `25.00%` | `1,024` |", summary)
            self.assertIn("| different color / same depth | `2` | `50.00%` | `2,048` |", summary)
            self.assertIn("| different color / different depth | `1` | `25.00%` | `4,096` |", summary)
            self.assertIn("### Same-Key Re-entry Touch Distance Distribution", summary)
            self.assertIn(
                "| `color=none; depth=12` | `color=7; depth=8` | `3` | `100.00%` | "
                "`12,288` | `100.00%` | `1` |",
                summary,
            )
            self.assertIn("### Same-Key Re-entry Top Patterns", summary)
            self.assertIn(
                "| `0x222` | `0x111` | `1->2` | `3` | `12,288` | `4,096` | "
                "`color` | `depth` | `color=BlockPresent; depth=BlockPresent` | "
                "`color=BlockDrawTarget; depth=BlockDrawDepth` | "
                "`color=none; depth=12` | `color=7; depth=8` | `1` | `60` |",
                summary,
            )
            self.assertIn("### Same-Key Re-entry Top A/B Pairs", summary)
            self.assertIn(
                "| `0xaaa` | `0x111` | `0xbbb` | `0x222` | `3` | `12,288` | "
                "`color` | `depth` | `color=BlockPresent; depth=BlockPresent` | "
                "`color=BlockDrawTarget; depth=BlockDrawDepth` | "
                "`color=none; depth=12` | `color=7; depth=8` | `1` | `60/7->60/8` |",
                summary,
            )
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
            self.assertIn("### Alpha Blend Signature Breakdown", summary)
            self.assertIn("rgb=SrcAlpha,InvSrcAlpha,Add; sep=0; write=0xf", summary)
            self.assertIn("| 7 | 3 | rgb=SrcAlpha,InvSrcAlpha,Add; sep=0; write=0xf | 1 | 4,096 | 12,288 | 1/4,096 | 1 | 0 | 1 |", summary)
            self.assertIn("### Alpha Blend Material Breakdown", summary)
            self.assertIn("| 7 | 3 | rgb=SrcAlpha,InvSrcAlpha,Add; sep=0; write=0xf | 0xfff | 0x222 | 0x111 | 1 | 4,096 | 12,288 | 1/4,096 | 1 | 0 | 1 | 1 | 0 |", summary)
            self.assertIn("### Alpha Blend Signature Run Summary", summary)
            self.assertIn("| 7 | 3 | rgb=SrcAlpha,InvSrcAlpha,Add; sep=0; write=0xf | 1 | 1 | 4,096 | 2..2 | 1/4,096 | 1/4,096 | 1 | 0 | 1 | 1 | 1 | 1 | 1 | 0 |", summary)
            self.assertIn("### Alpha Blend Signature Runs", summary)
            self.assertIn("| 7 | 3 | 2 | 2 | rgb=SrcAlpha,InvSrcAlpha,Add; sep=0; write=0xf | 1 | 4,096 | 1/4,096 | 1 | 0 | 1 | 1 | 1 | 1 | 1 | 0 |", summary)
            self.assertIn("| `applied` | `1` |", summary)
            self.assertIn("| `optimized_eligible` | `1` |", summary)
            self.assertIn("| `alpha_blend_probe_applied` | `1` |", summary)
            self.assertIn("| `depth_func_probe_applied` | `1` |", summary)
            self.assertIn("| `vs_constants_hash_unique` | `1` |", summary)
            self.assertIn("| `ps_constants_hash_unique` | `1` |", summary)
            self.assertIn("| `uniform_payload_hash_unique` | `1` |", summary)


if __name__ == "__main__":
    unittest.main()
