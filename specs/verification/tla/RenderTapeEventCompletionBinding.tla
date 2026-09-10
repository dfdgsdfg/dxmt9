---- MODULE RenderTapeEventCompletionBinding ----
(******************************************************************************
 * Bounded event -> actual-source completion binding.
 *
 * This is the cold controller/refinement layer only.  It models the
 * authenticated Tape event order, the actual qualified queue source bound at
 * each submit, ordered completion callbacks, and the pending completion-owner
 * / completed-stamp relation used by this bounded adapter schedule when one
 * source-slot index is recycled. `pendingOwners` is not an independent
 * replay/resource residency pin: Pool and BufferBackingVersioning own that
 * proof. This model does not model Metal execution, Pool internals, or GPU
 * progress.
 *
 * FirstActual and SecondActual deliberately share sourceIndex=1 while their
 * source and storage generations differ.  In this bounded scenario the
 * second binding is admitted only after the first completion callback
 * advances completedSeq.  This is not a universal Pool/GPU reuse theorem:
 * production post-encode retirement may legally provision a new generation
 * before this schedule's callback boundary.  The native test invokes
 * RenderTapeCompletionPlanController directly; it does not copy this reducer
 * into a fake implementation.
 ******************************************************************************)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS AllowStaleIdentityBinding,
          AllowPrematureReuse,
          AllowPrematureCompletion

Events == 1 .. 4
SubmissionIds == 1 .. 2
SourceIndices == {1}

None == [kind |-> "None"]

FirstActual == [kind              |-> "Actual",
                seqId              |-> 11,
                sourceIndex       |-> 1,
                sourceGeneration  |-> 101,
                storageFirstPage  |-> 1,
                storagePageCount  |-> 1,
                storageGeneration |-> 1001]
SecondActual == [kind              |-> "Actual",
                 seqId              |-> 23,
                 sourceIndex       |-> 1,
                 sourceGeneration  |-> 202,
                 storageFirstPage  |-> 1,
                 storagePageCount  |-> 1,
                 storageGeneration |-> 2002]

StaleSecondActual ==
  [SecondActual EXCEPT
      !.sourceGeneration = FirstActual.sourceGeneration,
      !.storageGeneration = FirstActual.storageGeneration]

Actual(i) == IF i = 1 THEN FirstActual ELSE SecondActual
SubmitEvent(i) == IF i = 1 THEN 1 ELSE 3
ReleaseEvent(i) == IF i = 1 THEN 2 ELSE 4

VARIABLES
  phase,
  eventCursor,
  submitFrontier,
  releaseFrontier,
  bound,
  pendingOwners,
  released,
  liveGeneration,
  completedStamp,
  completedSeq,
  failed,
  lastDisposition

vars == <<phase, eventCursor, submitFrontier, releaseFrontier, bound,
          pendingOwners, released, liveGeneration, completedStamp, completedSeq,
          failed, lastDisposition>>

Core == [phase           |-> phase,
         eventCursor     |-> eventCursor,
         submitFrontier  |-> submitFrontier,
         releaseFrontier |-> releaseFrontier,
         bound           |-> bound,
         pendingOwners   |-> pendingOwners,
         released        |-> released,
         completedSeq    |-> completedSeq,
         failed          |-> failed]

Init ==
  /\ phase = "Active"
  /\ eventCursor = 1
  /\ submitFrontier = 1
  /\ releaseFrontier = 1
  /\ bound = [i \in SubmissionIds |-> None]
  /\ pendingOwners = {}
  /\ released = {}
  /\ liveGeneration = [i \in SourceIndices |-> 0]
  /\ completedStamp = [i \in SourceIndices |-> 0]
  /\ completedSeq = 0
  /\ failed = FALSE
  /\ lastDisposition = "Accepted"

SubmitFirst ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 1 /\ submitFrontier = 1
  /\ bound' = [bound EXCEPT ![1] = FirstActual]
  /\ pendingOwners' = {1}
  /\ liveGeneration' = [liveGeneration EXCEPT ![1] = FirstActual.sourceGeneration]
  /\ submitFrontier' = 2
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, eventCursor, releaseFrontier, released,
                  completedStamp, completedSeq, failed>>

SubmitSecond ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 3 /\ submitFrontier = 2
  /\ 1 \notin pendingOwners
  /\ completedSeq >= FirstActual.seqId
  /\ bound' = [bound EXCEPT ![2] = SecondActual]
  /\ pendingOwners' = {2}
  /\ liveGeneration' = [liveGeneration EXCEPT ![1] = SecondActual.sourceGeneration]
  /\ submitFrontier' = 3
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, eventCursor, releaseFrontier, released,
                  completedStamp, completedSeq, failed>>

SubmitCallbackFailure ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 1 /\ submitFrontier = 1
  /\ phase' = "Failed" /\ failed' = TRUE
  /\ lastDisposition' = "CallbackFailure"
  /\ UNCHANGED <<eventCursor, submitFrontier, releaseFrontier, bound,
                  pendingOwners, released, liveGeneration, completedStamp,
                  completedSeq>>

EndEvent(e) ==
  /\ phase = "Active" /\ ~failed
  /\ e \in Events /\ eventCursor = e
  /\ \A i \in SubmissionIds : SubmitEvent(i) <= e => i < submitFrontier
  /\ \A i \in SubmissionIds : ReleaseEvent(i) <= e => i < releaseFrontier
  /\ eventCursor' = e + 1
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, submitFrontier, releaseFrontier, bound, pendingOwners,
                  released, liveGeneration, completedStamp, completedSeq,
                  failed>>

ReleaseFirst ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 2 /\ releaseFrontier = 1
  /\ bound[1] = FirstActual /\ 1 \in pendingOwners
  /\ released' = {1}
  /\ pendingOwners' = {}
  /\ releaseFrontier' = 2
  /\ completedStamp' = [completedStamp EXCEPT ![1] = FirstActual.seqId]
  /\ completedSeq' = FirstActual.seqId
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, eventCursor, submitFrontier, bound,
                  liveGeneration, failed>>

ReleaseSecond ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 4 /\ releaseFrontier = 2
  /\ bound[2] = SecondActual /\ 2 \in pendingOwners
  /\ released' = SubmissionIds
  /\ pendingOwners' = {}
  /\ releaseFrontier' = 3
  /\ completedStamp' = [completedStamp EXCEPT ![1] = SecondActual.seqId]
  /\ completedSeq' = SecondActual.seqId
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, eventCursor, submitFrontier, bound,
                  liveGeneration, failed>>

ReleaseCallbackFailure ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 2 /\ releaseFrontier = 1
  /\ bound[1] = FirstActual /\ 1 \in pendingOwners
  /\ phase' = "Failed" /\ failed' = TRUE
  /\ lastDisposition' = "CallbackFailure"
  /\ UNCHANGED <<eventCursor, submitFrontier, releaseFrontier, bound,
                  pendingOwners, released, liveGeneration, completedStamp,
                  completedSeq>>

Finish ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 5 /\ submitFrontier = 3 /\ releaseFrontier = 3
  /\ released = SubmissionIds /\ pendingOwners = {}
  /\ phase' = "Finished"
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<eventCursor, submitFrontier, releaseFrontier, bound,
                  pendingOwners, released, liveGeneration, completedStamp,
                  completedSeq, failed>>

RejectStaleIdentity ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 3 /\ submitFrontier = 2 /\ 1 \notin pendingOwners
  /\ phase' = "Failed" /\ failed' = TRUE
  /\ lastDisposition' = "DuplicateSubmission"
  /\ UNCHANGED <<eventCursor, submitFrontier, releaseFrontier,
                  bound, pendingOwners, released, liveGeneration, completedStamp,
                  completedSeq>>

RejectPrematureReuse ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 1 /\ submitFrontier = 2 /\ 1 \in pendingOwners
  /\ phase' = "Failed" /\ failed' = TRUE
  /\ lastDisposition' = "PrematureReuse"
  /\ UNCHANGED <<eventCursor, submitFrontier, releaseFrontier,
                  bound, pendingOwners, released, liveGeneration, completedStamp,
                  completedSeq>>

RejectPrematureCompletion ==
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 1 /\ 1 \in pendingOwners
  /\ phase' = "Failed" /\ failed' = TRUE
  /\ lastDisposition' = "PrematureCompletion"
  /\ UNCHANGED <<eventCursor, submitFrontier, releaseFrontier,
                  bound, pendingOwners, released, liveGeneration, completedStamp,
                  completedSeq>>

BugStaleIdentityBinding ==
  /\ AllowStaleIdentityBinding
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 3 /\ submitFrontier = 2 /\ 1 \notin pendingOwners
  /\ bound' = [bound EXCEPT ![2] = StaleSecondActual]
  /\ pendingOwners' = {2}
  /\ submitFrontier' = 3
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, eventCursor, releaseFrontier, released,
                  liveGeneration, completedStamp, completedSeq, failed>>

BugPrematureReuse ==
  /\ AllowPrematureReuse
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 1 /\ submitFrontier = 2 /\ 1 \in pendingOwners
  /\ bound' = [bound EXCEPT ![2] = SecondActual]
  /\ pendingOwners' = SubmissionIds
  /\ liveGeneration' = [liveGeneration EXCEPT ![1] = SecondActual.sourceGeneration]
  /\ submitFrontier' = 3
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, eventCursor, releaseFrontier, released,
                  completedStamp, completedSeq, failed>>

BugPrematureCompletion ==
  /\ AllowPrematureCompletion
  /\ phase = "Active" /\ ~failed
  /\ eventCursor = 1 /\ 1 \in pendingOwners
  /\ completedStamp' = [completedStamp EXCEPT ![1] = FirstActual.seqId]
  /\ completedSeq' = FirstActual.seqId
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, eventCursor, submitFrontier, releaseFrontier,
                  bound, pendingOwners, released, liveGeneration, failed>>

Next ==
  \/ SubmitFirst
  \/ SubmitSecond
  \/ SubmitCallbackFailure
  \/ (\E e \in Events : EndEvent(e))
  \/ ReleaseFirst
  \/ ReleaseSecond
  \/ ReleaseCallbackFailure
  \/ Finish
  \/ RejectStaleIdentity
  \/ RejectPrematureReuse
  \/ RejectPrematureCompletion
  \/ BugStaleIdentityBinding
  \/ BugPrematureReuse
  \/ BugPrematureCompletion

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ phase \in {"Active", "Finished", "Failed"}
  /\ eventCursor \in 1..5
  /\ submitFrontier \in 1..3
  /\ releaseFrontier \in 1..3
  /\ bound \in [SubmissionIds -> {None, FirstActual, SecondActual,
                                  StaleSecondActual}]
  /\ pendingOwners \subseteq SubmissionIds
  /\ released \subseteq SubmissionIds
  /\ liveGeneration \in [SourceIndices -> Nat]
  /\ completedStamp \in [SourceIndices -> Nat]
  /\ completedSeq \in Nat
  /\ failed \in BOOLEAN
  /\ lastDisposition \in {"Accepted", "CallbackFailure",
                            "DuplicateSubmission", "InvalidActualSource",
                            "StaleIdentity",
                            "PrematureReuse", "PrematureCompletion"}

QualifiedSchedule ==
  /\ FirstActual.sourceIndex = SecondActual.sourceIndex
  /\ FirstActual.sourceGeneration # SecondActual.sourceGeneration
  /\ FirstActual.storageGeneration # SecondActual.storageGeneration
  /\ FirstActual.seqId < SecondActual.seqId
  /\ SubmitEvent(1) < ReleaseEvent(1)
  /\ ReleaseEvent(1) < SubmitEvent(2)
  /\ SubmitEvent(2) < ReleaseEvent(2)

BoundMatchesPlan ==
  /\ bound[1] # None => bound[1] = FirstActual
  /\ bound[2] # None => bound[2] = SecondActual

NoReuseBeforeCompletion ==
  liveGeneration[1] = SecondActual.sourceGeneration =>
    /\ completedSeq >= FirstActual.seqId
    /\ 1 \notin pendingOwners

CompletionOnlyAfterRelease ==
  completedStamp[1] # 0 => 1 \notin pendingOwners

CompletedWaterlineMatchesReleases ==
  /\ completedSeq >= FirstActual.seqId => 1 \in released
  /\ completedSeq >= SecondActual.seqId => released = SubmissionIds

OrderedFrontiers ==
  /\ submitFrontier = 3 => bound[1] # None /\ bound[2] # None
  /\ releaseFrontier = 3 => released = SubmissionIds
  /\ released \subseteq {i \in SubmissionIds : bound[i] # None}

FinishedAfterAllTapeEvents ==
  phase = "Finished" =>
    /\ eventCursor = 5
    /\ submitFrontier = 3
    /\ releaseFrontier = 3
    /\ released = SubmissionIds
    /\ pendingOwners = {}

FailureIsTerminal == failed => phase = "Failed" /\ phase # "Finished"

Invariant ==
  /\ TypeOK
  /\ QualifiedSchedule
  /\ BoundMatchesPlan
  /\ NoReuseBeforeCompletion
  /\ CompletionOnlyAfterRelease
  /\ CompletedWaterlineMatchesReleases
  /\ OrderedFrontiers
  /\ FinishedAfterAllTapeEvents
  /\ FailureIsTerminal

THEOREM Spec => []Invariant

=============================================================================
