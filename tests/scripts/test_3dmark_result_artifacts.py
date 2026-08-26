"""Contract tests for automatic 3DMark result-file artifact collection."""

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.run_apps import run_experiment  # noqa: E402
from scripts.run_apps import benchmark_result_artifacts  # noqa: E402


class ThreeDMarkResultArtifactTests(unittest.TestCase):
    def test_non_3dmark_app_has_no_capture_plan(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            env: dict[str, str] = {}
            plan = run_experiment.prepare_benchmark_result_capture(
                app_name="app-d3d9-sfiv-benchmark",
                workdir=root,
                prefix=root / "prefix",
                output_name="sfiv",
                env=env,
            )

        self.assertIsNone(plan)
        self.assertEqual(env, {})

    def test_auto_request_is_unique_basename_in_workdir(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            workdir = root / "3DMark06"
            workdir.mkdir()
            prefix = root / "prefix"
            env: dict[str, str] = {}

            plan = run_experiment.prepare_benchmark_result_capture(
                app_name="app-d3d9-3dmark06",
                workdir=workdir,
                prefix=prefix,
                output_name="app-d3d9-3dmark06-hdr run",
                env=env,
            )

            self.assertIsNotNone(plan)
            assert plan is not None
            requested = env["DXMT_3DMARK06_RESULT_FILE"]
            self.assertRegex(
                requested,
                r"^app-d3d9-3dmark06-hdr-run-\d+-\d+\.3dr$",
            )
            self.assertEqual(plan.requested_mode, "auto")
            self.assertEqual(plan.requested_path, (workdir / requested).absolute())

    def test_explicit_windows_result_path_maps_into_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            prefix = root / "prefix"
            env = {"DXMT_3DMARK05_RESULT_FILE": r"C:\\results\\gt1.3dr"}

            plan = run_experiment.prepare_benchmark_result_capture(
                app_name="app-d3d9-3dmark05",
                workdir=root / "3DMark05",
                prefix=prefix,
                output_name="gt1",
                env=env,
            )

            self.assertIsNotNone(plan)
            assert plan is not None
            self.assertEqual(plan.requested_mode, "explicit")
            self.assertEqual(
                plan.requested_path,
                (prefix / "drive_c" / "results" / "gt1.3dr").absolute(),
            )

    def test_collects_only_created_or_modified_regular_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            workdir = root / "3DMark05"
            users = root / "prefix" / "drive_c" / "users" / "runner"
            output = root / "artifact"
            workdir.mkdir(parents=True)
            users.mkdir(parents=True)
            unchanged = workdir / "unchanged.3dr"
            modified = workdir / "requested.3dr"
            ignored_target = workdir / "ignored-target.bin"
            unchanged.write_bytes(b"old unchanged")
            modified.write_bytes(b"old result")
            ignored_target.write_bytes(b"not a result")
            (workdir / "ignored.3dr").symlink_to(ignored_target)
            env = {"DXMT_3DMARK05_RESULT_FILE": modified.name}
            plan = run_experiment.prepare_benchmark_result_capture(
                app_name="app-d3d9-3dmark05",
                workdir=workdir,
                prefix=root / "prefix",
                output_name="gt1",
                env=env,
            )
            assert plan is not None

            modified.write_bytes(b"new requested result")
            (workdir / "same.3dr").write_bytes(b"workdir result")
            (users / "same.3dr").write_bytes(b"users result")
            payload = run_experiment.collect_benchmark_result_files(
                plan,
                output_dir=output,
            )

            self.assertEqual(payload["status"], "captured")
            self.assertEqual(payload["found"], 3)
            self.assertFalse(payload["missing_requested"])
            self.assertEqual(payload["errors"], [])
            changes = {Path(item["source"]).name: item["change"] for item in payload["files"]}
            self.assertEqual(changes["requested.3dr"], "modified")
            self.assertEqual(changes["same.3dr"], "created")
            self.assertNotIn("unchanged.3dr", changes)
            self.assertNotIn("ignored.3dr", changes)
            artifact_names = [Path(item["artifact"]).name for item in payload["files"]]
            self.assertEqual(len(artifact_names), len(set(artifact_names)))
            for item in payload["files"]:
                artifact = output / item["artifact"]
                self.assertTrue(artifact.is_file())
                self.assertEqual(
                    hashlib.sha256(artifact.read_bytes()).hexdigest(),
                    item["sha256"],
                )
            self.assertEqual(list((output / "benchmark-results").glob("*.tmp")), [])

    def test_missing_requested_is_observed_without_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            workdir = root / "3DMark06"
            workdir.mkdir()
            env = {"DXMT_3DMARK06_RESULT_FILE": "missing.3dr"}
            plan = run_experiment.prepare_benchmark_result_capture(
                app_name="app-d3d9-3dmark06",
                workdir=workdir,
                prefix=root / "prefix",
                output_name="hdr",
                env=env,
            )
            assert plan is not None

            payload = run_experiment.collect_benchmark_result_files(
                plan,
                output_dir=root / "artifact",
            )

            self.assertEqual(payload["status"], "not_emitted")
            self.assertEqual(payload["found"], 0)
            self.assertTrue(payload["missing_requested"])
            self.assertEqual(payload["files"], [])
            self.assertEqual(payload["errors"], [])

    def test_copy_error_is_recorded_without_partial_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            workdir = root / "3DMark05"
            workdir.mkdir()
            env = {"DXMT_3DMARK05_RESULT_FILE": "requested.3dr"}
            plan = run_experiment.prepare_benchmark_result_capture(
                app_name="app-d3d9-3dmark05",
                workdir=workdir,
                prefix=root / "prefix",
                output_name="gt1",
                env=env,
            )
            assert plan is not None
            (workdir / "requested.3dr").write_bytes(b"result")
            output = root / "artifact"

            with mock.patch.object(
                benchmark_result_artifacts,
                "_atomic_copy_benchmark_result",
                side_effect=OSError("copy failed"),
            ):
                payload = run_experiment.collect_benchmark_result_files(
                    plan,
                    output_dir=output,
                )

            self.assertEqual(payload["status"], "error")
            self.assertEqual(payload["found"], 0)
            self.assertTrue(payload["missing_requested"])
            self.assertEqual(len(payload["errors"]), 1)
            self.assertFalse((output / "benchmark-results").exists())


if __name__ == "__main__":
    unittest.main()
