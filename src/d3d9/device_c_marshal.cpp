#include "device_c_common.hpp"

#include "device_c_low4gb_pool.hpp"
#include "dxmt9/dxmt9_perf_counters.hpp"
#include "util/dynamic_symbol.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>

#if defined(__APPLE__)
#include <mach/kern_return.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#endif

namespace dxmt9::d3d9::devicec {

namespace {

thread_local uint32_t g_wow64ClientCallDepth = 0;

// Scan-start hint for allocateLow4GB's NtAllocateVirtualMemory low-address
// scan (see the loop in allocateLow4GB below). Persisted across calls so a
// pool-overflow or oversized allocation does not re-pay the syscall-per-
// failed-attempt scan from the fixed 0x10000000 base every time; the scan
// picks up where the last successful allocation left off and wraps back to
// the base if it runs off the top without success. Concurrency: multiple
// threads can call allocateLow4GB (game thread locks, offload-worker-driven
// releases that grow a shadow), so this is a plain atomic rather than the
// pre-existing non-atomic `nextHint` local static used by the mach_vm
// fallback loop further down — a torn read there only costs a slightly
// worse starting guess, never correctness, but a fresh atomic is just as
// cheap and avoids adding a second race.
std::atomic<uintptr_t> g_ntAllocScanHint{0x10000000u};

// Bounded pool of recycled low-4GB shadow-lock blocks (see
// device_c_low4gb_pool.hpp for why this exists and its bucket/eviction
// policy).
//
// R-BACK-43.4 `arena-protected` — serialized by `low4GBPoolMutex()` below,
// this component's OWN lock, never by `CommandQueue::mutex_`. Both entry
// points (`acquireLow4GB` / `releaseLow4GB`) take it, and no caller may assume
// the queue mutex covers this pool. No thread-affinity assert: two actors
// (game thread, replay offload worker) reach it by design, which is exactly
// why it has a lock instead of an ownership claim.
//
// Guarded by its own dedicated mutex — deliberately NOT
// CommandQueue::mutex_ — because shadow alloc happens on the game thread
// (D3D9 Lock calls) while shadow release can also be driven by the
// commit-replay offload worker when it drops the last reference to a
// D9CSurface/D9CTexture/D9CBuffer wrapper (releaseRetainedWrappers).
// Contention is negligible: shadow lock/unlock is well under one call per
// present.
std::mutex& low4GBPoolMutex() {
  static std::mutex mutex;
  return mutex;
}

Low4GBBlockPool<Low4GBAllocation>& low4GBPool() {
  static Low4GBBlockPool<Low4GBAllocation> pool;
  return pool;
}

struct Wow64NativePointerRange {
  uintptr_t begin = 0;
  uintptr_t end = 0;
};

thread_local std::vector<Wow64NativePointerRange> g_wow64NativePointerAllowances;

#if defined(__APPLE__)
using NtAllocateVirtualMemoryFn =
    int32_t (*)(void* process, void** baseAddress, uintptr_t zeroBits,
                size_t* regionSize, uint32_t allocationType, uint32_t protect);
using NtFreeVirtualMemoryFn =
    int32_t (*)(void* process, void** baseAddress, size_t* regionSize, uint32_t freeType);
using GetProcessHeapFn = void* (*)();
using RtlAllocateHeapFn = void* (*)(void* heap, uint32_t flags, size_t size);
using RtlFreeHeapFn = uint8_t (*)(void* heap, uint32_t flags, void* ptr);

NtAllocateVirtualMemoryFn resolveNtAllocateVirtualMemory() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<NtAllocateVirtualMemoryFn>("NtAllocateVirtualMemory",
                                                                   "_NtAllocateVirtualMemory");
  return fn;
}

NtFreeVirtualMemoryFn resolveNtFreeVirtualMemory() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<NtFreeVirtualMemoryFn>("NtFreeVirtualMemory",
                                                               "_NtFreeVirtualMemory");
  return fn;
}

GetProcessHeapFn resolveGetProcessHeap() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<GetProcessHeapFn>("GetProcessHeap", "_GetProcessHeap");
  return fn;
}

RtlAllocateHeapFn resolveRtlAllocateHeap() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<RtlAllocateHeapFn>("RtlAllocateHeap", "_RtlAllocateHeap");
  return fn;
}

RtlFreeHeapFn resolveRtlFreeHeap() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<RtlFreeHeapFn>("RtlFreeHeap", "_RtlFreeHeap");
  return fn;
}
#endif

}  // namespace

// Definition lives in the marshal TU because the only direct caller in this
// directory's free functions is allocateLow4GB; other TUs reach the symbol
// through the device_c_common.hpp declaration.
void dxmt9DebugLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-debug", fmt, args);
  va_end(args);
}

bool pointerFits32Bit(const void* ptr) {
  return reinterpret_cast<uintptr_t>(ptr) <= 0xffffffffu;
}

Low4GBAllocation allocateLow4GB(size_t size) {
#if defined(__APPLE__)
  if (size == 0) {
    return {};
  }
  const mach_vm_size_t rounded =
      static_cast<mach_vm_size_t>((size + vm_page_size - 1) & ~(static_cast<size_t>(vm_page_size) - 1));
  const uintptr_t limit = 0x100000000ull;
  const uintptr_t step =
      std::max<uintptr_t>(0x10000u, (static_cast<uintptr_t>(rounded) + 0xffffu) & ~0xffffull);

  if (auto getProcessHeap = resolveGetProcessHeap()) {
    if (auto rtlAlloc = resolveRtlAllocateHeap()) {
      void* heap = getProcessHeap();
      if (heap) {
        if (void* ptr = rtlAlloc(heap, 0, size)) {
          if (pointerFits32Bit(ptr)) {
            dxmt9DebugLog("allocateLow4GB using process heap ptr=%p size=%zu", ptr, size);
            return {ptr, size, false, true};
          }
          if (auto rtlFree = resolveRtlFreeHeap()) {
            rtlFree(heap, 0, ptr);
          }
        }
      }
    }
  }

  if (auto ntAlloc = resolveNtAllocateVirtualMemory()) {
    const auto ntFree = resolveNtFreeVirtualMemory();
    constexpr uintptr_t kScanBase = 0x10000000u;
    auto tryNtAt = [&](uintptr_t attempt) -> Low4GBAllocation {
      void* base = reinterpret_cast<void*>(attempt);
      size_t region = static_cast<size_t>(rounded);
      const int32_t status =
          ntAlloc(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, 0, &region,
                  0x3000u, 0x04u);
      if (status != 0) {
        return {};
      }
      if (reinterpret_cast<uintptr_t>(base) <= 0xffffffffu) {
        return {base, region, true, false};
      }
      if (ntFree) {
        size_t freeSize = 0;
        ntFree(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, &freeSize, 0x8000u);
      }
      return {};
    };

    // Start from the last successful base instead of always re-scanning
    // from kScanBase — a workload that churns many small allocations
    // otherwise re-walks the same already-claimed low region on every
    // pool-overflow/oversized allocation, paying a syscall per failed
    // attempt for ground already covered. Ignore a stale/out-of-range hint.
    uintptr_t hint = g_ntAllocScanHint.load(std::memory_order_relaxed);
    if (hint < kScanBase || hint + rounded >= limit) {
      hint = kScanBase;
    }
    for (uintptr_t attempt = hint; attempt + rounded < limit; attempt += step) {
      if (auto alloc = tryNtAt(attempt)) {
        g_ntAllocScanHint.store(attempt + step, std::memory_order_relaxed);
        return alloc;
      }
    }
    // Wrap: the hinted tail didn't have room; retry the skipped head
    // (kScanBase..hint) before falling through to the mach_vm path.
    if (hint != kScanBase) {
      for (uintptr_t attempt = kScanBase; attempt < hint && attempt + rounded < limit;
           attempt += step) {
        if (auto alloc = tryNtAt(attempt)) {
          g_ntAllocScanHint.store(attempt + step, std::memory_order_relaxed);
          return alloc;
        }
      }
    }
  }

  auto tryAllocate = [&](mach_vm_address_t address, int flags) -> Low4GBAllocation {
    mach_vm_address_t candidate = address;
    const kern_return_t kr = mach_vm_allocate(mach_task_self(), &candidate, rounded, flags);
    if (kr != KERN_SUCCESS) {
      return {};
    }
    if (candidate > 0xffffffffu) {
      mach_vm_deallocate(mach_task_self(), candidate, rounded);
      return {};
    }
    return {reinterpret_cast<void*>(static_cast<uintptr_t>(candidate)),
            static_cast<size_t>(rounded), false, false};
  };

  if (auto alloc = tryAllocate(0, VM_FLAGS_ANYWHERE | VM_FLAGS_4GB_CHUNK)) {
    return alloc;
  }

  static mach_vm_address_t nextHint = 0x10000000u;
  for (mach_vm_address_t attempt = nextHint; attempt + rounded < limit; attempt += step) {
    if (auto alloc = tryAllocate(attempt, VM_FLAGS_FIXED)) {
      nextHint = attempt + step;
      return alloc;
    }
  }
  dxmt9DebugLog("allocateLow4GB failed size=%zu rounded=%llu", size,
                static_cast<unsigned long long>(rounded));
  return {};
#else
  (void)size;
  return {};
#endif
}

void freeLow4GB(Low4GBAllocation alloc) {
#if defined(__APPLE__)
  if (!alloc.ptr || alloc.size == 0) {
    return;
  }
  if (alloc.viaHeap) {
    if (auto getProcessHeap = resolveGetProcessHeap()) {
      if (auto rtlFree = resolveRtlFreeHeap()) {
        if (void* heap = getProcessHeap()) {
          rtlFree(heap, 0, alloc.ptr);
        }
      }
    }
    return;
  }
  if (alloc.viaNt) {
    if (auto ntFree = resolveNtFreeVirtualMemory()) {
      void* base = alloc.ptr;
      size_t freeSize = 0;
      ntFree(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, &freeSize, 0x8000u);
    }
    return;
  }
  mach_vm_deallocate(mach_task_self(),
                     static_cast<mach_vm_address_t>(reinterpret_cast<uintptr_t>(alloc.ptr)),
                     static_cast<mach_vm_size_t>(alloc.size));
#else
  (void)alloc;
#endif
}

Low4GBAllocation acquireLow4GB(size_t size) {
  using Pool = Low4GBBlockPool<Low4GBAllocation>;
  const size_t bucketCapacity = Pool::bucketCapacityFor(size);
  if (bucketCapacity != 0) {
    {
      std::lock_guard guard(low4GBPoolMutex());
      if (auto block = low4GBPool().tryAcquire(size)) {
        dxmt9::perf::countSurfaceLockShadowPoolHit();
        return *block;
      }
    }
    dxmt9::perf::countSurfaceLockShadowPoolMiss();
    // Miss: allocate at the bucket's exact capacity (not the caller's
    // smaller requested size) so the block it produces is poolable when
    // it is later released — otherwise every miss would allocate an
    // odd-sized block that can never be recycled.
    return allocateLow4GB(bucketCapacity);
  }
  // Oversized (or zero) request: never touches the pool, in either
  // direction. Pooling multi-megabyte blocks indefinitely would hoard a
  // scarce sub-4GB address range for shadow locks that are already rare.
  return allocateLow4GB(size);
}

void releaseLow4GB(Low4GBAllocation alloc) {
  if (!alloc) {
    return;
  }
  using Pool = Low4GBBlockPool<Low4GBAllocation>;
  const size_t bucketCapacity = Pool::bucketCapacityFor(alloc.size);
  if (bucketCapacity != 0 && alloc.size == bucketCapacity) {
    std::lock_guard guard(low4GBPoolMutex());
    if (low4GBPool().tryRelease(bucketCapacity, alloc)) {
      return;
    }
    dxmt9::perf::countSurfaceLockShadowPoolEviction();
  }
  freeLow4GB(alloc);
}

void releaseShadowLock(ShadowLock& lock) {
  releaseLow4GB(lock.shadow);
  lock = ShadowLock{};
}

bool requiresWow64PointerShadow() {
  return g_wow64ClientCallDepth != 0;
}

size_t computeShadowBytesUpperBound(uint32_t nativePitch, uint32_t rectHeight,
                                    uint32_t blockHeight) {
  if (nativePitch == 0 || rectHeight == 0) {
    return 0;
  }
  const uint32_t bh = blockHeight ? blockHeight : 1u;
  // Pad height up to a block boundary so the backing storage's last block
  // row is fully addressable when the game iterates by texel rows.
  const uint32_t alignedHeight =
      ((static_cast<uint64_t>(rectHeight) + bh - 1u) / bh) * bh;
  // For BC formats on tiny mips (e.g. BC3 1x1), guarantee at least one
  // full block tall — the native storage is always at least blockHeight
  // texels, and the game may walk that span via pitch.
  const uint32_t paddedHeight = std::max(alignedHeight, bh);
  const uint64_t paddedBytes =
      static_cast<uint64_t>(nativePitch) * static_cast<uint64_t>(paddedHeight);

  // Compatibility floor for tiny BC mips: SFIV (and likely other
  // D3DX-using games) iterates the lock pointer past the strict
  // block-row bound — observed faults at +0x1000 and +0x2000 from
  // BC3 1x1 / 2x2 locks where the pitch is the parent-level pitch
  // (Metal returns 1024 for the 1x1 mip of a 256x256 BC3 texture
  // because the underlying buffer is row-aligned). On a real
  // Wine-builtin Windows host the lock pointer lives inside a larger
  // heap arena so over-writes land in mapped (but undefined) memory;
  // dxmt9's wow64 shadow uses a page-aligned NtAllocateVirtualMemory /
  // mach_vm_allocate which makes the post-buffer page UNMAPPED and
  // any over-write faults immediately.
  //
  // We bridge that compatibility gap by enforcing a minimum allocation
  // size of `nativePitch * blockHeight * kCompressedMipMinBlockRows`
  // — i.e., at least kCompressedMipMinBlockRows block rows of pitch
  // tall storage. For BC3 1x1 with pitch=1024 this lands at 16 KB
  // (four pages), comfortably swallowing the observed game write
  // patterns. For larger mips the natural `paddedBytes` already
  // exceeds the floor, so this affects only tiny mips that are
  // already small in absolute byte count.
  constexpr uint64_t kCompressedMipMinBlockRows = 4u;
  const uint64_t floorBytes = static_cast<uint64_t>(nativePitch) *
                              static_cast<uint64_t>(bh) *
                              kCompressedMipMinBlockRows;
  return static_cast<size_t>(std::max(paddedBytes, floorBytes));
}

size_t computeShadowVolumeBytesUpperBound(size_t perSliceBytes,
                                          size_t shadowSlicePitch,
                                          uint32_t slices) {
  if (perSliceBytes == 0 || shadowSlicePitch == 0 || slices == 0) {
    return 0;
  }
  if (slices == 1) {
    return perSliceBytes;
  }
  const size_t precedingSlices = static_cast<size_t>(slices - 1u);
  if (shadowSlicePitch >
      (std::numeric_limits<size_t>::max() - perSliceBytes) / precedingSlices) {
    return 0;
  }
  return precedingSlices * shadowSlicePitch + perSliceBytes;
}

bool isWow64NativePointerAllowed(uint64_t value) {
  if (value == 0 || value > static_cast<uint64_t>(UINTPTR_MAX)) {
    return false;
  }
  const auto ptr = static_cast<uintptr_t>(value);
  for (const auto& range : g_wow64NativePointerAllowances) {
    if (ptr >= range.begin && ptr < range.end) {
      return true;
    }
  }
  return false;
}

ScopedWow64ClientCall::ScopedWow64ClientCall() {
  ++g_wow64ClientCallDepth;
}

ScopedWow64ClientCall::~ScopedWow64ClientCall() {
  if (g_wow64ClientCallDepth != 0) {
    --g_wow64ClientCallDepth;
  }
}

ScopedWow64NativePointerAllowance::ScopedWow64NativePointerAllowance(const void* ptr, size_t size)
: ptr_(ptr), size_(size) {
  if (!ptr_ || size_ == 0) {
    return;
  }
  const auto begin = reinterpret_cast<uintptr_t>(ptr_);
  if (begin > UINTPTR_MAX - size_) {
    ptr_ = nullptr;
    size_ = 0;
    return;
  }
  g_wow64NativePointerAllowances.push_back({begin, begin + size_});
}

ScopedWow64NativePointerAllowance::~ScopedWow64NativePointerAllowance() {
  if (!ptr_ || size_ == 0) {
    return;
  }
  const auto begin = reinterpret_cast<uintptr_t>(ptr_);
  const auto end = begin + size_;
  const auto it = std::find_if(
      g_wow64NativePointerAllowances.rbegin(),
      g_wow64NativePointerAllowances.rend(),
      [begin, end](const Wow64NativePointerRange& range) {
        return range.begin == begin && range.end == end;
      });
  if (it != g_wow64NativePointerAllowances.rend()) {
    g_wow64NativePointerAllowances.erase(std::next(it).base());
  }
}

}  // namespace dxmt9::d3d9::devicec
