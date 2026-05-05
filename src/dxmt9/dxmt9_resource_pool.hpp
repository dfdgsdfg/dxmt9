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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>
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
  u32 slice = 0;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
};

namespace detail {

enum class ResourceHandleKind : u64 {
  Buffer = 1,
  Texture = 2,
  Surface = 3,
};

template <typename Record, ResourceHandleKind Kind>
class HandleArena {
 public:
  using RecordType = Record;

  core::Handle insert(Record&& record) {
    u32 index = 0;
    if (!freeList_.empty()) {
      index = freeList_.back();
      freeList_.pop_back();
    } else {
      if (slots_.size() >= static_cast<std::size_t>(std::numeric_limits<u32>::max())) {
        return {};
      }
      index = static_cast<u32>(slots_.size());
      slots_.push_back({});
    }

    auto& slot = slots_[index];
    if (slot.generation == 0) {
      slot.generation = 1;
    }
    slot.record.emplace(std::move(record));
    return core::Handle{encode(index, slot.generation)};
  }

  Record* find(u64 handleValue) noexcept {
    const auto decoded = decode(handleValue);
    if (!decoded) {
      return nullptr;
    }
    auto& [index, generation] = *decoded;
    if (index >= slots_.size()) {
      return nullptr;
    }
    auto& slot = slots_[index];
    if (!slot.record || slot.generation != generation) {
      return nullptr;
    }
    return &*slot.record;
  }

  const Record* find(u64 handleValue) const noexcept {
    const auto decoded = decode(handleValue);
    if (!decoded) {
      return nullptr;
    }
    auto& [index, generation] = *decoded;
    if (index >= slots_.size()) {
      return nullptr;
    }
    const auto& slot = slots_[index];
    if (!slot.record || slot.generation != generation) {
      return nullptr;
    }
    return &*slot.record;
  }

  template <typename BeforeErase>
  void reclaimCompleted(u64 completedSeqId, BeforeErase&& beforeErase) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      auto& slot = slots_[i];
      if (!slot.record) {
        continue;
      }
      auto& record = *slot.record;
      if (record.destroyPending && record.lastUsedSeqId <= completedSeqId) {
        beforeErase(record);
        releaseSlot(static_cast<u32>(i));
      }
    }
  }

  void clear() noexcept {
    slots_.clear();
    freeList_.clear();
  }

 private:
  struct DecodedHandle {
    u32 index = 0;
    u32 generation = 0;
  };

  struct Slot {
    std::optional<Record> record;
    u32 generation = 1;
  };

  static constexpr u32 kIndexBits = 32;
  static constexpr u32 kGenerationBits = 24;
  static constexpr u32 kKindBits = 8;
  static constexpr u32 kGenerationShift = kIndexBits;
  static constexpr u32 kKindShift = kIndexBits + kGenerationBits;
  static constexpr u64 kIndexMask = (u64{1} << kIndexBits) - 1u;
  static constexpr u64 kGenerationMask = (u64{1} << kGenerationBits) - 1u;
  static constexpr u64 kKindMask = (u64{1} << kKindBits) - 1u;

  static u64 encode(u32 index, u32 generation) noexcept {
    return (static_cast<u64>(Kind) << kKindShift) |
           ((static_cast<u64>(generation) & kGenerationMask) << kGenerationShift) |
           (static_cast<u64>(index) & kIndexMask);
  }

  static std::optional<DecodedHandle> decode(u64 handleValue) noexcept {
    if (handleValue == 0) {
      return std::nullopt;
    }
    const u64 kind = (handleValue >> kKindShift) & kKindMask;
    if (kind != static_cast<u64>(Kind)) {
      return std::nullopt;
    }
    const u32 generation =
        static_cast<u32>((handleValue >> kGenerationShift) & kGenerationMask);
    if (generation == 0) {
      return std::nullopt;
    }
    return DecodedHandle{
        .index = static_cast<u32>(handleValue & kIndexMask),
        .generation = generation,
    };
  }

  static u32 nextGeneration(u32 generation) noexcept {
    generation = (generation + 1u) & static_cast<u32>(kGenerationMask);
    return generation == 0 ? 1u : generation;
  }

  void releaseSlot(u32 index) noexcept {
    auto& slot = slots_[index];
    slot.record.reset();
    slot.generation = nextGeneration(slot.generation);
    freeList_.push_back(index);
  }

  std::vector<Slot> slots_;
  std::vector<u32> freeList_;
};

}  // namespace detail

// Pool container. The typed arenas are the only production storage path.
struct Pool {
  std::unordered_set<u64> dumpedGpuTextures;

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

  // Mark a record destroy-pending and immediately reclaim if the GPU has
  // already passed its last-used watermark. Returns true if the record
  // existed.
  bool markBufferDestroyAndGc(u64 handleValue, u64 completedSeqId);
  bool markTextureDestroyAndGc(u64 handleValue, u64 completedSeqId);
  bool markSurfaceDestroyAndGc(u64 handleValue, u64 completedSeqId);

  // Drop ALL records (teardown path; bypasses destroyPending / seq checks).
  void purgeAll() noexcept {
    bufferArena_.clear();
    textureArena_.clear();
    surfaceArena_.clear();
  }

  // Record a CPU-visible write to a buffer (updates shadow + mirrors to
  // `contents` if the buffer is shared-mode). Returns true if the handle
  // resolved. Caller holds the pool's mutex.
  bool uploadBufferData(u64 handleValue, const std::uint8_t* bytes, std::size_t byteCount);

  // Returns the seqId the CPU should wait on before this buffer is safe
  // to CPU-map, given `flags`. Returns 0 if the caller may proceed
  // immediately (UsageDiscard/UsageNoOverwrite, missing handle, or the
  // buffer is idle). Pure storage-side query; does not consult queue
  // state — the caller compares against completedSeqId_ to decide.
  u64 mapWaitSeqId(core::BufferHandle handle, u32 flags) const noexcept;

  // Apply the map flags' side effects (UsageDiscard zero-fill) and
  // return the CPU pointer for the buffer. Must be called AFTER any
  // required wait has completed. Returns nullptr for missing handle
  // or empty storage. Caller holds the pool's mutex.
  void* finalizeBufferMap(core::BufferHandle handle, u32 flags);

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

  // Upload CPU-visible bytes into a texture mip level. For shared-mode
  // textures this is a direct replaceRegion. For private-mode textures a
  // staging texture is created, populated, and copied via a synchronous
  // blit submitted on `queue`. Applies format-specific channel padding
  // (X8R8G8B8 alpha fix-up etc.) via a scratch buffer.
  void uploadTextureLevel(WMT::Device device,
                           WMT::CommandQueue queue,
                           core::TextureHandle handle,
                           u32 level,
                           u32 width,
                           u32 height,
                           u32 pitch,
                           const std::uint8_t* bytes,
                           std::size_t byteCount);

  // Deferred-upload variant. Handles shared-mode inline via replaceRegion
  // (no command buffer work). For private-mode textures, allocates a
  // staging texture + populates it from `bytes`, and returns a
  // StagingCopy describing the blit that the caller must encode. The
  // ResourceInitializer batches these across frames and commits them
  // behind a single SharedEvent signal before the next render chunk.
  struct StagingCopy {
    WMT::Reference<WMT::Texture> stagingTexture;
    WMT::Texture destTexture;
    u32 mipLevel = 0;
    u32 slice = 0;
    u32 width = 0;
    u32 height = 0;
  };
  std::optional<StagingCopy>
  stageTextureUpload(WMT::Device device,
                     core::TextureHandle handle,
                     u32 level,
                     u32 width,
                     u32 height,
                     u32 pitch,
                     const std::uint8_t* bytes,
                     std::size_t byteCount);

  // Stamp lastUsedSeqId on a record so the finish-thread GC respects the
  // in-flight watermark. No-ops on zero handle.
  void markBufferUse(core::Handle handle, u64 seqId);
  void markTextureUse(core::Handle handle, u64 seqId);
  void markSurfaceUse(core::Handle handle, u64 seqId);

  // Per-command-kind bulk marks. Walk the descriptor's resources and stamp
  // their last-used watermark.
  void markDrawResources(const core::FlatDrawStateRecord& hot, u64 seqId);
  void markClearResources(const core::ClearDesc& desc, u64 seqId);
  void markSurfaceCopyResources(const core::SurfaceCopyDesc& desc, u64 seqId);
  void markStretchResources(const core::StretchRectDesc& desc, u64 seqId);
  void markReadbackResources(const core::ReadbackDesc& desc, u64 seqId);
  void markColorFillResources(const core::ColorFillDesc& desc, u64 seqId);

 private:
  detail::HandleArena<BufferRecord, detail::ResourceHandleKind::Buffer> bufferArena_;
  detail::HandleArena<TextureRecord, detail::ResourceHandleKind::Texture> textureArena_;
  detail::HandleArena<SurfaceRecord, detail::ResourceHandleKind::Surface> surfaceArena_;
  std::vector<u8> textureUploadScratch_;
};

}  // namespace dxmt9::resources
