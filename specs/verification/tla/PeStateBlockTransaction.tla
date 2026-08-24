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

CONSTANTS StagedRefMultiplicity, Mutation, MaxRecordingEpoch
ASSUME StagedRefMultiplicity \in Nat \ {0}
ASSUME Mutation \in {"Guarded", "NoPoison", "NoRelease", "StaleOpen",
                     "LostDuplicate", "PoisonLeak", "StaleCapability"}
ASSUME MaxRecordingEpoch \in Nat \ {0}

Phases == {"Idle", "Recording", "EndPublication", "ApplyPrepared",
           "Poisoned", "Terminal"}
Failures == {"None", "EndBackend", "EndWrapper", "CaptureBackend",
             "ApplyBackend"}
VARIABLES phase, candidateOpen, stagedRefCount, captureVersion, failure,
          resetStarted, recordingEpoch, heldCapabilityEpoch,
          capabilityIssued, staleCapabilityWrite, capabilityWriteAttempted
vars == <<phase, candidateOpen, stagedRefCount, captureVersion, failure,
          resetStarted, recordingEpoch, heldCapabilityEpoch,
          capabilityIssued, staleCapabilityWrite, capabilityWriteAttempted>>
epochVars == <<recordingEpoch, heldCapabilityEpoch, capabilityIssued,
               staleCapabilityWrite, capabilityWriteAttempted>>

Init ==
  /\ phase = "Idle" /\ candidateOpen = FALSE /\ stagedRefCount = 0
  /\ captureVersion = 0 /\ failure = "None" /\ resetStarted = FALSE
  /\ recordingEpoch = 0 /\ heldCapabilityEpoch = 0
  /\ capabilityIssued = FALSE /\ staleCapabilityWrite = FALSE
  /\ capabilityWriteAttempted = FALSE

PoisonRequested ==
  /\ phase # "Terminal"
  /\ StateBlockMatches(phase, "PoisonRequested", "Poisoned", "FailStop",
                       "Discard", "Release", "Preserve")
  /\ phase' = "Poisoned"
  /\ candidateOpen' = IF Mutation = "PoisonLeak" THEN candidateOpen ELSE FALSE
  /\ stagedRefCount' = IF Mutation = "PoisonLeak" THEN stagedRefCount ELSE 0
  /\ UNCHANGED <<captureVersion, failure, resetStarted, recordingEpoch,
                 heldCapabilityEpoch, capabilityIssued,
                 staleCapabilityWrite, capabilityWriteAttempted>>

BeginFailed ==
  /\ phase = "Idle"
  /\ StateBlockMatches("Idle", "BeginFailed", "Idle", "Preserve",
                       "Preserve", "Preserve", "Preserve")
  /\ UNCHANGED vars
BeginAccepted ==
  /\ phase = "Idle"
  /\ recordingEpoch < MaxRecordingEpoch
  /\ StateBlockMatches("Idle", "BeginAccepted", "Recording",
                       "BeginRecording", "Discard", "Preserve", "Preserve")
  /\ phase' = "Recording" /\ candidateOpen' = TRUE /\ failure' = "None"
  /\ recordingEpoch' = recordingEpoch + 1
  /\ UNCHANGED <<stagedRefCount, captureVersion, resetStarted,
                 heldCapabilityEpoch, capabilityIssued,
                 staleCapabilityWrite, capabilityWriteAttempted>>

\* The owner fails closed when the monotonic epoch is exhausted instead of
\* wrapping and issuing a capability that could alias an old Begin.
BeginEpochExhausted ==
  /\ phase = "Idle" /\ recordingEpoch = MaxRecordingEpoch
  /\ StateBlockMatches("Idle", "PoisonRequested", "Poisoned", "FailStop",
                       "Discard", "Release", "Preserve")
  /\ phase' = "Poisoned" /\ candidateOpen' = FALSE
  /\ stagedRefCount' = 0 /\ failure' = "None"
  /\ UNCHANGED <<captureVersion, resetStarted, recordingEpoch,
                 heldCapabilityEpoch, capabilityIssued,
                 staleCapabilityWrite, capabilityWriteAttempted>>

IssueRecordingCapability ==
  /\ phase = "Recording" /\ ~capabilityIssued
  /\ heldCapabilityEpoch' = recordingEpoch
  /\ capabilityIssued' = TRUE
  /\ UNCHANGED <<phase, candidateOpen, stagedRefCount, captureVersion,
                 failure, resetStarted, recordingEpoch,
                 staleCapabilityWrite, capabilityWriteAttempted>>

AttemptStaleCapabilityWrite ==
  /\ phase = "Recording" /\ capabilityIssued
  /\ heldCapabilityEpoch # recordingEpoch
  /\ ~capabilityWriteAttempted
  /\ staleCapabilityWrite' = (Mutation = "StaleCapability")
  /\ capabilityWriteAttempted' = TRUE
  /\ UNCHANGED <<phase, candidateOpen, stagedRefCount, captureVersion,
                 failure, resetStarted, recordingEpoch,
                 heldCapabilityEpoch, capabilityIssued>>
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
  /\ UNCHANGED epochVars
EndBackendFailed ==
  /\ phase = "Recording"
  /\ StateBlockMatches("Recording", "EndBackendFailed", "Poisoned",
                       "FailStop", "Discard", "Preserve", "Preserve")
  /\ phase' = IF Mutation = "StaleOpen" THEN "Recording" ELSE "Poisoned"
  /\ candidateOpen' = (Mutation = "StaleOpen")
  /\ failure' = "EndBackend"
  /\ UNCHANGED <<stagedRefCount, captureVersion, resetStarted>>
  /\ UNCHANGED epochVars
EndWrapperFailed ==
  /\ phase = "EndPublication"
  /\ StateBlockMatches("EndPublication", "EndWrapperFailed", "Poisoned",
                       "FailStop", "Discard", "Preserve", "Preserve")
  /\ phase' = "Poisoned" /\ candidateOpen' = FALSE
  /\ failure' = "EndWrapper"
  /\ UNCHANGED <<stagedRefCount, captureVersion, resetStarted>>
  /\ UNCHANGED epochVars
EndPublished ==
  /\ phase = "EndPublication"
  /\ StateBlockMatches("EndPublication", "EndPublished", "Idle",
                       "PublishEnd", "Discard", "Preserve", "Preserve")
  /\ phase' = "Idle" /\ candidateOpen' = FALSE
  /\ UNCHANGED <<stagedRefCount, captureVersion, failure, resetStarted>>
  /\ UNCHANGED epochVars

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
  /\ UNCHANGED epochVars
CapturePublished ==
  /\ phase = "Idle"
  /\ captureVersion = 0
  /\ StateBlockMatches("Idle", "CapturePublished", "Idle", "PublishCapture",
                       "Preserve", "Preserve", "Publish")
  /\ captureVersion' = captureVersion + 1
  /\ UNCHANGED <<phase, candidateOpen, stagedRefCount, failure, resetStarted>>
  /\ UNCHANGED epochVars

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
  /\ UNCHANGED epochVars
ApplyBackendFailed ==
  /\ phase = "ApplyPrepared"
  /\ StateBlockMatches("ApplyPrepared", "ApplyBackendFailed", "Poisoned",
                       "FailStop", "Preserve", "Release", "Preserve")
  /\ phase' = IF Mutation = "NoPoison" THEN "ApplyPrepared" ELSE "Poisoned"
  /\ stagedRefCount' = IF Mutation = "NoRelease" THEN stagedRefCount ELSE 0
  /\ failure' = "ApplyBackend"
  /\ UNCHANGED <<candidateOpen, captureVersion, resetStarted>>
  /\ UNCHANGED epochVars
ApplyBackendAccepted ==
  /\ phase = "ApplyPrepared"
  /\ StateBlockMatches("ApplyPrepared", "ApplyBackendAccepted", "Idle",
                       "TransferApplyRefs", "Preserve", "Transfer", "Preserve")
  /\ phase' = "Idle" /\ stagedRefCount' = 0
  /\ UNCHANGED <<candidateOpen, captureVersion, failure, resetStarted>>
  /\ UNCHANGED epochVars

ResetStarted ==
  /\ phase \in {"Idle", "Recording", "Poisoned"}
  /\ StateBlockMatches(phase, "ResetStarted",
                       IF phase = "Recording" THEN "Idle" ELSE phase,
                       "AbandonForReset", "Discard", "Preserve", "Preserve")
  /\ phase' = IF phase = "Recording" THEN "Idle" ELSE phase
  /\ candidateOpen' = FALSE /\ resetStarted' = TRUE
  /\ UNCHANGED <<stagedRefCount, captureVersion, failure>>
  /\ UNCHANGED epochVars
ResetFailed ==
  /\ resetStarted /\ phase \in {"Idle", "Poisoned"}
  /\ StateBlockMatches(phase, "ResetFailed", phase, "Preserve", "Preserve",
                       "Preserve", "Preserve")
  /\ resetStarted' = FALSE
  /\ UNCHANGED <<phase, candidateOpen, stagedRefCount, captureVersion, failure>>
  /\ UNCHANGED epochVars
ResetAccepted ==
  /\ resetStarted /\ phase \in {"Idle", "Poisoned"}
  /\ StateBlockMatches(phase, "ResetAccepted", "Idle", "RecoverReset",
                       "Discard", "Release", "Preserve")
  /\ phase' = "Idle" /\ stagedRefCount' = 0 /\ failure' = "None"
  /\ resetStarted' = FALSE /\ UNCHANGED <<candidateOpen, captureVersion>>
  /\ UNCHANGED epochVars
Teardown ==
  /\ phase # "Terminal"
  /\ StateBlockMatches(phase, "Teardown", "Terminal", "Teardown",
                       "Discard", "Release", "Preserve")
  /\ phase' = "Terminal" /\ candidateOpen' = FALSE /\ stagedRefCount' = 0
  /\ failure' = "None" /\ resetStarted' = FALSE
  /\ UNCHANGED captureVersion
  /\ UNCHANGED epochVars

Next == PoisonRequested \/ BeginFailed \/ BeginAccepted \/ BeginEpochExhausted \/
        IssueRecordingCapability \/ AttemptStaleCapabilityWrite \/
        EndPreEffectFailed \/
        EndBackendAccepted \/ EndBackendFailed \/ EndWrapperFailed \/
        EndPublished \/ CapturePreEffectFailed \/ CaptureBackendFailed \/
        CapturePublished \/ ApplyPrepareFailed \/ ApplyPrepared \/
        ApplyBackendFailed \/ ApplyBackendAccepted \/ ResetStarted \/
        ResetFailed \/ ResetAccepted \/ Teardown

TypeOK == phase \in Phases /\ stagedRefCount \in Nat /\
          captureVersion \in Nat /\ failure \in Failures /\
          recordingEpoch \in 0..MaxRecordingEpoch /\
          heldCapabilityEpoch \in 0..MaxRecordingEpoch /\
          capabilityIssued \in BOOLEAN /\ staleCapabilityWrite \in BOOLEAN /\
          capabilityWriteAttempted \in BOOLEAN
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
PoisonOwnsNoCandidateOrRefs ==
  phase = "Poisoned" => ~candidateOpen /\ stagedRefCount = 0
CaptureVersionOnlyPublishes == captureVersion \in 0..1
NoStaleCapabilityWrite == ~staleCapabilityWrite
PoisonEventuallyResolves ==
  [](phase = "Poisoned" ~> (phase = "Idle" \/ phase = "Terminal"))

Spec == Init /\ [][Next]_vars /\ WF_vars(ResetStarted) /\
        WF_vars(ResetAccepted) /\ WF_vars(Teardown)
THEOREM Spec => []TypeOK /\ []CandidateMatchesSerialPhase /\
                 []PreparedRefMultiplicity /\ []FailedRefsReleased /\
                 []NoStaleOpenAfterPostEffectFailure /\
                 []NoRefsOutsidePrepared /\ []PoisonOwnsNoCandidateOrRefs /\
                 []CaptureVersionOnlyPublishes /\ []NoStaleCapabilityWrite
====
