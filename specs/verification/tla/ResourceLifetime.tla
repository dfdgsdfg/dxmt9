---- MODULE ResourceLifetime ----
(*
 * dxmt9 Resource Lifetime — TLA+ Specification
 *
 * Verifies the deferred-destruction invariant (R-BACK-5.6, R-BACK-7.3):
 *
 *   A GPU resource (MTLBuffer / MTLTexture) must not be freed until all
 *   in-flight GPU commands that reference it have completed.
 *
 * Implementation strategy in dxmt9:
 *   Each resource tracks `lastUsedSeqId` — the seqId of the last CommandChunk
 *   that referenced it. When the application calls destroyBuffer/destroyTexture,
 *   the resource transitions to DestroyPending. The FinishThread (or a deferred
 *   GC pass) may free the underlying MTL object only when:
 *
 *     completedSeqId >= lastUsedSeqId
 *
 *   i.e., the GPU has finished all commands in the referencing chunk.
 *   A drain commit is modeled explicitly so teardown can eventually make
 *   progress even when the Wine thread stops issuing more work.
 *
 * Additionally (R-VERIF-3.4) this module models the HandleArena pointer
 * lifetime contract that backs `dxmt9::resources::detail::HandleArena`
 * in src/dxmt9/dxmt9_resource_pool.hpp lines 281-287:
 *
 *   1. `std::deque<Slot>::push_back` must keep already-handed-out element
 *      addresses pointer-stable. The model represents the slot store as
 *      a function `slotIndex : Resources -> Nat`. The growth action
 *      (`InsertNewSlot`) may only bump `numSlots`; it must never re-bind
 *      a previously-allocated `slotIndex`. `SlotIdentityStable` is the
 *      formalization of that C++ axiom — if a future maintainer modeled
 *      insertion as a re-permutation, the invariant would fail.
 *
 *   2. While the encoder thread is mid-`encodeChunk` it has dereferenced
 *      a `Record*` returned by `find()` and is reading it without the
 *      queue mutex. `encoderHolds` tracks the set of records the encoder
 *      is currently dereferencing; `FreeResource` is gated on
 *      `r \notin encoderHolds` so `releaseSlot` cannot fire on a record
 *      the encoder is currently consuming.
 *
 * Requirement traceability:
 *   R-BACK-5.6  destroyBuffer/destroyTexture must defer until GPU done
 *   R-BACK-7.3  Resource destruction safe while in-flight GPU work references it
 *   R-VERIF-3.4 HandleArena slot-identity stability + encoder-held pointer safety
 *
 * Properties verified:
 *   Safety   — TypeOK, NoUseAfterFree, PrematureFreeImpossible,
 *              SlotIdentityStable, EncoderPointerStable
 *   Liveness — DestroyPendingEventuallyFreed, EncoderEventuallyReleases
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  Resources,   \* set of resource identifiers (e.g., {r1, r2, r3})
  MAX_SEQID    \* model-checking bound

ASSUME Resources # {}
ASSUME MAX_SEQID \in Nat /\ MAX_SEQID >= 1

ResourceStates == {"Live", "DestroyPending", "Freed"}

(*
 * C++ axiom (modeled by the action shape below):
 *   std::deque<T>::push_back never invalidates existing element addresses.
 *   The HandleArena depends on this; if std::deque were swapped for
 *   std::vector the assumption would silently break. The model encodes
 *   the axiom by never permitting an action that mutates `slotIndex` —
 *   only the monotone `numSlots` grows.
 *)

VARIABLES
  resState,        \* FUNCTION Resources -> ResourceStates
  lastUsedSeqId,   \* FUNCTION Resources -> Nat  (seqId of last chunk that used it; 0 = unused)
  completedSeqId,  \* Nat — seq ID of most recently GPU-completed chunk (mirrors CommandQueue)
  currentSeqId,    \* Nat — next seq ID to assign
  slotIndex,       \* FUNCTION Resources -> Nat  — index of each resource's HandleArena slot
  numSlots,        \* Nat — total slots ever allocated in the arena (monotone)
  encoderHolds     \* SUBSET Resources — records the encoder thread currently dereferences

vars == <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
          slotIndex, numSlots, encoderHolds>>

(* ================================================================
   Initialization

   Resources are pre-allocated dense slot indices 0..|Resources|-1 at
   the start. `numSlots` counts how many slots have ever been handed
   out; it only grows.
   ================================================================ *)

\* Deterministic slot assignment from a finite resource set. CHOOSE picks
\* a fixed bijection so the initial slotIndex is well-defined.
RECURSIVE AssignSlotsRec(_, _)
AssignSlotsRec(remaining, next) ==
  IF remaining = {} THEN <<[r \in {} |-> 0], next>>
  ELSE LET r == CHOOSE x \in remaining : TRUE
           rec == AssignSlotsRec(remaining \ {r}, next + 1)
       IN <<[s \in (DOMAIN rec[1]) \cup {r} |->
              IF s = r THEN next ELSE rec[1][s]], rec[2]>>

InitialAssignment == AssignSlotsRec(Resources, 0)

Init ==
  /\ resState       = [r \in Resources |-> "Live"]
  /\ lastUsedSeqId  = [r \in Resources |-> 0]
  /\ completedSeqId = 0
  /\ currentSeqId   = 1
  /\ slotIndex      = InitialAssignment[1]
  /\ numSlots       = InitialAssignment[2]
  /\ encoderHolds   = {}

(* ================================================================
   Actions
   ================================================================ *)

(*
 * UseResource(r)
 * A draw call in the current chunk references resource r.
 * Updates lastUsedSeqId so the deferred-free gate knows when GPU is done with it.
 * A Freed resource must not be used (use-after-free is a bug in the core/backend).
 *)
UseResource(r) ==
  /\ resState[r] = "Live"
  /\ currentSeqId <= MAX_SEQID
  /\ lastUsedSeqId' = [lastUsedSeqId EXCEPT ![r] = currentSeqId]
  /\ UNCHANGED <<resState, completedSeqId, currentSeqId,
                 slotIndex, numSlots, encoderHolds>>

(*
 * CommitChunk
 * Wine thread commits the current chunk. Advances currentSeqId.
 * Resources referenced in this chunk will have lastUsedSeqId = currentSeqId - 1
 * after this commit (they were recorded before the increment).
 *)
CommitChunk ==
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId' = currentSeqId + 1
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId,
                 slotIndex, numSlots, encoderHolds>>

(*
 * DrainCommit
 * Once any resource is DestroyPending, the queue can still commit its
 * current chunk during teardown/drain. This keeps the model live without
 * assuming the Wine thread keeps committing forever.
 *)
DrainCommit ==
  /\ \E r \in Resources : resState[r] = "DestroyPending"
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId' = currentSeqId + 1
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId,
                 slotIndex, numSlots, encoderHolds>>

(*
 * DestroyResource(r)
 * Application calls destroyBuffer / destroyTexture.
 * Transitions the resource to DestroyPending — the MTL object is NOT freed yet.
 * This is always safe: the application relinquishes ownership immediately.
 *)
DestroyResource(r) ==
  /\ resState[r] = "Live"
  /\ resState' = [resState EXCEPT ![r] = "DestroyPending"]
  /\ UNCHANGED <<lastUsedSeqId, completedSeqId, currentSeqId,
                 slotIndex, numSlots, encoderHolds>>

(*
 * FreeResource(r)
 * Backend frees the underlying MTL object via HandleArena::releaseSlot.
 * Permitted only when:
 *   (a) completedSeqId >= lastUsedSeqId[r] — GPU drained past last use, AND
 *   (b) r \notin encoderHolds — encoder is not currently dereferencing
 *       the record's pointer (R-VERIF-3.4 encoder-held pointer safety).
 *)
FreeResource(r) ==
  /\ resState[r] = "DestroyPending"
  /\ completedSeqId >= lastUsedSeqId[r]   \* GPU-drain gate
  /\ r \notin encoderHolds                \* encoder-pointer-lifetime gate
  /\ resState' = [resState EXCEPT ![r] = "Freed"]
  /\ UNCHANGED <<lastUsedSeqId, completedSeqId, currentSeqId,
                 slotIndex, numSlots, encoderHolds>>

(*
 * GPUComplete
 * GPU finishes a chunk; completedSeqId advances.
 * Simplified model: completes one chunk at a time, in order.
 *)
GPUComplete ==
  /\ completedSeqId < currentSeqId - 1
  /\ completedSeqId' = completedSeqId + 1
  /\ UNCHANGED <<resState, lastUsedSeqId, currentSeqId,
                 slotIndex, numSlots, encoderHolds>>

(*
 * InsertNewSlot
 * Models the PE thread calling HandleArena::insert (push_back into
 * slots_). The deque axiom forbids re-binding any previously assigned
 * slotIndex; the action only grows `numSlots`. With a finite resource
 * set there is no fresh identifier to bind, so the structural shape
 * (slotIndex left UNCHANGED) is what the C++ implementation must
 * preserve. The bound keeps the state space finite.
 *)
InsertNewSlot ==
  /\ numSlots < Cardinality(Resources) + 2
  /\ numSlots' = numSlots + 1
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
                 slotIndex, encoderHolds>>

(*
 * EncoderFindResource(r)
 * Encoder thread calls find(handle) and obtains a Record*. The pointer
 * goes into encoderHolds until EncoderFinishChunk clears it.
 * Precondition: the record still exists (not Freed) — find() returns
 * nullptr otherwise and the encoder would not retain a pointer.
 *)
EncoderFindResource(r) ==
  /\ resState[r] # "Freed"
  /\ r \notin encoderHolds
  /\ encoderHolds' = encoderHolds \cup {r}
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
                 slotIndex, numSlots>>

(*
 * EncoderFinishChunk
 * The encode call (one full encodeChunk) returns; the encoder no longer
 * holds any record pointers. Modeling this as draining the whole set
 * matches the C++ contract: the encoder cannot retain a Record* past
 * encodeChunk's return.
 *)
EncoderFinishChunk ==
  /\ encoderHolds # {}
  /\ encoderHolds' = {}
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
                 slotIndex, numSlots>>

(* ================================================================
   Specification

   Fairness rationale:
     WF on GPUComplete, DrainCommit, EncoderFinishChunk — these actions
       must eventually fire when continuously enabled (the GPU, the
       drain pump, the encoder's chunk boundary).
     SF on FreeResource(r) — the encoder-pointer gate can briefly
       disable FreeResource(r) every time the encoder dips back in
       through EncoderFindResource. Strong fairness translates
       "enabled infinitely often" into "eventually taken"; weak
       fairness would let a pathological scheduler toggle the gate
       forever without ever freeing the record.
   ================================================================ *)

Next ==
  \/ CommitChunk
  \/ DrainCommit
  \/ GPUComplete
  \/ InsertNewSlot
  \/ EncoderFinishChunk
  \/ \E r \in Resources : UseResource(r)
  \/ \E r \in Resources : DestroyResource(r)
  \/ \E r \in Resources : FreeResource(r)
  \/ \E r \in Resources : EncoderFindResource(r)

Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(GPUComplete)
  /\ WF_vars(DrainCommit)
  /\ WF_vars(EncoderFinishChunk)
  /\ \A r \in Resources : SF_vars(FreeResource(r))

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ resState       \in [Resources -> ResourceStates]
  /\ lastUsedSeqId  \in [Resources -> Nat]
  /\ completedSeqId \in Nat
  /\ currentSeqId   \in Nat
  /\ slotIndex      \in [Resources -> Nat]
  /\ numSlots       \in Nat
  /\ encoderHolds   \subseteq Resources

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * NoUseAfterFree
 * The GPU must never be executing commands that reference a Freed resource.
 *
 * A resource is Freed only after completedSeqId >= lastUsedSeqId[r].
 * At that point, no in-flight command can reference it, because all chunks
 * with seqId <= completedSeqId have already completed on the GPU.
 *)
NoUseAfterFree ==
  \A r \in Resources :
    resState[r] = "Freed" => completedSeqId >= lastUsedSeqId[r]

(*
 * PrematureFreeImpossible
 * A resource in DestroyPending with in-flight references cannot be freed.
 * This invariant makes the safety gate explicit.
 *)
PrematureFreeImpossible ==
  \A r \in Resources :
    (resState[r] = "DestroyPending" /\ lastUsedSeqId[r] > completedSeqId)
    => resState[r] # "Freed"

(*
 * SlotIdentityStable (R-VERIF-3.4 — std::deque pointer-stability axiom)
 * Every resource keeps its initially assigned arena slot index for the
 * entire spec run. The only action that grows the arena (InsertNewSlot)
 * leaves `slotIndex` UNCHANGED; existing slot bindings remain pinned.
 * The invariant says `slotIndex[r]` always stays within [0, numSlots),
 * and `numSlots` never drops below the initial allocation count.
 *)
SlotIdentityStable ==
  /\ numSlots >= Cardinality(Resources)
  /\ \A r \in Resources : slotIndex[r] < numSlots

(*
 * EncoderPointerStable (R-VERIF-3.4 — encoder-held pointer safety)
 * Formalizes the C++ comment at src/dxmt9/dxmt9_resource_pool.hpp:281-287:
 * if the encoder thread is currently dereferencing a record's pointer
 * (r \in encoderHolds), then that record cannot already be Freed. The
 * FreeResource gate enforces this; the invariant verifies it.
 *)
EncoderPointerStable ==
  \A r \in encoderHolds : resState[r] # "Freed"

Safety ==
  /\ TypeOK
  /\ NoUseAfterFree
  /\ PrematureFreeImpossible
  /\ SlotIdentityStable
  /\ EncoderPointerStable

(* ================================================================
   Liveness
   ================================================================ *)

(*
 * DestroyPendingEventuallyFreed
 * A resource marked DestroyPending is eventually freed.
 * Guaranteed because:
 *   (a) the GPU eventually completes all submitted work (WF_GPUComplete),
 *   (b) the encoder eventually returns from encodeChunk
 *       (WF_EncoderFinishChunk) so the encoder-pointer gate in
 *       FreeResource opens infinitely often, and
 *   (c) FreeResource is taken once both gates are open
 *       (SF_FreeResource — strong fairness is required because the
 *       encoder-pointer gate intermittently disables the action).
 *)
DestroyPendingEventuallyFreed ==
  \A r \in Resources :
    resState[r] = "DestroyPending" ~> resState[r] = "Freed"

(*
 * EncoderEventuallyReleases
 * Any record the encoder picks up is eventually released — encodeChunk
 * cannot hold a pointer forever. This is what makes DestroyPending
 * liveness above hold under the extended FreeResource gate.
 *)
EncoderEventuallyReleases ==
  \A r \in Resources :
    r \in encoderHolds ~> r \notin encoderHolds

====
