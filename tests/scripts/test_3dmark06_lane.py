"""Contract tests for the external 3DMark06 experiment lane."""

import os
import subprocess
import sys
import tomllib
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = REPO_ROOT / "experiments/launchers/app-d3d9-3dmark06.sh"
CATALOGUE = REPO_ROOT / "experiments/CATALOGUE.toml"
RUNNER = REPO_ROOT / "scripts/run_apps/run_experiment.py"


def dry_run(**overrides: str) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.update(
        {
            "DXMT_3DMARK06_DRY_RUN": "1",
            "DXMT_EXPERIMENT_BINARY": "/tmp/3DMark06 Install/3DMark06.exe",
            "DXMT_EXPERIMENT_WINE_BIN": "/tmp/fake wine/bin/wine",
        }
    )
    env.update(overrides)
    return subprocess.run(
        ["bash", str(LAUNCHER)],
        cwd=REPO_ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


class ThreeDMark06LaneTests(unittest.TestCase):
    def test_catalogue_declares_bounded_external_d3d9_lane(self) -> None:
        with CATALOGUE.open("rb") as handle:
            entries = tomllib.load(handle)["app"]
        entry = next(item for item in entries if item["name"] == "app-d3d9-3dmark06")

        self.assertEqual(entry["source_kind"], "external-application")
        self.assertEqual(entry["license_scope"], "external-not-vendored")
        self.assertEqual(entry["wine_id"], "sikarugir-cx-24.0.7")
        self.assertTrue(entry["require_positive_timeout"])
        self.assertGreater(entry["run_timeout_sec"], 0)
        self.assertTrue({"d3d9", "sm2", "sm3", "hdr"}.issubset(entry["features"]))
        self.assertNotIn("prefixs/", entry["binary"])
        self.assertTrue(os.access(LAUNCHER, os.X_OK))

    def test_default_plan_is_one_sm2_graphics_test(self) -> None:
        result = dry_run()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[3dmark-lane] product=06 lane=gt1 source=preset", result.stdout)
        self.assertIn("args=-gt1 -nosplash -nosysteminfo -noscreens", result.stdout)
        self.assertIn("workdir=/tmp/3DMark06 Install", result.stdout)

    def test_named_presets_cover_every_supported_lane_class(self) -> None:
        expected = {
            "gt2": "-gt2",
            "sm2": "-gt1 -gt2",
            "hdr1": "-hdr1",
            "hdr2": "-hdr2",
            "hdr": "-hdr1 -hdr2",
            "graphics": "-gtall",
            "cpu1": "-cpu1",
            "cpu2": "-cpu2",
            "cpu": "-cpuall",
            "score": "-gtall -cpuall",
            "feature": "-featureall",
            "batch": "-batchall",
            "all": "-gtall -cpuall -featureall -batchall",
        }

        for lane, selection in expected.items():
            with self.subTest(lane=lane):
                result = dry_run(DXMT_3DMARK06_LANE=lane)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(
                    f"[3dmark-lane] product=06 lane={lane} source=preset",
                    result.stdout,
                )
                self.assertIn(
                    f"args={selection} -nosplash -nosysteminfo -noscreens",
                    result.stdout,
                )

    def test_raw_args_override_is_reported_as_custom(self) -> None:
        result = dry_run(
            DXMT_3DMARK06_LANE="cpu",
            DXMT_3DMARK06_ARGS="-hdr1 -nosplash",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[3dmark-lane] product=06 lane=custom source=args", result.stdout)
        self.assertIn("args=-hdr1 -nosplash", result.stdout)
        self.assertNotIn("args=-gt1", result.stdout)

    def test_invalid_named_preset_fails_before_staging(self) -> None:
        result = dry_run(DXMT_3DMARK06_LANE="gt3")

        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown 3DMark06 lane 'gt3'", result.stderr)

    def test_result_file_is_the_last_positional_argument(self) -> None:
        result = dry_run(
            DXMT_3DMARK06_LANE="gt2",
            DXMT_3DMARK06_RESULT_FILE="dxmt9_gt2.3dr",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        args_line = next(
            line for line in result.stdout.splitlines() if line.startswith("[3dmark06-dry-run] args=")
        )
        self.assertTrue(args_line.endswith(" dxmt9_gt2.3dr"), args_line)

    def test_catalogue_runner_rejects_disabled_timeout(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(RUNNER),
                "run",
                "app-d3d9-3dmark06",
                "--timeout",
                "0",
            ],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("app-d3d9-3dmark06: --timeout must be positive", result.stderr)


if __name__ == "__main__":
    unittest.main()
