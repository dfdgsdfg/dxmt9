#include "dxmt9_resource_pool.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
#include <sstream>
#include <vector>

namespace dxmt9::resources {

BufferRecord* Pool::findBuffer(u64 handle) noexcept {
  return bufferArena_.find(handle);
}

const BufferRecord* Pool::findBuffer(u64 handle) const noexcept {
  return bufferArena_.find(handle);
}

TextureRecord* Pool::findTexture(u64 handle) noexcept {
  return textureArena_.find(handle);
}

const TextureRecord* Pool::findTexture(u64 handle) const noexcept {
  return textureArena_.find(handle);
}

SurfaceRecord* Pool::findSurface(u64 handle) noexcept {
  return surfaceArena_.find(handle);
}

const SurfaceRecord* Pool::findSurface(u64 handle) const noexcept {
  return surfaceArena_.find(handle);
}

namespace {
struct TextureSubresource {
  u32 mipLevel = 0;
  u32 slice = 0;
  bool valid = true;
};

TextureSubresource decodeTextureSubresource(const core::TextureDesc& desc, u32 subresource) {
  const u32 mipLevels = std::max(1u, desc.levels);
  if (desc.type == core::TextureType::Cube) {
    const u32 slice = subresource / mipLevels;
    if (slice >= 6u) {
      return {.mipLevel = 0, .slice = 0, .valid = false};
    }
    return {.mipLevel = subresource % mipLevels, .slice = slice, .valid = true};
  }
  if (subresource >= mipLevels) {
    return {.mipLevel = 0, .slice = 0, .valid = false};
  }
  return {.mipLevel = subresource, .slice = 0, .valid = true};
}

bool shouldTraceResourceHandle(core::Handle handle) {
  return core::metalqueue::queueTraceEnabled() || dxmt9::debug::shouldTraceTexture(handle);
}

void traceTextureCreate(core::TextureHandle handle, const core::TextureDesc& desc,
                        bool hasMetalTexture, bool isPrivate) {
  if (!shouldTraceResourceHandle(handle)) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-resource] texture create handle=0x" << std::hex << handle.value
      << std::dec << " size=" << desc.width << "x" << desc.height << "x" << desc.depth
      << " levels=" << desc.levels
      << " usage=0x" << std::hex << static_cast<u32>(desc.usage) << std::dec
      << " fmt=" << static_cast<u32>(desc.format)
      << " pool=" << static_cast<u32>(desc.pool)
      << " type=" << static_cast<u32>(desc.type)
      << " metal=" << (hasMetalTexture ? 1 : 0)
      << " private=" << (isPrivate ? 1 : 0);
  core::metalqueue::emitTextureTraceLine(out.str());
}

void traceSurfaceCreate(core::SurfaceHandle handle, const core::SurfaceDesc& desc,
                        bool hasMetalTexture) {
  if (!core::metalqueue::queueTraceEnabled()) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-resource] surface create handle=0x" << std::hex << handle.value
      << std::dec << " size=" << desc.width << "x" << desc.height
      << " usage=0x" << std::hex << static_cast<u32>(desc.usage) << std::dec
      << " fmt=" << static_cast<u32>(desc.format)
      << " pool=" << static_cast<u32>(desc.pool)
      << " msaa=" << static_cast<u32>(desc.multiSampleType)
      << " metal=" << (hasMetalTexture ? 1 : 0);
  core::metalqueue::emitTextureTraceLine(out.str());
}

void traceSurfaceAlias(core::SurfaceHandle handle,
                       core::TextureHandle texture,
                       u32 subresource,
                       TextureSubresource decoded,
                       const core::SurfaceDesc& desc,
                       bool usedParentTexture,
                       bool hasMetalTexture) {
  if (!shouldTraceResourceHandle(texture)) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-resource] surface alias handle=0x" << std::hex << handle.value
      << " texture=0x" << texture.value << std::dec
      << " subresource=" << subresource
      << " mip=" << decoded.mipLevel
      << " slice=" << decoded.slice
      << " size=" << desc.width << "x" << desc.height
      << " usage=0x" << std::hex << static_cast<u32>(desc.usage) << std::dec
      << " fmt=" << static_cast<u32>(desc.format)
      << " parent=" << (usedParentTexture ? 1 : 0)
      << " metal=" << (hasMetalTexture ? 1 : 0);
  core::metalqueue::emitTextureTraceLine(out.str());
}

template <typename Arena>
void gcArena(Arena& arena, u64 completedSeqId) {
  arena.reclaimCompleted(completedSeqId, [completedSeqId](const auto& record) {
    // TLA+: NoUseAfterFree
    DXMT_ASSERT(record.lastUsedSeqId <= completedSeqId);
  });
}
}  // namespace

void Pool::reclaimCompleted(u64 completedSeqId) {
  gcArena(bufferArena_, completedSeqId);
  gcArena(textureArena_, completedSeqId);
  gcArena(surfaceArena_, completedSeqId);
}

bool Pool::markBufferDestroyAndGc(u64 handleValue, u64 completedSeqId) {
  auto* record = bufferArena_.find(handleValue);
  if (!record) {
    return false;
  }
  record->destroyPending = true;
  reclaimCompleted(completedSeqId);
  return true;
}

bool Pool::markTextureDestroyAndGc(u64 handleValue, u64 completedSeqId) {
  auto* record = textureArena_.find(handleValue);
  if (!record) {
    return false;
  }
  record->destroyPending = true;
  reclaimCompleted(completedSeqId);
  return true;
}

bool Pool::markSurfaceDestroyAndGc(u64 handleValue, u64 completedSeqId) {
  auto* record = surfaceArena_.find(handleValue);
  if (!record) {
    return false;
  }
  record->destroyPending = true;
  reclaimCompleted(completedSeqId);
  return true;
}

core::BufferHandle Pool::createBuffer(WMT::Device device, const core::BufferDesc& desc) {
  BufferRecord record;
  record.desc = desc;
  record.shadow.resize(static_cast<std::size_t>(desc.size));
  if (desc.pool != core::Pool::SystemMem && desc.pool != core::Pool::Scratch) {
    WMTBufferInfo info{};
    info.length = desc.size;
    info.options = WMTResourceStorageModeShared;
    record.buffer = device.newBuffer(info);
    if (record.buffer) {
      perf::countMetalBuffer(static_cast<std::size_t>(info.length));
    }
    record.contents = info.memory.ptr;  // shared mode: contents ptr returned in info
  }
  return bufferArena_.insert(std::move(record));
}

core::TextureHandle Pool::createTexture(WMT::Device device,
                                          const core::BackendLimits& limits,
                                          const core::TextureDesc& desc) {
  TextureRecord record;
  record.desc = desc;
  if (desc.pool != core::Pool::SystemMem && desc.pool != core::Pool::Scratch) {
    WMTTextureInfo info{};
    info.type = convert::toTextureType(desc.type, false);
    info.pixel_format = convert::toPixelFormat(desc.format, limits);
    info.width = std::max(1u, desc.width);
    info.height = std::max(1u, desc.height);
    info.depth = std::max(1u, desc.depth);
    info.mipmap_level_count = std::max(1u, desc.levels);
    info.sample_count = 1;
    info.array_length = 1;
    info.options = convert::toResourceOptions(desc.pool, desc.usage);
    info.usage = convert::toTextureUsage(desc);
    record.texture = device.newTexture(info);
    record.isPrivate = (info.options == WMTResourceStorageModePrivate);
  }
  const auto handle = textureArena_.insert(std::move(record));
  const auto* stored = textureArena_.find(handle.value);
  traceTextureCreate(handle, desc, stored && stored->texture, stored ? stored->isPrivate : false);
  return handle;
}

core::SurfaceHandle Pool::createSurface(WMT::Device device,
                                          const core::BackendLimits& limits,
                                          const core::SurfaceDesc& desc) {
  SurfaceRecord record;
  record.desc = desc;
  if (desc.pool != core::Pool::SystemMem && desc.pool != core::Pool::Scratch) {
    const uint32_t sc = std::max(1u, core::sampleCount(desc.multiSampleType));
    WMTTextureInfo info{};
    info.type = convert::toTextureType(core::TextureType::TwoD,
                                         desc.multiSampleType != core::MultiSampleType::None);
    info.pixel_format = convert::toPixelFormat(desc.format, limits);
    info.width = std::max(1u, desc.width);
    info.height = std::max(1u, desc.height);
    info.depth = 1;
    info.mipmap_level_count = 1;
    info.sample_count = sc;
    info.array_length = 1;
    info.options = convert::toResourceOptions(desc.pool, desc.usage);
    info.usage = convert::toTextureUsage(desc);
    record.texture = device.newTexture(info);
    if (sc > 1) {
      WMTTextureInfo resolveInfo = info;
      resolveInfo.sample_count = 1;
      resolveInfo.type = WMTTextureType2D;
      resolveInfo.usage =
          static_cast<WMTTextureUsage>(WMTTextureUsageShaderRead | WMTTextureUsageRenderTarget);
      record.resolveTexture = device.newTexture(resolveInfo);
    }
  }
  const auto handle = surfaceArena_.insert(std::move(record));
  const auto* stored = surfaceArena_.find(handle.value);
  traceSurfaceCreate(handle, desc, stored && stored->texture);
  return handle;
}

core::SurfaceHandle Pool::createSurfaceForTexture(core::TextureHandle textureHandle,
                                                    u32 level,
                                                    const core::SurfaceDesc& desc) {
  auto* textureRecord = findTexture(textureHandle.value);
  if (!textureRecord || !textureRecord->texture) {
    return {};
  }
  SurfaceRecord record;
  record.desc = desc;
  record.aliasTexture = textureHandle;
  const auto subresource = decodeTextureSubresource(textureRecord->desc, level);
  if (!subresource.valid) {
    return {};
  }
  record.level = 0;
  record.slice = 0;
  bool usedParentTexture = false;
  WMT::Texture parentTexture{textureRecord->texture.handle};
  if (textureRecord->desc.type != core::TextureType::Cube &&
      subresource.mipLevel == 0 && desc.width == textureRecord->desc.width &&
      desc.height == textureRecord->desc.height) {
    record.texture = WMT::Reference<WMT::Texture>(parentTexture);
    usedParentTexture = true;
  } else {
    WMTTextureSwizzleChannels swizzle{
        WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
        WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha};
    uint64_t gpuId = 0;
    const auto viewType = textureRecord->desc.type == core::TextureType::Cube
                              ? WMTTextureType2D
                              : parentTexture.textureType();
    auto view = parentTexture.newTextureView(parentTexture.pixelFormat(),
                                               viewType,
                                               subresource.mipLevel, 1,
                                               subresource.slice, 1, swizzle, gpuId);
    if (!view && textureRecord->desc.type == core::TextureType::Cube) {
      return {};
    }
    record.texture = view ? std::move(view) : WMT::Reference<WMT::Texture>(parentTexture);
    usedParentTexture = !view;
  }
  const auto handle = surfaceArena_.insert(std::move(record));
  const auto* stored = surfaceArena_.find(handle.value);
  traceSurfaceAlias(handle, textureHandle, level, subresource, desc, usedParentTexture,
                    stored && stored->texture);
  return handle;
}

namespace {

std::span<const std::uint8_t> normalizeUploadBytes(core::Format format, u32 width, u32 height,
                                                     u32 pitch,
                                                     std::span<const std::uint8_t> bytes,
                                                     std::vector<std::uint8_t>& scratch) {
  if (bytes.empty()) {
    return bytes;
  }
  switch (format) {
    case core::Format::X8R8G8B8:
    case core::Format::X8B8G8R8: {
      const std::size_t rowBytes = static_cast<std::size_t>(pitch);
      const std::size_t expected = rowBytes * static_cast<std::size_t>(height);
      if (expected == 0 || bytes.size() < expected) {
        return bytes;
      }
      scratch.assign(bytes.begin(), bytes.begin() + expected);
      for (u32 y = 0; y < height; ++y) {
        std::uint8_t* row = scratch.data() + static_cast<std::size_t>(y) * rowBytes;
        for (u32 x = 0; x < width; ++x) {
          row[static_cast<std::size_t>(x) * 4 + 3] = 0xffu;
        }
      }
      return scratch;
    }
    case core::Format::X1R5G5B5: {
      const std::size_t rowBytes = static_cast<std::size_t>(pitch);
      const std::size_t expected = rowBytes * static_cast<std::size_t>(height);
      if (expected == 0 || bytes.size() < expected) {
        return bytes;
      }
      scratch.assign(bytes.begin(), bytes.begin() + expected);
      for (u32 y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint16_t*>(scratch.data() +
                                                       static_cast<std::size_t>(y) * rowBytes);
        for (u32 x = 0; x < width; ++x) {
          row[x] |= 0x8000u;
        }
      }
      return scratch;
    }
    default:
      return bytes;
  }
}

}  // namespace

std::optional<Pool::StagingCopy>
Pool::stageTextureUpload(WMT::Device device,
                          core::TextureHandle handle,
                          u32 level,
                          u32 width,
                          u32 height,
                          u32 pitch,
                          const std::uint8_t* bytes,
                          std::size_t byteCount) {
  auto* record = findTexture(handle.value);
  if (!record || !record->texture || byteCount == 0) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> scratch;
  const auto normalized = normalizeUploadBytes(record->desc.format, width, height, pitch,
                                                  {bytes, byteCount}, scratch);

  const auto subresource = decodeTextureSubresource(record->desc, level);
  if (!subresource.valid) {
    return std::nullopt;
  }
  WMT::Texture texture{record->texture.handle};
  const u32 mipLevel = subresource.mipLevel;
  const u32 slice = subresource.slice;
  const u32 mipWidth = std::max(1u, width);
  const u32 mipHeight = std::max(1u, height);

  if (!record->isPrivate) {
    // Shared-mode: CPU write straight to the destination; no blit.
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, 1};
    texture.replaceRegion(origin, size, mipLevel, slice, normalized.data(), pitch, 0);
    return std::nullopt;
  }

  // Private-mode: populate a shared staging texture; caller encodes the
  // staging→private blit (batched with other deferred uploads).
  WMTTextureInfo stagingInfo{};
  stagingInfo.type = WMTTextureType2D;
  stagingInfo.pixel_format = texture.pixelFormat();
  stagingInfo.width = mipWidth;
  stagingInfo.height = mipHeight;
  stagingInfo.depth = 1;
  stagingInfo.mipmap_level_count = 1;
  stagingInfo.sample_count = 1;
  stagingInfo.array_length = 1;
  stagingInfo.options = WMTResourceStorageModeShared;
  stagingInfo.usage = WMTTextureUsageShaderRead;
  auto stagingTexture = device.newTexture(stagingInfo);
  if (!stagingTexture) {
    return std::nullopt;
  }
  {
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, 1};
    WMT::Texture{stagingTexture.handle}.replaceRegion(origin, size, 0, 0,
                                                       normalized.data(), pitch, 0);
  }
  return StagingCopy{std::move(stagingTexture), texture, mipLevel, slice, mipWidth, mipHeight};
}

void Pool::uploadTextureLevel(WMT::Device device,
                               WMT::CommandQueue queue,
                               core::TextureHandle handle,
                               u32 level,
                               u32 width,
                               u32 height,
                               u32 pitch,
                               const std::uint8_t* bytes,
                               std::size_t byteCount) {
  auto* record = findTexture(handle.value);
  if (!record || !record->texture || byteCount == 0) {
    return;
  }

  std::vector<std::uint8_t> scratch;
  const auto normalized = normalizeUploadBytes(record->desc.format, width, height, pitch,
                                                  {bytes, byteCount}, scratch);

  const auto subresource = decodeTextureSubresource(record->desc, level);
  if (!subresource.valid) {
    return;
  }
  WMT::Texture texture{record->texture.handle};
  const u32 mipLevel = subresource.mipLevel;
  const u32 slice = subresource.slice;
  const u32 mipWidth = std::max(1u, width);
  const u32 mipHeight = std::max(1u, height);

  if (!record->isPrivate) {
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, 1};
    texture.replaceRegion(origin, size, mipLevel, slice, normalized.data(), pitch, 0);
    return;
  }

  // Private-mode: allocate a shared staging texture, upload into it, then
  // blit into the private destination on `queue`.
  WMTTextureInfo stagingInfo{};
  stagingInfo.type = WMTTextureType2D;
  stagingInfo.pixel_format = texture.pixelFormat();
  stagingInfo.width = mipWidth;
  stagingInfo.height = mipHeight;
  stagingInfo.depth = 1;
  stagingInfo.mipmap_level_count = 1;
  stagingInfo.sample_count = 1;
  stagingInfo.array_length = 1;
  stagingInfo.options = WMTResourceStorageModeShared;
  stagingInfo.usage = WMTTextureUsageShaderRead;
  auto stagingTexture = device.newTexture(stagingInfo);
  if (!stagingTexture) {
    return;
  }
  {
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, 1};
    WMT::Texture{stagingTexture.handle}.replaceRegion(origin, size, 0, 0,
                                                       normalized.data(), pitch, 0);
  }
  auto commandBuffer = queue.commandBuffer();
  if (!commandBuffer) {
    return;
  }
  perf::countCommandBuffer();
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return;
  }
  WMTOrigin origin{0, 0, 0};
  WMTSize size{mipWidth, mipHeight, 1};
  blit.copyFromTextureToTexture(WMT::Texture{stagingTexture.handle}, 0, 0,
                                 origin, size, texture, slice, mipLevel, origin);
  blit.endEncoding();
  commandBuffer.commit();
  const auto started = std::chrono::steady_clock::now();
  commandBuffer.waitUntilCompleted();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  perf::countSyncWait(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
}

void Pool::markBufferUse(core::Handle handle, u64 seqId) {
  if (!handle) return;
  if (auto* rec = findBuffer(handle.value)) {
    rec->lastUsedSeqId = std::max(rec->lastUsedSeqId, seqId);
  }
}

void Pool::markTextureUse(core::Handle handle, u64 seqId) {
  if (!handle) return;
  if (auto* rec = findTexture(handle.value)) {
    rec->lastUsedSeqId = std::max(rec->lastUsedSeqId, seqId);
  }
}

void Pool::markSurfaceUse(core::Handle handle, u64 seqId) {
  if (!handle) return;
  if (auto* rec = findSurface(handle.value)) {
    rec->lastUsedSeqId = std::max(rec->lastUsedSeqId, seqId);
  }
}

void Pool::markDrawResources(const core::DrawDesc& desc, u64 seqId) {
  markBufferUse(desc.indexBuffer, seqId);
  for (const auto& stream : desc.vertexDecl.streams) {
    if (stream.buffer) {
      markBufferUse(stream.buffer->handle(), seqId);
    }
  }
  for (const auto& texture : desc.textures) {
    markTextureUse(texture.handle, seqId);
  }
  for (const auto& rt : desc.rts.color) {
    markSurfaceUse(rt.handle, seqId);
  }
  markSurfaceUse(desc.rts.depthStencil.handle, seqId);
}

void Pool::markDrawResources(const core::FlatDrawStateRecord& hot, u64 seqId) {
  markBufferUse(hot.indexBuffer, seqId);
  for (auto handle : hot.streamBuffers) {
    markBufferUse(handle, seqId);
  }
  for (auto handle : hot.textures) {
    markTextureUse(handle, seqId);
  }
  for (const auto& rt : hot.colorAttachments) {
    markSurfaceUse(rt.handle, seqId);
  }
  markSurfaceUse(hot.depthStencil.handle, seqId);
}

void Pool::markClearResources(const core::ClearDesc& desc, u64 seqId) {
  if (desc.clearColor) {
    for (const auto& attachment : desc.colorAttachments) {
      markSurfaceUse(attachment.handle, seqId);
    }
  }
  if (desc.clearDepth || desc.clearStencil) {
    markSurfaceUse(desc.depthStencil.handle, seqId);
  }
}

void Pool::markSurfaceCopyResources(const core::SurfaceCopyDesc& desc, u64 seqId) {
  markSurfaceUse(desc.source, seqId);
  markSurfaceUse(desc.destination, seqId);
}

void Pool::markStretchResources(const core::StretchRectDesc& desc, u64 seqId) {
  markSurfaceUse(desc.source, seqId);
  markSurfaceUse(desc.destination, seqId);
}

void Pool::markReadbackResources(const core::ReadbackDesc& desc, u64 seqId) {
  markSurfaceUse(desc.source, seqId);
  markSurfaceUse(desc.destination, seqId);
}

void Pool::markColorFillResources(const core::ColorFillDesc& desc, u64 seqId) {
  markSurfaceUse(desc.destination, seqId);
}

bool Pool::uploadBufferData(u64 handleValue, const std::uint8_t* bytes, std::size_t byteCount) {
  auto* record = findBuffer(handleValue);
  if (!record) {
    return false;
  }
  record->shadow.assign(bytes, bytes + byteCount);
  if (!record->buffer || byteCount == 0 || !record->contents) {
    return true;
  }
  const std::size_t copySize = std::min(byteCount, static_cast<std::size_t>(record->desc.size));
  std::memcpy(record->contents, bytes, copySize);
  return true;
}

u64 Pool::mapWaitSeqId(core::BufferHandle handle, u32 flags) const noexcept {
  if ((flags & core::UsageDiscard) != 0 || (flags & core::UsageNoOverwrite) != 0) {
    return 0;
  }
  auto* record = findBuffer(handle.value);
  if (!record) {
    return 0;
  }
  return record->lastUsedSeqId;
}

void* Pool::finalizeBufferMap(core::BufferHandle handle, u32 flags) {
  auto* record = findBuffer(handle.value);
  if (!record) {
    return nullptr;
  }
  if ((flags & core::UsageDiscard) != 0) {
    std::fill(record->shadow.begin(), record->shadow.end(), 0);
    if (record->contents) {
      std::memset(record->contents, 0, record->shadow.size());
    }
  }
  if (record->contents) {
    return record->contents;
  }
  return record->shadow.empty() ? nullptr : record->shadow.data();
}

}  // namespace dxmt9::resources
