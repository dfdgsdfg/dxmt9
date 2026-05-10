#include "device_c_common.hpp"

#include "util/dynamic_symbol.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <iterator>

#if defined(__APPLE__)
#include <mach/kern_return.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#endif

namespace dxmt9::d3d9::devicec {

namespace {

thread_local uint32_t g_wow64ClientCallDepth = 0;

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
    for (uintptr_t attempt = 0x10000000u; attempt + rounded < limit; attempt += step) {
      void* base = reinterpret_cast<void*>(attempt);
      size_t region = static_cast<size_t>(rounded);
      const int32_t status =
          ntAlloc(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, 0, &region,
                  0x3000u, 0x04u);
      if (status != 0) {
        continue;
      }
      if (reinterpret_cast<uintptr_t>(base) <= 0xffffffffu) {
        return {base, region, true, false};
      }
      if (ntFree) {
        size_t freeSize = 0;
        ntFree(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, &freeSize, 0x8000u);
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

void releaseShadowLock(ShadowLock& lock) {
  freeLow4GB(lock.shadow);
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
