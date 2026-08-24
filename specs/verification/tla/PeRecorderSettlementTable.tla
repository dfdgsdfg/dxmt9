---- MODULE PeRecorderSettlementTable ----
EXTENDS Naturals, Sequences
(***************************************************************************
 * Generated from src/d3d9/d3d9_pe_recorder_settlement_table.inc.
 * Do not hand-edit. This is the composed model/code row binding.
 ***************************************************************************)
SettlementRows == <<
  [point |-> "CapacityPre", result |-> "Succeeded", action |-> "Continue", acceptedRecord |-> FALSE, retryable |-> FALSE, rollbackEmitter |-> FALSE, poison |-> FALSE],
  [point |-> "CapacityPre", result |-> "FailedPreEffect", action |-> "RetryUnattempted", acceptedRecord |-> FALSE, retryable |-> TRUE, rollbackEmitter |-> FALSE, poison |-> FALSE],
  [point |-> "CapacityPre", result |-> "FailedEffectUnknown", action |-> "PoisonFailStop", acceptedRecord |-> FALSE, retryable |-> FALSE, rollbackEmitter |-> FALSE, poison |-> TRUE],
  [point |-> "Emitter", result |-> "Succeeded", action |-> "KeepAccepted", acceptedRecord |-> TRUE, retryable |-> FALSE, rollbackEmitter |-> FALSE, poison |-> FALSE],
  [point |-> "Emitter", result |-> "FailedPreEffect", action |-> "RollbackEmitter", acceptedRecord |-> FALSE, retryable |-> TRUE, rollbackEmitter |-> TRUE, poison |-> FALSE],
  [point |-> "CapacityPost", result |-> "Succeeded", action |-> "Continue", acceptedRecord |-> TRUE, retryable |-> FALSE, rollbackEmitter |-> FALSE, poison |-> FALSE],
  [point |-> "CapacityPost", result |-> "FailedPreEffect", action |-> "KeepAccepted", acceptedRecord |-> TRUE, retryable |-> TRUE, rollbackEmitter |-> FALSE, poison |-> FALSE],
  [point |-> "CapacityPost", result |-> "FailedEffectUnknown", action |-> "PoisonFailStop", acceptedRecord |-> TRUE, retryable |-> FALSE, rollbackEmitter |-> FALSE, poison |-> TRUE],
  [point |-> "Bridge", result |-> "Succeeded", action |-> "Continue", acceptedRecord |-> TRUE, retryable |-> FALSE, rollbackEmitter |-> FALSE, poison |-> FALSE],
  [point |-> "Bridge", result |-> "FailedPreEffect", action |-> "RetryUnattempted", acceptedRecord |-> FALSE, retryable |-> TRUE, rollbackEmitter |-> FALSE, poison |-> FALSE],
  [point |-> "Bridge", result |-> "FailedEffectUnknown", action |-> "PoisonFailStop", acceptedRecord |-> FALSE, retryable |-> FALSE, rollbackEmitter |-> FALSE, poison |-> TRUE]
>>
SettlementMatches(point, result, action, acceptedRecord, retryable,
                  rollbackEmitter, poison) ==
  \E i \in 1..Len(SettlementRows) :
    LET row == SettlementRows[i] IN
      /\ row.point = point /\ row.result = result
      /\ row.action = action /\ row.acceptedRecord = acceptedRecord
      /\ row.retryable = retryable
      /\ row.rollbackEmitter = rollbackEmitter /\ row.poison = poison
====
