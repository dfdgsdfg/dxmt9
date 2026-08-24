#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>

namespace dxmt9::d3d9::pe {

enum class PublicAllocationResult : std::uint8_t {
  Completed,
  Rejected,
  OutOfMemory,
};

// Run the allocation-bearing part of a public PE entry point without allowing
// a recoverable allocation failure to escape its noexcept COM boundary.
template <typename Prepare>
PublicAllocationResult runPublicAllocationPhase(Prepare&& prepare) noexcept {
  try {
    std::forward<Prepare>(prepare)();
    return PublicAllocationResult::Completed;
  } catch (const std::bad_alloc&) {
    return PublicAllocationResult::OutOfMemory;
  } catch (const std::length_error&) {
    return PublicAllocationResult::OutOfMemory;
  }
}

// Build SWVP indices in a candidate vector and publish them only after the
// filter accepts the candidate. The destination therefore remains unchanged
// when either preparation or filtering runs out of memory.
template <typename Vector, typename Filter>
PublicAllocationResult prepareSwvpIndices(
    Vector& destination, const void* source, std::size_t byteCount,
    Filter&& filter) noexcept {
  try {
    Vector candidate;
    candidate.resize(byteCount);
    if (byteCount != 0u) {
      std::memcpy(candidate.data(), source, byteCount);
    }
    if (!std::forward<Filter>(filter)(candidate)) {
      return PublicAllocationResult::Rejected;
    }
    destination = std::move(candidate);
    return PublicAllocationResult::Completed;
  } catch (const std::bad_alloc&) {
    return PublicAllocationResult::OutOfMemory;
  } catch (const std::length_error&) {
    return PublicAllocationResult::OutOfMemory;
  }
}

// Insert a new palette or replace an existing one without exposing a
// partially-created map entry. The caller prepares the value before entering
// this helper, so an allocation failure leaves the prior palette untouched.
template <typename Map, typename Key, typename Value>
PublicAllocationResult replacePalette(Map& palettes, const Key& key,
                                       const Value& value) noexcept {
  try {
    const auto it = palettes.find(key);
    if (it != palettes.end()) {
      it->second = value;
      return PublicAllocationResult::Completed;
    }
    const auto [inserted, didInsert] = palettes.emplace(key, value);
    (void)inserted;
    return didInsert ? PublicAllocationResult::Completed
                     : PublicAllocationResult::Rejected;
  } catch (const std::bad_alloc&) {
    return PublicAllocationResult::OutOfMemory;
  } catch (const std::length_error&) {
    return PublicAllocationResult::OutOfMemory;
  }
}

}  // namespace dxmt9::d3d9::pe
