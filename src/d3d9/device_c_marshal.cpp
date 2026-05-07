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
