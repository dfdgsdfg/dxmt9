#pragma once

// Resource pool — per-backend storage for buffer/texture/surface records
// keyed by opaque Handle. Lifted out of backend_metal.mm so the pool has a
// named home matching dxmt's per-resource-type managers.
//
// Concurrency contract:
//
//  * **Mutating ops** (`createBuffer`/`createTexture`/`createSurface`,
//    `destroy*`, `mark*`, `finalizeBufferMap`, `reclaim*`) must be called
//    with `CommandQueue::mutex_` held. DeviceImpl wraps each PE-side
//    entry point with `std::lock_guard lock(queue_.mutex_)`.
//
//  * **Lookup ops** (`findBuffer`/`findTexture`/`findSurface`) may be
//    called without the queue mutex. HandleArena serializes its own slot
//    metadata with a shared mutex, so encoder-side lookups do not race
//    with PE-thread insert/reclaim even though `encodeChunk` runs with
//    `CommandQueue::mutex_` released.
//
//  * **Pointer stability**: a record pointer returned by `find*()`
//    stays valid for the entire lifetime of a single `encodeChunk`
//    even if a PE-thread `createBuffer`/`createTexture` runs in
//    parallel. Two reasons:
//      - `HandleArena::slots_` is a `std::deque`, whose `push_back`
//        preserves every previously-handed-out element address. Arena
//        mutation is serialized against lookup before the pointer is
//        handed out.
//      - The TLA+ `NoUseAfterFree` invariant guarantees a record is
//        not released into the free list while its `lastUsedSeqId`
//        is ahead of the GPU-completed watermark; chunk N's encoder
//        holds the marking that pins every record it consumes.

#include "dxmt9/core.hpp"
#include "dxmt9_heap_manager.hpp"
#include "../winemetal/Metal.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dxmt9 {

// Same-process D3D9Ex shared-resource backing. These records retain the
// concrete Metal allocations while each importing device receives its own
// pool handle. Cross-process handle transport is intentionally kept out of
// this layer; the PE/provider bridge owns the public opaque-handle policy.
struct SharedBufferBacking {
  WMT::Reference<WMT::Buffer> buffer;
  void* contents = nullptr;
};

struct SharedTextureBacking {
  WMT::Reference<WMT::Texture> texture;
  WMT::Reference<WMT::Texture> shaderReadTexture;
  WMT::Reference<WMT::Texture> srgbShaderReadTexture;
  bool needsStagingBlit = false;
  bool isManagedDiscrete = false;
};

struct SharedSurfaceBacking {
  WMT::Reference<WMT::Texture> texture;
  WMT::Reference<WMT::Texture> srgbTexture;
  WMT::Reference<WMT::Texture> resolveTexture;
};

}  // namespace dxmt9

namespace dxmt9::resources {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// R-BACK-5.8 / 5.11 — per-buffer-handle backing-version entry. DYNAMIC +
// DEFAULT buffers rotate these Shared allocations on D3DLOCK_DISCARD;
// MANAGED buffers rotate them on writable unlock/upload. Draw submissions
// snapshot the concrete Metal buffer backing so already-recorded work does
// not observe a later rotation of the same logical BufferHandle.
struct BufferRenameRingEntry {
  WMT::Reference<WMT::Buffer> buffer;
  void* contents = nullptr;  // shared-mode CPU pointer
  u64 lastUsedSeqId = 0;
};

enum class ReorderedIndexOrder : u32 {
  VertexCacheLru32 = 1,
};

struct ReorderedIndexBufferCacheKey {
  u64 sourceRevision = 0;
  u32 startIndex = 0;
  u64 indexCount = 0;
  core::IndexType indexType = core::IndexType::UInt16;
  ReorderedIndexOrder order = ReorderedIndexOrder::VertexCacheLru32;
  u32 cacheSize = 0;

  friend constexpr bool operator==(const ReorderedIndexBufferCacheKey&,
                                   const ReorderedIndexBufferCacheKey&) = default;
};

struct ReorderedIndexBufferCacheEntry {
  ReorderedIndexBufferCacheKey key{};
  WMT::Reference<WMT::Buffer> buffer;
  u64 byteCount = 0;
  u64 lastUsedSeqId = 0;
  bool rejected = false;
};

struct ReorderedIndexBufferLookup {
  WMT::Buffer buffer{};
  u64 byteCount = 0;
  bool hit = false;
  bool created = false;
  bool rejected = false;
};

struct BufferRecord {
  core::BufferDesc desc{};
  WMT::Reference<WMT::Buffer> buffer;
  void* contents = nullptr;  // CPU-mapped pointer (shared mode only)
  std::vector<u8> shadow;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
  // Content revision invalidates derived immutable buffers, such as
  // cached reordered index buffers, without freeing in-flight entries.
  u64 contentRevision = 1;
  // R-BACK-14.* — heap-backed allocation tracking. When `isHeapBacked`
  // is true, `heap` is a non-owning view of the MTLHeap that owns this
  // buffer; the WMT::Reference above still owns the suballocation. On
  // record reclaim, Pool calls heapManager.releaseHeapMember(heap,
  // lastUsedSeqId) before dropping the reference so retire bookkeeping
  // sees the final use.
  bool isHeapBacked = false;
  WMT::Heap heap{};
  // R-BACK-5.8 / 5.11 — Shared-buffer backing-version state.
  // `isDynamicRename` selects DEFAULT+DYNAMIC DISCARD rotation;
  // `isManagedVersioned` selects MANAGED writable-unlock rotation.
  // `renameRing` always carries at least the create-time allocation for
  // either policy, and `renameActiveIndex` points at the entry mirrored into
  // `buffer` / `contents`. Runtime rotation is safe only when draw bindings
  // snapshot concrete ring entries.
  bool isDynamicRename = false;
  bool isManagedVersioned = false;
  u32 renameActiveIndex = 0;
  std::vector<BufferRenameRingEntry> renameRing;
  std::vector<ReorderedIndexBufferCacheEntry> reorderedIndexCache;

  bool hasVersionedBacking() const noexcept {
    return isDynamicRename || isManagedVersioned;
  }
};

struct TextureRecord {
  core::TextureDesc desc{};
  WMT::Reference<WMT::Texture> texture;
  // Some D3D9 formats store compact Metal R/RG data but sample with expanded
  // channels (luminance replication, float missing channels as one, XRGB alpha
  // as one). `shaderReadTexture` is an optional swizzled view used only for
  // shader binding; uploads, copies, and surface aliases keep using the storage
  // texture above.
  WMT::Reference<WMT::Texture> shaderReadTexture;
  WMT::Reference<WMT::Texture> srgbShaderReadTexture;
  // R-BACK-5.7: storage-mode classification recorded at create time and
  // never updated. `needsStagingBlit` is true when CPU-side
  // `replaceRegion` is not the upload path (Private; or Managed on a
  // discrete-memory device). `isManagedDiscrete` is the subset of
  // `needsStagingBlit` that came from `D3DPOOL_MANAGED` on a non-unified
  // device — used to drive `perf::countManagedTextureUploadBlit` on the
  // discrete path while keeping the counter at 0 on Apple Silicon.
  bool needsStagingBlit = false;
  bool isManagedDiscrete = false;
  bool destroyPending = false;
  u64 lastUsedSeqId = 0;
  // R-BACK-14.* — heap-backed allocation tracking; same shape as
  // BufferRecord. `isHeapBacked` drives the encoder's useHeap dedup
  // path (collapses N useResource calls into a single useHeap per
  // owning MTLHeap).
  bool isHeapBacked = false;
  WMT::Heap heap{};
};

inline WMT::Texture textureForShaderRead(const TextureRecord& record, bool srgb = false) noexcept {
  if (srgb && record.srgbShaderReadTexture) {
    return WMT::Texture{record.srgbShaderReadTexture.handle};
  }
  return record.shaderReadTexture ? WMT::Texture{record.shaderReadTexture.handle}
                                  : WMT::Texture{record.texture.handle};
}

struct SurfaceRecord {
  core::SurfaceDesc desc{};
  WMT::Reference<WMT::Texture> texture;
  WMT::Reference<WMT::Texture> srgbTexture;
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

// Slot-reuse + per-slot 24-bit generation handle arena. The ABA-safety
// invariants of this pattern (a stale handle whose slot has been
// re-occupied resolves to nullptr rather than aliasing onto the new
// record) are formally proven in
// specs/verification/tla/PresentIdAba.tla — StaleResolvesNull,
// NoCrossSlotAlias, GenerationMonotone, EventualReclaim. The spec
// documents the boundedness assumption that the 24-bit generation
// counter never wraps during the lifetime of any outstanding handle.
template <typename Record, ResourceHandleKind Kind>
class HandleArena {
 public:
  using RecordType = Record;

  core::Handle insert(Record&& record) {
    std::unique_lock lock(mutex_);
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
    std::shared_lock lock(mutex_);
    return findUnlocked(handleValue);
  }

  const Record* find(u64 handleValue) const noexcept {
    std::shared_lock lock(mutex_);
    return findUnlocked(handleValue);
  }

  template <typename Fn>
  bool update(u64 handleValue, Fn&& fn) {
    std::unique_lock lock(mutex_);
    auto* record = findUnlocked(handleValue);
    if (!record) {
      return false;
    }
    fn(*record);
    return true;
  }

  template <typename Fn>
  bool inspect(u64 handleValue, Fn&& fn) const {
    std::shared_lock lock(mutex_);
    const auto* record = findUnlocked(handleValue);
    if (!record) {
      return false;
    }
    fn(*record);
    return true;
  }

  template <typename BeforeErase>
  void reclaimCompleted(u64 completedSeqId, BeforeErase&& beforeErase) {
    std::unique_lock lock(mutex_);
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
    std::unique_lock lock(mutex_);
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

  Record* findUnlocked(u64 handleValue) noexcept {
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

  const Record* findUnlocked(u64 handleValue) const noexcept {
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

  // std::deque keeps `Slot` storage pointer-stable across `push_back` so
  // that a record pointer returned by `find()` remains valid while the
  // encoder thread runs without the queue mutex (PE-thread `insert` can
  // grow this container concurrently). The TLA+ `NoUseAfterFree`
  // invariant separately ensures `releaseSlot` cannot fire on a record
  // the encoder is currently consuming (its `lastUsedSeqId` is ahead of
  // the completed watermark).
  //
  // R-VERIF-3.4 (SlotIdentityStable): the slot container must be
  // std::deque because its growth must not invalidate previously
  // handed-out element addresses. Swapping to std::vector silently
  // breaks the encoder-side pointer contract on the first reallocation.
  // The static_assert below pins this at compile time so a casual edit
  // tripping the TLC invariant in `ResourceLifetime.tla` would also
  // refuse to build.
  mutable std::shared_mutex mutex_;
  std::deque<Slot> slots_;
  std::vector<u32> freeList_;

  static_assert(std::is_same_v<decltype(slots_), std::deque<Slot>>,
                "HandleArena::slots_ must be std::deque — R-VERIF-3.4 "
                "SlotIdentityStable depends on pointer-stable growth");
};

}  // namespace detail

// Pool container. The typed arenas are the only production storage path.
struct Pool {
  std::unordered_set<u64> dumpedGpuTextures;

  // R-BACK-5.7: cached `MTLDevice.hasUnifiedMemory` snapshot. Set ONCE by
  // CommandQueue at construction (`setHasUnifiedMemory`). Read by
  // createBuffer/createTexture/createSurface and the texture upload path
  // to pick the storage mode and decide whether the MANAGED-staging blit
  // is needed. Never re-probed per-resource — the spec requires the
  // selection to be made once at device init and at resource create time.
  bool hasUnifiedMemory_ = false;
  void setHasUnifiedMemory(bool value) noexcept { hasUnifiedMemory_ = value; }

  // R-BACK-13.* — Apple3 GPU family capability cache. Set once at device
  // init via `setSupportsApple3` so the tile-shader FFP path can gate on
  // a single bool without re-querying Metal per encoder. The selector at
  // BeginRenderPass reads this via the encoder context.
  bool supportsApple3_ = false;
  void setSupportsApple3(bool value) noexcept { supportsApple3_ = value; }
  bool supportsApple3() const noexcept { return supportsApple3_; }

  // R-BACK-12.22 — Stage 2 argument-buffer hybrid capability cache. The
  // CommandQueue probes `MTLDevice.argumentBuffersSupport` once at
  // construction and ANDs the result with `supportsApple3_`. Per-encoder
  // selection reads this via `argbufHybridEnabled()`; the bool is the
  // single source of truth for "is the device eligible for Stage 2",
  // matching the Apple-Silicon-only contract in spec.md §11.1.
  bool argbufHybridEnabled_ = false;
  void setArgbufHybridEnabled(bool value) noexcept { argbufHybridEnabled_ = value; }
  bool argbufHybridEnabled() const noexcept { return argbufHybridEnabled_; }

  // R-BACK-14.* — small-resource heap pooling. Owned by the pool so the
  // create/destroy paths can route eligible allocations through the heap
  // before falling back to direct allocation. Initialized once from
  // CommandQueue::CommandQueue (init() snapshot of the same WMT::Device
  // and hasUnifiedMemory_ probe used by the storage-mode selectors).
  HeapManager heapManager_{};
  HeapManager& heapManager() noexcept { return heapManager_; }
  const HeapManager& heapManager() const noexcept { return heapManager_; }

  // Lookup helpers — return nullptr on miss. Safe to call from the
  // encoder thread without holding the queue mutex; the returned
  // pointer remains valid for the duration of a single encodeChunk
  // (see HandleArena::slots_ stability note in this header).
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
    // R-BACK-14.* — drop all heap instances after the records that
    // referenced them are gone. Order matters: arena.clear releases the
    // WMT::Reference<Buffer/Texture> suballocations first; only then is
    // it safe to drop the owning MTLHeaps.
    heapManager_.purgeAll();
  }

  // Record a CPU-visible write to a buffer. MANAGED writes rotate to an idle
  // or fresh Shared Metal backing before the full CPU shadow is copied;
  // other pools mirror into the active `contents` directly. Returns true if
  // the handle resolved. Caller holds the pool's mutex.
  bool uploadBufferData(WMT::Device device,
                        u64 handleValue,
                        const std::uint8_t* bytes,
                        std::size_t byteCount,
                        u64 completedSeqId);

  // Look up or create immutable Metal index buffers containing reordered index
  // bytes derived from a stable source buffer. Entries are keyed by source
  // buffer content revision + draw span and are retained on the source
  // BufferRecord. A zero key revision resolves to the record's current
  // revision; a draw-binding snapshot passes its explicit historical revision.
  // Caller holds the queue mutex; stale entries are pruned only when their
  // last use has completed.
  //
  // findReorderedIndexBuffer is hit-only; it never builds candidate bytes or
  // allocates a Metal buffer.
  ReorderedIndexBufferLookup findReorderedIndexBuffer(
      u64 sourceHandle,
      ReorderedIndexBufferCacheKey key,
      u64 seqId,
      u64 completedSeqId);

  bool rememberRejectedReorderedIndexBuffer(
      u64 sourceHandle,
      ReorderedIndexBufferCacheKey key,
      u64 seqId,
      u64 completedSeqId);

  ReorderedIndexBufferLookup getOrCreateReorderedIndexBuffer(
      WMT::Device device,
      u64 sourceHandle,
      ReorderedIndexBufferCacheKey key,
      std::span<const std::uint8_t> bytes,
      u64 seqId,
      u64 completedSeqId);

  // Returns the seqId the CPU should wait on before this buffer is safe
  // to CPU-map, given `flags`. Returns 0 if the caller may proceed
  // immediately (UsageNoOverwrite, MANAGED, missing handle, or the buffer is
  // idle).
  // UsageDiscard skips the wait for DYNAMIC + DEFAULT buffers when dynamic
  // renaming is enabled because draw payloads snapshot the concrete backing.
  // Pure storage-side query; does not consult queue state — the caller
  // compares against completedSeqId_.
  u64 mapWaitSeqId(core::BufferHandle handle, u32 flags) const noexcept;

  // Apply the map flags' side effects (UsageDiscard zero-fill) and
  // return the CPU pointer for the buffer. Must be called AFTER any
  // required wait has completed. Returns nullptr for missing handle
  // or empty storage. Caller holds the pool's mutex.
  //
  // R-BACK-5.8 — when the record is `isDynamicRename` and `flags` carries
  // `UsageDiscard`, this rotates the per-handle rename ring before returning
  // the CPU pointer. MANAGED rotation is deliberately deferred to writable
  // unlock/upload (R-BACK-5.11), so mapping never mutates an in-flight
  // MANAGED backing.
  void* finalizeBufferMap(WMT::Device device,
                          core::BufferHandle handle,
                          u32 flags,
                          u64 completedSeqId);

  // Allocate a new buffer record (shared-mode WMT buffer + shadow).
  // Pool::Scratch / Pool::SystemMem skip the WMT allocation.
  core::BufferHandle createBuffer(WMT::Device device, const core::BufferDesc& desc);
  bool exportSharedBuffer(core::BufferHandle handle, SharedBufferBacking& out) const;
  core::BufferHandle importSharedBuffer(const core::BufferDesc& desc,
                                        const SharedBufferBacking& backing);

  // Allocate a new texture record. WMT texture is created for non-system
  // pools; pixel format + storage mode derived from the device limits.
  core::TextureHandle createTexture(WMT::Device device,
                                    const core::BackendLimits& limits,
                                    const core::TextureDesc& desc);
  bool exportSharedTexture(core::TextureHandle handle, SharedTextureBacking& out) const;
  core::TextureHandle importSharedTexture(const core::TextureDesc& desc,
                                          const SharedTextureBacking& backing);

  // Allocate a new surface record (render target / depth-stencil attachment).
  // For MSAA surfaces (sampleCount > 1), also allocates a matching resolve
  // texture.
  core::SurfaceHandle createSurface(WMT::Device device,
                                    const core::BackendLimits& limits,
                                    const core::SurfaceDesc& desc);
  bool exportSharedSurface(core::SurfaceHandle handle, SharedSurfaceBacking& out) const;
  core::SurfaceHandle importSharedSurface(const core::SurfaceDesc& desc,
                                          const SharedSurfaceBacking& backing);

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
                           u32 depth,
                           u32 pitch,
                           u32 slicePitch,
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
    u32 depth = 1;
    // R-BACK-14.3 — capture the destination TextureRecord's heap-backed
    // flag and heap handle at staging time so the Initializer's batched
    // flush can perform the per-encoder useHeap dedup walk without an
    // extra Pool lookup (the raw destTexture handle would not be
    // reverse-resolvable to a TextureRecord). Default-zero values mean
    // "not heap-backed"; the flush walk skips them.
    bool destIsHeapBacked = false;
    obj_handle_t destHeap = 0;
  };
  std::optional<StagingCopy>
  stageTextureUpload(WMT::Device device,
                      core::TextureHandle handle,
                      u32 level,
                      u32 width,
                      u32 height,
                      u32 depth,
                      u32 pitch,
                      u32 slicePitch,
                      const std::uint8_t* bytes,
                      std::size_t byteCount);

  // Stamp lastUsedSeqId on a record so the finish-thread GC respects the
  // in-flight watermark. No-ops on zero handle.
  void markBufferUse(core::Handle handle, u64 seqId);
  void markBufferSnapshotUse(core::Handle handle,
                             const core::DrawBufferBindingSnapshot& snapshot,
                             u64 seqId);
  void markTextureUse(core::Handle handle, u64 seqId);
  void markSurfaceUse(core::Handle handle, u64 seqId);

  core::DrawBufferBindingSnapshot snapshotBufferBinding(core::Handle handle) const noexcept;

  // Per-command-kind bulk marks. Walk the descriptor's resources and stamp
  // their last-used watermark.
  void markDrawResources(const core::FlatDrawStateRecord& hot, u64 seqId);
  void markClearResources(const core::ClearDesc& desc, u64 seqId);
  void markSurfaceCopyResources(const core::SurfaceCopyDesc& desc, u64 seqId);
  void markStretchResources(const core::StretchRectDesc& desc, u64 seqId);
  void markReadbackResources(const core::ReadbackDesc& desc, u64 seqId);
  void markColorFillResources(const core::ColorFillDesc& desc, u64 seqId);
  void markDepthResolveResources(const core::DepthResolveDesc& desc, u64 seqId);

 private:
  detail::HandleArena<BufferRecord, detail::ResourceHandleKind::Buffer> bufferArena_;
  detail::HandleArena<TextureRecord, detail::ResourceHandleKind::Texture> textureArena_;
  detail::HandleArena<SurfaceRecord, detail::ResourceHandleKind::Surface> surfaceArena_;
  std::vector<u8> textureUploadScratch_;
};

}  // namespace dxmt9::resources
