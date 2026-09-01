---- MODULE DirectChunkSlotContinuation ----
(***************************************************************************)
(* R-BACK-2.84 populated-slot direct continuation.                        *)
(*                                                                         *)
(* This model is intentionally smaller than the queue lifecycle models.   *)
(* It checks the transaction at the exact boundary changed by the        *)
(* optimization: a complete DrawRun/APPLY_STATE plan may append to an     *)
(* existing final ChunkSlot only when structural, capacity, and producer   *)
(* identity proofs hold.                                                   *)
(*                                                                         *)
(* APPLY_STATE and constant setters are state-only in the wire plan; they  *)
(* do not add command headers. Any coordinator/resource/readback/Present   *)
(* shape is represented by StructuralEligible=FALSE and must take the      *)
(* Legacy path before effects. A pre-effect build failure restores the      *)
(* exact prefix. A post-effect failure is terminal and has no Legacy retry. *)
(***************************************************************************)

EXTENDS Naturals, TLC

Stages == {"Idle", "Legacy", "Admitted", "Building", "Prepared",
           "Effects", "Committed", "RolledBack", "FailStopped"}
Dispositions == {"Unset", "Legacy", "Direct"}

VARIABLES stage, disposition, shapeValid, capacityReady,
          structuralEligible, currentEventOrdinal, nextEventOrdinal,
          currentSourceOrdinal, nextSourceOrdinal, prefixCount, stagedCount,
          appendedCount, effectsStarted, retryCount

vars == <<stage, disposition, shapeValid, capacityReady,
  structuralEligible, currentEventOrdinal, nextEventOrdinal,
  currentSourceOrdinal, nextSourceOrdinal, prefixCount, stagedCount,
  appendedCount, effectsStarted, retryCount>>

(* A populated compatibility slot may be extended only by the exact next
   producer interval in both identity dimensions. These are the current
   slot's last values and the candidate's first values. The native predicate
   also rejects invalid/max values before applying these +1 relations; the
   bounded model's domain contains only valid values. *)
IdentityAppendable ==
  /\ nextEventOrdinal = currentEventOrdinal + 1
  /\ nextSourceOrdinal = currentSourceOrdinal + 1

Init ==
  /\ stage = "Idle"
  /\ disposition = "Unset"
  /\ shapeValid \in BOOLEAN
  /\ capacityReady \in BOOLEAN
  /\ structuralEligible \in BOOLEAN
  /\ currentEventOrdinal \in 1..3
  /\ nextEventOrdinal \in 1..3
  /\ currentSourceOrdinal \in 1..3
  /\ nextSourceOrdinal \in 1..3
  /\ prefixCount = 2
  /\ stagedCount = 0
  /\ appendedCount = 0
  /\ effectsStarted = FALSE
  /\ retryCount = 0

(* The production predicate is the conjunction of the structural/capacity
   premises and exact producer-identity adjacency. Invalid plans, including
   an identity gap or overlap, are handed to the ordinary Legacy route
   before any semantic effect. *)
Admit ==
  /\ stage = "Idle"
  /\ IF shapeValid /\ capacityReady /\ structuralEligible /\ IdentityAppendable
        THEN /\ stage' = "Admitted"
             /\ disposition' = "Direct"
        ELSE /\ stage' = "Legacy"
             /\ disposition' = "Legacy"
  /\ UNCHANGED <<shapeValid, capacityReady, structuralEligible,
      currentEventOrdinal, nextEventOrdinal, currentSourceOrdinal,
      nextSourceOrdinal, prefixCount, stagedCount, appendedCount,
      effectsStarted, retryCount>>

Build ==
  /\ stage = "Admitted"
  /\ stage' = "Building"
  /\ UNCHANGED <<disposition, shapeValid, capacityReady,
      structuralEligible, currentEventOrdinal, nextEventOrdinal,
      currentSourceOrdinal, nextSourceOrdinal, prefixCount, stagedCount,
      appendedCount, effectsStarted, retryCount>>

(* Construction may have a complete private candidate before Prepare.  It is
   not visible in the final slot until StartEffects, so rollback can prove a
   non-trivial staged append is discarded. *)
BuildPartial ==
  /\ stage = "Building"
  /\ stagedCount = 0
  /\ stagedCount' = 1
  /\ UNCHANGED <<stage, disposition, shapeValid, capacityReady,
      structuralEligible, currentEventOrdinal, nextEventOrdinal,
      currentSourceOrdinal, nextSourceOrdinal, prefixCount, appendedCount,
      effectsStarted, retryCount>>

PrepareSuccess ==
  /\ stage = "Building"
  /\ stagedCount = 1
  /\ stage' = "Prepared"
  /\ UNCHANGED <<disposition, shapeValid, capacityReady,
      structuralEligible, currentEventOrdinal, nextEventOrdinal,
      currentSourceOrdinal, nextSourceOrdinal, prefixCount, stagedCount,
      appendedCount, effectsStarted, retryCount>>

(* No visible effect has happened, so the exact populated prefix survives and
   the failed candidate is not retried through Legacy in this transaction. *)
PreEffectRollback ==
  /\ stage = "Building"
  /\ stage' = "RolledBack"
  /\ stagedCount = 1
  /\ stagedCount' = 0
  /\ appendedCount' = 0
  /\ UNCHANGED <<disposition, shapeValid, capacityReady,
      structuralEligible, currentEventOrdinal, nextEventOrdinal,
      currentSourceOrdinal, nextSourceOrdinal, prefixCount, effectsStarted,
      retryCount>>

StartEffects ==
  /\ stage = "Prepared"
  /\ stagedCount = 1
  /\ stage' = "Effects"
  /\ effectsStarted' = TRUE
  /\ appendedCount' = 1
  /\ stagedCount' = 0
  /\ UNCHANGED <<disposition, shapeValid, capacityReady,
      structuralEligible, currentEventOrdinal, nextEventOrdinal,
      currentSourceOrdinal, nextSourceOrdinal, prefixCount, retryCount>>

Commit ==
  /\ stage = "Effects"
  /\ stage' = "Committed"
  /\ UNCHANGED <<disposition, shapeValid, capacityReady,
      structuralEligible, currentEventOrdinal, nextEventOrdinal,
      currentSourceOrdinal, nextSourceOrdinal, prefixCount, stagedCount,
      appendedCount, effectsStarted, retryCount>>

(* Once semantic effects have started, retrying the source through Legacy is
   forbidden: effect ownership is unknown and the queue must fail-stop. *)
PostEffectFailStop ==
  /\ stage = "Effects"
  /\ stage' = "FailStopped"
  /\ retryCount' = 0
  /\ UNCHANGED <<disposition, shapeValid, capacityReady,
      structuralEligible, currentEventOrdinal, nextEventOrdinal,
      currentSourceOrdinal, nextSourceOrdinal, prefixCount, stagedCount,
      appendedCount, effectsStarted>>

Next == Admit \/ Build \/ BuildPartial \/ PrepareSuccess \/ PreEffectRollback \/ StartEffects
         \/ Commit \/ PostEffectFailStop

TypeOK ==
  /\ stage \in Stages
  /\ disposition \in Dispositions
  /\ shapeValid \in BOOLEAN
  /\ capacityReady \in BOOLEAN
  /\ structuralEligible \in BOOLEAN
  /\ currentEventOrdinal \in 1..3
  /\ nextEventOrdinal \in 1..3
  /\ currentSourceOrdinal \in 1..3
  /\ nextSourceOrdinal \in 1..3
  /\ prefixCount = 2
  /\ stagedCount \in 0..1
  /\ appendedCount \in 0..1
  /\ effectsStarted \in BOOLEAN
  /\ retryCount = 0

AdmissionSound ==
  stage = "Admitted" =>
    disposition = "Direct" /\ shapeValid /\ capacityReady /\
      structuralEligible /\ IdentityAppendable

NonAppendableIdentityCannotEnterDirect ==
  ~IdentityAppendable =>
    stage \notin {"Admitted", "Building", "Prepared", "Effects", "Committed"}

LegacyBeforeEffects ==
  disposition = "Legacy" => ~effectsStarted /\ stagedCount = 0 /\ appendedCount = 0

PrefixPreservedOnRollback ==
  stage = "RolledBack" => prefixCount = 2 /\ stagedCount = 0 /\ appendedCount = 0

CommitRequiresEffects ==
  stage = "Committed" => effectsStarted /\ appendedCount = 1

NoRetryAfterPostEffectFailure ==
  stage = "FailStopped" => effectsStarted /\ retryCount = 0

NoHalfPublishedPrefix ==
  stage \in {"Idle", "Legacy", "Admitted", "Building", "Prepared",
              "RolledBack"} => appendedCount = 0

StagedCandidateIsPrivate ==
  stagedCount = 1 => stage \in {"Building", "Prepared"} /\ appendedCount = 0

Spec == Init /\ [][Next]_vars

====
