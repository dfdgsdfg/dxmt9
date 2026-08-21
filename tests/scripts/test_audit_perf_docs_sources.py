#!/usr/bin/env python3
"""Regression tests for docs/perfomance provenance audit."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


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
            root = Path(tmp)
            path = root / "docs" / "perfomance" / "domain" / "leaf.md"
            path.parent.mkdir(parents=True)
            path.write_text(
                "---\n"
                "source: specs/perfomance.plan.md#L1-L2\n"
                "---\n",
                encoding="utf-8",
            )

            failures = module.audit_paths([path], root)

        self.assertEqual(len(failures), 1)
        self.assertIn("deleted/retired specs/perfomance.plan.md", failures[0])

    def test_accepts_semicolon_and_brace_expansion_when_all_paths_exist(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "docs" / "perfomance" / "domain" / "leaf.md"
            path.parent.mkdir(parents=True)
            for name in ("gt1", "gt3", "sfiv"):
                (root / "experiments" / "output" / f"run-{name}").mkdir(
                    parents=True
                )
            path.write_text(
                "---\n"
                "source: experiments/output/run-{gt1,gt3}; experiments/output/run-sfiv\n"
                "---\n",
                encoding="utf-8",
            )

            failures = module.audit_paths([path], root)

        self.assertEqual(failures, [])
        self.assertEqual(
            module.concrete_source_paths(
                "experiments/output/run-{gt1,gt3}; experiments/output/run-sfiv"
            ),
            [
                "experiments/output/run-gt1",
                "experiments/output/run-gt3",
                "experiments/output/run-sfiv",
            ],
        )

    def test_rejects_missing_brace_expansion_branch(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "docs" / "perfomance" / "domain" / "leaf.md"
            path.parent.mkdir(parents=True)
            (root / "experiments" / "output" / "run-gt1").mkdir(parents=True)
            path.write_text(
                "---\nsource: experiments/output/run-{gt1,gt3}\n---\n",
                encoding="utf-8",
            )

            failures = module.audit_paths([path], root)

        self.assertEqual(len(failures), 1)
        self.assertIn("source path does not exist", failures[0])
        self.assertIn("run-gt3", failures[0])

    def test_requires_source(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "docs" / "perfomance" / "domain" / "leaf.md"
            path.parent.mkdir(parents=True)
            path.write_text("---\nstatus: accepted\n---\n", encoding="utf-8")

            failures = module.audit_paths([path], root)

        self.assertEqual(len(failures), 1)
        self.assertIn("missing frontmatter source", failures[0])

    def test_explicit_outdated_marker_preserves_legacy_missing_evidence(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "docs" / "perfomance" / "domain" / "leaf.md"
            path.parent.mkdir(parents=True)
            path.write_text(
                "---\n"
                "status: rejected\n"
                "outdated: evidence-missing\n"
                "source: experiments/output/retired-run\n"
                "---\n",
                encoding="utf-8",
            )

            failures = module.audit_paths([path], root)

        self.assertEqual(failures, [])

    def test_current_verdict_cannot_hide_missing_evidence_behind_outdated_marker(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "docs" / "perfomance" / "domain" / "leaf.md"
            path.parent.mkdir(parents=True)
            path.write_text(
                "---\n"
                "status: accepted-verdict\n"
                "outdated: evidence-missing\n"
                "source: experiments/output/retired-run\n"
                "---\n",
                encoding="utf-8",
            )

            failures = module.audit_paths([path], root)

        self.assertEqual(len(failures), 1)
        self.assertIn("source path does not exist", failures[0])

    def test_current_accepted_verdict_walk_is_non_vacuous(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            leaf = root / "docs" / "perfomance" / "domain" / "leaf.md"
            leaf.parent.mkdir(parents=True)
            source = root / "experiments" / "output" / "surviving-run"
            source.mkdir(parents=True)
            leaf.write_text(
                "---\nstatus: accepted-verdict\nsource: experiments/output/surviving-run\n---\n",
                encoding="utf-8",
            )

            accepted = module.current_accepted_verdict_perf_docs(root)

        self.assertEqual(accepted, [leaf])

    def test_body_status_does_not_enter_current_verdict_scope(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            leaf = root / "docs" / "perfomance" / "domain" / "leaf.md"
            leaf.parent.mkdir(parents=True)
            leaf.write_text(
                "---\nstatus: rejected\nsource: docs/source.md\n---\n\n"
                "status: accepted-verdict\n",
                encoding="utf-8",
            )

            accepted = module.current_accepted_verdict_perf_docs(root)

        self.assertEqual(accepted, [])

    def test_changed_leaf_walk_includes_modified_files(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            leaf = root / "docs" / "perfomance" / "domain" / "leaf.md"
            leaf.parent.mkdir(parents=True)
            leaf.write_text(
                "---\nsource: src/surviving.cpp\n---\n", encoding="utf-8"
            )
            result = SimpleNamespace(
                returncode=0,
                stderr="",
                stdout=" M docs/perfomance/domain/leaf.md\n",
            )
            with patch.object(module.subprocess, "run", return_value=result):
                changed = module.git_changed_perf_docs(root)

        self.assertEqual(changed, [leaf])


if __name__ == "__main__":
    unittest.main()
