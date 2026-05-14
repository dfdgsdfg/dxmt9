#!/usr/bin/env bash
set -euo pipefail

# Interactive installer wrapper for the 3DMark series. Runs the user-
# supplied installer EXE inside the named prefix under the sikarugir
# wine runtime, and reminds the user to pick the D:\ drive as the
# install destination so the resulting binary lands at the path the
# CATALOGUE expects (experiments/apps_3rd/<name>/Program Files/...).
#
# Usage:
#   bash scripts/run_apps/install_3dmark.sh <prefix-name> <installer.exe>
#
# Example:
#   bash scripts/run_apps/install_3dmark.sh 3dmark03 \
#       /Users/dididi/Downloads/3DMark03.exe

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <prefix-name> <installer.exe>" >&2
  exit 2
fi

name="$1"
installer="$2"

if [[ ! -e "$installer" ]]; then
  echo "installer not found: $installer" >&2
  exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
prefix="$repo_root/experiments/prefixs/$name"
wine_root="$repo_root/experiments/wine/sikarugir-cx-24.0.7"
wine_bin="$wine_root/bin/wine"

if [[ ! -d "$prefix" ]]; then
  echo "prefix missing: $prefix" >&2
  echo "run: python3 scripts/wine/bootstrap_prefix.py --name $name --wine-id sikarugir-cx-24.0.7" >&2
  exit 1
fi

echo "=================================================================="
echo "Installing $name into prefix:"
echo "  $prefix"
echo ""
echo "When the InstallShield wizard reaches the 'Choose Destination"
echo "Location' step, change the destination to:"
echo "  D:\\Program Files\\Futuremark\\<benchmark-name>"
echo ""
echo "D:\\ in this prefix maps to:"
echo "  $repo_root/experiments/apps_3rd/$name/"
echo "=================================================================="

WINEPREFIX="$prefix" \
DXMT_VALIDATE=0 \
"$wine_bin" "$installer"

echo ""
echo "Installer exited. Verify the binary at the path declared in CATALOGUE:"
echo "  experiments/apps_3rd/$name/Program Files/Futuremark/.../*.exe"
