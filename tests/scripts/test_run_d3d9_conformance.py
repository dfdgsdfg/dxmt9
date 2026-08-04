"""Regression tests for the D3D9 conformance runner's chunk verdicts."""

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.tools import run_d3d9_conformance as runner


class ChunkProcessFailureTests(unittest.TestCase):
    def test_run_chunk_preserves_nonzero_process_status(self):
        args = SimpleNamespace(wine=Path("/wine"), exe=Path("/suite.exe"))
        completed = SimpleNamespace(stdout="", stderr="loader failed\n",
                                    returncode=17)

        with patch.object(runner, "build_env", return_value={}), \
                patch.object(runner.subprocess, "run", return_value=completed):
            verdicts, output, timed_out, returncode = runner.run_chunk(
                args, 0, 1, 1.0)

        self.assertEqual(verdicts, {})
        self.assertEqual(output, "loader failed\n")
        self.assertFalse(timed_out)
        self.assertEqual(returncode, 17)

    def test_nonzero_process_exit_fails_only_missing_results(self):
        results = {"reported": "pass"}

        runner.mark_failed_process_missing_results(
            ["reported", "missing"], results,
            timed_out=False, returncode=1)

        self.assertEqual(results, {"reported": "pass", "missing": "fail"})

    def test_clean_process_exit_leaves_missing_results_for_retry_policy(self):
        results = {"reported": "pass"}

        runner.mark_failed_process_missing_results(
            ["reported", "missing"], results,
            timed_out=False, returncode=0)

        self.assertEqual(results, {"reported": "pass"})

    def test_timeout_keeps_timeout_classification_separate(self):
        results = {}

        runner.mark_failed_process_missing_results(
            ["missing"], results, timed_out=True, returncode=None)

        self.assertEqual(results, {})


if __name__ == "__main__":
    unittest.main()
