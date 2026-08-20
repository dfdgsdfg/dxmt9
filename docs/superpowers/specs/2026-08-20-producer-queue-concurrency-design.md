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

## 7. Open questions (blocking T2 design finalization)

1. Capture read-set vs worker write-set (§2) — source audit of
   `captureChunkBufferBinding` and every worker-side writer of the fields it
   reads.
2. Exact pin release point vs commit return — is the retainer release
   provably after the last mark of the same chunk on every path including
   rollback/discard?
3. `map_buffer`'s rename-ring rotate under lock — which parts are
   producer-private (ring indices) vs shared (backing allocation), and does
   the DISCARD fast path need the queue mutex at all?
4. Does any reclaim path run outside `mutex_` today (destroy-and-gc from
   wrapper destructors on the producer itself) — if reclaim is
   producer-driven-only plus completion-driven, the actor set shrinks.
