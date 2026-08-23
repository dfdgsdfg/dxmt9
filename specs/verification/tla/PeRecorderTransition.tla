---- MODULE PeRecorderTransition ----
(***************************************************************************
 * Bounded PE recorder transition algebra.  The pure operators
 * PlanRecorderStateWrite and SettleRecorderAppend are the exact model twins
 * of planRecorderStateWrite and settleRecorderAppend in
 * src/d3d9/d3d9_pe_transition_algebra.hpp.  QualifiedKeys models category +
 * key identity; preparation is non-reentrant by construction.
 ***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS Categories, Keys, Values,
          AppendDiscipline, MaxOperations, MaxDurable

ASSUME Categories # {} /\ Keys # {} /\ Values # {}
ASSUME AppendDiscipline \in {"Guarded", "ConsumeOnPrepare"}
ASSUME MaxOperations \in Nat /\ MaxOperations > 0
ASSUME MaxDurable \in Nat /\ MaxDurable > 0

QualifiedKeys == Categories \X Keys
AppendPhases == {"Idle", "Prepared", "Accepted", "Failed", "Discarded"}
RecorderPhases == {"Live", "Recording"}

DefaultValue == CHOOSE v \in Values : TRUE
OtherValue(v) == CHOOSE candidate \in Values : candidate # v

(***************************************************************************
 * Shared production/model vocabulary.
 ***************************************************************************)
PlanRecorderStateWrite(facts) ==
  LET equal == facts.liveContains /\ facts.liveEquals IN
  IF facts.phase = "Live"
  THEN IF equal
       THEN [kind |-> IF facts.pendingContains THEN "RetainPending" ELSE "NoOp",
             writeLive |-> FALSE, writePending |-> FALSE,
             writeRecorded |-> FALSE, directOrderedCall |-> FALSE,
             semanticTransition |-> FALSE]
       ELSE [kind |-> "QueueDelta", writeLive |-> TRUE,
             writePending |-> TRUE, writeRecorded |-> FALSE,
             directOrderedCall |-> FALSE, semanticTransition |-> TRUE]
  ELSE IF facts.origin = "PriorValueOperation"
       THEN [kind |-> "ApplyPriorValueOnly", writeLive |-> TRUE,
             writePending |-> FALSE, writeRecorded |-> FALSE,
             directOrderedCall |-> TRUE,
             semanticTransition |-> ~equal]
       ELSE [kind |-> "RecordExplicit",
             writeLive |-> FALSE, writePending |-> FALSE,
             writeRecorded |-> TRUE, directOrderedCall |-> FALSE,
             semanticTransition |-> TRUE]

SettleRecorderAppend(facts) ==
  IF facts.phase # "Prepared" \/
     (facts.appendSucceeded /\ facts.explicitDiscard)
  THEN [next |-> facts.phase, consumeRepresentedPending |-> FALSE,
        retainPreparedProjection |-> facts.phase = "Prepared",
        recordDurable |-> FALSE, valid |-> FALSE]
  ELSE IF facts.appendSucceeded
       THEN [next |-> "Accepted", consumeRepresentedPending |-> TRUE,
             retainPreparedProjection |-> FALSE, recordDurable |-> TRUE,
             valid |-> TRUE]
       ELSE IF facts.explicitDiscard
            THEN [next |-> "Discarded",
                  consumeRepresentedPending |-> FALSE,
                  retainPreparedProjection |-> FALSE,
                  recordDurable |-> FALSE, valid |-> TRUE]
            ELSE [next |-> "Failed", consumeRepresentedPending |-> FALSE,
                  retainPreparedProjection |-> TRUE,
                  recordDurable |-> FALSE, valid |-> TRUE]

VARIABLES phase, live, pendingDomain, pendingValues,
          recordedDomain, recordedValues,
          preparedKeys, preparedValues, preparedPendingSnapshot, appendPhase,
          durableRecords, durableReplayed, server, priorRead,
          operationOrdinal, capturedKeys, publishedKeys, publishedValues,
          lastConsumed, inputClosed, failureOrdinal, writeHistory

vars == <<phase, live, pendingDomain, pendingValues,
          recordedDomain, recordedValues,
          preparedKeys, preparedValues, preparedPendingSnapshot, appendPhase,
          durableRecords, durableReplayed, server, priorRead,
          operationOrdinal, capturedKeys, publishedKeys, publishedValues,
          lastConsumed, inputClosed, failureOrdinal, writeHistory>>

Init ==
  /\ phase = "Live"
  /\ live = [q \in QualifiedKeys |-> DefaultValue]
  /\ pendingDomain = {}
  /\ pendingValues = [q \in QualifiedKeys |-> DefaultValue]
  /\ recordedDomain = {}
  /\ recordedValues = [q \in QualifiedKeys |-> DefaultValue]
  /\ preparedKeys = {}
  /\ preparedValues = [q \in QualifiedKeys |-> DefaultValue]
  /\ preparedPendingSnapshot = {}
  /\ appendPhase = "Idle"
  /\ durableRecords = <<>>
  /\ durableReplayed = 0
  /\ server = live
  /\ priorRead = live
  /\ operationOrdinal = 0
  /\ capturedKeys = {}
  /\ publishedKeys = {}
  /\ publishedValues = live
  /\ lastConsumed = {}
  /\ inputClosed = FALSE
  /\ failureOrdinal = 0
  /\ writeHistory = <<>>

CanInput == ~inputClosed /\ operationOrdinal < MaxOperations /\
            appendPhase # "Prepared"

BeginRecording ==
  /\ CanInput /\ phase = "Live"
  /\ pendingDomain = {}
  /\ phase' = "Recording"
  /\ recordedDomain' = {}
  /\ operationOrdinal' = operationOrdinal + 1
  /\ UNCHANGED <<live, pendingDomain, pendingValues, recordedValues,
                 preparedKeys, preparedValues,
                 preparedPendingSnapshot, appendPhase, durableRecords,
                 durableReplayed, server, priorRead, capturedKeys,
                 publishedKeys, publishedValues, lastConsumed, inputClosed,
                 failureOrdinal, writeHistory>>

ExplicitSet(q, value) ==
  /\ CanInput /\ q \in QualifiedKeys /\ value \in Values
  /\ LET facts == [phase |-> phase, origin |-> "ExplicitSet",
                   liveContains |-> TRUE, liveEquals |-> live[q] = value,
                   pendingContains |-> q \in pendingDomain,
                   recordedContains |-> q \in recordedDomain]
         plan == PlanRecorderStateWrite(facts)
         nextLive == IF plan.writeLive THEN [live EXCEPT ![q] = value]
                     ELSE live
         nextPendingDomain == IF plan.writePending
                              THEN pendingDomain \cup {q} ELSE pendingDomain
         nextPendingValues == IF plan.writePending
                              THEN [pendingValues EXCEPT ![q] = value]
                              ELSE pendingValues
         nextRecordedDomain == IF plan.writeRecorded
                               THEN recordedDomain \cup {q} ELSE recordedDomain
         nextRecordedValues == IF plan.writeRecorded
                               THEN [recordedValues EXCEPT ![q] = value]
                               ELSE recordedValues
         nextServer == IF plan.directOrderedCall
                       THEN [server EXCEPT ![q] = value] ELSE server
         nextHistory == IF phase = "Recording"
                        THEN Append(writeHistory,
                             [kind |-> "ExplicitRecording", key |-> q,
                              value |-> value,
                              beforeLive |-> live, afterLive |-> nextLive,
                              beforePendingDomain |-> pendingDomain,
                              afterPendingDomain |-> nextPendingDomain,
                              beforePendingValues |-> pendingValues,
                              afterPendingValues |-> nextPendingValues,
                              beforeRecordedDomain |-> recordedDomain,
                              afterRecordedDomain |-> nextRecordedDomain,
                              beforeRecordedValues |-> recordedValues,
                              afterRecordedValues |-> nextRecordedValues,
                              beforeServer |-> server, afterServer |-> nextServer])
                        ELSE writeHistory
     IN /\ plan.semanticTransition =
              IF phase = "Live" THEN ~(live[q] = value) ELSE TRUE
        /\ live' = nextLive
        /\ pendingDomain' = nextPendingDomain
        /\ pendingValues' = nextPendingValues
        /\ recordedDomain' = nextRecordedDomain
        /\ recordedValues' = nextRecordedValues
        /\ server' = nextServer
        /\ writeHistory' = nextHistory
  /\ operationOrdinal' = operationOrdinal + 1
  /\ UNCHANGED <<phase, preparedKeys, preparedValues,
                 preparedPendingSnapshot, appendPhase, durableRecords,
                 durableReplayed, priorRead, capturedKeys, publishedKeys,
                 publishedValues, lastConsumed, inputClosed, failureOrdinal>>

PriorValueWrite(q, value) ==
  /\ CanInput /\ phase = "Recording"
  /\ q \notin pendingDomain
  /\ q \in QualifiedKeys /\ value \in Values
  /\ LET facts == [phase |-> phase, origin |-> "PriorValueOperation",
                   liveContains |-> TRUE, liveEquals |-> live[q] = value,
                   pendingContains |-> q \in pendingDomain,
                   recordedContains |-> q \in recordedDomain]
         plan == PlanRecorderStateWrite(facts)
         nextLive == [live EXCEPT ![q] = value]
         nextServer == [server EXCEPT ![q] = value]
         nextHistory == Append(writeHistory,
                         [kind |-> "PriorValueOperation", key |-> q,
                          value |-> value,
                          beforeLive |-> live, afterLive |-> nextLive,
                          beforePendingDomain |-> pendingDomain,
                          afterPendingDomain |-> pendingDomain,
                          beforePendingValues |-> pendingValues,
                          afterPendingValues |-> pendingValues,
                          beforeRecordedDomain |-> recordedDomain,
                          afterRecordedDomain |-> recordedDomain,
                          beforeRecordedValues |-> recordedValues,
                          afterRecordedValues |-> recordedValues,
                          beforeServer |-> server, afterServer |-> nextServer])
     IN /\ plan.writeLive /\ ~plan.writePending /\ ~plan.writeRecorded
        /\ plan.directOrderedCall
        /\ plan.semanticTransition = ~(live[q] = value)
        /\ live' = nextLive
        /\ server' = nextServer
        /\ writeHistory' = nextHistory
  /\ priorRead' = [priorRead EXCEPT ![q] = live[q]]
  /\ operationOrdinal' = operationOrdinal + 1
  /\ UNCHANGED <<phase, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedKeys, preparedValues,
                 preparedPendingSnapshot, appendPhase, durableRecords,
                 durableReplayed, capturedKeys, publishedKeys,
                 publishedValues, lastConsumed, inputClosed, failureOrdinal>>

EndRecording ==
  /\ CanInput /\ phase = "Recording"
  /\ phase' = "Live"
  /\ server' = server
  /\ publishedKeys' = recordedDomain
  /\ publishedValues' = recordedValues
  /\ capturedKeys' = recordedDomain
  /\ operationOrdinal' = operationOrdinal + 1
  /\ UNCHANGED <<live, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedKeys,
                 preparedValues, preparedPendingSnapshot, appendPhase,
                 durableRecords, durableReplayed, priorRead, lastConsumed,
                 inputClosed, failureOrdinal, writeHistory>>

Prepare(keysToPrepare) ==
  /\ appendPhase \in {"Idle", "Failed", "Discarded"}
  /\ keysToPrepare \in SUBSET pendingDomain /\ keysToPrepare # {}
  /\ preparedKeys' = keysToPrepare
  /\ preparedValues' = [q \in QualifiedKeys |-> pendingValues[q]]
  /\ preparedPendingSnapshot' = pendingDomain
  /\ appendPhase' = "Prepared"
  /\ pendingDomain' = IF AppendDiscipline = "ConsumeOnPrepare"
                      THEN pendingDomain \ keysToPrepare ELSE pendingDomain
  /\ operationOrdinal' = IF operationOrdinal < MaxOperations
                          THEN operationOrdinal + 1 ELSE operationOrdinal
  /\ UNCHANGED <<phase, live, pendingValues, recordedDomain, recordedValues,
                 durableRecords,
                 durableReplayed, server, priorRead, capturedKeys,
                 publishedKeys, publishedValues, lastConsumed, inputClosed,
                 failureOrdinal, writeHistory>>

AcceptAppend ==
  /\ appendPhase = "Prepared" /\ Len(durableRecords) < MaxDurable
  /\ LET plan == SettleRecorderAppend(
        [phase |-> appendPhase, appendSucceeded |-> TRUE,
         explicitDiscard |-> FALSE])
     IN /\ appendPhase' = plan.next
        /\ pendingDomain' = pendingDomain \ preparedKeys
        /\ durableRecords' = Append(durableRecords,
             [keys |-> preparedKeys, values |-> preparedValues])
        /\ lastConsumed' = preparedKeys
  /\ UNCHANGED <<phase, live, pendingValues, recordedDomain, recordedValues,
                 preparedKeys, preparedValues,
                 preparedPendingSnapshot, durableReplayed, server, priorRead,
                 operationOrdinal, capturedKeys, publishedKeys,
                 publishedValues, inputClosed, failureOrdinal, writeHistory>>

FailAppend ==
  /\ appendPhase = "Prepared"
  /\ appendPhase' = SettleRecorderAppend(
       [phase |-> appendPhase, appendSucceeded |-> FALSE,
        explicitDiscard |-> FALSE]).next
  /\ lastConsumed' = {}
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedKeys,
                 preparedValues, preparedPendingSnapshot, durableRecords,
                 durableReplayed, server, priorRead, operationOrdinal,
                 capturedKeys, publishedKeys, publishedValues, inputClosed,
                 failureOrdinal, writeHistory>>

DiscardPrepared ==
  /\ appendPhase = "Prepared"
  /\ appendPhase' = SettleRecorderAppend(
       [phase |-> appendPhase, appendSucceeded |-> FALSE,
        explicitDiscard |-> TRUE]).next
  /\ lastConsumed' = {}
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedKeys,
                 preparedValues, preparedPendingSnapshot, durableRecords,
                 durableReplayed, server, priorRead, operationOrdinal,
                 capturedKeys, publishedKeys, publishedValues, inputClosed,
                 failureOrdinal, writeHistory>>

RetireAccepted ==
  /\ appendPhase = "Accepted"
  /\ appendPhase' = "Idle"
  /\ preparedKeys' = {}
  /\ lastConsumed' = {}
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedValues,
                 preparedPendingSnapshot, durableRecords, durableReplayed,
                 server, priorRead, operationOrdinal, capturedKeys,
                 publishedKeys, publishedValues, inputClosed, failureOrdinal,
                 writeHistory>>

ReplayAccepted ==
  /\ durableReplayed < Len(durableRecords)
  /\ LET record == durableRecords[durableReplayed + 1]
     IN server' = [q \in QualifiedKeys |->
          IF q \in record.keys THEN record.values[q] ELSE server[q]]
  /\ durableReplayed' = durableReplayed + 1
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedKeys,
                 preparedValues, preparedPendingSnapshot, appendPhase,
                 durableRecords, priorRead, operationOrdinal, capturedKeys,
                 publishedKeys, publishedValues, lastConsumed, inputClosed,
                 failureOrdinal, writeHistory>>

CaptureFixedSet ==
  /\ phase = "Live" /\ publishedKeys # {}
  /\ capturedKeys' = publishedKeys
  /\ publishedValues' = [q \in QualifiedKeys |->
       IF q \in publishedKeys THEN live[q] ELSE publishedValues[q]]
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedKeys,
                 preparedValues, preparedPendingSnapshot, appendPhase,
                 durableRecords, durableReplayed, server, priorRead,
                 operationOrdinal, publishedKeys, lastConsumed, inputClosed,
                 failureOrdinal, writeHistory>>

ApplyRecorded ==
  /\ phase = "Live" /\ capturedKeys = publishedKeys
  /\ live' = [q \in QualifiedKeys |->
       IF q \in publishedKeys THEN publishedValues[q] ELSE live[q]]
  /\ server' = [q \in QualifiedKeys |->
       IF q \in publishedKeys THEN publishedValues[q] ELSE server[q]]
  /\ UNCHANGED <<phase, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedKeys,
                 preparedValues, preparedPendingSnapshot, appendPhase,
                 durableRecords, durableReplayed, priorRead, operationOrdinal,
                 capturedKeys, publishedKeys, publishedValues, lastConsumed,
                 inputClosed, failureOrdinal, writeHistory>>

(***************************************************************************
 * Pre-effect Begin/End/Capture failures are stuttering semantic failures.
 * The production Apply continuation can fail after backend effects and is
 * deliberately outside this model; specs/d3d9/recorder/gap.md owns it.
 ****************************************************************************)
FailedPreEffectRecorderOperation ==
  /\ failureOrdinal < MaxOperations
  /\ failureOrdinal' = failureOrdinal + 1
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedKeys,
                 preparedValues, preparedPendingSnapshot, appendPhase,
                 durableRecords, durableReplayed, server, priorRead,
                 operationOrdinal, capturedKeys, publishedKeys,
                 publishedValues, lastConsumed, inputClosed, writeHistory>>

Finish ==
  /\ ~inputClosed
  /\ inputClosed' = TRUE
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, recordedDomain,
                 recordedValues, preparedKeys,
                 preparedValues, preparedPendingSnapshot, appendPhase,
                 durableRecords, durableReplayed, server, priorRead,
                 operationOrdinal, capturedKeys, publishedKeys,
                 publishedValues, lastConsumed, failureOrdinal, writeHistory>>

PrepareSome == \E keysToPrepare \in SUBSET pendingDomain :
                 Prepare(keysToPrepare)
SettlePrepared == AcceptAppend \/ FailAppend \/ DiscardPrepared

Next ==
  \/ BeginRecording
  \/ \E q \in QualifiedKeys, value \in Values : ExplicitSet(q, value)
  \/ \E q \in QualifiedKeys, value \in Values : PriorValueWrite(q, value)
  \/ EndRecording
  \/ PrepareSome
  \/ SettlePrepared
  \/ RetireAccepted
  \/ ReplayAccepted
  \/ CaptureFixedSet
  \/ ApplyRecorded
  \/ FailedPreEffectRecorderOperation
  \/ Finish

Spec == Init /\ [][Next]_vars
        /\ WF_vars(PrepareSome)
        /\ WF_vars(SettlePrepared)
        /\ WF_vars(RetireAccepted)
        /\ WF_vars(ReplayAccepted)

DurableKeys == UNION {durableRecords[i].keys : i \in 1..Len(durableRecords)}

WriteHistoryRecord ==
  [kind : {"ExplicitRecording", "PriorValueOperation"},
   key : QualifiedKeys,
   value : Values,
   beforeLive : [QualifiedKeys -> Values],
   afterLive : [QualifiedKeys -> Values],
   beforePendingDomain : SUBSET QualifiedKeys,
   afterPendingDomain : SUBSET QualifiedKeys,
   beforePendingValues : [QualifiedKeys -> Values],
   afterPendingValues : [QualifiedKeys -> Values],
   beforeRecordedDomain : SUBSET QualifiedKeys,
   afterRecordedDomain : SUBSET QualifiedKeys,
   beforeRecordedValues : [QualifiedKeys -> Values],
   afterRecordedValues : [QualifiedKeys -> Values],
   beforeServer : [QualifiedKeys -> Values],
   afterServer : [QualifiedKeys -> Values]]

TypeOK ==
  /\ phase \in RecorderPhases
  /\ live \in [QualifiedKeys -> Values]
  /\ pendingDomain \subseteq QualifiedKeys
  /\ pendingValues \in [QualifiedKeys -> Values]
  /\ recordedDomain \subseteq QualifiedKeys
  /\ recordedValues \in [QualifiedKeys -> Values]
  /\ preparedKeys \subseteq QualifiedKeys
  /\ preparedValues \in [QualifiedKeys -> Values]
  /\ preparedPendingSnapshot \subseteq QualifiedKeys
  /\ appendPhase \in AppendPhases
  /\ durableReplayed \in 0..Len(durableRecords)
  /\ Len(durableRecords) <= MaxDurable
  /\ server \in [QualifiedKeys -> Values]
  /\ priorRead \in [QualifiedKeys -> Values]
  /\ capturedKeys \subseteq QualifiedKeys
  /\ publishedKeys \subseteq QualifiedKeys
  /\ publishedValues \in [QualifiedKeys -> Values]
  /\ lastConsumed \subseteq QualifiedKeys
  /\ operationOrdinal \in 0..MaxOperations
  /\ failureOrdinal \in 0..MaxOperations
  /\ writeHistory \in Seq(WriteHistoryRecord)
  /\ Len(writeHistory) <= MaxOperations

LiveAuthoritative == \A q \in QualifiedKeys : live[q] \in Values
PendingLastWriteWins == \A q \in pendingDomain : pendingValues[q] \in Values
QualifiedKeysDoNotAlias ==
  \A c1, c2 \in Categories, k1, k2 \in Keys :
    <<c1, k1>> = <<c2, k2>> => c1 = c2 /\ k1 = k2
ExplicitRecordingTracksSameValue ==
  \A i \in 1..Len(writeHistory) :
    LET h == writeHistory[i] IN
      h.kind = "ExplicitRecording" =>
        /\ h.afterLive = h.beforeLive
        /\ h.afterPendingDomain = h.beforePendingDomain
        /\ h.afterPendingValues = h.beforePendingValues
        /\ h.afterServer = h.beforeServer
        /\ h.afterRecordedDomain = h.beforeRecordedDomain \cup {h.key}
        /\ h.afterRecordedValues =
             [h.beforeRecordedValues EXCEPT ![h.key] = h.value]
PriorValueDoesNotEnlargeRecorded ==
  \A i \in 1..Len(writeHistory) :
    LET h == writeHistory[i] IN
      h.kind = "PriorValueOperation" =>
        /\ h.afterRecordedDomain = h.beforeRecordedDomain
        /\ h.afterRecordedValues = h.beforeRecordedValues
        /\ h.afterPendingDomain = h.beforePendingDomain
        /\ h.afterPendingValues = h.beforePendingValues
        /\ h.afterLive = [h.beforeLive EXCEPT ![h.key] = h.value]
        /\ h.afterServer = [h.beforeServer EXCEPT ![h.key] = h.value]
CaptureDoesNotEnlargeTrackedSet == capturedKeys \subseteq publishedKeys
PreparedIsNonConsuming ==
  AppendDiscipline # "Guarded" \/ appendPhase # "Prepared" \/
    pendingDomain = preparedPendingSnapshot
OnlyAcceptedConsumes ==
  appendPhase # "Accepted" \/ lastConsumed = preparedKeys
AcceptedExactlyRepresented ==
  appendPhase # "Accepted" \/ lastConsumed = preparedKeys
FailedRetryStable ==
  AppendDiscipline # "Guarded" \/ appendPhase # "Failed" \/
    preparedKeys \subseteq pendingDomain
NoLostPending ==
  appendPhase # "Failed" \/
    preparedPendingSnapshot \subseteq pendingDomain \cup DurableKeys
DurableRecordsAreQualified ==
  \A i \in 1..durableReplayed :
    durableRecords[i].keys \subseteq QualifiedKeys

PreparedEventuallySettles ==
  [] (appendPhase = "Prepared" ~> appendPhase # "Prepared")
AcceptedEventuallyReplayed ==
  [] (durableReplayed < Len(durableRecords) ~>
      durableReplayed = Len(durableRecords))
InputStopEventuallySettledOrRetained ==
  [] (inputClosed ~>
      (pendingDomain = {} \/ appendPhase \in {"Failed", "Discarded"}))

====
