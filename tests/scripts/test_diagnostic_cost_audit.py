import tempfile
import unittest
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.check import audit_diagnostic_costs  # noqa: E402


GOOD_MANIFEST = """\
schema = 1

[[harness]]
id = "diagnostic-cost-audit"
name = "Diagnostic cost-class inventory audit"
path = "tests/HARNESS_MANIFEST.toml"
kind = "manifest-audit"
status = "implemented"
cost_class = "compile-time-test-only"
release_default = "not-linked"
requirements = [
  "R-TEST-14.19",
  "R-TEST-14.20",
  "R-TEST-14.21",
  "R-TEST-14.22",
  "R-TEST-14.23",
]
meson_targets = []
evidence = []
next = []
"""


class DiagnosticCostAuditTests(unittest.TestCase):
    def make_repo(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        (root / "tests").mkdir()
        (root / "agents" / "rules").mkdir(parents=True)
        (root / "src").mkdir()
        manifest = root / "tests" / "HARNESS_MANIFEST.toml"
        registry = root / "agents" / "rules" / "environment_variables.rules.md"
        manifest.write_text(GOOD_MANIFEST, encoding="utf-8")
        registry.write_text(
            "| Var | Purpose | Default |\n"
            "|---|---|---|\n"
            "| `DXMT_DEBUG_KNOWN` | known debug knob | `0` |\n",
            encoding="utf-8",
        )
        return root, manifest, registry

    def test_missing_consumed_env_var_is_reported(self):
        root, manifest, registry = self.make_repo()
        source = root / "src" / "sample.cpp"
        source.write_text(
            'const char* a = std::getenv("DXMT_DEBUG_KNOWN");\n'
            'const char* b = std::getenv("DXMT_DEBUG_MISSING");\n'
            'const char* c = "DXMT_ASSERT";\n',
            encoding="utf-8",
        )

        result = audit_diagnostic_costs.run_audit(
            root,
            manifest,
            registry,
            [root / "src"],
        )

        self.assertIn("DXMT_DEBUG_MISSING", result.missing_registry)
        self.assertNotIn("DXMT_DEBUG_KNOWN", result.missing_registry)
        self.assertNotIn("DXMT_ASSERT", result.missing_registry)

    def test_manifest_cost_release_mismatch_is_reported(self):
        root, manifest, registry = self.make_repo()
        manifest.write_text(
            GOOD_MANIFEST.replace(
                'release_default = "not-linked"',
                'release_default = "disabled"',
            ),
            encoding="utf-8",
        )

        result = audit_diagnostic_costs.run_audit(
            root,
            manifest,
            registry,
            [root / "src"],
        )

        self.assertTrue(
            any(
                "requires release_default='not-linked'" in error
                for error in result.manifest_errors
            )
        )


if __name__ == "__main__":
    unittest.main()
