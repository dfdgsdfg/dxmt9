#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "d3d9_pe_trace.hpp"

#include "util/diagnostic_report.hpp"
#include <algorithm>
#include <cstring>
#include <array>
#include <atomic>
#include <cstdlib>

namespace {

struct PeTraceThreadState {
  uint32_t processId = 0;
  uint32_t threadId = 0;
  uint64_t processCreateTime100ns = 0;
  uint64_t threadCreateTime100ns = 0;
  uint64_t qpcFrequency = 0;
  uint64_t droppedBatchCount = 0;
  uint64_t lastSessionGeneration = 0;
  Dxmt9PeTracePointBracket pendingBegin{};
  uint64_t pendingSourceOrder = 0;
  bool identityValid = false;
};

std::atomic<uint64_t> gPeTraceSourceOrder{1};

uint64_t fileTimeValue(const FILETIME& time) noexcept {
  ULARGE_INTEGER value{};
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return value.QuadPart;
}

PeTraceThreadState& threadState() noexcept {
  static thread_local PeTraceThreadState state = []() noexcept {
    PeTraceThreadState result{};
    FILETIME processCreate{}, processExit{}, processKernel{}, processUser{};
    FILETIME threadCreate{}, threadExit{}, threadKernel{}, threadUser{};
    LARGE_INTEGER frequency{};
    result.processId = GetCurrentProcessId();
    result.threadId = GetCurrentThreadId();
    if (!GetProcessTimes(GetCurrentProcess(), &processCreate, &processExit,
                         &processKernel, &processUser) ||
        !GetThreadTimes(GetCurrentThread(), &threadCreate, &threadExit,
                        &threadKernel, &threadUser) ||
        !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
      return result;
    }
    result.processCreateTime100ns = fileTimeValue(processCreate);
    result.threadCreateTime100ns = fileTimeValue(threadCreate);
    result.qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
    result.identityValid = result.processId != 0u && result.threadId != 0u &&
                           result.processCreateTime100ns != 0u &&
                           result.threadCreateTime100ns != 0u;
    return result;
  }();
  return state;
}

uint64_t queryQpc() noexcept {
  LARGE_INTEGER value{};
  return QueryPerformanceCounter(&value) && value.QuadPart >= 0
             ? static_cast<uint64_t>(value.QuadPart)
             : 0u;
}

Dxmt9PeTracePointBracket samplePoint(D9CDevice* device) noexcept {
  Dxmt9PeTracePointBracket selected{};
  for (uint32_t i = 0; i < 8u; ++i) {
    D9CTraceClockProbe before{};
    before.structSize = static_cast<uint32_t>(sizeof(before));
    before.version = D9C_TRACE_BATCH_VERSION;
    const int32_t beforeStatus = dxmt9c_trace_clock_probe(device, &before);
    const uint64_t qpc = queryQpc();
    if (selected.qpcTicks == 0u) selected.qpcTicks = qpc;
    D9CTraceClockProbe after{};
    after.structSize = static_cast<uint32_t>(sizeof(after));
    after.version = D9C_TRACE_BATCH_VERSION;
    const int32_t afterStatus = dxmt9c_trace_clock_probe(device, &after);
    const bool beforeShape = before.structSize ==
            static_cast<uint32_t>(sizeof(before)) &&
        before.version == D9C_TRACE_BATCH_VERSION && before.reserved == 0u;
    const bool afterShape = after.structSize ==
            static_cast<uint32_t>(sizeof(after)) &&
        after.version == D9C_TRACE_BATCH_VERSION && after.reserved == 0u;
    const bool beforeAccepted = beforeStatus == 0 && beforeShape &&
        before.status == D9C_TRACE_RECEIPT_ACCEPTED &&
        before.sessionGeneration != 0u && before.nativeThreadId != 0u;
    const bool afterAccepted = afterStatus == 0 && afterShape &&
        after.status == D9C_TRACE_RECEIPT_ACCEPTED &&
        after.sessionGeneration != 0u && after.nativeThreadId != 0u;
    if (selected.sessionGeneration == 0u) {
      const D9CTraceClockProbe& identityProbe = beforeAccepted
          ? before : after;
      if (beforeAccepted || afterAccepted) {
        selected.sessionGeneration = identityProbe.sessionGeneration;
        selected.nativeThreadId = identityProbe.nativeThreadId;
      }
    }
    if (beforeStatus == 0 && afterStatus == 0 && beforeShape && afterShape &&
        before.status == D9C_TRACE_RECEIPT_DISABLED &&
        after.status == D9C_TRACE_RECEIPT_DISABLED) {
      selected.disabled = true;
      return selected;
    }
    const bool beforeInactive = before.status == D9C_TRACE_RECEIPT_DISABLED;
    const bool afterInactive = after.status == D9C_TRACE_RECEIPT_DISABLED;
    if (!selected.present && ((beforeAccepted && afterAccepted &&
         (before.sessionGeneration != after.sessionGeneration ||
          before.nativeThreadId != after.nativeThreadId)) ||
        (beforeAccepted && afterInactive) ||
        (afterAccepted && beforeInactive)))
      selected.generationMismatch = true;
    uint64_t uncertainty = 0u;
    const bool valid = beforeStatus == 0 && afterStatus == 0 && qpc != 0u &&
        beforeAccepted && afterAccepted &&
        before.sessionGeneration != 0u &&
        before.sessionGeneration == after.sessionGeneration &&
        before.nativeThreadId != 0u &&
        before.nativeThreadId == after.nativeThreadId &&
        dxmt9PeTracePointUncertainty(
            before.hostMonotonicNs, after.hostMonotonicNs,
            before.resolutionNs > after.resolutionNs
                ? before.resolutionNs : after.resolutionNs,
            threadState().qpcFrequency, &uncertainty);
    if (!valid) continue;
    const bool qualified = uncertainty <= 1000u;
    // Candidate ranking is local to one admitted identity. A restarted
    // session must never inherit a lower-error point from its predecessor.
    if (selected.sessionGeneration != before.sessionGeneration ||
        selected.nativeThreadId != before.nativeThreadId ||
        dxmt9PeTracePreferPointBracket(uncertainty, true,
                                      selected.uncertaintyNs,
                                      selected.present)) {
      selected = {qpc, before.hostMonotonicNs, after.hostMonotonicNs,
                  before.resolutionNs > after.resolutionNs
                      ? before.resolutionNs : after.resolutionNs,
                  before.sessionGeneration, before.nativeThreadId,
                  uncertainty, true, qualified, false, false};
    }
    if (qualified) break;
  }
  return selected;
}

void noteDrop(PeTraceThreadState& state) noexcept {
  if (state.droppedBatchCount != UINT64_MAX) ++state.droppedBatchCount;
}

}  // namespace

bool dxmt9PeTraceChunkBegin(D9CDevice* device) noexcept {
  const DWORD priorError = GetLastError();
  PeTraceThreadState& state = threadState();
  bool active = false;
  if (dxmt9PeTraceCaptureEligible(dxmt9PeTraceEnabled(), state.identityValid)) {
    state.pendingBegin = samplePoint(device);
    if (state.pendingBegin.disabled) {
      state.pendingBegin = {};
      state.pendingSourceOrder = 0u;
      state.droppedBatchCount = 0u;
      state.lastSessionGeneration = 0u;
      SetLastError(priorError);
      return false;
    }
    if (state.pendingBegin.sessionGeneration != 0u &&
        state.lastSessionGeneration !=
            state.pendingBegin.sessionGeneration) {
      state.droppedBatchCount = 0u;
      state.lastSessionGeneration = state.pendingBegin.sessionGeneration;
    }
    state.pendingSourceOrder = gPeTraceSourceOrder.fetch_add(
        1u, std::memory_order_relaxed);
    active = state.pendingSourceOrder != 0u &&
        state.pendingBegin.sessionGeneration != 0u &&
        state.pendingBegin.nativeThreadId != 0u;
    if (!active || !state.pendingBegin.present) noteDrop(state);
    if (!active) state.pendingBegin = {};
  } else {
    state.pendingBegin = {};
    state.pendingSourceOrder = 0u;
    if (!state.identityValid) noteDrop(state);
  }
  SetLastError(priorError);
  return active;
}

static bool dxmt9PeTraceRawRecord(const Dxmt9PeRawSample& sample,
                                 const char* schema) noexcept {
  const DWORD savedError = GetLastError();
  PeTraceThreadState& state = threadState();
  if (!dxmt9PeTraceEnabled() || !state.identityValid) {
    SetLastError(savedError);
    return false;
  }
  D9CTraceClockProbe probe{};
  if (dxmt9c_trace_clock_probe(nullptr, &probe) != 0 ||
      probe.status != D9C_TRACE_RECEIPT_ACCEPTED ||
      probe.sessionGeneration == 0u || probe.nativeThreadId == 0u) {
    SetLastError(savedError);
    return false;
  }
  D9CDiagnosticReport report{};
  report.structSize = sizeof(report);
  report.version = D9C_DIAGNOSTIC_REPORT_VERSION;
  report.encoding = D9C_DIAGNOSTIC_ENCODING_U64_LE;
  report.sessionGeneration = probe.sessionGeneration;
  report.nativeThreadId = probe.nativeThreadId;
  report.guestProcessId = state.processId;
  report.guestThreadId = state.threadId;
  report.processCreateTime100ns = state.processCreateTime100ns;
  report.threadCreateTime100ns = state.threadCreateTime100ns;
  report.sourceOrder = sample.ordinal;
  const size_t schemaSize = std::strlen(schema);
  std::memcpy(report.schema, schema, schemaSize + 1u);
  const auto fields = dxmt9PeRawSampleFields(sample);
  report.length = static_cast<uint32_t>(sizeof(fields));
  std::memcpy(report.payload, fields.data(), sizeof(fields));
  const bool accepted = dxmt9c_device_diagnostic_report(nullptr, &report) == 0;
  SetLastError(savedError);
  return accepted;
}

bool dxmt9PeTraceRawSample(const Dxmt9PeRawSample& sample) noexcept {
  return dxmt9PeTraceRawRecord(sample, "pe-pc-sample-v1");
}

bool dxmt9PeTraceRawPopulation(const Dxmt9PeRawSample& sample) noexcept {
  return dxmt9PeTraceRawRecord(sample, "pe-pc-population-v1");
}

void dxmt9PeTraceChunkEnd(D9CDevice* device) noexcept {
  const DWORD priorError = GetLastError();
  PeTraceThreadState& state = threadState();
  Dxmt9PeTracePointBracket endPoint = samplePoint(device);
  Dxmt9PeTracePointBracket beginPoint = state.pendingBegin;
  const uint64_t sourceOrder = state.pendingSourceOrder;
  state.pendingBegin = {};
  state.pendingSourceOrder = 0u;
  if (sourceOrder == 0u) {
    SetLastError(priorError);
    return;
  }
  if (sourceOrder > (UINT64_MAX - 1u) / 2u) {
    noteDrop(state);
    SetLastError(priorError);
    return;
  }
  if (beginPoint.generationMismatch || endPoint.generationMismatch) {
    noteDrop(state);
    SetLastError(priorError);
    return;
  }

  uint64_t generation = beginPoint.sessionGeneration != 0u
      ? beginPoint.sessionGeneration : endPoint.sessionGeneration;
  uint64_t nativeTid = beginPoint.nativeThreadId != 0u
      ? beginPoint.nativeThreadId : endPoint.nativeThreadId;
  if (generation == 0u || nativeTid == 0u) {
    noteDrop(state);
    SetLastError(priorError);
    return;
  }
  if ((beginPoint.sessionGeneration != 0u &&
       endPoint.sessionGeneration != 0u &&
       beginPoint.sessionGeneration != endPoint.sessionGeneration) ||
      (beginPoint.nativeThreadId != 0u && endPoint.nativeThreadId != 0u &&
       beginPoint.nativeThreadId != endPoint.nativeThreadId)) {
    noteDrop(state);
    SetLastError(priorError);
    return;
  }
  if (beginPoint.sessionGeneration != generation ||
      beginPoint.nativeThreadId != nativeTid)
    beginPoint = dxmt9PeTraceClearBracket(beginPoint);
  if (endPoint.sessionGeneration != generation ||
      endPoint.nativeThreadId != nativeTid)
    endPoint = dxmt9PeTraceClearBracket(endPoint);

  D9CTraceBatch batch{};
  batch.structSize = static_cast<uint32_t>(sizeof(batch));
  batch.version = D9C_TRACE_BATCH_VERSION;
  batch.eventCount = D9C_TRACE_BATCH_MAX_EVENTS;
  batch.guestProcessId = state.processId;
  batch.guestThreadId = state.threadId;
  batch.processCreateTime100ns = state.processCreateTime100ns;
  batch.threadCreateTime100ns = state.threadCreateTime100ns;
  batch.qpcFrequency = state.qpcFrequency;
  batch.droppedBatchCount = state.droppedBatchCount;
  batch.sessionGeneration = generation;
  batch.nativeThreadId = nativeTid;
  batch.events[0] = {beginPoint.qpcTicks, sourceOrder, sourceOrder,
                     beginPoint.hostBeforeNs, beginPoint.hostAfterNs,
                     beginPoint.hostResolutionNs,
                     D9C_TRACE_EVENT_SCOPE_BEGIN, 0u};
  batch.events[1] = {endPoint.qpcTicks, sourceOrder, sourceOrder,
                     endPoint.hostBeforeNs, endPoint.hostAfterNs,
                     endPoint.hostResolutionNs,
                     D9C_TRACE_EVENT_SCOPE_END, 0u};
  if (!beginPoint.present || !endPoint.present) {
    // Keep the admitted chunk visible as an unqualified loss event.
    noteDrop(state);
  }
  if (beginPoint.qpcTicks == 0u || endPoint.qpcTicks == 0u ||
      beginPoint.qpcTicks > endPoint.qpcTicks) {
    noteDrop(state);
    SetLastError(priorError);
    return;
  }
  batch.droppedBatchCount = state.droppedBatchCount;

  D9CTraceBatchReceipt receipt{};
  receipt.structSize = static_cast<uint32_t>(sizeof(receipt));
  receipt.version = D9C_TRACE_BATCH_VERSION;
  const int32_t transportStatus =
      dxmt9c_device_trace_batch(device, &batch, &receipt);
  const bool receiptValid =
      dxmt9c_trace_batch_receipt_validate(&receipt) == 0u;
  const auto action = dxmt9PeTraceReceiptAction(
      transportStatus, receiptValid, receipt.status);
  const auto transition = dxmt9PeTraceTransitionReceipt(
      state.droppedBatchCount, action);
  state.droppedBatchCount = transition.droppedBatchCount;
  SetLastError(priorError);
}

namespace {
void sendDiagnosticReport(const char* schema, std::string_view bytes, bool truncated) noexcept {
  if (!dxmt9PeTraceEnabled()) return;
  const std::string_view tag(schema);
  if (!tag.starts_with("dxmt9-pe") && !tag.starts_with("dxmt9-query") &&
      !tag.starts_with("dxmt9-bridge")) return;
  const DWORD savedError = GetLastError();
  auto& state = threadState();
  D9CTraceClockProbe probe{};
  if (!state.identityValid || dxmt9c_trace_clock_probe(nullptr, &probe) != 0 ||
      probe.status != D9C_TRACE_RECEIPT_ACCEPTED) {
    SetLastError(savedError); return;
  }
  D9CDiagnosticReport report{};
  report.structSize = sizeof(report);
  report.version = D9C_DIAGNOSTIC_REPORT_VERSION;
  report.encoding = D9C_DIAGNOSTIC_ENCODING_TEXT;
  report.sessionGeneration = probe.sessionGeneration;
  report.nativeThreadId = probe.nativeThreadId;
  report.guestProcessId = state.processId;
  report.guestThreadId = state.threadId;
  report.processCreateTime100ns = state.processCreateTime100ns;
  report.threadCreateTime100ns = state.threadCreateTime100ns;
  report.sourceOrder = gPeTraceSourceOrder.fetch_add(1, std::memory_order_relaxed);
  // Transport loss belongs to this thread and generation, separate from scopes.
  static thread_local uint64_t generation = 0, lost = 0;
  if (generation != probe.sessionGeneration) { generation = probe.sessionGeneration; lost = 0; }
  report.droppedReports = lost;
  const auto schemaSize = std::strlen(schema);
  report.truncated = truncated || bytes.size() >= sizeof(report.payload) || schemaSize >= sizeof(report.schema);
  report.length = static_cast<uint32_t>(std::min(bytes.size(), sizeof(report.payload) - 1));
  std::memcpy(report.payload, bytes.data(), report.length);
  std::memcpy(report.schema, schema, std::min(schemaSize, sizeof(report.schema) - 1));
  const auto status = dxmt9c_device_diagnostic_report(nullptr, &report);
  if (status == 0 || status == 4) lost = 0; // Receiver already accounted for output loss.
  else if (lost != UINT64_MAX) ++lost;
  SetLastError(savedError);
}
struct DiagnosticRegistration {
  DiagnosticRegistration() noexcept {
    dxmt9::diagnostics::consumer.store(sendDiagnosticReport, std::memory_order_release);
  }
} diagnosticRegistration;
}  // namespace
