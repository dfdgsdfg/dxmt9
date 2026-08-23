---- MODULE PeStateBlockTransaction ----
(***************************************************************************
 * Bounded serial/reference model for the PE StateBlock transaction owner.
 * PeStateBlockTransitionTable is generated from the production C++ matrix;
 * every modeled step asserts its exact phase/event/action/effect row.
 * stagedRefCount is an abstract cardinality that includes duplicate COM
 * identities. Native fake-COM tests provide concrete AddRef/Release/transfer
 * evidence for repeated identities in different category/slot owners.
*)
EXTENDS Naturals, Sequences, TLC, PeStateBlockTransitionTable

CONSTANTS StagedRefMultiplicity, Mutation
ASSUME StagedRefMultiplicity \in Nat \ {0}
ASSUME Mutation \in {"Guarded", "NoPoison", "NoRelease", "StaleOpen",
                     "LostDuplicate"}

Phases == {"Idle", "Recording", "EndPublication", "ApplyPrepared",
           "Poisoned", "Terminal"}
Failures == {"None", "EndBackend", "EndWrapper", "CaptureBackend",
             "ApplyBackend"}
VARIABLES phase, candidateOpen, stagedRefCount, captureVersion, failure,
          resetStarted
vars == <<phase, candidateOpen, stagedRefCount, captureVersion, failure,
          resetStarted>>

Init ==
  /\ phase = "Idle" /\ candidateOpen = FALSE /\ stagedRefCount = 0
  /\ captureVersion = 0 /\ failure = "None" /\ resetStarted = FALSE

BeginFailed ==
  /\ phase = "Idle"
  /\ StateBlockMatches("Idle", "BeginFailed", "Idle", "Preserve",
                       "Preserve", "Preserve", "Preserve")
  /\ UNCHANGED vars
BeginAccepted ==
  /\ phase = "Idle"
  /\ StateBlockMatches("Idle", "BeginAccepted", "Recording",
                       "BeginRecording", "Discard", "Preserve", "Preserve")
  /\ phase' = "Recording" /\ candidateOpen' = TRUE /\ failure' = "None"
  /\ UNCHANGED <<stagedRefCount, captureVersion, resetStarted>>
EndPreEffectFailed ==
  /\ phase = "Recording"
  /\ StateBlockMatches("Recording", "EndPreEffectFailed", "Recording",
                       "Preserve", "Preserve", "Preserve", "Preserve")
  /\ UNCHANGED vars
EndBackendAccepted ==
  /\ phase = "Recording"
  /\ StateBlockMatches("Recording", "EndBackendAccepted", "EndPublication",
                       "EnterEndPublication", "Preserve", "Preserve",
                       "Preserve")
  /\ phase' = "EndPublication"
  /\ UNCHANGED <<candidateOpen, stagedRefCount, captureVersion, failure,
                 resetStarted>>
EndBackendFailed ==
  /\ phase = "Recording"
  /\ StateBlockMatches("Recording", "EndBackendFailed", "Poisoned",
                       "FailStop", "Discard", "Preserve", "Preserve")
  /\ phase' = IF Mutation = "StaleOpen" THEN "Recording" ELSE "Poisoned"
  /\ candidateOpen' = (Mutation = "StaleOpen")
  /\ failure' = "EndBackend"
  /\ UNCHANGED <<stagedRefCount, captureVersion, resetStarted>>
EndWrapperFailed ==
  /\ phase = "EndPublication"
  /\ StateBlockMatches("EndPublication", "EndWrapperFailed", "Poisoned",
                       "FailStop", "Discard", "Preserve", "Preserve")
  /\ phase' = "Poisoned" /\ candidateOpen' = FALSE
  /\ failure' = "EndWrapper"
  /\ UNCHANGED <<stagedRefCount, captureVersion, resetStarted>>
EndPublished ==
  /\ phase = "EndPublication"
  /\ StateBlockMatches("EndPublication", "EndPublished", "Idle",
                       "PublishEnd", "Discard", "Preserve", "Preserve")
  /\ phase' = "Idle" /\ candidateOpen' = FALSE
  /\ UNCHANGED <<stagedRefCount, captureVersion, failure, resetStarted>>

CapturePreEffectFailed ==
  /\ phase = "Idle"
  /\ StateBlockMatches("Idle", "CapturePreEffectFailed", "Idle", "Preserve",
                       "Preserve", "Preserve", "Preserve")
  /\ UNCHANGED vars
CaptureBackendFailed ==
  /\ phase = "Idle"
  /\ StateBlockMatches("Idle", "CaptureBackendFailed", "Poisoned",
                       "FailStop", "Preserve", "Preserve", "Preserve")
  /\ phase' = IF Mutation = "NoPoison" THEN "Idle" ELSE "Poisoned"
  /\ failure' = "CaptureBackend"
  /\ UNCHANGED <<candidateOpen, stagedRefCount, captureVersion, resetStarted>>
CapturePublished ==
  /\ phase = "Idle"
  /\ captureVersion = 0
  /\ StateBlockMatches("Idle", "CapturePublished", "Idle", "PublishCapture",
                       "Preserve", "Preserve", "Publish")
  /\ captureVersion' = captureVersion + 1
  /\ UNCHANGED <<phase, candidateOpen, stagedRefCount, failure, resetStarted>>

ApplyPrepareFailed ==
  /\ phase = "Idle"
  /\ StateBlockMatches("Idle", "ApplyPrepareFailed", "Idle", "Preserve",
                       "Preserve", "Preserve", "Preserve")
  /\ UNCHANGED vars
ApplyPrepared ==
  /\ phase = "Idle"
  /\ StateBlockMatches("Idle", "ApplyPrepared", "ApplyPrepared",
                       "RetainApplyRefs", "Preserve", "Retain", "Preserve")
  /\ phase' = "ApplyPrepared"
  /\ stagedRefCount' = IF Mutation = "LostDuplicate"
                       THEN StagedRefMultiplicity - 1
                       ELSE StagedRefMultiplicity
  /\ UNCHANGED <<candidateOpen, captureVersion, failure, resetStarted>>
ApplyBackendFailed ==
  /\ phase = "ApplyPrepared"
  /\ StateBlockMatches("ApplyPrepared", "ApplyBackendFailed", "Poisoned",
                       "FailStop", "Preserve", "Release", "Preserve")
  /\ phase' = IF Mutation = "NoPoison" THEN "ApplyPrepared" ELSE "Poisoned"
  /\ stagedRefCount' = IF Mutation = "NoRelease" THEN stagedRefCount ELSE 0
  /\ failure' = "ApplyBackend"
  /\ UNCHANGED <<candidateOpen, captureVersion, resetStarted>>
ApplyBackendAccepted ==
  /\ phase = "ApplyPrepared"
  /\ StateBlockMatches("ApplyPrepared", "ApplyBackendAccepted", "Idle",
                       "TransferApplyRefs", "Preserve", "Transfer", "Preserve")
  /\ phase' = "Idle" /\ stagedRefCount' = 0
  /\ UNCHANGED <<candidateOpen, captureVersion, failure, resetStarted>>

ResetStarted ==
  /\ phase \in {"Idle", "Recording", "Poisoned"}
  /\ StateBlockMatches(phase, "ResetStarted",
                       IF phase = "Recording" THEN "Idle" ELSE phase,
                       "AbandonForReset", "Discard", "Preserve", "Preserve")
  /\ phase' = IF phase = "Recording" THEN "Idle" ELSE phase
  /\ candidateOpen' = FALSE /\ resetStarted' = TRUE
  /\ UNCHANGED <<stagedRefCount, captureVersion, failure>>
ResetFailed ==
  /\ resetStarted /\ phase \in {"Idle", "Poisoned"}
  /\ StateBlockMatches(phase, "ResetFailed", phase, "Preserve", "Preserve",
                       "Preserve", "Preserve")
  /\ resetStarted' = FALSE
  /\ UNCHANGED <<phase, candidateOpen, stagedRefCount, captureVersion, failure>>
ResetAccepted ==
  /\ resetStarted /\ phase \in {"Idle", "Poisoned"}
  /\ StateBlockMatches(phase, "ResetAccepted", "Idle", "RecoverReset",
                       "Discard", "Release", "Preserve")
  /\ phase' = "Idle" /\ stagedRefCount' = 0 /\ failure' = "None"
  /\ resetStarted' = FALSE /\ UNCHANGED <<candidateOpen, captureVersion>>
Teardown ==
  /\ phase # "Terminal"
  /\ StateBlockMatches(phase, "Teardown", "Terminal", "Teardown",
                       "Discard", "Release", "Preserve")
  /\ phase' = "Terminal" /\ candidateOpen' = FALSE /\ stagedRefCount' = 0
  /\ failure' = "None" /\ resetStarted' = FALSE
  /\ UNCHANGED captureVersion

Next == BeginFailed \/ BeginAccepted \/ EndPreEffectFailed \/
        EndBackendAccepted \/ EndBackendFailed \/ EndWrapperFailed \/
        EndPublished \/ CapturePreEffectFailed \/ CaptureBackendFailed \/
        CapturePublished \/ ApplyPrepareFailed \/ ApplyPrepared \/
        ApplyBackendFailed \/ ApplyBackendAccepted \/ ResetStarted \/
        ResetFailed \/ ResetAccepted \/ Teardown

TypeOK == phase \in Phases /\ stagedRefCount \in Nat /\
          captureVersion \in Nat /\ failure \in Failures
CandidateMatchesSerialPhase ==
  candidateOpen = (phase \in {"Recording", "EndPublication"})
PreparedRefMultiplicity ==
  phase = "ApplyPrepared" => stagedRefCount = StagedRefMultiplicity
FailedRefsReleased ==
  failure = "ApplyBackend" /\ phase = "Poisoned" => stagedRefCount = 0
NoStaleOpenAfterPostEffectFailure ==
  failure \in {"EndBackend", "EndWrapper", "CaptureBackend",
               "ApplyBackend"} => phase = "Poisoned"
NoRefsOutsidePrepared == phase # "ApplyPrepared" => stagedRefCount = 0
CaptureVersionOnlyPublishes == captureVersion \in 0..1
PoisonEventuallyResolves ==
  [](phase = "Poisoned" ~> (phase = "Idle" \/ phase = "Terminal"))

Spec == Init /\ [][Next]_vars /\ WF_vars(ResetStarted) /\
        WF_vars(ResetAccepted) /\ WF_vars(Teardown)
THEOREM Spec => []TypeOK /\ []CandidateMatchesSerialPhase /\
                 []PreparedRefMultiplicity /\ []FailedRefsReleased /\
                 []NoStaleOpenAfterPostEffectFailure /\
                 []NoRefsOutsidePrepared /\ []CaptureVersionOnlyPublishes
====
