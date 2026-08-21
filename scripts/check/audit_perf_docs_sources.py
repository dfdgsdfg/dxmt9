#!/usr/bin/env python3
"""Audit provenance for current or changed ``docs/perfomance`` leaves.

The performance tree contains historical leaves whose source artifacts were
intentionally retired. Auditing every historical leaf would turn that legacy
debt into a repository-wide migration. The default scope is therefore changed
leaves plus current ``accepted-verdict`` leaves, including the latter when the
tree is clean.

Source entries are separated by semicolons (the preferred spelling) or commas,
and support simple brace expansion such as ``run-{gt1,gt3}``. Every expanded
entry must name a surviving file or directory. A source fragment (for example
``result.json#L1-L4``) is checked by its path without the fragment.
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
STATUS_RE = re.compile(r"^status:\s*([^\s#]+)", re.MULTILINE)
OUTDATED_RE = re.compile(r"^outdated:\s*([^\s#]+)", re.MULTILINE)
OUTDATED_VALUES = {"knob-removed", "evidence-missing", "retired-journal"}
FRONTMATTER_RE = re.compile(r"\A---\s*\n(.*?)\n---\s*(?:\n|\Z)", re.DOTALL)


def is_leaf(path: Path, repo_root: Path = REPO_ROOT) -> bool:
    """Return whether *path* is a Markdown leaf below ``docs/perfomance``."""

    try:
        rel = path.resolve().relative_to((repo_root / "docs" / "perfomance").resolve())
    except ValueError:
        return False
    return path.suffix == ".md" and len(rel.parts) > 1


def _status_paths(repo_root: Path) -> list[Path]:
    result = subprocess.run(
        [
            "git",
            "status",
            "--porcelain=v1",
            "--untracked-files=all",
            "--",
            "docs/perfomance",
        ],
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
        if status == "??" or status[0] in "AMR" or status[1] in "AM":
            path = repo_root / raw_path
            if path.exists() and is_leaf(path, repo_root):
                paths.append(path)
    return paths


def git_changed_perf_docs(repo_root: Path = REPO_ROOT) -> list[Path]:
    """Return existing changed performance leaves, including modifications."""

    return _status_paths(repo_root)


# Compatibility name for callers of the original audit helper.
git_new_perf_docs = git_changed_perf_docs


def current_accepted_verdict_perf_docs(repo_root: Path = REPO_ROOT) -> list[Path]:
    """Return all current accepted-verdict performance leaves.

    This intentionally walks the tree rather than consulting Git status, so a
    clean checkout still audits the current verdict surface.
    """

    root = repo_root / "docs" / "perfomance"
    paths: list[Path] = []
    for path in sorted(root.rglob("*.md")):
        if not is_leaf(path, repo_root):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            continue
        status = STATUS_RE.search(_frontmatter(text))
        if status is not None and status.group(1) == "accepted-verdict":
            paths.append(path)
    return paths


def _frontmatter(text: str) -> str:
    match = FRONTMATTER_RE.match(text)
    return "" if match is None else match.group(1)


def _source_value(text: str) -> str | None:
    match = SOURCE_RE.search(_frontmatter(text))
    return None if match is None else match.group(1).strip().strip('"\'')


def _metadata(text: str, pattern: re.Pattern[str]) -> str | None:
    match = pattern.search(_frontmatter(text))
    return None if match is None else match.group(1)


def split_source_entries(source: str) -> list[str]:
    """Split source syntax on top-level semicolons/commas.

    Commas inside a simple brace expression are alternatives, not separators.
    Empty entries and unbalanced/nested braces are rejected so a typo cannot
    silently become an unchecked provenance string.
    """

    entries: list[str] = []
    start = 0
    depth = 0
    for index, char in enumerate(source):
        if char == "{":
            if depth:
                raise ValueError("nested brace expansion is not supported")
            depth = 1
        elif char == "}":
            if not depth:
                raise ValueError("closing brace has no matching opening brace")
            depth = 0
        elif char in ";," and not depth:
            entry = source[start:index].strip()
            if not entry:
                raise ValueError("empty source entry")
            entries.append(entry)
            start = index + 1
    if depth:
        raise ValueError("opening brace has no matching closing brace")
    entry = source[start:].strip()
    if not entry:
        raise ValueError("empty source entry")
    entries.append(entry)
    return entries


def expand_source_entry(entry: str) -> list[str]:
    """Expand one simple ``{a,b}`` expression, recursively if needed."""

    match = re.search(r"\{([^{}]*)\}", entry)
    if match is None:
        return [entry]
    alternatives = match.group(1).split(",")
    if not alternatives or any(not alternative for alternative in alternatives):
        raise ValueError(f"empty brace alternative in {entry!r}")
    expanded: list[str] = []
    for alternative in alternatives:
        expanded.extend(
            expand_source_entry(entry[: match.start()] + alternative + entry[match.end() :])
        )
    return expanded


def concrete_source_paths(source: str) -> list[str]:
    """Return each concrete source path represented by a source field."""

    concrete: list[str] = []
    for entry in split_source_entries(source):
        concrete.extend(expand_source_entry(entry))
    return concrete


def _source_path(raw_source: str, repo_root: Path) -> Path:
    # Line/fragment references identify a location in the artifact, not part
    # of the filesystem path.
    raw_path = raw_source.split("#", 1)[0].strip()
    path = Path(raw_path)
    return path if path.is_absolute() else repo_root / path


def audit_paths(paths: list[Path], repo_root: Path = REPO_ROOT) -> list[str]:
    """Audit source metadata and surviving concrete paths for *paths*."""

    failures: list[str] = []
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            failures.append(f"{path}: cannot read: {exc}")
            continue

        source = _source_value(text)
        if source is None:
            failures.append(f"{path}: missing frontmatter source")
            continue

        try:
            concrete = concrete_source_paths(source)
        except ValueError as exc:
            failures.append(f"{path}: invalid source syntax: {exc}")
            continue

        status = _metadata(text, STATUS_RE)
        outdated = _metadata(text, OUTDATED_RE)
        # Explicitly marked historical leaves are the one permitted exception:
        # their missing evidence is debt recorded by the leaf itself. A current
        # accepted-verdict leaf never gets this escape hatch.
        legacy_exception = outdated in OUTDATED_VALUES and status != "accepted-verdict"
        for raw_source in concrete:
            if raw_source.split("#", 1)[0].strip() == RETIRED_SOURCE:
                if not legacy_exception:
                    failures.append(
                        f"{path}: source uses deleted/retired {RETIRED_SOURCE}; cite "
                        "experiments/output, traces/analysis, exported Xcode counters, "
                        "generated reports, or related docs"
                    )
                continue
            if legacy_exception:
                continue
            source_path = _source_path(raw_source, repo_root)
            if not source_path.exists():
                failures.append(
                    f"{path}: source path does not exist: "
                    f"{raw_source.split('#', 1)[0].strip()}"
                )
    return failures


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--path",
        action="append",
        type=Path,
        default=[],
        help=(
            "Explicit docs/perfomance leaf to audit. Defaults to changed leaves "
            "plus clean-tree accepted-verdict leaves."
        ),
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.path:
        paths = [path if path.is_absolute() else REPO_ROOT / path for path in args.path]
        paths = [path for path in paths if is_leaf(path)]
        scope = "explicit"
    else:
        paths = git_changed_perf_docs()
        paths.extend(current_accepted_verdict_perf_docs())
        paths = list(dict.fromkeys(paths))
        scope = "changed + accepted-verdict"

    failures = audit_paths(paths)
    if failures:
        print("audit_perf_docs_sources: FAIL", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print(f"audit_perf_docs_sources: OK ({len(paths)} {scope} leaf file(s) checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
