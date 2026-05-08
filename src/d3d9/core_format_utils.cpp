#include "core_format_utils.hpp"

#include "dxmt9/core.hpp"
#include "util/util_bmp.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace dxmt9::core {

// Pure pixel-format helpers extracted from core_resources.cpp. These
// helpers perform byte/pixel arithmetic only — they do not touch D3D9
// resource state, Metal-side state, or backend handles.

namespace {

u32 clampToByte(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<u32>(std::lround(value * 255.0f)) & 0xffu;
}

u32 clampToBits(float value, u32 bits) {
  value = std::clamp(value, 0.0f, 1.0f);
  const u32 maxValue = (1u << bits) - 1u;
  return static_cast<u32>(std::lround(value * static_cast<float>(maxValue))) &
         maxValue;
}

u16 pack565(ColorRGBA c) {
  return static_cast<u16>((clampToBits(c.r, 5) << 11) |
                          (clampToBits(c.g, 6) << 5) | clampToBits(c.b, 5));
}

u16 pack1555(ColorRGBA c, bool forceAlpha) {
  const u16 a = static_cast<u16>(forceAlpha ? 1u : clampToBits(c.a, 1));
  return static_cast<u16>((a << 15) | (clampToBits(c.r, 5) << 10) |
                          (clampToBits(c.g, 5) << 5) | clampToBits(c.b, 5));
}

u16 pack4444(ColorRGBA c) {
  return static_cast<u16>((clampToBits(c.a, 4) << 12) |
                          (clampToBits(c.r, 4) << 8) |
                          (clampToBits(c.g, 4) << 4) | clampToBits(c.b, 4));
}

u32 pack2101010(ColorRGBA c, bool bgraOrder) {
  const u32 a = clampToBits(c.a, 2);
  const u32 r = clampToBits(c.r, 10);
  const u32 g = clampToBits(c.g, 10);
  const u32 b = clampToBits(c.b, 10);
  if (bgraOrder) {
    return (a << 30) | (b << 20) | (g << 10) | r;
  }
  return (a << 30) | (r << 20) | (g << 10) | b;
}

ColorRGBA decodeColor(Format format, const u8 *src) {
  switch (format) {
  case Format::A8R8G8B8:
  case Format::X8R8G8B8:
    return {src[2] / 255.0f, src[1] / 255.0f, src[0] / 255.0f,
            format == Format::X8R8G8B8 ? 1.0f : src[3] / 255.0f};
  case Format::A8B8G8R8:
  case Format::X8B8G8R8:
    return {src[0] / 255.0f, src[1] / 255.0f, src[2] / 255.0f,
            format == Format::X8B8G8R8 ? 1.0f : src[3] / 255.0f};
  case Format::R5G6B5: {
    const u16 v = std::bit_cast<u16>(std::array<u8, 2>{src[0], src[1]});
    return {((v >> 11) & 0x1f) / 31.0f, ((v >> 5) & 0x3f) / 63.0f,
            (v & 0x1f) / 31.0f, 1.0f};
  }
  case Format::A1R5G5B5:
  case Format::X1R5G5B5: {
    const u16 v = std::bit_cast<u16>(std::array<u8, 2>{src[0], src[1]});
    return {((v >> 10) & 0x1f) / 31.0f, ((v >> 5) & 0x1f) / 31.0f,
            (v & 0x1f) / 31.0f,
            format == Format::X1R5G5B5 ? 1.0f
            : ((v >> 15) & 1u)         ? 1.0f
                                       : 0.0f};
  }
  case Format::A4R4G4B4: {
    const u16 v = std::bit_cast<u16>(std::array<u8, 2>{src[0], src[1]});
    return {((v >> 8) & 0xf) / 15.0f, ((v >> 4) & 0xf) / 15.0f,
            (v & 0xf) / 15.0f, ((v >> 12) & 0xf) / 15.0f};
  }
  case Format::A8:
    return {0.0f, 0.0f, 0.0f, src[0] / 255.0f};
  case Format::L8: {
    const float l = src[0] / 255.0f;
    return {l, l, l, 1.0f};
  }
  case Format::A8L8:
    return {src[0] / 255.0f, src[0] / 255.0f, src[0] / 255.0f, src[1] / 255.0f};
  case Format::A2R10G10B10: {
    const u32 v =
        std::bit_cast<u32>(std::array<u8, 4>{src[0], src[1], src[2], src[3]});
    return {((v >> 20) & 0x3ff) / 1023.0f, ((v >> 10) & 0x3ff) / 1023.0f,
            (v & 0x3ff) / 1023.0f, ((v >> 30) & 0x3) / 3.0f};
  }
  case Format::A2B10G10R10: {
    const u32 v =
        std::bit_cast<u32>(std::array<u8, 4>{src[0], src[1], src[2], src[3]});
    return {(v & 0x3ff) / 1023.0f, ((v >> 10) & 0x3ff) / 1023.0f,
            ((v >> 20) & 0x3ff) / 1023.0f, ((v >> 30) & 0x3) / 3.0f};
  }
  default:
    return {};
  }
}

bool encodeColor(Format format, ColorRGBA c, u8 *dst) {
  switch (format) {
  case Format::A8R8G8B8:
  case Format::X8R8G8B8:
    dst[0] = static_cast<u8>(clampToByte(c.b));
    dst[1] = static_cast<u8>(clampToByte(c.g));
    dst[2] = static_cast<u8>(clampToByte(c.r));
    dst[3] =
        format == Format::X8R8G8B8 ? 0xffu : static_cast<u8>(clampToByte(c.a));
    return true;
  case Format::A8B8G8R8:
  case Format::X8B8G8R8:
    dst[0] = static_cast<u8>(clampToByte(c.r));
    dst[1] = static_cast<u8>(clampToByte(c.g));
    dst[2] = static_cast<u8>(clampToByte(c.b));
    dst[3] =
        format == Format::X8B8G8R8 ? 0xffu : static_cast<u8>(clampToByte(c.a));
    return true;
  case Format::R5G6B5: {
    const u16 v = pack565(c);
    const auto raw = std::bit_cast<std::array<u8, 2>>(v);
    dst[0] = raw[0];
    dst[1] = raw[1];
    return true;
  }
  case Format::A1R5G5B5:
  case Format::X1R5G5B5: {
    const u16 v = pack1555(c, format == Format::X1R5G5B5);
    const auto raw = std::bit_cast<std::array<u8, 2>>(v);
    dst[0] = raw[0];
    dst[1] = raw[1];
    return true;
  }
  case Format::A4R4G4B4: {
    const u16 v = pack4444(c);
    const auto raw = std::bit_cast<std::array<u8, 2>>(v);
    dst[0] = raw[0];
    dst[1] = raw[1];
    return true;
  }
  case Format::A8:
    dst[0] = static_cast<u8>(clampToByte(c.a));
    return true;
  case Format::L8:
    dst[0] = static_cast<u8>(clampToByte((c.r + c.g + c.b) / 3.0f));
    return true;
  case Format::A8L8:
    dst[0] = static_cast<u8>(clampToByte((c.r + c.g + c.b) / 3.0f));
    dst[1] = static_cast<u8>(clampToByte(c.a));
    return true;
  case Format::A2R10G10B10: {
    const u32 v = pack2101010(c, false);
    const auto raw = std::bit_cast<std::array<u8, 4>>(v);
    std::memcpy(dst, raw.data(), 4);
    return true;
  }
  case Format::A2B10G10R10: {
    const u32 v = pack2101010(c, true);
    const auto raw = std::bit_cast<std::array<u8, 4>>(v);
    std::memcpy(dst, raw.data(), 4);
    return true;
  }
  case Format::R32F:
    std::memcpy(dst, &c.r, sizeof(float));
    return true;
  case Format::G32R32F:
  case Format::G16R16:
  case Format::V16U16:
  case Format::Q8W8V8U8:
    std::memcpy(dst, &c.r, std::min<size_t>(sizeof(float) * 2, 4));
    return true;
  default:
    return false;
  }
}

[[maybe_unused]] bool isColorRenderable(Format format) {
  switch (format) {
  case Format::A8R8G8B8:
  case Format::X8R8G8B8:
  case Format::A8B8G8R8:
  case Format::X8B8G8R8:
  case Format::R5G6B5:
  case Format::A1R5G5B5:
  case Format::X1R5G5B5:
  case Format::A4R4G4B4:
  case Format::A8:
  case Format::A16B16G16R16F:
  case Format::A32B32G32R32F:
  case Format::G16R16F:
  case Format::R16F:
  case Format::G32R32F:
  case Format::R32F:
  case Format::A16B16G16R16:
  case Format::G16R16:
  case Format::A2R10G10B10:
  case Format::A2B10G10R10:
  case Format::L8:
  case Format::L16:
  case Format::A8L8:
  case Format::V8U8:
  case Format::Q8W8V8U8:
  case Format::V16U16:
    return true;
  default:
    return false;
  }
}

bool isDepthFormat(Format format) {
  switch (format) {
  case Format::D24S8:
  case Format::D24X8:
  case Format::D16:
  case Format::D32:
  case Format::D32F_LOCKABLE:
  case Format::D16_LOCKABLE:
  case Format::D24FS8:
    return true;
  default:
    return false;
  }
}

[[maybe_unused]] bool isSupportedDataFormat(Format format) {
  return bytesPerPixel(format) != 0 && !isCompressedFormat(format) &&
         !isDepthFormat(format);
}

} // namespace

u32 pitchForFormat(Format format, u32 width) {
  return formatRowPitch(format, width);
}

// Note: the dxmt9FormatPair_* capability helpers used by the PE-side
// IDirect3D9 factory are defined inline in core_format_utils.hpp so that
// translation units which include <d3d9.h> (and thus already have the
// D3DERR_* macros installed) can consume them without dragging in
// dxmt9/core.hpp, whose `constexpr HRESULT D3DERR_*` declarations clash
// with those macros. See the header for the implementation and the Wine
// behavioural-oracle references.

bool writeBmpScreenshot(const std::string &path, Format format, u32 width,
                        u32 height, u32 pitch, std::span<const u8> bytes) {
  return dxmt9::util::writeBmp(path, format, width, height, pitch, bytes);
}

void fillBuffer(std::vector<u8> &bytes, u32 pitch, u32 width, u32 height,
                Format format, const Rect *rect, ColorRGBA color) {
  if (!encodeColor(format, color, bytes.data())) {
    return;
  }

  const i32 left = rect ? std::max(0, rect->left) : 0;
  const i32 top = rect ? std::max(0, rect->top) : 0;
  const i32 right =
      rect ? std::min<i32>(width, rect->right) : static_cast<i32>(width);
  const i32 bottom =
      rect ? std::min<i32>(height, rect->bottom) : static_cast<i32>(height);
  const u32 bpp = bytesPerPixel(format);
  std::vector<u8> pixel(bpp);
  if (!encodeColor(format, color, pixel.data())) {
    return;
  }
  for (i32 y = top; y < bottom; ++y) {
    u8 *row = bytes.data() + static_cast<size_t>(y) * pitch;
    for (i32 x = left; x < right; ++x) {
      std::memcpy(row + static_cast<size_t>(x) * bpp, pixel.data(), bpp);
    }
  }
}

bool copyPixels(std::vector<u8> &dst, u32 dstPitch, u32 dstWidth, u32 dstHeight,
                Format dstFormat, const std::vector<u8> &src, u32 srcPitch,
                u32 srcWidth, u32 srcHeight, Format srcFormat) {
  const u32 dstBpp = bytesPerPixel(dstFormat);
  const u32 srcBpp = bytesPerPixel(srcFormat);
  if (dstBpp == 0 || srcBpp == 0) {
    return false;
  }
  const u32 width = std::min(dstWidth, srcWidth);
  const u32 height = std::min(dstHeight, srcHeight);
  std::vector<u8> temp(dstBpp);
  for (u32 y = 0; y < height; ++y) {
    const u8 *srcRow = src.data() + static_cast<size_t>(y) * srcPitch;
    u8 *dstRow = dst.data() + static_cast<size_t>(y) * dstPitch;
    for (u32 x = 0; x < width; ++x) {
      const u8 *srcPx = srcRow + static_cast<size_t>(x) * srcBpp;
      ColorRGBA color = decodeColor(srcFormat, srcPx);
      if (!encodeColor(dstFormat, color, temp.data())) {
        return false;
      }
      std::memcpy(dstRow + static_cast<size_t>(x) * dstBpp, temp.data(),
                  dstBpp);
    }
  }
  return true;
}

bool stretchPixels(std::vector<u8> &dst, u32 dstPitch, u32 dstWidth,
                   u32 dstHeight, Format dstFormat,
                   const std::vector<u8> &src, u32 srcPitch, u32 srcWidth,
                   u32 srcHeight, Format srcFormat) {
  const u32 dstBpp = bytesPerPixel(dstFormat);
  const u32 srcBpp = bytesPerPixel(srcFormat);
  if (dstBpp == 0 || srcBpp == 0) {
    return false;
  }
  std::vector<u8> temp(dstBpp);
  for (u32 y = 0; y < dstHeight; ++y) {
    const u32 srcY = srcHeight == 0 ? 0 : (y * srcHeight) / dstHeight;
    const u8 *srcRow = src.data() + static_cast<size_t>(srcY) * srcPitch;
    u8 *dstRow = dst.data() + static_cast<size_t>(y) * dstPitch;
    for (u32 x = 0; x < dstWidth; ++x) {
      const u32 srcX = srcWidth == 0 ? 0 : (x * srcWidth) / dstWidth;
      const u8 *srcPx = srcRow + static_cast<size_t>(srcX) * srcBpp;
      ColorRGBA color = decodeColor(srcFormat, srcPx);
      if (!encodeColor(dstFormat, color, temp.data())) {
        return false;
      }
      std::memcpy(dstRow + static_cast<size_t>(x) * dstBpp, temp.data(),
                  dstBpp);
    }
  }
  return true;
}

void fillDepthStencil(std::vector<u8> &bytes, u32 pitch, u32 width, u32 height,
                      Format format, const Rect *rect, bool clearDepth,
                      f32 depth, bool clearStencil, u32 stencil) {
  if (width == 0 || height == 0) {
    return;
  }

  const i32 left = rect ? std::max(0, rect->left) : 0;
  const i32 top = rect ? std::max(0, rect->top) : 0;
  const i32 right =
      rect ? std::min<i32>(width, rect->right) : static_cast<i32>(width);
  const i32 bottom =
      rect ? std::min<i32>(height, rect->bottom) : static_cast<i32>(height);
  const u32 bpp = bytesPerPixel(format);
  if (bpp == 0) {
    return;
  }

  const auto encodeDepth24 = [](f32 value) -> u32 {
    const auto clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<u32>(std::lround(clamped * 16777215.0f)) & 0x00ffffffu;
  };

  const auto readDepth24 = [](const u8 *px) -> f32 {
    const u32 raw = static_cast<u32>(px[0]) | (static_cast<u32>(px[1]) << 8) |
                    (static_cast<u32>(px[2]) << 16);
    return static_cast<f32>(raw) / 16777215.0f;
  };

  for (i32 y = top; y < bottom; ++y) {
    u8 *row = bytes.data() + static_cast<size_t>(y) * pitch;
    for (i32 x = left; x < right; ++x) {
      u8 *px = row + static_cast<size_t>(x) * bpp;
      f32 currentDepth = 0.0f;
      u32 currentStencil = 0;
      switch (format) {
      case Format::D16:
      case Format::D16_LOCKABLE: {
        const u16 raw = std::bit_cast<u16>(std::array<u8, 2>{px[0], px[1]});
        currentDepth = static_cast<f32>(raw) / 65535.0f;
        break;
      }
      case Format::D32:
      case Format::D32F_LOCKABLE: {
        const u32 raw =
            std::bit_cast<u32>(std::array<u8, 4>{px[0], px[1], px[2], px[3]});
        currentDepth = std::bit_cast<f32>(raw);
        break;
      }
      case Format::D24S8: {
        currentDepth = readDepth24(px);
        currentStencil = px[3];
        break;
      }
      case Format::D24X8: {
        currentDepth = readDepth24(px);
        break;
      }
      case Format::D24FS8: {
        const u32 raw =
            std::bit_cast<u32>(std::array<u8, 4>{px[0], px[1], px[2], px[3]});
        currentDepth = std::bit_cast<f32>(raw);
        currentStencil = px[4];
        break;
      }
      case Format::S8_LOCKABLE:
        currentStencil = px[0];
        break;
      default:
        break;
      }

      if (clearDepth) {
        currentDepth = depth;
      }
      if (clearStencil) {
        currentStencil = stencil & 0xffu;
      }

      switch (format) {
      case Format::D16:
      case Format::D16_LOCKABLE: {
        const u16 raw = static_cast<u16>(
            std::lround(std::clamp(currentDepth, 0.0f, 1.0f) * 65535.0f));
        const auto bytes16 = std::bit_cast<std::array<u8, 2>>(raw);
        px[0] = bytes16[0];
        px[1] = bytes16[1];
        break;
      }
      case Format::D32:
      case Format::D32F_LOCKABLE: {
        const u32 raw = std::bit_cast<u32>(currentDepth);
        const auto bytes32 = std::bit_cast<std::array<u8, 4>>(raw);
        px[0] = bytes32[0];
        px[1] = bytes32[1];
        px[2] = bytes32[2];
        px[3] = bytes32[3];
        break;
      }
      case Format::D24S8: {
        const u32 raw = encodeDepth24(currentDepth);
        px[0] = static_cast<u8>(raw & 0xffu);
        px[1] = static_cast<u8>((raw >> 8) & 0xffu);
        px[2] = static_cast<u8>((raw >> 16) & 0xffu);
        px[3] = static_cast<u8>(currentStencil);
        break;
      }
      case Format::D24X8: {
        const u32 raw = encodeDepth24(currentDepth);
        px[0] = static_cast<u8>(raw & 0xffu);
        px[1] = static_cast<u8>((raw >> 8) & 0xffu);
        px[2] = static_cast<u8>((raw >> 16) & 0xffu);
        px[3] = 0;
        break;
      }
      case Format::D24FS8: {
        const u32 raw = std::bit_cast<u32>(currentDepth);
        const auto bytes32 = std::bit_cast<std::array<u8, 4>>(raw);
        px[0] = bytes32[0];
        px[1] = bytes32[1];
        px[2] = bytes32[2];
        px[3] = bytes32[3];
        px[4] = static_cast<u8>(currentStencil);
        px[5] = 0;
        px[6] = 0;
        px[7] = 0;
        break;
      }
      case Format::S8_LOCKABLE:
        px[0] = static_cast<u8>(currentStencil);
        break;
      default:
        break;
      }
    }
  }
}

} // namespace dxmt9::core
