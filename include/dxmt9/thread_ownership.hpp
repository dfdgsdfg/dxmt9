#pragma once

// Shared debug-only thread-affinity assertion helper — R-BACK-43.5.
//
// `specs/backend/producer-concurrency/requirements.md` R-BACK-43.4 classifies
// every piece of state reachable from more than one of {producer, replay
// worker, encode thread, completion path}. The two classes that carry NO
// synchronization — `producer-owned` and `worker-owned` — are only as good as
// the confinement claim behind them, so R-BACK-43.5 requires each of them to
// be guarded by a debug-only thread-affinity assertion that compiles out of
// release builds, from ONE shared helper rather than a per-site
// re-implementation.
//
// `D3D9DeviceImpl`'s `creatingThreadId_` + `assertRecorderThreadConfined()`
// (commit `b96fdbda`) is the reference shape this generalizes; it is now a
// caller of this header rather than a private copy of it.
//
// This header is deliberately dependency-free (no `<thread>`, no
// `<windows.h>`, no pthreads) so the SAME header serves both the PE/frontend
// side (`src/d3d9`, cross-compiled with llvm-mingw) and the unix side
// (`src/dxmt9`). Thread identity is the address of a function-local
// `thread_local` object: unique per thread for the process lifetime, shared
// across translation units because the enclosing function is `inline`, and
// available on every toolchain that already compiles this codebase.
//
// Three shapes are supported, matching how the audited surfaces in
// `specs/backend/producer-concurrency/spec.md` §2 are actually owned:
//
//   (a) FIXED AT CONSTRUCTION — `producer-owned` fields whose owner is the
//       thread that built the containing object (the game thread, because
//       device/queue/pool construction runs inside the app's CreateDevice
//       call). Default-construct the token next to the field it guards.
//
//   (b) REBINDABLE — `worker-owned`-between-events state, where ownership is
//       acquired at one point and released at another (the writing slot
//       between `ensureWritingSlot` and publish). Construct with
//       `deferThreadOwnership` and call `bindToCurrentThread()` at the
//       acquisition point.
//
//   (c) OWNER-OR-LOCKED — a documented exception where a second actor may
//       legitimately touch owned state while holding a named lock (the
//       producer's map-wait force-publish reaching the writing slot under
//       `CommandQueue::mutex_`; the D3D9 recorder under `recorderMutex_` when
//       the app passed `D3DCREATE_MULTITHREADED`). Use
//       `DXMT_ASSERT_OWNED_BY_OR_LOCKED(token, witness)` where `witness` is a
//       real expression proving the lock is held — never a hardcoded `true`.
//
// Cost: under `NDEBUG` the class has no members and every method is an empty
// inline, and the macros expand through `DXMT_ASSERT` to `((void)0)` so
// neither the token nor the witness expression is evaluated. Prefer
// `[[no_unique_address]]` at member declaration sites so the empty release
// object cannot add padding to a hot struct.

#include "dxmt9/assert.hpp"

#ifndef NDEBUG
#include <atomic>
#endif

namespace dxmt9::core {

#ifndef NDEBUG
namespace detail {

// Address of a per-thread object. `inline` gives the whole program one
// instance per thread, so two translation units comparing identities agree.
inline const void* currentThreadOwnershipIdentity() noexcept {
  static thread_local const char marker = 0;
  return &marker;
}

}  // namespace detail
#endif

// Tag type selecting the deferred-binding constructor (shape (b) above).
struct DeferThreadOwnership {};
inline constexpr DeferThreadOwnership deferThreadOwnership{};

class ThreadOwnershipToken {
 public:
  // Shape (a): binds to the constructing thread.
  ThreadOwnershipToken() noexcept { bindToCurrentThread(); }
  // Shape (b): starts unowned. An unowned token accepts every thread, so a
  // token that is never bound never fires — bind it at the acquisition point.
  explicit ThreadOwnershipToken(DeferThreadOwnership) noexcept {}

  ThreadOwnershipToken(const ThreadOwnershipToken&) = delete;
  ThreadOwnershipToken& operator=(const ThreadOwnershipToken&) = delete;

  // Claim ownership for the calling thread (shape (b) acquisition point).
  void bindToCurrentThread() noexcept {
#ifndef NDEBUG
    owner_.store(detail::currentThreadOwnershipIdentity(),
                 std::memory_order_relaxed);
#endif
  }

  // Drop ownership; the state becomes unowned until the next bind.
  void unbind() noexcept {
#ifndef NDEBUG
    owner_.store(nullptr, std::memory_order_relaxed);
#endif
  }

  bool ownedByCurrentThread() const noexcept {
#ifndef NDEBUG
    const void* const owner = owner_.load(std::memory_order_relaxed);
    return owner == nullptr ||
           owner == detail::currentThreadOwnershipIdentity();
#else
    return true;
#endif
  }

 private:
#ifndef NDEBUG
  // Atomic because a rebindable token is legitimately written by one thread
  // and read by another (the shape-(c) exception); a plain member would make
  // the DEBUG BUILD ITSELF racy while diagnosing a race.
  std::atomic<const void*> owner_{nullptr};
#endif
};

}  // namespace dxmt9::core

// R-BACK-43.5 — `producer-owned` / `worker-owned` confinement check.
#define DXMT_ASSERT_OWNED_BY(token) DXMT_ASSERT((token).ownedByCurrentThread())

// R-BACK-43.5 shape (c) — owner OR a documented lock holder. `lockedWitness`
// must be an expression that actually observes the lock (e.g.
// `lock.owns_lock()`, a `recorderLockRequired_` policy flag), so the assert
// still fires when a future edit reaches the state with neither.
#define DXMT_ASSERT_OWNED_BY_OR_LOCKED(token, lockedWitness) \
  DXMT_ASSERT((lockedWitness) || (token).ownedByCurrentThread())
