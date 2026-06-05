#!/usr/bin/env python3
"""Regression tests for docs/perfomance provenance audit."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check" / "audit_perf_docs_sources.py"


def load_module():
    spec = importlib.util.spec_from_file_location("audit_perf_docs_sources", SCRIPT)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class AuditPerfDocsSourcesTests(unittest.TestCase):
    def test_rejects_retired_spec_source(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "leaf.md"
            path.write_text(
                "---\n"
                "source: specs/perfomance.plan.md#L1-L2\n"
                "---\n",
                encoding="utf-8",
            )

            failures = module.audit_paths([path])

        self.assertEqual(len(failures), 1)
        self.assertIn("deleted/retired specs/perfomance.plan.md", failures[0])

    def test_accepts_artifact_source(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "leaf.md"
            path.write_text(
                "---\n"
                "source: traces/app-d3d9-3dmark05-r1/analysis/report.md\n"
                "---\n",
                encoding="utf-8",
            )

            failures = module.audit_paths([path])

        self.assertEqual(failures, [])

    def test_requires_source(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "leaf.md"
            path.write_text("---\nstatus: accepted\n---\n", encoding="utf-8")

            failures = module.audit_paths([path])

        self.assertEqual(len(failures), 1)
        self.assertIn("missing frontmatter source", failures[0])


if __name__ == "__main__":
    unittest.main()
