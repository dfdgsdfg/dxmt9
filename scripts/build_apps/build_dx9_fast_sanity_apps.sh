#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
common_inc="$repo_root/experiments/apps/common"

if [[ -d "$HOME/llvm-mingw/bin" ]]; then
  export PATH="$HOME/llvm-mingw/bin:$PATH"
fi

apps=(
  "D9VKD3D9Clear:d3d9_clear.cpp:d3d9-clear"
  "D9VKD3D9Buffer:d3d9_buffer.cpp:d3d9-buffer"
  "D9VKD3D9FixedFunctionQuirks:d3d9_fixed_function_quirks.cpp:d3d9-ffp-quirks"
  "D9VKD3D9LockMatrix:d3d9_lock_matrix.cpp:d3d9-lock-matrix"
  "D9VKD3D9Triangle:d3d9_triangle.cpp:d3d9-triangle"
)

build_one() {
  local cxx=$1
  local arch=$2
  local dir_name=$3
  local source_name=$4
  local output_stem=$5

  if ! command -v "$cxx" >/dev/null 2>&1; then
    printf 'error: missing cross compiler %s\n' "$cxx" >&2
    exit 1
  fi

  local app_dir="$repo_root/experiments/apps/$dir_name"
  local source_file="$app_dir/$source_name"
  local output_file="$app_dir/$output_stem-$arch.exe"

  mkdir -p "$app_dir"

  set -x
  "$cxx" \
    -std=c++20 \
    -O2 \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -I"$common_inc" \
    -o "$output_file" \
    "$source_file" \
    -ld3d9 \
    -ld3dx9_43 \
    -ldxguid \
    -lgdi32 \
    -luser32 \
    -lshell32 \
    -lole32 \
    -luuid
  { set +x; } 2>/dev/null
}

for app in "${apps[@]}"; do
  IFS=: read -r dir_name source_name output_stem <<<"$app"
  build_one "${CXX_X64:-x86_64-w64-mingw32-clang++}" "x64" "$dir_name" "$source_name" "$output_stem"
  build_one "${CXX_X86:-i686-w64-mingw32-clang++}" "x86" "$dir_name" "$source_name" "$output_stem"
done
