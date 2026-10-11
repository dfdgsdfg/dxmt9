#pragma once

#include "device_c_render_tape_capture.hpp"
#include "dxmt9/render_tape_capture_policy.hpp"

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

inline constexpr bool dxmt9PeRenderTapeCaptureEnabledFromText(
    bool requested, std::string_view profile,
    std::string_view actionProfile) noexcept {
  return dxmt9::renderTapeCaptureActionModeEnabled(
             dxmt9::resolveRenderTapeCaptureActionMode(requested, actionProfile)) &&
      dxmt9PeRenderTapeProfileFromText(profile) != 0u;
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

inline std::uint32_t dxmt9PeRenderTapeBoundedCountFromText(
    std::string_view value, std::uint32_t defaultCount,
    std::uint32_t hardMaxCount) noexcept {
  if (value.empty()) return defaultCount;
  std::uint64_t parsed = 0u;
  const auto result = std::from_chars(
      value.data(), value.data() + value.size(), parsed, 10);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      parsed == 0u) return defaultCount;
  return static_cast<std::uint32_t>(
      std::min(parsed, static_cast<std::uint64_t>(hardMaxCount)));
}

inline std::uint32_t dxmt9PeRenderTapeMaxEventsFromText(
    std::string_view value) noexcept {
  return dxmt9PeRenderTapeBoundedCountFromText(value,
      dxmt9::d3d9::kRenderTapeDefaultMaxEvents,
      dxmt9::d3d9::kRenderTapeHardMaxEvents);
}

inline std::uint32_t dxmt9PeRenderTapeMaxBlobEntriesFromText(
    std::string_view value) noexcept {
  return dxmt9PeRenderTapeBoundedCountFromText(value,
      dxmt9::d3d9::kRenderTapeDefaultMaxBlobEntries,
      dxmt9::d3d9::kRenderTapeHardMaxBlobEntries);
}

// Capture storage only: disabled capture keeps defaults regardless of text.
// No cardinality override selects a renderer route or filters journal events.
inline dxmt9::d3d9::RenderTapeCaptureLimits dxmt9PeRenderTapeCaptureLimitsFromText(
    bool captureEnabled, std::string_view maxEvents = {},
    std::string_view maxBlobEntries = {}, std::string_view maxBlobBytes = {}) noexcept {
  dxmt9::d3d9::RenderTapeCaptureLimits limits{};
  if (captureEnabled) {
    limits.maxEvents = dxmt9PeRenderTapeMaxEventsFromText(maxEvents);
    limits.maxBlobEntries = dxmt9PeRenderTapeMaxBlobEntriesFromText(maxBlobEntries);
    limits.maxBlobBytes = dxmt9PeRenderTapeMaxBlobBytesFromText(maxBlobBytes);
  }
  return limits;
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
