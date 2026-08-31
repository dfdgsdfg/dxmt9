#include "d3d9_pe_commit_transition.hpp"
#include "d3d9_pe_recorder_transaction.hpp"
#include "d3d9_pe_recorder_state.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_recorder.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <type_traits>
#include <vector>

namespace pe = dxmt9::d3d9::pe;

namespace {

static_assert(!std::is_aggregate_v<pe::RecorderCommitPlan>);
static_assert(!std::is_aggregate_v<pe::RecorderSettlementPlan>);

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

struct CommitWitness {
  pe::RecorderCommitPhase phase = pe::RecorderCommitPhase::Unsealed;
  std::vector<std::uint8_t> bytes{1u, 2u, 3u, 4u};
  std::uint32_t records = 2u;
  std::uint32_t handles = 3u;
  std::uint32_t pendingRefs = 2u;
  std::uint32_t aliasDestroys = 0u;
  std::uint32_t parentDestroys = 0u;
  std::uint32_t drainBegins = 0u;
  std::uint32_t drainCompletes = 0u;
  std::uint32_t builderResets = 0u;
  std::uint32_t warmAdvances = 0u;
  bool commandAccepted = false;

  bool apply(pe::RecorderCommitFacts facts) {
    const auto plan = pe::settleRecorderCommit(facts);
    if (!plan.valid()) return false;
    phase = plan.next();
    commandAccepted = commandAccepted || plan.commandAccepted();
    if (plan.objectDestroy()) {
      if (plan.action() == pe::RecorderCommitAction::DestroyAlias) {
        ++aliasDestroys;
        if (pendingRefs != 0u) --pendingRefs;
      } else if (plan.action() == pe::RecorderCommitAction::DestroyParent) {
        ++parentDestroys;
        if (pendingRefs != 0u) --pendingRefs;
      }
    }
    if (plan.action() == pe::RecorderCommitAction::BeginDrain) {
      ++drainBegins;
    } else if (plan.action() == pe::RecorderCommitAction::FinishDrain) {
      ++drainCompletes;
    }
    if (plan.resetBuilder()) {
      ++builderResets;
      bytes.clear();
      records = handles = pendingRefs = 0u;
    }
    if (plan.advanceWarmEpoch()) ++warmAdvances;
    return true;
  }
};

void testRetryAndAcceptedCapture() {
  CommitWitness witness{};
  const auto retryBytes = witness.bytes;
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Unsealed,
            .event = pe::RecorderCommitEvent::SealFailed}) &&
            witness.phase == pe::RecorderCommitPhase::Unsealed &&
            witness.bytes == retryBytes,
        "seal failure preserves retry bytes");
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Unsealed,
            .event = pe::RecorderCommitEvent::SealAccepted}) &&
            witness.phase == pe::RecorderCommitPhase::Sealed,
        "successful seal enters sealed phase");
  const auto sealedBytes = witness.bytes;
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Sealed,
            .event = pe::RecorderCommitEvent::BridgePreEffectFailed}) &&
            witness.phase == pe::RecorderCommitPhase::Sealed &&
            witness.bytes == sealedBytes,
        "pre-effect bridge failure preserves sealed retry bytes");
  const auto effectUnknown = pe::settleRecorderCommit({
      .phase = pe::RecorderCommitPhase::Sealed,
      .event = pe::RecorderCommitEvent::BridgeEffectUnknown});
  check(effectUnknown.valid() && effectUnknown.poisons() &&
            effectUnknown.next() == pe::RecorderCommitPhase::Poisoned &&
            !effectUnknown.preserveRetryBytes(),
        "entered bridge failure is effect-unknown fail-stop");
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Sealed,
            .event = pe::RecorderCommitEvent::BridgeAccepted}) &&
            witness.commandAccepted &&
            witness.phase == pe::RecorderCommitPhase::Accepted,
        "bridge acceptance publishes command only");
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Accepted,
            .event = pe::RecorderCommitEvent::CaptureRejected}) &&
            witness.commandAccepted &&
            witness.phase == pe::RecorderCommitPhase::CaptureSettled,
        "capture rejection does not retract accepted command");

  CommitWitness skipped{};
  skipped.commandAccepted = true;
  check(skipped.apply({
            .phase = pe::RecorderCommitPhase::Accepted,
            .event = pe::RecorderCommitEvent::CaptureSkipped}) &&
            skipped.commandAccepted &&
            skipped.phase == pe::RecorderCommitPhase::CaptureSettled,
        "capture skipped settles without capture side effects");
}

void testDrainOrderingAndReset() {
  CommitWitness witness{};
  witness.apply({.phase = pe::RecorderCommitPhase::Unsealed,
                 .event = pe::RecorderCommitEvent::SealAccepted});
  witness.apply({.phase = pe::RecorderCommitPhase::Sealed,
                 .event = pe::RecorderCommitEvent::BridgeAccepted});
  check(witness.apply({.phase = pe::RecorderCommitPhase::Accepted,
                       .event = pe::RecorderCommitEvent::CaptureRejected}) &&
            witness.phase == pe::RecorderCommitPhase::CaptureSettled,
        "capture rejection settles after accepted bridge");
  check(witness.apply({.phase = pe::RecorderCommitPhase::CaptureSettled,
                       .event = pe::RecorderCommitEvent::DrainPending}) &&
            witness.phase == pe::RecorderCommitPhase::Draining,
        "accepted capture begins pending-reference drain");
  check(!pe::settleRecorderCommit({
              .phase = pe::RecorderCommitPhase::Draining,
              .event = pe::RecorderCommitEvent::DrainParent,
              .aliasesRemain = true,
              .parentPending = true})
             .valid(),
        "parent cannot destroy before aliases");
  check(witness.apply({.phase = pe::RecorderCommitPhase::Draining,
                       .event = pe::RecorderCommitEvent::DrainAlias,
                       .aliasesRemain = true}) &&
            witness.aliasDestroys == 1u,
        "alias destroy is admitted first");
  check(witness.apply({.phase = pe::RecorderCommitPhase::Draining,
                       .event = pe::RecorderCommitEvent::DrainParent,
                       .parentPending = true}) &&
            witness.parentDestroys == 1u && witness.phase ==
                pe::RecorderCommitPhase::Drained,
        "parent destroy is exactly once after aliases");
  check(witness.drainBegins == 1u,
        "capture failure begins pending-reference drain exactly once");
  check(!witness.apply({.phase = pe::RecorderCommitPhase::Drained,
                        .event = pe::RecorderCommitEvent::DrainParent,
                        .parentPending = true}),
        "repeated parent destroy is rejected");
  check(witness.apply({.phase = pe::RecorderCommitPhase::Drained,
                       .event = pe::RecorderCommitEvent::BuilderReset}) &&
            witness.bytes.empty() && witness.builderResets == 1u,
        "builder reset follows reference drain");
  check(witness.apply({.phase = pe::RecorderCommitPhase::Reset,
                       .event = pe::RecorderCommitEvent::WarmEpochAdvance}) &&
            witness.warmAdvances == 1u &&
            witness.phase == pe::RecorderCommitPhase::WarmAdvanced,
        "warm epoch advances after reset");
  check(witness.apply({.phase = pe::RecorderCommitPhase::WarmAdvanced,
                       .event = pe::RecorderCommitEvent::DrainComplete}) &&
            witness.phase == pe::RecorderCommitPhase::Unsealed,
        "completed settlement returns to unsealed");

  // The no-pending-reference variant takes the explicit DrainComplete edge;
  // it exercises the same pure helper used by production cleanup.
  CommitWitness noPending{};
  noPending.apply({.phase = pe::RecorderCommitPhase::Unsealed,
                   .event = pe::RecorderCommitEvent::SealAccepted});
  noPending.apply({.phase = pe::RecorderCommitPhase::Sealed,
                   .event = pe::RecorderCommitEvent::BridgeAccepted});
  noPending.apply({.phase = pe::RecorderCommitPhase::Accepted,
                   .event = pe::RecorderCommitEvent::CaptureRejected});
  noPending.apply({.phase = pe::RecorderCommitPhase::CaptureSettled,
                   .event = pe::RecorderCommitEvent::DrainPending});
  check(noPending.apply({.phase = pe::RecorderCommitPhase::Draining,
                         .event = pe::RecorderCommitEvent::DrainComplete,
                         .parentPending = false}) &&
            noPending.drainCompletes == 1u &&
            noPending.phase == pe::RecorderCommitPhase::Drained,
        "capture failure completes drain exactly once");
}

void testDiscardAndFailureMatrix() {
  check(peRecorderResetDisposition(false) ==
                PeRecorderFlushDisposition::Submit &&
            peRecorderResetDisposition(true) ==
                PeRecorderFlushDisposition::DiscardForRecovery,
        "healthy Reset submits queued work while poisoned recovery discards");
  check(planPeRecorderFlush(PeRecorderFlushDisposition::Submit, false) ==
            PeRecorderFlushAction::Submit &&
            planPeRecorderFlush(PeRecorderFlushDisposition::Submit, true) ==
                PeRecorderFlushAction::RejectPoisoned &&
            planPeRecorderFlush(
                PeRecorderFlushDisposition::DiscardForRecovery, true) ==
                PeRecorderFlushAction::Discard,
        "production flush seam rejects poisoned resubmission and admits recovery discard");
  constexpr std::array<pe::RecorderCommitPhase, 9> phases = {
      pe::RecorderCommitPhase::Unsealed, pe::RecorderCommitPhase::Sealed,
      pe::RecorderCommitPhase::Accepted,
      pe::RecorderCommitPhase::CaptureSettled,
      pe::RecorderCommitPhase::Draining, pe::RecorderCommitPhase::Drained,
      pe::RecorderCommitPhase::Reset, pe::RecorderCommitPhase::WarmAdvanced,
      pe::RecorderCommitPhase::Poisoned};
  for (const auto phase : phases) {
    CommitWitness witness{};
    witness.phase = phase;
    const auto plan = pe::settleRecorderCommit({
        .phase = phase,
        .event = pe::RecorderCommitEvent::ExplicitDiscard});
    check(plan.valid() && plan.action() == pe::RecorderCommitAction::DiscardAll &&
              plan.resetBuilder() && !plan.advanceWarmEpoch(),
          "explicit discard releases pins without warm epoch advance");
    witness.apply({.phase = phase,
                   .event = pe::RecorderCommitEvent::ExplicitDiscard});
    check(witness.bytes.empty() && witness.records == 0u &&
              witness.handles == 0u && witness.pendingRefs == 0u &&
              witness.aliasDestroys == 0u && witness.parentDestroys == 0u,
          "discard clears bytes/counts/pins without ObjectDestroy");
    check(!pe::settleRecorderCommit({
                .phase = pe::RecorderCommitPhase::Discarded,
                .event = pe::RecorderCommitEvent::ExplicitDiscard})
             .valid(),
          "repeated discard is rejected");
  }
}

void testComposedSettlementPredicates() {
  constexpr std::array points{
      pe::RecorderSettlementPoint::CapacityPre,
      pe::RecorderSettlementPoint::Emitter,
      pe::RecorderSettlementPoint::CapacityPost,
      pe::RecorderSettlementPoint::Bridge};
  constexpr std::array results{
      pe::RecorderSettlementResult::Succeeded,
      pe::RecorderSettlementResult::FailedPreEffect,
      pe::RecorderSettlementResult::FailedEffectUnknown};
  std::size_t valid = 0u;
  for (const auto point : points) {
    for (const auto result : results) {
      const auto plan = pe::planRecorderSettlement({point, result});
      std::size_t matches = 0u;
      for (const auto& row : pe::kRecorderSettlementTable) {
        if (row.point == point && row.result == result) ++matches;
      }
      check(matches <= 1u && plan.valid() == (matches == 1u),
            "composed settlement table is exhaustive and unambiguous");
      valid += plan.valid() ? 1u : 0u;
      if (plan.valid()) {
        check(plan.poison() == (result ==
                  pe::RecorderSettlementResult::FailedEffectUnknown),
              "effect-unknown rows alone fail-stop");
      }
    }
  }
  check(valid == pe::kRecorderSettlementTable.size() && valid == 11u,
        "all composed production settlement rows are reachable");

  const auto pre = pe::planRecorderSettlement({
      pe::RecorderSettlementPoint::CapacityPre,
      pe::RecorderSettlementResult::FailedPreEffect});
  const auto emitter = pe::planRecorderSettlement({
      pe::RecorderSettlementPoint::Emitter,
      pe::RecorderSettlementResult::FailedPreEffect});
  const auto post = pe::planRecorderSettlement({
      pe::RecorderSettlementPoint::CapacityPost,
      pe::RecorderSettlementResult::FailedPreEffect});
  const auto unknown = pe::planRecorderSettlement({
      pe::RecorderSettlementPoint::Bridge,
      pe::RecorderSettlementResult::FailedEffectUnknown});
  const auto preUnknown = pe::planRecorderSettlement({
      pe::RecorderSettlementPoint::CapacityPre,
      pe::RecorderSettlementResult::FailedEffectUnknown});
  check(pre.retryable() && !pre.acceptedRecord() &&
            emitter.rollbackEmitter() && !emitter.acceptedRecord() &&
            post.retryable() && post.acceptedRecord() && unknown.poison() &&
            !unknown.retryable() && preUnknown.poison() &&
            !preUnknown.retryable() &&
            pe::recorderFlushSettlementResult(true, false) ==
                pe::RecorderSettlementResult::Succeeded &&
            pe::recorderFlushSettlementResult(false, false) ==
                pe::RecorderSettlementResult::FailedPreEffect &&
            pe::recorderFlushSettlementResult(false, true) ==
                pe::RecorderSettlementResult::FailedEffectUnknown,
        "capacity/emitter/bridge dispositions remain distinct");

  check(!pe::recorderCaptureMayRetract(true) &&
            pe::recorderParentDrainAllowed(false, true) &&
            !pe::recorderParentDrainAllowed(true, true) &&
            pe::recorderResetAfterDrainAllowed(false, false, false) &&
            !pe::recorderResetAfterDrainAllowed(true, false, false) &&
            pe::recorderWarmAdvanceAllowed(true, true) &&
            !pe::recorderWarmAdvanceAllowed(false, true),
        "capture/drain/reset/warm predicates enforce composed order");
}

void testCapacityPreFlushComposition() {
  constexpr std::array captureEvents{
      pe::RecorderCommitEvent::CaptureMaterialized,
      pe::RecorderCommitEvent::CaptureRejected,
      pe::RecorderCommitEvent::CaptureSkipped};
  for (const auto captureEvent : captureEvents) {
    CommitWitness prior{};
    const auto retry = pe::planRecorderSettlement({
        pe::RecorderSettlementPoint::CapacityPre,
        pe::recorderFlushSettlementResult(false, false)});
    check(retry.valid() && retry.retryable() && !retry.acceptedRecord(),
          "capacity-pre local failure leaves proposed append unattempted");
    check(prior.apply({.phase = pe::RecorderCommitPhase::Unsealed,
                       .event = pe::RecorderCommitEvent::SealAccepted}) &&
              prior.apply({.phase = pe::RecorderCommitPhase::Sealed,
                           .event = pe::RecorderCommitEvent::BridgeAccepted}) &&
              prior.apply({.phase = pe::RecorderCommitPhase::Accepted,
                           .event = captureEvent}) &&
              prior.commandAccepted,
          "capacity-pre flush crosses seal, bridge, and capture settlement");
    check(prior.apply({.phase = pe::RecorderCommitPhase::CaptureSettled,
                       .event = pe::RecorderCommitEvent::DrainPending}) &&
              prior.apply({.phase = pe::RecorderCommitPhase::Draining,
                           .event = pe::RecorderCommitEvent::DrainAlias,
                           .aliasesRemain = true}) &&
              prior.apply({.phase = pe::RecorderCommitPhase::Draining,
                           .event = pe::RecorderCommitEvent::DrainParent,
                           .parentPending = true}) &&
              prior.apply({.phase = pe::RecorderCommitPhase::Drained,
                           .event = pe::RecorderCommitEvent::BuilderReset}) &&
              prior.apply({.phase = pe::RecorderCommitPhase::Reset,
                           .event = pe::RecorderCommitEvent::WarmEpochAdvance}) &&
              prior.apply({.phase = pe::RecorderCommitPhase::WarmAdvanced,
                           .event = pe::RecorderCommitEvent::DrainComplete}),
          "capacity-pre flush drains aliases before parent, resets, then warms");
    const auto success = pe::planRecorderSettlement({
        pe::RecorderSettlementPoint::CapacityPre,
        pe::recorderFlushSettlementResult(true, false)});
    const auto emit = pe::planRecorderSettlement({
        pe::RecorderSettlementPoint::Emitter,
        pe::RecorderSettlementResult::Succeeded});
    check(prior.phase == pe::RecorderCommitPhase::Unsealed &&
              success.valid() && !success.acceptedRecord() && emit.valid() &&
              emit.acceptedRecord(),
          "only the post-flush emitter accepts the proposed append");
  }
}

void testRecorderChunkTransactionAndPendingTicket() {
  using Phase = pe::RecorderChunkTransactionPhase;
  pe::PeRecorderChunkTransaction append;
  check(!append.recordCapacityPreEvidence(true) &&
            !append.recordCapacityPostResult(true),
        "capacity evidence rejects an idle owner");
  check(!append.seal(true) && append.phase() == Phase::Idle,
        "an untracked builder cannot synthesize a production owner at seal");
  check(append.beginRecord(17u, 64u),
        "transaction accepts a bounded record intent");
  check(!append.capacityPre(false) && append.retryable() &&
            append.phase() == Phase::Retry,
        "capacity-pre failure is retryable before emitter effects");

  pe::PeRecorderChunkTransaction emitter;
  check(emitter.beginRecord(17u, 64u) && emitter.capacityPre(true) &&
            !emitter.emit(false) &&
            emitter.phase() == Phase::Collecting && emitter.recordCount() == 0u,
        "emitter failure rolls back only the active record intent");

  pe::PeRecorderChunkTransaction post;
  check(post.beginRecord(17u, 64u) && post.emit(true) &&
            !post.capacityPost(false) && post.retryable(),
        "capacity-post failure keeps the accepted record retryable");

  pe::PeRecorderChunkTransaction bridge;
  check(bridge.beginChunk() && bridge.seal(true) && !bridge.bridge(false) &&
            bridge.poisoned(),
        "entered bridge failure is effect-unknown and poisons");

  pe::PeRecorderChunkTransaction capture;
  check(capture.beginChunk() && capture.seal(true) &&
            (capture.recordCaptureReservation(0x90u, 0x91u, true),
             capture.captureReserved() && capture.captureToken() == 0x90u &&
             capture.captureOrdinal() == 0x91u) && capture.bridge(true) &&
            capture.capture(pe::RecorderChunkCaptureDisposition::Rejected) &&
            capture.complete() && capture.completed(),
        "capture rejection settles after command acceptance");

  pe::PeRecorderChunkTransaction multi;
  check(multi.beginChunk() &&
            multi.noteRecord(17u, 64u, {.generation = 11u}, 1u, 2u, 3u, 4u) &&
            multi.emit(true) &&
            multi.transactionEpoch() != 0u &&
            [&multi] {
              const auto epoch = multi.transactionEpoch();
              return multi.noteRecord(18u, 72u, {.generation = 12u},
                                      5u, 6u, 7u, 8u) &&
                     multi.transactionEpoch() == epoch &&
                     !multi.emit(false);
            }() &&
            multi.phase() == Phase::Emitted &&
            multi.recordCount() == 1u &&
            multi.pendingTicket().generation == 11u &&
            multi.recordCheckpoint() == 1u &&
            multi.activeRecordCheckpoint() == 0u &&
            multi.activeHandleCheckpoint() == 0u &&
            multi.activePayloadCheckpoint() == 0u &&
            multi.activeRetainerCheckpoint() == 0u,
        "failed second record restores the prior transaction frontier");

  pe::PeRecorderChunkTransaction poisoned;
  check(poisoned.beginChunk() && poisoned.seal(true) &&
            !poisoned.bridge(false) && poisoned.poisoned() &&
            !poisoned.recordCapacityPreResult(false) && poisoned.poisoned() &&
            poisoned.recordCapacityPostResult(false) && poisoned.poisoned(),
        "capacity-pre failure cannot reopen an effect-unknown owner");

  // This is deliberately a bounded production-type seam harness rather than
  // a D3D9DeviceImpl/COM fixture. It drives the same PeRecorderState helpers
  // used by appendRecord/flushPendingCommandChunk, with the real bounded
  // CommandChunkBuilder and a canonical Clear emitter.
  pe::PeRecorderState productionState;
  const auto pendingKey = renderStateSlotKey(7u);
  productionState.peState.transition().setRenderState(pendingKey, 1u);
  const auto pendingTicket = productionState.peState.pendingTicket();
  const D9CCommandChunkWireClear clear{.flags = 1u,
                                       .colorARGB = 0xff102030u,
                                       .z = 0.5f,
                                       .stencil = 3u};
  const std::array<D9CRect, 0u> rects{};
  check(productionState.prepareChunkRecord(
              D9C_COMMAND_RECORD_CLEAR, sizeof(clear), false) &&
            productionState.chunkTransaction.pendingTicket().generation ==
                pendingTicket.generation &&
            dxmt9::d3d9::pe::appendClear(
                productionState.commandChunk, clear, rects) &&
            productionState.settleChunkEmitter(true),
        "production-type append records through the persistent owner");
  const auto sealed = productionState.commandChunk.seal();
  check(sealed.valid() &&
            productionState.chunkTransaction.recordSealResult(true) &&
            productionState.recordChunkSealedEvidence() &&
            productionState.sealedEvidenceMatchesChunk() &&
            productionState.chunkTransaction.recordCaptureReservation(
                0u, 0u, false) &&
            productionState.chunkTransaction.recordBridgeResult(true) &&
            productionState.chunkTransaction.recordCaptureResult(
                pe::RecorderChunkCaptureDisposition::Skipped) &&
            productionState.chunkTransaction.recordCapacityPostResult(true) &&
            productionState.chunkTransaction.complete() &&
            productionState.chunkTransaction.completed(),
        "production-type seal freezes storage before bridge settlement");
  productionState.commandChunk.reset();
  productionState.chunkTransaction.discard();
  check(!productionState.commandChunk.sealed() &&
            productionState.commandChunk.recordCount() == 0u &&
            productionState.chunkTransaction.phase() == Phase::Idle,
        "production-type reset releases sealed storage and owner");

  PeHotStateShadow shadow{};
  const auto key = renderStateSlotKey(7u);
  shadow.transition().setRenderState(key, 1u);
  const auto stale = shadow.pendingTicket();
  shadow.transition().setRenderState(key, 2u);
  shadow.transition().setRenderState(key, 1u);
  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  const std::array<D9CCommandChunkWireRenderState, 1u> row{{{7u, 1u}}};
  auto staleConsumer = shadow.consume(stale);
  check(!staleConsumer.valid() &&
            shadow.pendingRenderStatesTyped().contains(key),
        "A-to-B-to-A invalidates the old pending ticket");
  auto currentConsumer = shadow.consume(shadow.pendingTicket());
  check(currentConsumer.valid() &&
            currentConsumer.acceptRenderStateBatch(row, accepted) &&
            shadow.pendingRenderStatesTyped().empty(),
        "the current ticket conditionally settles the pending row");
}

}  // namespace

int main() {
  testRetryAndAcceptedCapture();
  testDrainOrderingAndReset();
  testDiscardAndFailureMatrix();
  testComposedSettlementPredicates();
  testCapacityPreFlushComposition();
  testRecorderChunkTransactionAndPendingTicket();
  if (failures != 0) return 1;
  std::puts("pe commit transition/production-type seam spec: PASS");
  return 0;
}
