// Producer mark / reclaim DETERMINISTIC INTERLEAVING harness.
//
// Discharges the "C++ memory-order harness (R-BACK-43.6 residual)" row in
// `specs/backend/producer-concurrency/gap.md`; design brief §5 layer 3,
// R-VERIF-7.3 direction. It is the third and last layer of the
// semantic-isomorphism plan:
//
//   layer 1  shared pure predicates          src/dxmt9/dxmt9_mark_reclaim_predicates.hpp
//   layer 2  TLC-trace conformance           tests/native/backend/producer_mark_reclaim_spec.cpp
//   layer 3  THIS FILE — real threads, real atomics, real production calls
//
// Layer 2's own closing paragraph states what it does not cover: "it does NOT
// mean the C++ atomics are correctly ordered". That is this file's job.
//
// ===========================================================================
// WHAT EXECUTES REAL PRODUCTION CODE, AND WHAT IS MIRRORED
// ===========================================================================
// A mirrored piece proves strictly less than a real one, so the split is
// stated up front rather than left to be inferred from the code.
//
// REAL — this harness calls the production symbol itself:
//
//   symbol                                              declared at
//   --------------------------------------------------  -------------------------------------
//   CommandQueue (ArenaLeaseTestQueueTag ctor)           dxmt9_command_queue.hpp:369
//   CommandQueue::markTicketAcquire()                    dxmt9_command_queue.hpp:907
//     -> nextSeqId_.load(std::memory_order_acquire)      dxmt9_command_queue.hpp:908
//   CommandQueue::nextSeqId_ (the std::atomic itself)    dxmt9_command_queue.hpp:902
//   CommandQueue::mutex_ (the std::mutex itself)         dxmt9_command_queue.hpp:989
//   CommandQueue::completedSeqId_                        dxmt9_command_queue.hpp:920
//   CommandQueue::markChunkResources                     dxmt9_command_queue.hpp:541
//     -> seqIdForMark / markChunkResourcesWithExactSeq /
//        restampIfTicketAdvancedLocked                   dxmt9_command_queue.cpp:3300-3336
//   CommandQueue::markChunkResourcesAndCaptureBufferBindings
//                                                        dxmt9_command_queue.hpp:543
//     (the T2a stamps-before-capture path)               dxmt9_command_queue.cpp:3338-3391
//   resources::Pool::createBuffer / findBuffer           dxmt9_resource_pool.hpp
//   resources::Pool::markBufferUse                       dxmt9_resource_pool.cpp:965
//   resources::Pool::markBufferDestroyAndGc              dxmt9_resource_pool.cpp:193
//   resources::Pool::reclaimCompleted -> gcArena         dxmt9_resource_pool.cpp:180 / :160
//   resources::Pool::captureChunkBufferBinding           dxmt9_resource_pool.cpp:1000
//   detail::HandleArena<>::update / inspect /
//     reclaimCompleted / insert  (its std::shared_mutex
//     and its std::deque slots)                          dxmt9_resource_pool.hpp:307-503
//   resources::canReclaimRecord / markStampUpper         dxmt9_mark_reclaim_predicates.hpp
//
// MIRRORED — reimplemented here, with the production line it mirrors and the
// drift audit that catches a divergence:
//
//   1. SlotAdvance (the publisher that raises `nextSeqId_`).
//      Mirrors  QueueLifecycleController::commitCurrentChunk's publish
//               increment, `src/dxmt9/dxmt9_queue.cpp:1734-1735`, and the
//               arena-admission increment,
//               `src/dxmt9/dxmt9_command_queue.cpp:3883`.
//      Why      Both writers sit at the bottom of the slot-publication /
//               arena-reservation machinery, which needs a Metal device and
//               a full source pipeline that a host spec cannot stand up.
//      Real     The atomic AND the mutex are the production objects. Only the
//               surrounding slot bookkeeping is absent.
//      Audit    `auditProductionMemoryOrders()` below reads both files and
//               fails if either store stops being `memory_order_release`, or
//               if `markTicketAcquire()` stops being `memory_order_acquire`.
//
//   2. restampIfTicketAdvancedLocked, decomposed.
//      Mirrors  `src/dxmt9/dxmt9_command_queue.cpp:2255-2264` (a file-local
//               template — not reachable from a test TU).
//      Why      Schedule S2A needs a race-free breadcrumb BETWEEN the
//               unlocked stamp and the frozen re-read, which the integrated
//               call does not expose.
//      Real     Every constituent — `markTicketAcquire()`, `markBufferUse`,
//               `mutex_` — is the production symbol. Only the four-line
//               control flow is copied.
//      Audit    `auditProductionMemoryOrders()` pins the copied body text.
//      Note     Schedule S2B runs the same race through the REAL
//               `markChunkResources`, so the integrated path is covered too;
//               S2A exists to prove which branch S2B's outcome came from.
//
//   3. The retainer pin (PinDiscipline's premise).
//      Mirrors  `src/d3d9/d3d9_pe_retainer.hpp` — PE-side, a different
//               translation unit family that a unix-side backend spec does
//               not link. The premise it supplies ("a chunk-named record is
//               reference-held for the whole marking window, so it cannot be
//               destroyPending") is modelled as a refcount here.
//      Audit    None available in-process; the premise itself is what
//               `ProducerMarkReclaim.tla`'s `PinDiscipline="Removed"` cfg
//               model-checks, and schedule S1B below is that cfg's trace made
//               executable, so a regression in the premise is visible as a
//               behaviour difference between S1A and S1B.
//
// The retired replay submission carrier no longer has a separate worker-side
// batch-mark call site. The producer schedule below remains the native binding
// for `restampIfTicketAdvancedLocked` and the shared stamp predicate.
//
// ===========================================================================
// WHAT A SCRIPTED SCHEDULE CAN AND CANNOT SEE
// ===========================================================================
// `interleaving_scheduler.hpp` advances a shared cursor with release/acquire
// between consecutive steps. That is a happens-before edge, so a scripted
// schedule proves PROTOCOL order (which statement runs before which) and is
// structurally blind to MEMORY order (whether an atomic is `acquire` or
// `relaxed`) — the scheduler would supply the missing edge itself.
//
// Memory order is therefore covered by three other things, in decreasing
// strength:
//
//   (a) A ThreadSanitizer build of this same binary. TSan instruments the
//       real atomics with their real orders and does not care about the
//       scheduler's edge. This is the strongest evidence the harness offers.
//   (b) `auditProductionMemoryOrders()` — a deterministic source-text
//       contract on the production orders. It is the piece that FAILS, on
//       every host, on the first run, the moment someone weakens `acquire` to
//       `relaxed`; a race-based lane can only fail probabilistically.
//   (c) `testFreeRunningPublishAcquirePairing()` — an unscripted lane with no
//       cursor between the threads, plus a deliberately-`relaxed` negative
//       control run through the identical lane so the lane's detection power
//       is measured rather than assumed.
//
// On x86-TSO (a) and (b) are the whole story: TSO forbids the store-store and
// load-load reordering a relaxed pair would need, so lane (c) is blind there
// by construction. This file is developed on arm64, where the reordering is
// architecturally permitted and (c) has a real chance — the negative control
// prints whether it took it. Read that number before citing (c) anywhere.
//
// RUNNING LANE (a). There is no sanitizer build dir in-tree; configure one
// beside the host build and run this spec plus its layer-2 companion:
//
//   meson setup build-tsan -Db_sanitize=thread -Db_lundef=false
//   meson compile -C build-tsan dxmt9-producer-interleaving-spec
//   TSAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
//     build-tsan/tests/native/backend/dxmt9-producer-interleaving-spec "$PWD"
//
// `TSAN_OPTIONS` is not optional. `tests/meson.build` sets halt-on-error for
// ASan/UBSan/MSan but NOT for TSan, so a plain `meson test -C build-tsan` run
// prints a race warning and still reports OK. A green TSan `meson test` line
// is therefore weaker evidence than it looks; cite the explicit invocation.
//
// LANE (a) AND LANE (c) MASK EACH OTHER, measured 2026-08-20 on arm64: the
// relaxed negative control reports tens of thousands of violations in a plain
// build and ZERO under TSan, because TSan's instrumentation serializes the
// threads enough to hide the reordering. Run both; neither subsumes the other.
//
// A FINDING, recorded here because it bounds what (c) is evidence FOR: on
// today's code the mark path does not actually depend on the release/acquire
// pair. `dxmt9_command_queue.hpp:886-901` says so itself — "The mark path
// does not depend on that". What the stamp protocol needs from `nextSeqId_`
// is atomicity and monotonicity, plus a frozen re-read whose ordering comes
// from `mutex_`. The pair is the guarantee a LOCK-FREE CONSUMER OF THE
// PUBLISHED SLOT would rest on, and gap.md lists three unbuilt features
// (T2b capture, T2c `completedSeqId_`, T2d append) that each add exactly such
// a consumer. Lane (c) stands in for that consumer so the guarantee is pinned
// before something depends on it — not because something already does.

#include "../../../src/dxmt9/dxmt9_command_queue.hpp"
#include "../../../src/dxmt9/dxmt9_mark_reclaim_predicates.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"

#include "interleaving_scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using dxmt9::CommandQueue;
using dxmt9::core::BackendLimits;
using dxmt9::core::ChunkHandleEntry;
using dxmt9::core::ChunkHandleKind;
using dxmt9::core::Handle;
using dxmt9::resources::canReclaimRecord;
using dxmt9::resources::markStampUpper;
using dxmt9::test::interleave::DeterministicRandom;
using dxmt9::test::interleave::Schedule;

using u64 = std::uint64_t;

// ---------------------------------------------------------------------------
// Test plumbing
// ---------------------------------------------------------------------------

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

// ---------------------------------------------------------------------------
// (b) Production memory-order source contract
// ---------------------------------------------------------------------------
//
// The deterministic half of the memory-order obligation. A race-based lane
// fails probabilistically and only on a weak-memory host; this fails on every
// host, on the first run, the moment a production order is weakened or a
// mirrored body drifts from the original.
//
// Kept inside this spec rather than added as a `scripts/check` audit so the
// obligation and its guard live in one file — a future reader of the mirror
// sees the drift check three lines away from the copy.

std::string readSourceFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    fail("could not open production source for the memory-order audit: " + path);
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// Collapses runs of whitespace so the contract survives reformatting/rewrapping
// but not a token change.
std::string squeeze(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(c);
  }
  return out;
}

void requireContains(const std::string& haystack, std::string_view needle,
                     std::string_view what) {
  if (haystack.find(squeeze(needle)) == std::string::npos) {
    std::ostringstream out;
    out << "production memory-order contract drifted: " << what
        << " — expected to find `" << needle << "`";
    fail(out.str());
  }
}

void auditProductionMemoryOrders(const std::string& sourceRoot) {
  const std::string queueHpp =
      squeeze(readSourceFile(sourceRoot + "/src/dxmt9/dxmt9_command_queue.hpp"));
  const std::string queueCpp =
      squeeze(readSourceFile(sourceRoot + "/src/dxmt9/dxmt9_command_queue.cpp"));
  const std::string lifecycleCpp =
      squeeze(readSourceFile(sourceRoot + "/src/dxmt9/dxmt9_queue.cpp"));

  // The ticket is an ATOMIC and the lock-free read is an ACQUIRE load. Both
  // halves matter: making the member plain would remove the atomicity this
  // whole harness exercises, and relaxing the load would drop the edge that
  // pairs with the two release publishes below.
  requireContains(queueHpp, "std::atomic<std::uint64_t> nextSeqId_{1};",
                  "CommandQueue::nextSeqId_ must stay a std::atomic");
  requireContains(queueHpp,
                  "return nextSeqId_.load(std::memory_order_acquire);",
                  "markTicketAcquire() must stay an acquire load");

  // Publisher 1 — arena admission (`beginCpuReadyArenaSource`). Mirrored by
  // `Harness::slotAdvance()`.
  requireContains(queueCpp,
                  "nextSeqId_.store(admittedSeqId + 1, std::memory_order_release);",
                  "the arena-admission publish must stay a release store");

  // Publisher 2 — slot publication (`commitCurrentChunk`). Same mirror.
  requireContains(lifecycleCpp,
                  "nextSeqId->store(nextSeqId->load(std::memory_order_relaxed) + 1, "
                  "std::memory_order_release);",
                  "the slot-publication publish must stay a release store");

  // The mirrored re-stamp body. If production's control flow changes, S2A's
  // decomposition is no longer the same protocol and must be re-derived.
  // (T2b/c added the gated restamp-fire counter between the frozen re-read
  // and the conditional stamp — race-neutral: two atomic adds under mutex_,
  // no effect on the frozen-ticket/monotone-max protocol S2A mirrors.)
  requireContains(queueCpp,
                  "const std::uint64_t frozenTicket = q.markTicketAcquire(); "
                  "const bool restamped = frozenTicket != markTicket;",
                  "restampIfTicketAdvancedLocked's frozen re-read is mirrored by S2A");
  requireContains(queueCpp,
                  "if (restamped) { stamp(frozenTicket); } "
                  "return frozenTicket;",
                  "restampIfTicketAdvancedLocked's conditional stamp is mirrored by S2A");

  // The two production callers this harness drives must still take the ticket
  // BEFORE the mutex — the whole stale-window premise. `markChunkResources`
  // and its capture twin both read the same way.
  requireContains(queueCpp,
                  "const std::uint64_t markTicket = seqIdForMark(*this, 0); "
                  "markChunkResourcesWithExactSeq(pool_, entries, markTicket);",
                  "the producer bulk mark must still stamp before locking");

  // Stamps-before-capture (spec.md §4). T2b moved the capture LOOP into
  // Pool::captureChunkBufferBindings; the ordering obligation now lives at
  // the queue's combined mark+capture entry: the frozen-ticket re-stamp must
  // precede the pool capture CALL. If the capture call ever moves ahead of
  // the re-stamp, schedule S3d's premise is void.
  const auto restampAt = queueCpp.find("(void)restampIfTicketAdvancedLocked(");
  const auto captureCallAt =
      queueCpp.find("return pool_.captureChunkBufferBindings(entries, snapshots);");
  check(restampAt != std::string::npos && captureCallAt != std::string::npos,
        "memory-order audit could not locate the capture/re-stamp sites");
  check(restampAt < captureCallAt,
        "stamps-before-capture ordering drifted: the re-stamp must precede "
        "the pool capture call in the combined mark+capture entry");
}

// NEGATIVE CONTROL for the audit above. An audit that silently stopped
// matching would pass forever and guard nothing, and asserting that a wrong
// token is ABSENT passes vacuously in exactly that case. So instead: take the
// real production text, apply the exact weakening the audit claims to catch,
// and require the matcher to reject it.
void testMemoryOrderAuditDetectsAWeakening(const std::string& sourceRoot) {
  const auto weakenAndExpectFailure =
      [](std::string text, std::string_view from, std::string_view to,
         std::string_view contract, std::string_view what) {
        const auto at = text.find(from);
        check(at != std::string::npos,
              "audit negative control could not find the token to weaken");
        text.replace(at, from.size(), to);
        bool caught = false;
        try {
          requireContains(squeeze(text), contract, what);
        } catch (const TestFailure&) {
          caught = true;
        }
        check(caught,
              "the memory-order audit does not detect the weakening it claims "
              "to catch — it is not guarding anything");
      };

  weakenAndExpectFailure(
      readSourceFile(sourceRoot + "/src/dxmt9/dxmt9_command_queue.hpp"),
      "nextSeqId_.load(std::memory_order_acquire)",
      "nextSeqId_.load(std::memory_order_relaxed)",
      "return nextSeqId_.load(std::memory_order_acquire);",
      "markTicketAcquire() weakened to relaxed");

  weakenAndExpectFailure(
      readSourceFile(sourceRoot + "/src/dxmt9/dxmt9_command_queue.cpp"),
      "nextSeqId_.store(admittedSeqId + 1, std::memory_order_release)",
      "nextSeqId_.store(admittedSeqId + 1, std::memory_order_relaxed)",
      "nextSeqId_.store(admittedSeqId + 1, std::memory_order_release);",
      "the arena-admission publish weakened to relaxed");

  weakenAndExpectFailure(
      readSourceFile(sourceRoot + "/src/dxmt9/dxmt9_queue.cpp"),
      "std::memory_order_release);\n    slot.state",
      "std::memory_order_relaxed);\n    slot.state",
      "nextSeqId->store(nextSeqId->load(std::memory_order_relaxed) + 1, "
      "std::memory_order_release);",
      "the slot-publication publish weakened to relaxed");
}

// ---------------------------------------------------------------------------
// Harness over the real queue + pool
// ---------------------------------------------------------------------------

dxmt9::core::BufferDesc dynamicBufferDesc(u64 size = 64u) {
  dxmt9::core::BufferDesc desc{};
  desc.size = size;
  desc.pool = dxmt9::core::Pool::Default;
  // DEFAULT+DYNAMIC keeps the record off the heap manager (heap classify
  // rejects UsageDynamic, R-BACK-14.2) and arms the rename ring, so the
  // capture read-set schedule below has something to capture.
  desc.usage = dxmt9::core::UsageDynamic;
  return desc;
}

// Owns one real `CommandQueue` (thread-free arena-lease fixture) and the
// harness-side actors the production objects need around them.
class Harness {
 public:
  Harness() : queue_(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{}) {}

  CommandQueue& queue() noexcept { return queue_; }
  dxmt9::resources::Pool& pool() noexcept { return queue_.pool(); }

  // Production contract: creates are queue-mutex ops (they touch
  // `Pool::heapManager_`, which the arena mutex does not cover).
  Handle createBuffer(u64 size = 64u) {
    std::lock_guard<std::mutex> guard(queue_.mutex_);
    return queue_.pool().createBuffer(WMT::Device{NULL_OBJECT_HANDLE},
                                      dynamicBufferDesc(size));
  }

  bool alive(Handle handle) noexcept {
    // Locked lookup; only the returned POINTER is inspected, never a field, so
    // this is race-free even inside a free-running window.
    return queue_.pool().findBuffer(handle.value) != nullptr;
  }

  // Plain field read. Safe ONLY from a scheduler sync step or after a join —
  // the cursor's release/acquire (or the join) orders it against the writer.
  u64 stampOf(Handle handle) {
    const auto* record = queue_.pool().findBuffer(handle.value);
    return record ? record->lastUsedSeqId : 0;
  }

  u64 ticket() noexcept { return queue_.markTicketAcquire(); }

  // MIRROR of the two production publishers — see the file header, item 1.
  // `src/dxmt9/dxmt9_queue.cpp:1734` and
  // `src/dxmt9/dxmt9_command_queue.cpp:3883`. The atomic and the mutex are
  // real; the slot bookkeeping the real publishers write BEFORE the release
  // store (`slot.seqId`, `writingPayload->seqId`, `sealAndPublish`) is stood
  // in for by `publishedBookkeeping_`, so the release store still has prior
  // writes to order — which is what the production comment at
  // `dxmt9_queue.cpp:1732` says the release is for.
  //
  // The stand-in is `std::atomic` with RELAXED accesses where production uses
  // plain writes. That substitution is required so the free-running pairing
  // lane below is not itself a data race (it would turn the ThreadSanitizer
  // lane red for a harness artifact). It does not change the ordering
  // question: relaxed stores before a release store, read by relaxed loads
  // after an acquire load, is exactly the message-passing shape the
  // production release/acquire pair provides.
  void slotAdvance() {
    std::lock_guard<std::mutex> guard(queue_.mutex_);
    slotAdvanceLocked();
  }

  // Same publish, for a caller that already holds `queue_.mutex_`. Production
  // publishers are always called with the mutex held; the S2 schedules need
  // this form because they keep the lock across scheduler steps to hold the
  // producer inside its stale-ticket window.
  void slotAdvanceLocked() {
    const u64 current = queue_.nextSeqId_.load(std::memory_order_relaxed);
    for (auto& slot : publishedBookkeeping_) {
      slot.store(current, std::memory_order_relaxed);
    }
    queue_.nextSeqId_.store(current + 1, std::memory_order_release);
  }

  // Lowest ordinal any bookkeeping slot has been published with. Monotone, so
  // a reader that has observed seq S through the acquire load must see at
  // least S-1 here if the release/acquire pair holds.
  u64 publishedOrdinalMin() const noexcept {
    u64 lowest = ~u64{0};
    for (const auto& slot : publishedBookkeeping_) {
      lowest = std::min(lowest, slot.load(std::memory_order_relaxed));
    }
    return lowest;
  }

  // Completion analog: advance the GPU watermark, then run the real reclaim
  // scan. Guarded like the model's `AdvanceCompleted` — the GPU can only
  // complete a seq that has been published.
  bool completionAdvance() {
    u64 completed = 0;
    {
      std::lock_guard<std::mutex> guard(queue_.mutex_);
      if (queue_.completedSeqId_ + 1 >= queue_.nextSeqId_.load(std::memory_order_relaxed)) {
        return false;
      }
      queue_.completedSeqId_ += 1;
      completed = queue_.completedSeqId_;
    }
    queue_.pool().reclaimCompleted(completed);
    return true;
  }

  u64 completedSeqId() {
    std::lock_guard<std::mutex> guard(queue_.mutex_);
    return queue_.completedSeqId_;
  }

  static std::vector<ChunkHandleEntry> entriesFor(
      const std::vector<Handle>& handles) {
    std::vector<ChunkHandleEntry> entries;
    entries.reserve(handles.size());
    for (const Handle handle : handles) {
      entries.push_back(ChunkHandleEntry{ChunkHandleKind::Buffer, handle});
    }
    return entries;
  }

 private:
  CommandQueue queue_;
  std::array<std::atomic<u64>, 3> publishedBookkeeping_{};
};

// MIRROR of the PE-side retainer (`src/d3d9/d3d9_pe_retainer.hpp`) — see the
// file header, item 3. What matters is not the refcount mechanics but the
// PREMISE it supplies: while a chunk names a record, a reference is held, so
// the record cannot become destroyPending. `enforcePinDiscipline` is the
// model's `PinDiscipline` constant: `true` is production, `false` is the
// `PinDiscipline = "Removed"` counterexample world.
class Retainer {
 public:
  explicit Retainer(bool enforcePinDiscipline) noexcept
      : enforce_(enforcePinDiscipline) {}

  void pin(Handle handle) {
    std::lock_guard<std::mutex> guard(mutex_);
    ++refs_[handle.value];
  }

  void unpin(Handle handle) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = refs_.find(handle.value);
    if (it != refs_.end() && it->second > 0) {
      --it->second;
    }
  }

  bool pinned(Handle handle) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = refs_.find(handle.value);
    return it != refs_.end() && it->second > 0;
  }

  // TLA+: ProducerMarkReclaim!SetDestroyPending(r). Under the production
  // discipline the `!pinned` conjunct is the guard; removing it is the whole
  // of `PinDiscipline = "Removed"`.
  //
  // Returns true when the destroy was actually issued into the real pool.
  bool dropLastRefAndDestroy(Harness& harness, Handle handle) {
    if (enforce_ && pinned(handle)) {
      return false;
    }
    return harness.pool().markBufferDestroyAndGc(handle.value,
                                                 harness.completedSeqId());
  }

 private:
  bool enforce_;
  std::mutex mutex_;
  std::unordered_map<u64, int> refs_;
};

// ===========================================================================
// (a) Scripted schedules — model counterexample translations
// ===========================================================================

// Thread roles. Named so a schedule reads like the model's actor list.
constexpr int kProducer = 0;    // game thread: commit_chunk's mark/capture
constexpr int kPublisher = 1;   // whoever raises nextSeqId_ (SlotAdvance)
constexpr int kCompletion = 2;  // finish loop: watermark + reclaim

// ---------------------------------------------------------------------------
// S1 — `ProducerMarkReclaim.counterexample.cfg`, PinDiscipline = "Removed".
//
// TLC's minimal trace, three states:
//
//   State 2: PinChunkResources({r1})
//   State 3: SetDestroyPending(r1)
//   State 4: Reclaim(r1)            <- Invariant NoUseAfterFree is violated
//
// S1A runs it against the PRODUCTION premise and asserts the SAFE outcome:
// the destroy is not enabled, so nothing is reclaimed and the producer's real
// `markChunkResources` lands on a live record.
//
// S1B runs the identical script with the premise removed and asserts the
// harness can actually SEE the fault — a model that cannot see its own bug
// class proves nothing, and neither can a harness.
// ---------------------------------------------------------------------------

struct PinScheduleResult {
  bool destroyIssued = false;
  bool aliveAfterDestroyStep = false;
  bool aliveAtEnd = false;
  u64 stampAtEnd = 0;
  u64 ticketAtCommit = 0;
};

PinScheduleResult runPinSchedule(bool enforcePinDiscipline) {
  Harness harness;
  Retainer retainer(enforcePinDiscipline);
  PinScheduleResult result;

  const Handle r1 = harness.createBuffer();
  const auto entries = Harness::entriesFor({r1});

  Schedule schedule(3);

  // State 2 — PinChunkResources({r1}). The PE recorder retainer takes its
  // reference as the chunk names the resource; the commit window is open from
  // here until the mark returns.
  schedule.sync(kProducer, [&] {
    retainer.pin(r1);
    check(harness.alive(r1), "S1: r1 must exist when the chunk names it");
  });

  // State 3 — SetDestroyPending(r1), from the completion actor. In production
  // this is a last-unix-ref drop reaching `mark*DestroyAndGc`, which also runs
  // the reclaim scan, so States 3 and 4 are ONE C++ call.
  schedule.sync(kCompletion, [&] {
    result.destroyIssued = retainer.dropLastRefAndDestroy(harness, r1);
    result.aliveAfterDestroyStep = harness.alive(r1);
  });

  // The producer's real bulk mark, running after the reclaim attempt. Under
  // the production discipline this stamps a live record; under the removed
  // discipline the handle no longer resolves and the stamp is silently lost —
  // which is the C++ shape of the model's `useAfterFree`.
  schedule.sync(kProducer, [&] {
    result.ticketAtCommit = harness.ticket();
    harness.queue().markChunkResources(entries);
    result.aliveAtEnd = harness.alive(r1);
    result.stampAtEnd = harness.stampOf(r1);
    retainer.unpin(r1);
  });

  schedule.run();
  return result;
}

void testPinDisciplineCounterexampleTraceIsSafeInProduction() {
  const PinScheduleResult production = runPinSchedule(true);

  check(!production.destroyIssued,
        "S1A: the retainer pin must disable SetDestroyPending — this is the "
        "pin-ordering premise, executed rather than assumed");
  check(production.aliveAfterDestroyStep,
        "S1A: no reclaim may happen inside the commit window");
  check(production.aliveAtEnd, "S1A: r1 survives the whole schedule");
  checkEq(production.stampAtEnd, production.ticketAtCommit,
          "S1A: the real markChunkResources stamped the live record with its "
          "own ticket");

  // Stated through the shared predicate, not a restatement of it: a record
  // that never reached destroyPending is not reclaimable at ANY watermark.
  check(!canReclaimRecord(/*destroyPending=*/false, production.stampAtEnd,
                          production.stampAtEnd),
        "S1A: the reclaim gate stays closed for a pinned record");
}

void testPinDisciplineRemovedReproducesTheFault() {
  const PinScheduleResult buggy = runPinSchedule(false);

  check(buggy.destroyIssued,
        "S1B: with the premise removed the destroy must actually be issued");
  check(!buggy.aliveAfterDestroyStep,
        "S1B: the record is reclaimed while the commit window is open — the "
        "model's Reclaim(r1) at State 4");
  check(!buggy.aliveAtEnd, "S1B: the record stays gone");
  checkEq(buggy.stampAtEnd, u64{0},
          "S1B: the producer's stamp landed on nothing — the handle no longer "
          "resolves, so markBufferUse's arena update is a no-op and the mark "
          "is silently lost");

  // The same gate, evaluated on the state the buggy world reached: at
  // completedSeqId 0 a never-stamped destroyPending record IS reclaimable.
  // That is exactly why the pin, not the watermark, is what protects a record
  // being marked (design brief §2).
  check(canReclaimRecord(/*destroyPending=*/true, /*lastUsedSeqId=*/0,
                         /*completedSeqId=*/0),
        "S1B: the watermark alone cannot see the open commit window");

  // And the discriminating fact between the two worlds, so a future change
  // that made S1B vacuous would fail here rather than pass quietly.
  const PinScheduleResult production = runPinSchedule(true);
  check(production.aliveAtEnd != buggy.aliveAtEnd,
        "S1: the two disciplines must reach different outcomes, or the "
        "harness is not testing the premise");
}

// ---------------------------------------------------------------------------
// S2 — `ProducerMarkReclaim.restamp.counterexample.cfg`,
//      RestampDiscipline = "Removed". TLC's nine-state trace:
//
//   WorkerBeginBatch({r2}) -> SlotAdvance -> WorkerStampMark(r2) ->
//   WorkerEndStamping -> WorkerAppend -> AdvanceCompleted ->
//   WorkerReleaseBatchRefs -> SetDestroyPending(r2) -> Reclaim(r2)
//
// The C++ shape of the first five states is: read the ticket lock-free, let a
// concurrent publisher raise it, stamp with the stale value, then take the
// queue mutex. Reproducing that ordering needs the publisher to HOLD
// `CommandQueue::mutex_` across the producer's call, which is why the
// producer's step is `async` — a sync step would block the cursor.
//
// S2A decomposes the protocol so the stale branch is provably taken;
// S2B runs the identical script through the real `markChunkResources`.
// ---------------------------------------------------------------------------

struct RestampScheduleResult {
  u64 initialTicket = 0;
  u64 staleStamp = 0;
  u64 frozenTicket = 0;
  u64 finalStamp = 0;
  u64 publishedSeqId = 0;
};

// S2A — decomposed. MIRROR of restampIfTicketAdvancedLocked
// (`src/dxmt9/dxmt9_command_queue.cpp:2255-2264`); every constituent call is
// production. The breadcrumbs are `std::atomic`, so the publisher's
// observation of them is race-free even though it happens mid-window.
RestampScheduleResult runRestampScheduleDecomposed() {
  Harness harness;
  RestampScheduleResult result;

  const Handle r2 = harness.createBuffer();

  std::atomic<bool> staleStampLanded{false};
  std::atomic<u64> observedTicket{0};
  std::optional<std::unique_lock<std::mutex>> publisherLock;

  Schedule schedule(3);

  // The publisher takes the queue mutex and keeps it. Everything the producer
  // does before its own `lock(mutex_)` is now provably inside the window.
  schedule.sync(kPublisher, [&] {
    publisherLock.emplace(harness.queue().mutex_);
  });

  // WorkerBeginBatch + WorkerStampMark, then block at WorkerEndStamping's
  // mutex acquire. Async: this op is EXPECTED to block.
  schedule.async(kProducer, [&] {
    // REAL: CommandQueue::markTicketAcquire(), an acquire load of the real
    // atomic, taken with no queue lock held. TLA+: WorkerBeginBatch.
    const u64 ticket = harness.queue().markTicketAcquire();
    observedTicket.store(ticket, std::memory_order_release);

    // REAL: Pool::markBufferUse -> HandleArena::update -> markStampUpper,
    // under the arena's own mutex. This is the arena-stamp exception the pool
    // header licenses. TLA+: WorkerStampMark.
    harness.pool().markBufferUse(r2, ticket);
    staleStampLanded.store(true, std::memory_order_release);

    // REAL: the production mutex. Blocks — the publisher holds it.
    // TLA+: WorkerEndStamping.
    std::unique_lock<std::mutex> lock(harness.queue().mutex_);

    // MIRRORED CONTROL FLOW (production body cited above); REAL calls.
    // TLA+: WorkerRestamp.
    const u64 frozenTicket = harness.queue().markTicketAcquire();
    if (frozenTicket != ticket) {
      harness.pool().markBufferUse(r2, frozenTicket);
    }
    result.frozenTicket = frozenTicket;
  });

  // Deterministic rendezvous, no sleep: the producer's very next action after
  // setting this flag is the mutex acquire the publisher is holding, so the
  // publisher is guaranteed to observe it and guaranteed that nothing else in
  // the producer can run until the unlock below.
  schedule.sync(kPublisher, [&] {
    Schedule::spinUntil(
        [&] { return staleStampLanded.load(std::memory_order_acquire); },
        "producer's unlocked stale stamp");
    result.initialTicket = observedTicket.load(std::memory_order_acquire);
    // The stale watermark, OBSERVED rather than inferred. This read is
    // race-free on both sides: the acquire on `staleStampLanded` pairs with
    // the producer's release after its `markBufferUse`, and the producer
    // cannot write the field again until it takes `queue.mutex_`, which this
    // thread is holding.
    result.staleStamp = harness.stampOf(r2);
  });

  // SlotAdvance, inside the hold. This is the force-publish the design brief
  // §9 names: "the writing slot is not worker-exclusive". It must use the
  // already-held lock: `CommandQueue::mutex_` is a plain `std::mutex`, so
  // re-entering it from the same thread would be undefined behaviour.
  schedule.sync(kPublisher, [&] {
    harness.slotAdvanceLocked();
    result.publishedSeqId =
        harness.queue().nextSeqId_.load(std::memory_order_relaxed);
    publisherLock.reset();  // unlock — the producer's acquire can now proceed
  });

  schedule.joinAsync(kPublisher, kProducer);

  schedule.sync(kPublisher, [&] { result.finalStamp = harness.stampOf(r2); });

  schedule.run();
  return result;
}

void testRestampCoversAdvancedSlotSeqDecomposed() {
  const RestampScheduleResult r = runRestampScheduleDecomposed();

  checkEq(r.initialTicket, u64{1}, "S2A: the lock-free ticket read saw seq 1");
  checkEq(r.staleStamp, u64{1},
          "S2A: the record really was stamped with the stale ticket while the "
          "publish was still pending — this is the branch S2B cannot observe");
  checkEq(r.publishedSeqId, u64{2},
          "S2A: the concurrent publish raised the seq to 2");
  checkEq(r.frozenTicket, u64{2},
          "S2A: the re-read under the queue mutex saw the raised value — the "
          "ticket is frozen there because every writer needs the same mutex");
  checkEq(r.finalStamp, u64{2},
          "S2A: the re-stamp raised the record's watermark to the seq its "
          "chunk will be published under");

  // The counterexample, argued on values this run actually observed rather
  // than on a hypothetical. Without the re-stamp the record's watermark would
  // still be `staleStamp`, and at the moment the GPU completes seq 1 the
  // shared reclaim gate opens on a record whose chunk (seq 2) is still
  // pending — a PREMATURE RECLAIM, which is the failure mode design brief §9
  // calls out as invisible to the PinDiscipline axis.
  check(canReclaimRecord(/*destroyPending=*/true, r.staleStamp,
                         /*completedSeqId=*/1),
        "S2A: the stale stamp WOULD have been reclaimable at completedSeqId 1");
  check(!canReclaimRecord(/*destroyPending=*/true, r.finalStamp,
                          /*completedSeqId=*/1),
        "S2A: the re-stamped value is not — this single difference is the "
        "whole of RestampDiscipline");

  // MarkMonotonic: the repair could only raise the watermark, because
  // `markStampUpper` is a max. Stated on the observed pair.
  check(r.finalStamp >= r.staleStamp, "S2A: stamps never regress");
  checkEq(markStampUpper(r.staleStamp, r.frozenTicket), r.finalStamp,
          "S2A: the final watermark is exactly the shared predicate's value");
}

// S2B — identical script, but the producer's whole step is the REAL
// `CommandQueue::markChunkResources`, which does the ticket read, the stamp
// loop, the lock, the frozen re-read and the re-stamp itself.
//
// SCOPE NOTE, stated because it is a real limit: this schedule proves the
// OUTCOME (the record's watermark covers the published seq) but not WHICH
// branch produced it. If the producer's internal ticket read happens to land
// after the publisher's advance, the ticket is already 2 and no re-stamp is
// needed — same outcome, different path. There is no race-free way to observe
// the inside of the production call, which is why S2A exists. Together: S2A
// proves the stale branch is real and repaired, S2B proves the production
// entry point reaches the same watermark under the same script.
u64 runRestampScheduleIntegrated() {
  Harness harness;
  const Handle r2 = harness.createBuffer();
  const auto entries = Harness::entriesFor({r2});

  std::atomic<bool> producerEntered{false};
  std::optional<std::unique_lock<std::mutex>> publisherLock;
  u64 finalStamp = 0;

  Schedule schedule(3);

  schedule.sync(kPublisher,
                [&] { publisherLock.emplace(harness.queue().mutex_); });

  schedule.async(kProducer, [&] {
    producerEntered.store(true, std::memory_order_release);
    harness.queue().markChunkResources(entries);  // REAL, end to end
  });

  schedule.sync(kPublisher, [&] {
    Schedule::spinUntil(
        [&] { return producerEntered.load(std::memory_order_acquire); },
        "producer entered markChunkResources");
    harness.slotAdvanceLocked();  // the lock is already held — see S2A
    publisherLock.reset();
  });

  schedule.joinAsync(kPublisher, kProducer);
  schedule.sync(kPublisher, [&] { finalStamp = harness.stampOf(r2); });

  schedule.run();
  return finalStamp;
}

void testRestampCoversAdvancedSlotSeqIntegrated() {
  // Repeat: the branch taken varies with timing, the OUTCOME must not.
  constexpr int kRepeats = 64;
  for (int i = 0; i < kRepeats; ++i) {
    const u64 finalStamp = runRestampScheduleIntegrated();
    checkEq(finalStamp, u64{2},
            "S2B: the real markChunkResources must leave the record's "
            "watermark at the seq the slot is published under, on every run "
            "and whichever internal branch it took");
  }
}

// ---------------------------------------------------------------------------
// S3 — protocol-edge schedules that are not counterexample translations.
// ---------------------------------------------------------------------------

// S3a — advance BEFORE the ticket read. No re-stamp is needed; the outcome
// must be the same watermark. Complements S2B's "after" case, so the pair
// covers both sides of the window edge.
void testAdvanceBeforeTicketReadNeedsNoRestamp() {
  Harness harness;
  const Handle r = harness.createBuffer();
  const auto entries = Harness::entriesFor({r});
  u64 ticketAfterAdvance = 0;
  u64 finalStamp = 0;

  Schedule schedule(3);
  schedule.sync(kPublisher, [&] { harness.slotAdvance(); });
  schedule.sync(kProducer, [&] {
    ticketAfterAdvance = harness.ticket();
    harness.queue().markChunkResources(entries);
    finalStamp = harness.stampOf(r);
  });
  schedule.run();

  checkEq(ticketAfterAdvance, u64{2}, "S3a: the producer read the raised seq");
  checkEq(finalStamp, u64{2}, "S3a: stamped with the current seq, no repair");
}

// S3b — MarkMonotonic across two actors, executed. Two threads stamp the same
// record with DESCENDING seq values; the watermark must never go down. The
// checks run in sync steps, so the reads are ordered by the cursor.
void testConcurrentStampsAreMonotoneUnderTheArenaLock() {
  Harness harness;
  const Handle r = harness.createBuffer();

  // Raise the queue's seq so the descending stamps are all legitimate values
  // the protocol could produce.
  for (int i = 0; i < 8; ++i) {
    harness.slotAdvance();
  }

  u64 previous = 0;
  Schedule schedule(3);

  // Interleave two actors, deliberately descending, plus a repeat of an
  // already-applied value (idempotence) and a tie.
  const std::array<std::pair<int, u64>, 8> program{{
      {kProducer, 5}, {kPublisher, 3}, {kProducer, 7}, {kPublisher, 2},
      {kProducer, 7}, {kPublisher, 9}, {kProducer, 1}, {kPublisher, 9},
  }};

  for (const auto& [tid, seq] : program) {
    schedule.sync(tid, [&, seq] {
      // REAL: Pool::markBufferUse under the arena's unique lock.
      harness.pool().markBufferUse(r, seq);
      const u64 now = harness.stampOf(r);
      check(now >= previous, "S3b: MarkMonotonic — a stamp regressed");
      checkEq(now, markStampUpper(previous, seq),
              "S3b: the observed watermark is exactly markStampUpper's value");
      previous = now;
    });
  }

  schedule.run();
  checkEq(previous, u64{9}, "S3b: the watermark settles at the maximum stamp");
}

// S3c — reclaim of one record racing the stamp of ANOTHER. Both take the same
// arena mutex, so this is the contention the T2a relaxation actually creates:
// the producer's unlocked stamp loop against a worker-thread last-ref drop.
void testReclaimOfOtherRecordDoesNotDisturbTheMarkedOne() {
  Harness harness;
  Retainer retainer(/*enforcePinDiscipline=*/true);

  const Handle kept = harness.createBuffer();
  std::vector<Handle> doomed;
  doomed.reserve(32);
  for (int i = 0; i < 32; ++i) {
    doomed.push_back(harness.createBuffer());
  }

  const auto keptEntries = Harness::entriesFor({kept});
  retainer.pin(kept);

  std::atomic<int> destroyed{0};
  std::atomic<bool> keptVanished{false};

  // Free-running on purpose: the point is real arena-lock contention, not a
  // scripted order. Only race-free observations happen inside the window
  // (atomics and the pointer-only `alive` probe).
  dxmt9::test::interleave::freeRunning({
      [&] {
        for (int i = 0; i < 512; ++i) {
          harness.queue().markChunkResources(keptEntries);
          if (!harness.alive(kept)) {
            keptVanished.store(true, std::memory_order_relaxed);
          }
        }
      },
      [&] {
        for (const Handle handle : doomed) {
          if (retainer.dropLastRefAndDestroy(harness, handle)) {
            destroyed.fetch_add(1, std::memory_order_relaxed);
          }
        }
      },
  });

  check(!keptVanished.load(std::memory_order_relaxed),
        "S3c: a pinned, chunk-named record must never be reclaimed by another "
        "record's destroy scan");
  checkEq(destroyed.load(std::memory_order_relaxed), 32,
          "S3c: every unpinned record was actually destroyed — otherwise the "
          "contention this test claims did not happen");
  check(harness.alive(kept), "S3c: the marked record survived");
  check(harness.stampOf(kept) > 0, "S3c: and carries its stamp");
  for (const Handle handle : doomed) {
    check(!harness.alive(handle), "S3c: destroyed records stay gone");
  }
}

// S3d — capture read during a concurrent unrelated-record erase. This is the
// deque-stability contract (R-VERIF-3.4 / the pool header's pointer-stability
// note) and the T2b question in one: `captureChunkBufferBinding` inspects one
// slot while another slot is being erased and new ones pushed.
void testCaptureIsStableAcrossUnrelatedErase() {
  Harness harness;
  Retainer retainer(/*enforcePinDiscipline=*/true);

  const Handle kept = harness.createBuffer();
  retainer.pin(kept);
  const auto keptEntries = Harness::entriesFor({kept});

  // Pointer identity taken BEFORE the window; only compared, never
  // dereferenced, inside it.
  const auto* keptRecord = harness.pool().findBuffer(kept.value);
  check(keptRecord != nullptr, "S3d: fixture record exists");

  std::atomic<bool> captureDrifted{false};
  std::atomic<bool> pointerMoved{false};
  std::atomic<int> churn{0};

  dxmt9::test::interleave::freeRunning({
      [&] {
        std::vector<dxmt9::core::ChunkBufferBindingSnapshot> snapshots;
        for (int i = 0; i < 512; ++i) {
          // REAL: the T2a stamps-before-capture path, mutex and all.
          (void)harness.queue().markChunkResourcesAndCaptureBufferBindings(
              keptEntries, snapshots);
          if (snapshots.size() != 1 || snapshots[0].buffer.value != kept.value ||
              !snapshots[0].requiresCapturedBacking) {
            captureDrifted.store(true, std::memory_order_relaxed);
          }
          if (harness.pool().findBuffer(kept.value) != keptRecord) {
            pointerMoved.store(true, std::memory_order_relaxed);
          }
        }
      },
      [&] {
        // Grow the deque past its initial block and recycle freed slots, which
        // is what would invalidate a pointer if the container were a vector.
        for (int i = 0; i < 256; ++i) {
          const Handle scratch = harness.createBuffer();
          if (retainer.dropLastRefAndDestroy(harness, scratch)) {
            churn.fetch_add(1, std::memory_order_relaxed);
          }
        }
      },
  });

  check(!captureDrifted.load(std::memory_order_relaxed),
        "S3d: the captured binding for the kept record must not be disturbed "
        "by an unrelated record's erase");
  check(!pointerMoved.load(std::memory_order_relaxed),
        "S3d: HandleArena's std::deque must keep the record pointer stable "
        "across concurrent growth (R-VERIF-3.4 SlotIdentityStable)");
  checkEq(churn.load(std::memory_order_relaxed), 256,
          "S3d: the erase/create churn actually ran");
  check(harness.alive(kept), "S3d: the kept record survived the churn");
}

// ===========================================================================
// (c) Seeded pseudo-random schedules
// ===========================================================================
//
// Several hundred schedules from fixed seeds, invariants after every step.
//
// SCOPE NOTE: every step here is a `sync` step, so the schedule explores the
// interleaving of whole protocol steps but not the inside of one production
// call. That is deliberate and is the price of the determinism this lane is
// required to have — an `async` step would make the outcome depend on how the
// two threads happened to overlap, and "same seed => same result" would be
// false. Intra-call interleaving is covered by S2 (scripted, with a mutex
// rendezvous) and by S3c/S3d (free-running).

// Everything an invariant checker needs, carried alongside the real state.
struct RandomLaneState {
  std::vector<Handle> handles;
  std::vector<u64> lastSeenStamp;
  std::vector<bool> everDestroyed;
  u64 lastTicket = 0;
  u64 lastCompleted = 0;
  int stepsRun = 0;
};

void checkRandomLaneInvariants(Harness& harness, RandomLaneState& state,
                               std::string_view where) {
  const u64 ticket = harness.ticket();
  const u64 completed = harness.completedSeqId();

  if (ticket < state.lastTicket) {
    fail(std::string(where) + ": nextSeqId_ regressed");
  }
  if (completed < state.lastCompleted) {
    fail(std::string(where) + ": completedSeqId_ regressed");
  }
  if (completed >= ticket) {
    // The model's AdvanceCompleted guard: the GPU cannot complete a seq that
    // has not been published.
    fail(std::string(where) + ": completedSeqId_ reached an unpublished seq");
  }
  state.lastTicket = ticket;
  state.lastCompleted = completed;

  for (std::size_t i = 0; i < state.handles.size(); ++i) {
    const bool alive = harness.alive(state.handles[i]);
    if (!alive) {
      if (!state.everDestroyed[i]) {
        // MarkReclaim safety: a record can only disappear through a destroy
        // this lane issued.
        fail(std::string(where) + ": a record vanished without a destroy");
      }
      continue;
    }
    const u64 stamp = harness.stampOf(state.handles[i]);
    if (stamp < state.lastSeenStamp[i]) {
      fail(std::string(where) + ": MarkMonotonic violated");
    }
    // ReclaimRespectsWatermark, stated as the contrapositive through the
    // shared predicate: a live record either is not destroyPending, or its
    // watermark is still ahead of the GPU. `canReclaimRecord` returning true
    // for a record that is still here would mean the scan missed it.
    if (canReclaimRecord(/*destroyPending=*/true, stamp, completed) &&
        state.everDestroyed[i]) {
      fail(std::string(where) +
           ": a destroyed record that satisfies canReclaimRecord is still "
           "resolvable — the reclaim scan missed it");
    }
    state.lastSeenStamp[i] = stamp;
  }
}

// A digest of everything the lane can observe, used for the determinism gate.
u64 digestRandomLane(Harness& harness, const RandomLaneState& state) {
  u64 digest = 0xcbf29ce484222325ull;
  const auto fold = [&digest](u64 value) {
    digest ^= value;
    digest *= 0x100000001b3ull;
  };
  fold(harness.ticket());
  fold(harness.completedSeqId());
  for (std::size_t i = 0; i < state.handles.size(); ++i) {
    fold(harness.alive(state.handles[i]) ? 1u : 0u);
    fold(harness.stampOf(state.handles[i]));
  }
  fold(static_cast<u64>(state.stepsRun));
  return digest;
}

constexpr std::size_t kRandomLaneHandles = 4;
constexpr int kRandomLaneSteps = 48;

u64 runRandomSchedule(u64 seed) {
  Harness harness;
  Retainer retainer(/*enforcePinDiscipline=*/true);
  RandomLaneState state;
  DeterministicRandom rng(seed);

  for (std::size_t i = 0; i < kRandomLaneHandles; ++i) {
    state.handles.push_back(harness.createBuffer());
    state.lastSeenStamp.push_back(0);
    state.everDestroyed.push_back(false);
    // Half the handles start pinned, so the lane always carries records the
    // pin premise is protecting alongside records it is not.
    if ((i % 2) == 0) {
      retainer.pin(state.handles[i]);
    }
  }

  Schedule schedule(3);

  for (int step = 0; step < kRandomLaneSteps; ++step) {
    const int tid = static_cast<int>(rng.below(3));
    const std::size_t opRoll = rng.below(100);
    const std::size_t victim = rng.below(kRandomLaneHandles);
    const std::size_t second = rng.below(kRandomLaneHandles);

    // `tid` and the three rolls are captured BY VALUE: the lambda outlives
    // this loop iteration — it runs during `schedule.run()` below — so a
    // reference capture of a loop-local would dangle.
    schedule.sync(tid, [&, tid, opRoll, victim, second] {
      if (tid == kProducer) {
        std::vector<Handle> named{state.handles[victim]};
        if (second != victim) {
          named.push_back(state.handles[second]);
        }
        const auto entries = Harness::entriesFor(named);
        const u64 ticketBefore = harness.ticket();
        if (opRoll < 55) {
          harness.queue().markChunkResources(entries);
        } else {
          std::vector<dxmt9::core::ChunkBufferBindingSnapshot> snapshots;
          (void)harness.queue().markChunkResourcesAndCaptureBufferBindings(
              entries, snapshots);
          if (snapshots.size() != named.size()) {
            fail("random lane: capture produced the wrong snapshot count");
          }
        }
        // CommitStampsCoverChunkSeq: no other step can run during this one, so
        // the ticket is exact and every live named record must carry it.
        for (const Handle handle : named) {
          if (harness.alive(handle) && harness.stampOf(handle) < ticketBefore) {
            fail("random lane: a committed record's stamp does not cover the "
                 "chunk's seq");
          }
        }
      } else if (tid == kPublisher) {
        if (opRoll < 70) {
          harness.slotAdvance();
        } else {
          // Last-ref drop from the worker actor. Gated by the pin premise, so
          // a pinned record is never destroyed.
          const Handle handle = state.handles[victim];
          const bool wasPinned = retainer.pinned(handle);
          if (retainer.dropLastRefAndDestroy(harness, handle)) {
            state.everDestroyed[victim] = true;
            if (wasPinned) {
              fail("random lane: the pin premise let a pinned record be "
                   "destroyed");
            }
          }
        }
      } else {
        if (opRoll < 75) {
          (void)harness.completionAdvance();
        } else {
          // An unpin can make a record destroyable on a later step; this is
          // what keeps the lane from being pin-frozen.
          retainer.unpin(state.handles[victim]);
        }
      }
      state.stepsRun += 1;
      checkRandomLaneInvariants(harness, state, "random lane");
    });
  }

  schedule.run();
  checkRandomLaneInvariants(harness, state, "random lane (final)");
  return digestRandomLane(harness, state);
}

void testSeededRandomSchedules() {
  constexpr u64 kSeedCount = 384;
  std::vector<u64> digests;
  digests.reserve(kSeedCount);

  for (u64 seed = 1; seed <= kSeedCount; ++seed) {
    digests.push_back(runRandomSchedule(seed * 0x9e3779b97f4a7c15ull));
  }

  // Determinism gate: same seed, same schedule, same result. Re-run a sample
  // in the same process and compare digests. The external three-repeat gate
  // covers process-to-process determinism.
  for (u64 seed = 1; seed <= kSeedCount; seed += 17) {
    const u64 again = runRandomSchedule(seed * 0x9e3779b97f4a7c15ull);
    checkEq(again, digests[static_cast<std::size_t>(seed - 1)],
            "random lane is not deterministic: same seed produced a different "
            "final state");
  }

  // A generator that collapsed to one trivial schedule would pass everything
  // above while testing nothing, so require real spread.
  std::vector<u64> unique = digests;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  check(unique.size() > kSeedCount / 8,
        "random lane produced too few distinct outcomes to be exploring the "
        "interleaving space");

  // Cross-PROCESS determinism is not something an in-process comparison can
  // establish, so fold every seed's digest into one line. Three repeat runs
  // printing the same corpus digest is the external gate; a differing line is
  // a determinism regression even when every assertion still passes.
  u64 corpus = 0xcbf29ce484222325ull;
  for (const u64 digest : digests) {
    corpus ^= digest;
    corpus *= 0x100000001b3ull;
  }
  std::cout << "  [random-lane] seeds=" << kSeedCount
            << " distinct_outcomes=" << unique.size()
            << " corpus_digest=0x" << std::hex << corpus << std::dec << '\n';
}

// ===========================================================================
// (c) Free-running memory-order lane + negative control
// ===========================================================================
//
// WHAT THE PRODUCTION PAIR ACTUALLY GUARANTEES — stated precisely, because
// overclaiming it would be the easiest mistake in this file.
//
// `dxmt9_command_queue.hpp:886-901` is explicit that the MARK PATH does not
// depend on the release: "The mark path does not depend on that, but the
// release keeps the store from being reordered ahead of the slot bookkeeping
// it seals." That is correct and worth restating as a finding: on today's
// code, weakening `markTicketAcquire()` to `relaxed` would not break the
// stamp protocol, because what the protocol needs from `nextSeqId_` is
// ATOMICITY (no torn or invented value) and MONOTONICITY, plus the frozen
// re-read whose ordering comes from `mutex_`, not from the atomic.
//
// The release/acquire pair is the guarantee a lock-free consumer of the
// PUBLISHED SLOT would rest on — the T2b capture move, T2c's
// `completedSeqId_` atomic, and T2d's reserve-copy-commit append all add
// exactly such a consumer, and gap.md lists all three as open. So this lane
// stands in for that consumer and pins the guarantee BEFORE something depends
// on it, rather than claiming a dependence that does not exist yet.
//
// Shape: the publisher writes a monotone ordinal into three bookkeeping slots
// and then release-stores the raised seq. A reader that acquire-loads seq S
// must therefore see every bookkeeping slot at ordinal >= S-1. Formulating it
// on a MONOTONE value is what makes the check immune to the publisher racing
// ahead mid-read — a later publish only makes the ordinal larger.
//
// The negative control is the identical lane with `relaxed` on both sides.
// Running both turns "this lane could catch a weakened order" from an
// assumption into a measurement.
//
// That measurement is LOAD-DEPENDENT, and the variance is large enough to
// matter. Measured on this arm64 host across six runs: 19,012 / 27,291 / 2 /
// 0 / 147 violations per 200k-400k observations, the low readings coming from
// runs where two other build trees were saturating the machine and the two
// threads were being descheduled rather than racing. So: a nonzero count
// proves the lane can see a weakened order; a ZERO count proves nothing
// either way. That is precisely why the count is printed and not asserted,
// and why `auditProductionMemoryOrders()` — which fails deterministically on
// any host under any load — is the guard this row actually rests on.

struct PairingReport {
  long long observations = 0;
  long long violations = 0;
};

constexpr int kPairingIterations = 400000;

PairingReport runReleaseAcquirePairing() {
  Harness harness;
  PairingReport report;
  std::atomic<long long> observations{0};
  std::atomic<long long> violations{0};
  std::atomic<bool> done{false};

  dxmt9::test::interleave::freeRunning({
      [&] {
        for (int i = 0; i < kPairingIterations; ++i) {
          harness.slotAdvance();  // REAL atomic, REAL mutex, release store
        }
        done.store(true, std::memory_order_release);
      },
      [&] {
        u64 lastSeen = 0;
        for (;;) {
          const bool finished = done.load(std::memory_order_acquire);
          // REAL acquire load, taken with no lock — exactly the production
          // lock-free read.
          const u64 seq = harness.queue().markTicketAcquire();
          if (seq > lastSeen) {
            lastSeen = seq;
            observations.fetch_add(1, std::memory_order_relaxed);
            if (harness.publishedOrdinalMin() + 1 < seq) {
              violations.fetch_add(1, std::memory_order_relaxed);
            }
          }
          if (finished) {
            break;
          }
        }
      },
  });

  report.observations = observations.load(std::memory_order_relaxed);
  report.violations = violations.load(std::memory_order_relaxed);
  return report;
}

// NEGATIVE CONTROL. Same lane, same invariant, `relaxed` on both sides. Not
// production code and never will be — its only job is to report whether this
// lane has any detection power on this host.
PairingReport runRelaxedPairingControl() {
  struct RelaxedPublisher {
    std::mutex mutex;
    std::atomic<u64> seq{1};
    std::array<std::atomic<u64>, 3> book{};

    u64 ordinalMin() const noexcept {
      u64 lowest = ~u64{0};
      for (const auto& slot : book) {
        lowest = std::min(lowest, slot.load(std::memory_order_relaxed));
      }
      return lowest;
    }
  };
  RelaxedPublisher publisher;
  PairingReport report;
  std::atomic<long long> observations{0};
  std::atomic<long long> violations{0};
  std::atomic<bool> done{false};

  dxmt9::test::interleave::freeRunning({
      [&] {
        for (int i = 0; i < kPairingIterations; ++i) {
          std::lock_guard<std::mutex> guard(publisher.mutex);
          const u64 current = publisher.seq.load(std::memory_order_relaxed);
          for (auto& slot : publisher.book) {
            slot.store(current, std::memory_order_relaxed);
          }
          // THE ONLY DIFFERENCE FROM THE REAL LANE.
          publisher.seq.store(current + 1, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
      },
      [&] {
        u64 lastSeen = 0;
        for (;;) {
          const bool finished = done.load(std::memory_order_acquire);
          // AND THE OTHER HALF OF THE DIFFERENCE.
          const u64 seq = publisher.seq.load(std::memory_order_relaxed);
          if (seq > lastSeen) {
            lastSeen = seq;
            observations.fetch_add(1, std::memory_order_relaxed);
            if (publisher.ordinalMin() + 1 < seq) {
              violations.fetch_add(1, std::memory_order_relaxed);
            }
          }
          if (finished) {
            break;
          }
        }
      },
  });

  report.observations = observations.load(std::memory_order_relaxed);
  report.violations = violations.load(std::memory_order_relaxed);
  return report;
}

void testFreeRunningPublishAcquirePairing() {
  const PairingReport real = runReleaseAcquirePairing();

  check(real.observations > 0,
        "pairing lane observed no publishes — the lane did not run");
  checkEq(real.violations, 0LL,
          "release/acquire pairing violated: a reader that saw the raised seq "
          "did not see the bookkeeping the release store orders before it");

  std::cout << "  [pairing] release/acquire: observations=" << real.observations
            << " violations=" << real.violations << '\n';

  const PairingReport control = runRelaxedPairingControl();
  check(control.observations > 0, "relaxed control did not run");

  std::cout << "  [pairing] relaxed control:  observations="
            << control.observations << " violations=" << control.violations
            << (control.violations > 0
                    ? "  <- lane HAS detection power on this host\n"
                    : "  <- lane showed NO detection power on this host; the "
                      "source-order audit and the TSan lane are the guards\n");

  // Deliberately NOT asserted either way. On x86-TSO the control is EXPECTED
  // to report zero violations, because TSO forbids the store-store and
  // load-load reordering the relaxed pair would need; asserting a nonzero
  // count would make this spec fail on a conforming host for a reason that
  // has nothing to do with dxmt9. The number is printed so a reader can weigh
  // this lane's evidence instead of assuming it.
}

}  // namespace

int main(int argc, char** argv) {
  // argv[1] is the repository source root, supplied by the meson test
  // definition, for the production memory-order source contract.
  const std::string sourceRoot = argc > 1 ? argv[1] : std::string{};

  try {
    if (sourceRoot.empty()) {
      fail("producer_interleaving_spec requires the source root as argv[1] so "
           "it can audit the production memory orders");
    }
    auditProductionMemoryOrders(sourceRoot);
    testMemoryOrderAuditDetectsAWeakening(sourceRoot);

    testPinDisciplineCounterexampleTraceIsSafeInProduction();
    testPinDisciplineRemovedReproducesTheFault();
    testRestampCoversAdvancedSlotSeqDecomposed();
    testRestampCoversAdvancedSlotSeqIntegrated();
    testAdvanceBeforeTicketReadNeedsNoRestamp();
    testConcurrentStampsAreMonotoneUnderTheArenaLock();
    testReclaimOfOtherRecordDoesNotDisturbTheMarkedOne();
    testCaptureIsStableAcrossUnrelatedErase();
    testSeededRandomSchedules();
    testFreeRunningPublishAcquirePairing();
  } catch (const TestFailure& failure) {
    std::cerr << "producer_interleaving_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "producer_interleaving_spec unexpected exception: "
              << ex.what() << '\n';
    return 1;
  }

  std::cout << "producer_interleaving_spec passed\n";
  return 0;
}
