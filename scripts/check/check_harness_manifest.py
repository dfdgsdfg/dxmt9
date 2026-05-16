#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

import tomllib


VALID_STATUS = {"implemented", "partial", "scaffolded", "todo", "skipped"}
VALID_COST_CLASS = {
    "compile-time-test-only",
    "opt-in-cold-diagnostic",
    "release-retained-telemetry",
}
VALID_RELEASE_DEFAULT = {"not-linked", "disabled", "enabled-bounded"}
VALID_KIND = {
    "native-unit",
    "runtime-readback",
    "module-boundary",
    "wine-pe-conformance",
    "integration",
    "experiment",
    "manifest-audit",
}
REQ_RE = re.compile(r"^R-[A-Z0-9]+-[0-9]+(?:\.[0-9]+)?$")


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_manifest() -> Path:
    return repo_root() / "tests" / "HARNESS_MANIFEST.toml"


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise FileNotFoundError(f"harness manifest missing: {path}")
    with path.open("rb") as handle:
        return tomllib.load(handle)


def require_string(table: dict[str, Any], key: str, label: str, errors: list[str]) -> str | None:
    value = table.get(key)
    if not isinstance(value, str) or not value:
        errors.append(f"{label}: {key} must be a non-empty string")
        return None
    return value


def require_string_list(table: dict[str, Any], key: str, label: str, errors: list[str]) -> list[str]:
    value = table.get(key)
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        errors.append(f"{label}: {key} must be a string list")
        return []
    return value


def validate_manifest(path: Path) -> list[str]:
    root = repo_root()
    data = load_manifest(path)
    errors: list[str] = []

    if data.get("schema") != 1:
        errors.append("schema must be 1")

    harnesses = data.get("harness")
    if not isinstance(harnesses, list) or not harnesses:
        errors.append("manifest must contain [[harness]] entries")
        return errors

    seen_ids: set[str] = set()
    for index, harness in enumerate(harnesses, 1):
        if not isinstance(harness, dict):
            errors.append(f"harness #{index}: entry is not a table")
            continue

        label = f"harness #{index}"
        harness_id = require_string(harness, "id", label, errors)
        if harness_id:
            label = f"harness {harness_id}"
            if harness_id in seen_ids:
                errors.append(f"{label}: duplicate id")
            seen_ids.add(harness_id)

        require_string(harness, "name", label, errors)
        path_value = require_string(harness, "path", label, errors)
        if path_value:
            harness_path = root / path_value
            if not harness_path.exists():
                errors.append(f"{label}: path does not exist: {path_value}")

        kind = require_string(harness, "kind", label, errors)
        if kind and kind not in VALID_KIND:
            errors.append(f"{label}: invalid kind {kind!r}")

        status = require_string(harness, "status", label, errors)
        if status and status not in VALID_STATUS:
            errors.append(f"{label}: invalid status {status!r}")

        cost_class = require_string(harness, "cost_class", label, errors)
        if cost_class and cost_class not in VALID_COST_CLASS:
            errors.append(f"{label}: invalid cost_class {cost_class!r}")

        release_default = require_string(harness, "release_default", label, errors)
        if release_default and release_default not in VALID_RELEASE_DEFAULT:
            errors.append(f"{label}: invalid release_default {release_default!r}")

        requirements = require_string_list(harness, "requirements", label, errors)
        if not requirements:
            errors.append(f"{label}: requirements must not be empty")
        for req in requirements:
            if not REQ_RE.match(req):
                errors.append(f"{label}: invalid requirement id {req!r}")

        require_string_list(harness, "meson_targets", label, errors)
        evidence = require_string_list(harness, "evidence", label, errors)
        for evidence_path in evidence:
            if not (root / evidence_path).exists():
                errors.append(f"{label}: evidence path does not exist: {evidence_path}")

        next_items = require_string_list(harness, "next", label, errors)
        if status in {"partial", "scaffolded", "todo"} and not next_items:
            errors.append(f"{label}: non-complete status requires next actions")

        if cost_class == "compile-time-test-only" and release_default != "not-linked":
            errors.append(f"{label}: compile-time-test-only must use release_default='not-linked'")
        if cost_class == "opt-in-cold-diagnostic" and release_default != "disabled":
            errors.append(f"{label}: opt-in-cold-diagnostic must use release_default='disabled'")
        if cost_class == "release-retained-telemetry" and release_default != "enabled-bounded":
            errors.append(f"{label}: release-retained-telemetry must use release_default='enabled-bounded'")

    required_ids = {
        "native-core",
        "native-shader",
        "native-backend",
        "native-bridge",
        "shader-runner",
        "module-boundary",
        "d3d9-conformance",
        "wsi-present",
        "wild-experiments",
        "diagnostic-cost-audit",
    }
    missing_ids = sorted(required_ids - seen_ids)
    if missing_ids:
        errors.append(f"missing required harness ids: {', '.join(missing_ids)}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate dxmt9 harness inventory manifest.")
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

    print(f"harness manifest ok: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
