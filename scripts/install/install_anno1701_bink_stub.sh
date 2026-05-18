#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
game_dir="$HOME/games/_Heroic/Anno 1701 AD"
toolchain_bin="$HOME/llvm-mingw/bin"
stub_dir="$repo_root/tools/anno1701_bink_stub"
out_dll="$stub_dir/build/binkw32.dll"

usage() {
  cat <<'EOF'
Usage:
  bash scripts/install/install_anno1701_bink_stub.sh [--game-dir <path>] [--toolchain-bin <path>]

Builds a 32-bit native binkw32.dll stub and installs it into the Anno 1701 game
directory, backing up the original DLL as binkw32.dll.dxmt9-backup on first use.
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
  "$stub_dir/binkw32_stub.c" \
  -Wl,"$stub_dir/binkw32.def"

target="$game_dir/binkw32.dll"
backup="$target.dxmt9-backup"
if [[ ! -f "$target" ]]; then
  echo "error: missing target DLL: $target" >&2
  exit 1
fi
if [[ ! -f "$backup" ]]; then
  cp -f "$target" "$backup"
fi
cp -f "$out_dll" "$target"
echo "installed $out_dll -> $target"
