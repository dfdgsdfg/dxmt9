---- MODULE QueryEndCoalescing ----
EXTENDS Integers, FiniteSets, TLC

(* Deferred ordinary END refinement. Two writer generations, two public
   query slots, bounded inflight admission. Initializer success is composed
   with QueryInitializerFence; Metal result bytes are outside this model.
   Production: EndMarker::bind/classifyEnd, publishQueryEnd, pollQueryFence,
   QueueLifecycleController::commitCurrentChunk after sealAndPublish. *)
CONSTANTS MaxSources, Queries, Mutation
VARIABLES writer, work, binding, published, submitted, completed, wanted,
          requested, issue, prefix, epoch, issueEpoch, failed, ok, witness
vars == <<writer, work, binding, published, submitted, completed, wanted,
          requested, issue, prefix, epoch, issueEpoch, failed, ok, witness>>
Init ==
  /\ writer = 1 /\ work = FALSE
  /\ binding = [s \in 1..MaxSources |-> 0]
  /\ published = 0 /\ submitted = 0 /\ completed = 0
  /\ wanted = {} /\ requested = 0
  /\ issue = [q \in Queries |-> -1]
  /\ prefix = [q \in Queries |-> 0]
  /\ issueEpoch = [q \in Queries |-> 1] /\ epoch = 1
  /\ failed = FALSE /\ ok = FALSE
  /\ witness = [bound |-> FALSE, target |-> 0, done |-> 0,
                 sameEpoch |-> TRUE, live |-> TRUE]
NoObservation == UNCHANGED <<ok, witness>>
Append ==
  /\ ~failed /\ writer <= MaxSources /\ ~work /\ work' = TRUE
  /\ UNCHANGED <<writer, binding, published, submitted, completed, wanted,
                 requested, issue, prefix, epoch, issueEpoch, failed>>
  /\ NoObservation
End(q) ==
  /\ ~failed
  /\ ~work \/ published - completed < 1
  /\ issue' = [issue EXCEPT ![q] = IF work THEN writer ELSE 0]
  /\ prefix' = [prefix EXCEPT ![q] = IF work THEN 0 ELSE published]
  /\ issueEpoch' = [issueEpoch EXCEPT ![q] = epoch]
  /\ UNCHANGED <<writer, work, binding, published, submitted, completed,
                 wanted, requested, epoch, failed>>
  /\ NoObservation
Publish ==
  /\ ~failed /\ work /\ published - completed < 1
  /\ binding' = [binding EXCEPT ![writer] =
       IF Mutation = "StaleBinding" /\ writer > 1 THEN published ELSE published + 1]
  /\ published' = published + 1 /\ writer' = writer + 1 /\ work' = FALSE
  /\ UNCHANGED <<submitted, completed, wanted, requested, issue, prefix,
                 epoch, issueEpoch, failed>>
  /\ NoObservation
Flush(q) ==
  /\ ~failed /\ issue[q] >= 0 /\ issueEpoch[q] = epoch
  /\ wanted' = IF issue[q] > 0 THEN wanted \cup {issue[q]} ELSE wanted
  /\ requested' = IF prefix[q] > requested THEN prefix[q] ELSE requested
  /\ UNCHANGED <<writer, work, binding, published, submitted, completed,
                 issue, prefix, epoch, issueEpoch, failed>>
  /\ NoObservation
FlushPublish == writer \in wanted /\ Publish
(* The Tape seal can have succeeded before a rejected binding. Preserve the
   published source, leave the failed receipt unbound and stop before encode. *)
FailAfterSeal ==
  /\ ~failed /\ work /\ published - completed < 1
  /\ published' = published + 1 /\ writer' = writer + 1 /\ work' = FALSE
  /\ failed' = TRUE
  /\ UNCHANGED <<binding, submitted, completed, wanted, requested, issue,
                 prefix, epoch, issueEpoch>>
  /\ NoObservation
Targets == {binding[s] : s \in wanted}
(* Full admission requests the already published prefix, not a future ID. *)
IssuePressure ==
  /\ ~failed /\ work /\ published - completed >= 1
  /\ requested' = published
  /\ UNCHANGED <<writer, work, binding, published, submitted, completed,
                 wanted, issue, prefix, epoch, issueEpoch, failed>>
  /\ NoObservation
EffectiveTargets == IF Mutation = "DropPressure" THEN {} ELSE Targets
Target == CHOOSE n \in EffectiveTargets \cup {requested, 0}:
             \A v \in EffectiveTargets \cup {requested, 0}: n >= v
Submit ==
  /\ ~failed /\ submitted < Target /\ submitted' = Target
  /\ UNCHANGED <<writer, work, binding, published, completed, wanted,
                 requested, issue, prefix, epoch, issueEpoch, failed>>
  /\ NoObservation
Complete ==
  /\ ~failed /\ completed < submitted /\ completed' = completed + 1
  /\ UNCHANGED <<writer, work, binding, published, submitted, wanted,
                 requested, issue, prefix, epoch, issueEpoch, failed>>
  /\ NoObservation
Poll(q) ==
  /\ issue[q] >= 0
  /\ LET target == IF issue[q] = 0 THEN prefix[q] ELSE binding[issue[q]]
         bound == issue[q] = 0 \/ target > 0
     IN /\ ok' = (bound \/ Mutation = "UnboundSuccess") /\ ~failed /\
                  issueEpoch[q] = epoch /\ completed >= target
        /\ witness' = [bound |-> bound, target |-> target, done |-> completed,
                       sameEpoch |-> issueEpoch[q] = epoch, live |-> ~failed]
  /\ UNCHANGED <<writer, work, binding, published, submitted, completed,
                 wanted, requested, issue, prefix, epoch, issueEpoch, failed>>
Release(q) ==
  /\ issue' = [issue EXCEPT ![q] = -1]
  /\ UNCHANGED <<writer, work, binding, published, submitted, completed,
                 wanted, requested, prefix, epoch, issueEpoch, failed>>
  /\ NoObservation
Reset ==
  /\ epoch = 1 /\ epoch' = 2
  /\ UNCHANGED <<writer, work, binding, published, submitted, completed,
                 wanted, requested, issue, prefix, issueEpoch, failed>>
  /\ NoObservation
Fail ==
  /\ ~failed /\ failed' = TRUE
  /\ UNCHANGED <<writer, work, binding, published, submitted, completed,
                 wanted, requested, issue, prefix, epoch, issueEpoch>>
  /\ NoObservation
Next == Append \/ Publish \/ FailAfterSeal \/ IssuePressure \/ Submit \/ Complete \/ Reset \/ Fail \/
        (\E q \in Queries: End(q) \/ Flush(q) \/ Poll(q) \/ Release(q))
Spec == Init /\ [][Next]_vars /\ WF_vars(FlushPublish) /\
        WF_vars(Submit) /\ WF_vars(Complete)
TypeOK ==
  /\ writer \in 1..(MaxSources+1) /\ work \in BOOLEAN
  /\ binding \in [1..MaxSources -> 0..MaxSources]
  /\ wanted \subseteq 1..MaxSources
  /\ completed <= submitted /\ submitted <= published /\ published < writer
  /\ published \in 0..MaxSources /\ epoch \in 1..2
  /\ issue \in [Queries -> (-1)..MaxSources]
CompletionSafety == ok => witness.bound /\ witness.sameEpoch /\ witness.live /\
                          witness.done >= witness.target
BindingIdentity == \A s \in 1..MaxSources:
                     IF s < writer THEN binding[s] = s \/ (failed /\ binding[s] = 0)
                     ELSE binding[s] = 0
AdmissionProgress == \A s \in 1..MaxSources:
                      (s \in wanted /\ ~failed) ~> (completed >= s \/ failed)
====
