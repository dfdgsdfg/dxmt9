#include "device_c_provider_api.hpp"
#include "d3d9_pe_trace.hpp"

#include "dxmt9/dxmt9_trace.hpp"
#include "dxmt9/observations/diagnostic_sink.hpp"

#include <array>
#include <cstdint>
#include <mach/mach_time.h>
#include <pthread.h>
#include <unistd.h>

namespace {

constexpr uint32_t kHostMonotonicClockId = 3u;
constexpr uint64_t kHostMonotonicClockFrequency = 1'000'000'000ull;

uint64_t nativeThreadId() noexcept {
  uint64_t tid = 0;
  (void)pthread_threadid_np(nullptr, &tid);
  return tid;
}

uint64_t hostClockResolutionNs() noexcept {
  static const uint64_t resolution = []() noexcept {
    mach_timebase_info_data_t scale{};
    if (mach_timebase_info(&scale) != KERN_SUCCESS || scale.denom == 0u)
      return 0u;
    return scale.numer / scale.denom +
        (scale.numer % scale.denom != 0u);
  }();
  return resolution;
}

void initializeReceipt(D9CTraceBatchReceipt* receipt,
                       uint32_t status) noexcept {
  *receipt = {};
  receipt->structSize = static_cast<uint32_t>(sizeof(*receipt));
  receipt->version = D9C_TRACE_BATCH_VERSION;
  receipt->status = status;
}

bool mapPointBracket(const D9CTraceBatchEvent& source, uint64_t qpcFrequency,
                     uint64_t endpoint,
                     uint64_t* hostNs, uint64_t* uncertaintyNs,
                     double* scale, double* offset) noexcept {
  if (!hostNs || !uncertaintyNs || !scale || !offset ||
      source.hostBeforeNs == 0u || source.hostAfterNs < source.hostBeforeNs ||
      source.hostResolutionNs == 0u || qpcFrequency == 0u || endpoint > 1u)
    return false;
  if (!dxmt9PeTracePointUncertainty(source.hostBeforeNs, source.hostAfterNs,
                                   source.hostResolutionNs, qpcFrequency,
                                   uncertaintyNs))
    return false;
  const uint64_t midpoint = source.hostBeforeNs +
      (source.hostAfterNs - source.hostBeforeNs) / 2u;
  if (midpoint == 0u) return false;
  *hostNs = midpoint;
  *scale = static_cast<double>(kHostMonotonicClockFrequency) /
      static_cast<double>(qpcFrequency);
  *offset = static_cast<double>(midpoint) -
      static_cast<double>(source.qpcTicks) * *scale;
  return true;
}

}  // namespace

extern "C" int32_t dxmt9c_trace_clock_probe(
    D9CDevice*, D9CTraceClockProbe* outProbe) {
  if (!outProbe) return 1;
  *outProbe = {};
  outProbe->structSize = static_cast<uint32_t>(sizeof(*outProbe));
  outProbe->version = D9C_TRACE_BATCH_VERSION;
  const uint64_t generation = dxmt9::tracing::capture_generation();
  if (generation == 0u) {
    outProbe->status = D9C_TRACE_RECEIPT_DISABLED;
    return 0;
  }
  const uint64_t tid = nativeThreadId();
  const uint64_t resolution = hostClockResolutionNs();
  const uint64_t hostNs = dxmt9::tracing::now_ns();
  if (tid == 0u || resolution == 0u || hostNs == 0u ||
      generation != dxmt9::tracing::capture_generation()) {
    outProbe->status = D9C_TRACE_RECEIPT_DROPPED;
    return 0;
  }
  outProbe->status = D9C_TRACE_RECEIPT_ACCEPTED;
  outProbe->sessionGeneration = generation;
  outProbe->nativeThreadId = tid;
  outProbe->hostMonotonicNs = hostNs;
  outProbe->resolutionNs = resolution;
  return 0;
}

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
  if (capturedGeneration != batch->sessionGeneration) {
    outReceipt->status = D9C_TRACE_RECEIPT_DROPPED;
    return 0;
  }

  const uint64_t nativeTid = nativeThreadId();
  if (nativeTid == 0u || nativeTid != batch->nativeThreadId) {
    outReceipt->status = D9C_TRACE_RECEIPT_DROPPED;
    return 0;
  }
  std::array<dxmt9::tracing::TraceEvent, D9C_TRACE_BATCH_MAX_EVENTS> events{};
  for (uint32_t i = 0; i < batch->eventCount; ++i) {
    const D9CTraceBatchEvent& source = batch->events[i];
    auto& event = events[i];
    event.timestamp_ns = source.qpcTicks;
    event.track_id = 0u;
    event.work_id = source.sourceOrder;
    event.scope_id = source.scopeId;
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
    uint64_t uncertaintyNs = 0u;
    if (dxmt9PeTraceCalibrationId(source.sourceOrder, i,
                                    &event.calibration_id) &&
        mapPointBracket(source, batch->qpcFrequency, i,
                        &alignedNs, &uncertaintyNs,
                        &event.calibration_scale,
                        &event.calibration_offset)) {
      event.timestamp_ns = alignedNs;
      event.clock_id = kHostMonotonicClockId;
      event.uncertainty_ns = uncertaintyNs;
      event.calibration_source_first = source.qpcTicks;
      event.calibration_host_first_ns = source.hostBeforeNs;
      event.calibration_source_second = source.qpcTicks;
      event.calibration_host_second_ns = source.hostAfterNs;
      event.calibration_valid_begin = source.qpcTicks;
      event.calibration_valid_end = source.qpcTicks;
      event.calibration_resolution_ns = source.hostResolutionNs;
    } else {
      event.calibration_id = 0u;
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
      std::span<const dxmt9::tracing::TraceEvent>(events.data(),
                                                  batch->eventCount),
      batch->sessionGeneration, batch->droppedBatchCount);
  outReceipt->status = emitted ? D9C_TRACE_RECEIPT_ACCEPTED
                               : D9C_TRACE_RECEIPT_DROPPED;
  outReceipt->nativeThreadId = nativeTid;
  outReceipt->hostClockId = kHostMonotonicClockId;
  outReceipt->hostMonotonicNs = dxmt9::tracing::now_ns();
  outReceipt->hostUncertaintyNs = hostClockResolutionNs();
  if (outReceipt->hostUncertaintyNs == 0u) outReceipt->hostUncertaintyNs = 1u;
  return 0;
}

extern "C" int32_t dxmt9c_device_diagnostic_report(
    D9CDevice*, const D9CDiagnosticReport* report) {
  if (dxmt9c_diagnostic_report_validate(report)) return 2;
  using namespace dxmt9;
  const auto generation = tracing::capture_generation();
  if (!generation) return 0;
  if (generation != report->sessionGeneration || nativeThreadId() != report->nativeThreadId) return 3;
  observations::DiagnosticContext context;
  context.generation = generation;
  context.record = report->sourceOrder;
  // A received summary is not a QPC sample endpoint.
  tracing::TraceEvent identity{};
  identity.guest_process_id = report->guestProcessId;
  identity.guest_thread_id = report->guestThreadId;
  identity.process_create_time100ns = report->processCreateTime100ns;
  identity.thread_create_time100ns = report->threadCreateTime100ns;
  if (report->droppedReports) {
    tracing::TraceEvent invalid{};
    invalid.category = static_cast<uint8_t>(tracing::Category::Count);
    (void)tracing::emit_batch_captured({&invalid, 1}, generation, report->droppedReports);
  }
  return observations::report(observations::Source::PeReport, report->schema,
      {report->payload, report->length}, context, report->truncated, &identity, true) ? 0 : 4;
}
