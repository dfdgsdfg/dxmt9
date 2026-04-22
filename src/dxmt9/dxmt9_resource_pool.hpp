#pragma once

// Resource pool — per-backend storage for buffer/texture/surface records
// keyed by opaque Handle. Lifted out of backend_metal.mm so the pool has a
// named home matching dxmt's per-resource-type managers.
//
// The pool carries no mutex of its own; it's protected by
// commandQueue_->mutex_ (same mutex guarding queue state). Splitting the
// mutex is a deferred task; leaving it shared preserves all existing
// lock-ordering invariants.

#include "dxmt9/core.hpp"
#include "../winemetal/Metal.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt9::resources {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct BufferRecord {
  core::BufferDesc desc{};
  WMT::Reference<WMT::Buffer> buffer;
  void* contents = nullptr;  // CPU-mapped pointer (shared mode only)
  std::vector<u8> shadow;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

struct TextureRecord {
  core::TextureDesc desc{};
  WMT::Reference<WMT::Texture> texture;
  bool isPrivate = false;  // true if storage mode is private (no CPU access)
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

struct SurfaceRecord {
  core::SurfaceDesc desc{};
  WMT::Reference<WMT::Texture> texture;
  WMT::Reference<WMT::Texture> resolveTexture;
  core::TextureHandle aliasTexture{};
  u32 level = 0;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

// Pool container. Members are public so existing callers can manipulate the
// maps directly; the find* / reclaim helpers below are the preferred access
// pattern for new code.
struct Pool {
  std::unordered_map<u64, BufferRecord> buffers;
  std::unordered_map<u64, TextureRecord> textures;
  std::unordered_map<u64, SurfaceRecord> surfaces;
  std::unordered_set<u64> dumpedGpuTextures;
  u64 nextHandle = 1;

  // Lookup helpers — return nullptr on miss. Caller is expected to hold the
  // protecting mutex (currently commandQueue_->mutex_).
  BufferRecord* findBuffer(u64 handle) noexcept;
  const BufferRecord* findBuffer(u64 handle) const noexcept;
  TextureRecord* findTexture(u64 handle) noexcept;
  const TextureRecord* findTexture(u64 handle) const noexcept;
  SurfaceRecord* findSurface(u64 handle) noexcept;
  const SurfaceRecord* findSurface(u64 handle) const noexcept;

  // Free records whose destroyPending is set and lastUsedSeqId is ≤ the
  // GPU-completed watermark. Called from the finish thread under the queue
  // mutex. Preserves dxmt9's TLA+ NoUseAfterFree invariant.
  void reclaimCompleted(u64 completedSeqId);

  // Drop ALL records (teardown path; bypasses destroyPending / seq checks).
  void purgeAll() noexcept {
    buffers.clear();
    textures.clear();
    surfaces.clear();
  }

  // Mark a record destroy-pending and immediately reclaim if the GPU has
  // already passed its last-used watermark. Returns true if the record
  // existed.
  template <typename Map>
  bool markDestroyAndGc(Map& map, u64 handleValue, u64 completedSeqId) {
    auto it = map.find(handleValue);
    if (it == map.end()) return false;
    it->second.destroyPending = true;
    reclaimCompleted(completedSeqId);
    return true;
  }

  // Record a CPU-visible write to a buffer (updates shadow + mirrors to
  // `contents` if the buffer is shared-mode). Returns true if the handle
  // resolved. Caller holds the pool's mutex.
  bool uploadBufferData(u64 handleValue, const std::uint8_t* bytes, std::size_t byteCount);

  // Allocate a new buffer record (shared-mode WMT buffer + shadow).
  // Pool::Scratch / Pool::SystemMem skip the WMT allocation.
  core::BufferHandle createBuffer(WMT::Device device, const core::BufferDesc& desc);

  // Allocate a new texture record. WMT texture is created for non-system
  // pools; pixel format + storage mode derived from the device limits.
  core::TextureHandle createTexture(WMT::Device device,
                                     const core::BackendLimits& limits,
                                     const core::TextureDesc& desc);

  // Allocate a new surface record (render target / depth-stencil attachment).
  // For MSAA surfaces (sampleCount > 1), also allocates a matching resolve
  // texture.
  core::SurfaceHandle createSurface(WMT::Device device,
                                      const core::BackendLimits& limits,
                                      const core::SurfaceDesc& desc);

  // Create a surface record that aliases an existing texture's mip level.
  // If the surface covers the full level-0 of the parent, it references the
  // texture directly; otherwise a WMT texture view is created.
  core::SurfaceHandle createSurfaceForTexture(core::TextureHandle textureHandle,
                                                u32 level,
                                                const core::SurfaceDesc& desc);

  // Stamp lastUsedSeqId on a record so the finish-thread GC respects the
  // in-flight watermark. No-ops on zero handle.
  void markBufferUse(core::Handle handle, u64 seqId);
  void markTextureUse(core::Handle handle, u64 seqId);
  void markSurfaceUse(core::Handle handle, u64 seqId);

  // Per-command-kind bulk marks. Walk the descriptor's resources and stamp
  // their last-used watermark.
  void markDrawResources(const core::DrawDesc& desc, u64 seqId);
  void markClearResources(const core::ClearDesc& desc, u64 seqId);
  void markSurfaceCopyResources(const core::SurfaceCopyDesc& desc, u64 seqId);
  void markStretchResources(const core::StretchRectDesc& desc, u64 seqId);
  void markReadbackResources(const core::ReadbackDesc& desc, u64 seqId);
  void markColorFillResources(const core::ColorFillDesc& desc, u64 seqId);
};

}  // namespace dxmt9::resources
