#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "d3d9_pe_trace.hpp"

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
  uint64_t pendingQpcBegin = 0;
  uint64_t pendingSourceOrder = 0;
  D9CTraceClockCalibration previousCalibration{};
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

}  // namespace

bool dxmt9PeTraceChunkBegin() noexcept {
  const DWORD priorError = GetLastError();
  PeTraceThreadState& state = threadState();
  bool active = false;
  if (dxmt9PeTraceCaptureEligible(dxmt9PeTraceEnabled(),
                                  state.identityValid)) {
    state.pendingQpcBegin = queryQpc();
    state.pendingSourceOrder = gPeTraceSourceOrder.fetch_add(
        1u, std::memory_order_relaxed);
    active = state.pendingQpcBegin != 0u && state.pendingSourceOrder != 0u;
    if (!active) ++state.droppedBatchCount;
  } else if (!state.identityValid) {
    ++state.droppedBatchCount;
  }
  SetLastError(priorError);
  return active;
}

void dxmt9PeTraceChunkEnd(D9CDevice* device) noexcept {
  const DWORD priorError = GetLastError();
  PeTraceThreadState& state = threadState();
  const uint64_t qpcEnd = queryQpc();
  const uint64_t qpcBegin = state.pendingQpcBegin;
  const uint64_t sourceOrder = state.pendingSourceOrder;
  state.pendingQpcBegin = 0u;
  state.pendingSourceOrder = 0u;
  if (qpcEnd == 0u || qpcBegin == 0u || sourceOrder == 0u ||
      qpcEnd < qpcBegin) {
    ++state.droppedBatchCount;
    SetLastError(priorError);
    return;
  }

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
  batch.previousCalibration = state.previousCalibration;
  batch.events[0] = {qpcBegin, sourceOrder, sourceOrder,
                     D9C_TRACE_EVENT_SCOPE_BEGIN, 0u};
  batch.events[1] = {qpcEnd, sourceOrder, sourceOrder,
                     D9C_TRACE_EVENT_SCOPE_END, 0u};

  const uint64_t calibrationQpcBefore = queryQpc();
  D9CTraceBatchReceipt receipt{};
  receipt.structSize = static_cast<uint32_t>(sizeof(receipt));
  receipt.version = D9C_TRACE_BATCH_VERSION;
  const int32_t transportStatus =
      dxmt9c_device_trace_batch(device, &batch, &receipt);
  const uint64_t calibrationQpcAfter = queryQpc();

  const bool receiptValid =
      dxmt9c_trace_batch_receipt_validate(&receipt) == 0u;
  const bool calibrationWindowValid = calibrationQpcBefore != 0u &&
      calibrationQpcAfter >= calibrationQpcBefore;
  const auto action = dxmt9PeTraceReceiptAction(
      transportStatus, receiptValid, receipt.status, calibrationWindowValid);
  const auto transition = dxmt9PeTraceTransitionReceipt(
      state.droppedBatchCount, action);
  if (action == Dxmt9PeTraceReceiptAction::Accepted) {
    state.previousCalibration = {calibrationQpcBefore,
                                 receipt.hostMonotonicNs,
                                 calibrationQpcAfter,
                                 receipt.hostUncertaintyNs,
                                 sourceOrder,
                                 receipt.nativeThreadId,
                                 1u,
                                 0u};
  } else if (!transition.keepCalibration) {
    state.previousCalibration = {};
  }
  state.droppedBatchCount = transition.droppedBatchCount;
  SetLastError(priorError);
}
