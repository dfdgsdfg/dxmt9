#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tla_dir="$repo_root/specs/verification/tla"
tlc_workers="${DXMT9_TLC_WORKERS:-auto}"

# Keep the standalone safety inventory explicit.  TLA helper modules are
# intentionally absent from this list: they are imported by an owning model
# and must not acquire a standalone configuration by filename convention.
normal_models=(
  AutogenMipGeneration
  BufferBackingVersioning
  BufferMutationOffload
  CommandQueue
  ConcurrentProgressSignals
  CpuPipelineLifecycle
  CpuPipelineOwnership
  CpuReadyActiveHeadLookahead
  CpuReadySemanticTransfer
  CpuReadySessionProgress
  DceChunkLookahead
  DirectChunkSlotContinuation
  DirectSlotAggregateCapacityLease
  DirectSlotCapacityProvisioning
  DrawPsoIdentity
  DrawableToken
  EncodeSchedulingProgress
  EncodeSessionCompletion
  EncoderLifecycle
  MutationComposition
  NoOverwriteByteRange
  ParallelDrawBinding
  ParallelPolicySelection
  PeRecorderCommit
  PeRecorderScalarProjection
  PeRecorderSemanticProjection
  PeRecorderSettlement
  PeRecorderTransition
  PeStateBlockTransaction
  PeStateBlockValues
  PostEncodePayloadRetirement
  PresentFrameLatency
  PresentIdAba
  ProducerMarkReclaim
  PsoSlotPublication
  QuerySeqId
  QueueLifecycleRefinement
  QueueT2dReserveCopyCommit
  RenderTapeIdentitySegments
  RenderTapeParallelJoin
  SegmentedTransportV1
  ReplayEmissionPlanIslands
  ReplayScopedDrain
  ReplayProjectionTransaction
  ResourceLifetime
  SessionCapacityLease
  StateBlockOrderedReplay
  WireObjectRegistry
  WsiPresenterReplacement
)

expected_normal_count=49
expected_progress_count=1
expected_counterexample_count=92
expected_cfg_count=142
progress_model=CpuPipelineLifecycle
progress_cfg_name="$progress_model.progress.cfg"

# Generated TLA vocabulary modules are checked against their production enum
# sources.  A stale module is a vocabulary drift, not a harmless documentation
# diff; predicate semantics remain explicitly owned by each model/native case.
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_transition_table.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_commit_transition_table.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_stateblock_transition_table.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_semantic_producer_table.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_composed_tables.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pe_composed_tables.py" --verify-generation
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/check_pe_scalar_projection_model.py"
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_mutation_composition_table.py" --check
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/check/gen_pipeline_lifecycle_table.py" --check

jar_dir=""
inventory_dir=""
cleanup() {
  if [[ -n "${metadir:-}" && -d "${metadir:-}" ]]; then
    rm -rf "$metadir"
  fi
  if [[ -n "${inventory_dir:-}" && -d "$inventory_dir" ]]; then
    rm -rf "$inventory_dir"
  fi
  if [[ -n "${jar_dir:-}" && -d "$jar_dir" ]]; then
    rm -rf "$jar_dir"
  fi
}
trap cleanup EXIT

# Some models carry a deliberately broken companion configuration. Keep those
# counterexamples executable: a change that makes a buggy configuration green
# means the model no longer distinguishes the regression it exists to guard,
# while a production configuration failure is still handled by the safety loop
# below.
#
# Rows are "<model>|<cfg suffix>|<expected TLC message>". The model name
# selects `<model>.tla`; the suffix selects `<model><suffix>.cfg`, so one model
# may carry several independent broken premises.
counterexample_models=(
  # Lease-span replay of one raw. Each row deletes exactly one of the three
  # disciplines a divisible raw needs and that an indivisible one never did:
  # the active-raw span witness, separation of its raw-local interval from the
  # populated-slot aggregate, the post-separator fail-stop cut, and the
  # explicit draw-run closure at a cut.
  # Empty-slot storage provisioning. Removing provisioning restores exact-fit
  # reservation, which is the measured HEAD regression: every adjacent source
  # rotates and Direct publishes more often than a same-capacity serial
  # reference. Removing the empty-only discipline buys capacity by
  # reallocating an already published extent.
  "DirectSlotCapacityProvisioning|.exact-fit.counterexample|Invariant BoundaryCreditsNotExceeded is violated"
  "DirectSlotCapacityProvisioning|.grow-populated.counterexample|Invariant NoGrowWhilePopulated is violated"
  "DirectSlotAggregateCapacityLease|.partial-adoption.counterexample|Invariant AdoptionIsAtomic is violated"
  "DirectSlotAggregateCapacityLease|.leaked-credit.counterexample|Invariant RetainedCreditConserved is violated"
  "ReplayEmissionPlanIslands|.span-identity.counterexample|Invariant EachRecordEmittedOnce is violated"
  "ReplayEmissionPlanIslands|.raw-local-witness.counterexample|Invariant RawLocalWitnessSeparated is violated"
  "ReplayEmissionPlanIslands|.separator-cut.counterexample|Invariant NoLegacyRetryAfterSeparator is violated"
  "ReplayEmissionPlanIslands|.run-closure.counterexample|Invariant RunClosedAcrossSeparator is violated"
  # Queue T2d is deferred, but its proposed reserve/copy/commit protocol is
  # modelled before any production lock narrowing.  Publication of a
  # half-constructed slot, reuse of a frozen reservation after generation
  # advance, and lost-prefix rollback are independent expected failures.
  "QueueT2dReserveCopyCommit|.half-appended-slot.counterexample|Invariant NoHalfAppendedSlot is violated"
  "QueueT2dReserveCopyCommit|.stale-reservation.counterexample|Invariant NoStaleReservationWrite is violated"
  "QueueT2dReserveCopyCommit|.lost-prefix-rollback.counterexample|Invariant RollbackRestoresPrefix is violated"
  # The historical R15 shadow planner dropped the active render seed after
  # retaining and restoring a Ready head, so it could never prove A|B|A.
  "CpuReadyActiveHeadLookahead|.seedless.counterexample|Invariant ActiveSeedPreserved is violated"
  # End-to-end CPU pipeline mutations are independent: reclaim must wake a
  # parked admission, publication requires the complete assembler prefix,
  # completion authority follows the child join, owner reclaim follows
  # ordered completion, and skipped/reclaim-before-completion mechanisms are
  # checked separately.
  "CpuPipelineOwnership|.missing-wake.counterexample|Invariant AdmissionReleaseNotifies is violated"
  "CpuPipelineOwnership|.premature-reclaim.counterexample|Invariant NoPrematureReclaim is violated"
  "CpuPipelineOwnership|.skipped-completion.counterexample|Invariant NoSkippedCompletion is violated"
  "CpuPipelineOwnership|.reclaim-before-completion.counterexample|Invariant NoReclaimBeforeCompletion is violated"
  "CpuPipelineOwnership|.fabricated-gpu-milestone.counterexample|Invariant NoGpuTerminalIsTerminal is violated"
  "CpuPipelineOwnership|.partial-publication.counterexample|Invariant PublicationIsComplete is violated"
  "CpuPipelineOwnership|.completion-before-join.counterexample|Invariant CompletionAuthorityAfterJoin is violated"
  "CpuPipelineLifecycle|.missing-wake.counterexample|Invariant WakeOnReclaim is violated"
  "CpuPipelineLifecycle|.no-reset-generation.counterexample|Invariant ResetGenerationAdvances is violated"
  "CpuPipelineLifecycle|.present-reclaim.counterexample|Invariant PresentSettledBeforeReclaim is violated"
  "CpuPipelineLifecycle|.fabricated-gpu-milestone.counterexample|Invariant NoGpuMilestone is violated"
  "CpuPipelineLifecycle|.owner-omission.counterexample|Invariant SelectedOwnerExact is violated"
  "CpuPipelineLifecycle|.receipt-omission.counterexample|Invariant ReceiptAuthorityExact is violated"
  "CpuPipelineLifecycle|.import-omission.counterexample|Invariant OwnersAreExplicit is violated"
  "CpuPipelineLifecycle|.device-loss-omission.counterexample|Invariant OwnersAreExplicit is violated"
  "CpuPipelineLifecycle|.duplicate-event-identity.counterexample|Invariant EndToEndSourceIdentityExact is violated"
  "CpuPipelineLifecycle|.duplicate-source-identity.counterexample|Invariant EndToEndSourceIdentityExact is violated"
  "CpuPipelineLifecycle|.partial-source-identity.counterexample|Invariant EndToEndSourceIdentityExact is violated"
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
  # Every heterogeneous producer family binds exact source/record ordinals,
  # byte range, value or qualified identity, and fail-stop settlement.
  "PeRecorderSemanticProjection|.missing-source.counterexample|Invariant ExactProjection is violated"
  "PeRecorderSemanticProjection|.wrong-source.counterexample|Invariant ExactProjection is violated"
  "PeRecorderSemanticProjection|.aba.counterexample|Invariant ExactProjection is violated"
  "PeRecorderSemanticProjection|.wrong-producer.counterexample|Invariant ExactProjection is violated"
  "PeRecorderSemanticProjection|.wrong-record.counterexample|Invariant ExactProjection is violated"
  "PeRecorderSemanticProjection|.missing-range.counterexample|Invariant ExactProjection is violated"
  "PeRecorderSemanticProjection|.wrong-value.counterexample|Invariant ExactProjection is violated"
  "PeRecorderSemanticProjection|.wrong-identity.counterexample|Invariant ExactProjection is violated"
  "PeRecorderSemanticProjection|.bridge-retry.counterexample|Invariant BridgeEffectUnknownFailStop is violated"
  "PeRecorderSemanticProjection|.capture-retract.counterexample|Invariant CaptureAfterAccept is violated"
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
  # Reading the staged slot before its release publication breaks the
  # segmented table's fail-closed lookup contract.
  "PsoSlotPublication|.counterexample|Invariant LookupFailClosed is violated"
  # A worker that skips the FIFO head can complete ChunkB before StateBlock.
  "StateBlockOrderedReplay|.non-fifo.counterexample|Invariant CompletedIsAcceptedPrefix is violated"
  # SegmentedTransportV1 fixes the role order ReserveAll -> AdoptAll ->
  # ExactFixed.  These controls independently remove complete reservation,
  # atomic adoption,
  # pre-effect-only fallback, checkpoint restoration, exact ledger commit,
  # and reclaim wake publication.
  "SegmentedTransportV1|.partial-reservation.counterexample|Invariant ReservationIsComplete is violated"
  "SegmentedTransportV1|.partial-adoption.counterexample|Invariant AdoptionIsAtomic is violated"
  "SegmentedTransportV1|.fallback-after-effect.counterexample|Invariant NoFallbackAfterEffect is violated"
  "SegmentedTransportV1|.lost-checkpoint.counterexample|Invariant RollbackRestoresCheckpoint is violated"
  "SegmentedTransportV1|.partial-ledger.counterexample|Invariant LedgerConserved is violated"
  "SegmentedTransportV1|.missing-wake.counterexample|Invariant WakeOnReclaim is violated"
  # Source-wide replay projection keeps persistent state, working state, the
  # representation-preserving effective stream, exact emission receipt, and
  # publication as separate transaction facts.  Each negative control removes
  # one gate and is expected to fail at the corresponding invariant.
  "ReplayProjectionTransaction|.state-before-emission.counterexample|Invariant PersistentStateCommitsOnlyAfterReceipt is violated"
  "ReplayProjectionTransaction|.partial-publish.counterexample|Invariant PublishedOnlyAfterCompleteReceipt is violated"
  "ReplayProjectionTransaction|.optimizer-state.counterexample|Invariant WorkingStateMatchesProjection is violated"
  "ReplayProjectionTransaction|.optimizer-without-proof.counterexample|Invariant OptimizerRequiresProof is violated"
  "ReplayProjectionTransaction|.dedup-attribution.counterexample|Invariant DedupAttributionExact is violated"
  "ReplayProjectionTransaction|.stale-generation.counterexample|Invariant WorkingGenerationQualified is violated"
  "ReplayProjectionTransaction|.ordered-control-reorder.counterexample|Invariant EmittedIsExactSourcePrefix is violated"
  "ReplayProjectionTransaction|.retry-after-effect.counterexample|Invariant NoRetryAfterEffect is violated"
)

# Validate the complete on-disk inventory before starting TLC.  This is kept
# separate from the execution loops so a missing model cannot disappear via a
# wildcard, and imported/generated helper modules remain excluded.
if (( ${#normal_models[@]} != expected_normal_count )); then
  echo "normal safety inventory declaration drifted: expected $expected_normal_count, found ${#normal_models[@]}" >&2
  exit 1
fi
normal_duplicates="$(printf '%s\n' "${normal_models[@]}" | sort | uniq -d)"
if [[ -n "$normal_duplicates" ]]; then
  echo "duplicate normal safety model declarations: $normal_duplicates" >&2
  exit 1
fi

if (( ${#counterexample_models[@]} != expected_counterexample_count )); then
  echo "counterexample inventory declaration drifted: expected $expected_counterexample_count, found ${#counterexample_models[@]}" >&2
  exit 1
fi
counterexample_cfg_names=()
for row in "${counterexample_models[@]}"; do
  model="${row%%|*}"
  rest="${row#*|}"
  cfg_suffix="${rest%%|*}"
  counterexample_cfg_names+=("$model$cfg_suffix.cfg")
done
counterexample_duplicates="$(printf '%s\n' "${counterexample_cfg_names[@]}" | sort | uniq -d)"
if [[ -n "$counterexample_duplicates" ]]; then
  echo "duplicate counterexample declarations: $counterexample_duplicates" >&2
  exit 1
fi

inventory_dir="$(mktemp -d)"
expected_cfgs="$inventory_dir/expected.cfgs"
actual_cfgs="$inventory_dir/actual.cfgs"
declared_counterexamples="$inventory_dir/declared.counterexamples"
actual_counterexamples="$inventory_dir/actual.counterexamples"
{
  for model in "${normal_models[@]}"; do
    printf '%s\n' "$model.cfg"
  done
  printf '%s\n' "$progress_cfg_name"
  printf '%s\n' "${counterexample_cfg_names[@]}"
} | sort >"$expected_cfgs"
inventory_duplicates="$(sort "$expected_cfgs" | uniq -d)"
if [[ -n "$inventory_duplicates" ]]; then
  echo "duplicate TLA configuration inventory entries: $inventory_duplicates" >&2
  exit 1
fi

find "$tla_dir" -maxdepth 1 -type f -name '*.cfg' -exec basename {} \; | sort >"$actual_cfgs"
find "$tla_dir" -maxdepth 1 -type f -name '*.counterexample.cfg' -exec basename {} \; | sort >"$actual_counterexamples"
printf '%s\n' "${counterexample_cfg_names[@]}" | sort >"$declared_counterexamples"

# The progress pair is mandatory even when both files disappear together;
# otherwise the safety inventory can pass while the temporal proof silently
# vanishes.  A second progress configuration is also inventory drift.
progress_spec="$tla_dir/$progress_model.tla"
progress_cfg="$tla_dir/$progress_cfg_name"
if [[ ! -f "$progress_spec" || ! -f "$progress_cfg" ]]; then
  echo "lifecycle progress configuration pair is missing: $progress_model.tla + $progress_cfg_name" >&2
  exit 1
fi
actual_progress_count="$(find "$tla_dir" -maxdepth 1 -type f -name '*.progress.cfg' | wc -l | tr -d '[:space:]')"
if (( actual_progress_count != expected_progress_count )); then
  echo "lifecycle progress configuration count drifted: expected $expected_progress_count, found $actual_progress_count" >&2
  exit 1
fi

for model in "${normal_models[@]}"; do
  if [[ ! -f "$tla_dir/$model.tla" || ! -f "$tla_dir/$model.cfg" ]]; then
    echo "normal safety configuration pair is missing: $model.tla + $model.cfg" >&2
    exit 1
  fi
done

missing_counterexamples="$(comm -23 "$declared_counterexamples" "$actual_counterexamples")"
extra_counterexamples="$(comm -13 "$declared_counterexamples" "$actual_counterexamples")"
if [[ -n "$missing_counterexamples" ]]; then
  echo "listed counterexample configurations are missing:" >&2
  printf '%s\n' "$missing_counterexamples" >&2
  exit 1
fi
if [[ -n "$extra_counterexamples" ]]; then
  echo "unlisted counterexample configurations are present:" >&2
  printf '%s\n' "$extra_counterexamples" >&2
  exit 1
fi

missing_cfgs="$(comm -23 "$expected_cfgs" "$actual_cfgs")"
extra_cfgs="$(comm -13 "$expected_cfgs" "$actual_cfgs")"
if [[ -n "$missing_cfgs" ]]; then
  echo "documented TLA configurations are missing:" >&2
  printf '%s\n' "$missing_cfgs" >&2
  exit 1
fi
if [[ -n "$extra_cfgs" ]]; then
  echo "unlisted TLA configurations are present:" >&2
  printf '%s\n' "$extra_cfgs" >&2
  exit 1
fi

actual_safety_count="$(find "$tla_dir" -maxdepth 1 -type f -name '*.cfg' \
  ! -name '*.counterexample.cfg' ! -name '*.progress.cfg' | wc -l | tr -d '[:space:]')"
actual_counterexample_count="$(find "$tla_dir" -maxdepth 1 -type f -name '*.counterexample.cfg' | wc -l | tr -d '[:space:]')"
actual_cfg_count="$(find "$tla_dir" -maxdepth 1 -type f -name '*.cfg' | wc -l | tr -d '[:space:]')"
if (( actual_safety_count != expected_normal_count ||
      actual_counterexample_count != expected_counterexample_count ||
      actual_cfg_count != expected_cfg_count )); then
  echo "TLA configuration count drifted: expected ${expected_normal_count} safety + ${expected_progress_count} progress + ${expected_counterexample_count} counterexamples = ${expected_cfg_count}, found ${actual_safety_count} + ${actual_progress_count} + ${actual_counterexample_count} = ${actual_cfg_count}" >&2
  exit 1
fi

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

for model in "${normal_models[@]}"; do
  spec="$tla_dir/$model.tla"
  cfg="$tla_dir/$model.cfg"
  metadir="$(mktemp -d)"
  echo "=== $model.tla ==="
  "${tlc_cmd[@]}" -workers "$tlc_workers" -metadir "$metadir" -config "$cfg" "$spec"
  rm -rf "$metadir"
  unset metadir
done

metadir="$(mktemp -d)"
echo "=== $progress_cfg_name ==="
"${tlc_cmd[@]}" -workers "$tlc_workers" -metadir "$metadir" \
  -config "$progress_cfg" "$progress_spec"
rm -rf "$metadir"
unset metadir

for row in "${counterexample_models[@]}"; do
  model="${row%%|*}"
  rest="${row#*|}"
  cfg_suffix="${rest%%|*}"
  expected="${rest#*|}"
  counterexample_cfg="$tla_dir/$model$cfg_suffix.cfg"
  counterexample_spec="$tla_dir/$model.tla"
  if [[ ! -f "$counterexample_cfg" || ! -f "$counterexample_spec" ]]; then
    echo "listed counterexample configuration is missing: $model$cfg_suffix.cfg" >&2
    exit 1
  fi

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
