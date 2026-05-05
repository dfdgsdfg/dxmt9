#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$repo_root/tests/shader_runner/corpus/MANIFEST.toml"
tests_dir="$repo_root/tests/shader_runner/corpus"

if [[ ! -f "$manifest" ]]; then
  echo "manifest missing: $manifest" >&2
  exit 1
fi

actual_files=()
while IFS= read -r file; do
  [[ -z "$file" ]] && continue
  actual_files+=("$file")
done < <(find "$tests_dir" -name '*.shader_test' -type f | sed "s#^$repo_root/tests/shader_runner/corpus/##" | sort)

manifest_files=()
while IFS= read -r file; do
  [[ -z "$file" ]] && continue
  manifest_files+=("$file")
done < <(grep -E '^[[:space:]]*file[[:space:]]*=' "$manifest" | sed -E 's/.*"([^"]+)".*/\1/' | sort)

diff_output="$(
  comm -3 \
    <(printf '%s\n' "${actual_files[@]}") \
    <(printf '%s\n' "${manifest_files[@]}") || true
)"
if [[ -n "$diff_output" ]]; then
  echo "manifest mismatch:" >&2
  printf '%s\n' "$diff_output" >&2
  exit 1
fi

python3 - "$manifest" <<'PY'
from __future__ import annotations

import sys
import tomllib
from pathlib import Path

manifest = Path(sys.argv[1])
required = {"file", "status", "source", "source_kind", "license", "license_scope", "oracle", "oracle-date"}
valid_source_kind = {
    "project-authored",
    "behavioral-oracle",
    "structure-reference",
    "third-party-fixture",
    "implementation-source",
}
valid_license_scope = {"project-mit", "third-party-fixture", "external-not-vendored"}

data = tomllib.loads(manifest.read_text())
errors: list[str] = []
for index, entry in enumerate(data.get("test", []), 1):
    missing = sorted(required - entry.keys())
    if missing:
        errors.append(f"manifest test #{index}: missing fields: {', '.join(missing)}")
    if entry.get("source_kind") not in valid_source_kind:
        errors.append(f"manifest test #{index}: invalid source_kind {entry.get('source_kind')!r}")
    if entry.get("license_scope") not in valid_license_scope:
        errors.append(f"manifest test #{index}: invalid license_scope {entry.get('license_scope')!r}")
    if entry.get("license_scope") == "project-mit" and entry.get("license") != "MIT":
        errors.append(f"manifest test #{index}: project-mit entries must use license = \"MIT\"")

if errors:
    for error in errors:
        print(error, file=sys.stderr)
    raise SystemExit(1)
PY

missing_provenance=0
for file in "${actual_files[@]}"; do
  full_path="$tests_dir/$file"
  source_line="$(grep -m1 '^; source:' "$full_path" || true)"
  source_value="${source_line#; source: }"
  if ! grep -q '^; \[provenance\]' "$full_path" || ! grep -q '^; source:' "$full_path" || ! grep -q '^; source_kind:' "$full_path" || ! grep -q '^; license:' "$full_path" || ! grep -q '^; license_scope:' "$full_path" || ! grep -q '^; oracle:' "$full_path" || ! grep -q '^; oracle-date:' "$full_path"; then
    echo "missing provenance fields: $file" >&2
    missing_provenance=1
  elif [[ "$source_value" == vkd3d || "$source_value" == wine/visual.c:* ]]; then
    if ! grep -q '^; upstream-commit:' "$full_path"; then
      echo "missing upstream provenance: $file" >&2
      missing_provenance=1
    fi
  fi
done

if [[ "$missing_provenance" -ne 0 ]]; then
  exit 1
fi

echo "manifest ok: ${#actual_files[@]} shader tests"
