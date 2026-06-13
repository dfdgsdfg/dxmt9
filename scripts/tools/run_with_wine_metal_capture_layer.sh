#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: run_with_wine_metal_capture_layer.sh --wine-root PATH [options] -- COMMAND [ARGS...]

Temporarily replaces PATH/bin/wine.real and PATH/bin/wine-preloader with
capture-enabled copies, runs COMMAND, then restores the originals even if the
command fails.

Options:
  --wine-root PATH              Wine runtime root containing bin/wine.real.
  --capture-real PATH           Capture-enabled wine.real copy.
                                Default: PATH/bin/wine.capture.real
  --capture-preloader PATH      Capture-enabled wine-preloader copy.
                                Default: PATH/bin/wine.capture.real-preloader
  --backup-dir PATH             Backup directory. Default: mktemp under /tmp.
  --allow-3dmark05              Allow a deliberate 3DMark05 capture-layer
                                diagnostic despite the known black-screen path.
  -h, --help                    Show this help.

The capture copies must already contain MetalCaptureEnabled in their embedded
Info.plist. This script does not set MTL_CAPTURE_ENABLED; for 3DMark05 that env
has reproduced black-screen startup.
EOF
}

fail() {
  printf 'run_with_wine_metal_capture_layer: %s\n' "$*" >&2
  exit 2
}

wine_root=
capture_real=
capture_preloader=
backup_dir=
allow_3dmark05=${DXMT9_ALLOW_3DMARK05_CAPTURE_LAYER:-0}

while (($#)); do
  case "$1" in
    --wine-root)
      (($# >= 2)) || fail "--wine-root requires a path"
      wine_root=$2
      shift 2
      ;;
    --capture-real)
      (($# >= 2)) || fail "--capture-real requires a path"
      capture_real=$2
      shift 2
      ;;
    --capture-preloader)
      (($# >= 2)) || fail "--capture-preloader requires a path"
      capture_preloader=$2
      shift 2
      ;;
    --backup-dir)
      (($# >= 2)) || fail "--backup-dir requires a path"
      backup_dir=$2
      shift 2
      ;;
    --allow-3dmark05)
      allow_3dmark05=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

[[ -n "$wine_root" ]] || fail "--wine-root is required"
(($# > 0)) || fail "missing command after --"

wine_root=$(cd "$wine_root" && pwd)
wine_bin_dir="$wine_root/bin"
target_real="$wine_bin_dir/wine.real"
target_preloader="$wine_bin_dir/wine-preloader"
capture_real=${capture_real:-"$wine_bin_dir/wine.capture.real"}
capture_preloader=${capture_preloader:-"$wine_bin_dir/wine.capture.real-preloader"}

if (( ! allow_3dmark05 )); then
  command_text=$(printf '%s\n' "$@")
  if printf '%s\n' "$command_text" | grep -Eiq '(^|[^[:alnum:]])(3dmark05|app-d3d9-3dmark05)([^[:alnum:]]|$)'; then
    fail "refusing 3DMark05 capture-layer run: this path black-screens before draw/present and writes no .gputrace; use no-gputrace/xctrace sidecars or pass --allow-3dmark05 for a deliberate diagnostic"
  fi
fi

[[ -f "$target_real" ]] || fail "missing target: $target_real"
[[ -f "$target_preloader" ]] || fail "missing target: $target_preloader"
[[ -f "$capture_real" ]] || fail "missing capture copy: $capture_real"
[[ -f "$capture_preloader" ]] || fail "missing capture copy: $capture_preloader"

if ! strings "$capture_real" | grep -q 'MetalCaptureEnabled'; then
  fail "capture copy lacks MetalCaptureEnabled: $capture_real"
fi
if ! strings "$capture_preloader" | grep -q 'MetalCaptureEnabled'; then
  fail "capture copy lacks MetalCaptureEnabled: $capture_preloader"
fi

if [[ -z "$backup_dir" ]]; then
  backup_dir=$(mktemp -d "${TMPDIR:-/tmp}/dxmt9-wine-capture-layer.XXXXXX")
else
  mkdir -p "$backup_dir"
  backup_dir=$(cd "$backup_dir" && pwd)
fi

backup_real="$backup_dir/wine.real"
backup_preloader="$backup_dir/wine-preloader"
cp -p "$target_real" "$backup_real"
cp -p "$target_preloader" "$backup_preloader"

restored=0
restore() {
  if ((restored)); then
    return
  fi
  cp -p "$backup_real" "$target_real"
  cp -p "$backup_preloader" "$target_preloader"
  restored=1
}

trap 'status=$?; restore; exit "$status"' EXIT INT TERM

cp -p "$capture_real" "$target_real"
cp -p "$capture_preloader" "$target_preloader"

printf 'run_with_wine_metal_capture_layer: patched %s\n' "$wine_bin_dir" >&2
printf 'run_with_wine_metal_capture_layer: backup %s\n' "$backup_dir" >&2

run_status=0
"$@" || run_status=$?

restore
trap - EXIT INT TERM

python3 - "$target_real" "$backup_real" "$target_preloader" "$backup_preloader" <<'PY' || \
  fail "restore verification failed"
import pathlib
import sys

pairs = [
    (pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])),
    (pathlib.Path(sys.argv[3]), pathlib.Path(sys.argv[4])),
]
for restored, backup in pairs:
    if restored.read_bytes() != backup.read_bytes():
        print(f"{restored} differs from {backup}", file=sys.stderr)
        sys.exit(1)
PY

printf 'run_with_wine_metal_capture_layer: restored %s\n' "$wine_bin_dir" >&2
exit "$run_status"
