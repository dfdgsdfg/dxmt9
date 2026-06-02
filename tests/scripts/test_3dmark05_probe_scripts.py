#!/usr/bin/env python3
"""Regression tests for 3DMark05 perf probe shell wrappers."""

from __future__ import annotations

import json
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_WRAPPER = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_perf_probe.sh"
FINALIZER = REPO_ROOT / "scripts" / "tools" / "finalize_3dmark05_perf_probe.sh"
SUMMARIZER = REPO_ROOT / "scripts" / "tools" / "summarize_3dmark05_perf.py"
XCODE_SUMMARIZER = REPO_ROOT / "scripts" / "tools" / "summarize_xcode_encoder_counters.py"


def load_xcode_summarizer():
    spec = importlib.util.spec_from_file_location("summarize_xcode_encoder_counters", XCODE_SUMMARIZER)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_result(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    path.joinpath("result.json").write_text(
        json.dumps({"dxmt9_perf_counters": {}}),
        encoding="utf-8",
    )


class ThreeDMark05ProbeScriptTests(unittest.TestCase):
    def run_script(self, script: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(script), *args],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_wrapper_rejects_run_level_gate_without_baseline_output(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--require-draw-run-records-increase",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("run-level comparison gates require", result.stderr)

    def test_finalizer_rejects_run_level_gate_without_baseline_output(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--require-draw-run-records-increase",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("run-level comparison gates require", result.stderr)

    def test_wrapper_rejects_xcode_compare_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--require-top-unexplained-buffer-write-decrease",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_finalizer_rejects_xcode_compare_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--max-top-unexplained-buffer-write-ratio",
            "0.25",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_wrapper_rejects_missing_baseline_output_path(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--compare-baseline-output",
            "does-not-exist",
            "--require-draw-run-records-increase",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing baseline result.json", result.stderr)

    def test_finalizer_rejects_missing_baseline_output_path(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--baseline-output",
            "does-not-exist",
            "--require-draw-run-records-increase",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing baseline result.json", result.stderr)

    def test_wrapper_rejects_missing_baseline_joined_path(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--baseline-joined",
            "does-not-exist.csv",
            "--require-top-gpu-decrease",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing baseline joined CSV", result.stderr)

    def test_wrapper_dry_run_low_space_warning_does_not_interleave_commands(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "dry-run-order",
            "--min-free-mb",
            "999999999",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, "")
        lines = result.stdout.splitlines()
        finalize_lines = [
            line for line in lines if line.startswith("finalize_cmd_after_xcode_export:")
        ]
        self.assertEqual(len(finalize_lines), 1)
        self.assertNotIn("dry-run:", finalize_lines[0])
        dry_run_index = lines.index(
            "dry-run: free space is below the launch guard; cleanup candidates follow"
        )
        finalize_index = lines.index(finalize_lines[0])
        self.assertGreater(dry_run_index, finalize_index)
        self.assertIn("large ignored/manual-review candidates:", result.stdout)

    def test_wrapper_rejects_low_gputrace_free_space_guard_without_override(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "unsafe-low-gputrace-space",
            "--min-free-mb",
            "256",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("refusing low free-space gputrace launch guard", result.stderr)
        self.assertIn("DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB=1", result.stderr)

    def test_wrapper_forwards_unexplained_write_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-unexplained",
                "--baseline-joined",
                str(baseline_joined),
                "--require-top-unexplained-buffer-write-decrease",
                "--max-top-unexplained-buffer-write-ratio",
                "0.50",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-top-unexplained-buffer-write-decrease", finalize_line)
        self.assertIn("--max-top-unexplained-buffer-write-ratio", finalize_line)
        self.assertIn("0.50", finalize_line)

    def test_wrapper_forwards_frame_shape_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-frame-shape",
                "--baseline-joined",
                str(baseline_joined),
                "--require-top-row-key-match",
                "--max-top-draw-call-delta-ratio",
                "0.05",
                "--max-top-vertex-count-delta-ratio",
                "0.05",
                "--max-top-triangle-delta-ratio",
                "0.05",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-top-row-key-match", finalize_line)
        self.assertIn("--max-top-draw-call-delta-ratio", finalize_line)
        self.assertIn("--max-top-vertex-count-delta-ratio", finalize_line)
        self.assertIn("--max-top-triangle-delta-ratio", finalize_line)
        self.assertIn("0.05", finalize_line)

    def test_wrapper_forwards_stable_frame_proof_preset_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-stable-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertIn("--require-result-json", finalize_line)
        self.assertIn("--require-top-pso-attribution", finalize_line)
        self.assertIn("--require-xcode-counter-coverage", finalize_line)
        self.assertIn("--require-dxmt-join-coverage", finalize_line)
        self.assertIn("--max-top-draw-call-delta-ratio", finalize_line)
        self.assertIn("--max-top-vertex-count-delta-ratio", finalize_line)
        self.assertIn("--max-top-triangle-delta-ratio", finalize_line)
        self.assertIn("0.05", finalize_line)

    def test_wrapper_forwards_top_and_hot_share_to_finalizer(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "forward-hot-set",
            "--top",
            "4",
            "--hot-gpu-share",
            "98",
            "--min-free-mb",
            "0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--top 4", finalize_line)
        self.assertIn("--hot-gpu-share 98", finalize_line)

    def test_wrapper_forwards_result_json_gate_to_finalizer(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "forward-result-json",
            "--require-result-json",
            "--min-free-mb",
            "0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-result-json", finalize_line)

    def test_wrapper_dry_run_includes_sparse_const_split_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--split-sparse-const-records",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_SPLIT_SPARSE_CONST_RECORDS=1", result.stdout)

    def test_wrapper_dry_run_includes_vertex_temp_trim_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--trim-vertex-temps",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_TRIM_VERTEX_TEMPS=1", result.stdout)

    def test_wrapper_dry_run_includes_vsout_point_size_probe_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--drop-vsout-point-size",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1", result.stdout)

    def test_wrapper_dry_run_includes_force_fragment_color_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--force-fragment-color",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1", result.stdout)

    def test_wrapper_dry_run_includes_index_reuse_measure_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--measure-index-reuse",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)

    def test_wrapper_dry_run_includes_indexed_geometry_dump_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "geometry-dump-dry-run",
            "--no-gputrace",
            "--dump-indexed-geometry",
            "--dump-indexed-geometry-max-draws",
            "3",
            "--probe-reverse-indexed-triangles-rows",
            "60/0,60/1",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("geometry_dump_dir:", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_INDEXED_GEOMETRY_DIR=",
            result.stdout,
        )
        self.assertIn(
            "traces/app-d3d9-3dmark05-geometry-dump-dry-run/analysis/geometry",
            result.stdout,
        )
        self.assertIn("DXMT9_DUMP_INDEXED_GEOMETRY_MAX_DRAWS=3", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROWS=60/0\\,60/1",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_x8_shader_alpha_fill_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--suppress-x8-rt-pixel-format-view",
            "--x8-shader-alpha-fill",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_SUPPRESS_X8_RT_PIXEL_FORMAT_VIEW=1", result.stdout)
        self.assertIn("DXMT9_X8_SHADER_ALPHA_FILL=1", result.stdout)

    def test_summarizer_accepts_partial_log_without_result_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp) / "run"
            output_dir.mkdir()
            output_dir.joinpath("dxmt9.log").write_text(
                "\n".join([
                    "[dxmt9-bridge-perf] bridge_factory=1 bridge_draw=2",
                    "[dxmt9-perf-encoder seq=60 encoder=2 draw_calls=3 "
                    "pso_state_samples=3 stream_handle_changes=4]",
                    "[dxmt9-perf-encoder-stream seq=60 encoder=2 stream=0 samples=3 "
                    "metal_binds=3]",
                    "[dxmt9-perf] present_encoded=5 draw_calls=7 "
                    "map_buffer_total_ms=0.250 completion_wait_ms=1.500",
                ]),
                encoding="utf-8",
            )

            result = subprocess.run(
                ["python3", str(SUMMARIZER), str(output_dir)],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            summary = output_dir / "3dmark05-perf-summary.md"
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(summary.exists())
            text = summary.read_text(encoding="utf-8")
            self.assertIn("- Status: `partial-log`", text)
            self.assertIn("| `present_encoded` | `5` |", text)
            self.assertTrue(output_dir.joinpath("3dmark05-perf-encoders.csv").exists())
            self.assertTrue(output_dir.joinpath("3dmark05-perf-encoder-streams.csv").exists())

    def test_finalizer_result_json_gate_rejects_partial_log(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp) / "run"
            output_dir.mkdir()
            output_dir.joinpath("dxmt9.log").write_text(
                "[dxmt9-perf] present_encoded=5 draw_calls=7\n",
                encoding="utf-8",
            )

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "partial-log",
                "--output-dir",
                str(output_dir),
                "--require-result-json",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing required result.json", result.stderr)

    def test_xcode_summarizer_joins_x8_shader_alpha_fill_fields(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            encoder_csv = Path(tmp) / "encoders.csv"
            encoder_csv.write_text(
                "\n".join([
                    "seq,encoder,x8_rt_texture_binding_samples,x8_shader_alpha_fill_samples,x8_shader_alpha_fill_mask_or",
                    "60,8,2,2,0x3",
                ]),
                encoding="utf-8",
            )

            summarizer = load_xcode_summarizer()
            dxmt = summarizer.load_dxmt_from_csv(encoder_csv)
            joined = summarizer.join_dxmt({"seq": 60, "enc": 8}, dxmt)

        self.assertEqual(joined["dxmt_x8_rt_texture_binding_samples"], 2)
        self.assertEqual(joined["dxmt_x8_shader_alpha_fill_samples"], 2)
        self.assertEqual(joined["dxmt_x8_shader_alpha_fill_mask_or"], "0x3")

    def test_xcode_summarizer_derives_index_reuse_fields(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            encoder_csv = Path(tmp) / "encoders.csv"
            encoder_csv.write_text(
                "\n".join([
                    "seq,encoder,indexed_vertex_reference_count,indexed_unique_vertex_estimate,indexed_vertex_reuse_samples,indexed_vertex_reuse_skipped,indexed_vertex_cache_miss_estimate_16,indexed_vertex_cache_miss_estimate_32,indexed_vertex_cache_miss_estimate_64",
                    "60,2,300,120,1,0,150,120,120",
                ]),
                encoding="utf-8",
            )

            summarizer = load_xcode_summarizer()
            dxmt = summarizer.load_dxmt_from_csv(encoder_csv)
            joined = {
                "seq": 60,
                "enc": 2,
                "buffer_write_mib": 1.0,
                "vs_buffer_write_mib": 1.0,
                "vs_invocations": 120.0,
            }
            joined = summarizer.join_dxmt(joined, dxmt)

        self.assertEqual(joined["dxmt_indexed_vertex_reference_count"], 300)
        self.assertEqual(joined["dxmt_indexed_unique_vertex_estimate"], 120)
        self.assertEqual(joined["dxmt_indexed_vertex_reuse_ratio"], 2.5)
        self.assertEqual(joined["dxmt_vs_invocations_per_indexed_unique_vertex"], 1.0)
        self.assertEqual(joined["dxmt_indexed_vertex_cache_miss_estimate_16"], 150)
        self.assertEqual(joined["dxmt_indexed_vertex_cache_miss_over_unique_16"], 1.25)
        self.assertEqual(joined["dxmt_vs_invocations_per_indexed_cache_miss_32"], 1.0)

    def test_xcode_summarizer_classifies_hidden_backend_storage(self) -> None:
        summarizer = load_xcode_summarizer()
        joined = {
            "buffer_write_mib": 225.0,
            "vs_buffer_write_mib": 224.0,
            "vs_buffer_bytes_per_vs_invocation": 1500.0,
            "vs_buffer_bytes_per_primitive": 2400.0,
            "tiled_vertex_buffer_mib": 10.0,
            "tiled_primitive_block_mib": 1.0,
            "vs_invocations": 1000.0,
            "dxmt_draw_calls": 10,
            "dxmt_vertex_count": 1000,
            "dxmt_primitive_count": 400,
            "dxmt_vsout_layout_last": "0xfff",
            "dxmt_argbuf_table_bytes": 1024,
            "dxmt_argbuf_cbuf_bytes": 1024,
            "dxmt_set_vertex_bytes_bytes": 0,
            "dxmt_transient_vertex_bytes": 0,
            "dxmt_transient_index_bytes": 0,
            "dxmt_stream0_stride_min": 24,
            "dxmt_stream0_stride_max": 24,
            "dxmt_stream_handle_changes": 10,
            "dxmt_ib_handle_changes": 10,
        }

        summarizer.derive_dxmt_attribution(joined)

        self.assertEqual(joined["dxmt_gpu_write_hint"], "gpu_vs_buffer_write")
        self.assertEqual(
            joined["dxmt_backend_storage_class"],
            "hidden_vertex_tiler_parameter_storage",
        )
        self.assertEqual(joined["dxmt_backend_storage_confidence"], "high")
        self.assertGreater(joined["dxmt_hidden_backend_write_ratio"], 0.90)

    def test_wrapper_dry_run_includes_vs_output_scratch_trim_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--trim-vs-output-scratch",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_TRIM_VS_OUTPUT_SCRATCH=1", result.stdout)

    def test_wrapper_dry_run_includes_render_state_ab_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--disable-cull",
            "--disable-scissor",
            "--probe-disable-alpha-blend",
            "--probe-disable-depth-write",
            "--probe-depth-func-always",
            "--force-cull-mode",
            "back",
            "--force-visible",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_DISABLE_CULL=1", result.stdout)
        self.assertIn("DXMT_DISABLE_SCISSOR=1", result.stdout)
        self.assertIn("DXMT9_PROBE_DISABLE_ALPHA_BLEND=1", result.stdout)
        self.assertIn("DXMT9_PROBE_DISABLE_DEPTH_WRITE=1", result.stdout)
        self.assertIn("DXMT9_PROBE_DEPTH_FUNC_ALWAYS=1", result.stdout)
        self.assertIn("DXMT_DEBUG_FORCE_CULL_MODE=back", result.stdout)
        self.assertIn("DXMT_DEBUG_FORCE_VISIBLE=1", result.stdout)

    def test_wrapper_dry_run_includes_force_expand_indexed_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--force-expand-indexed",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_FORCE_EXPAND_INDEXED=1", result.stdout)

    def test_wrapper_dry_run_includes_reverse_nonopaque_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-reverse-nonopaque-indexed-triangles",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_REVERSE_NONOPAQUE_INDEXED_TRIANGLES=1", result.stdout)

    def test_wrapper_dry_run_includes_indexed_triangle_encoder_draw_range_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-sort-indexed-triangles-by-min-index",
            "--probe-reverse-indexed-triangles-row",
            "60/2",
            "--probe-indexed-triangle-encoder-draw-min",
            "71",
            "--probe-indexed-triangle-encoder-draw-max",
            "188",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_SORT_INDEXED_TRIANGLES_BY_MIN_INDEX=1", result.stdout)
        self.assertIn("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW=60/2", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=71", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=188", result.stdout)

    def test_wrapper_dry_run_includes_scissor_rect_probe_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-scissor-rect",
            "0,0,190,553",
            "--probe-scissor-rect-row",
            "60/4",
            "--probe-scissor-rect-classes",
            "large4096,alpha-blend,scissor",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_SCISSOR_RECT=0\\,0\\,190\\,553", result.stdout)
        self.assertIn("DXMT9_PROBE_SCISSOR_RECT_ROW=60/4", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_SCISSOR_RECT_CLASSES=large4096\\,alpha-blend\\,scissor",
            result.stdout,
        )

    def test_wrapper_rejects_invalid_force_cull_mode(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--force-cull-mode",
            "sideways",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--force-cull-mode must be one of", result.stderr)

    def test_wrapper_dry_run_includes_metal_capture_layer_env_for_gputrace(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MTL_CAPTURE_ENABLED=1", result.stdout)

    def test_wrapper_dry_run_omits_metal_capture_layer_env_without_gputrace(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("MTL_CAPTURE_ENABLED=1", result.stdout)

    def test_wrapper_forwards_const_upload_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-const-gates",
                "--compare-baseline-output",
                str(baseline_output),
                "--require-const-upload-break-bytes-decrease",
                "--require-draw-submission-batch-present",
                "--max-const-upload-break-count-ratio",
                "1.10",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-const-upload-break-bytes-decrease", finalize_line)
        self.assertIn("--require-draw-submission-batch-present", finalize_line)
        self.assertIn("--max-const-upload-break-count-ratio", finalize_line)
        self.assertIn("1.10", finalize_line)

    def test_finalizer_forwards_unexplained_write_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-unexplained",
                "--baseline-joined",
                str(baseline_joined),
                "--require-top-unexplained-buffer-write-decrease",
                "--max-top-unexplained-buffer-write-ratio",
                "0.50",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        summary_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_summary_cmd:")
        )
        self.assertIn("--dxmt-streams-csv", summary_line)
        self.assertIn("--require-top-unexplained-buffer-write-decrease", compare_line)
        self.assertIn("--max-top-unexplained-buffer-write-ratio", compare_line)
        self.assertIn("0.50", compare_line)

    def test_finalizer_forwards_frame_shape_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-frame-shape",
                "--baseline-joined",
                str(baseline_joined),
                "--require-top-row-key-match",
                "--max-top-draw-call-delta-ratio",
                "0.05",
                "--max-top-vertex-count-delta-ratio",
                "0.05",
                "--max-top-triangle-delta-ratio",
                "0.05",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--require-top-row-key-match", compare_line)
        self.assertIn("--max-top-draw-call-delta-ratio", compare_line)
        self.assertIn("--max-top-vertex-count-delta-ratio", compare_line)
        self.assertIn("--max-top-triangle-delta-ratio", compare_line)
        self.assertIn("0.05", compare_line)

    def test_finalizer_expands_stable_frame_proof_preset(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "stable-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        summary_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_summary_cmd:")
        )
        self.assertIn("--require-stable-frame-proof", compare_line)
        self.assertIn("--require-xcode-counter-coverage", summary_line)
        self.assertIn("--require-dxmt-join-coverage", summary_line)
        self.assertIn("--require-top-pso-attribution", summary_line)
        self.assertIn("--max-top-draw-call-delta-ratio", compare_line)
        self.assertIn("--max-top-vertex-count-delta-ratio", compare_line)
        self.assertIn("--max-top-triangle-delta-ratio", compare_line)
        self.assertIn("0.05", compare_line)

    def test_finalizer_forwards_const_upload_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-const-gates",
                "--baseline-output",
                str(baseline_output),
                "--require-const-upload-break-bytes-decrease",
                "--require-draw-submission-batch-present",
                "--max-const-upload-break-count-ratio",
                "1.10",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("perf_compare_cmd:")
        )
        self.assertIn("--require-const-upload-break-bytes-decrease", compare_line)
        self.assertIn("--require-draw-submission-batch-present", compare_line)
        self.assertIn("--max-const-upload-break-count-ratio", compare_line)
        self.assertIn("1.10", compare_line)

    def test_finalizer_rejects_missing_baseline_joined_path(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--baseline-joined",
            "does-not-exist.csv",
            "--require-top-gpu-decrease",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing baseline joined CSV", result.stderr)


if __name__ == "__main__":
    unittest.main()
