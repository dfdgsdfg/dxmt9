#pragma once

#include "dxmt9/core.hpp"
#include "../dxmt9/dxmt9_query_timeline_observer.hpp"
#include "util/log/log.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>

namespace dxmt9::core::querydiag {

inline bool enabled() noexcept {
  static const bool value = [] {
    const char* setting = std::getenv("DXMT9_QUERY_OBSERVER");
    return setting && setting[0] && setting[0] != '0';
  }();
  return value;
}

// Cold fixed storage, entered only behind enabled(). No query pointers are
// retained; queue incarnation + object identity + issue ordinal qualify events.
struct Ledger {
  static constexpr std::size_t kEndCapacity = 128;

  // This is a value-only receipt captured on the thread that owns the core
  // Query.  It is deliberately separate from Query: replay-drain diagnostics
  // must never inspect mutable issue/fence state after this callback returns.
  struct EndRecord {
    bool valid = false;
    u64 identity = 0;
    u64 issue = 0;
    u64 epoch = 0;
    u64 backendQueue = 0;
    u64 backendSequence = 0;
    u64 rawOrdinal = 0;
    u64 ordinal = 0;
  };

  struct EndStats {
    u64 evictions = 0;
    u64 invalidations = 0;
  };

  struct Counts {
    u64 polls = 0, flush = 0, ok = 0, pending = 0, error = 0, nanos = 0;
  };
  mutable std::mutex mutex;
  std::array<Counts, 5> types{};
  u64 issues = 0, polls = 0, events = 0, pollEvents = 0, reports = 0;
  std::array<EndRecord, kEndCapacity> ends{};
  u64 nextEndOrdinal = 0;
  u64 endEvictions = 0;
  u64 endInvalidations = 0;
  bool logging_ = true;

  explicit Ledger(bool logging = true) : logging_(logging) {
    // Initialize the logger before this static finishes construction, so its
    // stream/mutex outlive the final report (including quiet native fixtures).
    if (logging_) {
      util::logf(util::LogLevel::Info, "dxmt9-query-observer",
                 "enabled: waterlines are post-call samples; elapsed scope is core-only");
    }
  }

  void report() {
    if (!logging_) return;
    for (std::size_t type = 0; type < types.size(); ++type) {
      const auto& c = types[type];
      util::logf(util::LogLevel::Info, "dxmt9-query-summary",
          "type=%zu polls=%llu flush=%llu ok=%llu pending=%llu error=%llu "
          "elapsed_ms=%.3f issues_total=%llu events=%llu omitted=%llu reports=%llu",
          type, (unsigned long long)c.polls, (unsigned long long)c.flush,
          (unsigned long long)c.ok, (unsigned long long)c.pending,
          (unsigned long long)c.error, double(c.nanos) / 1e6,
          (unsigned long long)issues, (unsigned long long)events,
          (unsigned long long)(issues + polls - events),
          (unsigned long long)reports);
    }
    util::logf(util::LogLevel::Info, "dxmt9-query-end-map",
               "capacity=%zu evictions=%llu invalidations=%llu",
               kEndCapacity, (unsigned long long)endEvictions,
               (unsigned long long)endInvalidations);
  }
  ~Ledger() { report(); }

  void invalidateEnd(u64 identity) noexcept {
    for (auto& entry : ends) {
      if (entry.valid && entry.identity == identity) {
        entry.valid = false;
        ++endInvalidations;
      }
    }
  }

  void recordEnd(u64 identity, u64 issue, u64 epoch, u64 backendQueue,
                 u64 backendSequence, u64 rawOrdinal) noexcept {
    EndRecord* destination = nullptr;
    for (auto& entry : ends) {
      if (entry.valid && entry.identity == identity) {
        destination = &entry;
        break;
      }
      if (!destination && !entry.valid) destination = &entry;
    }
    if (!destination) {
      destination = &ends[0];
      for (auto& entry : ends)
        if (entry.ordinal < destination->ordinal) destination = &entry;
      ++endEvictions;
    }
    *destination = EndRecord{true, identity, issue, epoch, backendQueue,
                             backendSequence, rawOrdinal, ++nextEndOrdinal};
  }

  std::optional<EndRecord> endSnapshot(u64 identity) const noexcept {
    std::lock_guard lock(mutex);
    for (const auto& entry : ends) {
      if (entry.valid && entry.identity == identity) return entry;
    }
    return std::nullopt;
  }

  EndStats endStats() const noexcept {
    std::lock_guard lock(mutex);
    return {endEvictions, endInvalidations};
  }
};

inline Ledger& ledger() { static Ledger value; return value; }

inline thread_local u64 replayRawOrdinalTls = 0;

inline u64 replayRawOrdinal() noexcept { return replayRawOrdinalTls; }

class ScopedReplayRawOrdinal final {
 public:
  explicit ScopedReplayRawOrdinal(u64 ordinal) noexcept
      : previous_(replayRawOrdinalTls) { replayRawOrdinalTls = ordinal; }
  ~ScopedReplayRawOrdinal() { replayRawOrdinalTls = previous_; }
  ScopedReplayRawOrdinal(const ScopedReplayRawOrdinal&) = delete;
  ScopedReplayRawOrdinal& operator=(const ScopedReplayRawOrdinal&) = delete;

 private:
  u64 previous_ = 0;
};

inline ::dxmt9::query::timeline_diag::Identity timelineIdentity(const Query& q) noexcept {
  const auto fence = q.backendFence();
  return {fence.queue, q.identity(), q.issueOrdinal(), q.issueDeviceEpoch()};
}

inline ::dxmt9::query::timeline_diag::MarkerWitness timelineMarker(const Query& q) noexcept {
  const auto owner = q.backendMarkerOwner();
  if (!owner) return {};
  return {owner->queue, owner->index, owner->generation,
          owner->storageGeneration};
}

inline void event(const Query& q, const char* action, u32 flags, HResult result,
                  u64 nanos, query::Progress progress, bool authoritative) {
  const auto fence = q.backendFence();
  util::logf(util::LogLevel::Info, "dxmt9-query-event",
      "action=%s type=%u query=%llu issue=%llu epoch=%llu core_seq=%llu flags=%u hr=%d "
      "authoritative=%u queue=%llu fence_queue=%llu fence=%llu published=%llu completed=%llu "
      "init_fence=%llu init_published=%llu init_completed=%llu failed=%u elapsed_ns=%llu deferred=%u bound=%u",
      action, unsigned(q.type()), (unsigned long long)q.identity(),
      (unsigned long long)q.issueOrdinal(), (unsigned long long)q.issueDeviceEpoch(),
      (unsigned long long)q.issuedSequenceId(),
      flags, int(result), unsigned(authoritative),
      (unsigned long long)progress.queue, (unsigned long long)fence.queue,
      (unsigned long long)fence.sequence,
      (unsigned long long)progress.published, (unsigned long long)progress.completed,
      (unsigned long long)fence.initializer,
      (unsigned long long)progress.initializerPublished,
      (unsigned long long)progress.initializerCompleted,
      unsigned(progress.failed), (unsigned long long)nanos,
      unsigned(q.deferredBackendFence()),
      unsigned(!q.deferredBackendFence() || fence.sequence != 0));
}

inline void issue(const Query& q, bool begin, bool async, HResult result,
                  query::Progress progress) {
  auto& l = ledger();
  std::lock_guard lock(l.mutex);
  const auto fence = q.backendFence();
  const auto identity = q.identity();
  // BEGIN and every failed issue invalidate the previous successful END.  A
  // later drain may therefore report that its before-snapshot was stale,
  // without treating a post-drain END as authority for the current issue.
  if (begin || result != S_OK) {
    l.invalidateEnd(identity);
  }
  if (!begin && result == S_OK) {
    l.recordEnd(identity, q.issueOrdinal(), q.issueDeviceEpoch(), fence.queue,
                fence.sequence, replayRawOrdinal());
  }
  ++l.issues;
  if (l.issues <= 128) {
    ++l.events;
    event(q, begin ? "begin" : "end", 0, result, 0, progress,
          async && q.backendFence().valid());
  }
  if (!begin) {
    ::dxmt9::query::timeline_diag::end(
        {fence.queue, identity, q.issueOrdinal(), q.issueDeviceEpoch()},
        static_cast<u32>(q.type()), fence.sequence, timelineMarker(q),
        fence.initializer);
  }
}

inline void poll(const Query& q, u32 flags, HResult result, u64 nanos,
                 query::Progress progress) {
  auto& l = ledger();
  std::lock_guard lock(l.mutex);
  const auto type = static_cast<std::size_t>(q.type());
  if (type >= l.types.size()) return;
  auto& c = l.types[type];
  ++c.polls;
  c.flush += (flags & QUERY_GETDATA_FLUSH) != 0;
  c.ok += result == S_OK;
  c.pending += result == S_FALSE;
  c.error += result != S_OK && result != S_FALSE;
  c.nanos += nanos;
  ++l.polls;
  if (l.pollEvents < 128 || (l.pollEvents < 384 && l.polls % 4096 == 0)) {
    ++l.events;
    ++l.pollEvents;
    event(q, "poll", flags, result, nanos, progress, q.backendFence().valid());
  }
  if (l.reports < 64 && (l.polls == 1 || l.polls % 4096 == 0)) {
    ++l.reports;
    l.report();
  }
  ::dxmt9::query::timeline_diag::poll(timelineIdentity(q), static_cast<int>(result),
                             result == S_OK, q.resolvedValue());
}

}  // namespace dxmt9::core::querydiag
