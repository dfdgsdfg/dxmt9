#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

# Vanilla Wine — see agents/rules/test_wild.rules.md.
# NOTE: anno-1404-gold is a documented exception that historically only ran
# under Wine-*-DXMT (vanilla Wine trips d3dx10_43 / D3DX10SaveTextureToMemory
# before the game becomes a usable baseline; see experiments/README.md).
# When that path is needed, override explicitly:
#   bash run_anno1404_experiment.sh --wine-root .../Wine-11.7-DXMT/.../wine
# Defaulting to vanilla here keeps the runner consistent with every other wild
# runner; the user opts into the patched runtime per-invocation.
wine_root_default="$HOME/Library/Application Support/heroic/tools/wine/Wine-11.7/Contents/Resources/wine"
prefix_default="$HOME/Games/_Prefixes/Anno 1404 Gold Edition"

args=("$@")
has_wine_root=false
has_prefix=false
for ((i=0; i<${#args[@]}; ++i)); do
  if [[ "${args[i]}" == "--wine-root" ]]; then
    has_wine_root=true
  fi
  if [[ "${args[i]}" == "--prefix" ]]; then
    has_prefix=true
  fi
done

cmd=(python3 "$repo_root/scripts/run_apps/run_experiment.py" run anno-1404-gold)
if [[ "$has_wine_root" == false ]]; then
  cmd+=(--wine-root "$wine_root_default")
fi
if [[ "$has_prefix" == false ]]; then
  cmd+=(--prefix "$prefix_default")
fi
cmd+=("${args[@]}")

"${cmd[@]}"
