#pragma once

// Deterministic scripted scheduler for multi-threaded protocol specs.
//
// Test-only scaffolding (this header lives under `tests/`, never `src/`, and
// production code neither includes it nor knows it exists). It exists so a
// concurrency obligation can be written as an ORDERED SCRIPT over real
// threads instead of a sleep-and-hope stress loop.
//
// ---------------------------------------------------------------------------
// WHAT IT GUARANTEES, AND WHAT IT DELIBERATELY DOES NOT
// ---------------------------------------------------------------------------
// A `Schedule` is a list of steps; each step names the thread that runs it.
// Exactly one step is runnable at a time — every thread spins on a shared
// cursor until the cursor names it. So the *interleaving of protocol steps* is
// exactly the script, on real `std::thread`s, running real production code.
// No sleeps, no timing dependence for correctness.
//
// The cursor is an `std::atomic<std::size_t>` advanced with `release` and read
// with `acquire`. That is a synchronization edge between every consecutive
// pair of steps, which means **a scripted schedule cannot observe a weakened
// memory order inside the code under test** — the scheduler's own edge would
// supply the missing happens-before. Scripted schedules therefore prove
// *protocol* order (which statement runs before which), not *memory* order.
// Memory order is the job of `freeRunning()` below, of a ThreadSanitizer
// build, and of a source-text contract assertion on the production orders.
// Both facts are stated where the specs use them.
//
// Three step kinds:
//
//   sync(tid, op)      Run `op` on thread `tid`; the cursor advances after it
//                      returns. The normal case.
//   async(tid, op)     Advance the cursor FIRST, then run `op` on thread
//                      `tid`. For an op that is EXPECTED to block on a lock
//                      another thread is holding across steps — a sync step
//                      there would deadlock the cursor.
//   joinAsync(observerTid, targetTid)
//                      A step on `observerTid` that spins until `targetTid`'s
//                      pending async op has returned.
//
// `spinUntil` is the only wait primitive; it is bounded by a wall-clock
// watchdog whose sole job is to turn a malformed script into a test failure
// instead of a hung CI job. Nothing about correctness depends on the clock.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dxmt9::test::interleave {

// Thrown when a script stalls (a step whose turn never comes, or a
// `spinUntil` predicate that never becomes true). Always a harness bug or a
// genuine production deadlock, never an expected outcome.
struct ScheduleStall : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Watchdog budget. Generous: every schedule in the specs completes in
// microseconds, so a hit means "stalled", not "slow machine".
inline constexpr std::chrono::seconds kStallBudget{20};

class Schedule {
 public:
  using Op = std::function<void()>;

  explicit Schedule(int threadCount)
      : programs_(static_cast<std::size_t>(threadCount)),
        asyncDone_(static_cast<std::size_t>(threadCount)) {
    for (auto& flag : asyncDone_) {
      flag.store(true, std::memory_order_relaxed);
    }
  }

  Schedule(const Schedule&) = delete;
  Schedule& operator=(const Schedule&) = delete;

  int threadCount() const noexcept {
    return static_cast<int>(programs_.size());
  }

  std::size_t stepCount() const noexcept { return order_.size(); }

  // Run `op` on thread `tid` when the script reaches this step; the cursor
  // advances only after `op` returns.
  Schedule& sync(int tid, Op op) {
    return push(tid, Entry::Kind::Sync, std::move(op), 0);
  }

  // Advance the cursor, THEN run `op` on thread `tid`. Use only for an op
  // that is expected to block on a lock held across script steps.
  Schedule& async(int tid, Op op) {
    return push(tid, Entry::Kind::Async, std::move(op), 0);
  }

  // A step on `observerTid` that waits for `targetTid`'s async op to return.
  Schedule& joinAsync(int observerTid, int targetTid) {
    return push(observerTid, Entry::Kind::JoinAsync, {}, targetTid);
  }

  // Spawn the threads, run the script to completion, join. Rethrows the first
  // exception any step threw, so a `check(...)` failure inside an op surfaces
  // as a normal test failure on the main thread.
  void run() {
    std::vector<std::thread> threads;
    threads.reserve(programs_.size());
    for (std::size_t t = 0; t < programs_.size(); ++t) {
      threads.emplace_back([this, t] { runThread(static_cast<int>(t)); });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    if (failure_) {
      std::rethrow_exception(failure_);
    }
    if (aborted_.load(std::memory_order_acquire)) {
      throw ScheduleStall(stallMessage_);
    }
  }

  // Bounded spin on a predicate. The ONLY wait primitive available to an op.
  // Deterministic in outcome: the caller must arrange that the predicate is
  // guaranteed to become true (usually because the only code that can run
  // next is the code that makes it true).
  template <typename Predicate>
  static void spinUntil(Predicate&& predicate, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + kStallBudget;
    while (!predicate()) {
      if (std::chrono::steady_clock::now() > deadline) {
        throw ScheduleStall(std::string("spinUntil timed out: ") + what);
      }
      std::this_thread::yield();
    }
  }

 private:
  struct Entry {
    enum class Kind { Sync, Async, JoinAsync };
    Kind kind = Kind::Sync;
    Op op;
    int target = 0;
  };

  Schedule& push(int tid, Entry::Kind kind, Op op, int target) {
    programs_[static_cast<std::size_t>(tid)].push_back(
        Entry{kind, std::move(op), target});
    order_.push_back(tid);
    return *this;
  }

  // Wait until the cursor names `tid`. Returns false once the script has been
  // aborted, so every thread can unwind instead of spinning forever.
  bool awaitTurn(int tid) {
    const auto deadline = std::chrono::steady_clock::now() + kStallBudget;
    for (;;) {
      if (aborted_.load(std::memory_order_acquire)) {
        return false;
      }
      const std::size_t cursor = cursor_.load(std::memory_order_acquire);
      if (cursor >= order_.size()) {
        return false;
      }
      if (order_[cursor] == tid) {
        return true;
      }
      if (std::chrono::steady_clock::now() > deadline) {
        std::ostringstream out;
        out << "schedule stalled at step " << cursor << " (owned by thread "
            << order_[cursor] << "); thread " << tid << " was still waiting";
        abortWith(out.str());
        return false;
      }
      std::this_thread::yield();
    }
  }

  void advance() { cursor_.fetch_add(1, std::memory_order_release); }

  // `diagnosticMutex_` guards `stallMessage_` / `failure_`. Only one step runs
  // at a time EXCEPT while an async op is in flight, so a sync step and an
  // async step genuinely can fault concurrently; without the lock that would
  // be a race in the harness itself, which a ThreadSanitizer run of a failing
  // spec would then report instead of the real finding.
  void abortWith(std::string message) {
    bool expected = false;
    if (aborted_.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
      std::lock_guard<std::mutex> guard(diagnosticMutex_);
      stallMessage_ = std::move(message);
    }
  }

  void captureFailure() {
    {
      std::lock_guard<std::mutex> guard(diagnosticMutex_);
      if (!failure_) {
        failure_ = std::current_exception();
      }
    }
    abortWith("a scheduled step threw");
  }

  void runThread(int tid) {
    auto& program = programs_[static_cast<std::size_t>(tid)];
    for (auto& entry : program) {
      if (!awaitTurn(tid)) {
        return;
      }
      switch (entry.kind) {
        case Entry::Kind::Sync:
          try {
            entry.op();
          } catch (...) {
            captureFailure();
            advance();
            return;
          }
          advance();
          break;

        case Entry::Kind::Async: {
          asyncDone_[static_cast<std::size_t>(tid)].store(
              false, std::memory_order_release);
          // Cursor first: the op is allowed — expected — to block.
          advance();
          try {
            entry.op();
          } catch (...) {
            captureFailure();
          }
          asyncDone_[static_cast<std::size_t>(tid)].store(
              true, std::memory_order_release);
          break;
        }

        case Entry::Kind::JoinAsync: {
          const auto target = static_cast<std::size_t>(entry.target);
          const auto deadline = std::chrono::steady_clock::now() + kStallBudget;
          while (!asyncDone_[target].load(std::memory_order_acquire)) {
            if (aborted_.load(std::memory_order_acquire)) {
              return;
            }
            if (std::chrono::steady_clock::now() > deadline) {
              std::ostringstream out;
              out << "joinAsync timed out waiting for thread " << entry.target;
              abortWith(out.str());
              advance();
              return;
            }
            std::this_thread::yield();
          }
          advance();
          break;
        }
      }
    }
  }

  std::vector<std::vector<Entry>> programs_;
  std::vector<int> order_;
  std::vector<std::atomic<bool>> asyncDone_;
  std::atomic<std::size_t> cursor_{0};
  std::atomic<bool> aborted_{false};
  std::mutex diagnosticMutex_;
  std::string stallMessage_;
  std::exception_ptr failure_;
};

// Free-running (UNSCRIPTED) lane. Runs `bodies` concurrently on real threads
// with no cursor between them, so no scheduler-supplied happens-before masks
// the code under test. This is the lane that can, in principle, observe a
// weakened memory order — see the spec's negative control for whether it
// actually does on a given host architecture.
inline void freeRunning(std::vector<std::function<void()>> bodies) {
  std::vector<std::thread> threads;
  threads.reserve(bodies.size());
  std::atomic<bool> go{false};
  for (auto& body : bodies) {
    threads.emplace_back([&go, fn = std::move(body)] {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      fn();
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& thread : threads) {
    thread.join();
  }
}

// Deterministic 64-bit LCG. Fixed seed in, fixed sequence out, on every host
// and every standard library — which `std::mt19937` plus a distribution does
// not guarantee across implementations.
class DeterministicRandom {
 public:
  explicit DeterministicRandom(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    std::uint64_t value = state_;
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return value;
  }

  // Uniform-enough for schedule selection; the requirement is reproducibility,
  // not statistical quality.
  std::size_t below(std::size_t bound) noexcept {
    return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
  }

 private:
  std::uint64_t state_;
};

}  // namespace dxmt9::test::interleave
