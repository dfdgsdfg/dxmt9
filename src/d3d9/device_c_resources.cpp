#include "device_c_provider.hpp"
#include "dxmt9/dxmt9_perf_counters.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

using namespace dxmt9::d3d9::devicec;

namespace {

struct LockFootprint {
  uint32_t rowBytes = 0;
  uint32_t rows = 0;
};

constexpr uint32_t kD3DLockDiscard = 0x00002000u;
constexpr uint32_t kD3DLockNoOverwrite = 0x00001000u;
constexpr uint32_t kD3DLockReadOnly = 0x00000010u;
constexpr uint32_t kD3DUsageAutoGenMipmap = 0x00000400u;
constexpr uint32_t kD3DFmtA8R8G8B8 = 21u;
constexpr uint32_t kD3DFmtA8P8 = 40u;
constexpr uint32_t kD3DFmtP8 = 41u;

// D3D9Ex shared handles are public Win32 HANDLE values. Until the unix bridge
// grows an IOSurface / MTLSharedTextureHandle transport, keep a process-wide
// registry whose entries retain the source core resource and whose imports
// clone the same Metal backing into the destination device's resource pool.
// Values stay within 32 bits so WoW64 and x64 observe the same token.
constexpr uint64_t kDxmtSharedHandlePrefix = 0xd9000000ull;
constexpr uint64_t kDxmtSharedHandleMask = 0xff000000ull;
std::atomic<uint32_t> gNextSharedHandle{1u};
std::mutex gSharedResourceMutex;

struct SharedTextureSignature {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t levels = 0;
  uint32_t usage = 0;
  uint32_t format = 0;
  uint32_t pool = 0;
  uint32_t type = 0;
  bool operator==(const SharedTextureSignature&) const = default;
};

struct SharedBufferSignature {
  uint32_t length = 0;
  uint32_t usage = 0;
  uint32_t formatOrFvf = 0;
  uint32_t pool = 0;
  uint32_t type = 0;
  bool operator==(const SharedBufferSignature&) const = default;
};

struct SharedSurfaceSignature {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t multiSampleType = 0;
  uint32_t multiSampleQuality = 0;
  uint32_t flags = 0;
  uint32_t pool = 0;
  uint32_t type = 0;
  bool operator==(const SharedSurfaceSignature&) const = default;
};

struct SharedTextureEntry {
  SharedTextureSignature signature;
  std::shared_ptr<dxmt9::core::Texture> resource;
};

struct SharedBufferEntry {
  SharedBufferSignature signature;
  std::shared_ptr<dxmt9::core::Buffer> resource;
};

struct SharedSurfaceEntry {
  SharedSurfaceSignature signature;
  std::shared_ptr<dxmt9::core::Surface> resource;
};

std::unordered_map<uint64_t, SharedTextureEntry> gSharedTextures;
std::unordered_map<uint64_t, SharedBufferEntry> gSharedBuffers;
std::unordered_map<uint64_t, SharedSurfaceEntry> gSharedSurfaces;

bool isDxmtSharedHandle(uint64_t handle) {
  return handle <= 0xffffffffull &&
         (handle & kDxmtSharedHandleMask) == kDxmtSharedHandlePrefix;
}

uint64_t allocateSharedHandleLocked() {
  for (;;) {
    const uint64_t sequence = gNextSharedHandle.fetch_add(1u, std::memory_order_relaxed) & 0x00ffffffu;
    const uint64_t handle = kDxmtSharedHandlePrefix | (sequence == 0u ? 1u : sequence);
    if (!gSharedTextures.contains(handle) && !gSharedBuffers.contains(handle) &&
        !gSharedSurfaces.contains(handle)) {
      return handle;
    }
  }
}

uint64_t traceBufferHandleFilter() {
  static const uint64_t filter = [] {
    const char* env = std::getenv("DXMT9_TRACE_BUFFER_HANDLE");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return std::strtoull(env, nullptr, 0);
  }();
  return filter;
}

uint64_t bufferHandleValue(const D9CBuffer* buffer) {
  return buffer && buffer->obj ? buffer->obj->handle().value : 0ull;
}

bool shouldTraceBufferHandle(const D9CBuffer* buffer) {
  const uint64_t filter = traceBufferHandleFilter();
  return filter != 0ull && bufferHandleValue(buffer) == filter;
}

void bufferTraceLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Warn, "dxmt9-buffer-trace", fmt, args);
  va_end(args);
}

uint32_t lockFlagsToCore(uint32_t flags) {
  uint32_t out = 0;
  if ((flags & kD3DLockDiscard) != 0) {
    out |= dxmt9::core::UsageDiscard;
  }
  if ((flags & kD3DLockNoOverwrite) != 0) {
    out |= dxmt9::core::UsageNoOverwrite;
  }
  if ((flags & kD3DLockReadOnly) != 0) {
    out |= dxmt9::core::UsageReadOnly;
  }
  return out;
}

LockFootprint lockFootprint(dxmt9::core::Format format,
                            uint32_t width,
                            uint32_t height,
                            const D9CRect* rect) {
  const uint32_t blockWidth = dxmt9::core::formatBlockWidth(format);
  const uint32_t blockHeight = dxmt9::core::formatBlockHeight(format);
  const uint32_t blockBytes = dxmt9::core::formatBlockBytes(format);
  if (width == 0 || height == 0 || blockBytes == 0) {
    return {};
  }

  const int32_t left = rect ? std::clamp(rect->left, 0, static_cast<int32_t>(width)) : 0;
  const int32_t top = rect ? std::clamp(rect->top, 0, static_cast<int32_t>(height)) : 0;
  const int32_t right =
      rect ? std::clamp(rect->right, left, static_cast<int32_t>(width)) : static_cast<int32_t>(width);
  const int32_t bottom =
      rect ? std::clamp(rect->bottom, top, static_cast<int32_t>(height)) : static_cast<int32_t>(height);
  if (right == left || bottom == top) {
    return {};
  }

  const uint32_t blockLeft = static_cast<uint32_t>(left) / blockWidth;
  const uint32_t blockTop = static_cast<uint32_t>(top) / blockHeight;
  const uint32_t blockRight =
      (static_cast<uint32_t>(right) + blockWidth - 1u) / blockWidth;
  const uint32_t blockBottom =
      (static_cast<uint32_t>(bottom) + blockHeight - 1u) / blockHeight;
  return {(blockRight - blockLeft) * blockBytes, blockBottom - blockTop};
}

bool invalidCompressedTextureDimensions(dxmt9::core::Format format, uint32_t width, uint32_t height) {
  if (!dxmt9::core::isCompressedFormat(format)) {
    return false;
  }
  return width == 0 || height == 0 ||
         (width % dxmt9::core::formatBlockWidth(format)) != 0 ||
         (height % dxmt9::core::formatBlockHeight(format)) != 0;
}

uint32_t fullMipLevelCount(uint32_t width, uint32_t height, uint32_t depth) {
  uint32_t dimension = std::max({width, height, depth});
  uint32_t levels = 1;
  while (dimension > 1u) {
    dimension >>= 1u;
    ++levels;
  }
  return levels;
}

uint32_t resolveMipLevelCount(uint32_t levels,
                              uint32_t usage,
                              uint32_t width,
                              uint32_t height,
                              uint32_t depth) {
  if (levels != 0u) {
    return levels;
  }
  if ((usage & kD3DUsageAutoGenMipmap) != 0u) {
    return 1u;
  }
  return fullMipLevelCount(width, height, depth);
}

uint32_t mipDimension(uint32_t base, uint32_t level) {
  return std::max(1u, base >> std::min(level, 31u));
}

bool isPalettizedD3DFormat(uint32_t format) {
  return format == kD3DFmtP8 || format == kD3DFmtA8P8;
}

uint32_t palettizedTexelBytes(uint32_t format) {
  return format == kD3DFmtA8P8 ? 2u : 1u;
}

size_t palettizedSubresourceBytes(const dxmt9::core::TextureDesc& desc,
                                  uint32_t subresource,
                                  uint32_t texelBytes) {
  const uint32_t mipLevels = std::max(1u, desc.levels);
  const uint32_t level = desc.type == dxmt9::core::TextureType::Cube
                             ? subresource % mipLevels
                             : subresource;
  return static_cast<size_t>(mipDimension(desc.width, level)) *
         static_cast<size_t>(mipDimension(desc.height, level)) *
         static_cast<size_t>(desc.type == dxmt9::core::TextureType::Volume
                                 ? mipDimension(desc.depth, level)
                                 : 1u) *
         texelBytes;
}

void initPalettizedTexture(D9CTexture* texture, uint32_t d3dFormat) {
  if (!texture || !isPalettizedD3DFormat(d3dFormat)) {
    return;
  }
  texture->d3dFormat = d3dFormat;
  texture->palettized = true;
  for (uint32_t i = 0; i < texture->p8Palette.size(); ++i) {
    texture->p8Palette[i] = 0xff000000u | (i << 16) | (i << 8) | i;
  }
  const auto& desc = texture->obj->desc();
  texture->p8Levels.resize(texture->obj->subresourceCount());
  const uint32_t texelBytes = palettizedTexelBytes(d3dFormat);
  for (uint32_t subresource = 0; subresource < texture->p8Levels.size(); ++subresource) {
    texture->p8Levels[subresource].assign(
        palettizedSubresourceBytes(desc, subresource, texelBytes), 0);
  }
}

void expandP8SubresourceToBackend(D9CTexture* texture, uint32_t subresource) {
  if (!texture || !texture->palettized || subresource >= texture->p8Levels.size()) {
    return;
  }
  const auto& desc = texture->obj->desc();
  const uint32_t mipLevels = std::max(1u, desc.levels);
  const uint32_t level = desc.type == dxmt9::core::TextureType::Cube
                             ? subresource % mipLevels
                             : subresource;
  const uint32_t width = mipDimension(desc.width, level);
  const uint32_t height = mipDimension(desc.height, level);
  const uint32_t depth = desc.type == dxmt9::core::TextureType::Volume
                             ? mipDimension(desc.depth, level)
                             : 1u;
  const uint32_t texelBytes = palettizedTexelBytes(texture->d3dFormat);
  const auto& indices = texture->p8Levels[subresource];
  if (indices.size() <
      static_cast<size_t>(width) * height * depth * texelBytes) {
    return;
  }

  auto lock = texture->obj->lockRect(subresource, nullptr, 0);
  if (!lock.data || lock.pitch == 0) {
    return;
  }
  auto* dst = static_cast<uint8_t*>(lock.data);
  const size_t sourceSlicePitch =
      static_cast<size_t>(width) * height * texelBytes;
  const size_t destSlicePitch = static_cast<size_t>(lock.pitch) * height;
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const size_t sourceOffset =
            static_cast<size_t>(z) * sourceSlicePitch +
            (static_cast<size_t>(y) * width + x) * texelBytes;
        const uint8_t index = indices[sourceOffset];
        const uint32_t color = texture->p8Palette[index];
        auto* pixel = dst + static_cast<size_t>(z) * destSlicePitch +
                      static_cast<size_t>(y) * lock.pitch +
                      static_cast<size_t>(x) * 4u;
        pixel[0] = static_cast<uint8_t>(color & 0xffu);
        pixel[1] = static_cast<uint8_t>((color >> 8) & 0xffu);
        pixel[2] = static_cast<uint8_t>((color >> 16) & 0xffu);
        pixel[3] = texture->d3dFormat == kD3DFmtA8P8
                       ? indices[sourceOffset + 1u]
                       : static_cast<uint8_t>((color >> 24) & 0xffu);
      }
    }
  }
  texture->obj->unlockRect(subresource);
}

void copyNativeToShadow(ShadowLock& shadow) {
  auto* dst = static_cast<uint8_t*>(shadow.shadow.ptr);
  auto* src = static_cast<const uint8_t*>(shadow.nativePtr);
  for (uint32_t row = 0; row < shadow.rows; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * shadow.nativePitch,
                src + static_cast<size_t>(row) * shadow.nativePitch,
                shadow.rowBytes);
  }
}

void copyShadowToNative(const ShadowLock& shadow) {
  auto* dst = static_cast<uint8_t*>(shadow.nativePtr);
  auto* src = static_cast<const uint8_t*>(shadow.shadow.ptr);
  for (uint32_t row = 0; row < shadow.rows; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * shadow.nativePitch,
                src + static_cast<size_t>(row) * shadow.nativePitch,
                shadow.rowBytes);
  }
}

}  // namespace

void dxmt9c_expand_palettized_subresource(D9CTexture* texture, uint32_t subresource) {
  expandP8SubresourceToBackend(texture, subresource);
}

bool dxmt9c_copy_palettized_subresource(D9CTexture* srcTexture,
                                        uint32_t srcSubresource,
                                        D9CTexture* dstTexture,
                                        uint32_t dstSubresource) {
  if (!srcTexture || !dstTexture || !srcTexture->palettized ||
      !dstTexture->palettized ||
      srcTexture->d3dFormat != dstTexture->d3dFormat ||
      srcSubresource >= srcTexture->p8Levels.size() ||
      dstSubresource >= dstTexture->p8Levels.size()) {
    return false;
  }
  // Match the validity checks dxmt9c_device_update_texture's palettized path
  // performs. Without them an invalidated texture still writes its shadow and
  // reports success, because expandP8SubresourceToBackend fails silently.
  if (!srcTexture->obj || !dstTexture->obj || !srcTexture->obj->valid() ||
      !dstTexture->obj->valid()) {
    return false;
  }
  const auto& srcDesc = srcTexture->obj->desc();
  const auto& dstDesc = dstTexture->obj->desc();
  const uint32_t texelBytes = palettizedTexelBytes(dstTexture->d3dFormat);
  const uint32_t srcMipLevels = std::max(1u, srcDesc.levels);
  const uint32_t dstMipLevels = std::max(1u, dstDesc.levels);
  const uint32_t srcLevel = srcDesc.type == dxmt9::core::TextureType::Cube
                                ? srcSubresource % srcMipLevels
                                : srcSubresource;
  const uint32_t dstLevel = dstDesc.type == dxmt9::core::TextureType::Cube
                                ? dstSubresource % dstMipLevels
                                : dstSubresource;
  const uint32_t srcWidth = mipDimension(srcDesc.width, srcLevel);
  const uint32_t dstWidth = mipDimension(dstDesc.width, dstLevel);
  const uint32_t srcHeight = mipDimension(srcDesc.height, srcLevel);
  const uint32_t dstHeight = mipDimension(dstDesc.height, dstLevel);
  const uint32_t srcDepth = srcDesc.type == dxmt9::core::TextureType::Volume
                                ? mipDimension(srcDesc.depth, srcLevel)
                                : 1u;
  const uint32_t dstDepth = dstDesc.type == dxmt9::core::TextureType::Volume
                                ? mipDimension(dstDesc.depth, dstLevel)
                                : 1u;
  const auto& srcIndices = srcTexture->p8Levels[srcSubresource];
  auto& dstIndices = dstTexture->p8Levels[dstSubresource];
  if (srcIndices.size() <
          palettizedSubresourceBytes(srcDesc, srcSubresource, texelBytes) ||
      dstIndices.size() <
          palettizedSubresourceBytes(dstDesc, dstSubresource, texelBytes)) {
    return false;
  }
  // D3D9 allows a smaller source than destination (UpdateSurface validates
  // only that the copy region fits), so clamp per axis rather than assuming
  // equal extents. dxmt9c_device_update_surface ignores srcRect/dstPoint —
  // a pre-existing gap on the non-palettized path too — so this copies from
  // each origin.
  const uint32_t width = std::min(srcWidth, dstWidth);
  const uint32_t height = std::min(srcHeight, dstHeight);
  const uint32_t depth = std::min(srcDepth, dstDepth);
  const size_t rowBytes = static_cast<size_t>(width) * texelBytes;
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < height; ++y) {
      const size_t srcOffset =
          ((static_cast<size_t>(z) * srcHeight + y) * srcWidth) * texelBytes;
      const size_t dstOffset =
          ((static_cast<size_t>(z) * dstHeight + y) * dstWidth) * texelBytes;
      std::memcpy(dstIndices.data() + dstOffset, srcIndices.data() + srcOffset,
                  rowBytes);
    }
  }
  if (!dstTexture->lockedLevels.contains(dstSubresource)) {
    expandP8SubresourceToBackend(dstTexture, dstSubresource);
  }
  return true;
}

extern "C" D9CTexture* dxmt9c_device_create_texture(D9CDevice* d, uint32_t w, uint32_t h,
                                                    uint32_t levels, uint32_t usage,
                                                    uint32_t fmt, uint32_t pool) {
  return dxmt9c_device_create_texture_shared(d, w, h, levels, usage, fmt, pool, nullptr);
}

extern "C" D9CTexture* dxmt9c_device_create_texture_shared(D9CDevice* d, uint32_t w, uint32_t h,
                                                           uint32_t levels, uint32_t usage,
                                                           uint32_t fmt, uint32_t pool,
                                                           uint64_t* sharedHandle) {
  dxmt9DebugLog("device_create_texture begin device=%p size=%ux%u levels=%u usage=0x%x fmt=%u(%s) pool=%u",
                static_cast<void*>(d), w, h, levels, usage, fmt,
                dxmt9::core::formatName(fmtFromD3D(fmt)).c_str(), pool);
  dxmt9::core::TextureDesc desc;
  desc.width = w;
  desc.height = h;
  desc.levels = resolveMipLevelCount(levels, usage, w, h, 1u);
  desc.format = fmtFromD3D(isPalettizedD3DFormat(fmt) ? kD3DFmtA8R8G8B8 : fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usageFromD3D(usage);
  desc.type = dxmt9::core::TextureType::TwoD;
  const SharedTextureSignature sharedSignature{
      w, h, 1u, desc.levels, usage, fmt, pool,
      static_cast<uint32_t>(desc.type)};
  if (invalidCompressedTextureDimensions(desc.format, desc.width, desc.height)) {
    dxmt9DebugLog("device_create_texture rejected compressed alignment size=%ux%u fmt=%u(%s)",
                  w, h, fmt, dxmt9::core::formatName(desc.format).c_str());
    return nullptr;
  }
  std::shared_ptr<dxmt9::core::Texture> sharedSource;
  if (sharedHandle && *sharedHandle != 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const auto entry = gSharedTextures.find(*sharedHandle);
    if (entry != gSharedTextures.end()) {
      if (entry->second.signature != sharedSignature) {
        return nullptr;
      }
      sharedSource = entry->second.resource;
    } else if (isDxmtSharedHandle(*sharedHandle)) {
      return nullptr;
    }
  }
  auto tex = sharedSource ? d->dev().openSharedTexture(sharedSource)
                          : d->iface->CreateTexture(desc);
  if (!tex) {
    dxmt9DebugLog("device_create_texture failed device=%p", static_cast<void*>(d));
    return nullptr;
  }
  dxmt9DebugLog("device_create_texture ok texture=%p levels=%u",
                static_cast<void*>(tex.get()), tex->levelCount());
  auto* out = new D9CTexture{tex, d};
  out->d3dFormat = fmt;
  out->sharedOpened = static_cast<bool>(sharedSource);
  initPalettizedTexture(out, fmt);
  if (sharedHandle && *sharedHandle == 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const uint64_t handle = allocateSharedHandleLocked();
    gSharedTextures.emplace(handle, SharedTextureEntry{sharedSignature, tex});
    *sharedHandle = handle;
  }
  return out;
}

extern "C" D9CTexture* dxmt9c_device_create_cube_texture(D9CDevice* d, uint32_t size,
                                                         uint32_t levels, uint32_t usage,
                                                         uint32_t fmt, uint32_t pool) {
  return dxmt9c_device_create_cube_texture_shared(d, size, levels, usage, fmt, pool, nullptr);
}

extern "C" D9CTexture* dxmt9c_device_create_cube_texture_shared(D9CDevice* d, uint32_t size,
                                                                uint32_t levels, uint32_t usage,
                                                                uint32_t fmt, uint32_t pool,
                                                                uint64_t* sharedHandle) {
  dxmt9::core::TextureDesc desc;
  desc.width = size;
  desc.height = size;
  desc.levels = resolveMipLevelCount(levels, usage, size, size, 1u);
  desc.format = fmtFromD3D(isPalettizedD3DFormat(fmt) ? kD3DFmtA8R8G8B8 : fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usageFromD3D(usage);
  desc.type = dxmt9::core::TextureType::Cube;
  const SharedTextureSignature sharedSignature{
      size, size, 1u, desc.levels, usage, fmt, pool,
      static_cast<uint32_t>(desc.type)};
  if (invalidCompressedTextureDimensions(desc.format, desc.width, desc.height)) {
    return nullptr;
  }
  std::shared_ptr<dxmt9::core::Texture> sharedSource;
  if (sharedHandle && *sharedHandle != 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const auto entry = gSharedTextures.find(*sharedHandle);
    if (entry != gSharedTextures.end()) {
      if (entry->second.signature != sharedSignature) {
        return nullptr;
      }
      sharedSource = entry->second.resource;
    } else if (isDxmtSharedHandle(*sharedHandle)) {
      return nullptr;
    }
  }
  auto tex = sharedSource ? d->dev().openSharedTexture(sharedSource)
                          : d->iface->CreateTexture(desc);
  if (!tex) {
    return nullptr;
  }
  auto* out = new D9CTexture{tex, d};
  out->d3dFormat = fmt;
  out->sharedOpened = static_cast<bool>(sharedSource);
  initPalettizedTexture(out, fmt);
  if (sharedHandle && *sharedHandle == 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const uint64_t handle = allocateSharedHandleLocked();
    gSharedTextures.emplace(handle, SharedTextureEntry{sharedSignature, tex});
    *sharedHandle = handle;
  }
  return out;
}

extern "C" D9CTexture* dxmt9c_device_create_volume_texture(D9CDevice* d, uint32_t w, uint32_t h,
                                                           uint32_t depth, uint32_t levels,
                                                           uint32_t usage, uint32_t fmt,
                                                           uint32_t pool) {
  return dxmt9c_device_create_volume_texture_shared(
      d, w, h, depth, levels, usage, fmt, pool, nullptr);
}

extern "C" D9CTexture* dxmt9c_device_create_volume_texture_shared(
    D9CDevice* d, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
    uint32_t usage, uint32_t fmt, uint32_t pool, uint64_t* sharedHandle) {
  dxmt9::core::TextureDesc desc;
  desc.width = w;
  desc.height = h;
  desc.depth = depth;
  desc.levels = resolveMipLevelCount(levels, usage, w, h, depth);
  desc.format = fmtFromD3D(isPalettizedD3DFormat(fmt) ? kD3DFmtA8R8G8B8 : fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usageFromD3D(usage);
  desc.type = dxmt9::core::TextureType::Volume;
  const SharedTextureSignature sharedSignature{
      w, h, depth, desc.levels, usage, fmt, pool,
      static_cast<uint32_t>(desc.type)};
  if (invalidCompressedTextureDimensions(desc.format, desc.width, desc.height)) {
    return nullptr;
  }
  std::shared_ptr<dxmt9::core::Texture> sharedSource;
  if (sharedHandle && *sharedHandle != 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const auto entry = gSharedTextures.find(*sharedHandle);
    if (entry != gSharedTextures.end()) {
      if (entry->second.signature != sharedSignature) {
        return nullptr;
      }
      sharedSource = entry->second.resource;
    } else if (isDxmtSharedHandle(*sharedHandle)) {
      return nullptr;
    }
  }
  auto tex = sharedSource ? d->dev().openSharedTexture(sharedSource)
                          : d->iface->CreateTexture(desc);
  if (!tex) {
    return nullptr;
  }
  auto* out = new D9CTexture{tex, d};
  out->d3dFormat = fmt;
  out->sharedOpened = static_cast<bool>(sharedSource);
  initPalettizedTexture(out, fmt);
  if (sharedHandle && *sharedHandle == 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const uint64_t handle = allocateSharedHandleLocked();
    gSharedTextures.emplace(handle, SharedTextureEntry{sharedSignature, tex});
    *sharedHandle = handle;
  }
  return out;
}

extern "C" D9CBuffer* dxmt9c_device_create_vertex_buffer(D9CDevice* d, uint32_t len,
                                                         uint32_t usage, uint32_t fvf,
                                                         uint32_t pool) {
  return dxmt9c_device_create_vertex_buffer_shared(d, len, usage, fvf, pool, nullptr);
}

extern "C" D9CBuffer* dxmt9c_device_create_vertex_buffer_shared(D9CDevice* d, uint32_t len,
                                                                uint32_t usage, uint32_t fvf,
                                                                uint32_t pool,
                                                                uint64_t* sharedHandle) {
  dxmt9::core::BufferDesc desc{len, poolFromD3D(pool),
                               static_cast<uint32_t>(usageFromD3D(usage) |
                                                     dxmt9::core::UsageVertexBuffer)};
  const SharedBufferSignature sharedSignature{len, usage, fvf, pool, 1u};
  std::shared_ptr<dxmt9::core::Buffer> sharedSource;
  if (sharedHandle && *sharedHandle != 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const auto entry = gSharedBuffers.find(*sharedHandle);
    if (entry != gSharedBuffers.end()) {
      if (entry->second.signature != sharedSignature) {
        return nullptr;
      }
      sharedSource = entry->second.resource;
    } else if (isDxmtSharedHandle(*sharedHandle)) {
      return nullptr;
    }
  }
  auto buf = sharedSource ? d->dev().openSharedBuffer(sharedSource)
                          : d->iface->CreateBuffer(desc);
  if (!buf) {
    return nullptr;
  }
  auto* out = new D9CBuffer{buf, d};
  out->desc.size = len;
  out->desc.usage = usage;
  out->desc.pool = pool;
  out->desc.fvf = fvf;
  out->desc.format = 0;
  out->sharedOpened = static_cast<bool>(sharedSource);
  if (sharedHandle && *sharedHandle == 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const uint64_t handle = allocateSharedHandleLocked();
    gSharedBuffers.emplace(handle, SharedBufferEntry{sharedSignature, buf});
    *sharedHandle = handle;
  }
  return out;
}

extern "C" D9CBuffer* dxmt9c_device_create_index_buffer(D9CDevice* d, uint32_t len,
                                                        uint32_t usage, uint32_t fmt,
                                                        uint32_t pool) {
  return dxmt9c_device_create_index_buffer_shared(d, len, usage, fmt, pool, nullptr);
}

extern "C" D9CBuffer* dxmt9c_device_create_index_buffer_shared(D9CDevice* d, uint32_t len,
                                                               uint32_t usage, uint32_t fmt,
                                                               uint32_t pool,
                                                               uint64_t* sharedHandle) {
  dxmt9::core::BufferDesc desc{len, poolFromD3D(pool),
                               static_cast<uint32_t>(usageFromD3D(usage) |
                                                     dxmt9::core::UsageIndexBuffer)};
  const SharedBufferSignature sharedSignature{len, usage, fmt, pool, 2u};
  std::shared_ptr<dxmt9::core::Buffer> sharedSource;
  if (sharedHandle && *sharedHandle != 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const auto entry = gSharedBuffers.find(*sharedHandle);
    if (entry != gSharedBuffers.end()) {
      if (entry->second.signature != sharedSignature) {
        return nullptr;
      }
      sharedSource = entry->second.resource;
    } else if (isDxmtSharedHandle(*sharedHandle)) {
      return nullptr;
    }
  }
  auto buf = sharedSource ? d->dev().openSharedBuffer(sharedSource)
                          : d->iface->CreateBuffer(desc);
  if (!buf) {
    return nullptr;
  }
  auto* out = new D9CBuffer{buf, d};
  out->desc.size = len;
  out->desc.usage = usage;
  out->desc.pool = pool;
  out->desc.fvf = 0;
  out->desc.format = fmt;
  out->sharedOpened = static_cast<bool>(sharedSource);
  if (sharedHandle && *sharedHandle == 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const uint64_t handle = allocateSharedHandleLocked();
    gSharedBuffers.emplace(handle, SharedBufferEntry{sharedSignature, buf});
    *sharedHandle = handle;
  }
  return out;
}

extern "C" D9CSurface* dxmt9c_device_create_render_target(D9CDevice* d, uint32_t w, uint32_t h,
                                                          uint32_t fmt, uint32_t msType,
                                                          uint32_t msQuality, uint32_t lockable,
                                                          uint64_t* sharedHandle) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w;
  desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.renderTarget = true;
  desc.multiSampleType = msTypeFromD3D(msType);
  const SharedSurfaceSignature sharedSignature{
      w, h, fmt, msType, msQuality, lockable, 0u, 1u};
  std::shared_ptr<dxmt9::core::Surface> sharedSource;
  if (sharedHandle && *sharedHandle != 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const auto entry = gSharedSurfaces.find(*sharedHandle);
    if (entry != gSharedSurfaces.end()) {
      if (entry->second.signature != sharedSignature) {
        return nullptr;
      }
      sharedSource = entry->second.resource;
    } else if (isDxmtSharedHandle(*sharedHandle)) {
      return nullptr;
    }
  }
  auto surf = sharedSource ? d->dev().openSharedSurface(sharedSource)
                           : d->iface->CreateSurface(desc);
  if (!surf) {
    return nullptr;
  }
  auto* out = new D9CSurface{surf, nullptr, 0u, d};
  out->sharedOpened = static_cast<bool>(sharedSource);
  if (sharedHandle && *sharedHandle == 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const uint64_t handle = allocateSharedHandleLocked();
    gSharedSurfaces.emplace(handle, SharedSurfaceEntry{sharedSignature, surf});
    *sharedHandle = handle;
  }
  return out;
}

extern "C" D9CSurface* dxmt9c_device_create_depth_stencil(D9CDevice* d, uint32_t w, uint32_t h,
                                                          uint32_t fmt, uint32_t msType,
                                                          uint32_t msQuality, uint32_t discard,
                                                          uint64_t* sharedHandle) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w;
  desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.depthStencil = true;
  desc.multiSampleType = msTypeFromD3D(msType);
  const SharedSurfaceSignature sharedSignature{
      w, h, fmt, msType, msQuality, discard, 0u, 2u};
  std::shared_ptr<dxmt9::core::Surface> sharedSource;
  if (sharedHandle && *sharedHandle != 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const auto entry = gSharedSurfaces.find(*sharedHandle);
    if (entry != gSharedSurfaces.end()) {
      if (entry->second.signature != sharedSignature) {
        return nullptr;
      }
      sharedSource = entry->second.resource;
    } else if (isDxmtSharedHandle(*sharedHandle)) {
      return nullptr;
    }
  }
  auto surf = sharedSource ? d->dev().openSharedSurface(sharedSource)
                           : d->iface->CreateSurface(desc);
  if (!surf) {
    return nullptr;
  }
  auto* out = new D9CSurface{surf, nullptr, 0u, d};
  out->sharedOpened = static_cast<bool>(sharedSource);
  if (sharedHandle && *sharedHandle == 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const uint64_t handle = allocateSharedHandleLocked();
    gSharedSurfaces.emplace(handle, SharedSurfaceEntry{sharedSignature, surf});
    *sharedHandle = handle;
  }
  return out;
}

extern "C" D9CSurface* dxmt9c_device_create_offscreen_surface(D9CDevice* d, uint32_t w,
                                                              uint32_t h, uint32_t fmt,
                                                              uint32_t pool,
                                                              uint64_t* sharedHandle) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w;
  desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  const SharedSurfaceSignature sharedSignature{
      w, h, fmt, 0u, 0u, 0u, pool, 3u};
  std::shared_ptr<dxmt9::core::Surface> sharedSource;
  if (sharedHandle && *sharedHandle != 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const auto entry = gSharedSurfaces.find(*sharedHandle);
    if (entry != gSharedSurfaces.end()) {
      if (entry->second.signature != sharedSignature) {
        return nullptr;
      }
      sharedSource = entry->second.resource;
    } else if (isDxmtSharedHandle(*sharedHandle)) {
      return nullptr;
    }
  }
  auto surf = sharedSource ? d->dev().openSharedSurface(sharedSource)
                           : d->iface->CreateSurface(desc);
  if (!surf) {
    return nullptr;
  }
  auto* out = new D9CSurface{surf, nullptr, 0u, d};
  out->sharedOpened = static_cast<bool>(sharedSource);
  if (sharedHandle && *sharedHandle == 0u) {
    std::lock_guard lock(gSharedResourceMutex);
    const uint64_t handle = allocateSharedHandleLocked();
    gSharedSurfaces.emplace(handle, SharedSurfaceEntry{sharedSignature, surf});
    *sharedHandle = handle;
  }
  return out;
}

extern "C" void dxmt9c_texture_addref(D9CTexture* t) {
  if (t) {
    t->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_texture_release(D9CTexture* t) {
  if (!t) {
    return 0;
  }
  const uint32_t refs = t->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    for (auto& [_, lock] : t->wow64Locks) {
      releaseShadowLock(lock);
    }
    delete t;
  }
  return refs;
}

extern "C" int32_t dxmt9c_texture_lock_rect(D9CTexture* t, uint32_t level, D9CLockedRect* out,
                                            const D9CRect* r, uint32_t flags) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (t->lockedLevels.contains(level)) {
    dxmt9DebugLog("texture_lock_rect rejected double-lock texture=%p level=%u",
                  static_cast<void*>(t), level);
    out->pitch = 0;
    out->bits = nullptr;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (r) {
    dxmt9DebugLog("texture_lock_rect begin texture=%p level=%u flags=0x%x rect=(%d,%d)-(%d,%d)",
                  static_cast<void*>(t), level, flags, r->left, r->top, r->right, r->bottom);
  } else {
    dxmt9DebugLog("texture_lock_rect begin texture=%p level=%u flags=0x%x rect=<full>",
                  static_cast<void*>(t), level, flags);
  }
  if (t->palettized) {
    const auto& desc = t->obj->desc();
    if (level >= t->p8Levels.size()) {
      out->pitch = 0;
      out->bits = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    const uint32_t mipLevel = desc.type == dxmt9::core::TextureType::Cube
                                  ? level % std::max(1u, desc.levels)
                                  : level;
    const uint32_t width = mipDimension(desc.width, mipLevel);
    const uint32_t height = mipDimension(desc.height, mipLevel);
    const uint32_t depth = desc.type == dxmt9::core::TextureType::Volume
                               ? mipDimension(desc.depth, mipLevel)
                               : 1u;
    const uint32_t left = r ? static_cast<uint32_t>(std::clamp(r->left, 0, static_cast<int32_t>(width))) : 0u;
    const uint32_t top = r ? static_cast<uint32_t>(std::clamp(r->top, 0, static_cast<int32_t>(height))) : 0u;
    const uint32_t texelBytes = palettizedTexelBytes(t->d3dFormat);
    if (t->p8Levels[level].size() <
        static_cast<size_t>(width) * height * depth * texelBytes) {
      t->p8Levels[level].assign(
          static_cast<size_t>(width) * height * depth * texelBytes, 0);
    }
    out->pitch = static_cast<int32_t>(width * texelBytes);
    out->bits = t->p8Levels[level].data() +
                static_cast<size_t>(top) * out->pitch +
                static_cast<size_t>(left) * texelBytes;
    t->lockedLevels.insert(level);
    dxmt9DebugLog("texture_lock_rect p8 texture=%p level=%u pitch=%d bits=%p",
                  static_cast<void*>(t), level, out->pitch, out->bits);
    return dxmt9::core::D3D_OK;
  }
  auto* rect = r ? new dxmt9::core::Rect{r->left, r->top, r->right, r->bottom} : nullptr;
  auto lock = t->obj->lockRect(level, rect, lockFlagsToCore(flags));
  delete rect;
  out->pitch = static_cast<int32_t>(lock.pitch);
  out->bits = lock.data;
  if (!lock.data || lock.pitch == 0) {
    dxmt9DebugLog("texture_lock_rect failed texture=%p level=%u pitch=%u bits=%p",
                  static_cast<void*>(t), level, lock.pitch, lock.data);
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (lock.data && requiresWow64PointerShadow() && !pointerFits32Bit(lock.data)) {
    const auto& desc = t->obj->desc();
    const uint32_t levelWidth = std::max(1u, desc.width >> std::min(level, 31u));
    const uint32_t levelHeight = std::max(1u, desc.height >> std::min(level, 31u));
    const auto footprint = lockFootprint(desc.format, levelWidth, levelHeight, r);
    const uint32_t rowBytes = footprint.rowBytes;
    const uint32_t rows = footprint.rows;
    // SFIV BC3 level-9 page-fault repro (2026-05-10): for tiny mips
    // (e.g. 1x1 BC3) Metal/the runtime reports `lock.pitch` equal to
    // the BASE-level row pitch (1024 for a 256x256 BC3 base), and the
    // game walks the lock pointer as if it had base-level rows worth
    // of storage — observed writes at offsets 0x1000, 0x2000, 0x4000
    // from the lock pointer. footprint.rows is the LEVEL block-row
    // count (1 for a 1x1 mip), so `lock.pitch * rows` heavily
    // underestimates the worst-case extent. Use the parent texture's
    // full block-padded height as the upper bound (and let
    // computeShadowBytesUpperBound enforce its own floor on top).
    // copyNativeToShadow / copyShadowToNative continue to copy
    // `rowBytes * rows` block-rows — that part is correct; only the
    // allocation upper bound widens.
    const uint32_t rectHeight =
        r ? static_cast<uint32_t>(std::max<int32_t>(
                0, std::clamp(r->bottom, 0, static_cast<int32_t>(levelHeight)) -
                       std::clamp(r->top, 0, static_cast<int32_t>(levelHeight))))
          : levelHeight;
    const uint32_t blockHeight = dxmt9::core::formatBlockHeight(desc.format);
    // When the lock's reported pitch exceeds the strictly-correct
    // row pitch for the level (`> rowBytes`), the game evidently
    // treats the lock as a base-level surface; size against the
    // base-level height. Otherwise the level's own height is fine.
    const uint32_t levelRowPitch =
        dxmt9::core::formatRowPitch(desc.format, levelWidth);
    const uint32_t effectiveHeight =
        (rowBytes != 0 && lock.pitch > levelRowPitch) ? desc.height : rectHeight;
    const size_t shadowBytes =
        computeShadowBytesUpperBound(lock.pitch, effectiveHeight, blockHeight);
    auto& shadow = t->wow64Locks[level];
    if (rowBytes == 0 || rows == 0) {
      dxmt9DebugLog("texture_lock_rect shadow alloc failed texture=%p level=%u nativeBits=%p rowBytes=%u rows=%u",
                    static_cast<void*>(t), level, lock.data, rowBytes, rows);
      out->pitch = 0;
      out->bits = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    if (!shadow.shadow || shadow.shadow.size < shadowBytes) {
      releaseShadowLock(shadow);
      shadow.shadow = allocateLow4GB(shadowBytes);
    }
    if (!shadow.shadow) {
      dxmt9DebugLog("texture_lock_rect shadow alloc failed texture=%p level=%u nativeBits=%p rowBytes=%u rows=%u",
                    static_cast<void*>(t), level, lock.data, rowBytes, rows);
      out->pitch = 0;
      out->bits = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    shadow.nativePtr = lock.data;
    shadow.nativePitch = lock.pitch;
    shadow.rowBytes = rowBytes;
    shadow.rows = rows;
    shadow.active = true;
    copyNativeToShadow(shadow);
    out->pitch = static_cast<int32_t>(shadow.nativePitch);
    out->bits = shadow.shadow.ptr;
    dxmt9DebugLog("texture_lock_rect shadow texture=%p level=%u nativeBits=%p shadowBits=%p pitch=%u rowBytes=%u rows=%u",
                  static_cast<void*>(t), level, shadow.nativePtr, out->bits,
                  shadow.nativePitch, rowBytes, rows);
  }
  t->lockedLevels.insert(level);
  dxmt9DebugLog("texture_lock_rect ok texture=%p level=%u pitch=%d bits=%p",
                static_cast<void*>(t), level, out->pitch, out->bits);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_texture_unlock_rect(D9CTexture* t, uint32_t level) {
  if (!t->lockedLevels.erase(level)) {
    dxmt9DebugLog("texture_unlock_rect rejected not-locked texture=%p level=%u",
                  static_cast<void*>(t), level);
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (t->palettized) {
    expandP8SubresourceToBackend(t, level);
    dxmt9DebugLog("texture_unlock_rect p8 texture=%p level=%u", static_cast<void*>(t), level);
    return dxmt9::core::D3D_OK;
  }
  if (auto it = t->wow64Locks.find(level); it != t->wow64Locks.end()) {
    auto& shadow = it->second;
    if (shadow.active) {
      copyShadowToNative(shadow);
      shadow.active = false;
      dxmt9DebugLog("texture_unlock_rect shadow texture=%p level=%u nativeBits=%p shadowBits=%p rowBytes=%u rows=%u",
                    static_cast<void*>(t), level, shadow.nativePtr, shadow.shadow.ptr,
                    shadow.rowBytes, shadow.rows);
    }
  }
  dxmt9DebugLog("texture_unlock_rect texture=%p level=%u", static_cast<void*>(t), level);
  t->obj->unlockRect(level);
  return dxmt9::core::D3D_OK;
}

extern "C" D9CSurface* dxmt9c_texture_get_surface_level(D9CTexture* t, uint32_t level) {
  auto surf = t->obj->surfaceLevel(level);
  if (!surf) {
    return nullptr;
  }
  auto* wrap = new D9CSurface{surf, t, level};
  t->refs.fetch_add(1);
  return wrap;
}

extern "C" uint32_t dxmt9c_texture_get_level_count(D9CTexture* t) {
  return t->obj->levelCount();
}

extern "C" int32_t dxmt9c_texture_get_level_desc(D9CTexture* t, uint32_t level,
                                                 D9CSurfaceDesc* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  std::memset(out, 0, sizeof(*out));
  if (level >= t->obj->levelCount()) {
    dxmt9DebugLog("texture_get_level_desc rejected texture=%p level=%u levelCount=%u",
                  static_cast<void*>(t), level, t->obj->levelCount());
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto& desc = t->obj->desc();
  const uint32_t shift = std::min<uint32_t>(level, 31);
  out->format = t->d3dFormat ? t->d3dFormat : fmtToD3D(desc.format);
  out->resourceType = textureTypeToResourceType(desc.type);
  out->usage = usageToD3D(desc.usage);
  out->pool = poolToD3D(desc.pool);
  out->multiSampleType = 0;
  out->multiSampleQuality = 0;
  out->width = std::max(1u, desc.width >> shift);
  out->height = std::max(1u, desc.height >> shift);
  out->depth = desc.type == dxmt9::core::TextureType::Volume
                   ? std::max(1u, desc.depth >> shift)
                   : 1u;
  dxmt9DebugLog("texture_get_level_desc texture=%p level=%u fmt=%u(%s) usage=0x%x pool=%u size=%ux%u",
                static_cast<void*>(t), level, out->format,
                dxmt9::core::formatName(desc.format).c_str(), out->usage, out->pool, out->width,
                out->height);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_texture_generate_mip_sublevels(D9CTexture* t) {
  if (!t) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return t->obj->generateMipSubLevels();
}

extern "C" uint32_t dxmt9c_texture_set_lod(D9CTexture* t, uint32_t lod) {
  if (!t || !t->obj) {
    return 0;
  }
  return t->obj->setLod(lod);
}

extern "C" int32_t dxmt9c_texture_sample_2d(D9CTexture* t, uint32_t level,
                                            float u, float v,
                                            float* outRgba4) {
  if (!t || !t->obj || !outRgba4) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const auto& desc = t->obj->desc();
  if (desc.type != dxmt9::core::TextureType::TwoD ||
      dxmt9::core::bytesPerPixel(desc.format) != 4u ||
      level >= t->obj->levelCount()) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const uint32_t width = mipDimension(desc.width, level);
  const uint32_t height = mipDimension(desc.height, level);
  auto lock = t->obj->lockRect(level, nullptr, 0);
  if (!lock.data || lock.pitch == 0u) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const auto clampTexel = [](float value, uint32_t size) -> uint32_t {
    const long texel =
        static_cast<long>(std::floor(value * static_cast<float>(size)));
    return static_cast<uint32_t>(
        std::clamp<long>(texel, 0, static_cast<long>(size - 1u)));
  };
  const uint32_t x = clampTexel(u, width);
  const uint32_t y = clampTexel(v, height);
  const auto* pixel = static_cast<const uint8_t*>(lock.data) +
                      static_cast<size_t>(y) * lock.pitch +
                      static_cast<size_t>(x) * 4u;
  outRgba4[0] = static_cast<float>(pixel[2]) / 255.0f;
  outRgba4[1] = static_cast<float>(pixel[1]) / 255.0f;
  outRgba4[2] = static_cast<float>(pixel[0]) / 255.0f;
  outRgba4[3] = static_cast<float>(pixel[3]) / 255.0f;
  t->obj->unlockRect(level);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_texture_set_palette(D9CTexture* t,
                                               const uint32_t* argbEntries,
                                               uint32_t entryCount) {
  if (!t || !t->palettized || !argbEntries || entryCount < 256u) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  std::copy_n(argbEntries, 256u, t->p8Palette.begin());
  for (uint32_t subresource = 0; subresource < t->p8Levels.size(); ++subresource) {
    if (!t->lockedLevels.contains(subresource)) {
      expandP8SubresourceToBackend(t, subresource);
    }
  }
  return dxmt9::core::D3D_OK;
}

extern "C" void dxmt9c_buffer_addref(D9CBuffer* b) {
  if (b) {
    b->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_buffer_release(D9CBuffer* b) {
  if (!b) {
    return 0;
  }
  const uint32_t refs = b->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    releaseShadowLock(b->wow64Lock);
    delete b;
  }
  return refs;
}

extern "C" int32_t dxmt9c_buffer_lock(D9CBuffer* b, uint32_t offset, uint32_t size, void** data,
                                      uint32_t flags) {
  if (!data || !b || !b->obj) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const auto& desc = b->obj->desc();
  if (offset > desc.size || (size != 0u && size > desc.size - offset)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const uint32_t actualSize = size != 0u ? size : desc.size - offset;
  const uint64_t handle = bufferHandleValue(b);
  const bool traceBuffer = shouldTraceBufferHandle(b);
  dxmt9DebugLog("buffer_lock begin buffer=%p handle=0x%llx offset=%u size=%u actual_size=%u flags=0x%x desc_size=%u",
                static_cast<void*>(b), static_cast<unsigned long long>(handle), offset, size,
                actualSize, flags, desc.size);
  if (traceBuffer) {
    bufferTraceLog("buffer_trace lock_begin buffer=%p handle=0x%llx offset=%u size=%u actual_size=%u flags=0x%x desc_size=%u usage=0x%x pool=%u fvf=0x%x",
                   static_cast<void*>(b), static_cast<unsigned long long>(handle), offset, size,
                   actualSize, flags, b->desc.size, b->desc.usage,
                   static_cast<uint32_t>(b->desc.pool), b->desc.fvf);
  }
  b->lastLockReadOnly = (flags & kD3DLockReadOnly) != 0;
  b->lastLockOffset = offset;
  b->lastLockSize = actualSize;
  b->lastLockFlags = flags;
  const auto start = std::chrono::steady_clock::now();
  auto lock = b->obj->lock(offset, actualSize, lockFlagsToCore(flags));
  *data = lock.data;
  bool shadowCopy = false;
  std::uint64_t shadowAllocNs = 0;
  std::uint64_t shadowCopyNs = 0;
  if (lock.data && requiresWow64PointerShadow() && !pointerFits32Bit(lock.data)) {
    if (!b->wow64Lock.shadow || b->wow64Lock.shadow.size < actualSize) {
      releaseShadowLock(b->wow64Lock);
      const auto allocStart = std::chrono::steady_clock::now();
      b->wow64Lock.shadow = allocateLow4GB(actualSize);
      shadowAllocNs += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - allocStart).count());
    }
    if (!b->wow64Lock.shadow) {
      dxmt9DebugLog("buffer_lock shadow alloc failed buffer=%p native=%p size=%u",
                    static_cast<void*>(b), lock.data, actualSize);
      *data = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    b->wow64Lock.nativePtr = lock.data;
    b->wow64Lock.rowBytes = actualSize;
    b->wow64Lock.rows = 1;
    b->wow64Lock.active = true;
    const auto copyStart = std::chrono::steady_clock::now();
    std::memcpy(b->wow64Lock.shadow.ptr, lock.data, actualSize);
    shadowCopyNs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - copyStart).count());
    shadowCopy = true;
    *data = b->wow64Lock.shadow.ptr;
    dxmt9DebugLog("buffer_lock shadow buffer=%p native=%p shadow=%p size=%u",
                  static_cast<void*>(b), lock.data, *data, actualSize);
  }
  const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start).count();
  dxmt9::perf::countD3D9BufferLock(
      static_cast<std::uint64_t>(elapsedNs),
      actualSize,
      shadowAllocNs,
      shadowCopyNs,
      flags,
      b->desc.usage,
      static_cast<std::uint32_t>(b->desc.pool),
      offset == 0u && actualSize == desc.size,
      shadowCopy);
  dxmt9DebugLog("buffer_lock ok buffer=%p handle=0x%llx data=%p",
                static_cast<void*>(b), static_cast<unsigned long long>(handle), *data);
  if (traceBuffer) {
    bufferTraceLog("buffer_trace lock_ok buffer=%p handle=0x%llx data=%p pitch=%u shadow=%u",
                   static_cast<void*>(b), static_cast<unsigned long long>(handle), *data,
                   lock.pitch, shadowCopy ? 1u : 0u);
  }
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_buffer_unlock(D9CBuffer* b) {
  if (!b || !b->obj) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const uint64_t handle = bufferHandleValue(b);
  const bool traceBuffer = shouldTraceBufferHandle(b);
  if (b->wow64Lock.active) {
    if (!b->lastLockReadOnly) {
      std::memcpy(b->wow64Lock.nativePtr, b->wow64Lock.shadow.ptr, b->wow64Lock.rowBytes);
      dxmt9DebugLog("buffer_unlock shadow writeback buffer=%p native=%p shadow=%p size=%u",
                    static_cast<void*>(b), b->wow64Lock.nativePtr, b->wow64Lock.shadow.ptr,
                    b->wow64Lock.rowBytes);
    } else {
      dxmt9DebugLog("buffer_unlock shadow readonly buffer=%p native=%p shadow=%p size=%u",
                    static_cast<void*>(b), b->wow64Lock.nativePtr, b->wow64Lock.shadow.ptr,
                    b->wow64Lock.rowBytes);
    }
    b->wow64Lock.nativePtr = nullptr;
    b->wow64Lock.nativePitch = 0;
    b->wow64Lock.rowBytes = 0;
    b->wow64Lock.rows = 0;
    b->wow64Lock.active = false;
  }
  b->obj->unlock(!b->lastLockReadOnly);
  if (traceBuffer) {
    bufferTraceLog("buffer_trace unlock buffer=%p handle=0x%llx offset=%u actual_size=%u flags=0x%x readonly=%u upload=%u",
                   static_cast<void*>(b), static_cast<unsigned long long>(handle),
                   b->lastLockOffset, b->lastLockSize, b->lastLockFlags,
                   b->lastLockReadOnly ? 1u : 0u, b->lastLockReadOnly ? 0u : 1u);
  }
  b->lastLockReadOnly = false;
  b->lastLockOffset = 0;
  b->lastLockSize = 0;
  b->lastLockFlags = 0;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_buffer_get_desc(D9CBuffer* b, D9CBufferDesc* out) {
  if (!b || !out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  *out = b->desc;
  return dxmt9::core::D3D_OK;
}

extern "C" void dxmt9c_surface_addref(D9CSurface* s) {
  if (s) {
    s->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_surface_release(D9CSurface* s) {
  if (!s) {
    return 0;
  }
  const uint32_t refs = s->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    releaseShadowLock(s->wow64Lock);
    if (s->ownerTex) {
      dxmt9c_texture_release(s->ownerTex);
    }
    delete s;
  }
  return refs;
}

extern "C" int32_t dxmt9c_surface_lock_rect(D9CSurface* s, D9CLockedRect* out, const D9CRect* r,
                                            uint32_t flags) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (s->ownerTex) {
    if (s->ownerTex->lockedLevels.contains(s->ownerLevel)) {
      dxmt9DebugLog("surface_lock_rect rejected double-lock surface=%p ownerTexture=%p level=%u",
                    static_cast<void*>(s), static_cast<void*>(s->ownerTex), s->ownerLevel);
      out->pitch = 0;
      out->bits = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  } else if (s->locked) {
    dxmt9DebugLog("surface_lock_rect rejected double-lock surface=%p",
                  static_cast<void*>(s));
    out->pitch = 0;
    out->bits = nullptr;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (r) {
    dxmt9DebugLog("surface_lock_rect begin surface=%p flags=0x%x rect=(%d,%d)-(%d,%d)",
                  static_cast<void*>(s), flags, r->left, r->top, r->right, r->bottom);
  } else {
    dxmt9DebugLog("surface_lock_rect begin surface=%p flags=0x%x rect=<full>",
                  static_cast<void*>(s), flags);
  }
  auto* rect = r ? new dxmt9::core::Rect{r->left, r->top, r->right, r->bottom} : nullptr;
  auto lock = s->obj->lockRect(rect, lockFlagsToCore(flags));
  delete rect;
  out->pitch = static_cast<int32_t>(lock.pitch);
  out->bits = lock.data;
  if (!lock.data || lock.pitch == 0) {
    dxmt9DebugLog("surface_lock_rect failed surface=%p pitch=%u bits=%p",
                  static_cast<void*>(s), lock.pitch, lock.data);
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (lock.data && requiresWow64PointerShadow() && !pointerFits32Bit(lock.data)) {
    const auto& desc = s->obj->desc();
    const uint32_t nativePitch = static_cast<uint32_t>(std::abs(out->pitch));
    const uint32_t rectHeight = static_cast<uint32_t>(std::max<int32_t>(
        0, (r ? std::clamp(r->bottom, 0, static_cast<int32_t>(desc.height))
              : static_cast<int32_t>(desc.height)) -
               (r ? std::clamp(r->top, 0, static_cast<int32_t>(desc.height)) : 0)));
    uint32_t rowBytes = nativePitch;
    uint32_t rows = rectHeight;
    if (dxmt9::core::isCompressedFormat(desc.format)) {
      const auto footprint = lockFootprint(desc.format, desc.width, desc.height, r);
      rowBytes = footprint.rowBytes;
      rows = footprint.rows;
    }
    // See texture_lock_rect note: footprint.rows is the block-row count
    // and `nativePitch` is the native row stride; games may iterate by
    // texel row (or write a full base-level worth on tiny mips), so
    // size the shadow against rectHeight (or the base-level height
    // when the lock's pitch leaks the parent-level row stride).
    const uint32_t blockHeight = dxmt9::core::formatBlockHeight(desc.format);
    const uint32_t levelRowPitch =
        dxmt9::core::formatRowPitch(desc.format, desc.width);
    const uint32_t effectiveHeight =
        (rowBytes != 0 && nativePitch > levelRowPitch) ? desc.height : rectHeight;
    const size_t bytes =
        computeShadowBytesUpperBound(nativePitch, effectiveHeight, blockHeight);
    if (bytes != 0) {
      auto& shadow = s->ownerTex ? s->ownerTex->wow64Locks[s->ownerLevel] : s->wow64Lock;
      if (!shadow.shadow || shadow.shadow.size < bytes) {
        releaseShadowLock(shadow);
        shadow.shadow = allocateLow4GB(bytes);
      }
      if (!shadow.shadow) {
        out->bits = nullptr;
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      shadow.nativePtr = lock.data;
      shadow.nativePitch = nativePitch;
      shadow.rowBytes = rowBytes;
      shadow.rows = rows;
      shadow.active = true;
      copyNativeToShadow(shadow);
      out->bits = shadow.shadow.ptr;
      dxmt9DebugLog("surface_lock_rect shadow surface=%p native=%p shadow=%p pitch=%u rowBytes=%u rows=%u bytes=%zu",
                    static_cast<void*>(s), lock.data, out->bits, nativePitch,
                    rowBytes, rows, bytes);
    }
  }
  if (s->ownerTex) {
    s->ownerTex->lockedLevels.insert(s->ownerLevel);
  } else {
    s->locked = true;
  }
  dxmt9DebugLog("surface_lock_rect ok surface=%p pitch=%d bits=%p",
                static_cast<void*>(s), out->pitch, out->bits);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_surface_unlock_rect(D9CSurface* s) {
  if (s->ownerTex) {
    if (!s->ownerTex->lockedLevels.erase(s->ownerLevel)) {
      dxmt9DebugLog("surface_unlock_rect rejected not-locked surface=%p ownerTexture=%p level=%u",
                    static_cast<void*>(s), static_cast<void*>(s->ownerTex), s->ownerLevel);
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  } else if (!s->locked) {
    dxmt9DebugLog("surface_unlock_rect rejected not-locked surface=%p",
                  static_cast<void*>(s));
    return dxmt9::core::D3DERR_INVALIDCALL;
  } else {
    s->locked = false;
  }

  auto* shadow = s->ownerTex ? [&]() -> ShadowLock* {
    auto it = s->ownerTex->wow64Locks.find(s->ownerLevel);
    return it != s->ownerTex->wow64Locks.end() ? &it->second : nullptr;
  }() : &s->wow64Lock;
  if (shadow && shadow->active) {
    const size_t bytes =
        static_cast<size_t>(shadow->nativePitch) * static_cast<size_t>(shadow->rows);
    if (bytes != 0) {
      copyShadowToNative(*shadow);
      shadow->active = false;
      dxmt9DebugLog("surface_unlock_rect shadow surface=%p native=%p shadow=%p rowBytes=%u rows=%u bytes=%zu",
                    static_cast<void*>(s), shadow->nativePtr, shadow->shadow.ptr,
                    shadow->rowBytes, shadow->rows, bytes);
    }
  }
  dxmt9DebugLog("surface_unlock_rect surface=%p", static_cast<void*>(s));
  s->obj->unlockRect();
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_surface_get_desc(D9CSurface* s, D9CSurfaceDesc* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto& desc = s->obj->desc();
  std::memset(out, 0, sizeof(*out));
  out->format = fmtToD3D(desc.format);
  out->resourceType = 1;
  out->usage = usageToD3D(desc.usage);
  if (desc.renderTarget) {
    out->usage |= 0x00000001u;
  }
  if (desc.depthStencil) {
    out->usage |= 0x00000002u;
  }
  out->pool = poolToD3D(desc.pool);
  out->multiSampleType = msTypeToD3D(desc.multiSampleType);
  out->multiSampleQuality =
      desc.multiSampleType == dxmt9::core::MultiSampleType::None ? 0u : 1u;
  out->width = desc.width;
  out->height = desc.height;
  out->depth = 1;
  return dxmt9::core::D3D_OK;
}

extern "C" D9CTexture* dxmt9c_surface_get_container_texture(D9CSurface* s) {
  return s->ownerTex;
}
