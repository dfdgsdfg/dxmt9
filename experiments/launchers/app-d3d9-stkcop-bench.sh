#!/usr/bin/env bash
set -euo pipefail

# S.T.A.L.K.E.R.: Call of Pripyat standalone benchmark (X-Ray 1.6, D3D9).
# Fully headless: the Benchmark.exe GUI is bypassed by invoking
# bin_test/xrEngine.exe directly through the GUI's OpenAutomate/native
# benchmark path. The engine writes <Scene>.result into
# $fs_root$/_appdata_/ (fsgame.ltx maps $app_data_root$ there); this launcher
# regenerates the benchmark config per lane/preset/renderer, runs one scene,
# and copies the .result and engine log into the experiment output dir.
#
# Env knobs:
#   DXMT_STKCOP_LANE      day | night | rain | sunshafts   (default day)
#   DXMT_STKCOP_PRESET    minimum|low|default|high|extreme|ultra (default high)
#   DXMT_STKCOP_RENDERER  engine renderer token (default renderer_r2.5; the
#                         engine clamps to what its HW probe accepts)
#   DXMT_STKCOP_VID_MODE  WxH (default 1280x720), windowed
#   DXMT_STKCOP_EXTRA_ARGS extra engine args appended verbatim
#   DXMT_STKCOP_SND_TARGETS optional 4..32 voice cap; unset preserves preset
#
# The engine needs native d3dx9 (its runtime-compiled SM1/SM3 shaders break
# Wine's vkd3d-shader HLSL frontend: E5017 "SM1 non-float expression"), so
# native d3dx9_24..43 + D3DCompiler_42 from redist/ are staged into the
# prefix's syswow64 (32-bit engine) when absent or Wine-builtin; the
# catalogue's wine_dll_overrides selects them.

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

app_root="$exp_repo_root/experiments/apps_3rd/app-d3d9-stkcop-bench/app"
redist_dir="$exp_repo_root/experiments/apps_3rd/app-d3d9-stkcop-bench/redist"

lane=${DXMT_STKCOP_LANE:-day}
preset=${DXMT_STKCOP_PRESET:-high}
renderer=${DXMT_STKCOP_RENDERER:-renderer_r2.5}
vid_mode=${DXMT_STKCOP_VID_MODE:-1280x720}
snd_targets=${DXMT_STKCOP_SND_TARGETS:-}
if [[ -n "$snd_targets" ]] && ! [[ "$snd_targets" =~ ^([4-9]|[12][0-9]|3[0-2])$ ]]; then
  echo "[stkcop] DXMT_STKCOP_SND_TARGETS must be an integer 4..32, or unset" >&2
  exit 2
fi

case "$lane" in
  day)       test_file="dayBenchmark.test" ;;
  night)     test_file="NightBenchmark.test" ;;
  rain)      test_file="RainBenchmark.test" ;;
  sunshafts) test_file="SunShaftsBenchmark.test" ;;
  *)
    echo "[stkcop] unknown DXMT_STKCOP_LANE '$lane'; expected day|night|rain|sunshafts" >&2
    exit 2
    ;;
esac

rspec="$app_root/BenchCfg/rspec_${preset}.ltx"
if [[ ! -f "$rspec" ]]; then
  echo "[stkcop] unknown DXMT_STKCOP_PRESET '$preset' (missing $rspec)" >&2
  exit 2
fi

stage_stkcop_redist() {
  exp_require_var DXMT_EXPERIMENT_PREFIX
  local syswow64="$DXMT_EXPERIMENT_PREFIX/drive_c/windows/syswow64"
  if [[ ! -d "$syswow64" ]]; then
    echo "[stkcop] prefix syswow64 missing: $syswow64" >&2
    exit 2
  fi
  local staged=0
  local dll base
  for dll in "$redist_dir"/*.dll; do
    base=$(basename -- "$dll")
    if [[ ! -f "$syswow64/$base" ]] ||
       head -c 200 "$syswow64/$base" | grep -aq "Wine builtin DLL"; then
      cp "$dll" "$syswow64/$base"
      staged=$((staged + 1))
    fi
  done
  echo "[stkcop] native redist staged: $staged updated (of $(ls "$redist_dir"/*.dll | wc -l | tr -d ' '))"
}

generate_stkcop_config() {
  local appdata="$app_root/_appdata_"
  mkdir -p "$appdata"
  cp "$exp_repo_root/experiments/apps_3rd/app-d3d9-stkcop-bench/commondocs/CallOfPripyatBench/_appdata_/"*.test \
    "$appdata/" 2>/dev/null || true
  {
    cat "$app_root/BenchCfg/base.cfg"
    echo
    cat "$rspec"
    printf 'renderer %s\nrs_fullscreen off\nvid_mode %s\n' "$renderer" "$vid_mode"
    if [[ -n "$snd_targets" ]]; then printf 'snd_targets %s\n' "$snd_targets"; fi
  } > "$appdata/test.ltx"
  # The engine reads $app_data_root$/user.ltx earlier than the -ltx config;
  # keep both in sync so the renderer request is visible at every read point.
  cp "$appdata/test.ltx" "$appdata/user.ltx"
  if [[ -n "${DXMT_EXPERIMENT_OUTPUT_DIR:-}" ]]; then
    mkdir -p "$DXMT_EXPERIMENT_OUTPUT_DIR"
    cp "$appdata/test.ltx" "$DXMT_EXPERIMENT_OUTPUT_DIR/stkcop-requested.ltx"
    printf 'snd_targets_override=%s\nextra_args=%s\nqualification=%s\n' \
      "${snd_targets:-unset}" "${DXMT_STKCOP_EXTRA_ARGS:-}" \
      'requested config only; verify engine/source evidence; audio cap changes quality' \
      > "$DXMT_EXPERIMENT_OUTPUT_DIR/stkcop-controls.txt"
  fi
  echo "[stkcop] config: lane=$lane preset=$preset renderer=$renderer vid_mode=$vid_mode"
}

collect_stkcop_results() {
  local appdata="$app_root/_appdata_"
  local out_dir="${DXMT_EXPERIMENT_OUTPUT_DIR:-}"
  if [[ -z "$out_dir" ]]; then
    return 0
  fi
  mkdir -p "$out_dir"
  if [[ -f "$appdata/user.ltx" ]]; then
    cp "$appdata/user.ltx" "$out_dir/stkcop-after-run.ltx"
  fi
  local copied=0
  local f
  for f in "$appdata"/*.result; do
    [[ -f "$f" ]] || continue
    cp "$f" "$out_dir/"
    copied=$((copied + 1))
    echo "[stkcop] result: $(basename -- "$f")"
    cat "$f" | sed 's/^/[stkcop-result] /'
  done
  if [[ -f "$appdata/logs/xray_crossover.log" ]]; then
    cp "$appdata/logs/xray_crossover.log" "$out_dir/xray_engine.log"
  fi
  if (( copied == 0 )); then
    echo "[stkcop] no .result produced (run killed early, crash, or benchmark did not finish)" >&2
  fi
}

# Config files belong to the shared app tree, not to a Wine prefix. Serialize
# every lane and restore exact prior bytes, including prior absence.
config_lock="$app_root/.dxmt9-experiment-config-lock"
if ! mkdir "$config_lock"; then
  echo "[stkcop] shared app is locked; do not remove the lock until its run has stopped" >&2
  exit 2
fi
config_ready=0
finish_stkcop() {
  local status=$?
  trap - EXIT
  if (( config_ready )); then
    collect_stkcop_results || status=1
    local name
    for name in test.ltx user.ltx; do
      if [[ -f "$config_lock/$name" ]]; then
        if cp "$config_lock/$name" "$app_root/_appdata_/$name"; then
          rm -f "$config_lock/$name"
        else
          status=1 # Preserve the backup and lock if restoration fails.
        fi
      else
        rm -f "$app_root/_appdata_/$name"
      fi
    done
  fi
  rmdir "$config_lock" || status=1
  exit "$status"
}
trap finish_stkcop EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$app_root/_appdata_"
for config_name in test.ltx user.ltx; do
  if [[ -f "$app_root/_appdata_/$config_name" ]]; then
    cp "$app_root/_appdata_/$config_name" "$config_lock/$config_name"
  fi
done
config_ready=1

exp_stage_dxmt9
stage_stkcop_redist
generate_stkcop_config
rm -f "$app_root/_appdata_"/*.result

export DXMT_EXPERIMENT_WORKDIR="$app_root"

read -r -a extra_args <<< "${DXMT_STKCOP_EXTRA_ARGS:-}"
echo "[stkcop] running lane=$lane via $test_file"
exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" \
  -ltx test.ltx -silent_error_mode -openautomate "$test_file" \
  ${extra_args[@]+"${extra_args[@]}"}

if ! compgen -G "$app_root/_appdata_/*.result" >/dev/null; then
  echo "[stkcop] engine exited successfully without producing a benchmark result" >&2
  exit 1
fi
