#pragma once

// MTLHeap small-resource pooling (R-BACK-5.9 / R-BACK-5.10 / R-BACK-14.*).
//
// HeapManager is owned by the resources::Pool. It groups heap instances by
// "family" — each family has a fixed (storage mode, hazard tracking,
// resource class) profile so members are heap-compatible per R-BACK-14.1:
//
//   * PrivateTexture   — D3DPOOL_DEFAULT non-RT non-DS small textures
//                        (storage = Private; texture allocations only)
//   * SharedTextureUm  — MANAGED / SYSTEMMEM small textures on a unified-
//                        memory device (storage = Shared; texture only)
//   * SharedBuffer     — small non-dynamic VB / IB / CB (storage = Shared;
//                        buffer allocations only)
//
// Eligibility: footprint ≤ kHeapEligibilityFootprintBytes (64 KB) AND
// usage compatible (no RT / DS / Dynamic-rename — those always go direct
// per R-BACK-14.2).
//
// Geometric growth (R-BACK-5.10): families start at kHeapInitialBytes
// (4 MB) and double per new heap until kHeapMaxBytesPerFamily (256 MB),
// after which new heaps are added at the cap size. There is no explicit
// hard ceiling on heap count; out-of-memory at the device level falls
// through to direct allocation (R-BACK-14.6) and bumps
// `heap_alloc_failure_count`.
//
// Fragmentation (R-BACK-14.5): if heap.makeBuffer / heap.makeTexture
// returns nil while usedSize < currentAllocatedSize, the manager bumps
// `heap_fragmentation_failure_count`, walks to the next heap in the
// family, and finally grows. If even a fresh heap returns nil (or grow
// itself fails) the manager bumps `heap_alloc_failure_count` and returns
// a null reference so the caller can fall through to direct allocation.
//
// Lifetime (R-BACK-14.4): heap-backed resources observe the same
// destroyPending + lastUsedSeqId gate as direct allocations. The owning
// Pool record carries `isHeapBacked` and a borrowed (non-owning)
// `WMT::Heap` view referring to one of the heap instances kept alive by
// HeapManager. When the Pool record is reclaimed, the
// WMT::Reference<Buffer/Texture> drops its retain and notifies
// HeapManager via `releaseHeapMember(heap)` so the heap's live-member
// count tracks accurately. `retireFreedHeaps(completedSeqId)` walks the
// families and retires heaps whose live count is zero and whose
// `lastUsedSeqId` <= the completed waterline; each retirement advances
// `heap_compaction_count`.
//
// Concurrency: HeapManager has no internal mutex. It is accessed under
// the same lock as resources::Pool (currently CommandQueue::mutex_).

#include "dxmt9/core.hpp"
#include "dxmt9/core_constants.hpp"
#include "../winemetal/Metal.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace dxmt9::resources {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum class HeapFamily : u8 {
  PrivateTexture = 0,
  SharedTextureUm = 1,
  SharedBuffer = 2,
  Count = 3,
};

// Result of HeapManager::classify — pure value, no allocation, callable
// from createBuffer/createTexture before the WMTTextureInfo is even
// built.
struct HeapEligibility {
  bool eligible = false;
  HeapFamily family = HeapFamily::PrivateTexture;
};

// R-BACK-14.2 — eligibility threshold default (configurable in code).
inline constexpr u64 kHeapEligibilityFootprintBytes = 64u * 1024u;

// R-BACK-5.10 — geometric growth bounds. Per-family starting heap size
// and per-family per-heap ceiling. New heaps double until they hit the
// ceiling, then stay at the ceiling.
inline constexpr u64 kHeapInitialBytes = 4ull * 1024ull * 1024ull;     // 4 MB
inline constexpr u64 kHeapMaxBytesPerFamily = 256ull * 1024ull * 1024ull;  // 256 MB

class HeapManager {
 public:
  HeapManager() = default;
  HeapManager(const HeapManager&) = delete;
  HeapManager& operator=(const HeapManager&) = delete;

  // Bind to the queue's WMT device + cached unified-memory probe. Called
  // once from CommandQueue init, before any allocation.
  void init(WMT::Device device, bool hasUnifiedMemory) noexcept;

  // R-BACK-14.2: classify a hypothetical allocation. Returns eligibility
  // + family. Inputs match what the resource pool already has at create
  // time. `usage` carries D3D9 usage bits (UsageRenderTarget /
  // UsageDepthStencil / UsageDynamic).
  HeapEligibility classifyTexture(u64 footprintBytes, core::Pool pool,
                                   u32 usage) const noexcept;
  HeapEligibility classifyBuffer(u64 footprintBytes, core::Pool pool,
                                  u32 usage) const noexcept;

  // R-BACK-14.5 / R-BACK-14.6: try to allocate from `family`. Returns a
  // null Reference if the heap path failed at every step (fragmentation
  // walk + grow); the caller falls through to direct allocation and
  // bumps `heap_direct_fallback_count`. On a successful allocation,
  // `outHeap` is set to a non-owning view of the heap that satisfied the
  // alloc, suitable for storing on the resource record for later
  // useHeap()/retire bookkeeping.
  WMT::Reference<WMT::Buffer> allocBuffer(HeapFamily family, u64 length,
                                           WMTResourceOptions options,
                                           WMT::Heap& outHeap);
  WMT::Reference<WMT::Texture> allocTexture(HeapFamily family,
                                             WMTTextureInfo& info,
                                             WMT::Heap& outHeap);

  // R-BACK-14.4: bump the live-member count for a heap. Pool calls this
  // when it stores a heap-backed record. Symmetric `releaseHeapMember`
  // is called when the record is reclaimed (slot drop in
  // HandleArena::reclaimCompleted's beforeErase callback). Both update
  // the heap's lastUsedSeqId on the way through.
  void retainHeapMember(obj_handle_t heapHandle, u64 seqId) noexcept;
  void releaseHeapMember(obj_handle_t heapHandle, u64 seqId) noexcept;

  // R-BACK-14.4: walk all families, retire any heap whose member count
  // is zero AND lastUsedSeqId <= completedSeqId. Bumps
  // `heap_compaction_count` per retired heap. Returns the number retired
  // (test-visible).
  u32 retireFreedHeaps(u64 completedSeqId);

  // Drop everything — used by Pool::purgeAll() teardown.
  void purgeAll() noexcept;

  // ─── Test-visible introspection. ─────────────────────────────────────
  u32 instanceCount(HeapFamily family) const noexcept;
  u32 totalInstanceCount() const noexcept;

  // R-BACK-14.3 — encoder-side useHeap helper. Iterates every live heap
  // instance and applies `fn(WMT::Heap)` once per heap. Callers (render
  // / blit / compute encoder open paths) pass a lambda that issues the
  // appropriate `encoder.useHeap(heap)` call and bumps
  // `countUseHeap`. The dedup is implicit — each heap instance appears
  // exactly once in the families_ tables. Doing this on encoder open
  // (rather than lazily on first heap-backed bind) keeps the per-draw
  // hot path allocation-free; extra useHeap calls for heaps that this
  // encoder does not bind from are cheap residency hints, not a
  // correctness issue (per Metal docs `useHeap` is idempotent and
  // bounded by heap count).
  template <typename Fn>
  void forEachHeapInstance(Fn&& fn) const {
    for (const auto& family : families_) {
      for (const auto& instance : family.heaps) {
        if (!instance.heap) continue;
        fn(WMT::Heap{instance.heap.handle});
      }
    }
  }

 private:
  // Per-heap bookkeeping. The Reference owns the MTLHeap; liveMembers
  // tracks how many resource records still reference it; lastUsedSeqId
  // is the max of all members' lastUsedSeqId.
  struct Instance {
    WMT::Reference<WMT::Heap> heap;
    u32 liveMembers = 0;
    u64 lastUsedSeqId = 0;
  };

  struct Family {
    std::vector<Instance> heaps;
    // Next heap-create size for this family. Starts at kHeapInitialBytes,
    // doubles until kHeapMaxBytesPerFamily, then stays at the cap.
    u64 nextHeapBytes = kHeapInitialBytes;
    // Static profile.
    WMTResourceOptions storageOptions = WMTResourceStorageModePrivate;
  };

  Family& familyRef(HeapFamily f) noexcept {
    return families_[static_cast<std::size_t>(f)];
  }
  const Family& familyRef(HeapFamily f) const noexcept {
    return families_[static_cast<std::size_t>(f)];
  }

  // Find a heap by handle within all families. Returns nullptr if not
  // found (e.g. heap was retired but a stale resource record references
  // it — guarded against in releaseHeapMember).
  Instance* findInstance(obj_handle_t heapHandle) noexcept;

  // R-BACK-5.10: create a new heap for `family`, append to its vector,
  // advance nextHeapBytes geometrically. Returns nullptr on
  // newHeapWithDescriptor failure (caller bumps heap_alloc_failure
  // counter).
  Instance* growFamily(Family& family);

  std::array<Family, static_cast<std::size_t>(HeapFamily::Count)> families_{};
  WMT::Device device_{};
  bool hasUnifiedMemory_ = false;
  bool initialized_ = false;
};

}  // namespace dxmt9::resources
