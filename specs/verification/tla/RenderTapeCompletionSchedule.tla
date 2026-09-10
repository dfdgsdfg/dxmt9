---- MODULE RenderTapeCompletionSchedule ----
(******************************************************************************
 * Small refinement of RenderTapeCompletionSchedule.
 *
 * The production reducer is a cold adapter around real queue submit/release
 * callbacks.  This model keeps only its bounded temporal contract: two
 * authenticated, generation-qualified actions are appended in order, then
 * submitted and acknowledged through ordered frontiers.  A callback failure
 * is terminal; malformed, stale, duplicate, and skipped requests are
 * rejected without changing the reducer state.
 *
 * This is deliberately not a queue, Metal, resource, or pixel model.  The
 * callback bodies and production waterlines remain outside this abstraction.
 ******************************************************************************)

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS
  AllowPrematureRelease,
  AllowDuplicateRelease

ActionIndex == 1 .. 2

Keys == <<
  [iteration |-> 1, submission |-> 11, source |-> 1,
   actionOrdinal |-> 0, sourceGeneration |-> 7],
  [iteration |-> 1, submission |-> 29, source |-> 2,
   actionOrdinal |-> 1, sourceGeneration |-> 7]
>>

StaleKey ==
  [iteration |-> 2, submission |-> 11, source |-> 1,
   actionOrdinal |-> 0, sourceGeneration |-> 6]

VARIABLES
  phase,
  appended,
  active,
  submitFrontier,
  releaseFrontier,
  submitted,
  releaseCount,
  failed,
  finished,
  lastDisposition,
  witness

vars == <<phase, appended, active, submitFrontier, releaseFrontier,
          submitted, releaseCount, failed, finished, lastDisposition, witness>>

Core == [phase            |-> phase,
         appended         |-> appended,
         active           |-> active,
         submitFrontier   |-> submitFrontier,
         releaseFrontier  |-> releaseFrontier,
         submitted        |-> submitted,
         releaseCount     |-> releaseCount,
         failed           |-> failed,
         finished         |-> finished]

Init ==
  /\ phase = "Idle"
  /\ appended = 0
  /\ active = FALSE
  /\ submitFrontier = 1
  /\ releaseFrontier = 1
  /\ submitted = {}
  /\ releaseCount = [i \in ActionIndex |-> 0]
  /\ failed = FALSE
  /\ finished = FALSE
  /\ lastDisposition = "Accepted"
  /\ witness = [kind |-> "Accepted", before |-> Core, after |-> Core]

AcceptedMeta ==
  /\ lastDisposition' = "Accepted"
  /\ witness' = [witness EXCEPT !.kind = "Accepted"]

CallbackFailureMeta ==
  /\ lastDisposition' = "CallbackFailed"
  /\ witness' = [witness EXCEPT !.kind = "Accepted"]

RejectedMeta(disposition) ==
  /\ lastDisposition' = disposition
  /\ witness' = [kind |-> "Rejected", before |-> Core, after |-> Core]

AppendFirst ==
  /\ phase = "Idle"
  /\ appended = 0
  /\ appended' = 1
  /\ AcceptedMeta
  /\ UNCHANGED <<phase, active, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed, finished>>

AppendSecond ==
  /\ phase = "Idle"
  /\ appended = 1
  /\ appended' = 2
  /\ AcceptedMeta
  /\ UNCHANGED <<phase, active, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed, finished>>

BeginIteration ==
  /\ phase = "Idle"
  /\ appended = 2
  /\ active = FALSE
  /\ phase' = "Active"
  /\ active' = TRUE
  /\ AcceptedMeta
  /\ UNCHANGED <<appended, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed, finished>>

Submit(i) ==
  /\ i \in ActionIndex
  /\ phase = "Active" /\ active /\ ~failed
  /\ submitFrontier = i
  /\ i \notin submitted
  /\ submitted' = submitted \cup {i}
  /\ submitFrontier' = i + 1
  /\ AcceptedMeta
  /\ UNCHANGED <<phase, appended, active, releaseFrontier,
                  releaseCount, failed, finished>>

SubmitCallbackFailure(i) ==
  /\ i \in ActionIndex
  /\ phase = "Active" /\ active /\ ~failed
  /\ submitFrontier = i
  /\ i \notin submitted
  /\ submitFrontier' = i + 1
  /\ failed' = TRUE
  /\ CallbackFailureMeta
  /\ UNCHANGED <<phase, appended, active, releaseFrontier,
                  submitted, releaseCount, finished>>

Release(i) ==
  /\ i \in ActionIndex
  /\ phase = "Active" /\ active /\ ~failed
  /\ releaseFrontier = i
  /\ i \in submitted
  /\ releaseCount[i] = 0
  /\ releaseCount' = [releaseCount EXCEPT ![i] = 1]
  /\ releaseFrontier' = i + 1
  /\ AcceptedMeta
  /\ UNCHANGED <<phase, appended, active, submitFrontier,
                  submitted, failed, finished>>

ReleaseCallbackFailure(i) ==
  /\ i \in ActionIndex
  /\ phase = "Active" /\ active /\ ~failed
  /\ releaseFrontier = i
  /\ i \in submitted
  /\ releaseCount[i] = 0
  /\ releaseCount' = [releaseCount EXCEPT ![i] = 1]
  /\ releaseFrontier' = i + 1
  /\ failed' = TRUE
  /\ CallbackFailureMeta
  /\ UNCHANGED <<phase, appended, active, submitFrontier,
                  submitted, finished>>

FinishIteration ==
  /\ phase = "Active" /\ active /\ ~failed
  /\ submitFrontier = 3
  /\ releaseFrontier = 3
  /\ submitted = ActionIndex
  /\ releaseCount = [i \in ActionIndex |-> 1]
  /\ phase' = "Done"
  /\ active' = FALSE
  /\ finished' = TRUE
  /\ AcceptedMeta
  /\ UNCHANGED <<appended, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed>>

RejectStale ==
  /\ phase = "Active" /\ active /\ ~failed
  /\ submitFrontier = 1
  /\ StaleKey.iteration # Keys[1].iteration
  /\ StaleKey.sourceGeneration # Keys[1].sourceGeneration
  /\ RejectedMeta("Stale")
  /\ UNCHANGED <<phase, appended, active, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed, finished>>

RejectSkippedSubmit ==
  /\ phase = "Active" /\ active /\ ~failed
  /\ submitFrontier = 1
  /\ RejectedMeta("SkippedFrontier")
  /\ UNCHANGED <<phase, appended, active, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed, finished>>

RejectDuplicateSubmit ==
  /\ phase = "Active" /\ active /\ ~failed
  /\ 1 \in submitted
  /\ RejectedMeta("DuplicateAction")
  /\ UNCHANGED <<phase, appended, active, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed, finished>>

RejectReleaseBeforeSubmit ==
  /\ phase = "Active" /\ active /\ ~failed
  /\ releaseFrontier = 1
  /\ 1 \notin submitted
  /\ RejectedMeta("ReleaseBeforeSubmit")
  /\ UNCHANGED <<phase, appended, active, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed, finished>>

RejectSkippedRelease ==
  /\ phase = "Active" /\ active /\ ~failed
  /\ releaseFrontier = 1
  /\ submitted = ActionIndex
  /\ RejectedMeta("SkippedFrontier")
  /\ UNCHANGED <<phase, appended, active, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed, finished>>

RejectDuplicateRelease ==
  /\ phase = "Active" /\ active /\ ~failed
  /\ releaseCount[1] = 1
  /\ RejectedMeta("DuplicateRelease")
  /\ UNCHANGED <<phase, appended, active, submitFrontier, releaseFrontier,
                  submitted, releaseCount, failed, finished>>

PrematureReleaseBug ==
  /\ AllowPrematureRelease
  /\ phase = "Active" /\ active /\ ~failed
  /\ releaseFrontier = 1
  /\ 1 \notin submitted
  /\ releaseCount' = [releaseCount EXCEPT ![1] = 1]
  /\ releaseFrontier' = 2
  /\ AcceptedMeta
  /\ UNCHANGED <<phase, appended, active, submitFrontier,
                  submitted, failed, finished>>

DuplicateReleaseBug ==
  /\ AllowDuplicateRelease
  /\ phase = "Active" /\ active /\ ~failed
  /\ releaseCount[1] = 1
  /\ releaseCount' = [releaseCount EXCEPT ![1] = 2]
  /\ AcceptedMeta
  /\ UNCHANGED <<phase, appended, active, submitFrontier, releaseFrontier,
                  submitted, failed, finished>>

Next ==
  \/ AppendFirst
  \/ AppendSecond
  \/ BeginIteration
  \/ (\E i \in ActionIndex : Submit(i))
  \/ (\E i \in ActionIndex : SubmitCallbackFailure(i))
  \/ (\E i \in ActionIndex : Release(i))
  \/ (\E i \in ActionIndex : ReleaseCallbackFailure(i))
  \/ FinishIteration
  \/ RejectStale
  \/ RejectSkippedSubmit
  \/ RejectDuplicateSubmit
  \/ RejectReleaseBeforeSubmit
  \/ RejectSkippedRelease
  \/ RejectDuplicateRelease
  \/ PrematureReleaseBug
  \/ DuplicateReleaseBug

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ phase \in {"Idle", "Active", "Done"}
  /\ appended \in 0..2
  /\ active \in BOOLEAN
  /\ submitFrontier \in 1..3
  /\ releaseFrontier \in 1..3
  /\ submitted \subseteq ActionIndex
  /\ releaseCount \in [ActionIndex -> 0..2]
  /\ failed \in BOOLEAN
  /\ finished \in BOOLEAN
  /\ lastDisposition \in {"Accepted", "Rejected", "CallbackFailed", "Stale",
                            "SkippedFrontier", "DuplicateAction",
                            "ReleaseBeforeSubmit", "DuplicateRelease"}
  /\ witness.kind \in {"Accepted", "Rejected"}

ScheduleQualified ==
  /\ Len(Keys) = 2
  /\ Keys[1].iteration = Keys[2].iteration
  /\ Keys[1].sourceGeneration # 0
  /\ Keys[2].sourceGeneration # 0
  /\ Keys[1].actionOrdinal = 0
  /\ Keys[2].actionOrdinal = 1
  /\ Keys[1] # Keys[2]

SubmissionOrder == 2 \in submitted => 1 \in submitted
ReleaseOrder == releaseCount[2] = 1 => releaseCount[1] = 1
NoReleaseBeforeSubmit ==
  \A i \in ActionIndex : releaseCount[i] > 0 => i \in submitted
NoDuplicateRelease ==
  \A i \in ActionIndex : releaseCount[i] <= 1

FinishRequiresAllAcknowledged ==
  finished =>
    /\ ~failed
    /\ ~active
    /\ submitted = ActionIndex
    /\ submitFrontier = 3
    /\ releaseFrontier = 3
    /\ releaseCount = [i \in ActionIndex |-> 1]

CallbackFailureTerminal == failed => ~finished

NoRejectedMutation ==
  witness.kind = "Rejected" => witness.before = witness.after

Invariant ==
  /\ TypeOK
  /\ ScheduleQualified
  /\ SubmissionOrder
  /\ ReleaseOrder
  /\ NoReleaseBeforeSubmit
  /\ NoDuplicateRelease
  /\ FinishRequiresAllAcknowledged
  /\ CallbackFailureTerminal
  /\ NoRejectedMutation

THEOREM Spec => []Invariant

=============================================================================
