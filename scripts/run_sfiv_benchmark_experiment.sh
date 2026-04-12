#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

heroic_wine_root_default="$HOME/Library/Application Support/heroic/tools/wine/Wine-11.6-DXMT/Contents/Resources/wine"
crossover_root_default="$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
heroic_prefix_default="$HOME/Games/_Prefixes/Street Fighter IV Benchmark"
crossover_prefix_default="$HOME/Library/Application Support/CrossOver/Bottles/Heroic"
binary_default="$HOME/games/_Heroic/Street Fighter IV Benchmark/Benchmark.exe"
binary_default_alt="$HOME/games/_Heroic/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe"
cache_root_default="$HOME/.cache/dxmt9/sfiv-benchmark"
host_default="heroic"

discover_binary() {
  local candidates=(
    "$binary_default"
    "$binary_default_alt"
    "$HOME/games/Street Fighter IV Benchmark/Benchmark.exe"
    "$HOME/games/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe"
    "$HOME/Games/Street Fighter IV Benchmark/Benchmark.exe"
    "$HOME/Games/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe"
    "$HOME/Applications/Street Fighter IV Benchmark/Benchmark.exe"
    "$HOME/Applications/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe"
    "$HOME/Downloads/Street Fighter IV Benchmark/Benchmark.exe"
    "$HOME/Downloads/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe"
  )
  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

extract_benchmark_from_installer() {
  local installer_path="$1"
  local cache_root="$2"
  local wine_bin="$3"
  local temp_prefix="$cache_root/prefix"
  local extracted_root="$cache_root/extracted"
  local extracted_binary="$extracted_root/Program Files/CAPCOM/STREETFIGHTERIV_BENCHMARK/StreetFighterIV_Benchmark.exe"
  local cached_msi="$cache_root/StreetFighterIV_Benchmark.msi"
  local installer_download_root msi_path last_size stable_count current_size
  local min_expected_size=$((300 * 1024 * 1024))

  if [[ -f "$extracted_binary" ]]; then
    printf '%s\n' "$extracted_binary"
    return 0
  fi

  if ! command -v msiextract >/dev/null 2>&1; then
    echo "msiextract is required to unpack Street Fighter IV Benchmark installer." >&2
    echo "Install it with: brew install msitools" >&2
    return 2
  fi

  rm -rf "$temp_prefix" "$extracted_root"
  mkdir -p "$cache_root" "$temp_prefix" "$extracted_root"

  WINEPREFIX="$temp_prefix" \
    "$wine_bin" "$installer_path" /s >/dev/null 2>&1 &
  local installer_pid=$!

  installer_download_root="$temp_prefix/drive_c/users/$USER/AppData/Local/Downloaded Installations"
  msi_path=""
  for _ in $(seq 1 180); do
    msi_path=$(find "$installer_download_root" -type f -iname 'STREET FIGHTER IV BENCHMARK.msi' 2>/dev/null | head -n 1 || true)
    if [[ -n "$msi_path" ]]; then
      break
    fi
    sleep 1
  done

  kill "$installer_pid" >/dev/null 2>&1 || true
  wait "$installer_pid" >/dev/null 2>&1 || true

  if [[ -z "$msi_path" || ! -f "$msi_path" ]]; then
    kill "$installer_pid" >/dev/null 2>&1 || true
    wait "$installer_pid" >/dev/null 2>&1 || true
    echo "failed to extract STREET FIGHTER IV BENCHMARK.msi from installer: $installer_path" >&2
    return 3
  fi

  last_size=0
  stable_count=0
  for _ in $(seq 1 30); do
    current_size=$(python3 - <<'PY' "$msi_path"
from pathlib import Path
import sys
print(Path(sys.argv[1]).stat().st_size)
PY
)
    if [[ "$current_size" -ge "$min_expected_size" && "$current_size" -eq "$last_size" ]]; then
      stable_count=$((stable_count + 1))
      if [[ "$stable_count" -ge 3 ]]; then
        break
      fi
    else
      stable_count=0
      last_size="$current_size"
    fi
    sleep 1
  done

  current_size=$(python3 - <<'PY' "$msi_path"
from pathlib import Path
import sys
print(Path(sys.argv[1]).stat().st_size)
PY
)
  if [[ "$current_size" -lt "$min_expected_size" ]]; then
    kill "$installer_pid" >/dev/null 2>&1 || true
    wait "$installer_pid" >/dev/null 2>&1 || true
    echo "installer did not produce a complete MSI archive: $msi_path ($current_size bytes)" >&2
    return 4
  fi

  cp "$msi_path" "$cached_msi"
  kill "$installer_pid" >/dev/null 2>&1 || true
  wait "$installer_pid" >/dev/null 2>&1 || true

  msiextract -C "$extracted_root" "$cached_msi" >/dev/null

  if [[ ! -f "$extracted_binary" ]]; then
    echo "failed to unpack benchmark executable from MSI: $cached_msi" >&2
    return 5
  fi

  printf '%s\n' "$extracted_binary"
}

ensure_native_d3dx9_41() {
  local prefix="$1"
  local wine_bin="$2"
  local syswow64_dll="$prefix/drive_c/windows/syswow64/d3dx9_41.dll"

  if [[ -f "$syswow64_dll" ]]; then
    return 0
  fi
  if ! command -v winetricks >/dev/null 2>&1; then
    echo "winetricks is required to install native d3dx9_41 into $prefix" >&2
    return 6
  fi
  WINEPREFIX="$prefix" WINE="$wine_bin" winetricks -q d3dx9_41
}

prepare_app_local_d3dx9_41() {
  local prefix="$1"
  local extracted_binary="$2"
  local source_dll="$prefix/drive_c/windows/syswow64/d3dx9_41.dll"
  local target_dll
  target_dll="$(dirname -- "$extracted_binary")/d3dx9_41.dll"
  if [[ -f "$source_dll" ]]; then
    cp -f "$source_dll" "$target_dll"
  fi
}

args=("$@")
has_wine_root=false
has_prefix=false
has_binary=false
cache_root="$cache_root_default"
host="$host_default"
resolved_prefix=""
resolved_wine_root=""
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
  if [[ "${args[i]}" == "--cache-root" && $((i + 1)) -lt ${#args[@]} ]]; then
    cache_root="${args[i + 1]}"
  fi
  if [[ "${args[i]}" == "--host" && $((i + 1)) -lt ${#args[@]} ]]; then
    host="${args[i + 1]}"
  fi
done

if [[ "$host" == "heroic" ]]; then
  wine_root_default="$heroic_wine_root_default"
  prefix_default="$heroic_prefix_default"
  app_name="street-fighter-iv-benchmark"
elif [[ "$host" == "crossover" ]]; then
  wine_root_default="$crossover_root_default"
  prefix_default="$crossover_prefix_default"
  app_name="street-fighter-iv-benchmark-crossover-oracle"
else
  echo "unsupported --host value: $host" >&2
  exit 2
fi

if [[ "$has_wine_root" == true ]]; then
  for ((i=0; i<${#args[@]}; ++i)); do
    if [[ "${args[i]}" == "--wine-root" && $((i + 1)) -lt ${#args[@]} ]]; then
      resolved_wine_root="${args[i + 1]}"
      break
    fi
  done
else
  resolved_wine_root="$wine_root_default"
fi

if [[ "$has_prefix" == true ]]; then
  for ((i=0; i<${#args[@]}; ++i)); do
    if [[ "${args[i]}" == "--prefix" && $((i + 1)) -lt ${#args[@]} ]]; then
      resolved_prefix="${args[i + 1]}"
      break
    fi
  done
else
  resolved_prefix="$prefix_default"
fi

wine_bin_default="$wine_root_default/bin/wine"
wine_bin_resolved="$resolved_wine_root/bin/wine"

resolved_binary=""
if [[ "$has_binary" == false ]]; then
  if resolved_binary=$(discover_binary); then
    :
  else
    {
      echo "Street Fighter IV Benchmark binary not found."
      echo "Expected one of:"
      echo "  $binary_default"
      echo "  $binary_default_alt"
      echo "  $HOME/games/Street Fighter IV Benchmark/Benchmark.exe"
      echo "  $HOME/games/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe"
      echo "  $HOME/Games/Street Fighter IV Benchmark/Benchmark.exe"
      echo "  $HOME/Games/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe"
      echo "  $HOME/Applications/Street Fighter IV Benchmark/Benchmark.exe"
      echo "  $HOME/Applications/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe"
      echo "  $HOME/Downloads/Street Fighter IV Benchmark/Benchmark.exe"
      echo "  $HOME/Downloads/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe"
      echo
      echo "Pass the executable explicitly:"
      echo "  bash scripts/run_sfiv_benchmark_experiment.sh --binary \"/path/to/Benchmark.exe\""
    } >&2
    exit 2
  fi
fi

if [[ "$has_binary" == true ]]; then
  for ((i=0; i<${#args[@]}; ++i)); do
    if [[ "${args[i]}" == "--binary" && $((i + 1)) -lt ${#args[@]} ]]; then
      resolved_binary="${args[i + 1]}"
      break
    fi
  done
fi

if [[ -n "$resolved_binary" ]]; then
  resolved_binary=$(python3 - <<'PY' "$resolved_binary"
from pathlib import Path
import sys
print(Path(sys.argv[1]).expanduser().resolve())
PY
)
  if [[ "$(basename "$resolved_binary")" == "StreetFighterIV_Benchmark.exe" && "$resolved_binary" != */STREETFIGHTERIV_BENCHMARK/StreetFighterIV_Benchmark.exe ]]; then
    resolved_binary=$(extract_benchmark_from_installer "$resolved_binary" "$cache_root" "$wine_bin_resolved")
  fi
  if [[ "$host" == "heroic" ]]; then
    ensure_native_d3dx9_41 "$resolved_prefix" "$wine_bin_resolved"
    if [[ "$resolved_binary" == */STREETFIGHTERIV_BENCHMARK/StreetFighterIV_Benchmark.exe ]]; then
      prepare_app_local_d3dx9_41 "$resolved_prefix" "$resolved_binary"
    fi
  fi
fi

cmd=(python3 "$repo_root/scripts/run_experiment.py" run "$app_name")
if [[ "$has_wine_root" == false ]]; then
  cmd+=(--wine-root "$wine_root_default")
fi
if [[ "$has_prefix" == false ]]; then
  cmd+=(--prefix "$prefix_default")
fi
if [[ "$host" == "crossover" ]]; then
  cmd+=(--wine-bin "$wine_bin_default")
fi
if [[ -n "$resolved_binary" ]]; then
  cmd+=(--binary "$resolved_binary")
fi
for ((i=0; i<${#args[@]}; ++i)); do
  if [[ "${args[i]}" == "--binary" || "${args[i]}" == "--cache-root" || "${args[i]}" == "--host" ]]; then
    ((i+=1))
    continue
  fi
  cmd+=("${args[i]}")
done

"${cmd[@]}"
