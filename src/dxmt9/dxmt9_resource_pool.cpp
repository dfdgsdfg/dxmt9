#include "dxmt9_resource_pool.hpp"

#include "dxmt9/assert.hpp"

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

}  // namespace dxmt9::resources
