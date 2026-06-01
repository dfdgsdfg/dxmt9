#!/usr/bin/env python3
"""Regression tests for 3DMark05 perf counter comparison gates."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "compare_3dmark05_perf_counters.py"


def write_result(path: Path, counters: dict[str, int | float]) -> None:
    path.mkdir(parents=True, exist_ok=True)
    path.joinpath("result.json").write_text(
        json.dumps({"dxmt9_perf_counters": counters}, indent=2),
        encoding="utf-8",
    )


class Compare3DMark05PerfCountersTests(unittest.TestCase):
    def run_compare(self, root: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                str(root / "before"),
                str(root / "after"),
                "--output",
                str(root / "comparison.md"),
                *args,
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_mechanism_gates_pass_when_counters_move(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "present_encoded": 2,
                "draw_calls": 100,
                "commit_chunk_draw_run_submits": 10,
                "commit_chunk_draw_run_records": 20,
                "commit_chunk_draw_run_binding_override_records": 0,
                "commit_chunk_draw_batch_const_upload_passthrough": 0,
                "commit_chunk_draw_submission_batch_submits": 4,
                "commit_chunk_draw_submission_batch_records": 8,
                "commit_chunk_draw_submission_batch_max_records": 3,
                "commit_chunk_draw_run_break_type_const_upload": 50,
                "commit_chunk_draw_run_break_type_const_upload_bytes": 800,
                "commit_chunk_draw_run_break_type_const_upload_registers": 50,
                "commit_chunk_draw_run_break_type_const_vs_f": 40,
                "commit_chunk_draw_run_break_type_const_vs_f_bytes": 640,
                "commit_chunk_draw_run_break_type_const_vs_f_registers": 40,
                "commit_chunk_draw_run_break_type_const_vs_i": 5,
                "commit_chunk_draw_run_break_type_const_vs_i_bytes": 80,
                "commit_chunk_draw_run_break_type_const_vs_i_registers": 5,
                "commit_chunk_draw_run_break_type_const_vs_b": 0,
                "commit_chunk_draw_run_break_type_const_vs_b_bytes": 0,
                "commit_chunk_draw_run_break_type_const_vs_b_registers": 0,
                "commit_chunk_draw_run_break_type_const_ps_f": 5,
                "commit_chunk_draw_run_break_type_const_ps_f_bytes": 80,
                "commit_chunk_draw_run_break_type_const_ps_f_registers": 5,
                "commit_chunk_draw_run_break_type_const_ps_i": 0,
                "commit_chunk_draw_run_break_type_const_ps_i_bytes": 0,
                "commit_chunk_draw_run_break_type_const_ps_i_registers": 0,
                "commit_chunk_draw_run_break_type_const_ps_b": 0,
                "commit_chunk_draw_run_break_type_const_ps_b_bytes": 0,
                "commit_chunk_draw_run_break_type_const_ps_b_registers": 0,
                "commit_chunk_draw_run_break_state_delta": 25,
                "commit_chunk_draw_run_break_state_delta_stream_only": 5,
                "commit_chunk_draw_run_break_state_delta_mixed": 20,
                "commit_chunk_draw_run_break_state_delta_mixed_group2": 18,
                "commit_chunk_draw_run_break_state_delta_stream_ib_only": 15,
                "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_ib": 18,
                "commit_chunk_draw_delta_stream": 50,
                "commit_chunk_draw_delta_ib": 45,
                "encode_draw_cpu_ms": 100.0,
            })
            write_result(root / "after", {
                "present_encoded": 2,
                "draw_calls": 100,
                "commit_chunk_draw_run_submits": 10,
                "commit_chunk_draw_run_records": 40,
                "commit_chunk_draw_run_binding_override_records": 12,
                "commit_chunk_draw_batch_const_upload_passthrough": 5,
                "commit_chunk_draw_submission_batch_submits": 5,
                "commit_chunk_draw_submission_batch_records": 25,
                "commit_chunk_draw_submission_batch_max_records": 7,
                "commit_chunk_draw_run_break_type_const_upload": 40,
                "commit_chunk_draw_run_break_type_const_upload_bytes": 640,
                "commit_chunk_draw_run_break_type_const_upload_registers": 40,
                "commit_chunk_draw_run_break_type_const_vs_f": 30,
                "commit_chunk_draw_run_break_type_const_vs_f_bytes": 480,
                "commit_chunk_draw_run_break_type_const_vs_f_registers": 30,
                "commit_chunk_draw_run_break_type_const_vs_i": 4,
                "commit_chunk_draw_run_break_type_const_vs_i_bytes": 64,
                "commit_chunk_draw_run_break_type_const_vs_i_registers": 4,
                "commit_chunk_draw_run_break_type_const_vs_b": 0,
                "commit_chunk_draw_run_break_type_const_vs_b_bytes": 0,
                "commit_chunk_draw_run_break_type_const_vs_b_registers": 0,
                "commit_chunk_draw_run_break_type_const_ps_f": 6,
                "commit_chunk_draw_run_break_type_const_ps_f_bytes": 96,
                "commit_chunk_draw_run_break_type_const_ps_f_registers": 6,
                "commit_chunk_draw_run_break_type_const_ps_i": 0,
                "commit_chunk_draw_run_break_type_const_ps_i_bytes": 0,
                "commit_chunk_draw_run_break_type_const_ps_i_registers": 0,
                "commit_chunk_draw_run_break_type_const_ps_b": 0,
                "commit_chunk_draw_run_break_type_const_ps_b_bytes": 0,
                "commit_chunk_draw_run_break_type_const_ps_b_registers": 0,
                "commit_chunk_draw_run_break_state_delta": 20,
                "commit_chunk_draw_run_break_state_delta_stream_only": 4,
                "commit_chunk_draw_run_break_state_delta_mixed": 16,
                "commit_chunk_draw_run_break_state_delta_mixed_group2": 14,
                "commit_chunk_draw_run_break_state_delta_stream_ib_only": 12,
                "commit_chunk_draw_run_break_state_delta_mixed_pair_stream_ib": 14,
                "commit_chunk_draw_delta_stream": 40,
                "commit_chunk_draw_delta_ib": 35,
                "encode_draw_cpu_ms": 90.0,
            })

            result = self.run_compare(
                root,
                "--require-draw-run-records-increase",
                "--require-draw-run-records-per-submit-increase",
                "--require-binding-overrides-present",
                "--require-const-upload-passthrough-present",
                "--require-const-upload-break-bytes-decrease",
                "--max-const-upload-break-count-ratio",
                "1.10",
                "--require-encode-draw-cpu-decrease",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            report = root.joinpath("comparison.md").read_text(encoding="utf-8")
            self.assertIn("binding_override_records_per_draw_run_record", report)
            self.assertIn("draw_submission_batch_records_per_submit", report)
            self.assertIn("const_upload_breaks_per_draw", report)
            self.assertIn("const_upload_passthrough_per_present", report)
            self.assertIn("const_upload_subtype_coverage_pct", report)
            self.assertIn("const_vs_f_share_pct", report)
            self.assertIn("const_vs_f_byte_share_pct", report)
            self.assertIn("state_delta_mixed_share_pct", report)
            self.assertIn("state_delta_mixed_pair_stream_ib_share_pct", report)
            self.assertIn("stream_deltas_per_draw", report)
            self.assertIn("ib_deltas_per_draw", report)
            self.assertIn("| `const_uploads_per_state_delta_break` | `2.000` | `2.000`", report)
            self.assertIn("| `const_upload_break_bytes_per_break` | `16.000` | `16.000`", report)
            self.assertIn("| `const_upload_registers_per_break` | `1.000` | `1.000`", report)
            self.assertIn("| `const_upload_subtype_coverage_pct` | `100.000` | `100.000`", report)
            self.assertIn("| `draw_submission_batch_records_per_submit` | `2.000` | `5.000`", report)

    def test_const_upload_byte_and_count_gates_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "commit_chunk_draw_run_break_type_const_upload": 50,
                "commit_chunk_draw_run_break_type_const_upload_bytes": 800,
            })
            write_result(root / "after", {
                "commit_chunk_draw_run_break_type_const_upload": 70,
                "commit_chunk_draw_run_break_type_const_upload_bytes": 900,
            })

            result = self.run_compare(
                root,
                "--require-const-upload-break-bytes-decrease",
                "--max-const-upload-break-count-ratio",
                "1.10",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "commit_chunk_draw_run_break_type_const_upload_bytes did not decrease",
                result.stderr,
            )
            self.assertIn(
                "commit_chunk_draw_run_break_type_const_upload exceeded count ratio",
                result.stderr,
            )

    def test_output_parent_directory_is_created(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "commit_chunk_draw_run_submits": 1,
                "commit_chunk_draw_run_records": 1,
            })
            write_result(root / "after", {
                "commit_chunk_draw_run_submits": 1,
                "commit_chunk_draw_run_records": 2,
            })

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root / "before"),
                    str(root / "after"),
                    "--output",
                    str(root / "nested" / "reports" / "comparison.md"),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(root.joinpath("nested", "reports", "comparison.md").exists())

    def test_mechanism_gates_fail_when_counters_do_not_move(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {
                "commit_chunk_draw_run_submits": 10,
                "commit_chunk_draw_run_records": 20,
                "commit_chunk_draw_run_binding_override_records": 0,
                "commit_chunk_draw_batch_const_upload_passthrough": 0,
                "encode_draw_cpu_ms": 100.0,
            })
            write_result(root / "after", {
                "commit_chunk_draw_run_submits": 10,
                "commit_chunk_draw_run_records": 20,
                "commit_chunk_draw_run_binding_override_records": 0,
                "commit_chunk_draw_batch_const_upload_passthrough": 0,
                "encode_draw_cpu_ms": 100.0,
            })

            result = self.run_compare(
                root,
                "--require-draw-run-records-increase",
                "--require-draw-run-records-per-submit-increase",
                "--require-binding-overrides-present",
                "--require-const-upload-passthrough-present",
                "--require-draw-submission-batch-present",
                "--require-encode-draw-cpu-decrease",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("commit_chunk_draw_run_records did not increase", result.stderr)
            self.assertIn("draw_run_records_per_submit did not increase", result.stderr)
            self.assertIn("binding_override_records stayed zero", result.stderr)
            self.assertIn("const_upload_passthrough stayed zero", result.stderr)
            self.assertIn("commit_chunk_draw_submission_batch counters stayed zero", result.stderr)
            self.assertIn("encode_draw_cpu_ms did not decrease", result.stderr)

    def test_draw_submission_batch_gate_fails_when_counters_are_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_result(root / "before", {})
            write_result(root / "after", {
                "commit_chunk_draw_submission_batch_submits": 0,
                "commit_chunk_draw_submission_batch_records": 0,
                "commit_chunk_draw_submission_batch_max_records": 0,
            })

            result = self.run_compare(
                root,
                "--require-draw-submission-batch-present",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "commit_chunk_draw_submission_batch counters stayed zero",
                result.stderr,
            )


if __name__ == "__main__":
    unittest.main()
