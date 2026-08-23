---- MODULE PeRecorderTransition ----
(***************************************************************************
 * Bounded PE recorder transition algebra.  The pure operators
 * PlanRecorderStateWrite and SettleRecorderAppend are the exact model twins
 * of planRecorderStateWrite and settleRecorderAppend in
 * src/d3d9/d3d9_pe_transition_algebra.hpp.  QualifiedKeys models category +
 * key identity, while pending/durable witnesses are qualified
 * <key,value,ordinal> tokens; preparation is non-reentrant by construction.
 ***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences, TLC, PeRecorderTransitionTable

CONSTANTS Categories, Keys, Values,
          AppendDiscipline, TokenDiscipline, WitnessDiscipline,
          PriorPendingDiscipline,
          MaxOperations, MaxDurable

ASSUME Categories # {} /\ Keys # {} /\ Values # {}
ASSUME AppendDiscipline \in {"Guarded", "ConsumeOnPrepare"}
ASSUME TokenDiscipline \in {"Qualified", "KeyOnly"}
ASSUME WitnessDiscipline \in
       {"Exact", "ConsumeOnFailure", "UnderRepresentAccepted"}
ASSUME PriorPendingDiscipline \in {"Replace", "PreserveExisting"}
ASSUME MaxOperations \in Nat /\ MaxOperations > 0
ASSUME MaxDurable \in Nat /\ MaxDurable > 0

QualifiedKeys == Categories \X Keys
QualifiedTokens == QualifiedKeys \X Values \X (1..MaxOperations)
AppendPhases == {"Idle", "Prepared", "Accepted", "Failed", "Discarded"}
RecorderPhases == {"Live", "Recording"}

DefaultValue == CHOOSE v \in Values : TRUE
OtherValue(v) == CHOOSE candidate \in Values : candidate # v

(***************************************************************************
 * Shared production/model vocabulary.
 ***************************************************************************)
TruthMatches(expected, actual) ==
  expected = "Any" \/ (expected = "True") = actual

OriginMatches(expected, actual) == expected = "Any" \/ expected = actual

StateWriteRowMatches(row, facts, equal) ==
  /\ row.phase = facts.phase
  /\ OriginMatches(row.origin, facts.origin)
  /\ TruthMatches(row.liveEquals, equal)
  /\ TruthMatches(row.pendingContains, facts.pendingContains)

PlanRecorderStateWrite(facts) ==
  LET equal == facts.liveContains /\ facts.liveEquals
      rowIndex == CHOOSE i \in 1..Len(StateWriteTable) :
                    StateWriteRowMatches(StateWriteTable[i], facts, equal)
      row == StateWriteTable[rowIndex]
  IN [kind |-> row.kind,
      writeLive |-> row.writeLive,
      writePending |-> row.writePending,
      writeRecorded |-> row.writeRecorded,
      directOrderedCall |-> row.directOrderedCall,
      semanticTransition |->
        IF row.semanticTransition = "AnyNotEqualLive"
        THEN ~equal
        ELSE row.semanticTransition = "True"]

AppendRowMatches(row, facts) ==
  /\ row.phase = facts.phase
  /\ TruthMatches(row.appendSucceeded, facts.appendSucceeded)
  /\ TruthMatches(row.explicitDiscard, facts.explicitDiscard)

SettleRecorderAppend(facts) ==
  LET rowIndex == CHOOSE i \in 1..Len(AppendTable) :
                    AppendRowMatches(AppendTable[i], facts)
      row == AppendTable[rowIndex]
  IN row

VARIABLES phase, live, pendingDomain, pendingValues, pendingOrdinals,
          recordedDomain, recordedValues,
          preparedKeys, preparedValues, preparedOrdinals,
          preparedPendingSnapshot, appendPhase,
          durableRecords, durableReplayed, server, priorRead,
          operationOrdinal, capturedKeys, publishedKeys, publishedValues,
          lastConsumed, inputClosed, failureOrdinal, writeHistory

vars == <<phase, live, pendingDomain, pendingValues, pendingOrdinals,
          recordedDomain, recordedValues,
          preparedKeys, preparedValues, preparedOrdinals,
          preparedPendingSnapshot, appendPhase,
          durableRecords, durableReplayed, server, priorRead,
          operationOrdinal, capturedKeys, publishedKeys, publishedValues,
          lastConsumed, inputClosed, failureOrdinal, writeHistory>>

Init ==
  /\ phase = "Live"
  /\ live = [q \in QualifiedKeys |-> DefaultValue]
  /\ pendingDomain = {}
  /\ pendingValues = [q \in QualifiedKeys |-> DefaultValue]
  /\ pendingOrdinals = [q \in QualifiedKeys |-> 0]
  /\ recordedDomain = {}
  /\ recordedValues = [q \in QualifiedKeys |-> DefaultValue]
  /\ preparedKeys = {}
  /\ preparedValues = [q \in QualifiedKeys |-> DefaultValue]
  /\ preparedOrdinals = [q \in QualifiedKeys |-> 0]
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
            appendPhase \in {"Idle", "Discarded"}
\* Closing a state block consumes no write ordinal; keeping End enabled at the
\* bound preserves Begin -> Set -> End/Capture/Apply coverage at the two-input
\* bound.
CanEnd == ~inputClosed /\ appendPhase \in {"Idle", "Discarded"}

PendingTokens == {<<q, pendingValues[q], pendingOrdinals[q]>> :
                  q \in pendingDomain}
PreparedTokens == {<<q, preparedValues[q], preparedOrdinals[q]>> :
                   q \in preparedKeys}
DurableTokens == UNION {durableRecords[i].tokens :
                        i \in 1..Len(durableRecords)}

BeginRecording ==
  /\ CanInput /\ phase = "Live"
  /\ pendingDomain = {}
  /\ phase' = "Recording"
  /\ recordedDomain' = {}
  /\ operationOrdinal' = operationOrdinal + 1
  /\ UNCHANGED <<live, pendingDomain, pendingValues, pendingOrdinals,
                 recordedValues, preparedKeys, preparedValues,
                 preparedOrdinals,
                 preparedPendingSnapshot, appendPhase, durableRecords,
                 durableReplayed, server, priorRead, capturedKeys,
                 publishedKeys, publishedValues, lastConsumed, inputClosed,
                 failureOrdinal, writeHistory>>

ExplicitSet(q, value) ==
  /\ CanInput /\ q \in QualifiedKeys /\ value \in Values
  /\ LET facts == [phase |-> phase, origin |-> "ExplicitSet",
                   liveContains |-> TRUE, liveEquals |-> live[q] = value,
                   pendingContains |-> q \in pendingDomain]
         plan == PlanRecorderStateWrite(facts)
         nextLive == IF plan.writeLive THEN [live EXCEPT ![q] = value]
                     ELSE live
         nextPendingDomain == IF plan.writePending
                              THEN pendingDomain \cup {q} ELSE pendingDomain
         nextPendingValues == IF plan.writePending
                              THEN [pendingValues EXCEPT ![q] = value]
                              ELSE pendingValues
         nextPendingOrdinals == IF plan.writePending
                                THEN [pendingOrdinals EXCEPT
                                      ![q] = operationOrdinal + 1]
                                ELSE pendingOrdinals
         nextRecordedDomain == IF plan.writeRecorded
                               THEN recordedDomain \cup {q} ELSE recordedDomain
         nextRecordedValues == IF plan.writeRecorded
                               THEN [recordedValues EXCEPT ![q] = value]
                               ELSE recordedValues
         nextServer == IF plan.directOrderedCall
                       THEN [server EXCEPT ![q] = value] ELSE server
         nextHistory == IF phase = "Recording"
                        THEN Append(writeHistory,
                             [kind |-> "ExplicitRecording",
                              key |-> q,
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
        /\ pendingOrdinals' = nextPendingOrdinals
        /\ recordedDomain' = nextRecordedDomain
        /\ recordedValues' = nextRecordedValues
        /\ server' = nextServer
        /\ writeHistory' = nextHistory
  /\ operationOrdinal' = operationOrdinal + 1
  /\ UNCHANGED <<phase, preparedKeys, preparedValues, preparedOrdinals,
                 preparedPendingSnapshot, appendPhase, durableRecords,
                 durableReplayed, priorRead, capturedKeys, publishedKeys,
                 publishedValues, lastConsumed, inputClosed, failureOrdinal>>

PriorValueWrite(q, value) ==
  /\ CanInput /\ phase \in RecorderPhases
  /\ (phase = "Live" \/ q \notin pendingDomain)
  /\ q \in QualifiedKeys /\ value \in Values
  /\ LET facts == [phase |-> phase, origin |-> "PriorValueOperation",
                   liveContains |-> TRUE, liveEquals |-> live[q] = value,
                   pendingContains |-> q \in pendingDomain]
         plan == PlanRecorderStateWrite(facts)
         nextLive == IF plan.writeLive THEN [live EXCEPT ![q] = value]
                     ELSE live
         replacePending == plan.writePending /\
                           (PriorPendingDiscipline = "Replace" \/
                            q \notin pendingDomain)
         nextPendingDomain == IF replacePending
                              THEN pendingDomain \cup {q} ELSE pendingDomain
         nextPendingValues == IF replacePending
                              THEN [pendingValues EXCEPT ![q] = value]
                              ELSE pendingValues
         nextPendingOrdinals == IF replacePending
                                THEN [pendingOrdinals EXCEPT
                                      ![q] = operationOrdinal + 1]
                                ELSE pendingOrdinals
         nextServer == IF plan.directOrderedCall
                       THEN [server EXCEPT ![q] = value] ELSE server
         nextHistory == IF phase = "Recording"
                       THEN Append(writeHistory,
                         [kind |-> "PriorValueOperation",
                          key |-> q,
                          value |-> value,
                          beforeLive |-> live, afterLive |-> nextLive,
                          beforePendingDomain |-> pendingDomain,
                          afterPendingDomain |-> nextPendingDomain,
                          beforePendingValues |-> pendingValues,
                          afterPendingValues |-> nextPendingValues,
                          beforeRecordedDomain |-> recordedDomain,
                          afterRecordedDomain |-> recordedDomain,
                          beforeRecordedValues |-> recordedValues,
                          afterRecordedValues |-> recordedValues,
                          beforeServer |-> server, afterServer |-> nextServer])
                       ELSE writeHistory
     IN /\ plan.writeLive = (phase = "Recording" \/ ~(live[q] = value))
        /\ plan.writeRecorded = FALSE
        /\ plan.semanticTransition = ~(live[q] = value)
        /\ live' = nextLive
        /\ pendingDomain' = nextPendingDomain
        /\ pendingValues' = nextPendingValues
        /\ pendingOrdinals' = nextPendingOrdinals
        /\ server' = nextServer
        /\ writeHistory' = nextHistory
  /\ priorRead' = [priorRead EXCEPT ![q] = live[q]]
  /\ operationOrdinal' = operationOrdinal + 1
  /\ UNCHANGED <<phase, recordedDomain, recordedValues, preparedKeys,
                 preparedValues, preparedOrdinals,
                 preparedPendingSnapshot, appendPhase, durableRecords,
                 durableReplayed, capturedKeys, publishedKeys,
                 publishedValues, lastConsumed, inputClosed, failureOrdinal>>

EndRecording ==
  /\ CanEnd /\ phase = "Recording"
  /\ phase' = "Live"
  /\ server' = server
  /\ publishedKeys' = recordedDomain
  /\ publishedValues' = recordedValues
  /\ capturedKeys' = recordedDomain
  /\ UNCHANGED <<live, pendingDomain, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues, preparedKeys,
                 preparedValues, preparedOrdinals, preparedPendingSnapshot,
                 appendPhase,
                 durableRecords, durableReplayed, priorRead, operationOrdinal,
                 lastConsumed,
                 inputClosed, failureOrdinal, writeHistory>>

Prepare(keysToPrepare) ==
  /\ appendPhase \in {"Idle", "Failed", "Discarded"}
  /\ keysToPrepare \in SUBSET pendingDomain /\ keysToPrepare # {}
  /\ preparedKeys' = keysToPrepare
  /\ preparedValues' = [q \in QualifiedKeys |-> pendingValues[q]]
  /\ preparedOrdinals' = [q \in QualifiedKeys |-> pendingOrdinals[q]]
  /\ preparedPendingSnapshot' = PendingTokens
  /\ appendPhase' = "Prepared"
  /\ pendingDomain' = IF AppendDiscipline = "ConsumeOnPrepare"
                      THEN pendingDomain \ keysToPrepare ELSE pendingDomain
  /\ operationOrdinal' = operationOrdinal
  /\ UNCHANGED <<phase, live, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues,
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
        /\ plan.valid
        /\ plan.consumeRepresentedPending
        /\ ~plan.retainPreparedProjection
        /\ plan.recordDurable
        /\ pendingDomain' =
             IF plan.consumeRepresentedPending
             THEN pendingDomain \ preparedKeys ELSE pendingDomain
        /\ durableRecords' =
             IF plan.recordDurable
             THEN Append(durableRecords,
                    [keys |-> preparedKeys, values |-> preparedValues,
                     ordinals |-> preparedOrdinals,
                     tokens |-> IF TokenDiscipline = "KeyOnly"
                                 THEN {<<q, DefaultValue,
                                          preparedOrdinals[q]>> :
                                       q \in preparedKeys}
                                 ELSE PreparedTokens])
             ELSE durableRecords
        /\ lastConsumed' =
             IF WitnessDiscipline = "UnderRepresentAccepted"
             THEN {} ELSE PreparedTokens
  /\ UNCHANGED <<phase, live, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues,
                 preparedKeys, preparedValues, preparedOrdinals,
                 preparedPendingSnapshot, durableReplayed, server, priorRead,
                 operationOrdinal, capturedKeys, publishedKeys,
                 publishedValues, inputClosed, failureOrdinal, writeHistory>>

FailAppend ==
  /\ appendPhase = "Prepared"
  /\ LET plan == SettleRecorderAppend(
        [phase |-> appendPhase, appendSucceeded |-> FALSE,
         explicitDiscard |-> FALSE])
     IN /\ plan.valid
        /\ ~plan.consumeRepresentedPending
        /\ plan.retainPreparedProjection
        /\ ~plan.recordDurable
        /\ appendPhase' = plan.next
        /\ lastConsumed' =
             IF WitnessDiscipline = "ConsumeOnFailure"
             THEN PreparedTokens ELSE {}
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues, preparedKeys,
                 preparedValues, preparedOrdinals, preparedPendingSnapshot,
                 durableRecords,
                 durableReplayed, server, priorRead, operationOrdinal,
                 capturedKeys, publishedKeys, publishedValues, inputClosed,
                 failureOrdinal, writeHistory>>

DiscardPrepared ==
  /\ appendPhase = "Prepared"
  /\ LET plan == SettleRecorderAppend(
        [phase |-> appendPhase, appendSucceeded |-> FALSE,
         explicitDiscard |-> TRUE])
     IN /\ plan.valid
        /\ ~plan.consumeRepresentedPending
        /\ ~plan.retainPreparedProjection
        /\ ~plan.recordDurable
        /\ appendPhase' = plan.next
        /\ lastConsumed' = {}
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues, preparedKeys,
                 preparedValues, preparedOrdinals, preparedPendingSnapshot,
                 durableRecords,
                 durableReplayed, server, priorRead, operationOrdinal,
                 capturedKeys, publishedKeys, publishedValues, inputClosed,
                 failureOrdinal, writeHistory>>

RetireAccepted ==
  /\ appendPhase = "Accepted"
  /\ appendPhase' = "Idle"
  /\ preparedKeys' = {}
  /\ lastConsumed' = {}
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues, preparedValues,
                 preparedOrdinals,
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
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues, preparedKeys,
                 preparedValues, preparedOrdinals, preparedPendingSnapshot,
                 appendPhase,
                 durableRecords, priorRead, operationOrdinal, capturedKeys,
                 publishedKeys, publishedValues, lastConsumed, inputClosed,
                 failureOrdinal, writeHistory>>

CaptureFixedSet ==
  /\ phase = "Live" /\ publishedKeys # {}
  /\ capturedKeys' = publishedKeys
  /\ publishedValues' = [q \in QualifiedKeys |->
       IF q \in publishedKeys THEN live[q] ELSE publishedValues[q]]
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues, preparedKeys,
                 preparedValues, preparedOrdinals, preparedPendingSnapshot,
                 appendPhase,
                 durableRecords, durableReplayed, server, priorRead,
                 operationOrdinal, publishedKeys, lastConsumed, inputClosed,
                 failureOrdinal, writeHistory>>

ApplyRecorded ==
  /\ phase = "Live" /\ capturedKeys = publishedKeys
  /\ live' = [q \in QualifiedKeys |->
       IF q \in publishedKeys THEN publishedValues[q] ELSE live[q]]
  /\ server' = [q \in QualifiedKeys |->
       IF q \in publishedKeys THEN publishedValues[q] ELSE server[q]]
  /\ UNCHANGED <<phase, pendingDomain, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues, preparedKeys,
                 preparedValues, preparedOrdinals, preparedPendingSnapshot,
                 appendPhase,
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
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues, preparedKeys,
                 preparedValues, preparedOrdinals, preparedPendingSnapshot,
                 appendPhase,
                 durableRecords, durableReplayed, server, priorRead,
                 operationOrdinal, capturedKeys, publishedKeys,
                 publishedValues, lastConsumed, inputClosed, writeHistory>>

Finish ==
  /\ ~inputClosed
  /\ inputClosed' = TRUE
  /\ UNCHANGED <<phase, live, pendingDomain, pendingValues, pendingOrdinals,
                 recordedDomain, recordedValues, preparedKeys,
                 preparedValues, preparedOrdinals, preparedPendingSnapshot,
                 appendPhase,
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

DurableRecord ==
  [keys : SUBSET QualifiedKeys,
   values : [QualifiedKeys -> Values],
   ordinals : [QualifiedKeys -> 0..MaxOperations],
   tokens : SUBSET QualifiedTokens]

TypeOK ==
  /\ phase \in RecorderPhases
  /\ live \in [QualifiedKeys -> Values]
  /\ pendingDomain \subseteq QualifiedKeys
  /\ pendingValues \in [QualifiedKeys -> Values]
  /\ pendingOrdinals \in [QualifiedKeys -> 0..MaxOperations]
  /\ recordedDomain \subseteq QualifiedKeys
  /\ recordedValues \in [QualifiedKeys -> Values]
  /\ preparedKeys \subseteq QualifiedKeys
  /\ preparedValues \in [QualifiedKeys -> Values]
  /\ preparedOrdinals \in [QualifiedKeys -> 0..MaxOperations]
  /\ preparedPendingSnapshot \subseteq QualifiedTokens
  /\ appendPhase \in AppendPhases
  /\ durableReplayed \in 0..Len(durableRecords)
  /\ Len(durableRecords) <= MaxDurable
  /\ durableRecords \in Seq(DurableRecord)
  /\ server \in [QualifiedKeys -> Values]
  /\ priorRead \in [QualifiedKeys -> Values]
  /\ capturedKeys \subseteq QualifiedKeys
  /\ publishedKeys \subseteq QualifiedKeys
  /\ publishedValues \in [QualifiedKeys -> Values]
  /\ lastConsumed \subseteq QualifiedTokens
  /\ operationOrdinal \in 0..MaxOperations
  /\ failureOrdinal \in 0..MaxOperations
  /\ writeHistory \in Seq(WriteHistoryRecord)
  /\ Len(writeHistory) <= MaxOperations

LiveAuthoritative == \A q \in QualifiedKeys : live[q] \in Values
PendingLastWriteWins == \A q \in pendingDomain : pendingValues[q] \in Values
PendingTokensQualified == PendingTokens \subseteq QualifiedTokens
PendingMatchesLive ==
  \A q \in pendingDomain : pendingValues[q] = live[q]
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
    PendingTokens = preparedPendingSnapshot
OnlyAcceptedConsumes ==
  appendPhase = "Accepted" \/ lastConsumed = {}
AcceptedExactlyRepresented ==
  appendPhase # "Accepted" \/ lastConsumed = PreparedTokens
FailedRetryStable ==
  AppendDiscipline # "Guarded" \/ appendPhase # "Failed" \/
    PreparedTokens \subseteq PendingTokens
NoLostPending ==
  appendPhase # "Failed" \/
    preparedPendingSnapshot \subseteq PendingTokens \cup DurableTokens
DurableRecordsAreQualified ==
  \A i \in 1..Len(durableRecords) :
    /\ durableRecords[i].keys \subseteq QualifiedKeys
    /\ durableRecords[i].tokens \subseteq QualifiedTokens
DurableTokenMatchesPayload ==
  \A i \in 1..Len(durableRecords) :
    durableRecords[i].tokens =
      {<<q, durableRecords[i].values[q], durableRecords[i].ordinals[q]>> :
       q \in durableRecords[i].keys}

PreparedEventuallySettles ==
  [] (appendPhase = "Prepared" ~> appendPhase # "Prepared")
AcceptedEventuallyReplayed ==
  [] (durableReplayed < Len(durableRecords) ~>
      durableReplayed = Len(durableRecords))
InputStopEventuallySettledOrRetained ==
  [] (inputClosed ~>
      (pendingDomain = {} \/ appendPhase \in {"Failed", "Discarded"}))

====
