#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

wine_root_default="$HOME/Library/Application Support/heroic/tools/wine/Wine-11.6-DXMT/Contents/Resources/wine"
prefix_default="$HOME/Games/_Prefixes/Street Fighter IV Benchmark"
binary_default="$HOME/games/_Heroic/Street Fighter IV Benchmark/Benchmark.exe"

args=("$@")
has_wine_root=false
has_prefix=false
has_binary=false
for ((i=0; i<${#args[@]}; ++i)); do
  if [[ "${args[i]}" == "--wine-root" ]]; then
    has_wine_root=true
  fi
  if [[ "${args[i]}" == "--prefix" ]]; then
    has_prefix=true
  fi
  if [[ "${args[i]}" == "--binary" ]]; then
    has_binary=true
  fi
done

cmd=(python3 "$repo_root/scripts/run_experiment.py" run street-fighter-iv-benchmark)
if [[ "$has_wine_root" == false ]]; then
  cmd+=(--wine-root "$wine_root_default")
fi
if [[ "$has_prefix" == false ]]; then
  cmd+=(--prefix "$prefix_default")
fi
if [[ "$has_binary" == false ]]; then
  cmd+=(--binary "$binary_default")
fi
cmd+=("${args[@]}")

"${cmd[@]}"
