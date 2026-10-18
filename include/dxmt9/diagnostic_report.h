/* Bounded report transport. No pointer or native SDK type crosses the ABI. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define D9C_DIAGNOSTIC_REPORT_VERSION 2u
#define D9C_DIAGNOSTIC_REPORT_BYTES 4096u
#define D9C_PE_RAW_SAMPLE_BYTES 136u
typedef enum D9CDiagnosticEncoding {
  D9C_DIAGNOSTIC_ENCODING_TEXT = 1u,
  D9C_DIAGNOSTIC_ENCODING_U64_LE = 4u
} D9CDiagnosticEncoding;
typedef struct D9CDiagnosticReport {
  uint32_t structSize;
  uint32_t version;
  uint32_t length;
  uint32_t truncated;
  uint32_t encoding;
  uint32_t reserved;
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
  if (r->length >= D9C_DIAGNOSTIC_REPORT_BYTES || r->truncated > 1 || r->reserved != 0u) return 2;
  if (!r->sessionGeneration || !r->nativeThreadId || !r->guestProcessId || !r->guestThreadId ||
      !r->processCreateTime100ns || !r->threadCreateTime100ns || !r->sourceOrder) return 3;
  if (r->schema[0] == 0) return 4;
  size_t i = 0;
  while (i < sizeof(r->schema) && r->schema[i]) {
    const unsigned char c = (unsigned char)r->schema[i++];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':' || c == '/')) return 4;
  }
  if (i == sizeof(r->schema)) return 4;
  if (r->encoding == D9C_DIAGNOSTIC_ENCODING_TEXT) {
    if (r->payload[r->length] != 0) return 4;
    for (i = 0; i < r->length; ++i) if (r->payload[i] == 0) return 4;
  } else if (r->encoding == D9C_DIAGNOSTIC_ENCODING_U64_LE) {
    if (r->length == 0u || (r->length % 8u) != 0u) return 4;
    const char *sampleSchema = "pe-pc-sample-v1";
    const char *populationSchema = "pe-pc-population-v1";
    size_t schemaIndex = 0u;
    while (r->schema[schemaIndex] && sampleSchema[schemaIndex] &&
           r->schema[schemaIndex] == sampleSchema[schemaIndex]) ++schemaIndex;
    const int isSample = !r->schema[schemaIndex] && !sampleSchema[schemaIndex];
    schemaIndex = 0u;
    while (r->schema[schemaIndex] && populationSchema[schemaIndex] &&
           r->schema[schemaIndex] == populationSchema[schemaIndex]) ++schemaIndex;
    const int isPopulation = !r->schema[schemaIndex] && !populationSchema[schemaIndex];
    if (isSample || isPopulation) {
      if (r->length != D9C_PE_RAW_SAMPLE_BYTES) return 4;
      uint64_t recordVersion = 0u;
      for (size_t byte = 0u; byte < 8u; ++byte)
        recordVersion |= (uint64_t)(unsigned char)r->payload[byte] << (byte * 8u);
      if (recordVersion != 1u) return 4;
    }
  } else return 4;
  return 0;
}
