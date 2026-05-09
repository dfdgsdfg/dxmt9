#pragma once

// Debug-only invariant: hot encode paths must not heap-allocate per draw or
// per state. See agents/rules/codebase_conventions.rules.md ("Avoid per-draw
// or per-state heap allocation in normal rendering paths.").
//
// Two compile/runtime modes:
//
//   1. Default (DXMT_DEBUG_NO_PER_DRAW_ALLOC undefined OR ==0 in env): the
//      ScopedNoHeapAllocGuard is an empty inline RAII type with no body. The
//      DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE macro expands to nothing; zero release
//      overhead.
//
//   2. Opt-in debug build with -DDXMT_DEBUG_NO_PER_DRAW_ALLOC=1: a thread-local
//      counter tracks calls to global operator new / delete. The guard
//      snapshots the counter at scope entry and DXMT_ASSERTs equality at scope
//      exit, but only when the env flag DXMT_DEBUG_NO_PER_DRAW_ALLOC=1 is also
//      set. The compile flag wires the override; the env flag arms the assert
//      so production debug builds do not abort if the flag is left off.
//
// Thread-safety: the counter is `thread_local`; only the local thread reads
// or mutates it, so no atomics are required.

#include "dxmt9/assert.hpp"

#include <cstdint>

namespace dxmt9::debug {

#if defined(DXMT_DEBUG_NO_PER_DRAW_ALLOC) && DXMT_DEBUG_NO_PER_DRAW_ALLOC

// Returns the per-thread count of global operator new/new[] calls observed
// since the thread started. Defined in dxmt9_debug_alloc_guard.cpp.
std::uint64_t threadHeapAllocCount() noexcept;

// Returns true when the env flag DXMT_DEBUG_NO_PER_DRAW_ALLOC=1 is set; the
// guard's destructor only asserts when this is true.
bool guardArmedFromEnv() noexcept;

class ScopedNoHeapAllocGuard {
public:
  explicit ScopedNoHeapAllocGuard(const char* scope_name) noexcept
      : scope_name_(scope_name), baseline_(threadHeapAllocCount()) {}

  ~ScopedNoHeapAllocGuard() noexcept(false) {
    if (!guardArmedFromEnv()) {
      return;
    }
    const std::uint64_t now = threadHeapAllocCount();
    (void)scope_name_;
    DXMT_ASSERT(now == baseline_);
  }

  ScopedNoHeapAllocGuard(const ScopedNoHeapAllocGuard&) = delete;
  ScopedNoHeapAllocGuard& operator=(const ScopedNoHeapAllocGuard&) = delete;

private:
  const char* scope_name_;
  std::uint64_t baseline_;
};

#define DXMT9_NO_HEAP_ALLOC_GUARD_PASTE_INNER(a, b) a##b
#define DXMT9_NO_HEAP_ALLOC_GUARD_PASTE(a, b) DXMT9_NO_HEAP_ALLOC_GUARD_PASTE_INNER(a, b)
#define DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE(name)                                  \
  ::dxmt9::debug::ScopedNoHeapAllocGuard                                      \
      DXMT9_NO_HEAP_ALLOC_GUARD_PASTE(dxmt9_no_heap_alloc_guard_, __LINE__){name}

#else  // !DXMT_DEBUG_NO_PER_DRAW_ALLOC

class ScopedNoHeapAllocGuard {
public:
  explicit ScopedNoHeapAllocGuard(const char* /*scope_name*/) noexcept {}
};

#define DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE(name) ((void)0)

#endif  // DXMT_DEBUG_NO_PER_DRAW_ALLOC

}  // namespace dxmt9::debug
