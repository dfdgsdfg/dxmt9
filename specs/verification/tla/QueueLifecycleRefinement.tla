---- MODULE QueueLifecycleRefinement ----
(*
 * dxmt9 QueueLifecycleController refinement model
 *
 * This is a concrete-ish model of the C++ QueueLifecycleController fields
 * that sit below CommandQueue.tla:
 *
 *   writingSlot          -> std::optional<size_t> writingSlot_
 *   writeIndex           -> size_t writeIndex_
 *   nextSeqId            -> u64 nextSeqId_
 *   readySlots           -> std::deque<size_t> readySlots_
 *   pendingCompletion    -> QueueLifecycleController::pendingCompletion_
 *   completedSeqQueue    -> std::deque<u64> completedSeqQueue_
 *   inflightCount        -> size_t inflightCount_
 *   completedSeqId       -> u64 completedSeqId_
 *   lastCommittedSeqId   -> u64 lastCommittedSeqId_
 *   stop                 -> bool stop_
 *
 * Compared with CommandQueue.tla, this model exposes implementation staging:
 *
 *   Writing --CommitPublish--> Pending --EncodeDequeue--> Encoding
 *     --EncodeSubmitToGpu--> GPU --GpuComplete--> completedSeqQueue
 *     --FinishDequeue--> completedSeqId --ReclaimFree--> Free
 *
 * Empty commits cancel a Writing slot without publishing work. Inline
 * completion is modeled as an Encoding chunk that has no Metal command buffer:
 * it reaches Free directly and queues its seqId for the same finish-thread
 * completion accounting path.
 *
 * TLC uses small constants; they are enough to exercise wrap-around,
 * back-pressure, split GPU/finish/reclaim staging, inline completion,
 * empty commit, waitForSequence, and stop/shutdown wakeups.
 *)

EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
  RING_SIZE,
  MAX_INFLIGHT,
  MAX_SEQID

ASSUME RING_SIZE \in Nat /\ RING_SIZE > 1
ASSUME MAX_INFLIGHT \in Nat /\ MAX_INFLIGHT >= 1
ASSUME MAX_INFLIGHT < RING_SIZE
ASSUME MAX_SEQID \in Nat /\ MAX_SEQID > MAX_INFLIGHT

Slots == 0 .. (RING_SIZE - 1)
NoSlot == RING_SIZE
MaybeSlot == Slots \cup {NoSlot}
SeqIds == 0 .. MAX_SEQID
LiveSeqIds == 1 .. MAX_SEQID

SlotStates == {"Free", "Writing", "Pending", "Encoding", "GPU"}
InflightStates == {"Pending", "Encoding", "GPU"}

PendingRecord == [slot : Slots, seq : LiveSeqIds]

VARIABLES
  slotState,
  slotSeqId,
  slotHasCommands,
  writingSlot,
  writeIndex,
  nextSeqId,
  readySlots,
  pendingCompletion,
  completedSeqQueue,
  inflightCount,
  completedSeqId,
  lastCommittedSeqId,
  stop,
  waitActive,
  waitTarget,
  lastWaitEndedTarget,
  lastWaitEndedCompletedSeqId,
  lastWaitEndedStopped

vars ==
  <<slotState, slotSeqId, slotHasCommands, writingSlot, writeIndex,
    nextSeqId, readySlots, pendingCompletion, completedSeqQueue,
    inflightCount, completedSeqId, lastCommittedSeqId, stop,
    waitActive, waitTarget, lastWaitEndedTarget,
    lastWaitEndedCompletedSeqId, lastWaitEndedStopped>>

IsSeqOver(q, elems) ==
  /\ DOMAIN q = 1 .. Len(q)
  /\ \A i \in 1 .. Len(q) : q[i] \in elems

IsPendingRecordSeq(q) ==
  /\ DOMAIN q = 1 .. Len(q)
  /\ \A i \in 1 .. Len(q) : q[i] \in PendingRecord

NoDuplicateSlots(q) ==
  \A i, j \in 1 .. Len(q) : i # j => q[i] # q[j]

NoDuplicatePendingSeqIds(q) ==
  \A i, j \in 1 .. Len(q) : i # j => q[i].seq # q[j].seq

NoDuplicateSeqIds(q) ==
  \A i, j \in 1 .. Len(q) : i # j => q[i] # q[j]

NextCompletionSeq ==
  completedSeqId + Len(completedSeqQueue) + 1

AbstractSlotState(st, seq, hasCommands, completed, s) ==
  IF st[s] = "Writing" /\ ~hasCommands[s] THEN
    "Free"
  ELSE IF st[s] = "GPU" /\ seq[s] # 0 /\ seq[s] <= completed THEN
    "Free"
  ELSE
    st[s]

AbstractInflightSlotCount ==
  Cardinality({s \in Slots :
    AbstractSlotState(slotState, slotSeqId, slotHasCommands,
                      completedSeqId, s) \in InflightStates})

AllowedAbstractEdges ==
  {<<"Free", "Free">>,
   <<"Free", "Writing">>,
   <<"Writing", "Writing">>,
   <<"Writing", "Pending">>,
   <<"Pending", "Pending">>,
   <<"Pending", "Encoding">>,
   <<"Encoding", "Encoding">>,
   <<"Encoding", "GPU">>,
   \* Inline completion collapses the unobservable GPU stage.
   <<"Encoding", "Free">>,
   <<"GPU", "GPU">>,
   <<"GPU", "Free">>}

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ slotState = [s \in Slots |-> "Free"]
  /\ slotSeqId = [s \in Slots |-> 0]
  /\ slotHasCommands = [s \in Slots |-> FALSE]
  /\ writingSlot = NoSlot
  /\ writeIndex = 0
  /\ nextSeqId = 1
  /\ readySlots = <<>>
  /\ pendingCompletion = <<>>
  /\ completedSeqQueue = <<>>
  /\ inflightCount = 0
  /\ completedSeqId = 0
  /\ lastCommittedSeqId = 0
  /\ stop = FALSE
  /\ waitActive = FALSE
  /\ waitTarget = 0
  /\ lastWaitEndedTarget = 0
  /\ lastWaitEndedCompletedSeqId = 0
  /\ lastWaitEndedStopped = FALSE

(* ================================================================
   Wine thread / producer actions
   ================================================================ *)

WriterAcquire ==
  /\ ~stop
  /\ writingSlot = NoSlot
  /\ slotState[writeIndex] = "Free"
  /\ inflightCount < MAX_INFLIGHT
  /\ slotState' = [slotState EXCEPT ![writeIndex] = "Writing"]
  /\ slotSeqId' = [slotSeqId EXCEPT ![writeIndex] = 0]
  /\ slotHasCommands' = [slotHasCommands EXCEPT ![writeIndex] = FALSE]
  /\ writingSlot' = writeIndex
  /\ UNCHANGED <<writeIndex, nextSeqId, readySlots, pendingCompletion,
                completedSeqQueue, inflightCount, completedSeqId,
                lastCommittedSeqId, stop, waitActive, waitTarget,
                lastWaitEndedTarget, lastWaitEndedCompletedSeqId,
                lastWaitEndedStopped>>

AppendCommand ==
  /\ ~stop
  /\ writingSlot # NoSlot
  /\ slotState[writingSlot] = "Writing"
  /\ ~slotHasCommands[writingSlot]
  /\ slotHasCommands' = [slotHasCommands EXCEPT ![writingSlot] = TRUE]
  /\ UNCHANGED <<slotState, slotSeqId, writingSlot, writeIndex, nextSeqId,
                readySlots, pendingCompletion, completedSeqQueue,
                inflightCount, completedSeqId, lastCommittedSeqId, stop,
                waitActive, waitTarget, lastWaitEndedTarget,
                lastWaitEndedCompletedSeqId, lastWaitEndedStopped>>

CommitEmpty ==
  /\ ~stop
  /\ writingSlot # NoSlot
  /\ slotState[writingSlot] = "Writing"
  /\ ~slotHasCommands[writingSlot]
  /\ slotState' = [slotState EXCEPT ![writingSlot] = "Free"]
  /\ slotSeqId' = [slotSeqId EXCEPT ![writingSlot] = 0]
  /\ slotHasCommands' = [slotHasCommands EXCEPT ![writingSlot] = FALSE]
  /\ writingSlot' = NoSlot
  /\ UNCHANGED <<writeIndex, nextSeqId, readySlots, pendingCompletion,
                completedSeqQueue, inflightCount, completedSeqId,
                lastCommittedSeqId, stop, waitActive, waitTarget,
                lastWaitEndedTarget, lastWaitEndedCompletedSeqId,
                lastWaitEndedStopped>>

CommitPublish ==
  /\ ~stop
  /\ writingSlot # NoSlot
  /\ slotState[writingSlot] = "Writing"
  /\ slotHasCommands[writingSlot]
  /\ inflightCount < MAX_INFLIGHT
  /\ nextSeqId <= MAX_SEQID
  /\ slotState' = [slotState EXCEPT ![writingSlot] = "Pending"]
  /\ slotSeqId' = [slotSeqId EXCEPT ![writingSlot] = nextSeqId]
  /\ slotHasCommands' = [slotHasCommands EXCEPT ![writingSlot] = TRUE]
  /\ readySlots' = Append(readySlots, writingSlot)
  /\ inflightCount' = inflightCount + 1
  /\ lastCommittedSeqId' = nextSeqId
  /\ nextSeqId' = nextSeqId + 1
  /\ writeIndex' = (writeIndex + 1) % RING_SIZE
  /\ writingSlot' = NoSlot
  /\ UNCHANGED <<pendingCompletion, completedSeqQueue, completedSeqId,
                stop, waitActive, waitTarget, lastWaitEndedTarget,
                lastWaitEndedCompletedSeqId, lastWaitEndedStopped>>

(* ================================================================
   Encode / GPU / finish actions
   ================================================================ *)

EncodeDequeue ==
  /\ Len(readySlots) > 0
  /\ slotState[Head(readySlots)] = "Pending"
  /\ slotState' = [slotState EXCEPT ![Head(readySlots)] = "Encoding"]
  /\ readySlots' = Tail(readySlots)
  /\ UNCHANGED <<slotSeqId, slotHasCommands, writingSlot, writeIndex,
                nextSeqId, pendingCompletion, completedSeqQueue,
                inflightCount, completedSeqId, lastCommittedSeqId, stop,
                waitActive, waitTarget, lastWaitEndedTarget,
                lastWaitEndedCompletedSeqId, lastWaitEndedStopped>>

EncodeSubmitToGpu(s) ==
  /\ s \in Slots
  /\ slotState[s] = "Encoding"
  /\ slotSeqId[s] \in LiveSeqIds
  /\ slotState' = [slotState EXCEPT ![s] = "GPU"]
  /\ pendingCompletion' =
       Append(pendingCompletion, [slot |-> s, seq |-> slotSeqId[s]])
  /\ UNCHANGED <<slotSeqId, slotHasCommands, writingSlot, writeIndex,
                nextSeqId, readySlots, completedSeqQueue, inflightCount,
                completedSeqId, lastCommittedSeqId, stop, waitActive,
                waitTarget, lastWaitEndedTarget,
                lastWaitEndedCompletedSeqId, lastWaitEndedStopped>>

EncodeCompleteInline(s) ==
  /\ s \in Slots
  /\ slotState[s] = "Encoding"
  /\ slotSeqId[s] = NextCompletionSeq
  /\ slotState' = [slotState EXCEPT ![s] = "Free"]
  /\ slotSeqId' = [slotSeqId EXCEPT ![s] = 0]
  /\ slotHasCommands' = [slotHasCommands EXCEPT ![s] = FALSE]
  /\ completedSeqQueue' = Append(completedSeqQueue, slotSeqId[s])
  /\ UNCHANGED <<writingSlot, writeIndex, nextSeqId, readySlots,
                pendingCompletion, inflightCount, completedSeqId,
                lastCommittedSeqId, stop, waitActive, waitTarget,
                lastWaitEndedTarget, lastWaitEndedCompletedSeqId,
                lastWaitEndedStopped>>

GpuComplete ==
  /\ Len(pendingCompletion) > 0
  /\ Head(pendingCompletion).seq = NextCompletionSeq
  /\ pendingCompletion' = Tail(pendingCompletion)
  /\ completedSeqQueue' = Append(completedSeqQueue, Head(pendingCompletion).seq)
  /\ UNCHANGED <<slotState, slotSeqId, slotHasCommands, writingSlot,
                writeIndex, nextSeqId, readySlots, inflightCount,
                completedSeqId, lastCommittedSeqId, stop, waitActive,
                waitTarget, lastWaitEndedTarget, lastWaitEndedCompletedSeqId,
                lastWaitEndedStopped>>

FinishDequeue ==
  /\ Len(completedSeqQueue) > 0
  /\ Head(completedSeqQueue) = completedSeqId + 1
  /\ completedSeqId' = Head(completedSeqQueue)
  /\ completedSeqQueue' = Tail(completedSeqQueue)
  /\ inflightCount' = IF inflightCount > 0 THEN inflightCount - 1 ELSE 0
  /\ UNCHANGED <<slotState, slotSeqId, slotHasCommands, writingSlot,
                writeIndex, nextSeqId, readySlots, pendingCompletion,
                lastCommittedSeqId, stop, waitActive, waitTarget,
                lastWaitEndedTarget, lastWaitEndedCompletedSeqId,
                lastWaitEndedStopped>>

ReclaimFree(s) ==
  /\ s \in Slots
  /\ slotState[s] = "GPU"
  /\ slotSeqId[s] # 0
  /\ slotSeqId[s] <= completedSeqId
  /\ slotState' = [slotState EXCEPT ![s] = "Free"]
  /\ slotSeqId' = [slotSeqId EXCEPT ![s] = 0]
  /\ slotHasCommands' = [slotHasCommands EXCEPT ![s] = FALSE]
  /\ UNCHANGED <<writingSlot, writeIndex, nextSeqId, readySlots,
                pendingCompletion, completedSeqQueue, inflightCount,
                completedSeqId, lastCommittedSeqId, stop, waitActive,
                waitTarget, lastWaitEndedTarget,
                lastWaitEndedCompletedSeqId, lastWaitEndedStopped>>

(* ================================================================
   waitForSequence / stop actions
   ================================================================ *)

BeginWaitForSequence(t) ==
  /\ t \in LiveSeqIds
  /\ ~waitActive
  /\ completedSeqId < t
  /\ waitActive' = TRUE
  /\ waitTarget' = t
  /\ UNCHANGED <<slotState, slotSeqId, slotHasCommands, writingSlot,
                writeIndex, nextSeqId, readySlots, pendingCompletion,
                completedSeqQueue, inflightCount, completedSeqId,
                lastCommittedSeqId, stop, lastWaitEndedTarget,
                lastWaitEndedCompletedSeqId, lastWaitEndedStopped>>

EndWaitForSequence ==
  /\ waitActive
  /\ (stop \/ completedSeqId >= waitTarget)
  /\ waitActive' = FALSE
  /\ waitTarget' = 0
  /\ lastWaitEndedTarget' = waitTarget
  /\ lastWaitEndedCompletedSeqId' = completedSeqId
  /\ lastWaitEndedStopped' = stop
  /\ UNCHANGED <<slotState, slotSeqId, slotHasCommands, writingSlot,
                writeIndex, nextSeqId, readySlots, pendingCompletion,
                completedSeqQueue, inflightCount, completedSeqId,
                lastCommittedSeqId, stop>>

StopQueue ==
  /\ ~stop
  /\ stop' = TRUE
  /\ UNCHANGED <<slotState, slotSeqId, slotHasCommands, writingSlot,
                writeIndex, nextSeqId, readySlots, pendingCompletion,
                completedSeqQueue, inflightCount, completedSeqId,
                lastCommittedSeqId, waitActive, waitTarget,
                lastWaitEndedTarget, lastWaitEndedCompletedSeqId,
                lastWaitEndedStopped>>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ WriterAcquire
  \/ AppendCommand
  \/ CommitEmpty
  \/ CommitPublish
  \/ EncodeDequeue
  \/ \E s \in Slots : EncodeSubmitToGpu(s)
  \/ \E s \in Slots : EncodeCompleteInline(s)
  \/ GpuComplete
  \/ FinishDequeue
  \/ \E s \in Slots : ReclaimFree(s)
  \/ \E t \in LiveSeqIds : BeginWaitForSequence(t)
  \/ EndWaitForSequence
  \/ StopQueue

Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(EncodeDequeue)
  /\ WF_vars(GpuComplete)
  /\ WF_vars(FinishDequeue)

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ slotState \in [Slots -> SlotStates]
  /\ slotSeqId \in [Slots -> SeqIds]
  /\ slotHasCommands \in [Slots -> BOOLEAN]
  /\ writingSlot \in MaybeSlot
  /\ writeIndex \in Slots
  /\ nextSeqId \in 1 .. (MAX_SEQID + 1)
  /\ IsSeqOver(readySlots, Slots)
  /\ IsPendingRecordSeq(pendingCompletion)
  /\ IsSeqOver(completedSeqQueue, LiveSeqIds)
  /\ Len(readySlots) <= RING_SIZE
  /\ Len(pendingCompletion) <= MAX_INFLIGHT
  /\ Len(completedSeqQueue) <= MAX_INFLIGHT
  /\ inflightCount \in 0 .. MAX_INFLIGHT
  /\ completedSeqId \in SeqIds
  /\ lastCommittedSeqId \in SeqIds
  /\ stop \in BOOLEAN
  /\ waitActive \in BOOLEAN
  /\ waitTarget \in SeqIds
  /\ lastWaitEndedTarget \in SeqIds
  /\ lastWaitEndedCompletedSeqId \in SeqIds
  /\ lastWaitEndedStopped \in BOOLEAN

(* ================================================================
   Safety invariants
   ================================================================ *)

ReadySlotsPending ==
  \A i \in 1 .. Len(readySlots) :
    slotState[readySlots[i]] = "Pending"

ReadySlotsUnique ==
  NoDuplicateSlots(readySlots)

PendingCompletionWellFormed ==
  /\ NoDuplicatePendingSeqIds(pendingCompletion)
  /\ \A i \in 1 .. Len(pendingCompletion) :
       /\ slotState[pendingCompletion[i].slot] = "GPU"
       /\ slotSeqId[pendingCompletion[i].slot] = pendingCompletion[i].seq
       /\ pendingCompletion[i].seq <= lastCommittedSeqId

CompletedSeqQueueBound ==
  /\ NoDuplicateSeqIds(completedSeqQueue)
  /\ \A i \in 1 .. Len(completedSeqQueue) :
       /\ completedSeqQueue[i] <= lastCommittedSeqId
       /\ completedSeqQueue[i] > completedSeqId

CompletedSeqIdBound ==
  completedSeqId <= lastCommittedSeqId

BoundedInflight ==
  /\ inflightCount <= MAX_INFLIGHT
  /\ AbstractInflightSlotCount <= MAX_INFLIGHT

WriterSlotSafety ==
  /\ writingSlot = NoSlot \/ slotState[writingSlot] = "Writing"
  /\ AbstractSlotState(slotState, slotSeqId, slotHasCommands,
                       completedSeqId, writeIndex) \in {"Free", "Writing"}

SeqIdAssignmentSafety ==
  /\ lastCommittedSeqId < nextSeqId
  /\ \A s \in Slots :
       slotState[s] \in {"Pending", "Encoding", "GPU"} =>
         /\ slotSeqId[s] \in LiveSeqIds
         /\ slotSeqId[s] <= lastCommittedSeqId
  /\ \A s \in Slots :
       slotState[s] = "Free" => ~slotHasCommands[s]

WaitEndRecordSafety ==
  lastWaitEndedTarget = 0 \/
    lastWaitEndedStopped \/
    lastWaitEndedCompletedSeqId >= lastWaitEndedTarget

WaitForSequenceSafety ==
  [][waitActive /\ ~waitActive' =>
       (stop \/ completedSeqId >= waitTarget)]_vars

ConcreteSlotLifecycleRefinesAbstract ==
  [][\A s \in Slots :
      <<AbstractSlotState(slotState, slotSeqId, slotHasCommands, completedSeqId, s),
        AbstractSlotState(slotState', slotSeqId', slotHasCommands',
                          completedSeqId', s)>>
          \in AllowedAbstractEdges]_vars

Safety ==
  /\ TypeOK
  /\ ReadySlotsPending
  /\ ReadySlotsUnique
  /\ PendingCompletionWellFormed
  /\ CompletedSeqQueueBound
  /\ CompletedSeqIdBound
  /\ BoundedInflight
  /\ WriterSlotSafety
  /\ SeqIdAssignmentSafety
  /\ WaitEndRecordSafety

====
