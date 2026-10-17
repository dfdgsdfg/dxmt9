#pragma once

#include "dxmt9/device_c.h"

#include <cstdlib>
#include <cstdint>

inline bool dxmt9PeTraceEnabled() noexcept {
  static const bool enabled = []() noexcept {
    const char* value = std::getenv("DXMT9_TRACE_PE");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

// Transport receipts describe the current capture window. They never alter
// whether a later chunk should be observed while the user opt-in is set.
inline bool dxmt9PeTraceCaptureEligible(bool optedIn,
                                        bool identityValid) noexcept {
  return optedIn && identityValid;
}

enum class Dxmt9PeTraceReceiptAction : uint8_t {
  Accepted,
  Disabled,
  Loss,
};

constexpr Dxmt9PeTraceReceiptAction dxmt9PeTraceReceiptAction(
    int32_t transportStatus, bool receiptValid, uint32_t receiptStatus,
    bool calibrationWindowValid) noexcept {
  if (transportStatus == 0 && receiptValid &&
      receiptStatus == D9C_TRACE_RECEIPT_DISABLED) {
    return Dxmt9PeTraceReceiptAction::Disabled;
  }
  if (transportStatus == 0 && receiptValid && calibrationWindowValid &&
      receiptStatus == D9C_TRACE_RECEIPT_ACCEPTED) {
    return Dxmt9PeTraceReceiptAction::Accepted;
  }
  return Dxmt9PeTraceReceiptAction::Loss;
}

struct Dxmt9PeTraceReceiptTransition {
  bool keepCalibration;
  uint64_t droppedBatchCount;
};

constexpr bool dxmt9PeTraceCalibrationUncertainty(
    uint64_t qpcBefore, uint64_t qpcAfter, uint64_t qpcFrequency,
    uint64_t hostUncertaintyNs, uint64_t* outUncertaintyNs) noexcept {
  if (!outUncertaintyNs || qpcFrequency == 0u || qpcAfter < qpcBefore)
    return false;

  constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000u;
  const uint64_t bracketTicks = qpcAfter - qpcBefore;
  const uint64_t whole = bracketTicks / qpcFrequency;
  const uint64_t remainder = bracketTicks % qpcFrequency;
  if (whole > UINT64_MAX / kNanosecondsPerSecond) return false;
  const uint64_t wholeNs = whole * kNanosecondsPerSecond;

  // Compute ceil(remainder * 1e9 / frequency) without a wide integer.
  // `remainder < frequency` keeps the quotient bounded by 1e9. The
  // bitwise quotient/remainder recurrence avoids overflow in the product.
  uint64_t fractionalNs = 0u;
  uint64_t fractionalRemainder = 0u;
  for (int bit = 63; bit >= 0; --bit) {
    fractionalNs *= 2u;
    if (fractionalRemainder >= qpcFrequency - fractionalRemainder) {
      fractionalRemainder -= qpcFrequency - fractionalRemainder;
      ++fractionalNs;
    } else {
      fractionalRemainder += fractionalRemainder;
    }
    if ((kNanosecondsPerSecond >> bit) & 1u) {
      if (fractionalRemainder >= qpcFrequency - remainder) {
        fractionalRemainder -= qpcFrequency - remainder;
        ++fractionalNs;
      } else {
        fractionalRemainder += remainder;
      }
    }
  }
  if (fractionalRemainder != 0u) ++fractionalNs;
  if (wholeNs > UINT64_MAX - fractionalNs) return false;
  const uint64_t bracketNs = wholeNs + fractionalNs;
  const uint64_t halfBracketNs = bracketNs / 2u + bracketNs % 2u;
  if (halfBracketNs > UINT64_MAX - hostUncertaintyNs) return false;
  *outUncertaintyNs = halfBracketNs + hostUncertaintyNs;
  return true;
}

constexpr Dxmt9PeTraceReceiptTransition dxmt9PeTraceTransitionReceipt(
    uint64_t droppedBatchCount,
    Dxmt9PeTraceReceiptAction action) noexcept {
  switch (action) {
    case Dxmt9PeTraceReceiptAction::Accepted:
      return {true, 0u};
    case Dxmt9PeTraceReceiptAction::Disabled:
      return {false, 0u};
    case Dxmt9PeTraceReceiptAction::Loss:
      return {false, droppedBatchCount == UINT64_MAX
                         ? droppedBatchCount : droppedBatchCount + 1u};
  }
  return {false, droppedBatchCount};
}

// One optional PE scope around an already-existing chunk bridge operation.
// The caller gates both calls with dxmt9PeTraceEnabled(); disabled execution
// does not enter this helper or construct its TLS record storage.
bool dxmt9PeTraceChunkBegin() noexcept;
void dxmt9PeTraceChunkEnd(D9CDevice* device) noexcept;
