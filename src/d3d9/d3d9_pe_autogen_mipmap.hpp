#pragma once

namespace dxmt9::d3d9::pe {

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
