"""Tests for scripts/wine/resolve.py.

Run:
    python3 -m unittest tests.scripts.test_wine_resolve

Or via meson once the test() entry is registered:
    meson test -C build wine_resolve
"""

import os
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.wine import resolve  # noqa: E402

FIXTURE_DIR = REPO_ROOT / "tests" / "scripts" / "fixtures"


class LoadManifestTests(unittest.TestCase):
    def test_good_manifest_loads_two_entries(self):
        entries = resolve.load_manifest(FIXTURE_DIR / "wine_manifest_good.toml")
        self.assertEqual(len(entries), 2)
        ids = sorted(e.id for e in entries)
        self.assertEqual(ids, ["fake-dxmt", "fake-vanilla"])

    def test_path_expands_repo_root(self):
        entries = resolve.load_manifest(FIXTURE_DIR / "wine_manifest_good.toml")
        vanilla = next(e for e in entries if e.id == "fake-vanilla")
        self.assertTrue(vanilla.path.is_absolute())
        self.assertTrue(str(vanilla.path).startswith(str(REPO_ROOT)))
        self.assertTrue((vanilla.path / "bin" / "wine").exists())

    def test_optional_fields_default_to_none(self):
        entries = resolve.load_manifest(FIXTURE_DIR / "wine_manifest_good.toml")
        vanilla = next(e for e in entries if e.id == "fake-vanilla")
        dxmt = next(e for e in entries if e.id == "fake-dxmt")
        self.assertIsNone(vanilla.notes)
        self.assertEqual(dxmt.notes, "second entry, exercises optional fields")
        self.assertEqual(vanilla.metal_surface_protocol, "unsupported")
        self.assertEqual(dxmt.metal_surface_protocol, "legacy-macdrv-symbols")

    def test_invalid_surface_protocol_raises(self):
        fixture = FIXTURE_DIR / "wine_manifest_invalid_protocol.toml"
        with self.assertRaises(resolve.ManifestError) as cm:
            resolve.load_manifest(fixture)
        self.assertIn("metal_surface_protocol", str(cm.exception))

    def test_duplicate_id_raises(self):
        with self.assertRaises(resolve.ManifestError) as cm:
            resolve.load_manifest(FIXTURE_DIR / "wine_manifest_dup_id.toml")
        self.assertIn("duplicate id", str(cm.exception).lower())

    def test_missing_required_field_raises(self):
        with self.assertRaises(resolve.ManifestError) as cm:
            resolve.load_manifest(FIXTURE_DIR / "wine_manifest_missing_field.toml")
        self.assertIn("variant", str(cm.exception).lower())


class ResolveWineIdTests(unittest.TestCase):
    def setUp(self):
        self.entries = resolve.load_manifest(FIXTURE_DIR / "wine_manifest_good.toml")

    def test_cli_takes_priority_over_env_and_catalogue(self):
        entry, source = resolve.resolve_wine_id(
            entries=self.entries,
            cli_arg="fake-vanilla",
            env_var="fake-dxmt",
            catalogue_value="fake-dxmt",
            app_name="x",
        )
        self.assertEqual(entry.id, "fake-vanilla")
        self.assertEqual(source, "--wine-id")

    def test_env_takes_priority_over_catalogue(self):
        entry, source = resolve.resolve_wine_id(
            entries=self.entries,
            cli_arg=None,
            env_var="fake-vanilla",
            catalogue_value="fake-dxmt",
            app_name="x",
        )
        self.assertEqual(entry.id, "fake-vanilla")
        self.assertEqual(source, "DXMT_EXPERIMENT_WINE_ID")

    def test_catalogue_used_when_no_cli_or_env(self):
        entry, source = resolve.resolve_wine_id(
            entries=self.entries,
            cli_arg=None,
            env_var=None,
            catalogue_value="fake-vanilla",
            app_name="x",
        )
        self.assertEqual(entry.id, "fake-vanilla")
        self.assertIn("CATALOGUE", source)

    def test_unknown_id_raises_with_diagnostic(self):
        with self.assertRaises(resolve.ManifestError) as cm:
            resolve.resolve_wine_id(
                entries=self.entries,
                cli_arg="nonexistent",
                env_var=None,
                catalogue_value=None,
                app_name="x",
            )
        msg = str(cm.exception)
        self.assertIn("nonexistent", msg)
        self.assertIn("manifest.toml", msg)

    def test_no_id_anywhere_raises(self):
        with self.assertRaises(resolve.ManifestError) as cm:
            resolve.resolve_wine_id(
                entries=self.entries,
                cli_arg=None,
                env_var=None,
                catalogue_value=None,
                app_name="some-app",
            )
        self.assertIn("some-app", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
