#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tla_dir="$repo_root/specs/verification/tla"
tlc_workers="${DXMT9_TLC_WORKERS:-auto}"

# The PE recorder TLA module is generated from the same bounded decision table
# included by the production C++ algebra.  A stale generated module is a
# model/code binding failure, not a harmless documentation diff.
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_transition_table.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_commit_transition_table.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_stateblock_transition_table.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_composed_tables.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_composed_tables.py" --verify-generation
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/check_pe_scalar_projection_model.py"

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
  # Imported/generated helper modules have no standalone behavior to check.
  # Their owning model imports them after the freshness check above.
  [[ -f "$cfg" ]] || continue
  metadir="$(mktemp -d)"
  echo "=== $(basename "$spec") ==="
  "${tlc_cmd[@]}" -workers "$tlc_workers" -metadir "$metadir" -config "$cfg" "$spec"
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
  # Same pin premise removed as the row above, but reported through the
  # capture-side invariant: `captureChunkBufferBinding` publishing a snapshot of
  # a record `gcArena` already released. This is what licenses T2b (capture off
  # `CommandQueue::mutex_`); design doc §8 T2b.
  "ProducerMarkReclaim|.capture.counterexample|Invariant NoCaptureAfterFree is violated"
  # Managed mutation offload, premise 1: mutation tasks on a SECOND queue,
  # ordered among themselves but not against chunk replay. A later chunk is
  # then replayed and encoded while the backing it captured still holds
  # pre-mutation bytes. See specs/backend/buffer-mutation-offload/spec.md §4
  # ("no second queue") and R-BACK-44.3.
  "BufferMutationOffload|.counterexample|Invariant EncodeReadsAppliedBytes is violated"
  # Same model, premise 2: the LOGICAL rename-ring rotation deferred to the
  # worker instead of running between reserve and commit. A chunk committed
  # after the unlock returned captures the pre-rotation backing/revision, so
  # its draws render pre-unlock content self-consistently — invisible to the
  # visibility invariant above. R-BACK-44.2 step 2.
  "BufferMutationOffload|.rotation.counterexample|Invariant SnapshotRevisionIsCurrent is violated"
  # Consuming a projection during Prepare loses it when append subsequently
  # fails; production consumes only the accepted represented set.
  "PeRecorderTransition|.counterexample|Invariant NoLostPending is violated"
  # A failed append must not publish a consumption witness. This independently
  # guards OnlyAcceptedConsumes rather than relying on exact Accepted payloads.
  "PeRecorderTransition|.consume-witness.counterexample|Invariant OnlyAcceptedConsumes is violated"
  # Accepted must witness every and only prepared qualified token. This is
  # distinct from forbidding witnesses on non-Accepted transitions.
  "PeRecorderTransition|.accepted-witness.counterexample|Invariant AcceptedExactlyRepresented is violated"
  # A live-phase prior-value operation produces a new ordinary state result;
  # preserving the older pending value would replay stale state.
  "PeRecorderTransition|.prior-pending.counterexample|Invariant PendingMatchesLive is violated"
  # Dropping the value from a durable key/token lets the model accept a stale
  # payload even though the key and ordinal still look valid.
  "PeRecorderTransition|.stale.counterexample|Invariant DurableTokenMatchesPayload is violated"
  # Scalar semantic projection must preserve exact value/source and bind one
  # accepted record ordinal; each mutation is independent evidence.
  "PeRecorderScalarProjection|.no-token.counterexample|Invariant NoTokenIsExplicit is violated"
  "PeRecorderScalarProjection|.missing.counterexample|Invariant ExactProjection is violated"
  "PeRecorderScalarProjection|.duplicate.counterexample|Invariant ExactProjection is violated"
  "PeRecorderScalarProjection|.value.counterexample|Invariant ExactProjection is violated"
  "PeRecorderScalarProjection|.source-ordinal.counterexample|Invariant ExactProjection is violated"
  "PeRecorderScalarProjection|.record-ordinal.counterexample|Invariant ExactProjection is violated"
  "PeRecorderScalarProjection|.category.counterexample|Invariant ExactProjection is violated"
  "PeRecorderScalarProjection|.key.counterexample|Invariant ExactProjection is violated"
  # Parent destruction before alias retirement breaks the ordering contract.
  "PeRecorderCommit|.parent-before-alias.counterexample|Invariant AliasBeforeParent is violated"
  # Builder reset while pending references remain breaks no-early-drain/reset.
  "PeRecorderCommit|.early-reset.counterexample|Invariant NoEarlyDrainReset is violated"
  # The old success cycle left commandAccepted set in WarmAdvanced and could
  # never prove return to a reusable Unsealed builder.
  "PeRecorderCommit|.stuck-success.counterexample|Temporal properties were violated"
  # An entered bridge failure has unknown unix effect under the unchanged ABI
  # and must poison instead of retrying the sealed projection.
  "PeRecorderSettlement|.bridge-retry.counterexample|Invariant BridgeEffectUnknownFailStop is violated"
  # CapacityPre failure leaves the proposed record unattempted and cannot
  # consume its qualified pending token.
  "PeRecorderSettlement|.capacity-pre-consume.counterexample|Invariant CapacityPreDoesNotConsume is violated"
  # Capture settlement follows command acceptance and cannot retract it.
  "PeRecorderSettlement|.capture-retract.counterexample|Invariant CaptureAfterAccept is violated"
  # Builder reset cannot precede pending/alias/parent drain.
  "PeRecorderSettlement|.early-reset.counterexample|Invariant NoEarlyDrainReset is violated"
  # StateBlock Apply backend failure must poison before Reset recovery.
  "PeStateBlockTransaction|.no-poison.counterexample|Invariant NoStaleOpenAfterPostEffectFailure is violated"
  # Every poisoned Apply path releases staged references before recovery.
  "PeStateBlockTransaction|.no-release.counterexample|Invariant FailedRefsReleased is violated"
  # Leaving the recording candidate open after backend End consumption is a
  # stale/open serial-domain regression distinct from generic no-poison Apply.
  "PeStateBlockTransaction|.stale-open.counterexample|Invariant NoStaleOpenAfterPostEffectFailure is violated"
  # Treating retained COM identities as a set loses one AddRef/Release when the
  # same object occupies multiple StateBlock categories or slots.
  "PeStateBlockTransaction|.lost-duplicate.counterexample|Invariant PreparedRefMultiplicity is violated"
  # A generic fail-stop transition must clean both candidate and staged
  # ownership before publishing the Poisoned phase.
  "PeStateBlockTransaction|.poison-leak.counterexample|Invariant PoisonOwnsNoCandidateOrRefs is violated"
  # A capability retained across End/Reset must not write into the next
  # monotonic recording epoch (the concrete ABA witness is also native).
  "PeStateBlockTransaction|.stale-capability.counterexample|Invariant NoStaleCapabilityWrite is violated"
  # Capture refreshes the original frozen category-qualified key set only.
  "PeStateBlockValues|.mutable-tracked.counterexample|Invariant FrozenTrackedSet is violated"
  # A failed Capture may not publish its candidate values.
  "PeStateBlockValues|.failed-capture.counterexample|Invariant FailedCapturePreservesSnapshot is violated"
  # Apply must publish the latest successful captured ordinal and value.
  "PeStateBlockValues|.stale-apply.counterexample|Invariant LatestCapturedApplied is violated"
  # Retained Initializer ownership removed: arena reclamation deallocates the
  # destination while the pending upload still names it.
  "ResourceLifetime|.counterexample|Invariant NoUseAfterFree is violated"
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
  "${tlc_cmd[@]}" -workers "$tlc_workers" -metadir "$metadir" \
    -config "$counterexample_cfg" "$counterexample_spec" >"$counterexample_log" 2>&1
  status=$?
  set -e
  if [[ "$status" -eq 0 ]] || ! grep -q "$expected" "$counterexample_log"; then
    cat "$counterexample_log"
    echo "expected $model$cfg_suffix counterexample was not observed" >&2
    exit 1
  fi
  if [[ "$model" == "ResourceLifetime" ]] &&
     { ! grep -Fq "<StageInitializerUpload " "$counterexample_log" ||
       ! grep -Fq "<DestroyResource " "$counterexample_log" ||
       ! grep -Fq "<FreeResource " "$counterexample_log"; }; then
    cat "$counterexample_log"
    echo "expected ResourceLifetime historical trace was not observed" >&2
    exit 1
  fi
  echo "=== $model$cfg_suffix.cfg (expected failure) ==="
  echo "expected invariant counterexample observed: $expected"
  rm -rf "$metadir"
  unset metadir
done
