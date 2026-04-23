#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9_transfers.hpp"

#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9_capture.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_resource_pool.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

namespace dxmt9::transfers {

namespace {

using core::BufferHandle;
using core::Format;
using core::Handle;
using core::ReadbackDesc;
using core::ReadbackPixels;
using core::Rect;
using core::TextureDesc;
using core::TextureHandle;

using dxmt9::core::metalcapture::gpuDumpTextureHandle;
using dxmt9::core::metalcapture::gpuDumpTexturePath;
using dxmt9::core::metalcapture::writeTextureBmp;
using dxmt9::core::metalqueue::CommandBufferDiagnostics;
using dxmt9::core::metalqueue::emitTextureTraceLine;

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

bool shouldDumpGpuTexture(Handle handle) {
  const u64 wanted = gpuDumpTextureHandle();
  return wanted != 0ull && handle.value == wanted;
}

void emitUploadTrace(resources::Pool& pool, TextureHandle handle, u32 level,
                      u32 width, u32 height, u32 pitch, std::span<const u8> bytes) {
  u32 minAlpha = 255u;
  u32 maxAlpha = 0u;
  u64 nonZeroAlpha = 0u;
  u64 nonZeroRgb = 0u;
  auto* rec = pool.findTexture(handle.value);
  if (rec && (rec->desc.format == Format::A8R8G8B8 || rec->desc.format == Format::A8B8G8R8 ||
              rec->desc.format == Format::X8R8G8B8 || rec->desc.format == Format::X8B8G8R8) &&
      pitch >= width * 4u) {
    for (u32 y = 0; y < height; ++y) {
      const u8* row = bytes.data() + static_cast<std::size_t>(y) * pitch;
      for (u32 x = 0; x < width; ++x) {
        const u8 b = row[static_cast<std::size_t>(x) * 4u + 0u];
        const u8 g = row[static_cast<std::size_t>(x) * 4u + 1u];
        const u8 r = row[static_cast<std::size_t>(x) * 4u + 2u];
        const u8 a = row[static_cast<std::size_t>(x) * 4u + 3u];
        minAlpha = std::min<u32>(minAlpha, a);
        maxAlpha = std::max<u32>(maxAlpha, a);
        nonZeroAlpha += (a != 0u) ? 1u : 0u;
        nonZeroRgb += (r != 0u || g != 0u || b != 0u) ? 1u : 0u;
      }
    }
  }
  std::ostringstream out;
  out << "[dxmt9-texture] upload handle=0x" << std::hex << handle.value << std::dec
      << " level=" << level
      << " size=" << width << "x" << height
      << " pitch=" << pitch
      << " bytes=" << bytes.size()
      << " alphaMin=" << minAlpha
      << " alphaMax=" << maxAlpha
      << " nonZeroAlpha=" << nonZeroAlpha
      << " nonZeroRgb=" << nonZeroRgb
      << " head=";
  const std::size_t preview = std::min<std::size_t>(16, bytes.size());
  for (std::size_t i = 0; i < preview; ++i) {
    if (i) out << ',';
    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]) << std::dec;
  }
  emitTextureTraceLine(out.str());
}

void dumpTextureSnapshotUnlocked(CommandQueue& queue,
                                   resources::Pool& pool,
                                   WMT::Reference<WMT::Device> device,
                                   Handle handle,
                                   const TextureDesc& desc,
                                   obj_handle_t sourceTextureHandle) {
  if (!sourceTextureHandle || !shouldDumpGpuTexture(handle) ||
      pool.dumpedGpuTextures.contains(handle.value)) {
    return;
  }
  if (desc.levels == 0 || desc.width == 0 || desc.height == 0) return;
  const char* path = gpuDumpTexturePath();
  if (!path || path[0] == '\0') return;
  if (desc.format != Format::A8R8G8B8 && desc.format != Format::X8R8G8B8 &&
      desc.format != Format::A8B8G8R8 && desc.format != Format::X8B8G8R8) {
    std::ostringstream out;
    out << "[dxmt9-texture] gpu-dump skip handle=0x" << std::hex << handle.value << std::dec
        << " unsupported-format=" << static_cast<unsigned>(desc.format);
    emitTextureTraceLine(out.str());
    pool.dumpedGpuTextures.insert(handle.value);
    return;
  }

  WMT::Texture srcTex{sourceTextureHandle};
  WMTTextureInfo stagingInfo{};
  stagingInfo.type = WMTTextureType2D;
  stagingInfo.pixel_format = srcTex.pixelFormat();
  stagingInfo.width = std::max(1u, desc.width);
  stagingInfo.height = std::max(1u, desc.height);
  stagingInfo.depth = 1; stagingInfo.mipmap_level_count = 1;
  stagingInfo.sample_count = 1; stagingInfo.array_length = 1;
  stagingInfo.options = WMTResourceStorageModeShared;
  stagingInfo.usage = WMTTextureUsageShaderRead;
  auto stagingTexture = device.newTexture(stagingInfo);
  if (!stagingTexture) return;

  auto commandBuffer = queue.newCommandBuffer();
  if (!commandBuffer) return;
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) return;
  WMTOrigin origin{0, 0, 0};
  WMTSize size{std::max(1u, desc.width), std::max(1u, desc.height), 1};
  blit.copyFromTextureToTexture(srcTex, 0, 0, origin, size,
                                WMT::Texture{stagingTexture.handle}, 0, 0, origin);
  blit.endEncoding();
  commandBuffer.commit();
  commandBuffer.waitUntilCompleted();
  {
    std::lock_guard lock(queue.mutex_);
    CommandBufferDiagnostics diagnostics;
    diagnostics.hasBlit = true;
    queue.submissionDiagnostics_.inspect(commandBuffer.handle, diagnostics, "gpu-dump");
  }

  const u32 pitch = std::max(1u, desc.width) * 4u;
  std::vector<u8> bytes(static_cast<std::size_t>(pitch) * std::max(1u, desc.height));
  WMTBufferInfo bufInfo{};
  bufInfo.length = bytes.size();
  bufInfo.options = WMTResourceStorageModeShared;
  auto readBuf = device.newBuffer(bufInfo);
  if (readBuf && bufInfo.memory.ptr) {
    auto cmdBuf2 = queue.newCommandBuffer();
    if (cmdBuf2) {
      auto blit2 = cmdBuf2.blitCommandEncoder();
      if (blit2) {
        WMTOrigin origin2{0, 0, 0};
        WMTSize size2{std::max(1u, desc.width), std::max(1u, desc.height), 1};
        blit2.copyFromTextureToBuffer(WMT::Texture{stagingTexture.handle}, 0, 0,
                                      origin2, size2, WMT::Buffer{readBuf.handle},
                                      0, pitch, 0);
        blit2.endEncoding();
      }
      cmdBuf2.commit();
      cmdBuf2.waitUntilCompleted();
    }
    std::memcpy(bytes.data(), bufInfo.memory.ptr, bytes.size());
  }
  const bool wrote = writeTextureBmp(path, desc.format, std::max(1u, desc.width),
                                     std::max(1u, desc.height), pitch, bytes);
  std::ostringstream out;
  out << "[dxmt9-texture] gpu-dump handle=0x" << std::hex << handle.value << std::dec
      << " size=" << desc.width << "x" << desc.height
      << " format=" << static_cast<unsigned>(desc.format)
      << " path=" << path << " wrote=" << (wrote ? 1 : 0);
  emitTextureTraceLine(out.str());
  pool.dumpedGpuTextures.insert(handle.value);
}

}  // namespace

void* mapBuffer(CommandQueue& queue,
                 resources::Pool& pool,
                 BufferHandle handle,
                 u32 flags) {
  std::unique_lock lock(queue.mutex_);
  auto it = pool.buffers.find(handle.value);
  if (it == pool.buffers.end()) {
    return nullptr;
  }
  if ((flags & core::UsageDiscard) == 0 && (flags & core::UsageNoOverwrite) == 0 &&
      it->second.lastUsedSeqId > queue.completedSeqId_) {
    queue.queueLifecycle_.waitForSequence(lock, it->second.lastUsedSeqId);
  }
  auto& record = it->second;
  if ((flags & core::UsageDiscard) != 0) {
    std::fill(record.shadow.begin(), record.shadow.end(), 0);
    if (record.contents) {
      std::memset(record.contents, 0, record.shadow.size());
    }
  }
  if (record.contents) {
    return record.contents;
  }
  return record.shadow.empty() ? nullptr : record.shadow.data();
}

void uploadTextureLevel(CommandQueue& queue,
                         resources::Pool& pool,
                         WMT::Reference<WMT::Device> device,
                         TextureHandle handle,
                         u32 level,
                         u32 width,
                         u32 height,
                         u32 pitch,
                         std::span<const u8> bytes) {
  std::lock_guard lock(queue.mutex_);
  if (debug::shouldTraceTexture(handle)) {
    emitUploadTrace(pool, handle, level, width, height, pitch, bytes);
  }
  pool.uploadTextureLevel(device, queue.raw(), handle, level,
                          width, height, pitch, bytes.data(), bytes.size());
  if (level == 0 && shouldDumpGpuTexture(handle)) {
    if (auto* rec = pool.findTexture(handle.value); rec && rec->texture) {
      dumpTextureSnapshotUnlocked(queue, pool, device, handle, rec->desc, rec->texture.handle);
    }
  }
}

bool readbackSurface(CommandQueue& queue,
                      resources::Pool& pool,
                      WMT::Reference<WMT::Device> device,
                      const core::BackendLimits& limits,
                      const ReadbackDesc& desc,
                      ReadbackPixels& pixels) {
  WMT::Reference<WMT::Texture> sourceTexture;
  Format format = Format::Unknown;
  Rect sourceRect{};
  u32 sourceLevel = desc.sourceLevel;
  {
    std::lock_guard lock(queue.mutex_);
    auto* surface = pool.findSurface(desc.source.value);
    if (!surface || !surface->texture) {
      return false;
    }
    sourceTexture = surface->resolveTexture ? surface->resolveTexture : surface->texture;
    format = surface->desc.format;
    sourceRect = desc.sourceRect;
    if (sourceRect.right <= sourceRect.left || sourceRect.bottom <= sourceRect.top) {
      sourceRect.right = static_cast<i32>(surface->desc.width);
      sourceRect.bottom = static_cast<i32>(surface->desc.height);
    }
  }

  const u32 width = static_cast<u32>(std::max(1, sourceRect.right - sourceRect.left));
  const u32 height = static_cast<u32>(std::max(1, sourceRect.bottom - sourceRect.top));
  const u32 bpp = core::bytesPerPixel(format);
  if (!sourceTexture || bpp == 0) {
    return false;
  }

  WMTTextureInfo stagingInfo{};
  stagingInfo.type = WMTTextureType2D;
  stagingInfo.pixel_format = dxmt9::convert::toPixelFormat(format, limits);
  stagingInfo.width = width;
  stagingInfo.height = height;
  stagingInfo.depth = 1;
  stagingInfo.mipmap_level_count = 1;
  stagingInfo.sample_count = 1;
  stagingInfo.array_length = 1;
  stagingInfo.options = WMTResourceStorageModeShared;
  stagingInfo.usage = WMTTextureUsageShaderRead;
  auto stagingTexture = device.newTexture(stagingInfo);
  if (!stagingTexture) {
    return false;
  }

  auto commandBuffer = queue.newCommandBuffer();
  if (!commandBuffer) {
    return false;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return false;
  }
  WMTOrigin srcOrigin{(uint64_t)sourceRect.left, (uint64_t)sourceRect.top, 0};
  WMTSize srcSize{width, height, 1};
  WMTOrigin dstOrigin{0, 0, 0};
  blit.copyFromTextureToTexture(WMT::Texture{sourceTexture.handle}, 0, sourceLevel,
                                srcOrigin, srcSize,
                                WMT::Texture{stagingTexture.handle}, 0, 0, dstOrigin);
  blit.endEncoding();
  commandBuffer.commit();
  commandBuffer.waitUntilCompleted();

  pixels.pitch = width * bpp;
  pixels.bytes.resize(static_cast<std::size_t>(pixels.pitch) * height);
  WMTBufferInfo bufInfo{};
  bufInfo.length = static_cast<uint64_t>(pixels.pitch) * height;
  bufInfo.options = WMTResourceStorageModeShared;
  auto readbackBuf = device.newBuffer(bufInfo);
  if (readbackBuf) {
    auto cmdBuf2 = queue.newCommandBuffer();
    if (cmdBuf2) {
      auto blit2 = cmdBuf2.blitCommandEncoder();
      if (blit2) {
        WMTOrigin origin{0, 0, 0};
        WMTSize size{width, height, 1};
        blit2.copyFromTextureToBuffer(WMT::Texture{stagingTexture.handle}, 0, 0,
                                      origin, size, WMT::Buffer{readbackBuf.handle},
                                      0, pixels.pitch, 0);
        blit2.endEncoding();
      }
      cmdBuf2.commit();
      cmdBuf2.waitUntilCompleted();
    }
    if (bufInfo.memory.ptr) {
      std::memcpy(pixels.bytes.data(), bufInfo.memory.ptr, pixels.bytes.size());
    }
  }
  return true;
}

}  // namespace dxmt9::transfers
