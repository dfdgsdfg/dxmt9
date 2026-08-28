#!/usr/bin/env bash
set -euo pipefail

# Build the tool-winctl Win32 GUI automation helper (see
# experiments/apps/tool-winctl/tool_winctl.cpp). Produces
# tool-winctl-{x64,x86}.exe beside the source, matching the fixture layout
# used by build_conf-d3d9-fast-sanity.sh.

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

if [[ -d "$HOME/llvm-mingw/bin" ]]; then
  export PATH="$HOME/llvm-mingw/bin:$PATH"
fi

build_one() {
  local cxx=$1
  local arch=$2

  if ! command -v "$cxx" >/dev/null 2>&1; then
    printf 'error: missing cross compiler %s\n' "$cxx" >&2
    exit 1
  fi

  local app_dir="$repo_root/experiments/apps/tool-winctl"
  set -x
  "$cxx" \
    -std=c++20 \
    -O2 \
    -Wall \
    -Wextra \
    -o "$app_dir/tool-winctl-$arch.exe" \
    "$app_dir/tool_winctl.cpp" \
    -luser32
  { set +x; } 2>/dev/null
}

build_one "${CXX_X64:-x86_64-w64-mingw32-clang++}" "x64"
build_one "${CXX_X86:-i686-w64-mingw32-clang++}" "x86"
