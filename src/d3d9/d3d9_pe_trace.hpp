#pragma once

#include "dxmt9/device_c.h"

#include <cstdlib>
#include <cstdint>
#include <array>

inline bool dxmt9PeTraceEnabled() noexcept {
  static const bool enabled = []() noexcept {
    const char* value = std::getenv("DXMT9_TRACE_PE");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

inline bool dxmt9PeTraceCaptureEligible(bool optedIn,
                                        bool identityValid) noexcept {
  return optedIn && identityValid;
}

enum class Dxmt9PeTraceReceiptAction : uint8_t { Accepted, Disabled, Loss };

constexpr Dxmt9PeTraceReceiptAction dxmt9PeTraceReceiptAction(
    int32_t transportStatus, bool receiptValid, uint32_t receiptStatus) noexcept {
  if (transportStatus == 0 && receiptValid &&
      receiptStatus == D9C_TRACE_RECEIPT_DISABLED)
    return Dxmt9PeTraceReceiptAction::Disabled;
  if (transportStatus == 0 && receiptValid &&
      receiptStatus == D9C_TRACE_RECEIPT_ACCEPTED)
    return Dxmt9PeTraceReceiptAction::Accepted;
  return Dxmt9PeTraceReceiptAction::Loss;
}

struct Dxmt9PeTraceReceiptTransition {
  uint64_t droppedBatchCount;
};

constexpr Dxmt9PeTraceReceiptTransition dxmt9PeTraceTransitionReceipt(
    uint64_t droppedBatchCount,
    Dxmt9PeTraceReceiptAction action) noexcept {
  if (action == Dxmt9PeTraceReceiptAction::Accepted ||
      action == Dxmt9PeTraceReceiptAction::Disabled)
    return {0u};
  return {droppedBatchCount == UINT64_MAX ? droppedBatchCount
                                           : droppedBatchCount + 1u};
}

struct Dxmt9PeTracePointBracket {
  uint64_t qpcTicks;
  uint64_t hostBeforeNs;
  uint64_t hostAfterNs;
  uint64_t hostResolutionNs;
  uint64_t sessionGeneration;
  uint64_t nativeThreadId;
  uint64_t uncertaintyNs;
  bool present;
  bool qualified;
  bool disabled;
  bool generationMismatch;
};

struct Dxmt9PeRawSample {
  uint64_t ordinal;
  uint64_t targetProcessId;
  uint64_t targetThreadId;
  uint64_t targetProcessCreateTime100ns;
  uint64_t targetThreadCreateTime100ns;
  uint64_t qpcFrequency;
  uint64_t qpcBeforeSuspend;
  uint64_t qpcAfterResume;
  uint64_t pc;
  uint64_t outcome;
  uint64_t attempts;
  uint64_t successfulSamples;
  uint64_t suspendFailures;
  uint64_t contextFailures;
  uint64_t resumeFailures;
  uint64_t omittedSamples;
};

constexpr std::array<uint64_t, 17> dxmt9PeRawSampleFields(
    const Dxmt9PeRawSample& sample) noexcept {
  return {1u, sample.ordinal, sample.targetProcessId, sample.targetThreadId,
          sample.targetProcessCreateTime100ns, sample.targetThreadCreateTime100ns,
          sample.qpcFrequency, sample.qpcBeforeSuspend, sample.qpcAfterResume,
          sample.pc, sample.outcome, sample.attempts, sample.successfulSamples,
          sample.suspendFailures, sample.contextFailures, sample.resumeFailures,
          sample.omittedSamples};
}

bool dxmt9PeTraceRawSample(const Dxmt9PeRawSample& sample) noexcept;
bool dxmt9PeTraceRawPopulation(const Dxmt9PeRawSample& sample) noexcept;

constexpr Dxmt9PeTracePointBracket dxmt9PeTraceClearBracket(
    Dxmt9PeTracePointBracket point) noexcept {
  point.hostBeforeNs = 0u;
  point.hostAfterNs = 0u;
  point.hostResolutionNs = 0u;
  point.uncertaintyNs = 0u;
  point.present = false;
  point.qualified = false;
  return point;
}

constexpr bool dxmt9PeTracePointUncertainty(
    uint64_t hostBeforeNs, uint64_t hostAfterNs, uint64_t hostResolutionNs,
    uint64_t qpcFrequency, uint64_t* outUncertaintyNs) noexcept {
  if (!outUncertaintyNs || hostBeforeNs == 0u || hostAfterNs < hostBeforeNs ||
      hostResolutionNs == 0u || qpcFrequency == 0u)
    return false;
  constexpr uint64_t kNsPerSecond = 1'000'000'000u;
  const uint64_t span = hostAfterNs - hostBeforeNs;
  const uint64_t halfSpan = span / 2u + span % 2u;
  const uint64_t qpcResolution = kNsPerSecond / qpcFrequency +
      (kNsPerSecond % qpcFrequency != 0u);
  if (halfSpan > UINT64_MAX - qpcResolution) return false;
  const uint64_t subtotal = halfSpan + qpcResolution;
  if (subtotal > UINT64_MAX - hostResolutionNs) return false;
  *outUncertaintyNs = subtotal + hostResolutionNs;
  return true;
}

// Keep the first qualifying bracket; when all are slow, retain the best one.
constexpr bool dxmt9PeTracePreferPointBracket(
    uint64_t candidateUncertaintyNs, bool candidateValid,
    uint64_t selectedUncertaintyNs, bool selectedValid) noexcept {
  constexpr uint64_t kQualificationLimitNs = 1000u;
  if (!candidateValid) return false;
  if (!selectedValid) return true;
  const bool candidateQualified = candidateUncertaintyNs <= kQualificationLimitNs;
  const bool selectedQualified = selectedUncertaintyNs <= kQualificationLimitNs;
  return candidateQualified != selectedQualified
      ? candidateQualified
      : (!selectedQualified && candidateUncertaintyNs < selectedUncertaintyNs);
}

constexpr bool dxmt9PeTraceCalibrationId(uint64_t sourceOrder,
                                         uint64_t endpoint,
                                         uint64_t* outId) noexcept {
  if (!outId || endpoint > 1u || sourceOrder == 0u ||
      sourceOrder > (UINT64_MAX - endpoint) / 2u)
    return false;
  *outId = sourceOrder * 2u + endpoint;
  return true;
}

constexpr bool dxmt9PeTraceGenerationMatches(
    uint64_t probeBeforeGeneration, uint64_t probeAfterGeneration,
    uint64_t endpointGeneration, uint64_t batchGeneration,
    uint64_t batchNativeThreadId, uint64_t probeBeforeNativeThreadId,
    uint64_t probeAfterNativeThreadId) noexcept {
  return probeBeforeGeneration != 0u &&
      probeBeforeGeneration == probeAfterGeneration &&
      probeBeforeGeneration == endpointGeneration &&
      probeBeforeGeneration == batchGeneration &&
      batchNativeThreadId != 0u &&
      batchNativeThreadId == probeBeforeNativeThreadId &&
      batchNativeThreadId == probeAfterNativeThreadId;
}

bool dxmt9PeTraceChunkBegin(D9CDevice* device) noexcept;
void dxmt9PeTraceChunkEnd(D9CDevice* device) noexcept;
