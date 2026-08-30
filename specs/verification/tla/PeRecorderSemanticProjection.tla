---- MODULE PeRecorderSemanticProjection ----
(***************************************************************************
 * Bounded all-family PE semantic-token binding. The generated producer set
 * comes from production C++; this model owns source/record/range/value/
 * identity conservation through retry, bridge poison, and capture settlement.
 ***************************************************************************)
EXTENDS Naturals, Sequences, TLC, PeRecorderSemanticProjectionTable

CONSTANT Mode

RecordFor(p) == SemanticProducerTable[
  CHOOSE i \in 1..Len(SemanticProducerTable) :
    SemanticProducerTable[i].producer = p]

VARIABLES phase, producer, sourceOrdinal, issuedProducer, issuedRecord,
          issuedSourceOrdinal, issuanceStep, boundProducer, boundCategory,
          boundRecord, semanticKey, boundSource, recordOrdinal, byteRange,
          exactValue, exactIdentity,
          accepted, issuanceConsumed, commandAccepted, captureDisposition,
          poisoned

vars == <<phase, producer, sourceOrdinal, issuedProducer, issuedRecord,
          issuedSourceOrdinal, issuanceStep, boundProducer, boundCategory,
          boundRecord, semanticKey, boundSource, recordOrdinal, byteRange,
          exactValue, exactIdentity,
          accepted, issuanceConsumed, commandAccepted, captureDisposition,
          poisoned>>

Init ==
  /\ phase = "Prepared"
  /\ producer = SemanticProducerTable[1].producer
  /\ sourceOrdinal = 1
  /\ issuedProducer = producer
  /\ issuedRecord = RecordFor(producer).record
  /\ issuedSourceOrdinal = sourceOrdinal
  /\ issuanceStep = IF Mode = "Aba" THEN 1 ELSE 0
  /\ boundProducer = ""
  /\ boundCategory = ""
  /\ boundRecord = ""
  /\ semanticKey = ""
  /\ boundSource = 0
  /\ recordOrdinal = 0
  /\ byteRange = 0
  /\ exactValue = 0
  /\ exactIdentity = FALSE
  /\ accepted = FALSE
  /\ issuanceConsumed = FALSE
  /\ commandAccepted = FALSE
  /\ captureDisposition = "None"
  /\ poisoned = FALSE

PreEffectReject ==
  /\ phase = "Prepared"
  /\ phase' = "Retained"
  /\ UNCHANGED <<producer, sourceOrdinal, issuedProducer, issuedRecord,
                 issuedSourceOrdinal, issuanceStep, boundProducer, boundCategory,
                 boundRecord, semanticKey, boundSource, recordOrdinal, byteRange, exactValue,
                 exactIdentity, accepted, issuanceConsumed, commandAccepted,
                 captureDisposition, poisoned>>

Retry ==
  /\ phase = "Retained"
  /\ phase' = "Prepared"
  /\ UNCHANGED <<producer, sourceOrdinal, issuedProducer, issuedRecord,
                 issuedSourceOrdinal, issuanceStep, boundProducer, boundCategory,
                 boundRecord, semanticKey, boundSource, recordOrdinal, byteRange, exactValue,
                 exactIdentity, accepted, issuanceConsumed, commandAccepted,
                 captureDisposition, poisoned>>

IssueAbaB ==
  /\ Mode = "Aba"
  /\ phase = "Prepared"
  /\ issuanceStep = 1
  /\ producer' = SemanticProducerTable[2].producer
  /\ sourceOrdinal' = 2
  /\ issuedProducer' = producer'
  /\ issuedRecord' = RecordFor(producer').record
  /\ issuedSourceOrdinal' = sourceOrdinal'
  /\ issuanceStep' = 2
  /\ UNCHANGED <<phase, boundProducer, boundCategory, boundRecord,
                 semanticKey, boundSource, recordOrdinal, byteRange,
                 exactValue, exactIdentity, accepted, issuanceConsumed,
                 commandAccepted,
                 captureDisposition, poisoned>>

IssueAbaA ==
  /\ Mode = "Aba"
  /\ phase = "Prepared"
  /\ issuanceStep = 2
  /\ producer' = SemanticProducerTable[1].producer
  /\ sourceOrdinal' = 3
  /\ issuedProducer' = producer'
  /\ issuedRecord' = RecordFor(producer').record
  /\ issuedSourceOrdinal' = sourceOrdinal'
  /\ issuanceStep' = 3
  /\ UNCHANGED <<phase, boundProducer, boundCategory, boundRecord,
                 semanticKey, boundSource, recordOrdinal, byteRange,
                 exactValue, exactIdentity, accepted, issuanceConsumed,
                 commandAccepted,
                 captureDisposition, poisoned>>

AppendAccepted ==
  /\ phase = "Prepared"
  /\ Mode # "Aba" \/ issuanceStep = 3
  /\ phase' = "Accepted"
  /\ boundProducer' = IF Mode = "WrongProducer" THEN "Present" ELSE producer
  /\ boundCategory' = RecordFor(producer).category
  /\ boundRecord' = IF Mode = "WrongRecord" THEN "UnknownRecord"
                     ELSE RecordFor(producer).record
  /\ semanticKey' = IF Mode = "WrongRecord" THEN "UnknownRecord"
                    ELSE RecordFor(producer).record
  /\ boundSource' = IF Mode = "MissingSource" THEN 0
                    ELSE IF Mode = "WrongSource" THEN 2
                    ELSE IF Mode = "Aba" THEN 1 ELSE sourceOrdinal
  /\ recordOrdinal' = IF Mode = "MissingRecord" THEN 0 ELSE 1
  /\ byteRange' = IF Mode = "MissingRange" THEN 0 ELSE 4
  /\ exactValue' = IF Mode = "WrongValue" THEN 8 ELSE 7
  /\ exactIdentity' = (Mode # "WrongIdentity")
  /\ accepted' = TRUE
  /\ issuanceConsumed' = TRUE
  /\ UNCHANGED <<producer, sourceOrdinal, issuedProducer, issuedRecord,
                 issuedSourceOrdinal, issuanceStep, commandAccepted,
                 captureDisposition, poisoned>>

BridgeAccepted ==
  /\ phase = "Accepted" /\ accepted
  /\ phase' = "CapturePending"
  /\ commandAccepted' = TRUE
  /\ UNCHANGED <<producer, sourceOrdinal, issuedProducer, issuedRecord,
                 issuedSourceOrdinal, issuanceStep, boundProducer, boundCategory,
                 boundRecord, semanticKey, boundSource, recordOrdinal, byteRange, exactValue,
                 exactIdentity, accepted, issuanceConsumed, captureDisposition,
                 poisoned>>

BridgeEffectUnknown ==
  /\ phase = "Accepted" /\ accepted
  /\ phase' = "Poisoned"
  /\ poisoned' = (Mode # "BridgeRetry")
  /\ accepted' = IF Mode = "BridgeRetry" THEN FALSE ELSE accepted
  /\ UNCHANGED <<producer, sourceOrdinal, issuedProducer, issuedRecord,
                 issuedSourceOrdinal, issuanceStep, boundProducer, boundCategory,
                 boundRecord, semanticKey, boundSource, recordOrdinal, byteRange, exactValue,
                 exactIdentity, issuanceConsumed, commandAccepted,
                 captureDisposition>>

CaptureSettle(disposition) ==
  /\ phase = "CapturePending" /\ commandAccepted
  /\ disposition \in {"Materialized", "Rejected", "Skipped"}
  /\ phase' = "Settled"
  /\ captureDisposition' = disposition
  /\ commandAccepted' = IF Mode = "CaptureRetract" THEN FALSE
                        ELSE commandAccepted
  /\ UNCHANGED <<producer, sourceOrdinal, issuedProducer, issuedRecord,
                 issuedSourceOrdinal, issuanceStep, boundProducer, boundCategory,
                 boundRecord, semanticKey, boundSource, recordOrdinal, byteRange, exactValue,
                 exactIdentity, accepted, issuanceConsumed, poisoned>>

Next == PreEffectReject \/ Retry \/ IssueAbaB \/ IssueAbaA \/ AppendAccepted \/ BridgeAccepted \/
        BridgeEffectUnknown \/ \E d \in {"Materialized", "Rejected", "Skipped"} :
          CaptureSettle(d)

TypeOK ==
  /\ phase \in {"Prepared", "Retained", "Accepted", "CapturePending",
                 "Settled", "Poisoned"}
  /\ producer \in SemanticProducers
  /\ sourceOrdinal \in 0..3
  /\ issuedProducer \in SemanticProducers
  /\ issuedRecord \in SemanticRecords
  /\ issuedSourceOrdinal \in 0..3
  /\ issuanceStep \in 0..3
  /\ boundProducer \in SemanticProducers \cup {""}
  /\ boundCategory \in SemanticCategories \cup {""}
  /\ boundRecord \in SemanticRecords \cup {"", "UnknownRecord"}
  /\ semanticKey \in SemanticRecords \cup {"", "UnknownRecord"}
  /\ boundSource \in 0..3
  /\ recordOrdinal \in 0..1
  /\ byteRange \in {0, 4}
  /\ exactValue \in {0, 7, 8}
  /\ exactIdentity \in BOOLEAN
  /\ accepted \in BOOLEAN
  /\ issuanceConsumed \in BOOLEAN
  /\ commandAccepted \in BOOLEAN
  /\ captureDisposition \in {"None", "Materialized", "Rejected", "Skipped"}
  /\ poisoned \in BOOLEAN

ExactProjection ==
  ~accepted \/
    /\ boundProducer = issuedProducer
    /\ boundCategory = RecordFor(issuedProducer).category
    /\ boundRecord = issuedRecord
    /\ semanticKey = issuedRecord
    /\ boundSource = issuedSourceOrdinal
    /\ issuedSourceOrdinal # 0
    /\ recordOrdinal # 0
    /\ byteRange # 0
    /\ exactValue = 7
    /\ exactIdentity
    /\ issuanceConsumed

RetryStable == phase # "Retained" \/ issuedSourceOrdinal = sourceOrdinal

CaptureAfterAccept ==
  captureDisposition = "None" \/ commandAccepted

BridgeEffectUnknownFailStop ==
  phase # "Poisoned" \/ (poisoned /\ accepted)

Spec == Init /\ [][Next]_vars

====
