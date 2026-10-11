#pragma once

#include <cstdint>
#include <string_view>

namespace dxmt9 {

// Shared cold configuration only. Actual policy provenance still comes from
// the queue's construction/arm/freeze observations, never from this selector.
enum class RenderTapeCaptureActionMode : std::uint8_t {
  Disabled,
  TapeExact,
  Normal,
  Invalid,
};

[[nodiscard]] constexpr RenderTapeCaptureActionMode
resolveRenderTapeCaptureActionMode(bool captureRequested,
                                  std::string_view actionProfile) noexcept {
  if (!captureRequested) return RenderTapeCaptureActionMode::Disabled;
  if (actionProfile.empty() || actionProfile == "tape-exact")
    return RenderTapeCaptureActionMode::TapeExact;
  if (actionProfile == "normal") return RenderTapeCaptureActionMode::Normal;
  return RenderTapeCaptureActionMode::Invalid;
}

[[nodiscard]] constexpr bool renderTapeCaptureActionModeEnabled(
    RenderTapeCaptureActionMode mode) noexcept {
  return mode == RenderTapeCaptureActionMode::TapeExact ||
      mode == RenderTapeCaptureActionMode::Normal;
}

} // namespace dxmt9
