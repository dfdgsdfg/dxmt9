---- MODULE PeRecorderScalarProjection ----
(***************************************************************************
 * Handwritten exact bounded refinement for the render/TSS/sampler scalar projection.
 * Pending values remain the semantic owner. When the default-off cold
 * observer is enabled, its projection carries category/key/index/value/source
 * ordinal and binds it to one record ordinal. This model does not claim that
 * the default production path retains a source ordinal.
 ***************************************************************************)
EXTENDS Naturals, Integers, Sequences, TLC

CONSTANT ExpectedCategory, ExpectedKey, ExpectedIndex,
         ExpectedValue, ExpectedSource, ExpectedRecord, Mode

Categories == {"RenderState", "TextureStageState", "SamplerState"}
ScalarKeys == {0, ExpectedKey, ExpectedKey + 1}
ScalarIndexes == {0, ExpectedIndex}
ScalarValues == {0, ExpectedValue, ExpectedValue + 1}
ScalarOrdinals == {0, ExpectedSource, ExpectedSource + 1,
                    ExpectedRecord, ExpectedRecord + 1}
BoundRecord == [category : Categories, key : ScalarKeys,
                index : ScalarIndexes, value : ScalarValues,
                source : ScalarOrdinals, record : ScalarOrdinals]

VARIABLES phase, pending, pendingCategory, pendingKey, pendingIndex,
          pendingValue, pendingSource,
          prepared, preparedCategory, preparedKey, preparedIndex,
          preparedValue, preparedSource,
          acceptedCount, bound, recordOrdinal,
          noTokenCount, noTokenBound, noTokenBoundSeq, noTokenWitness,
          noTokenPending, noTokenAcceptedCount, noTokenRecordOrdinal

vars == <<phase, pending, pendingCategory, pendingKey, pendingIndex,
          pendingValue, pendingSource, prepared, preparedCategory,
          preparedKey, preparedIndex, preparedValue, preparedSource,
          acceptedCount, bound, recordOrdinal, noTokenCount, noTokenBound,
          noTokenBoundSeq, noTokenWitness, noTokenPending, noTokenAcceptedCount,
          noTokenRecordOrdinal>>

Init ==
  /\ phase = "Idle"
  /\ pending = TRUE
  /\ pendingCategory = ExpectedCategory
  /\ pendingKey = ExpectedKey
  /\ pendingIndex = ExpectedIndex
  /\ pendingValue = ExpectedValue
  /\ pendingSource = ExpectedSource
  /\ prepared = FALSE
  /\ preparedCategory = ""
  /\ preparedKey = 0
  /\ preparedIndex = 0
  /\ preparedValue = 0
  /\ preparedSource = 0
  /\ acceptedCount = 0
  /\ bound = <<>>
  /\ recordOrdinal = 0
  /\ noTokenCount = 0
  /\ noTokenBound = 0
  /\ noTokenBoundSeq = <<>>
  /\ noTokenWitness = FALSE
  /\ noTokenPending = FALSE
  /\ noTokenAcceptedCount = 0
  /\ noTokenRecordOrdinal = 0

Prepare ==
  /\ phase = "Idle" /\ pending /\ ~prepared
  /\ phase' = "Prepared"
  /\ prepared' = TRUE
  /\ preparedCategory' = pendingCategory
  /\ preparedKey' = pendingKey
  /\ preparedIndex' = pendingIndex
  /\ preparedValue' = pendingValue
  /\ preparedSource' = pendingSource
  /\ UNCHANGED <<pending, pendingCategory, pendingKey, pendingIndex,
                 pendingValue, pendingSource, acceptedCount, bound,
                 recordOrdinal, noTokenCount, noTokenBound, noTokenBoundSeq,
                 noTokenWitness,
                 noTokenPending, noTokenAcceptedCount, noTokenRecordOrdinal>>

Retry ==
  /\ phase = "Prepared" /\ prepared
  /\ phase' = "Idle"
  /\ prepared' = FALSE
  /\ UNCHANGED <<pending, pendingCategory, pendingKey, pendingIndex,
                 pendingValue, pendingSource, preparedCategory,
                 preparedKey, preparedIndex, preparedValue, preparedSource,
                 acceptedCount, bound, recordOrdinal, noTokenCount,
                 noTokenBound, noTokenBoundSeq, noTokenWitness, noTokenPending,
                 noTokenAcceptedCount, noTokenRecordOrdinal>>

Discard ==
  /\ phase = "Prepared" /\ prepared
  /\ phase' = "Idle"
  /\ prepared' = FALSE
  /\ pending' = pending
  /\ UNCHANGED <<pendingCategory, pendingKey, pendingIndex, pendingValue,
                 pendingSource, preparedCategory, preparedKey, preparedIndex,
                 preparedValue, preparedSource, acceptedCount, bound,
                 recordOrdinal, noTokenCount, noTokenBound, noTokenBoundSeq,
                 noTokenWitness,
                 noTokenPending, noTokenAcceptedCount, noTokenRecordOrdinal>>

Accept ==
  /\ phase = "Prepared" /\ prepared
  /\ phase' = "Idle"
  /\ prepared' = FALSE
  /\ pending' = FALSE
  /\ acceptedCount' = acceptedCount + 1
  /\ recordOrdinal' = recordOrdinal + 1
  /\ bound' =
       IF Mode = "Missing" THEN <<>>
       ELSE IF Mode = "Duplicate" THEN
          <<[category |-> preparedCategory, key |-> preparedKey,
              index |-> preparedIndex, value |-> preparedValue,
              source |-> preparedSource, record |-> recordOrdinal + 1],
            [category |-> preparedCategory, key |-> preparedKey,
              index |-> preparedIndex, value |-> preparedValue,
              source |-> preparedSource, record |-> recordOrdinal + 1]>>
       ELSE IF Mode = "Value" THEN
          <<[category |-> preparedCategory, key |-> preparedKey,
              index |-> preparedIndex, value |-> preparedValue + 1,
              source |-> preparedSource, record |-> recordOrdinal + 1]>>
       ELSE IF Mode = "SourceOrdinal" THEN
          <<[category |-> preparedCategory, key |-> preparedKey,
              index |-> preparedIndex, value |-> preparedValue,
              source |-> preparedSource + 1, record |-> recordOrdinal + 1]>>
       ELSE IF Mode = "RecordOrdinal" THEN
          <<[category |-> preparedCategory, key |-> preparedKey,
              index |-> preparedIndex, value |-> preparedValue,
              source |-> preparedSource, record |-> recordOrdinal]>>
       ELSE IF Mode = "Category" THEN
          <<[category |-> "SamplerState", key |-> preparedKey,
              index |-> preparedIndex, value |-> preparedValue,
              source |-> preparedSource, record |-> recordOrdinal + 1]>>
       ELSE IF Mode = "Key" THEN
          <<[category |-> preparedCategory, key |-> preparedKey + 1,
              index |-> preparedIndex, value |-> preparedValue,
              source |-> preparedSource, record |-> recordOrdinal + 1]>>
       ELSE IF Mode = "Normal" THEN
          <<[category |-> preparedCategory, key |-> preparedKey,
              index |-> preparedIndex, value |-> preparedValue,
              source |-> preparedSource, record |-> recordOrdinal + 1]>>
       ELSE
          <<[category |-> preparedCategory, key |-> preparedKey,
              index |-> preparedIndex, value |-> preparedValue,
              source |-> preparedSource, record |-> recordOrdinal + 1]>>
  /\ UNCHANGED <<pendingCategory, pendingKey, pendingIndex, pendingValue,
                 pendingSource, preparedCategory, preparedKey, preparedIndex,
                 preparedValue, preparedSource, noTokenCount, noTokenBound,
                 noTokenBoundSeq,
                 noTokenWitness, noTokenPending, noTokenAcceptedCount,
                 noTokenRecordOrdinal>>

AcceptNoToken ==
  /\ phase = "Idle" /\ ~pending /\ ~prepared
  /\ noTokenCount = 0
  /\ phase' = "Idle"
  /\ noTokenCount' = noTokenCount + 1
  /\ noTokenBound' = Len(bound)
  /\ noTokenBoundSeq' = bound
  /\ noTokenWitness' = TRUE
  /\ noTokenPending' =
       IF Mode = "NoTokenMutation" THEN ~pending ELSE pending
  /\ noTokenAcceptedCount' = acceptedCount
  /\ noTokenRecordOrdinal' = recordOrdinal
  /\ UNCHANGED <<pending, pendingCategory, pendingKey, pendingIndex,
                 pendingValue, pendingSource, prepared, preparedCategory,
                 preparedKey, preparedIndex, preparedValue, preparedSource,
                 acceptedCount, bound, recordOrdinal>>

Next == Prepare \/ Retry \/ Discard \/ Accept \/ AcceptNoToken

TypeOK ==
  /\ phase \in {"Idle", "Prepared"}
  /\ pending \in BOOLEAN
  /\ pendingCategory \in Categories
  /\ pendingKey \in ScalarKeys
  /\ pendingIndex \in ScalarIndexes
  /\ pendingValue \in ScalarValues
  /\ pendingSource \in ScalarOrdinals
  /\ prepared \in BOOLEAN
  /\ preparedCategory \in Categories \cup {""}
  /\ preparedKey \in ScalarKeys
  /\ preparedIndex \in ScalarIndexes
  /\ preparedValue \in ScalarValues
  /\ preparedSource \in ScalarOrdinals
  /\ acceptedCount \in 0..1
  /\ recordOrdinal \in 0..1
  /\ noTokenCount \in 0..1
  /\ noTokenBound \in 0..2
  /\ noTokenBoundSeq \in Seq(BoundRecord)
  /\ Len(noTokenBoundSeq) <= 2
  /\ noTokenWitness \in BOOLEAN
  /\ noTokenPending \in BOOLEAN
  /\ noTokenAcceptedCount \in 0..1
  /\ noTokenRecordOrdinal \in 0..1
  /\ bound \in Seq(BoundRecord)
  /\ Len(bound) <= 2

ExactProjection ==
  /\ acceptedCount = 0 \/ Len(bound) = 1
  /\ \A i \in 1..Len(bound) :
       /\ bound[i].category = ExpectedCategory
       /\ bound[i].key = ExpectedKey
       /\ bound[i].index = ExpectedIndex
       /\ bound[i].value = ExpectedValue
       /\ bound[i].source = ExpectedSource
       /\ bound[i].record = ExpectedRecord

NoTokenIsExplicit ==
  /\ noTokenCount = 0 \/ noTokenBound = Len(bound)
  /\ ~noTokenWitness \/
       /\ pending = noTokenPending
       /\ acceptedCount = noTokenAcceptedCount
       /\ bound = noTokenBoundSeq
       /\ recordOrdinal = noTokenRecordOrdinal

Spec == Init /\ [][Next]_vars

====
