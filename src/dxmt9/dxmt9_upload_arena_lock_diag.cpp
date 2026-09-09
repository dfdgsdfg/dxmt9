#include "dxmt9_upload_arena_lock_diag.hpp"

#include "util/log/log.hpp"

#include <array>
#include <cstdlib>

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace dxmt9 {
namespace {

constexpr std::size_t kBucketCount = 256;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);

struct Bucket {
  // 0 = unused, 1 = being published, 2 = immutable metadata published.
  std::atomic<std::uint8_t> state{0};
  std::uint64_t nativeThreadId = 0;
  UploadLockSite site = UploadLockSite::QueueMutex;
  UploadLockOperation operation = UploadLockOperation::UploadData;
  UploadLockMode mode = UploadLockMode::Exclusive;
  std::atomic<std::uint64_t> sequence{0};
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> waitNs{0};
  std::atomic<std::uint64_t> holdNs{0};
  std::atomic<std::uint64_t> maxHoldNs{0};
};

struct UploadLockTable {
  std::array<Bucket, kBucketCount> buckets{};
  std::atomic<std::uint64_t> overflow{0};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> inconsistentSnapshots{0};
};

// Constant initialization avoids a lazy-initialization guard while an outer
// upload queue lock may still be held. No runtime allocation is involved.
constinit UploadLockTable storage;
UploadLockTable& table() noexcept { return storage; }

std::uint64_t mix(std::uint64_t value) noexcept {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

std::uint64_t bucketHash(std::uint64_t tid, UploadLockSite site,
                         UploadLockOperation operation,
                         UploadLockMode mode) noexcept {
  std::uint64_t value = tid;
  value ^= static_cast<std::uint64_t>(site) * 0x9e3779b97f4a7c15ull;
  value ^= static_cast<std::uint64_t>(operation) * 0x517cc1b727220a95ull;
  value ^= static_cast<std::uint64_t>(mode) * 0x6eed0e9da4d94a4full;
  return mix(value);
}

bool sameKey(const Bucket& bucket, std::uint64_t tid, UploadLockSite site,
             UploadLockOperation operation, UploadLockMode mode) noexcept {
  return bucket.nativeThreadId == tid && bucket.site == site &&
      bucket.operation == operation && bucket.mode == mode;
}

const char* siteName(UploadLockSite site) noexcept {
  return site == UploadLockSite::QueueMutex ? "upload_queue_mutex"
                                             : "buffer_arena";
}

const char* operationName(UploadLockOperation operation) noexcept {
  switch (operation) {
    case UploadLockOperation::UploadData:
      return "upload_data";
    case UploadLockOperation::UploadRange:
      return "upload_range";
    case UploadLockOperation::Update:
      return "update";
    case UploadLockOperation::UpdateMany:
      return "update_many";
    case UploadLockOperation::Inspect:
      return "inspect";
    case UploadLockOperation::Reclaim:
      return "reclaim";
  }
  return "unknown";
}

const char* modeName(UploadLockMode mode) noexcept {
  return mode == UploadLockMode::Shared ? "shared" : "exclusive";
}

}  // namespace

std::uint64_t uploadArenaNativeThreadId() noexcept {
  static thread_local const std::uint64_t cached = [] {
#if defined(__APPLE__)
    std::uint64_t nativeId = 0;
    if (pthread_threadid_np(nullptr, &nativeId) == 0 && nativeId != 0) {
      return nativeId;
    }
#elif defined(__linux__)
    const auto nativeId = syscall(SYS_gettid);
    if (nativeId > 0) {
      return static_cast<std::uint64_t>(nativeId);
    }
#endif
    // A hash of std::thread::id is not a native identity and may collide.
    // Missing native identity is lost diagnostic coverage, never an alias.
    return std::uint64_t{0};
  }();
  return cached;
}

void recordUploadArenaLock(UploadLockSite site,
                           UploadLockOperation operation,
                           UploadLockMode mode,
                           std::uint64_t waitNs,
                           std::uint64_t holdNs) noexcept {
  auto& t = table();
  const auto tid = uploadArenaNativeThreadId();
  if (tid == 0) {
    t.dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto hash = bucketHash(tid, site, operation, mode);
  const auto start = static_cast<std::size_t>(hash % kBucketCount);
  for (std::size_t probe = 0; probe < kBucketCount; ++probe) {
    auto& bucket = t.buckets[(start + probe) % kBucketCount];
    const auto state = bucket.state.load(std::memory_order_acquire);
    if (state == 1) {
      // Never wait behind another thread publishing metadata while this
      // observation is still nested in an outer product lock.
      continue;
    }
    if (state == 2) {
      if (!sameKey(bucket, tid, site, operation, mode)) {
        continue;
      }
    } else {
      std::uint8_t expected = 0;
      if (!bucket.state.compare_exchange_strong(
              expected, 1, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        continue;
      }
      bucket.nativeThreadId = tid;
      bucket.site = site;
      bucket.operation = operation;
      bucket.mode = mode;
      bucket.state.store(2, std::memory_order_release);
    }
    bucket.sequence.fetch_add(1, std::memory_order_acq_rel);
    // If a reader observes any following payload store, its acquire fence
    // orders the final sequence check after this odd publication.
    std::atomic_thread_fence(std::memory_order_release);
    bucket.count.fetch_add(1, std::memory_order_relaxed);
    bucket.waitNs.fetch_add(waitNs, std::memory_order_relaxed);
    bucket.holdNs.fetch_add(holdNs, std::memory_order_relaxed);
    // The native-TID key has one writer. Readers never mutate these fields.
    const auto previous = bucket.maxHoldNs.load(std::memory_order_relaxed);
    if (previous < holdNs) {
      bucket.maxHoldNs.store(holdNs, std::memory_order_relaxed);
    }
    bucket.sequence.fetch_add(1, std::memory_order_release);
    return;
  }
  t.overflow.fetch_add(1, std::memory_order_relaxed);
  t.dropped.fetch_add(1, std::memory_order_relaxed);
}

void collectUploadArenaLockSnapshot(UploadLockSnapshot& out) noexcept {
  out = {};
  auto& t = table();
  for (std::size_t index = 0; index < t.buckets.size(); ++index) {
    const auto& bucket = t.buckets[index];
    if (bucket.state.load(std::memory_order_acquire) != 2) {
      continue;
    }
    const auto sequenceBefore = bucket.sequence.load(std::memory_order_acquire);
    if ((sequenceBefore & 1u) != 0u) {
      t.inconsistentSnapshots.fetch_add(1, std::memory_order_relaxed);
      ++out.skippedRows;
      continue;
    }
    const auto count = bucket.count.load(std::memory_order_relaxed);
    const auto waitNs = bucket.waitNs.load(std::memory_order_relaxed);
    const auto holdNs = bucket.holdNs.load(std::memory_order_relaxed);
    const auto maxHoldNs = bucket.maxHoldNs.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto sequenceAfter = bucket.sequence.load(std::memory_order_acquire);
    if (sequenceBefore != sequenceAfter) {
      t.inconsistentSnapshots.fetch_add(1, std::memory_order_relaxed);
      ++out.skippedRows;
      continue;
    }
    auto& row = out.buckets[index];
    row.valid = true;
    row.nativeThreadId = bucket.nativeThreadId;
    row.site = bucket.site;
    row.operation = bucket.operation;
    row.mode = bucket.mode;
    row.count = count;
    row.waitNs = waitNs;
    row.holdNs = holdNs;
    row.maxHoldNs = maxHoldNs;
    ++out.validCount;
  }
  out.overflow = t.overflow.load(std::memory_order_relaxed);
  out.dropped = t.dropped.load(std::memory_order_relaxed);
  out.inconsistent = t.inconsistentSnapshots.load(std::memory_order_relaxed);
}

void snapshotUploadArenaLockSites(std::uint64_t snapshotOrdinal) noexcept {
  if (!uploadArenaLockSplitEnabled()) {
    return;
  }
  UploadLockSnapshot snapshot;
  collectUploadArenaLockSnapshot(snapshot);
  for (const auto& bucket : snapshot.buckets) {
    if (!bucket.valid) {
      continue;
    }
    util::logf(util::LogLevel::Info, "dxmt9-upload-arena-lock",
               "snapshot_ordinal=%llu native_tid=0x%llx site=%s operation=%s "
               "mode=%s buffer_kind=buffer count=%llu wait_ns=%llu "
               "hold_ns=%llu max_hold_ns=%llu overflow=%llu dropped=%llu "
               "inconsistent=%llu skipped_rows=%llu",
               static_cast<unsigned long long>(snapshotOrdinal),
               static_cast<unsigned long long>(bucket.nativeThreadId),
               siteName(bucket.site), operationName(bucket.operation),
               modeName(bucket.mode),
               static_cast<unsigned long long>(bucket.count),
               static_cast<unsigned long long>(bucket.waitNs),
               static_cast<unsigned long long>(bucket.holdNs),
               static_cast<unsigned long long>(bucket.maxHoldNs),
               static_cast<unsigned long long>(snapshot.overflow),
               static_cast<unsigned long long>(snapshot.dropped),
               static_cast<unsigned long long>(snapshot.inconsistent),
               static_cast<unsigned long long>(snapshot.skippedRows));
  }
  util::logf(util::LogLevel::Info, "dxmt9-upload-arena-lock",
             "snapshot_ordinal=%llu site=<table> operation=<aggregate> "
             "mode=<aggregate> buffer_kind=buffer count=0 wait_ns=0 hold_ns=0 "
             "max_hold_ns=0 overflow=%llu dropped=%llu inconsistent=%llu "
             "skipped_rows=%llu endpoint=1",
             static_cast<unsigned long long>(snapshotOrdinal),
             static_cast<unsigned long long>(snapshot.overflow),
             static_cast<unsigned long long>(snapshot.dropped),
             static_cast<unsigned long long>(snapshot.inconsistent),
             static_cast<unsigned long long>(snapshot.skippedRows));
}

}  // namespace dxmt9
