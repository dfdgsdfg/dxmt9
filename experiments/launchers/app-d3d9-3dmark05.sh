#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$script_dir/common.sh"

default_3dmark05_args="-gt1 -nosplash -nosysteminfo -noscreens"

focus_app-d3d9-3dmark05() {
  osascript \
    -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark05" to true'
}

send_enter_app-d3d9-3dmark05() {
  osascript \
    -e 'tell application "System Events" to set frontmost of first process whose name contains "3DMark05" to true' \
    -e 'tell application "System Events" to key code 36'
}

schedule_app-d3d9-3dmark05_enter() {
  local default_focus_keepalive=${1:-120}
  (
    echo "[3dmark05-auto-enter] delay=${DXMT_3DMARK05_ENTER_DELAY_SEC:-20}s count=${DXMT_3DMARK05_ENTER_COUNT:-3} interval=${DXMT_3DMARK05_ENTER_INTERVAL_SEC:-2}s"
    sleep "${DXMT_3DMARK05_ENTER_DELAY_SEC:-20}"

    local enter_count=${DXMT_3DMARK05_ENTER_COUNT:-3}
    local enter_interval=${DXMT_3DMARK05_ENTER_INTERVAL_SEC:-2}
    if [[ ! "$enter_count" =~ ^[0-9]+$ ]]; then
      enter_count=3
    fi
    if [[ ! "$enter_interval" =~ ^[0-9]+$ ]]; then
      enter_interval=2
    fi

    local i=0
    while (( i < enter_count )); do
      echo "[3dmark05-auto-enter] enter attempt $((i + 1))/$enter_count"
      if ! send_enter_app-d3d9-3dmark05; then
        echo "[3dmark05-auto-enter] enter attempt failed"
      fi
      i=$((i + 1))
      if (( i < enter_count )); then
        sleep "$enter_interval"
      fi
    done

    local remaining=${DXMT_3DMARK05_FOCUS_KEEPALIVE_SEC:-$default_focus_keepalive}
    while [[ "$remaining" =~ ^[0-9]+$ ]] && (( remaining > 0 )); do
      sleep 1
      focus_app-d3d9-3dmark05 || true
      remaining=$((remaining - 1))
    done
  ) &
}

if [[ "${DXMT_3DMARK05_DIRECT:-0}" != "0" ]]; then
  prefix=${DXMT_3DMARK05_PREFIX:-"$exp_repo_root/experiments/prefixs/app-d3d9-3dmark05-verify"}
  wine_root=${DXMT_3DMARK05_WINE_ROOT:-"$exp_repo_root/experiments/wine/sikarugir-cx-24.0.7"}
  wine_bin=${DXMT_3DMARK05_WINE_BIN:-"$wine_root/bin/wine"}
  wine_server=${DXMT_3DMARK05_WINESERVER:-"$wine_root/bin/wineserver"}
  exe_dir="$prefix/drive_c/Program Files (x86)/Futuremark/3DMark05"
  exe="$exe_dir/3DMark05.exe"
  log_path=${DXMT_3DMARK05_LOG:-/tmp/3dmark05-direct.log}
  winemetal_so=${DXMT9_WINEMETAL_SO:-"$exp_repo_root/build-x86_64-builtin/src/winemetal/unix/winemetal.so"}

  if [[ ! -x "$wine_bin" ]]; then
    echo "missing wine binary: $wine_bin" >&2
    exit 2
  fi

  if [[ ! -f "$exe" ]]; then
    echo "missing 3DMark05 executable: $exe" >&2
    exit 2
  fi

  export DXMT_EXPERIMENT_PREFIX="$prefix"
  export DXMT_EXPERIMENT_WINE_ROOT="$wine_root"
  export DXMT_EXPERIMENT_PE_BUILD_DIR="${DXMT_3DMARK05_PE_BUILD_DIR:-${DXMT_EXPERIMENT_PE_BUILD_DIR:-$exp_repo_root/build-win32-x64-builtin/src/win32}}"
  export DXMT_EXPERIMENT_RUNTIME_PE_BUILD_DIR="${DXMT_3DMARK05_RUNTIME_PE_BUILD_DIR:-${DXMT_EXPERIMENT_RUNTIME_PE_BUILD_DIR:-$exp_repo_root/build-win32-x64-builtin/src/winemetal}}"
  export DXMT_EXPERIMENT_WOW64_PE_BUILD_DIR="${DXMT_3DMARK05_WOW64_PE_BUILD_DIR:-${DXMT_EXPERIMENT_WOW64_PE_BUILD_DIR:-$exp_repo_root/build-win32-x86-builtin/src/win32}}"
  export DXMT_EXPERIMENT_WOW64_RUNTIME_PE_BUILD_DIR="${DXMT_3DMARK05_WOW64_RUNTIME_PE_BUILD_DIR:-${DXMT_EXPERIMENT_WOW64_RUNTIME_PE_BUILD_DIR:-$exp_repo_root/build-win32-x86-builtin/src/winemetal}}"
  export DXMT_EXPERIMENT_UNIX_BUILD_DIR="${DXMT_3DMARK05_UNIX_BUILD_DIR:-${DXMT_EXPERIMENT_UNIX_BUILD_DIR:-$exp_repo_root/build-x86_64-builtin/src}}"

  if [[ "${DXMT_3DMARK05_STAGE:-1}" != "0" ]]; then
    exp_stage_dxmt9
  fi

  if [[ "${DXMT_3DMARK05_KILL_SERVER:-1}" != "0" ]]; then
    WINEPREFIX="$prefix" "$wine_server" -k >/dev/null 2>&1 || true
  fi

  mkdir -p "$(dirname -- "$log_path")"
  : > "$log_path"

  read -r -a dxmt_3dmark05_args <<< "${DXMT_3DMARK05_ARGS:-$default_3dmark05_args}"

  export DXMT_EXPERIMENT_WORKDIR
  DXMT_EXPERIMENT_WORKDIR="$exe_dir"

  echo "[3dmark05-direct] prefix=$prefix"
  echo "[3dmark05-direct] wine=$wine_bin"
  echo "[3dmark05-direct] log=$log_path"
  echo "[3dmark05-direct] args=${dxmt_3dmark05_args[*]}"

  if [[ "${DXMT_3DMARK05_AUTO_ENTER:-1}" != "0" ]]; then
    schedule_app-d3d9-3dmark05_enter 0
  fi

  cd "$DXMT_EXPERIMENT_WORKDIR"
  WINEPREFIX="$prefix" \
  WINEDLLOVERRIDES="${DXMT_3DMARK05_DLLOVERRIDES:-d3d9,winemetal=n,b;d3dx9_25,d3dx9_26,d3dx9_27,d3dx9_28,d3dcompiler_43,d3dcompiler_47=n,b}" \
  DYLD_LIBRARY_PATH="$wine_root/lib/wine/x86_64-unix${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
  DXMT_VALIDATE="${DXMT_VALIDATE:-1}" \
  DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK="${DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK:-1}" \
  DXMT9_WINEMETAL_SO="$winemetal_so" \
  DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-info}" \
  DXMT_LOG_PATH="${DXMT_LOG_PATH:-$(dirname -- "$log_path")}" \
  "$wine_bin" "$exe" "${dxmt_3dmark05_args[@]}" 2>&1 | tee -a "$log_path"
  exit "${PIPESTATUS[0]}"
fi

exp_stage_dxmt9

export DXMT_EXPERIMENT_WORKDIR
DXMT_EXPERIMENT_WORKDIR="$exp_repo_root/experiments/prefixs/app-d3d9-3dmark05/drive_c/Program Files (x86)/Futuremark/3DMark05"

if [[ "${DXMT_3DMARK05_AUTO_ENTER:-1}" != "0" ]]; then
  schedule_app-d3d9-3dmark05_enter 120
fi

read -r -a dxmt_3dmark05_args <<< "${DXMT_3DMARK05_ARGS:-$default_3dmark05_args}"
exp_run_wine_binary "$DXMT_EXPERIMENT_BINARY" "${dxmt_3dmark05_args[@]}"
