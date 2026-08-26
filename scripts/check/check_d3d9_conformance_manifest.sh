#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
manifest="$repo_root/tests/conformance/d3d9/MANIFEST.toml"

"$repo_root/scripts/run_python.sh" - "$repo_root" "$manifest" <<'PY'
from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

repo_root = Path(sys.argv[1])
manifest_path = Path(sys.argv[2])

required_fields = {
    "executable",
    "function",
    "source",
    "source_kind",
    "license",
    "license_scope",
    "upstream_commit",
    "lanes",
    "arches",
    "area",
    "owner",
    "requirements",
    "acceptance",
    "status",
}
valid_lanes = {"app-local", "builtin"}
valid_arches = {"x64", "x86"}
valid_status = {"passing", "failing", "partial", "skipped", "scaffolded", "todo"}
evidence_status = {"passing", "failing", "skipped"}
# Two provenance shapes, and they must not be confused. Almost every case is a
# Wine behavioral oracle: LGPL, not vendored, anchored to an upstream commit.
# A handful are dxmt9's own policy tests, written here to pin behaviour Wine has
# no test for -- those are MIT and must NOT carry a wine/ anchor or an
# upstream_commit, because claiming upstream provenance for our own code is a
# false license claim in the exact field that exists to prevent one.
valid_source_kind = {"behavioral-oracle", "dxmt9-policy"}
valid_license_scope = {"external-not-vendored", "dxmt9"}
requirement_re = re.compile(r"^R-TEST-12\.\d+$")
evidence_source_re = re.compile(r"^(?P<path>[^:]+):(?P<line>[1-9]\d*)$")

if not manifest_path.is_file():
    raise SystemExit(f"manifest missing: {manifest_path}")

with manifest_path.open("rb") as handle:
    data = tomllib.load(handle)

cases = data.get("case")
if not isinstance(cases, list) or not cases:
    raise SystemExit("manifest has no [[case]] entries")

seen: set[tuple[str, str]] = set()
errors: list[str] = []

for index, case in enumerate(cases, 1):
    if not isinstance(case, dict):
        errors.append(f"case #{index}: entry is not a table")
        continue

    # upstream_commit is required for Wine oracles and forbidden for dxmt9's own
    # policy tests, so it is not in the unconditional required set.
    needed = required_fields
    if case.get("source_kind") == "dxmt9-policy":
        needed = required_fields - {"upstream_commit"}
    missing = sorted(needed - case.keys())
    if missing:
        errors.append(f"case #{index}: missing fields: {', '.join(missing)}")

    executable = case.get("executable")
    function = case.get("function")
    if isinstance(executable, str) and isinstance(function, str):
        key = (executable, function)
        if key in seen:
            errors.append(f"case #{index}: duplicate executable/function {key}")
        seen.add(key)
    else:
        errors.append(f"case #{index}: executable and function must be strings")

    status = case.get("status")
    if status not in valid_status:
        errors.append(f"case #{index}: invalid status {status!r}")

    lanes = case.get("lanes")
    if not isinstance(lanes, list) or not lanes or any(lane not in valid_lanes for lane in lanes):
        errors.append(f"case #{index}: lanes must be non-empty subset of {sorted(valid_lanes)}")

    arches = case.get("arches")
    if not isinstance(arches, list) or not arches or any(arch not in valid_arches for arch in arches):
        errors.append(f"case #{index}: arches must be non-empty subset of {sorted(valid_arches)}")

    evidence = case.get("evidence", [])
    if not isinstance(evidence, list):
        errors.append(f"case #{index}: evidence must be a list of tables")
        evidence = []

    evidence_keys: set[tuple[str, str]] = set()
    evidence_statuses: set[str] = set()
    for evidence_index, item in enumerate(evidence, 1):
        if not isinstance(item, dict):
            errors.append(f"case #{index} evidence #{evidence_index}: entry is not a table")
            continue

        item_lane = item.get("lane")
        item_arch = item.get("arch")
        item_status = item.get("status")
        item_summary = item.get("summary")
        item_source = item.get("source")

        if item_lane not in valid_lanes:
            errors.append(f"case #{index} evidence #{evidence_index}: invalid lane {item_lane!r}")
        elif isinstance(lanes, list) and item_lane not in lanes:
            errors.append(f"case #{index} evidence #{evidence_index}: lane {item_lane!r} not declared by case")

        if item_arch not in valid_arches:
            errors.append(f"case #{index} evidence #{evidence_index}: invalid arch {item_arch!r}")
        elif isinstance(arches, list) and item_arch not in arches:
            errors.append(f"case #{index} evidence #{evidence_index}: arch {item_arch!r} not declared by case")

        if item_status not in evidence_status:
            errors.append(f"case #{index} evidence #{evidence_index}: invalid status {item_status!r}")
        else:
            evidence_statuses.add(item_status)

        if isinstance(item_lane, str) and isinstance(item_arch, str):
            evidence_key = (item_lane, item_arch)
            if evidence_key in evidence_keys:
                errors.append(f"case #{index} evidence #{evidence_index}: duplicate lane/arch {evidence_key}")
            evidence_keys.add(evidence_key)

        if not isinstance(item_summary, str) or not item_summary:
            errors.append(f"case #{index} evidence #{evidence_index}: summary must be a non-empty string")

        if not isinstance(item_source, str):
            errors.append(f"case #{index} evidence #{evidence_index}: source must be a path:line string")
        else:
            match = evidence_source_re.match(item_source)
            if not match:
                errors.append(f"case #{index} evidence #{evidence_index}: source must be a path:line string")
            else:
                evidence_path = repo_root / match.group("path")
                if not evidence_path.is_file():
                    errors.append(f"case #{index} evidence #{evidence_index}: evidence source does not exist: {item_source}")
                elif int(match.group("line")) > len(evidence_path.read_text(encoding="utf-8").splitlines()):
                    errors.append(f"case #{index} evidence #{evidence_index}: evidence source line is out of range: {item_source}")

    if status in {"passing", "failing", "partial", "skipped"} and not evidence:
        errors.append(f"case #{index}: status {status!r} requires lane/arch evidence")
    if status == "scaffolded" and evidence:
        errors.append(f"case #{index}: scaffolded status must not carry runtime evidence")
    if status == "todo" and evidence:
        errors.append(f"case #{index}: todo status must not carry runtime evidence")
    if status == "failing" and "failing" not in evidence_statuses:
        errors.append(f"case #{index}: failing status requires failing evidence")
    if status == "partial" and "failing" in evidence_statuses:
        errors.append(f"case #{index}: partial status cannot include failing evidence")
    if status == "passing" and isinstance(lanes, list) and isinstance(arches, list):
        expected_keys = {(lane, arch) for lane in lanes for arch in arches}
        if evidence_keys != expected_keys or evidence_statuses != {"passing"}:
            errors.append(f"case #{index}: passing status requires passing evidence for every declared lane/arch")

    requirements = case.get("requirements")
    if not isinstance(requirements, list) or not requirements:
        errors.append(f"case #{index}: requirements must be a non-empty list")
    elif any(not isinstance(req, str) or not requirement_re.match(req) for req in requirements):
        errors.append(f"case #{index}: requirements must use R-TEST-12.x anchors")

    acceptance = case.get("acceptance")
    if not isinstance(acceptance, list) or not acceptance or any(not isinstance(item, str) or not item for item in acceptance):
        errors.append(f"case #{index}: acceptance must be a non-empty string list")

    source = case.get("source")
    source_kind = case.get("source_kind")
    license_value = case.get("license")
    license_scope = case.get("license_scope")
    upstream_commit = case.get("upstream_commit")
    if source_kind not in valid_source_kind:
        errors.append(f"case #{index}: source_kind must be {sorted(valid_source_kind)}")
    elif source_kind == "dxmt9-policy":
        if not isinstance(source, str) or source.startswith("wine/"):
            errors.append(f"case #{index}: dxmt9-policy source must not be a wine/... anchor")
        if license_value != "MIT":
            errors.append(f"case #{index}: dxmt9-policy license must be 'MIT'")
        if license_scope != "dxmt9":
            errors.append(f"case #{index}: dxmt9-policy license_scope must be 'dxmt9'")
        if upstream_commit is not None:
            errors.append(f"case #{index}: dxmt9-policy must not claim an upstream_commit")
    else:
        if not isinstance(source, str) or not source.startswith("wine/"):
            errors.append(f"case #{index}: source must be a wine/... anchor")
        if license_value != "LGPL-2.1-or-later":
            errors.append(f"case #{index}: license must be 'LGPL-2.1-or-later'")
        if license_scope != "external-not-vendored":
            errors.append(f"case #{index}: license_scope must be 'external-not-vendored'")
        if not isinstance(upstream_commit, str) or not re.fullmatch(r"[0-9a-f]{40}", upstream_commit):
            errors.append(f"case #{index}: upstream_commit must be a 40-character hex commit")

    source_file = case.get("source_file")
    if source_file is not None:
        if not isinstance(source_file, str):
            errors.append(f"case #{index}: source_file must be a string")
            continue
        local_path = repo_root / "tests" / "conformance" / "d3d9" / source_file
        if not local_path.is_file():
            errors.append(f"case #{index}: source_file does not exist: {source_file}")
            continue
        text = local_path.read_text(encoding="utf-8")
        if isinstance(function, str) and function not in text:
            errors.append(f"case #{index}: function {function!r} not found in {source_file}")

if errors:
    for error in errors:
        print(error, file=sys.stderr)
    raise SystemExit(1)

print(f"d3d9 conformance manifest ok: {len(cases)} cases")
PY
