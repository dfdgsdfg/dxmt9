---- MODULE PeRecorderCommit ----
(***************************************************************************
 * Bounded commit settlement model.  The phase/event vocabulary is the TLA+
 * twin of settleRecorderCommit in d3d9_pe_commit_transition.hpp.
 * ByteTokens, handle tokens, pins, and pending references stand for the
 * builder/capture obligations that must survive bridge retry and must drain
 * only after an accepted command.  MaxOperations bounds all seal/bridge
 * failure retries; every successful transition advances the finite phase or
 * drains one finite token, so the resulting operation graph is finite.
 ***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS ByteTokens, HandleTokens, PinTokens, PendingTokens, AliasTokens,
          Mutation, MaxOperations

ASSUME ByteTokens # {} /\ HandleTokens # {} /\ PinTokens # {} /\
       PendingTokens # {} /\ AliasTokens # {}
ASSUME Mutation \in {"Guarded", "ParentBeforeAlias", "EarlyReset"}
ASSUME MaxOperations \in Nat \ {0}

Phases == {"Unsealed", "Sealed", "Accepted", "CaptureSettled",
           "Draining", "Drained", "Reset", "WarmAdvanced", "Discarded"}

VARIABLES phase, bytes, sealedBytes, recordCount, handleCount, pins,
          pendingRefs, aliases, parentPending, destroyed, bridgeFailures,
          commandAccepted

vars == <<phase, bytes, sealedBytes, recordCount, handleCount, pins,
          pendingRefs, aliases, parentPending, destroyed, bridgeFailures,
          commandAccepted>>

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

SealSuccess ==
  /\ phase = "Unsealed"
  /\ phase' = "Sealed"
  /\ sealedBytes' = bytes
  /\ UNCHANGED <<bytes, recordCount, handleCount, pins, pendingRefs,
                 aliases, parentPending, destroyed, bridgeFailures,
                 commandAccepted>>

SealFailure ==
  /\ phase = "Unsealed"
  /\ bridgeFailures < MaxOperations
  /\ phase' = "Unsealed"
  /\ bridgeFailures' = bridgeFailures + 1
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 commandAccepted>>

BridgeFailure ==
  /\ phase = "Sealed"
  /\ bridgeFailures < MaxOperations
  /\ phase' = "Sealed"
  /\ bridgeFailures' = bridgeFailures + 1
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 commandAccepted>>

BridgeSuccess ==
  /\ phase = "Sealed"
  /\ phase' = "Accepted"
  /\ commandAccepted' = TRUE
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures>>

CaptureMaterialized ==
  /\ phase = "Accepted"
  /\ phase' = "CaptureSettled"
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures, commandAccepted>>

CaptureRejected == CaptureMaterialized

BeginDrain ==
  /\ phase = "CaptureSettled"
  /\ phase' = "Draining"
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures, commandAccepted>>

DrainAlias ==
  /\ phase = "Draining"
  /\ aliases # {}
  /\ LET alias == CHOOSE a \in aliases : TRUE IN
       /\ aliases' = aliases \ {alias}
       /\ destroyed' = Append(destroyed, alias)
  /\ UNCHANGED <<phase, bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, parentPending, bridgeFailures,
                 commandAccepted>>

DrainParent ==
  /\ phase = "Draining"
  /\ (aliases = {} \/ Mutation = "ParentBeforeAlias")
  /\ parentPending
  /\ phase' = "Drained"
  /\ parentPending' = FALSE
  /\ pendingRefs' = {}
  /\ destroyed' = Append(destroyed, "parent")
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 aliases, bridgeFailures, commandAccepted>>

DrainComplete ==
  /\ phase = "Draining"
  /\ aliases = {}
  /\ ~parentPending
  /\ phase' = "Drained"
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pins,
                 pendingRefs, aliases, parentPending, destroyed,
                 bridgeFailures, commandAccepted>>

BuilderReset ==
  /\ (phase = "Drained" \/ (phase = "Draining" /\ Mutation = "EarlyReset"))
  /\ phase' = "Reset"
  /\ bytes' = {}
  /\ recordCount' = 0
  /\ handleCount' = 0
  /\ UNCHANGED <<sealedBytes, pins, pendingRefs, aliases, parentPending,
                 destroyed, bridgeFailures, commandAccepted>>

WarmEpochAdvance ==
  /\ phase = "Reset"
  /\ phase' = "WarmAdvanced"
  /\ pins' = {}
  /\ UNCHANGED <<bytes, sealedBytes, recordCount, handleCount, pendingRefs,
                 aliases, parentPending, destroyed, bridgeFailures,
                 commandAccepted>>

Complete ==
  /\ phase = "WarmAdvanced"
  /\ ~commandAccepted
  /\ phase' = "Unsealed"
  /\ sealedBytes' = {}
  /\ UNCHANGED <<bytes, recordCount, handleCount, pins, pendingRefs,
                 aliases, parentPending, destroyed, bridgeFailures,
                 commandAccepted>>

Discard ==
  /\ phase # "Discarded"
  /\ phase' = "Discarded"
  /\ bytes' = {}
  /\ sealedBytes' = {}
  /\ recordCount' = 0
  /\ handleCount' = 0
  /\ pins' = {}
  /\ pendingRefs' = {}
  /\ aliases' = {}
  /\ parentPending' = FALSE
  /\ UNCHANGED <<destroyed, bridgeFailures, commandAccepted>>

Next ==
  LET transition == SealSuccess \/ SealFailure \/ BridgeFailure \/
          BridgeSuccess \/ CaptureMaterialized \/ CaptureRejected \/
          BeginDrain \/ DrainAlias \/ DrainParent \/ DrainComplete \/
          BuilderReset \/ WarmEpochAdvance \/ Complete \/ Discard
  IN transition

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
ExactlyOnceParent ==
    Cardinality({i \in 1..Len(destroyed) : destroyed[i] = "parent"}) <= 1
AliasBeforeParent ==
    (\E i \in 1..Len(destroyed) : destroyed[i] = "parent") => aliases = {}
DiscardDrains == phase = "Discarded" =>
    /\ bytes = {}
    /\ pins = {}
    /\ pendingRefs = {}

Spec == Init /\ [][Next]_vars /\
        WF_vars(DrainAlias) /\
        WF_vars(DrainParent) /\
        WF_vars(DrainComplete)

Progress == [] (phase = "Draining" ~> (phase = "Drained" \/ phase = "Discarded"))

THEOREM Spec => []RetryProjectionStable /\ []NoEarlyDrainReset /\
                 []AcceptedCommandStable /\ []ExactlyOnceParent /\
                 []AliasBeforeParent /\ []DiscardDrains

====
