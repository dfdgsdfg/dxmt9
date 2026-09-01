"""Regression tests for the D3D9 conformance runner's chunk verdicts."""

import sys
import tempfile
import unittest
import os
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

    def test_recorder_fault_fixture_environment_is_deterministic(self):
        args = SimpleNamespace(
            prefix=Path("/tmp/dxmt9-fault-prefix"),
            pe_recorder_fault="capacity_pre_reserve",
            stateblock_fault=None,
            output=Path("/tmp/dxmt9-fault-matrix/recorder-capacity.json"),
        )
        with patch.dict(os.environ, {
            "DXMT9_PE_CHUNK_MAX_BYTES": "999",
            "DXMT9_RENDER_TAPE_CAPTURE": "1",
            "DXMT9_RENDER_TAPE_OUTPUT_ROOT": "/tmp/stale-capture",
        }, clear=True):
            env = runner.build_env(args)
        self.assertEqual(env["DXMT9_PE_CHUNK_MAX_BYTES"], "50")
        self.assertNotIn("DXMT9_RENDER_TAPE_CAPTURE", env)
        self.assertNotIn("DXMT9_RENDER_TAPE_OUTPUT_ROOT", env)

    def test_recorder_capture_root_is_pe_absolute_wine_path(self):
        args = SimpleNamespace(
            prefix=Path("/tmp/dxmt9-fault-prefix"),
            pe_recorder_fault="capture_throw",
            stateblock_fault=None,
            output=Path("/tmp/dxmt9-fault-matrix/recorder-capture.json"),
        )
        with patch.dict(os.environ, {}, clear=True):
            env = runner.build_env(args)
        self.assertEqual(env["DXMT9_RENDER_TAPE_CAPTURE"], "1")
        expected_root = Path(
            "/tmp/dxmt9-fault-matrix/recorder-capture-capture").resolve()
        self.assertEqual(
            env["DXMT9_RENDER_TAPE_OUTPUT_ROOT"],
            "Z:" + str(expected_root).replace("/", "\\"))

    def test_recorder_aux_requires_reached_marker(self):
        args = SimpleNamespace(wine=Path("/wine"),
                               pe_recorder_fault="bridge_pre")
        completed = SimpleNamespace(stdout="recorder_fault_matrix: passed\n",
                                    stderr="", returncode=0)
        with patch.object(runner, "build_env", return_value={}), \
                patch.object(runner.subprocess, "run", return_value=completed):
            verdicts, _, timed_out = runner.run_aux_exe(
                args, Path("/fixture.exe"), ["run_fault"], 1.0)
        self.assertFalse(timed_out)
        self.assertEqual(verdicts, {"run_fault": "fail"})

    def test_recorder_aux_accepts_reached_marker(self):
        args = SimpleNamespace(wine=Path("/wine"),
                               pe_recorder_fault="bridge_pre")
        completed = SimpleNamespace(
            stdout=("DXMT9_PE_RECORDER_FAULT_CONSUMED=bridge_pre\n"
                    "REACHED:recorder_fault:bridge_pre\n"), stderr="",
            returncode=0)
        with patch.object(runner, "build_env", return_value={}), \
                patch.object(runner.subprocess, "run", return_value=completed):
            verdicts, _, timed_out = runner.run_aux_exe(
                args, Path("/fixture.exe"), ["run_fault"], 1.0)
        self.assertFalse(timed_out)
        self.assertEqual(verdicts, {"run_fault": "pass"})

    def test_recorder_aux_rejects_missing_consumption_receipt(self):
        args = SimpleNamespace(wine=Path("/wine"),
                               pe_recorder_fault="bridge_pre")
        completed = SimpleNamespace(
            stdout="REACHED:recorder_fault:bridge_pre\n", stderr="",
            returncode=0)
        with patch.object(runner, "build_env", return_value={}), \
                patch.object(runner.subprocess, "run", return_value=completed):
            verdicts, _, timed_out = runner.run_aux_exe(
                args, Path("/fixture.exe"), ["run_fault"], 1.0)
        self.assertFalse(timed_out)
        self.assertEqual(verdicts, {"run_fault": "fail"})

    def test_recorder_aux_rejects_wrong_consumption_receipt(self):
        args = SimpleNamespace(wine=Path("/wine"),
                               pe_recorder_fault="bridge_pre")
        completed = SimpleNamespace(
            stdout=("DXMT9_PE_RECORDER_FAULT_CONSUMED=bridge_entered\n"
                    "REACHED:recorder_fault:bridge_pre\n"), stderr="",
            returncode=0)
        with patch.object(runner, "build_env", return_value={}), \
                patch.object(runner.subprocess, "run", return_value=completed):
            verdicts, _, timed_out = runner.run_aux_exe(
                args, Path("/fixture.exe"), ["run_fault"], 1.0)
        self.assertFalse(timed_out)
        self.assertEqual(verdicts, {"run_fault": "fail"})

    def test_recorder_aux_preserves_explicit_runtime_skip(self):
        args = SimpleNamespace(wine=Path("/wine"),
                               pe_recorder_fault="capture_throw")
        completed = SimpleNamespace(
            stdout="SKIP:run_fault: capture unsupported\n",
            stderr="", returncode=77)
        with patch.object(runner, "build_env", return_value={}), \
                patch.object(runner.subprocess, "run", return_value=completed):
            verdicts, _, timed_out = runner.run_aux_exe(
                args, Path("/fixture.exe"), ["run_fault"], 1.0)
        self.assertFalse(timed_out)
        self.assertEqual(verdicts, {"run_fault": "skip"})


class BuiltinStagingArchitectureTests(unittest.TestCase):
    @staticmethod
    def _pe(machine: int) -> bytes:
        data = bytearray(0x80)
        data[:2] = b"MZ"
        data[0x3c:0x40] = (0x40).to_bytes(4, "little")
        data[0x40:0x44] = b"PE\0\0"
        data[0x44:0x46] = machine.to_bytes(2, "little")
        return bytes(data)

    def test_detects_x86_machine(self):
        data = bytearray(0x80)
        data[:2] = b"MZ"
        data[0x3c:0x40] = (0x40).to_bytes(4, "little")
        data[0x40:0x44] = b"PE\0\0"
        data[0x44:0x46] = (0x014c).to_bytes(2, "little")
        with tempfile.NamedTemporaryFile() as pe:
            pe.write(data)
            pe.flush()
            self.assertEqual(runner.detect_pe_arch(Path(pe.name)), "x86")

    def test_detects_x64_machine(self):
        data = self._pe(0x8664)
        with tempfile.NamedTemporaryFile() as pe:
            pe.write(data)
            pe.flush()
            self.assertEqual(runner.detect_pe_arch(Path(pe.name)), "x64")

    def test_stages_canonical_pair_not_test_directory_copies(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build-win32-x64-builtin"
            canonical_win32 = build / "src/win32"
            canonical_winemetal = build / "src/winemetal"
            canonical_win32.mkdir(parents=True)
            canonical_winemetal.mkdir(parents=True)
            canonical_d3d9 = canonical_win32 / "d3d9.dll"
            canonical_runtime = canonical_winemetal / "winemetal_dxmt9.dll"
            canonical_d3d9.write_bytes(self._pe(0x8664) + b"canonical-d3d9")
            canonical_runtime.write_bytes(self._pe(0x8664) + b"canonical-runtime")
            for dll in (canonical_d3d9, canonical_runtime):
                stamp = Path(str(dll) + ".postproc")
                stamp.write_text("postprocessed\n")
                os.utime(stamp, ns=(dll.stat().st_atime_ns,
                                    dll.stat().st_mtime_ns + 1))

            test_dir = build / "tests/conformance/d3d9"
            test_dir.mkdir(parents=True)
            (test_dir / "d3d9.dll").write_bytes(b"stale-test-copy")
            (test_dir / "winemetal_dxmt9.dll").write_bytes(b"stale-test-copy")
            wine = root / "wine/bin/wine"
            (root / "wine/lib/wine/x86_64-windows").mkdir(parents=True)
            (root / "wine/lib/wine/x86_64-unix").mkdir(parents=True)
            wine.parent.mkdir(parents=True, exist_ok=True)
            wine.write_bytes(b"wine")
            provider = root / "provider/winemetal_dxmt9.so"
            provider.parent.mkdir()
            provider.write_bytes(b"provider")
            args = SimpleNamespace(
                exe=test_dir / "d3d9_stateblock_x64.exe",
                pe_dll_dir=build,
                pe_arch="auto",
                wine=wine,
                winemetal_so=provider,
            )

            runner.stage_builtin_pe_dlls(args)
            staged_d3d9 = root / "wine/lib/wine/x86_64-windows/d3d9.dll"
            staged_runtime = root / "wine/lib/wine/x86_64-windows/winemetal_dxmt9.dll"
            self.assertEqual(staged_d3d9.read_bytes(), canonical_d3d9.read_bytes())
            self.assertEqual(staged_runtime.read_bytes(), canonical_runtime.read_bytes())
            self.assertEqual(args.staged_pe_arch, "x64")
            self.assertEqual(args.staged_build[str(staged_d3d9)]["source"],
                             str(canonical_d3d9.resolve()))

    def test_rejects_old_postprocess_stamp(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            d3d9_dir = root / "src/win32"
            runtime_dir = root / "src/winemetal"
            d3d9_dir.mkdir(parents=True)
            runtime_dir.mkdir(parents=True)
            for dll in (d3d9_dir / "d3d9.dll", runtime_dir / "winemetal_dxmt9.dll"):
                dll.write_bytes(self._pe(0x8664))
                Path(str(dll) + ".postproc").write_text("old\n")
                os.utime(Path(str(dll) + ".postproc"),
                         ns=(dll.stat().st_atime_ns, dll.stat().st_mtime_ns - 1))
            args = SimpleNamespace(exe=root / "test.exe", pe_dll_dir=root,
                                   pe_arch="auto")
            with self.assertRaises(SystemExit):
                runner.canonical_pe_dlls(args)


if __name__ == "__main__":
    unittest.main()
