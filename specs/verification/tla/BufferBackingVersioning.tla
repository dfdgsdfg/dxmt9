---- MODULE BufferBackingVersioning ----
(*
 * dxmt9 MANAGED Buffer Backing Versioning — TLA+ Specification
 *
 * Verifies R-BACK-5.11 for a logical D3DPOOL_MANAGED buffer backed by a
 * grow-only set of concrete Shared MTLBuffers:
 *
 *   * a draw snapshots and stamps exactly the active concrete backing;
 *   * writable upload may reuse only a backing whose last-use sequence has
 *     completed, otherwise it selects a fresh backing;
 *   * the logical destruction watermark remains the maximum concrete use;
 *   * destroying the logical buffer cannot free an in-flight backing.
 *
 * The finite Backings set bounds fresh allocation for model checking. The
 * production implementation allocates a fresh MTLBuffer when the finite set
 * of already-created entries has no idle member.
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS Backings, MAX_SEQID

ASSUME Backings # {}
ASSUME MAX_SEQID \in Nat /\ MAX_SEQID >= 1

BufferStates == {"Live", "DestroyPending", "Freed"}

VARIABLES
  bufferState,
  allocated,
  activeBacking,
  backingLastUsed,
  logicalLastUsed,
  completedSeqId,
  currentSeqId,
  lastUploadSafe

vars == <<bufferState, allocated, activeBacking, backingLastUsed,
          logicalLastUsed, completedSeqId, currentSeqId, lastUploadSafe>>

Init ==
  \E initial \in Backings :
    /\ bufferState = "Live"
    /\ allocated = {initial}
    /\ activeBacking = initial
    /\ backingLastUsed = [b \in Backings |-> 0]
    /\ logicalLastUsed = 0
    /\ completedSeqId = 0
    /\ currentSeqId = 1
    /\ lastUploadSafe = TRUE

(* A draw packet snapshots activeBacking and stamps that exact member. *)
SnapshotDraw ==
  /\ bufferState = "Live"
  /\ currentSeqId <= MAX_SEQID
  /\ activeBacking \in allocated
  /\ backingLastUsed' =
       [backingLastUsed EXCEPT ![activeBacking] = currentSeqId]
  /\ logicalLastUsed' = currentSeqId
  /\ UNCHANGED <<bufferState, allocated, activeBacking, completedSeqId,
                  currentSeqId, lastUploadSafe>>

Commit ==
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId' = currentSeqId + 1
  /\ UNCHANGED <<bufferState, allocated, activeBacking, backingLastUsed,
                  logicalLastUsed, completedSeqId, lastUploadSafe>>

DrainCommit ==
  /\ bufferState = "DestroyPending"
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId' = currentSeqId + 1
  /\ UNCHANGED <<bufferState, allocated, activeBacking, backingLastUsed,
                  logicalLastUsed, completedSeqId, lastUploadSafe>>

GPUComplete ==
  /\ completedSeqId < currentSeqId - 1
  /\ completedSeqId' = completedSeqId + 1
  /\ UNCHANGED <<bufferState, allocated, activeBacking, backingLastUsed,
                  logicalLastUsed, currentSeqId, lastUploadSafe>>

(* Writable upload can stay on or rotate to any already-idle backing. *)
UploadIdle(b) ==
  /\ bufferState = "Live"
  /\ b \in allocated
  /\ backingLastUsed[b] <= completedSeqId
  /\ activeBacking' = b
  /\ lastUploadSafe' = (backingLastUsed[b] <= completedSeqId)
  /\ UNCHANGED <<bufferState, allocated, backingLastUsed, logicalLastUsed,
                  completedSeqId, currentSeqId>>

(* When no allocated backing is idle, upload grows rather than waiting. *)
UploadFresh(b) ==
  /\ bufferState = "Live"
  /\ b \in Backings \ allocated
  /\ \A old \in allocated : backingLastUsed[old] > completedSeqId
  /\ allocated' = allocated \cup {b}
  /\ activeBacking' = b
  /\ backingLastUsed' = [backingLastUsed EXCEPT ![b] = 0]
  /\ lastUploadSafe' = TRUE
  /\ UNCHANGED <<bufferState, logicalLastUsed, completedSeqId,
                  currentSeqId>>

Destroy ==
  /\ bufferState = "Live"
  /\ bufferState' = "DestroyPending"
  /\ UNCHANGED <<allocated, activeBacking, backingLastUsed, logicalLastUsed,
                  completedSeqId, currentSeqId, lastUploadSafe>>

Free ==
  /\ bufferState = "DestroyPending"
  /\ completedSeqId >= logicalLastUsed
  /\ bufferState' = "Freed"
  /\ UNCHANGED <<allocated, activeBacking, backingLastUsed, logicalLastUsed,
                  completedSeqId, currentSeqId, lastUploadSafe>>

Next ==
  \/ SnapshotDraw
  \/ Commit
  \/ DrainCommit
  \/ GPUComplete
  \/ Destroy
  \/ Free
  \/ \E b \in Backings : UploadIdle(b)
  \/ \E b \in Backings : UploadFresh(b)

Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(GPUComplete)
  /\ WF_vars(DrainCommit)
  /\ WF_vars(Free)

TypeOK ==
  /\ bufferState \in BufferStates
  /\ allocated \subseteq Backings
  /\ allocated # {}
  /\ activeBacking \in Backings
  /\ backingLastUsed \in [Backings -> Nat]
  /\ logicalLastUsed \in Nat
  /\ completedSeqId \in Nat
  /\ currentSeqId \in Nat
  /\ lastUploadSafe \in BOOLEAN

ActiveBackingAllocated == activeBacking \in allocated

LogicalWatermarkCoversEveryBacking ==
  \A b \in allocated : backingLastUsed[b] <= logicalLastUsed

NoUploadOverwriteInFlight == lastUploadSafe

NoBackingFreedInFlight ==
  bufferState = "Freed" =>
    \A b \in allocated : backingLastUsed[b] <= completedSeqId

DestroyPendingCannotFreeEarly ==
  (bufferState = "DestroyPending" /\ logicalLastUsed > completedSeqId)
    => bufferState # "Freed"

DestroyPendingEventuallyFreed ==
  bufferState = "DestroyPending" ~> bufferState = "Freed"

====
