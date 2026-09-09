#include "../../../src/dxmt9/dxmt9_upload_arena_lock_diag.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"

#include <cassert>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace dxmt9;

struct FakeClock {
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::time_point<FakeClock>;
  static inline duration ticks{0};
  static inline int reads = 0;
  static time_point now() noexcept { ++reads; return time_point(ticks); }
};

struct FakeMutex {
  bool held = false;
  bool fail = false;
  unsigned releases = 0;
  void lock() {
    assert(!held);
    FakeClock::ticks += FakeClock::duration(17);
    if (fail) throw std::runtime_error("lock failed");
    held = true;
  }
  void unlock() noexcept { assert(held); held = false; ++releases; }
  void lock_shared() { lock(); }
  void unlock_shared() noexcept { unlock(); }
};

UploadLockBucketSnapshot row(UploadLockSite site, UploadLockOperation op,
                            UploadLockMode mode) {
  UploadLockSnapshot snapshot;
  collectUploadArenaLockSnapshot(snapshot);
  assert(snapshot.skippedRows == 0);
  for (const auto& r : snapshot.buckets) {
    if (r.valid && r.nativeThreadId == uploadArenaNativeThreadId() &&
        r.site == site && r.operation == op && r.mode == mode) return r;
  }
  return {};
}

struct Record {
  bool destroyPending = false;
  std::uint64_t lastUsedSeqId = 0;
  int value = 0;
};

int main() {
  const bool enabled = uploadArenaLockSplitEnabled();
  assert(uploadArenaNativeThreadId() != 0);
  FakeMutex mutex;
  if (enabled) {
    {
      UploadLockScope<std::unique_lock<FakeMutex>, FakeClock> scope(
          mutex, UploadLockSite::QueueMutex, UploadLockOperation::UploadData,
          UploadLockMode::Exclusive);
      assert(mutex.held && FakeClock::reads == 2);
      FakeClock::ticks += FakeClock::duration(43);
    }
    auto r = row(UploadLockSite::QueueMutex, UploadLockOperation::UploadData,
                 UploadLockMode::Exclusive);
    assert(!mutex.held && mutex.releases == 1 && FakeClock::reads == 3);
    assert(r.count == 1 && r.waitNs == 17 && r.holdNs == 43 && r.maxHoldNs == 43);
    try {
      UploadLockScope<std::shared_lock<FakeMutex>, FakeClock> scope(
          mutex, UploadLockSite::BufferArena, UploadLockOperation::Inspect,
          UploadLockMode::Shared);
      FakeClock::ticks += FakeClock::duration(29);
      throw std::runtime_error("callback failed");
    } catch (const std::runtime_error&) {}
    r = row(UploadLockSite::BufferArena, UploadLockOperation::Inspect,
            UploadLockMode::Shared);
    assert(!mutex.held && mutex.releases == 2);
    assert(r.count == 1 && r.waitNs == 17 && r.holdNs == 29);
    mutex.fail = true;
    try {
      UploadLockScope<std::unique_lock<FakeMutex>, FakeClock> scope(
          mutex, UploadLockSite::QueueMutex, UploadLockOperation::UploadData,
          UploadLockMode::Exclusive);
      assert(false);
    } catch (const std::runtime_error&) {}
    assert(mutex.releases == 2);
    assert(row(UploadLockSite::QueueMutex, UploadLockOperation::UploadData,
               UploadLockMode::Exclusive).count == 1);
  }

  // Actual production arena wrappers, including early-return and throw paths.
  resources::detail::HandleArena<Record, resources::detail::ResourceHandleKind::Buffer> arena;
  auto handle = arena.insert({});
  assert(arena.update(handle.value, [](auto& r) { r.value = 7; }));
  assert(!arena.update(0, [](auto&) { assert(false); }));
  assert(arena.inspect(handle.value, [](const auto& r) { assert(r.value == 7); }));
  try {
    arena.update(handle.value, [](auto&) { throw std::runtime_error("arena"); });
    assert(false);
  } catch (const std::runtime_error&) {}
  assert(arena.update(handle.value, [](auto& r) { r.destroyPending = true; }));
  std::array<core::Handle, 2> handles{handle, core::Handle{0}};
  assert(arena.updateMany(std::span<const core::Handle>(handles), [](auto&) {}) == 1);
  arena.reclaimCompleted(0, [](auto&) {});
  assert(!arena.find(handle.value));

  UploadLockSnapshot snapshot;
  collectUploadArenaLockSnapshot(snapshot);
  if (!enabled) {
    assert(FakeClock::reads == 0 && snapshot.validCount == 0);
    assert(snapshot.dropped == 0 && snapshot.overflow == 0);
    return 0;
  }

  // Single-writer rows must remain coherent during concurrent snapshots.
  std::atomic<bool> start{false};
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (int i = 0; i < 10000; ++i)
        recordUploadArenaLock(UploadLockSite::QueueMutex,
            UploadLockOperation::UploadRange, UploadLockMode::Exclusive, 7, 11);
    });
  }
  start.store(true, std::memory_order_release);
  for (int i = 0; i < 2000; ++i) {
    collectUploadArenaLockSnapshot(snapshot);
    for (const auto& r : snapshot.buckets) {
      if (r.valid && r.operation == UploadLockOperation::UploadRange) {
        assert(r.waitNs == r.count * 7 && r.holdNs == r.count * 11);
      }
    }
  }
  for (auto& writer : writers) writer.join();
  collectUploadArenaLockSnapshot(snapshot);
  std::uint64_t total = 0;
  for (const auto& r : snapshot.buckets)
    if (r.valid && r.operation == UploadLockOperation::UploadRange) total += r.count;
  assert(total == 40000 && snapshot.overflow == 0 && snapshot.dropped == 0);

  // More distinct native TIDs than the bounded table: loss must be visible.
  for (int i = 0; i < 260; ++i) {
    std::thread([] {
      recordUploadArenaLock(UploadLockSite::QueueMutex,
          UploadLockOperation::UploadRange, UploadLockMode::Exclusive, 7, 11);
    }).join();
  }
  collectUploadArenaLockSnapshot(snapshot);
  assert(snapshot.validCount == UploadLockSnapshot::kBucketCount);
  assert(snapshot.overflow > 0 && snapshot.dropped == snapshot.overflow);
}
