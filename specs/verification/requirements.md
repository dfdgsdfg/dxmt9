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

Traceability: R-CORE-8.1, R-CORE-8.2, d3d9/queries/design.md §2–3

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
