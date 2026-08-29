"""Contract tests for the external S.T.A.L.K.E.R. CoP benchmark lane."""

import os
import re
import tomllib
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = REPO_ROOT / "experiments/launchers/app-d3d9-stkcop-bench.sh"
CATALOGUE = REPO_ROOT / "experiments/CATALOGUE.toml"


class StalkerCopLaneTests(unittest.TestCase):
    def test_catalogue_declares_bounded_external_d3d9_lane(self) -> None:
        with CATALOGUE.open("rb") as handle:
            entries = tomllib.load(handle)["app"]
        entry = next(
            item for item in entries if item["name"] == "app-d3d9-stkcop-bench"
        )

        self.assertEqual(entry["source_kind"], "external-application")
        self.assertEqual(entry["license_scope"], "external-not-vendored")
        self.assertEqual(entry["wine_id"], "sikarugir-cx-24.0.7")
        self.assertTrue(entry["require_positive_timeout"])
        self.assertFalse(entry["allow_timeout"])
        self.assertGreater(entry["run_timeout_sec"], 0)
        self.assertTrue(
            {"benchmark", "d3d9", "deferred", "sm3"}.issubset(entry["features"])
        )
        self.assertNotIn("prefixs/", entry["binary"])
        self.assertTrue(os.access(LAUNCHER, os.X_OK))

    def test_launcher_uses_renderer_selecting_openautomate_path(self) -> None:
        source = LAUNCHER.read_text()

        self.assertIn('-openautomate "$test_file"', source)
        self.assertIn("DXMT_STKCOP_RENDERER:-renderer_r2.5", source)
        self.assertNotIn("-batch_benchmark", source)
        self.assertNotIn("-start \"server(", source)
        self.assertIn("without producing a benchmark result", source)

    def test_launcher_maps_every_scene_to_native_test_file(self) -> None:
        source = LAUNCHER.read_text()
        expected = {
            "day": "dayBenchmark.test",
            "night": "NightBenchmark.test",
            "rain": "RainBenchmark.test",
            "sunshafts": "SunShaftsBenchmark.test",
        }

        for lane, test_file in expected.items():
            with self.subTest(lane=lane):
                self.assertRegex(
                    source,
                    re.compile(rf"{lane}\)\s+test_file=\"{test_file}\""),
                )


if __name__ == "__main__":
    unittest.main()
