#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$repo_root/tests/shader_tests/MANIFEST.toml"

if [[ ! -f "$manifest" ]]; then
  echo "manifest missing: $manifest" >&2
  exit 1
fi

echo "shader corpus drift report"
echo "manifest: $manifest"
echo

current_section=""
while IFS= read -r line; do
  trimmed="${line#"${line%%[!$' \t']*}"}"
  trimmed="${trimmed%"${trimmed##*[!$' \t']}"}"
  case "$trimmed" in
    '[[test]]')
      current_section="test"
      echo "---"
      ;;
    file\ =*)
      echo "${trimmed#file = }"
      ;;
    source\ =*)
      echo "${trimmed#source = }"
      ;;
    oracle\ =*)
      echo "${trimmed#oracle = }"
      ;;
  esac
done < "$manifest"

echo
echo "report-only: no drift enforcement is applied yet"
