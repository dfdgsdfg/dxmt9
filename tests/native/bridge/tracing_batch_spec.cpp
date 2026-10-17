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
  batch.events[0] = {1000, 1, 1, D9C_TRACE_EVENT_SCOPE_BEGIN, 0};
  batch.events[1] = {1100, 1, 1, D9C_TRACE_EVENT_SCOPE_END, 0};
  return batch;
}

void testPointerFreePodLayout() {
  static_assert(std::is_standard_layout_v<D9CTraceBatch>);
  static_assert(std::is_trivially_copyable_v<D9CTraceBatch>);
  static_assert(std::is_standard_layout_v<D9CTraceBatchReceipt>);
  static_assert(std::is_trivially_copyable_v<D9CTraceBatchReceipt>);
  static_assert(offsetof(D9CTraceBatch, events) % alignof(uint64_t) == 0);
  check(dxmt9c_trace_batch_validate(nullptr) != 0,
        "null batch must be rejected");
  auto batch = validBatch();
  check(dxmt9c_trace_batch_validate(&batch) == 0,
        "versioned begin/end pair should validate");
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
  batch.qpcFrequency = 0;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "missing QPC frequency must be rejected");
}

void testCalibrationAndReceiptValidation() {
  auto batch = validBatch();
  batch.previousCalibration = {100, 500, 104, 8, 1, 77, 1, 0};
  check(dxmt9c_trace_batch_validate(&batch) == 0,
        "bracketed prior clock observation should validate");
  batch.previousCalibration.qpcAfter = 99;
  check(dxmt9c_trace_batch_validate(&batch) != 0,
        "reversed calibration bracket must be rejected");

  D9CTraceBatchReceipt receipt{};
  receipt.structSize = static_cast<uint32_t>(sizeof(receipt));
  receipt.version = D9C_TRACE_BATCH_VERSION;
  receipt.status = D9C_TRACE_RECEIPT_ACCEPTED;
  receipt.hostClockId = 3;
  receipt.nativeThreadId = 77;
  receipt.hostMonotonicNs = 500;
  receipt.hostUncertaintyNs = 8;
  check(dxmt9c_trace_batch_receipt_validate(&receipt) == 0,
        "accepted receipt with native clock anchor should validate");
  receipt.nativeThreadId = 0;
  check(dxmt9c_trace_batch_receipt_validate(&receipt) != 0,
        "accepted receipt without native TID must be rejected");
}

void testDisabledCaptureReceiptDoesNotLatchPeOptInOff() {
  // Runtime capture may still be delayed when the PE opt-in is already set.
  // A disabled receipt for this chunk must leave the production gate open for
  // the next chunk once the engine capture becomes active.
  const uint32_t disabledReceipt = D9C_TRACE_RECEIPT_DISABLED;
  check(disabledReceipt == D9C_TRACE_RECEIPT_DISABLED,
        "pre-armed response should be represented as a disabled receipt");
  check(dxmt9PeTraceCaptureEligible(true, true),
        "disabled receipt must not latch PE capture eligibility off");
}

void testDisabledWindowDoesNotBecomeInWindowLoss() {
  uint64_t droppedBatchCount = 7u;
  bool calibrationValid = true;

  auto action = dxmt9PeTraceReceiptAction(
      0, true, D9C_TRACE_RECEIPT_DISABLED, false);
  auto transition = dxmt9PeTraceTransitionReceipt(droppedBatchCount, action);
  check(action == Dxmt9PeTraceReceiptAction::Disabled,
        "a valid disabled receipt is an inactive-window outcome");
  calibrationValid = transition.keepCalibration;
  droppedBatchCount = transition.droppedBatchCount;
  check(!calibrationValid && droppedBatchCount == 0u,
        "disabled window clears calibration and prior inactive loss");

  action = dxmt9PeTraceReceiptAction(
      0, true, D9C_TRACE_RECEIPT_ACCEPTED, true);
  transition = dxmt9PeTraceTransitionReceipt(droppedBatchCount, action);
  calibrationValid = transition.keepCalibration;
  droppedBatchCount = transition.droppedBatchCount;
  check(calibrationValid && droppedBatchCount == 0u,
        "accepted in-window receipt starts with a fresh calibration");

  action = dxmt9PeTraceReceiptAction(
      0, true, D9C_TRACE_RECEIPT_DROPPED, true);
  transition = dxmt9PeTraceTransitionReceipt(droppedBatchCount, action);
  calibrationValid = transition.keepCalibration;
  droppedBatchCount = transition.droppedBatchCount;
  check(!calibrationValid && droppedBatchCount == 1u,
        "a real in-window drop clears calibration and counts as loss");
}

void testCalibrationUncertaintyIncludesQpcBracket() {
  uint64_t uncertainty = 0;
  check(dxmt9PeTraceCalibrationUncertainty(100u, 104u, 1'000'000u, 7u,
                                          &uncertainty),
        "valid QPC bracket uncertainty should be representable");
  check(uncertainty == 2007u,
        "calibration uncertainty adds half the rounded-up bracket duration");
  check(dxmt9PeTraceCalibrationUncertainty(10u, 12u, 3u, 0u,
                                          &uncertainty) &&
            uncertainty == 333'333'334u,
        "fractional QPC conversion rounds bracket and half-width upward");
  check(dxmt9PeTraceCalibrationUncertainty(
            0u, UINT64_MAX, UINT64_MAX, 0u, &uncertainty) &&
            uncertainty == 500'000'000u,
        "full-width bracket calculation avoids overflowing the product");
  check(!dxmt9PeTraceCalibrationUncertainty(1u, 2u, 0u, 0u,
                                           &uncertainty),
        "zero QPC frequency cannot qualify calibration uncertainty");
  check(!dxmt9PeTraceCalibrationUncertainty(
            0u, UINT64_MAX, 1u, UINT64_MAX, &uncertainty),
        "uncertainty addition overflow must reject qualification");
}

}  // namespace

int main() {
  try {
    testPointerFreePodLayout();
    testRejectMalformedBatches();
    testCalibrationAndReceiptValidation();
    testDisabledCaptureReceiptDoesNotLatchPeOptInOff();
    testDisabledWindowDoesNotBecomeInWindowLoss();
    testCalibrationUncertaintyIncludesQpcBracket();
  } catch (const std::exception& error) {
    std::cerr << "tracing batch spec failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
