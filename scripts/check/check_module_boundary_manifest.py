#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

import tomllib


VALID_STATUS = {"passing", "failing", "partial", "skipped", "scaffolded", "todo"}
VALID_LANES = {"provider-side", "app-local", "builtin"}
VALID_ARCHES = {"native", "x64", "x86"}
VALID_COST_CLASS = {
    "compile-time-test-only",
    "opt-in-cold-diagnostic",
    "release-retained-telemetry",
}
VALID_FAILURE_CATEGORIES = {
    "none",
    "artifact-staging",
    "pe-loader-export",
    "bridge-abi-mismatch",
    "unix-module-load",
    "provider-entry-dispatch",
    "public-d3d9-smoke",
    "command-submission",
    "unsupported-runtime",
}
REQ_RE = re.compile(r"^R-TEST-13\.(?:[1-9]|10)$")


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_manifest() -> Path:
    return repo_root() / "tests" / "module_boundary" / "MANIFEST.toml"


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise FileNotFoundError(f"module-boundary manifest missing: {path}")
    with path.open("rb") as handle:
        return tomllib.load(handle)


def required_string(case: dict[str, Any], key: str, label: str, errors: list[str]) -> str | None:
    value = case.get(key)
    if not isinstance(value, str) or not value:
        errors.append(f"{label}: {key} must be a non-empty string")
        return None
    return value


def required_string_list(case: dict[str, Any], key: str, label: str, errors: list[str]) -> list[str]:
    value = case.get(key)
    if not isinstance(value, list) or not value or any(not isinstance(item, str) or not item for item in value):
        errors.append(f"{label}: {key} must be a non-empty string list")
        return []
    return value


def validate_manifest(path: Path) -> list[str]:
    data = load_manifest(path)
    errors: list[str] = []

    if data.get("schema") != 1:
        errors.append("schema must be 1")

    lanes = data.get("valid_lanes")
    if not isinstance(lanes, list) or set(lanes) != VALID_LANES:
        errors.append(f"valid_lanes must be {sorted(VALID_LANES)}")
    arches = data.get("valid_arches")
    if not isinstance(arches, list) or set(arches) != VALID_ARCHES:
        errors.append(f"valid_arches must be {sorted(VALID_ARCHES)}")

    cases = data.get("case")
    if not isinstance(cases, list) or not cases:
        errors.append("manifest must contain [[case]] entries")
        return errors

    seen_ids: set[str] = set()
    covered_requirements: set[str] = set()

    for index, case in enumerate(cases, 1):
        if not isinstance(case, dict):
            errors.append(f"case #{index}: entry is not a table")
            continue

        label = f"case #{index}"
        case_id = required_string(case, "id", label, errors)
        if case_id:
            label = f"case {case_id}"
            if case_id in seen_ids:
                errors.append(f"{label}: duplicate id")
            seen_ids.add(case_id)

        required_string(case, "title", label, errors)
        required_string(case, "owner", label, errors)

        status = required_string(case, "status", label, errors)
        if status and status not in VALID_STATUS:
            errors.append(f"{label}: invalid status {status!r}")

        cost_class = required_string(case, "cost_class", label, errors)
        if cost_class and cost_class not in VALID_COST_CLASS:
            errors.append(f"{label}: invalid cost_class {cost_class!r}")

        requirements = required_string_list(case, "requirements", label, errors)
        for req in requirements:
            if not REQ_RE.match(req):
                errors.append(f"{label}: requirement must be R-TEST-13.x, got {req!r}")
            covered_requirements.add(req)

        case_lanes = required_string_list(case, "lanes", label, errors)
        for lane in case_lanes:
            if lane not in VALID_LANES:
                errors.append(f"{label}: invalid lane {lane!r}")

        case_arches = required_string_list(case, "arches", label, errors)
        for arch in case_arches:
            if arch not in VALID_ARCHES:
                errors.append(f"{label}: invalid arch {arch!r}")
        if "provider-side" in case_lanes and case_arches != ["native"]:
            if case_id == "result-schema" or case_id == "evidence-boundaries":
                pass
            else:
                errors.append(f"{label}: provider-side cases should use arches=['native']")

        failure_categories = required_string_list(case, "failure_categories", label, errors)
        for category in failure_categories:
            if category not in VALID_FAILURE_CATEGORIES:
                errors.append(f"{label}: invalid failure category {category!r}")

        checks = required_string_list(case, "checks", label, errors)
        acceptance = required_string_list(case, "acceptance", label, errors)
        if len(checks) < 2:
            errors.append(f"{label}: expected at least two checks")
        if len(acceptance) < 1:
            errors.append(f"{label}: expected at least one acceptance rule")

        if status in {"passing", "failing", "partial", "skipped"}:
            evidence = case.get("evidence")
            if not isinstance(evidence, list) or not evidence:
                errors.append(f"{label}: status {status!r} requires evidence")
        if status in {"scaffolded", "todo"} and "none" in failure_categories:
            errors.append(f"{label}: non-runtime case must not use failure category 'none'")

    required_reqs = {f"R-TEST-13.{i}" for i in range(1, 11)}
    missing = sorted(required_reqs - covered_requirements)
    if missing:
        errors.append(f"missing R-TEST-13 coverage: {', '.join(missing)}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate module-boundary harness manifest.")
    parser.add_argument("--manifest", type=Path, default=default_manifest())
    args = parser.parse_args()

    try:
        errors = validate_manifest(args.manifest)
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(f"module-boundary manifest ok: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
