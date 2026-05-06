#include "../dxmt9/dxmt9_presenter.hpp"
#include "core_private.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"
#include "util/util_bmp.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dxmt9::core {

// Split from core.cpp; keep this unit private to the D3D9 frontend.
namespace {

u32 clampToByte(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<u32>(std::lround(value * 255.0f)) & 0xffu;
}

std::optional<u32> parseEnvU32Auto(const char *name) {
  return dxmt9::util::getenvU32Auto(name);
}

std::string getenvString(const char *name) {
  return dxmt9::util::getenvString(name);
}

bool backendOwnsSurfaceContents(const SurfaceDesc &desc) {
  return desc.renderTarget || desc.depthStencil ||
         (desc.usage & (UsageRenderTarget | UsageDepthStencil)) != 0 ||
         desc.multiSampleType != MultiSampleType::None;
}

bool backendOwnsTextureContents(const TextureDesc &desc) {
  return (desc.usage & (UsageRenderTarget | UsageDepthStencil)) != 0;
}

bool canTrustGpuReadback(const std::shared_ptr<dxmt9::Device> &backend) {
  return backend && backend->supportsGpuReadback();
}

bool renderTraceEnabled() {
  static const bool enabled = [] {
    return dxmt9::util::getenvFlag("DXMT_TRACE_RENDER");
  }();
  return enabled;
}

std::optional<u32> textureDumpHandle() {
  static const auto value = parseEnvU32Auto("DXMT_DUMP_TEXTURE_HANDLE");
  return value;
}

std::string textureDumpDir() {
  static const std::string value = [] {
    const auto env = getenvString("DXMT_DUMP_TEXTURE_DIR");
    return env.empty() ? std::string("/tmp") : env;
  }();
  return value;
}

void emitRenderTrace(const char *fmt, ...) {
  if (!renderTraceEnabled()) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-render", fmt, args);
  va_end(args);
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

bool writeBmpScreenshot(const std::string &path, Format format, u32 width,
                        u32 height, u32 pitch, std::span<const u8> bytes) {
  return dxmt9::util::writeBmp(path, format, width, height, pitch, bytes);
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

u32 pitchForFormat(Format format, u32 width) {
  return formatRowPitch(format, width);
}

[[maybe_unused]] bool isSupportedDataFormat(Format format) {
  return bytesPerPixel(format) != 0 && !isCompressedFormat(format) &&
         !isDepthFormat(format);
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

[[maybe_unused]] bool
stretchPixels(std::vector<u8> &dst, u32 dstPitch, u32 dstWidth, u32 dstHeight,
              Format dstFormat, const std::vector<u8> &src, u32 srcPitch,
              u32 srcWidth, u32 srcHeight, Format srcFormat) {
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

} // namespace

std::vector<u8> convertTextureUpload(Format format, u32 width, u32 height,
                                     std::span<const u8> input) {
  const u32 bpp = bytesPerPixel(format);
  if (bpp == 0) {
    return {};
  }
  std::vector<u8> output(static_cast<size_t>(width) * height * bpp);
  const u32 srcPitch = pitchForFormat(format, width);
  if (input.size() < output.size()) {
    return {};
  }
  if (!copyPixels(output, srcPitch, width, height, format,
                  std::vector<u8>(input.begin(), input.end()), srcPitch, width,
                  height, format)) {
    // Fall back to a raw copy when the format is not color-decodable.
    std::copy_n(input.begin(), std::min(output.size(), input.size()),
                output.begin());
  }
  return output;
}

Buffer::Buffer(std::shared_ptr<Device> owner, BufferHandle handle,
               BufferDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc),
      storage_(static_cast<size_t>(desc.size)) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
}

Buffer::~Buffer() { invalidate(); }

LockedRegion Buffer::lock(u64 offset, u64 size, u32 flags) {
  if (!valid_) {
    return {};
  }
  if ((flags & UsageDiscard) != 0 && (desc_.usage & UsageDynamic) != 0) {
    storage_.assign(static_cast<size_t>(std::max<u64>(size, desc_.size)), 0);
    offset = 0;
  } else if (storage_.size() < offset + size) {
    storage_.resize(static_cast<size_t>(offset + size));
  }
  if (backend_ && handle_) {
    backend_->mapBuffer(handle_, flags);
  }
  locked_ = true;
  return {storage_.data() + offset, static_cast<u32>(size)};
}

void Buffer::unlock() {
  if (backend_ && handle_) {
    backend_->uploadBufferData(handle_, storage_);
    backend_->unmapBuffer(handle_);
  }
  locked_ = false;
}

void Buffer::invalidate() {
  if (!valid_) {
    return;
  }
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroyBuffer(handle_);
  }
  handle_ = {};
}

Texture::Texture(std::shared_ptr<Device> owner, TextureHandle handle,
                 TextureDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
  const u32 mipLevels = levelCount();
  const u32 faceCount = desc_.type == TextureType::Cube ? 6u : 1u;
  levels_.resize(static_cast<size_t>(mipLevels) * faceCount);
  for (u32 subresource = 0; subresource < levels_.size(); ++subresource) {
    const u32 level = mipLevelForSubresource(subresource);
    LevelStorage storage;
    storage.width = std::max(1u, desc_.width >> level);
    storage.height = std::max(1u, desc_.height >> level);
    storage.pitch = formatRowPitch(desc_.format, storage.width);
    storage.bytes.resize(
        formatByteSize(desc_.format, storage.width, storage.height), 0);
    levels_[subresource] = std::move(storage);
  }
}

Texture::~Texture() { invalidate(); }

u32 Texture::levelCount() const noexcept { return std::max(1u, desc_.levels); }

u32 Texture::mipLevelForSubresource(u32 subresource) const noexcept {
  const u32 mipLevels = levelCount();
  if (desc_.type == TextureType::Cube && mipLevels != 0) {
    return subresource % mipLevels;
  }
  return subresource;
}

LockedRegion Texture::lockRect(u32 subresource, const Rect *rect, u32 flags) {
  if (!valid_ || subresource >= levels_.size()) {
    return {};
  }
  LevelStorage &storage = levels_[subresource];
  if ((flags & UsageDiscard) != 0) {
    storage.bytes.assign(
        formatByteSize(desc_.format, storage.width, storage.height), 0);
  }
  locked_ = true;
  if (storage.pitch == 0 || storage.bytes.empty()) {
    return {};
  }
  const u32 left = rect ? std::max(0, rect->left) : 0;
  const u32 top = rect ? std::max(0, rect->top) : 0;
  if (isCompressedFormat(desc_.format)) {
    const u32 blockWidth = formatBlockWidth(desc_.format);
    const u32 blockHeight = formatBlockHeight(desc_.format);
    const u32 blockBytes = formatBlockBytes(desc_.format);
    const u32 blockX = std::min(left, storage.width - 1u) / blockWidth;
    const u32 blockY = std::min(top, storage.height - 1u) / blockHeight;
    return {storage.bytes.data() + static_cast<size_t>(blockY) * storage.pitch +
                static_cast<size_t>(blockX) * blockBytes,
            storage.pitch};
  }
  const u32 bpp = bytesPerPixel(desc_.format);
  return {storage.bytes.data() + static_cast<size_t>(top) * storage.pitch +
              static_cast<size_t>(left) * bpp,
          storage.pitch};
}

void Texture::unlockRect(u32 subresource) {
  if (subresource < levels_.size()) {
    levels_[subresource].dirty = true;
    syncLevelToBackend(subresource);
  }
  locked_ = false;
}

std::shared_ptr<Surface> Texture::surfaceLevel(u32 subresource) {
  if (subresource >= levels_.size()) {
    return {};
  }
  if (subresource < surfaces_.size()) {
    if (auto surface = surfaces_[subresource].lock()) {
      return surface;
    }
  } else {
    surfaces_.resize(subresource + 1);
  }
  auto owner = owner_.lock();
  if (!owner) {
    return {};
  }
  const u32 level = mipLevelForSubresource(subresource);
  SurfaceDesc surfaceDesc;
  surfaceDesc.width = std::max(1u, desc_.width >> level);
  surfaceDesc.height = std::max(1u, desc_.height >> level);
  surfaceDesc.format = desc_.format;
  surfaceDesc.pool = desc_.pool;
  surfaceDesc.usage = desc_.usage;
  surfaceDesc.renderTarget = (desc_.usage & UsageRenderTarget) != 0;
  surfaceDesc.depthStencil = (desc_.usage & UsageDepthStencil) != 0;
  auto surfaceHandle =
      backend_
          ? backend_->createSurfaceForTexture(handle_, subresource, surfaceDesc)
          : SurfaceHandle{};
  if (!surfaceHandle && backend_) {
    surfaceHandle = backend_->createSurface(surfaceDesc);
  }
  if (!surfaceHandle) {
    surfaceHandle = Handle{owner->nextHandle_++};
  }
  auto surface = std::make_shared<Surface>(owner, surfaceHandle,
                                           shared_from_this(), subresource);
  surfaces_[subresource] = surface;
  return surface;
}

std::span<const u8> Texture::levelBytes(u32 subresource) const {
  if (subresource >= levels_.size()) {
    return {};
  }
  const auto &storage = levels_[subresource];
  return std::span<const u8>(storage.bytes.data(), storage.bytes.size());
}

void Texture::fillColor(const Rect *rect, ColorRGBA color) {
  if (!valid_ || levels_.empty()) {
    return;
  }
  auto &storage = levels_[0];
  fillBuffer(storage.bytes, storage.pitch, storage.width, storage.height,
             desc_.format, rect, color);
  storage.dirty = true;
  syncLevelToBackend(0);
}

void Texture::fillColor(u32 subresource, const Rect *rect, ColorRGBA color) {
  if (!valid_ || subresource >= levels_.size()) {
    return;
  }
  auto &storage = levels_[subresource];
  fillBuffer(storage.bytes, storage.pitch, storage.width, storage.height,
             desc_.format, rect, color);
  storage.dirty = true;
  syncLevelToBackend(subresource);
}

void Texture::copyFrom(const Texture &src) {
  if (!valid_ || !src.valid_ || desc_.format != src.desc_.format) {
    return;
  }
  const size_t levels = std::min(levels_.size(), src.levels_.size());
  for (size_t i = 0; i < levels; ++i) {
    levels_[i].bytes = src.levels_[i].bytes;
    levels_[i].dirty = true;
    syncLevelToBackend(static_cast<u32>(i));
  }
}

void Texture::syncLevelToBackend(u32 subresource) {
  if (!valid_ || !backend_ || !handle_ || subresource >= levels_.size()) {
    return;
  }
  const auto &storage = levels_[subresource];
  if (storage.bytes.empty() || storage.width == 0 || storage.height == 0 ||
      storage.pitch == 0) {
    return;
  }
  if (const auto wanted = textureDumpHandle();
      wanted && *wanted == handle_.value) {
    const auto path = (std::filesystem::path(textureDumpDir()) /
                       ("dxmt9_tex_" + std::to_string(handle_.value) +
                        "_subresource_" + std::to_string(subresource) + ".bmp"))
                          .string();
    if (writeBmpScreenshot(
            path, desc_.format, storage.width, storage.height, storage.pitch,
            std::span<const u8>(storage.bytes.data(), storage.bytes.size()))) {
      emitRenderTrace("texture dump handle=0x%x subresource=%u path=%s "
                      "format=%u size=%ux%u pitch=%u",
                      handle_.value, subresource, path.c_str(),
                      static_cast<unsigned>(desc_.format), storage.width,
                      storage.height, storage.pitch);
    } else {
      emitRenderTrace("texture dump handle=0x%x subresource=%u failed "
                      "format=%u size=%ux%u pitch=%u",
                      handle_.value, subresource,
                      static_cast<unsigned>(desc_.format), storage.width,
                      storage.height, storage.pitch);
    }
  }
  backend_->uploadTextureLevel(
      handle_, subresource, storage.width, storage.height, storage.pitch,
      std::span<const u8>(storage.bytes.data(), storage.bytes.size()));
}

void Texture::invalidate() {
  if (!valid_) {
    return;
  }
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroyTexture(handle_);
  }
  handle_ = {};
}

Surface::Surface(std::shared_ptr<Device> owner, SurfaceHandle handle,
                 SurfaceDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc),
      containerKind_(ContainerKind::Device) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
  if (desc_.width != 0 && desc_.height != 0) {
    standalonePitch_ = formatRowPitch(desc_.format, desc_.width);
    standaloneBytes_.resize(
        formatByteSize(desc_.format, desc_.width, desc_.height), 0);
  }
}

Surface::Surface(std::shared_ptr<Device> owner, SurfaceHandle handle,
                 std::shared_ptr<Texture> texture, u32 level)
    : owner_(std::move(owner)), textureContainer_(std::move(texture)),
      handle_(handle), level_(level), containerKind_(ContainerKind::Texture) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
  if (auto tex = textureContainer_.lock()) {
    const u32 mipLevel = tex->mipLevelForSubresource(level_);
    desc_.width = std::max(1u, tex->desc().width >> mipLevel);
    desc_.height = std::max(1u, tex->desc().height >> mipLevel);
    desc_.format = tex->desc().format;
    desc_.pool = tex->desc().pool;
    desc_.usage = tex->desc().usage;
    desc_.renderTarget = (tex->desc().usage & UsageRenderTarget) != 0;
    desc_.depthStencil = (tex->desc().usage & UsageDepthStencil) != 0;
  }
}

Surface::~Surface() { invalidate(); }

LockedRegion Surface::lockRect(const Rect *rect, u32 flags) {
  if (!valid_) {
    return {};
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      return tex->lockRect(level_, rect, flags);
    }
    return {};
  }
  if ((flags & UsageDiscard) != 0) {
    standaloneBytes_.assign(
        formatByteSize(desc_.format, desc_.width, desc_.height), 0);
  }
  locked_ = true;
  if (standalonePitch_ == 0 || standaloneBytes_.empty()) {
    return {};
  }
  const u32 left = rect ? std::max(0, rect->left) : 0;
  const u32 top = rect ? std::max(0, rect->top) : 0;
  if (isCompressedFormat(desc_.format)) {
    const u32 blockWidth = formatBlockWidth(desc_.format);
    const u32 blockHeight = formatBlockHeight(desc_.format);
    const u32 blockBytes = formatBlockBytes(desc_.format);
    const u32 blockX = std::min(left, desc_.width - 1u) / blockWidth;
    const u32 blockY = std::min(top, desc_.height - 1u) / blockHeight;
    return {standaloneBytes_.data() +
                static_cast<size_t>(blockY) * standalonePitch_ +
                static_cast<size_t>(blockX) * blockBytes,
            standalonePitch_};
  }
  const u32 bpp = bytesPerPixel(desc_.format);
  return {standaloneBytes_.data() +
              static_cast<size_t>(top) * standalonePitch_ +
              static_cast<size_t>(left) * bpp,
          standalonePitch_};
}

void Surface::unlockRect() {
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      tex->unlockRect(level_);
    }
  }
  locked_ = false;
}

void Surface::fillColor(const Rect *rect, ColorRGBA color) {
  if (!valid_) {
    return;
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      tex->fillColor(level_, rect, color);
    }
    return;
  }
  fillBuffer(standaloneBytes_, standalonePitch_, desc_.width, desc_.height,
             desc_.format, rect, color);
}

void Surface::copyFrom(const Surface &src) {
  if (!valid_ || !src.valid_ || desc_.format != src.desc_.format) {
    return;
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      if (src.containerKind_ == ContainerKind::Texture) {
        if (auto srcTex = src.textureContainer_.lock()) {
          tex->copyFrom(*srcTex);
        }
      }
    }
    return;
  }
  if (src.containerKind_ == ContainerKind::Texture) {
    if (auto srcTex = src.textureContainer_.lock()) {
      if (!srcTex->levelBytes(src.level_).empty()) {
        const auto bytes = srcTex->levelBytes(src.level_);
        const size_t count = std::min(bytes.size(), standaloneBytes_.size());
        std::copy_n(bytes.begin(), count, standaloneBytes_.begin());
      }
    }
    return;
  }
  const size_t count =
      std::min(standaloneBytes_.size(), src.standaloneBytes_.size());
  std::copy_n(src.standaloneBytes_.begin(), count, standaloneBytes_.begin());
}

void Surface::invalidate() {
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroySurface(handle_);
  }
  handle_ = {};
}

Query::Query(QueryType type) : type_(type) {
  if (type_ == QueryType::TimestampFreq) {
    resolvedValue_ = 1000000000ull;
  } else if (type_ == QueryType::TimestampDisjoint) {
    resolvedValue_ = 0ull;
  }
}

/*
 * TLA+: QuerySeqId binding
 *   QueryIds[q]       -> each core::Query instance.
 *   qIssuedSeqId[q]  -> Query::issuedSequenceId_.
 *   currentSeqId     -> Device::submittedSequenceId_ + 1 before assignment;
 *                       the assigned seqId is submittedSequenceId_ after ++.
 *   completedSeqId   -> Device::completedSequenceId_.
 *   qState[q]        -> derived at GetData: unresolved while
 *                       completedSeqId < qIssuedSeqId[q], resolved at the
 *                       S_OK/data path once completedSeqId >= qIssuedSeqId[q].
 *   pendingFlush     -> caller-side Query::GetData(FLUSH) recorder/bridge
 *                       flush before this core query read observes the fence.
 *
 * Action mapping:
 *   IssueQuery       -> Device::issueQuery() assigns the seqId; Query::end()
 *                       stores it as qIssuedSeqId for D3DISSUE_END.
 *   GetDataFlush    -> Query::getData() unresolved FLUSH branch returns
 *                       S_FALSE after the synchronous flush boundary.
 *   GetDataSOK      -> Query::getData() resolved branch guarded by
 *                       completedSeqId >= qIssuedSeqId[q].
 *   GPUComplete     -> Device::completeUpTo() advances completedSeqId.
 */
void Query::begin(u64 sequenceId) {
  active_ = true;
  issuedSequenceId_ = sequenceId;
  resolvedValue_.reset();
}

void Query::end(u64 sequenceId) {
  active_ = false;
  issuedSequenceId_ = sequenceId;
}

void Query::resolve(u64 value) { resolvedValue_ = value; }

HRESULT Query::getData(void *output, size_t size, u32 flags,
                       u64 completedSequenceId) const {
  if (type_ == QueryType::TimestampFreq) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64 *>(output) = 1000000000ull;
    }
    return S_OK;
  }
  if (type_ == QueryType::TimestampDisjoint) {
    if (output && size >= sizeof(u32)) {
      *static_cast<u32 *>(output) = 0u;
    }
    return S_OK;
  }

  if (completedSequenceId < issuedSequenceId_) {
    if ((flags & QUERY_GETDATA_FLUSH) != 0) {
      // TLA+: QuerySeqId / GetDataFlush
      // The FLUSH caller has committed pending query records before this
      // read; the fence is still unresolved, so the poll reports S_FALSE.
      // TLA+: QuerySeqId / NoDeadlockOnFlushSpin
      // A repeated FLUSH spin observes progress through completedSequenceId.
      return S_FALSE;
    }
    return S_FALSE;
  }

  // TLA+: QuerySeqId / GetDataSOK
  // TLA+: QuerySeqId / QueryResolutionSafety
  // A resolved query is only reported once the GPU has completed the chunk
  // that issued it.
  DXMT_ASSERT(completedSequenceId >= issuedSequenceId_);

  if (type_ == QueryType::Event) {
    return S_OK;
  }

  const u64 value = resolvedValue_.value_or(0ull);
  if (type_ == QueryType::Occlusion) {
    if (!output || size == 0) {
      return S_OK;
    }
    if (size >= sizeof(u32)) {
      *static_cast<u32 *>(output) = static_cast<u32>(
          std::min<u64>(value, std::numeric_limits<u32>::max()));
    }
    return S_OK;
  }
  if (type_ == QueryType::Timestamp) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64 *>(output) = value;
    }
    return S_OK;
  }
  return D3DERR_NOTAVAILABLE;
}

SwapChain::SwapChain(std::shared_ptr<Device> owner, SwapChainHandle handle,
                     PresentParameters params,
                     std::shared_ptr<Surface> backBuffer,
                     std::shared_ptr<Surface> depthStencil)
    : owner_(std::move(owner)), handle_(handle), params_(params),
      backBuffer_(std::move(backBuffer)),
      depthStencilSurface_(std::move(depthStencil)) {
  ensurePresenter();
}

SwapChain::~SwapChain() = default;

void SwapChain::ensurePresenter() {
  auto owner = owner_.lock();
  if (!owner) {
    return;
  }
  const auto &upper = owner->upperDevice();
  if (!upper) {
    return;
  }
  auto wmtDevice = upper->wmtDevice();
  if (!wmtDevice) {
    return;
  }
  const u64 hwnd = params_.deviceWindow.value;
  if (!hwnd) {
    return;
  }
  presenter_ = std::make_unique<dxmt9::Presenter>(wmtDevice, hwnd, 0ull,
                                                  upper->shaderArchive(),
                                                  upper->shaderArchivePath());
  if (!presenter_->valid()) {
    presenter_.reset();
  }
}

bool SwapChain::displaySyncEnabled() const noexcept {
  return params_.presentationInterval != PresentInterval::Immediate;
}

void SwapChain::resize(const PresentParameters &params) {
  params_ = params;
  auto owner = owner_.lock();
  if (!owner) {
    return;
  }

  const u32 width = std::max(1u, params_.backBufferWidth);
  const u32 height = std::max(1u, params_.backBufferHeight);
  backBuffer_ = owner->createSurface({width, height, params_.backBufferFormat,
                                      Pool::Default, UsageRenderTarget, true,
                                      false, params_.multiSampleType});
  if (params_.enableAutoDepthStencil) {
    depthStencilSurface_ = owner->createSurface(
        {width, height, params_.autoDepthStencilFormat, Pool::Default,
         UsageDepthStencil, false, true, params_.multiSampleType});
  } else {
    depthStencilSurface_.reset();
  }
}

HResult SwapChain::present(std::shared_ptr<dxmt9::Device> device,
                           const SwapDesc &desc) {
  if (device) {
    SwapDesc adjusted = desc;
    if (backBuffer_) {
      adjusted.sourceSurface = backBuffer_->handle();
    }
    device->present(adjusted);
  }
  return D3D_OK;
}

std::shared_ptr<Buffer> Device::createBuffer(const BufferDesc &desc) {
  auto handle =
      upperDevice_ ? upperDevice_->createBuffer(desc) : BufferHandle{};
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto buffer = std::make_shared<Buffer>(shared_from_this(), handle, desc);
  registerBuffer(buffer);
  return buffer;
}

std::shared_ptr<Texture> Device::createTexture(const TextureDesc &desc) {
  auto handle =
      upperDevice_ ? upperDevice_->createTexture(desc) : TextureHandle{};
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto texture = std::make_shared<Texture>(shared_from_this(), handle, desc);
  registerTexture(texture);
  return texture;
}

std::shared_ptr<Surface> Device::createSurface(const SurfaceDesc &desc) {
  auto handle =
      upperDevice_ ? upperDevice_->createSurface(desc) : SurfaceHandle{};
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto surface = std::make_shared<Surface>(shared_from_this(), handle, desc);
  registerSurface(surface);
  return surface;
}

std::shared_ptr<Query> Device::createQuery(QueryType type) {
  auto query = std::make_shared<Query>(type);
  queries_.push_back(query);
  return query;
}

std::shared_ptr<SwapChain>
Device::createAdditionalSwapChain(const PresentParameters &params) {
  if (validatePresentParameters(params, extendedDevice_) != D3D_OK) {
    return {};
  }
  const auto normalized = normalizePresentParameters(adapter_, params);
  auto backBuffer = createSurface({std::max(1u, normalized.backBufferWidth),
                                   std::max(1u, normalized.backBufferHeight),
                                   normalized.backBufferFormat, Pool::Default,
                                   UsageRenderTarget, true, false,
                                   normalized.multiSampleType});
  std::shared_ptr<Surface> depth;
  if (normalized.enableAutoDepthStencil) {
    depth = createSurface({std::max(1u, normalized.backBufferWidth),
                           std::max(1u, normalized.backBufferHeight),
                           normalized.autoDepthStencilFormat, Pool::Default,
                           UsageDepthStencil, false, true,
                           normalized.multiSampleType});
  }
  auto swapChain = std::make_shared<SwapChain>(
      shared_from_this(), Handle{nextHandle_++}, normalized, backBuffer, depth);
  swapChains_.push_back(swapChain);
  return swapChain;
}

void Device::initializeDefaultSwapChain() {
  if (!swapChains_.empty()) {
    return;
  }
  const u32 width = std::max(1u, presentParameters_.backBufferWidth);
  const u32 height = std::max(1u, presentParameters_.backBufferHeight);
  auto backBuffer = createSurface(
      {width, height, presentParameters_.backBufferFormat, Pool::Default,
       UsageRenderTarget, true, false, presentParameters_.multiSampleType});
  std::shared_ptr<Surface> depth;
  if (presentParameters_.enableAutoDepthStencil) {
    depth =
        createSurface({width, height, presentParameters_.autoDepthStencilFormat,
                       Pool::Default, UsageDepthStencil, false, true,
                       presentParameters_.multiSampleType});
  }
  swapChains_.push_back(
      std::make_shared<SwapChain>(shared_from_this(), Handle{nextHandle_++},
                                  presentParameters_, backBuffer, depth));
  state_.renderTargets[0] =
      backBuffer ? RenderTargetAttachment{backBuffer->handle(), 0,
                                          backBuffer->multiSampleCount()}
                 : RenderTargetAttachment{};
  state_.depthStencil = depth
                            ? RenderTargetAttachment{depth->handle(), 0,
                                                     depth->multiSampleCount()}
                            : RenderTargetAttachment{};
  invalidateDrawStateCache();
}

std::shared_ptr<SwapChain> Device::swapChain(size_t index) const {
  if (index >= swapChains_.size()) {
    return {};
  }
  return swapChains_[index];
}

ClearDesc Device::snapshotClearDesc(const ClearDesc &desc) const {
  ClearDesc snapshot = desc;
  if (snapshot.clearColor) {
    bool hasExplicitColor = false;
    for (const auto &attachment : snapshot.colorAttachments) {
      if (attachment.handle) {
        hasExplicitColor = true;
        break;
      }
    }
    if (!hasExplicitColor) {
      snapshot.colorAttachments = state_.renderTargets;
    }
  }
  if ((snapshot.clearDepth || snapshot.clearStencil) &&
      !snapshot.depthStencil.handle) {
    snapshot.depthStencil = state_.depthStencil;
  }
  return snapshot;
}

HResult Device::clear(const ClearDesc &desc) {
  auto snapshot = snapshotClearDesc(desc);
  if (snapshot.clearColor) {
    for (const auto &attachment : snapshot.colorAttachments) {
      if (!attachment.handle) {
        continue;
      }
      for (auto &surface : surfaces_) {
        if (auto sp = surface.lock();
            sp && sp->handle() == attachment.handle && sp->valid()) {
          if (canTrustGpuReadback(backend_) &&
              backendOwnsSurfaceContents(sp->desc())) {
            continue;
          }
          if (snapshot.rects.empty()) {
            sp->fillColor(nullptr, snapshot.color);
          } else {
            for (const auto &rect : snapshot.rects) {
              sp->fillColor(&rect, snapshot.color);
            }
          }
        }
      }
      for (auto &texture : textures_) {
        if (auto tp = texture.lock();
            tp && tp->handle() == attachment.handle && tp->valid()) {
          if (canTrustGpuReadback(backend_) &&
              backendOwnsTextureContents(tp->desc())) {
            continue;
          }
          if (snapshot.rects.empty()) {
            tp->fillColor(nullptr, snapshot.color);
          } else {
            for (const auto &rect : snapshot.rects) {
              tp->fillColor(&rect, snapshot.color);
            }
          }
        }
      }
    }
  }

  if (snapshot.clearDepth || snapshot.clearStencil) {
    const auto applyDepthClear = [&](const std::shared_ptr<Surface> &surface) {
      if (!surface || !surface->valid()) {
        return;
      }
      const auto &surfaceDesc = surface->desc();
      if (canTrustGpuReadback(backend_) &&
          backendOwnsSurfaceContents(surfaceDesc)) {
        return;
      }
      if (!surfaceDesc.depthStencil) {
        return;
      }
      if (snapshot.rects.empty()) {
        auto region = surface->lockRect(nullptr, 0);
        if (region.data) {
          std::vector<u8> scratch(
              static_cast<size_t>(region.pitch) * surfaceDesc.height, 0);
          fillDepthStencil(scratch, region.pitch, surfaceDesc.width,
                           surfaceDesc.height, surfaceDesc.format, nullptr,
                           snapshot.clearDepth, snapshot.depth,
                           snapshot.clearStencil, snapshot.stencil);
          std::memcpy(region.data, scratch.data(), scratch.size());
        }
        surface->unlockRect();
      } else {
        for (const auto &rect : snapshot.rects) {
          auto region = surface->lockRect(&rect, 0);
          if (!region.data) {
            continue;
          }
          auto *bytes = static_cast<u8 *>(region.data);
          const u32 rectWidth =
              static_cast<u32>(std::max(0, rect.right - rect.left));
          const u32 rectHeight =
              static_cast<u32>(std::max(0, rect.bottom - rect.top));
          std::vector<u8> scratch(
              static_cast<size_t>(region.pitch) * rectHeight, 0);
          // Fill a temporary region, then copy it into the locked surface area.
          fillDepthStencil(scratch, region.pitch, rectWidth, rectHeight,
                           surfaceDesc.format, nullptr, snapshot.clearDepth,
                           snapshot.depth, snapshot.clearStencil,
                           snapshot.stencil);
          for (u32 y = 0; y < rectHeight; ++y) {
            std::memcpy(bytes + static_cast<size_t>(y) * region.pitch,
                        scratch.data() + static_cast<size_t>(y) * region.pitch,
                        static_cast<size_t>(rectWidth) *
                            bytesPerPixel(surfaceDesc.format));
          }
          surface->unlockRect();
        }
      }
    };

    if (snapshot.depthStencil.handle) {
      for (auto &surface : surfaces_) {
        if (auto sp = surface.lock();
            sp && sp->handle() == snapshot.depthStencil.handle) {
          applyDepthClear(sp);
        }
      }
    }
  }
  submitClearInternal(snapshot);
  return D3D_OK;
}

HResult Device::issueQuery(const std::shared_ptr<Query> &query, bool begin) {
  if (!query) {
    return D3DERR_INVALIDCALL;
  }
  // TLA+: QuerySeqId / IssueQuery
  // The command stream assigns the next seqId, and D3DISSUE_END records that
  // seqId as qIssuedSeqId for the query fence.
  ++submittedSequenceId_;
  // TLA+: QuerySeqId / SeqIdMonotone
  // Queries advance the submission sequence but never allow the completed
  // sequence to move ahead of it.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
  if (begin) {
    query->begin(submittedSequenceId_);
    if (query->type() == QueryType::Occlusion) {
      activeOcclusionQuery_ = query;
      activeOcclusionCount_ = 0;
    }
  } else {
    query->end(submittedSequenceId_);
    if (query->type() == QueryType::Occlusion) {
      query->resolve(activeOcclusionCount_);
      activeOcclusionQuery_.reset();
    } else if (query->type() == QueryType::Timestamp) {
      query->resolve(submittedSequenceId_);
    }
  }
  return D3D_OK;
}

HResult Device::getQueryData(const std::shared_ptr<Query> &query, void *output,
                             size_t size, u32 flags) {
  if (!query) {
    return D3DERR_INVALIDCALL;
  }
  if ((flags & QUERY_GETDATA_FLUSH) != 0 && backend_) {
    upperDevice_->flush();
    completeUpTo(submittedSequenceId_);
  }
  // TLA+: QuerySeqId / SeqIdMonotone
  // The completed sequence never exceeds submitted query/draw work.
  DXMT_ASSERT(completedSequenceId_ <= submittedSequenceId_);
  return query->getData(output, size, flags, completedSequenceId_);
}

void Device::completeUpTo(u64 sequenceId) {
  // TLA+: QuerySeqId / SeqIdMonotone
  // GPUComplete advances completedSeqId monotonically and keeps it bounded by
  // the submitted sequence cursor.
  DXMT_ASSERT(sequenceId >= completedSequenceId_);
  completedSequenceId_ = std::max(completedSequenceId_, sequenceId);
  DXMT_ASSERT(completedSequenceId_ <= submittedSequenceId_);
}

void Device::registerBuffer(const std::shared_ptr<Buffer> &buffer) {
  buffers_.push_back(buffer);
}

void Device::registerTexture(const std::shared_ptr<Texture> &texture) {
  textures_.push_back(texture);
}

void Device::registerSurface(const std::shared_ptr<Surface> &surface) {
  surfaces_.push_back(surface);
}

void Device::invalidateDefaultPoolResources() {
  auto invalidateWeak = [](auto &list) {
    list.erase(std::remove_if(list.begin(), list.end(),
                              [](const auto &weak) {
                                if (auto ptr = weak.lock()) {
                                  if (ptr->desc().pool == Pool::Default) {
                                    ptr->invalidate();
                                  }
                                  return false;
                                }
                                return true;
                              }),
               list.end());
  };
  invalidateWeak(buffers_);
  invalidateWeak(textures_);
  invalidateWeak(surfaces_);
}

void Device::submitClearInternal(const ClearDesc &desc) {
  if (renderTraceEnabled()) {
    emitRenderTrace(
        "clear seq=%llu color=%d depth=%d stencil=%d color0=0x%llx "
        "depthStencil=0x%llx rects=%zu rgba=(%.3f,%.3f,%.3f,%.3f) "
        "depthValue=%.3f stencilValue=%u",
        static_cast<unsigned long long>(submittedSequenceId_ + 1),
        desc.clearColor ? 1 : 0, desc.clearDepth ? 1 : 0,
        desc.clearStencil ? 1 : 0,
        static_cast<unsigned long long>(desc.colorAttachments[0].handle.value),
        static_cast<unsigned long long>(desc.depthStencil.handle.value),
        desc.rects.size(), desc.color.r, desc.color.g, desc.color.b,
        desc.color.a, desc.depth, desc.stencil);
  }
  upperDevice_->submitClear(desc);
  ++submittedSequenceId_;
  // SeqIdSafety: a submission can advance the current sequence, but never
  // below the completed sequence.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
}

void Device::maybeCaptureExperimentFrame() {
  if (experimentCapture_.captured || experimentCapture_.frame == 0 ||
      experimentCapture_.path.empty()) {
    return;
  }
  if (presentCount_ < experimentCapture_.frame) {
    return;
  }
  const bool trace = renderTraceEnabled();
  if (trace) {
    emitRenderTrace("capture frame=%u path=%s begin", presentCount_,
                    experimentCapture_.path.c_str());
  }
  auto chain = swapChain(0);
  if (!chain) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: no swap chain", presentCount_);
    }
    return;
  }
  auto backBuffer = chain->backBuffer();
  if (!backBuffer || !backBuffer->valid()) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: invalid backbuffer",
                      presentCount_);
    }
    return;
  }
  const auto &desc = backBuffer->desc();
  const u32 bpp = bytesPerPixel(desc.format);
  if (bpp == 0) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: unsupported format=%u",
                      presentCount_, static_cast<unsigned>(desc.format));
    }
    return;
  }
  auto scratch =
      createSurface({desc.width, desc.height, desc.format, Pool::Scratch, 0,
                     false, false, MultiSampleType::None});
  if (!scratch) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: scratch alloc failed",
                      presentCount_);
    }
    return;
  }
  const auto readbackHr = getRenderTargetData(backBuffer, scratch);
  if (readbackHr != D3D_OK) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: getRenderTargetData hr=0x%08x",
                      presentCount_, static_cast<unsigned>(readbackHr));
    }
    return;
  }
  auto region = scratch->lockRect(nullptr, 0);
  if (!region.data) {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: lockRect failed",
                      presentCount_);
    }
    return;
  }
  const size_t byteCount = static_cast<size_t>(region.pitch) * desc.height;
  const bool wrote = writeBmpScreenshot(
      experimentCapture_.path, desc.format, desc.width, desc.height,
      region.pitch,
      std::span<const u8>(static_cast<const u8 *>(region.data), byteCount));
  scratch->unlockRect();
  if (wrote) {
    experimentCapture_.captured = true;
    if (trace) {
      emitRenderTrace("capture frame=%u wrote=%s", presentCount_,
                      experimentCapture_.path.c_str());
    }
  } else {
    if (trace) {
      emitRenderTrace("capture frame=%u aborted: writeBmp failed",
                      presentCount_);
    }
  }
}

HResult Device::fillSurface(const std::shared_ptr<Surface> &surface,
                            const Rect *rect, ColorRGBA color) {
  if (!surface || !surface->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    ColorFillDesc backendDesc;
    backendDesc.destination = surface->handle();
    if (rect) {
      backendDesc.rect = *rect;
      backendDesc.hasRect = true;
    }
    backendDesc.color = color;
    upperDevice_->submitColorFill(backendDesc);
  }
  surface->fillColor(rect, color);
  return D3D_OK;
}

HResult Device::stretchRect(const std::shared_ptr<Surface> &src,
                            const Rect *srcRect,
                            const std::shared_ptr<Surface> &dst,
                            const Rect *dstRect, bool linear) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }
  Rect srcArea = srcRect ? *srcRect
                         : Rect{0, 0, static_cast<i32>(src->desc().width),
                                static_cast<i32>(src->desc().height)};
  Rect dstArea = dstRect ? *dstRect
                         : Rect{0, 0, static_cast<i32>(dst->desc().width),
                                static_cast<i32>(dst->desc().height)};
  const i32 srcWidth = std::max(0, srcArea.right - srcArea.left);
  const i32 srcHeight = std::max(0, srcArea.bottom - srcArea.top);
  const i32 dstWidth = std::max(0, dstArea.right - dstArea.left);
  const i32 dstHeight = std::max(0, dstArea.bottom - dstArea.top);
  if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0) {
    return D3DERR_INVALIDCALL;
  }

  if (backend_) {
    StretchRectDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = srcArea;
    backendDesc.destinationRect = dstArea;
    backendDesc.linear = linear;
    upperDevice_->submitStretchRect(backendDesc);
    if (canTrustGpuReadback(backend_) &&
        (backendOwnsSurfaceContents(src->desc()) ||
         backendOwnsSurfaceContents(dst->desc()))) {
      return D3D_OK;
    }
  }

  auto extractRegion = [&](const std::shared_ptr<Surface> &surface,
                           const Rect &area) -> std::vector<u8> {
    const u32 bpp = bytesPerPixel(surface->desc().format);
    const u32 width = static_cast<u32>(std::max(0, area.right - area.left));
    const u32 height = static_cast<u32>(std::max(0, area.bottom - area.top));
    auto region = surface->lockRect(&area, 0);
    if (!region.data || bpp == 0) {
      if (region.data) {
        surface->unlockRect();
      }
      return {};
    }
    std::vector<u8> out(static_cast<size_t>(width) * height * bpp);
    const auto *srcBytes = static_cast<const u8 *>(region.data);
    for (u32 y = 0; y < height; ++y) {
      std::memcpy(out.data() + static_cast<size_t>(y) * width * bpp,
                  srcBytes + static_cast<size_t>(y) * region.pitch,
                  static_cast<size_t>(width) * bpp);
    }
    surface->unlockRect();
    return out;
  };

  auto blitRegion = [&](const std::shared_ptr<Surface> &surface,
                        const Rect &area, std::span<const u8> bytes,
                        u32 srcWidthPixels, u32 srcHeightPixels) -> HResult {
    const u32 bpp = bytesPerPixel(surface->desc().format);
    if (bpp == 0) {
      return D3DERR_INVALIDCALL;
    }
    auto region = surface->lockRect(&area, 0);
    if (!region.data) {
      return D3DERR_INVALIDCALL;
    }
    const u32 dstW = static_cast<u32>(std::max(0, area.right - area.left));
    const u32 dstH = static_cast<u32>(std::max(0, area.bottom - area.top));
    std::vector<u8> temp;
    if (srcWidthPixels == dstW && srcHeightPixels == dstH) {
      temp.assign(bytes.begin(), bytes.end());
    } else {
      temp.resize(static_cast<size_t>(dstW) * dstH * bpp);
      std::vector<u8> srcCopy(bytes.begin(), bytes.end());
      if (!stretchPixels(temp, dstW * bpp, dstW, dstH, surface->desc().format,
                         srcCopy, srcWidthPixels * bpp, srcWidthPixels,
                         srcHeightPixels, surface->desc().format)) {
        surface->unlockRect();
        return D3DERR_INVALIDCALL;
      }
    }
    const auto *srcBytes = temp.data();
    for (u32 y = 0; y < dstH; ++y) {
      std::memcpy(static_cast<u8 *>(region.data) +
                      static_cast<size_t>(y) * region.pitch,
                  srcBytes + static_cast<size_t>(y) * dstW * bpp,
                  static_cast<size_t>(dstW) * bpp);
    }
    surface->unlockRect();
    return D3D_OK;
  };

  const auto srcBytes = extractRegion(src, srcArea);
  if (srcBytes.empty()) {
    return D3DERR_INVALIDCALL;
  }

  const HResult result = blitRegion(
      dst, dstArea, std::span<const u8>(srcBytes.data(), srcBytes.size()),
      static_cast<u32>(srcWidth), static_cast<u32>(srcHeight));
  if (result != D3D_OK) {
    return result;
  }
  return D3D_OK;
}

HResult Device::updateSurface(const std::shared_ptr<Surface> &src,
                              const std::shared_ptr<Surface> &dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }

  if (backend_) {
    SurfaceCopyDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = {0, 0, static_cast<i32>(src->desc().width),
                              static_cast<i32>(src->desc().height)};
    backendDesc.destinationRect = {0, 0, static_cast<i32>(dst->desc().width),
                                   static_cast<i32>(dst->desc().height)};
    upperDevice_->submitSurfaceCopy(backendDesc);
  }

  auto srcRegion = src->lockRect(nullptr, 0);
  auto dstRegion = dst->lockRect(nullptr, 0);
  if (!srcRegion.data || !dstRegion.data) {
    if (srcRegion.data) {
      src->unlockRect();
    }
    if (dstRegion.data) {
      dst->unlockRect();
    }
    return D3DERR_INVALIDCALL;
  }

  const u32 width = std::min(src->desc().width, dst->desc().width);
  const u32 height = std::min(src->desc().height, dst->desc().height);
  const u32 rowBytes = formatRowPitch(src->desc().format, width);
  const u32 rows = formatRowCount(src->desc().format, height);
  if (rowBytes == 0 || rows == 0 || srcRegion.pitch < rowBytes ||
      dstRegion.pitch < rowBytes) {
    src->unlockRect();
    dst->unlockRect();
    return D3DERR_INVALIDCALL;
  }
  for (u32 y = 0; y < rows; ++y) {
    std::memcpy(static_cast<u8 *>(dstRegion.data) +
                    static_cast<size_t>(y) * dstRegion.pitch,
                static_cast<const u8 *>(srcRegion.data) +
                    static_cast<size_t>(y) * srcRegion.pitch,
                rowBytes);
  }
  src->unlockRect();
  dst->unlockRect();
  return D3D_OK;
}

HResult Device::updateTexture(const std::shared_ptr<Texture> &src,
                              const std::shared_ptr<Texture> &dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }
  const u32 levels = std::min(src->levelCount(), dst->levelCount());
  for (u32 level = 0; level < levels; ++level) {
    auto srcSurface = src->surfaceLevel(level);
    auto dstSurface = dst->surfaceLevel(level);
    if (!srcSurface || !dstSurface) {
      return D3DERR_INVALIDCALL;
    }
    if (backend_) {
      SurfaceCopyDesc backendDesc;
      backendDesc.source = srcSurface->handle();
      backendDesc.destination = dstSurface->handle();
      backendDesc.sourceLevel = 0;
      backendDesc.destinationLevel = 0;
      backendDesc.sourceRect = {0, 0,
                                static_cast<i32>(srcSurface->desc().width),
                                static_cast<i32>(srcSurface->desc().height)};
      backendDesc.destinationRect = {
          0, 0, static_cast<i32>(dstSurface->desc().width),
          static_cast<i32>(dstSurface->desc().height)};
      upperDevice_->submitSurfaceCopy(backendDesc);
    }
  }
  dst->copyFrom(*src);
  return D3D_OK;
}

HResult Device::getRenderTargetData(const std::shared_ptr<Surface> &src,
                                    const std::shared_ptr<Surface> &dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    ReadbackDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = {0, 0, static_cast<i32>(src->desc().width),
                              static_cast<i32>(src->desc().height)};
    upperDevice_->submitReadback(backendDesc);
    upperDevice_->flush();
    ReadbackPixels pixels;
    if (backend_->readbackSurface(backendDesc, pixels)) {
      auto dstRegion = dst->lockRect(nullptr, 0);
      if (!dstRegion.data) {
        return D3DERR_INVALIDCALL;
      }
      const u32 bpp = bytesPerPixel(src->desc().format);
      if (bpp == 0) {
        dst->unlockRect();
        return D3DERR_NOTAVAILABLE;
      }
      const u32 width = std::min(src->desc().width, dst->desc().width);
      const u32 height = std::min(src->desc().height, dst->desc().height);
      const size_t rowBytes = static_cast<size_t>(width) * bpp;
      if (pixels.pitch < rowBytes ||
          pixels.bytes.size() < static_cast<size_t>(pixels.pitch) * height) {
        dst->unlockRect();
        return D3DERR_INVALIDCALL;
      }
      for (u32 y = 0; y < height; ++y) {
        std::memcpy(static_cast<u8 *>(dstRegion.data) +
                        static_cast<size_t>(y) * dstRegion.pitch,
                    pixels.bytes.data() + static_cast<size_t>(y) * pixels.pitch,
                    rowBytes);
      }
      dst->unlockRect();
      return D3D_OK;
    }
  }
  return updateSurface(src, dst);
}

} // namespace dxmt9::core
