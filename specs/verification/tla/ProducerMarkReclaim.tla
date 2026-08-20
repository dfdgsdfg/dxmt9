---- MODULE ProducerMarkReclaim ----
(*
 * dxmt9 Producer Mark / Reclaim — TLA+ Specification
 *
 * Licenses the T2a / T2b relaxations of the producer↔queue concurrency track
 * (`docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md`
 * §8): commit-time resource marking, and the per-buffer binding capture that
 * rides with it, move OFF `CommandQueue::mutex_` and run under HandleArena's
 * own mutex instead. The safety argument they rest on is the pin-ordering
 * premise (§2): during `commit_chunk` the PE recorder retainer holds a unix
 * reference on every resource the chunk names, so `destroyPending` cannot be
 * set for a resource that is being marked, so a reclaim racing a lock-free
 * mark can never free a record the producer is still touching.
 *
 * Today that premise is enforced *trivially* by the shared mutex, not by the
 * pins. This model checks it as a standalone ordering property, so removing
 * the mutex does not silently remove the guarantee.
 *
 * Actors (all four run concurrently; no action pair is mutually excluded
 * except where a `commitPhase` guard says so):
 *
 *   Producer   — PinChunkResources → BeginMark → StampMark* / CaptureRead*
 *                → EndCommit; plus MapFastRead (T2c's atomic watermark read).
 *   Retainer   — the pin lifetime itself: pins are a *precondition* of
 *                marking (PinChunkResources) and are released strictly after
 *                EndCommit (ReleasePins), per design §7 Q2.
 *   Worker     — the commit-replay offload worker: ReleasePins hands the
 *                retained wrappers to it, and WorkerReleaseRefs models
 *                `releaseRetainedWrappers` dropping them on replay
 *                completion. Design §7 Q4: the worker is a *third* reclaim
 *                actor, and its ref drop can land anywhere inside a later
 *                chunk's mark window.
 *   Completion — AdvanceCompleted (`completedSeqId` watermark) and Reclaim
 *                (`Pool::reclaimCompleted` → `gcArena`).
 *
 * The reclaim gate is modelled FAITHFULLY, not defensively: `Reclaim(r)`
 * tests only `destroyPending[r] /\ lastUsedSeqId[r] <= completedSeqId`, which
 * is exactly `gcArena`'s condition in `src/dxmt9/dxmt9_resource_pool.hpp`.
 * Pins never appear in that gate. They appear one step earlier, in
 * `SetDestroyPending(r)`: a pinned record has refs > 0, so its destructor
 * path cannot run. `PinDiscipline = "Removed"` deletes exactly that premise
 * and nothing else; the companion counterexample configuration is expected
 * to fail `NoUseAfterFree`.
 *
 * Scope / non-claims:
 *   - This model does NOT prove the C++ atomics ordering of a lock-free
 *     `lastUsedSeqId` stamp (release/acquire pairing, torn reads). `StampMark`
 *     is one atomic action because HandleArena's own shared mutex serializes
 *     the slot metadata; the memory-model obligation stays with the
 *     deterministic interleaving harness (design §5 layer 3, R-VERIF-7.3).
 *   - It does not model HandleArena's generation check, which is an
 *     independent fail-closed *detection* of a stale handle. The property
 *     checked here is ordering: a being-marked record is never reclaimed in
 *     the first place.
 *   - It does not model Metal, pixels, or driver behaviour.
 *
 * Properties verified:
 *   Safety   — TypeOK, NoUseAfterFree, NoReclaimInsideMarkWindow,
 *              ReclaimRespectsWatermark, MapReadSound
 *   Action   — MarkMonotonic, CompletedMonotonic, ObservedCompletedMonotonic
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  Resources,      \* set of resource identifiers (e.g., {r1, r2})
  MAX_SEQID,      \* model-checking bound on the seq-id domain
  PinDiscipline   \* "Enforced" (production) | "Removed" (counterexample)

ASSUME Resources # {}
ASSUME MAX_SEQID \in Nat /\ MAX_SEQID >= 2
ASSUME PinDiscipline \in {"Enforced", "Removed"}

CommitPhases == {"Idle", "Pinned", "Marking", "Committed"}

VARIABLES
  retainerPinned,          \* Resources → BOOLEAN — PE retainer holds a ref
  workerPinned,            \* Resources → BOOLEAN — replay worker holds a ref
  destroyPending,          \* Resources → BOOLEAN — last unix ref dropped
  freed,                   \* Resources → BOOLEAN — slot released by gcArena
  lastUsedSeqId,           \* Resources → Nat     — the marked watermark
  nextSeqId,               \* Nat — next ticket the producer may reserve
  completedSeqId,          \* Nat — GPU-completed watermark (truth)
  observedCompletedSeqId,  \* Nat — the producer's own (stale) atomic read
  commitPhase,             \* one of CommitPhases
  chunkNamed,              \* SUBSET Resources — resources this chunk names
  marked,                  \* SUBSET chunkNamed — already stamped
  captured,                \* SUBSET marked — binding capture already read
  useAfterFree             \* BOOLEAN — sticky fault flag (see NoUseAfterFree)

vars ==
  <<retainerPinned, workerPinned, destroyPending, freed, lastUsedSeqId,
    nextSeqId, completedSeqId, observedCompletedSeqId, commitPhase,
    chunkNamed, marked, captured, useAfterFree>>

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

\* The ticket BeginMark reserved for the chunk currently being committed.
\* BeginMark is the only action that moves nextSeqId, so this is exact for
\* every state in which a commit is between BeginMark and ReleasePins.
ChunkSeqId == nextSeqId - 1

CommitInFlight == commitPhase \in {"Pinned", "Marking", "Committed"}

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
  /\ chunkNamed             = {}
  /\ marked                 = {}
  /\ captured               = {}
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
                 marked, captured, useAfterFree>>

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
                 completedSeqId, observedCompletedSeqId, useAfterFree>>

(* ================================================================
   Producer
   ================================================================ *)

(*
 * BeginMark
 * Reserves the chunk's ticket (`seqIdForMark`'s `nextSeqId` read). After
 * this step ChunkSeqId names it.
 *)
BeginMark ==
  /\ commitPhase = "Pinned"
  /\ nextSeqId <= MAX_SEQID
  /\ nextSeqId'   = nextSeqId + 1
  /\ commitPhase' = "Marking"
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, completedSeqId, observedCompletedSeqId,
                 chunkNamed, marked, captured, useAfterFree>>

(*
 * StampMark(r)  — T2a.
 * `Pool::mark*Use`: stamp the max of the record's watermark and this chunk's
 * ticket. Under T2a this runs WITHOUT `CommandQueue::mutex_`, so it is not
 * excluded against Reclaim / AdvanceCompleted / SetDestroyPending /
 * WorkerReleaseRefs — TLC interleaves it freely with all of them. It is a
 * single atomic action because HandleArena's own mutex serializes the slot.
 *
 * The guard deliberately does NOT test `freed[r]`: the production mark loop
 * walks the chunk's resource list and does not re-check liveness. Touching a
 * reclaimed record is therefore *reachable* in the model and is recorded in
 * `useAfterFree` rather than being excluded by fiat. That is what makes the
 * counterexample configuration able to see the bug class.
 *)
StampMark(r) ==
  /\ commitPhase = "Marking"
  /\ r \in chunkNamed
  /\ r \notin marked
  /\ marked' = marked \cup {r}
  /\ lastUsedSeqId' =
       [lastUsedSeqId EXCEPT ![r] = MarkStampUpper(lastUsedSeqId[r], ChunkSeqId)]
  \* The parentheses are load-bearing: TLA+ binds `=` tighter than `\/`, so
  \* the unparenthesized form would parse as `(useAfterFree' = useAfterFree)
  \* \/ (...)` and leave the fault flag unconstrained.
  /\ useAfterFree' = (useAfterFree \/ freed[r])
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, chunkNamed, captured>>

(*
 * CaptureRead(r)  — T2b.
 * `captureChunkBufferBinding`: read the producer-written fields of a record
 * the chunk names. Design §7 Q1 established the read-set is producer-written
 * only, so no ordering against worker progress is required — the ONLY safety
 * obligation left is that the record still exists. Modelled with the same
 * fault-recording shape as StampMark.
 *)
CaptureRead(r) ==
  /\ commitPhase = "Marking"
  /\ r \in marked
  /\ r \notin captured
  /\ captured'     = captured \cup {r}
  /\ useAfterFree' = (useAfterFree \/ freed[r])
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, nextSeqId, completedSeqId,
                 observedCompletedSeqId, commitPhase, chunkNamed, marked>>

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
                 observedCompletedSeqId, chunkNamed, marked, captured,
                 useAfterFree>>

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
                 chunkNamed, marked, captured, useAfterFree>>

(* ================================================================
   Worker
   ================================================================ *)

(*
 * WorkerReleaseRefs
 * `releaseRetainedWrappers` on replay completion (or fail-stop) drops the
 * offload worker's references. Unguarded by `commitPhase` on purpose: it runs
 * on the worker thread and routinely lands inside the producer's mark window
 * for a LATER chunk. When the same resource is also named by that later
 * chunk, `retainerPinned` still covers it — that overlap is the whole
 * pin-ordering argument, and it is what the counterexample configuration
 * removes.
 *)
WorkerReleaseRefs ==
  /\ \E r \in Resources : workerPinned[r]
  /\ workerPinned' = [r \in Resources |-> FALSE]
  /\ UNCHANGED <<retainerPinned, destroyPending, freed, lastUsedSeqId,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, chunkNamed, marked, captured, useAfterFree>>

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
                 commitPhase, chunkNamed, marked, captured, useAfterFree>>

(*
 * Reclaim(r)
 * `Pool::reclaimCompleted` → `gcArena`. The gate is the production gate and
 * nothing more: destroyPending AND the GPU watermark has passed the record's
 * last use. Pins are NOT consulted here.
 *
 * Freeing a record that a live commit window still names is itself the
 * use-after-free — the producer is about to dereference it — so it is
 * recorded in `useAfterFree` at the moment it happens, not only when the
 * later StampMark / CaptureRead touches it.
 *)
Reclaim(r) ==
  /\ ~freed[r]
  /\ CanReclaimRecord(destroyPending[r], lastUsedSeqId[r], completedSeqId)
  /\ freed' = [freed EXCEPT ![r] = TRUE]
  /\ useAfterFree' = (useAfterFree \/ (CommitInFlight /\ r \in chunkNamed))
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, lastUsedSeqId,
                 nextSeqId, completedSeqId, observedCompletedSeqId,
                 commitPhase, chunkNamed, marked, captured>>

(* ================================================================
   Completion
   ================================================================ *)

(*
 * AdvanceCompleted
 * The GPU completes chunks in order. A ticket cannot complete before the
 * producer has reserved the next one, which keeps the currently-committing
 * chunk's own ticket out of the watermark.
 *)
AdvanceCompleted ==
  /\ completedSeqId < nextSeqId - 1
  /\ completedSeqId' = completedSeqId + 1
  /\ UNCHANGED <<retainerPinned, workerPinned, destroyPending, freed,
                 lastUsedSeqId, nextSeqId, observedCompletedSeqId,
                 commitPhase, chunkNamed, marked, captured, useAfterFree>>

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
  /\ lastUsedSeqId          \in [Resources -> 0..MAX_SEQID]
  /\ nextSeqId              \in 1..(MAX_SEQID + 1)
  /\ completedSeqId         \in 0..MAX_SEQID
  /\ observedCompletedSeqId \in 0..MAX_SEQID
  /\ commitPhase            \in CommitPhases
  /\ chunkNamed             \subseteq Resources
  /\ marked                 \subseteq chunkNamed
  /\ captured               \subseteq marked
  /\ useAfterFree           \in BOOLEAN

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * NoUseAfterFree
 * The sticky fault flag is set by exactly three sites:
 *   - StampMark(r)   on a record already freed,
 *   - CaptureRead(r) on a record already freed,
 *   - Reclaim(r)     of a record the in-flight commit window still names.
 * This is the model's central claim, and the one the "Removed" configuration
 * is expected to violate.
 *)
NoUseAfterFree == ~useAfterFree

(*
 * NoReclaimInsideMarkWindow
 * The same property stated positionally rather than through the fault flag:
 * between PinChunkResources and ReleasePins, nothing the chunk names is
 * freed. Kept separate so a future change that loses the fault-flag wiring
 * still fails.
 *)
NoReclaimInsideMarkWindow ==
  CommitInFlight => \A r \in chunkNamed : ~freed[r]

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
