#pragma once

#include "core_constants.hpp"
#include "query_end_marker.hpp"
#include "query_occlusion.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace dxmt9 {
class Device;
}

namespace dxmt9::core {

class Device;

// A by-value, core-owned view of one Query issue.  The source Query is not
// retained: a snapshot keeps the issue generation and the backend receipt (or
// result owner) alive so reissuing or destroying the public Query cannot
// redirect a later poll.
struct QueryPollSnapshot {
  const Device* owner = nullptr;
  u64 identity = 0;
  QueryType type = QueryType::Event;
  u64 issueOrdinal = 0;
  u64 issueDeviceEpoch = 0;
  bool active = false;
  bool failed = false;
  u64 issuedSequenceId = 0;
  std::optional<u64> resolvedValue;
  query::EndReceipt backendFence{};
  std::shared_ptr<query::OcclusionResult> occlusionResult;
  std::shared_ptr<dxmt9::Device> backend;

  // Only constants/pre-issue snapshots and receipts without a coalescing
  // marker are eligible for this independent poll.  A marker is deliberately
  // rejected so replay can retain the legacy Query until its writer binds.
  bool eligible = false;

  bool constant() const noexcept {
    return type == QueryType::TimestampFreq ||
           type == QueryType::TimestampDisjoint;
  }
  bool preIssue() const noexcept { return issueOrdinal == 0; }
  bool deferredBackendFence() const noexcept {
    return static_cast<bool>(backendFence.marker);
  }
};

// Pure D3D9 query result-byte formatting. Completion/failure is intentionally
// handled by the caller; this function only preserves the old write contract.
inline std::size_t queryPollResultBytes(QueryType type) noexcept {
  switch (type) {
  case QueryType::TimestampFreq:
  case QueryType::Timestamp: return sizeof(u64);
  case QueryType::TimestampDisjoint:
  case QueryType::Occlusion: return sizeof(u32);
  case QueryType::Event: return 0;  // Preserve the current reference contract.
  }
  return 0;
}

inline HResult formatQueryPollResult(QueryType type, std::optional<u64> value,
                                     void* output, std::size_t size) noexcept {
  if (type == QueryType::TimestampFreq) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64*>(output) = 1000000000ull;
    }
    return S_OK;
  }
  if (type == QueryType::TimestampDisjoint) {
    if (output && size >= sizeof(u32)) {
      *static_cast<u32*>(output) = 0u;
    }
    return S_OK;
  }
  if (type == QueryType::Event) {
    return S_OK;
  }
  const u64 resolved = value.value_or(0ull);
  if (type == QueryType::Occlusion) {
    if (!output || size == 0) {
      return S_OK;
    }
    if (size >= sizeof(u32)) {
      *static_cast<u32*>(output) = static_cast<u32>(
          std::min<u64>(resolved, std::numeric_limits<u32>::max()));
    }
    return S_OK;
  }
  if (type == QueryType::Timestamp) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64*>(output) = resolved;
    }
    return S_OK;
  }
  return D3DERR_NOTAVAILABLE;
}

}  // namespace dxmt9::core
