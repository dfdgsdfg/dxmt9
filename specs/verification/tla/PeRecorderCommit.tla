---- MODULE PeRecorderCommit ----
(***************************************************************************
 * Bounded commit settlement model.  The phase/event vocabulary is the TLA+
 * twin of settleRecorderCommit in d3d9_pe_commit_transition.hpp.
 *
 * PeRecorderCommitTable is generated from that C++ algebra. Every modeled
 * action checks its exact generated row, so changing the C++ transition or
 * forgetting a capture disposition makes the freshness/isomorphism gate fail.
 * Complete deliberately clears commandAccepted and rearms the bounded token
 * sets: this models the reusable builder after WarmEpochAdvance rather than
 * ending the model at the first successful chunk.
 ***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences, TLC, PeRecorderCommitTable

CONSTANTS ByteTokens, HandleTokens, PinTokens, PendingTokens, AliasTokens,
          Mutation, MaxOperations

ASSUME ByteTokens # {} /\ HandleTokens # {} /\ PinTokens # {} /\
       PendingTokens # {} /\ AliasTokens # {}
ASSUME Mutation \in {"Guarded", "ParentBeforeAlias", "EarlyReset",
                     "StuckSuccess"}
ASSUME MaxOperations \in Nat \ {0}

Phases == {"Unsealed", "Sealed", "Accepted", "CaptureSettled",
           "Draining", "Drained", "Reset", "WarmAdvanced", "Discarded"}
CaptureDispositions == {"Pending", "Materialized", "Rejected", "Skipped",
                        "None"}

VARIABLES phase, bytes, sealedBytes, recordCount, handleCount, pins,
          pendingRefs, aliases, parentPending, destroyed, bridgeFailures,
          commandAccepted, captureDisposition

vars == <<phase, bytes, sealedBytes, recordCount, handleCount, pins,
          pendingRefs, aliases, parentPending, destroyed, bridgeFailures,
          commandAccepted, captureDisposition>>

Init ==
  /\ phase = "Unsealed"
  /\ bytes = ByteTokens
  /\ sealedBytes = ByteTokens
  /\ recordCount = Cardinality(HandleTokens)
  /\ handleCount = Cardinality(HandleTokens)
  /\ pins = PinTokens
  /\ pendingRefs = PendingTokens
  /\ aliases = AliasTokens
  /\ parentPending = TRUE
  /\ destroyed = <<>>
  /\ bridgeFailures = 0
  /\ commandAccepted = FALSE
  /\ captureDisposition = "Pending"

SealSuccess ==
  /\ phase = "Unsealed"
  /\ CommitMatches("Unsealed", "SealAccepted", "Sealed", "Seal",
                   FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Sealed"
  /\ sealedBytes' = bytes
  /\ UNCHANGED <<bytes, recordCount, handleCount, pins, pendingRefs,
                 aliases, parentPending, destroyed, bridgeFailures,
                 commandAccepted, captureDisposition>>

SealFailure ==
  /\ phase = "Unsealed"
  /\ bridgeFailures < MaxOperations
  /\ CommitMatches("Unsealed", "SealFailed", "Unsealed", "Retry",
                   TRUE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Unsealed"
  /\ bridgeFailures' = bridgeFailures + 1
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 commandAccepted, captureDisposition>>

BridgeFailure ==
  /\ phase = "Sealed"
  /\ bridgeFailures < MaxOperations
  /\ CommitMatches("Sealed", "BridgeFailed", "Sealed", "Retry",
                   TRUE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Sealed"
  /\ bridgeFailures' = bridgeFailures + 1
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 commandAccepted, captureDisposition>>

BridgeSuccess ==
  /\ phase = "Sealed"
  /\ CommitMatches("Sealed", "BridgeAccepted", "Accepted", "AcceptCommand",
                   FALSE, TRUE, FALSE, FALSE, FALSE)
  /\ phase' = "Accepted"
  /\ commandAccepted' = TRUE
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures, captureDisposition>>

CaptureMaterialized ==
  /\ phase = "Accepted"
  /\ CommitMatches("Accepted", "CaptureMaterialized", "CaptureSettled",
                   "CaptureCommit", FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "CaptureSettled"
  /\ captureDisposition' = "Materialized"
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures, commandAccepted>>

CaptureRejected ==
  /\ phase = "Accepted"
  /\ CommitMatches("Accepted", "CaptureRejected", "CaptureSettled",
                   "CaptureReject", FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "CaptureSettled"
  /\ captureDisposition' = "Rejected"
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures, commandAccepted>>

CaptureSkipped ==
  /\ phase = "Accepted"
  /\ CommitMatches("Accepted", "CaptureSkipped", "CaptureSettled", "NoOp",
                   FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "CaptureSettled"
  /\ captureDisposition' = "Skipped"
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures, commandAccepted>>

BeginDrain ==
  /\ phase = "CaptureSettled"
  /\ CommitMatches("CaptureSettled", "DrainPending", "Draining", "BeginDrain",
                   FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Draining"
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures, commandAccepted, captureDisposition>>

DrainAlias ==
  /\ phase = "Draining"
  /\ aliases # {}
  /\ CommitMatches("Draining", "DrainAlias", "Draining", "DestroyAlias",
                   FALSE, FALSE, TRUE, FALSE, FALSE)
  /\ LET alias == CHOOSE a \in aliases : TRUE IN
       /\ aliases' = aliases \ {alias}
       /\ destroyed' = Append(destroyed, alias)
  /\ UNCHANGED <<phase, bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, parentPending, bridgeFailures, commandAccepted,
                 captureDisposition>>

DrainParent ==
  /\ phase = "Draining"
  /\ (aliases = {} \/ Mutation = "ParentBeforeAlias")
  /\ parentPending
  /\ CommitMatches("Draining", "DrainParent", "Drained", "DestroyParent",
                   FALSE, FALSE, TRUE, FALSE, FALSE)
  /\ phase' = "Drained"
  /\ parentPending' = FALSE
  /\ pendingRefs' = {}
  /\ destroyed' = Append(destroyed, "parent")
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 aliases, bridgeFailures, commandAccepted, captureDisposition>>

DrainComplete ==
  /\ phase = "Draining"
  /\ aliases = {}
  /\ ~parentPending
  /\ CommitMatches("Draining", "DrainComplete", "Drained", "FinishDrain",
                   FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Drained"
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures, commandAccepted, captureDisposition>>

BuilderReset ==
  /\ ((phase = "Drained" /\
       CommitMatches("Drained", "BuilderReset", "Reset", "ResetBuilder",
                     FALSE, FALSE, FALSE, TRUE, FALSE))
      \/ (phase = "Draining" /\ Mutation = "EarlyReset"))
  /\ phase' = "Reset"
  /\ bytes' = {}
  /\ recordCount' = 0
  /\ handleCount' = 0
  /\ UNCHANGED <<sealedBytes, pins, pendingRefs, aliases, parentPending,
                 destroyed, bridgeFailures, commandAccepted,
                 captureDisposition>>

WarmEpochAdvance ==
  /\ phase = "Reset"
  /\ CommitMatches("Reset", "WarmEpochAdvance", "WarmAdvanced",
                   "AdvanceWarmEpoch", FALSE, FALSE, FALSE, FALSE, TRUE)
  /\ phase' = "WarmAdvanced"
  /\ pins' = {}
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pendingRefs,
                 aliases, parentPending, destroyed, bridgeFailures,
                 commandAccepted, captureDisposition>>

Complete ==
  /\ phase = "WarmAdvanced"
  /\ CommitMatches("WarmAdvanced", "DrainComplete", "Unsealed", "NoOp",
                   FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = IF Mutation = "StuckSuccess" THEN "WarmAdvanced" ELSE "Unsealed"
  /\ sealedBytes' = {}
  /\ bytes' = ByteTokens
  /\ recordCount' = Cardinality(HandleTokens)
  /\ handleCount' = Cardinality(HandleTokens)
  /\ pins' = PinTokens
  /\ pendingRefs' = PendingTokens
  /\ aliases' = AliasTokens
  /\ parentPending' = TRUE
  /\ destroyed' = <<>>
  /\ bridgeFailures' = 0
  /\ commandAccepted' = IF Mutation = "StuckSuccess"
                            THEN commandAccepted ELSE FALSE
  /\ captureDisposition' = "Pending"

Discard ==
  /\ phase # "Discarded"
  /\ ~(Mutation = "StuckSuccess" /\ phase = "WarmAdvanced")
  /\ (CommitMatches(phase, "ExplicitDiscard", "Discarded", "DiscardAll",
                    FALSE, FALSE, FALSE, TRUE, FALSE)
      \/ CommitMatches(phase, "DeviceReset", "Discarded", "DiscardAll",
                       FALSE, FALSE, FALSE, TRUE, FALSE))
  /\ phase' = "Discarded"
  /\ bytes' = {}
  /\ sealedBytes' = {}
  /\ recordCount' = 0
  /\ handleCount' = 0
  /\ pins' = {}
  /\ pendingRefs' = {}
  /\ aliases' = {}
  /\ parentPending' = FALSE
  /\ captureDisposition' = "None"
  /\ commandAccepted' = FALSE
  /\ UNCHANGED <<destroyed, bridgeFailures>>

Next ==
  SealSuccess \/ SealFailure \/ BridgeFailure \/ BridgeSuccess \/
  CaptureMaterialized \/ CaptureRejected \/ CaptureSkipped \/ BeginDrain \/
  DrainAlias \/ DrainParent \/ DrainComplete \/ BuilderReset \/
  WarmEpochAdvance \/ Complete \/ Discard

TypeOK ==
  /\ phase \in Phases
  /\ bytes \subseteq ByteTokens
  /\ sealedBytes \subseteq ByteTokens
  /\ recordCount \in Nat
  /\ handleCount \in Nat
  /\ pins \subseteq PinTokens
  /\ pendingRefs \subseteq PendingTokens
  /\ aliases \subseteq AliasTokens
  /\ parentPending \in BOOLEAN
  /\ bridgeFailures \in Nat
  /\ commandAccepted \in BOOLEAN
  /\ captureDisposition \in CaptureDispositions

RetryProjectionStable == phase = "Sealed" /\ bridgeFailures > 0 =>
  /\ bytes = sealedBytes
  /\ recordCount = Cardinality(HandleTokens)
  /\ handleCount = Cardinality(HandleTokens)
  /\ pins = PinTokens
  /\ pendingRefs = PendingTokens

NoEarlyDrainReset == Cardinality(pendingRefs) > 0 =>
  phase \notin {"Reset", "WarmAdvanced", "Discarded"}

AcceptedCommandStable == commandAccepted =>
  phase \notin {"Unsealed", "Sealed"}

CaptureDispositionSettled ==
  (phase = "Accepted" => captureDisposition = "Pending")
  /\ (phase = "CaptureSettled" =>
       captureDisposition \in {"Materialized", "Rejected", "Skipped"})

ExactlyOnceParent ==
  Cardinality({i \in 1..Len(destroyed) : destroyed[i] = "parent"}) <= 1

AliasBeforeParent ==
  (\E i \in 1..Len(destroyed) : destroyed[i] = "parent") => aliases = {}

DiscardDrains == phase = "Discarded" =>
  /\ bytes = {}
  /\ pins = {}
  /\ pendingRefs = {}
  /\ captureDisposition = "None"

ReusableWarmCompletion ==
  []((phase = "WarmAdvanced") ~>
      ((phase = "Unsealed" /\ ~commandAccepted) \/ phase = "Discarded"))

AcceptedEventuallySettles ==
  [](commandAccepted ~> (phase = "Unsealed" /\ ~commandAccepted) \/
                         phase = "Discarded")

Spec == Init /\ [][Next]_vars /\
        WF_vars(SealSuccess) /\ WF_vars(SealFailure) /\
        WF_vars(BridgeSuccess) /\ WF_vars(BridgeFailure) /\
        WF_vars(CaptureMaterialized) /\ WF_vars(CaptureRejected) /\
        WF_vars(CaptureSkipped) /\ WF_vars(BeginDrain) /\
        WF_vars(DrainAlias) /\ WF_vars(DrainParent) /\
        WF_vars(DrainComplete) /\ WF_vars(BuilderReset) /\
        WF_vars(WarmEpochAdvance) /\ WF_vars(Complete) /\ WF_vars(Discard)

Progress == [](phase = "Draining" ~> (phase = "Drained" \/ phase = "Discarded"))

THEOREM Spec => []TypeOK /\ []RetryProjectionStable /\ []NoEarlyDrainReset /\
                 []AcceptedCommandStable /\ []CaptureDispositionSettled /\
                 []ExactlyOnceParent /\ []AliasBeforeParent /\ []DiscardDrains

====
