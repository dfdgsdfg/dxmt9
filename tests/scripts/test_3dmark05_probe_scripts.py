#!/usr/bin/env python3
"""Regression tests for 3DMark05 perf probe shell wrappers."""

from __future__ import annotations

import json
import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_WRAPPER = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_perf_probe.sh"
FINALIZER = REPO_ROOT / "scripts" / "tools" / "finalize_3dmark05_perf_probe.sh"
SUMMARIZER = REPO_ROOT / "scripts" / "tools" / "summarize_3dmark05_perf.py"
XCODE_SUMMARIZER = REPO_ROOT / "scripts" / "tools" / "summarize_xcode_encoder_counters.py"
RUN_EXPERIMENT = REPO_ROOT / "scripts" / "run_apps" / "run_experiment.py"
DIRECT_WRAPPER = REPO_ROOT / "scripts" / "run_apps" / "run_app-d3d9-3dmark05-verify_direct.sh"
LAUNCHER = REPO_ROOT / "experiments" / "launchers" / "app-d3d9-3dmark05.sh"
RUN_WITH_TIMEOUT = REPO_ROOT / "scripts" / "tools" / "run_with_timeout.py"


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
    def run_script(
        self,
        script: Path,
        *args: str,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        process_env = os.environ.copy()
        if env:
            process_env.update(env)
        return subprocess.run(
            [str(script), *args],
            cwd=REPO_ROOT,
            env=process_env,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_wrapper_defaults_timeout_for_gputrace(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "timeout-default-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("--timeout 420", cmd_line)

    def test_wrapper_defaults_timeout_for_no_gputrace(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "timeout-default-no-gputrace",
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("--timeout 180", cmd_line)

    def test_wrapper_dry_run_prints_top_level_watchdog_timeout(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "watchdog-timeout",
            "--no-gputrace",
            "--timeout",
            "10",
            "--dry-run",
            env={"DXMT_3DMARK05_PROBE_TIMEOUT_SLACK": "7"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("runner_timeout_sec: 10", result.stdout)
        self.assertIn("watchdog_timeout_sec: 10+7", result.stdout)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("--timeout 10", cmd_line)

    def test_wrapper_rejects_invalid_top_level_watchdog_slack(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dry-run",
            env={"DXMT_3DMARK05_PROBE_TIMEOUT_SLACK": "bad"},
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("DXMT_3DMARK05_PROBE_TIMEOUT_SLACK must be", result.stderr)

    def test_run_with_timeout_kills_long_child(self) -> None:
        result = self.run_script(
            RUN_WITH_TIMEOUT,
            "--timeout",
            "0.2",
            "--grace",
            "0.2",
            "--label",
            "test-watchdog",
            "--",
            "python3",
            "-c",
            "import time; time.sleep(5)",
        )

        self.assertEqual(result.returncode, 124)
        self.assertIn("[test-watchdog] timeout after", result.stderr)

    def test_wrapper_dry_run_prints_index_cache_runtime_report_path(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "index-cache-runtime-path",
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "index_cache_runtime_report:",
            result.stdout,
        )
        self.assertIn(
            "3dmark05-index-cache-runtime-summary.md",
            result.stdout,
        )

    def test_wrapper_direct_run_uses_catalogue_prefix(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "direct-prefix",
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        env_line = next(line for line in result.stdout.splitlines() if line.startswith("env:"))
        self.assertIn("DXMT_3DMARK05_DIRECT=1", env_line)
        self.assertIn(
            "DXMT_3DMARK05_PREFIX="
            f"{REPO_ROOT}/experiments/prefixs/app-d3d9-3dmark05",
            env_line,
        )
        self.assertIn(
            "DXMT_3DMARK05_WINESERVER="
            f"{REPO_ROOT}/experiments/wine/sikarugir-cx-24.0.7/bin/wineserver",
            env_line,
        )

    def test_wrapper_runs_wineserver_cleanup_after_watchdog(self) -> None:
        text = RUN_WRAPPER.read_text(encoding="utf-8")

        self.assertIn("cleanup_3dmark05_probe_wineserver", text)
        self.assertIn('WINEPREFIX="$probe_prefix" "$probe_wineserver" -k', text)
        self.assertIn("cleanup_3dmark05_probe_wineserver\n\npython3", text)

    def test_wrapper_rejects_disabled_timeout(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--timeout",
            "0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--timeout must be positive numeric seconds", result.stderr)

    def test_catalogue_runner_rejects_disabled_timeout_for_3dmark05(self) -> None:
        result = self.run_script(
            RUN_EXPERIMENT,
            "run",
            "app-d3d9-3dmark05",
            "--timeout",
            "0",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("app-d3d9-3dmark05: --timeout must be positive", result.stderr)

    def test_direct_wrapper_defaults_timeout(self) -> None:
        result = self.run_script(
            DIRECT_WRAPPER,
            env={"DXMT_3DMARK05_DIRECT_DRY_RUN": "1"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("timeout: 180s", result.stdout)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("experiments/launchers/app-d3d9-3dmark05.sh", cmd_line)

    def test_direct_wrapper_rejects_disabled_timeout(self) -> None:
        result = self.run_script(
            DIRECT_WRAPPER,
            env={
                "DXMT_3DMARK05_DIRECT_DRY_RUN": "1",
                "DXMT_3DMARK05_DIRECT_TIMEOUT": "0",
            },
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("DXMT_3DMARK05_DIRECT_TIMEOUT must be positive numeric seconds", result.stderr)

    def test_direct_launcher_self_timeout_dry_run(self) -> None:
        result = self.run_script(
            LAUNCHER,
            env={
                "DXMT_3DMARK05_DIRECT_DRY_RUN": "1",
                "DXMT_3DMARK05_LAUNCHER_TIMEOUT": "12",
            },
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("launcher_timeout: 12s", result.stdout)
        self.assertIn("DXMT_3DMARK05_SELF_SUPERVISED=1", result.stdout)

    def test_direct_launcher_rejects_disabled_self_timeout(self) -> None:
        result = self.run_script(
            LAUNCHER,
            env={
                "DXMT_3DMARK05_DIRECT_DRY_RUN": "1",
                "DXMT_3DMARK05_LAUNCHER_TIMEOUT": "0",
            },
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("DXMT_3DMARK05_LAUNCHER_TIMEOUT must be positive numeric seconds", result.stderr)

    def test_direct_launcher_traps_timeout_cleanup(self) -> None:
        text = LAUNCHER.read_text(encoding="utf-8")

        self.assertIn("dxmt_3dmark05_auto_enter_pid=$!", text)
        self.assertIn("DXMT_3DMARK05_LAUNCHER_TIMEOUT", text)
        self.assertIn("DXMT_3DMARK05_SELF_SUPERVISED", text)
        self.assertIn("trap 'cleanup_app_d3d9_3dmark05_direct 143' TERM", text)
        self.assertIn('WINEPREFIX="$prefix" "$wine_server" -k', text)

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

    def test_wrapper_rejects_tvb_mechanism_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--require-tvb-mechanism-proof",
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

    def test_finalizer_rejects_tvb_mechanism_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-tvb-baseline",
            "--require-tvb-mechanism-proof",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_wrapper_rejects_non_target_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--target-row-key",
            "50/1",
            "--max-non-target-gpu-regression-ms",
            "1.0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_finalizer_rejects_non_target_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--target-row-key",
            "50/1",
            "--max-non-target-vs-buffer-write-regression-mib",
            "16",
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
        self.assertIn("large trace run directories:", result.stdout)
        self.assertIn("large output run directories:", result.stdout)
        self.assertIn("large ignored/manual-review candidates:", result.stdout)
        self.assertIn("cleanup note: remove only obsolete run ids", result.stdout)

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

    def test_wrapper_forwards_non_target_hot_row_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-non-target",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/1",
                "--target-row-key",
                "50/3",
                "--max-non-target-gpu-regression-ms",
                "1.0",
                "--max-non-target-vs-buffer-write-regression-mib",
                "16",
                "--max-non-target-vs-invocations-regression-ratio",
                "0.05",
                "--max-non-target-draw-call-delta-ratio",
                "0.05",
                "--max-non-target-vertex-count-delta-ratio",
                "0.05",
                "--max-non-target-triangle-delta-ratio",
                "0.05",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--target-row-key", finalize_line)
        self.assertIn("50/1", finalize_line)
        self.assertIn("50/3", finalize_line)
        self.assertIn("--max-non-target-gpu-regression-ms", finalize_line)
        self.assertIn("1.0", finalize_line)
        self.assertIn("--max-non-target-vs-buffer-write-regression-mib", finalize_line)
        self.assertIn("16", finalize_line)
        self.assertIn(
            "--max-non-target-vs-invocations-regression-ratio",
            finalize_line,
        )
        self.assertIn("0.05", finalize_line)
        self.assertIn("--max-non-target-draw-call-delta-ratio", finalize_line)
        self.assertIn("--max-non-target-vertex-count-delta-ratio", finalize_line)
        self.assertIn("--max-non-target-triangle-delta-ratio", finalize_line)

    def test_wrapper_forwards_target_row_apply_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-target",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--target-row-key",
                "50/1",
                "--require-target-index-cache-miss32-decrease",
                "--require-target-index-cache-opt-miss32-decrease",
                "--require-target-reordered-index-cache-hits",
                "--require-target-vs-buffer-write-decrease",
                "--require-target-vs-invocations-decrease",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--target-row-key", finalize_line)
        self.assertIn("50/0", finalize_line)
        self.assertIn("50/1", finalize_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", finalize_line)
        self.assertIn("--require-target-reordered-index-cache-hits", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)

    def test_wrapper_expands_cache_opt_apply_proof_preset_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "cache-opt-apply-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertIn("--target-row-key", finalize_line)
        self.assertIn("50/0", finalize_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)

    def test_wrapper_rejects_cache_opt_apply_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "cache-opt-apply-proof-missing-row",
                "--baseline-joined",
                str(baseline_joined),
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-cache-opt-apply-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_wrapper_rejects_unsafe_nonopaque_cache_apply_proof_without_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "unsafe-cache-opt-apply-proof-missing-semantic",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--probe-apply-index-cache-opt-candidate",
                "--probe-apply-index-cache-opt-candidate-unsafe-nonopaque",
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-semantic-image-proof requires --semantic-image-policy",
            result.stderr,
        )

    def test_wrapper_forwards_unsafe_nonopaque_cache_apply_semantic_proof(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "unsafe-cache-opt-apply-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--semantic-image-policy",
                "exact",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--probe-apply-index-cache-opt-candidate",
                "--probe-apply-index-cache-opt-candidate-unsafe-nonopaque",
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-semantic-image-proof", finalize_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)
        self.assertIn("--semantic-image-policy", finalize_line)
        self.assertIn("exact", finalize_line)

    def test_wrapper_rejects_screen_blend_cache_proof_without_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "screen-blend-cache-proof-missing-semantic",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires --semantic-image-policy",
            result.stderr,
        )

    def test_wrapper_rejects_opaque_depth_index_cache_proof_without_opt_flag(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "opaque-depth-cache-proof-missing-opt",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/1",
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-opaque-depth-index-cache-proof requires --optimize-opaque-depth-index-cache",
            result.stderr,
        )

    def test_wrapper_rejects_opaque_depth_index_cache_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "opaque-depth-cache-proof-missing-target-row",
                "--baseline-joined",
                str(baseline_joined),
                "--optimize-opaque-depth-index-cache",
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-opaque-depth-index-cache-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_wrapper_forwards_opaque_depth_index_cache_proof(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "opaque-depth-cache-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--target-row-key",
                "50/1",
                "--optimize-opaque-depth-index-cache",
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-opaque-depth-index-cache-proof", finalize_line)
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertNotIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", finalize_line)
        self.assertIn("--require-target-reordered-index-cache-hits", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)
        self.assertIn("--target-row-key 50/0", finalize_line)
        self.assertIn("--target-row-key 50/1", finalize_line)

    def test_wrapper_rejects_screen_blend_cache_proof_without_cache_opt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "screen-blend-cache-proof-missing-cache-opt",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires --optimize-screen-blend-index-cache",
            result.stderr,
        )

    def test_wrapper_rejects_screen_blend_cache_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "screen-blend-cache-proof-missing-target-row",
                "--baseline-joined",
                str(baseline_joined),
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--optimize-screen-blend-index-cache",
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_wrapper_forwards_screen_blend_cache_proof_with_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "screen-blend-cache-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--optimize-screen-blend-index-cache",
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-screen-blend-cache-proof", finalize_line)
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertNotIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", finalize_line)
        self.assertIn("--require-target-reordered-index-cache-hits", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)
        self.assertIn("--semantic-image-policy", finalize_line)
        self.assertIn("lsb1", finalize_line)

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

    def test_wrapper_allows_partial_stable_frame_proof_without_result_json_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-partial-stable-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--allow-partial-stable-frame-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertIn("--allow-partial-stable-frame-proof", finalize_line)
        self.assertNotIn("--require-result-json", finalize_line)

    def test_wrapper_forwards_tvb_mechanism_proof_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-tvb-mechanism",
                "--baseline-joined",
                str(baseline_joined),
                "--require-tvb-mechanism-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-tvb-mechanism-proof", finalize_line)

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

    def test_wrapper_forwards_semantic_image_gate_to_finalizer(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "forward-semantic-image",
            "--semantic-image-policy",
            "lsb1",
            "--semantic-image-before",
            "before.ppm",
            "--semantic-image-after",
            "after.ppm",
            "--semantic-image-min-active-pct",
            "2",
            "--min-free-mb",
            "0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--semantic-image-policy lsb1", finalize_line)
        self.assertIn("--semantic-image-before before.ppm", finalize_line)
        self.assertIn("--semantic-image-after after.ppm", finalize_line)
        self.assertIn("--semantic-image-min-active-pct 2", finalize_line)

    def test_wrapper_rejects_incomplete_semantic_image_gate(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "bad-semantic-image",
            "--semantic-image-policy",
            "exact",
            "--semantic-image-before",
            "before.ppm",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--semantic-image-policy requires --semantic-image-before and --semantic-image-after",
            result.stderr,
        )

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

    def test_wrapper_dry_run_includes_scoped_varying_trim_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--trim-unused-varyings",
            "--trim-unused-varyings-vs-hashes",
            "0x61be862718e1d00c",
            "--trim-unused-varyings-ps-hashes",
            "0xfbeb0f02c65a9526",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_TRIM_UNUSED_VARYINGS=1", result.stdout)
        self.assertIn("DXMT9_TRIM_UNUSED_VARYINGS_VS_HASHES=0x61be862718e1d00c", result.stdout)
        self.assertIn("DXMT9_TRIM_UNUSED_VARYINGS_PS_HASHES=0xfbeb0f02c65a9526", result.stdout)

    def test_wrapper_rejects_scoped_varying_trim_without_trim_flag(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--trim-unused-varyings-vs-hashes",
            "0x61be862718e1d00c",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("require --trim-unused-varyings", result.stderr)

    def test_wrapper_dry_run_includes_vsout_point_size_probe_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--drop-vsout-point-size",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1", result.stdout)

    def test_wrapper_dry_run_includes_half_vsout_probe_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-half-vsout",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_HALF_VSOUT=1", result.stdout)

    def test_wrapper_dry_run_includes_force_fragment_color_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--force-fragment-color",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1", result.stdout)

    def test_wrapper_gputrace_dry_run_auto_scopes_encoder_breakdown_to_frame(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=50", result.stdout)

    def test_wrapper_gputrace_dry_run_can_keep_all_frame_encoder_breakdown(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--encoder-breakdown-all-frames",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertNotIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=", result.stdout)

    def test_wrapper_no_encoder_breakdown_for_no_gputrace_smoke(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--no-gputrace",
            "--no-encoder-breakdown",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertNotIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=", result.stdout)

    def test_wrapper_rejects_no_encoder_breakdown_with_gputrace(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--no-encoder-breakdown",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--no-encoder-breakdown requires --no-gputrace", result.stderr)

    def test_wrapper_no_gputrace_index_diagnostics_auto_scope_to_frame(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--no-gputrace",
            "--probe-apply-index-cache-opt-candidate",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=50", result.stdout)

    def test_wrapper_no_gputrace_index_diagnostics_can_keep_all_frames(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--no-gputrace",
            "--measure-index-reuse",
            "--encoder-breakdown-all-frames",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertNotIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=", result.stdout)

    def test_wrapper_dry_run_includes_index_reuse_measure_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--measure-index-reuse",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)

    def test_wrapper_dry_run_includes_index_cache_opt_candidate_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--measure-index-cache-opt-candidate",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)

    def test_wrapper_default_perf_profile_does_not_enable_mutating_index_opts(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_EXPERIMENT_PROFILE=perf", result.stdout)
        self.assertIn("DXMT_DISABLE_AUTO_EXPAND_INDEXED=1", result.stdout)
        self.assertNotIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        self.assertNotIn("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE=1", result.stdout)
        self.assertNotIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertNotIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)

    def test_wrapper_dry_run_includes_apply_index_cache_opt_candidate_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-apply-index-cache-opt-candidate",
            "--probe-apply-index-cache-opt-candidate-min-gain-pct",
            "12",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_MIN_GAIN_PCT=12",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_opaque_depth_index_cache_opt_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-opaque-depth-index-cache",
            "--optimize-opaque-depth-index-cache-min-gain-pct",
            "12",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        self.assertIn(
            "DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE_MIN_GAIN_PCT=12",
            result.stdout,
        )
        self.assertNotIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertNotIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)

    def test_wrapper_rejects_invalid_opaque_depth_index_cache_min_gain(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-opaque-depth-index-cache-min-gain-pct",
            "bad",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--optimize-opaque-depth-index-cache-min-gain-pct must be",
            result.stderr,
        )

    def test_wrapper_dry_run_includes_index_cache_candidate_frontier_cap_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-opaque-depth-index-cache",
            "--index-cache-candidate-frontier-cap",
            "64",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_INDEX_CACHE_CANDIDATE_FRONTIER_CAP=64", result.stdout)

    def test_wrapper_rejects_invalid_index_cache_candidate_frontier_cap(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--index-cache-candidate-frontier-cap",
            "bad",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--index-cache-candidate-frontier-cap must be", result.stderr)

    def test_wrapper_dry_run_includes_index_cache_candidate_lazy_frontier_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-opaque-depth-index-cache",
            "--index-cache-candidate-lazy-frontier",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_INDEX_CACHE_CANDIDATE_LAZY_FRONTIER=1", result.stdout)

    def test_wrapper_dry_run_includes_index_cache_candidate_bucketed_select_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-opaque-depth-index-cache",
            "--index-cache-candidate-bucketed-select",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_INDEX_CACHE_CANDIDATE_BUCKETED_SELECT=1", result.stdout)

    def test_wrapper_dry_run_includes_index_cache_candidate_upper_bound_gate_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-opaque-depth-index-cache",
            "--index-cache-candidate-upper-bound-gate",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_INDEX_CACHE_CANDIDATE_UPPER_BOUND_GATE=1", result.stdout)

    def test_wrapper_rejects_combined_index_cache_candidate_lazy_and_bucketed(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--index-cache-candidate-lazy-frontier",
            "--index-cache-candidate-bucketed-select",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("mutually exclusive", result.stderr)

    def test_wrapper_dry_run_includes_screen_blend_index_cache_opt_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-screen-blend-index-cache",
            "--optimize-screen-blend-index-cache-min-gain-pct",
            "11",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE=1", result.stdout)
        self.assertIn(
            "DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE_MIN_GAIN_PCT=11",
            result.stdout,
        )
        self.assertIn(
            "warning: --optimize-screen-blend-index-cache is mechanism/profiling-only until a same-input semantic proof is attached.",
            result.stdout,
        )
        self.assertIn(
            "warning: add --semantic-image-policy exact|lsb1",
            result.stdout,
        )
        self.assertNotIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertNotIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)

    def test_wrapper_rejects_invalid_screen_blend_index_cache_min_gain(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-screen-blend-index-cache-min-gain-pct",
            "bad",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--optimize-screen-blend-index-cache-min-gain-pct must be",
            result.stderr,
        )

    def test_wrapper_dry_run_includes_unsafe_nonopaque_apply_index_cache_opt_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-apply-index-cache-opt-candidate",
            "--probe-apply-index-cache-opt-candidate-unsafe-nonopaque",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_UNSAFE_NONOPAQUE=1",
            result.stdout,
        )
        self.assertIn(
            "warning: --probe-apply-index-cache-opt-candidate-unsafe-nonopaque",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_indexed_geometry_dump_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "geometry-dump-dry-run",
            "--no-gputrace",
            "--dump-indexed-geometry",
            "--dump-indexed-geometry-cbufs",
            "--dump-indexed-geometry-max-draws",
            "3",
            "--dump-indexed-geometry-vs",
            "0x7836c3b4c98a465b",
            "--dump-indexed-geometry-ps",
            "0x11cc89f85cc54054",
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
        self.assertIn("DXMT9_DUMP_INDEXED_GEOMETRY_CBUFS=1", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_INDEXED_GEOMETRY_VS=0x7836c3b4c98a465b",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_DUMP_INDEXED_GEOMETRY_PS=0x11cc89f85cc54054",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROWS=60/0\\,60/1",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_depth_attachment_dump_env(self) -> None:
        depth_path = REPO_ROOT / "traces/depth-sidecar/analysis/depth.bin"

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "depth-sidecar",
            "--frame",
            "50",
            "--no-gputrace",
            "--dump-depth-attachment-handle",
            "0x300000100000001",
            "--dump-depth-attachment-seq",
            "50",
            "--dump-depth-attachment-enc",
            "2",
            "--dump-depth-attachment-path",
            "traces/depth-sidecar/analysis/depth.bin",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"depth_attachment_dump: {depth_path}", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE=0x300000100000001",
            result.stdout,
        )
        self.assertIn(f"DXMT9_DUMP_DEPTH_ATTACHMENT_PATH={depth_path}", result.stdout)
        self.assertIn("DXMT9_DUMP_DEPTH_ATTACHMENT_SEQ=50", result.stdout)
        self.assertIn("DXMT9_DUMP_DEPTH_ATTACHMENT_ENC=2", result.stdout)

    def test_wrapper_dry_run_defaults_depth_attachment_dump_path(self) -> None:
        depth_path = (
            REPO_ROOT
            / "traces/app-d3d9-3dmark05-depth-default/analysis/frame50-depth.bin"
        )

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "depth-default",
            "--frame",
            "50",
            "--no-gputrace",
            "--dump-depth-attachment-handle",
            "0x300000100000001",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"depth_attachment_dump: {depth_path}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_DEPTH_ATTACHMENT_PATH={depth_path}", result.stdout)

    def test_wrapper_dry_run_includes_draw_texture_dump_env(self) -> None:
        texture_dir = REPO_ROOT / "traces/texture-sidecar/analysis/textures"

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "texture-sidecar",
            "--frame",
            "60",
            "--no-gputrace",
            "--dump-draw-texture-handles",
            "0x20000010000008d,0x200000100000072",
            "--dump-draw-texture-seq",
            "60",
            "--dump-draw-texture-enc",
            "2",
            "--dump-draw-texture-dir",
            "traces/texture-sidecar/analysis/textures",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"draw_texture_dump_dir: {texture_dir}", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_DRAW_TEXTURE_HANDLES=0x20000010000008d\\,0x200000100000072",
            result.stdout,
        )
        self.assertIn(f"DXMT9_DUMP_DRAW_TEXTURE_DIR={texture_dir}", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE_SEQ=60", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE_ENC=2", result.stdout)

    def test_wrapper_dry_run_defaults_draw_texture_dump_dir(self) -> None:
        texture_dir = (
            REPO_ROOT
            / "traces/app-d3d9-3dmark05-texture-default/analysis/textures"
        )

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "texture-default",
            "--frame",
            "60",
            "--no-gputrace",
            "--dump-draw-texture-handles",
            "0x20000010000008d",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"draw_texture_dump_dir: {texture_dir}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_DRAW_TEXTURE_DIR={texture_dir}", result.stdout)

    def test_wrapper_rejects_draw_texture_dir_without_handles(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dump-draw-texture-dir",
            "traces/texture-sidecar/analysis/textures",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--dump-draw-texture-seq/enc/dir require --dump-draw-texture-handles",
            result.stderr,
            result.stderr,
        )

    def test_wrapper_rejects_depth_attachment_path_without_handle(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dump-depth-attachment-path",
            "traces/depth-sidecar/analysis/depth.bin",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--dump-depth-attachment-seq/enc/path require --dump-depth-attachment-handle",
            result.stderr,
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

    def test_finalizer_partial_stable_frame_proof_accepts_partial_log(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp) / "run"
            output_dir.mkdir()
            output_dir.joinpath("dxmt9.log").write_text(
                "[dxmt9-perf] present_encoded=5 draw_calls=7\n",
                encoding="utf-8",
            )
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "partial-stable-proof",
                "--output-dir",
                str(output_dir),
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--allow-partial-stable-frame-proof",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("warning: missing result.json; using dxmt9.log partial-run counters", result.stderr)
        self.assertIn("missing Xcode encoder counters CSV", result.stderr)
        self.assertNotIn("missing required result.json", result.stderr)

    def test_finalizer_result_json_gate_wins_over_partial_stable_frame_proof(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp) / "run"
            output_dir.mkdir()
            output_dir.joinpath("dxmt9.log").write_text(
                "[dxmt9-perf] present_encoded=5 draw_calls=7\n",
                encoding="utf-8",
            )
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "partial-stable-proof",
                "--output-dir",
                str(output_dir),
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--allow-partial-stable-frame-proof",
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

    def test_wrapper_dry_run_includes_visibility_scout_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "visibility-scout-dry-run",
            "--no-gputrace",
            "--visibility-scout-row",
            "60/2",
            "--visibility-scout-draw-indices",
            "36..37",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_VISIBILITY_SCOUT=1", result.stdout)
        self.assertIn("DXMT9_VISIBILITY_SCOUT_ROW=60/2", result.stdout)
        self.assertIn(
            "traces/app-d3d9-3dmark05-visibility-scout-dry-run/analysis/"
            "frame60-visibility-scout.csv",
            result.stdout,
        )
        self.assertIn("visibility_scout_draw_indices: 36..37", result.stdout)
        self.assertIn(
            "traces/app-d3d9-3dmark05-visibility-scout-dry-run/analysis/"
            "frame60-visibility-scout-summary.md",
            result.stdout,
        )
        self.assertIn(
            "traces/app-d3d9-3dmark05-visibility-scout-dry-run/analysis/"
            "frame60-visibility-scout-summary.csv",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_scoped_force_texture_white_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-force-texture-white-row",
            "50/2",
            "--probe-force-texture-white-classes",
            "depth-read,screen-blend,textured",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_FORCE_TEXTURE_WHITE_ROW=50/2", result.stdout)
        self.assertIn(
            r"DXMT9_PROBE_FORCE_TEXTURE_WHITE_CLASSES=depth-read\,screen-blend\,textured",
            result.stdout,
        )
        self.assertIn(
            "warning: --probe-force-texture-white is diagnostic only",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_force_expand_indexed_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--force-expand-indexed",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_FORCE_EXPAND_INDEXED=1", result.stdout)

    def test_wrapper_dry_run_includes_scoped_force_expand_indexed_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-force-expand-indexed-row",
            "50/2",
            "--probe-force-expand-indexed-classes",
            "depth-read,screen-blend,textured",
            "--probe-indexed-triangle-encoder-draw-min",
            "71",
            "--probe-indexed-triangle-encoder-draw-max",
            "188",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_FORCE_EXPAND_INDEXED_ROW=50/2", result.stdout)
        self.assertIn(
            r"DXMT9_PROBE_FORCE_EXPAND_INDEXED_CLASSES=depth-read\,screen-blend\,textured",
            result.stdout,
        )
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=71", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=188", result.stdout)
        self.assertIn(
            "warning: --probe-force-expand-indexed is diagnostic only",
            result.stdout,
        )

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

    def test_wrapper_dry_run_includes_indexed_triangle_encoder_draw_exclude_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-apply-index-cache-opt-candidate",
            "--probe-reverse-indexed-triangles-row",
            "50/2",
            "--probe-indexed-triangle-encoder-draw-min",
            "14",
            "--probe-indexed-triangle-encoder-draw-max",
            "32",
            "--probe-indexed-triangle-encoder-draw-exclude",
            "18,21",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=14", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=32", result.stdout)
        self.assertIn(
            r"DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_EXCLUDE=18\,21",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_no_alpha_blend_class_filter(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-optimize-indexed-triangles-vertex-cache",
            "--probe-reverse-indexed-triangles-row",
            "50/2",
            "--probe-reverse-indexed-triangles-classes",
            "depth-read,no-alpha-blend,no-scissor,textured",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_OPTIMIZE_INDEXED_TRIANGLES_VERTEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW=50/2", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASSES=depth-read\\,no-alpha-blend\\,no-scissor\\,textured",
            result.stdout,
        )

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

    def test_wrapper_dry_run_omits_metal_capture_layer_env_for_gputrace_by_default(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_METAL_CAPTURE_FRAME=60", result.stdout)
        self.assertIn("DXMT_METAL_CAPTURE_PATH=", result.stdout)
        self.assertNotIn("MTL_CAPTURE_ENABLED=1", result.stdout)

    def test_wrapper_dry_run_includes_metal_capture_layer_env_when_opted_in(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--dry-run",
            env={"DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED": "1"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MTL_CAPTURE_ENABLED=1", result.stdout)

    def test_wrapper_dry_run_forwards_metal_capture_destination(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--dry-run",
            env={"DXMT_3DMARK05_METAL_CAPTURE_DESTINATION": "developerTools"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_METAL_CAPTURE_DESTINATION=developerTools", result.stdout)

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

    def test_finalizer_builds_index_cache_runtime_summary_command(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "index-cache-runtime",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        command_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("index_cache_runtime_cmd:")
        )
        self.assertIn("summarize_index_cache_runtime.py", command_line)
        self.assertIn("3dmark05-perf-encoders.csv", command_line)
        self.assertIn("3dmark05-perf-indexed-probe-draws.csv", command_line)
        self.assertIn("frame60-index-cache-runtime-summary.md", command_line)

    def test_finalizer_builds_indexed_class_proxy_command(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "class-proxy",
            "--class-proxy-top",
            "5",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        command_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("class_proxy_cmd:")
        )
        self.assertIn("analyze_indexed_probe_classes.py", command_line)
        self.assertIn("3dmark05-perf-indexed-probe-draws.csv", command_line)
        self.assertIn("--group row-state-class", command_line)
        self.assertIn("--joined-summary", command_line)
        self.assertIn("frame60-xcode-dxmt-joined-summary.csv", command_line)
        self.assertIn("--top 5", command_line)
        self.assertIn("frame60-indexed-state-class-xcode-proxy.md", command_line)
        self.assertIn("frame60-indexed-state-class-xcode-proxy.csv", command_line)

    def test_finalizer_builds_semantic_image_compare_command(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "semantic-image",
            "--semantic-image-policy",
            "exact",
            "--semantic-image-before",
            "before.ppm",
            "--semantic-image-after",
            "after.ppm",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("semantic_image_compare_cmd:")
        )
        self.assertIn("scripts/tools/compare_experiment_images.py", compare_line)
        self.assertIn("--policy exact", compare_line)
        self.assertIn("--min-before-active-pct 1", compare_line)
        self.assertIn("--min-after-active-pct 1", compare_line)
        self.assertIn("frame60-semantic-image-policy-exact-compare.md", compare_line)
        self.assertIn("frame60-semantic-image-policy-exact-compare.csv", compare_line)
        self.assertIn("frame60-semantic-image-policy-exact-compare.png", compare_line)

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

    def test_finalizer_forwards_tvb_mechanism_proof_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-tvb-mechanism",
                "--baseline-joined",
                str(baseline_joined),
                "--require-tvb-mechanism-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--require-tvb-mechanism-proof", compare_line)

    def test_finalizer_forwards_non_target_hot_row_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-non-target",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/1",
                "--target-row-key",
                "50/3",
                "--max-non-target-gpu-regression-ms",
                "1.0",
                "--max-non-target-vs-buffer-write-regression-mib",
                "16",
                "--max-non-target-vs-invocations-regression-ratio",
                "0.05",
                "--max-non-target-draw-call-delta-ratio",
                "0.05",
                "--max-non-target-vertex-count-delta-ratio",
                "0.05",
                "--max-non-target-triangle-delta-ratio",
                "0.05",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--target-row-key", compare_line)
        self.assertIn("50/1", compare_line)
        self.assertIn("50/3", compare_line)
        self.assertIn("--max-non-target-gpu-regression-ms", compare_line)
        self.assertIn("1.0", compare_line)
        self.assertIn("--max-non-target-vs-buffer-write-regression-mib", compare_line)
        self.assertIn("16", compare_line)
        self.assertIn(
            "--max-non-target-vs-invocations-regression-ratio",
            compare_line,
        )
        self.assertIn("0.05", compare_line)
        self.assertIn("--max-non-target-draw-call-delta-ratio", compare_line)
        self.assertIn("--max-non-target-vertex-count-delta-ratio", compare_line)
        self.assertIn("--max-non-target-triangle-delta-ratio", compare_line)

    def test_finalizer_forwards_target_row_apply_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-target",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--target-row-key",
                "50/1",
                "--require-target-index-cache-miss32-decrease",
                "--require-target-index-cache-opt-miss32-decrease",
                "--require-target-reordered-index-cache-hits",
                "--require-target-vs-buffer-write-decrease",
                "--require-target-vs-invocations-decrease",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--target-row-key", compare_line)
        self.assertIn("50/0", compare_line)
        self.assertIn("50/1", compare_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", compare_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", compare_line)
        self.assertIn("--require-target-reordered-index-cache-hits", compare_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", compare_line)
        self.assertIn("--require-target-vs-invocations-decrease", compare_line)

    def test_finalizer_expands_cache_opt_apply_proof_preset_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "cache-opt-apply-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/1",
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--require-stable-frame-proof", compare_line)
        self.assertIn("--target-row-key", compare_line)
        self.assertIn("50/1", compare_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", compare_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", compare_line)
        self.assertIn("--require-target-vs-invocations-decrease", compare_line)

    def test_finalizer_rejects_cache_opt_apply_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "cache-opt-apply-proof-missing-row",
                "--baseline-joined",
                str(baseline_joined),
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-cache-opt-apply-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_finalizer_rejects_semantic_image_proof_without_semantic_gate(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "semantic-proof-missing-image",
            "--require-semantic-image-proof",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-semantic-image-proof requires --semantic-image-policy",
            result.stderr,
        )

    def test_finalizer_accepts_semantic_image_proof_with_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "semantic-proof",
                "--semantic-image-policy",
                "exact",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--require-semantic-image-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        semantic_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("semantic_image_compare_cmd:")
        )
        self.assertIn("--policy exact", semantic_line)

    def test_finalizer_rejects_screen_blend_cache_proof_without_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "screen-blend-cache-proof-missing-semantic",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires --semantic-image-policy",
            result.stderr,
        )

    def test_finalizer_rejects_opaque_depth_index_cache_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "opaque-depth-cache-proof-missing-target-row",
                "--baseline-joined",
                str(baseline_joined),
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-opaque-depth-index-cache-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_finalizer_expands_opaque_depth_index_cache_proof(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "opaque-depth-cache-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--target-row-key",
                "50/1",
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--require-stable-frame-proof", compare_line)
        self.assertNotIn("--require-target-index-cache-miss32-decrease", compare_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", compare_line)
        self.assertIn("--require-target-reordered-index-cache-hits", compare_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", compare_line)
        self.assertIn("--require-target-vs-invocations-decrease", compare_line)
        self.assertIn("--target-row-key 50/0", compare_line)
        self.assertIn("--target-row-key 50/1", compare_line)

    def test_finalizer_rejects_screen_blend_cache_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "screen-blend-cache-proof-missing-target-row",
                "--baseline-joined",
                str(baseline_joined),
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_finalizer_expands_screen_blend_cache_proof_and_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "screen-blend-cache-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        semantic_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("semantic_image_compare_cmd:")
        )
        self.assertIn("--require-stable-frame-proof", compare_line)
        self.assertNotIn("--require-target-index-cache-miss32-decrease", compare_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", compare_line)
        self.assertIn("--require-target-reordered-index-cache-hits", compare_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", compare_line)
        self.assertIn("--require-target-vs-invocations-decrease", compare_line)
        self.assertIn("--target-row-key", compare_line)
        self.assertIn("50/2", compare_line)
        self.assertIn("--policy lsb1", semantic_line)

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
