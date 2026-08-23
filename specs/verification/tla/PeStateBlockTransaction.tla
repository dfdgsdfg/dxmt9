---- MODULE PeStateBlockTransaction ----
(***************************************************************************
 * Small temporal model for the PE StateBlock Apply transaction. Preparation
 * owns staged references; backend failure poisons the recorder because the
 * unix operation may have partially mutated; commit failure uses the same
 * conservative boundary. A successful Reset recovers the poisoned recorder,
 * as required by the task-1B coordination contract. Terminal is reachable
 * only through explicit teardown, not as the normal Reset policy.
 ***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS StagedRefs, Mutation

ASSUME StagedRefs # {}
ASSUME Mutation \in {"Guarded", "NoPoison", "NoRelease"}

Phases == {"Idle", "Prepared", "Backend", "Committed", "Poisoned",
           "Terminal"}
CaptureDispositions == {"Pending", "Materialized", "Rejected", "Skipped",
                        "None"}

VARIABLES phase, stagedRefs, captureDisposition, failure

vars == <<phase, stagedRefs, captureDisposition, failure>>

Init ==
  /\ phase = "Idle"
  /\ stagedRefs = {}
  /\ captureDisposition = "None"
  /\ failure = "None"

PrepareSuccess ==
  /\ phase = "Idle"
  /\ phase' = "Prepared"
  /\ stagedRefs' = StagedRefs
  /\ captureDisposition' = "Pending"
  /\ failure' = "None"

PrepareFailure ==
  /\ phase = "Idle"
  /\ UNCHANGED vars

BackendSuccess ==
  /\ phase = "Prepared"
  /\ phase' = "Backend"
  /\ UNCHANGED <<stagedRefs, captureDisposition, failure>>

BackendFailure ==
  /\ phase = "Prepared"
  /\ phase' = IF Mutation = "NoPoison" THEN "Prepared" ELSE "Poisoned"
  /\ stagedRefs' = IF Mutation = "NoRelease" THEN stagedRefs ELSE {}
  /\ failure' = "Backend"
  /\ UNCHANGED captureDisposition

CommitSuccess ==
  /\ phase = "Backend"
  /\ phase' = "Committed"
  /\ stagedRefs' = {}
  /\ UNCHANGED <<captureDisposition, failure>>

CommitFailure ==
  /\ phase = "Backend"
  /\ phase' = IF Mutation = "NoPoison" THEN "Backend" ELSE "Poisoned"
  /\ stagedRefs' = IF Mutation = "NoRelease" THEN stagedRefs ELSE {}
  /\ failure' = "Commit"
  /\ UNCHANGED captureDisposition

CaptureMaterialized ==
  /\ phase = "Committed"
  /\ phase' = "Idle"
  /\ captureDisposition' = "Materialized"
  /\ UNCHANGED <<stagedRefs, failure>>

CaptureRejected ==
  /\ phase = "Committed"
  /\ phase' = "Idle"
  /\ captureDisposition' = "Rejected"
  /\ UNCHANGED <<stagedRefs, failure>>

CaptureSkipped ==
  /\ phase = "Committed"
  /\ phase' = "Idle"
  /\ captureDisposition' = "Skipped"
  /\ UNCHANGED <<stagedRefs, failure>>

ResetSuccess ==
  /\ phase = "Poisoned"
  /\ phase' = "Idle"
  /\ stagedRefs' = {}
  /\ captureDisposition' = "None"
  /\ failure' = "None"

Teardown ==
  /\ phase # "Terminal"
  /\ phase' = "Terminal"
  /\ stagedRefs' = {}
  /\ captureDisposition' = "None"
  /\ failure' = "None"

Next == PrepareSuccess \/ PrepareFailure \/ BackendSuccess \/ BackendFailure \/
        CommitSuccess \/ CommitFailure \/ CaptureMaterialized \/
        CaptureRejected \/ CaptureSkipped \/ ResetSuccess \/ Teardown

TypeOK ==
  /\ phase \in Phases
  /\ stagedRefs \subseteq StagedRefs
  /\ captureDisposition \in CaptureDispositions
  /\ failure \in {"None", "Backend", "Commit"}

PreparedOwnsStagedRefs ==
  phase \in {"Prepared", "Backend"} => stagedRefs = StagedRefs

FailurePoisoned == failure # "None" => phase = "Poisoned"

FailedRefsReleased == failure # "None" => stagedRefs = {}

CommittedHasNoStagedRefs == phase \in {"Committed", "Idle", "Terminal"} =>
  stagedRefs = {}

CaptureSettled == phase = "Idle" =>
  captureDisposition \in {"None", "Materialized", "Rejected", "Skipped"}

PoisonEventuallyResolves ==
  [](phase = "Poisoned" ~> (phase = "Idle" \/ phase = "Terminal"))

Spec == Init /\ [][Next]_vars /\
        WF_vars(PrepareSuccess) /\ WF_vars(BackendSuccess) /\
        WF_vars(BackendFailure) /\ WF_vars(CommitSuccess) /\
        WF_vars(CommitFailure) /\ WF_vars(CaptureMaterialized) /\
        WF_vars(CaptureRejected) /\ WF_vars(CaptureSkipped) /\
        WF_vars(ResetSuccess) /\ WF_vars(Teardown)

THEOREM Spec => []TypeOK /\ []PreparedOwnsStagedRefs /\
                 []FailurePoisoned /\ []FailedRefsReleased /\
                 []CommittedHasNoStagedRefs /\ []CaptureSettled

====
