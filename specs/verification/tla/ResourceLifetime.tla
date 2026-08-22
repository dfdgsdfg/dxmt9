---- MODULE ResourceLifetime ----
(******************************************************************************
 * Deferred resource destruction with an Initializer-held reference.
 *
 * Arena records are protected by the chunk sequence watermark. Private-texture
 * uploads are different: Initializer::pendingUploads_ owns a retained
 * destination reference before the upload has a chunk seqId, and the encoded
 * Metal command buffer owns that reference after submission. The original
 * model collapsed arena-record release and Metal-object deallocation, so it
 * could not express the 2026-08-02 pending-upload use-after-free.
 *
 * Implementation = "Retained" models Pool::StagingCopy::destTexture.
 * Implementation = "Bare" deliberately models the old bare destination handle
 * and is checked by ResourceLifetime.counterexample.cfg.
 *
 * Requirement traceability: R-VERIF-3.1--3.3, R-VERIF-6.1--6.3,
 * R-BACK-5.6, and R-BACK-7.3.
 *******************************************************************************)

EXTENDS Naturals, FiniteSets

CONSTANTS Resources, MAX_SEQID, Implementation

ASSUME Resources # {}
ASSUME MAX_SEQID \in Nat /\ MAX_SEQID >= 1
ASSUME Implementation \in {"Retained", "Bare"}

ResourceStates == {"Live", "DestroyPending", "Freed"}
InitializerStates == {"None", "Pending", "InFlight"}

VARIABLES
  resState,
  lastUsedSeqId,
  completedSeqId,
  currentSeqId,
  initializerState,
  metalAlive

vars == <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
          initializerState, metalAlive>>

Init ==
  /\ resState = [r \in Resources |-> "Live"]
  /\ lastUsedSeqId = [r \in Resources |-> 0]
  /\ completedSeqId = 0
  /\ currentSeqId = 1
  /\ initializerState = [r \in Resources |-> "None"]
  /\ metalAlive = [r \in Resources |-> TRUE]

UseResource(r) ==
  /\ resState[r] = "Live"
  /\ metalAlive[r]
  /\ currentSeqId <= MAX_SEQID
  /\ lastUsedSeqId' = [lastUsedSeqId EXCEPT ![r] = currentSeqId]
  /\ UNCHANGED <<resState, completedSeqId, currentSeqId,
                  initializerState, metalAlive>>

CommitChunk ==
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId' = currentSeqId + 1
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId,
                  initializerState, metalAlive>>

DrainCommit ==
  /\ \E r \in Resources : resState[r] = "DestroyPending"
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId' = currentSeqId + 1
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId,
                  initializerState, metalAlive>>

GPUComplete ==
  /\ completedSeqId < currentSeqId - 1
  /\ completedSeqId' = completedSeqId + 1
  /\ UNCHANGED <<resState, lastUsedSeqId, currentSeqId,
                  initializerState, metalAlive>>

(* Pool::stageTextureUpload constructs StagingCopy::destTexture before the
 * application may drop the arena handle. *)
StageInitializerUpload(r) ==
  /\ resState[r] = "Live"
  /\ metalAlive[r]
  /\ initializerState[r] = "None"
  /\ initializerState' = [initializerState EXCEPT ![r] = "Pending"]
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
                  metalAlive>>

(* flushToWaitUnlocked encodes and commits while StagingCopy still retains the
 * destination. The abstract Initializer actor then denotes Metal's in-flight
 * command-buffer ownership until completion. *)
SubmitInitializerUpload(r) ==
  /\ initializerState[r] = "Pending"
  /\ metalAlive[r]
  /\ initializerState' = [initializerState EXCEPT ![r] = "InFlight"]
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
                  metalAlive>>

(* A committed Metal command buffer releases its encoded resource references
 * when it settles, both on ordinary completion and on command-buffer failure
 * or device loss. This is the terminal InFlight ownership transition; it does
 * not claim that the upload succeeded. *)
SettleInitializerUpload(r) ==
  /\ initializerState[r] = "InFlight"
  /\ initializerState' = [initializerState EXCEPT ![r] = "None"]
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
                  metalAlive>>

(* Null command-buffer / encoder failure clears the pending vector before any
 * GPU use; releasing the retained reference is therefore safe. *)
AbortInitializerUpload(r) ==
  /\ initializerState[r] = "Pending"
  /\ initializerState' = [initializerState EXCEPT ![r] = "None"]
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
                  metalAlive>>

DestroyResource(r) ==
  /\ resState[r] = "Live"
  /\ resState' = [resState EXCEPT ![r] = "DestroyPending"]
  /\ UNCHANGED <<lastUsedSeqId, completedSeqId, currentSeqId,
                  initializerState, metalAlive>>

(* TLA+: resources::canReclaimRecord. Releasing the arena record is still
 * gated only by destroyPending and the chunk completion watermark. In the
 * retained implementation that release cannot deallocate the Metal object
 * while the independent Initializer actor owns it. The Bare branch is the old
 * bug kept executable for the companion counterexample. *)
FreeResource(r) ==
  /\ resState[r] = "DestroyPending"
  /\ completedSeqId >= lastUsedSeqId[r]
  /\ resState' = [resState EXCEPT ![r] = "Freed"]
  /\ IF Implementation = "Retained"
        THEN UNCHANGED metalAlive
        ELSE metalAlive' = [metalAlive EXCEPT ![r] = FALSE]
  /\ UNCHANGED <<lastUsedSeqId, completedSeqId, currentSeqId,
                  initializerState>>

(* Objective-C reference counting performs this abstract transition after both
 * the arena and Initializer/command-buffer owners have released their
 * references. Production intentionally has no global owner-oracle predicate. *)
ReleaseMetalObject(r) ==
  /\ Implementation = "Retained"
  /\ resState[r] = "Freed"
  /\ initializerState[r] = "None"
  /\ metalAlive[r]
  /\ metalAlive' = [metalAlive EXCEPT ![r] = FALSE]
  /\ UNCHANGED <<resState, lastUsedSeqId, completedSeqId, currentSeqId,
                  initializerState>>

Next ==
  \/ CommitChunk
  \/ DrainCommit
  \/ GPUComplete
  \/ \E r \in Resources :
       \/ UseResource(r)
       \/ StageInitializerUpload(r)
       \/ SubmitInitializerUpload(r)
       \/ SettleInitializerUpload(r)
       \/ AbortInitializerUpload(r)
       \/ DestroyResource(r)
       \/ FreeResource(r)
       \/ ReleaseMetalObject(r)

Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(GPUComplete)
  /\ WF_vars(DrainCommit)
  /\ \A r \in Resources :
       /\ WF_vars(SubmitInitializerUpload(r) \/ AbortInitializerUpload(r))
       /\ WF_vars(SettleInitializerUpload(r))
       /\ WF_vars(FreeResource(r))
       /\ WF_vars(ReleaseMetalObject(r))

TypeOK ==
  /\ resState \in [Resources -> ResourceStates]
  /\ lastUsedSeqId \in [Resources -> Nat]
  /\ completedSeqId \in Nat
  /\ currentSeqId \in Nat
  /\ initializerState \in [Resources -> InitializerStates]
  /\ metalAlive \in [Resources -> BOOLEAN]

(* No arena record, queued initializer upload, or initializer command buffer
 * may retain a reference to a deallocated Metal object. *)
NoUseAfterFree ==
  \A r \in Resources :
    (resState[r] # "Freed" \/ initializerState[r] # "None")
      => metalAlive[r]

PrematureFreeImpossible ==
  \A r \in Resources :
    resState[r] = "Freed" => completedSeqId >= lastUsedSeqId[r]

InitializerReferenceSafety ==
  \A r \in Resources :
    initializerState[r] # "None" => metalAlive[r]

MetalReleaseAfterAllOwners ==
  \A r \in Resources :
    ~metalAlive[r] =>
      /\ resState[r] = "Freed"
      /\ initializerState[r] = "None"
      /\ completedSeqId >= lastUsedSeqId[r]

DestroyPendingEventuallyFreed ==
  \A r \in Resources :
    resState[r] = "DestroyPending" ~> resState[r] = "Freed"

InitializerEventuallySettled ==
  \A r \in Resources :
    initializerState[r] # "None" ~> initializerState[r] = "None"

FreedEventuallyMetalReleased ==
  \A r \in Resources :
    resState[r] = "Freed" ~> ~metalAlive[r]

====
