---
type: "Spec"
title: "Verification Spec"
description: "Verification spec, ownership, ordering, and evidence mapping."
tags: [specs, verification, spec]
---

# Verification Spec

---

## 1. Approach

dxmt9 uses **TLA+** with the **TLC model checker** for formal verification.
TLA+ is a specification language for concurrent and distributed systems.
TLC exhaustively explores all reachable states of a finite model and
checks that every invariant holds and every liveness property is satisfied.

The verification layer sits alongside — not above — the English specs. Each
TLA+ module is the formal counterpart of a section in `backend/spec.md`
or `d3d9/queries/spec.md`. They describe the same system; TLA+ is more
precise and machine-checkable.

| English spec | Formal / deterministic evidence | C++ implementation |
|---|---|---|
| `archicture/spec.md` §6 / `backend/spec.md` §2 | `tla/CommandQueue.tla` | `src/dxmt9/dxmt9_queue.*`, `src/dxmt9/dxmt9_command_queue.*` |
| `archicture/spec.md` §6 / `backend/spec.md` §2.2 | `tla/QueueLifecycleRefinement.tla` | `QueueLifecycleController` in `src/dxmt9/dxmt9_queue.*` |
| `backend/spec.md` §3 | `tla/EncoderLifecycle.tla` | `src/dxmt9/dxmt9_draw_encoder.*`, blit/readback encoder helpers |
| `backend/spec.md` §5 / §7 | `tla/ResourceLifetime.tla` | `src/dxmt9/dxmt9_resource_pool.*` |
| `backend/spec.md` §5 / R-BACK-5.11 | `tla/BufferBackingVersioning.tla` | `src/dxmt9/dxmt9_resource_pool.*`, draw binding snapshots in `src/dxmt9/dxmt9_command_queue.cpp` |
| `backend/spec.md` §8 / §8.1 | `tla/PresentFrameLatency.tla` | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/dxmt9_presenter.*` |
| `backend/spec.md` §6.5 (pacing independence) | `tla/ConcurrentProgressSignals.tla` | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/dxmt9_queue.*` |
| `backend/spec.md` §8.2 (drawable token handoff) | `tla/DrawableToken.tla` | `src/dxmt9/dxmt9_presenter.*` (PresentDrawableToken), `src/dxmt9/dxmt9_command_queue.*` (stash/take) |
| `backend/encode-scheduling/requirements.md` R-BACK-2.49 (EncodeSession completion refinement) | `tla/EncodeSessionCompletion.tla` | `src/dxmt9/dxmt9_queue.*`, `src/dxmt9/dxmt9_command_queue.*` |
| `d3d9-renderer/requirements.md` R-BACK-32.10 (one-successor DCE window) | `tla/DceChunkLookahead.tla` | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/render/framegraph_backend.*` |
| R-BACK-32.11 / R-VERIF-2.13 (bounded ready-prefix DCE) | missing model extension or refinement | planned queue ready-prefix and FrameGraph summary owners |
| R-BACK-2.60 / R-VERIF-2.14 (CPU-ready/session admission progress) | `tla/CpuReadySessionProgress.tla` (bounded scheduling refinement; allocation geometry remains outside this model) | `CpuReadyTape` and `QueueLifecycleController` in `src/dxmt9/dxmt9_cpu_ready_tape.hpp` / `src/dxmt9/dxmt9_queue.*` |
| R-BACK-2.63–2.64 / R-VERIF-2.15 (parallel/joint completion) | missing refinement model | planned encode coordinator and partition workers |
| `backend/spec.md` §2.7 / `include/dxmt9/device_c.h` (cross-side generation) | `tla/WireHandleGeneration.tla` | `src/d3d9/d3d9_pe_recorder.*`, `src/d3d9/device_c_chunk_replay.cpp`, `src/dxmt9/dxmt9_resource_pool.hpp` (HandleArena) |
| `backend/spec.md` §7.2 (slot reuse ABA-safety) | `tla/PresentIdAba.tla` | `src/dxmt9/dxmt9_resource_pool.hpp` (HandleArena), forward-looking PresenterSlot registry in `src/dxmt9/dxmt9_command_queue.*` |
| `d3d9/queries/spec.md` §2-3 | `tla/QuerySeqId.tla` | `src/d3d9/core.cpp` |
| `backend/spec.md` §2 and `tests/spec.md` §0.1 | queue observer / fake backend tests | `QueueLifecycleController`, chunk importer replay path |

---

## 2. Module Structure

```
specs/verification/
├── requirements.md        What must be formally verified and why
├── spec.md              This file: approach, binding, how to run
└── tla/
    ├── CommandQueue.tla   3-thread ring buffer
    ├── CommandQueue.cfg   TLC model parameters
    ├── QueueLifecycleRefinement.tla  QueueLifecycleController staging/refinement
    ├── QueueLifecycleRefinement.cfg
    ├── PresentFrameLatency.tla  Present token and frame-latency gating
    ├── PresentFrameLatency.cfg
    ├── ConcurrentProgressSignals.tla  Pacing independence across three axes
    ├── ConcurrentProgressSignals.cfg
    ├── DrawableToken.tla  PresentDrawableToken stash/take/wait handoff
    ├── DrawableToken.cfg
    ├── EncodeSessionCompletion.tla  one Metal tail → ordered source completions
    ├── EncodeSessionCompletion.cfg
    ├── CpuReadySessionProgress.tla  tentative Ready-prefix and release-fence progress
    ├── CpuReadySessionProgress.cfg
    ├── DceChunkLookahead.tla  one held source → FIFO successor proof window
    ├── DceChunkLookahead.cfg
    ├── WireHandleGeneration.tla  PE → unix generation-stamp gate
    ├── WireHandleGeneration.cfg
    ├── PresentIdAba.tla   (slot, generation) tagged-handle ABA-safety
    ├── PresentIdAba.cfg
    ├── ResourceLifetime.tla  Deferred GPU resource destruction
    ├── ResourceLifetime.cfg
    ├── BufferBackingVersioning.tla  MANAGED concrete-backing reuse lifetime
    ├── BufferBackingVersioning.cfg
    ├── EncoderLifecycle.tla  MTLCommandEncoder state machine
    ├── EncoderLifecycle.cfg
    ├── QuerySeqId.tla     D3D9 query seq-ID fence
    └── QuerySeqId.cfg
```

---

## 3. TLA+ Module Anatomy

Each module follows this structure:

```
CONSTANTS     — model parameters (ring size, limits, bounds)
VARIABLES     — the full system state
Init          — initial state predicate
Actions       — one named action per observable state transition
Next          — disjunction of all actions
Spec          — Init ∧ □[Next]_vars ∧ fairness conditions
TypeOK        — type invariant (always the first invariant checked)
Safety        — conjunction of all safety invariants
Liveness      — temporal properties (leads-to, eventually)
```

---

## 4. Binding to C++ Implementation

The TLA+ spec is an **abstraction** of the C++ implementation. The
relationship is **refinement**: every concrete execution must be a
behavior that the abstract spec permits.

### 4.1 Variable mapping

Each TLA+ variable corresponds to a concrete C++ field. The mapping must
be documented as a comment block at the top of the C++ class:

```cpp
// TLA+: CommandQueue module
// state[s]          → chunks_[s].state     (enum class SlotState)
// chunkSeqId[s]     → chunks_[s].seq_id    (uint64_t)
// currentSeqId      → current_seq_id_      (std::atomic<uint64_t>, Wine thread)
// completedSeqId    → completed_seq_id_    (std::atomic<uint64_t>, finish thread)
// writeIdx          → write_idx_           (size_t, Wine thread only)
// encodeIdx         → encode_idx_          (size_t, encode thread only)
class CommandQueue { ... };
```

### 4.2 Action mapping

Each TLA+ action corresponds to a C++ method. The method must carry a
comment referencing its abstract action:

```cpp
// TLA+: WineCommit — requires InflightCount < MAX_INFLIGHT
void CommandQueue::commitChunk() { ... }

// TLA+: EncodeBegin
void EncodeThread::dequeueChunk() { ... }

// TLA+: FinishComplete — advances completedSeqId
void FinishThread::onGPUCompletion(uint64_t seq_id) { ... }
```

### 4.3 Invariant assertions

Every TLA+ safety invariant must appear as a `DXMT_ASSERT` in the
C++ implementation, executed on every relevant code path in debug builds:

```cpp
// SeqIdSafety: completedSeqId < currentSeqId
DXMT_ASSERT(completed_seq_id_.load(std::memory_order_relaxed)
            < current_seq_id_.load(std::memory_order_relaxed));

// BoundedInflight: InflightCount <= MAX_INFLIGHT
DXMT_ASSERT(inflight_count_.load() <= kMaxInflight);

// RingSafety: state[writeIdx] ∈ {Free, Writing}
DXMT_ASSERT(chunks_[write_idx_].state == SlotState::Free ||
            chunks_[write_idx_].state == SlotState::Writing);

// NoUseAfterFree (ResourceLifetime)
DXMT_ASSERT(res->state != ResourceState::Freed ||
            completed_seq_id_.load() >= res->last_used_seq_id);
```

---

## 5. Model Parameters

TLC requires finite models. The `.cfg` files use small parameters that
exercise all structural behaviors while keeping the state space tractable.

| Module | TLC parameter | TLC value | Production |
|---|---|---|---|
| CommandQueue | `RING_SIZE` | 4 | 32 |
| CommandQueue | `MAX_INFLIGHT` | 2 | 3 |
| CommandQueue | `MAX_SEQID` | 6 | unbounded |
| QueueLifecycleRefinement | `RING_SIZE` | 3 | 32 |
| QueueLifecycleRefinement | `MAX_INFLIGHT` | 2 | `kMaxQueuedChunks` back-pressure limit |
| QueueLifecycleRefinement | `MAX_SEQID` | 4 | unbounded |
| PresentFrameLatency | `MAX_SEQID` | 5 | unbounded |
| PresentFrameLatency | `MAX_FRAME_LATENCY` | 2 | `SetMaximumFrameLatency()` / backend latency cap |
| ConcurrentProgressSignals | `MaxRing` / `MaxFrameLatency` / `MaxSeqId` / `Queries` | 3 / 2 / 6 / `{q1,q2}` | ring=32, latency=2, seq unbounded, queries dynamic |
| DrawableToken | `MAX_PIDS` / `Drawables` | 2 / `{D1,D2}` | unbounded present-ids, opaque drawables |
| EncodeSessionCompletion | `MaxSeqId` / `MaxSessionLen` | 5 / 3 | unbounded source seqIds, bounded by `kMaxEncodeSessionSources` and queue ring size |
| CpuReadySessionProgress | `MaxSources` / `MaxReady` / `MaxResident` / `MaxBatch` / `MaxSessionLen` / `MaxReleaseEvents` / `MaxReleaseGeneration` / `MaxPressureGeneration` | 2 / 2 / 2 / 2 / 2 / 2 / 2 / 2 | unbounded source ordinals; fixed Tape/Ready/session/release capacities and monotone release/latch generations |
| DceChunkLookahead | `MaxSources` / `MaxInflight` | 4 / 3 | unbounded source seqIds / `kMaxQueuedChunks`; exactly one held lookahead source |
| WireHandleGeneration | `Handles` / `WireSlots` / `GENERATION_DOMAIN` / `MAX_BUMPS` | `{h1,h2}` / `{w1,w2}` / 3 / 3 | unbounded handles, generation domain = 2^24 (`D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_BITS`) |
| PresentIdAba | `Slots` / `Entities` / `MAX_GEN` / `MAX_OPS` | `{s1,s2}` / `{p1,p2}` / 3 / 6 | unbounded slots, generation domain = 2^24 (`HandleArena::kGenerationBits`) |
| ResourceLifetime | `Resources` | `{r1,r2,r3}` | dynamic |
| ResourceLifetime | `MAX_SEQID` | 5 | unbounded |
| BufferBackingVersioning | `Backings` | `{b1,b2,b3}` | grow-only per-buffer backing ring |
| BufferBackingVersioning | `MAX_SEQID` | 3 | unbounded |
| EncoderLifecycle | `RenderTargets` | `{rt1,rt2}` | dynamic |
| EncoderLifecycle | `MAX_OPS` | 8 | unbounded |
| QuerySeqId | `MAX_QUERIES` | 3 | dynamic |
| QuerySeqId | `MAX_SEQID` | 5 | unbounded |

**Why small parameters are sufficient:** The structural behaviors (ring
wrap-around, back-pressure blocking, hazard-triggered splits, flush-commit
sequencing) all occur within these bounds. Increasing parameters does not
reveal new behaviors — it only generates more repetitions of the same
patterns.

---

## 6. Refinement Spec (Advanced)

For stronger assurance, a concrete-level TLA+ module can be written that
models the C++ fields directly and proves it **refines** the abstract
module using TLA+'s `INSTANCE` operator:

```tla
---- MODULE CommandQueueImpl ----
VARIABLES chunks, current_seq_id, completed_seq_id, write_idx, encode_idx

CQ == INSTANCE CommandQueue WITH
  state          <- [s \in Slots |-> chunks[s].state],
  chunkSeqId     <- [s \in Slots |-> chunks[s].seq_id],
  currentSeqId   <- current_seq_id,
  completedSeqId <- completed_seq_id,
  writeIdx       <- write_idx,
  encodeIdx      <- encode_idx

\* Refinement obligation: Impl behaviors ⊆ Abstract behaviors
THEOREM ImplSpec => CQ!Spec
====
```

TLC checks this by verifying `CQ!Spec` is satisfied whenever `ImplSpec`
steps occur. This is the strongest form of binding short of a full proof
in a proof assistant (Lean 4, Coq).

Concrete refinement specs should be added once the implementation state is
stable enough to name. `QueueLifecycleRefinement.tla` is the first such model:
it binds the queue's abstract lifecycle to `QueueLifecycleController` staging
fields (`readySlots`, `pendingCompletion`, `completedSeqQueue`, inline
completion, empty commits, and `waitForSequence`).

---

## 7. TLA+ ↔ Code ↔ Test Traceability Matrix

This matrix is the single-page index that maps every formal model to its
concrete code site, the safety/liveness obligations it proves, and the
deterministic native spec that asserts the same contracts without a model
checker. It is the operational counterpart to the high-level binding
table in §1 and is intended to be the first stop when adding, retiring,
or reviewing a TLA+ module.

| Model | System | Code site (primary) | Invariants (safety) | Liveness | Companion native spec |
|---|---|---|---|---|---|
| `CommandQueue.tla` | 3-thread ring buffer (Wine / Encode / Finish), multi-sub-CB chain | `src/dxmt9/dxmt9_queue.*`, `src/dxmt9/dxmt9_command_queue.*` | `TypeOK`, `SeqIdSafety`, `BoundedInflight`, `RingSafety`, `EncodeSafety`, `SubCBProgressBounded`, `OnlyFinalAdvancesSeqId`, `PresentRoutedToTail` | `PendingEventuallyFree`, `EventuallyDrained` | `tests/native/backend/chunk_record_v2_replay_spec.cpp`, `tests/native/backend/dod_replay_observer_spec.cpp` |
| `QueueLifecycleRefinement.tla` | Concrete refinement of `QueueLifecycleController` staging fields | `QueueLifecycleController` in `src/dxmt9/dxmt9_queue.*` | `TypeOK`, `ReadySlotsArePending`, `PendingCompletionAreSubmitted`, `CompletedSeqQueueBounded`, lifecycle refinement of `CommandQueue!Spec` | `WaitForSequenceProgress`, `StopUnblocksWaits` | `tests/native/backend/dod_replay_observer_spec.cpp` |
| `PresentFrameLatency.tla` | Queue-owned frame-latency tokens, present vs non-present timelines | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/dxmt9_presenter.*` | `TypeOK`, `SeqTimelineSafety`, `PresentCompletionSafety`, `OutstandingPresentBound`, `PresentQueueSafety`, `AppWaitReturnSafe` | `SubmittedPresentsEventuallyComplete`, `WaitEventuallyReturnsOrStops` | `tests/native/backend/present_boundary_policy_spec.cpp` |
| `ConcurrentProgressSignals.tla` | Pacing independence across `completedSeqId` / `presentCompletedSeqId` / `ringSlotOccupancy` | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/dxmt9_queue.*` | `TypeOK`, `PacingOrdering` (`presentCompletedSeqId ≤ completedSeqId`), `RingOccupancyBound`, `FrameLatencyBound`, `OutstandingAccounting` | `NoQueryWaitBlocksPresent`, `NoFrameLatencyBlocksQuery`, `NoRingPressureBlocksPresentCompletion` | _(gap: no native spec — cross-axis non-blocking is observable only at the queue, not as a pure-data transform; tracked as `R-VERIF-2.9 / 2.10` evidence shortfall in `specs/verification/gap.md`)_ |
| `DrawableToken.tla` | `PresentDrawableToken` lifecycle: Stash / Complete / Fail / Take / Wait | `src/dxmt9/dxmt9_presenter.*` (token), `src/dxmt9/dxmt9_command_queue.*` (stash/take) | `TypeOK`, `NoDoubleComplete`, `NoUseAfterTake`, `StashTakeOrdering`, `DrawableValueShape`, `TakenIsSink`, `FulfilledMonotonic` | `WaitProgress`, `EventuallyResolved` | `tests/native/backend/present_acquire_policy_spec.cpp` covers the surrounding **policy** (env-var → AcquirePolicy resolver); _(gap: no deterministic spec for the token's stash/take/wait state machine — only the TLA+ model and runtime exercise)_ |
| `EncodeSessionCompletion.tla` | One Metal session tail expands into ordered per-source `seqId` completions | `QueueSubmissionRecord::fixedCompletionSources`, `QueueLifecycleController::submitEncodedSubmission`, pending completion drain in `src/dxmt9/dxmt9_queue.*` | `TypeOK`, `CommittedWaterlineOK`, `OrderedCompletionExpansion`, `NoInlineCompletionOfSessionSources`, `PresentCompletionAfterTail` | _(safety/refinement model; no fairness property)_ | `tests/native/backend/queue_completion_sources_spec.cpp` |
| `CpuReadySessionProgress.tla` | Bounded Ready FIFO, one tentative pre-effect prefix, ordered release FIFO plus generation-stamped pressure latches, forward-looking terminal fence, session/pass acknowledgement, completion/reclaim | `CpuReadyTape` in `src/dxmt9/dxmt9_cpu_ready_tape.hpp`; tentative queue seam and source metadata in `QueueLifecycleController` (`src/dxmt9/dxmt9_queue.*`); `SessionReleaseState::requestTerminal` is implemented as pure state but has no production caller | `TypeOK`, `BoundedStores`, `FifoSourceOrder`, `OneTentativePrefix`, `CompatibleSuffixStaysReady`, `NoYoungerThanEarliestFence`, `RollbackBeforeEffectsRestoresExactPrefix`, `ReleaseQueueOrdered`, `AckAfterRequiredActionAndFenceCoverage`, `NoCompletionBeforeSubmit`, `NoStalePressureAck`, `TerminalFenceSafety` (model-only terminal refinement) | `OrdinaryReleaseProgress`, `AdmissionPressureProgress`, `WriterPressureProgress`; `ShutdownProgress` is a forward obligation, not a claim about the current direct-drain runtime; weak fairness only for enabled coordinator/completion/finish actions | `tests/native/backend/cpu_ready_tape_spec.cpp`, `tests/native/backend/queue_completion_sources_spec.cpp`, `tests/native/backend/cpu_ready_session_join_spec.cpp` |
| `DceChunkLookahead.tla` | One held FrameGraph source optionally owns an encoded prefix, selects an already-ready immediate FIFO successor, or fails open without waiting | `CommandQueue::runDceChunkLookaheadEncodeLoop`, `resolveDceChunkLookaheadAction`, `FrameGraphBackend::onChunkReady` | `TypeOK`, `SubmittedPrefix`, `HeldIsNext`, `ReadyIsFollowingPrefix`, `PublishedPartition`, `BoundedHold`, `PrefixOwnedByHeld`, `CompletionPrefix` | _(safety/refinement model; no-ready is an immediate fail-open action)_ | `tests/native/backend/queue_completion_sources_spec.cpp`, `tests/native/framegraph/fg_optimizer_spec.cpp`, `tests/native/framegraph/fg_linearizer_spec.cpp` |
| Bounded ready-prefix DCE (missing) | Already-ready FIFO prefix summaries shared without DCE source ownership (`R-VERIF-2.13`) | planned ready-prefix owner and FrameGraph summary consumer | consecutive bounded snapshot, independent DAG/completion, conservative stop | immediate no-proof release | missing summary native spec |
| `CpuReadySessionProgress.tla` | Bounded source admission, pre-effect prefix rollback, ordered release fences, and pressure progress (`R-VERIF-2.14` scheduling subset), plus a model-only terminal extension | `CpuReadyTape`, tentative `QueueLifecycleController` APIs, and the command-queue release coordinator; terminal production wiring remains planned | exact FIFO partition, bounded Ready/resident/session/release storage, suffix-stays-Ready, fence-respecting representation, action-covered acknowledgement, no completion before submit, generation-current pressure acknowledgement | posted ordinary/pressure fences eventually acknowledge; the terminal submit/complete/reclaim property is forward-looking until runtime posts `requestTerminal` | `tests/native/backend/cpu_ready_tape_spec.cpp`, `tests/native/backend/queue_completion_sources_spec.cpp`, `tests/native/backend/cpu_ready_session_join_spec.cpp` |
| Parallel/joint completion (missing) | Serial-order refinement for child encoders and Metal 4 groups (`R-VERIF-2.15`) | planned coordinator/partition executor | ordered child/segment join, completion after all represented work | bounded jobs eventually join or fail before side effects | missing fake-child executor spec |
| `WireHandleGeneration.tla` | PE → unix cross-side generation-stamp gate on `D9CCommandChunkWireHandleEntryV2` | `src/d3d9/d3d9_pe_chunk_v2_builder.*`, `src/d3d9/device_c_chunk_v2_registry.cpp`, `src/dxmt9/dxmt9_resource_pool.hpp` (`HandleArena`, `kGenerationBits=24`), `include/dxmt9/device_c.h` | `TypeOK`, `NoZombieAccept`, `StampedMatchesArenaOnAdmit`, `NoForwardInconsistency` | `EventuallyDecided`, `EventuallyRejectStale` | `tests/native/bridge/chunk_record_v2_registry_spec.cpp`, `tests/native/bridge/chunk_record_v2_validation_spec.cpp`, `tests/native/bridge/chunk_record_v2_layout_spec.cpp` |
| `PresentIdAba.tla` | (slot, generation) tagged-handle ABA-safety — `HandleArena` today, forward-looking `PresenterSlot` registry | `src/dxmt9/dxmt9_resource_pool.hpp` (`detail::HandleArena<R,K>`, `encode`, `find`, `releaseSlot`) | `TypeOK`, `StaleResolvesNull`, `NoCrossSlotAlias`, `GenerationOverflowDocumented`, `GenerationMonotone` | `EventualReclaim` | `tests/native/bridge/chunk_record_v2_registry_spec.cpp` (generation reuse/reject path); _(gap: no dedicated `HandleArena` slot-reuse spec)_ |
| `ResourceLifetime.tla` | Deferred GPU resource destruction; `lastUsedSeqId ≤ completedSeqId` before free | `src/dxmt9/dxmt9_resource_pool.*` | `TypeOK`, `NoUseAfterFree` | `DestroyPendingEventuallyFreed` | `tests/native/backend/resource_hazard_spec.cpp`, `tests/native/core/resource_format_boundary_spec.cpp` |
| `BufferBackingVersioning.tla` | MANAGED CPU-shadow upload selects a safe concrete backing and draw packets pin it by sequence | `BufferRecord::renameRing`, `Pool::uploadBufferData`, `Pool::snapshotBufferBinding`, `Pool::markBufferSnapshotUse` | `TypeOK`, `ActiveBackingAllocated`, `LogicalWatermarkCoversEveryBacking`, `NoUploadOverwriteInFlight`, `NoBackingFreedInFlight`, `DestroyPendingCannotFreeEarly` | `DestroyPendingEventuallyFreed` | `tests/native/backend/dynamic_rename_ring_spec.cpp`, `tests/native/core/core_device_com_spec.cpp`, `tests/native/bridge/pe_buffer_lock_order_spec.cpp` |
| `ReplayScopedDrain.tla` | Core-buffer-identity commit-replay ledger shared by wrapper aliases, scoped/global terminal wakeups, failure-before-completion publication, pre-rename backing capture, and queued plus in-flight raw-entry residency | `ReplayDrainLedger`, `ReplayOffloadQueue`, the rename ring, and raw V2 admission/replay in `src/d3d9/device_c_*` / `src/dxmt9/dxmt9_resource_pool.*` | `TypeOK`, `ReplayedLeQueued`, `NoRejectedWatermark`, `ScopedReturnSafe`, `GlobalReturnSafe`, `StopUnblocksWithoutSuccess`, `QueuedChunkUsesCapturedGeneration`, `OneGenerationPerRawChunk`, `FailedNeverAcknowledged`, `RawEntryImmutable`, `RawResidencyMatchesOutstanding`, `InFlightRetainsResidency`, `TerminalPrecedesCompletion`, `FailureBlocksAdmission`, `InlineTraceEquivalent`, `UnrelatedResourceDoesNotBlock` | `ScopedWaitEventuallyReturnsOrStopsOrPoisons`, `GlobalWaitEventuallyReturnsOrStopsOrPoisons` | `tests/native/backend/replay_offload_queue_spec.cpp`, `tests/native/backend/dynamic_rename_ring_spec.cpp`, `tests/native/backend/replay_byte_identity_spec.cpp`, `tests/native/bridge/pe_buffer_lock_order_spec.cpp`, `tests/native/core/core_device_com_spec.cpp` |
| `EncoderLifecycle.tla` | `MTLCommandEncoder` mutual exclusion + exact hazard sets + Bloom-as-diagnostic | `src/dxmt9/dxmt9_draw_encoder.*`, blit/readback encoder helpers | `TypeOK`, `KindSwitchThroughIdle`, `RenderTargetConsistency`, `ExactHazardBlocksMerge`, `BloomNeverForcesSplit` | `ActiveEncoderEventuallyEnds` | `tests/native/backend/resource_hazard_v2_spec.cpp` |
| `QuerySeqId.tla` | D3D9 query seq-ID fence; `D3DGETDATA_FLUSH` deadlock freedom | `src/d3d9/core.cpp`, `src/d3d9/core_query.cpp` | `TypeOK`, `QueryResolutionSafety`, `SeqIdMonotone` | `QueriesEventuallyResolve`, `NoDeadlockOnFlushSpin` | `tests/conformance/d3d9/d3d9_queries.cpp`, `tests/native/core/core_device_lifecycle_spec.cpp` |

### 7.1 What the three new models close

The three models added in May 2026 each close a class of bug that the
prior pre-existing models could not see:

- **`DrawableToken.tla`** — pins down the encoder-thread / async-acquire
  worker race over a single-use token. Before this model, the token's
  `mutex_ / cv_ / ready_` handoff was justified only by code review;
  the spec now machine-checks `NoDoubleComplete` (`fail()` or
  `complete()` may not both fire), `NoUseAfterTake` (the queue's slot
  is empty after `takeDrawableToken`), and liveness of `waitDrawable()`
  under fair `complete/fail`. The pre-existing `present_acquire_policy_spec`
  only covers env-var resolution into an `AcquirePolicy` value; the
  state machine itself was previously unverified.

- **`WireHandleGeneration.tla`** — formalises the cross-side
  generation-stamp invariant on `D9CCommandChunkWireHandleEntry`.
  Without it, a stale handle could survive PE-side recording and
  alias onto a freshly-recycled arena slot at unix-side commit time
  (a classic recorder/importer zombie race). The model proves
  `NoZombieAccept` for stamped handles, `LegacyNoneAlwaysAccepts`
  for opaque-pointer recorder paths (the documented soft exception),
  and `NoForwardInconsistency` between the stamp and the live arena
  generation at admit time.

- **`PresentIdAba.tla`** — the abstract slot/generation skeleton
  shared by `HandleArena` today and the planned `PresenterSlot`
  registry. Proves `StaleResolvesNull` (an unregistered id never
  resolves to a re-handed occupant) and `NoCrossSlotAlias`
  (lookup of an id returns exactly the slot the id points at), the
  two ABA-safety properties that justify the 24-bit generation
  tag. The model deliberately exposes wrap so reviewers can see the
  documented residual risk (the model proves the invariants
  conditionally on `UnwrappedIds`, matching the implementation's
  "2^24 is large enough" assumption).

### 7.2 Evidence gaps surfaced by this matrix

Compiling the matrix surfaced three "TLA-only" rows where there is no
deterministic native spec. They are tracked together in `specs/verification/gap.md`:

- `ConcurrentProgressSignals.tla` — cross-axis non-blocking is a
  queue-observation property, not a pure-data transform. A fake-backend
  observer covering all three axes simultaneously would be useful but
  does not exist today.
- `DrawableToken.tla` — the token's `stash → wait → take → complete/fail`
  interleaving has no deterministic companion. The native
  `present_acquire_policy_spec` covers only policy selection.
- `PresentIdAba.tla` — `HandleArena` slot reuse is only exercised end-to-end
  through `chunk_record_v2_registry_spec`'s generation-reject path; a
  focused `HandleArena` slot-recycle spec would tighten the binding.

---

## 8. How to Run TLC

### Run all specs

```sh
bash scripts/check/verify_tla.sh
```

This is the current verification path in the repository. Remote CI is not
configured yet; when it is added, it should invoke the same script.

### Expected output

```
Model checking completed. No error has been found.
  Estimates of the probability that TLC did not check all reachable states
  because two distinct states had the same fingerprint:
  calculated (optimistic):  ...
```

Any `Invariant ... is violated` or `Temporal properties were violated`
output is a spec defect and must be resolved before implementation proceeds.

The checked-in `.cfg` files set `CHECK_DEADLOCK FALSE` so TLC focuses on the
bounded-model invariants and liveness properties that matter for these specs.
