---
type: "Spec Gap"
title: "Verification Gap"
description: "Implementation and evidence gaps for formal and native verification."
tags: [specs, gap, verification]
---
# Verification Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## Occlusion lifecycle and attachment preservation (2026-09-17)

`QueryOcclusionResults` covers bounded result ownership, issue identity,
once-only contributions and fail-closed completion, not attachment bytes or
Store semantics. [H320](../../docs/perfomance/present-pacing/present-pacing-occlusion-timeline-stateblock.320.md)
exposes that distinction: R643 passes the lifecycle/error gates but loses
geometry after source-local discard at query cuts. The provider now preserves
concrete color/depth/stencil stores on queried and unqueried predecessors,
including multisample storage with resolve. Arm64/x86_64 Metal fixtures check
real Store accounting and successive depth-reading queries; R644 recovers
the inspected scenes with both zero and nonzero query results. Spot inspection
does not establish whole-sequence equivalence. Worker verification overlaps
R644, so its FPS/timing cannot qualify performance.
[H321](../../docs/perfomance/present-pacing/present-pacing-query-queue-drain-attribution.321.md)
adds same-image R645 observer-OFF scene inspection and separate R646 diagnostic
attribution without session-scheduled build/test/profiler overlap; sampled host
checks also find no competing verification work. Global perf counters remain
ON. Both runs exit normally with all eight health counters zero, but the spot
images and single pair do not establish full-sequence equivalence or performance
non-regression. Matching bridge/drain populations and bounded encode-stage
joins have native tests; their timing does not prove a removable critical path.
The provider remains default OFF; public PE/MSAA/fault breadth, full-sequence
wild equivalence and repeated performance evidence remain open independently
of the 214-configuration TLC gate.

[H322](../../docs/perfomance/present-pacing/present-pacing-query-ready-replay-frontier.322.md)
adds bounded Ready-frontier/predecessor and retrospective END/raw drain
diagnostics, with 12 native and six x86_64 focused gates plus the unchanged
214-configuration TLC inventory. R647 demonstrates later raw work in 1,773
stable-END drain calls, not an authorized narrower wait or removable time.
Admission-time issue identity, successful replay-prefix completion and polling
independent of mutable Device/Query ownership are still unimplemented gates.
The observed ordinal alone cannot make the current core polling call concurrent
with later replay. Full preceding-source-chain attribution, in-flight identity
and same-raw suffix separation remain open. R647's title capture fails; one
explicitly restored scene spot is retained, and its diagnostic/focus-affected
FPS is not performance evidence.

## Bounded lock-scope refinement (2026-09-09)

`R-BACK-SYNC-1`–`R-BACK-SYNC-5` and the unconditional implementation are owned by
[lock-scope refinement](../backend/lock-scope-refinement.md). Historical native ON/OFF
tests cover the actual range entry under queue contention, Shared buffer
bytes, Pool stale-generation/exact-backing max stamps, final-Slot differential
contents and live snapshot capacity. A concrete three-draw fixture pins three
scalar captured-mark acquisitions OFF to one batch acquisition ON. The shared
destination predicate covers all eight boolean rows.

`LockScopeRefinement` is a bounded model with independent expected captured
keys/waterlines and four executable negative controls. Full TLC passes:
52 normal, two progress and 113 expected-counterexample configurations
(260.93s); the new normal model has 24,297 distinct states. Its generation is fixed;
generation reuse/reclaim is not composed into this model. Bounded flush
fairness is not a proof of OS mutex fairness or end-to-end queue liveness.
The manual native/model correspondence is not generated whole-program
isomorphism. Publication-walk fence placement has code-review evidence but
not a dedicated native concurrent publish/reclaim differential.
[H309](../../docs/perfomance/present-pacing/present-pacing-lock-scope-wild.309.md)
adds cross-runtime builds, six runtime specs/audits and 05/06 GT2 OFF/ON wild
repeats with spot visual inspection. Lock acquisition work falls in the 06
diagnostic pair. Four PE32/PE64 OFF/ON dynamic-VB DISCARD/NOOVERWRITE/DISCARD
processes pass twelve exact readback samples; the experiment fixture is not
an exhaustive GPU-overlap or MT oracle. The 06 low-observer perf is slower in
both paired directions amid large repeat drift. CPU/latency/FPS non-regression and broader rendered/
concurrent correctness are not closed. The 2026-09-17 structural acceptance
retires the runtime opt-out; it does not turn those results into performance
proof. Unconditional native tests retain the actual range contention, bounded
batch/reference effects and final-Slot checks. This does not
upgrade H308's bounded model to a complete timing or concurrency proof.

The integrated [H318 consolidation](../../docs/perfomance/present-pacing/present-pacing-policy-consolidation.318.md)
passes the full unchanged 210-configuration TLA inventory, focused native
reference-effect tests and arm64/x86_64 Metal/runtime gates. Four retired
selectors are absent from production source; the source audit does not itself
prove refinement. Arbitrary MT interleavings and new whole-game equivalence
are not asserted by this revalidation.

[H310](../../docs/perfomance/present-pacing/present-pacing-payload-count-batch-cost.310.md)
adds a shared checked snapshot-byte delta, 2,048 bounded arithmetic-oracle
cells, overflow edges and actual multi-entry mixed-payload final-Slot tests.
The full TLC suite still passes (213.84s); no transition model changed and
no timing or exhaustive integer-domain proof is claimed.

## End-to-end immutable source composition

### Segmented geometry/owned-semantics split (2026-09-08)

R-CORE-REC-7.10 is bound to production preparation's finite
geometry/body/allocation acceptance and storage/ledger conservation checks,
with a failing-before/passing-after post-copy rejection test. Protected role
pages prove the geometry helper does not read semantic bodies; existing
post-admission source mutation pins owned-byte isolation. Twelve actual
PE32/PE64 processes pass 1,152 exact edge-layout pixel samples across
contiguous/segmented worker and segmented synchronous replay. Existing TLC
passes (160.90s), but does not prove parsing, concurrent client mutation,
allocator internals, or pixels. The native allocation seam does not prove the
exported C ABI OOM HRESULT: generic admission rejection remains a separate
R-CORE-REC-3.6 gap. See
[H272](../../docs/perfomance/present-pacing/present-pacing-segmented-preflight.272.md).

### Segmented empty-role and high-register boundaries (2026-09-08)

R-CORE-REC-7.6's absent-role token rule now has a failing-then-passing native
production-owner pin, including caller-owned non-null empty spans, ExactFixed
role parity and the production segmented-validity predicate. H271 reproduces
the resulting empty-outer-chunk/readback failure on both PE widths and fixes
the emitter plus selected-seal guard. Twelve fresh post-fix PE processes pass
1,152 exact SM3 edge-register/indexed-UP samples across inline OFF/ON,
contiguous/segmented worker and segmented sync replay. Existing TLC passes;
it does not prove role bytes or final pixels. This is an absent-handle pin,
not an independent zero-payload semantic case or general consumer/transport
promotion. See [H271](../../docs/perfomance/present-pacing/present-pacing-pe-transport-inline-boundaries.271.md).

### PE private selection and fused retention (2026-09-06)

R-CORE-REC-7.20–7.21 require producer-issued immutable selection and a fused,
kind-qualified warm lookup/acquisition. These are bounded value/ownership
refinements of the existing recorder transaction, not new queue transitions.
Inactive binding-locator storage has native canary/reset/null coverage plus
the existing all-family byte/ownership differentials; PE32/PE64 release
prologues confirm removal of the inactive initialization loop. Private-plan
writer/rebind/noncopy/nonmove checks, 192 production-vs-reference family/mask
byte/quota comparisons, and stale authority/constant-range/binding-collision
negatives pass. Fused retention has 9,604 bounded length-four traces over seven
operations and capacities 0–3, plus opaque cross-kind address, allocation-free
ceiling, and fresh-process fault/refcount pins. Checkpoints do not cross epoch
compaction. All four builds, native 21/21, x86_64 7/7, and unchanged TLC
(154.84s) pass. These are concrete bounded refinements, not a new temporal model
or a general freeze of borrowed constant/binding bytes. Full Windows envelope,
broader Wine faults/pixels and repeat promotion remain separate. Low-perturbation GT2 checkpoints are
tracked in [H246](../../docs/perfomance/present-pacing/present-pacing-pe-plan-authority.246.md).

### PE compact destination and identity refinement (2026-09-05)

R-CORE-REC-7.17–7.19 add calibrated compact/destination/family attribution,
gate the no-effect scalar walks outside observer-off traversal, and consolidate
record-local identity/pin/wire authority. The production typed lazy destination
resolver is bound to 512 three-stream and 64 IB boolean combinations with exact
query counts, A/B/A re-resolution, and observer bytes/PendingDelta differentials.
Owner fixtures cover 80/64 active frontiers, retained-only wire promotion,
collisions, canonical emission and rollback/retry. Existing recorder/ticket
negatives remain. Review follow-up gates new scope construction outside the
disabled path, pins sampled/unsampled nesting restoration, and checks all five
settlement family classifications and exact interval conservation. Pure
retention-spy tests and actual qualified-owner lookup tests remain separate;
their full Windows device-envelope composition is not natively pinned.
All four builds, native 18/18, x86_64 4/4 and existing TLC
(144.02s) pass. No temporal model changed: native value/ownership refinement
and release codegen are separate from existing acceptance/settlement proofs.
GT2 OFF/ON direction runs pass with zero errors, but final ON does not establish
diagnostic performance neutrality. Low-perturbation attribution, repeat FPS,
full Windows envelope, broader fault and pixel gates remain open. That checkpoint
preserved mutable source-plan checks; H246 separately closes source-selection
authority while retaining dependency/schema validation. See
[H245](../../docs/perfomance/present-pacing/present-pacing-pe-compact-reuse.245.md).

### PE prepared representation refinement (2026-09-05)

R-CORE-REC-7.14–7.16 keep the existing admission/settlement protocol while
reusing active section layouts, narrowing constant inputs, and replacing
duplicate binding-row storage with qualified occurrence metadata and canonical
rows. The integrated compact-plan implementation passes all four canonical
builds, 18 focused native specs, and x86_64 owner/batch/producer differential
and install-name checks (4/4). Tests cover all six constant kinds, all seven
binding families, byte ownership, capacity, rollback/retry, failure precedence,
observer populations, nonzero all-category compact-plan values, destination
rebase, and stale ticket/epoch/same-epoch wrong-recorder authority. Existing
TLA checks pass in 155.19 seconds; no model changed, so the new byte and source
refinement evidence is native rather than a new temporal proof. Release PE32
inspection also pins small constant witness frames after outlining; generic
and plan frames still require stack probes. Broader Wine fault/capture/pixel
coverage remains open. Matched GT2 candidate OFF/ON runs pass with zero errors,
about 4 CB/P, and effectively neutral OFF FPS. Independent review found no
production-reachable blocker, but dynamic D3D9DeviceImpl envelope coverage,
broader randomized compact-owner byte differentials (including FVF/repeated
rebase), and forged-plan constant-end-bound rejection remain open. Production
buildSparseStatePlan validates those end bounds before admission; the owner
layout helper does not independently repeat that check. Repeat promotion
evidence remains distinct from these bounded checks. See
[H244](../../docs/perfomance/present-pacing/present-pacing-pe-prepared-emission.244.md).

### PE final-wire closure slice (2026-09-02)

The PE recorder now carries an explicit ticket-scoped immutable
`PendingDeltaView` and reuses one pure `PeSemanticAdmissionPlan` for qualified
identity deduplication, payload layout, capacity, and owner emission-frontier
accounting. `PeSemanticBatchOwner` remains the sole production ExactFixed /
segmented final-wire owner for all 21 producer families; the old builder path
remains a differential oracle. A `CapacityPre` flush still requires a fresh
context projection because destination-chunk retention changes at that
boundary; this is intentional and preserves the state transition contract.
Production admission now uses one owner-local transactional operation: it
computes pure facts and six batch-global retention deltas internally, then
copies and settles immediately; wire handle counts remain record-local
because `EmissionHandleContext` resets per record.
R-CORE-REC-7.11 removes duplicate scalar/variable value arenas; canonical
payload owns those bytes after admission, while typed pins and binding metadata
remain distinct ownership information. R-CORE-REC-7.12–7.13 now reuse prepared
pin/wire ordinals, stage hot families separately, and revoke active prefixes
instead of clearing inactive value storage. Native byte/ownership differential
tests bind those representation changes; existing recorder TLA transitions
continue to own acceptance/settlement and do not prove wire values. Cold
observer re-emission remains instrumentation, and fresh copy-ledger
reconciliation plus broader Wine/GPU/pixel/locality evidence remain open.

The production identity spine now composes PE semantic acceptance through
terminal reclaim. PE ExactFixed and segmented descriptors carry a closed
producer event/source interval; Unix import rejects partial intervals, Raw and
CPU-ready owners preserve it, direct continuation extends only consecutive
intervals without adding a publish boundary, and the generation-qualified
lifecycle sidecar carries it through completion and reclaim. The bounded
production lifecycle projection begins at successful `AdmitWitness`
consumption; `ImportRaw` and `Plan` are abstract reducer states and are not
fabricated runtime events. The strict production observer rejects missing or
cross-raw duplicate identity.
`CpuPipelineLifecycle` checks the same completeness/non-overlap relation and
has independent duplicate-event, duplicate-source, and partial-identity
expected failures. Existing
borrow, effect-cut, child-join, completion, reclaim, device-loss, and wake
counterexamples remain the other axes of R-VERIF-7.9. Broader all-family direct
projection, C++ atomic-order evidence, Wine/GPU oracles, and promotion gates
remain open; they no longer represent a missing PE-to-reclaim identity join.
The composed model/code binding does not yet distinguish the valid complete
`RawOwned` copy from an invalid PE span retained after bridge return or an
unowned same-address alias. Those two expected-failure traces are required by
R-ARCH-7.20 and R-VERIF-7.9 before any shared-address adoption proposal.

R-VERIF-7.10's source-wide replay-state settlement is now implemented through
one shared pure phase relation plus a fixed-capacity typed undo journal. The
production compatibility, ordinary Direct, and Arena paths bind the same exact
source/record order, mark the irreversible cut before control or command
effects, restore both state and a private destination on pre-effect failure,
require an exact destination receipt before state commit, and prohibit retry
after effects. `ReplayProjectionTransaction.tla` and the native transaction
truth table cover A→B→A ordering, generation qualification, exact dedup
attribution, optimizer isolation/proof, state-before-emission, partial publish,
ordered-control reorder, and retry-after-effect. The immutable authenticated
raw sequence is the representation-preserving effective stream; no second
complete carrier is introduced by the transaction.

The complete final-slot/direct differential over effective commands, next
state, final storage values, resource/completion identity, and failure
disposition remains open, as does production optimizer-policy admission
binding. The R-ARCH-7.23 / R-BACK-2.99 production/API source audit now proves
`DrawRunSubmission`, its batch APIs, carrier-specific counters/adapters/scratch,
and carrier-only test seams are absent; the remaining negative audit continues
to forbid equivalent per-draw AoS carriers. The stable
`OwnedRawFinalChunkSlot` is an intentional queue-owned destination consumed by
the dedicated encode thread, not a compatibility carrier.
R-ARCH-7.24 and R-BACK-2.100 additionally require a fresh owner-qualified
ledger proving the PE emission, current-ABI RawOwned copy, one queue-final-slot
transaction, and required GPU-visible writes are the only large
materializations; no current run closes that four-boundary reconciliation yet.

## Recorder fault evidence boundary (2026-08-31)

The native `dxmt9-pe-recorder-fault-matrix-spec` binds the common typed fault
envelope: pre-reserve/retain failures, bridge pre-effect retry, entered bridge
effect-unknown poison, active capture disposition/throw, and Reset/teardown
handling. `scripts/tools/run_d3d9_fault_matrix.py` delegates one selector at a
time to the canonical conformance runner with a singleton chunk and timeout
replay disabled. Fresh Sikarugir x64 processes pass all nine selectors; the
earlier x86 matrix remains green. This is bounded recorder-envelope evidence,
not all-family exact-owner promotion or proof of the unchanged bridge ABI's
entered-call disposition. The existing StateBlock nine points are preserved,
with `bridge_pre` explicitly represented by the auxiliary fixture; unknown
selectors fail closed.

## Current status correction (2026-08-31)

The production parallel-policy proof adapter is implemented and enforced at the
pre-effect boundary. Its epoch-identity and exact-coverage proof blockers are
closed; certificate-invalid typed counters are zero on the final GT2/SFIV
probes. `ExplicitParallel` remains an explicit, default-off provider with no
promotion claim. The verification runner now inventories **50 production safety
TLC `.cfg` files**, two dedicated production progress configurations, and **108
deliberate counterexample configurations (160 total)**. The complete inventory
passed on 2026-09-03 with the pinned local JRE: 50 safety, two fair-progress,
and 108 deliberate expected-failure configurations all produced their expected
result. This run predates the subsequent runtime-projection cleanup; no TLA
file changed afterward, so the result remains the applicable formal evidence.
Direct nominal, progress, and all Direct expected-failure configurations have
also been rerun with the pinned local JRE. This inventory
includes `CpuReadyActiveHeadLookahead` and its seed-dropping expected failure,
alongside the existing StateBlock controls. The model now also covers
absent/adopted replay-FIFO intent, Direct/non-source disposition, cancellation,
replacement wake, and exact successor publication. It does not prove
frame-wide replay ordering or late Metal Store-action equivalence: H239
measured zero production intent selections and a persistent GT2 locality
failure, so that cross-boundary refinement remains open.
The fixed-role model contributes one production configuration and six
deliberate counterexamples. Its opt-in bridge and host-only value binding
remain distinct from the bounded `CpuReadySemanticTransfer` production seam;
pooled storage and copy-removal evidence remain open.

## Populated-slot carrier-free continuation (2026-09-01)

The Direct ChunkSlot queue now has a bounded pre-effect continuation proof for
non-UP Draw plus APPLY_STATE/constants. It admits only when every final SoA
vector and uniform lookup chain already has capacity for the immutable plan,
and the candidate producer interval is exactly adjacent to the populated
slot's last event and source ordinals. A gap, overlap, reordering, invalid
identity, or ordinal overflow therefore takes Legacy before effects; the
bounded model enumerates the exact `next = current + 1` relation in both
identity dimensions. Present, ordered control/query/readback, Update*, UP,
capture/trace, resource mutation, and unsupported shapes remain Legacy
fallback. `TriangleFan` is an explicit structural exclusion because
canonical replay decomposes it through the Legacy draw sink; classifying it as
Direct would cross the active transaction and fail-stop the queue. Typed
admission and fallback counters are emitted only when perf observation is
enabled. Native
coverage includes a populated Draw→Apply→Draw continuation, an insufficient
capacity fallback, Present-tail exclusion, terminal commit failure,
A→B→A state/completion differential, carrier-ledger zero assertions, and a
direct native truth table for malformed DrawRun shape, TriangleFan, Clear,
Readback, and resource-mutation exclusions. `DirectChunkSlotContinuation.tla`
is the small model/code boundary for the same admission, pre-effect rollback,
commit, and post-effect fail-stop/no-retry contract. The ordinary Direct lane
is enabled by default; the continuation itself is still strict fail-closed and
does not claim broad performance promotion. A 2026-09-02 GT2 run exposed the
previously unpinned populated-slot producer-identity gap after semantic replay;
the preflight relation above now rejects that shape before effects and the late
commit guard remains defensive. The fixed default-on run completed 1,578
Presents with zero commit failures, chunk rejects, or GPU command-buffer errors,
while retaining 3.999 command buffers and 15.790 render passes per Present. A
same-build direct-off diagnostic retained 3.999 command buffers and 15.797
passes per Present, so this correction does not create a Metal locality split.
The run is not performance-promotion evidence: default-on measured 24.362 FPS
against 25.204 FPS direct-off in one sequential spot pair. Of 15,277
continuation attempts, 79.54% rejected for capacity, 5.32% rejected for producer
identity, and 15.13% were admitted. The remaining economic question is therefore
the CPU cost of planning and attempting capacity-rejected continuations;
broader ordered-control/resource/capture faults remain open.

The PE recorder Wave 1A correction supersedes the older recorder counts below:
`PeRecorderCommit.tla` now checks the generated C++ commit matrix, models the
explicit `CaptureSkipped` disposition, and proves the reusable
`WarmAdvanced -> Unsealed` success edge; its stuck-success configuration must
fail. `PeStateBlockTransaction.tla` now imports a generated production
transition matrix and covers bounded Begin/End/Capture/Apply/reset/teardown
serial state, candidate/capture effects, staged-reference cardinality, and
Reset recovery. Its no-poison, no-release, stale-open, lost-duplicate,
poison-leak, and stale-capability controls fail at their named invariants.
The last control retains one capability across End/Reset/Begin and proves that
phase-only validation admits an ABA write. Exact state counts for the changed
commit model are intentionally not reused; the StateBlock lifecycle model's
earlier 131 / 34 count is not a claim about this full rerun. The Wave
1 composed settlement and repeated-value models now include a capacity-pre
flush crossing the bridge/capture/drain/reset/warm sequence and explicit
between-Capture/Apply mutation, respectively; the complete
`dxmt9-verify-tla` bundle passes. Exact state counts are intentionally omitted
because these bounded models changed in this correction.
The historical counts in the rows below remain historical and are not reused as
the current recorder count.

The all-family semantic projection now binds each accepted fact to the exact
latest `beginSource` issuance, including producer/record family and source
ordinal. Native A/B/A, wrong-producer, wrong-source, and retry cases are bound
to that predicate; the model adds dedicated A/B/A, wrong-producer, and
wrong-source expected failures. Runtime bridge fault injection and Wine
evidence remain open.

Row 47 is refreshed by the native physical-inventory, typed-adapter, and
common-append poison-gate evidence described in its StateBlock row.

## Verification Layer

### Mutation-composition observer (2026-08-31 correction)

| Area | Status | Evidence / remaining gate |
|---|---|---|
| R-BACK-44.9–44.11 production observer | ✅ observer identity correction / economic gate must be remeasured / composition forbidden | `MutationCompositionObserver` is an opt-in, fixed-capacity owner of exact resource/backing/source identity, Render Tape identity, byte ranges, disposition, completion, first CPU/GPU use, barriers, overflow, reset, and 60-Present window finalization. `classifyComposition` is the production pure predicate; `MutationCompositionTable.tla` is generated from its decision vocabulary only, while the ordered predicate relation and typed ordering-generation binding remain explicit in `MutationComposition.tla` and native cases. Every mutation/use now receives one observer-issued generation-qualified ordering identity; the production source ordinal remains retained for exact settlement. Mixed synchronous↔deferred adjacency and reset fail-closed behavior are covered natively and in TLA. The prior matched GT2 observer-off/on bundle passed behaviorally, but its zero-candidate windows are unreliable because sync used a private CPU counter while deferred/replay used `replaySeq`; the `<0.2 ms/Present` gate is therefore open for remeasurement, while composition remains forbidden and no fusion implementation is justified. See [`present-pacing-mutation-composition-observer.238`](../../docs/perfomance/present-pacing/present-pacing-mutation-composition-observer.238.md). |

### CPU pipeline lifecycle closure (2026-08-30)

The bounded `CpuPipelineOwnership` model separates `GPUInFlight -> Completed
-> Reclaimed`; the companion `CpuPipelineLifecycle` model owns the contiguous
GPU-completion frontier and finish-thread waterline composition. Its production
configuration and deliberate
missing-wake, skipped-completion, reclaim-before-completion, and
fabricated-GPU/no-GPU configurations are checked by TLC. The queue observer
now emits owner-qualified `PeImport`, `Receipt`, `SelectedParallel`,
`DeviceLoss`, or `Queue` evidence, and zero-command-buffer inline finish emits
`Encoding -> Reclaimed` with `NoGpuTerminal`. Full asynchronous owner traces,
GPU/Wine/wild equivalence, and promotion remain open.

Task 7 extends this with the generated `PipelineLifecycleTable`, the
generation-qualified `CpuPipelineLifecycle` safety model and independent
missing-wake, no-reset-generation, present-reclaim, and fabricated-GPU
counterexamples, plus `CpuPipelineLifecycle.progress.cfg`: a bounded
two-source FIFO with one credit and explicit weak fairness over arrival,
admission, replay-borrow return, publication, encode/join, GPU completion,
Present settlement, and reclaim. TLC checks `EventuallyReclaimed`, normal GPU
completion/reclaim, and selected-parallel join there; CompleteGpu settles the
Present watermark directly, matching the production observer (there is no
fictional SettlePresent edge). Native CV/interleaving tests cover the same
mixed Draw+Present Arena case. Reset/Teardown now emit a cold opt-in control
observation containing epoch, completion watermark, capacity generation,
live-source count, and `drained`; it never claims a live source is Reclaimed.
The opt-in production owner sink retains bounded exact events and runs the
same strict reducer fail-closed, with no retry/rebase after a discontinuity;
the watchdog receives owner events independently of the lifecycle sink (while
retaining the disabled-before-clock gate).

Selected-parallel child joins are now counted at the production backend's
`joinChild` boundary and retained only as a generation-qualified certified
post-join summary. The queue projection deliberately emits no fabricated
per-child `ChildJoin` events from the final count; the TLA+ per-child Join
action remains a model/code refinement gap until the production observer can
carry those boundary events without widening queue records.

Remaining environment assumptions are scheduler fairness, GPU completion,
C++ atomic ordering, and driver behavior. Typed BridgeReject/ReplayFailure/
EncodeFailure rows remain model/native vocabulary until real production fault
sites can classify those failures; production poison currently emits the
owner-qualified DeviceLoss/FailStop row instead.

The lifecycle matrix rows below carry the same bounded closure claims. Their
remaining open items are limited to the asynchronous and cross-runtime gates
named here; no row treats completion and reclaim as one phase or infers a
device-loss owner from ordinary queue reclaim.

| Area | Status | Evidence |
|---|---|---|
| End-to-end ownership/progress refinement (`R-VERIF-2.23`) | ⚠️ bounded abstract model and identity-qualified queue/Tape lifecycle binding partial | `CpuPipelineOwnership.tla` conserves FIFO identities across distinct `GPUInFlight -> Completed -> Reclaimed` phases; `CpuPipelineLifecycle.tla` separates the contiguous GPU-completion frontier from the finish-thread waterline. Production records Arena arrival/publication, every identity in multi-source dequeue/submit/completion batches, finish advance, actual Tape reclaim, and pending-completion fail-stop. Physical batch id/index/count prevents a second source from reusing a stale global snapshot. Observer-only occupancy accounting cannot poison or otherwise change queue behavior. The bounded Direct projection begins at successful admission-witness consumption; RawOwned import and planning remain abstract model states. Existing Direct/Legacy differential, Metal oracle, generation-qualified borrow batch, and hostile-escape claim boundaries remain unchanged. Selected-parallel child/join, C++ atomic ordering, bounded Wine faults, and GPU/wild equivalence remain open. |
| Direct-construction lifecycle observer (`R-VERIF-7.6`) | ✅ bounded production queue/Tape projection with full Direct terminal-edge binding; broader runtime isomorphism open | `dxmt9_pipeline_lifecycle.hpp` owns the fixed-capacity reducer, replay-failure cut, physical batch qualification, and separate GPU/finish frontiers. The nullable production sink records full source ordinal/seqId/storage-generation locators at successful admission, destination receipt/publication, Encode, Complete, Detach, Restore-or-Poison, and Reclaim. Admission uses the same raw/span/source FIFO predicate as abstract import. A compatibility-owned prefix has no fabricated Direct row; its first appended Direct sibling becomes the one behavioral physical-credit owner. Pre-effect rollback transactionally removes the admitted row, so later compatibility publication is ignored and no `RolledBack` row leaks. Terminal transitions preflight the complete sibling event batch against scratch shared-reducer state, require exactly one physical-credit owner, and expose neither ledger nor sink on rejection; a poisoned detached token remains retained on mismatch. Enabled-path correctness is owned by the exact Metal Draw/Clear/Draw/Present pixel/completion/fail-stop oracle and its recorded Apple-GPU pass. Disabled-path cost is a separate native source-contract claim: one cached Boolean branch guards event construction and helper entry, the sink facade is not copied, and no observer/ledger is allocated. Native fault pins additionally cover admission reorder, atomic later-sibling rejection, omitted source, stale batch snapshot, frontier drift, completion/reclaim order, writer-frontier reclaim, and fail-stop without changing production queue behavior. RawOwned import and planning remain abstract model states; selected-parallel child/join, device-loss detail, broader C++ atomic ordering, and GPU/wild equivalence remain outside this bounded projection. |
| PE/backend direct-construction and heterogeneous acceptance (`R-VERIF-7.7`) | production all-family chunk settlement connected; mandatory bounded 256-record/256-pin semantic owner; segmented bridge opt-in | The sole PE production final-wire path binds all 21 producers to one typed owner and persistent transaction. Exact owner pins now also qualify buffer hazards and Render Tape pending-chunk leases; allocation failure is a device-creation failure and no append/seal/transport failure selects a compatibility builder. Contiguous ExactFixed remains the capture/default representation and segmented roles remain negotiated. Focused native owner/batch/commit/capture tests plus current-toolchain x64/x86 PE builds pass. Missing evidence remains the bounded Wine all-family fault matrix, supervised workload/pixel/locality runs, fresh copy-ledger attribution, pooled CPU-ready storage, and selected-parallel lifecycle closure. |

Direct-slot positive headroom now reconciles all 64 physical payloads through a
scalar CpuReadyTape capacity snapshot before staging; the pure lease reducer
enforces the explicit limit and generation/serial-qualified settlement tickets
reject stale or double adoption/rollback. A shared compile-time schema now
owns the exact 32-semantic / 31-physical / 30-staged inventory; native coverage
is non-vacuous for readback and real-primitive fault injection visits all 30
staged sites. Expected-failure TLA controls for partial adoption and leaked
credit remain; the TLA model uses two payloads only as a documented symmetry
reduction. Opt-in Mach task and low-4-GiB address-domain counters provide the
requested diagnostic surface. R-BACK-2.105 additionally models and binds
complete retained-capacity reuse across reclaim, including the detached
owner-storage and stale-ledger counterexamples. Phase A adds a private-factory
generation-qualified linear admission witness and a full-identity move-only
detached owner token, with native stale/ABA/double-consume, mismatch,
restore/abandon, and ledger-conservation evidence. Phase B adds the shared
production/native `DirectSourceLifecycle` reducer, two-source/two-slot bounded
projection, and fourteen independent expected failures for witness, ordering,
effect, completion, credit, restore, and adoption errors. The extended Metal
oracle passes a noncommutative Draw/Clear/Draw multi-cut scene, trailing
Present, completion identity, exact pixels, and forced Direct fail-stop on the
available Apple GPU. The 2026-09-03 Metal-capable validation run passed
783/783 non-TLA Meson entries; this records the complete native result, not a claim
that the disabled observer path executed Metal. The previous 160-configuration TLC inventory passed on
2026-09-03 with the pinned local JRE. The current 161-configuration inventory
adds the production-admission reorder counterexample and also passes: 50
production safety, two fair-progress, and 109 deliberate expected-failure
configurations produced their required outcomes.
A 2026-09-03 same-build GT1
triplet recorded non-zero reuse, stable peak RSS, about 16.3 MiB additional peak
physical footprint, and lower CB/pass density at the 64 MiB aggregate ceiling.
Positive headroom remains default-off and unpromoted because 6,502 aggregate
denials and 14,102 capacity rotations remain, locality is still above Legacy,
the resolved policy is not yet first-class artifact metadata, and GT2 has not
been qualified on the reuse policy.
| WSI presenter replacement and release refinement | ✅ bounded model/native binding | `WsiPresenterReplacement.tla` checks attempted/accepted/completed Present-ordinal conservation and fairness-backed completion in addition to gate-before-candidate ordering, candidate failure recovery to a retryable idle state, the exact 1→2→1 shared physical-view claim count, gate-held registry swap, and zero old-layer producer/GPU users before old-claim release. The native protocol truth table binds final-claim release and bounded PE finalization; `dxmt9-cpu-ready-arena-lease-spec` executes the production queue gate, wait/notify, stop wake, registration failure, and active-arena finalization. The source-order audit pins null-client-view rejection, claim-before-raw-release, lifecycle-locked `PresentId`/mirror readers, and absence of a raw Presenter accessor. The model remains bounded, assumes one audited pointer identity for the aliased Wine host view, and does not replace repeated PE x64/WoW64 Wine reset/destroy evidence. |
| Stateful rendering-optimization evidence ladder (`R-VERIF-1.5`–`1.8`, `R-VERIF-6.4`) | policy specified / adoption partial | Existing queue, lifetime, scheduling, shader-runner, mini-replay, and wild evidence already provide individual layers, but older performance lanes do not consistently record the complete semantic → bounded refinement → model/code binding → GPU oracle → wild promotion chain. Apply the ladder when each lane is next changed; do not retroactively claim missing evidence. Parallel render-pass execution is the first explicit adoption target. |
| Verification-method taxonomy and full Render Tape identity (`R-VERIF-6.5`–`6.6`) | bounded R-VERIF-6.6 frame/sequence identity complete / broader grammar partial | Exhaustive native, SMT, TLA+/TLC, concrete replay, and wild evidence have non-substitutable claim boundaries. `dxmt9-render-tape-spec` exhaustively checks a declared 45-case domain against production validation, replay, and registry predicates: 20 create/write/draw/Present initial-content/generation cases, 10 two-Present boundary-mutation cases, 12 old/new generation registry resolutions across all identity kinds, and 3 conservative retained-tape reuse rejections. Every admitted tape passes a serial abstract-state sink for live handles, mutation-before-draw, draw-before-Present, completion conservation, and final digest. Deterministic fresh-process repetition, the low-level structural reducer/bisect, and bounded two-interval sequence identity are implemented; the high-level Python reducer/bisect remains separately provider-gated. The reducer now has native structural coverage for source-ordered, closure-qualified `OrderedControl`/`ObjectDestroy` and post-seed identity mutations, but this does not make their provider semantics executable. Native capture tests bind the production session's deferred-seal/final-validation boundary and explicit profile selector, while native Metal tests produce two distinct interval outputs and repeat their ordered digest vector. Existing Sikarugir frame bundles close capture/replay identity for uniform Clear and non-uniform textured-UP intervals. The 2026-08-14 production sequence capture validates 12 events/four chunks/six records and, on one warm-up plus two measured fresh-device provider runs, reproduces both captured 256×256 digests in order with 3/3 object and blob conservation. Remaining gaps are prior-output-load proof, broader provider grammar, executable PresentEx and volume capture/provider closure, non-Gamma control semantics, broader control/destruction provider-oracle evidence, tape-to-draw-slice projection, unbounded sequences, and claims outside the finite checker such as concurrency or driver behavior. No SMT encoding is required merely to close this row. |
| Bounded non-uniform textured frame-tape increment | native and Sikarugir captured-bundle identity complete / broader grammar open | Provider preflight flattens real command-chunk events without reordering and admits exactly `Clear` → one fixed-function textured `DrawPrimitiveUP` → standard `Present`. Tests pin the full single-level A8R8G8B8 seed, direct FVF and the byte-exact production-generated four-element declaration, and reject shader/arbitrary-vdecl/VB/IB, PresentEx, partial, and compressed near-misses before effects. Apple M1 production-path replays return the same non-uniform tight 16×16 SHA-256 `3dc6ca2708ccbb285106dea4b1cba42e6d67dd69ffe72ab003d87d7d8250b72e` through both binding representations. The Sikarugir bundle `experiments/output/render-tape-wild-textured-r2/tapes/frame-99975454225700-1` validates and replays with 2/2 blob references, 3/3 object conservation, `output_non_degenerate=true`, and matching capture/replay 256×256 SHA-256 `866e45bc5527c590f7cbf1deb9ca8fd5aa3ac2eddcd6746bdaf0572848a78c17`. |
| GT2 managed-atlas arm snapshot and source oracle | ✅ bounded production evidence | The exact single-level A8R8G8B8 MANAGED shader-texture shape now takes a generation-qualified GPU-visible arm snapshot and preserves meaningful alpha; all unsupported shapes remain fail-closed. In same-run r83 the 128×32 atlas blob is byte-identical to the post-draw GPU dump. The pre-fix replay error (1,582 pixels, max RGB delta 252, total 7,336) falls to r83 89/max 2/total 131 and independent r84 13/max 3/total 31. Two fresh provider processes per bundle are byte-identical and pass the authenticated 96-pixel/max-3/total-192 envelope. Source acceptance reuses that result only when capture source=output and replay source=output are each byte-identical; exact and equivalent modes remain separately reported. General MANAGED formats/mips and non-equivalent source/output domains remain unsupported rather than inferred. |
| RenderTapeIdentitySegments bounded refinement (`R-BACK-2.76`–`2.81`) | ✅ bounded model/native counterpart; GPU/provider promotion open | `RenderTapeIdentitySegments.tla`/`.cfg` checks exact disjoint ordered segment ranges, pass-piece edge continuity, atomic Ready publication, two-phase newest-suffix abort with reverse-tail finish and high-water restoration, strict flattened completion, exact full-run/tail settlement, greatest resource/page watermarks, reclaim safety, and pre-effect fallback/post-effect fail-stop. Parameterized attempt actions model correct reverse-tail detach/finish versus wrong-order fail-stop, and an explicit rollback-fault input covers partial abort. Native tape coverage detaches the complete suffix before any finish and rejects wrong-order finish; capture-ledger tests bind exact same-event source-run/tail registration. The model is intentionally bounded and is not an unbounded identity-grammar or concurrency proof. Metal, unbounded provider grammar, and GT2 promotion remain open. |
| TLA+ specs: CommandQueue, QueueLifecycleRefinement, PresentFrameLatency, ResourceLifetime, BufferBackingVersioning, NoOverwriteByteRange, EncoderLifecycle, QuerySeqId, ConcurrentProgressSignals, DrawableToken, WireObjectRegistry, PresentIdAba, EncodeSessionCompletion, DceChunkLookahead, CpuReadySessionProgress, CpuReadyActiveHeadLookahead, SessionCapacityLease, PostEncodePayloadRetirement, EncodeSchedulingProgress, ReplayScopedDrain, ParallelDrawBinding, RenderTapeParallelJoin, ParallelPolicySelection, ProducerMarkReclaim, PeRecorderTransition, PeRecorderCommit, PeRecorderSettlement, PeRecorderSemanticProjection, PeStateBlockTransaction, PeStateBlockValues, CpuPipelineOwnership | ✅ existing models / ✅ composed, repeated-value, and all-family semantic binding | The existing scheduling/lifetime models retain their documented owners. Recorder evidence now also includes `PeRecorderSemanticProjection.tla`, whose producer set is generated from production and whose nine mutations independently remove exact source, record, range, value, identity, latest-issuance identity, bridge fail-stop, or capture conservation. The recorder models do not cover C++ object layout/atomic ordering, PE COM behavior, or Wine runtime bridge/capture fault injection. |
| ResourceLifetime Initializer ownership refinement | ✅ bounded model/code binding | `ResourceLifetime.tla` separates arena-record release from Metal-object lifetime and carries the Initializer owner through Pending/InFlight settlement. Its retained production config explores 47,817 generated / 9,224 distinct states, while `ResourceLifetime.counterexample.cfg` must reproduce the historical bare-destination `StageInitializerUpload -> DestroyResource -> FreeResource -> NoUseAfterFree` trace. Production reclamation keeps the already-shared `canReclaimRecord` gate used by `ProducerMarkReclaim`; staging/enqueue/flush assertions use `pendingInitializerReferenceSafe`. The abstract deallocation relation deliberately has no C++ owner-oracle predicate. This does not prove Objective-C reference-count implementation or Metal command-buffer retention. |
| Encode-scheduling model extensions (`R-VERIF-2.13`–`2.15`) | ⚠️ | `SessionCapacityLease.tla` separates physical residency from encoded-unsubmitted work, and `PostEncodePayloadRetirement.tla` covers receipt, retirement, device-loss, resource-retention, and Present ordering. `ParallelDrawBinding.tla` now covers the source-local child binding/order/join subset of `R-VERIF-2.15`. Production DCE/session lookahead now uses queue-issued generation-qualified borrow batches; generalized ready-prefix DCE, cross-source parallel completion, and Metal 4 multi-segment completion remain open. |
| Parallel render-pass binding/order refinement and GPU oracle (`R-BACK-2.63`, `R-VERIF-1.5`–`1.8`, `R-VERIF-2.15`) | ⚠️ deterministic Stage 1/Stage 2b ladder and pre-effect boundary economics fallback implemented / completed wild matrix rejects default promotion | `ParallelDrawBinding.tla` checks two child shadows, one pass-wide compatible direct ABI, A→B→A generations, PSO ABI, exact draw ownership, ordered join, parent end, completion, and pre-effect serial fallback. Whole-command prefix planning, child imbalance, and child-boundary PSO/uniform eligibility are pure pre-effect guards refining that existing fallback transition; they add no post-effect state or new TLA transition. Native tests pin balanced single-run subdivision, jagged indivisible-command coverage and locators, no-two-child/invalid/overflow fail-close, stable-boundary and internal-churn rejection, all-boundaries-changed acceptance, `considered = accepted + serial_fallback`, reason conservation, and zero parallel effects followed by exact serial replay. Shared binding-transition tests and the API-validation Metal fixture retain Stage 2b slots 0/3 A→B→A coverage. Pre-gate GT2 regressed 6.44%; the first post-gate pair selected zero work because of the now-repaired planner defect. The valid decisive `r2` pair was non-vacuous but mixed: official +1.2441%, harmonic +1.7771%, and median -1.3590%, with 632 selected Stage 2b passes / 3,746 children / 252,381 draws. The completed matrix rejects promotion: repeat GT2 selected 641 Stage 2b passes / 3,799 children / 256,011 draws but measured harmonic -3.7556% and median -2.4312%, with no official delta because its identity result was overwritten before preservation; GT1 selected 3,245 passes and measured official -1.2492%, harmonic -0.5347%, median -0.9295%, while all 22 bounded captures showed no black polygon; GT3 selected 409 passes and measured official +2.2912%, harmonic +2.2642%; SFIV selected zero production parallel work, making its harmonic -0.9742% descriptive only, and its parallel screenshot was excluded after a harness capture miss. All eight lanes reported zero GPU errors, and locality was closely conserved for the non-vacuous 3DMark lanes. Identity remains default and ExplicitParallel opt-in because provider economics are workload-selective and unstable. SFIV zero eligibility was attributed (2026-08-16 counter run): `parallel_pass_shadow_reject_command=3239` of 3240 attempts matched `submit_stretch=3240`/`stretch_copy=3239` one-to-one — the per-present StretchRect tripped the sealed-pass builder's stream-wide `DrawRun`/`Clear`/`Present` precondition before candidates or economics. That precondition is replaced by pass-interval extraction: the five non-child coordinator helpers now seal an interval at their own serial ordinal and remain coordinator-side, a same-attachment resume across one fails both fragments closed, and only an unclassifiable command kind rejects a whole source. Native coverage pins the SFIV `Clear -> DrawRun -> StretchRect -> Present` shape, the split fail-closed case, the distinct-attachment two-pass case, the leading-helper carried-source case, and a coordinator-free regression pin. Wild evidence (2026-08-16): SFIV moves from `shadow_candidates=0`/`sealed=0` to `63182`/`63182` over `2760` attempts with `coordinator_boundaries=2759` matching `stretch_copy=2759` and `coordinator_splits=2673`, while still reporting `shadow_eligible=0` because its passes are below the two-child 64-draw floor; GT2 is coordinator-free and byte-identical to a matched `871dc233` baseline across every producer counter. Zero GPU command-buffer errors on both. Cross-source and carried-session coverage, cost-model/provider-policy refinement, SFIV wild capture evidence, and Metal 4 segment completion remain open; repetitions of the unchanged policy are not the next verification gate. |
| Parallel policy algebra and evidence-role separation (`R-BACK-2.68`–`2.75`, `R-VERIF-2.16`–`2.22`) | bounded proof core, authenticated structural explorer, production proof adapter, and fail-closed enforcement implemented / proof blockers closed / ExplicitParallel default-off with no promotion claim | `ParallelPassSemanticPlanView` is value-owned; `validateParallelPassSemanticPlan` performs one owner-issued interval snapshot lookup plus one synchronous per-child exact coverage resolution and binds independent source/storage generations, first-child locator, authoritative DrawRun begin/count, epoch/coordinator/attachment/resource/route facts, and ordered coverage. The native parallel-pass spec provides bounded adversarial coverage/arithmetic/selection evidence, including exact read/write overlap, shifted/truncated absolute ranges, stale sealing authority, one-call counts, whole-command ranges, checked arithmetic, safe-only ranking, permutation-independent argmax, ties, monotonic negative evidence, and serial fallback. `ParallelPolicySelection.tla`/`.cfg` adds the bounded all-input-valid/benefit-zero selection, effect/join/parent/completion refinement with fairness-backed progress. The Render Tape `policy-explore` surface authenticates tape plus identity, enumerates deterministic whole-record 2/4/8/16-child structural candidates, and excludes coordinator ranges, but explicitly reports `structural_only=true` and `proof_core_validated=false`. Candidate-capacity exhaustion invalidates the complete exploration instead of publishing a truncated candidate set; reported pipeline and uniform features are input-section counts, not PSO identities or transition claims. The identity sidecar is not a substitute for owner-issued first-draw, attachment, resource, route, hazard, or epoch facts. The production coordinator now owns exactly one adapter into this core: `runParallelPassProofCoreAdapter` runs in `tryEncodeParallelPass` after complete locator/ABI/PSO/uniform re-resolution and before render-pass preparation, with `resolveParallelPassSnapshotAuthority` re-reading the pass from the producer's own sealed-pass batch and `resolveParallelPassCoverage` re-reading and canonicalizing every command a child owns from the live source. A certificate-invalid candidate never reaches selection, an unselected candidate never reaches child creation, and the pre-existing economics classifier still runs and rejects independently. `parallel_pass_adapter_{considered,certificate_valid,certificate_invalid,selected,serial_fallback}` conserve, and native coverage pins selected-and-executed, three certificate-invalid mutations with zero backend calls plus exact serial replay, invalid-economics and non-positive-benefit selection failures after a valid certificate, and conservation across every bounded child count. Identity remains the default and no promotion claim is made. The first measured GT2 adapter run is fail-closed: `parallel_pass_adapter_considered=3114`, `certificate_invalid=3114`, `selected=0`. A matched same-machine `871dc233` baseline reports numerically identical GT2 counters including `parallel_pass_selected=0`, so no execution was lost; GT2 already selected zero passes before the adapter existed. The certificate rejection was attributed by instrumentation to exactly two proof-core properties. The first — `PassIdentity`'s `coordinatorProof.firstPassActionEpoch == passActionEpoch`, which admitted only a source's first pass (observed `epoch=3` versus `proofEpoch=1`) — is now closed without relaxation: the producer issues the coordinator proof per sealed pass with that pass's own epoch, and `validateParallelPassSemanticPlan` independently re-derives the interval's epoch through `deriveParallelPassActionEpoch`, a pure fold of the shared `ParallelPassActionEpochState` over the generation-pinned replay stream using the shared `classifyParallelPassCommandRole` classifier and a coordinator-supplied seed. Native pins cover fold determinism, the exact GT2 two-pass shape certifying both intervals, a self-consistent cross-pass stamp failing `PassIdentity` through an echoing owner, and a stale storage generation failing `SourceIdentity` with zero epoch-witness reads. The second — the 16-row `ParallelPassResolvedCoverage::commands` capacity against whole-command children owning 26-52 commands — is also closed: `ParallelPassCoverageFold` streams resolved commands through O(1) exact accumulator state with no hash summary anywhere, enforcing order, non-emptiness, arithmetic, non-overlap, and whole-command contiguity against the previous boundary at append time behind private fields. Its adversarial evidence is a test-local stored-row reference reproducing the pre-change predicate set, compared against the fold over a generated domain of ~320 gap/overlap/duplicate/out-of-order/empty/overflow/subrange/exactly-boundary row sets, plus a 61-command production-validator case whose widest child exceeds the former row capacity and still fails closed under per-row mutation. A downstream-consumer audit confirmed coverage rows never escape validation. Both closures strengthen rather than relax the proof core. The wild GT2 measurement against these commits (2026-08-17, `0136be51`) reports `certificate_valid=3164` of 3,210 considered (98.6%), first-ever adapter-gated execution `parallel_pass_selected=598` (3,540 Stage 2b child tasks), exact adapter conservation, zero GPU errors, and a visually normal scene frame; the same-build SFIV probe keeps 60,388 sealed passes with eligibility now owned by the economics floor alone. Mechanism evidence only — not a performance or promotion claim. A typed certificate-invalid breakdown then attributed 100% of the residual on both workloads (GT2 47/47, SFIV quantum-8 1,387/1,387) to the single `pass_identity` checkpoint, and that checkpoint is now diagnosed and closed for two independent false negatives, each reproduced deterministically through the real producer and the real certificate in `boundaryAdjacentPassIdentityCertifies`. First, `encodeChunk` seeded the epoch fold with the domain's invalid sentinel (`0`) on any source carrying an open encode session, so the witness was invalid and no candidate of that source could ever be re-derived; the seed is now the shared per-source constant `kParallelPassSeedActionEpoch = 1` and the producer fails a zero-seeded source closed with the typed epoch reason so both sides share one seed requirement. Second, a pass sealed by an attachment change carries the next pass's first draw as its sealing locator — the spelling `parallelPassCloseReason` already maps to `RenderTargetChange` — which the certificate rejected as a non-coordinator sealing kind; it is now admitted only after a second independent epoch re-derivation proves the sealing ordinal opens a different pass, which is an added obligation rather than a relaxation and is sufficient because exact coverage already forbids any boundary command or foreign attachment inside the interval. Native pins cover both boundary-adjacent SFIV passes certifying on fresh and carried sources, cross-boundary stamp forgery failing through an echoing owner, the attachment-change seal failing under a witness truncated at the sealing ordinal, single-fold cost for coordinator-sealed passes, and the zero-seed source failing closed with zero candidates; each of the three code changes was reverted individually and produced a distinct pin failure. The wild re-measurement (2026-08-17, `5a52e037`) confirms the collapse: GT2 `certificate_invalid=0` (3,114/3,114 valid, `selected=647` now exactly equal to `economics_accepted`), SFIV quantum-8 `certificate_invalid=0` (1,750/1,750 valid, selected still 0 via the known imbalance-bound economics rejection), every typed reason zero, zero GPU errors on both. Render Tape still owns deterministic production-import/replay identity; Metal readback still owns concrete ABI/resource/shader/pixel behavior. |
| Post-encode payload retirement (`R-BACK-2.44`, `2.45`, `2.49`, `2.59`, `2.65`) | behavior implemented behind default-off Tape gate / promotion pending | A bounded generation-stamped receipt ledger replaces eligible Legacy or Arena payload locators after synchronous encode. Retirement uses locked detach, out-of-lock destruction, and locked generation/page finish; completion accepts mixed receipt and legacy identities, rejects stale/duplicate/ABA receipts before effects, and preserves resource/GPU waterlines. Native specs cover mixed completion, more than 30 resident-retired sources in one open session, the deterministic 128-source work cap, and receipt failure cases; the behavior-bearing TLA model covers device-loss and Present ordering. Wild visual/locality/performance promotion evidence remains intentionally open. |
| PE recorder composed settlement and repeated StateBlock values (`R-VERIF-6.7`) | ✅ bounded production-table/model/native binding / ⚠️ x86+x64 Wine fault matrices complete, broader runtime evidence open | `PeRecorderSettlement` imports generated settlement and commit tables and checks CapacityPre full-builder crossing, effect-unknown bridge fail-stop, all capture dispositions, alias-before-parent drain, reset/warm return, emitter acceptance, all production CapacityPost rows, and liveness only after successful internal settlement is enabled. Exact scalar pending conservation remains in `PeRecorderTransition`; all 21 heterogeneous producers bind through the generated `PeRecorderSemanticProjection` family table and production `planCommittedPeSemanticProjection`, exact builder payload/identity visitors, and exhaustive native differential/counterexample rows. `PeStateBlockValues` binds Capture/Apply failures, intervening live mutation, latest publication, and untracked isolation. Fresh Sikarugir x86 and x64 processes pass all nine CapacityPre/retain/bridge/capture/Reset/teardown selectors with exact production consumption receipts. Capture disposition preserves the accepted command by settling its reserved token before abort. | Runtime bridge-effect classification remains unavailable in the unchanged ABI, so entered-call failure is fail-stop until explicit Reset/teardown discard. Unbounded traces, broader COM-runtime instrumentation, and wild workload evidence remain open. No wire ABI, Metal, vtable, or key-function change is part of this bounded wave. |
| `SegmentedTransportV1` fixed-role transport (`R-VERIF-2.24`) | ✅ bounded protocol + opt-in bridge/native value binding / ⚠️ pooled ownership and copy removal open | The model checks `ReserveRole -> AdoptRole -> EmitRole`, exact geometry/unique identities, the 144-byte descriptor, 32 MiB cap, complete reservation, atomic adoption, one ExactFixed extent, rollback/fail-stop, conservation, FIFO settlement, and reclaim wake. Six independent configurations fail for incomplete reservation, partial adoption, fallback-after-effect, lost checkpoint, partial ledger, and missing wake. Native/codegen/cross-build evidence binds negotiation, ABI layout, WoW64 4 GiB bounds, all-or-nothing Unix ownership, idempotent ledger release, no-gather import, and all 21 canonical families. The separate `CpuReadySemanticTransfer` seam now binds real queue Reserve/Adopt/Emit/Publish-or-Abort transitions; fixed-role pooled storage, copy removal, GPU pixels, and wild promotion remain missing. |
| Production TLC inventory | ✅ 63 production safety + 4 fair-progress + 147 expected-failure configurations | The complete 214-configuration inventory passed with the pinned local JRE. It includes `QueryOcclusionResults.tla` and its three executable occlusion counterexamples; the inventory remains a bounded safety/progress gate and does not claim wild qualification. |
| Companion native spec for `DrawableToken.tla` stash/take/wait state machine | ✅ bounded single-token binding | `dxmt9-drawable-token-spec` executes the production `drawableTokenMayFulfill` and `SingleUseTokenSlot` owners, pins null-acquisition fallback, no-overwrite stash, and single-use take, and covers complete-before-wait plus wait/fail without sleeps or Metal timing. `PresentDrawableToken::complete/fail` now reject a second fulfilment in production. The finite model and native spec cover one token lifecycle per Presenter slot; repeated or overlapping Present lifecycles through the same slot remain outside this claim. |
| PE recorder transaction and model/code binding (`R-CORE-REC-2.3`, `3.*`, `4.*`, `5.1.4`, `6.*`; `R-VERIF-1.5`–`1.6`, `6.4`, `7.1`–`7.4`) | ✅ bounded generated algebra/model/code binding; ⚠️ runtime promotion open | Recorder write, commit, and StateBlock transition matrices are production-used and generator freshness-checked before TLC. `planPeStateBlockTransition` is exhaustively compared to every shared phase/event row; generator completeness rejects unmapped enum phases/events/actions. The StateBlock lifecycle includes total non-terminal `PoisonRequested` rows, the ownership invariant `PoisonOwnsNoCandidateOrRefs`, monotonic Recording epochs, and `NoStaleCapabilityWrite`. Deliberate `PoisonLeak` and phase-only `StaleCapability` configurations fail at those invariants. Concrete native fake-COM coverage owns exact same-cell rejection, distinct-cell duplicate retention, poison cleanup, and the Begin-A/Reset/Begin-B ABA witness. This does not prove unbounded traces, epoch wrap reachability, COM runtime internals, or Wine bridge failure behavior. Detailed owner: [`d3d9/recorder/gap.md`](../d3d9/recorder/gap.md). |
| Companion native spec for `ConcurrentProgressSignals.tla` cross-axis non-blocking | ⚠️ production lifecycle emission bound; three-axis binding partial | `dxmt9-pipeline-lifecycle-observer-spec` owns the deterministic reducer and condition-variable counterexamples through `FakePipelineQueue`. Separately, `queue_completion_sources_spec` drives the actual `QueueLifecycleController` and its queue-owned sink, including 64-bit Tape generation identity and the Arena single-arrival rule. Its inline zero-command-buffer case binds the explicit `NoGpuTerminal` terminal disposition without synthetic GPU milestones; poison-stop owner truth-table coverage binds `DeviceLoss` for raw/encoding fail-stop edges. The shared pure header `include/dxmt9/progress_predicates.hpp` binds the non-blocking `queryGetDataPollSatisfied` poll plus the `presentTokenWaitSatisfied` and `ringAdmissionWaitSatisfied` CV predicates to production and native truth tables. PE/import/selected-parallel/device-loss owners remain bounded to existing observer abstractions. |
| Companion native spec for `PresentIdAba.tla` slot-reuse ABA-safety | ✅ | `dxmt9-resource-lifetime-spec` directly instantiates production `HandleArena`, forces LIFO slot reuse after watermark-gated reclaim, and pins generation advance, stale/wrong-kind rejection, exact replacement lookup, and sibling-slot preservation. The model's documented 24-bit non-wrap assumption remains unchanged. |
| Pacing-axis independence (`R-ARCH-6.8` / `R-ARCH-6.9`) | ✅ abstract model and bounded query binding; ⚠️ whole-wrapper wait freedom not claimed | `ConcurrentProgressSignals.tla` proves abstract `NoQueryWaitBlocksPresent`, `NoFrameLatencyBlocksQuery`, `NoRingPressureBlocksPresentCompletion` liveness with `PacingOrdering`. The default-ON R-CORE-11.5(a–c) path has actual-prefix and persistent-submit evidence below, with submission pressure separate from successful completion. Explicit OFF still uses synchronous flush. PE recorder settlement, bridge replay drain, queue mutexes and bounded END admission remain; neither model claims a wait-free public wrapper. |
| Hazard model: exact handle sets, not Bloom (`R-BACK-2.28`) | ✅ | `EncoderLifecycle.tla` `MergeRenderDraw` checks RAW/WAR/WAW set intersection; `BloomNeverForcesSplit` invariant proves split path ignores Bloom signal; `bloomFalsePositiveCount` advances only when exact disagrees (diagnostic-only role formal). |
| `QueueLifecycleRefinement` concrete queue lifecycle model checked and asserted | ✅ | `QueueLifecycleController` debug invariants cover `readySlots`, `pendingCompletion`, `completedSeqQueue`, inline completion, empty commit, `waitForSequence`, and shutdown paths |
| `PresentFrameLatency` present-token model checked and asserted | ✅ | `completedPresentSeqQueue_` advances `presentCompletedSeqId` only after `completedSeqId`; `presentBoundary()` asserts `MAX_FRAME_LATENCY` wait return safety |
| `SeqIdSafety` asserted with `// TLA+:` label | ✅ | `Device` submitted/completed sequence guards and queue completion watermarks |
| `QueryResolutionSafety` predicate | ✅ bounded ordinary/initializer completion and opt-in GPU occlusion result provider; ⚠️ default-off wild/promotion and other result providers remain partial | H316/R637–638 binds `query::classify` / `submissionWorkTarget` to actual queue publication/submit/completion/reclaim and persistent encode-CV wake. `QueryPublicationFence` checks safety/progress and five rejected mutations, including retiring pressure at publication instead of submission. `QueryInitializerFence` adds the ordinary/event/successful-upload product, capacity and three rejected mutations. `QueryOcclusionResults.tla` adds issue-qualified result generations, immutable active membership, independent per-pass contributions, once-only callbacks, terminal release identity, reclaim safety and fail-closed read gates; its 31,008-state production configuration and three mutation counterexamples pass the TLC gate. Native core/Metal and x86 occlusion lanes cover zero/1024/sample-count overlap, reissue/reset, pending-byte preservation and both tile modes. Real driver allocation/failure injection and recovery, C++ memory ordering, unbounded histories, other MSAA/query capacities, full-frame wild qualification and performance promotion remain outside this claim. |
| Deferred END writer-to-publication refinement (`R-CORE-11.5(d–e)`) | ✅ bounded model/native/GPU/Wine; ⚠️ default-OFF promotion gates | H317/R639: `QueryEndCoalescing` has 62,150 distinct states, bounded safety and AdmissionProgress; unbound success, stale bind and lost submission pressure are rejected. `canBindEnd` / `classifyEnd` are production native predicates. Queue tests cover actual cap, allocation failure, multiple ENDs, natural/FLUSH publication, reuse/release and terminal failure; core tests cover deferred reset/reissue. Actual GPU gate plus 256 exact bytes and delayed x64/x86 Wine draw/readback cases pass. Initializer dependency composes with its separate model/oracle. C++ memory ordering, unbounded histories, driver recovery, whole-frame wild equivalence and stable FPS non-regression are not proved. |
| `BoundedInflight` asserted | ✅ | sim + metal `QueueLifecycleController::commitCurrentChunk()` |
| `NoUseAfterFree` asserted with `// TLA+:` label | ✅ bounded model/code binding | `ResourceLifetime.tla` now represents the missing Initializer actor, its Pending/InFlight ownership transfer, arena-record release, abstract Metal-object release, pre-submit abort, and success/device-loss settlement after submission. Production reclaim retains the shared `canReclaimRecord` predicate already bound to `ProducerMarkReclaim`; staging/enqueue/flush assertions use `pendingInitializerReferenceSafe`. The 22-case native truth table binds both production guards, and the expected-failure `Bare` config reproduces the 2026-08-02 escape. The abstract deallocation relation deliberately has no C++ predicate because production has no global owner oracle. Claim boundary: the model does not prove Objective-C reference-count implementation or Metal's encoded command-buffer retention. |
| `RingSafety` asserted with `// TLA+:` label | ✅ | `RingArena::allocateBytes()` + slot ring |
| `EncodeSafety` asserted with `// TLA+:` label | ✅ | encode loop |
| `WineCommit` action mapping comments | ✅ | `QueueLifecycleController::commitCurrentChunk()` and `CommandQueue::submit*()` paths |
| DOD wire-schema acceptance | ⚠️ | Existing chunk tests cover many POD/layout and import validation cases; R-VERIF-7.1 now tracks full wire-schema acceptance for size/alignment, command IDs, version constants, offsets, and variable-tail rules |
| PE/unix bridge ABI skew detection | ✅ deterministic codegen fingerprint / runtime pairing evidence separate | `gen_wine_bridge.py` fingerprints declaration-order bridge ordinals, generated argument-record fields, source POD record fields, and layout-affecting preprocessor context such as `#pragma pack` and ABI macro values. `dxmt9-bridge-codegen-abi-spec` proves deterministic output, repository-real record/opcode participation, and rejection of prototype-order, pointed-record-layout, packing, and array-extent mutations; generated bridge files remain custom-target outputs. The source schema is the pointer-width-independent cross-target layout contract, while generated native/WoW64 Args layouts have separate target-local static assertions; compiler-specific `sizeof`/`offsetof` values are intentionally outside the shared hash. Mixed PE/unix Wine load tests remain the runtime evidence layer. |
| Queue observer / fake-backend verification | ⚠️ partial — selected serial binding complete, selected parallel binding open | R-VERIF-7.3 requires deterministic queue-facing evidence for chunk seq IDs, exact retained handles, replay categories, barrier/readback boundaries, and effective encoded command order without Metal timing. `dxmt9-producer-interleaving-spec` retains the real queue/pool concurrency evidence for sequence publication and mark/pin/reclaim. `dxmt9-encode-session-lifecycle-spec` now drives the real `encodeChunk` entry for Legacy and Arena storage and closes the selected-serial dispatch clauses: mixed Draw/Clear/Copy/Readback/Present metadata, exact shared-visitor resource spans including payload override/snapshot handles, DCE omission, permutation, explicit subranges, fragments, original ordinals, exact callback counts, and invalid-range zero callbacks. The production sink has one null gate per effective stream/fragment and the disabled instantiation performs no observer storage or resource walk. **Still open:** the test-only suppression fixture bypasses `tryEncodeParallelPass`, so no selected fake/recorded parallel batch pins effective-order callbacks after eligibility/proof gates and immediately before the first child effect. Do not infer that bounded parallel binding from serial coverage. The separate `ConcurrentProgressSignals` three-axis queue-observation gap also remains open. |
| DXMT concept mapping acceptance | ⚠️ | README mapping exists; R-VERIF-7.4 requires explicit implementation-owner and test evidence for each hot-path concept before calling DXMT merge readiness complete |

**The verification layer is partial for DXMT merge readiness.** Existing
R-VERIF-1.x through R-VERIF-6.x evidence remains complete, while new R-VERIF-7.x
acceptance tracks wire-schema, fake-backend/queue-observer, bridge-budget, and
DXMT concept-mapping evidence.

---
