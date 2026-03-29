#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$repo_root/tests/shader_tests/MANIFEST.toml"
tests_dir="$repo_root/tests/shader_tests"

if [[ ! -f "$manifest" ]]; then
  echo "manifest missing: $manifest" >&2
  exit 1
fi

actual_files=()
while IFS= read -r file; do
  [[ -z "$file" ]] && continue
  actual_files+=("$file")
done < <(find "$tests_dir" -name '*.shader_test' -type f | sed "s#^$repo_root/tests/shader_tests/##" | sort)

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

missing_provenance=0
for file in "${actual_files[@]}"; do
  full_path="$tests_dir/$file"
  source_line="$(grep -m1 '^; source:' "$full_path" || true)"
  source_value="${source_line#; source: }"
  if ! grep -q '^; \[provenance\]' "$full_path" || ! grep -q '^; source:' "$full_path" || ! grep -q '^; oracle:' "$full_path" || ! grep -q '^; oracle-date:' "$full_path"; then
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
