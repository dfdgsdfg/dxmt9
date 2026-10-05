#pragma once

#include <cstdint>
#include <span>

namespace WMT { class Buffer; }

namespace dxmt9::test {

enum class CpuTapeEncodeDestinationStatus : std::uint8_t {
  Equal, InvalidRequest, ArithmeticOverflow, ByteLimitExceeded, SliceSizeMismatch,
  SliceOutOfBounds, UnsupportedStorage, ContentsUnavailable, ByteMismatch,
  InspectionFailed,
};

// Small value-only result. comparedBytes qualifies the complete requested
// slice only on Equal; failures expose no partial comparison or contents view.
struct CpuTapeEncodeDestinationResult {
  CpuTapeEncodeDestinationStatus status = CpuTapeEncodeDestinationStatus::InvalidRequest;
  std::uint64_t comparedBytes = 0u;
  bool equal() const noexcept { return status == CpuTapeEncodeDestinationStatus::Equal; }
};

// Cold synchronous observation of the actual accepted transient destination.
// PRECONDITIONS: the caller supplies a genuine native id<MTLBuffer> represented
// by WMT::Buffer, retains that buffer and its actual source lifetime throughout
// this call, supplies a valid live expected span, and excludes concurrent CPU
// or GPU writes. A nonzero numeric handle cannot authenticate those facts;
// invalid/stale object pointers are not made safe by Objective-C exceptions.
//
// Requires nonempty expected bytes covering the EXACT accepted slice, a
// positive bounded comparison limit, and Shared storage. No private/managed
// mapping, GPU synchronization, locks, waits, writes, or buffer retains occur.
// No contents pointer escapes. Equal proves one instantaneous CPU destination
// equality only: not future bytes, backing retention, GPU execution, exact
// source/origin attribution, full semantic equivalence, or pixels. Call only
// inside the genuine accepted-upload callback, not with copied input as a
// stand-in for the destination or after the caller's source borrow expires.
[[nodiscard]] CpuTapeEncodeDestinationResult compareCpuTapeEncodeDestination(
    const WMT::Buffer& acceptedBuffer, std::uint64_t acceptedOffset,
    std::uint64_t acceptedSliceSize, std::span<const std::uint8_t> expected,
    std::uint64_t maxCompareBytes) noexcept;

} // namespace dxmt9::test
