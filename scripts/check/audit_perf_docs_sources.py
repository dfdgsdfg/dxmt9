#!/usr/bin/env python3
"""Audit new docs/perfomance leaf provenance.

The deleted/retired specs/perfomance.plan.md journal may remain in historical
frontmatter for old leaves, but it is no longer maintained and new leaves must
cite concrete artifacts or analysis files. By default this script checks only
untracked or staged-added docs/perfomance Markdown files so it can coexist with
the legacy corpus.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RETIRED_SOURCE = "specs/perfomance.plan.md"
SOURCE_RE = re.compile(r"^source:\s*(.*)$", re.MULTILINE)


def is_leaf(path: Path, repo_root: Path = REPO_ROOT) -> bool:
    try:
        rel = path.resolve().relative_to((repo_root / "docs" / "perfomance").resolve())
    except ValueError:
        return False
    return path.suffix == ".md" and len(rel.parts) > 1


def git_new_perf_docs(repo_root: Path = REPO_ROOT) -> list[Path]:
    result = subprocess.run(
        ["git", "status", "--porcelain", "--", "docs/perfomance"],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(result.stderr.strip() or "git status failed")

    paths: list[Path] = []
    for line in result.stdout.splitlines():
        if len(line) < 4:
            continue
        status = line[:2]
        raw_path = line[3:]
        if " -> " in raw_path:
            raw_path = raw_path.rsplit(" -> ", 1)[1]
        if status == "??" or "A" in status:
            path = repo_root / raw_path
            if is_leaf(path, repo_root):
                paths.append(path)
    return paths


def audit_paths(paths: list[Path]) -> list[str]:
    failures: list[str] = []
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            failures.append(f"{path}: cannot read: {exc}")
            continue
        match = SOURCE_RE.search(text)
        if match is None:
            failures.append(f"{path}: missing frontmatter source")
            continue
        source = match.group(1)
        if RETIRED_SOURCE in source:
            failures.append(
                f"{path}: source uses deleted/retired {RETIRED_SOURCE}; cite "
                "experiments/output, traces/analysis, exported Xcode counters, "
                "generated reports, or related docs"
            )
    return failures


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--path",
        action="append",
        type=Path,
        default=[],
        help="Explicit docs/perfomance leaf to audit. Defaults to git new files.",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    paths = [path for path in args.path if is_leaf(path)] if args.path else git_new_perf_docs()
    failures = audit_paths(paths)
    if failures:
        print("audit_perf_docs_sources: FAIL", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print(f"audit_perf_docs_sources: OK ({len(paths)} new leaf file(s) checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
