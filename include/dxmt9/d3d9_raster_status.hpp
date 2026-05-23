#pragma once

#include <cstdint>

namespace dxmt9::d3d9 {

// Result of a synthetic raster-status estimate. Used to populate the
// ScanLine / InVBlank fields of D3D9 D3DRASTER_STATUS so apps that VBlank-
// poll do not spin forever — we have no real vblank tracking on Metal.
struct RasterStatusEstimate {
  uint32_t scanLine;
  bool inVBlank;
};

// Pure helper: given a monotonic tick counter (incremented per call) and a
// display height in scanlines, derive a non-zero monotonically-advancing
// `ScanLine` value modulo `displayHeight`. `InVBlank` follows the D3D9 idiom
// of "scanLine == 0 means we are inside vertical-blank".
//
// `displayHeight` must be > 0; on zero the helper returns a neutral
// `{scanLine=0, inVBlank=true}` because there is no useful estimate to
// produce.
constexpr RasterStatusEstimate computeRasterStatusEstimate(
    uint64_t monotonicTick, uint32_t displayHeight) noexcept {
  if (displayHeight == 0) {
    return RasterStatusEstimate{0u, true};
  }
  const uint32_t scanLine =
      static_cast<uint32_t>(monotonicTick % static_cast<uint64_t>(displayHeight));
  return RasterStatusEstimate{scanLine, scanLine == 0u};
}

}  // namespace dxmt9::d3d9
