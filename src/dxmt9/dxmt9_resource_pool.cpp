#include "dxmt9_resource_pool.hpp"

#include "dxmt9/assert.hpp"

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
