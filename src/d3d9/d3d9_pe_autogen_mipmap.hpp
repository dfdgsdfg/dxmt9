#pragma once

#include <cstdint>

namespace dxmt9::d3d9::pe {

// Derived only from immutable texture-creation metadata. Dirty is deliberately
// not cached here: a bound render target can become dirty without rebinding.
class BoundAutogenTextureSlots {
 public:
  static constexpr std::uint32_t kSlotCount = 20u;

  constexpr void publish(std::uint32_t slot, bool enabled) noexcept {
    if (slot >= kSlotCount) return;
    const auto bit = std::uint32_t{1u} << slot;
    mask_ = enabled ? mask_ | bit : mask_ & ~bit;
  }

  constexpr void clear() noexcept { mask_ = 0u; }
  [[nodiscard]] constexpr std::uint32_t mask() const noexcept { return mask_; }

 private:
  std::uint32_t mask_ = 0u;
};

static_assert(sizeof(BoundAutogenTextureSlots) == sizeof(std::uint32_t));

enum class AutogenMipmapEvent {
  RenderTargetWrite,
  GenerationSucceeded,
  GenerationFailed,
};

struct AutogenMipmapState {
  bool enabled = false;
  bool dirty = false;

  [[nodiscard]] constexpr bool generationRequired() const noexcept {
    return enabled && dirty;
  }
};

[[nodiscard]] constexpr AutogenMipmapState transitionAutogenMipmap(
    AutogenMipmapState state, AutogenMipmapEvent event) noexcept {
  if (!state.enabled)
    return state;
  switch (event) {
    case AutogenMipmapEvent::RenderTargetWrite:
      state.dirty = true;
      break;
    case AutogenMipmapEvent::GenerationSucceeded:
      state.dirty = false;
      break;
    case AutogenMipmapEvent::GenerationFailed:
      // Failure is retryable and must never publish a false Clean state.
      state.dirty = true;
      break;
  }
  return state;
}

} // namespace dxmt9::d3d9::pe
