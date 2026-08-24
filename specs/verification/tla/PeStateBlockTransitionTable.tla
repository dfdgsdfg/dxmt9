---- MODULE PeStateBlockTransitionTable ----
EXTENDS Naturals, Sequences
(***************************************************************************
 * Generated from src/d3d9/d3d9_pe_stateblock_transition_table.inc.
 * Do not hand-edit. This is the model/code isomorphism witness.
 ***************************************************************************)
StateBlockRows == <<
  [phase |-> "Idle", event |-> "PoisonRequested", next |-> "Poisoned", action |-> "FailStop", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "Recording", event |-> "PoisonRequested", next |-> "Poisoned", action |-> "FailStop", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "EndPublication", event |-> "PoisonRequested", next |-> "Poisoned", action |-> "FailStop", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "ApplyPrepared", event |-> "PoisonRequested", next |-> "Poisoned", action |-> "FailStop", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "Poisoned", event |-> "PoisonRequested", next |-> "Poisoned", action |-> "FailStop", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "BeginFailed", next |-> "Idle", action |-> "Preserve", candidateEffect |-> "Preserve", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "BeginAccepted", next |-> "Recording", action |-> "BeginRecording", candidateEffect |-> "Discard", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Recording", event |-> "EndPreEffectFailed", next |-> "Recording", action |-> "Preserve", candidateEffect |-> "Preserve", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Recording", event |-> "EndBackendFailed", next |-> "Poisoned", action |-> "FailStop", candidateEffect |-> "Discard", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Recording", event |-> "EndBackendAccepted", next |-> "EndPublication", action |-> "EnterEndPublication", candidateEffect |-> "Preserve", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "EndPublication", event |-> "EndWrapperFailed", next |-> "Poisoned", action |-> "FailStop", candidateEffect |-> "Discard", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "EndPublication", event |-> "EndPublished", next |-> "Idle", action |-> "PublishEnd", candidateEffect |-> "Discard", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "CapturePreEffectFailed", next |-> "Idle", action |-> "Preserve", candidateEffect |-> "Preserve", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "CaptureBackendFailed", next |-> "Poisoned", action |-> "FailStop", candidateEffect |-> "Preserve", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "CapturePublished", next |-> "Idle", action |-> "PublishCapture", candidateEffect |-> "Preserve", stagedRefEffect |-> "Preserve", captureEffect |-> "Publish"],
  [phase |-> "Idle", event |-> "ApplyPrepareFailed", next |-> "Idle", action |-> "Preserve", candidateEffect |-> "Preserve", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "ApplyPrepared", next |-> "ApplyPrepared", action |-> "RetainApplyRefs", candidateEffect |-> "Preserve", stagedRefEffect |-> "Retain", captureEffect |-> "Preserve"],
  [phase |-> "ApplyPrepared", event |-> "ApplyBackendFailed", next |-> "Poisoned", action |-> "FailStop", candidateEffect |-> "Preserve", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "ApplyPrepared", event |-> "ApplyBackendAccepted", next |-> "Idle", action |-> "TransferApplyRefs", candidateEffect |-> "Preserve", stagedRefEffect |-> "Transfer", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "ResetStarted", next |-> "Idle", action |-> "AbandonForReset", candidateEffect |-> "Discard", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Recording", event |-> "ResetStarted", next |-> "Idle", action |-> "AbandonForReset", candidateEffect |-> "Discard", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Poisoned", event |-> "ResetStarted", next |-> "Poisoned", action |-> "AbandonForReset", candidateEffect |-> "Discard", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "ResetFailed", next |-> "Idle", action |-> "Preserve", candidateEffect |-> "Preserve", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Poisoned", event |-> "ResetFailed", next |-> "Poisoned", action |-> "Preserve", candidateEffect |-> "Preserve", stagedRefEffect |-> "Preserve", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "ResetAccepted", next |-> "Idle", action |-> "RecoverReset", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "Poisoned", event |-> "ResetAccepted", next |-> "Idle", action |-> "RecoverReset", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "Idle", event |-> "Teardown", next |-> "Terminal", action |-> "Teardown", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "Recording", event |-> "Teardown", next |-> "Terminal", action |-> "Teardown", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "EndPublication", event |-> "Teardown", next |-> "Terminal", action |-> "Teardown", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "ApplyPrepared", event |-> "Teardown", next |-> "Terminal", action |-> "Teardown", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"],
  [phase |-> "Poisoned", event |-> "Teardown", next |-> "Terminal", action |-> "Teardown", candidateEffect |-> "Discard", stagedRefEffect |-> "Release", captureEffect |-> "Preserve"]
>>
StateBlockMatches(phase, event, next, action, candidateEffect,
                  stagedRefEffect, captureEffect) ==
  \E i \in 1..Len(StateBlockRows) :
    LET row == StateBlockRows[i] IN
      /\ row.phase = phase
      /\ row.event = event
      /\ row.next = next
      /\ row.action = action
      /\ row.candidateEffect = candidateEffect
      /\ row.stagedRefEffect = stagedRefEffect
      /\ row.captureEffect = captureEffect
====
