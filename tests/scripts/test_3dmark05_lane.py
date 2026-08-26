"""Contract tests for canonical 3DMark05 lanes and lane identity output."""

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.run_apps import run_experiment  # noqa: E402


LAUNCHER = REPO_ROOT / "experiments/launchers/app-d3d9-3dmark05.sh"


class ThreeDMark05LaneTests(unittest.TestCase):
    def run_direct(self, **overrides: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            prefix = root / "prefix"
            wine_root = root / "wine"
            exe = (
                prefix
                / "drive_c/Program Files (x86)/Futuremark/3DMark05/3DMark05.exe"
            )
            wine_bin = wine_root / "bin/wine"
            wineserver = wine_root / "bin/wineserver"
            exe.parent.mkdir(parents=True)
            wine_bin.parent.mkdir(parents=True)
            exe.write_bytes(b"")
            wine_bin.write_text(
                "#!/bin/sh\nprintf 'fake-wine'\n"
                "for arg in \"$@\"; do printf ' <%s>' \"$arg\"; done\n"
                "printf '\\n'\n"
            )
            wineserver.write_text("#!/bin/sh\nexit 0\n")
            wine_bin.chmod(wine_bin.stat().st_mode | stat.S_IXUSR)
            wineserver.chmod(wineserver.stat().st_mode | stat.S_IXUSR)

            env = os.environ.copy()
            env.update(
                {
                    "DXMT_EXPERIMENT_NAME": "3dmark05-lane-test",
                    "DXMT_EXPERIMENT_PROFILE": "perf",
                    "DXMT_3DMARK05_DIRECT": "1",
                    "DXMT_3DMARK05_STAGE": "0",
                    "DXMT_3DMARK05_KILL_SERVER": "0",
                    "DXMT_3DMARK05_KILL_SERVER_ON_EXIT": "0",
                    "DXMT_3DMARK05_AUTO_ENTER": "0",
                    "DXMT_3DMARK05_REQUIRE_UNLOCKED": "0",
                    "DXMT_3DMARK05_PREFIX": str(prefix),
                    "DXMT_3DMARK05_WINE_ROOT": str(wine_root),
                    "DXMT_3DMARK05_WINE_BIN": str(wine_bin),
                    "DXMT_3DMARK05_WINESERVER": str(wineserver),
                    "DXMT_3DMARK05_LOG": str(root / "3dmark05.log"),
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

    def test_default_is_bounded_gt1(self) -> None:
        result = self.run_direct()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[3dmark-lane] product=05 lane=gt1 source=preset", result.stdout)
        self.assertIn(
            "<-gt1> <-nosplash> <-nosysteminfo> <-noscreens>", result.stdout
        )

    def test_named_presets_cover_every_supported_lane_class(self) -> None:
        expected = {
            "gt2": "<-gt2>",
            "gt3": "<-gt3>",
            "graphics": "<-gtall>",
            "cpu1": "<-cpu1>",
            "cpu2": "<-cpu2>",
            "cpu": "<-cpuall>",
            "score": "<-gtall> <-cpuall>",
            "feature": "<-featureall>",
            "batch": "<-batchall>",
            "all": "<-gtall> <-cpuall> <-featureall> <-batchall>",
        }

        for lane, selection in expected.items():
            with self.subTest(lane=lane):
                result = self.run_direct(DXMT_3DMARK05_LANE=lane)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(
                    f"[3dmark-lane] product=05 lane={lane} source=preset",
                    result.stdout,
                )
                self.assertIn(selection, result.stdout)

    def test_raw_args_override_is_reported_as_custom(self) -> None:
        result = self.run_direct(
            DXMT_3DMARK05_LANE="gt3",
            DXMT_3DMARK05_ARGS="-cpu1 -nosplash",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[3dmark-lane] product=05 lane=custom source=args", result.stdout)
        self.assertIn("<-cpu1> <-nosplash>", result.stdout)
        self.assertNotIn("<-nosysteminfo>", result.stdout)

    def test_invalid_named_preset_fails_before_launch(self) -> None:
        result = self.run_direct(DXMT_3DMARK05_LANE="hdr1")

        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown 3DMark05 lane 'hdr1'", result.stderr)
        self.assertNotIn("fake-wine", result.stdout)

    def test_result_identity_parser(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            log = Path(temp_dir) / "run.log"
            log.write_text(
                "prefix line\n"
                "[3dmark-lane] product=05 lane=cpu2 source=preset\n"
            )

            self.assertEqual(
                run_experiment.extract_3dmark_lane(log),
                {"product": "3dmark05", "name": "cpu2", "source": "preset"},
            )

    def test_non_3dmark_log_has_no_lane_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            log = Path(temp_dir) / "run.log"
            log.write_text("[experiment] profile: name=perf source=default\n")

            self.assertIsNone(run_experiment.extract_3dmark_lane(log))


if __name__ == "__main__":
    unittest.main()
