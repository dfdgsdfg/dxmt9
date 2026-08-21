---- MODULE ProducerMarkReclaim ----
(*
 * dxmt9 Producer Mark / Reclaim — TLA+ Specification
 *
 * Licenses the T2a / T2a' / T2b relaxations of the producer↔queue concurrency
 * track (`docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md`
 * §8 / §9): commit-time resource marking, the replay worker's per-batch draw
 * resource marking, and the per-buffer binding capture that rides with the
 * former, move OFF `CommandQueue::mutex_` and run under HandleArena's own
 * mutex instead.
 *
 * TWO independent premises are checked, and each has its own purpose-built
 * counterexample configuration:
 *
 *  1. PIN ORDERING (`PinDiscipline`). During `commit_chunk` the PE recorder
 *     retainer holds a unix reference on every resource the chunk names, and
 *     the replay worker holds the retained wrappers for every resource its
 *     batch names until its replay is done. `destroyPending` therefore cannot
 *     be set for a resource that is being marked, so a reclaim racing a
 *     lock-free mark can never free a record a marker is still touching.
 *
 *  2. RE-STAMP DISCIPLINE (`RestampDiscipline`). Once the stamp value leaves
 *     the queue mutex, the seq TICKET it carries and the seq the records
 *     finally land under stop being the same read. The ticket is an acquire
 *     load of `nextSeqId_`; a concurrent force-publish / chunk split
 *     (`SlotAdvance`) can raise that seq between the ticket read and the
 *     append, leaving stamps BELOW the chunk's final seq. The GPU watermark
 *     then passes the stamp while the chunk that names the record is still
 *     pending, and `gcArena` frees it — a premature reclaim that the mutex
 *     used to make unreachable for free, because ticket and append happened
 *     inside one hold. The protocol that restores it is to re-read the ticket
 *     after re-acquiring the mutex and, if it moved, re-stamp before the
 *     append — a generalization of the existing production
 *     `forceDrawResourceMarkingAfterSplit_` mechanism
 *     (`src/dxmt9/dxmt9_command_queue.cpp`).
 *
 * Before T2a/T2b, the queue mutex supplied these exclusions incidentally; the
 * production contract now relies on HandleArena locking, retainer pins, and
 * the frozen-ticket re-read. This model checks the premises as standalone
 * ordering properties so narrowing the queue lock does not silently remove a
 * guarantee.
 *
 * Actors (all five run concurrently; no action pair is mutually excluded
 * except where a `commitPhase` / `workerPhase` guard says so):
 *
 *   Producer   — PinChunkResources → BeginMark → StampMark* / CaptureRead*
 *                → EndCommit; plus MapFastRead (T2c's atomic watermark read).
 *   Retainer   — the pin lifetime itself: pins are a *precondition* of
 *                marking (PinChunkResources) and are released strictly after
 *                EndCommit (ReleasePins), per design §7 Q2.
 *   Worker     — the commit-replay offload worker. Two roles, both modelled:
 *                (a) WorkerReleaseRefs models `releaseRetainedWrappers`
 *                dropping refs it inherited from an earlier chunk, which can
 *                land anywhere inside a later chunk's mark window (design §7
 *                Q4 — the worker is a *third* reclaim actor); and
 *                (b) WorkerBeginBatch → WorkerStampMark* → WorkerEndStamping
 *                → WorkerRestamp? → WorkerAppend → WorkerReleaseBatchRefs →
 *                WorkerRetireBatch is `submitDrawRunBatchImpl`'s per-batch
 *                marking + append, the T2a' half. Its pin premise is
 *                symmetric to the producer's: it holds its batch's retained
 *                refs across its own marking window and releases them
 *                strictly after the append.
 *   Publisher  — SlotAdvance: any actor that force-publishes the open writing
 *                slot (the producer's map-wait commit, the draw/payload chunk
 *                limits) and so raises the seq a pending append will get.
 *                Design §9: "the writing slot is not worker-exclusive".
 *   Completion — AdvanceCompleted (`completedSeqId` watermark) and Reclaim
 *                (`Pool::reclaimCompleted` → `gcArena`).
 *
 * The reclaim gate is modelled FAITHFULLY, not defensively: `Reclaim(r)`
 * tests only `destroyPending[r] /\ lastUsedSeqId[r] <= completedSeqId`, which
 * is exactly `gcArena`'s condition in `src/dxmt9/dxmt9_resource_pool.hpp`.
 * Pins never appear in that gate. They appear one step earlier, in
 * `SetDestroyPending(r)`: a pinned record has refs > 0, so its destructor
 * path cannot run. `PinDiscipline = "Removed"` deletes exactly that premise;
 * `RestampDiscipline = "Removed"` deletes exactly the re-stamp step. Each has
 * a companion counterexample configuration expected to fail `NoUseAfterFree`.
 *
 * Scope / non-claims:
 *   - This model does NOT prove the C++ atomics ordering of a lock-free
 *     `lastUsedSeqId` stamp (release/acquire pairing, torn reads). `StampMark`
 *     is one atomic action because HandleArena's own shared mutex serializes
 *     the slot; the ticket is one atomic action because `nextSeqId_` is a
 *     single `std::atomic<u64>` acquire load. The memory-model obligation
 *     stays with the deterministic interleaving harness (design §5 layer 3,
 *     R-VERIF-7.3).
 *   - `CaptureRead` abstracts WHAT the capture copies to a single "the record
 *     existed" obligation. That abstraction is licensed by design §7 Q1 (the
 *     read-set is producer-ordered, while storage is arena-protected), NOT
 *     proven here; HandleArena shared/unique locking and the immutable
 *     commit-time snapshot are the production guards. `D3DCREATE_MULTITHREADED`
 *     still serializes producer API order, but is not a Pool ownership claim.
 *   - `MapFastRead` models only the T2c watermark READ. The rename-ring
 *     rotation that `finalizeBufferMap` performs on the same lock-free fast
 *     path is `arena-protected` and is modelled by `BufferBackingVersioning`,
 *     not here. Producer API order remains serialized separately when
 *     `D3DCREATE_MULTITHREADED` is enabled.
 *   - It does not model HandleArena's generation check, which is an
 *     independent fail-closed *detection* of a stale handle. The property
 *     checked here is ordering: a being-marked record is never reclaimed in
 *     the first place.
 *   - It does not model Metal, pixels, or driver behaviour.
 *
 * Properties verified:
 *   Safety   — TypeOK, NoUseAfterFree, NoReclaimInsideMarkWindow,
 *              ReclaimRespectsWatermark, PinnedRecordsAreNotDestroyPending,
 *              MapReadSound, WorkerAppendCoveredByStamps,
 *              CommitStampsCoverChunkSeq, NoCaptureAfterFree,
 *              StampsPrecedeCapture
 *   Action   — MarkMonotonic, CompletedMonotonic, ObservedCompletedMonotonic
 *
 * Counterexample configurations (all executed by
 * `scripts/check/verify_tla.sh` as expected failures):
 *   .counterexample          PinDiscipline="Removed"     → NoUseAfterFree
 *   .restamp.counterexample  RestampDiscipline="Removed" → NoUseAfterFree
 *   .capture.counterexample  PinDiscipline="Removed"     → NoCaptureAfterFree
 *
 * The third reuses the EXISTING pin axis rather than adding a redundant
 * `CapturePinDiscipline` constant: T2b's capture has exactly one safety
 * premise beyond Q1's read-set audit — the record still exists — and that
 * premise is the pin chain, so deleting the pin is precisely the deletion
 * that exposes the capture-side violation. It differs from
 * `.counterexample` only in which invariant it asks TLC to report: listing
 * `NoUseAfterFree` there too would hide the capture trace behind the shorter
 * mark-window one (same axis, three steps instead of six).
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  Resources,         \* set of resource identifiers (e.g., {r1, r2})
  MAX_SEQID,         \* model-checking bound on the seq-id domain
  PinDiscipline,     \* "Enforced" (production) | "Removed" (counterexample)
  RestampDiscipline  \* "Enforced" (production) | "Removed" (counterexample)

ASSUME Resources # {}
ASSUME MAX_SEQID \in Nat /\ MAX_SEQID >= 2
ASSUME PinDiscipline \in {"Enforced", "Removed"}
ASSUME RestampDiscipline \in {"Enforced", "Removed"}

CommitPhases == {"Idle", "Pinned", "Marking", "Committed"}

\* Worker batch phases. "Marked" is the ONLY phase in which the worker owns
\* `CommandQueue::mutex_`: it re-acquires at WorkerEndStamping and releases
\* after WorkerAppend. SlotAdvance is excluded against exactly that phase,
\* which is what makes an in-lock re-stamp a fixed point.
WorkerPhases == {"Idle", "Marking", "Marked", "Appended", "Pending"}

VARIABLES
  retainerPinned,          \* Resources → BOOLEAN — PE retainer holds a ref
  workerPinned,            \* Resources → BOOLEAN — replay worker holds a ref
  destroyPending,          \* Resources → BOOLEAN — last unix ref dropped
  freed,                   \* Resources → BOOLEAN — slot released by gcArena
  lastUsedSeqId,           \* Resources → Nat     — the marked watermark
  nextSeqId,               \* Nat — seq the open writing slot will be given
  completedSeqId,          \* Nat — GPU-completed watermark (truth)
  observedCompletedSeqId,  \* Nat — the producer's own (stale) atomic read
  commitPhase,             \* one of CommitPhases
  commitSeqId,             \* Nat — the ticket BeginMark reserved
  chunkNamed,              \* SUBSET Resources — resources this chunk names
  marked,                  \* SUBSET chunkNamed — already stamped
  captured,                \* SUBSET marked — binding capture already read
  workerPhase,             \* one of WorkerPhases
  workerBatch,             \* SUBSET Resources — resources this batch names
  workerStamped,           \* SUBSET workerBatch — already stamped
  workerTicket,            \* Nat — the seq ticket this batch stamped with
  workerAppendSeqId,       \* Nat — seq the appended records landed under (0=none)
  useAfterFree             \* BOOLEAN — sticky fault flag (see NoUseAfterFree)

vars ==
  <<retainerPinned, workerPinned, destroyPending, freed, lastUsedSeqId,
    nextSeqId, completedSeqId, observedCompletedSeqId, commitPhase,
    commitSeqId, chunkNamed, marked, captured, workerPhase, workerBatch,
    workerStamped, workerTicket, workerAppendSeqId, useAfterFree>>

(* ================================================================
   Shared predicate vocabulary
   ----------------------------------------------------------------
   These two operators are the model half of the shared pure predicates in
   `src/dxmt9/dxmt9_mark_reclaim_predicates.hpp`. Production code and
   `tests/native/backend/producer_mark_reclaim_spec.cpp` call the C++ twins by
   the same names, so a TLC trace is mechanically translatable into a native
   step sequence.
   ================================================================ *)

\* C++: dxmt9::resources::canReclaimRecord
CanReclaimRecord(dp, lastUsed, completed) == dp /\ lastUsed <= completed

\* C++: dxmt9::resources::markStampUpper
MarkStampUpper(current, stamp) == IF stamp > current THEN stamp ELSE current

(* ================================================================
   Derived state
   ================================================================ *)

\* A record is pinned when ANY holder still owns a unix reference.
IsPinned(r) == retainerPinned[r] \/ workerPinned[r]

CommitInFlight == commitPhase \in {"Pinned", "Marking", "Committed"}

\* The worker owns `CommandQueue::mutex_` only while it is between
\* WorkerEndStamping and WorkerAppend.
WorkerHoldsQueueMutex == workerPhase = "Marked"

(*
 * WorkerRecordInUse(r)
 * A record the worker's batch still needs. Two disjoint reasons:
 *   - the batch is mid-mark / mid-append, so the worker is literally
 *     dereferencing the record; or
 *   - the batch's records have been appended into a chunk (seq
 *     workerAppendSeqId) that the GPU has NOT completed, so the encoder is
 *     still going to consume them.
 * Freeing such a record is a use-after-free, which is what the seq stamp —
 * not the pin — is supposed to prevent past the ref release.
 *)
WorkerRecordInUse(r) ==
  /\ r \in workerBatch
  /\ \/ workerPhase \in {"Marking", "Marked", "Appended"}
     \/ (workerPhase = "Pending" /\ workerAppendSeqId > completedSeqId)

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ retainerPinned         = [r \in Resources |-> FALSE]
  /\ workerPinned           = [r \in Resources |-> FALSE]
  /\ destroyPending         = [r \in Resources |-> FALSE]
  /\ freed                  = [r \in Resources |-> FALSE]
  /\ lastUsedSeqId          = [r \in Resources |-> 0]
  /\ nextSeqId              = 1
  /\ completedSeqId         = 0
  /\ observedCompletedSeqId = 0
  /\ commitPhase            = "Idle"
  /\ commitSeqId            = 0
  /\ chunkNamed             = {}
  /\ marked                 = {}
  /\ captured               = {}
  /\ workerPhase            = "Idle"
  /\ workerBatch            = {}
  /\ workerStamped          = {}
  /\ workerTicket           = 0
  /\ workerAppendSeqId      = 0
  /\ useAfterFree           = FALSE

(* ================================================================
   Retainer
   ================================================================ *)

(*
 * PinChunkResources(S)
 * The PE recorder retainer takes a unix reference on every resource the
 * chunk about to be committed names. Design §7 Q2: this happens BEFORE any
 * marking, so the pin lifetime strictly contains the mark window. A chunk
 * cannot name a record whose destruction has already begun.
 *)
PinChunkResources(S) ==
  /\ commitPhase = "Idle"
  /\ S # {}
  /\ \A r \in S : ~destroyPending[r] /\ ~freed[r]
  /\ retainerPinned' =
       [r \in Resources |-> IF r \in S THEN TRUE ELSE retainerPinned[r]]
  /\ chunkNamed'  = S
  /\ commitPhase' = "Pinned"
  /\ UNCHANGED <<workerPinned, destroyPending, freed, lastUsedSeqId,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitSeqId, marked, captured, workerPhase, workerBatch,
                 workerStamped, workerTicket, workerAppendSeqId, useAfterFree>>

(*
 * ReleasePins
 * `commit_chunk` returned successfully, so the retainer's epoch ends. The
 * retained wrappers are handed to the commit-replay offload worker, which is
 * the actor that will eventually drop the last reference. Pin coverage is
 * never zero across this boundary — that is what makes the handoff safe.
 *)
ReleasePins ==
  /\ commitPhase = "Committed"
  /\ retainerPinned' =
       [r \in Resources |-> IF r \in chunkNamed THEN FALSE ELSE retainerPinned[r]]
  /\ workerPinned' =
       [r \in Resources |-> IF r \in chunkNamed THEN TRUE ELSE workerPinned[r]]
  /\ commitPhase' = "Idle"
  /\ chunkNamed'  = {}
  /\ marked'      = {}
  /\ captured'    = {}
  /\ UNCHANGED <<destroyPending, freed, lastUsedSeqId, nextSeqId,
                 completedSeqId, observedCompletedSeqId, commitSeqId,
                 workerPhase, workerBatch, workerStamped, workerTicket,
                 workerAppendSeqId, useAfterFree>>

(* ================================================================
   Producer
   ================================================================ *)

(*
 * BeginMark
 * Reserves the chunk's ticket (`seqIdForMark`'s `nextSeqId` read) and, unlike
 * the worker's batch, publishes under it: `commit_chunk`'s synchronous half
 * both takes the ticket and owns the chunk it names. That is why the producer
 * side carries no re-stamp obligation — `commitSeqId` cannot move once taken.
 * `CommitStampsCoverChunkSeq` states this as an invariant so a future change
 * that decouples the two fails here instead of in the field.
 *)
BeginMark ==
  /\ commitPhase = "Pinned"
  /\ nextSeqId <= MAX_SEQID
  /\ commitSeqId' = nextSeqId
  /\ nextSeqId'   = nextSeqId + 1
  /\ commitPhase' = "Marking"
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, completedSeqId, observedCompletedSeqId,
                 chunkNamed, marked, captured, workerPhase, workerBatch,
                 workerStamped, workerTicket, workerAppendSeqId, useAfterFree>>

(*
 * StampMark(r)  — T2a.
 * `Pool::mark*Use`: stamp the max of the record's watermark and this chunk's
 * ticket. Under T2a this runs WITHOUT `CommandQueue::mutex_`, so it is not
 * excluded against Reclaim / AdvanceCompleted / SetDestroyPending /
 * WorkerReleaseRefs / SlotAdvance — TLC interleaves it freely with all of
 * them. It is a single atomic action because HandleArena's own mutex
 * serializes the slot.
 *
 * The guard deliberately does NOT test `freed[r]`: the production mark loop
 * walks the chunk's resource list and does not re-check liveness. Touching a
 * reclaimed record is therefore *reachable* in the model and is recorded in
 * `useAfterFree` rather than being excluded by fiat. That is what makes the
 * counterexample configurations able to see the bug class.
 *)
StampMark(r) ==
  /\ commitPhase = "Marking"
  /\ r \in chunkNamed
  /\ r \notin marked
  /\ marked' = marked \cup {r}
  /\ lastUsedSeqId' =
       [lastUsedSeqId EXCEPT ![r] = MarkStampUpper(lastUsedSeqId[r], commitSeqId)]
  \* The parentheses are load-bearing: TLA+ binds `=` tighter than `\/`, so
  \* the unparenthesized form would parse as `(useAfterFree' = useAfterFree)
  \* \/ (...)` and leave the fault flag unconstrained.
  /\ useAfterFree' = (useAfterFree \/ freed[r])
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, captured, workerPhase,
                 workerBatch, workerStamped, workerTicket, workerAppendSeqId>>

(*
 * CaptureRead(r)  — T2b.
 * `captureChunkBufferBinding`: read the producer-ordered fields of a record
 * the chunk names. The live record is arena-protected: HandleArena's shared
 * lock serializes this read against unique-lock mutation/reclaim. The
 * immutable commit-time snapshot is the worker/encoder publication boundary,
 * and the retainer pin keeps the named record alive. No ordering against
 * worker progress is required beyond those mechanisms; the model records the
 * remaining existence obligation with the same fault shape as StampMark.
 *
 * T2b RUNS THIS WITHOUT `CommandQueue::mutex_`, so — exactly like StampMark —
 * it must not be excluded against Reclaim / SetDestroyPending /
 * AdvanceCompleted / WorkerReleaseRefs / WorkerStampMark / SlotAdvance, and
 * it is not: the only guards are the commit phase and the per-record
 * capture/mark bookkeeping. Three consequences worth stating explicitly,
 * because each is a premise the mutex used to supply for free:
 *
 *   1. EXISTENCE. `freed[r]` is deliberately NOT a guard, so capturing a
 *      reclaimed record is *reachable* and recorded rather than excluded by
 *      fiat. What actually keeps it unreachable in production is the pin
 *      chain: chunk-named ⇒ retainerPinned (PinChunkResources) ⇒
 *      ~destroyPending (SetDestroyPending's premise) ⇒ outside `gcArena`'s
 *      gate. `NoCaptureAfterFree` states the obligation on its own, and
 *      `.capture.counterexample.cfg` (PinDiscipline="Removed") is the trace
 *      that violates it.
 *
 *   2. OTHER RECORDS. Design §8's T2b row asks for the worker-reclaim actor
 *      erasing records the capture does NOT name, concurrently with it.
 *      WorkerReleaseRefs → SetDestroyPending(r') → Reclaim(r') interleaves
 *      freely with CaptureRead(r) here. In production that is a `std::deque`
 *      slot release under HandleArena's own unique lock while the capture
 *      holds its shared lock, which is why container-level aliasing is a
 *      pointer-stability argument (pool header) rather than a model variable.
 *
 *   3. NO RE-STAMP ANALOG. The capture copies producer-ordered VALUES under
 *      the arena shared lock; nothing it reads is derived from the seq ticket,
 *      and nothing downstream
 *      compares the snapshot against `commitSeqId`. So the frozen-ticket
 *      re-read that StampMark's counterpart owes (WorkerRestamp) has no
 *      capture-side twin — there is no quantity a later SlotAdvance could
 *      make stale. That is also why `StampsPrecedeCapture` is the only
 *      ordering the two owe each other, and it points the one way that is
 *      repairable: a too-low stamp is fixed by a monotone re-stamp, an early
 *      capture is not fixable at all.
 *)
CaptureRead(r) ==
  /\ commitPhase = "Marking"
  /\ r \in marked
  /\ r \notin captured
  /\ captured'     = captured \cup {r}
  /\ useAfterFree' = (useAfterFree \/ freed[r])
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, nextSeqId, completedSeqId,
                 observedCompletedSeqId, commitPhase, commitSeqId, chunkNamed,
                 marked, workerPhase, workerBatch, workerStamped, workerTicket,
                 workerAppendSeqId>>

(*
 * EndCommit
 * The synchronous half of `commit_chunk` returns once every named resource
 * has been marked and captured.
 *)
EndCommit ==
  /\ commitPhase = "Marking"
  /\ marked   = chunkNamed
  /\ captured = chunkNamed
  /\ commitPhase' = "Committed"
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, nextSeqId, completedSeqId,
                 observedCompletedSeqId, commitSeqId, chunkNamed, marked,
                 captured, workerPhase, workerBatch, workerStamped,
                 workerTicket, workerAppendSeqId, useAfterFree>>

(*
 * MapFastRead  — T2c.
 * The DISCARD fast path's `completedSeqId_` read becomes a plain atomic load.
 * An atomic load may be stale (any value the writer has already published)
 * but never invents a value the writer has not reached, and never goes
 * backwards. The nondeterministic choice models exactly that latitude.
 *)
MapFastRead ==
  /\ \E v \in (observedCompletedSeqId + 1)..completedSeqId :
       observedCompletedSeqId' = v
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, nextSeqId, completedSeqId, commitPhase,
                 commitSeqId, chunkNamed, marked, captured, workerPhase,
                 workerBatch, workerStamped, workerTicket, workerAppendSeqId,
                 useAfterFree>>

(* ================================================================
   Publisher — the ticket/slot-seq race
   ================================================================ *)

(*
 * SlotAdvance
 * Some OTHER actor publishes the open writing slot, so the seq a pending
 * append will finally get moves up. In production this is
 * `QueueLifecycleController::commitCurrentChunk`'s seq increment, reached
 * from the producer's map-wait force-publish and from the draw-count /
 * payload-arena chunk limits — design §9's "the writing slot is not
 * worker-exclusive".
 *
 * It is excluded against `WorkerHoldsQueueMutex` and nothing else: `nextSeqId_`
 * is only ever mutated under `CommandQueue::mutex_`, so an actor holding that
 * mutex observes a frozen ticket. That exclusion is exactly what makes the
 * in-lock re-stamp below a fixed point rather than another race.
 *)
SlotAdvance ==
  /\ ~WorkerHoldsQueueMutex
  /\ nextSeqId <= MAX_SEQID
  /\ nextSeqId' = nextSeqId + 1
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, marked, captured,
                 workerPhase, workerBatch, workerStamped, workerTicket,
                 workerAppendSeqId, useAfterFree>>

(* ================================================================
   Worker — T2a', the symmetric marking actor
   ================================================================ *)

(*
 * WorkerBeginBatch(S)
 * `submitDrawRunBatchImpl` starts a batch. The worker already holds the
 * retained wrappers for every resource the batch names (they were handed over
 * at `commit_chunk` and are dropped only by `releaseRetainedWrappers` after
 * replay), which is the pin premise, symmetric to the producer's. The ticket
 * is read here as one acquire load of `nextSeqId_` — NOT under the queue
 * mutex, which is the whole point of T2a'.
 *)
WorkerBeginBatch(S) ==
  /\ workerPhase = "Idle"
  /\ S # {}
  /\ \A r \in S : ~destroyPending[r] /\ ~freed[r]
  /\ nextSeqId <= MAX_SEQID
  /\ workerPinned' = [r \in Resources |-> IF r \in S THEN TRUE ELSE workerPinned[r]]
  /\ workerBatch'   = S
  /\ workerStamped' = {}
  /\ workerTicket'  = nextSeqId
  /\ workerPhase'   = "Marking"
  /\ UNCHANGED <<retainerPinned, destroyPending, freed, lastUsedSeqId,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, marked, captured,
                 workerAppendSeqId, useAfterFree>>

(*
 * WorkerStampMark(r)  — T2a'.
 * The worker's `pool.markDrawResources` / `markDrawBindingSnapshotResources`
 * / `markDrawBindingOverrideResources` loop, moved off `CommandQueue::mutex_`
 * onto the pool's documented arena-stamp exception. Identical shape to the
 * producer's StampMark: monotone max through the shared predicate, no
 * `freed[r]` guard, fault recorded rather than excluded.
 *)
WorkerStampMark(r) ==
  /\ workerPhase = "Marking"
  /\ r \in workerBatch
  /\ r \notin workerStamped
  /\ workerStamped' = workerStamped \cup {r}
  /\ lastUsedSeqId' =
       [lastUsedSeqId EXCEPT ![r] = MarkStampUpper(lastUsedSeqId[r], workerTicket)]
  /\ useAfterFree' = (useAfterFree \/ freed[r])
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, marked, captured,
                 workerPhase, workerBatch, workerTicket, workerAppendSeqId>>

(*
 * WorkerEndStamping
 * The unlocked marking window closes: the worker re-acquires
 * `CommandQueue::mutex_` for the slot-append section.
 *)
WorkerEndStamping ==
  /\ workerPhase = "Marking"
  /\ workerStamped = workerBatch
  /\ workerPhase' = "Marked"
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, nextSeqId, completedSeqId,
                 observedCompletedSeqId, commitPhase, commitSeqId, chunkNamed,
                 marked, captured, workerBatch, workerStamped, workerTicket,
                 workerAppendSeqId, useAfterFree>>

(*
 * WorkerRestamp  — THE PROTOCOL.
 * With the mutex held, re-read the ticket. If a SlotAdvance moved it while
 * the stamps were being written unlocked, every resource in the batch is
 * re-stamped with the new seq before the append. This is the generalization
 * of the production `forceDrawResourceMarkingAfterSplit_` flag, which today
 * covers only the narrower case of a split that the batch loop itself caused.
 *
 * `RestampDiscipline = "Removed"` deletes this action and the WorkerAppend
 * guard that pairs with it, and nothing else.
 *)
WorkerRestamp ==
  /\ RestampDiscipline = "Enforced"
  /\ workerPhase = "Marked"
  /\ workerTicket # nextSeqId
  /\ lastUsedSeqId' =
       [r \in Resources |-> IF r \in workerBatch
                            THEN MarkStampUpper(lastUsedSeqId[r], nextSeqId)
                            ELSE lastUsedSeqId[r]]
  /\ workerTicket' = nextSeqId
  /\ useAfterFree' = (useAfterFree \/ (\E r \in workerBatch : freed[r]))
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, marked, captured,
                 workerPhase, workerBatch, workerStamped, workerAppendSeqId>>

(*
 * WorkerAppend
 * `currentSlotUnlocked(queue).appendDrawRunBatch(batch)` — the records land
 * in the open writing slot, which will be published as seq `nextSeqId`. The
 * safety obligation `WorkerAppendCoveredByStamps` is evaluated from here on.
 *)
WorkerAppend ==
  /\ workerPhase = "Marked"
  /\ (RestampDiscipline = "Enforced" => workerTicket = nextSeqId)
  /\ workerAppendSeqId' = nextSeqId
  /\ workerPhase'       = "Appended"
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, nextSeqId, completedSeqId,
                 observedCompletedSeqId, commitPhase, commitSeqId, chunkNamed,
                 marked, captured, workerBatch, workerStamped, workerTicket,
                 useAfterFree>>

(*
 * WorkerReleaseBatchRefs
 * `releaseRetainedWrappers` on replay completion drops the batch's refs. Note
 * this happens BEFORE the chunk it appended into has completed on the GPU —
 * which is precisely why the seq stamp, not the pin, is what protects the
 * record from here to completion, and therefore why a stamp below the chunk's
 * seq is a real use-after-free rather than a bookkeeping detail.
 *)
WorkerReleaseBatchRefs ==
  /\ workerPhase = "Appended"
  /\ workerPinned' =
       [r \in Resources |-> IF r \in workerBatch THEN FALSE ELSE workerPinned[r]]
  /\ workerPhase' = "Pending"
  /\ UNCHANGED <<retainerPinned, destroyPending, freed, lastUsedSeqId,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, marked, captured,
                 workerBatch, workerStamped, workerTicket, workerAppendSeqId,
                 useAfterFree>>

(*
 * WorkerRetireBatch
 * The GPU watermark passed the chunk the batch appended into, so the records
 * are no longer live and the worker may start another batch.
 *)
WorkerRetireBatch ==
  /\ workerPhase = "Pending"
  /\ completedSeqId >= workerAppendSeqId
  /\ workerPhase'       = "Idle"
  /\ workerBatch'       = {}
  /\ workerStamped'     = {}
  /\ workerTicket'      = 0
  /\ workerAppendSeqId' = 0
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, nextSeqId, completedSeqId,
                 observedCompletedSeqId, commitPhase, commitSeqId, chunkNamed,
                 marked, captured, useAfterFree>>

(*
 * WorkerReleaseRefs
 * `releaseRetainedWrappers` dropping refs the worker inherited from an
 * EARLIER chunk. Unguarded by `commitPhase` on purpose: it runs on the worker
 * thread and routinely lands inside the producer's mark window for a later
 * chunk. When the same resource is also named by that later chunk,
 * `retainerPinned` still covers it — that overlap is the whole pin-ordering
 * argument, and it is what `PinDiscipline = "Removed"` removes. It never
 * touches the batch the worker is currently replaying; those refs are dropped
 * by WorkerReleaseBatchRefs, strictly after the append.
 *)
WorkerReleaseRefs ==
  /\ \E r \in Resources : workerPinned[r] /\ r \notin workerBatch
  /\ workerPinned' =
       [r \in Resources |-> IF r \in workerBatch THEN workerPinned[r] ELSE FALSE]
  /\ UNCHANGED <<retainerPinned, destroyPending, freed, lastUsedSeqId,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, marked, captured,
                 workerPhase, workerBatch, workerStamped, workerTicket,
                 workerAppendSeqId, useAfterFree>>

(* ================================================================
   Reclaim — three actors, one enabling condition
   ================================================================ *)

(*
 * SetDestroyPending(r)
 * The last unix reference dropped, so the object's destruction path ran and
 * `mark*DestroyAndGc` set `destroyPending`. Design §7 Q4: THREE actors can be
 * the one that drops it — the producer (wrapper release), the completion loop
 * (`runFinishLoop`), and the replay offload worker
 * (`releaseRetainedWrappers`). They share this exact enabling condition, so
 * one transition covers all three; what distinguishes them (which thread, and
 * therefore what else may be running concurrently) is already covered because
 * no other action is excluded against this one.
 *
 * `~IsPinned(r)` IS the pin-ordering premise, and it is the only thing
 * PinDiscipline = "Removed" deletes.
 *)
SetDestroyPending(r) ==
  /\ ~destroyPending[r]
  /\ ~freed[r]
  /\ (PinDiscipline = "Enforced" => ~IsPinned(r))
  /\ destroyPending' = [destroyPending EXCEPT ![r] = TRUE]
  /\ UNCHANGED <<retainerPinned, workerPinned, freed, lastUsedSeqId,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, marked, captured,
                 workerPhase, workerBatch, workerStamped, workerTicket,
                 workerAppendSeqId, useAfterFree>>

(*
 * Reclaim(r)
 * `Pool::reclaimCompleted` → `gcArena`. The gate is the production gate and
 * nothing more: destroyPending AND the GPU watermark has passed the record's
 * last use. Pins are NOT consulted here.
 *
 * Freeing a record that a live commit window or a live worker batch still
 * needs is itself the use-after-free — someone is about to dereference it —
 * so it is recorded in `useAfterFree` at the moment it happens, not only when
 * a later StampMark / CaptureRead touches it.
 *)
Reclaim(r) ==
  /\ ~freed[r]
  /\ CanReclaimRecord(destroyPending[r], lastUsedSeqId[r], completedSeqId)
  /\ freed' = [freed EXCEPT ![r] = TRUE]
  /\ useAfterFree' = (useAfterFree
                      \/ (CommitInFlight /\ r \in chunkNamed)
                      \/ WorkerRecordInUse(r))
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, lastUsedSeqId,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, marked, captured,
                 workerPhase, workerBatch, workerStamped, workerTicket,
                 workerAppendSeqId>>

(* ================================================================
   Completion
   ================================================================ *)

(*
 * AdvanceCompleted
 * The GPU completes chunks in order. A seq cannot complete before the slot
 * after it has been opened, which keeps the newest published chunk out of the
 * watermark.
 *)
AdvanceCompleted ==
  /\ completedSeqId < nextSeqId - 1
  /\ completedSeqId' = completedSeqId + 1
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, nextSeqId, observedCompletedSeqId,
                 commitPhase, commitSeqId, chunkNamed, marked, captured,
                 workerPhase, workerBatch, workerStamped, workerTicket,
                 workerAppendSeqId, useAfterFree>>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ \E S \in (SUBSET Resources) : PinChunkResources(S)
  \/ BeginMark
  \/ \E r \in Resources : StampMark(r)
  \/ \E r \in Resources : CaptureRead(r)
  \/ EndCommit
  \/ ReleasePins
  \/ MapFastRead
  \/ SlotAdvance
  \/ \E S \in (SUBSET Resources) : WorkerBeginBatch(S)
  \/ \E r \in Resources : WorkerStampMark(r)
  \/ WorkerEndStamping
  \/ WorkerRestamp
  \/ WorkerAppend
  \/ WorkerReleaseBatchRefs
  \/ WorkerRetireBatch
  \/ WorkerReleaseRefs
  \/ \E r \in Resources : SetDestroyPending(r)
  \/ \E r \in Resources : Reclaim(r)
  \/ AdvanceCompleted

Spec == Init /\ [][Next]_vars

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ retainerPinned         \in [Resources -> BOOLEAN]
  /\ workerPinned           \in [Resources -> BOOLEAN]
  /\ destroyPending         \in [Resources -> BOOLEAN]
  /\ freed                  \in [Resources -> BOOLEAN]
  /\ \A r \in Resources : freed[r] => destroyPending[r]
  /\ lastUsedSeqId          \in [Resources -> 0..(MAX_SEQID + 1)]
  /\ nextSeqId              \in 1..(MAX_SEQID + 1)
  /\ completedSeqId         \in 0..MAX_SEQID
  /\ observedCompletedSeqId \in 0..MAX_SEQID
  /\ commitPhase            \in CommitPhases
  /\ commitSeqId            \in 0..MAX_SEQID
  /\ chunkNamed             \subseteq Resources
  /\ marked                 \subseteq chunkNamed
  /\ captured               \subseteq marked
  /\ workerPhase            \in WorkerPhases
  /\ workerBatch            \subseteq Resources
  /\ workerStamped          \subseteq workerBatch
  /\ workerTicket           \in 0..(MAX_SEQID + 1)
  /\ workerAppendSeqId      \in 0..(MAX_SEQID + 1)
  /\ useAfterFree           \in BOOLEAN

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * NoUseAfterFree
 * The sticky fault flag is set by exactly five sites:
 *   - StampMark(r) / CaptureRead(r) / WorkerStampMark(r) / WorkerRestamp
 *     on a record already freed, and
 *   - Reclaim(r) of a record an in-flight commit window or an in-use worker
 *     batch still needs.
 * This is the model's central claim, and the one BOTH "Removed"
 * configurations are expected to violate.
 *)
NoUseAfterFree == ~useAfterFree

(*
 * NoReclaimInsideMarkWindow
 * The same property stated positionally rather than through the fault flag:
 * nothing a live commit window or a live worker batch needs is freed. Kept
 * separate so a future change that loses the fault-flag wiring still fails.
 *)
NoReclaimInsideMarkWindow ==
  /\ CommitInFlight => \A r \in chunkNamed : ~freed[r]
  /\ \A r \in Resources : WorkerRecordInUse(r) => ~freed[r]

(*
 * ReclaimRespectsWatermark
 * `gcArena`'s DXMT_ASSERT stated as an invariant over the whole reachable
 * state space: a freed record's watermark never runs ahead of the GPU.
 *)
ReclaimRespectsWatermark ==
  \A r \in Resources :
    freed[r] => (destroyPending[r] /\ lastUsedSeqId[r] <= completedSeqId)

(*
 * PinnedRecordsAreNotDestroyPending
 * The pin-ordering premise itself, as an invariant. Under "Removed" this is
 * the first thing to go — it is listed after NoUseAfterFree so the fault
 * remains the headline violation.
 *)
PinnedRecordsAreNotDestroyPending ==
  (PinDiscipline = "Enforced") =>
    \A r \in Resources : IsPinned(r) => ~destroyPending[r]

(*
 * WorkerAppendCoveredByStamps  — the re-stamp obligation.
 * Once the worker's records are in the slot, every resource the batch names
 * must already carry a stamp at least as high as the seq that slot will be
 * published under. This is the property the queue mutex used to give away for
 * free by making the ticket read and the append one hold, and the one
 * `RestampDiscipline = "Removed"` breaks.
 *)
WorkerAppendCoveredByStamps ==
  (workerPhase \in {"Appended", "Pending"}) =>
    \A r \in workerBatch : lastUsedSeqId[r] >= workerAppendSeqId

(*
 * CommitStampsCoverChunkSeq
 * The producer's counterpart. It holds without any re-stamp because
 * `commit_chunk` reserves the ticket and owns the chunk published under it,
 * so `commitSeqId` cannot move inside the window. Stated as an invariant so a
 * future change that decouples the two is caught here.
 *)
CommitStampsCoverChunkSeq ==
  (commitPhase \in {"Marking", "Committed"}) =>
    \A r \in marked : lastUsedSeqId[r] >= commitSeqId

(*
 * NoCaptureAfterFree  — the T2b existence obligation, on its own.
 * Stated over `captured` rather than through the shared `useAfterFree` flag so
 * a configuration can ask TLC to report the CAPTURE-side violation
 * specifically. It is strictly two-sided: it fails both when a CaptureRead
 * touches an already-freed record and when a Reclaim frees a record this
 * commit has already captured, which is the whole of "the snapshot the chunk
 * is about to publish describes a record that still exists".
 *
 * Under `PinDiscipline = "Enforced"` it follows from the pin chain:
 * `captured \subseteq marked \subseteq chunkNamed`, every chunkNamed record is
 * retainerPinned for the whole window, a pinned record cannot become
 * destroyPending, and `gcArena`'s gate needs destroyPending. `ReleasePins`
 * clears `captured` in the same step it drops the pins, so the invariant never
 * outlives its premise.
 *
 * `.capture.counterexample.cfg` deletes that chain and is expected to fail
 * exactly this invariant.
 *)
NoCaptureAfterFree == \A r \in captured : ~freed[r]

(*
 * StampsPrecedeCapture  — design spec §4's third ordering protocol.
 * Also a TypeOK conjunct, and repeated here on purpose: as a named invariant a
 * regression is reported as the protocol it broke rather than as a type error,
 * and T2b is the change that makes the ordering a real obligation instead of a
 * side effect of both steps sharing one mutex hold.
 *)
StampsPrecedeCapture == captured \subseteq marked

(*
 * MapReadSound
 * The producer's atomic `completedSeqId` read never runs ahead of the truth,
 * so a DISCARD fast path that acts on it can only be conservative.
 *)
MapReadSound == observedCompletedSeqId <= completedSeqId

Safety ==
  /\ TypeOK
  /\ NoUseAfterFree
  /\ NoReclaimInsideMarkWindow
  /\ ReclaimRespectsWatermark
  /\ PinnedRecordsAreNotDestroyPending
  /\ MapReadSound
  /\ WorkerAppendCoveredByStamps
  /\ CommitStampsCoverChunkSeq
  /\ NoCaptureAfterFree
  /\ StampsPrecedeCapture

(* ================================================================
   Action properties
   ================================================================ *)

(* Stamps never decrease — `markStampUpper` is a monotone max. *)
MarkMonotonic ==
  [][\A r \in Resources : lastUsedSeqId'[r] >= lastUsedSeqId[r]]_vars

(* The GPU watermark never regresses. *)
CompletedMonotonic == [][completedSeqId' >= completedSeqId]_vars

(* Neither does the producer's stale view of it. *)
ObservedCompletedMonotonic ==
  [][observedCompletedSeqId' >= observedCompletedSeqId]_vars

====
