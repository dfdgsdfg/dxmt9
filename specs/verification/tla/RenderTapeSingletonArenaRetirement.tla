---- MODULE RenderTapeSingletonArenaRetirement ----
(******************************************************************************
 * Small singleton Arena completion/refinement model.
 *
 * A strict singleton may retire its physical Tape source immediately after
 * encode.  The source can then be reused while the queue receipt remains the
 * completion authority.  The group identity therefore has to be copied into
 * that receipt before physical retirement, taken exactly once at completion,
 * and consumed by the source-ordered finish settlement FIFO only after the
 * completion tail reaches the finish waterline.
 *
 * This model deliberately uses two one-source groups.  The first group
 * retires and reuses one physical source before completion; finishing it frees
 * the receipt slot and advances its generation, allowing the second group to
 * exercise stale-receipt rejection against the recycled slot.
 *
 * It does not prove allocator internals, Metal/GPU behavior, byte contents,
 * callback memory ordering, or an unbounded completion schedule.  The queue
 * and source order are represented by the bounded sequence/frontier values
 * below.  AllowOmittedIdentityTransfer is intentionally a liveness negative:
 * completion may still advance the sequence, but the exact event waiter can
 * never observe its group tuple.
 ******************************************************************************)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS AllowOmittedIdentityTransfer,
          AllowPrematurePublish,
          AllowStaleReceipt

Events == 1 .. 2
Phases == {"Represented", "ReceiptActive", "Retired", "Submitted",
           "Completed", "Taken", "Done"}
ReceiptStates == {"Free", "Active", "Submitted", "Completed"}

NoIdentity == [rawOrdinal         |-> 0,
               buildGeneration    |-> 0,
               firstSourceOrdinal |-> 0,
               tailSeqId          |-> 0,
               sourceCount        |-> 0,
               hasPresent         |-> FALSE]

GroupOne == [rawOrdinal         |-> 501,
             buildGeneration    |-> 11,
             firstSourceOrdinal |-> 71,
             tailSeqId          |-> 1,
             sourceCount        |-> 1,
             hasPresent         |-> FALSE]

GroupTwo == [rawOrdinal         |-> 502,
             buildGeneration    |-> 12,
             firstSourceOrdinal |-> 72,
             tailSeqId          |-> 2,
             sourceCount        |-> 1,
             hasPresent         |-> TRUE]

ExpectedGroups == <<GroupOne, GroupTwo>>
GroupValues == {NoIdentity, GroupOne, GroupTwo}
StaleReceiptGeneration == 1

ExpectedGroup(e) == ExpectedGroups[e]
IdentityValid(g) ==
  /\ g.rawOrdinal # 0
  /\ g.buildGeneration # 0
  /\ g.firstSourceOrdinal # 0
  /\ g.tailSeqId # 0
  /\ g.sourceCount = 1

VARIABLES phase,
          event,
          physicalGeneration,
          sourceAlive,
          physicalReuseCount,
          slotGeneration,
          receiptGeneration,
          receiptState,
          receiptGroup,
          gpuCompletedSeq,
          finishWaterline,
          settlementQueue,
          settledGroups,
          markCompletedCount,
          takeCount,
          staleProbeDone,
          staleAccepted,
          staleRejected,
          prematurePublished

vars == <<phase, event, physicalGeneration, sourceAlive,
          physicalReuseCount, slotGeneration, receiptGeneration,
          receiptState, receiptGroup, gpuCompletedSeq, finishWaterline,
          settlementQueue, settledGroups, markCompletedCount, takeCount,
          staleProbeDone, staleAccepted, staleRejected, prematurePublished>>

Init ==
  /\ phase = "Represented"
  /\ event = 1
  /\ physicalGeneration = 1
  /\ sourceAlive = TRUE
  /\ physicalReuseCount = 0
  /\ slotGeneration = 1
  /\ receiptGeneration = 0
  /\ receiptState = "Free"
  /\ receiptGroup = NoIdentity
  /\ gpuCompletedSeq = 0
  /\ finishWaterline = 0
  /\ settlementQueue = <<>>
  /\ settledGroups = <<>>
  /\ markCompletedCount = 0
  /\ takeCount = 0
  /\ staleProbeDone = FALSE
  /\ staleAccepted = FALSE
  /\ staleRejected = FALSE
  /\ prematurePublished = FALSE

ActivateReceipt ==
  /\ phase = "Represented"
  /\ receiptState = "Free"
  /\ receiptGeneration' = slotGeneration
  /\ receiptGroup' =
       IF AllowOmittedIdentityTransfer THEN NoIdentity
       ELSE ExpectedGroup(event)
  /\ receiptState' = "Active"
  /\ phase' = "ReceiptActive"
  /\ UNCHANGED <<event, physicalGeneration, sourceAlive,
      physicalReuseCount, slotGeneration, gpuCompletedSeq, finishWaterline,
      settlementQueue, settledGroups, markCompletedCount, takeCount,
      staleProbeDone, staleAccepted, staleRejected, prematurePublished>>

RetirePhysicalSource ==
  /\ phase = "ReceiptActive"
  /\ sourceAlive
  /\ sourceAlive' = FALSE
  /\ phase' = "Retired"
  /\ UNCHANGED <<event, physicalGeneration, physicalReuseCount,
      slotGeneration, receiptGeneration, receiptState, receiptGroup,
      gpuCompletedSeq, finishWaterline, settlementQueue, settledGroups,
      markCompletedCount, takeCount, staleProbeDone, staleAccepted,
      staleRejected, prematurePublished>>

ReusePhysicalSource ==
  /\ event \in Events
  /\ phase = "Retired"
  /\ ~sourceAlive
  /\ physicalGeneration' = IF event = 1 THEN 2 ELSE physicalGeneration
  /\ sourceAlive' = TRUE
  /\ physicalReuseCount' = IF event = 1 THEN 1 ELSE physicalReuseCount
  /\ UNCHANGED <<phase, event, slotGeneration, receiptGeneration,
      receiptState, receiptGroup, gpuCompletedSeq, finishWaterline,
      settlementQueue, settledGroups, markCompletedCount, takeCount,
      staleProbeDone, staleAccepted, staleRejected, prematurePublished>>

SubmitReceipt ==
  /\ phase = "Retired"
  /\ sourceAlive
  /\ receiptState = "Active"
  /\ receiptState' = "Submitted"
  /\ phase' = "Submitted"
  /\ UNCHANGED <<event, physicalGeneration, sourceAlive,
      physicalReuseCount, slotGeneration, receiptGeneration, receiptGroup,
      gpuCompletedSeq, finishWaterline, settlementQueue, settledGroups,
      markCompletedCount, takeCount, staleProbeDone, staleAccepted,
      staleRejected, prematurePublished>>

MarkCompleted ==
  /\ phase = "Submitted"
  /\ receiptState = "Submitted"
  /\ gpuCompletedSeq' = event
  /\ receiptState' = "Completed"
  /\ phase' = "Completed"
  /\ markCompletedCount' = markCompletedCount + 1
  /\ UNCHANGED <<event, physicalGeneration, sourceAlive,
      physicalReuseCount, slotGeneration, receiptGeneration, receiptGroup,
      finishWaterline, settlementQueue, settledGroups, takeCount,
      staleProbeDone, staleAccepted, staleRejected, prematurePublished>>

TakeCompletedIdentity ==
  /\ phase = "Completed"
  /\ receiptState = "Completed"
  /\ takeCount = markCompletedCount - 1
  /\ settlementQueue' =
       IF IdentityValid(receiptGroup)
       THEN Append(settlementQueue, receiptGroup)
       ELSE settlementQueue
  /\ receiptState' = "Completed"
  /\ phase' = "Taken"
  /\ takeCount' = takeCount + 1
  /\ receiptGroup' = NoIdentity
  /\ UNCHANGED <<event, physicalGeneration, sourceAlive,
      physicalReuseCount, slotGeneration, receiptGeneration,
      gpuCompletedSeq, finishWaterline, settledGroups, markCompletedCount,
      staleProbeDone, staleAccepted, staleRejected, prematurePublished>>

(* This is the exact-event bridge's visible settlement.  A valid tuple is
 * consumed only from the source-ordered FIFO and only once its tail is done.
 * The receipt group is already cleared by TakeCompletedIdentity; the FIFO is
 * the sole owner after that transfer. *)
FinishPublishExact ==
  LET hasIdentity == Len(settlementQueue) > 0 IN
    /\ phase = "Taken"
    /\ event = finishWaterline + 1
    /\ gpuCompletedSeq >= event
    /\ (hasIdentity =>
          /\ Len(settlementQueue) > 0
          /\ Head(settlementQueue) = ExpectedGroup(event))
    /\ finishWaterline' = event
    /\ settlementQueue' =
         IF hasIdentity THEN Tail(settlementQueue) ELSE settlementQueue
    /\ settledGroups' =
         IF hasIdentity
         THEN Append(settledGroups, ExpectedGroup(event))
         ELSE settledGroups
    /\ receiptState' = "Free"
    /\ slotGeneration' = slotGeneration + 1
    /\ receiptGeneration' = 0
    /\ receiptGroup' = NoIdentity
    /\ phase' = IF event = 1 THEN "Represented" ELSE "Done"
    /\ event' = event + 1
    /\ UNCHANGED <<physicalGeneration, sourceAlive, physicalReuseCount,
        gpuCompletedSeq, markCompletedCount, takeCount, staleProbeDone,
        staleAccepted, staleRejected, prematurePublished>>

(* A valid stale-generation probe is rejected in production.  The negative
 * configuration bypasses the generation match and consumes the newer
 * completed entry through an old receipt. *)
ProbeStaleReceipt ==
  /\ event = 2
  /\ phase = "Completed"
  /\ slotGeneration = 2
  /\ receiptGeneration = 2
  /\ receiptState = "Completed"
  /\ StaleReceiptGeneration # receiptGeneration
  /\ ~staleProbeDone
  /\ staleProbeDone' = TRUE
  /\ staleAccepted' = AllowStaleReceipt
  /\ staleRejected' = ~AllowStaleReceipt
  /\ receiptGroup' =
       IF AllowStaleReceipt THEN NoIdentity ELSE receiptGroup
  /\ settlementQueue' =
       IF AllowStaleReceipt
       THEN Append(settlementQueue, receiptGroup)
       ELSE settlementQueue
  /\ phase' = IF AllowStaleReceipt THEN "Taken" ELSE phase
  /\ takeCount' =
       IF AllowStaleReceipt THEN takeCount + 1 ELSE takeCount
  /\ UNCHANGED <<event, physicalGeneration, sourceAlive,
      physicalReuseCount, slotGeneration, receiptGeneration,
      receiptState, gpuCompletedSeq, finishWaterline, settledGroups,
      markCompletedCount,
      prematurePublished>>

(* Negative safety control: publishing the sequence/frontier before the
 * completion tail is done is not an admissible finish transition. *)
PrematurePublishBug ==
  /\ AllowPrematurePublish
  /\ phase = "ReceiptActive"
  /\ finishWaterline < event
  /\ prematurePublished' = TRUE
  /\ finishWaterline' = event
  /\ phase' = "Done"
  /\ UNCHANGED <<event, physicalGeneration, sourceAlive,
      physicalReuseCount, slotGeneration, receiptGeneration, receiptState,
      receiptGroup, gpuCompletedSeq, settlementQueue, settledGroups,
      markCompletedCount, takeCount, staleProbeDone, staleAccepted,
      staleRejected>>

Next ==
  \/ ActivateReceipt
  \/ RetirePhysicalSource
  \/ ReusePhysicalSource
  \/ SubmitReceipt
  \/ MarkCompleted
  \/ TakeCompletedIdentity
  \/ FinishPublishExact
  \/ ProbeStaleReceipt
  \/ PrematurePublishBug

ProgressFairness ==
  /\ WF_vars(ActivateReceipt)
  /\ WF_vars(RetirePhysicalSource)
  /\ WF_vars(ReusePhysicalSource)
  /\ WF_vars(SubmitReceipt)
  /\ WF_vars(MarkCompleted)
  /\ WF_vars(TakeCompletedIdentity)
  /\ WF_vars(FinishPublishExact)
  /\ WF_vars(ProbeStaleReceipt)

Spec == Init /\ [][Next]_vars
ProgressSpec == Spec /\ ProgressFairness

TypeOK ==
  /\ phase \in Phases
  /\ event \in 1 .. 3
  /\ physicalGeneration \in 1 .. 2
  /\ sourceAlive \in BOOLEAN
  /\ physicalReuseCount \in 0 .. 1
  /\ slotGeneration \in 1 .. 3
  /\ receiptGeneration \in 0 .. 2
  /\ receiptState \in ReceiptStates
  /\ receiptGroup \in GroupValues
  /\ gpuCompletedSeq \in 0 .. 2
  /\ finishWaterline \in 0 .. 2
  /\ settlementQueue \in Seq(GroupValues)
  /\ settledGroups \in Seq(GroupValues)
  /\ markCompletedCount \in 0 .. 2
  /\ takeCount \in 0 .. 2
  /\ staleProbeDone \in BOOLEAN
  /\ staleAccepted \in BOOLEAN
  /\ staleRejected \in BOOLEAN
  /\ prematurePublished \in BOOLEAN

ReceiptGenerationMatches ==
  receiptState \in {"Active", "Submitted", "Completed"} =>
    receiptGeneration = slotGeneration

ExactIdentityTransferred ==
  (~AllowOmittedIdentityTransfer) =>
    ((receiptState \in {"Active", "Submitted", "Completed"} /\
       phase # "Taken") =>
      receiptGroup = ExpectedGroup(event))

NoPrematurePublish ==
  ~prematurePublished /\ finishWaterline <= gpuCompletedSeq

NoStaleReceiptAccepted == ~staleAccepted

OneShotCompletionTake == takeCount <= markCompletedCount

PhysicalReuseRetainsReceipt ==
  (~AllowOmittedIdentityTransfer) =>
    ((physicalReuseCount = 1 /\ receiptState # "Free" /\
       phase # "Taken") =>
      receiptGroup = ExpectedGroup(event))

SettlementIsExactPrefix ==
  (~AllowOmittedIdentityTransfer) =>
    settledGroups = SubSeq(ExpectedGroups, 1, Len(settledGroups))

Inv ==
  /\ TypeOK
  /\ ReceiptGenerationMatches
  /\ ExactIdentityTransferred
  /\ NoPrematurePublish
  /\ NoStaleReceiptAccepted
  /\ OneShotCompletionTake
  /\ PhysicalReuseRetainsReceipt
  /\ SettlementIsExactPrefix

EventuallySettled == <>(settledGroups = ExpectedGroups)

THEOREM Spec => []Inv
====
