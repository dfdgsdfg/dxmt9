(*
 * Bounded identity-v2 SegmentSerial refinement.
 *
 * This is the small event-group model for R-BACK-2.76--2.81 and
 * R-HARN-REPLAY-7.25.  It deliberately separates the admission order
 * (three source members are published as one atomic group) from the
 * settlement proof (the source run is matched exactly and its tail seq is
 * checked, without assuming sourceOrdinal/seqId are numerically adjacent).
 *
 * The abort path is two phase: every newest member may be detached and held
 * Reclaiming before any owner is destroyed, then owners finish strictly in
 * reverse-tail order.  A successful pre-effect abort restores all high
 * waters and enables exactly one EventSerial fallback.  Wrong order and
 * partial/corrupt rollback are terminal fail-stop states.
 *
 * This model does not claim Metal, pixels, Objective-C ownership, or the
 * unbounded provider grammar.  Those remain native/provider obligations.
 *)

---- MODULE RenderTapeIdentitySegments ----
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS MaxRecords, MaxSeqId, MaxPassPieces, MaxPages

Sources == 1 .. 3
Records == 1 .. MaxRecords
SourceOrder == <<1, 2, 3>>
EventRecordOrder == <<1, 2, 3, 4, 5, 6>>
ExpectedSegmentRanges ==
  [s \in Sources |->
    IF s = 1 THEN <<1, 2>>
    ELSE IF s = 2 THEN <<3, 4>> ELSE <<5, 6>>]
EventSourceOrdinals == <<11, 17, 29>>
EventSeqIds == <<2, 5, 9>>
ReverseSourceOrder == <<3, 2, 1>>
PassPieceIds == 1 .. MaxPassPieces
PassPieceRanges ==
  [s \in Sources |->
    IF s = 1 THEN <<1, 2>>
    ELSE IF s = 2 THEN <<2, 3>> ELSE <<3>>]
ResourceDependentSeqs == <<5, 9>>
PageDependentSeqs == <<2, 9>>
Phases == {"Idle", "Writing", "Published", "Active", "Completed",
           "Detached", "Reclaiming", "FallbackEligible", "Fallback",
           "Poisoned"}

SeqSet(s) == {s[i] : i \in DOMAIN s}
StrictlyIncreasing(s) ==
  \A i \in DOMAIN s : i < Len(s) => s[i] < s[i + 1]
PrefixOf(s, full) == s = SubSeq(full, 1, Len(s))

VARIABLES
  phase,
  rawHighWater,
  sourceHighWater,
  seqHighWater,
  rawHighWaterBefore,
  sourceHighWaterBefore,
  seqHighWaterBefore,
  recordCoverage,
  passPieceId,
  readySegments,
  published,
  effects,
  settlementRegistered,
  completedSeqs,
  completedSources,
  eventSettlementCount,
  resourceWatermark,
  pageWatermark,
  tailWatermark,
  detachedSources,
  reclaimingSources,
  destroyedSources,
  reclaimedSources,
  abortSucceeded,
  fallbackCount

vars ==
  <<phase, rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, readySegments, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark,
    tailWatermark, detachedSources, reclaimingSources, destroyedSources,
    reclaimedSources, abortSucceeded, fallbackCount>>

PlanValid == passPieceId = [s \in Sources |-> 1]

Terminal == phase \in {"Completed", "Fallback", "Poisoned"}
ReclaimTerminal ==
  \/ phase \in {"Fallback", "Poisoned"}
  \/ (phase = "Completed" /\ reclaimedSources = SourceOrder)

Greatest(s) == CHOOSE x \in s : \A y \in s : x >= y

Init ==
  /\ phase = "Idle"
  /\ rawHighWater = 0
  /\ sourceHighWater = 10
  /\ seqHighWater = 1
  /\ rawHighWaterBefore = 0
  /\ sourceHighWaterBefore = 10
  /\ seqHighWaterBefore = 1
  /\ recordCoverage = <<>>
  /\ passPieceId \in {[s \in Sources |-> 1], [s \in Sources |-> 2]}
  /\ readySegments = {}
  /\ published = FALSE
  /\ effects = 0
  /\ settlementRegistered = FALSE
  /\ completedSeqs = <<>>
  /\ completedSources = <<>>
  /\ eventSettlementCount = 0
  /\ resourceWatermark = 0
  /\ pageWatermark = 0
  /\ tailWatermark = 0
  /\ detachedSources = <<>>
  /\ reclaimingSources = <<>>
  /\ destroyedSources = <<>>
  /\ reclaimedSources = <<>>
  /\ abortSucceeded = FALSE
  /\ fallbackCount = 0

BeginBatch ==
  /\ phase = "Idle"
  /\ PlanValid
  /\ rawHighWater' = 41
  /\ sourceHighWater' = EventSourceOrdinals[Len(EventSourceOrdinals)]
  /\ seqHighWater' = EventSeqIds[Len(EventSeqIds)]
  /\ phase' = "Writing"
  /\ UNCHANGED <<rawHighWaterBefore, sourceHighWaterBefore,
    seqHighWaterBefore, recordCoverage, passPieceId, readySegments,
    published, effects, settlementRegistered, completedSeqs,
    completedSources, eventSettlementCount, resourceWatermark,
    pageWatermark, tailWatermark, detachedSources, reclaimingSources,
    destroyedSources, reclaimedSources, abortSucceeded, fallbackCount>>

WriteRecord ==
  /\ phase = "Writing"
  /\ Len(recordCoverage) < MaxRecords
  /\ LET nextRecord == Len(recordCoverage) + 1 IN
       /\ nextRecord = EventRecordOrder[Len(recordCoverage) + 1]
       /\ recordCoverage' = Append(recordCoverage, nextRecord)
  /\ UNCHANGED <<phase, rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    passPieceId, readySegments, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark,
    tailWatermark, detachedSources, reclaimingSources, destroyedSources,
    reclaimedSources, abortSucceeded, fallbackCount>>

PublishGroup ==
  /\ phase = "Writing"
  /\ PlanValid
  /\ recordCoverage = EventRecordOrder
  /\ passPieceId = [s \in Sources |-> 1]
  /\ readySegments' = Sources
  /\ published' = TRUE
  /\ resourceWatermark' = Greatest(SeqSet(ResourceDependentSeqs))
  /\ pageWatermark' = Greatest(SeqSet(PageDependentSeqs))
  /\ tailWatermark' = Greatest(SeqSet(ResourceDependentSeqs) \cup
                           SeqSet(PageDependentSeqs))
  /\ phase' = "Published"
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, effects, settlementRegistered,
    completedSeqs, completedSources, eventSettlementCount,
    detachedSources, reclaimingSources, destroyedSources,
    reclaimedSources, abortSucceeded, fallbackCount>>

InvalidPlanEventSerial ==
  /\ phase = "Idle"
  /\ ~PlanValid
  /\ phase' = "Fallback"
  /\ fallbackCount' = 1
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, readySegments, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources, reclaimedSources,
    abortSucceeded>>

RegisterExactSettlement ==
  /\ phase = "Published"
  /\ published
  /\ ~settlementRegistered
  /\ Len(EventSourceOrdinals) = 3
  /\ EventSourceOrdinals[1] = 11
  /\ EventSeqIds[Len(EventSeqIds)] = tailWatermark
  /\ settlementRegistered' = TRUE
  /\ UNCHANGED <<phase, rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, readySegments, published, effects,
    completedSeqs, completedSources, eventSettlementCount,
    resourceWatermark, pageWatermark, tailWatermark, detachedSources,
    reclaimingSources, destroyedSources, reclaimedSources, abortSucceeded,
    fallbackCount>>

EncodeGroup ==
  /\ phase = "Published"
  /\ settlementRegistered
  /\ phase' = "Active"
  /\ effects' = 1
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, readySegments, published,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources,
    reclaimedSources, abortSucceeded, fallbackCount>>

CompleteSegment ==
  /\ phase = "Active"
  /\ settlementRegistered
  /\ Len(completedSeqs) < Len(EventSeqIds)
  /\ LET nextIndex == Len(completedSeqs) + 1 IN
       /\ EventSeqIds[nextIndex] \notin SeqSet(completedSeqs)
       /\ completedSeqs' = Append(completedSeqs, EventSeqIds[nextIndex])
       /\ completedSources' = Append(completedSources, SourceOrder[nextIndex])
       /\ phase' = IF nextIndex = Len(EventSeqIds)
                     THEN "Completed" ELSE "Active"
       /\ eventSettlementCount' = IF nextIndex = Len(EventSeqIds)
                                  THEN 1 ELSE eventSettlementCount
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, readySegments, published, effects,
    settlementRegistered, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources,
    reclaimedSources, abortSucceeded, fallbackCount>>

ReclaimSegment ==
  /\ phase = "Completed"
  /\ Len(completedSeqs) = Len(EventSeqIds)
  /\ eventSettlementCount = 1
  /\ Len(reclaimedSources) < Len(SourceOrder)
  /\ tailWatermark >= resourceWatermark
  /\ tailWatermark >= pageWatermark
  /\ LET nextIndex == Len(reclaimedSources) + 1 IN
       /\ completedSources[nextIndex] = SourceOrder[nextIndex]
       /\ reclaimedSources' = Append(reclaimedSources, SourceOrder[nextIndex])
  /\ UNCHANGED <<phase, rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, readySegments, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark,
    tailWatermark, detachedSources, reclaimingSources, destroyedSources,
    abortSucceeded, fallbackCount>>

StartAbort ==
  /\ phase = "Writing"
  /\ effects = 0
  /\ phase' = "Detached"
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, readySegments, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources,
    reclaimedSources, abortSucceeded, fallbackCount>>

DetachNewest ==
  /\ phase = "Detached"
  /\ Len(detachedSources) < Len(SourceOrder)
  /\ LET nextIndex == Len(detachedSources) + 1 IN
       /\ ReverseSourceOrder[nextIndex] \notin SeqSet(detachedSources)
       /\ detachedSources' = Append(detachedSources,
                                      ReverseSourceOrder[nextIndex])
       /\ reclaimingSources' = Append(reclaimingSources,
                                      ReverseSourceOrder[nextIndex])
       /\ phase' = IF nextIndex = Len(SourceOrder)
                     THEN "Reclaiming" ELSE "Detached"
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, readySegments, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    destroyedSources, reclaimedSources, abortSucceeded, fallbackCount>>

FinishDetachedOwner ==
  /\ phase = "Reclaiming"
  /\ Len(destroyedSources) < Len(SourceOrder)
  /\ LET nextIndex == Len(destroyedSources) + 1 IN
       /\ ReverseSourceOrder[nextIndex] = reclaimingSources[nextIndex]
       /\ destroyedSources' = Append(destroyedSources,
                                      ReverseSourceOrder[nextIndex])
       /\ phase' = IF nextIndex = Len(SourceOrder)
                     THEN "FallbackEligible" ELSE "Reclaiming"
       /\ rawHighWater' = IF nextIndex = Len(SourceOrder)
                           THEN rawHighWaterBefore ELSE rawHighWater
       /\ sourceHighWater' = IF nextIndex = Len(SourceOrder)
                              THEN sourceHighWaterBefore ELSE sourceHighWater
       /\ seqHighWater' = IF nextIndex = Len(SourceOrder)
                           THEN seqHighWaterBefore ELSE seqHighWater
       /\ abortSucceeded' = IF nextIndex = Len(SourceOrder) THEN TRUE
                            ELSE abortSucceeded
  /\ readySegments' = {}
  /\ UNCHANGED <<rawHighWaterBefore, sourceHighWaterBefore,
    seqHighWaterBefore, recordCoverage, passPieceId, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, reclaimedSources, fallbackCount>>

FallbackOnce ==
  /\ phase = "FallbackEligible"
  /\ abortSucceeded
  /\ effects = 0
  /\ fallbackCount = 0
  /\ phase' = "Fallback"
  /\ fallbackCount' = 1
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, readySegments, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources,
    reclaimedSources, abortSucceeded>>

PoisonWrongDetach ==
  /\ phase = "Detached"
  /\ Len(detachedSources) < Len(SourceOrder)
  /\ phase' = "Poisoned"
  /\ readySegments' = {}
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources, reclaimedSources,
    abortSucceeded, fallbackCount>>

PoisonWrongFinish ==
  /\ phase = "Reclaiming"
  /\ Len(destroyedSources) < Len(SourceOrder)
  /\ phase' = "Poisoned"
  /\ readySegments' = {}
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources, reclaimedSources,
    abortSucceeded, fallbackCount>>

PoisonPartialAbort ==
  /\ phase \in {"Detached", "Reclaiming"}
  /\ Len(destroyedSources) < Len(SourceOrder)
  /\ phase' = "Poisoned"
  /\ readySegments' = {}
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources, reclaimedSources,
    abortSucceeded, fallbackCount>>

PreEffectPassMismatchFallback ==
  /\ phase = "Writing"
  /\ phase' = "Detached"
  /\ passPieceId' = [passPieceId EXCEPT ![2] = 2]
  /\ readySegments' = {}
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, published, effects, settlementRegistered,
    completedSeqs, completedSources, eventSettlementCount,
    resourceWatermark, pageWatermark, tailWatermark, detachedSources,
    reclaimingSources, destroyedSources, reclaimedSources, abortSucceeded,
    fallbackCount>>

PreEffectPartitionMismatchFallback ==
  /\ phase = "Writing"
  /\ effects = 0
  /\ phase' = "Detached"
  /\ readySegments' = {}
  \* Gap/overlap/duplicate range rejection is pre-publication.  The concrete
  \* range predicate is bound by RecordPartition and native capture tests.
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources, reclaimedSources,
    abortSucceeded, fallbackCount>>

PoisonAfterEffect ==
  /\ phase = "Active"
  /\ effects = 1
  /\ phase' = "Poisoned"
  /\ readySegments' = {}
  /\ UNCHANGED <<rawHighWater, sourceHighWater, seqHighWater,
    rawHighWaterBefore, sourceHighWaterBefore, seqHighWaterBefore,
    recordCoverage, passPieceId, published, effects,
    settlementRegistered, completedSeqs, completedSources,
    eventSettlementCount, resourceWatermark, pageWatermark, tailWatermark,
    detachedSources, reclaimingSources, destroyedSources, reclaimedSources,
    abortSucceeded, fallbackCount>>

Next ==
  \/ BeginBatch
  \/ InvalidPlanEventSerial
  \/ WriteRecord
  \/ PublishGroup
  \/ RegisterExactSettlement
  \/ EncodeGroup
  \/ CompleteSegment
  \/ ReclaimSegment
  \/ StartAbort
  \/ DetachNewest
  \/ FinishDetachedOwner
  \/ FallbackOnce
  \/ PoisonWrongDetach
  \/ PoisonWrongFinish
  \/ PoisonPartialAbort
  \/ PreEffectPassMismatchFallback
  \/ PreEffectPartitionMismatchFallback
  \/ PoisonAfterEffect

TypeOK ==
  /\ phase \in Phases
  /\ rawHighWater \in Nat
  /\ sourceHighWater \in Nat
  /\ seqHighWater \in Nat
  /\ rawHighWaterBefore \in Nat
  /\ sourceHighWaterBefore \in Nat
  /\ seqHighWaterBefore \in Nat
  /\ MaxRecords = 6
  /\ recordCoverage \in Seq(Records)
  /\ passPieceId \in [Sources -> PassPieceIds]
  /\ readySegments \subseteq Sources
  /\ published \in BOOLEAN
  /\ effects \in 0..1
  /\ settlementRegistered \in BOOLEAN
  /\ completedSeqs \in Seq(1..MaxSeqId)
  /\ completedSources \in Seq(Sources)
  /\ eventSettlementCount \in 0..1
  /\ resourceWatermark \in 0..MaxSeqId
  /\ pageWatermark \in 0..MaxSeqId
  /\ tailWatermark \in 0..MaxSeqId
  /\ detachedSources \in Seq(Sources)
  /\ reclaimingSources \in Seq(Sources)
  /\ destroyedSources \in Seq(Sources)
  /\ reclaimedSources \in Seq(Sources)
  /\ abortSucceeded \in BOOLEAN
  /\ fallbackCount \in 0..1

RecordPartition ==
  /\ Cardinality(SeqSet(recordCoverage)) = Len(recordCoverage)
  /\ PrefixOf(recordCoverage, EventRecordOrder)
  /\ (phase # "Poisoned" =>
       /\ UNION {SeqSet(ExpectedSegmentRanges[s]) : s \in Sources} = Records
       /\ \A s, t \in Sources : s < t =>
            SeqSet(ExpectedSegmentRanges[s]) \cap
              SeqSet(ExpectedSegmentRanges[t]) = {}
       /\ ExpectedSegmentRanges[1] \o ExpectedSegmentRanges[2] \o
              ExpectedSegmentRanges[3] =
            EventRecordOrder)
  /\ (published => recordCoverage = EventRecordOrder)

PassPieceContinuity ==
  /\ (phase \in {"Published", "Active", "Completed"} =>
        /\ passPieceId = [s \in Sources |-> 1]
        /\ PassPieceRanges[1][Len(PassPieceRanges[1])] =
             PassPieceRanges[2][1]
        /\ PassPieceRanges[2][Len(PassPieceRanges[2])] =
             PassPieceRanges[3][1])
  /\ MaxPassPieces >= 2

FlattenedCompletion ==
  /\ Len(completedSeqs) = Len(completedSources)
  /\ StrictlyIncreasing(completedSeqs)
  /\ PrefixOf(completedSeqs, EventSeqIds)
  /\ PrefixOf(completedSources, SourceOrder)

AtomicReadyPublication ==
  /\ readySegments = {} \/ readySegments = Sources
  /\ (phase \in {"Writing", "Detached", "Reclaiming", "Fallback",
                 "FallbackEligible", "Poisoned"} => readySegments = {})
  /\ (readySegments # {} => published)

TwoPhaseAbortOrder ==
  /\ PrefixOf(detachedSources, ReverseSourceOrder)
  /\ reclaimingSources = detachedSources
  /\ PrefixOf(destroyedSources, ReverseSourceOrder)
  /\ Len(destroyedSources) <= Len(reclaimingSources)
  /\ (abortSucceeded =>
       /\ detachedSources = ReverseSourceOrder
       /\ reclaimingSources = ReverseSourceOrder
       /\ destroyedSources = ReverseSourceOrder
       /\ rawHighWater = rawHighWaterBefore
       /\ sourceHighWater = sourceHighWaterBefore
       /\ seqHighWater = seqHighWaterBefore)

SettlementExact ==
  /\ (settlementRegistered =>
       /\ published
       /\ Len(EventSourceOrdinals) = 3
       /\ EventSourceOrdinals[1] = 11
       /\ EventSeqIds[Len(EventSeqIds)] = tailWatermark)
  /\ eventSettlementCount <= 1
  /\ (eventSettlementCount = 1 =>
       /\ settlementRegistered
       /\ completedSeqs = EventSeqIds
       /\ completedSources = SourceOrder)

WatermarkSafety ==
  /\ (published =>
       /\ resourceWatermark = Greatest(SeqSet(ResourceDependentSeqs))
       /\ pageWatermark = Greatest(SeqSet(PageDependentSeqs))
       /\ tailWatermark = Greatest(SeqSet(ResourceDependentSeqs) \cup
                               SeqSet(PageDependentSeqs)))
  /\ (~published =>
       /\ resourceWatermark = 0
       /\ pageWatermark = 0
       /\ tailWatermark = 0)
  /\ resourceWatermark <= MaxSeqId
  /\ pageWatermark <= MaxSeqId
  /\ (Len(reclaimedSources) > 0 =>
       /\ completedSeqs = EventSeqIds
       /\ eventSettlementCount = 1
       /\ tailWatermark >= resourceWatermark
       /\ tailWatermark >= pageWatermark)

FallbackBeforeEffects ==
  /\ (fallbackCount = 1 =>
       /\ phase = "Fallback"
       /\ effects = 0
       /\ (abortSucceeded \/ ~PlanValid)
       /\ readySegments = {})
  /\ (phase = "Fallback" => fallbackCount = 1)

FailStopSafety ==
  /\ (phase = "Poisoned" =>
       /\ readySegments = {}
       /\ fallbackCount = 0)
  /\ (effects = 1 => phase \in {"Active", "Completed", "Poisoned"})

Inv ==
  /\ TypeOK
  /\ RecordPartition
  /\ PassPieceContinuity
  /\ FlattenedCompletion
  /\ AtomicReadyPublication
  /\ TwoPhaseAbortOrder
  /\ SettlementExact
  /\ WatermarkSafety
  /\ FallbackBeforeEffects
  /\ FailStopSafety

Fairness ==
  /\ WF_vars(BeginBatch)
  /\ WF_vars(InvalidPlanEventSerial)
  /\ WF_vars(WriteRecord)
  /\ WF_vars(PublishGroup)
  /\ WF_vars(RegisterExactSettlement)
  /\ WF_vars(EncodeGroup)
  /\ WF_vars(CompleteSegment)
  /\ WF_vars(ReclaimSegment)
  /\ WF_vars(StartAbort)
  /\ WF_vars(DetachNewest)
  /\ WF_vars(FinishDetachedOwner)
  /\ WF_vars(FallbackOnce)

Spec == Init /\ [][Next]_vars

FairSpec == Spec /\ Fairness

EventuallyTerminal == <>Terminal
EventuallySettledOrReclaimed == <>ReclaimTerminal

THEOREM Spec => []Inv
THEOREM FairSpec => EventuallyTerminal
====
