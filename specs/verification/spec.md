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

### 1.1 Rendering-optimization evidence ladder

Rendering performance work commonly preserves the public D3D9 command stream
while changing when, where, or with which cached state Metal observes it. Those
lanes use the following evidence order. Later layers do not replace earlier
ones.

| Layer | Question | Preferred evidence |
|---|---|---|
| Semantic reference | What serial behavior and visible state must remain equivalent? | backend requirement plus explicit source-qualified order/state contract |
| Bounded transition proof | Can any reordering, reuse, split/join, failure, or interleaving reach an invalid state? | small TLA+ refinement or exhaustive finite-state checker |
| Model-to-code binding | Does production C++ make the same transition decision? | shared pure predicate plus native truth-table/property/fake-backend spec |
| GPU-visible oracle | Do concrete bindings, shader layouts, resource bytes, and pixels agree? | offscreen readback, shader runner, or same-input mini replay |
| Wild integration | Does the complete application expose an omitted state shape? | supervised representative scene run with visual and mechanism evidence |
| Performance promotion | Does the proven lane improve the intended bottleneck without locality regressions? | matched counters, frame timing, and the owning benchmark gate |

The bounded model should be the smallest model that contains the distinguishing
trace. Examples include `A -> B -> A` binding generations, two partition-local
shadows, a coordinator plus two children, one failure boundary, or one
store/load-action epoch. Do not start by composing the entire queue, renderer,
and GPU: keep ownership/liveness and binding/pixel safety in separate models and
compose only their published interface invariants.

Use safety invariants for stale bindings, wrong PSO ABI, duplicated or reordered
draws, illegal pass actions, and premature completion. Use temporal properties
for progress, join, wakeup, and eventual drain. Pixel equality remains outside
the temporal abstraction and requires the GPU-visible layer. Runtime counters,
assertions, and watchdogs are useful monitors at production scale, but they are
not substitutes for refinement or a pixel/resource oracle.

A counterexample discovered in a wild run must be reduced into the earliest
applicable deterministic layer before the lane is reconsidered for promotion.
For example, a scene-dependent stale-uniform artifact should become a bounded
payload-generation trace and an alternating-uniform GPU readback fixture, rather
than remaining a requirement to watch a long benchmark by eye.

### 1.2 Method selection: exhaustive native, SMT, and TLA+

These mechanisms overlap in convenience but own different claims:

| Mechanism | Executes | Best claim | Does not establish |
|---|---|---|---|
| Exhaustive native test | Production pure predicate over every tuple in a declared small finite domain | Truth-table completeness, model/code isomorphism, boundary arithmetic | Unbounded behavior, thread interleavings outside the predicate, Metal/pixels |
| SMT solver | Symbolic formula over bit-vectors, integers, arrays, or booleans | A bounded counterexample exists (`SAT`), cannot exist (`UNSAT`), or two static policies are equivalent under assumptions | Temporal progress, driver behavior, representative workload economics |
| TLA+ / TLC | Abstract transition system with nondeterministic actions and fairness | Reachability safety, interleavings, ownership, join/order, eventual progress | Shader/resource bytes, floating point, real pixels, performance |
| Concrete Render Tape replay | Captured shaders, resources, commands, production importer/provider, Metal | Same-input structural and GPU-visible equivalence; fast repeatable experiments | States absent from the tape, exact application call timing, a universal proof |
| Supervised wild run | Full application and environment | Integration discovery and final workload evidence | Determinism or isolation by itself |

An exhaustive native test is therefore not an SMT solver: it literally calls the
real C++ predicate for every member of a small domain. An SMT test encodes the
predicate or policy as constraints and asks a solver for a witness. SMT is useful
when the Cartesian product is too large but the policy is still a static value
relation—for example partition coverage, range non-overlap, bounded hazard-set
equivalence, or whether any accepted cost-model assignment violates a threshold.
It is a poor primary tool for queue wakeups or child-join progress; those belong
to a temporal transition model.

For every SMT-backed claim, record the formula owner, production predicate or
translation it refines, bit widths/bounds, assumptions, solver/version, expected
result, and a native witness replay for every `SAT` counterexample retained as a
regression. For every exhaustive native claim, record the generated domain and
case count. Neither result may be summarized merely as "formally verified."

### 1.3 Render Tape as a concrete refinement layer

The unified replay architecture is specified in
`specs/experiments/harness/replay/spec.md` §0/§8. Verification treats its
profiles differently:

- `draw-slice` is an implemented, narrow GPU oracle for selected draws and
  emitted shader/resource payloads. It does not exercise the production queue.
- `frame-tape` is the first bounded reference workload: one complete Present
  interval, a consistent checkpoint, resource/direct-control journal, canonical
  wire chunks, production import/queue/provider replay, and offscreen readback
  evidence. One captured `perf-d3d9-present-loop` bundle has completed this
  narrow production-provider path; broader grammar and sequence evidence remain
  separate.
- `sequence-tape` currently extends the same proof over exactly two textured-UP
  intervals separated by one complete texture mutation. A nominal ten-second
  capture remains a future corpus-selection policy, not a new semantics.

`dxmt9-render-tape-spec` contains the bounded capture/replay refinement required
by `R-VERIF-6.6`. It exhaustively executes 45 cases against production
`validateRenderTape`, `replayPrevalidatedRenderTape`, and
`WireObjectRegistry::resolveAndRetain`:

| Finite domain | Cases | Checked production decision |
|---|---:|---|
| Five initial-content states (Upload, CpuUnlock, missing, short, stale) × live/stale draw × live/stale oracle | 20 | create→write→draw→Present admission, exact initial-byte closure, stale command/oracle rejection |
| Five between-Present mutation states (Upload, CpuUnlock, missing, stale, duplicate) × live/stale second draw | 10 | exactly two complete intervals, one boundary mutation, ordered per-interval Present/completion |
| Six wire identity kinds × old/new generation lookup after destroy/recreate | 12 | stale generation rejects before retain; replacement generation admits |
| Three mutable resource kinds reused inside one retained tape | 3 | conservative `RetainedSlotReuse` rejection |

Every accepted tape is replayed through a serial value sink that checks live
handle resolution, mutation-before-draw, draw-before-Present, one completion per
Present, and the final mutation digest. This is an exhaustive native checker of
the declared 45-case domain, not a universal or temporal proof. In particular,
the live wire registry permits destroy/recreate only after generation advance,
while one retained Render Tape deliberately rejects reuse because the old
generation remains reachable in its journal. Concrete Metal replay closes what
this abstract checker cannot see: actual resource bytes, shader layouts, pass
actions, and output pixels. Prior-output loads, arbitrary controls, broader
provider grammar, unbounded sequences, concurrency, and driver behavior remain
outside this checker.

The v2 identity-segment lane has a bounded refinement in
`RenderTapeIdentitySegments.tla`. One event is bounded to three authenticated
source rows and six records: the model proves disjoint/ordered segment ranges,
pass-piece continuity at source edges, flattened (strictly ordered but not
numerically adjacent) source/sequence completion, atomic all-row Ready
publication, and exact full-run settlement registration through the tail seq.
It also models two-phase newest-suffix abort (all members may be detached and
held Reclaiming before destruction, then finish strictly reverse-tail), exact
high-water restoration, one pre-effect EventSerial fallback, post-effect
fail-stop, shared greatest-dependent resource/page watermarks, final event
settlement, and reclaim liveness under weak fairness. Native
`cpu_ready_tape_spec.cpp` and `render_tape_capture_spec.cpp` bind the abort,
partition, and settlement predicates; Metal/pixels, unbounded provider
grammar, and GPU completion scheduling remain outside this bounded claim.

| English spec | Formal / deterministic evidence | C++ implementation |
|---|---|---|
| `d3d9/recorder/requirements.md` R-CORE-REC-1.1–3.5, 5.1.3–5.1.4, 6.1–6.3, 7.4 / R-VERIF-6.7 | `PeRecorderTransition.tla` binds the production semantic pending tuple; `PeRecorderCommit.tla` and composed `PeRecorderSettlement.tla` bind append/CapacityPost/bridge/capture/drain/reset. `PeRecorderSemanticProjection.tla` imports its 21-row family set from production and binds exact source/record/range/value/identity through retry, poison, and capture settlement. StateBlock transition/transaction/value models bind lifecycle, epochs, tracked values, and staged-ref cardinality. | `planRecorderStateWrite` / `settleRecorderAppend` / `settleRecorderCommit` / `planCommittedPeSemanticProjection` / `planPeStateBlockTransition` / `planPeStateBlockValue`; production settlement remains in `d3d9_pe_producer.cpp`, the common `D3D9DeviceImpl::appendRecord` envelope, `D3D9StateBlockImpl`, and `D3D9DeviceImpl`. Exact same-slot COM reference conservation is native fake-object evidence; generic runtime fault and Wine evidence remain open. |
| `archicture/spec.md` §6 / `backend/spec.md` §2 | `tla/CommandQueue.tla` | `src/dxmt9/dxmt9_queue.*`, `src/dxmt9/dxmt9_command_queue.*` |
| `archicture/spec.md` §6 / `backend/spec.md` §2.2 | `tla/QueueLifecycleRefinement.tla` | `QueueLifecycleController` in `src/dxmt9/dxmt9_queue.*` |
| `backend/spec.md` §3 | `tla/EncoderLifecycle.tla` | `src/dxmt9/dxmt9_draw_encoder.*`, blit/readback encoder helpers |
| `backend/spec.md` §5 / §7 | `tla/ResourceLifetime.tla` plus the expected-failure `ResourceLifetime.counterexample.cfg` | shared `resources::canReclaimRecord`, `resources::lifetime::pendingInitializerReferenceSafe`, `HandleArena`, `Pool::StagingCopy`, and `Initializer` in `src/dxmt9/dxmt9_{mark_reclaim_predicates,resource_lifetime,resource_pool,resource_initializer}.*` |
| `backend/spec.md` §5 / R-BACK-5.11 | `tla/BufferBackingVersioning.tla` | `src/dxmt9/dxmt9_resource_pool.*`, draw binding snapshots in `src/dxmt9/dxmt9_command_queue.cpp` |
| `backend/buffer-mutation-offload/requirements.md` R-BACK-44.2–44.4 / R-BACK-44.6 | `tla/BufferMutationOffload.tla` plus the expected-failure `BufferMutationOffload.counterexample.cfg` (second queue) and `BufferMutationOffload.rotation.counterexample.cfg` (deferred rotation) | shared `resources::mutation_offload::{admitsManagedMutationOffload,mutationBlocksChunkReplay,chunkBlocksMutationApply,captureRevisionIsCurrent}` in `src/dxmt9/dxmt9_mutation_offload_predicates.hpp`, consumed by the production `Buffer::unlock`, `Pool` rotation/apply owners, and `ReplayOffloadQueue` drain loop. V1 is implemented and default on. The opt-in `MutationCompositionObserver` is bound to exact mutation/use/barrier/completion edges and assigns one typed generation-qualified ordering identity at ingress; `MutationComposition.tla` models that binding and the native cases cover both mixed directions plus reset fail-closed behavior. The earlier zero-candidate runtime result is unreliable due to the former sync-private-counter versus replaySeq domain mismatch; composition remains forbidden pending remeasurement and matched semantic/runtime evidence. |
| Ordered StateBlock replay FIFO | `tla/StateBlockOrderedReplay.tla` plus the expected-failure `StateBlockOrderedReplay.non-fifo.counterexample.cfg` | `ReplayOffloadQueue::pushStateBlockApply` and `ReplayOffloadWorker::run`; `dxmt9-managed-mutation-offload-transaction-spec` pins `Chunk -> StateBlock -> Chunk`, retained snapshot ownership, teardown release, and worker-failure poison. |
| `backend/spec.md` §7.2.1 / R-BACK-5.12 | `tla/NoOverwriteByteRange.tla` | `core::Buffer` lock metadata, `Device::uploadBufferDataRange`, `Pool::uploadBufferDataRange`; `tests/native/core/core_device_com_spec.cpp`, `tests/native/backend/dynamic_rename_ring_spec.cpp` |
| `backend/spec.md` §8 / §8.1 | `tla/PresentFrameLatency.tla` | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/dxmt9_presenter.*` |
| `backend/spec.md` §6.5 (pacing independence) | `tla/ConcurrentProgressSignals.tla` | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/dxmt9_queue.*` |
| `backend/spec.md` §8.2 (drawable token handoff) | `tla/DrawableToken.tla` | `src/dxmt9/dxmt9_presenter.*` (PresentDrawableToken), `src/dxmt9/dxmt9_command_queue.*` (stash/take) |
| `backend/encode-scheduling/requirements.md` R-BACK-2.49 (EncodeSession completion refinement) | `tla/EncodeSessionCompletion.tla` | `src/dxmt9/dxmt9_queue.*`, `src/dxmt9/dxmt9_command_queue.*` |
| R-ARCH-7.11–7.24 / R-BACK-2.85–2.100 / R-VERIF-2.23 and R-VERIF-7.9–7.10 (end-to-end CPU pipeline owner lifecycle) | `tla/CpuPipelineLifecycle.tla` imports generated `PipelineLifecycleTable.tla`; the bounded safety cfg checks producer interval completeness/non-overlap, generation/occupancy/owner/wake/present/no-GPU predicates and independent failure counterexamples, while `CpuPipelineLifecycle.progress.cfg` checks weak-fair normal completion, reclaim, Present settlement, and selected-parallel join. Duplicate and partial producer-identity configurations fail independently. `tla/ReplayProjectionTransaction.tla` composes at the emission-receipt boundary and checks source-order A→B→A projection, generation qualification, exact logical attribution, optimizer isolation/proof, receipt-before-state/publication commit, pre-effect rollback, and post-effect no-retry; eight independent negative configurations remove one gate each. Retained-PE-span, false same-address-adoption, carrier-removal audit, and four-boundary ledger reconciliation remain open. | `PeSemanticBatchOwner`, contiguous/segmented bridge import, `RawCommandChunk`, `ReplayTransaction`, `DeviceStateUndoJournal`, `CpuReadyTape`, `QueueLifecycleController`, generation-qualified `PipelineLifecycleSidecar`, completion/reclaim owners, `pipelineReplayFailureMayRollback`, and focused native bridge/backend specs; Direct/Arena final destinations and compatibility replay use the same transaction relation |
| `backend/encode-scheduling/requirements.md` R-BACK-2.44 / 2.45 / 2.49 / 2.59 / 2.65 (post-encode payload retirement) | `tla/PostEncodePayloadRetirement.tla` models encode-before-retire, receipt authority, two-phase destruction, finish-time generation reuse, mixed completion, device-loss settlement, and Present ordering | `PostEncodeCompletionLedger`, `QueueSubmissionRecord`, `PendingCompletion`, and the Tape/session retirement path in `src/dxmt9/dxmt9_post_encode_retirement.hpp`, `src/dxmt9/dxmt9_queue.*`, and `src/dxmt9/dxmt9_command_queue_cpu_ready_session.cpp` |
| `backend/encode-scheduling/requirements.md` R-BACK-2.67 (composed scheduling progress) | `tla/EncodeSchedulingProgress.tla` composes the published interfaces of the admission, capacity, wake, session, retirement, completion, Present, and stage-specific terminal-drain models. Its seeded denied-first-lease state forces later Arena admission pressure, two exact ordinary-Direct singleton escapes for distinct FIFO heads under one unchanged capacity generation, complete batch-control readiness, and FIFO completion/reclaim; source-arrival and GPU-settlement environment assumptions remain explicit and separate from queue weak fairness. | `render/encode_scheduling_progress.hpp`, `SchedulingProgressWatchdog`, `QueueLifecycleController`, `CommandQueue`, and `tests/native/backend/encode_scheduling_progress_spec.cpp` |
| R-ARCH-7.11–7.24 / R-BACK-2.85–2.100 / R-VERIF-2.23 and R-VERIF-7.9–7.10 (direct construction and end-to-end ownership progress) | Arena/ordinary final-storage transactions, source-wide state checkpoint/settlement, same-raw differential, production Metal oracle, queue/Tape batch projection, PE-to-reclaim identity composition, and concrete carrier retirement implemented; complete final-slot/direct differential, materialization-floor evidence, and promotion evidence closed by focused native production routing and transaction tests | `CpuPipelineOwnership.tla` separates `GPUInFlight`, `Completed`, and `Reclaimed`; `CpuPipelineLifecycle.tla` owns the immutable producer interval, contiguous GPU-completion frontier, and finish-thread waterline composition. `ReplayProjectionTransaction.tla` owns source projection through exact destination receipt; production and native tests share `advanceReplayTransaction`, while a fixed-capacity typed undo journal contains in-place working-state mutations until settlement. `dxmt9_pipeline_lifecycle.hpp` supplies the strict production reducer, storage-generation sidecar, physical batch id/index/count, and replay rollback cut. Production emits compatibility and Arena arrival/publication, every multi-source dequeue/submit/completion identity, finish advance, actual reclaim, inline no-GPU terminal, and pending-completion fail-stop. `TransactionalChunkSlotAssembler` retains sticky pre-effect rollback/post-effect fail-stop and direct continuation extends only adjacent PE intervals. The model/code binding still does not distinguish a valid Unix `RawOwned` copy from an invalid retained PE span or unowned same-address alias. `DrawRunSubmission`, its APIs/adapters/scratch, and carrier-specific counters/tests are absent from production; ordinary fallback now uses synchronous borrowed-state ingress into final `ChunkSlot` storage. The stable `OwnedRawFinalChunkSlot` topology is represented by `FinalOwned` → `Encoding` in `CpuPipelineLifecycle.tla`, with the dedicated encode thread as the stable execution owner. The final-slot/direct effective-stream, projected next-state, final-storage, resource/completion, and failure differential are closed by the focused production routing and transaction evidence. A fresh four-boundary ledger, C++ atomic ordering, and Wine/GPU/wild equivalence remain open; compatibility-only ranges remain compatibility-owned and outside the carrier-free claim. |
| `d3d9-renderer/requirements.md` R-BACK-32.10 (one-successor DCE window) | `tla/DceChunkLookahead.tla` | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/render/framegraph_backend.*` |
| R-BACK-32.11 / R-VERIF-2.13 (bounded ready-prefix DCE) | missing model extension or refinement | planned queue ready-prefix and FrameGraph summary owners |
| R-BACK-2.60 / R-BACK-2.65 / R-VERIF-2.14 (CPU-ready/session admission progress) | `tla/CpuReadySessionProgress.tla` constrains releases to semantic/terminal events; `tla/SessionCapacityLease.tla` refines the generation lease, complete ordinary-successor reserve, cap boundary, and completion-independent grouping | `CpuReadyTape`, `SessionCapacityLeaseState`, and the session coordinator in `src/dxmt9/dxmt9_cpu_ready_tape.hpp`, `src/dxmt9/render/encode_session_admission.*`, and `src/dxmt9/dxmt9_command_queue_cpu_ready_session.cpp` |
| R-BACK-2.41 / R-BACK-2.65 (active-seeded retained-head lookahead) | `tla/CpuReadyActiveHeadLookahead.tla` checks no intent for an empty FIFO, intent only for an already-adopted raw, non-source cancellation, pre-effect hold, exact FIFO restore, active seed preservation, successor/fallback progress, and exactly-once effects; the seedless companion configuration reproduces the retired R15 planner defect | `ReplayOffloadQueue::pop`, `CommandQueue::{arm,cancel}CpuReadyNextSourceIntent`, `CommandQueue::runCpuReadySessionEncodeLoop`, `encodeChunkSessionReplayFrontierState`, and the replay/session native specs |
| R-BACK-2.63–2.64 / R-VERIF-2.15 (parallel execution correctness and joint completion) | `tla/ParallelDrawBinding.tla` plus `dxmt9-draw-uniforms-dirty-spec` and `dxmt9-parallel-draw-binding-metal-spec` close source-local Stage 1/Stage 2b pass-wide binding, order, and join; Metal 4 segment completion remains open | `planDrawBindingTransition`, pass-wide binding preflight, source-local encode coordinator, and production child workers |
| R-BACK-2.68–2.75 / R-VERIF-2.16–2.22 (parallel policy safety and selection) | `ParallelPassSemanticPlanView` is a value-owned proof-core certificate; `validateParallelPassSemanticPlan` performs one owner-issued interval snapshot lookup and one synchronous resolver call per child, binding exact child-wide coverage, authoritative DrawRun begin/count, first-child locator, independent generations, epoch/coordinator/attachment/resource/route identity, and ordered ranges; `selectParallelPassCandidate` provides structural-economics validation, checked Q16.16 safe-only ranking, overflow/invalid/non-positive serial fallback, fewer-child/full source-qualified range tie-breaks, and permutation-independent argmax. Native `parallel_render_pass_spec` owns bounded adversarial coverage/arithmetic/selection evidence; `ParallelPolicySelection.tla`/`.cfg` covers all-input-valid selection, benefit-zero skipping, selected-proof-only effect, serial fallback, join, parent, completion, and fairness-backed progress. The production coordinator does not invoke this selector in this increment; production replay remains first-match/default-off. Render Tape owns deterministic production-import/replay identity and Metal readback owns concrete ABI/resource/shader/pixel behavior; no layer alone authorizes promotion or claims production R-BACK-2.69 enforcement. | `src/dxmt9/dxmt9_parallel_render_pass.hpp/.cpp`, `tests/native/backend/parallel_render_pass_spec.cpp`, `tla/ParallelPolicySelection.tla`, and `tla/ParallelPolicySelection.cfg` |
| `backend/spec.md` §2.7 / `include/dxmt9/device_c.h` (cross-side stable identity) | `tla/WireObjectRegistry.tla` | `src/d3d9/d3d9_pe_chunk_builder.*`, `src/d3d9/device_c_chunk_registry.*`, `src/d3d9/device_c_chunk_validate.*` |
| `backend/spec.md` §7.2 (slot reuse ABA-safety) | `tla/PresentIdAba.tla` | `src/dxmt9/dxmt9_resource_pool.hpp` (HandleArena), forward-looking PresenterSlot registry in `src/dxmt9/dxmt9_command_queue.*` |
| `d3d9/queries/spec.md` §2-3 | `tla/QuerySeqId.tla` | `src/d3d9/core.cpp` |
| `backend/spec.md` §2 and `tests/spec.md` §0.1 | effective replay observer tests | `ReplayObserverSink` in `dxmt9_queue.hpp`, the post-selection per-command seam in `dxmt9_draw_encoder_chunk.mm`, and `encode_session_lifecycle_spec.cpp` |
| `experiments/harness/replay/requirements.md` R-HARN-REPLAY-7.2–7.15 / R-VERIF-6.6 | bounded 45-case capture/replay refinement, deterministic repetition, closure-aware reducer/bisect, two-interval sequence identity, production routing, capture-time output oracle, and captured frame identity complete; broader grammar/captured sequence evidence open | `device_c_render_tape.*` provides the production validator/replay predicates used by the exhaustive checker and whole-command reducer. `device_c_render_tape_provider.*` shares the production canonical-chunk validator, `DeviceReplaySink`, queue/completion, and offscreen presenter seam. The CLI constructs a fresh process/device per warm-up and measured run and requires exact ordered output/conservation identity. Production capture fences prior replay and renderer work before publishing a one-shot normal-Presenter mirror for canonical Present, reuses the same-command-buffer pass, and drains plus flushes captured work before typed readback/tight-hash validation. Native provider tests prove two distinct outputs across one sequence mutation and repeat the two-digest vector on a fresh device. Captured production evidence remains the two bounded frame bundles below; no captured sequence bundle exists yet. |
| Bounded textured-UP increment for R-HARN-REPLAY-7.6–7.8 / R-VERIF-6.6 | native fail-closed grammar, production Metal repeat identity, and captured wild identity complete | `device_c_render_tape_provider.*` preserves flattened record order across command-chunk events and admits one exact `Clear` → fixed-function textured `DrawPrimitiveUP` → standard `Present` form. `dxmt9-render-tape-provider-spec` covers split/combined chunks, full A8R8G8B8 seed closure, exact production FVF-generated declaration admission and near-miss rejection, tight non-uniform readback, representation-equivalent Metal SHA-256 `3dc6ca2708ccbb285106dea4b1cba42e6d67dd69ffe72ab003d87d7d8250b72e`, and 2/2 or 3/3 object conservation. The 2026-08-13 Sikarugir `PRESENT_LOOP_TEXTURED=1` bundle `experiments/output/render-tape-wild-textured-r2/tapes/frame-99975454225700-1` has 8 events, 3 records in 2 chunks, 3 definitions, 2 blobs, and one mutation; provider replay returns `complete`, `output_non_degenerate=true`, 2/2 blob references, 3/3 object conservation, and exact capture/replay 262144-byte SHA-256 `866e45bc5527c590f7cbf1deb9ca8fd5aa3ac2eddcd6746bdaf0572848a78c17`. |
| Generation-qualified ProducedByCapturedPass full-clear admission | bounded native value proof and validator/provider plumbing complete; wild and GPU promotion evidence open | `RenderTapeFirstAccess` proves one exact surface→texture alias whose first terminal access in the same chunk is an unrestricted target Clear; `validateRenderTape` tracks one unresolved obligation and resolves it once before import, while rejecting unresolved/multiple/cube/multi-mip cases. Provider replay creates the parent texture without an initial mutation and then its texture-derived surface alias, but no captured production bundle or wild replay claim is made. |

---

## 2. Module Structure

The final-slot carrier-free claim is limited to eligible Direct whole-range
replay. Compatibility-only or excluded ranges remain compatibility-owned and
are not included in that claim.

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
    ├── CpuReadyActiveHeadLookahead.tla  active-seeded whole-head hold/restore
    ├── CpuReadyActiveHeadLookahead.cfg
    ├── SessionCapacityLease.tla  fixed lease/headroom and cap-boundary refinement
    ├── SessionCapacityLease.cfg
    ├── ParallelDrawBinding.tla  child-local A→B→A binding/order/join refinement
    ├── ParallelDrawBinding.cfg
    ├── RenderTapeParallelJoin.tla  Clear/FULL_SNAPSHOT DrawRun/child join/Present refinement
    ├── RenderTapeParallelJoin.cfg
    ├── RenderTapeIdentitySegments.tla  bounded SegmentSerial event-group admission/abort/completion
    ├── RenderTapeIdentitySegments.cfg
    ├── PostEncodePayloadRetirement.tla  encode/retire/receipt/completion refinement
    ├── PostEncodePayloadRetirement.cfg
    ├── EncodeSchedulingProgress.tla  composed queue progress and Present obligations
    ├── EncodeSchedulingProgress.cfg
    ├── PipelineLifecycleTable.tla  generated owner/control vocabulary
    ├── CpuPipelineLifecycle.tla  generation-qualified end-to-end owner lifecycle
    ├── CpuPipelineLifecycle.cfg  bounded safety and counterexample baseline
    ├── CpuPipelineLifecycle.missing-wake.counterexample.cfg
    ├── CpuPipelineLifecycle.no-reset-generation.counterexample.cfg
    ├── CpuPipelineLifecycle.present-reclaim.counterexample.cfg
    ├── CpuPipelineLifecycle.fabricated-gpu-milestone.counterexample.cfg
    ├── CpuPipelineLifecycle.progress.cfg  small weak-fair progress configuration
    ├── DceChunkLookahead.tla  one held source → FIFO successor proof window
    ├── DceChunkLookahead.cfg
    ├── WireObjectRegistry.tla  canonical stable object identity
    ├── WireObjectRegistry.cfg
    ├── PeRecorderTransition.tla  PE qualified-state and append settlement
    ├── PeRecorderTransitionTable.tla  generated C++/TLA transition rows
    ├── PeRecorderTransition.cfg
    ├── PeRecorderTransition.counterexample.cfg  expected consume-on-prepare failure
    ├── PeRecorderTransition.consume-witness.counterexample.cfg  expected non-Accepted consumption failure
    ├── PeRecorderTransition.accepted-witness.counterexample.cfg  expected inexact Accepted witness failure
    ├── PeRecorderTransition.prior-pending.counterexample.cfg  expected stale live prior-value pending failure
    ├── PeRecorderTransition.stale.counterexample.cfg  expected stale-token failure
    ├── PeRecorderCommit.tla  bounded PE seal/bridge/capture/drain settlement
    ├── PeRecorderCommitTable.tla  generated commit algebra matrix
    ├── PeRecorderCommit.cfg  MaxOperations=8 positive configuration
    ├── PeRecorderCommit.parent-before-alias.counterexample.cfg  expected ordering failure
    ├── PeRecorderCommit.early-reset.counterexample.cfg  expected early-reset failure
    ├── PeRecorderCommit.stuck-success.counterexample.cfg  expected reuse-liveness failure
    ├── PeRecorderSettlement.tla  composed append/capacity/commit/reset refinement
    ├── PeRecorderSettlement.cfg
    ├── PeRecorderSettlement.bridge-retry.counterexample.cfg
    ├── PeRecorderSettlement.capacity-pre-consume.counterexample.cfg
    ├── PeRecorderSettlement.capture-retract.counterexample.cfg
    ├── PeRecorderSettlement.early-reset.counterexample.cfg
    ├── PeStateBlockTransitionTable.tla  generated StateBlock matrix
    ├── PeStateBlockTransaction.tla  serial/reference/Reset temporal model
    ├── PeStateBlockTransaction.cfg  successful Reset recovery
    ├── PeStateBlockTransaction.no-poison.counterexample.cfg  expected backend-failure poison violation
    ├── PeStateBlockTransaction.no-release.counterexample.cfg  expected reset/teardown staged-release violation
    ├── PeStateBlockTransaction.stale-open.counterexample.cfg  expected stale Recording interval violation
    ├── PeStateBlockTransaction.lost-duplicate.counterexample.cfg  expected duplicate-retain loss
    ├── PeStateBlockTransaction.poison-leak.counterexample.cfg  expected poisoned-owner leak
    ├── PeStateBlockTransaction.stale-capability.counterexample.cfg  expected phase-only capability ABA write
    ├── PeStateBlockValues.tla  repeated qualified Capture/Apply value refinement
    ├── PeStateBlockValues.cfg
    ├── PeStateBlockValues.mutable-tracked.counterexample.cfg
    ├── PeStateBlockValues.failed-capture.counterexample.cfg
    ├── PeStateBlockValues.stale-apply.counterexample.cfg
    ├── ReplayScopedDrain.tla  scoped raw-replay drain ledger
    ├── ReplayScopedDrain.cfg
    ├── PresentIdAba.tla   (slot, generation) tagged-handle ABA-safety
    ├── PresentIdAba.cfg
    ├── ResourceLifetime.tla  Deferred GPU resource destruction plus Initializer ownership
    ├── ResourceLifetime.cfg
    ├── ResourceLifetime.counterexample.cfg  expected bare-destination failure
    ├── BufferBackingVersioning.tla  MANAGED concrete-backing reuse lifetime
    ├── BufferBackingVersioning.cfg
    ├── BufferMutationOffload.tla  Managed unlock byte materialization on the offload worker
    ├── BufferMutationOffload.cfg
    ├── BufferMutationOffload.counterexample.cfg  expected second-queue failure
    ├── BufferMutationOffload.rotation.counterexample.cfg  expected deferred-rotation failure
    ├── QueueT2dReserveCopyCommit.tla  deferred reserve/copy/commit gate
    ├── QueueT2dReserveCopyCommit.cfg  bounded production safety configuration
    ├── QueueT2dReserveCopyCommit.half-appended-slot.counterexample.cfg  expected half-publication failure
    ├── QueueT2dReserveCopyCommit.stale-reservation.counterexample.cfg  expected stale-ticket/generation failure
    ├── QueueT2dReserveCopyCommit.lost-prefix-rollback.counterexample.cfg  expected lost-prefix rollback failure
    ├── MutationComposition.tla  Observer identity/disposition/completion decision gate
    ├── MutationComposition.cfg
    ├── MutationCompositionTable.tla  generated classifier vocabulary
    ├── NoOverwriteByteRange.tla  DEFAULT+DYNAMIC exact NOOVERWRITE range
    ├── NoOverwriteByteRange.cfg
    ├── NoOverwriteByteRange.counterexample.cfg  expected invariant failure
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
| RenderTapeIdentitySegments | `MaxRecords` / `MaxSeqId` / `MaxPassPieces` | 6 / 9 / 3 | one event, three source rows, six records; production ordinals/seqIds and provider grammar unbounded |
| CpuReadySessionProgress | `MaxSources` / `MaxReady` / `MaxResident` / `MaxBatch` / `MaxSessionLen` / `MaxReleaseEvents` / `MaxReleaseGeneration` / `MaxPressureGeneration` | 2 / 2 / 2 / 2 / 2 / 2 / 2 / 2 | unbounded source ordinals; fixed Tape/Ready/session/release capacities and monotone release/latch generations |
| CpuReadyActiveHeadLookahead | fixed sources / adopted-next-raw disposition / `DropActiveSeed` | active source 1, retained head 2, optional exact successor 3 / absent or adopted Direct/non-source / false | production has unbounded ordinals and bounded Tape/session capacity; the companion true configuration is an expected seed-loss failure |
| EncodeSchedulingProgress | `MaxSources` / `MaxSessionLen` / `MaxPresentOutstanding` | 3 / 3 / 1 | unbounded source seqIds; bounded queue/session storage and runtime Present pacing cap |
| DceChunkLookahead | `MaxSources` / `MaxInflight` | 4 / 3 | unbounded source seqIds / `kMaxQueuedChunks`; exactly one held lookahead source |
| WireObjectRegistry | `Slots` / `Requests` / `Kinds` / `MAX_GENERATION` | `{s1,s2}` / `{r1,r2}` / `{Texture,Buffer}` / 3 | device-local stable object IDs, nonzero full `uint32_t` generations, exact kind/ID/generation admission, and retirement at `UINT32_MAX` |
| PresentIdAba | `Slots` / `Entities` / `MAX_GEN` / `MAX_OPS` | `{s1,s2}` / `{p1,p2}` / 3 / 6 | unbounded slots, generation domain = 2^24 (`HandleArena::kGenerationBits`) |
| ResourceLifetime | `Resources` | `{r1,r2}` | dynamic; two resources distinguish independent arena/initializer ownership |
| ResourceLifetime | `MAX_SEQID` | 3 | unbounded; three sequence values cover use, commit/drain, and completion |
| ResourceLifetime | `Implementation` | `Retained` (`Bare` in the expected-failure config) | retained `StagingCopy::destTexture`; the `Bare` branch reproduces the historical escape |
| BufferBackingVersioning | `Backings` | `{b1,b2,b3}` | grow-only per-buffer backing ring |
| BufferBackingVersioning | `MAX_SEQID` | 3 | unbounded |
| BufferMutationOffload | `Backings` | `{b1,b2,b3}` (`{b1,b2}` in both counterexample configs) | same grow-only ring as above; the third entry keeps a two-entry stall out of the state space rather than mistaking it for coverage |
| BufferMutationOffload | `NumChunks` / `NumMutations` | 3 / 2 (2 / 1 in both counterexample configs) | unbounded; chunk → mutation → chunk is the minimum for the FIFO obligation, and the third chunk plus second mutation add the `A -> B -> A` backing reuse |
| BufferMutationOffload | `MAX_ORDINAL` | 6 (4 in both counterexample configs) | unbounded `ReplayOffloadQueue` positions; one more than the items can consume, so exactly one `AbortReserve` can burn a position |
| BufferMutationOffload | `FifoDiscipline` / `RotationDiscipline` | `Enforced` / `Synchronous` (each `Removed` / `Deferred` in its own expected-failure config) | R-BACK-44.3's one queue and R-BACK-44.2 step 2's synchronous rotation |
| QueueT2dReserveCopyCommit | `RequiredParts` / `PublishDiscipline` / `ReservationDiscipline` / `CommitDiscipline` / `RollbackDiscipline` | 2 / `BlockWhileReserved` / `PinGeneration` / `ValidateGeneration` / `RestorePrefix` (counterexamples remove publication blocking, identity validation, or exact-prefix rollback) | deferred-only bounded gate; production append remains mutex-protected and the model does not authorize lock narrowing |
| MutationComposition | `Resources` / `Generations` / `Ordinals` | `{1,2}` / `{1,2}` / `{1,2,3,900}` | bounded exact resource/backing/source identity domain; production identities are wider; `900` distinguishes a replay/source ordinal from the observer ordering ordinal in mixed-path witnesses |
| MutationComposition | `OrderingGeneration` / `OrderingOrdinal` / `MutationSources` | `1` / `{1,2,3}` / `{synchronous,deferred,replay}` | observer-issued generation-qualified ordering identity; ordinals restart only after a generation advance, and cross-generation comparison fails closed |
| MutationComposition | `Decisions` | generated `CompositionDecisions` vocabulary | `scripts/check/gen_mutation_composition_table.py` must match `MutationCompositionTable.tla`; this is a vocabulary freshness check, not semantic relation generation. Native test calls `classifyComposition` directly for legacy value cases and records through the production observer for typed mixed-path binding. |
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
| `AutogenMipGeneration.tla` | D3D9 AUTOGEN public-level/hidden-pyramid ordered-record-to-sample refinement | `AutogenMipmapState` / `transitionAutogenMipmap`, `D3D9DeviceImpl::generateBoundAutogenMipmaps`, `D9C_COMMAND_RECORD_GENERATE_MIPMAPS`, queue `MetalCommandKind::GenerateMipmaps`, PE surface-write dirty transitions, and `TextureRecord::physicalMipLevels` | `QueuedNeverAhead`, `GeneratedNeverAhead`, `AccessibleCleanCoversLatestWrite`, `SampleNeverUsesStaleMip`, `AcceptedRequestHasExactWrite`, `ReadyHasExactGeneration`, `FatalIsTerminal`; append failure preserves Dirty and post-accept failure is fail-stop | `RequestEventuallySettles` under fair queued replay/generate/sample settlement; a failed encode reaches the terminal device state without sampling | `tests/native/core/core_d3d9_generate_mipmaps_spec.cpp`, `tests/native/backend/chunk_record_replay_spec.cpp`, `tests/native/bridge/pe_chunk_record_value_spec.cpp`, `tests/native/backend/wmt_generate_mipmaps_metal_spec.mm`, plus the 3DMark06 HDR1 water-reflection wild oracle |
| `CommandQueue.tla` | 3-thread ring buffer (Wine / Encode / Finish), multi-sub-CB chain | `src/dxmt9/dxmt9_queue.*`, `src/dxmt9/dxmt9_command_queue.*` | `TypeOK`, `SeqIdSafety`, `BoundedInflight`, `RingSafety`, `EncodeSafety`, `SubCBProgressBounded`, `OnlyFinalAdvancesSeqId`, `PresentRoutedToTail` | `PendingEventuallyFree`, `EventuallyDrained` | `tests/native/backend/chunk_record_replay_spec.cpp`, `tests/native/backend/dod_replay_observer_spec.cpp`, `tests/native/backend/encode_session_lifecycle_spec.cpp` (effective replay observer) |
| `QueueLifecycleRefinement.tla` | Concrete refinement of `QueueLifecycleController` staging fields | `QueueLifecycleController` in `src/dxmt9/dxmt9_queue.*` | `TypeOK`, `ReadySlotsArePending`, `PendingCompletionAreSubmitted`, `CompletedSeqQueueBounded`, lifecycle refinement of `CommandQueue!Spec` | `WaitForSequenceProgress`, `StopUnblocksWaits` | `tests/native/backend/dod_replay_observer_spec.cpp` |
| `CpuPipelineOwnership.tla` / `CpuPipelineLifecycle.tla` | Bounded CPU ownership/progress model with FIFO sources plus a concrete queue/Tape batch projection and separate GPU-completion/finish frontiers; full runtime refinement partial | Shared queue predicates and `CpuPipelineOwnership.tla` own source-borrow/admission conservation. `CpuPipelineLifecycle.tla` and the fixed-capacity reducer bind Arena arrival/publication, identity-qualified physical batch members, inline/GPU completion, finish advance, actual reclaim, and pending-completion fail-stop. `CompletionFrontierContiguous` binds the GPU tail to finish waterline plus completed FIFO depth without using observer accounting to alter queue behavior. Generation-qualified synchronous borrows and worker-owned snapshots retain their existing scope. PE import, selected-parallel child/join, receipt detail, and atomic ordering remain outside the concrete projection. | Existing ownership/conservation invariants plus `CompletionFrontierContiguous`; independent mutation configs retain distinct failures. | Existing weak-fairness progress properties | `pipeline_lifecycle_observer_spec.cpp` covers missing-source/stale-batch/frontier/fail-stop counterexamples; `queue_completion_sources_spec.cpp` binds production writer-frontier reclaim, completion, finish, and reclaim behavior; `cpu_ready_production_routing_spec.cpp` binds Direct/Legacy routing |
| `PresentFrameLatency.tla` | Queue-owned frame-latency tokens, present vs non-present timelines | `src/dxmt9/dxmt9_command_queue.*`, `src/dxmt9/dxmt9_presenter.*` | `TypeOK`, `SeqTimelineSafety`, `PresentCompletionSafety`, `OutstandingPresentBound`, `PresentQueueSafety`, `AppWaitReturnSafe` | `SubmittedPresentsEventuallyComplete`, `WaitEventuallyReturnsOrStops` | `tests/native/backend/present_boundary_policy_spec.cpp` |
| `ConcurrentProgressSignals.tla` | Pacing independence across `completedSeqId` / `presentCompletedSeqId` / `ringSlotOccupancy` | `include/dxmt9/progress_predicates.hpp` owns the PE/Unix shared pure projections: `queryGetDataPollSatisfied` is the non-blocking `Query::getData` poll, while `presentTokenWaitSatisfied` and `ringAdmissionWaitSatisfied` are production CV predicates | `TypeOK`, `PacingOrdering` (`presentCompletedSeqId ≤ completedSeqId`), `RingOccupancyBound`, `FrameLatencyBound`, `OutstandingAccounting` | `NoQueryWaitBlocksPresent`, `NoFrameLatencyBlocksQuery`, `NoRingPressureBlocksPresentCompletion` | `tests/native/backend/pipeline_lifecycle_observer_spec.cpp` covers the bounded completion/Present/occupancy projection, query poll, Present-token wait, and ring/admission wake; live unbounded production emission remains a gap |
| `DrawableToken.tla` | `PresentDrawableToken` lifecycle: Stash / Complete / Fail / Take / Wait | `detail::drawableTokenMayFulfill`, `detail::SingleUseTokenSlot`, `PresentDrawableToken`, and `CommandQueue::stash/takeDrawableToken` | `TypeOK`, `NoDoubleComplete`, `NoUseAfterTake`, `StashTakeOrdering`, `DrawableValueShape`, `TakenIsSink`, `FulfilledMonotonic` | `WaitProgress`, `EventuallyResolved` | `tests/native/backend/drawable_token_spec.cpp` pins the production fulfilment predicate, no-overwrite stash, single-use take, complete-before-wait, and wait/fail handoff without sleeps or Metal timing |
| `EncodeSessionCompletion.tla` | One Metal session tail expands into ordered per-source `seqId` completions | `QueueSubmissionRecord::fixedCompletionSources`, `QueueLifecycleController::submitEncodedSubmission`, pending completion drain in `src/dxmt9/dxmt9_queue.*` | `TypeOK`, `CommittedWaterlineOK`, `OrderedCompletionExpansion`, `NoInlineCompletionOfSessionSources`, `PresentCompletionAfterTail` | _(safety/refinement model; no fairness property)_ | `tests/native/backend/queue_completion_sources_spec.cpp` |
| `CpuReadySessionProgress.tla` | Bounded Ready FIFO, one tentative pre-effect prefix, ordered semantic release FIFO, forward-looking terminal fence, session/pass acknowledgement, and completion/reclaim; an additional submitted-Present/Ready-Direct initial branch forces the capacity-wait trace without replacing the original empty-start state space, and pressure-created release actions are unreachable | `CpuReadyTape` in `src/dxmt9/dxmt9_cpu_ready_tape.hpp`; tentative queue seam, capacity-progress generation, and source metadata in `QueueLifecycleController` (`src/dxmt9/dxmt9_queue.*`); when Tape is selected, `CommandQueue::cpuReadySessionCoordinatorSelected` gives the session coordinator priority over uncomposed DCE lookahead, and `beginFinalWsiQuiescence` binds `SessionReleaseState::requestTerminal` to the final writer fence, coordinator acknowledgement, sequence completion, and Tape reclaim; the idle coordinator wait delegates its terminal wake to the same `sessionReleaseFenceCovered` predicate used by acknowledgement | `TypeOK`, `BoundedStores`, `FifoSourceOrder`, `OneTentativePrefix`, `CompatibleSuffixStaysReady`, `NoYoungerThanEarliestFence`, `RollbackBeforeEffectsRestoresExactPrefix`, `ReleaseQueueOrdered`, `AckAfterRequiredActionAndFenceCoverage`, `NoCompletionBeforeSubmit`, `NoPressureCreatedRelease`, `CapacityWakeMatchesReclaim`, `TerminalFenceSafety` | `OrdinaryReleaseProgress`, `StartupCapacityWakeProgress`, `StartupDirectProgress`, `ShutdownProgress`; GPU settlement remains an explicit environment action and weak fairness applies only to enabled coordinator/completion/finish actions | `tests/native/backend/cpu_ready_tape_spec.cpp`, `tests/native/backend/queue_completion_sources_spec.cpp`, `tests/native/backend/cpu_ready_session_join_spec.cpp` (idle covered-terminal wake, open session + Ready suffix + Present terminal binding, and Tape+DCE fail-closed selection matrix) |
| `CpuReadyActiveHeadLookahead.tla` | One active-render seed, one held Ready head, an absent or already-adopted next raw, and Direct/non-source disposition | `ReplayOffloadQueue::pop` immediate-raw hint, publication-time intent advance, cancellation-aware wake predicate, whole-head tentative reserve/restore, and `encodeChunkSessionReplayFrontierState` | `IntentRequiresAdoptedRaw`, `EmptyFifoNeverPromises`, `HeldOwnsNoNewEffects`, `RestorePreservesFifo`, `ActiveSeedPreserved`, `ExactlyOnce`; the seedless companion must fail | `EmptyFifoProgress`, `AdoptedRawProgress`, `HeldExitProgress`, `RestoredProgress`; an adopted FIFO raw is worker-owned rather than an unconstrained arrival | `tests/native/backend/replay_offload_queue_spec.cpp` pins empty/adjacent/control hint shape; `cpu_ready_session_join_spec.cpp` pins exact successor, cancellation wake, stop restore, active-seeded planning, and FIFO completion |
| `PostEncodePayloadRetirement.tla` | Eligible payload retirement after synchronous encode while work remains unsubmitted, with a bounded source-kind-neutral receipt ledger and mixed legacy/retired completion | `PostEncodeCompletionLedger`, `QueueCompletionSource`, `CpuReadyTape` two-phase retirement, and submission/completion handling in `src/dxmt9/dxmt9_queue.*` | `TypeOK`, `RetirementRequiresEncode`, `TwoPhasePageRelease`, `LiveReceiptGenerationMatches`, `ReleasedReceiptIsStale`, `CompletionExactlyOnce`, `DeviceLossSettlesExactlyOnce`, `ResourcesSurviveCompletion`, `OrderedWaterlines`, `PresentNeverRetires`, `GpuAccountingIndependentOfResidency`, `BoundedReceipts` | _(safety/refinement model; no fairness property)_ | `tests/native/backend/post_encode_payload_retirement_spec.cpp`, `tests/native/backend/queue_completion_sources_spec.cpp`, `tests/native/backend/cpu_ready_tape_spec.cpp`, `tests/native/backend/cpu_ready_session_join_spec.cpp` |
| `EncodeSchedulingProgress.tla` | Bounded composed scheduling pipeline with admission escapes followed by an exact post-admission producer fence | `classifyFirstLeaseCapacityWait` in `render/encode_scheduling_progress.hpp`, `SchedulingProgressWatchdog`, `QueueLifecycleController`, and `CommandQueue` | Distinct admission/producer actions preserve FIFO ownership, exact identity/target coverage, standalone execution, and no release/session/capacity transition | Admission progress, changed-head rearm, `PostAdmissionCaptureBoundaryProgress`, and `CaptureBoundaryReturnsAfterFence`; GPU settlement remains environmental | Pure action/identity/target truth tables plus the real 31-source, seven-admission and 23-producer-fence CV composition |
| `DceChunkLookahead.tla` | One held FrameGraph source optionally owns an encoded prefix, selects an already-ready immediate FIFO successor, or fails open without waiting | `CommandQueue::runDceChunkLookaheadEncodeLoop`, `resolveDceChunkLookaheadAction`, `FrameGraphBackend::onChunkReady` | `TypeOK`, `SubmittedPrefix`, `HeldIsNext`, `ReadyIsFollowingPrefix`, `PublishedPartition`, `BoundedHold`, `PrefixOwnedByHeld`, `CompletionPrefix` | _(safety/refinement model; no-ready is an immediate fail-open action)_ | `tests/native/backend/queue_completion_sources_spec.cpp`, `tests/native/framegraph/fg_optimizer_spec.cpp`, `tests/native/framegraph/fg_linearizer_spec.cpp` |
| Bounded ready-prefix DCE (missing) | Already-ready FIFO prefix summaries shared without DCE source ownership (`R-VERIF-2.13`) | planned ready-prefix owner and FrameGraph summary consumer | consecutive bounded snapshot, independent DAG/completion, conservative stop | immediate no-proof release | missing summary native spec |
| `SessionCapacityLease.tla` (`R-BACK-2.65` refinement) | Fixed encoded-work cap plus separate physical-residency vector and complete wrap-aware ordinary-successor reserve; an explicit full-residency Writing startup credits the unique successor once, eligible encoded heads may retire residency without reducing work, the deterministic work-cap candidate remains Ready, and exact predecessor submission is independent of GPU completion | `SessionCapacityLeaseState`, the typed `CpuReadyTape` first-acquisition snapshot, the queue-owned capacity-progress generation, and the command-queue session coordinator | `TypeOK`, `BoundedCapacity`, `LeaseOwnsCompleteHeadroom`, `WritingSuccessorIsUnique`, `CapCandidateStaysReady`, `NoPressureCreatedRelease`, `CapacityWakeMatchesProgress`, `SubmittedGroupsRespectCap`, `ResidencyIsSeparateFromWork` | `CapProgress`, `StartupCapacityWakeProgress`, `StartupDirectLeaseProgress`, `WritingSuccessorStartupProgress`; Writing startup reaches acquire/admit/retire/publish without completion or pressure release, while older submitted residency still waits for reclaim | `tests/native/backend/encode_session_admission_spec.cpp`, `tests/native/backend/cpu_ready_tape_spec.cpp`, `tests/native/backend/cpu_ready_session_join_spec.cpp`, `tests/native/backend/post_encode_payload_retirement_spec.cpp` |
| `ParallelDrawBinding.tla` | Source-local parallel child binding/order/join refinement (`R-BACK-2.63`, source-local slice of `R-VERIF-2.15`) | `planDrawBindingTransition` / `applyDrawBindingTransition`, `BindingState::lastDrawBindingPayloadIdentity`, pass-wide pipeline-handle ABI preflight, and production child execution in `src/dxmt9/dxmt9_draw_encoder_chunk.mm` | `DrawUsesRequiredUniformGeneration`, `PsoBindingAbiMatchesChildBinding`, one Stage 1-or-Stage 2b ABI per pass, `ChildBindingShadowsAreIsolated`, `DrawsExecuteExactlyOnceInSerialOrder`, `AllChildrenEndBeforeParent`, `CompletionAfterJoinedParent` | `CreatedChildEventuallyJoinedOrFallback`, `ParentAndCompletionProgress` | `tests/native/core/draw_uniforms_dirty_spec.cpp`, `tests/native/backend/parallel_render_pass_spec.cpp`, `tests/native/backend/parallel_draw_binding_metal_spec.mm` (Stage 2b slots 0/3, two concurrently encoded Metal children, additive A→B→A, 100 serial/parallel byte comparisons plus stale-transition negative control) |
| `DrawPsoIdentity.tla` | Backend PSO identity canonicalization (`R-BACK-3.12`, `R-BACK-3.13`, `R-BACK-3.15`) | `detail::makeBackendShaderIdentityHash`, `detail::canonicalizeBlendAttachmentKey`, and `makeShaderVariantKey` in `src/dxmt9/dxmt9_pipeline_cache.*` | Portable FFP alpha-test omission, tile-FFP alpha-test retention, general-draw filtering omission versus stretch retention, inactive/invalid blend-entry canonicalization | _(finite value model; no scheduling liveness)_ | `tests/native/backend/backend_pipeline_key_spec.cpp` exhaustive bounded truth tables; Metal shader ABI and final-pixel behavior remain covered by the separate shader/GPU-oracle gates |
| `PsoSlotPublication.tla` | Append-only segmented draw-PSO handle publication (`R-BACK-3.14`) | `detail::SegmentedImmutableSlotTable<PsoSlot>` and `Cache::{drawPipelineForHandle,drawPipelineKeyForHandle}` in `src/dxmt9/dxmt9_segmented_slot_table.hpp` / `dxmt9_pipeline_cache.*` | `TypeOK`, `AppendOnly`, `PublishedValuesValid`, `LookupFailClosed`, `NoUnpublishedValue`; the mutated config permits a staged read and must violate `LookupFailClosed` | _(finite safety model; one serialized writer and concurrent acquire readers, no scheduling liveness)_ | `tests/native/backend/pso_slot_publication_spec.cpp` pins block boundaries, full 16-bit capacity/exhaustion, immutable generation-qualified lookup, and one-writer/four-reader publication |
| `ParallelPolicySelection.tla` | Bounded safe-only candidate selection/refinement (`R-BACK-2.68`–`2.75`, `R-VERIF-2.16`–`2.22`) | `validateParallelPassSemanticPlan` / `selectParallelPassCandidate` proof-core predicates and no production call site; native bounded adversarial value tests own exact coverage/arithmetic/selection equivalence | `TypeOK`, `SelectionIsSafe`, `SelectionIsArgmax`, `NoEffectBeforeSelection`, `SerialFallbackHasNoParallelEffect`, `SelectedProofOnlyEffect`, `JoinParentCompletion` | `EventuallySettles`, `SelectedEventuallySettles`, and `SerialEventuallySettles` under weak fairness; bounded invalid-batch serial fallback, benefit-zero skip, selected effect, join, parent end, completion | `tests/native/backend/parallel_render_pass_spec.cpp` |
| `CpuReadySemanticTransfer.tla` | Small Unix-side RawCommandChunk → CPU-ready Tape ownership refinement: Reserve → Adopt → Emit → Publish/Abort, post-effect fail-stop, source identity, and exactly-once return/release | `CpuReadySemanticTransfer` owns a move-only `RawCommandChunk` plus `CpuReadyArenaBuildLease` during direct-Arena replay; the model starts after Reserve, when the transfer already owns the lease. Recoverable pre-effect Abort is batch-only; a single-source abort remains terminal fail-stop. The source is restored only after settlement, without changing the PE↔Unix ABI or claiming copy removal. Native transfer coverage exercises real Tape reserve, semantic emit, publish/reclaim, batch fallback, single-source terminal abort, post-effect publication failure, and identity conservation. | `TypeOK`, `ReserveBeforeAdopt`, `AdoptBeforeEmit`, `NoPartialReady`, `NoDoubleRelease`, `TerminalRelease` | `EventuallySettledOrFailStopped` requires every nonterminal transfer stage to reach a terminal stage under weak fairness; queue progress remains owned by `CpuPipelineOwnership`/`CpuPipelineLifecycle` | `tests/native/backend/cpu_ready_production_routing_spec.cpp` |
| `DirectChunkSlotContinuation.tla` | Small populated-slot continuation transaction: strict structural/capacity/**exact identity-adjacency** admission for DrawRun plus state-only APPLY_STATE/constants, private staged append, exact pre-effect rollback, commit, and post-effect fail-stop without retry | `core::directContinuationAdmission` and `core::compatibilityProducerIdentityAppendable` are the shared typed gates used by `CommandQueue::beginDirectChunkSlotReplay`; a populated slot remains on the ordinary Direct lane only when final vector/arena and lookup capacity is already present **and the candidate first event/source ordinals equal the current last ordinals plus one**. A valid gap, overlap, or reordering fails closed to Legacy before effects; invalid/overflowing identities are rejected by the same pre-effect import/predicate boundary. Clear, Present, ordered control/readback, resource mutation, UP, `TriangleFan`, and malformed shapes remain excluded. | `TypeOK`, `AdmissionSound`, `NonAppendableIdentityCannotEnterDirect`, `LegacyBeforeEffects`, `PrefixPreservedOnRollback`, `CommitRequiresEffects`, `NoRetryAfterPostEffectFailure`, `NoHalfPublishedPrefix`, `StagedCandidateIsPrivate` | Bounded safety model; native truth-table and populated-slot replay pin the same structural/capacity/identity admission, pre-effect rollback, commit, and post-effect fail-stop/no-retry contract. It does not prove Metal pixels, unbounded command grammar, or wild workload behavior. | `tests/native/backend/cpu_ready_production_routing_spec.cpp`, `src/dxmt9/dxmt9_direct_continuation.hpp`, `src/dxmt9/dxmt9_cpu_ready_tape.hpp` |
| `SegmentedTransportV1.tla` | Fixed-role three-stage semantic-batch transport: pure exact count/unique-handle proof, complete reservation, atomic adoption, contiguous ExactFixed CPU Tape emission, fallback/poison, FIFO settlement, and reclaim wake | Opt-in PE/WoW64/Unix bridge validates three borrowed roles and adopts one aligned pooled move-only Unix extent with exact cross-thread ledger settlement. The production recorder now has one mandatory all-family chunk settlement transaction: its immutable owner emits negotiated segmented roles for normal bridge transport and one ExactFixed V2 extent for capture/compatibility; compatibility builders are test/bootstrap oracles only. Production CPU-ready Tape role integration is now represented by the separate bounded `CpuReadySemanticTransfer` seam; the three-role model still does not claim pooled allocator ownership or copy removal. | Existing transport invariants unchanged | Existing weak-fairness settlement properties | `dxmt9-pe-semantic-batch-spec`, `dxmt9-replay-offload-queue-spec`, segmented validator/registry tests, ABI codegen tests, and PE32/PE64/Unix layout assertions |
| `RenderTapeIdentitySegments.tla` | Bounded SegmentSerial event-group admission, exact six-record partition, pass-piece edge continuity, atomic publish/abort, flattened completion, settlement, watermarks, reclaim, and pre-effect fallback | `CpuReadyTape::reserveArenaBatch` / `beginArenaAbort` / `finishArenaAbort` / `restoreArenaBatchHighWaters`; production identity-ledger exact same-event run/tail registration; `InvalidPlanEventSerial` models plan-validator rejection before reservation while `PreEffectPassMismatchFallback` models a post-reservation abort | `TypeOK`, `RecordPartition`, `PassPieceContinuity`, `FlattenedCompletion`, `AtomicReadyPublication`, `TwoPhaseAbortOrder`, `SettlementExact`, `WatermarkSafety`, `FallbackBeforeEffects`, `FailStopSafety` | `EventuallyTerminal`, `EventuallySettledOrReclaimed` under weak fairness; GPU completion remains environmental | `tests/native/backend/cpu_ready_plan_spec.cpp` plan-validator negatives, `tests/native/backend/cpu_ready_tape_spec.cpp` (`batchAbortDetachesSuffixBeforeReverseFinish`), `tests/native/bridge/render_tape_capture_spec.cpp` exact settlement truth table |
| `PeRecorderTransition.tla` | PE qualified state writes, fixed state-block tracking, live prior-value replacement of an existing pending result, pre-effect recorder failure, and prepare/accept/fail/discard append settlement; post-effect Apply failure is explicitly outside the model | `planRecorderStateWrite`, `settleRecorderAppend`, `acceptPreparedSparseState`, the four normalized keyed setters, six constant adapters, and PE state-block wrapper snapshot; the state/append rows come from generated `PeRecorderTransitionTable.tla` and are freshness-checked before TLC | `TypeOK`, `PreparedIsNonConsuming`, distinct `OnlyAcceptedConsumes`/`AcceptedExactlyRepresented`, `FailedRetryStable`, per-key token-qualified `NoLostPending`, `DurableTokenMatchesPayload`, `PendingMatchesLive`, and qualified-key/fixed-set/prior-value invariants; five independent mutations must violate their named invariants | `PreparedEventuallySettles`, `AcceptedEventuallyReplayed`, `InputStopEventuallySettledOrRetained` under weak fairness | `tests/native/bridge/pe_transition_algebra_spec.cpp`, `pe_shadow_native_spec.cpp`, `pe_producer_differential_spec.cpp`, `tests/native/core/core_stateblock_restore_spec.cpp` |
| `PeRecorderScalarProjection.tla` | Exact bounded scalar cross-projection for render-state/TSS/sampler PendingDelta values: source ordinal → one accepted record ordinal, retry/discard conservation, and explicit no-token behavior | Default production settlement owns canonical order plus exact category/key/index/value; the default-off `DXMT9_PE_SCALAR_SEMANTIC_OBSERVER` adds `PeScalarSemanticTokenLedger` setter-source and builder-record ordinals without enlarging `PeRecorderState` | `TypeOK`, `ExactProjection`, no duplicate accepted tuple, and no-token preservation; missing/duplicate/value/ordinal mutations are deliberate expected failures | _(finite safety model)_ | `tests/native/bridge/pe_producer_views_spec.cpp`, `pe_semantic_projection_spec.cpp`; this remains the scalar-only refinement. `check_pe_scalar_projection_model.py` is a regex structural freshness audit, not semantic model-code refinement; the sibling all-family model owns other records. |
| `PeRecorderSemanticProjection.tla` | All 21 production PE record families: source ordinal before CapacityPre; exact accepted record ordinal, byte range, payload and qualified identities; retry, effect-unknown poison, and three capture dispositions | `DXMT9_PE_SEMANTIC_PRODUCER_TABLE`, `planCommittedPeSemanticProjection`, the common `D3D9DeviceImpl::appendRecord` envelope, `PeSemanticBatchOwner::emitExactFixed`, `observeCommittedSemanticOwnerRecord`, and the cold `PeAllFamilySemanticTokenLedger` | `TypeOK`, `ExactProjection`, `RetryStable`, `CaptureAfterAccept`, `BridgeEffectUnknownFailStop`; ten independent expected-failure configs remove source, record, range, value, identity, A/B/A, wrong-producer, wrong-source, fail-stop, or capture conservation | _(finite safety model)_ | `tests/native/bridge/pe_semantic_projection_spec.cpp` exhausts every producer and exact-field combination, immutable oracle/owner facts, full identities, retry, poison, capture, and discard. `dxmt9-pe-diagnostics-spec` pins the production source binding and rejects a builder-backed observer route. `gen_pe_semantic_producer_table.py --check` rejects model/table drift. PE/Wine faults remain open. |
| `RenderTapeParallelJoin.tla` | Bounded Render Tape identity/ExplicitParallel refinement for one Clear, a `FULL_SNAPSHOT` DrawRun, two child partitions, joined completion, and Present | `device_c_render_tape_provider.*` partition-mode replay and child join; value-level contract for the production provider | `TypeOK`, `SelectionAndPartitionNonVacuous`, `IdentitySerialOrder`, `ChildWorkIsOrderedAndOwned`, `ChildrenJoinInOrder`, `ParallelOutputPreservesSerialOrder`, `SnapshotPrecedesDraw`, `ExactIdentityRefinement` | `ClearProgress`, `SnapshotProgress`, `IdentityProgress`, `WorkerWorkProgress`, `JoinProgress`, `PresentProgress` | Provider CLI `parallel-verify` supplies the fresh-process identity/ExplicitParallel oracle; native provider and Metal tests remain the concrete binding layer |
| `WireObjectRegistry.tla` | PE → unix canonical stable identity and slot reuse | `src/d3d9/d3d9_pe_chunk_builder.*`, `src/d3d9/device_c_chunk_registry.*`, `src/d3d9/device_c_chunk_validate.*`, `include/dxmt9/device_c.h` | `TypeOK`, `NoZombieAccept`, `KindStable`, `NoReuseWithoutGenerationAdvance`, `NoGenerationWrap` | _(safety model; no fairness property)_ | `tests/native/bridge/chunk_record_registry_spec.cpp`, `tests/native/bridge/chunk_record_validation_spec.cpp` |
| `PresentIdAba.tla` | (slot, generation) tagged-handle ABA-safety — `HandleArena` today, forward-looking `PresenterSlot` registry | `src/dxmt9/dxmt9_resource_pool.hpp` (`detail::HandleArena<R,K>`, `encode`, `find`, `releaseSlot`) | `TypeOK`, `StaleResolvesNull`, `NoCrossSlotAlias`, `GenerationOverflowDocumented`, `GenerationMonotone` | `EventualReclaim` | `tests/native/backend/resource_lifetime_spec.cpp` directly forces LIFO slot reuse, generation advance, stale/wrong-kind rejection, and sibling-slot preservation; bridge registry coverage remains complementary |
| `ResourceLifetime.tla` | Deferred chunk-watermark destruction plus pending/in-flight Initializer ownership and eventual reference release | `resources::canReclaimRecord`, `resources::lifetime::pendingInitializerReferenceSafe`, `HandleArena::reclaimCompleted`, `Pool::stageTextureUpload`, and `Initializer::{enqueuePendingUploadUnlocked,flushToWaitUnlocked}` | `TypeOK`, `NoUseAfterFree`, `PrematureFreeImpossible`, `InitializerReferenceSafety`, `MetalReleaseAfterAllOwners`; the `Bare` config must violate `NoUseAfterFree` | `DestroyPendingEventuallyFreed`, `InitializerEventuallySettled`, `FreedEventuallyMetalReleased` | `tests/native/backend/resource_lifetime_spec.cpp` exhausts 22 production-predicate tuples and directly pins HandleArena reclamation/ABA behavior |
| `BufferBackingVersioning.tla` | MANAGED CPU-shadow upload selects a safe concrete backing and draw packets pin it by sequence | `BufferRecord::renameRing`, `Pool::uploadBufferData`, `Pool::snapshotBufferBinding`, `Pool::markBufferSnapshotUse` | `TypeOK`, `ActiveBackingAllocated`, `LogicalWatermarkCoversEveryBacking`, `NoUploadOverwriteInFlight`, `NoBackingFreedInFlight`, `DestroyPendingCannotFreeEarly` | `DestroyPendingEventuallyFreed` | `tests/native/backend/dynamic_rename_ring_spec.cpp`, `tests/native/core/core_device_com_spec.cpp`, `tests/native/bridge/pe_buffer_lock_order_spec.cpp` |
| `BufferMutationOffload.tla` | Managed plain writable unlock defers only its BYTE MATERIALIZATION to the commit-replay offload worker: producer reserve/rotate/commit, one FIFO shared by chunk replay and mutation application, commit-time snapshot capture, and an encode-side byte read deliberately left unordered against other chunks (R-BACK-44.2/44.2a/44.3/44.4/44.4a) | shared `resources::mutation_offload::{admitsManagedMutationOffload,fifoOrdinalPrecedes,mutationBlocksChunkReplay,chunkBlocksMutationApply,captureRevisionIsCurrent}` in `src/dxmt9/dxmt9_mutation_offload_predicates.hpp`; the mode is designed only, so the planned owners `Buffer::unlock`, `Pool::captureChunkBufferBinding`, and the `ReplayOffloadQueue` drain loop do not call them yet | `TypeOK`, `EncodeReadsAppliedBytes`, `SnapshotRevisionIsCurrent`, `NoLiveCaptureOverwritten`, `FifoApplicationOrder`, `OrdinalsAreUnique`, `PublishedRevisionCount`, `RecordRevisionCountsRotations`, `TaskCarriesRotatedTarget`; the `FifoDiscipline="Removed"` config must violate `EncodeReadsAppliedBytes` and the `RotationDiscipline="Deferred"` config must violate `SnapshotRevisionIsCurrent` | _(safety model; action properties `RecordRevisionMonotonic`, `LogicalRevisionMonotonic`, `OrdinalMonotonic` only — no fairness property)_ | `tests/native/backend/buffer_mutation_offload_spec.cpp` pins the predicate truth tables and replays both TLC counterexample traces verbatim. Backing SELECTION safety is not re-proved here: the model imports `BufferBackingVersioning.tla`'s reuse rule as a deliberately weaker encode-scoped gate, and weakens neither `NoUploadOverwriteInFlight` nor `NoBackingFreedInFlight`. The R-BACK-44.5 direct-reader fence stays with `ReplayScopedDrain.tla`. |
| `QueueT2dReserveCopyCommit.tla` (retired-carrier historical gate) | Bounded reserve/copy/commit decision model for one queue writing slot: pre-existing prefix checkpoint, private reservation bytes, capacity charge, frozen generation/ticket, complete prefix-plus-reservation publication, rollback/wake, and recycle/commit hazards | The modeled `submitDrawRunBatchImpl` carrier append is retired. Ordinary final-slot append remains under `CommandQueue::mutex_`; no unlocked production transition is enabled. | `TypeOK`, `CapacityConserved`, `NoHalfAppendedSlot`, `NoStaleReservationWrite`, `CompletePublication`, `ReservationIsolation`, `RollbackRestoresCharge`, `RollbackWakesWaiters`, `RollbackRestoresPrefix`; production config passes, while `.half-appended-slot.counterexample.cfg`, `.stale-reservation.counterexample.cfg`, and `.lost-prefix-rollback.counterexample.cfg` fail their named invariants | _(bounded historical safety/preflight model; no fairness claim)_ | Retained to keep the rejected unlocked-append counterexamples executable. No production concurrency implementation is claimed. |
| `ReplayScopedDrain.tla` | Core-buffer-identity commit-replay ledger shared by wrapper aliases, scoped/global terminal wakeups, failure-before-completion publication, pre-rename backing capture, and queued plus in-flight raw-entry residency | `ReplayDrainLedger`, `ReplayOffloadQueue`, the rename ring, and raw canonical admission/replay in `src/d3d9/device_c_*` / `src/dxmt9/dxmt9_resource_pool.*` | `TypeOK`, `ReplayedLeQueued`, `NoRejectedWatermark`, `ScopedReturnSafe`, `GlobalReturnSafe`, `StopUnblocksWithoutSuccess`, `QueuedChunkUsesCapturedGeneration`, `OneGenerationPerRawChunk`, `FailedNeverAcknowledged`, `RawEntryImmutable`, `RawResidencyMatchesOutstanding`, `InFlightRetainsResidency`, `TerminalPrecedesCompletion`, `FailureBlocksAdmission`, `InlineTraceEquivalent`, `UnrelatedResourceDoesNotBlock` | `ScopedWaitEventuallyReturnsOrStopsOrPoisons`, `GlobalWaitEventuallyReturnsOrStopsOrPoisons` | `tests/native/backend/replay_offload_queue_spec.cpp`, `tests/native/backend/dynamic_rename_ring_spec.cpp`, `tests/native/backend/replay_byte_identity_spec.cpp`, `tests/native/bridge/pe_buffer_lock_order_spec.cpp`, `tests/native/core/core_device_com_spec.cpp` |
| `EncoderLifecycle.tla` | `MTLCommandEncoder` mutual exclusion + exact hazard sets + Bloom-as-diagnostic | `src/dxmt9/dxmt9_draw_encoder.*`, blit/readback encoder helpers | `TypeOK`, `KindSwitchThroughIdle`, `RenderTargetConsistency`, `ExactHazardBlocksMerge`, `BloomNeverForcesSplit` | `ActiveEncoderEventuallyEnds` | `tests/native/backend/resource_hazard_spec.cpp` |
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

- **`WireObjectRegistry.tla`** — formalises the cross-side canonical stable-
  identity invariant on `D9CCommandChunkWireHandleEntry`. Without it, a stale
  handle could survive PE-side recording and alias onto a freshly recycled
  registry slot at unix-side commit time. The model proves exact live
  kind/object-ID/generation admission, generation advance before reuse, and
  permanent slot retirement at the maximum full-`uint32_t` generation.

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

The all-family PE semantic projection binds an accepted fact to the exact
latest `beginSource` issuance, including producer/record family and issued
source ordinal. The `Aba` model mode and its expected-failure configuration
cover A/B/A, while the native projection spec covers A/B/A, wrong-producer,
wrong-source, and retry re-issuance; structural payload/range validity alone
is intentionally not treated as semantic binding. Production acceptance then
consumes that issuance; native coverage rejects a second accept without a new
`beginSource`, while pre-effect retry preserves and reissues the same witness.
Together with the original
seven field/settlement controls, these are ten independent semantic
counterexamples.

The DrawableToken and HandleArena rows now have deterministic production-
predicate/owner pins. One TLA-only row remains and is tracked in
`specs/verification/gap.md`:

- `ConcurrentProgressSignals.tla` — cross-axis non-blocking is a
  queue-observation property, not a pure-data transform. A fake-backend
  observer covering all three axes simultaneously would be useful but
  does not exist today.

`DrawableToken.tla` covers one stash/take/fulfil/wait lifecycle for each of
two present IDs; `Taken` is terminal in that finite model. The native spec pins
that same single-token production transition. Repeated or overlapping Present
lifecycles through one Presenter slot are outside this model and test, rather
than being inferred from the exact-once result.

The ResourceLifetime model now distinguishes arena-record release from Metal-
object deallocation and carries an independent Initializer owner through
`Pending -> InFlight -> None`, including pre-submit abort and success/device-
loss settlement after submission. The 22-case native truth table executes the
shared production reclaim/reference predicates and the expected-failure TLC config
reproduces the old bare-destination escape. This evidence does not prove
Objective-C reference-count implementation or Metal command-buffer retention;
those platform mechanisms remain assumptions at the abstraction boundary.

### 7.3 Effective replay observer

`ReplayObserverSink` is copied from `CommandQueue` into `EncodeContext`. One
cached null-function gate per `encodeChunk` effective stream or fragment
selects either a no-op instantiation or enabled observer storage before replay
begins. Serial replay resolves and publishes one original source-qualified
command after range validation, DCE/permutation, partition selection, and
fallback checks, immediately before that selected command's encoder effects.
A selected parallel batch publishes its covered commands in effective order
after every proof/economics/fallback gate and immediately before the first
child encoder effect. There is no source-wide observation pre-pass. The no-op
instantiation constructs no observer storage and performs no resource-visitor
work.

`visitSourceCommandResources` is the shared policy for queue marking and
observation. It conserves flat Draw state, per-draw binding overrides and
backing snapshots, and every non-draw endpoint. Snapshot rows preserve
`markBufferSnapshotUse` in the production marker. The observer projects exact
resource handles into growable call-local enabled-only storage, deduplicates
without a fixed capacity, and exposes the resulting span only for the
synchronous callback. No callback executes while the queue scheduling lock is
held and no span is retained in queue, session, or submission state.

`encode_session_lifecycle_spec.cpp` drives the real `encodeChunk` entry with
Legacy and Arena storage. It pins mixed Draw/Clear/Copy/Readback/Present
metadata and resources, DCE omission, permutation order, explicit DrawRun
subranges, pre-registered source fragments, original ordinals, exact callback
counts, and zero callbacks for an invalid range on the selected serial path.
The production selected-parallel batch seam is implemented, but the lifecycle
fixture's test-only side-effect suppression intentionally bypasses
`tryEncodeParallelPass`; no selected fake/recorded parallel batch currently
pins callback timing after the proof gates. R-VERIF-7.3 therefore remains
partial on that bounded parallel binding. The independent
`ConcurrentProgressSignals` cross-axis queue-observation gap in §7.2 also
remains open.

### 7.4 Direct-construction refinement and lifecycle observer

This is the evidence owner for R-VERIF-2.23 and R-VERIF-7.6–7.10; architecture
owns the stage vocabulary, the recorder owns PE semantic tokens and wire bytes,
and encode scheduling owns the concrete assembler.

> Current lifecycle closure (2026-08-30): the bounded CPU model and native
> observer now distinguish `Completed` from `Reclaimed`. Queue completion uses
> `GPUInFlight -> Completed -> Reclaimed`; inline/state-only terminal work uses
> an owner-qualified `NoGpuTerminal` disposition and does not synthesize GPU
> milestones. PE/import, receipt, selected-parallel, and device-loss owner
> labels are covered by deterministic native truth tables; asynchronous GPU,
> Wine, wild, and promotion evidence remain open.

The planned formal bundle has three bounded modules or equivalent composed
relations:

| Relation | State carried | Required counterexamples |
|---|---|---|
| PE final-wire transaction | reserved/used region prefixes, retain and settlement tokens, commit/rollback, bridge effect disposition | partial publish, leaked retain, retry after effect, byte/token loss |
| ChunkSlot assembly | final-region capacities, constructed-owner prefixes, source/control/page credit, publish flag | publish before build, incomplete reverse rollback, duplicate final owner |
| pipeline ownership progress | exact identity/generation, abstract stage, borrow count, receipt/completion authority, resource and payload reclaim | borrow escape, stage skip/regress, premature reclaim, missing completion, fabricated GPU stage |

The composition maps concrete actions to the architecture stages. Planning and
representation-local bookkeeping may stutter. `ReplayBorrowed` and `Encoding`
require borrow count one or a bounded child count whose join returns to zero
before transition. Receipt-backed early payload retirement releases payload
storage while the abstract identity remains `GPUInFlight`; resource waterlines,
query/frame tokens, callbacks, and completion credit remain live. A state-only
source takes an explicit no-GPU terminal row, and a pre-effect rollback returns
to its exact prior owner without creating a completion.

The native lifecycle observer consumes events from production queue owners and
now has an isomorphic bounded completion/reclaim split. `CpuPipelineOwnership.tla`
models `GPUInFlight -> Completed -> Reclaimed`, while the production inline
path emits `Encoding -> Reclaimed` with an explicit no-GPU disposition instead
of fabricated GPU milestones.
`pipeline_lifecycle_observer_spec` uses `FakePipelineQueue` for its exhaustive
reducer and missing-wake/partial-publication counterexamples, while
`cpu_ready_production_routing_spec` drives real queue admission, publication,
dequeue, completion, and reclaim owners with deterministic failure pins.
R-VERIF-7.3 remains the effective-command oracle and is joined by
raw/source identity and ordinal in the same-raw Direct/Legacy fixture rather
than expanding into a second ownership authority; GPU, Wine, and wild layers
remain promotion gates.

R-VERIF-7.9 extends this bundle across the previously abstract PE/import cut.
The composed trace now uses the architecture's `EndToEndSourceIdentity` as its
join key. A closed PE event/source interval crosses contiguous or segmented
import and is preserved by Raw ownership, Arena or compatibility publication,
the storage-generation sidecar, serial/selected-child encode, completion, and
reclaim. Direct continuation widens the interval only for adjacent events and
sources, so physical coalescing does not erase logical provenance. The model
and native truth tables reject partial identity and reuse by another raw;
SegmentSerial may project one raw interval to several queue-local sources.
Physical representation, lease/borrow count, effect cut, completion authority,
and capacity wake remain bound by the existing owner transitions. Broader
resolved-sidecar contents, C++ atomic ordering, Wine/GPU output, and promotion
evidence remain layered requirements rather than missing identity joins. The
current import specialization establishes Unix `RawOwned` storage before the
bridge returns. The bounded trace still needs explicit expected failures for a
retained PE span and for a same-address alias that lacks atomic reclaim-authority
transfer; address equality alone is not a valid `SourceLease` transition.

| Architecture term | Production specialization | Evidence owner |
|---|---|---|
| `ImmutableSemanticSource` | `PeSemanticBatchOwner` output plus authenticated `RawOwned`/Arena representation | recorder all-family projection, bridge/import differential |
| `SourceLease` | queue generation/residency authority and `GenerationQualifiedSourceBorrow` | typed-borrow native tests and lifecycle model |
| `SynchronousSourceFacade` | `SynchronousSourcePayloadBorrow` and call-local `SourcePayloadView` | stale/escape/reclaim-with-borrow counterexamples |
| `ResolvedSourceSidecar` | resource closure, hazard/pass/binding snapshots qualified by source generation | direct/Legacy differential and renderer locator tests |
| `CompletionProjection` | receipt, source completion, waterline, Present/query/frame-token facts without payload locators | completion/reclaim and device-loss/no-GPU traces |

The PE heterogeneous projection emits cold source and committed tokens over a
bounded all-family corpus, then the importer and effective-command observer
demonstrate that each committed token has one corresponding semantic effect or
specified rejection. x64/x86 Wine fixtures inject the required pre-entry and
effect-unknown failures. Only after formal, native, Metal, and Wine layers pass
may wild evidence use the architecture copy ledger to argue removal and
promotion.

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
