---- MODULE PeRecorderSettlement ----
(***************************************************************************
 * Composition of a capacity-pre append with settlement of the already-full
 * builder: seal -> bridge -> all capture dispositions -> ordered ref drain ->
 * reset -> warm advance -> emitter -> capacity-post settlement. Generated
 * tables bind each production decision; the proposed append remains
 * unattempted until Emit and accepted across every CapacityPost outcome.
 ****************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences, TLC,
        PeRecorderSettlementTable, PeRecorderCommitTable

CONSTANT Mutation
ASSUME Mutation \in {"Guarded", "BridgeRetry", "CapacityPreConsume",
                     "CaptureRetract", "EarlyReset"}

PriorToken == [category |-> "Transform", key |-> 2, value |-> 1, ordinal |-> 1]
Token == [category |-> "RenderState", key |-> 7, value |-> 3, ordinal |-> 2]
AliasTokens == {"alias0"}
Phases == {"CapacityPre", "Sealed", "BridgeAccepted", "Captured",
           "Draining", "Drained", "Reset", "Warm", "Emit", "CapacityPost",
           "Done", "PostFailed", "Poisoned", "Recovered"}

VARIABLES phase, pending, durable, builderOrdinal, recordCount, handleCount,
          payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
          captureDisposition, pendingRefs, aliases, parentPending, destroyed,
          effectUnknownSeen, unattemptedConsumed, warmAdvanced

vars == <<phase, pending, durable, builderOrdinal, recordCount, handleCount,
          payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
          captureDisposition, pendingRefs, aliases, parentPending, destroyed,
          effectUnknownSeen, unattemptedConsumed, warmAdvanced>>

Init ==
  /\ phase = "CapacityPre" /\ pending = {Token} /\ durable = {PriorToken}
  /\ builderOrdinal = PriorToken.ordinal /\ recordCount = 1 /\ handleCount = 1
  /\ payloadBytes = 1 /\ acceptedRecord = TRUE /\ acceptedEver = TRUE
  /\ commandAccepted = FALSE /\ captureDisposition = "None"
  /\ pendingRefs = {PriorToken} /\ aliases = AliasTokens
  /\ parentPending = TRUE /\ destroyed = <<>>
  /\ effectUnknownSeen = FALSE /\ unattemptedConsumed = FALSE
  /\ warmAdvanced = FALSE

CapacityPreFailure ==
  /\ phase = "CapacityPre"
  /\ SettlementMatches("CapacityPre", "FailedPreEffect", "RetryUnattempted",
                       FALSE, TRUE, FALSE, FALSE)
  /\ pending' = IF Mutation = "CapacityPreConsume" THEN {} ELSE pending
  /\ unattemptedConsumed' = (Mutation = "CapacityPreConsume")
  /\ UNCHANGED <<phase, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, effectUnknownSeen, warmAdvanced>>

Seal ==
  /\ phase = "CapacityPre"
  /\ CommitMatches("Unsealed", "SealAccepted", "Sealed", "Seal",
                   FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Sealed"
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, effectUnknownSeen, unattemptedConsumed,
                 warmAdvanced>>

BridgePreEffectFailure ==
  /\ phase = "Sealed"
  /\ SettlementMatches("Bridge", "FailedPreEffect", "RetryUnattempted",
                       FALSE, TRUE, FALSE, FALSE)
  /\ CommitMatches("Sealed", "BridgePreEffectFailed", "Sealed", "Retry",
                   TRUE, FALSE, FALSE, FALSE, FALSE)
  /\ UNCHANGED vars

BridgeEffectUnknown ==
  /\ phase = "Sealed"
  /\ SettlementMatches("Bridge", "FailedEffectUnknown", "PoisonFailStop",
                       FALSE, FALSE, FALSE, TRUE)
  /\ SettlementMatches("CapacityPre", "FailedEffectUnknown", "PoisonFailStop",
                       FALSE, FALSE, FALSE, TRUE)
  /\ CommitMatches("Sealed", "BridgeEffectUnknown", "Poisoned", "FailStop",
                   FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = IF Mutation = "BridgeRetry" THEN "Sealed" ELSE "Poisoned"
  /\ effectUnknownSeen' = TRUE
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, unattemptedConsumed, warmAdvanced>>

BridgeAccepted ==
  /\ phase = "Sealed"
  /\ SettlementMatches("Bridge", "Succeeded", "Continue",
                       TRUE, FALSE, FALSE, FALSE)
  /\ CommitMatches("Sealed", "BridgeAccepted", "Accepted", "AcceptCommand",
                   FALSE, TRUE, FALSE, FALSE, FALSE)
  /\ phase' = "BridgeAccepted" /\ commandAccepted' = TRUE
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, effectUnknownSeen, unattemptedConsumed,
                 warmAdvanced>>

CaptureMaterialized ==
  /\ phase = "BridgeAccepted"
  /\ CommitMatches("Accepted", "CaptureMaterialized", "CaptureSettled",
                   "CaptureCommit", FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Captured" /\ captureDisposition' = "Materialized"
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 pendingRefs, aliases, parentPending, destroyed,
                 effectUnknownSeen, unattemptedConsumed, warmAdvanced>>

CaptureRejected ==
  /\ phase = "BridgeAccepted"
  /\ CommitMatches("Accepted", "CaptureRejected", "CaptureSettled",
                   "CaptureReject", FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Captured" /\ captureDisposition' = "Rejected"
  /\ commandAccepted' = IF Mutation = "CaptureRetract" THEN FALSE
                         ELSE commandAccepted
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, pendingRefs,
                 aliases, parentPending, destroyed, effectUnknownSeen,
                 unattemptedConsumed, warmAdvanced>>

CaptureSkipped ==
  /\ phase = "BridgeAccepted"
  /\ CommitMatches("Accepted", "CaptureSkipped", "CaptureSettled",
                   "NoOp", FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Captured" /\ captureDisposition' = "Skipped"
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 pendingRefs, aliases, parentPending, destroyed,
                 effectUnknownSeen, unattemptedConsumed, warmAdvanced>>

BeginDrain ==
  /\ phase = "Captured"
  /\ CommitMatches("CaptureSettled", "DrainPending", "Draining",
                   "BeginDrain", FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Draining"
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, effectUnknownSeen, unattemptedConsumed,
                 warmAdvanced>>

DrainAlias ==
  /\ phase = "Draining" /\ aliases # {}
  /\ CommitMatches("Draining", "DrainAlias", "Draining", "DestroyAlias",
                   FALSE, FALSE, TRUE, FALSE, FALSE)
  /\ LET alias == CHOOSE a \in aliases : TRUE IN
       /\ aliases' = aliases \ {alias}
       /\ destroyed' = Append(destroyed, alias)
  /\ UNCHANGED <<phase, pending, durable, builderOrdinal, recordCount,
                 handleCount, payloadBytes, acceptedRecord, acceptedEver,
                 commandAccepted, captureDisposition, pendingRefs,
                 parentPending, effectUnknownSeen, unattemptedConsumed,
                 warmAdvanced>>

DrainParent ==
  /\ phase = "Draining" /\ aliases = {} /\ parentPending
  /\ CommitMatches("Draining", "DrainParent", "Drained", "DestroyParent",
                   FALSE, FALSE, TRUE, FALSE, FALSE)
  /\ phase' = "Drained" /\ parentPending' = FALSE /\ pendingRefs' = {}
  /\ destroyed' = Append(destroyed, "parent")
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, aliases, effectUnknownSeen,
                 unattemptedConsumed, warmAdvanced>>

BuilderReset ==
  /\ (phase = "Drained" \/ (phase = "Captured" /\ Mutation = "EarlyReset"))
  /\ phase' = "Reset" /\ recordCount' = 0 /\ handleCount' = 0
  /\ payloadBytes' = 0 /\ acceptedRecord' = FALSE
  /\ UNCHANGED <<pending, durable, builderOrdinal, acceptedEver,
                 commandAccepted, captureDisposition, pendingRefs,
                 aliases, parentPending, destroyed, effectUnknownSeen,
                 unattemptedConsumed, warmAdvanced>>

WarmAdvance ==
  /\ phase = "Reset"
  /\ CommitMatches("Reset", "WarmEpochAdvance", "WarmAdvanced",
                   "AdvanceWarmEpoch", FALSE, FALSE, FALSE, FALSE, TRUE)
  /\ phase' = "Warm" /\ warmAdvanced' = TRUE
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, effectUnknownSeen, unattemptedConsumed>>

CapacityPreSuccess ==
  /\ phase = "Warm"
  /\ CommitMatches("WarmAdvanced", "DrainComplete", "Unsealed", "NoOp",
                   FALSE, FALSE, FALSE, FALSE, FALSE)
  /\ SettlementMatches("CapacityPre", "Succeeded", "Continue",
                       FALSE, FALSE, FALSE, FALSE)
  /\ phase' = "Emit"
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, effectUnknownSeen, unattemptedConsumed,
                 warmAdvanced>>

EmitterFailure ==
  /\ phase = "Emit"
  /\ SettlementMatches("Emitter", "FailedPreEffect", "RollbackEmitter",
                       FALSE, TRUE, TRUE, FALSE)
  /\ phase' = "Emit"
  /\ UNCHANGED vars

EmitterAccepted ==
  /\ phase = "Emit"
  /\ SettlementMatches("Emitter", "Succeeded", "KeepAccepted",
                       TRUE, FALSE, FALSE, FALSE)
  /\ phase' = "CapacityPost" /\ pending' = {}
  /\ durable' = durable \union {Token}
  /\ builderOrdinal' = Token.ordinal /\ recordCount' = 1
  /\ handleCount' = 1 /\ payloadBytes' = 1 /\ acceptedRecord' = TRUE
  /\ acceptedEver' = TRUE
  /\ UNCHANGED <<commandAccepted, captureDisposition, pendingRefs, aliases,
                 parentPending, destroyed, effectUnknownSeen,
                 unattemptedConsumed, warmAdvanced>>

CapacityPostAccepted ==
  /\ phase = "CapacityPost"
  /\ SettlementMatches("CapacityPost", "Succeeded", "Continue",
                       TRUE, FALSE, FALSE, FALSE)
  /\ phase' = "Done"
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, effectUnknownSeen, unattemptedConsumed,
                 warmAdvanced>>

CapacityPostPreEffectFailure ==
  /\ phase = "CapacityPost"
  /\ SettlementMatches("CapacityPost", "FailedPreEffect", "KeepAccepted",
                       TRUE, TRUE, FALSE, FALSE)
  /\ phase' = "PostFailed"
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, effectUnknownSeen, unattemptedConsumed,
                 warmAdvanced>>

CapacityPostEffectUnknown ==
  /\ phase = "CapacityPost"
  /\ SettlementMatches("CapacityPost", "FailedEffectUnknown", "PoisonFailStop",
                       TRUE, FALSE, FALSE, TRUE)
  /\ phase' = "Poisoned" /\ effectUnknownSeen' = TRUE
  /\ UNCHANGED <<pending, durable, builderOrdinal, recordCount, handleCount,
                 payloadBytes, acceptedRecord, acceptedEver, commandAccepted,
                 captureDisposition, pendingRefs, aliases, parentPending,
                 destroyed, unattemptedConsumed, warmAdvanced>>

DeviceResetRecovery ==
  /\ phase = "Poisoned"
  /\ CommitMatches("Poisoned", "DeviceReset", "Discarded", "DiscardAll",
                   FALSE, FALSE, FALSE, TRUE, FALSE)
  /\ phase' = "Recovered" /\ pending' = {} /\ durable' = {}
  /\ recordCount' = 0 /\ handleCount' = 0 /\ payloadBytes' = 0
  /\ acceptedRecord' = FALSE /\ commandAccepted' = FALSE
  /\ captureDisposition' = "None" /\ pendingRefs' = {} /\ aliases' = {}
  /\ parentPending' = FALSE /\ warmAdvanced' = FALSE
  /\ UNCHANGED <<builderOrdinal, acceptedEver, destroyed,
                 effectUnknownSeen, unattemptedConsumed>>

Next == CapacityPreFailure \/ Seal \/ BridgePreEffectFailure \/
        BridgeEffectUnknown \/ BridgeAccepted \/ CaptureMaterialized \/
        CaptureRejected \/ CaptureSkipped \/ BeginDrain \/ DrainAlias \/
        DrainParent \/ BuilderReset \/ WarmAdvance \/ CapacityPreSuccess \/
        EmitterFailure \/ EmitterAccepted \/ CapacityPostAccepted \/
        CapacityPostPreEffectFailure \/ CapacityPostEffectUnknown \/
        DeviceResetRecovery

TypeOK == phase \in Phases /\ pending \subseteq {Token} /\
          durable \subseteq {PriorToken, Token} /\
          captureDisposition \in {"None", "Materialized", "Rejected", "Skipped"}
CapacityPreDoesNotConsume == ~unattemptedConsumed
ExactPendingConservation ==
  phase \in {"CapacityPre", "Sealed", "BridgeAccepted", "Captured",
             "Draining", "Drained", "Reset", "Warm", "Emit"} =>
    pending = {Token} /\ durable = {PriorToken}
CapacityPostSettlementPreservesAccepted ==
  phase \in {"CapacityPost", "Done", "PostFailed"} =>
    pending = {} /\ Token \in durable /\ acceptedRecord
BridgeEffectUnknownFailStop ==
  effectUnknownSeen => phase \in {"Poisoned", "Recovered"}
CaptureAfterAccept ==
  acceptedEver /\ captureDisposition # "None" => commandAccepted
AliasBeforeParent ==
  Len(destroyed) > 0 /\ destroyed[Len(destroyed)] = "parent" =>
    Len(destroyed) = 2 /\ destroyed[1] = "alias0"
NoEarlyDrainReset ==
  phase \in {"Reset", "Warm", "Emit", "CapacityPost", "Done", "PostFailed"} =>
    pendingRefs = {} /\ aliases = {} /\ ~parentPending

FairSettlement == WF_vars(BeginDrain) /\ WF_vars(DrainAlias) /\
                  WF_vars(DrainParent) /\ WF_vars(BuilderReset) /\
                  WF_vars(WarmAdvance) /\ WF_vars(CapacityPreSuccess)
SuccessfulCapacityFlushProgress == phase = "Captured" ~> phase = "Emit"

Spec == Init /\ [][Next]_vars /\ FairSettlement
====
