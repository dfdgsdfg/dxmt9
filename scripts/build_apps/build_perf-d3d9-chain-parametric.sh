#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
app_dir="$repo_root/experiments/apps/perf-d3d9-chain-parametric"
source_file="$app_dir/perf_d3d9_chain_parametric.cpp"
output_file="$app_dir/perf-d3d9-chain-parametric.exe"

if [[ -d "$HOME/llvm-mingw/bin" ]]; then
  export PATH="$HOME/llvm-mingw/bin:$PATH"
fi

cxx=${CXX:-x86_64-w64-mingw32-clang++}
if ! command -v "$cxx" >/dev/null 2>&1; then
  printf 'error: missing cross compiler %s\n' "$cxx" >&2
  exit 1
fi

mkdir -p "$app_dir"

set -x
"$cxx" \
  -std=c++20 \
  -O2 \
  -Wall \
  -Wextra \
  -Wno-unused-parameter \
  -o "$output_file" \
  "$source_file" \
  -ld3d9 \
  -ldxguid \
  -lgdi32 \
  -luser32 \
  -lshell32 \
  -lole32 \
  -luuid
