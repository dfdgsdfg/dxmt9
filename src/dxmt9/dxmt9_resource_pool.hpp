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
};

}  // namespace dxmt9::resources
