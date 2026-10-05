#include "render_tape_encode_destination.hpp"
#include "../../../src/winemetal/Metal.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using dxmt9::test::compareCpuTapeEncodeDestination;
using Result = dxmt9::test::CpuTapeEncodeDestinationResult;
using Status = dxmt9::test::CpuTapeEncodeDestinationStatus;
std::size_t checks = 0u;

static_assert(std::is_trivially_copyable_v<Result>);
static_assert(sizeof(Result) <= 16u);

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

void checkFailure(Result result, Status expected, std::string_view message) {
  check(result.status == expected && !result.equal() && !result.comparedBytes, message);
}

struct DeviceOwner {
  __strong id<MTLDevice> device = nil;
};

void realSharedAndPrivateDestinations(id<MTLDevice> nativeDevice) {
  // These are genuine native allocations, never fake recorder handles. The
  // WMT references retain them across every call; no command buffer, queue,
  // GPU writer, source/origin label or GPU synchronization is fabricated.
  WMT::Device device{static_cast<obj_handle_t>(
      reinterpret_cast<std::uintptr_t>((__bridge void*)nativeDevice))};
  std::array<std::uint8_t, 64u> seed{};
  for (std::size_t index = 0u; index != seed.size(); ++index)
    seed[index] = static_cast<std::uint8_t>(index + 1u);
  WMTBufferInfo sharedInfo{};
  sharedInfo.length = seed.size();
  sharedInfo.options = WMTResourceStorageModeShared;
  sharedInfo.memory.set(seed.data());
  auto shared = device.newBuffer(sharedInfo);
  check(bool(shared), "real WMT Shared destination allocation succeeds");
  __unsafe_unretained id<MTLBuffer> nativeShared = (__bridge id<MTLBuffer>)
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(shared.handle));
  [nativeShared setLabel:@"Render Tape accepted Shared destination"];
  check([nativeShared storageMode] == MTLStorageModeShared && [nativeShared length] == seed.size(),
      "real accepted destination has Shared CPU-visible storage and actual full extent");
  const auto expected = std::span<const std::uint8_t>(seed).subspan(8u, 8u);
  const auto full = compareCpuTapeEncodeDestination(shared, 0u, seed.size(), seed, seed.size());
  const auto slice = compareCpuTapeEncodeDestination(shared, 8u, expected.size(), expected, expected.size());
  const auto tail = std::span<const std::uint8_t>(seed).last(8u);
  check(full.equal() && full.comparedBytes == seed.size() && slice.equal() &&
      slice.comparedBytes == expected.size() &&
      compareCpuTapeEncodeDestination(shared, seed.size() - tail.size(), tail.size(), tail, tail.size()).equal(),
      "helper compares actual full destination and exact offset slices including the allocation's last byte");
  std::array<std::uint8_t, 8u> wrong{};
  std::copy(expected.begin(), expected.end(), wrong.begin());
  wrong[3u] ^= 0xffu;
  checkFailure(compareCpuTapeEncodeDestination(shared, 8u, wrong.size(), wrong, wrong.size()), Status::ByteMismatch,
      "one changed expected input byte distinguishes actual destination equality from observed-input-only digest");
  auto* contents = static_cast<std::uint8_t*>([nativeShared contents]);
  check(contents != nullptr && std::memcmp(contents, seed.data(), seed.size()) == 0,
      "comparison itself never writes or changes accepted destination bytes");
  contents[11u] ^= 0xffu;
  checkFailure(compareCpuTapeEncodeDestination(shared, 8u, expected.size(), expected, expected.size()), Status::ByteMismatch,
      "changed physical destination with unchanged upload input fails the immediate native byte comparison");
  check(slice.equal() && slice.comparedBytes == expected.size(),
      "an earlier value result remains only a historical observation, never future destination byte authority");
  contents[11u] ^= 0xffu;
  check(compareCpuTapeEncodeDestination(shared, 8u, expected.size(), expected, expected.size()).equal(),
      "a fresh comparison rechecks the actual destination rather than caching earlier equality");

  const WMT::Buffer nullBuffer{};
  checkFailure(compareCpuTapeEncodeDestination(nullBuffer, 0u, expected.size(), expected, expected.size()),
      Status::InvalidRequest, "null buffer rejects without messaging any invalid object");
  checkFailure(compareCpuTapeEncodeDestination(shared, 0u, 0u, {}, 8u), Status::InvalidRequest,
      "empty input and zero accepted slice grant no vacuous equality");
  checkFailure(compareCpuTapeEncodeDestination(shared, 0u, expected.size(), {}, 8u), Status::InvalidRequest,
      "empty expected input cannot stand in for a nonempty accepted slice");
  checkFailure(compareCpuTapeEncodeDestination(shared, 0u, 0u, expected, 8u), Status::InvalidRequest,
      "zero slice cannot silently compare a shorter or absent destination");
  checkFailure(compareCpuTapeEncodeDestination(shared, 8u, expected.size(), expected, 0u), Status::InvalidRequest,
      "zero comparison budget is not an implicit default authorization");
  checkFailure(compareCpuTapeEncodeDestination(shared, 8u, expected.size(), expected,
      std::numeric_limits<std::uint64_t>::max()), Status::InvalidRequest,
      "malformed unrepresentable byte limit rejects before native inspection");
  checkFailure(compareCpuTapeEncodeDestination(shared, 8u, expected.size(), expected, expected.size() - 1u),
      Status::ByteLimitExceeded, "one-under byte limit rejects the entire slice without partial comparison");
  checkFailure(compareCpuTapeEncodeDestination(shared, 8u, expected.size() + 1u, expected, 16u),
      Status::SliceSizeMismatch, "expected input must cover the entire exact accepted slice, not a shorter prefix");
  checkFailure(compareCpuTapeEncodeDestination(shared, 8u, expected.size() - 1u, expected, 16u),
      Status::SliceSizeMismatch, "expected input cannot exceed its purported accepted slice");
  checkFailure(compareCpuTapeEncodeDestination(shared, seed.size(), expected.size(), expected, 16u),
      Status::SliceOutOfBounds, "offset at allocation end rejects a nonempty destination slice");
  checkFailure(compareCpuTapeEncodeDestination(shared, seed.size() - 4u, expected.size(), expected, 16u),
      Status::SliceOutOfBounds, "slice end must fit the real buffer length, not only caller-provided extent");
  checkFailure(compareCpuTapeEncodeDestination(shared, seed.size() + 1u, expected.size(), expected, 16u),
      Status::SliceOutOfBounds, "offset beyond actual allocation rejects before contents access");
  checkFailure(compareCpuTapeEncodeDestination(shared, std::numeric_limits<std::uint64_t>::max() - 3u,
      expected.size(), expected, 16u), Status::ArithmeticOverflow,
      "offset plus accepted slice cannot wrap before native bounds checks");
  checkFailure(compareCpuTapeEncodeDestination(shared,
      static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) + 1u,
      expected.size(), expected, 16u), Status::ArithmeticOverflow,
      "pointer arithmetic must remain representable even without uint64 addition overflow");

  WMTBufferInfo privateInfo{};
  privateInfo.length = seed.size();
  privateInfo.options = WMTResourceStorageModePrivate;
  auto privateBuffer = device.newBuffer(privateInfo);
  check(bool(privateBuffer), "real WMT Private destination allocation succeeds");
  __unsafe_unretained id<MTLBuffer> nativePrivate = (__bridge id<MTLBuffer>)
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(privateBuffer.handle));
  [nativePrivate setLabel:@"Render Tape rejected Private destination"];
  check([nativePrivate storageMode] == MTLStorageModePrivate, "negative storage fixture is genuinely Private");
  checkFailure(compareCpuTapeEncodeDestination(privateBuffer, 8u, expected.size(), expected, expected.size()),
      Status::UnsupportedStorage, "Private destination rejects without mapping, requesting contents or inventing GPU synchronization");

  std::size_t cases = 0u;
  for (std::uint64_t offset = 0u; offset != seed.size() + 2u; ++offset)
    for (std::size_t size = 1u; size != 5u; ++size)
      for (std::uint64_t limit = 1u; limit != 5u; ++limit) {
        const bool inBounds = offset <= seed.size() && size <= seed.size() - offset;
        const auto bytes = std::span<const std::uint8_t>(seed).subspan(inBounds ? offset : 0u, size);
        const auto wanted = size > limit ? Status::ByteLimitExceeded :
            inBounds ? Status::Equal : Status::SliceOutOfBounds;
        const auto result = compareCpuTapeEncodeDestination(shared, offset, size, bytes, limit);
        check(result.status == wanted && result.equal() == (wanted == Status::Equal) &&
            result.comparedBytes == (wanted == Status::Equal ? size : 0u),
            "bounded real-buffer offset/extent/budget domain rejects before comparison or qualifies exactly its full slice");
        ++cases;
      }
  check(cases == 1056u && std::memcmp(contents, seed.data(), seed.size()) == 0,
      "bounded native destination checks exhaust their domain without helper writes or retained contents views");
}

} // namespace

int main() {
  @autoreleasepool {
    DeviceOwner owner{MTLCreateSystemDefaultDevice()};
    if (!owner.device) {
      std::cerr << "SKIP: no Metal device\n";
      return 77;
    }
    try {
      realSharedAndPrivateDestinations(owner.device);
      std::cout << "encode destination native checks: " << checks << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << "encode destination native failure: " << error.what() << '\n';
      return 1;
    }
  }
}
