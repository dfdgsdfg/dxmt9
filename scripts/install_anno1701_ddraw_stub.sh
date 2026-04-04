#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
game_dir="$HOME/games/_Heroic/Anno 1701 AD"
toolchain_bin="$HOME/llvm-mingw/bin"
stub_dir="$repo_root/tools/anno1701_bink_stub"
out_dll="$stub_dir/build/ddraw.dll"

usage() {
  cat <<'EOF'
Usage:
  bash scripts/install_anno1701_ddraw_stub.sh [--game-dir <path>] [--toolchain-bin <path>]

Builds a 32-bit native ddraw.dll stub and installs it into the Anno 1701 game
directory. This only affects local dynamic loads because the game directory is
searched before Wine's system ddraw.dll.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --game-dir)
      game_dir=${2:-}
      shift 2
      ;;
    --toolchain-bin)
      toolchain_bin=${2:-}
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

mkdir -p "$stub_dir/build"
export PATH="$toolchain_bin:$PATH"

i686-w64-mingw32-clang \
  -shared \
  -O2 \
  -s \
  -o "$out_dll" \
  "$stub_dir/ddraw_stub.c" \
  "$stub_dir/ddraw.def" \
  -lole32 \
  -luuid \
  -ldxguid

target="$game_dir/ddraw.dll"
cp -f "$out_dll" "$target"
echo "installed $out_dll -> $target"
