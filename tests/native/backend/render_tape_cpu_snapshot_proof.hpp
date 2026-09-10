#pragma once

#include "dxmt9/core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace dxmt9::test {

// A live Pool view is observation-only. It is not a commit-time capture and
// must never be installed into a DrawParam's bindingSnapshotRange.
struct CpuTapeLiveBufferView {
  std::uint64_t metalHandle = 0u;
  std::uint64_t contentsAddress = 0u;
  std::uint64_t byteSize = 0u;
  std::uint64_t contentRevision = 0u;
};

enum class CpuTapeBufferObservationRoute { Reject, Captured, Live };

inline constexpr CpuTapeBufferObservationRoute cpuTapeBufferObservationRoute(
    bool versioned, bool snapshotPresent, bool backingValid) noexcept {
  if (!backingValid || versioned != snapshotPresent)
    return CpuTapeBufferObservationRoute::Reject;
  return versioned ? CpuTapeBufferObservationRoute::Captured
                   : CpuTapeBufferObservationRoute::Live;
}

// Cold, bounded proof cache. Entries are valid only while both physical bytes
// and their expected logical shadow remain immutable. The event executor
// invalidates this owner before every mutation, definition and destruction.
class CpuTapeSnapshotProofCache {
public:
  static constexpr std::size_t kCapacity = 512u;
  void invalidate() noexcept { entries_.clear(); }
  std::size_t size() const noexcept { return entries_.size(); }

  bool prove(core::Handle buffer, const core::DrawBufferBindingSnapshot& snapshot,
             std::span<const std::uint8_t> expected,
             std::uint64_t& contentDigest) {
    if (!snapshot.valid()) return false;
    return proveBytes(buffer, snapshot.contentsAddress, snapshot.byteSize,
                      snapshot.contentRevision, expected, contentDigest);
  }

  bool proveLive(core::Handle buffer, const CpuTapeLiveBufferView& view,
                 std::span<const std::uint8_t> expected,
                 std::uint64_t& contentDigest) {
    if (!view.metalHandle) return false;
    return proveBytes(buffer, view.contentsAddress, view.byteSize,
                      view.contentRevision, expected, contentDigest);
  }

private:
  bool proveBytes(core::Handle buffer, std::uint64_t address,
                  std::uint64_t bytes, std::uint64_t revision,
                  std::span<const std::uint8_t> expected,
                  std::uint64_t& contentDigest) {
    if (!buffer || !address || !bytes || bytes != expected.size() ||
        bytes > std::numeric_limits<std::size_t>::max() ||
        address > std::numeric_limits<std::uintptr_t>::max() - bytes) return false;
    for (const auto& entry : entries_) {
      if (entry.buffer == buffer && entry.address == address &&
          entry.bytes == bytes && entry.revision == revision) {
        contentDigest = entry.digest;
        return true;
      }
    }
    if (entries_.size() == kCapacity) return false;
    const auto* actual = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(address));
    if (std::memcmp(actual, expected.data(), expected.size()) != 0) return false;
    std::uint64_t digest = 1469598103934665603ull;
    for (std::size_t index = 0u; index < expected.size(); ++index) {
      digest ^= actual[index];
      digest *= 1099511628211ull;
    }
    entries_.push_back({buffer, address, bytes, revision, digest});
    contentDigest = digest;
    return true;
  }

private:
  struct Entry {
    core::Handle buffer;
    std::uint64_t address, bytes, revision, digest;
  };
  std::vector<Entry> entries_;
};

} // namespace dxmt9::test
