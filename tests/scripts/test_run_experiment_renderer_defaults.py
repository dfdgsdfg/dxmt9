"""Tests for the wild-run renderer-mode default and rollback contract."""

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.run_apps.run_experiment import ExperimentApp  # noqa: E402


def catalogue_row(**overrides):
    row = {
        "name": "renderer-default-test",
        "source": "repo-local",
        "license": "mit",
        "source_kind": "project-authored",
        "license_scope": "project-mit",
        "binary": "test.exe",
        "launcher": "experiments/launchers/test.sh",
        "reference": "experiments/references/test.png",
        "features": [],
    }
    row.update(overrides)
    return row


class RendererDefaultsTests(unittest.TestCase):
    def test_omitted_render_mode_uses_framegraph(self):
        self.assertEqual(
            ExperimentApp.from_toml(catalogue_row()).render_mode,
            "framegraph",
        )

    def test_explicit_traditional_is_rollback(self):
        self.assertEqual(
            ExperimentApp.from_toml(
                catalogue_row(render_mode="traditional")
            ).render_mode,
            "traditional",
        )

    def test_explicit_framegraph_is_accepted(self):
        self.assertEqual(
            ExperimentApp.from_toml(
                catalogue_row(render_mode="framegraph")
            ).render_mode,
            "framegraph",
        )

    def test_unknown_render_mode_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "render_mode must be"):
            ExperimentApp.from_toml(catalogue_row(render_mode="unknown"))


if __name__ == "__main__":
    unittest.main()
