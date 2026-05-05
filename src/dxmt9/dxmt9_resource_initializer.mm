#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9_resource_initializer.hpp"

#include "dxmt9_command_queue.hpp"
#include "dxmt9_capture.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_resource_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace dxmt9::resources {

namespace {

using core::Format;
using core::Handle;
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

bool shouldDumpGpuTexture(Handle handle) {
  const u64 wanted = gpuDumpTextureHandle();
  return wanted != 0ull && handle.value == wanted;
}

void emitUploadTrace(Pool& pool, TextureHandle handle, u32 level,
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
                                   Pool& pool,
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
  const auto dumpCopyStarted = std::chrono::steady_clock::now();
  commandBuffer.waitUntilCompleted();
  const auto dumpCopyElapsed = std::chrono::steady_clock::now() - dumpCopyStarted;
  perf::countSyncWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(dumpCopyElapsed).count()));
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
  if (readBuf) {
    perf::countMetalBuffer(static_cast<std::size_t>(bufInfo.length));
  }
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
      const auto dumpReadStarted = std::chrono::steady_clock::now();
      cmdBuf2.waitUntilCompleted();
      const auto dumpReadElapsed = std::chrono::steady_clock::now() - dumpReadStarted;
      perf::countSyncWait(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(dumpReadElapsed).count()));
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

Initializer::Initializer(CommandQueue& queue, Pool& pool, WMT::Reference<WMT::Device> device)
    : queue_(&queue), pool_(&pool), device_(std::move(device)) {
  // Allocate the SharedEvent eagerly when we have a real device. On the
  // null-device test path event_ stays empty and flushToWait becomes a
  // no-op.
  if (device_) {
    event_ = device_.newSharedEvent();
  }
}

void Initializer::uploadTextureLevel(core::TextureHandle handle,
                                       std::uint32_t level,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       std::uint32_t pitch,
                                       std::span<const std::uint8_t> bytes) {
  std::lock_guard lock(queue_->mutex_);
  if (debug::shouldTraceTexture(handle)) {
    emitUploadTrace(*pool_, handle, level, width, height, pitch, bytes);
  }
  // stageTextureUpload either does an immediate replaceRegion (shared
  // mode, returns nullopt) or allocates+populates a staging texture
  // (private mode, returns StagingCopy that we queue for flush).
  auto staging = pool_->stageTextureUpload(device_, handle, level,
                                             width, height, pitch,
                                             bytes.data(), bytes.size());
  if (staging) {
    pendingUploads_.push_back(std::move(*staging));
  }

  // gpu-dump sidechannel: only meaningful after the destination is fully
  // populated. Flush any pending deferred work synchronously so the
  // snapshot sees the final GPU-side texture.
  if (level == 0 && shouldDumpGpuTexture(handle)) {
    auto flushResult = flushToWaitUnlocked();
    if (flushResult.event && flushResult.value > 0) {
      // Wait synchronously (CPU-side) so the gpu-dump blit sees final data.
      event_.waitUntilSignaledValue(flushResult.value, /*timeout-ms*/ 1000);
    }
    if (auto* rec = pool_->findTexture(handle.value); rec && rec->texture) {
      dumpTextureSnapshotUnlocked(*queue_, *pool_, device_, handle, rec->desc, rec->texture.handle);
    }
  }
}

Initializer::FlushResult Initializer::flushToWait() {
  std::lock_guard lock(queue_->mutex_);
  return flushToWaitUnlocked();
}

Initializer::FlushResult Initializer::flushToWaitUnlocked() {
  FlushResult result{};
  if (event_) {
    result.event = WMT::Event{event_.handle};
  }
  if (pendingUploads_.empty()) {
    result.value = lastSignaledValue_;
    return result;
  }
  // Encode all queued staging→private blits into one command buffer,
  // signal the event, and commit WITHOUT waiting — the render chunk
  // will wait via encodeWaitForEvent on its own command buffer.
  auto commandBuffer = queue_->newCommandBuffer();
  if (!commandBuffer) {
    pendingUploads_.clear();
    result.value = lastSignaledValue_;
    return result;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    pendingUploads_.clear();
    result.value = lastSignaledValue_;
    return result;
  }
  for (const auto& u : pendingUploads_) {
    WMTOrigin origin{0, 0, 0};
    WMTSize size{u.width, u.height, 1};
    blit.copyFromTextureToTexture(WMT::Texture{u.stagingTexture.handle}, 0, 0,
                                    origin, size, u.destTexture, u.slice, u.mipLevel, origin);
  }
  blit.endEncoding();
  const std::uint64_t value = nextEventValue_++;
  if (event_) {
    commandBuffer.encodeSignalEvent(WMT::Event{event_.handle}, value);
  }
  commandBuffer.commit();
  lastSignaledValue_ = value;
  pendingUploads_.clear();
  result.value = value;
  return result;
}

}  // namespace dxmt9::resources
