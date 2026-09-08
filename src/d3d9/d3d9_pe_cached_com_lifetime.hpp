#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace dxmt9::d3d9::pe {

// The public COM count occupies the low 31 bits.  The high bit is one private
// cache credit: it is not observable through AddRef/Release and is held only
// by a live parent cache.  Zero is terminal and cannot be reacquired.
enum class CachedComAction : std::uint8_t {
  AcquireExternal,
  ReleaseExternal,
  AcquireCache,
  ReleaseCache,
};

struct CachedComTransition {
  std::uint32_t next = 0u;
  std::uint32_t externalRefs = 0u;
  // Parent COM holds follow public external 0->1 / 1->0 edges.  Cache
  // credits are allocation ownership only and never produce these effects.
  bool acquireParents = false;
  bool releaseParents = false;
  bool destroy = false;
  bool valid = false;
};

inline constexpr std::uint32_t kCachedComCacheCredit = 0x80000000u;
inline constexpr std::uint32_t kCachedComExternalMask =
    ~kCachedComCacheCredit;

// This reducer only updates the compact object state.  A cache-only
// reacquire is legal only when the caller has already established a live
// parent getter/cache hold.  Conversely, a caller releasing its final
// external reference must snapshot all parent pointers before this transition
// and must not touch the object after the decrement can destroy it.  Transfer
// outstanding parent holds to locals and perform their Release calls outside
// the object/destructor; the atomic count itself does not prove parent
// lifetime or make arbitrary COM re-entry safe.
constexpr CachedComTransition planCachedComTransition(
    std::uint32_t packed, CachedComAction action) noexcept {
  const auto externalRefs = packed & kCachedComExternalMask;
  CachedComTransition result{
      .next = packed,
      .externalRefs = externalRefs,
  };
  switch (action) {
  case CachedComAction::AcquireExternal:
    if (packed == 0u || externalRefs == kCachedComExternalMask)
      return result;
    result.next = packed + 1u;
    result.externalRefs = externalRefs + 1u;
    result.acquireParents = externalRefs == 0u;
    result.valid = true;
    return result;
  case CachedComAction::ReleaseExternal:
    if (externalRefs == 0u)
      return result;
    result.next = packed - 1u;
    result.externalRefs = externalRefs - 1u;
    result.releaseParents = externalRefs == 1u;
    result.destroy = result.next == 0u;
    result.valid = true;
    return result;
  case CachedComAction::AcquireCache:
    if (packed == 0u || (packed & kCachedComCacheCredit) != 0u)
      return result;
    result.next = packed | kCachedComCacheCredit;
    result.valid = true;
    return result;
  case CachedComAction::ReleaseCache:
    if ((packed & kCachedComCacheCredit) == 0u)
      return result;
    result.next = packed & kCachedComExternalMask;
    result.destroy = result.next == 0u;
    result.valid = true;
    return result;
  }
  return result;
}

// The reducer's invalid state transitions are programming errors.  Production
// callers must establish the parent/cache preconditions described above; an
// invalid CAS attempt is not converted into a silently resurrected COM
// object.  Tests exercise invalid inputs through the pure reducer directly.
class CachedComLifetime final {
 public:
  constexpr CachedComLifetime() noexcept = default;

  CachedComTransition transition(CachedComAction action) noexcept {
    auto packed = state_.load(std::memory_order_acquire);
    for (;;) {
      const auto planned = planCachedComTransition(packed, action);
      if (!planned.valid)
        std::abort();
      if (state_.compare_exchange_weak(
              packed, planned.next, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return planned;
      }
    }
  }

  std::uint32_t packed() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<std::uint32_t> state_{1u};
};

}  // namespace dxmt9::d3d9::pe
