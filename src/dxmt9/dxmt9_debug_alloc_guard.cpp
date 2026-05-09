#include "dxmt9_debug_alloc_guard.hpp"

#if defined(DXMT_DEBUG_NO_PER_DRAW_ALLOC) && DXMT_DEBUG_NO_PER_DRAW_ALLOC

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <strings.h>

namespace dxmt9::debug {

namespace {

// Per-thread heap-allocation counter. `thread_local` storage so each thread
// reads/writes its own slot without atomics. Initialized to zero on first use.
thread_local std::uint64_t g_thread_alloc_count = 0;

// Cached arming flag from DXMT_DEBUG_NO_PER_DRAW_ALLOC env var. Computed once
// per process. T18 flipped the semantics: when this TU is compiled in (i.e.
// DXMT_DEBUG_NO_PER_DRAW_ALLOC=1 at build time), the guard is ON by default.
// The env var becomes an opt-out toggle: setting DXMT_DEBUG_NO_PER_DRAW_ALLOC
// to "0" or "false" disables the runtime assert without recompiling. Any
// other value (or env var absent) leaves the assert armed.
bool readEnvArmFlag() noexcept {
  const char* env = std::getenv("DXMT_DEBUG_NO_PER_DRAW_ALLOC");
  if (env == nullptr || env[0] == '\0') {
    return true;
  }
  if (std::strcmp(env, "0") == 0 || strcasecmp(env, "false") == 0) {
    return false;
  }
  return true;
}

}  // namespace

std::uint64_t threadHeapAllocCount() noexcept {
  return g_thread_alloc_count;
}

bool guardArmedFromEnv() noexcept {
  static const bool armed = readEnvArmFlag();
  return armed;
}

}  // namespace dxmt9::debug

// Global operator new/delete overrides. Replaceable per [new.delete] — the
// linker takes our definition over libc++'s default. Each allocation bumps the
// per-thread counter; deletes are not counted (the invariant is "no new heap
// allocations in scope", not "no allocator activity at all").
//
// We forward to malloc/free directly to avoid recursing into libc++ allocator
// hooks. Callers that pass aligned-new sizes get posix_memalign.

void* operator new(std::size_t size) {
  if (size == 0) {
    size = 1;
  }
  ++::dxmt9::debug::g_thread_alloc_count;
  void* p = std::malloc(size);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new[](std::size_t size) {
  if (size == 0) {
    size = 1;
  }
  ++::dxmt9::debug::g_thread_alloc_count;
  void* p = std::malloc(size);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (size == 0) {
    size = 1;
  }
  ++::dxmt9::debug::g_thread_alloc_count;
  return std::malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (size == 0) {
    size = 1;
  }
  ++::dxmt9::debug::g_thread_alloc_count;
  return std::malloc(size);
}

void operator delete(void* ptr) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, std::size_t /*size*/) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr, std::size_t /*size*/) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  std::free(ptr);
}

#endif  // DXMT_DEBUG_NO_PER_DRAW_ALLOC

namespace dxmt9::debug {

// Exported sentinel so this TU is never empty (silences linker warnings about
// archive members with no external symbols when the override is compiled out).
extern const int kDxmt9DebugAllocGuardTuPresent;
const int kDxmt9DebugAllocGuardTuPresent = 0;

}  // namespace dxmt9::debug
