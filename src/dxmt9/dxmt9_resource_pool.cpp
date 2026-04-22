#include "dxmt9_resource_pool.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_format_convert.hpp"

#include <algorithm>
#include <cstring>

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
