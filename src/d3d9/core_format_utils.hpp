#pragma once

#include <cstdint>

namespace dxmt9::core {

// ---------------------------------------------------------------------------
// Capability-pair helpers used by the PE-side IDirect3D9 factory. They take
// raw D3DFORMAT enumerants (kept as uint32_t so this section stays free of
// <windows.h> / <d3d9.h> / dxmt9/core.hpp). Defined inline so they can be
// consumed by translation units that include `<d3d9.h>` first (PE side)
// without dragging in dxmt9/core.hpp — that header redefines D3DERR_*
// constants as `constexpr HRESULT` which clashes with the macro forms
// installed by `<d3d9.h>`.
//
// Behavioural oracle: Wine `dlls/wined3d/directx.c` —
//   * `wined3d_check_device_format_conversion` for the conversion table
//   * `wined3d_check_depth_stencil_match` / `depth_stencil_match` for the
//     RT↔DS bit-depth pairing rules.
// Wine test: `dlls/d3d9/tests/device.c::test_check_device_format` ~12577.
//
// These helpers must remain inline + header-only so the PE-core static
// library can link them without depending on libdxmt9_frontend_core (which
// is a Darwin-only target on the current build graph).
// ---------------------------------------------------------------------------

namespace detail {

// d3d9types.h numeric values. Stable across the public ABI; documented in
// the Windows SDK and matched by the Wine / mingw-w64 headers we build
// against. Kept here (rather than including <d3d9.h>) so this section
// stays toolchain-neutral and can be used by host-side unit tests.
inline constexpr uint32_t kD3DFMT_A8R8G8B8 = 21;
inline constexpr uint32_t kD3DFMT_X8R8G8B8 = 22;
inline constexpr uint32_t kD3DFMT_R5G6B5 = 23;
inline constexpr uint32_t kD3DFMT_X1R5G5B5 = 24;
inline constexpr uint32_t kD3DFMT_A1R5G5B5 = 25;
inline constexpr uint32_t kD3DFMT_A4R4G4B4 = 26;
inline constexpr uint32_t kD3DFMT_D32 = 71;
inline constexpr uint32_t kD3DFMT_D24S8 = 75;
inline constexpr uint32_t kD3DFMT_D24X8 = 77;
inline constexpr uint32_t kD3DFMT_D16 = 80;

inline uint32_t rtBitDepth(uint32_t rtFmt) {
  switch (rtFmt) {
  case kD3DFMT_A8R8G8B8:
  case kD3DFMT_X8R8G8B8:
    return 32;
  case kD3DFMT_R5G6B5:
  case kD3DFMT_X1R5G5B5:
  case kD3DFMT_A1R5G5B5:
  case kD3DFMT_A4R4G4B4:
    return 16;
  default:
    return 0;
  }
}

inline uint32_t dsBitDepth(uint32_t dsFmt) {
  switch (dsFmt) {
  case kD3DFMT_D24S8:
  case kD3DFMT_D24X8:
  case kD3DFMT_D32:
    return 32;
  case kD3DFMT_D16:
    return 16;
  default:
    return 0;
  }
}

} // namespace detail

inline bool dxmt9FormatPair_isDepthStencilCompatible(uint32_t adapterFmt,
                                                     uint32_t rtFmt,
                                                     uint32_t dsFmt) {
  // Adapter format is consulted only to reject D3DFMT_UNKNOWN at the call
  // site; the deeper adapter-vs-RT pairing is enforced by CheckDeviceType.
  (void)adapterFmt;
  const uint32_t rtBits = detail::rtBitDepth(rtFmt);
  const uint32_t dsBits = detail::dsBitDepth(dsFmt);
  if (rtBits == 0 || dsBits == 0) {
    return false;
  }
  return rtBits == dsBits;
}

inline bool dxmt9FormatPair_canConvert(uint32_t srcFmt, uint32_t dstFmt) {
  if (srcFmt == dstFmt) {
    return true;
  }
  // A8R8G8B8 ↔ X8R8G8B8 — same memory layout, alpha ignored. Wine's
  // conversion table accepts the pair in both directions.
  const bool isXorA32 =
      (srcFmt == detail::kD3DFMT_A8R8G8B8 ||
       srcFmt == detail::kD3DFMT_X8R8G8B8) &&
      (dstFmt == detail::kD3DFMT_A8R8G8B8 ||
       dstFmt == detail::kD3DFMT_X8R8G8B8);
  if (isXorA32) {
    return true;
  }
  return false;
}

} // namespace dxmt9::core

// ---------------------------------------------------------------------------
// The legacy pixel-format helpers below depend on dxmt9::core::Format,
// Rect, and ColorRGBA from `dxmt9/core.hpp`. PE-side translation units
// that include `<d3d9.h>` should NOT include this header — the
// `constexpr HRESULT D3DERR_*` declarations in `core_constants.hpp` clash
// with the macro forms that `<d3d9.h>` installs. The PE factory uses only
// the inline helpers above and forward-declares them in its own .cpp.
// ---------------------------------------------------------------------------

#ifndef DXMT9_FORMAT_UTILS_INLINE_HELPERS_ONLY

#include "dxmt9/core.hpp"

#include <span>
#include <string>
#include <vector>

namespace dxmt9::core {

// Pure pixel-format helpers extracted from core_resources.cpp. These
// functions perform byte/pixel arithmetic only — they do not touch D3D9
// resource state or Metal-side state and remain unit-testable in isolation.

u32 pitchForFormat(Format format, u32 width);

void fillBuffer(std::vector<u8> &bytes, u32 pitch, u32 width, u32 height,
                Format format, const Rect *rect, ColorRGBA color);

bool copyPixels(std::vector<u8> &dst, u32 dstPitch, u32 dstWidth, u32 dstHeight,
                Format dstFormat, const std::vector<u8> &src, u32 srcPitch,
                u32 srcWidth, u32 srcHeight, Format srcFormat);

bool stretchPixels(std::vector<u8> &dst, u32 dstPitch, u32 dstWidth,
                   u32 dstHeight, Format dstFormat,
                   const std::vector<u8> &src, u32 srcPitch, u32 srcWidth,
                   u32 srcHeight, Format srcFormat);

void fillDepthStencil(std::vector<u8> &bytes, u32 pitch, u32 width, u32 height,
                      Format format, const Rect *rect, bool clearDepth,
                      f32 depth, bool clearStencil, u32 stencil);

bool writeBmpScreenshot(const std::string &path, Format format, u32 width,
                        u32 height, u32 pitch, std::span<const u8> bytes);

} // namespace dxmt9::core

#endif // DXMT9_FORMAT_UTILS_INLINE_HELPERS_ONLY
