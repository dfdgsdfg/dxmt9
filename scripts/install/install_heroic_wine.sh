#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

usage() {
  cat <<'EOF'
Usage:
  bash scripts/install/install_heroic_wine.sh --prefix <wine-prefix> [options]

Options:
  --prefix <path>         Target Wine prefix. Required.
  --wine-root <path>      Heroic Wine runtime root. Auto-detected when omitted.
  --pe-build-dir <path>   Directory containing d3d9.dll for
                          <prefix>/system32 and
                          <wine-root>/lib/wine/x86_64-windows.
                          Default: <repo>/build-win32-x64-builtin/src/win32
  --runtime-pe-build-dir <path>
                          Directory containing builtin winemetal.dll for
                          <prefix>/system32 and
                          <wine-root>/lib/wine/x86_64-windows.
                          Default: <repo>/build-win32-x64-builtin/src/winemetal
  --wow64-pe-build-dir <path>
                          Directory containing 32-bit d3d9.dll
                          for <prefix>/syswow64.
  --wow64-runtime-pe-build-dir <path>
                          Directory containing builtin 32-bit winemetal.dll for
                          <prefix>/syswow64 and
                          <wine-root>/lib/wine/i386-windows.
                          Default: <repo>/build-win32-x86-builtin/src/winemetal
  --unix-build-dir <path> Directory containing winemetal.so.
                          Default: <repo>/build-x86_64-builtin/src
  --mingw-bin-dir <path>  Directory containing libc++.dll and libunwind.dll.
                          Default: ~/llvm-mingw/x86_64-w64-mingw32/bin
  --wow64-mingw-bin-dir <path>
                          Directory containing 32-bit libc++.dll and
                          libunwind.dll. Default: ~/llvm-mingw/i686-w64-mingw32/bin
  --help                  Show this message.

This script installs the currently-built dxmt9 binaries into a Heroic Wine
runtime and prefix:
  64-bit lane:
  - d3d9.dll      -> <prefix>/drive_c/windows/system32
  - d3d9.dll      -> <wine-root>/lib/wine/x86_64-windows
  - winemetal.dll -> <prefix>/drive_c/windows/system32
  - winemetal.dll -> <wine-root>/lib/wine/x86_64-windows
  - winemetal.so  -> <wine-root>/lib/wine/x86_64-unix
  - libc++.dll    -> <prefix>/drive_c/windows/system32
  - libunwind.dll -> <prefix>/drive_c/windows/system32

  Optional WoW64 32-bit lane:
  - d3d9.dll      -> <prefix>/drive_c/windows/syswow64
  - d3d9.dll      -> <wine-root>/lib/wine/i386-windows
  - winemetal.dll -> <prefix>/drive_c/windows/syswow64
  - winemetal.dll -> <wine-root>/lib/wine/i386-windows
  - libc++.dll    -> <prefix>/drive_c/windows/syswow64
  - libunwind.dll -> <prefix>/drive_c/windows/syswow64

If an existing target file is present, a one-time .dxmt9-backup copy is created
before overwrite.

For 32-bit D3D9 games under Heroic, the game's config must also enable WoW64:
  "enableWoW64": true
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

find_meson_build_root() {
  local artifact build_root parent
  artifact=$1

  build_root=$(dirname -- "$artifact")
  while [[ "$build_root" != "/" ]]; do
    if [[ -f "$build_root/build.ninja" ]]; then
      printf '%s\n' "$build_root"
      return 0
    fi
    parent=$(dirname -- "$build_root")
    if [[ "$parent" == "$build_root" ]]; then
      break
    fi
    build_root=$parent
  done

  return 1
}

ensure_meson_postprocess() {
  local artifact build_root rel_target
  artifact=$1

  if [[ ! -f "$artifact" ]]; then
    return 0
  fi

  if ! build_root=$(find_meson_build_root "$artifact"); then
    return 0
  fi

  rel_target=${artifact#"$build_root"/}.postproc
  if [[ "$rel_target" == "$artifact.postproc" ]]; then
    return 0
  fi

  if ! rg -q --fixed-strings "build $rel_target:" "$build_root/build.ninja"; then
    return 0
  fi

  if ! command -v ninja >/dev/null 2>&1; then
    printf 'error: ninja is required to run Meson postprocess target: %s\n' "$rel_target" >&2
    exit 1
  fi

  ninja -C "$build_root" "$rel_target"
}

ensure_meson_artifact() {
  local artifact build_root rel_target
  artifact=$1

  if ! build_root=$(find_meson_build_root "$artifact"); then
    return 0
  fi

  rel_target=${artifact#"$build_root"/}
  if [[ "$rel_target" == "$artifact" ]]; then
    return 0
  fi

  if ! rg -q --fixed-strings "build $rel_target:" "$build_root/build.ninja"; then
    return 0
  fi

  if ! command -v ninja >/dev/null 2>&1; then
    printf 'error: ninja is required to build Meson target: %s\n' "$rel_target" >&2
    exit 1
  fi

  ninja -C "$build_root" "$rel_target"
}

ensure_winemetal_unix_fixup() {
  local artifact build_root rel_artifact rel_dir rel_target
  artifact=$1

  if ! build_root=$(find_meson_build_root "$artifact"); then
    return 0
  fi

  rel_artifact=${artifact#"$build_root"/}
  if [[ "$rel_artifact" == "$artifact" ]]; then
    return 0
  fi

  rel_dir=$(dirname -- "$rel_artifact")
  rel_target="$rel_dir/winemetal_unix_install_name_fixup.stamp"
  if ! rg -q --fixed-strings "build $rel_target:" "$build_root/build.ninja"; then
    return 0
  fi

  if ! command -v ninja >/dev/null 2>&1; then
    printf 'error: ninja is required to run Meson fixup target: %s\n' "$rel_target" >&2
    exit 1
  fi

  ninja -C "$build_root" "$rel_target"
}

audit_winemetal_unix_install_names() {
  local artifact bad_deps
  artifact=$1

  if [[ "$(uname -s)" != "Darwin" ]]; then
    return 0
  fi
  if ! command -v otool >/dev/null 2>&1; then
    return 0
  fi

  bad_deps=$(
    otool -L "$artifact" |
      awk 'NR > 1 { print $1 }' |
      grep -E '^(winemac|ntdll)\.so$' || true
  )
  if [[ -n "$bad_deps" ]]; then
    printf 'error: %s still has bare Wine unix deps after fixup:\n%s\n' \
      "$artifact" "$bad_deps" >&2
    printf 'run the Meson target winemetal_unix_install_name_fixup before staging.\n' >&2
    exit 1
  fi
}


prefix=""
wine_root=""
pe_build_dir="$repo_root/build-win32-x64-builtin/src/win32"
runtime_pe_build_dir="$repo_root/build-win32-x64-builtin/src/winemetal"
wow64_pe_build_dir=""
wow64_runtime_pe_build_dir=""
unix_build_dir="$repo_root/build-x86_64-builtin/src"
mingw_bin_dir="$HOME/llvm-mingw/x86_64-w64-mingw32/bin"
wow64_mingw_bin_dir="$HOME/llvm-mingw/i686-w64-mingw32/bin"

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
    --runtime-pe-build-dir)
      runtime_pe_build_dir=${2:-}
      shift 2
      ;;
    --wow64-pe-build-dir)
      wow64_pe_build_dir=${2:-}
      shift 2
      ;;
    --wow64-runtime-pe-build-dir)
      wow64_runtime_pe_build_dir=${2:-}
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
    --wow64-mingw-bin-dir)
      wow64_mingw_bin_dir=${2:-}
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
syswow64_dir="$prefix/drive_c/windows/syswow64"
windows_runtime_dir="$wine_root/lib/wine/x86_64-windows"
i386_windows_runtime_dir="$wine_root/lib/wine/i386-windows"
unix_runtime_dir="$wine_root/lib/wine/x86_64-unix"

if [[ ! -d "$windows_runtime_dir" ]]; then
  printf 'error: missing Heroic Windows runtime dir: %s\n' "$windows_runtime_dir" >&2
  exit 1
fi

if [[ ! -d "$unix_runtime_dir" ]]; then
  printf 'error: missing Heroic unix runtime dir: %s\n' "$unix_runtime_dir" >&2
  exit 1
fi

if [[ -n "$wow64_runtime_pe_build_dir" && ! -d "$i386_windows_runtime_dir" ]]; then
  printf 'error: missing Heroic WoW64 runtime dir: %s\n' "$i386_windows_runtime_dir" >&2
  exit 1
fi

if [[ ! -f "$unix_build_dir/winemetal/unix/winemetal.so" && -f "$repo_root/build/src/winemetal/unix/winemetal.so" ]]; then
  unix_build_dir="$repo_root/build/src"
fi

if [[ ! -f "$runtime_pe_build_dir/winemetal.dll" ]]; then
  printf 'error: could not locate winemetal.dll in runtime PE build dir: %s\n' "$runtime_pe_build_dir" >&2
  exit 1
fi

if [[ -z "$wow64_runtime_pe_build_dir" && -n "$wow64_pe_build_dir" ]]; then
  wow64_runtime_pe_build_dir="$wow64_pe_build_dir"
fi

if [[ -z "$wow64_runtime_pe_build_dir" && -f "$repo_root/build-win32-x86-builtin/src/winemetal/winemetal.dll" ]]; then
  if [[ -z "$wow64_pe_build_dir" ]]; then
    wow64_pe_build_dir="$repo_root/build-win32-x86-builtin/src/win32"
  fi
  wow64_runtime_pe_build_dir="$repo_root/build-win32-x86-builtin/src/winemetal"
fi

if [[ -n "$wow64_runtime_pe_build_dir" ]]; then
  if [[ ! -f "$wow64_runtime_pe_build_dir/winemetal.dll" ]]; then
    printf 'error: could not locate 32-bit winemetal.dll in runtime PE build dir: %s\n' "$wow64_runtime_pe_build_dir" >&2
    exit 1
  fi
fi

ensure_meson_postprocess "$pe_build_dir/d3d9.dll"
ensure_meson_postprocess "$runtime_pe_build_dir/winemetal.dll"
if [[ -n "$wow64_pe_build_dir" ]]; then
  ensure_meson_postprocess "$wow64_pe_build_dir/d3d9.dll"
fi
if [[ -n "$wow64_runtime_pe_build_dir" ]]; then
  ensure_meson_postprocess "$wow64_runtime_pe_build_dir/winemetal.dll"
fi
winemetal_unix_so="$unix_build_dir/winemetal/unix/winemetal.so"
ensure_meson_artifact "$winemetal_unix_so"
ensure_winemetal_unix_fixup "$winemetal_unix_so"
audit_winemetal_unix_install_names "$winemetal_unix_so"

install_file "$pe_build_dir/d3d9.dll" "$system32_dir/d3d9.dll"
install_file "$pe_build_dir/d3d9.dll" "$windows_runtime_dir/d3d9.dll"
install_file "$runtime_pe_build_dir/winemetal.dll" "$system32_dir/winemetal.dll"
install_file "$runtime_pe_build_dir/winemetal.dll" "$windows_runtime_dir/winemetal.dll"
install_file "$winemetal_unix_so" "$unix_runtime_dir/winemetal.so"
install_file "$mingw_bin_dir/libc++.dll" "$system32_dir/libc++.dll"
install_file "$mingw_bin_dir/libunwind.dll" "$system32_dir/libunwind.dll"

if [[ -n "$wow64_pe_build_dir" ]]; then
  install_file "$wow64_pe_build_dir/d3d9.dll" "$syswow64_dir/d3d9.dll"
  install_file "$wow64_pe_build_dir/d3d9.dll" "$i386_windows_runtime_dir/d3d9.dll"
  install_file "$wow64_mingw_bin_dir/libc++.dll" "$syswow64_dir/libc++.dll"
  install_file "$wow64_mingw_bin_dir/libunwind.dll" "$syswow64_dir/libunwind.dll"
fi

if [[ -n "$wow64_runtime_pe_build_dir" ]]; then
  install_file "$wow64_runtime_pe_build_dir/winemetal.dll" "$syswow64_dir/winemetal.dll"
  install_file "$wow64_runtime_pe_build_dir/winemetal.dll" "$i386_windows_runtime_dir/winemetal.dll"
fi

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
  WINEPREFIX="$prefix" WINEDLLOVERRIDES="d3d9,winemetal=n,b" "$wine_bin" game.exe
EOF
