#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$repo_root/tests/conformance/d3d9/MANIFEST.toml"

python3 - "$repo_root" "$manifest" <<'PY'
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
valid_status = {"passing", "failing", "skipped", "scaffolded", "todo"}
requirement_re = re.compile(r"^R-TEST-12\.\d+$")

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

    missing = sorted(required_fields - case.keys())
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

    requirements = case.get("requirements")
    if not isinstance(requirements, list) or not requirements:
        errors.append(f"case #{index}: requirements must be a non-empty list")
    elif any(not isinstance(req, str) or not requirement_re.match(req) for req in requirements):
        errors.append(f"case #{index}: requirements must use R-TEST-12.x anchors")

    acceptance = case.get("acceptance")
    if not isinstance(acceptance, list) or not acceptance or any(not isinstance(item, str) or not item for item in acceptance):
        errors.append(f"case #{index}: acceptance must be a non-empty string list")

    source = case.get("source")
    upstream_commit = case.get("upstream_commit")
    if not isinstance(source, str) or not source.startswith("wine/"):
        errors.append(f"case #{index}: source must be a wine/... anchor")
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
