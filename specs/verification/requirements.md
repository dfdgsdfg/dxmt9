---
type: "Spec Requirements"
title: "Verification Requirements"
description: "Verification requirements and compatibility contracts."
tags: [specs, verification, requirements]
---

# Verification Requirements

The verification layer provides machine-checked evidence that the most
error-prone concurrent and stateful subsystems of dxmt9 satisfy their
behavioral specifications. Informal English specs and code review are
necessary but not sufficient for these subsystems.

---

## 1. Scope

**R-VERIF-1.1** Formal verification is required for any subsystem where
correctness depends on the interleaving of two or more concurrent agents
(threads, callbacks, GPU signals).

**R-VERIF-1.2** Formal verification is required for any subsystem where
safety depends on a protocol tracked by monotonically advancing counters
(sequence IDs, reference counts, fence values).

**R-VERIF-1.3** Verification is not required for pure functions (shader
translation math, format mapping tables, decomposition algorithms). Those
are covered by unit tests and property-based tests.

**R-VERIF-1.4** For each verified subsystem, the formal spec must:
- Identify all agents and their actions.
- Identify all safety invariants that must hold in every reachable state.
- Identify all liveness properties that must hold under fair scheduling.
- Trace each property back to a requirement ID in `d3d9/` or `backend/`.

**R-VERIF-1.5** A rendering-performance lane must treat formal or exhaustive
finite-state refinement as the default first correctness layer when it changes
observable rendering through command reordering, state-shadow or cache reuse,
pass/action elision, encoder partitioning, deferred completion, resource
lifetime, or concurrent execution. The model must describe the serial reference
behavior and prove that every optimized transition either refines it or rejects
the candidate before externally visible side effects. A lane that does not use
this layer must document why its correctness reduces to a pure value transform
or to GPU behavior outside the finite-state abstraction.

**R-VERIF-1.6** Formal evidence for rendering correctness must be attached to
the implementation by deterministic code-level evidence. Transition predicates
used by the model must be implemented as shared pure functions where practical,
and native truth-table, property, or fake-backend tests must compare the C++
decisions with the modeled transition relation. A green model without this
binding is evidence about the abstraction only, not about the production path.

**R-VERIF-1.7** A model must state what it does not prove. In particular,
abstract state or temporal logic does not by itself prove shader byte layout,
Metal API/driver behavior, resource contents, floating-point results, or final
pixels. A lane that can change those observables must add a deterministic GPU
readback, shader-runner, same-input replay, or equivalent oracle before wild
application evidence is used for promotion.

**R-VERIF-1.8** Wild runs are discovery and final integration evidence, not the
first or sole proof of a stateful rendering optimization. A lane may use a wild
run to discover an unmodeled trace, but default-on promotion requires the
applicable formal/exhaustive, model-to-code, and deterministic GPU evidence to
be green first. Any reproducible visual failure blocks promotion even when
Metal validation, command-buffer status, and GPU error counters are clean.

---

## 2. Command Queue

Traceability: R-ARCH-6.1-R-ARCH-6.7, R-BACK-2.1, R-BACK-2.2,
R-BACK-2.12, R-BACK-2.13, R-BACK-6.4-R-BACK-6.8

**R-VERIF-2.1** The formal spec must prove that the ring buffer never
allows the Wine thread's write index to overwrite a slot that is still
Pending, Encoding, or GPU (ring overwrite safety).

**R-VERIF-2.2** The formal spec must prove that the number of simultaneously
in-flight chunks (Pending + Encoding + GPU) never exceeds `MAX_INFLIGHT`
in any reachable state (back-pressure correctness).

**R-VERIF-2.3** The formal spec must prove that `completedSeqId` never
exceeds `currentSeqId` (seq ID monotonicity).

**R-VERIF-2.4** The formal spec must prove, under weak fairness of the
encode and finish threads, that every committed chunk eventually reaches
the Free state (no permanent stall).

**R-VERIF-2.5** The formal spec must prove that once the Wine thread stops
committing, all in-flight work eventually drains and the queue reaches
quiescence.

**R-VERIF-2.6** The formal spec must include a concrete queue lifecycle model
for `QueueLifecycleController` that exposes `readySlots`, `pendingCompletion`,
`completedSeqQueue`, inline completion, empty commit, `lastCommittedSeqId`,
`waitForSequence`, and shutdown wakeups.

**R-VERIF-2.7** The concrete queue lifecycle model must prove that `readySlots`
contains only Pending slots, `pendingCompletion` contains only GPU-submitted
slots, `completedSeqQueue` entries never exceed `lastCommittedSeqId`, and the
implementation-level slot transitions refine the abstract queue lifecycle.

**R-VERIF-2.8** The concrete queue lifecycle model must prove that
`waitForSequence(target)` cannot return before `completedSeqId >= target`
unless queue shutdown has been requested.

**R-VERIF-2.9** The formal spec must model present-bearing chunks separately
from non-present chunks. Present completion must not advance ahead of normal
command-buffer completion, i.e. `presentCompletedSeqId <= completedSeqId` in
every reachable state.

**R-VERIF-2.10** The formal spec must prove that queue-owned frame-latency
tokens bound accepted but incomplete present-bearing chunks by the configured
maximum frame latency, and that an application present wait cannot return while
the gate remains full unless shutdown has been requested.

**R-VERIF-2.11** The formal spec must prove the `EncodeSession` completion
refinement used by R-BACK-2.49: one Metal session-tail completion may expand
into several consecutive source `seqId` completions, but each source completes
only after the Metal tail containing its commands completes, per-source
completion drains in strict sequence order, and present completion advances only
for the represented present tail source.

**R-VERIF-2.12** The formal spec must prove the bounded DCE successor-window
refinement from R-BACK-32.10: at most one source is held outside the ready FIFO,
the held source and ready suffix remain consecutive, submission stays in strict
source order, an encoded prefix remains owned by the held source, and no-ready
fail-open release cannot discard or overtake a source. The implementation
must not wait for a successor.
Coverage is provided by `tla/DceChunkLookahead.tla`.

**R-VERIF-2.13** Formal or equivalent refinement evidence must prove the
bounded ready-prefix DCE contract in R-BACK-32.11: the snapshot contains only an
already-ready consecutive FIFO prefix, DCE owns no future source, every source
keeps an independent DAG and completion identity, proof-stopping boundaries are
conservative, and lack of proof releases current work without waiting.

**R-VERIF-2.14** Formal evidence must prove CPU-ready source and EncodeSession
admission progress for R-BACK-2.44, R-BACK-2.60, and R-BACK-2.65: all stores are
bounded, back-pressure cannot hide a consumed visible prefix, every represented
source submits or is restored in FIFO order once an ordered semantic or
fixed-cap release event occurs, and admission pressure cannot block completion
progress required to release that pressure. After admission clears, a producer
sequence wait may authorize only exact eligible FIFO heads at or below its
ordered target, with distinct attribution, commit-time identity consumption,
restore eligibility, and no pressure-created release, capacity, or session
transition. Producer quiescence without such an
event has no liveness obligation. Liveness claims are under weak fairness for
enabled replay-worker, encode-coordinator, Metal-completion, and finish-thread
actions. The model must represent descriptor and page generations,
non-wrapping page reservations, `Writing -> Sealed -> Ready -> Represented ->
Submitted -> Completed -> Reclaiming -> Reclaimed`, atomic publication, strict
`sourceOrdinal`/`seqId` order, high/low watermarks, replay-worker-only admission
wait, finish-owned reclaim wakeup, a generation-stamped session capacity lease,
successor headroom including deterministic wrap padding, fixed session credits,
isolated/rollback disposition, snapshot suffixes that remain `Ready`,
pre-effect newly represented batch rollback without rewinding an older emitted
session prefix, non-present prefix submission, shutdown and device-loss release,
and stale-reference rejection. For the SegmentSerial lane it must additionally
model one PE event mapping to a bounded ordered source-segment group, atomic
group publication/abort, exact event-local record partition, per-segment
completion, and final event settlement only after the last segment. It must
prove that an incomplete or failed segment cannot leave an orphan Ready source
or bypass a younger event. It must model ordered Present/Flush/direct-
observation/producer-sequence-wait/fixed-cap/semantic release-event fences and
prove that they do not overtake older raw or ready work. It must prove bounded
resident sources/pages, no page reuse before ordered reclaim, no ready source
that references unsealed storage, no completion before submission, no
worker-arrival or completion/GPU-timing release input, identical session grouping
under different completion schedules, no pressure-created release, and no cycle
in which admission pressure prevents completion or reclaim needed to clear that
pressure.

**R-VERIF-2.15** Formal or equivalent refinement evidence must prove parallel
and joint completion for R-BACK-2.49, R-BACK-2.63, and R-BACK-2.64: child or
segment order refines the serial partition order, coordinator join precedes
logical-pass finalization, and per-source completion expands only after every
Metal segment containing represented work completes.

`tla/ParallelDrawBinding.tla` covers the source-local R-BACK-2.63 slice: child-
local binding generations and dirty shadows, direct-binding PSO ABI, exact
serial draw ownership, ordered child join, parent end, and completion. Metal 4
multi-segment completion and cross-source parallel grouping remain separate
open refinements; this bounded model must not be cited for those claims.

SegmentSerial source identity is a separate obligation: a pass-piece sequence
may cross an identity-segment edge only when event-local record coverage is
contiguous and pass identity is unchanged, while segment completion still
joins in strict source order. Physical Arena blocks and Metal 4 segments must
not be substituted for this identity proof.

**R-VERIF-2.16** Parallel policy verification must separate semantic safety
from economics. A safety certificate must refine the sealed serial stream
without consulting a cost score, and an economics record must rank only
already-safe candidates. No score, benchmark observation, worker count, or
Metal capability may turn an unproved candidate into an eligible one.

**R-VERIF-2.17** The policy proof must establish ordered contiguous exact
coverage: child ranges are source-qualified, non-overlapping, and complete;
their flattened draw sequence equals the serial draw sequence exactly once;
`DrawRun` parameter ranges have no gap or partial tail; and every coordinator
command remains at its serial position and outside child ownership. Invalid,
stale, duplicate, missing, or overflowed coverage is a pre-effect serial
fallback.

**R-VERIF-2.18** The policy proof must be pass-wide. It must include complete
attachment/sample identity and exact hazards, one action epoch, one supported
route and binding ABI, and a complete first-draw snapshot for every child.
Fragment-local evidence is insufficient. The proof must also establish that
the coordinator retains ownership of actions, sidecars, completion, parent
end, and command-buffer/segment lifecycle.

**R-VERIF-2.19** The economics and selection relation must use checked,
deterministic fixed-point arithmetic over a declared finite integer domain.
Overflow, invalid normalization, or inconsistent accounting must select the
serial fallback. Selection must be independent of evaluation order, timing,
worker arrival, GPU progress, allocation addresses, and floating-point
rounding; equal safe scores must tie-break to fewer children and then a stable
canonical range vector.

**R-VERIF-2.20** The policy relation must be monotone under added negative
evidence: adding a coordinator command, hazard, attachment ambiguity,
action-epoch or ABI mismatch, missing first-draw fact, stale generation,
capacity violation, or overflow may preserve or shrink the safe set but may
not introduce a parallel candidate. The relation must be checked for both
accepted and serial-fallback outcomes.

**R-VERIF-2.21** Evidence mechanisms have distinct owners. `ParallelDrawBinding`
and `RenderTapeParallelJoin` TLA+ models own reachable-state safety, child
interleavings, ordered join, and temporal progress. Exhaustive native or SMT
evidence owns finite static coverage, overflow, monotonicity, and selection
equivalence. Render Tape owns deterministic production-import/replay identity
and structural/output comparison. Metal readback owns concrete ABI, resource,
shader, and pixel behavior. No mechanism may be cited for a claim owned by
another without an explicit refinement link.

**R-VERIF-2.22** The algebraic and formal results do not authorize promotion.
The parallel policy must remain default-off, with serial fallback and existing
serial modes reachable, until the complete layered evidence bundle in
`R-VERIF-6.4` and the locality/performance gates in `R-BACK-2.50` pass. A
non-vacuous Render Tape or Metal result must not be described as a default-on
or performance-promotion claim by itself.

**R-VERIF-2.23** A composed ownership/progress specification must refine the
runtime stages `ProducerOwned -> RawOwned -> ReplayBorrowed -> FinalOwned ->
Encoding -> GPUInFlight -> Completed -> Reclaimed` and the
`TransactionalChunkSlotAssembler` sub-transaction. It must prove unique owner
or synchronous borrow at every stage; no borrow escape; reserve/build/commit or
exact reverse rollback; no publication before complete construction; no legacy
retry after a semantic/Metal effect; strict source/completion order; and
eventual reclaim under explicit worker, encoder, and GPU fairness assumptions.
It must model state-only, pre-effect reject, oversize compatibility fallback,
early payload retirement under a receipt, device loss, Reset/teardown, and
bounded pressure. Expected-failure configurations must independently remove
the borrow-return, rollback, publish-after-build, no-post-effect-retry, and
completion-before-reclaim premises.

**R-VERIF-2.24** The fixed-role `SegmentedTransportV1` refinement must model
exactly three ordered roles, `ReserveAll`/`AdoptAll` atomicity, and the later
immutable semantic-batch to pure count/dedup to `ExactFixed` CPU Tape path.
The bounded model must prove complete final-capacity reservation, pointer-free
publication after the trusted in-process recorder boundary, retain/capacity/
ledger conservation, one contiguous at-most-once pre-effect fallback, poison
after adoption or effect, strict FIFO settlement, and reclaim wake/progress.
Independent expected-failure configurations must expose partial adoption,
fallback after effect, lost checkpoint, partial ledger, and missing wake. This
model is protocol evidence only: it does not prove PE COM or bridge ABI bytes,
allocator internals, Objective-C/Metal behavior, or final pixels. The transport
and later Tape provider remain default-off until model-to-code, native/GPU,
wild, locality, and performance gates are complete.

**R-VERIF-2.25** Formal evidence for R-BACK-2.101 through R-BACK-2.103 must
compose source-range partitioning with final-slot lease ownership and the
R-VERIF-2.23 pipeline lifecycle. The bounded safety model must prove total and
ordered exact-once record coverage, immediate same-raw span succession,
witness reset on slot generation change, explicit draw-run closure at every
cut, one terminal Present at most, publication only after complete reserved SoA
construction, no fallback after any semantic or publication effect, and no
completion or reclaim before all spans of the raw settle. It must include
multiple separators, capacity rotation, slot reuse, and both CPU-ready session
dispositions for ordered control. Independent expected-failure configurations
must remove each of the span witness, separator effect cut, run closure,
Present-ordering gate, capacity bound, slot-generation reset, receipt-before-
commit, completion ordering, and wake/progress premises.

The rotation/provision/adoption sequence must complete before admission and
before the effect cut. Its admission witness must bind the final destination
slot and the slot, source, and storage generations observed there; destination
identity must remain stable through completion and detached-owner settlement
until reclaim releases the slot. Shared semantic spans backed by one physical
allocation must account for that credit behaviorally, with independent orphan
and duplicate-owner counterexamples when the claim is made.

The production-observer projection must model pre-effect cancellation after
successful admission as row absence with restoration of the preceding
admission frontier, not as a retained terminal row. The model-code truth table
must reject a non-tail cancellation without state mutation and must accept a
complete sibling group only as one tail-ordered atomic reduction. The existing
post-effect retry counterexample must remain independent and continue to violate
the no-fallback-after-effect invariant.
Cancellation and compatibility execution are distinct refinement steps: row
erasure releases the Direct destination immediately, while compatibility
publication and settlement advance the shared source FIFO only when every
preceding source has reached the corresponding frontier. The model must not
fabricate Direct completion or restore events for that external fallback.

Storage capacity must be modelled as a quantity, not a boolean premise, and the
model must distinguish the per-transaction exact reservation from the slot's
physical capacity (R-BACK-2.103, R-BACK-2.104, R-BACK-2.105). It must cover empty-slot
provisioning, the prohibition on growing a populated slot, slot-generation
advance across a publication, and **boundary credits** against a
same-capacity serial reference consuming the identical source sequence: Direct
must never publish more often than that reference. The provisioned amount must be the
budget-fixed one the implementation uses, not a per-source proportional one, and
the model's input must be a nondeterministic **set** of adjacent source
sequences covering heterogeneous shapes -- a tiny leading span before
budget-sized ones, a leading span above the budget, and sources at and beyond it
-- rather than one deterministic literal, which is a simulation and cannot
express first-span sensitivity at all. Independent expected-failure
configurations must remove the provisioning premise -- restoring exact-fit
reservation, which is the measured regression -- and the empty-only premise,
which reallocates a published extent. The growth action must not be the only
enabled move when the empty-only premise is removed: rotation stays enabled
beside it so the model *chooses* to grow. The reference in this model is a
same-capacity serial slot rather than the production Legacy lane, whose chunk
command limit is unbounded by default; parity against an unbounded reference is
an explicit open obligation, not a discharged one.

The native model/code binding must derive its inventory from the same
compile-time role-tagged schema as production and assert exactly 32 semantic
regions, 31 physical capacity rows, and 30 staged allocation/fault rows. It
must exercise readback coverage non-vacuously while retaining its structural
rejection, preserve the lookup-family special cases, and prove that only the
shader-layout row may detach. Production reserve, pricing, coverage, clear,
swap, and fault operations must expand to straight-line field access; a runtime
descriptor loop is not equivalent evidence for the hot path.

The scalar-capacity refinement is not allocator or process-memory evidence.
Its promotion obligation must compose the 64 physical compatibility payloads
owned by `queueCompatibility(32)`, not the 32 control shells, with an aggregate
retained-capacity lease keyed by `(payload index, capacity generation)`. Before
positive-headroom staging, all 64 physical payload capacities must be
reconciled through a scalar CpuReadyTape inspection; an over-limit observation
is denied. Stage/Adopt/Rollback use an explicit generation-qualified operation
ticket and reject stale or duplicate settlement. The model must cover
replacement as a fallible staged transaction whose transient credit
includes old plus new capacity: either all 21 vectors and all nine uniform
lookup allocations are adopted into an empty payload and the old generation's
credit is released, or the live payload and retained credit remain identical,
staged credit returns to zero, and ordinary exact-fit replay proceeds.
Expected-failure coverage must independently expose partial adoption and leaked
aggregate credit. Native injection through the real production primitive must
bind every allocation point and exact rollback topology; opt-in Mach RSS/VM/
physical-footprint and low-4-GiB mapped/largest-gap evidence owns the memory
binding. TLC success alone cannot discharge either obligation.

Reclaimed-payload reuse must be a distinct transition rather than an
unobservable shortcut around provisioning. The model must retain physical
capacity per payload across reclaim and permit allocation-free reuse only when
every final-slot allocation dimension covers the new plan, any owner-bearing
storage detached for out-of-lock destruction has returned, and the payload's
generation-qualified ledger exactly matches its current retained bytes with no
staged credit. Independent expected-failure configurations must make redundant
reprovisioning, incomplete detached storage, and stale-ledger reuse observable.
The native binding must enumerate the production allocation inventory, prove
allocation/address stability across reuse and lookup restoration, and exercise
the actual reclaim round trip. A same-build wild run with non-zero reuse owns
reachability and memory/locality evidence; it does not replace the formal or
native premises and does not by itself promote the default-off policy.

Admission and detached-owner capabilities require explicit linearity evidence.
A private-factory move-only admission witness must be minted once after
admission and consumed once at the pre-effect destination handoff; moved-from,
stale non-command capacity, wrong issuer, same-address ABA, and double-consume
cases must fail closed. The move-only detached-owner token must bind the full
payload/source/sequence/Tape identity, storage and capacity generations, an
explicit exact-fit-or-qualified receipt, bytes, and a named typed
`DrawShaderLayout` allocation identity; restore rejection must retain the
token until explicit poisoned-path abandon and must not complete reclaim.
Native tests must bind these concrete fields and ledger conservation. The
formal lifecycle projection must expose matching witness-consumption and
token-disposition states with independent stale/duplicate, reorder,
post-effect retry, early reclaim, phantom/leaked credit, missing restore, and
partial-adoption counterexamples.

The Direct source lifecycle must have one pure reducer shared by production
observer boundaries and native truth tables. It must cover RawOwned import,
plan/admission, effect cut, destination receipt/publication, encode/completion,
detach, restore or explicit poisoned abandon, and reclaim. A bounded
two-source/two-slot refinement must include separator/ordered-control modes,
Present, rotation/reuse, weak-fair settlement, exact FIFO and exactly-once
invariants, no fallback after effect, completion-before-reclaim, and aggregate
retained plus staged plus detached credit conservation.

A separate weak-fair progress configuration must prove that an admitted raw
eventually settles, fails terminally, or takes one pre-effect compatibility
fallback when replay, encode, GPU completion, and reclaim agents continue to
run. Disabling deadlock checks without an equivalent terminal-state predicate
does not satisfy this obligation.

Native model/code isomorphism must invoke the same pure span-lifecycle reducer
as production. Direct calls to production planning and admission predicates
are valid bindings for those predicates, while a manually mirrored test-only
transition system is not proof that production begin/commit/separator/failure
actions implement the model. Exact SoA capacity, layout, payload bytes, and
post-state equivalence remain native/property-test obligations, and concrete
Metal resource, shader, attachment, and pixel equivalence remains a GPU-oracle
obligation under R-VERIF-1.7.

---

## 3. Resource Lifetime

Traceability: R-BACK-5.6, R-BACK-7.3

**R-VERIF-3.1** The formal spec must prove that a resource's underlying
Metal object is never freed while the GPU is still executing commands that
reference it (no use-after-free).

**R-VERIF-3.2** The formal spec must prove that `FreeResource` is only
reachable from `DestroyPending` when `completedSeqId >= lastUsedSeqId`.
No other path to the Freed state may exist.

**R-VERIF-3.3** The formal spec must prove that a resource in
`DestroyPending` is eventually freed once the GPU has drained past its
last-use sequence ID (no permanent leak).

**R-VERIF-3.4** The formal spec must prove that (slot, generation)
tagged-handle lookups are ABA-safe: an unregistered id never resolves
to a re-handed occupant of the same slot, and a lookup of an id always
returns the entity in exactly the slot the id points at. This applies
to `HandleArena` today and to any future `PresenterSlot`-style registry
that hands out stable opaque ids across threads. Coverage is provided
by `tla/PresentIdAba.tla`.

**R-VERIF-3.5** The formal spec must prove the cross-side
stable-identity invariant on the PE → unix command-chunk bridge:
the device-local `WireObjectRegistry` must admit a wire entry only when its
nonzero full-`uint32_t` generation and exact kind/object ID match the live slot,
must reject stale generations and wrong kinds before any retain or record
dispatch, and must retire a slot at `UINT32_MAX` instead of wrapping. Coverage
is provided by `tla/WireObjectRegistry.tla`.

**R-VERIF-3.6** The formal spec must prove that the
`PresentDrawableToken` stash → wait → take → complete/fail handoff
between the PE thread, the async-acquire worker, and the encoder
thread is race-free: `complete()` and `fail()` are mutually
exclusive and single-use, `take` from the queue's slot is single-use,
and `waitDrawable()` eventually returns under fair fulfilment.
Coverage is provided by `tla/DrawableToken.tla`.

**R-VERIF-3.7** The formal spec must prove the backing-version lifetime
contract for a logical MANAGED buffer: a draw stamps the exact concrete
backing captured by its packet; writable upload selects only a backing whose
last-use sequence has completed or allocates a fresh backing; the logical
last-use watermark covers every concrete backing watermark and never decreases;
and logical destruction cannot release any backing still referenced by queued
or in-flight GPU work. Coverage is provided by
`tla/BufferBackingVersioning.tla`.

---

## 4. Encoder Lifecycle

Traceability: R-BACK-2.4, R-BACK-2.6

**R-VERIF-4.1** The formal spec must prove that at most one
`MTLCommandEncoder` of any kind is active at any point in the command
stream (mutual exclusion).

**R-VERIF-4.2** The formal spec must prove that a transition between two
different encoder kinds (Render → Blit, Blit → Compute, etc.) always
passes through the `None` state, i.e., `endEncoding` is always called
before opening a different kind.

**R-VERIF-4.3** The formal spec must prove that a Render encoder's active
render target is always a valid non-null render target whenever the encoder
is in the Render state.

**R-VERIF-4.4** The formal spec must prove that when an exact read/write resource
hazard is detected, no further draw merging into the existing encoder is possible
until the encoder is ended and a new one is opened.

---

## 5. Query Resolution

Traceability: R-CORE-8.1, R-CORE-8.2, d3d9/queries/spec.md §2–3

**R-VERIF-5.1** The formal spec must prove that a query is never
transitioned to the Resolved state before `completedSeqId >= issuedSeqId`
(premature resolution is impossible).

**R-VERIF-5.2** The formal spec must prove that every Issued query
eventually resolves (no permanent S_FALSE).

**R-VERIF-5.3** The formal spec must prove deadlock freedom for the
busy-wait pattern `while (GetData(D3DGETDATA_FLUSH) == S_FALSE) {}`.
Specifically: if a query is Issued and a FLUSH is requested, the system
eventually reaches Resolved without the Wine thread being permanently
blocked.

---

## 6. Verification Evidence

**R-VERIF-6.1** Each TLA+ specification must be checked exhaustively by
TLC with the model parameters defined in the corresponding `.cfg` file.
TLC must report zero errors and zero invariant violations.

**R-VERIF-6.2** The TLC model parameters (ring size, inflight limit,
etc.) must be documented with their production counterparts so reviewers
can assess coverage.

**R-VERIF-6.3** Each C++ implementation of a verified subsystem must
include debug-mode assertions (`DXMT_ASSERT`) for every safety invariant
in the corresponding TLA+ spec. The assertions must reference the TLA+
invariant name in a comment.

**R-VERIF-6.4** Evidence for a stateful rendering optimization must be recorded
as a layered promotion bundle: semantic/reference contract; bounded formal or
exhaustive transition evidence; model-to-code isomorphism or shared-predicate
tests; deterministic GPU-visible evidence when applicable; then supervised wild
correctness and performance evidence. Missing layers and deliberately unmodeled
observables must be recorded in the owning `gap.md`; no later layer silently
substitutes for an earlier one.

**R-VERIF-6.5** The chosen verification mechanism must match the claim and name
its finite domain. Exhaustive native tests enumerate every input/state tuple in
a declared bounded domain while executing the production pure predicate. TLA+
and TLC verify reachable state transitions, interleavings, safety, and liveness
in a finite abstract protocol. An SMT solver may prove satisfiability,
unsatisfiability, or equivalence of bounded value constraints and static policy
decisions. `UNSAT` proves only that the encoded formula has no counterexample
within its stated assumptions; it does not prove Metal behavior, final pixels,
runtime scheduling, or performance economics. A lane must not cite one method
as evidence for a claim owned by another without an explicit refinement link.

**R-VERIF-6.6** A Render Tape reference replay may serve as deterministic
GPU-visible evidence only after its capture consistency, object generations,
resource closure, event ordering, production-import path, and output oracle are
themselves verified. The first full-tape implementation must bind a bounded
capture/replay model to production predicates with native tests, and must replay
at least one complete Present interval twice with identical structural
conservation and output hashes. Existing draw-slice mini replay remains valid
for its declared window but does not satisfy this full-frame requirement.

For `dxmt9.render_tape.identity.v2`, native evidence must additionally bind the
production validator to exact event-to-segment record partitioning, strict
flattened identity order, pass-piece continuity across segment edges, atomic
whole-event failure, and per-segment completion/final event settlement. A v1
artifact or event-order-derived mapping cannot satisfy this evidence.

**R-VERIF-6.7** PE recorder settlement evidence must include one bounded
composition model that crosses append-capacity, emitter rollback, accepted
record sealing, bridge-effect disposition, every capture disposition, pending-reference
drain, alias-before-parent destruction, builder reset, warm-retainer advance,
emitter acceptance, every production CapacityPost outcome, discard, and
successful device-reset recovery. The evidence must bind generated production
rows or production-used pure predicates. Its semantic state-write projection
must conserve category/key/value/ordinal-qualified pending tokens exactly. The
heterogeneous generic append envelope must issue a producer-qualified source
ordinal before capacity settlement and, after acceptance, synchronously bind
exact payload bytes plus every qualified handle identity to the exact record
ordinal and represented byte range. Its complete producer inventory must
generate the bounded formal family table. A disabled observer retains no
ledger and may pay only its nullable-owner branch; record type, byte size,
count, or a hash without collision proof remains an invalid surrogate semantic
token. The bundle must include independent expected
counterexamples for retrying an effect-unknown bridge failure, consuming an
unattempted capacity-pre record, retracting capture after command acceptance,
and resetting before drain completion.
Capacity-pre success must settle an already-full builder across that complete
sequence before the proposed emitter may consume its token. Any liveness claim
must be limited to fair internal progress after successful settlement, not
retry/failure outcomes controlled by the environment.

The same evidence bundle must include a repeated StateBlock value model over at
least two category-qualified tracked values and one untracked category for two
or more Capture/Apply cycles. It must prove frozen tracked-set preservation,
successful Capture refresh, pre/backend failed Capture preservation, live
mutation between Capture and Apply, untracked isolation, latest-snapshot Apply
publication only on success, pre-effect Apply preservation, and post-entry
Apply fail-stop. Abstract
cardinality does not replace native fake-COM evidence for exact per-slot
AddRef/Release/transfer multiplicity.

---

## 7. Data-Oriented / DXMT Merge Acceptance

Traceability: R-BACK-2.7-R-BACK-2.27, R-CORE-11.14-R-CORE-11.18,
R-BENCH-2.3-R-BENCH-2.5

**R-VERIF-7.1** Command chunk wire records must have a layout acceptance test
that pins `sizeof`, `alignof`, command IDs, version constants, fixed header
offsets, and every variable-tail length rule. Any intentional wire-schema change
must update the acceptance evidence in the same change.

**R-VERIF-7.2** Imported-record validation must be testable without Metal or
Wine. Fake byte buffers must cover valid records, malformed sizes, truncated
tails, unknown command IDs, record-count mismatches, and stale or null handles.

**R-VERIF-7.3** Queue execution must expose a deterministic observer or
fake-backend hook for tests. The hook must record chunk sequence IDs, retained
resource handles, replay category, barrier/readback boundaries, and encoded
command kind order without depending on sleeps, real GPU timing, or Metal side
effects. The production hook must be one cold nullable sink selected once per
`encodeChunk` effective stream or fragment. A serial callback occurs after
range, DCE/permutation, partition, and fallback selection and immediately
before that selected command's encoder effects. A selected parallel batch may
publish its covered commands in effective order after every proof/economics/
fallback gate and immediately before the first child encoder effect; a
source-wide pre-pass is forbidden. Each callback must include source identity/
storage generations, original command ordinal, `MetalCommandKind`, replay
category, barrier/readback flags, and an exact synchronous call-local span of
retained `ChunkHandleEntry` values. Disabled observation must cost at most one
cached null branch and perform zero observer storage, allocation, or resource-
visitor work; the hook must not retain borrowed spans or truncate an enabled
observation at a fixed capacity.

**R-VERIF-7.4** DXMT concept mapping acceptance must be explicit: every hot-path
owner in the README mapping has a corresponding implementation owner and test
evidence. In particular, PE state shadow, POD chunk construction, unix import,
queue-owned execution, presentation pacing, and deferred resource safety must not
collapse into ad-hoc cross-boundary state.

**R-VERIF-7.5** Bridge-operation budget evidence from benchmarks must be linked
to verification results. A regression from chunked submission to per-state or
per-draw bridge calls is a DXMT merge-readiness failure even if rendering output
remains correct.

**R-VERIF-7.6** The production queue must expose a deterministic lifecycle
observer for R-BACK-2.88. A native fake-backend fixture must conserve one exact
identity and generation from Raw adoption through replay borrow, final
publication, serial and selected-parallel encode borrow, receipt/submission,
ordered completion, and reclaim. It must cover pre-effect rollback, state-only,
early payload retirement, device loss, stale/duplicate events, and reclaim with
an outstanding borrow, without sleeps, polling, real Metal timing, or a
test-only transition owner. The observer is separate from the effective-command
observer in R-VERIF-7.3 but may share its cold nullable-sink infrastructure.

**R-VERIF-7.7** Direct construction acceptance has four non-substitutable
layers. Native differential fixtures own PE blob byte identity and backend
effective command/payload/resource/failure equivalence. The R-VERIF-2.23 model
owns transaction and temporal refinement. Metal validation plus deterministic
readback owns concrete encoder-visible equivalence. Bounded x64/x86 Wine faults
own PE COM/bridge HRESULT, poison, retry, and retain behavior. The recorder's
heterogeneous semantic projection must bind exact category/key/value-or-
identity/source-ordinal/record-ordinal/range tokens across all record families;
record kind, byte count, or timing cannot substitute. Wild evidence is last and
must show the named copy-class reduction, bounded peak retention, conserved
commands/resources/completions, normal visual anchors, zero new GPU errors, and
no displaced wait or copy class before a default change.

**R-VERIF-7.8** The end-to-end CPU pipeline lifecycle model must retain a
bounded safety configuration and a separate small weak-fair progress
configuration. The progress configuration must prove eventual reclaim for
every admitted source, normal GPU completion and reclaim, explicit Present
settlement, and selected-parallel child join; it must not rely on an
unconstrained `[Next]_vars` stutter behavior. These proofs assume scheduler
fairness, GPU completion, the specified C++ atomic ordering, and driver
behavior; those assumptions are environment boundaries rather than model
proofs.

**R-VERIF-7.9** One composed model/code trace must bind R-ARCH-7.11 through
R-ARCH-7.24 across the real subsystem transitions. Its state must include the
complete `EndToEndSourceIdentity`, physical representation kind, storage
generation, lease/borrow count, sidecar generation, effect cut, completion
projection, reclaim authority, and capacity-wake generation. Native
isomorphism must compare the same transition relation against production PE
accept/import, direct and compatibility replay, serial and selected-child
encode, normal/no-GPU/device-loss completion, and reclaim. Deliberate
counterexamples must cover duplicate logical identity, partial adoption,
facade escape or reclaim with a borrow, retention of a PE span after bridge
return, false same-address adoption without reclaim transfer, stale
sidecar/locator use, retry after an effect, completion before child join,
double reclaim, and missing capacity wake. Component models remain useful but
cannot substitute for this composed trace.

For the bounded Direct runtime projection, `RawOwned` import and plan are
abstract model states rather than emitted runtime observations. The production
trace begins at successful `DirectSpanAdmissionWitness` admission and must not
fabricate Raw/Plan events. From that point, destination receipt precedes the
Direct physical commit, publication follows Tape seal under the queue mutex,
and Encode, Complete, Detach, Restore-or-Poison, and Reclaim must be bound to
their real owner transitions with full locator and atomic sibling validation.
On revalidation failure, a poisoned detached token remains retained until its
explicit terminal disposition.

**R-VERIF-7.10** Replay projection must have a model/code isomorphism over a
bounded persistent state, immutable source, source-local working state,
effective stream, exact physical plan, final emission, and state commit. The
production projection and native truth table must use the same pure transition
for representative A→B→A state, draw, resource-generation, and ordered-control
sequences. Counterexamples must reject state commit before successful emission,
partial destination publication, exact dedup that drops logical attribution,
optimizer output used as the next persistent state, and optimizer admission
without its typed proof/fallback certificate. A Legacy-versus-direct
differential must compare effective commands, next state, final SoA values,
resource/completion identity, and failure disposition. Retirement evidence must
also audit production sources and public backend APIs for the absence of
`DrawRunSubmission`, equivalent per-draw AoS carriers, carrier-specific
materialization counters, and carrier-only test seams.

The same evidence must reconcile the owner-qualified copy/materialization
ledger with R-ARCH-7.24: exactly one named PE semantic emission, at most one
current-ABI `RawOwned` import copy, one final-SoA construction per surviving
value/byte, and separately classified GPU-visible writes. It must reject hidden
queue/session gathers, repeated serialization, final-region relocation, and
Metal command/resource references misreported as known byte copies.
