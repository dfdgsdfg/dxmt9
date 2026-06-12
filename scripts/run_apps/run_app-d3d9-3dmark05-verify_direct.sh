#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

timeout_sec=${DXMT_3DMARK05_DIRECT_TIMEOUT:-120}

if [[ ! "$timeout_sec" =~ ^[0-9]+([.][0-9]+)?$ ||
      "$timeout_sec" =~ ^0+([.]0+)?$ ]]; then
  echo "DXMT_3DMARK05_DIRECT_TIMEOUT must be positive numeric seconds" >&2
  exit 2
fi

export DXMT_3DMARK05_DIRECT=1
export DXMT_3DMARK05_DIRECT_TIMEOUT="$timeout_sec"
export DXMT_3DMARK05_SELF_SUPERVISED=1

launcher="$repo_root/experiments/launchers/app-d3d9-3dmark05.sh"
if [[ "${DXMT_3DMARK05_DIRECT_DRY_RUN:-0}" != "0" ]]; then
  echo "timeout: ${timeout_sec}s"
  printf 'cmd:'
  printf ' %q' "$launcher" "$@"
  printf '\n'
  exit 0
fi

python3 - "$repo_root" "$timeout_sec" "$@" <<'PY'
import os
import signal
import subprocess
import sys

repo_root = sys.argv[1]
timeout_sec = float(sys.argv[2])
launcher_args = sys.argv[3:]
launcher = os.path.join(repo_root, "experiments", "launchers", "app-d3d9-3dmark05.sh")
cmd = [launcher, *launcher_args]

def terminate_process_group(process, timeout):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=timeout)

print(f"[3dmark05-direct-wrapper] timeout={timeout_sec:g}s", flush=True)
process = subprocess.Popen(cmd, cwd=repo_root, env=os.environ.copy(), start_new_session=True)
try:
    returncode = process.wait(timeout=timeout_sec)
except subprocess.TimeoutExpired:
    print(
        f"[3dmark05-direct-wrapper] timeout after {timeout_sec:g}s; terminating process group",
        file=sys.stderr,
        flush=True,
    )
    terminate_process_group(process, 5)
    sys.exit(124)
except KeyboardInterrupt:
    print(
        "[3dmark05-direct-wrapper] interrupted; terminating process group",
        file=sys.stderr,
        flush=True,
    )
    terminate_process_group(process, 5)
    sys.exit(130)

sys.exit(returncode)
PY
