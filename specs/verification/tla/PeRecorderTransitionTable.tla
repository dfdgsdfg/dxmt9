---- MODULE PeRecorderTransitionTable ----
(***************************************************************************
 * Generated from src/d3d9/d3d9_pe_transition_table.inc.  Do not hand-edit.
 ***************************************************************************)
StateWriteTable == <<
  [phase |-> "Live", origin |-> "Any", liveEquals |-> "False", pendingContains |-> "Any", kind |-> "QueueDelta", writeLive |-> TRUE, writePending |-> TRUE, writeRecorded |-> FALSE, directOrderedCall |-> FALSE, semanticTransition |-> "True"],
  [phase |-> "Live", origin |-> "Any", liveEquals |-> "True", pendingContains |-> "False", kind |-> "NoOp", writeLive |-> FALSE, writePending |-> FALSE, writeRecorded |-> FALSE, directOrderedCall |-> FALSE, semanticTransition |-> "False"],
  [phase |-> "Live", origin |-> "Any", liveEquals |-> "True", pendingContains |-> "True", kind |-> "RetainPending", writeLive |-> FALSE, writePending |-> FALSE, writeRecorded |-> FALSE, directOrderedCall |-> FALSE, semanticTransition |-> "False"],
  [phase |-> "Recording", origin |-> "PriorValueOperation", liveEquals |-> "Any", pendingContains |-> "Any", kind |-> "ApplyPriorValueOnly", writeLive |-> TRUE, writePending |-> FALSE, writeRecorded |-> FALSE, directOrderedCall |-> TRUE, semanticTransition |-> "AnyNotEqualLive"],
  [phase |-> "Recording", origin |-> "ExplicitSet", liveEquals |-> "Any", pendingContains |-> "Any", kind |-> "RecordExplicit", writeLive |-> FALSE, writePending |-> FALSE, writeRecorded |-> TRUE, directOrderedCall |-> FALSE, semanticTransition |-> "True"]
>>
AppendTable == <<
  [phase |-> "Prepared", appendSucceeded |-> "True", explicitDiscard |-> "False", next |-> "Accepted", consumeRepresentedPending |-> TRUE, retainPreparedProjection |-> FALSE, recordDurable |-> TRUE, valid |-> TRUE],
  [phase |-> "Prepared", appendSucceeded |-> "False", explicitDiscard |-> "True", next |-> "Discarded", consumeRepresentedPending |-> FALSE, retainPreparedProjection |-> FALSE, recordDurable |-> FALSE, valid |-> TRUE],
  [phase |-> "Prepared", appendSucceeded |-> "False", explicitDiscard |-> "False", next |-> "Failed", consumeRepresentedPending |-> FALSE, retainPreparedProjection |-> TRUE, recordDurable |-> FALSE, valid |-> TRUE],
  [phase |-> "Prepared", appendSucceeded |-> "True", explicitDiscard |-> "True", next |-> "Prepared", consumeRepresentedPending |-> FALSE, retainPreparedProjection |-> TRUE, recordDurable |-> FALSE, valid |-> FALSE],
  [phase |-> "Accepted", appendSucceeded |-> "Any", explicitDiscard |-> "Any", next |-> "Accepted", consumeRepresentedPending |-> FALSE, retainPreparedProjection |-> FALSE, recordDurable |-> FALSE, valid |-> FALSE],
  [phase |-> "Failed", appendSucceeded |-> "Any", explicitDiscard |-> "Any", next |-> "Failed", consumeRepresentedPending |-> FALSE, retainPreparedProjection |-> FALSE, recordDurable |-> FALSE, valid |-> FALSE],
  [phase |-> "Discarded", appendSucceeded |-> "Any", explicitDiscard |-> "Any", next |-> "Discarded", consumeRepresentedPending |-> FALSE, retainPreparedProjection |-> FALSE, recordDurable |-> FALSE, valid |-> FALSE]
>>
====
