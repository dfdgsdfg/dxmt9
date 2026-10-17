#include "device_c_provider_api.hpp"
#include "d3d9_pe_trace.hpp"

#include "dxmt9/dxmt9_trace.hpp"

#include <array>
#include <cstdint>
#include <pthread.h>
#include <unistd.h>

namespace {

constexpr uint32_t kHostMonotonicClockId = 3u;
constexpr uint64_t kHostMonotonicClockFrequency = 1'000'000'000ull;

// The PE gives us a one-call QPC bracket, not a calibration that can safely
// be extrapolated indefinitely. Align only samples inside that bracket.
bool mapQpcWithinCalibration(uint64_t qpc, uint64_t frequency,
                             const D9CTraceClockCalibration& calibration,
                             uint64_t* hostNs, double* scale,
                             double* offset,
                             uint64_t* uncertaintyNs) noexcept {
  if (!hostNs || !scale || !offset || !uncertaintyNs || frequency == 0u ||
      calibration.valid == 0u || qpc < calibration.qpcBefore ||
      qpc > calibration.qpcAfter ||
      calibration.qpcAfter < calibration.qpcBefore) {
    return false;
  }
  if (!dxmt9PeTraceCalibrationUncertainty(
          calibration.qpcBefore, calibration.qpcAfter, frequency,
          calibration.hostUncertaintyNs, uncertaintyNs)) {
    return false;
  }
  const uint64_t midpoint = calibration.qpcBefore +
      (calibration.qpcAfter - calibration.qpcBefore) / 2u;
  const __int128 delta = qpc >= midpoint
      ? static_cast<__int128>(qpc - midpoint)
      : -static_cast<__int128>(midpoint - qpc);
  const __int128 adjustment = delta * kHostMonotonicClockFrequency /
      static_cast<__int128>(frequency);
  const __int128 mapped = static_cast<__int128>(
      calibration.hostMonotonicNs) + adjustment;
  if (mapped <= 0 || mapped > static_cast<__int128>(UINT64_MAX))
    return false;
  *hostNs = static_cast<uint64_t>(mapped);
  *scale = static_cast<double>(kHostMonotonicClockFrequency) /
      static_cast<double>(frequency);
  *offset = static_cast<double>(calibration.hostMonotonicNs) -
      static_cast<double>(midpoint) * *scale;
  return true;
}

uint64_t nativeThreadId() noexcept {
  uint64_t tid = 0;
  (void)pthread_threadid_np(nullptr, &tid);
  return tid;
}

void initializeReceipt(D9CTraceBatchReceipt* receipt,
                       uint32_t status) noexcept {
  *receipt = {};
  receipt->structSize = static_cast<uint32_t>(sizeof(*receipt));
  receipt->version = D9C_TRACE_BATCH_VERSION;
  receipt->status = status;
}

}  // namespace

extern "C" int32_t dxmt9c_device_trace_batch(
    D9CDevice*, const D9CTraceBatch* batch,
    D9CTraceBatchReceipt* outReceipt) {
  if (!outReceipt) return 1;
  initializeReceipt(outReceipt, D9C_TRACE_RECEIPT_REJECTED);
  if (dxmt9c_trace_batch_validate(batch) != 0u) return 2;
  const uint64_t capturedGeneration =
      dxmt9::tracing::capture_generation();
  if (capturedGeneration == 0u) {
    outReceipt->status = D9C_TRACE_RECEIPT_DISABLED;
    return 0;
  }

  const uint64_t nativeTid = nativeThreadId();
  const uint64_t hostBefore = dxmt9::tracing::now_ns();
  std::array<dxmt9::tracing::TraceEvent,
             D9C_TRACE_BATCH_MAX_EVENTS + 1u> events{};
  uint32_t eventCount = 0u;
  if (batch->previousCalibration.valid != 0u) {
    auto& calibration = events[eventCount++];
    calibration.timestamp_ns = batch->previousCalibration.hostMonotonicNs;
    calibration.track_id = batch->previousCalibration.nativeThreadId;
    calibration.work_id = batch->previousCalibration.sourceOrder;
    calibration.scope_id = batch->previousCalibration.sourceOrder;
    // The unix collector owns source-record identity across PE and native
    // producers. sourceOrder remains the PE correlation key (work_id/scope_id).
    calibration.source_record = 0u;
    calibration.arg0 = batch->previousCalibration.qpcBefore;
    calibration.arg1 = batch->previousCalibration.qpcAfter;
    const uint64_t qpcMidpoint = batch->previousCalibration.qpcBefore +
        (batch->previousCalibration.qpcAfter -
         batch->previousCalibration.qpcBefore) / 2u;
    calibration.process_id = static_cast<uint64_t>(getpid());
    calibration.thread_id = batch->previousCalibration.nativeThreadId;
    calibration.guest_process_id = batch->guestProcessId;
    calibration.guest_thread_id = batch->guestThreadId;
    calibration.process_create_time100ns = batch->processCreateTime100ns;
    calibration.thread_create_time100ns = batch->threadCreateTime100ns;
    calibration.clock_frequency = batch->qpcFrequency;
    calibration.clock_id = kHostMonotonicClockId;
    calibration.source_timestamp = qpcMidpoint;
    calibration.source_clock_id = D9C_TRACE_CLOCK_QPC;
    calibration.calibration_id = batch->previousCalibration.sourceOrder;
    calibration.calibration_scale =
        static_cast<double>(kHostMonotonicClockFrequency) /
        static_cast<double>(batch->qpcFrequency);
    calibration.calibration_offset =
        static_cast<double>(batch->previousCalibration.hostMonotonicNs) -
        static_cast<double>(qpcMidpoint) * calibration.calibration_scale;
    calibration.event_id = static_cast<uint16_t>(
        dxmt9::tracing::EventId::PeClockCalibration);
    calibration.category = static_cast<uint8_t>(dxmt9::tracing::Category::Pe);
    calibration.phase = static_cast<uint8_t>(dxmt9::tracing::Phase::Instant);
    if (!dxmt9PeTraceCalibrationUncertainty(
            batch->previousCalibration.qpcBefore,
            batch->previousCalibration.qpcAfter, batch->qpcFrequency,
            batch->previousCalibration.hostUncertaintyNs,
            &calibration.uncertainty_ns)) {
      calibration.uncertainty_ns = UINT64_MAX;
    }
  }
  for (uint32_t i = 0; i < batch->eventCount; ++i) {
    const D9CTraceBatchEvent& source = batch->events[i];
    auto& event = events[eventCount++];
    event.timestamp_ns = source.qpcTicks;
    event.track_id = nativeTid;
    event.work_id = source.sourceOrder;
    event.scope_id = source.scopeId;
    // The scope/order ID is PE-owned. Let the unix collector assign unique
    // native source-record IDs to the begin and end rows independently.
    event.source_record = 0u;
    event.arg0 = batch->processCreateTime100ns;
    event.arg1 = batch->threadCreateTime100ns;
    event.process_id = static_cast<uint64_t>(getpid());
    event.thread_id = nativeTid;
    event.guest_process_id = batch->guestProcessId;
    event.guest_thread_id = batch->guestThreadId;
    event.process_create_time100ns = batch->processCreateTime100ns;
    event.thread_create_time100ns = batch->threadCreateTime100ns;
    event.clock_frequency = batch->qpcFrequency;
    event.clock_id = D9C_TRACE_CLOCK_QPC;
    event.source_timestamp = source.qpcTicks;
    event.source_clock_id = D9C_TRACE_CLOCK_QPC;
    uint64_t alignedNs = 0u;
    uint64_t alignedUncertaintyNs = 0u;
    if (mapQpcWithinCalibration(source.qpcTicks, batch->qpcFrequency,
                                batch->previousCalibration, &alignedNs,
                                &event.calibration_scale,
                                &event.calibration_offset,
                                &alignedUncertaintyNs)) {
      event.timestamp_ns = alignedNs;
      event.clock_id = kHostMonotonicClockId;
      event.calibration_id = batch->previousCalibration.sourceOrder;
      event.uncertainty_ns = alignedUncertaintyNs;
    }
    event.event_id = static_cast<uint16_t>(
        source.eventKind == D9C_TRACE_EVENT_SCOPE_BEGIN
            ? dxmt9::tracing::EventId::PeChunkCommitBegin
            : dxmt9::tracing::EventId::PeChunkCommitEnd);
    event.category = static_cast<uint8_t>(dxmt9::tracing::Category::Pe);
    event.phase = static_cast<uint8_t>(
        source.eventKind == D9C_TRACE_EVENT_SCOPE_BEGIN
            ? dxmt9::tracing::Phase::Begin
            : dxmt9::tracing::Phase::End);
  }
  const bool emitted = dxmt9::tracing::emit_batch_captured(
      std::span<const dxmt9::tracing::TraceEvent>(events.data(), eventCount),
      capturedGeneration,
      batch->droppedBatchCount);
  const uint64_t hostAfter = dxmt9::tracing::now_ns();
  outReceipt->status = emitted ? D9C_TRACE_RECEIPT_ACCEPTED
                               : D9C_TRACE_RECEIPT_DROPPED;
  outReceipt->nativeThreadId = nativeTid;
  outReceipt->hostClockId = kHostMonotonicClockId;
  outReceipt->hostMonotonicNs = hostBefore + (hostAfter - hostBefore) / 2u;
  outReceipt->hostUncertaintyNs = hostAfter - hostBefore;
  return 0;
}
