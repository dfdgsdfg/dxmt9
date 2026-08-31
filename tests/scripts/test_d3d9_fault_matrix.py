"""Tests for the bounded, one-fault-per-process conformance driver."""

import sys
import unittest
import os
import tempfile
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.tools import run_d3d9_fault_matrix as matrix
from scripts.tools import run_d3d9_conformance as runner


class FaultMatrixTests(unittest.TestCase):
    def test_recorder_matrix_is_typed_and_complete(self):
        self.assertEqual(matrix.matrix_points("recorder", None),
                         list(matrix.RECORDER_FAULT_POINTS))

    def test_rejects_multi_fault_selector(self):
        with self.assertRaises(ValueError):
            matrix.matrix_points("recorder", ["bridge_pre,reset"])

    def test_recorder_command_is_singleton_and_canonical(self):
        args = matrix.parse_args([
            "--fault-kind", "recorder", "--fault", "bridge_pre=0x80004005",
            "--runner-arg=--exe", "--runner-arg", "suite.exe",
        ])
        command = matrix.command_for_point(args, "bridge_pre=0x80004005")
        self.assertIn(str(matrix.DEFAULT_RUNNER), command)
        self.assertIn("--chunk-size", command)
        self.assertEqual(command[command.index("--chunk-size") + 1], "1")
        self.assertEqual(command[command.index("--start") + 1], "0")
        self.assertEqual(command[command.index("--end") + 1], "0")
        self.assertIn("--pe-recorder-fault", command)
        self.assertIn("--aux-exe", command)
        self.assertIn("d3d9_recorder_fault_x64.exe", command)
        output = command[command.index("--output") + 1]
        self.assertTrue(output.endswith("recorder-bridge_pre_0x80004005.json"))

    def test_stateblock_command_uses_one_aux_process(self):
        args = matrix.parse_args(["--fault-kind", "stateblock"])
        command = matrix.command_for_point(args, "bridge_pre")
        self.assertIn("--stateblock-fault", command)
        self.assertIn("--aux-exe", command)
        self.assertNotIn("--skip-aux", command)
        self.assertEqual(command[command.index("--start") + 1], "0")
        self.assertEqual(command[command.index("--end") + 1], "0")

    def test_recorder_command_can_reuse_clean_room_aux_fixture(self):
        args = matrix.parse_args([
            "--fault-kind", "recorder", "--aux-exe", "d3d9_stateblock_x64.exe",
        ])
        command = matrix.command_for_point(args, "retain_acquire")
        self.assertIn("--aux-exe", command)
        self.assertNotIn("--skip-aux", command)

    def test_recorder_command_selects_x86_clean_room_fixture(self):
        args = matrix.parse_args([
            "--fault-kind", "recorder", "--pe-arch", "x86",
        ])
        command = matrix.command_for_point(args, "retain_acquire=1")
        self.assertIn("d3d9_recorder_fault_x86.exe", command)
        self.assertEqual(command[command.index("--end") + 1], "0")

    def test_canonical_runner_exports_fault_environment(self):
        args = matrix.parse_args(["--fault", "bridge_pre"])
        args.prefix = Path("/tmp/dxmt9-fault-matrix-prefix")
        args.pe_recorder_fault = "bridge_pre"
        args.stateblock_fault = "capture_pre"
        with patch.dict(os.environ, {}, clear=True):
            env = runner.build_env(args)
        self.assertEqual(env["DXMT9_PE_RECORDER_FAULT"], "bridge_pre")
        self.assertEqual(env["DXMT9_PE_STATEBLOCK_FAULT"], "capture_pre")

    def test_matrix_result_file_fails_closed_on_non_pass_verdict(self):
        with tempfile.TemporaryDirectory() as directory:
            result = Path(directory) / "result.json"
            result.write_text('{"run_fault": "pass"}\n')
            self.assertTrue(matrix.result_file_passes(result))
            result.write_text('{"run_fault": "fail"}\n')
            self.assertFalse(matrix.result_file_passes(result))
            result.write_text('{"run_fault": "skip"}\n')
            self.assertFalse(matrix.result_file_passes(result))
            result.write_text('{}\n')
            self.assertFalse(matrix.result_file_passes(result))
            result.write_text('not-json\n')
            self.assertFalse(matrix.result_file_passes(result))
            self.assertFalse(matrix.result_file_passes(result.with_suffix(".missing")))


if __name__ == "__main__":
    unittest.main()
