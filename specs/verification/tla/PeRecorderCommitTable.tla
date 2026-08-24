---- MODULE PeRecorderCommitTable ----
EXTENDS Naturals, Sequences
(***************************************************************************
 * Generated from src/d3d9/d3d9_pe_commit_transition.hpp.  Do not hand-edit.
 * This table is a freshness/isomorphism witness for the PE commit algebra.
 ***************************************************************************)
CommitRows == <<
  [phase |-> "Any", event |-> "ExplicitDiscard", next |-> "Discarded", action |-> "DiscardAll", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> TRUE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Any", event |-> "DeviceReset", next |-> "Discarded", action |-> "DiscardAll", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> TRUE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Unsealed", event |-> "SealAccepted", next |-> "Sealed", action |-> "Seal", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Unsealed", event |-> "SealFailed", next |-> "Unsealed", action |-> "Retry", preserveRetryBytes |-> TRUE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Sealed", event |-> "BridgeAccepted", next |-> "Accepted", action |-> "AcceptCommand", preserveRetryBytes |-> FALSE, commandAccepted |-> TRUE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Sealed", event |-> "BridgePreEffectFailed", next |-> "Sealed", action |-> "Retry", preserveRetryBytes |-> TRUE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Sealed", event |-> "BridgeEffectUnknown", next |-> "Poisoned", action |-> "FailStop", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Accepted", event |-> "CaptureMaterialized", next |-> "CaptureSettled", action |-> "CaptureCommit", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Accepted", event |-> "CaptureRejected", next |-> "CaptureSettled", action |-> "CaptureReject", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Accepted", event |-> "CaptureSkipped", next |-> "CaptureSettled", action |-> "NoOp", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "CaptureSettled", event |-> "DrainPending", next |-> "Draining", action |-> "BeginDrain", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Draining", event |-> "DrainAlias", next |-> "Draining", action |-> "DestroyAlias", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> TRUE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Draining", event |-> "DrainParent", next |-> "Drained", action |-> "DestroyParent", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> TRUE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Draining", event |-> "DrainComplete", next |-> "Drained", action |-> "FinishDrain", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Drained", event |-> "BuilderReset", next |-> "Reset", action |-> "ResetBuilder", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> TRUE, advanceWarmEpoch |-> FALSE],
  [phase |-> "Reset", event |-> "WarmEpochAdvance", next |-> "WarmAdvanced", action |-> "AdvanceWarmEpoch", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> TRUE],
  [phase |-> "WarmAdvanced", event |-> "DrainComplete", next |-> "Unsealed", action |-> "NoOp", preserveRetryBytes |-> FALSE, commandAccepted |-> FALSE, objectDestroy |-> FALSE, resetBuilder |-> FALSE, advanceWarmEpoch |-> FALSE]
>>
CommitMatches(phase, event, next, action, preserveRetryBytes,
              commandAccepted, objectDestroy, resetBuilder,
              advanceWarmEpoch) ==
  \E i \in 1..Len(CommitRows) :
    LET row == CommitRows[i] IN
      /\ (row.phase = "Any" \/ row.phase = phase)
      /\ row.event = event
      /\ row.next = next
      /\ row.action = action
      /\ row.preserveRetryBytes = preserveRetryBytes
      /\ row.commandAccepted = commandAccepted
      /\ row.objectDestroy = objectDestroy
      /\ row.resetBuilder = resetBuilder
      /\ row.advanceWarmEpoch = advanceWarmEpoch
====
