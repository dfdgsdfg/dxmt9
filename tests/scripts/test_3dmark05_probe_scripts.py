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
            "--force-visible",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_DISABLE_CULL=1", result.stdout)
        self.assertIn("DXMT_DISABLE_SCISSOR=1", result.stdout)
        self.assertIn("DXMT_DEBUG_FORCE_VISIBLE=1", result.stdout)

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
