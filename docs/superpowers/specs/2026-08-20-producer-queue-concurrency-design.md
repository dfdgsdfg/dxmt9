# Producer↔Queue Concurrency — Design Brief

Status: draft (design track opened alongside the segment-hold instrumentation;
this document is the durable statement of the problem, the candidate target
architectures, and the formal/harness plan. It precedes any implementation.)

## 1. Problem

The game thread (producer) measurably loses ~1.0 ms/present on GT2 waiting to
acquire `CommandQueue::mutex_` (`mark_and_capture` 0.60 + `map_buffer` 0.42;
`state-churn-encode-append-decomposition.28`). The wait is not caused by
acquire-frequency contention (removing the loudest 406-acquires/present site
changed nothing) but by long holders at lock-handoff sites. Independent of
which holder is trimmed first, the architecture itself couples four actors —
producer, replay offload worker, encode thread, completion path — through one
coarse mutex over 42 critical sections, and every future producer-side
optimization will keep colliding with it. A deliberate concurrency design with
formal backing is warranted.

## 2. Current-state inventory (from source, 2026-08-20)

Producer-owned mutex uses per present (GT2):

| site | freq | what it does under the lock |
|---|---|---|
| `mark_and_capture` (commit_chunk sync half) | ~16 | stamps `lastUsedSeqId` on every unique chunk resource (`pool_.mark*Use`), captures per-buffer binding snapshots, reads `nextSeqId` |
| `map_buffer` (buffer Lock path) | ~21 | reads `mapWaitSeqId`, may `commitCurrentChunk(lock,…)`, may cv-wait `waitForSequence`, then `finalizeBufferMap` (rename-ring rotate/alloc on DISCARD) |
| `submit_clear` etc. | ~9 | direct-call drains |

Reclaim protocol (`Pool::reclaimCompleted`, `gcArena`): a record is freed only
when `destroyPending && lastUsedSeqId <= completedSeqId`. `destroyPending` is
set by the unix object's destruction path (`mark*DestroyAndGc`), which runs
when the last unix reference drops. The `// TLA+ NoUseAfterFree` gate at
`dxmt9_resource_pool.cpp:169` asserts the watermark half of this.

**Pin-ordering safety candidate.** During `commit_chunk`, the PE recorder
retainer holds a unix reference on every resource the chunk names (warm
epochs extend this across chunk boundaries, `c7b3b141`). If pin lifetime
strictly contains the marking window, `destroyPending` cannot be true for any
resource being marked, so a reclaim racing a lock-free mark can never free a
being-marked record. This is the central argument a formal model must pin —
today it is enforced trivially by the shared mutex, not by the pins.

**Capture-consistency open question.** `captureChunkBufferBinding` reads
per-buffer state at commit time while the replay worker may be mid-replay of
*earlier* chunks. The mutex makes each read atomic but does not order capture
against worker progress. Either (a) capture's read-set is producer-written
only (upload/lock-driven backing + revision, worker never writes it), making
commit-time capture well-defined without worker ordering — or (b) it is not,
and today's behavior already depends on scheduling. Resolving (a)/(b) from
source is the first design task; the answer decides whether capture can move
off the queue mutex at all.

## 3. Candidate target architectures (increasing ambition)

- **T1 — shorten holders (in flight).** Segment-hold instrumentation, then
  trim the top holder's critical section. No semantic change; bounded win
  (producer still queues behind residual holds).
- **T2 — producer-lock-free marking.** `lastUsedSeqId` becomes a relaxed/
  release atomic max-stamp; `seqIdForMark`'s `nextSeqId` read is already
  atomic-izable. Safety rests on the pin-ordering argument (§2), which must be
  model-checked, not assumed. Binding capture stays under the mutex initially
  (it is 164 buffer reads/commit, a shorter section) or moves per §2's answer.
  Removes the producer's mark acquire entirely (~0.6 ms/present).
- **T3 — producer decoupling.** The producer never touches pool/queue shared
  state: commit posts a message; marking, capture, and map-wait resolution are
  owned by the worker/queue side; `map_buffer` becomes a bounded wait on a
  future/handle filled by the owner thread. This is the clean endpoint but
  restructures the Lock path's blocking contract (Wine visibility policy #66:
  Draw→Lock must observe the draw) and is not justified until T1/T2 evidence
  shows the residual is worth it.

## 4. Formal layer (temporal-logic guarantee)

New bounded model `ProducerMarkReclaim.tla` (+ .cfg), joining the existing
22-model suite:

- **Actors:** Producer (mark, capture, map), Retainer (pin/unpin with warm
  epochs), Worker (replay, bind, submit), Completion (advance
  `completedSeqId`, reclaim).
- **State:** per-resource `{pinned, destroyPending, lastUsedSeqId}`, global
  `nextSeqId/completedSeqId`, a small resource set (2 resources, A→B→A
  patterns per `rendering_correctness.rules.md`).
- **Safety:** `NoUseAfterFree` — no reclaim of a resource whose mark is
  in-flight or whose pin is held; `MarkMonotonic` — stamps never regress;
  `MapVisibility` — a map that returned observes every draw sequenced before
  the lock (models Wine #66 as an ordering obligation, not a pixel claim).
- **Purpose-built counterexample:** run the model with the pin-ordering
  premise *removed* (a `Buggy` cfg, like `NoOverwriteByteRange.counterexample`)
  and confirm TLC produces the use-after-free trace — the model must be able
  to see the bug class it guards, or it proves nothing (this addresses the
  recorded `ResourceLifetime.tla` scope debt: refcount-held references are
  invisible to the watermark-only model, `specs/verification/gap.md`
  2026-08-02 row).

## 5. Semantic-isomorphism harness (fast native testing)

Three layers, strongest first:

1. **Shared pure predicates.** `canReclaimRecord(pinned, destroyPending,
   lastUsedSeqId, completedSeqId)` and `markStampUpper(current, stamp)`
   extracted as header-pure functions used by BOTH production code and the
   native spec — the TLA actions reference the same predicate names (the
   repo's established binding pattern).
2. **Trace-replay runner.** A native spec that replays *TLC-emitted traces*
   (action name + arguments per step) against a miniature C++ harness holding
   the real predicates and a scripted scheduler — a TLC counterexample
   becomes a deterministic failing native test without hand-translation.
   This is the "semantic isomorphism" lane: model and code share the action
   vocabulary, so traces are executable.
3. **Deterministic interleaving stress.** A fake-backend queue-observer test
   (the open `R-VERIF-7.3` evidence gap) driving producer/worker/completion
   steps from a seeded schedule across the real `CommandQueue` with a null
   backend, asserting the shared predicates at every step. This is the layer
   that catches C++-level races the pure predicates cannot express (torn
   reads, ordering of the actual atomics).

## 6. Evidence order (per rendering_correctness.rules.md)

1. State serial semantics + invariants (§4) — this document plus a
   requirements row.
2. Model the smallest distinguishing trace; safety first; the Buggy cfg must
   fail.
3. Bind transitions to C++ via the shared predicates + trace-replay runner.
4. GPU oracle: not required for T2 (no pixel-path change) beyond the existing
   conformance suite; T3 would need the Lock-visibility conformance cases
   (`vertex_buffer_read_write`, `writeonly_vertex_buffer_readback_policy`) as
   its oracle.
5. Wild: GT2/GT1/GT3/SFIV matched pairs; the win prediction (~0.6 ms for T2)
   is 2× the measured build-layout noise floor, so whole-build A/B is valid.

## 7. Open questions — RESOLVED by source audit (2026-08-20)

1. **Capture read-set: producer-written-only.** Every field
   `captureChunkBufferBinding` reads (`isDynamicRename`/`isManagedVersioned`,
   `buffer`, `contents`, `desc.size`, `contentRevision`, `renameActiveIndex`,
   `renameRing[].replayResidency`) is written exclusively on the game thread
   (create, Lock/Unlock upload, finalizeBufferMap). The worker/encode side
   never reads the live ring — it consumes only the immutable commit-time
   snapshot. Capture is well-defined without ordering against worker progress.
2. **Pin release is strictly after same-chunk marking on every path** —
   commit_chunk is synchronous on the game thread; `endEpoch()` runs only
   after a successful return; failure keeps pins; discard paths only touch
   never-committed chunks. Caveat: a pin release can synchronously drive
   destroy-and-gc/reclaim on the producer itself.
3. **Rename ring is producer-private.** `renameRing`/`renameActiveIndex` are
   referenced only inside the pool; the DISCARD fast path needs the queue
   mutex only for the `completedSeqId_` read (atomic-izable) and, in the
   Fresh case, the `device.newBuffer` call. The map slow path (visibility
   wait, Wine #66) legitimately needs commit/cv machinery.
4. **The reclaim actor set is THREE-way, not two** — producer (wrapper
   release), completion loop (`runFinishLoop` → `reclaimCompleted`), AND the
   replay offload worker (`releaseRetainedWrappers` on replay completion/
   fail-stop drops last refs → destroy-and-gc on the worker thread). All
   under `mutex_` today. `ProducerMarkReclaim.tla` must model a
   Worker-reclaim transition; §2's inventory understated this.

## 8. Post-audit T2 refinement

A load-bearing discovery: `dxmt9_resource_pool.hpp`'s header contract already
documents (a) lookup ops callable **without** the queue mutex (HandleArena
serializes slot metadata with its own shared mutex), (b) pointer stability
via `std::deque` slots plus the watermark, and (c) an existing exception
where a publisher **releases the queue lock and stamps retained objects
under the arena's own mutex** — i.e., queue-mutex-free marking is an already
documented, precedented pattern, currently with the explicit carve-out that
it "does not permit capture … outside `CommandQueue::mutex_`".

T2 therefore reduces to three provable relaxations, in increasing order of
contract change:

- **T2a — commit-time marking moves to the arena-stamp exception.** The
  producer stamps `lastUsedSeqId` under HandleArena's own mutex instead of
  `CommandQueue::mutex_`. Safety = pin-ordering (Q2, program order) +
  three-actor reclaim all still under either mutex with the watermark gate.
- **T2b — capture joins it.** Relax the header carve-out using Q1: the
  capture read-set is producer-written-only, and record existence during
  capture is pinned (chunk-named ⇒ retainer-pinned ⇒ not destroyPending).
  Requires the model to include the worker-reclaim actor erasing *other*
  records concurrently (deque slot stability covers the container).

  **Status: implemented** (see §9 step 2 for what the model review found and
  what it did not need).
- **T2c — map DISCARD fast path.** `completedSeqId_` becomes an atomic read;
  ring bookkeeping is producer-private; only the Fresh allocation and the
  slow visibility-wait path keep the queue mutex.

  **Status: implemented**, with one correction to the sketch above: the Fresh
  allocation does NOT keep the queue mutex. `rotateBufferBacking`'s
  `device.newBuffer` runs inside `finalizeBufferMap`'s single
  `arena.update()`, on `producer-owned` ring state, and Metal device buffer
  creation needs no queue lock — so the fast lane takes no queue mutex at all,
  Fresh case included. Only the slow visibility-wait path keeps it. See §9
  step 2.

Expected producer recovery if all three land: the full ~1.0 ms/present of
measured acquire-wait (mark 0.60 + map 0.42 common path), bounded below by
whatever the slow-path map waits keep. Each step is independently
measurable (≥2× the layout-noise floor for T2a+T2b combined; T2c alone is
borderline and should be bundled).

## 9. Segment-hold findings (2026-08-20, leaf .29) and the concretized plan

The instrumented holder ledger: worker slot-append copy 3.42 ms/present
(560 batches, max 9.9 ms), worker marking 0.93, slot publish 0.83,
encode submit 0.43, map finalize 0.28, producer mark body 0.16 — a ~17%
mutex duty cycle. The writing slot is not worker-exclusive (the producer's
map-wait force-publish reaches it), so unlocked appending is unsafe without
a protocol. Final implementation order:

1. **T2a' — marking to the arena-stamp exception, both actors** (worker
   0.93 + producer 0.16 + the producer's mark acquire disappears): extend
   `ProducerMarkReclaim.tla` with a symmetric `WorkerStampMark` action
   (same pin-ordering premise — the worker holds its chunk's retained refs
   until post-replay release), then move both mark paths off the queue
   mutex. Ticket (`seqIdForMark`) still taken under the queue lock or from
   an atomic `nextSeqId`.

   **Status: implemented.** Model first, per §6. The extension found a
   second premise the mutex had been supplying for free and that §9's
   original sketch did not name: **the ticket and the slot seq stop being
   the same read.** `seqIdForMark` returns `nextSeqId_`, i.e. the seq the
   open writing slot will be published under; taking it inside the hold
   that ends at the append made stamp == final chunk seq by construction.
   Outside that hold, a force-publish (`SlotAdvance` — the producer's
   map-wait commit, or the draw/payload chunk limits; §9's own "the writing
   slot is not worker-exclusive") can raise the seq in between, leaving the
   stamps BELOW the chunk's final seq. The watermark then passes them while
   that chunk is still pending and `gcArena` frees a record the encoder is
   still going to read. Note the failure mode: a **premature reclaim**, not
   a dereference of a freed record — so the `PinDiscipline` dimension could
   never have exposed it, which is why it needed its own Buggy dimension.

   Delivered:
   - Model: `WorkerBeginBatch` / `WorkerStampMark` / `WorkerEndStamping` /
     `WorkerRestamp` / `WorkerAppend` / `WorkerReleaseBatchRefs` /
     `WorkerRetireBatch`, plus `SlotAdvance` and the `RestampDiscipline`
     constant. New invariant `WorkerAppendCoveredByStamps` states the
     obligation directly; `CommitStampsCoverChunkSeq` states the producer's
     counterpart, which holds without a re-stamp because `commit_chunk`
     reserves the ticket and owns the chunk published under it.
     Production cfg green (1,140,594 states generated / 276,840 distinct /
     depth 29). Both counterexample cfgs produce
     `Invariant NoUseAfterFree is violated`: `PinDiscipline="Removed"` in 3
     steps, `RestampDiscipline="Removed"` in 9
     (`WorkerBeginBatch → SlotAdvance → WorkerStampMark →
     WorkerEndStamping → WorkerAppend → AdvanceCompleted →
     WorkerReleaseBatchRefs → SetDestroyPending → Reclaim`).
   - Ticket source: `CommandQueue::nextSeqId_` is `std::atomic<u64>` with
     `markTicketAcquire()`; every write stays where it was and still runs
     under `mutex_`, now with `release`.
   - Producer path: `markChunkResources` and
     `markChunkResourcesAndCaptureBufferBindings` stamp before acquiring,
     then keep one acquire for the frozen-ticket re-read (rare re-stamp) and
     the capture loop. Stamps-before-capture, because a monotone-max stamp
     that is too low is repairable and an early capture is not. On the
     producer path the re-stamp is *insurance*, not a proven requirement:
     `commit_chunk` reserves the ticket and its own publish, and every
     append path re-marks under the mutex with its own ticket
     (`skipDrawResourceMarking_` has no production setter, so per-draw
     marking is unconditionally live). It costs one acquire load.
   - Worker path: `submitDrawRunBatchImpl` releases the mutex around the
     per-batch stamp loop, re-establishes the writing slot, then re-stamps
     if the frozen ticket moved before appending — generalizing
     `forceDrawResourceMarkingAfterSplit_`. The
     `submit_draw_run_batch_impl/mark` segment leaves the hold ledger.
   - Contract: the pool header's arena-stamp exception now names its three
     production callers and states both obligations.

   Not done here: capture (T2b) stays under the mutex, the slot append
   (T2d) is untouched, and `CommandQueue::submitDrawRun` (the non-batch
   path) keeps its mark under the mutex. Evidence still owed: the profile
   re-measurement and the wild matched pair (step 3).
2. **T2b + T2c — capture and the map fast path.**

   **Status: implemented.** Model first, per §6, and the review's first
   finding was that the model needed no structural change: `CaptureRead` was
   already guarded only by the commit phase and per-record bookkeeping, never
   by a phase implying mutual exclusion with reclaim, and it already
   interleaved freely with `Reclaim` / `SetDestroyPending` /
   `AdvanceCompleted` / `WorkerReleaseRefs` / `SlotAdvance`. §8's T2b
   requirement — "the worker-reclaim actor erasing *other* records
   concurrently" — was already reachable. So the T2b work was to make the
   obligation SAYABLE rather than to make it true.

   Delivered:
   - Model: two new invariants over existing variables, so no new variable
     and no new `UNCHANGED` clause. `NoCaptureAfterFree ==
     \A r \in captured : ~freed[r]` is two-sided on purpose (it fails both
     when a capture touches a freed record and when a reclaim frees an
     already-captured one). `StampsPrecedeCapture == captured \subseteq
     marked` promotes §4's third protocol from a `TypeOK` conjunct to a named
     invariant, because T2b is what turns it from a side effect of one mutex
     hold into a real obligation.
   - Counterexample: **no new Buggy axis.** `PinDiscipline="Removed"` already
     exercises the capture-side violation, because the pin chain IS the
     capture's only safety premise beyond Q1's audit — a
     `CapturePinDiscipline` constant would delete the same conjunct under a
     second name. `ProducerMarkReclaim.capture.counterexample.cfg` therefore
     differs from its sibling only in which invariant it asks TLC to report,
     and that difference is load-bearing: `NoUseAfterFree` trips three steps
     in and would shadow the capture trace, while listing only
     `NoCaptureAfterFree` produces `PinChunkResources → BeginMark →
     SetDestroyPending → Reclaim → StampMark → CaptureRead`. Note the mark
     and capture run two steps AFTER the free — the production loops walk the
     chunk's resource list and re-check nothing, which is exactly why the pin
     has to be the guarantee rather than a liveness test.
   - Production cfg unchanged at 1,140,594 generated / 276,840 distinct /
     depth 29 (the new invariants add no state).
   - T2b: `Pool::captureChunkBufferBindings` owns the loop, so the
     R-BACK-43.5 assert lives with the token it uses. The per-record
     `captureChunkBufferBinding` stays assert-free — `snapshotBufferBinding`
     forwards to it from the replay worker's draw-run snapshot path, a
     legitimate non-producer reader. `markChunkResourcesAndCaptureBufferBindings`
     keeps one acquire around the frozen-ticket re-read and nothing else, and
     that acquire is scoped to close BEFORE the capture so
     stamps-before-capture stays literally true of both stampings.
     `CommandQueue::captureChunkBufferBindings` loses its acquire entirely.
   - T2c: `completedSeqId_` is `std::atomic<u64>` (release store at the one
     completion-loop writer, still under `mutex_`; `completedSeqIdLocked()`
     relaxed for mutex holders, `completedSeqIdAcquire()` for the fast path).
     `mapBuffer` splits on `mapWaitSeqId == 0`, which is safe to decide
     unlocked because that read-set contains no `queue-shared` state — and
     `mutex_` had already stopped protecting `lastUsedSeqId` at step 1. The
     slow lane re-reads `mapWaitSeqId` under the lock so its behaviour is
     byte-for-byte the pre-T2c one.
   - Contract: the pool header's carve-out ("does not permit capture …
     outside `CommandQueue::mutex_`") is replaced by three enumerated,
     exhaustive exceptions with their individual obligations.
   - Observability: `mark_ticket_restamp_checks` / `_fires` closes the
     "Restamp-fire observability" gap row.

   **What did NOT happen, and why it is a gap row rather than an oversight:**
   the producer's commit path still takes ONE acquire. `seqIdForMark` READS
   `nextSeqId_` where `ProducerMarkReclaim!BeginMark` RESERVES it
   (`nextSeqId' = nextSeqId + 1`). That reservation is what makes
   `CommitStampsCoverChunkSeq` hold with no re-stamp, so the model's producer
   premise is stronger than production and §9 step 1's "the re-stamp is
   insurance, not a proven requirement" is exactly right — but insurance
   needs a frozen ticket, and a frozen ticket needs the mutex. Removing that
   last acquire means either making the ticket a real reservation and
   re-checking the model against it, or proving the read sufficient. Tracked
   in gap.md as "Producer commit acquire not fully removed".

   Evidence still owed: the profile re-measurement and the wild matched pair
   (step 4).
3. **T2d — reserve-copy-commit slot append**: bump-allocate + ticket under
   the lock, copy outside, brief re-acquire to advance the slot's committed
   watermark; publishers ship only the committed prefix. Requires a bounded
   model of append/publish/force-publish interleaving (QueueLifecycle family
   extension) with a Buggy cfg showing the half-appended-slot escape when
   the watermark rule is removed.
4. Re-profile with the same segment instrumentation (duty-cycle target
   ~2-3%), then the wild fps pair. Rows to watch after T2b/T2c:
   `map_buffer` acquire count (the fast lane should remove most of them,
   which is why the count and not just the hold moves),
   `capture_chunk_buffer_bindings` acquires → 0,
   `mark_chunk_resources_and_capture_buffer_bindings` hold → near-zero with
   its acquire count unchanged, and `mark_ticket_restamp_fires /
   mark_ticket_restamp_checks` as the first wild reading of the re-stamp
   window.

## 10. Track closure (2026-08-21)

T2a'/T2b/T2c shipped model-first; the producer's queue-mutex footprint fell
to the frozen-ticket re-read plus legitimate visibility waits, and the
restamp window measured 1,066,758 checks / 0 fires on GT2. Cross-workload
profiles (GT1 total acquire-wait 0.126 ms/present, SFIV 0.089, GT2 ~1.0 with
producer share ~0.15) show no remaining producer contention anywhere, so
**T2d is deferred**: its fps motivation dissolved once the producer stopped
acquiring, and the holder's duty cycle has no waiting victim. T2d's
architectural value (duty-cycle reduction, live slot-assert, encode-side
concurrency precursor) is recorded in the gap row with its reopen
conditions. The multi-workload gate (GT1/GT3/SFIV pass, zero GPU errors,
visuals normal, SFIV in-benchmark average 43.0 → 47.0) closed the
GT2-only evidence debt for the whole series.

