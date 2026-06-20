#!/usr/bin/env python3
"""Regression tests for 3DMark05 perf summary parsing."""

from __future__ import annotations

import csv
import importlib.util
import json
import subprocess
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
    def test_current_uniform_compact_saved_gate_passes_when_present(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            temp_path.joinpath("result.json").write_text(
                json.dumps({
                    "status": "pass",
                    "dxmt9_perf_counters": {
                        "present_encoded": 4,
                        "d3d9_snapshot_uniform_materialized_compact_saved_bytes": 1024,
                    },
                }),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    "python3",
                    str(SCRIPT),
                    str(temp_path),
                    "--require-uniform-compact-saved-bytes-present",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(temp_path.joinpath("3dmark05-perf-summary.md").exists())

    def test_current_uniform_compact_saved_gate_fails_when_zero(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            temp_path.joinpath("result.json").write_text(
                json.dumps({
                    "status": "pass",
                    "dxmt9_perf_counters": {
                        "present_encoded": 4,
                        "d3d9_snapshot_uniform_materialized_compact_saved_bytes": 0,
                    },
                }),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    "python3",
                    str(SCRIPT),
                    str(temp_path),
                    "--require-uniform-compact-saved-bytes-present",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "uniform_compact_saved_bytes_per_present stayed zero",
                result.stderr,
            )

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
                "| `draw_uniform_payload_append_bytes` | `17,230,080` |\n\n"
                "## Uniform Payload Derived\n\n"
                "| Metric | Value |\n"
                "|---|---:|\n"
                "| `uniform_append_bytes_per_present` | `wrong-table` |\n\n"
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
            self.assertEqual(
                result["dxmt9_perf_counters"]["draw_uniform_payload_append_bytes"],
                17230080,
            )
            self.assertNotIn(
                "uniform_append_bytes_per_present",
                result["dxmt9_perf_counters"],
            )
            self.assertEqual(result["dxmt9_bridge_counters"]["bridge_factory"], 53)
            self.assertEqual(result["dxmt9_bridge_counters"]["bridge_draw"], 0)

    def test_pe_recorder_gap_phase_split_summary(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            summary_md = temp_path / "summary.md"

            module.write_markdown(
                summary_md,
                temp_path,
                {
                    "status": "pass",
                    "dxmt9_perf_counters": {
                        "present_encoded": 10,
                    },
                    "dxmt9_pe_recorder_counters": {
                        "interAppendTop1Ms": 100.0,
                        "gapDrawIndexedVsConstFTop1CallFamily": "draw",
                        "gapDrawIndexedVsConstFTop1Samples": 4,
                        "gapDrawIndexedVsConstFTop1Ms": 100.0,
                        "gapDrawIndexedVsConstFTop1MaxMs": 10.0,
                        "gapDrawIndexedVsConstFPhaseSamples": 4,
                        "gapDrawIndexedVsConstFPreCallMs": 30.0,
                        "gapDrawIndexedVsConstFPreCallMaxMs": 5.0,
                        "gapDrawIndexedVsConstFInsideCallMs": 70.0,
                        "gapDrawIndexedVsConstFInsideCallMaxMs": 8.0,
                        "gapDrawIndexedVsConstFTailSplitSamples": 4,
                        "gapDrawIndexedVsConstFPrevCallTailMs": 10.0,
                        "gapDrawIndexedVsConstFPrevCallTailMaxMs": 2.0,
                        "gapDrawIndexedVsConstFBetweenCallsMs": 20.0,
                        "gapDrawIndexedVsConstFBetweenCallsMaxMs": 4.0,
                        "gapDrawIndexedVsConstFBetweenCallBodyCalls": 15,
                        "gapDrawIndexedVsConstFBetweenCallBodyCpuMs": 8.0,
                        "gapDrawIndexedVsConstFBetweenCallBodyCpuMaxMs": 1.5,
                        "gapDrawIndexedVsConstFBetweenTop1CallFamily": "vs_const",
                        "gapDrawIndexedVsConstFBetweenTop1Samples": 12,
                        "gapDrawIndexedVsConstFBetweenTop2CallFamily": "texture",
                        "gapDrawIndexedVsConstFBetweenTop2Samples": 4,
                        "gapDrawIndexedVsConstFBetweenTop1CallName": "SetVertexShaderConstantF",
                        "gapDrawIndexedVsConstFBetweenTop1CallNameSamples": 11,
                        "gapDrawIndexedVsConstFBetweenTop1CallNameCpuMs": 5.5,
                        "gapDrawIndexedVsConstFBetweenTop1CallNameCpuMaxMs": 1.25,
                        "gapDrawIndexedVsConstFBetweenTop2CallName": "SetVertexShaderConstantI",
                        "gapDrawIndexedVsConstFBetweenTop2CallNameSamples": 1,
                        "gapDrawIndexedVsConstFBetweenTop2CallNameCpuMs": 0.25,
                        "gapDrawIndexedVsConstFBetweenTop2CallNameCpuMaxMs": 0.25,
                        "gapDrawIndexedVsConstFBetweenGapTop1PrevFamily": "draw",
                        "gapDrawIndexedVsConstFBetweenGapTop1NextFamily": "vs_const",
                        "gapDrawIndexedVsConstFBetweenGapTop1Samples": 4,
                        "gapDrawIndexedVsConstFBetweenGapTop1Ms": 12.0,
                        "gapDrawIndexedVsConstFBetweenGapTop1MaxMs": 3.0,
                        "gapDrawIndexedVsConstFBetweenGapTop1PrevCallName": "DrawIndexedPrimitive",
                        "gapDrawIndexedVsConstFBetweenGapTop1NextCallName": "SetVertexShaderConstantF",
                        "gapDrawIndexedVsConstFBetweenGapTop1NameSamples": 4,
                        "gapDrawIndexedVsConstFBetweenGapTop1NameMs": 12.0,
                        "gapDrawIndexedVsConstFBetweenGapTop1NameMaxMs": 3.0,
                        "gapDrawIndexedVsConstFBetweenGapSiteTop1PrevCallName": "DrawIndexedPrimitive",
                        "gapDrawIndexedVsConstFBetweenGapSiteTop1NextCallName": "SetVertexShaderConstantF",
                        "gapDrawIndexedVsConstFBetweenGapSiteTop1CallerModule": "3DMark05.exe",
                        "gapDrawIndexedVsConstFBetweenGapSiteTop1CallerRva": "0x1234",
                        "gapDrawIndexedVsConstFBetweenGapSiteTop1Samples": 4,
                        "gapDrawIndexedVsConstFBetweenGapSiteTop1Ms": 12.0,
                        "gapDrawIndexedVsConstFBetweenGapSiteTop1MaxMs": 3.0,
                        "gapDrawIndexedVsConstFBetweenGapTop2PrevFamily": "vs_const",
                        "gapDrawIndexedVsConstFBetweenGapTop2NextFamily": "vs_const",
                        "gapDrawIndexedVsConstFBetweenGapTop2Samples": 2,
                        "gapDrawIndexedVsConstFBetweenGapTop2Ms": 4.0,
                        "gapDrawIndexedVsConstFBetweenGapTop2MaxMs": 1.0,
                        "gapDrawIndexedVsConstFBetweenGapTop2PrevCallName": "SetVertexShaderConstantF",
                        "gapDrawIndexedVsConstFBetweenGapTop2NextCallName": "SetVertexShaderConstantF",
                        "gapDrawIndexedVsConstFBetweenGapTop2NameSamples": 2,
                        "gapDrawIndexedVsConstFBetweenGapTop2NameMs": 4.0,
                        "gapDrawIndexedVsConstFBetweenGapTop2NameMaxMs": 1.0,
                    },
                },
                [],
                [],
                temp_path / "encoders.csv",
                temp_path / "streams.csv",
            )

            summary = summary_md.read_text(encoding="utf-8")
            self.assertIn("## PE Recorder Inter-Append Call-Family Attribution", summary)
            self.assertIn("### Focused Inter-Append Phase Split", summary)
            self.assertIn("### Focused Pre-Call Tail Split", summary)
            self.assertIn(
                "| `draw_indexed -> set_vs_const_f` | `4` | `30.000` | "
                "`3.000` | `70.000` | `7.000` | `30.00%` | `70.00%` | "
                "`5.000` | `8.000` |",
                summary,
            )
            self.assertIn(
                "| `draw_indexed -> set_vs_const_f` | `4` | `10.000` | "
                "`1.000` | `20.000` | `2.000` | `33.33%` | `66.67%` | "
                "`2.000` | `4.000` |",
                summary,
            )
            self.assertIn("### Focused Between-Calls Entry Families", summary)
            self.assertIn(
                "| `draw_indexed -> set_vs_const_f` | `1` | `vs_const` | "
                "`12` | `3.000` | `1.200` |",
                summary,
            )
            self.assertIn("### Focused Between-Calls Entry Names", summary)
            self.assertIn(
                "| `draw_indexed -> set_vs_const_f` | `1` | "
                "`SetVertexShaderConstantF` | `11` | `2.750` | `1.100` | "
                "`5.500` | `0.550` | `1.250` |",
                summary,
            )
            self.assertIn("### Focused Between-Calls Return-To-Entry Gaps", summary)
            self.assertIn(
                "| `draw_indexed -> set_vs_const_f` | `1` | "
                "`draw -> vs_const` | `4` | `12.000` | `1.200` | "
                "`60.00%` | `3.000` |",
                summary,
            )
            self.assertIn(
                "### Focused Between-Calls Exact Return-To-Entry Gaps",
                summary,
            )
            self.assertIn(
                "| `draw_indexed -> set_vs_const_f` | `1` | "
                "`DrawIndexedPrimitive -> SetVertexShaderConstantF` | `4` | "
                "`12.000` | `1.200` | `60.00%` | `3.000` |",
                summary,
            )
            self.assertIn(
                "### Focused Between-Calls Exact Return-To-Entry Call Sites",
                summary,
            )
            self.assertIn(
                "| `draw_indexed -> set_vs_const_f` | `1` | "
                "`DrawIndexedPrimitive -> SetVertexShaderConstantF` | "
                "`3DMark05.exe` | `0x1234` | `4` | `12.000` | `1.200` | "
                "`60.00%` | `3.000` |",
                summary,
            )
            self.assertIn("### Focused Between-Calls Body Coverage", summary)
            self.assertIn(
                "| `draw_indexed -> set_vs_const_f` | `20.000` | `2.000` | "
                "`5.750` | `0.575` | `15` | `8.000` | `0.800` | "
                "`40.00%` | `12.000` | `1.200` | `60.00%` |",
                summary,
            )

    def test_argbuf_delta_source_lines_are_aggregated(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            temp_path.joinpath("result.json").write_text(
                json.dumps({
                    "status": "pass",
                    "dxmt9_perf_counters": {
                        "present_encoded": 1,
                    },
                }),
                encoding="utf-8",
            )
            temp_path.joinpath("dxmt9.log").write_text(
                "[dxmt9-perf-argbuf-payload-delta-source seq=10 "
                "overflow=0 vs_hash=0x111 ps_hash=0x222 prefix_regs=256 "
                "rows=2 changed_regs=512 span_regs=512 "
                "full_prefix_rows=2 full_prefix_regs=512]\n"
                "[dxmt9-perf-argbuf-payload-delta-source seq=11 "
                "overflow=0 vs_hash=0x111 ps_hash=0x222 prefix_regs=256 "
                "rows=3 changed_regs=768 span_regs=768 "
                "full_prefix_rows=3 full_prefix_regs=768]\n"
                "[dxmt9-perf-argbuf-payload-delta-source seq=12 "
                "overflow=1 rows=4 changed_regs=1024]\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                ["python3", str(SCRIPT), str(temp_path)],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            csv_path = temp_path / "3dmark05-perf-argbuf-payload-delta-sources.csv"
            self.assertTrue(csv_path.exists())
            with csv_path.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["vs_hash"], "0x111")
            self.assertEqual(rows[0]["ps_hash"], "0x222")
            self.assertEqual(rows[0]["prefix_regs"], "256")
            self.assertEqual(rows[0]["rows"], "5")
            self.assertEqual(rows[0]["changed_regs"], "1280")
            self.assertEqual(rows[0]["full_prefix_regs"], "1280")
            self.assertEqual(rows[1]["vs_hash"], "overflow")
            self.assertEqual(rows[1]["overflow_rows"], "4")
            summary = temp_path.joinpath("3dmark05-perf-summary.md").read_text(
                encoding="utf-8"
            )
            self.assertIn("## Argbuf Payload Delta Sources", summary)
            self.assertIn("0x111", summary)

    def test_vs_const_setter_range_lines_are_aggregated(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            temp_path.joinpath("result.json").write_text(
                json.dumps({
                    "status": "pass",
                    "dxmt9_perf_counters": {
                        "present_encoded": 1,
                    },
                }),
                encoding="utf-8",
            )
            temp_path.joinpath("dxmt9.log").write_text(
                "[dxmt9-perf-vs-const-setter-range event=present overflow=0 "
                "phase=call vs_hash=0x111 ps_hash=0x222 start=0 count=256 "
                "events=2 range_regs=512 changed_regs=500 "
                "changed_span_regs=512 full_range_events=2 "
                "full_changed_events=2]\n"
                "[dxmt9-perf-vs-const-setter-range event=present overflow=0 "
                "phase=call vs_hash=0x111 ps_hash=0x222 start=0 count=256 "
                "events=3 range_regs=768 changed_regs=700 "
                "changed_span_regs=768 full_range_events=3 "
                "full_changed_events=3]\n"
                "[dxmt9-perf-vs-const-setter-range event=present overflow=0 "
                "phase=flush vs_hash=0x111 ps_hash=0x222 start=0 count=256 "
                "events=4 range_regs=1024 changed_regs=1000 "
                "changed_span_regs=1024 full_range_events=4 "
                "full_changed_events=4]\n"
                "[dxmt9-perf-vs-const-setter-range event=present overflow=1 "
                "phase=call events=5 range_regs=20 changed_regs=10 "
                "changed_span_regs=12 full_range_events=0 "
                "full_changed_events=0]\n"
                "[dxmt9-perf-vs-const-setter-range event=present overflow=0 "
                "phase=call vs_hash=0x333 ps_hash=0x444[dxmt9-perf-frame "
                "frame=1 wall_ms=16.000]\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                ["python3", str(SCRIPT), str(temp_path)],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            csv_path = temp_path / "3dmark05-perf-vs-const-setter-ranges.csv"
            self.assertTrue(csv_path.exists())
            with csv_path.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 3)
            self.assertEqual(rows[0]["phase"], "call")
            self.assertEqual(rows[0]["vs_hash"], "0x111")
            self.assertEqual(rows[0]["ps_hash"], "0x222")
            self.assertEqual(rows[0]["start"], "0")
            self.assertEqual(rows[0]["count"], "256")
            self.assertEqual(rows[0]["events"], "5")
            self.assertEqual(rows[0]["range_regs"], "1280")
            self.assertEqual(rows[0]["changed_regs"], "1200")
            self.assertEqual(rows[0]["full_range_events"], "5")
            self.assertEqual(rows[1]["phase"], "flush")
            self.assertEqual(rows[1]["changed_regs"], "1000")
            self.assertEqual(rows[2]["vs_hash"], "overflow")
            self.assertEqual(rows[2]["overflow_events"], "5")
            summary = temp_path.joinpath("3dmark05-perf-summary.md").read_text(
                encoding="utf-8"
            )
            self.assertIn("## VS Const Setter Ranges", summary)
            self.assertIn("0x111", summary)
            self.assertIn("| `call` | `overflow` | `overflow` |", summary)
            self.assertIn("| `flush` | `0x111` | `0x222` |", summary)

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
                "sub_command_buffers=3 "
                "encode_session_carry_deferred_chunks=1 "
                "encode_session_carry_deferred_active_render_chunks=1 "
                "encode_session_carry_final_chunks=0 "
                "open_cb_tail_present_pending_started=1 "
                "open_cb_tail_present_pending_suppressed_no_tail=0 "
                "open_cb_tail_present_head_appended=0 "
                "open_cb_tail_present_tail_appended=0 "
                "open_cb_tail_present_tail_submitted=0 "
                "open_cb_tail_present_pending_abandoned_no_ready=0]\n",
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
                        "present_encoded": 80,
                        "completion_wait_ms": 800.0,
                        "completion_wait_with_enqueue_ms": 40.0,
                        "completion_wait_without_enqueue_ms": 760.0,
                        "completion_enqueue_while_waiting_present": 4,
                        "commit_chunk_replay_cpu_ms": 320.0,
                        "commit_chunk_replay_draw_record_cpu_ms": 300.0,
                        "commit_chunk_replay_pending_flush_cpu_ms": 120.0,
                        "commit_chunk_replay_pending_flush_before_record_cpu_ms": 60.0,
                        "commit_chunk_replay_pending_flush_draw_run_cpu_ms": 40.0,
                        "commit_chunk_replay_pending_flush_draw_fallback_cpu_ms": 15.0,
                        "commit_chunk_replay_pending_flush_failure_cpu_ms": 0.0,
                        "commit_chunk_replay_pending_flush_end_cpu_ms": 5.0,
                        "commit_chunk_replay_pending_flush_before_record_flushes": 3,
                        "commit_chunk_replay_pending_flush_draw_run_flushes": 4,
                        "commit_chunk_replay_pending_flush_draw_fallback_flushes": 1,
                        "commit_chunk_replay_pending_flush_failure_flushes": 0,
                        "commit_chunk_replay_pending_flush_end_flushes": 2,
                        "commit_chunk_replay_pending_flush_before_record_records": 6,
                        "commit_chunk_replay_pending_flush_draw_run_records": 20,
                        "commit_chunk_replay_pending_flush_draw_fallback_records": 2,
                        "commit_chunk_replay_pending_flush_failure_records": 0,
                        "commit_chunk_replay_pending_flush_end_records": 12,
                        "commit_chunk_replay_pending_flush_forced_resource_marking_cpu_ms": 20.0,
                        "commit_chunk_replay_pending_flush_forced_resource_marking_flushes": 2,
                        "commit_chunk_replay_pending_flush_forced_resource_marking_records": 8,
                        "commit_chunk_replay_draw_run_preflush_opportunities": 4,
                        "commit_chunk_replay_draw_run_preflush_pending_records": 20,
                        "commit_chunk_replay_draw_run_preflush_run_records": 12,
                        "commit_chunk_replay_draw_run_preflush_combined_records": 32,
                        "commit_chunk_replay_end_flush_probe_stored": 5,
                        "commit_chunk_replay_end_flush_probe_stored_records": 30,
                        "commit_chunk_replay_end_flush_probe_first_submission": 3,
                        "commit_chunk_replay_end_flush_probe_first_submission_same_state_lane": 2,
                        "commit_chunk_replay_end_flush_probe_first_submission_same_uniform_generation": 1,
                        "commit_chunk_replay_end_flush_probe_first_submission_same_uniform_payload_hash": 2,
                        "commit_chunk_replay_end_flush_probe_first_submission_same_state_lane_and_uniform_generation": 1,
                        "commit_chunk_replay_end_flush_probe_first_submission_same_state_lane_and_uniform_payload_hash": 1,
                        "commit_chunk_replay_end_flush_probe_first_submission_pending_records": 18,
                        "commit_chunk_replay_end_flush_probe_first_draw_run": 1,
                        "commit_chunk_replay_end_flush_probe_first_draw_run_pending_records": 8,
                        "commit_chunk_replay_end_flush_probe_first_draw_run_run_records": 4,
                        "commit_chunk_replay_end_flush_probe_blocked_non_draw": 1,
                        "commit_chunk_replay_end_flush_probe_blocked_draw_fallback": 0,
                        "commit_chunk_replay_end_flush_probe_blocked_pending_records": 4,
                        "commit_chunk_draw_batch_submit_cpu_ms": 150.0,
                        "commit_chunk_queue_draw_submission_cpu_ms": 160.0,
                        "commit_chunk_queue_draw_submission_snapshot_cpu_ms": 140.0,
                        "commit_chunk_queue_draw_submission_emplace_cpu_ms": 8.0,
                        "d3d9_snapshot_draw_submission_cpu_ms": 80.0,
                        "submit_draw_cpu_ms": 155.0,
                        "submit_draw_run_batch_queue_lock_cpu_ms": 2.0,
                        "submit_draw_run_batch_compat_scan_cpu_ms": 3.0,
                        "submit_draw_run_batch_binding_override_cpu_ms": 2.0,
                        "submit_draw_run_batch_binding_snapshot_cpu_ms": 4.0,
                        "submit_draw_run_batch_payload_bytes_cpu_ms": 1.0,
                        "submit_draw_run_batch_slot_prepare_cpu_ms": 2.0,
                        "submit_draw_run_batch_resource_mark_cpu_ms": 3.0,
                        "submit_draw_run_batch_append_cpu_ms": 10.0,
                        "submit_draw_run_batch_append_reserve_cpu_ms": 1.0,
                        "submit_draw_run_batch_append_state_cpu_ms": 2.0,
                        "submit_draw_run_batch_append_uniform_cpu_ms": 3.0,
                        "submit_draw_run_batch_append_payload_cpu_ms": 1.0,
                        "submit_draw_run_batch_append_param_cpu_ms": 0.5,
                        "submit_draw_run_batch_append_record_cpu_ms": 0.5,
                        "submit_draw_run_batch_chunk_commit_cpu_ms": 5.0,
                        "d3d9_snapshot_cache_lookup_cpu_ms": 40.0,
                        "d3d9_snapshot_cache_batch_hit_cpu_ms": 16.0,
                        "d3d9_snapshot_cache_batch_miss_cpu_ms": 24.0,
                        "d3d9_snapshot_cache_batch_miss_uniform_build_cpu_ms": 20.0,
                        "d3d9_snapshot_cache_batch_miss_uniform_build_hash_cpu_ms": 11.0,
                        "d3d9_snapshot_cache_batch_miss_hot_build_cpu_ms": 7.0,
                        "d3d9_snapshot_cache_batch_miss_shader_layout_cpu_ms": 3.0,
                        "encode_chunk_cpu_ms": 400.0,
                        "encode_draw_cpu_ms": 200.0,
                        "encode_slot_pso_prefetch_cpu_ms": 32.0,
                        "encode_slot_pso_prefetch_draw_key_resolve_cpu_ms": 16.0,
                        "encode_draw_argbuf_setup_cpu_ms": 120.0,
                        "encode_draw_argbuf_open_cpu_ms": 40.0,
                        "encode_draw_argbuf_open_call_cpu_ms": 25.0,
                        "encode_draw_argbuf_open_reserve_cpu_ms": 10.0,
                        "encode_draw_argbuf_reopen_post_cpu_ms": 30.0,
                        "encode_draw_argbuf_reopen_cbuf_force_dirty_cpu_ms": 11.0,
                        "encode_draw_argbuf_table_bind_cpu_ms": 6.0,
                        "encode_draw_stream_bind_cpu_ms": 40.0,
                        "encode_draw_binding_packet_cpu_ms": 30.0,
                        "encode_draw_argbuf_cbuf_update_cpu_ms": 24.0,
                        "encode_draw_argbuf_cbuf_update_vs_cpu_ms": 12.0,
                        "encode_draw_argbuf_cbuf_update_ps_cpu_ms": 8.0,
                        "encode_draw_argbuf_cbuf_build_cpu_ms": 9.0,
                        "encode_draw_argbuf_cbuf_upload_cpu_ms": 7.0,
                        "encode_draw_argbuf_cbuf_cached_repoint_cpu_ms": 18.0,
                        "encode_draw_argbuf_cbuf_content_probe_cpu_ms": 16.0,
                        "encode_draw_argbuf_table_bind_calls": 10,
                        "encode_draw_argbuf_table_bind_skipped": 2,
                        "encode_draw_argbuf_cbuf_update_calls": 20,
                        "encode_draw_argbuf_cbuf_update_dirty_calls": 15,
                        "encode_draw_argbuf_cbuf_update_write_calls": 14,
                        "encode_draw_argbuf_cbuf_reopen_no_dirty_hash_mismatch": 12,
                        "encode_draw_argbuf_cbuf_reopen_partial_candidates": 3,
                        "encode_draw_argbuf_cbuf_cached_repoint_calls": 11,
                        "encode_draw_argbuf_cbuf_content_probe_calls": 10,
                        "encode_draw_argbuf_cbuf_content_probe_vs_hits": 2,
                        "encode_draw_argbuf_cbuf_content_probe_ps_hits": 6,
                        "encode_draw_argbuf_cbuf_content_probe_ffp_ps_hits": 9,
                        "encode_draw_pipeline_lookup_cpu_ms": 18.0,
                        "completion_no_enqueue_wait_to_commit_chunk_entry_ms": 80.0,
                        "completion_no_enqueue_wait_to_commit_chunk_entry_p50_ms": 0.5,
                        "completion_no_enqueue_wait_to_commit_chunk_entry_p95_ms": 1.0,
                        "completion_no_enqueue_wait_to_commit_chunk_replay_start_ms": 96.0,
                        "completion_no_enqueue_wait_to_commit_chunk_replay_start_p50_ms": 0.6,
                        "completion_no_enqueue_wait_to_commit_chunk_replay_start_p95_ms": 1.2,
                        "completion_no_enqueue_wait_to_commit_chunk_replay_end_ms": 360.0,
                        "completion_no_enqueue_wait_to_commit_chunk_replay_end_p50_ms": 7.0,
                        "completion_no_enqueue_wait_to_commit_chunk_replay_end_p95_ms": 15.0,
                        "completion_no_enqueue_wait_to_commit_publish_ms": 420.0,
                        "completion_no_enqueue_wait_to_commit_publish": 50,
                        "completion_no_enqueue_wait_to_commit_publish_p50_ms": 8.5,
                        "completion_no_enqueue_wait_to_commit_publish_p95_ms": 16.5,
                        "completion_no_enqueue_commit_chunk_entries_before_publish": 120,
                        "completion_no_enqueue_commit_chunk_entries_before_publish_max": 4,
                        "completion_no_enqueue_commit_chunk_entries_before_publish_p50": 2,
                        "completion_no_enqueue_commit_chunk_entries_before_publish_p95": 4,
                        "completion_no_enqueue_commit_chunk_replay_starts_before_publish": 115,
                        "completion_no_enqueue_commit_chunk_replay_starts_before_publish_max": 4,
                        "completion_no_enqueue_commit_chunk_replay_starts_before_publish_p50": 2,
                        "completion_no_enqueue_commit_chunk_replay_starts_before_publish_p95": 4,
                        "completion_no_enqueue_commit_chunk_replay_ends_before_publish": 100,
                        "completion_no_enqueue_commit_chunk_replay_ends_before_publish_max": 3,
                        "completion_no_enqueue_commit_chunk_replay_ends_before_publish_p50": 2,
                        "completion_no_enqueue_commit_chunk_replay_ends_before_publish_p95": 3,
                        "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish": 50,
                        "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_ms": 120.0,
                        "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_max_ms": 12.0,
                        "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_p50_ms": 3.0,
                        "completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_p95_ms": 7.0,
                        "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish": 50,
                        "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_ms": 160.0,
                        "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_max_ms": 14.0,
                        "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_p50_ms": 4.0,
                        "completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_p95_ms": 8.0,
                        "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish": 50,
                        "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_ms": 24.0,
                        "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_max_ms": 2.0,
                        "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_p50_ms": 0.3,
                        "completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_p95_ms": 0.6,
                        "completion_no_enqueue_commit_publish_wait_before_publish": 50,
                        "completion_no_enqueue_commit_publish_wait_before_publish_ms": 16.0,
                        "completion_no_enqueue_commit_publish_wait_before_publish_max_ms": 2.0,
                        "completion_no_enqueue_commit_publish_wait_before_publish_p50_ms": 0.2,
                        "completion_no_enqueue_commit_publish_wait_before_publish_p95_ms": 0.4,
                        "completion_no_enqueue_commit_publish_on_before_publish_cpu": 50,
                        "completion_no_enqueue_commit_publish_on_before_publish_cpu_ms": 20.0,
                        "completion_no_enqueue_commit_publish_on_before_publish_cpu_max_ms": 1.0,
                        "completion_no_enqueue_commit_publish_on_before_publish_cpu_p50_ms": 0.25,
                        "completion_no_enqueue_commit_publish_on_before_publish_cpu_p95_ms": 0.75,
                        "completion_no_enqueue_commit_chunk_shape_samples_before_publish": 20,
                        "completion_no_enqueue_commit_chunk_records_before_publish": 240,
                        "completion_no_enqueue_commit_chunk_records_before_publish_max": 30,
                        "completion_no_enqueue_commit_chunk_records_before_publish_p50": 12,
                        "completion_no_enqueue_commit_chunk_records_before_publish_p95": 20,
                        "completion_no_enqueue_commit_chunk_chunks_with_draw_before_publish": 12,
                        "completion_no_enqueue_commit_chunk_chunks_with_present_before_publish": 2,
                        "completion_no_enqueue_commit_chunk_chunks_state_const_only_before_publish": 6,
                        "completion_no_enqueue_commit_chunk_chunks_no_draw_no_present_before_publish": 8,
                        "completion_no_enqueue_commit_chunk_draw_records_before_publish": 120,
                        "completion_no_enqueue_commit_chunk_const_records_before_publish": 80,
                        "completion_no_enqueue_commit_chunk_apply_state_records_before_publish": 10,
                        "completion_no_enqueue_commit_chunk_clear_records_before_publish": 4,
                        "completion_no_enqueue_commit_chunk_present_records_before_publish": 2,
                        "completion_no_enqueue_commit_chunk_surface_records_before_publish": 20,
                        "completion_no_enqueue_commit_chunk_query_records_before_publish": 1,
                        "completion_no_enqueue_commit_chunk_other_records_before_publish": 3,
                        "chunk_publish_reason_draw_limit": 7,
                        "chunk_publish_reason_payload_limit": 1,
                        "chunk_publish_reason_present": 10,
                        "chunk_publish_reason_present_acquire": 2,
                        "chunk_publish_reason_present_split_before": 3,
                        "chunk_publish_reason_flush": 4,
                        "chunk_publish_reason_stretch_split": 5,
                        "chunk_publish_reason_map_wait": 6,
                        "chunk_publish_reason_unknown": 1,
                        "chunk_publish_commands_draw_limit": 448,
                        "chunk_publish_commands_payload_limit": 16,
                        "chunk_publish_commands_present": 900,
                        "chunk_publish_commands_present_acquire": 24,
                        "chunk_publish_commands_present_split_before": 99,
                        "chunk_publish_commands_flush": 40,
                        "chunk_publish_commands_stretch_split": 50,
                        "chunk_publish_commands_map_wait": 60,
                        "chunk_publish_commands_unknown": 2,
                        "chunk_publish_present_split_before_tail_draw_run": 2,
                        "chunk_publish_present_split_before_tail_clear": 1,
                        "chunk_publish_present_split_before_draw_only": 2,
                        "chunk_publish_present_pre_present_opportunity_slots": 2,
                        "chunk_publish_present_pre_present_opportunity_tail_slots": 2,
                        "chunk_publish_present_pre_present_opportunity_commands": 100,
                        "chunk_publish_present_pre_present_opportunity_draw_runs": 80,
                        "chunk_publish_present_pre_present_opportunity_draw_items": 160,
                        "chunk_publish_present_pre_present_opportunity_non_draw_commands": 20,
                        "chunk_publish_present_pre_present_opportunity_payload_bytes": 4096,
                        "chunk_publish_present_pre_present_opportunity_residency_ms": 18.0,
                        "chunk_publish_present_pre_present_opportunity_tail_draw_run": 1,
                        "chunk_publish_present_pre_present_opportunity_tail_clear": 1,
                        "chunk_publish_present_pre_present_opportunity_draw_only": 1,
                        "open_cb_tail_present_pending_started": 4,
                        "open_cb_tail_present_pending_suppressed_no_tail": 2,
                        "open_cb_tail_present_head_appended": 2,
                        "open_cb_tail_present_tail_appended": 1,
                        "open_cb_tail_present_tail_submitted": 1,
                        "open_cb_tail_present_pending_abandoned_no_ready": 3,
                        "open_cb_tail_present_pending_abandoned_nonappendable": 2,
                        "open_cb_tail_present_pending_abandoned_retain_failed": 1,
                        "open_cb_tail_present_pending_abandoned_encode_null": 1,
                        "open_cb_tail_present_pending_merge_failed": 1,
                        "completion_no_enqueue_wait_to_encode_dequeue_ms": 460.0,
                        "completion_no_enqueue_wait_to_encode_dequeue_p50_ms": 9.0,
                        "completion_no_enqueue_wait_to_encode_dequeue_p95_ms": 17.0,
                        "completion_no_enqueue_wait_to_command_buffer_commit_ms": 760.0,
                        "completion_no_enqueue_wait_to_command_buffer_commit_p50_ms": 19.0,
                        "completion_no_enqueue_wait_to_command_buffer_commit_p95_ms": 35.0,
                        "completion_no_enqueue_stage_commit_entry_to_publish_ms": 320.0,
                        "completion_no_enqueue_stage_commit_entry_to_publish_p50_ms": 8.0,
                        "completion_no_enqueue_stage_commit_entry_to_publish_p95_ms": 16.0,
                        "completion_no_enqueue_stage_publish_to_encode_dequeue_ms": 40.0,
                        "completion_no_enqueue_stage_publish_to_encode_dequeue_p50_ms": 0.25,
                        "completion_no_enqueue_stage_publish_to_encode_dequeue_p95_ms": 0.5,
                        "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_ms": 400.0,
                        "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p50_ms": 10.0,
                        "completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p95_ms": 18.0,
                        "completion_no_enqueue_wait_to_next_enqueue_ms": 720.0,
                        "completion_no_enqueue_wait_to_next_enqueue_p50_ms": 15.0,
                        "completion_no_enqueue_wait_to_next_enqueue_p95_ms": 30.0,
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
                        "d3d9_snapshot_uniform_materialized": 4,
                        "d3d9_snapshot_uniform_materialized_bytes": 40960,
                        "d3d9_snapshot_uniform_materialized_compact_candidate_bytes": 20480,
                        "d3d9_snapshot_uniform_materialized_compact_saved_bytes": 20480,
                        "d3d9_snapshot_uniform_materialized_compact_fixed_bytes": 8192,
                        "d3d9_snapshot_uniform_materialized_compact_vertex_bytes": 4096,
                        "d3d9_snapshot_uniform_materialized_compact_pixel_bytes": 8192,
                        "d3d9_snapshot_submission_carrier_records": 160,
                        "d3d9_snapshot_submission_carrier_bytes": 3388160,
                        "d3d9_snapshot_submission_carrier_uniform_storage_bytes": 1643520,
                        "d3d9_snapshot_submission_carrier_unused_uniform_storage_records": 80,
                        "d3d9_snapshot_submission_carrier_unused_uniform_storage_bytes": 821760,
                        "d3d9_snapshot_uniform_elided": 1,
                        "d3d9_snapshot_uniform_adjacent_previous_payload": 10,
                        "d3d9_snapshot_uniform_adjacent_same_fixed_payload_hash": 8,
                        "d3d9_snapshot_uniform_adjacent_same_fixed_and_shader_const_hashes": 3,
                        "draw_uniform_payload_appends": 2,
                        "draw_uniform_payload_append_bytes": 20512,
                        "draw_uniform_fixed_payload_appends": 1,
                        "draw_uniform_fixed_payload_append_bytes": 8192,
                        "draw_uniform_vertex_constants_appends": 1,
                        "draw_uniform_vertex_constants_append_bytes": 4096,
                        "draw_uniform_pixel_constants_appends": 1,
                        "draw_uniform_pixel_constants_append_bytes": 2048,
                        "draw_uniform_payload_lookup_cpu_ms": 0.8,
                        "draw_uniform_payload_lookup_bucket_cpu_ms": 0.4,
                        "draw_uniform_payload_append_reserve_cpu_ms": 0.3,
                        "draw_uniform_payload_append_copy_cpu_ms": 0.4,
                        "draw_uniform_payload_append_link_cpu_ms": 0.5,
                        "draw_uniform_payload_append_fixed_find_cpu_ms": 0.1,
                        "draw_uniform_payload_append_vertex_find_cpu_ms": 0.2,
                        "draw_uniform_payload_append_pixel_find_cpu_ms": 0.1,
                        "draw_uniform_payload_append_fixed_append_cpu_ms": 0.05,
                        "draw_uniform_payload_append_vertex_append_cpu_ms": 0.1,
                        "draw_uniform_payload_append_pixel_append_cpu_ms": 0.05,
                        "draw_uniform_payload_materialized": 3,
                        "draw_uniform_payload_materialized_bytes": 30720,
                        "draw_uniform_payload_materialize_fallbacks": 1,
                        "draw_uniform_payload_materialize_cpu_ms": 1.5,
                        "draw_uniform_payload_materialized_draw_encoder_command": 1,
                        "draw_uniform_payload_materialized_draw_encoder_command_bytes": 10240,
                        "draw_uniform_payload_materialize_draw_encoder_command_cpu_ms": 0.5,
                        "draw_uniform_payload_materialized_draw_encoder_param": 2,
                        "draw_uniform_payload_materialized_draw_encoder_param_bytes": 20480,
                        "draw_uniform_payload_materialize_draw_encoder_param_cpu_ms": 1.0,
                        "draw_uniform_payload_materialized_queue_observation": 0,
                        "draw_uniform_payload_materialized_queue_observation_bytes": 0,
                        "draw_uniform_payload_materialize_queue_observation_cpu_ms": 0.0,
                        "draw_uniform_payload_lookup_semantic_hash_misses": 3,
                        "draw_uniform_payload_lookup_semantic_hash_miss_bytes": 30768,
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
            self.assertIn("## Chunk Publish Reason Derived", summary)
            self.assertIn(
                "| draw-limit | `7` | `17.95%` | `448` | `64.000` |",
                summary,
            )
            self.assertIn(
                "| present | `10` | `25.64%` | `900` | `90.000` |",
                summary,
            )
            self.assertIn(
                "| map-wait | `6` | `15.38%` | `60` | `10.000` |",
                summary,
            )
            self.assertIn("## PresentSplitBefore Tail Shape", summary)
            self.assertIn("| draw-run | `2` | `66.67%` |", summary)
            self.assertIn("| clear | `1` | `33.33%` |", summary)
            self.assertIn(
                "| `draw_only_split_before_share` | `66.67%` |",
                summary,
            )
            self.assertIn("## Present Pre-Present Work Opportunity", summary)
            self.assertIn("| Prefix tail command | slots | share |", summary)
            self.assertIn("| draw-run | `1` | `50.00%` |", summary)
            self.assertIn("| clear | `1` | `50.00%` |", summary)
            self.assertIn(
                "| `draw_only_pre_present_opportunity_share` | `50.00%` |",
                summary,
            )
            self.assertIn("| `draw_uniform_payload_append_bytes` | `20,512` |", summary)
            self.assertIn("| `draw_uniform_fixed_payload_append_bytes` | `8,192` |", summary)
            self.assertIn("| encoder_sum | `blend_screen_draws` | `1` |", summary)
            self.assertIn("| encoder_sum | `tile_ffp_fallback_not_ffp_draws` | `2` |", summary)
            self.assertIn("| encoder_sum | `transient_vertex_decl_fallback_bytes` | `8` |", summary)
            self.assertIn("| encoder_sum | `probe_force_texture_white_draws` | `2` |", summary)
            self.assertIn("## Uniform Payload Derived", summary)
            self.assertIn("| `uniform_materialized_bytes_per_present` | `512.000` |", summary)
            self.assertIn("| `uniform_compact_candidate_bytes_per_present` | `256.000` |", summary)
            self.assertIn("| `uniform_compact_saved_bytes_per_present` | `256.000` |", summary)
            self.assertIn("| `submission_carrier_bytes_per_record` | `21176.000` |", summary)
            self.assertIn(
                "| `submission_carrier_uniform_storage_bytes_per_record` | `10272.000` |",
                summary,
            )
            self.assertIn(
                "| `submission_carrier_unused_uniform_storage_records_per_present` | `1.000` |",
                summary,
            )
            self.assertIn(
                "| `submission_carrier_unused_uniform_storage_mib_per_present` | `0.010` |",
                summary,
            )
            self.assertIn(
                "| `submission_carrier_unused_uniform_storage_bytes_per_record` | `5136.000` |",
                summary,
            )
            self.assertIn(
                "| `submission_carrier_unused_uniform_storage_share` | `50.00%` |",
                summary,
            )
            self.assertIn("| `uniform_compact_fixed_bytes_per_present` | `102.400` |", summary)
            self.assertIn("| `uniform_compact_vertex_bytes_per_present` | `51.200` |", summary)
            self.assertIn("| `uniform_compact_pixel_bytes_per_present` | `102.400` |", summary)
            self.assertIn("| `uniform_compact_candidate_share_of_materialized_bytes` | `50.00%` |", summary)
            self.assertIn("| `uniform_compact_fixed_share_of_candidate_bytes` | `40.00%` |", summary)
            self.assertIn("| `uniform_compact_vertex_share_of_candidate_bytes` | `20.00%` |", summary)
            self.assertIn("| `uniform_compact_pixel_share_of_candidate_bytes` | `40.00%` |", summary)
            self.assertIn("| `uniform_compact_saved_share_of_materialized_bytes` | `50.00%` |", summary)
            self.assertIn("| `uniform_adjacent_same_fixed_payload_hash_share` | `80.00%` |", summary)
            self.assertIn("| `uniform_adjacent_same_fixed_and_shader_const_hashes_share` | `30.00%` |", summary)
            self.assertIn("| `uniform_append_bytes_per_present` | `256.400` |", summary)
            self.assertIn("| `uniform_fixed_append_bytes_per_present` | `102.400` |", summary)
            self.assertIn("| `uniform_vertex_constants_append_bytes_per_present` | `51.200` |", summary)
            self.assertIn("| `uniform_pixel_constants_append_bytes_per_present` | `25.600` |", summary)
            self.assertIn("| `uniform_stage_constants_append_bytes_per_present` | `76.800` |", summary)
            self.assertIn("| `uniform_vertex_append_amplification_vs_compact_vertex` | `1.000` |", summary)
            self.assertIn("| `uniform_pixel_append_amplification_vs_compact_pixel` | `0.250` |", summary)
            self.assertIn("| `uniform_stage_append_amplification_vs_compact_stage` | `0.500` |", summary)
            self.assertIn("| `uniform_append_bytes_per_append` | `10256.000` |", summary)
            self.assertIn("| `uniform_payload_record_append_bytes_per_append` | `3088.000` |", summary)
            self.assertIn("| `uniform_append_parent_cpu_ms_per_present` | `0.037` |", summary)
            self.assertIn("| `uniform_payload_lookup_cpu_ms_per_present` | `0.010` |", summary)
            self.assertIn("| `uniform_payload_lookup_bucket_cpu_ms_per_present` | `0.005` |", summary)
            self.assertIn("| `uniform_payload_append_reserve_cpu_ms_per_present` | `0.004` |", summary)
            self.assertIn("| `uniform_payload_append_copy_cpu_ms_per_present` | `0.005` |", summary)
            self.assertIn("| `uniform_payload_append_link_cpu_ms_per_present` | `0.006` |", summary)
            self.assertIn("| `uniform_payload_append_storage_cpu_ms_per_present` | `0.015` |", summary)
            self.assertIn("| `uniform_component_find_cpu_ms_per_present` | `0.005` |", summary)
            self.assertIn("| `uniform_component_append_cpu_ms_per_present` | `0.003` |", summary)
            self.assertIn("| `uniform_component_fixed_find_cpu_ms_per_present` | `0.001` |", summary)
            self.assertIn("| `uniform_component_vertex_find_cpu_ms_per_present` | `0.003` |", summary)
            self.assertIn("| `uniform_component_pixel_find_cpu_ms_per_present` | `0.001` |", summary)
            self.assertIn("| `uniform_component_fixed_append_cpu_ms_per_present` | `0.001` |", summary)
            self.assertIn("| `uniform_component_vertex_append_cpu_ms_per_present` | `0.001` |", summary)
            self.assertIn("| `uniform_component_pixel_append_cpu_ms_per_present` | `0.001` |", summary)
            self.assertIn("| `uniform_append_known_cpu_share_of_parent` | `66.67%` |", summary)
            self.assertIn("| `uniform_append_cpu_residual_ms_per_present` | `0.013` |", summary)
            self.assertIn("| `uniform_append_known_with_components_cpu_share_of_parent` | `86.67%` |", summary)
            self.assertIn("| `uniform_append_component_residual_ms_per_present` | `0.005` |", summary)
            self.assertIn("| `uniform_fixed_append_records_per_payload_append` | `0.500` |", summary)
            self.assertIn("| `uniform_vertex_constants_append_records_per_payload_append` | `0.500` |", summary)
            self.assertIn("| `uniform_pixel_constants_append_records_per_payload_append` | `0.500` |", summary)
            self.assertIn("| `uniform_backend_materialized_bytes_per_present` | `384.000` |", summary)
            self.assertIn("| `uniform_backend_materialize_cpu_ms_per_present` | `0.019` |", summary)
            self.assertIn("| `uniform_backend_materialized_bytes_per_call` | `10240.000` |", summary)
            self.assertIn("| `uniform_backend_materialize_fallbacks` | `1` |", summary)
            self.assertIn("| `uniform_backend_materialize_draw_encoder_command_share_pct` | `33.33%` |", summary)
            self.assertIn("| `uniform_backend_materialize_draw_encoder_command_bytes_per_present` | `128.000` |", summary)
            self.assertIn("| `uniform_backend_materialize_draw_encoder_command_cpu_ms_per_present` | `0.006` |", summary)
            self.assertIn("| `uniform_backend_materialize_draw_encoder_param_share_pct` | `66.67%` |", summary)
            self.assertIn("| `uniform_backend_materialize_draw_encoder_param_bytes_per_present` | `256.000` |", summary)
            self.assertIn("| `uniform_backend_materialize_queue_observation_share_pct` | `0.00%` |", summary)
            self.assertIn("| `uniform_append_records_per_materialized_snapshot` | `0.500` |", summary)
            self.assertIn("| `uniform_semantic_hash_misses` | `3` |", summary)
            self.assertIn("| `uniform_semantic_hash_miss_bytes_per_present` | `384.600` |", summary)
            self.assertIn("| `uniform_append_bytes_share_of_materialized_bytes` | `50.08%` |", summary)
            self.assertIn("| `uniform_fixed_append_bytes_share_of_append_bytes` | `39.94%` |", summary)
            self.assertIn("| `uniform_vertex_constants_append_bytes_share_of_append_bytes` | `19.97%` |", summary)
            self.assertIn("| `uniform_pixel_constants_append_bytes_share_of_append_bytes` | `9.98%` |", summary)
            self.assertIn("| `uniform_snapshot_elision_share` | `20.00%` |", summary)
            self.assertIn("## Frame Sampling / Low-FPS Windows", summary)
            self.assertIn("| `sampled_frames` | `1` |", summary)
            self.assertIn("| 1 | 125.000 | 8.000 | 40 | 5/5 | 3 | 9.000 | 6.000 | 11.000 | 10.000 | 3 | 1 |", summary)
            self.assertIn("### Frame-Sampled Open-CB Carry Deltas", summary)
            self.assertIn("| session deferred chunks | `1` | `1.000` |", summary)
            self.assertIn(
                "| session deferred active-render chunks | `1` | `1.000` |",
                summary,
            )
            self.assertIn("| pending started | `1` | `1.000` |", summary)
            self.assertIn("| tail submitted | `0` | `0.000` |", summary)
            self.assertIn("## Pacing / CPU Stage Derived", summary)
            self.assertIn("| `completion_wait_ms_per_present` | `10.000` |", summary)
            self.assertIn("| `completion_wait_with_enqueue_ms_per_present` | `0.500` |", summary)
            self.assertIn("| `completion_wait_without_enqueue_ms_per_present` | `9.500` |", summary)
            self.assertIn("| `completion_wait_overlap_share` | `5.000%` |", summary)
            self.assertIn("| `completion_wait_no_enqueue_share` | `95.000%` |", summary)
            self.assertIn("| `commit_chunk_replay_cpu_ms_per_present` | `4.000` |", summary)
            self.assertIn("| `encode_chunk_cpu_ms_per_present` | `5.000` |", summary)
            self.assertIn("### No-Enqueue Cumulative Timeline", summary)
            self.assertIn(
                "| wait -> commit chunk replay start | `1.200` | `0.600` | `1.200` |",
                summary,
            )
            self.assertIn(
                "| wait -> commit chunk replay end | `4.500` | `7.000` | `15.000` |",
                summary,
            )
            self.assertIn(
                "| wait -> command buffer commit | `9.500` | `19.000` | `35.000` |",
                summary,
            )
            self.assertIn("### No-Enqueue Commit Chunks Before Publish", summary)
            self.assertIn(
                "| entries | `120` | `2.400` | `4` | `2` | `4` |",
                summary,
            )
            self.assertIn(
                "| replay starts | `115` | `2.300` | `4` | `2` | `4` |",
                summary,
            )
            self.assertIn(
                "| replay ends | `100` | `2.000` | `3` | `2` | `3` |",
                summary,
            )
            self.assertIn(
                "### No-Enqueue Commit Entry Publish Attribution",
                summary,
            )
            self.assertIn(
                "| completed replay CPU before publish | `1.500` | `3.000` | `7.000` |",
                summary,
            )
            self.assertIn(
                "| active replay CPU before publish | `2.000` | `4.000` | `8.000` |",
                summary,
            )
            self.assertIn(
                "| inter-replay producer gap before publish | `0.300` | `0.300` | `0.600` |",
                summary,
            )
            self.assertIn(
                "| commit publish wait before publish | `0.200` | `0.200` | `0.400` |",
                summary,
            )
            self.assertIn(
                "| post-publish onBeforePublish CPU | `0.250` | `0.250` | `0.750` |",
                summary,
            )
            self.assertIn(
                "| residual after completed replay only | `2.500` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| residual after completed + active replay | `0.500` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| residual after completed + active replay + inter-replay gap | `0.200` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| residual after completed + active replay + inter-replay gap + publish wait | `0.000` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| completed replay share of commit entry -> publish | `37.500%` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| active replay share of commit entry -> publish | `50.000%` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| inter-replay producer gap share of commit entry -> publish | `7.500%` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| commit publish wait share of commit entry -> publish | `5.000%` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| completed + active replay share of commit entry -> publish | `87.500%` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| completed + active replay + inter-replay gap share of commit entry -> publish | `95.000%` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn(
                "| completed + active replay + inter-replay gap + publish wait share of commit entry -> publish | `100.000%` | `n/a` | `n/a` |",
                summary,
            )
            self.assertIn("### No-Enqueue Before-Publish Chunk Shape", summary)
            self.assertIn(
                "| scanned chunks | `20` | `0.400` | `1.000` |",
                summary,
            )
            self.assertIn(
                "| chunks with draw | `12` | `0.240` | `0.600` |",
                summary,
            )
            self.assertIn(
                "| state/const-only chunks | `6` | `0.120` | `0.300` |",
                summary,
            )
            self.assertIn(
                "| all records | `240` | `4.800` | `12.000` |",
                summary,
            )
            self.assertIn(
                "| draw records | `120` | `2.400` | `6.000` |",
                summary,
            )
            self.assertIn(
                "| const records | `80` | `1.600` | `4.000` |",
                summary,
            )
            self.assertIn(
                "| record count | `30` | `12` | `20` |",
                summary,
            )
            self.assertIn("### Open-CB Tail-Present Carry Shape", summary)
            self.assertIn("| pending started | `4` | `1.000` |", summary)
            self.assertIn(
                "| pending suppressed: no ready tail | `2` | `0.500` |",
                summary,
            )
            self.assertIn("| head appended | `2` | `0.500` |", summary)
            self.assertIn("| tail appended | `1` | `0.250` |", summary)
            self.assertIn("| tail submitted | `1` | `0.250` |", summary)
            self.assertIn(
                "| abandoned: no ready source | `3` | `0.750` |",
                summary,
            )
            self.assertIn(
                "| abandoned: non-appendable source | `2` | `0.500` |",
                summary,
            )
            self.assertIn("### Exposed No-Enqueue Stage Shape", summary)
            self.assertIn(
                "| encode dequeue -> command buffer commit | `5.000` | `10.000` | `18.000` |",
                summary,
            )
            self.assertIn("- Verdict: `under-pipelined-no-enqueue`.", summary)
            self.assertIn(
                "- Largest p50 no-enqueue row: `wait -> next enqueue`.",
                summary,
            )
            self.assertIn("## Replay / Snapshot CPU Derived", summary)
            self.assertIn(
                "| `commit_chunk_queue_draw_submission_snapshot_cpu_ms_per_present` | `1.750` |",
                summary,
            )
            self.assertIn("| `queue_submission_snapshot_share` | `87.50%` |", summary)
            self.assertIn("| `snapshot_cache_lookup_share` | `50.00%` |", summary)
            self.assertIn("| `snapshot_batch_miss_share_of_lookup` | `60.00%` |", summary)
            self.assertIn("| `pending_flush_share_of_replay` | `37.50%` |", summary)
            self.assertIn("| `pending_flush_before_record_share` | `50.00%` |", summary)
            self.assertIn("| `pending_flush_draw_run_share` | `33.33%` |", summary)
            self.assertIn("| `pending_flush_draw_fallback_share` | `12.50%` |", summary)
            self.assertIn("| `pending_flush_failure_share` | `0.00%` |", summary)
            self.assertIn("| `pending_flush_end_share` | `4.17%` |", summary)
            self.assertIn(
                "| `pending_flush_forced_resource_marking_share` | `16.67%` |",
                summary,
            )
            self.assertIn("| `pending_flush_records_per_flush` | `4.000` |", summary)
            self.assertIn(
                "| `pending_flush_forced_resource_marking_records_per_flush` | `4.000` |",
                summary,
            )
            self.assertIn(
                "| `pending_flush_forced_resource_marking_records_per_present` | `0.100` |",
                summary,
            )
            self.assertIn(
                "| `draw_run_preflush_combined_records_per_boundary` | `8.000` |",
                summary,
            )
            self.assertIn("### Chunk-End Flush Carry Probe", summary)
            self.assertIn(
                "| `end_flush_probe_records_per_stored_flush` | `6.000` |",
                summary,
            )
            self.assertIn(
                "| `end_flush_probe_first_submission_same_state_lane_share` | `66.67%` |",
                summary,
            )
            self.assertIn(
                "| `end_flush_probe_first_submission_same_state_lane_and_uniform_generation_share` | `33.33%` |",
                summary,
            )
            self.assertIn(
                "| `end_flush_probe_first_submission_same_state_lane_and_uniform_payload_hash_share` | `33.33%` |",
                summary,
            )
            self.assertIn(
                "| `end_flush_probe_first_draw_run_combined_records_per_candidate` | `12.000` |",
                summary,
            )
            self.assertIn(
                "| `end_flush_probe_resolved_or_blocked_share` | `100.00%` |",
                summary,
            )
            self.assertIn("| `draw_batch_submit_share_of_replay` | `46.88%` |", summary)
            self.assertIn(
                "| `draw_batch_submit_known_child_residual_ms_per_present` | `1.475` |",
                summary,
            )
            self.assertIn(
                "| `draw_batch_append_known_child_residual_ms_per_present` | `0.025` |",
                summary,
            )
            self.assertIn(
                "| `queue_submission_known_child_residual_ms_per_present` | `0.150` |",
                summary,
            )
            self.assertIn(
                "| `draw_record_known_child_residual_ms_per_present` | `1.750` |",
                summary,
            )
            self.assertIn("### Pending Flush Reason Volume", summary)
            self.assertIn(
                "| `draw_run` | `40.000` | `33.33%` | `4` | `20` | "
                "`5.000` | `0.050` | `0.250` |",
                summary,
            )
            self.assertIn("### Draw-Run Preflush Carrier Opportunity", summary)
            self.assertIn(
                "| `draw_run_preflush_pending_records_per_boundary` | `5.000` |",
                summary,
            )
            self.assertIn(
                "| `draw_run_preflush_run_records_per_boundary` | `3.000` |",
                summary,
            )
            self.assertIn(
                "| `draw_run_preflush_combined_records_per_present` | `0.400` |",
                summary,
            )
            self.assertIn(
                "| `draw_run_preflush_opportunity_share_of_draw_run_flushes` | `100.00%` |",
                summary,
            )
            self.assertIn("### Draw Batch Submit Residual", summary)
            self.assertIn(
                "| `known_child_residual` | `118.000` | `1.475` | `78.67%` |",
                summary,
            )
            self.assertIn(
                "| `queue_lock` | `2.000` | `0.025` | `1.33%` |",
                summary,
            )
            self.assertIn(
                "| `append` | `10.000` | `0.125` | `6.67%` |",
                summary,
            )
            self.assertIn(
                "| `known_child_residual` | `2.000` | `0.025` | `20.00%` |",
                summary,
            )
            self.assertIn("### Replay / Snapshot Candidate Ranking", summary)
            self.assertIn(
                "| 1 | replay | `commit_chunk_replay_cpu_ms` | `320.000` | "
                "`4.000` | `100.00%` | `200.00%` | `400.00%` |",
                summary,
            )
            self.assertIn(
                "| 3 | submission | `commit_chunk_queue_draw_submission_cpu_ms` | "
                "`160.000` | `2.000` | `50.00%` | `100.00%` | `200.00%` |",
                summary,
            )
            self.assertIn(
                "| 7 | snapshot | `d3d9_snapshot_draw_submission_cpu_ms` | `80.000` | "
                "`1.000` | `25.00%` | `50.00%` | `100.00%` |",
                summary,
            )
            self.assertIn(
                "- Largest replay/snapshot row: `commit_chunk_replay_cpu_ms`.",
                summary,
            )
            self.assertIn(
                "- Largest replay/snapshot row per present: `4.000ms`.",
                summary,
            )
            self.assertIn("## Encode CPU Derived", summary)
            self.assertIn("| `encode_draw_share_of_encode_chunk` | `50.00%` |", summary)
            self.assertIn("### Encode Candidate Ranking", summary)
            self.assertIn(
                "| 1 | draw | `encode_draw_argbuf_setup_cpu_ms` | `120.000` | "
                "`1.500` | `30.00%` | `60.00%` |",
                summary,
            )
            self.assertIn(
                "| 4 | slot | `encode_slot_pso_prefetch_cpu_ms` | `32.000` | "
                "`0.400` | `8.00%` | `n/a` |",
                summary,
            )
            self.assertIn("- Largest encode-stage row: `encode_draw_argbuf_setup_cpu_ms`.", summary)
            self.assertIn("- Largest encode-stage row per present: `1.500ms`.", summary)
            self.assertIn("## Argbuf CPU Derived", summary)
            self.assertIn("| `argbuf_open_share_of_setup` | `33.33%` |", summary)
            self.assertIn("| `argbuf_cbuf_update_share_of_setup` | `20.00%` |", summary)
            self.assertIn("### Argbuf Candidate Ranking", summary)
            self.assertIn(
                "| 2 | open | `encode_draw_argbuf_open_cpu_ms` | `40.000` | "
                "`0.500` | `33.33%` |",
                summary,
            )
            self.assertIn(
                "| 4 | open | `encode_draw_argbuf_open_call_cpu_ms` | `25.000` | "
                "`0.312` | `20.83%` |",
                summary,
            )
            self.assertIn(
                "| 5 | cbuf | `encode_draw_argbuf_cbuf_update_cpu_ms` | `24.000` | "
                "`0.300` | `20.00%` |",
                summary,
            )
            self.assertIn(
                "| 6 | cbuf | `encode_draw_argbuf_cbuf_cached_repoint_cpu_ms` | `18.000` | "
                "`0.225` | `15.00%` |",
                summary,
            )
            self.assertIn(
                "| 7 | cbuf | `encode_draw_argbuf_cbuf_content_probe_cpu_ms` | `16.000` | "
                "`0.200` | `13.33%` |",
                summary,
            )
            self.assertIn(
                "| 9 | open | `encode_draw_argbuf_reopen_cbuf_force_dirty_cpu_ms` | `11.000` | "
                "`0.138` | `9.17%` |",
                summary,
            )
            self.assertIn("| `argbuf_table_bind_skip_share` | `20.00%` |", summary)
            self.assertIn("| `argbuf_cbuf_update_dirty_share` | `75.00%` |", summary)
            self.assertIn("| `argbuf_cbuf_content_probe_ffp_ps_hit_share` | `90.00%` |", summary)
            self.assertIn("- Largest argbuf-stage row: `encode_draw_argbuf_setup_cpu_ms`.", summary)
            self.assertIn("- Largest argbuf-stage row per present: `1.500ms`.", summary)
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
