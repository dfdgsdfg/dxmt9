#!/usr/bin/env python3
"""Audit diagnostic cost classes and DXMT environment registry drift.

This is a source-level companion to tests/HARNESS_MANIFEST.toml. It keeps
R-TEST-14.19..14.23 visible without requiring build artifacts:

* harness manifest cost_class/release_default invariants are checked with
  diagnostics grouped by harness id;
* the diagnostic-cost-audit row must cover R-TEST-14.19..14.23;
* source and experiment scripts are scanned for consumed DXMT*/DXMT9*
  environment variables;
* consumed variables must be represented in the environment registry or in the
  harness manifest.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

import tomllib


VALID_COST_RELEASE = {
    "compile-time-test-only": "not-linked",
    "opt-in-cold-diagnostic": "disabled",
    "release-retained-telemetry": "enabled-bounded",
}
DIAGNOSTIC_AUDIT_REQUIREMENTS = {
    "R-TEST-14.19",
    "R-TEST-14.20",
    "R-TEST-14.21",
    "R-TEST-14.22",
    "R-TEST-14.23",
}
SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hpp",
    ".m",
    ".mm",
    ".py",
    ".sh",
    ".bash",
    ".build",
}
SKIP_DIRS = {
    ".git",
    ".mesonpy",
    "__pycache__",
    "build",
    "builddir",
    "subprojects",
}

# Variables in scripts/check itself are test fixtures or this audit's own
# pattern vocabulary, not runtime knobs consumed by dxmt9.
DEFAULT_SCAN_ROOTS = (
    "src",
    "scripts/run_apps",
    "scripts/run_suites",
    "scripts/tools",
)

ENV_NAME_RE = re.compile(r"\bDXMT9?_[A-Z0-9_]+\b|\bDXMT_[A-Z0-9_]+\b")
REGISTRY_NAME_RE = re.compile(r"`(DXMT9?_[A-Z0-9_]+|DXMT_[A-Z0-9_]+)`")
STRING_ENV_RE = re.compile(r"[\"'](DXMT9?_[A-Z0-9_]+|DXMT_[A-Z0-9_]+)[\"']")
WIDE_STRING_ENV_RE = re.compile(r"L[\"'](DXMT9?_[A-Z0-9_]+|DXMT_[A-Z0-9_]+)[\"']")
PY_ENV_GET_RE = re.compile(
    r"os\.environ(?:\.get)?\(\s*[\"'](DXMT9?_[A-Z0-9_]+|DXMT_[A-Z0-9_]+)[\"']"
)
SHELL_ENV_ASSIGN_RE = re.compile(r"(^|[\s;])((?:DXMT9?_|DXMT_)[A-Z0-9_]+)=")
SHELL_ENV_EXPAND_RE = re.compile(r"\$\{?((?:DXMT9?_|DXMT_)[A-Z0-9_]+)\}?")

# Not runtime environment variables. They are compile-time symbols, generated
# ABI/status names, or literals used as error text.
NON_ENV_SYMBOL_PREFIXES = (
    "DXMT9_STATUS_",
    "DXMT9_WINEMETAL_CALL_",
    "DXMT9_WINEMETAL_BRIDGE_OP_",
    "DXMT9_NO_HEAP_ALLOC_GUARD_",
    "DXMT9_FORMAT_UTILS_",
    "DXMT9_COM_",
    "DXMT9_TESTS_",
)
NON_ENV_SYMBOLS = {
    "DXMT9_VERSION",
    "DXMT_ASSERT",
    "DXMT9_NODISCARD",
    "DXMT9_WINE_BUILTIN_DLL",
    "DXMT9_DYNAMIC_WINEMETAL_BRIDGE",
}


@dataclass(frozen=True)
class EnvUse:
    name: str
    path: Path
    line: int
    text: str


@dataclass
class AuditResult:
    manifest_errors: list[str] = field(default_factory=list)
    missing_registry: dict[str, list[EnvUse]] = field(default_factory=dict)
    documented_count: int = 0
    consumed_count: int = 0
    scanned_files: int = 0

    def has_errors(self) -> bool:
        return bool(self.manifest_errors or self.missing_registry)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def is_probable_env_name(name: str) -> bool:
    if name in NON_ENV_SYMBOLS:
        return False
    return not any(name.startswith(prefix) for prefix in NON_ENV_SYMBOL_PREFIXES)


def load_toml(path: Path) -> dict[str, Any]:
    with path.open("rb") as handle:
        return tomllib.load(handle)


def manifest_names(path: Path) -> set[str]:
    if not path.exists():
        return set()
    return {name for name in ENV_NAME_RE.findall(path.read_text(encoding="utf-8")) if is_probable_env_name(name)}


def registry_names(path: Path) -> set[str]:
    if not path.exists():
        return set()
    text = path.read_text(encoding="utf-8")
    return {name for name in REGISTRY_NAME_RE.findall(text) if is_probable_env_name(name)}


def validate_manifest_costs(path: Path) -> list[str]:
    errors: list[str] = []
    if not path.is_file():
        return [f"manifest missing: {path}"]

    data = load_toml(path)
    harnesses = data.get("harness")
    if not isinstance(harnesses, list):
        return ["manifest must contain [[harness]] entries"]

    by_id: dict[str, dict[str, Any]] = {}
    for index, harness in enumerate(harnesses, 1):
        if not isinstance(harness, dict):
            errors.append(f"harness #{index}: entry is not a table")
            continue
        harness_id = str(harness.get("id") or f"#{index}")
        by_id[harness_id] = harness

        cost_class = harness.get("cost_class")
        release_default = harness.get("release_default")
        expected_release = VALID_COST_RELEASE.get(cost_class)
        if expected_release is None:
            errors.append(
                f"harness {harness_id}: unknown cost_class {cost_class!r}; "
                f"expected one of {', '.join(sorted(VALID_COST_RELEASE))}"
            )
            continue
        if release_default != expected_release:
            errors.append(
                f"harness {harness_id}: cost_class={cost_class!r} requires "
                f"release_default={expected_release!r}, found {release_default!r}"
            )

    audit = by_id.get("diagnostic-cost-audit")
    if not audit:
        errors.append("manifest missing harness diagnostic-cost-audit")
        return errors

    requirements = set(audit.get("requirements") or [])
    missing_requirements = sorted(DIAGNOSTIC_AUDIT_REQUIREMENTS - requirements)
    if missing_requirements:
        errors.append(
            "harness diagnostic-cost-audit: missing requirements "
            + ", ".join(missing_requirements)
        )
    if audit.get("cost_class") != "compile-time-test-only":
        errors.append(
            "harness diagnostic-cost-audit: source audit must be "
            "compile-time-test-only"
        )
    if audit.get("release_default") != "not-linked":
        errors.append(
            "harness diagnostic-cost-audit: source audit must be not-linked by default"
        )

    return errors


def iter_source_files(roots: Iterable[Path]) -> Iterable[Path]:
    for root in roots:
        if not root.exists():
            continue
        if root.is_file():
            yield root
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            if path.suffix in SOURCE_SUFFIXES or path.name == "meson.build":
                yield path


def extract_env_uses(path: Path, root: Path) -> list[EnvUse]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return []

    uses: list[EnvUse] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        names: set[str] = set()
        stripped = line.strip()
        if stripped.startswith("#define") or stripped.startswith("#ifndef") or stripped.startswith("#if defined"):
            continue

        if "getenv" in line or "os.environ" in line or "env[" in line or "env.update" in line:
            names.update(STRING_ENV_RE.findall(line))
            names.update(WIDE_STRING_ENV_RE.findall(line))
            names.update(PY_ENV_GET_RE.findall(line))
        if path.suffix in {".sh", ".bash"}:
            names.update(match.group(2) for match in SHELL_ENV_ASSIGN_RE.finditer(line))
            names.update(SHELL_ENV_EXPAND_RE.findall(line))

        for name in sorted(name for name in names if is_probable_env_name(name)):
            uses.append(
                EnvUse(
                    name=name,
                    path=path.relative_to(root),
                    line=line_number,
                    text=stripped[:160],
                )
            )
    return uses


def run_audit(root: Path, manifest: Path, registry: Path, scan_roots: list[Path]) -> AuditResult:
    documented = registry_names(registry) | manifest_names(manifest)
    result = AuditResult(
        manifest_errors=validate_manifest_costs(manifest),
        documented_count=len(documented),
    )

    consumed: dict[str, list[EnvUse]] = {}
    files = list(iter_source_files(scan_roots))
    result.scanned_files = len(files)
    for path in files:
        for use in extract_env_uses(path, root):
            consumed.setdefault(use.name, []).append(use)

    result.consumed_count = len(consumed)
    for name, uses in sorted(consumed.items()):
        if name not in documented:
            result.missing_registry[name] = uses
    return result


def format_report(result: AuditResult) -> str:
    lines: list[str] = []
    if result.manifest_errors:
        lines.append("manifest cost-class errors:")
        for error in result.manifest_errors:
            lines.append(f"  - {error}")

    if result.missing_registry:
        if lines:
            lines.append("")
        lines.append("environment registry drift:")
        lines.append(
            "  consumed DXMT*/DXMT9* variables below are absent from "
            "agents/rules/environment_variables.rules.md and tests/HARNESS_MANIFEST.toml"
        )
        for name, uses in sorted(result.missing_registry.items()):
            first = uses[0]
            lines.append(f"  - {name} ({len(uses)} use{'s' if len(uses) != 1 else ''})")
            lines.append(f"      first: {first.path}:{first.line}: {first.text}")

    if not lines:
        lines.append(
            "diagnostic cost audit ok: "
            f"{result.consumed_count} consumed env vars, "
            f"{result.documented_count} documented vars, "
            f"{result.scanned_files} files scanned"
        )
    else:
        lines.append("")
        lines.append(
            "summary: "
            f"{result.consumed_count} consumed env vars, "
            f"{result.documented_count} documented vars, "
            f"{result.scanned_files} files scanned"
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Audit diagnostic harness cost classes and DXMT env registry drift."
    )
    parser.add_argument("--root", type=Path, default=root)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=root / "tests" / "HARNESS_MANIFEST.toml",
    )
    parser.add_argument(
        "--registry",
        type=Path,
        default=root / "agents" / "rules" / "environment_variables.rules.md",
    )
    parser.add_argument(
        "--scan-root",
        action="append",
        default=None,
        help="Path to scan; may be repeated. Defaults to src and experiment scripts.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print findings but return success. Useful before wiring into CI/Meson.",
    )
    args = parser.parse_args(argv)

    root = args.root.resolve()
    manifest = args.manifest if args.manifest.is_absolute() else root / args.manifest
    registry = args.registry if args.registry.is_absolute() else root / args.registry
    scan_root_values = args.scan_root or list(DEFAULT_SCAN_ROOTS)
    scan_roots = [path if path.is_absolute() else root / path for path in map(Path, scan_root_values)]

    try:
        result = run_audit(root, manifest, registry, scan_roots)
    except Exception as exc:
        print(f"audit: {exc}", file=sys.stderr)
        return 2

    report = format_report(result)
    if result.has_errors():
        print(report, file=sys.stderr)
        return 0 if args.dry_run else 1

    print(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
