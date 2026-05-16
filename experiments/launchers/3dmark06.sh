#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

exp_stage_dxmt9

export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR="$exp_repo_root/experiments/prefixs/3dmark06/drive_c/Program Files (x86)/Futuremark/3DMark06"

## 3DMark06 compiles some legacy SM1-era HLSL at runtime. Wine/vkd3d-shader's
## builtin D3DX path currently fails that shader, so prefer the native D3DX9
## compiler lane by default while still allowing callers to override it.
export DXMT_EXPERIMENT_WINE_DLLOVERRIDES
DXMT_EXPERIMENT_WINE_DLLOVERRIDES="${DXMT_EXPERIMENT_WINE_DLLOVERRIDES:-d3d9,winemetal=n,b;d3dx9_28,d3dcompiler_43,d3dcompiler_47=n,b}"
export WINEDLLOVERRIDES
WINEDLLOVERRIDES="$DXMT_EXPERIMENT_WINE_DLLOVERRIDES"
exp_log "3DMark06 DLL overrides: $DXMT_EXPERIMENT_WINE_DLLOVERRIDES"

exp_3dmark06_dll_needs_native_install() {
  local dll=$1
  if [[ ! -f "$dll" ]]; then
    return 0
  fi

  if objdump -p "$dll" 2>/dev/null | grep -Eiq 'wined3d|vkd3d'; then
    return 0
  fi

  return 1
}

exp_3dmark06_ensure_native_d3dx() {
  if [[ "${DXMT_3DMARK06_ENSURE_NATIVE_D3DX:-1}" == "0" ]]; then
    exp_log "skipping native D3DX verification"
    return 0
  fi

  if [[ "$DXMT_EXPERIMENT_WINE_DLLOVERRIDES" != *d3dx9_28* ]]; then
    return 0
  fi

  exp_require_var DXMT_EXPERIMENT_PREFIX

  local syswow64="$DXMT_EXPERIMENT_PREFIX/drive_c/windows/syswow64"
  local needs_install=0
  local dll
  for dll in d3dx9_28.dll d3dcompiler_43.dll d3dcompiler_47.dll; do
    if [[ -f "$syswow64/$dll" ]] && ! command -v objdump >/dev/null 2>&1; then
      exp_log "objdump is required to verify native D3DX DLL provenance"
      exit 2
    fi
    if exp_3dmark06_dll_needs_native_install "$syswow64/$dll"; then
      needs_install=1
      break
    fi
  done

  if [[ "$needs_install" == "0" ]]; then
    exp_log "3DMark06 native D3DX/D3DCompiler DLLs verified"
    return 0
  fi

  if ! command -v winetricks >/dev/null 2>&1; then
    exp_log "winetricks is required to install native d3dx9_28/d3dcompiler DLLs"
    exit 2
  fi

  local wine_bin=${DXMT_EXPERIMENT_WINE_BIN:-wine}
  local wineserver_bin=${WINESERVER:-wineserver}
  if [[ -n "${DXMT_EXPERIMENT_WINE_ROOT:-}" ]]; then
    if [[ -x "$DXMT_EXPERIMENT_WINE_ROOT/bin/wine.real" ]]; then
      wine_bin="$DXMT_EXPERIMENT_WINE_ROOT/bin/wine.real"
    fi
    if [[ -x "$DXMT_EXPERIMENT_WINE_ROOT/bin/wineserver.real" ]]; then
      wineserver_bin="$DXMT_EXPERIMENT_WINE_ROOT/bin/wineserver.real"
    fi
  fi

  exp_log "installing native d3dx9_28/d3dcompiler DLLs for 3DMark06"
  WINE="$wine_bin" \
  WINESERVER="$wineserver_bin" \
  WINEPREFIX="$DXMT_EXPERIMENT_PREFIX" \
  WINETRICKS_FORCE=1 \
    winetricks -q d3dx9_28 d3dcompiler_43 d3dcompiler_47
}

exp_3dmark06_ensure_native_d3dx

exp_3dmark06_terminate_processes() {
  local pids
  pids=$(pgrep -f '[/]3DMark06\.exe.*Futuremark.*3DMark06' || true)
  if [[ -z "$pids" ]]; then
    exp_log "3DMark06 watchdog found no process to terminate"
    return 0
  fi

  exp_log "3DMark06 watchdog terminating process(es): ${pids//$'\n'/ }"
  kill $pids 2>/dev/null || true
  sleep 5

  local live
  live=$(pgrep -f '[/]3DMark06\.exe.*Futuremark.*3DMark06' || true)
  if [[ -n "$live" ]]; then
    exp_log "3DMark06 watchdog force-killing process(es): ${live//$'\n'/ }"
    kill -KILL $live 2>/dev/null || true
  fi
}

watchdog_pid=""
cleanup_watchdog() {
  if [[ -n "$watchdog_pid" ]]; then
    kill "$watchdog_pid" 2>/dev/null || true
  fi
}
trap cleanup_watchdog EXIT

## The 3DMark06 skinned UI accepts Enter as "Run 3DMark" after it settles.
## Let the benchmark run briefly after that, then terminate the app so this
## exploratory target does not wait on the full benchmark/result workflow.
if [[ "${DXMT_3DMARK06_AUTO_CONTROL:-1}" != "0" ]]; then
  (
    sleep "${DXMT_3DMARK06_ENTER_DELAY_SEC:-20}"
    osascript \
      -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark06" to true' \
      -e 'tell application "System Events" to key code 36' || true
    sleep "${DXMT_3DMARK06_RUN_AFTER_ENTER_SEC:-30}"
    if [[ "${DXMT_3DMARK06_AUTO_TERMINATE:-1}" != "0" ]]; then
      exp_3dmark06_terminate_processes
    fi
  ) &
  watchdog_pid=$!
fi

exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" \
  -gtall -batchall -featureall -nosplash -nosysteminfo -noscreens
