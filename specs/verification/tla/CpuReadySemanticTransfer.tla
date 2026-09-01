---- MODULE CpuReadySemanticTransfer ----
(***************************************************************************
 * R-BACK-2.90 / R-VERIF-2.24 bounded ownership refinement.
 *
 * This is intentionally a small seam model, not a second replay model.  A
 * single imported RawCommandChunk and one Tape reservation are composed into
 * the move-only Unix transfer owner.  The model starts after queue Reserve,
 * when the move-only owner already holds the lease.  Recoverable pre-effect
 * Abort is therefore the batch-only path; single-source abort is a native
 * fail-stop case and is intentionally not a rollback transition here.  The
 * existing CpuPipelineOwnership model owns queue progress.
 *************************************************************************** *)
EXTENDS Naturals, TLC

Stages == {"Reserved", "Adopted", "Emitted", "Published", "Aborted",
           "FailStopped"}
RawOwners == {"Transfer", "Caller"}
TapeOwners == {"Queue", "Transfer", "Ready", "Released"}

VARIABLES stage, rawOwner, tapeOwner, releaseCount, batchMode

vars == <<stage, rawOwner, tapeOwner, releaseCount, batchMode>>

Init ==
  /\ stage = "Reserved"
  /\ rawOwner = "Transfer"
  /\ tapeOwner = "Transfer"
  /\ releaseCount = 0
  /\ batchMode \in BOOLEAN

Adopt ==
  /\ stage = "Reserved"
  /\ tapeOwner' = "Transfer"
  /\ stage' = "Adopted"
  /\ UNCHANGED <<rawOwner, releaseCount, batchMode>>

Emit ==
  /\ stage = "Adopted"
  /\ stage' = "Emitted"
  /\ UNCHANGED <<rawOwner, tapeOwner, releaseCount, batchMode>>

Publish ==
  /\ stage = "Emitted"
  /\ stage' = "Published"
  /\ tapeOwner' = "Ready"
  /\ UNCHANGED <<rawOwner, releaseCount, batchMode>>

PostEffectFailStop ==
  /\ stage = "Emitted"
  /\ stage' = "FailStopped"
  /\ tapeOwner' = "Released"
  /\ rawOwner' = "Transfer"
  /\ releaseCount' = 1
  /\ UNCHANGED batchMode

Abort ==
  /\ batchMode
  /\ stage \in {"Reserved", "Adopted"}
  /\ stage' = "Aborted"
  /\ tapeOwner' = "Released"
  /\ rawOwner' = "Transfer"
  /\ releaseCount' = 1
  /\ UNCHANGED batchMode

SingleSourceAbort ==
  /\ ~batchMode
  /\ stage \in {"Reserved", "Adopted"}
  /\ stage' = "FailStopped"
  /\ tapeOwner' = "Released"
  /\ rawOwner' = "Transfer"
  /\ releaseCount' = 1
  /\ UNCHANGED batchMode

RestoreRaw ==
  /\ stage \in {"Published", "Aborted", "FailStopped"}
  /\ stage' = stage
  /\ rawOwner' = "Caller"
  /\ UNCHANGED <<tapeOwner, releaseCount, batchMode>>

Next ==
  \/ Adopt
  \/ Emit
  \/ Publish
  \/ PostEffectFailStop
  \/ Abort
  \/ SingleSourceAbort
  \/ RestoreRaw

TypeOK ==
  /\ stage \in Stages
  /\ rawOwner \in RawOwners
  /\ tapeOwner \in TapeOwners
  /\ releaseCount \in 0..1
  /\ batchMode \in BOOLEAN

ReserveBeforeAdopt ==
  stage = "Reserved" => tapeOwner = "Transfer"

AdoptBeforeEmit ==
  stage \in {"Emitted", "Published", "Aborted", "FailStopped"} =>
    tapeOwner \in {"Transfer", "Ready", "Released"}

NoPartialReady ==
  stage = "Published" => tapeOwner = "Ready" /\ releaseCount = 0

NoDoubleRelease ==
  releaseCount <= 1

TerminalRelease ==
  /\ stage = "Published" =>
    tapeOwner = "Ready" /\ releaseCount = 0
  /\ stage \in {"Aborted", "FailStopped"} =>
    tapeOwner = "Released" /\ releaseCount = 1
  /\ rawOwner = "Caller" =>
    stage \in {"Published", "Aborted", "FailStopped"}

Inv == TypeOK /\
       ReserveBeforeAdopt /\
       AdoptBeforeEmit /\
       NoPartialReady /\
       NoDoubleRelease /\
       TerminalRelease

Terminal ==
  stage \in {"Published", "Aborted", "FailStopped"} /\ rawOwner = "Caller"

EventuallySettledOrFailStopped ==
  [] (stage \in {"Reserved", "Adopted", "Emitted"} /\ rawOwner = "Transfer" =>
        <> Terminal)

Spec == Init /\ [][Next]_vars /\
       WF_vars(Adopt) /\ WF_vars(Emit) /\ WF_vars(Publish) /\
       WF_vars(PostEffectFailStop) /\ WF_vars(Abort) /\
       WF_vars(SingleSourceAbort) /\
       WF_vars(RestoreRaw)

THEOREM Spec => []Inv
====
