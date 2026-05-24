#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

usage() {
  cat <<'EOF'
Usage:
  bash scripts/install/install_heroic_experiment_prefix.sh --prefix <wine-prefix> [options]

Options:
  --prefix <path>         Target Heroic Wine prefix. Required.
  --wine-root <path>      Heroic Wine runtime root. Auto-detected when omitted.
  --skip-build            Do not rebuild repo-local experiment binaries before install.
  --help                  Show this message.

This is a permanent-prefix convenience wrapper for experiments. It:
  1. builds the repo-local sample exes
  2. installs dxmt9 into the selected prefix and Heroic runtime
  3. prints the exact experiment commands to reuse the same prefix
EOF
}

prefix=""
wine_root=""
skip_build=false

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
    --skip-build)
      skip_build=true
      shift
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

if [[ "$skip_build" != true ]]; then
  bash "$repo_root/scripts/build_apps/build_sample-d3d9-basic-hlsl.sh"
  bash "$repo_root/scripts/build_apps/build_sample-d3d9-tutorial07.sh"
  bash "$repo_root/scripts/build_apps/build_sample-d3d9-hdr-formats.sh"
  bash "$repo_root/scripts/build_apps/build_sample-d3d9-dxut-simple.sh"
  bash "$repo_root/scripts/build_apps/build_sample-d3d9-irrlicht-lights.sh"
fi

cmd=(
  bash "$repo_root/scripts/install/install_heroic_wine.sh"
  --prefix "$prefix"
  --pe-build-dir "$repo_root/build-win32-x64-builtin/src/win32"
  --unix-build-dir "$repo_root/build-x86_64-builtin/src"
)
if [[ -n "$wine_root" ]]; then
  cmd+=(--wine-root "$wine_root")
fi
"${cmd[@]}"

cat <<EOF

Experiment commands:
  python3 scripts/run_apps/run_experiment.py run sample-d3d9-basic-hlsl --build --prefix "$prefix"${wine_root:+ --wine-root "$wine_root"}
  python3 scripts/run_apps/run_experiment.py run sample-d3d9-tutorial07 --build --prefix "$prefix"${wine_root:+ --wine-root "$wine_root"}
  python3 scripts/run_apps/run_experiment.py run sample-d3d9-hdr-formats --build --prefix "$prefix"${wine_root:+ --wine-root "$wine_root"}
  python3 scripts/run_apps/run_experiment.py run sample-d3d9-dxut-simple --build --prefix "$prefix"${wine_root:+ --wine-root "$wine_root"}
  python3 scripts/run_apps/run_experiment.py run sample-d3d9-irrlicht-lights --build --prefix "$prefix"${wine_root:+ --wine-root "$wine_root"}
EOF
