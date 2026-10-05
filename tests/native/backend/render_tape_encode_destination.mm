#include "render_tape_encode_destination.hpp"
#include "../../../src/winemetal/Metal.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dxmt9::test {

CpuTapeEncodeDestinationResult compareCpuTapeEncodeDestination(
    const WMT::Buffer& acceptedBuffer, std::uint64_t acceptedOffset,
    std::uint64_t acceptedSliceSize, std::span<const std::uint8_t> expected,
    std::uint64_t maxCompareBytes) noexcept {
  using Status = CpuTapeEncodeDestinationStatus;
  static_assert(sizeof(std::uintptr_t) >= sizeof(obj_handle_t),
      "native WMT object handles must fit the pointer representation");
  constexpr auto maxArithmetic = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
  if (!acceptedBuffer.handle || expected.empty() || !expected.data() || !acceptedSliceSize ||
      !maxCompareBytes || maxCompareBytes > maxArithmetic) return {Status::InvalidRequest};
  if (acceptedSliceSize != expected.size()) return {Status::SliceSizeMismatch};
  if (acceptedOffset > std::numeric_limits<std::uint64_t>::max() - acceptedSliceSize ||
      acceptedOffset > maxArithmetic || acceptedSliceSize > maxArithmetic)
    return {Status::ArithmeticOverflow};
  if (acceptedSliceSize > maxCompareBytes) return {Status::ByteLimitExceeded};

  // The unix WMT provider stores the native object pointer as obj_handle_t.
  // unsafe_unretained also prevents an ARC build from extending its lifetime:
  // the caller's genuine accepted source/buffer owner is the only authority.
  __unsafe_unretained id<MTLBuffer> native = (__bridge id<MTLBuffer>)
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(acceptedBuffer.handle));
  @try {
    const auto allocationSize = static_cast<std::uint64_t>([native length]);
    if (acceptedOffset > allocationSize || acceptedSliceSize > allocationSize - acceptedOffset)
      return {Status::SliceOutOfBounds};
    if ([native storageMode] != MTLStorageModeShared) return {Status::UnsupportedStorage};
    const auto* contents = static_cast<const std::uint8_t*>([native contents]);
    if (!contents) return {Status::ContentsUnavailable};
    const auto address = reinterpret_cast<std::uintptr_t>(contents);
    constexpr auto maximumAddress = std::numeric_limits<std::uintptr_t>::max();
    if (acceptedOffset > maximumAddress - address ||
        acceptedSliceSize - 1u > maximumAddress - (address + acceptedOffset))
      return {Status::ArithmeticOverflow};
    if (std::memcmp(contents + static_cast<std::size_t>(acceptedOffset), expected.data(), expected.size()) != 0)
      return {Status::ByteMismatch};
    return {Status::Equal, acceptedSliceSize};
  } @catch (NSException* exception) {
    (void)exception;
    return {Status::InspectionFailed};
  }
}

} // namespace dxmt9::test
