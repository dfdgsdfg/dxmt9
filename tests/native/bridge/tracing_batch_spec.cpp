#include "dxmt9/trace_batch.h"
#include "../../../src/d3d9/d3d9_pe_trace.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

D9CTraceBatch validBatch() {
  D9CTraceBatch batch{};
  batch.structSize = static_cast<uint32_t>(sizeof(batch));
  batch.version = D9C_TRACE_BATCH_VERSION;
  batch.eventCount = D9C_TRACE_BATCH_MAX_EVENTS;
  batch.guestProcessId = 4;
  batch.guestThreadId = 9;
  batch.processCreateTime100ns = 100;
  batch.threadCreateTime100ns = 200;
  batch.qpcFrequency = 10'000'000;
  batch.sessionGeneration = 12;
  batch.nativeThreadId = 77;
  batch.events[0] = {1000, 1, 1, 5000, 5002, 1,
                     D9C_TRACE_EVENT_SCOPE_BEGIN, 0};
  batch.events[1] = {1100, 1, 1, 6000, 6002, 1,
                     D9C_TRACE_EVENT_SCOPE_END, 0};
  return batch;
}

void testPointerFreePodLayoutAndCurrentBatchBrackets() {
  static_assert(std::is_standard_layout_v<D9CTraceBatch>);
  static_assert(std::is_trivially_copyable_v<D9CTraceBatch>);
  static_assert(std::is_standard_layout_v<D9CTraceBatchReceipt>);
  static_assert(std::is_trivially_copyable_v<D9CTraceBatchReceipt>);
  static_assert(std::is_standard_layout_v<D9CTraceClockProbe>);
  static_assert(offsetof(D9CTraceBatch, events) % alignof(uint64_t) == 0);
  check(dxmt9c_trace_batch_validate(nullptr) != 0,
        "null batch must be rejected");
  auto batch = validBatch();
  check(dxmt9c_trace_batch_validate(&batch) == 0,
        "current-batch endpoint point brackets should validate without a prior batch");
  batch.events[0].hostBeforeNs = 0;
  batch.events[0].hostAfterNs = 0;
  batch.events[0].hostResolutionNs = 0;
  check(dxmt9c_trace_batch_validate(&batch) == 0,
        "source-local event may carry no usable point bracket");
  batch.events[1].hostBeforeNs = 0;
  batch.events[1].hostAfterNs = 0;
  batch.events[1].hostResolutionNs = 0;
  check(dxmt9c_trace_batch_validate(&batch) == 0,
        "last chunk needs no following chunk to qualify its endpoint");
}

void testRejectMalformedBatches() {
  auto batch = validBatch();
  batch.version++;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "unknown ABI version must be rejected");
  batch = validBatch();
  batch.eventCount++;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "unbounded event count must be rejected");
  batch = validBatch();
  batch.events[1].sourceOrder++;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "mismatched source order must be rejected");
  batch = validBatch();
  batch.events[1].qpcTicks = batch.events[0].qpcTicks - 1u;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "reversed QPC endpoints must be rejected");
  batch = validBatch();
  batch.events[0].sourceOrder = UINT64_MAX;
  batch.events[1].sourceOrder = UINT64_MAX;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "source order overflowing its calibration ID must be rejected");
  batch = validBatch();
  batch.qpcFrequency = 0;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "missing QPC frequency must be rejected");
  batch = validBatch();
  batch.events[0].hostAfterNs = batch.events[0].hostBeforeNs - 1u;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "reversed host point bracket must be rejected");
  batch = validBatch();
  batch.events[0].hostResolutionNs = 0u;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "point bracket without clock resolution must be rejected");
}

void testPointBracketErrorAndSelection() {
  uint64_t uncertainty = 0;
  check(dxmt9PeTracePointUncertainty(100u, 102u, 1u, 10'000'000u,
                                     &uncertainty) && uncertainty == 102u,
        "a short point bracket has bounded host, QPC and host-resolution error");
  check(uncertainty <= 1000u,
        "positive point bracket qualifies at the one-microsecond limit");
  check(dxmt9PeTracePointUncertainty(100u, 10'100u, 1u, 10'000'000u,
                                     &uncertainty) && uncertainty == 5101u,
        "slow probes retain their measured high uncertainty");
  check(dxmt9PeTracePreferPointBracket(5101u, true, 6000u, true),
        "when none qualify, retain the lowest-error slow candidate");
  check(dxmt9PeTracePreferPointBracket(999u, true, 1001u, true),
        "first qualifying candidate supersedes a slower candidate");
  check(!dxmt9PeTracePreferPointBracket(900u, true, 800u, true),
        "later qualified candidate does not replace the first qualified one");
  check(!dxmt9PeTracePointUncertainty(200u, 100u, 1u, 1u, &uncertainty),
        "reversed host bounds cannot qualify");
  check(!dxmt9PeTracePointUncertainty(1u, UINT64_MAX, UINT64_MAX, 1u,
                                      &uncertainty),
        "uncertainty addition overflow cannot qualify");
  check(dxmt9PeTracePointUncertainty(0u, 1u, 1u, 1u, &uncertainty) == false,
        "zero host clock point cannot qualify");
}

void testGenerationAndCalibrationIdentity() {
  Dxmt9PeTracePointBracket failedBracket{};
  failedBracket.qpcTicks = 1234u;
  failedBracket.sessionGeneration = 9u;
  failedBracket.nativeThreadId = 77u;
  failedBracket.hostBeforeNs = 500u;
  failedBracket.hostAfterNs = 400u;
  failedBracket.present = true;
  failedBracket = dxmt9PeTraceClearBracket(failedBracket);
  check(failedBracket.qpcTicks == 1234u &&
            failedBracket.sessionGeneration == 9u &&
            failedBracket.nativeThreadId == 77u &&
            !failedBracket.present && failedBracket.hostBeforeNs == 0u,
        "invalid bounds clear qualification while retaining endpoint identity and QPC");
  check(dxmt9PeTraceGenerationMatches(9u, 9u, 9u, 9u, 77u, 77u, 77u),
        "both endpoint probes and batch admission must share generation and native TID");
  check(!dxmt9PeTraceGenerationMatches(8u, 8u, 8u, 9u, 77u, 77u, 77u),
        "a stale generation cannot rearm an event into a newer session");
  check(!dxmt9PeTraceGenerationMatches(9u, 9u, 9u, 9u, 77u, 77u, 78u),
        "probe and admission native TIDs must match");
  uint64_t id = 0;
  check(dxmt9PeTraceCalibrationId(17u, 0u, &id) && id == 34u,
        "begin endpoint gets a unique checked point calibration ID");
  check(dxmt9PeTraceCalibrationId(17u, 1u, &id) && id == 35u,
        "end endpoint gets a unique checked point calibration ID");
  check(!dxmt9PeTraceCalibrationId(UINT64_MAX, 1u, &id),
        "calibration ID multiplication overflow is rejected");
}

void testReceiptValidation() {
  D9CTraceBatchReceipt receipt{};
  receipt.structSize = static_cast<uint32_t>(sizeof(receipt));
  receipt.version = D9C_TRACE_BATCH_VERSION;
  receipt.status = D9C_TRACE_RECEIPT_ACCEPTED;
  receipt.hostClockId = 3;
  receipt.nativeThreadId = 77;
  receipt.hostMonotonicNs = 500;
  receipt.hostUncertaintyNs = 1;
  check(dxmt9c_trace_batch_receipt_validate(&receipt) == 0,
        "accepted receipt should validate");
  receipt.nativeThreadId = 0;
  check(dxmt9c_trace_batch_receipt_validate(&receipt) != 0,
        "accepted receipt without native TID must be rejected");
}

void testDisabledCaptureReceiptDoesNotLatchPeOptInOff() {
  check(dxmt9PeTraceCaptureEligible(true, true),
        "disabled receipt must not latch PE capture eligibility off");
}

void testReceiptTransitions() {
  auto action = dxmt9PeTraceReceiptAction(
      0, true, D9C_TRACE_RECEIPT_DISABLED);
  check(action == Dxmt9PeTraceReceiptAction::Disabled &&
            dxmt9PeTraceTransitionReceipt(7u, action).droppedBatchCount == 0u,
        "disabled window clears prior inactive loss");
  action = dxmt9PeTraceReceiptAction(0, true, D9C_TRACE_RECEIPT_ACCEPTED);
  check(dxmt9PeTraceTransitionReceipt(7u, action).droppedBatchCount == 0u,
        "accepted batch clears prior loss");
  action = dxmt9PeTraceReceiptAction(1, false, D9C_TRACE_RECEIPT_ACCEPTED);
  check(dxmt9PeTraceTransitionReceipt(7u, action).droppedBatchCount == 8u,
        "failed in-window batch remains accounted as loss");
}

}  // namespace

int main() {
  try {
    testPointerFreePodLayoutAndCurrentBatchBrackets();
    testRejectMalformedBatches();
    testPointBracketErrorAndSelection();
    testGenerationAndCalibrationIdentity();
    testReceiptValidation();
    testDisabledCaptureReceiptDoesNotLatchPeOptInOff();
    testReceiptTransitions();
  } catch (const std::exception& error) {
    std::cerr << "tracing batch spec failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
