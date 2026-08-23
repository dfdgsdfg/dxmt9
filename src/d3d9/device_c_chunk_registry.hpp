#pragma once

#include "device_c_chunk_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

namespace dxmt9::d3d9 {

class WireObjectRegistry {
 public:
  using RetainFn = void (*)(std::uint32_t kind, void* object) noexcept;
  // RetainFn runs under the registry lock and must only perform the direct
  // AddRef/pin operation.  Re-entering any registry method is rejected before
  // locking; this keeps object lifetime race-free without permitting a
  // callback self-deadlock.
  static constexpr bool kRetainCallbackNonReentrant = true;
  static_assert(std::is_nothrow_invocable_r_v<void, RetainFn, std::uint32_t,
                                              void*>);

  WireObjectRegistry();
  WireObjectRegistry(const WireObjectRegistry&) = delete;
  WireObjectRegistry& operator=(const WireObjectRegistry&) = delete;

  D9CWireObjectIdentity insert(std::uint32_t kind, void* object);
  bool erase(const D9CWireObjectIdentity& identity, const void* object);

  bool contains(const D9CWireObjectIdentity& identity,
                const void* object = nullptr) const;

  // Validates the complete entry array first, then fills `objects` and calls
  // retain for every entry while registry mutation remains excluded. Callers
  // supply capacity-preserving scratch for `objects`.
  bool resolveAndRetain(
      std::span<const D9CCommandChunkWireHandleEntry> entries,
      std::span<void*> objects,
      RetainFn retain) const;

  std::size_t activeCount() const;

 private:
  struct Slot {
    void* object = nullptr;
    std::uint32_t kind = 0u;
    std::uint32_t generation = 1u;
    bool retired = false;
  };

 public:
  struct GenerationAdvance {
    std::uint32_t generation;
    bool retired;
  };

  static constexpr GenerationAdvance advanceGeneration(
      std::uint32_t generation) {
    if (generation == 0u ||
        generation == std::numeric_limits<std::uint32_t>::max()) {
      return GenerationAdvance{generation, true};
    }
    return GenerationAdvance{generation + 1u, false};
  }

 private:

  const Slot* findLocked(std::uint64_t objectId) const;
  Slot* findLocked(std::uint64_t objectId);
  static bool identityMatches(const Slot& slot, std::uint32_t kind,
                              std::uint32_t generation);

  mutable std::mutex mutex_;
  std::uint32_t registryId_ = 0u;
  std::vector<Slot> slots_;
  std::vector<std::uint32_t> freeSlots_;
  std::size_t activeCount_ = 0u;
};

constexpr D9CCommandChunkWireHandleEntry wireHandleEntry(
    const D9CWireObjectIdentity& identity) {
  return D9CCommandChunkWireHandleEntry{
      .kind = identity.kind,
      .generation = identity.generation,
      .objectId = identity.objectId,
  };
}

}  // namespace dxmt9::d3d9
