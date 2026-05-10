"""Tests for scripts/wine/resolve.py.

Run:
    python3 -m unittest tests.scripts.test_wine_resolve

Or via meson once the test() entry is registered:
    meson test -C build wine_resolve
"""

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

    def test_duplicate_id_raises(self):
        with self.assertRaises(resolve.ManifestError) as cm:
            resolve.load_manifest(FIXTURE_DIR / "wine_manifest_dup_id.toml")
        self.assertIn("duplicate id", str(cm.exception).lower())

    def test_missing_required_field_raises(self):
        with self.assertRaises(resolve.ManifestError) as cm:
            resolve.load_manifest(FIXTURE_DIR / "wine_manifest_missing_field.toml")
        self.assertIn("variant", str(cm.exception).lower())


if __name__ == "__main__":
    unittest.main()
