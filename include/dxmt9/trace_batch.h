/* Versioned, pointer-free PE-to-unix diagnostic batch. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D9C_TRACE_BATCH_VERSION 1u
#define D9C_TRACE_BATCH_MAX_EVENTS 2u
#define D9C_TRACE_CLOCK_QPC 0x44585150u /* "DXQP"; source is scoped by process incarnation. */

typedef enum D9CTraceBatchEventKind {
  D9C_TRACE_EVENT_SCOPE_BEGIN = 1u,
  D9C_TRACE_EVENT_SCOPE_END = 2u,
} D9CTraceBatchEventKind;

typedef enum D9CTraceBatchReceiptStatus {
  D9C_TRACE_RECEIPT_UNSET = 0u,
  D9C_TRACE_RECEIPT_ACCEPTED = 1u,
  D9C_TRACE_RECEIPT_DISABLED = 2u,
  D9C_TRACE_RECEIPT_REJECTED = 3u,
  D9C_TRACE_RECEIPT_DROPPED = 4u,
} D9CTraceBatchReceiptStatus;

typedef struct D9CTraceClockCalibration {
  uint64_t qpcBefore;
  uint64_t hostMonotonicNs;
  uint64_t qpcAfter;
  uint64_t hostUncertaintyNs;
  uint64_t sourceOrder;
  uint64_t nativeThreadId;
  uint32_t valid;
  uint32_t reserved;
} D9CTraceClockCalibration;

typedef struct D9CTraceBatchEvent {
  uint64_t qpcTicks;
  uint64_t sourceOrder;
  uint64_t scopeId;
  uint32_t eventKind;
  uint32_t reserved;
} D9CTraceBatchEvent;

typedef struct D9CTraceBatch {
  uint32_t structSize;
  uint32_t version;
  uint32_t eventCount;
  uint32_t flags;
  uint32_t guestProcessId;
  uint32_t guestThreadId;
  uint64_t processCreateTime100ns;
  uint64_t threadCreateTime100ns;
  uint64_t qpcFrequency;
  uint64_t droppedBatchCount;
  D9CTraceClockCalibration previousCalibration;
  D9CTraceBatchEvent events[D9C_TRACE_BATCH_MAX_EVENTS];
} D9CTraceBatch;

typedef struct D9CTraceBatchReceipt {
  uint32_t structSize;
  uint32_t version;
  uint32_t status;
  uint32_t hostClockId;
  uint32_t reserved;
  uint64_t nativeThreadId;
  uint64_t hostMonotonicNs;
  uint64_t hostUncertaintyNs;
} D9CTraceBatchReceipt;

/* Shared validation for PE, unix and native fixtures. Zero means valid. */
static inline uint32_t dxmt9c_trace_batch_validate(const D9CTraceBatch *batch) {
  if (batch == 0) return 1u;
  if (batch->structSize != (uint32_t)sizeof(D9CTraceBatch)) return 2u;
  if (batch->version != D9C_TRACE_BATCH_VERSION) return 3u;
  if (batch->eventCount != D9C_TRACE_BATCH_MAX_EVENTS) return 4u;
  if (batch->flags != 0u || batch->guestProcessId == 0u ||
      batch->guestThreadId == 0u || batch->qpcFrequency == 0u ||
      batch->processCreateTime100ns == 0u ||
      batch->threadCreateTime100ns == 0u) return 5u;
  if (batch->events[0].eventKind != D9C_TRACE_EVENT_SCOPE_BEGIN ||
      batch->events[1].eventKind != D9C_TRACE_EVENT_SCOPE_END ||
      batch->events[0].reserved != 0u || batch->events[1].reserved != 0u ||
      batch->events[0].sourceOrder == 0u ||
      batch->events[0].sourceOrder != batch->events[1].sourceOrder ||
      batch->events[0].scopeId == 0u ||
      batch->events[0].scopeId != batch->events[1].scopeId ||
      batch->events[0].qpcTicks > batch->events[1].qpcTicks) return 6u;
  if (batch->previousCalibration.valid > 1u) return 7u;
  if (batch->previousCalibration.valid != 0u &&
      (batch->previousCalibration.qpcAfter <
           batch->previousCalibration.qpcBefore ||
       batch->previousCalibration.hostMonotonicNs == 0u ||
       batch->previousCalibration.hostUncertaintyNs == 0u ||
       batch->previousCalibration.sourceOrder == 0u ||
       batch->previousCalibration.nativeThreadId == 0u ||
       batch->previousCalibration.reserved != 0u)) return 8u;
  return 0u;
}

static inline uint32_t dxmt9c_trace_batch_receipt_validate(
    const D9CTraceBatchReceipt *receipt) {
  if (receipt == 0) return 1u;
  if (receipt->structSize != (uint32_t)sizeof(D9CTraceBatchReceipt)) return 2u;
  if (receipt->version != D9C_TRACE_BATCH_VERSION) return 3u;
  if (receipt->status > D9C_TRACE_RECEIPT_DROPPED) return 4u;
  if (receipt->status == D9C_TRACE_RECEIPT_ACCEPTED &&
      (receipt->nativeThreadId == 0u || receipt->hostClockId == 0u ||
       receipt->hostMonotonicNs == 0u || receipt->hostUncertaintyNs == 0u)) return 5u;
  return 0u;
}

#ifdef __cplusplus
}
#endif
