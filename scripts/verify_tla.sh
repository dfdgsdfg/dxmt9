#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tla_dir="$repo_root/specs/verification/tla"

jar_dir=""
if command -v tlc >/dev/null 2>&1; then
  tlc_cmd=(tlc)
else
  if [[ -n "${TLA2TOOLS_JAR:-}" && -f "${TLA2TOOLS_JAR}" ]]; then
    jar_path="$TLA2TOOLS_JAR"
  else
    jar_dir="$(mktemp -d)"
    jar_path="$jar_dir/tla2tools.jar"
    curl -fsSL -o "$jar_path" \
      https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar
  fi
  tlc_cmd=(java -cp "$jar_path" tlc2.TLC)
fi

cleanup() {
  if [[ -n "${metadir:-}" && -d "${metadir:-}" ]]; then
    rm -rf "$metadir"
  fi
  if [[ -n "${jar_dir:-}" && -d "$jar_dir" ]]; then
    rm -rf "$jar_dir"
  fi
}
trap cleanup EXIT

for spec in "$tla_dir"/*.tla; do
  cfg="${spec%.tla}.cfg"
  metadir="$(mktemp -d)"
  echo "=== $(basename "$spec") ==="
  "${tlc_cmd[@]}" -workers auto -metadir "$metadir" -config "$cfg" "$spec"
  rm -rf "$metadir"
  unset metadir
done
