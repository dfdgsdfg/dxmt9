---- MODULE QueryPublicationFence ----
EXTENDS Naturals, TLC

(* Published END receipt, independent submit/completion and sticky async wake.
   Native binding: query::classify/submissionWorkTarget, CommandQueue::publishQueryEnd,
   pollQueryFence, QueueLifecycleController::requestQuerySubmission.
   Excludes Metal result bytes, mutex memory ordering, and admission capacity. *)
CONSTANTS MaxSeq, MaxEpoch, Mutation
VARIABLES published, submitted, completed, epoch, issueEpoch, issueSeq,
          oldIssueSeq, issued, failed, requested, obligation,
          ok, observedCompleted, observedIssue, observedEpoch,
          observedIssueEpoch, observedFailed
vars == <<published, submitted, completed, epoch, issueEpoch, issueSeq,
          oldIssueSeq, issued, failed, requested, obligation,
          ok, observedCompleted, observedIssue, observedEpoch,
          observedIssueEpoch, observedFailed>>
Init ==
  /\ published = 0 /\ submitted = 0 /\ completed = 0
  /\ epoch = 1 /\ issueEpoch = 1 /\ issueSeq = 0 /\ oldIssueSeq = 0
  /\ issued = FALSE /\ failed = FALSE /\ requested = 0 /\ obligation = 0
  /\ ok = FALSE /\ observedCompleted = 0 /\ observedIssue = 0
  /\ observedEpoch = 1 /\ observedIssueEpoch = 1 /\ observedFailed = FALSE
NoObservation == UNCHANGED <<ok, observedCompleted, observedIssue, observedEpoch,
                            observedIssueEpoch, observedFailed>>
Publish ==
  /\ ~failed /\ published < MaxSeq
  /\ published' = published + 1
  /\ UNCHANGED <<submitted, completed, epoch, issueEpoch, issueSeq, oldIssueSeq,
                 issued, failed, requested, obligation>>
  /\ NoObservation
End ==
  /\ ~failed
  /\ issueSeq' = published /\ oldIssueSeq' = issueSeq
  /\ issueEpoch' = epoch /\ issued' = TRUE
  /\ UNCHANGED <<published, submitted, completed, epoch, failed, requested, obligation>>
  /\ NoObservation
Flush ==
  /\ issued /\ ~failed /\ issueEpoch = epoch /\ completed < issueSeq
  /\ requested' = IF Mutation = "DropRequest" THEN 0
                   ELSE IF requested < issueSeq THEN issueSeq ELSE requested
  /\ obligation' = IF obligation < issueSeq THEN issueSeq ELSE obligation
  /\ completed' = IF Mutation = "EarlyCompletion" THEN issueSeq ELSE completed
  /\ UNCHANGED <<published, submitted, epoch, issueEpoch, issueSeq, oldIssueSeq,
                 issued, failed>>
  /\ NoObservation
EffectiveSubmissionTarget ==
  IF requested > (IF Mutation = "RetireOnPublish" THEN published ELSE submitted)
  THEN requested ELSE 0
Submit ==
  /\ ~failed /\ submitted < EffectiveSubmissionTarget
  /\ submitted' = requested
  /\ UNCHANGED <<published, completed, epoch, issueEpoch, issueSeq, oldIssueSeq,
                 issued, failed, requested, obligation>>
  /\ NoObservation
GPUComplete ==
  /\ ~failed /\ completed < submitted
  /\ completed' = completed + 1
  /\ UNCHANGED <<published, submitted, epoch, issueEpoch, issueSeq, oldIssueSeq,
                 issued, failed, requested, obligation>>
  /\ NoObservation
Poll ==
  /\ issued
  /\ ok' = /\ (Mutation = "IgnoreFailure" \/ ~failed)
            /\ issueEpoch = epoch
            /\ completed >= (IF Mutation = "OldIssue" THEN oldIssueSeq ELSE issueSeq)
  /\ observedCompleted' = completed /\ observedIssue' = issueSeq
  /\ observedEpoch' = epoch /\ observedIssueEpoch' = issueEpoch
  /\ observedFailed' = failed
  /\ UNCHANGED <<published, submitted, completed, epoch, issueEpoch, issueSeq,
                 oldIssueSeq, issued, failed, requested, obligation>>
Reset ==
  /\ epoch < MaxEpoch /\ epoch' = epoch + 1
  /\ UNCHANGED <<published, submitted, completed, issueEpoch, issueSeq, oldIssueSeq,
                 issued, failed, requested, obligation>>
  /\ NoObservation
Fail ==
  /\ ~failed /\ failed' = TRUE
  /\ UNCHANGED <<published, submitted, completed, epoch, issueEpoch, issueSeq,
                 oldIssueSeq, issued, requested, obligation>>
  /\ NoObservation
Next == Publish \/ End \/ Flush \/ Submit \/ GPUComplete \/ Poll \/ Reset \/ Fail
Spec == Init /\ [][Next]_vars /\ WF_vars(Submit) /\ WF_vars(GPUComplete)
TypeOK ==
  /\ published \in 0..MaxSeq /\ submitted \in 0..MaxSeq /\ completed \in 0..MaxSeq
  /\ issueSeq \in 0..MaxSeq /\ oldIssueSeq \in 0..MaxSeq
  /\ requested \in 0..MaxSeq /\ obligation \in 0..MaxSeq
  /\ epoch \in 1..MaxEpoch /\ issueEpoch \in 1..MaxEpoch
  /\ issued \in BOOLEAN /\ failed \in BOOLEAN /\ ok \in BOOLEAN
NoInventedCompletion == completed <= submitted /\ submitted <= published
ReceiptIsPublished == issueSeq <= published
CompletionSafety == ok => /\ observedCompleted >= observedIssue
                          /\ observedEpoch = observedIssueEpoch /\ ~observedFailed
StickyRequest == (~failed /\ completed < obligation) => requested >= obligation
SubmissionPressureSafety == (~failed /\ submitted < obligation) =>
                             EffectiveSubmissionTarget >= obligation
SubmissionLocality == (submitted >= requested) => EffectiveSubmissionTarget = 0
Progress == \A target \in 1..MaxSeq:
              (requested >= target /\ ~failed) ~> (completed >= target \/ failed)
====
