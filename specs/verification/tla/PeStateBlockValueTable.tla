---- MODULE PeStateBlockValueTable ----
EXTENDS Naturals, Sequences
(***************************************************************************
 * Generated from src/d3d9/d3d9_pe_stateblock_value_table.inc.
 * Do not hand-edit. This is the repeated-value model/code row binding.
 ***************************************************************************)
StateBlockValueRows == <<
  [event |-> "CapturePreEffectFailed", action |-> "Preserve", preserveTrackedSet |-> TRUE, refreshSnapshot |-> FALSE, publishLive |-> FALSE, poison |-> FALSE],
  [event |-> "CaptureBackendFailed", action |-> "PoisonFailStop", preserveTrackedSet |-> TRUE, refreshSnapshot |-> FALSE, publishLive |-> FALSE, poison |-> TRUE],
  [event |-> "CaptureAccepted", action |-> "PublishCapture", preserveTrackedSet |-> TRUE, refreshSnapshot |-> TRUE, publishLive |-> FALSE, poison |-> FALSE],
  [event |-> "ApplyPreEffectFailed", action |-> "Preserve", preserveTrackedSet |-> TRUE, refreshSnapshot |-> FALSE, publishLive |-> FALSE, poison |-> FALSE],
  [event |-> "ApplyPrepared", action |-> "Preserve", preserveTrackedSet |-> TRUE, refreshSnapshot |-> FALSE, publishLive |-> FALSE, poison |-> FALSE],
  [event |-> "ApplyBackendFailed", action |-> "PoisonFailStop", preserveTrackedSet |-> TRUE, refreshSnapshot |-> FALSE, publishLive |-> FALSE, poison |-> TRUE],
  [event |-> "ApplyAccepted", action |-> "PublishApply", preserveTrackedSet |-> TRUE, refreshSnapshot |-> FALSE, publishLive |-> TRUE, poison |-> FALSE]
>>
StateBlockValueMatches(event, action, preserveTrackedSet,
                       refreshSnapshot, publishLive, poison) ==
  \E i \in 1..Len(StateBlockValueRows) :
    LET row == StateBlockValueRows[i] IN
      /\ row.event = event /\ row.action = action
      /\ row.preserveTrackedSet = preserveTrackedSet
      /\ row.refreshSnapshot = refreshSnapshot
      /\ row.publishLive = publishLive /\ row.poison = poison
====
