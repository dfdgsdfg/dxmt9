---- MODULE PostEncodePayloadRetirement ----
(*
 * R-BACK-2.44/2.45/2.49/2.59/2.65 post-encode retirement refinement.
 *
 * The ownership-critical retirement sequence is explicit: Publish, Encode,
 * detach and activate a queue receipt while locked, destroy re-entrant owners
 * outside the lock, then finish page/generation release while locked. Receipt,
 * retained-resource, GPU-work, ordinary completion, device-loss settlement,
 * and Present completion are separate bounded state. A two-command current
 * source may retain a final borrow after its prefix while an exact successor
 * is encoded; neither source can retire out of FIFO order or more than once.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS MaxSeqId, MaxReceiptSlots

SeqIds == 1 .. MaxSeqId
SeqId0 == 0 .. MaxSeqId
ReceiptSlots == 1 .. MaxReceiptSlots
PayloadStates ==
  {"Absent", "Published", "PrefixEncoded", "Encoded", "Detached",
   "OwnerDestroyed", "Retired"}
ReceiptStates == {"None", "Active", "Submitted", "Completed", "Released"}
SettlementStates == {"None", "Gpu", "DeviceLoss"}

SlotOf(s) == ((s - 1) % MaxReceiptSlots) + 1
QueueSet(seq) == {seq[i] : i \in DOMAIN seq}
ExpectedCommands(s) == IF s = 1 THEN 2 ELSE 1

DeferredPaths == {"None", "Join", "StaleFailOpen", "Drain"}
DeferredAccountingState ==
  [remainingCommands : [SeqIds -> 0 .. 2],
   finalBorrow : [SeqIds -> BOOLEAN],
   commandEffects : [SeqIds -> 0 .. 2],
   receiptActivations : [SeqIds -> 0 .. 1],
   completionFinishes : [SeqIds -> 0 .. 1],
   reclaims : [SeqIds -> 0 .. 1],
   successorEffectful : BOOLEAN,
   rollbackAfterEffect : BOOLEAN,
   lastPath : DeferredPaths]

EmptyDeferredAccounting ==
  [remainingCommands |-> [s \in SeqIds |-> 0],
   finalBorrow |-> [s \in SeqIds |-> FALSE],
   commandEffects |-> [s \in SeqIds |-> 0],
   receiptActivations |-> [s \in SeqIds |-> 0],
   completionFinishes |-> [s \in SeqIds |-> 0],
   reclaims |-> [s \in SeqIds |-> 0],
   successorEffectful |-> FALSE,
   rollbackAfterEffect |-> FALSE,
   lastPath |-> "None"]

VARIABLES
  nextSeq,
  payloadState,
  payloadGeneration,
  hasPresent,
  receiptState,
  receiptSlot,
  receiptGeneration,
  slotOwner,
  slotGeneration,
  gpuOutstanding,
  completionQueue,
  completedSeqId,
  completedPresentSeqId,
  callbackCount,
  settlement,
  resourceRetained,
  deviceLost,
  deferredAccounting

vars ==
  << nextSeq,
     payloadState,
     payloadGeneration,
     hasPresent,
     receiptState,
     receiptSlot,
     receiptGeneration,
     slotOwner,
     slotGeneration,
     gpuOutstanding,
     completionQueue,
     completedSeqId,
     completedPresentSeqId,
     callbackCount,
     settlement,
     resourceRetained,
     deviceLost,
     deferredAccounting >>

PayloadResident(s) ==
  payloadState[s] \in
    {"Published", "PrefixEncoded", "Encoded", "Detached", "OwnerDestroyed"}
CompletionRegistered(s) ==
  s \in gpuOutstanding \/ settlement[s] # "None"

Init ==
  /\ nextSeq = 1
  /\ payloadState = [s \in SeqIds |-> "Absent"]
  /\ payloadGeneration = [s \in SeqIds |-> 1]
  /\ hasPresent = [s \in SeqIds |-> FALSE]
  /\ receiptState = [s \in SeqIds |-> "None"]
  /\ receiptSlot = [s \in SeqIds |-> 0]
  /\ receiptGeneration = [s \in SeqIds |-> 0]
  /\ slotOwner = [slot \in ReceiptSlots |-> 0]
  /\ slotGeneration = [slot \in ReceiptSlots |-> 1]
  /\ gpuOutstanding = {}
  /\ completionQueue = <<>>
  /\ completedSeqId = 0
  /\ completedPresentSeqId = 0
  /\ callbackCount = [s \in SeqIds |-> 0]
  /\ settlement = [s \in SeqIds |-> "None"]
  /\ resourceRetained = [s \in SeqIds |-> FALSE]
  /\ deviceLost = FALSE
  /\ deferredAccounting = EmptyDeferredAccounting

Publish ==
  /\ ~deviceLost
  /\ nextSeq <= MaxSeqId
  /\ payloadState' = [payloadState EXCEPT ![nextSeq] = "Published"]
  /\ hasPresent' =
       [hasPresent EXCEPT ![nextSeq] = (nextSeq = MaxSeqId)]
  /\ resourceRetained' =
       [resourceRetained EXCEPT ![nextSeq] = TRUE]
  /\ deferredAccounting' =
       [deferredAccounting EXCEPT
          !.remainingCommands[nextSeq] = ExpectedCommands(nextSeq)]
  /\ nextSeq' = nextSeq + 1
  /\ UNCHANGED << payloadGeneration,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  gpuOutstanding,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  deviceLost >>

EncodeWhole(s) ==
  /\ ~deviceLost
  /\ s # 1
  /\ (s # 2 \/ payloadState[1] # "PrefixEncoded")
  /\ payloadState[s] = "Published"
  /\ \A older \in SeqIds : older < s => payloadState[older] # "Published"
  /\ payloadState' = [payloadState EXCEPT ![s] = "Encoded"]
  /\ deferredAccounting' =
       [deferredAccounting EXCEPT
          !.remainingCommands[s] = 0,
          !.commandEffects[s] = 1]
  /\ UNCHANGED << nextSeq,
                  payloadGeneration,
                  hasPresent,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  gpuOutstanding,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deviceLost >>

EncodeDeferredPrefix ==
  /\ ~deviceLost
  /\ payloadState[1] = "Published"
  /\ deferredAccounting.remainingCommands[1] = 2
  /\ payloadState' = [payloadState EXCEPT ![1] = "PrefixEncoded"]
  /\ deferredAccounting' =
       [deferredAccounting EXCEPT
          !.remainingCommands[1] = 1,
          !.finalBorrow[1] = TRUE,
          !.commandEffects[1] = 1]
  /\ UNCHANGED << nextSeq,
                  payloadGeneration,
                  hasPresent,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  gpuOutstanding,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deviceLost >>

EncodeDeferredSuccessor ==
  /\ ~deviceLost
  /\ payloadState[1] = "PrefixEncoded"
  /\ deferredAccounting.finalBorrow[1]
  /\ ~deferredAccounting.successorEffectful
  /\ payloadState[2] = "Published"
  /\ deferredAccounting.remainingCommands[2] = 1
  /\ payloadState' = [payloadState EXCEPT ![2] = "Encoded"]
  /\ deferredAccounting' =
       [deferredAccounting EXCEPT
          !.remainingCommands[2] = 0,
          !.commandEffects[2] = 1,
          !.successorEffectful = TRUE,
          !.lastPath = "Join"]
  /\ UNCHANGED << nextSeq,
                  payloadGeneration,
                  hasPresent,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  gpuOutstanding,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deviceLost >>

FinishDeferredSuffix ==
  /\ payloadState[1] = "PrefixEncoded"
  /\ deferredAccounting.successorEffectful
  /\ deferredAccounting.lastPath = "Join"
  /\ payloadState' = [payloadState EXCEPT ![1] = "Encoded"]
  /\ deferredAccounting' =
       [deferredAccounting EXCEPT
          !.remainingCommands[1] = 0,
          !.finalBorrow[1] = FALSE,
          !.commandEffects[1] = @ + 1]
  /\ UNCHANGED << nextSeq,
                  payloadGeneration,
                  hasPresent,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  gpuOutstanding,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deviceLost >>

NaturalDrainDeferredSuffix(path) ==
  /\ path \in {"StaleFailOpen", "Drain"}
  /\ payloadState[1] = "PrefixEncoded"
  /\ ~deferredAccounting.successorEffectful
  /\ deferredAccounting.finalBorrow[1]
  /\ payloadState' = [payloadState EXCEPT ![1] = "Encoded"]
  /\ deferredAccounting' =
       [deferredAccounting EXCEPT
          !.remainingCommands[1] = 0,
          !.finalBorrow[1] = FALSE,
          !.commandEffects[1] = @ + 1,
          !.lastPath = path]
  /\ UNCHANGED << nextSeq,
                  payloadGeneration,
                  hasPresent,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  gpuOutstanding,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deviceLost >>

StaleFailOpen == NaturalDrainDeferredSuffix("StaleFailOpen")
DrainDeferredSuffix == NaturalDrainDeferredSuffix("Drain")

DetachRetiredPayload(s) ==
  LET slot == SlotOf(s) IN
    /\ payloadState[s] = "Encoded"
    /\ deferredAccounting.remainingCommands[s] = 0
    /\ ~deferredAccounting.finalBorrow[s]
    /\ s \notin gpuOutstanding
    /\ settlement[s] = "None"
    /\ ~hasPresent[s]
    /\ receiptState[s] = "None"
    /\ slotOwner[slot] = 0
    /\ \A older \in SeqIds : older < s => ~PayloadResident(older)
    /\ payloadState' = [payloadState EXCEPT ![s] = "Detached"]
    /\ receiptState' = [receiptState EXCEPT ![s] = "Active"]
    /\ receiptSlot' = [receiptSlot EXCEPT ![s] = slot]
    /\ receiptGeneration' =
         [receiptGeneration EXCEPT ![s] = slotGeneration[slot]]
    /\ slotOwner' = [slotOwner EXCEPT ![slot] = s]
    /\ deferredAccounting' =
         [deferredAccounting EXCEPT
            !.receiptActivations[s] = @ + 1]
    /\ UNCHANGED << nextSeq,
                    payloadGeneration,
                    hasPresent,
                    slotGeneration,
                    gpuOutstanding,
                    completionQueue,
                    completedSeqId,
                    completedPresentSeqId,
                    callbackCount,
                    settlement,
                    resourceRetained,
                    deviceLost >>

DestroyOwner(s) ==
  /\ payloadState[s] = "Detached"
  /\ receiptState[s] = "Active"
  /\ payloadState' = [payloadState EXCEPT ![s] = "OwnerDestroyed"]
  /\ UNCHANGED << nextSeq,
                  payloadGeneration,
                  hasPresent,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  gpuOutstanding,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deviceLost,
                  deferredAccounting >>

FinishPayloadRetirement(s) ==
  /\ payloadState[s] = "OwnerDestroyed"
  /\ receiptState[s] = "Active"
  /\ payloadState' = [payloadState EXCEPT ![s] = "Retired"]
  /\ payloadGeneration' = [payloadGeneration EXCEPT ![s] = @ + 1]
  /\ UNCHANGED << nextSeq,
                  hasPresent,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  gpuOutstanding,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deviceLost,
                  deferredAccounting >>

SubmitRetired(s) ==
  /\ ~deviceLost
  /\ payloadState[s] = "Retired"
  /\ receiptState[s] = "Active"
  /\ settlement[s] = "None"
  /\ s \notin gpuOutstanding
  /\ \A older \in SeqIds :
       older < s => CompletionRegistered(older)
  /\ receiptState' = [receiptState EXCEPT ![s] = "Submitted"]
  /\ gpuOutstanding' = gpuOutstanding \cup {s}
  /\ UNCHANGED << nextSeq,
                  payloadState,
                  payloadGeneration,
                  hasPresent,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deviceLost,
                  deferredAccounting >>

SubmitLegacy(s) ==
  /\ ~deviceLost
  /\ payloadState[s] = "Encoded"
  /\ receiptState[s] = "None"
  /\ (s > 2 \/ deferredAccounting.lastPath = "None")
  /\ settlement[s] = "None"
  /\ s \notin gpuOutstanding
  /\ \A older \in SeqIds :
       older < s => CompletionRegistered(older)
  /\ gpuOutstanding' = gpuOutstanding \cup {s}
  /\ UNCHANGED << nextSeq,
                  payloadState,
                  payloadGeneration,
                  hasPresent,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deviceLost,
                  deferredAccounting >>

GpuComplete(s) ==
  /\ ~deviceLost
  /\ s \in gpuOutstanding
  /\ \A older \in gpuOutstanding : s <= older
  /\ receiptState[s] \in {"None", "Submitted"}
  /\ gpuOutstanding' = gpuOutstanding \ {s}
  /\ completionQueue' = Append(completionQueue, s)
  /\ callbackCount' = [callbackCount EXCEPT ![s] = @ + 1]
  /\ settlement' = [settlement EXCEPT ![s] = "Gpu"]
  /\ receiptState' =
       IF receiptState[s] = "Submitted"
       THEN [receiptState EXCEPT ![s] = "Completed"]
       ELSE receiptState
  /\ UNCHANGED << nextSeq,
                  payloadState,
                  payloadGeneration,
                  hasPresent,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  completedSeqId,
                  completedPresentSeqId,
                  resourceRetained,
                  deviceLost,
                  deferredAccounting >>

BeginDeviceLoss ==
  /\ ~deviceLost
  /\ deviceLost' = TRUE
  /\ UNCHANGED << nextSeq,
                  payloadState,
                  payloadGeneration,
                  hasPresent,
                  receiptState,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  gpuOutstanding,
                  completionQueue,
                  completedSeqId,
                  completedPresentSeqId,
                  callbackCount,
                  settlement,
                  resourceRetained,
                  deferredAccounting >>

SettleDeviceLoss(s) ==
  /\ deviceLost
  /\ s \in gpuOutstanding
  /\ \A older \in gpuOutstanding : s <= older
  /\ receiptState[s] \in {"None", "Submitted"}
  /\ settlement[s] = "None"
  /\ gpuOutstanding' = gpuOutstanding \ {s}
  /\ completionQueue' = Append(completionQueue, s)
  /\ callbackCount' = [callbackCount EXCEPT ![s] = @ + 1]
  /\ settlement' = [settlement EXCEPT ![s] = "DeviceLoss"]
  /\ receiptState' =
       IF receiptState[s] = "Submitted"
       THEN [receiptState EXCEPT ![s] = "Completed"]
       ELSE receiptState
  /\ UNCHANGED << nextSeq,
                  payloadState,
                  payloadGeneration,
                  hasPresent,
                  receiptSlot,
                  receiptGeneration,
                  slotOwner,
                  slotGeneration,
                  completedSeqId,
                  completedPresentSeqId,
                  resourceRetained,
                  deviceLost,
                  deferredAccounting >>

FinishCompletion(s) ==
  /\ Len(completionQueue) > 0
  /\ Head(completionQueue) = s
  /\ s = completedSeqId + 1
  /\ callbackCount[s] = 1
  /\ settlement[s] \in {"Gpu", "DeviceLoss"}
  /\ receiptState[s] \in {"None", "Completed"}
  /\ LET retired == receiptState[s] = "Completed"
         slot == receiptSlot[s]
     IN
       /\ completedSeqId' = s
       /\ completedPresentSeqId' =
            IF hasPresent[s] THEN s ELSE completedPresentSeqId
       /\ completionQueue' = Tail(completionQueue)
       /\ resourceRetained' =
            [resourceRetained EXCEPT ![s] = FALSE]
       /\ payloadState' =
            IF retired
            THEN payloadState
            ELSE [payloadState EXCEPT ![s] = "Absent"]
       /\ payloadGeneration' =
            IF retired
            THEN payloadGeneration
            ELSE [payloadGeneration EXCEPT ![s] = @ + 1]
       /\ receiptState' =
            IF retired
            THEN [receiptState EXCEPT ![s] = "Released"]
            ELSE receiptState
       /\ slotOwner' =
            IF retired
            THEN [slotOwner EXCEPT ![slot] = 0]
            ELSE slotOwner
       /\ slotGeneration' =
            IF retired
            THEN [slotGeneration EXCEPT ![slot] = @ + 1]
            ELSE slotGeneration
       /\ deferredAccounting' =
            [deferredAccounting EXCEPT
               !.completionFinishes[s] = @ + 1,
               !.reclaims[s] = @ + 1]
  /\ UNCHANGED << nextSeq,
                  hasPresent,
                  receiptSlot,
                  receiptGeneration,
                  gpuOutstanding,
                  callbackCount,
                  settlement,
                  deviceLost >>

Next ==
  \/ Publish
  \/ \E s \in SeqIds : EncodeWhole(s)
  \/ EncodeDeferredPrefix
  \/ EncodeDeferredSuccessor
  \/ FinishDeferredSuffix
  \/ StaleFailOpen
  \/ DrainDeferredSuffix
  \/ \E s \in SeqIds : DetachRetiredPayload(s)
  \/ \E s \in SeqIds : DestroyOwner(s)
  \/ \E s \in SeqIds : FinishPayloadRetirement(s)
  \/ \E s \in SeqIds : SubmitRetired(s)
  \/ \E s \in SeqIds : SubmitLegacy(s)
  \/ \E s \in SeqIds : GpuComplete(s)
  \/ BeginDeviceLoss
  \/ \E s \in SeqIds : SettleDeviceLoss(s)
  \/ \E s \in SeqIds : FinishCompletion(s)

RetirementFairness ==
  /\ WF_vars(Publish)
  /\ WF_vars(EncodeDeferredPrefix)
  /\ WF_vars(EncodeDeferredSuccessor)
  /\ WF_vars(FinishDeferredSuffix)
  /\ WF_vars(StaleFailOpen)
  /\ WF_vars(DrainDeferredSuffix)
  /\ \A s \in SeqIds :
       /\ WF_vars(EncodeWhole(s))
       /\ WF_vars(DetachRetiredPayload(s))
       /\ WF_vars(DestroyOwner(s))
       /\ WF_vars(FinishPayloadRetirement(s))
       /\ WF_vars(SubmitRetired(s))
       /\ WF_vars(SubmitLegacy(s))
       /\ WF_vars(GpuComplete(s))
       /\ WF_vars(SettleDeviceLoss(s))
       /\ WF_vars(FinishCompletion(s))

Spec == Init /\ [][Next]_vars /\ RetirementFairness

TypeOK ==
  /\ nextSeq \in 1 .. (MaxSeqId + 1)
  /\ payloadState \in [SeqIds -> PayloadStates]
  /\ payloadGeneration \in [SeqIds -> Nat \ {0}]
  /\ hasPresent \in [SeqIds -> BOOLEAN]
  /\ receiptState \in [SeqIds -> ReceiptStates]
  /\ receiptSlot \in [SeqIds -> 0 .. MaxReceiptSlots]
  /\ receiptGeneration \in [SeqIds -> Nat]
  /\ slotOwner \in [ReceiptSlots -> SeqId0]
  /\ slotGeneration \in [ReceiptSlots -> Nat \ {0}]
  /\ gpuOutstanding \subseteq SeqIds
  /\ completionQueue \in Seq(SeqIds)
  /\ completedSeqId \in SeqId0
  /\ completedPresentSeqId \in SeqId0
  /\ callbackCount \in [SeqIds -> 0 .. 1]
  /\ settlement \in [SeqIds -> SettlementStates]
  /\ resourceRetained \in [SeqIds -> BOOLEAN]
  /\ deviceLost \in BOOLEAN
  /\ deferredAccounting \in DeferredAccountingState

RetirementRequiresEncode ==
  \A s \in SeqIds :
    payloadState[s] \in {"Detached", "OwnerDestroyed", "Retired"} =>
      receiptState[s] # "None"

TwoPhasePageRelease ==
  \A s \in SeqIds :
    /\ (payloadState[s] \in {"Detached", "OwnerDestroyed"} =>
          payloadGeneration[s] = 1)
    /\ (payloadState[s] = "Retired" => payloadGeneration[s] > 1)

LiveReceiptGenerationMatches ==
  \A s \in SeqIds :
    receiptState[s] \in {"Active", "Submitted", "Completed"} =>
      /\ receiptSlot[s] = SlotOf(s)
      /\ slotOwner[receiptSlot[s]] = s
      /\ slotGeneration[receiptSlot[s]] = receiptGeneration[s]

ReleasedReceiptIsStale ==
  \A s \in SeqIds :
    receiptState[s] = "Released" =>
      /\ slotOwner[receiptSlot[s]] # s
      /\ slotGeneration[receiptSlot[s]] > receiptGeneration[s]

CompletionExactlyOnce ==
  \A s \in SeqIds :
    /\ callbackCount[s] <= 1
    /\ (settlement[s] # "None" <=> callbackCount[s] = 1)

DeviceLossSettlesExactlyOnce ==
  \A s \in SeqIds :
    settlement[s] = "DeviceLoss" =>
      /\ deviceLost
      /\ s \notin gpuOutstanding
      /\ callbackCount[s] = 1

ResourcesSurviveCompletion ==
  \A s \in SeqIds :
    (s \in gpuOutstanding \/ s \in QueueSet(completionQueue)) =>
      resourceRetained[s]

OrderedWaterlines ==
  /\ completedPresentSeqId <= completedSeqId
  /\ completedSeqId < nextSeq
  /\ (completedPresentSeqId # 0 => hasPresent[completedPresentSeqId])
  /\ \A s \in SeqIds : s <= completedSeqId => callbackCount[s] = 1

PresentNeverRetires ==
  \A s \in SeqIds :
    hasPresent[s] =>
      payloadState[s] \notin {"Detached", "OwnerDestroyed", "Retired"}

GpuAccountingIndependentOfResidency ==
  \A s \in gpuOutstanding :
    payloadState[s] \in {"Encoded", "Retired"}

BoundedReceipts ==
  Cardinality({slot \in ReceiptSlots : slotOwner[slot] # 0}) <=
    MaxReceiptSlots

NoRetireWhileDeferred ==
  \A s \in SeqIds :
    (deferredAccounting.remainingCommands[s] > 0 \/
     deferredAccounting.finalBorrow[s]) =>
      payloadState[s] \notin {"Detached", "OwnerDestroyed", "Retired"}

CommandExactlyOnce ==
  /\ \A s \in SeqIds :
       deferredAccounting.commandEffects[s] <= ExpectedCommands(s)
  /\ (payloadState[1] = "PrefixEncoded" =>
        /\ deferredAccounting.remainingCommands[1] = 1
        /\ deferredAccounting.finalBorrow[1]
        /\ deferredAccounting.commandEffects[1] = 1)
  /\ \A s \in SeqIds :
       payloadState[s] \in
         {"Encoded", "Detached", "OwnerDestroyed", "Retired"} =>
         /\ deferredAccounting.remainingCommands[s] = 0
         /\ ~deferredAccounting.finalBorrow[s]
         /\ deferredAccounting.commandEffects[s] = ExpectedCommands(s)

ReceiptExactlyOnce ==
  \A s \in SeqIds :
    /\ deferredAccounting.receiptActivations[s] <= 1
    /\ (receiptState[s] # "None" <=>
          deferredAccounting.receiptActivations[s] = 1)

CompletionFinishExactlyOnce ==
  \A s \in SeqIds :
    /\ deferredAccounting.completionFinishes[s] <= 1
    /\ (deferredAccounting.completionFinishes[s] = 1 <=>
          s <= completedSeqId)

ReclaimExactlyOnce ==
  \A s \in SeqIds :
    /\ deferredAccounting.reclaims[s] <= 1
    /\ (deferredAccounting.reclaims[s] = 1 <=> s <= completedSeqId)

CurrentThenSuccessorFifo ==
  /\ (deferredAccounting.receiptActivations[2] = 1 =>
        deferredAccounting.receiptActivations[1] = 1)
  /\ (deferredAccounting.completionFinishes[2] = 1 =>
        deferredAccounting.completionFinishes[1] = 1)
  /\ (deferredAccounting.reclaims[2] = 1 =>
        deferredAccounting.reclaims[1] = 1)

CompletionRegistrationIsFifo ==
  \A s \in SeqIds :
    CompletionRegistered(s) =>
      \A older \in SeqIds : older < s => CompletionRegistered(older)

NoPostEffectRollback ==
  /\ ~deferredAccounting.rollbackAfterEffect
  /\ (deferredAccounting.successorEffectful =>
        deferredAccounting.lastPath = "Join")
  /\ (deferredAccounting.lastPath \in {"StaleFailOpen", "Drain"} =>
        ~deferredAccounting.successorEffectful)

DeferredJoinOrDrainProgress ==
  (payloadState[1] = "PrefixEncoded") ~>
    (payloadState[1] = "Encoded" /\
     deferredAccounting.lastPath # "None")

ReceiptActivationProgress ==
  \A s \in 1 .. 2 :
    (payloadState[s] = "Encoded" /\
     deferredAccounting.receiptActivations[s] = 0) ~>
      (deferredAccounting.receiptActivations[s] = 1)

CompletionFinishProgress ==
  \A s \in 1 .. 2 :
    (deferredAccounting.receiptActivations[s] = 1) ~>
      (deferredAccounting.completionFinishes[s] = 1 \/ deviceLost)

ReclaimProgress ==
  \A s \in 1 .. 2 :
    (deferredAccounting.receiptActivations[s] = 1) ~>
      (deferredAccounting.reclaims[s] = 1 \/ deviceLost)

CurrentBeforeSuccessorProgress ==
  (deferredAccounting.receiptActivations[2] = 1) ~>
    (deferredAccounting.completionFinishes[1] = 1 \/ deviceLost)

CurrentBeforeSuccessorCompletion ==
  [](deferredAccounting.completionFinishes[2] = 0 \/
     deferredAccounting.completionFinishes[1] = 1)

Inv ==
  /\ TypeOK
  /\ RetirementRequiresEncode
  /\ TwoPhasePageRelease
  /\ LiveReceiptGenerationMatches
  /\ ReleasedReceiptIsStale
  /\ CompletionExactlyOnce
  /\ DeviceLossSettlesExactlyOnce
  /\ ResourcesSurviveCompletion
  /\ OrderedWaterlines
  /\ PresentNeverRetires
  /\ GpuAccountingIndependentOfResidency
  /\ BoundedReceipts
  /\ NoRetireWhileDeferred
  /\ CommandExactlyOnce
  /\ ReceiptExactlyOnce
  /\ CompletionFinishExactlyOnce
  /\ ReclaimExactlyOnce
  /\ CurrentThenSuccessorFifo
  /\ CompletionRegistrationIsFifo
  /\ NoPostEffectRollback

====
