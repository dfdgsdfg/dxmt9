#include "dxmt9_heap_manager.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_perf_counters.hpp"

#include <algorithm>
#include <cstdio>

namespace dxmt9::resources {

namespace {

// R-BACK-14.1: a family's storage profile drives both newHeapWithDescriptor
// and the WMTResourceOptions used when suballocating from the heap. The
// classify() entry points map (pool, usage, hasUnifiedMemory) onto exactly
// one of these — never cross-allocated.
WMTResourceOptions familyStorageOptions(HeapFamily family) noexcept {
  switch (family) {
    case HeapFamily::PrivateTexture:
      return WMTResourceStorageModePrivate;
    case HeapFamily::SharedTextureUm:
    case HeapFamily::SharedBuffer:
      return WMTResourceStorageModeShared;
    case HeapFamily::Count:
      break;
  }
  return WMTResourceStorageModeShared;
}

WMTResourceStorageMode storageModeOf(WMTResourceOptions opts) noexcept {
  // WMTResourceOptions storage bits are 0 / 16 / 32 / 48 — see winemetal.h.
  switch (opts) {
    case WMTResourceStorageModePrivate:
      return WMTStorageModePrivate;
    case WMTResourceStorageModeShared:
      return WMTStorageModeShared;
    case WMTResourceStorageModeManaged:
      return WMTStorageModeManaged;
    case WMTResourceStorageModeMemoryless:
      return WMTStorageModeMemoryless;
    default:
      return WMTStorageModeShared;
  }
}

// R-BACK-14.2 — usage flags incompatible with heap allocation: render
// targets, depth/stencil, and dynamic-rename buffers always go direct.
bool usageBlocksHeap(u32 usage) noexcept {
  return (usage & core::UsageRenderTarget) != 0 ||
         (usage & core::UsageDepthStencil) != 0 ||
         (usage & core::UsageDynamic) != 0;
}

}  // namespace

void HeapManager::init(WMT::Device device, bool hasUnifiedMemory) noexcept {
  device_ = device;
  hasUnifiedMemory_ = hasUnifiedMemory;
  initialized_ = true;
  for (std::size_t i = 0; i < families_.size(); ++i) {
    families_[i].nextHeapBytes = kHeapInitialBytes;
    families_[i].storageOptions = familyStorageOptions(static_cast<HeapFamily>(i));
  }
}

HeapEligibility HeapManager::classifyTexture(u64 footprintBytes,
                                              core::Pool pool,
                                              u32 usage) const noexcept {
  if (!initialized_) {
    return {};
  }
  if (footprintBytes == 0 || footprintBytes > kHeapEligibilityFootprintBytes) {
    return {};
  }
  if (usageBlocksHeap(usage)) {
    return {};
  }
  switch (pool) {
    case core::Pool::Default:
      // R-BACK-14.1 PrivateTexture family: D3DPOOL_DEFAULT non-RT non-DS
      // small textures. Storage is Private regardless of unified-memory
      // probe — DEFAULT textures never live on Shared (R-BACK-5.7).
      return {true, HeapFamily::PrivateTexture};
    case core::Pool::Managed:
      // R-BACK-14.1 SharedTextureUm: MANAGED textures only land in a
      // shared heap on a unified-memory device. On discrete the storage
      // mode is Managed and the heap profile (Shared) does not match.
      if (hasUnifiedMemory_) {
        return {true, HeapFamily::SharedTextureUm};
      }
      return {};
    case core::Pool::SystemMem:
    case core::Pool::Scratch:
      // SystemMem / Scratch never allocate WMT textures (Pool::createTexture
      // skips the device.newTexture call), so heap-eligibility doesn't
      // apply.
      return {};
  }
  return {};
}

HeapEligibility HeapManager::classifyBuffer(u64 footprintBytes,
                                             core::Pool pool,
                                             u32 usage) const noexcept {
  // R-BACK-14.* — heap-backed buffer suballocations are temporarily
  // disabled. The bridge does not expose the host-mapped pointer of a
  // heap-suballocated MTLBuffer (heap.makeBuffer returns the buffer
  // reference but not `info.memory.ptr`), so `Pool::createBuffer`
  // pins `record.contents = nullptr` on the heap path. The intended
  // shadow-fallback in `uploadBufferData` was never wired: when
  // `record.contents` is null the writer short-circuits, leaving the
  // CPU-side write to never reach the GPU. The visible failure was
  // every D3DPOOL_DEFAULT vertex/index buffer smaller than the heap
  // footprint threshold ending up zero-filled at draw time —
  // conf-d3d9-triangle's 36-byte VB landed exactly here, the GPU
  // sampled (0,0,0,0) for every vertex, and the triangle collapsed
  // to a degenerate point. Falling back to direct allocation puts
  // the buffer on the same `device.newBuffer` path the encoder has
  // always relied on, which does populate `info.memory.ptr`. Restore
  // the heap-buffer fast path once the bridge exposes a Shared-mode
  // `MTLBuffer.contents()` (or equivalent) so contents stays non-null.
  (void)footprintBytes;
  (void)pool;
  (void)usage;
  return {};
}

HeapManager::Instance* HeapManager::growFamily(Family& family) {
  if (!initialized_) {
    return nullptr;
  }
  WMTHeapDescriptor desc{};
  desc.size = family.nextHeapBytes;
  desc.type = WMTHeapTypeAutomatic;
  // resourceOptions packs storage/cache/hazard bits in WMTResourceOptions
  // numeric form; the unix-side prefers individual fields when any are
  // non-default. We set both for clarity.
  desc.resourceOptions = family.storageOptions;
  desc.storageMode = storageModeOf(family.storageOptions);
  desc.cpuCacheMode = WMTCpuCacheModeDefault;
  desc.hazardTrackingMode = WMTHazardTrackingModeTracked;
  auto heap = device_.newHeapWithDescriptor(desc);
  if (!heap) {
    // R-BACK-14.6 — newHeapWithDescriptor failure → caller falls through
    // to direct allocation. Counter advance lives at the call site (so
    // tests can distinguish per-family failure from total fallback).
    perf::countHeapAllocFailure();
    return nullptr;
  }
  // Heap::setLabel takes const char* directly (unlike Buffer/Texture
  // which take WMT::String). Build the label into a stack buffer.
  char labelBuf[96];
  std::snprintf(labelBuf, sizeof(labelBuf),
                "dxmt9_heap_f%u_h0x%llx_sz%llu",
                static_cast<unsigned>(&family - &families_[0]),
                static_cast<unsigned long long>(heap.handle),
                static_cast<unsigned long long>(desc.size));
  WMT::Heap{heap.handle}.setLabel(labelBuf);
  perf::countHeapInstance();
  Instance instance;
  instance.heap = std::move(heap);
  family.heaps.push_back(std::move(instance));
  // R-BACK-5.10 — geometric doubling capped at kHeapMaxBytesPerFamily.
  if (family.nextHeapBytes < kHeapMaxBytesPerFamily) {
    family.nextHeapBytes = std::min(family.nextHeapBytes * 2u,
                                     kHeapMaxBytesPerFamily);
  }
  return &family.heaps.back();
}

WMT::Reference<WMT::Buffer> HeapManager::allocBuffer(HeapFamily f, u64 length,
                                                      WMTResourceOptions options,
                                                      WMT::Heap& outHeap) {
  if (!initialized_) {
    return {};
  }
  auto& family = familyRef(f);
  // R-BACK-14.5 — fragmentation walk: try every existing heap before
  // growing. heap.makeBuffer returning nil while usedSize <
  // currentAllocatedSize is the canonical fragmentation signal.
  for (auto& instance : family.heaps) {
    if (!instance.heap) {
      continue;
    }
    auto buf = instance.heap.makeBuffer(length, options);
    if (buf) {
      outHeap = WMT::Heap{instance.heap.handle};
      perf::countHeapAlloc(length);
      return buf;
    }
    const auto used = instance.heap.usedSize();
    const auto current = instance.heap.currentAllocatedSize();
    if (used < current) {
      perf::countHeapFragmentationFailure();
    }
  }
  // No existing heap satisfied the request — grow.
  Instance* fresh = growFamily(family);
  if (!fresh) {
    return {};
  }
  auto buf = fresh->heap.makeBuffer(length, options);
  if (!buf) {
    // Brand-new heap rejected the suballocation. Treat as alloc failure
    // (the caller will direct-allocate); leave the empty heap in place
    // — retireFreedHeaps will clean it up once liveMembers stays at 0.
    perf::countHeapAllocFailure();
    return {};
  }
  outHeap = WMT::Heap{fresh->heap.handle};
  perf::countHeapAlloc(length);
  return buf;
}

WMT::Reference<WMT::Texture> HeapManager::allocTexture(HeapFamily f,
                                                        WMTTextureInfo& info,
                                                        WMT::Heap& outHeap) {
  if (!initialized_) {
    return {};
  }
  auto& family = familyRef(f);
  for (auto& instance : family.heaps) {
    if (!instance.heap) {
      continue;
    }
    auto tex = instance.heap.makeTexture(info);
    if (tex) {
      outHeap = WMT::Heap{instance.heap.handle};
      perf::countHeapAlloc(info.width * info.height *
                           std::max<u64>(1u, info.depth));
      return tex;
    }
    const auto used = instance.heap.usedSize();
    const auto current = instance.heap.currentAllocatedSize();
    if (used < current) {
      perf::countHeapFragmentationFailure();
    }
  }
  Instance* fresh = growFamily(family);
  if (!fresh) {
    return {};
  }
  auto tex = fresh->heap.makeTexture(info);
  if (!tex) {
    perf::countHeapAllocFailure();
    return {};
  }
  outHeap = WMT::Heap{fresh->heap.handle};
  perf::countHeapAlloc(info.width * info.height *
                       std::max<u64>(1u, info.depth));
  return tex;
}

HeapManager::Instance* HeapManager::findInstance(obj_handle_t heapHandle) noexcept {
  if (heapHandle == 0) {
    return nullptr;
  }
  for (auto& family : families_) {
    for (auto& instance : family.heaps) {
      if (instance.heap.handle == heapHandle) {
        return &instance;
      }
    }
  }
  return nullptr;
}

void HeapManager::retainHeapMember(obj_handle_t heapHandle, u64 seqId) noexcept {
  auto* instance = findInstance(heapHandle);
  if (!instance) {
    return;
  }
  ++instance->liveMembers;
  instance->lastUsedSeqId = std::max(instance->lastUsedSeqId, seqId);
}

void HeapManager::releaseHeapMember(obj_handle_t heapHandle, u64 seqId) noexcept {
  auto* instance = findInstance(heapHandle);
  if (!instance) {
    return;
  }
  if (instance->liveMembers > 0) {
    --instance->liveMembers;
  }
  instance->lastUsedSeqId = std::max(instance->lastUsedSeqId, seqId);
}

u32 HeapManager::retireFreedHeaps(u64 completedSeqId) {
  u32 retired = 0;
  for (auto& family : families_) {
    auto& heaps = family.heaps;
    auto it = heaps.begin();
    while (it != heaps.end()) {
      // R-BACK-14.4 — retire only when no live members AND the heap's
      // last-used seqId has been GPU-completed. The first condition
      // alone does not satisfy the deferred-destroy gate because a
      // freshly destroyed member might still be in flight on the GPU
      // (member's seqId propagated to instance->lastUsedSeqId via
      // releaseHeapMember).
      if (it->liveMembers == 0 && it->lastUsedSeqId <= completedSeqId) {
        it = heaps.erase(it);
        perf::countHeapCompaction();
        ++retired;
        continue;
      }
      ++it;
    }
  }
  return retired;
}

void HeapManager::purgeAll() noexcept {
  for (auto& family : families_) {
    family.heaps.clear();
    family.nextHeapBytes = kHeapInitialBytes;
  }
}

u32 HeapManager::instanceCount(HeapFamily family) const noexcept {
  return static_cast<u32>(familyRef(family).heaps.size());
}

u32 HeapManager::totalInstanceCount() const noexcept {
  u32 total = 0;
  for (const auto& family : families_) {
    total += static_cast<u32>(family.heaps.size());
  }
  return total;
}

}  // namespace dxmt9::resources
