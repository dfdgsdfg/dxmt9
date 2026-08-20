#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tla_dir="$repo_root/specs/verification/tla"

jar_dir=""
if command -v tlc >/dev/null 2>&1; then
  tlc_cmd=(tlc)
else
  if [[ -n "${TLA2TOOLS_JAR:-}" && -f "${TLA2TOOLS_JAR}" ]]; then
    jar_path="$TLA2TOOLS_JAR"
  else
    jar_dir="$(mktemp -d)"
    jar_path="$jar_dir/tla2tools.jar"
    curl -fsSL -o "$jar_path" \
      https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar
  fi
  tlc_cmd=(java -cp "$jar_path" tlc2.TLC)
fi

cleanup() {
  if [[ -n "${metadir:-}" && -d "${metadir:-}" ]]; then
    rm -rf "$metadir"
  fi
  if [[ -n "${jar_dir:-}" && -d "$jar_dir" ]]; then
    rm -rf "$jar_dir"
  fi
}
trap cleanup EXIT

for spec in "$tla_dir"/*.tla; do
  cfg="${spec%.tla}.cfg"
  metadir="$(mktemp -d)"
  echo "=== $(basename "$spec") ==="
  "${tlc_cmd[@]}" -workers auto -metadir "$metadir" -config "$cfg" "$spec"
  rm -rf "$metadir"
  unset metadir
done

# Some models carry a deliberately broken companion configuration. Keep those
# counterexamples executable: a change that makes a buggy configuration green
# means the model no longer distinguishes the regression it exists to guard,
# while a production configuration failure is still handled by the loop above.
#
# Rows are "<model>|<cfg suffix>|<expected TLC message>". The model name
# selects `<model>.tla`; the suffix selects `<model><suffix>.cfg`, so one model
# may carry several independent broken premises.
counterexample_models=(
  # Full-shadow upload clobbers the in-flight NOOVERWRITE read range.
  "NoOverwriteByteRange|.counterexample|Invariant NoOverwriteReadPreserved is violated"
  # Pin-ordering premise removed: a reclaim frees a record the producer's
  # commit window still names. See
  # docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md §4.
  "ProducerMarkReclaim|.counterexample|Invariant NoUseAfterFree is violated"
  # Re-stamp protocol removed: a concurrent force-publish moves the seq the
  # replay worker's records land under after its lock-free ticket read, so the
  # stamps sit below the chunk's final seq and the watermark passes them while
  # that chunk is still pending. Design doc §9 step 1 (T2a').
  "ProducerMarkReclaim|.restamp.counterexample|Invariant NoUseAfterFree is violated"
)

for row in "${counterexample_models[@]}"; do
  model="${row%%|*}"
  rest="${row#*|}"
  cfg_suffix="${rest%%|*}"
  expected="${rest#*|}"
  counterexample_cfg="$tla_dir/$model$cfg_suffix.cfg"
  counterexample_spec="$tla_dir/$model.tla"
  [[ -f "$counterexample_cfg" && -f "$counterexample_spec" ]] || continue

  metadir="$(mktemp -d)"
  counterexample_log="$metadir/tlc.log"
  set +e
  "${tlc_cmd[@]}" -workers auto -metadir "$metadir" \
    -config "$counterexample_cfg" "$counterexample_spec" >"$counterexample_log" 2>&1
  status=$?
  set -e
  if [[ "$status" -eq 0 ]] || ! grep -q "$expected" "$counterexample_log"; then
    cat "$counterexample_log"
    echo "expected $model$cfg_suffix counterexample was not observed" >&2
    exit 1
  fi
  echo "=== $model$cfg_suffix.cfg (expected failure) ==="
  echo "expected invariant counterexample observed: $expected"
  rm -rf "$metadir"
  unset metadir
done
