#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

usage() {
  cat <<'EOF'
Usage:
  bash scripts/install_heroic_wine.sh --prefix <wine-prefix> [options]

Options:
  --prefix <path>         Target Wine prefix. Required.
  --wine-root <path>      Heroic Wine runtime root. Auto-detected when omitted.
  --pe-build-dir <path>   Directory containing d3d9.dll and dxmt9.dll.
                          Default: <repo>/build-win32-x64/src/win32
  --unix-build-dir <path> Directory containing dxmt9.so.
                          Default: <repo>/build-x86_64/src
  --mingw-bin-dir <path>  Directory containing libc++.dll and libunwind.dll.
                          Default: ~/llvm-mingw/x86_64-w64-mingw32/bin
  --help                  Show this message.

This script installs the currently-built dxmt9 binaries into a Heroic Wine
runtime and prefix:
  - d3d9.dll   -> <prefix>/drive_c/windows/system32
  - dxmt9.dll  -> <prefix>/drive_c/windows/system32
  - dxmt9.dll  -> <wine-root>/lib/wine/x86_64-windows
  - dxmt9.so   -> <wine-root>/lib/wine/x86_64-unix
  - libc++.dll -> <prefix>/drive_c/windows/system32
  - libunwind.dll -> <prefix>/drive_c/windows/system32

If an existing target file is present, a one-time .dxmt9-backup copy is created
before overwrite.
EOF
}

detect_heroic_wine_root() {
  local heroic_tools_root latest
  heroic_tools_root="$HOME/Library/Application Support/heroic/tools/wine"
  if [[ ! -d "$heroic_tools_root" ]]; then
    return 1
  fi

  latest=$(
    find "$heroic_tools_root" -maxdepth 1 -type d -name 'Wine-*' -print \
      | rg -v 'DXMT' \
      | sort -V \
      | tail -n 1
  )

  if [[ -z "$latest" ]]; then
    latest=$(
      find "$heroic_tools_root" -maxdepth 1 -type d -name 'Wine-*' -print \
        | sort -V \
        | tail -n 1
    )
  fi

  if [[ -z "$latest" ]]; then
    return 1
  fi

  printf '%s/Contents/Resources/wine\n' "$latest"
}

backup_if_needed() {
  local target backup
  target=$1
  backup="${target}.dxmt9-backup"
  if [[ -e "$target" && ! -e "$backup" ]]; then
    cp -p "$target" "$backup"
  fi
}

install_file() {
  local source target
  source=$1
  target=$2
  if [[ ! -f "$source" ]]; then
    printf 'error: required file not found: %s\n' "$source" >&2
    exit 1
  fi
  mkdir -p -- "$(dirname -- "$target")"
  backup_if_needed "$target"
  cp -f "$source" "$target"
  printf 'installed %s -> %s\n' "$source" "$target"
}

prefix=""
wine_root=""
pe_build_dir="$repo_root/build-win32-x64/src/win32"
unix_build_dir="$repo_root/build-x86_64/src"
mingw_bin_dir="$HOME/llvm-mingw/x86_64-w64-mingw32/bin"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)
      prefix=${2:-}
      shift 2
      ;;
    --wine-root)
      wine_root=${2:-}
      shift 2
      ;;
    --pe-build-dir)
      pe_build_dir=${2:-}
      shift 2
      ;;
    --unix-build-dir)
      unix_build_dir=${2:-}
      shift 2
      ;;
    --mingw-bin-dir)
      mingw_bin_dir=${2:-}
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      printf 'error: unknown argument: %s\n\n' "$1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$prefix" ]]; then
  printf 'error: --prefix is required\n\n' >&2
  usage >&2
  exit 1
fi

if [[ -z "$wine_root" ]]; then
  if ! wine_root=$(detect_heroic_wine_root); then
    printf 'error: could not auto-detect a Heroic Wine runtime; pass --wine-root\n' >&2
    exit 1
  fi
fi

system32_dir="$prefix/drive_c/windows/system32"
windows_runtime_dir="$wine_root/lib/wine/x86_64-windows"
unix_runtime_dir="$wine_root/lib/wine/x86_64-unix"

if [[ ! -d "$windows_runtime_dir" ]]; then
  printf 'error: missing Heroic Windows runtime dir: %s\n' "$windows_runtime_dir" >&2
  exit 1
fi

if [[ ! -d "$unix_runtime_dir" ]]; then
  printf 'error: missing Heroic unix runtime dir: %s\n' "$unix_runtime_dir" >&2
  exit 1
fi

if [[ ! -f "$unix_build_dir/dxmt9.so" && -f "$repo_root/build/src/dxmt9.so" ]]; then
  unix_build_dir="$repo_root/build/src"
fi

install_file "$pe_build_dir/d3d9.dll" "$system32_dir/d3d9.dll"
install_file "$pe_build_dir/dxmt9.dll" "$system32_dir/dxmt9.dll"
install_file "$pe_build_dir/dxmt9.dll" "$windows_runtime_dir/dxmt9.dll"
install_file "$unix_build_dir/dxmt9.so" "$unix_runtime_dir/dxmt9.so"
install_file "$mingw_bin_dir/libc++.dll" "$system32_dir/libc++.dll"
install_file "$mingw_bin_dir/libunwind.dll" "$system32_dir/libunwind.dll"

wine_bin="$wine_root/bin/wine"
if [[ ! -x "$wine_bin" && -x "$wine_root/bin/wine64" ]]; then
  wine_bin="$wine_root/bin/wine64"
fi

cat <<EOF

dxmt9 install complete.

Prefix:
  $prefix

Wine runtime:
  $wine_root

Run with:
  WINEPREFIX="$prefix" WINEDLLOVERRIDES="d3d9=n,b" "$wine_bin" game.exe
EOF
