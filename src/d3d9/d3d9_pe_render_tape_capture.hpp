#pragma once

#include "device_c_render_tape_capture.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

inline constexpr std::uint32_t dxmt9PeRenderTapeProfileFromText(
    std::string_view value) noexcept {
  if (value.empty() || value == "frame-tape") {
    return dxmt9::d3d9::kRenderTapeProfileFrame;
  }
  if (value == "sequence-tape") {
    return dxmt9::d3d9::kRenderTapeProfileSequence;
  }
  return 0u;
}

// This is deliberately a pure transform so the capture policy can be tested
// without constructing a PE device. Values are decimal bytes, not MiB
// multipliers. Invalid and zero values use the bounded default; a valid value
// above the hard ceiling is clamped rather than allowed to enlarge capture.
inline std::uint64_t dxmt9PeRenderTapeMaxBlobBytesFromText(
    std::string_view value) noexcept {
  if (value.empty()) {
    return dxmt9::d3d9::kRenderTapeDefaultMaxBlobBytes;
  }
  std::uint64_t parsed = 0u;
  const auto result = std::from_chars(
      value.data(), value.data() + value.size(), parsed, 10);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      parsed == 0u) {
    return dxmt9::d3d9::kRenderTapeDefaultMaxBlobBytes;
  }
  return std::min(parsed, dxmt9::d3d9::kRenderTapeHardMaxBlobBytes);
}

// These are opt-in PE-local test/publication seams. Production bootstrap state
// comes from the device-owned PE shadow; a non-null producer explicitly
// overrides that owner for native/PE tests. Neither callback is part of
// device_c.h or the generated PE/unix bridge ABI.
using D3D9PeRenderTapeBootstrapProducer = bool (*)(
    dxmt9::d3d9::RenderTapeCaptureBootstrapSeed& seed);
using D3D9PeRenderTapeArtifactPublisher = bool (*)(
    const dxmt9::d3d9::RenderTapePublicationBundle& bundle);

inline bool dxmt9PeRenderTapeCaptureCallbacksInstalled(
    bool captureEnabled, D3D9PeRenderTapeBootstrapProducer producer,
    D3D9PeRenderTapeArtifactPublisher publisher) noexcept {
  (void)producer;
  return captureEnabled && publisher != nullptr;
}

void dxmt9PeSetRenderTapeBootstrapProducer(
    D3D9PeRenderTapeBootstrapProducer producer) noexcept;
void dxmt9PeSetRenderTapeArtifactPublisher(
    D3D9PeRenderTapeArtifactPublisher publisher) noexcept;
