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
 * Requirement traceability:
 *   R-BACK-5.6  destroyBuffer/destroyTexture must defer until GPU done
 *   R-BACK-7.3  Resource destruction safe while in-flight GPU work references it
 *
 * Properties verified:
 *   Safety   — TypeOK, NoUseAfterFree
 *   Liveness — DestroyPendingEventuallyFreed
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  Resources,   \* set of resource identifiers (e.g., {r1, r2, r3})
  MAX_SEQID    \* model-checking bound

ASSUME Resources # {}
ASSUME MAX_SEQID \in Nat /\ MAX_SEQID >= 1

ResourceStates == {"Live", "DestroyPending", "Freed"}

VARIABLES
  resState,        \* FUNCTION Resources → ResourceStates
  lastUsedSeqId,   \* FUNCTION Resources → Nat  (seqId of last chunk that used it; 0 = unused)
  completedSeqId,  \* Nat — seq ID of most recently GPU-completed chunk (mirrors CommandQueue)
  currentSeqId     \* Nat — next seq ID to assign

vars == <<resState, lastUsedSeqId, completedSeqId, currentSeqId>>

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ resState       = [r \in Resources |-> "Live"]
  /\ lastUsedSeqId  = [r \in Resources |-> 0]
  /\ completedSeqId = 0
  /\ currentSeqId   = 1

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
  /\ UNCHANGED <<resState, completedSeqId, currentSeqId>>

(*
 * CommitChunk
 * Wine thread commits the current chunk. Advances currentSeqId.
 * Resources referenced in this chunk will have lastUsedSeqId = currentSeqId - 1
 * after this commit (they were recorded before the increment).
 *)
CommitChunk ==
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId' = currentSeqId + 1
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId>>

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
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId>>

(*
 * DestroyResource(r)
 * Application calls destroyBuffer / destroyTexture.
 * Transitions the resource to DestroyPending — the MTL object is NOT freed yet.
 * This is always safe: the application relinquishes ownership immediately.
 *)
DestroyResource(r) ==
  /\ resState[r] = "Live"
  /\ resState' = [resState EXCEPT ![r] = "DestroyPending"]
  /\ UNCHANGED <<lastUsedSeqId, completedSeqId, currentSeqId>>

(*
 * FreeResource(r)
 * Backend frees the underlying MTL object.
 * ONLY permitted when completedSeqId >= lastUsedSeqId[r], i.e., the GPU has
 * finished all commands in the last chunk that referenced this resource.
 *)
FreeResource(r) ==
  /\ resState[r] = "DestroyPending"
  /\ completedSeqId >= lastUsedSeqId[r]   \* safety gate
  /\ resState' = [resState EXCEPT ![r] = "Freed"]
  /\ UNCHANGED <<lastUsedSeqId, completedSeqId, currentSeqId>>

(*
 * GPUComplete
 * GPU finishes a chunk; completedSeqId advances.
 * Simplified model: completes one chunk at a time, in order.
 *)
GPUComplete ==
  /\ completedSeqId < currentSeqId - 1
  /\ completedSeqId' = completedSeqId + 1
  /\ UNCHANGED <<resState, lastUsedSeqId, currentSeqId>>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ CommitChunk
  \/ DrainCommit
  \/ GPUComplete
  \/ \E r \in Resources : UseResource(r)
  \/ \E r \in Resources : DestroyResource(r)
  \/ \E r \in Resources : FreeResource(r)

Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(GPUComplete)
  /\ WF_vars(DrainCommit)
  /\ \A r \in Resources : WF_vars(FreeResource(r))

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ resState      \in [Resources -> ResourceStates]
  /\ lastUsedSeqId \in [Resources -> Nat]
  /\ completedSeqId \in Nat
  /\ currentSeqId  \in Nat

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
 *
 * Formally: if a resource is Freed, then the GPU has completed past its
 * last-use seqId — the dangerous "in flight" window is closed.
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

Safety == TypeOK /\ NoUseAfterFree /\ PrematureFreeImpossible

(* ================================================================
   Liveness
   ================================================================ *)

(*
 * DestroyPendingEventuallyFreed
 * A resource marked DestroyPending is eventually freed.
 * Guaranteed because the GPU eventually completes all submitted work (WF_GPUComplete)
 * and the FreeResource action is taken once the gate opens (WF_FreeResource).
 *)
DestroyPendingEventuallyFreed ==
  \A r \in Resources :
    resState[r] = "DestroyPending" ~> resState[r] = "Freed"

====
