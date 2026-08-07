---- MODULE PostEncodePayloadRetirement ----
(*
 * R-BACK-2.44/2.45/2.49/2.59/2.65 post-encode retirement refinement.
 *
 * The ownership-critical retirement sequence is explicit: Publish, Encode,
 * detach and activate a queue receipt while locked, destroy re-entrant owners
 * outside the lock, then finish page/generation release while locked. Receipt,
 * retained-resource, GPU-work, ordinary completion, device-loss settlement,
 * and Present completion are separate bounded state.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS MaxSeqId, MaxReceiptSlots

SeqIds == 1 .. MaxSeqId
SeqId0 == 0 .. MaxSeqId
ReceiptSlots == 1 .. MaxReceiptSlots
PayloadStates ==
  {"Absent", "Published", "Encoded", "Detached", "OwnerDestroyed",
   "Retired"}
ReceiptStates == {"None", "Active", "Submitted", "Completed", "Released"}
SettlementStates == {"None", "Gpu", "DeviceLoss"}

SlotOf(s) == ((s - 1) % MaxReceiptSlots) + 1
QueueSet(seq) == {seq[i] : i \in DOMAIN seq}

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
  deviceLost

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
     deviceLost >>

PayloadResident(s) ==
  payloadState[s] \in
    {"Published", "Encoded", "Detached", "OwnerDestroyed"}

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

Publish ==
  /\ ~deviceLost
  /\ nextSeq <= MaxSeqId
  /\ payloadState' = [payloadState EXCEPT ![nextSeq] = "Published"]
  /\ hasPresent' =
       [hasPresent EXCEPT ![nextSeq] = (nextSeq = MaxSeqId)]
  /\ resourceRetained' =
       [resourceRetained EXCEPT ![nextSeq] = TRUE]
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

Encode(s) ==
  /\ ~deviceLost
  /\ payloadState[s] = "Published"
  /\ \A older \in SeqIds : older < s => payloadState[older] # "Published"
  /\ payloadState' = [payloadState EXCEPT ![s] = "Encoded"]
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

DetachRetiredPayload(s) ==
  LET slot == SlotOf(s) IN
    /\ payloadState[s] = "Encoded"
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
                  deviceLost >>

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
                  deviceLost >>

SubmitRetired(s) ==
  /\ ~deviceLost
  /\ payloadState[s] = "Retired"
  /\ receiptState[s] = "Active"
  /\ settlement[s] = "None"
  /\ s \notin gpuOutstanding
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
                  deviceLost >>

SubmitLegacy(s) ==
  /\ ~deviceLost
  /\ payloadState[s] = "Encoded"
  /\ receiptState[s] = "None"
  /\ settlement[s] = "None"
  /\ s \notin gpuOutstanding
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
                  deviceLost >>

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
                  deviceLost >>

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
                  resourceRetained >>

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
                  deviceLost >>

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
  \/ \E s \in SeqIds : Encode(s)
  \/ \E s \in SeqIds : DetachRetiredPayload(s)
  \/ \E s \in SeqIds : DestroyOwner(s)
  \/ \E s \in SeqIds : FinishPayloadRetirement(s)
  \/ \E s \in SeqIds : SubmitRetired(s)
  \/ \E s \in SeqIds : SubmitLegacy(s)
  \/ \E s \in SeqIds : GpuComplete(s)
  \/ BeginDeviceLoss
  \/ \E s \in SeqIds : SettleDeviceLoss(s)
  \/ \E s \in SeqIds : FinishCompletion(s)

Spec == Init /\ [][Next]_vars

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

====
