/* Bounded report transport. No pointer or native SDK type crosses the ABI. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define D9C_DIAGNOSTIC_REPORT_VERSION 1u
#define D9C_DIAGNOSTIC_REPORT_BYTES 4096u
typedef struct D9CDiagnosticReport {
  uint32_t structSize;
  uint32_t version;
  uint32_t length;
  uint32_t truncated;
  uint64_t sessionGeneration;
  uint64_t nativeThreadId;
  uint64_t guestProcessId;
  uint64_t guestThreadId;
  uint64_t processCreateTime100ns;
  uint64_t threadCreateTime100ns;
  uint64_t sourceOrder;
  uint64_t droppedReports;
  char schema[128];
  char payload[D9C_DIAGNOSTIC_REPORT_BYTES];
} D9CDiagnosticReport;
static inline uint32_t dxmt9c_diagnostic_report_validate(const D9CDiagnosticReport* r) {
  if (!r || r->structSize != sizeof(*r) || r->version != D9C_DIAGNOSTIC_REPORT_VERSION) return 1;
  if (r->length >= D9C_DIAGNOSTIC_REPORT_BYTES || r->truncated > 1) return 2;
  if (!r->sessionGeneration || !r->nativeThreadId || !r->guestProcessId || !r->guestThreadId ||
      !r->processCreateTime100ns || !r->threadCreateTime100ns || !r->sourceOrder) return 3;
  if (r->schema[0] == 0) return 4;
  size_t i = 0;
  while (i < sizeof(r->schema) && r->schema[i]) {
    const unsigned char c = (unsigned char)r->schema[i++];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':' || c == '/')) return 4;
  }
  if (i == sizeof(r->schema) || r->payload[r->length] != 0) return 4;
  for (i = 0; i < r->length; ++i) if (r->payload[i] == 0) return 4;
  return 0;
}
