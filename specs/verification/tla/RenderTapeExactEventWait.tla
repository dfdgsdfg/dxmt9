---- MODULE RenderTapeExactEventWait ----
(******************************************************************************
 * Bounded completion-request bridge for two authenticated Render Tape events.
 *
 * Each accepted row is identified by the event ordinal, raw ordinal, build
 * generation, first source ordinal, and completion tail sequence.  The bridge
 * drains replay before lookup, looks up exactly the selected event row, waits
 * on a queue completion prefix, and acknowledges only that row.  Queue
 * completion is abstracted as a prefix frontier; this model does not model
 * Metal, resource bytes, callback bodies, or GPU progress.
 ******************************************************************************)

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS AllowWrongLatestLookup,
          AllowPrematureAcknowledgment

Events == 1 .. 2
EventSequence == <<1, 2>>

Earlier == [eventOrdinal       |-> 1,
            rawOrdinal         |-> 101,
            buildGeneration    |-> 7,
            firstSourceOrdinal |-> 11,
            tailSeqId           |-> 41,
            sourceCount        |-> 1]

NewerTail == [eventOrdinal       |-> 2,
              rawOrdinal         |-> 102,
              buildGeneration    |-> 8,
              firstSourceOrdinal |-> 12,
              tailSeqId           |-> 43,
              sourceCount        |-> 1]

AcceptedRows == <<Earlier, NewerTail>>
NoTicket == [eventOrdinal       |-> 0,
             rawOrdinal         |-> 0,
             buildGeneration    |-> 0,
             firstSourceOrdinal |-> 0,
             tailSeqId           |-> 0,
             sourceCount        |-> 0]

ForeignTicket == [eventOrdinal       |-> 99,
                  rawOrdinal         |-> 901,
                  buildGeneration    |-> 70,
                  firstSourceOrdinal |-> 911,
                  tailSeqId           |-> 941,
                  sourceCount        |-> 1]

VARIABLES phase,
          event,
          requested,
          selected,
          replayDrained,
          releaseFenced,
          completionPrefix,
          completionTail,
          lookupCount,
          waitAttempts,
          successfulWaits,
          acknowledged,
          ackHistory,
          failed,
          lastDisposition,
          commandCount

vars == <<phase, event, requested, selected, replayDrained, releaseFenced,
          completionPrefix,
          completionTail, lookupCount, waitAttempts, successfulWaits,
          acknowledged, ackHistory, failed, lastDisposition, commandCount>>

Core == [phase              |-> phase,
         event              |-> event,
         requested          |-> requested,
         selected           |-> selected,
         replayDrained      |-> replayDrained,
         releaseFenced      |-> releaseFenced,
         completionPrefix   |-> completionPrefix,
         completionTail     |-> completionTail,
         lookupCount        |-> lookupCount,
         waitAttempts       |-> waitAttempts,
         successfulWaits    |-> successfulWaits,
         acknowledged       |-> acknowledged,
         ackHistory         |-> ackHistory,
         failed             |-> failed,
         commandCount       |-> commandCount]

Init ==
  /\ phase = "Lookup"
  /\ event = 1
  /\ requested = Earlier
  /\ selected = NoTicket
  /\ replayDrained = FALSE
  /\ releaseFenced = FALSE
  /\ completionPrefix = 0
  /\ completionTail = 0
  /\ lookupCount = 0
  /\ waitAttempts = 0
  /\ successfulWaits = {}
  /\ acknowledged = {}
  /\ ackHistory = <<>>
  /\ failed = FALSE
  /\ lastDisposition = "Accepted"
  /\ commandCount = 0

DrainReplay ==
  /\ phase = "Lookup"
  /\ ~replayDrained
  /\ replayDrained' = TRUE
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, event, requested, selected, releaseFenced,
                  completionPrefix,
                  completionTail,
                  lookupCount, waitAttempts, successfulWaits, acknowledged,
                  ackHistory, failed, commandCount>>

LookupCurrent ==
  /\ phase = "Lookup"
  /\ replayDrained
  /\ event \in Events
  /\ requested = AcceptedRows[event]
  /\ selected' = AcceptedRows[event]
  /\ phase' = "Wait"
  /\ lookupCount' = lookupCount + 1
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<event, requested, replayDrained, releaseFenced,
                  completionPrefix,
                  completionTail,
                  waitAttempts, successfulWaits, acknowledged, ackHistory,
                  failed, commandCount>>

WrongLatestLookupBug ==
  /\ AllowWrongLatestLookup
  /\ phase = "Lookup"
  /\ replayDrained
  /\ event = 1
  /\ requested = Earlier
  /\ selected' = AcceptedRows[2]
  /\ phase' = "Wait"
  /\ lookupCount' = lookupCount + 1
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<event, requested, replayDrained, releaseFenced,
                  completionPrefix,
                  completionTail,
                  waitAttempts, successfulWaits, acknowledged, ackHistory,
                  failed, commandCount>>

QueueCompleteEarlier ==
  /\ ~failed
  /\ completionPrefix = 0
  /\ completionPrefix' = 1
  /\ completionTail' = Earlier.tailSeqId
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, event, requested, selected, replayDrained,
                  releaseFenced,
                  lookupCount,
                  waitAttempts, successfulWaits, acknowledged, ackHistory,
                  failed, commandCount>>

QueueCompleteNewer ==
  /\ ~failed
  /\ completionPrefix = 1
  /\ completionPrefix' = 2
  /\ completionTail' = NewerTail.tailSeqId
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, event, requested, selected, replayDrained,
                  releaseFenced,
                  lookupCount, waitAttempts, successfulWaits, acknowledged,
                  ackHistory, failed, commandCount>>

WaitSuccess ==
  /\ phase = "Wait"
  /\ ~failed
  /\ requested = AcceptedRows[event]
  /\ selected = AcceptedRows[event]
  /\ releaseFenced
  /\ completionPrefix >= event
  /\ completionTail >= selected.tailSeqId
  /\ phase' = "Ack"
  /\ waitAttempts' = waitAttempts + 1
  /\ successfulWaits' = successfulWaits \cup {event}
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<event, requested, selected, replayDrained, releaseFenced,
                  completionPrefix,
                  completionTail, lookupCount, acknowledged, ackHistory,
                  failed, commandCount>>

WaitFailure ==
  /\ phase = "Wait"
  /\ ~failed
  /\ requested = AcceptedRows[event]
  /\ selected = AcceptedRows[event]
  /\ releaseFenced
  /\ phase' = "Failed"
  /\ waitAttempts' = waitAttempts + 1
  /\ failed' = TRUE
  /\ releaseFenced' = FALSE
  /\ lastDisposition' = "WaitFailed"
  /\ UNCHANGED <<event, requested, selected, replayDrained, completionPrefix,
                  completionTail, lookupCount, successfulWaits,
                  acknowledged, ackHistory, commandCount>>

WaitThrows ==
  /\ phase = "Wait"
  /\ ~failed
  /\ requested = AcceptedRows[event]
  /\ selected = AcceptedRows[event]
  /\ releaseFenced
  /\ phase' = "Failed"
  /\ waitAttempts' = waitAttempts + 1
  /\ failed' = TRUE
  /\ releaseFenced' = FALSE
  /\ lastDisposition' = "WaitThrew"
  /\ UNCHANGED <<event, requested, selected, replayDrained, completionPrefix,
                  completionTail, lookupCount, successfulWaits,
                  acknowledged, ackHistory, commandCount>>

AckSuccess ==
  /\ phase = "Ack"
  /\ ~failed
  /\ requested = AcceptedRows[event]
  /\ selected = AcceptedRows[event]
  /\ event \in successfulWaits
  /\ event \notin acknowledged
  /\ acknowledged' = acknowledged \cup {event}
  /\ ackHistory' = Append(ackHistory, event)
  /\ event' = IF event = 1 THEN 2 ELSE event
  /\ phase' = IF event = 1 THEN "Lookup" ELSE "Done"
  /\ requested' = IF event = 1 THEN NewerTail ELSE requested
  /\ selected' = IF event = 1 THEN NoTicket ELSE selected
  /\ lastDisposition' = "Accepted"
  /\ releaseFenced' = FALSE
  /\ UNCHANGED <<replayDrained, completionPrefix, completionTail,
                  lookupCount, waitAttempts, successfulWaits, failed,
                  commandCount>>

PrematureAcknowledgmentBug ==
  /\ AllowPrematureAcknowledgment
  /\ phase = "Wait"
  /\ ~failed
  /\ requested = AcceptedRows[event]
  /\ selected = AcceptedRows[event]
  /\ acknowledged' = acknowledged \cup {event}
  /\ ackHistory' = Append(ackHistory, event)
  /\ event' = IF event = 1 THEN 2 ELSE event
  /\ phase' = IF event = 1 THEN "Lookup" ELSE "Done"
  /\ requested' = IF event = 1 THEN NewerTail ELSE requested
  /\ selected' = IF event = 1 THEN NoTicket ELSE selected
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<replayDrained, releaseFenced, completionPrefix,
                  completionTail,
                  lookupCount, waitAttempts, successfulWaits, failed,
                  commandCount>>

SubmitMissingTicket ==
  /\ phase = "Lookup"
  /\ requested = AcceptedRows[event]
  /\ requested' = NoTicket
  /\ lastDisposition' = "RequestMissing"
  /\ UNCHANGED <<phase, event, selected, replayDrained, releaseFenced,
                  completionPrefix, completionTail, lookupCount,
                  waitAttempts, successfulWaits, acknowledged, ackHistory,
                  failed, commandCount>>

SubmitForeignTicket ==
  /\ phase = "Lookup"
  /\ requested = AcceptedRows[event]
  /\ requested' = ForeignTicket
  /\ lastDisposition' = "RequestForeign"
  /\ UNCHANGED <<phase, event, selected, replayDrained, releaseFenced,
                  completionPrefix, completionTail, lookupCount,
                  waitAttempts, successfulWaits, acknowledged, ackHistory,
                  failed, commandCount>>

RejectMissingTicket ==
  /\ phase = "Lookup"
  /\ requested = NoTicket
  /\ selected = NoTicket
  /\ requested' = AcceptedRows[event]
  /\ phase' = "Lookup"
  /\ lastDisposition' = "MissingTicket"
  /\ UNCHANGED <<event, selected, replayDrained, releaseFenced,
                  completionPrefix,
                  completionTail, lookupCount, waitAttempts, successfulWaits,
                  acknowledged, ackHistory, failed, commandCount>>

RejectForeignTicket ==
  /\ phase = "Lookup"
  /\ requested = ForeignTicket
  /\ selected = NoTicket
  /\ requested' = AcceptedRows[event]
  /\ phase' = "Lookup"
  /\ lastDisposition' = "ForeignTicket"
  /\ UNCHANGED <<event, selected, replayDrained, releaseFenced,
                  completionPrefix,
                  completionTail, lookupCount, waitAttempts, successfulWaits,
                  acknowledged, ackHistory, failed, commandCount>>

RejectDuplicateAcknowledgment ==
  /\ phase = "Done"
  /\ acknowledged = Events
  /\ lastDisposition' = "DuplicateAcknowledgment"
  /\ UNCHANGED <<phase, event, requested, selected, replayDrained,
                  releaseFenced,
                  completionPrefix,
                  completionTail, lookupCount, waitAttempts, successfulWaits,
                  acknowledged, ackHistory, failed, commandCount>>

ReleaseFence ==
  /\ phase = "Wait"
  /\ ~failed
  /\ requested = AcceptedRows[event]
  /\ selected = AcceptedRows[event]
  /\ ~releaseFenced
  /\ releaseFenced' = TRUE
  /\ lastDisposition' = "Accepted"
  /\ UNCHANGED <<phase, event, requested, selected, replayDrained,
                  completionPrefix,
                  completionTail, lookupCount, waitAttempts, successfulWaits,
                  acknowledged, ackHistory, failed, commandCount>>

ReleaseFenceFailure ==
  /\ phase = "Wait"
  /\ ~failed
  /\ requested = AcceptedRows[event]
  /\ selected = AcceptedRows[event]
  /\ ~releaseFenced
  /\ phase' = "Failed"
  /\ failed' = TRUE
  /\ lastDisposition' = "FenceFailed"
  /\ UNCHANGED <<event, requested, selected, replayDrained, releaseFenced,
                  completionPrefix, completionTail, lookupCount,
                  waitAttempts, successfulWaits, acknowledged, ackHistory,
                  commandCount>>

ReleaseFenceThrows ==
  /\ phase = "Wait"
  /\ ~failed
  /\ requested = AcceptedRows[event]
  /\ selected = AcceptedRows[event]
  /\ ~releaseFenced
  /\ phase' = "Failed"
  /\ failed' = TRUE
  /\ lastDisposition' = "FenceThrew"
  /\ UNCHANGED <<event, requested, selected, replayDrained, releaseFenced,
                  completionPrefix, completionTail, lookupCount,
                  waitAttempts, successfulWaits, acknowledged, ackHistory,
                  commandCount>>

Next ==
  \/ DrainReplay
  \/ LookupCurrent
  \/ WrongLatestLookupBug
  \/ QueueCompleteEarlier
  \/ QueueCompleteNewer
  \/ ReleaseFence
  \/ ReleaseFenceFailure
  \/ ReleaseFenceThrows
  \/ WaitSuccess
  \/ WaitFailure
  \/ WaitThrows
  \/ AckSuccess
  \/ PrematureAcknowledgmentBug
  \/ SubmitMissingTicket
  \/ SubmitForeignTicket
  \/ RejectMissingTicket
  \/ RejectForeignTicket
  \/ RejectDuplicateAcknowledgment

Spec == Init /\ [][Next]_vars

(* Conditional progress for two already accepted, valid requests. Malformed
 * request injection belongs to the safety spec above; it must not starve a
 * valid request in this environment. Completion of the parked queue prefix
 * is possible only after a release fence. Weak fairness here assumes that
 * enabled bridge/queue work eventually runs; it is not a driver guarantee.
 * All non-stuttering actions advance finite frontiers or terminate in failure,
 * so fairness of the combined action is sufficient in this bounded protocol.
 *)
ProgressWithoutFenceNext ==
  \/ DrainReplay
  \/ LookupCurrent
  \/ (releaseFenced /\ (QueueCompleteEarlier \/ QueueCompleteNewer))
  \/ WaitSuccess
  \/ WaitFailure
  \/ WaitThrows
  \/ AckSuccess

ProgressNext ==
  \/ ProgressWithoutFenceNext
  \/ ReleaseFence
  \/ ReleaseFenceFailure
  \/ ReleaseFenceThrows

ProgressSpec ==
  Init /\ [][ProgressNext]_vars /\ WF_vars(ProgressNext)

(* Removing the explicit release leaves an authenticated request parked with
 * no enabled progress action. Safety still holds, but eventual return fails.
 *)
MissingFenceProgressSpec ==
  Init /\ [][ProgressWithoutFenceNext]_vars
       /\ WF_vars(ProgressWithoutFenceNext)

EventuallyReturned == <>(phase = "Done" \/ phase = "Failed")

TypeOK ==
  /\ phase \in {"Lookup", "Wait", "Ack", "Done", "Failed"}
  /\ event \in Events
  /\ requested \in {NoTicket, ForeignTicket, Earlier, NewerTail}
  /\ selected \in {NoTicket, Earlier, NewerTail}
  /\ replayDrained \in BOOLEAN
  /\ releaseFenced \in BOOLEAN
  /\ completionPrefix \in 0..2
  /\ completionTail \in {0, Earlier.tailSeqId, NewerTail.tailSeqId}
  /\ lookupCount \in Nat
  /\ waitAttempts \in Nat
  /\ successfulWaits \subseteq Events
  /\ acknowledged \subseteq Events
  /\ ackHistory \in Seq(Events)
  /\ failed \in BOOLEAN
  /\ lastDisposition \in {"Accepted", "RequestMissing", "RequestForeign",
                            "MissingTicket", "ForeignTicket",
                            "WaitFailed", "WaitThrew",
                            "FenceFailed", "FenceThrew",
                            "DuplicateAcknowledgment"}
  /\ commandCount \in Nat

AcceptedRowsExact ==
  AcceptedRows = <<Earlier, NewerTail>>

SelectedIdentityExact ==
  selected = NoTicket \/ selected = AcceptedRows[event]

ReplayDrainedBeforeLookup ==
  lookupCount = 0 \/ replayDrained

WaitOnlyAfterExactLookup ==
  phase \in {"Wait", "Ack", "Done"} =>
    /\ requested = AcceptedRows[event]
    /\ selected = AcceptedRows[event]

ReleaseFenceOnlyAfterExactLookup ==
  releaseFenced => phase \in {"Wait", "Ack"} /\
    requested = AcceptedRows[event] /\ selected = AcceptedRows[event]

InvalidRequestNeverWaits ==
  requested \in {NoTicket, ForeignTicket} =>
    /\ phase = "Lookup"
    /\ selected = NoTicket
    /\ ~releaseFenced

WaitConfirmsCompletionPrefix ==
  phase = "Ack" =>
    /\ event \in successfulWaits
    /\ releaseFenced
    /\ completionPrefix >= event
    /\ completionTail >= selected.tailSeqId

NoAckWithoutSuccessfulWait ==
  acknowledged \subseteq successfulWaits

AcknowledgmentsAreSelectedPrefix ==
  Len(ackHistory) = 0 \/
    ackHistory = SubSeq(EventSequence, 1, Len(ackHistory))

NoDuplicateAcknowledgment ==
  Len(ackHistory) = Cardinality(acknowledged)

FailedWaitCannotAck ==
  failed => phase = "Failed" /\ acknowledged \subseteq successfulWaits

NoCommandFromCompletionBridge ==
  commandCount = 0

CompletionIsPrefix ==
  /\ completionPrefix = 0 => completionTail = 0
  /\ completionPrefix = 1 => completionTail = Earlier.tailSeqId
  /\ completionPrefix = 2 => completionTail = NewerTail.tailSeqId

Invariant ==
  /\ TypeOK
  /\ AcceptedRowsExact
  /\ SelectedIdentityExact
  /\ ReplayDrainedBeforeLookup
  /\ WaitOnlyAfterExactLookup
  /\ ReleaseFenceOnlyAfterExactLookup
  /\ InvalidRequestNeverWaits
  /\ WaitConfirmsCompletionPrefix
  /\ NoAckWithoutSuccessfulWait
  /\ AcknowledgmentsAreSelectedPrefix
  /\ NoDuplicateAcknowledgment
  /\ FailedWaitCannotAck
  /\ NoCommandFromCompletionBridge
  /\ CompletionIsPrefix

THEOREM Spec => []Invariant

=============================================================================
