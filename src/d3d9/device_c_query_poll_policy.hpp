#pragma once

#include <cstdlib>
#include <cstdint>
#include <optional>

struct D9CQuery;

namespace dxmt9::d3d9 {

// No value means that the unchanged global-drain provider owns this call.
std::optional<std::int32_t> tryPollQueryReceipt(
    D9CQuery* query, void* output, std::uint32_t size, std::uint32_t flags);

inline bool queryPollReceiptsEnabled() noexcept {
  static const bool enabled = [] {
    const auto* value = std::getenv("DXMT9_QUERY_POLL_RECEIPTS");
    return value && value[0] && value[0] != '0';
  }();
  return enabled;
}

inline thread_local std::uint64_t queryReplayOrdinalTls = 0;

inline std::uint64_t queryReplayOrdinal() noexcept {
  return queryReplayOrdinalTls;
}

class ScopedQueryReplayOrdinal final {
 public:
  explicit ScopedQueryReplayOrdinal(std::uint64_t ordinal) noexcept
      : previous_(queryReplayOrdinalTls) { queryReplayOrdinalTls = ordinal; }
  ~ScopedQueryReplayOrdinal() { queryReplayOrdinalTls = previous_; }
  ScopedQueryReplayOrdinal(const ScopedQueryReplayOrdinal&) = delete;
  ScopedQueryReplayOrdinal& operator=(const ScopedQueryReplayOrdinal&) = delete;

 private:
  std::uint64_t previous_;
};

}  // namespace dxmt9::d3d9
