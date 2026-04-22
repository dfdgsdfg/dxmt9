#include "dxmt9_resource_pool.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_format_convert.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace dxmt9::resources {

BufferRecord* Pool::findBuffer(u64 handle) noexcept {
  auto it = buffers.find(handle);
  return it == buffers.end() ? nullptr : &it->second;
}

const BufferRecord* Pool::findBuffer(u64 handle) const noexcept {
  auto it = buffers.find(handle);
  return it == buffers.end() ? nullptr : &it->second;
}

TextureRecord* Pool::findTexture(u64 handle) noexcept {
  auto it = textures.find(handle);
  return it == textures.end() ? nullptr : &it->second;
}

const TextureRecord* Pool::findTexture(u64 handle) const noexcept {
  auto it = textures.find(handle);
  return it == textures.end() ? nullptr : &it->second;
}

SurfaceRecord* Pool::findSurface(u64 handle) noexcept {
  auto it = surfaces.find(handle);
  return it == surfaces.end() ? nullptr : &it->second;
}

const SurfaceRecord* Pool::findSurface(u64 handle) const noexcept {
  auto it = surfaces.find(handle);
  return it == surfaces.end() ? nullptr : &it->second;
}

namespace {
template <typename Map>
void gcMap(Map& map, u64 completedSeqId) {
  for (auto it = map.begin(); it != map.end();) {
    auto& record = it->second;
    if (record.destroyPending && record.lastUsedSeqId <= completedSeqId) {
      // TLA+: NoUseAfterFree
      DXMT_ASSERT(record.lastUsedSeqId <= completedSeqId);
      it = map.erase(it);
    } else {
      ++it;
    }
  }
}
}  // namespace

void Pool::reclaimCompleted(u64 completedSeqId) {
  gcMap(buffers, completedSeqId);
  gcMap(textures, completedSeqId);
  gcMap(surfaces, completedSeqId);
}

core::BufferHandle Pool::createBuffer(WMT::Device device, const core::BufferDesc& desc) {
  const core::Handle handle{nextHandle++};
  BufferRecord record;
  record.desc = desc;
  record.shadow.resize(static_cast<std::size_t>(desc.size));
  if (desc.pool != core::Pool::SystemMem && desc.pool != core::Pool::Scratch) {
    WMTBufferInfo info{};
    info.length = desc.size;
    info.options = WMTResourceStorageModeShared;
    record.buffer = device.newBuffer(info);
    record.contents = info.memory.ptr;  // shared mode: contents ptr returned in info
  }
  buffers[handle.value] = std::move(record);
  return handle;
}

core::TextureHandle Pool::createTexture(WMT::Device device,
                                          const core::BackendLimits& limits,
                                          const core::TextureDesc& desc) {
  const core::Handle handle{nextHandle++};
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
  textures[handle.value] = std::move(record);
  return handle;
}

core::SurfaceHandle Pool::createSurface(WMT::Device device,
                                          const core::BackendLimits& limits,
                                          const core::SurfaceDesc& desc) {
  const core::Handle handle{nextHandle++};
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
  surfaces[handle.value] = std::move(record);
  return handle;
}

core::SurfaceHandle Pool::createSurfaceForTexture(core::TextureHandle textureHandle,
                                                    u32 level,
                                                    const core::SurfaceDesc& desc) {
  auto textureIt = textures.find(textureHandle.value);
  if (textureIt == textures.end() || !textureIt->second.texture) {
    return {};
  }
  const core::Handle handle{nextHandle++};
  SurfaceRecord record;
  record.desc = desc;
  record.aliasTexture = textureHandle;
  record.level = level;
  WMT::Texture parentTexture{textureIt->second.texture.handle};
  if (level == 0 && desc.width == textureIt->second.desc.width &&
      desc.height == textureIt->second.desc.height) {
    record.texture = WMT::Reference<WMT::Texture>(parentTexture);
  } else {
    WMTTextureSwizzleChannels swizzle{
        WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
        WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha};
    uint64_t gpuId = 0;
    auto view = parentTexture.newTextureView(parentTexture.pixelFormat(),
                                               parentTexture.textureType(),
                                               level, 1, 0, 1, swizzle, gpuId);
    record.texture = view ? std::move(view) : WMT::Reference<WMT::Texture>(parentTexture);
  }
  surfaces[handle.value] = std::move(record);
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

void Pool::uploadTextureLevel(WMT::Device device,
                               WMT::CommandQueue queue,
                               core::TextureHandle handle,
                               u32 level,
                               u32 width,
                               u32 height,
                               u32 pitch,
                               const std::uint8_t* bytes,
                               std::size_t byteCount) {
  auto it = textures.find(handle.value);
  if (it == textures.end() || !it->second.texture || byteCount == 0) {
    return;
  }

  std::vector<std::uint8_t> scratch;
  const auto normalized = normalizeUploadBytes(it->second.desc.format, width, height, pitch,
                                                  {bytes, byteCount}, scratch);

  WMT::Texture texture{it->second.texture.handle};
  const u32 mipLevel = level;
  const u32 mipWidth = std::max(1u, width);
  const u32 mipHeight = std::max(1u, height);

  if (!it->second.isPrivate) {
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, 1};
    texture.replaceRegion(origin, size, mipLevel, 0, normalized.data(), pitch, 0);
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
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return;
  }
  WMTOrigin origin{0, 0, 0};
  WMTSize size{mipWidth, mipHeight, 1};
  blit.copyFromTextureToTexture(WMT::Texture{stagingTexture.handle}, 0, 0,
                                 origin, size, texture, 0, mipLevel, origin);
  blit.endEncoding();
  commandBuffer.commit();
  commandBuffer.waitUntilCompleted();
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

void Pool::markClearResources(const core::ClearDesc& desc, u64 seqId) {
  for (const auto& attachment : desc.colorAttachments) {
    markSurfaceUse(attachment.handle, seqId);
  }
  markSurfaceUse(desc.depthStencil.handle, seqId);
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
  auto it = buffers.find(handleValue);
  if (it == buffers.end()) {
    return false;
  }
  it->second.shadow.assign(bytes, bytes + byteCount);
  if (!it->second.buffer || byteCount == 0 || !it->second.contents) {
    return true;
  }
  const std::size_t copySize = std::min(byteCount, static_cast<std::size_t>(it->second.desc.size));
  std::memcpy(it->second.contents, bytes, copySize);
  return true;
}

}  // namespace dxmt9::resources
