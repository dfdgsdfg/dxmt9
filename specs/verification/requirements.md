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
progress required to release that pressure. Producer quiescence without such an
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
and stale-reference rejection. It must model ordered Present/Flush/direct-
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
effects.

**R-VERIF-7.4** DXMT concept mapping acceptance must be explicit: every hot-path
owner in the README mapping has a corresponding implementation owner and test
evidence. In particular, PE state shadow, POD chunk construction, unix import,
queue-owned execution, presentation pacing, and deferred resource safety must not
collapse into ad-hoc cross-boundary state.

**R-VERIF-7.5** Bridge-operation budget evidence from benchmarks must be linked
to verification results. A regression from chunked submission to per-state or
per-draw bridge calls is a DXMT merge-readiness failure even if rendering output
remains correct.
