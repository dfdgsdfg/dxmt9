#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace dxmt9::perf {

inline constexpr std::uint64_t kLow4GiBDomainEnd = 1ull << 32;

struct LowAddressMapSummary {
  std::uint64_t cursor = 0;
  std::uint64_t mappedBytes = 0;
  std::uint64_t largestGapBytes = 0;
  bool valid = true;
};

// Pure reducer shared by the Mach walker and native tests. The Mach walker
// supplies nondecreasing intervals (sorted by begin); overlapping and
// contained ranges are coalesced against cursor. Endpoints are clipped to
// [0, 4 GiB), while reversed endpoints/overflow fail closed rather than
// producing address-space evidence. Out-of-order non-reversed input is not a
// supported contract and may be treated as already-covered overlap.
inline constexpr LowAddressMapSummary addLowAddressMappedInterval(
    LowAddressMapSummary state,
    std::uint64_t begin,
    std::uint64_t end) noexcept {
  if (!state.valid) {
    return state;
  }
  if (begin > end) {
    state.valid = false;
    return state;
  }
  if (end <= state.cursor) {
    return state;
  }
  begin = std::min(begin, kLow4GiBDomainEnd);
  end = std::min(end, kLow4GiBDomainEnd);
  if (begin >= end || end <= state.cursor) {
    return state;
  }
  const auto clippedBegin = std::max(begin, state.cursor);
  if (clippedBegin > state.cursor) {
    state.largestGapBytes = std::max(
        state.largestGapBytes, clippedBegin - state.cursor);
  }
  if (end > clippedBegin) {
    const auto bytes = end - clippedBegin;
    if (bytes > std::numeric_limits<std::uint64_t>::max() -
                    state.mappedBytes) {
      state.valid = false;
      return state;
    }
    state.mappedBytes += bytes;
    state.cursor = end;
  }
  return state;
}

inline constexpr LowAddressMapSummary finishLowAddressMapSummary(
    LowAddressMapSummary state) noexcept {
  if (!state.valid || state.cursor > kLow4GiBDomainEnd) {
    state.valid = false;
    return state;
  }
  state.largestGapBytes = std::max(
      state.largestGapBytes, kLow4GiBDomainEnd - state.cursor);
  return state;
}

}  // namespace dxmt9::perf
