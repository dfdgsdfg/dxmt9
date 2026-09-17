#pragma once

#include "dxmt9/core.hpp"
#include "util/log/log.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>

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
  struct Counts {
    u64 polls = 0, flush = 0, ok = 0, pending = 0, error = 0, nanos = 0;
  };
  std::mutex mutex;
  std::array<Counts, 5> types{};
  u64 issues = 0, polls = 0, events = 0, pollEvents = 0, reports = 0;

  Ledger() {
    // Initialize the logger before this static finishes construction, so its
    // stream/mutex outlive the final report (including quiet native fixtures).
    util::logf(util::LogLevel::Info, "dxmt9-query-observer",
               "enabled: waterlines are post-call samples; elapsed scope is core-only");
  }

  void report() {
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
  }
  ~Ledger() { report(); }
};

inline Ledger& ledger() { static Ledger value; return value; }

inline void event(const Query& q, const char* action, u32 flags, HResult result,
                  u64 nanos, query::Progress progress, bool authoritative) {
  const auto fence = q.backendFence();
  util::logf(util::LogLevel::Info, "dxmt9-query-event",
      "action=%s type=%u query=%llu issue=%llu epoch=%llu core_seq=%llu flags=%u hr=%d "
      "authoritative=%u queue=%llu fence_queue=%llu fence=%llu published=%llu completed=%llu "
      "init_fence=%llu init_published=%llu init_completed=%llu failed=%u elapsed_ns=%llu",
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
      unsigned(progress.failed), (unsigned long long)nanos);
}

inline void issue(const Query& q, bool begin, bool async, HResult result,
                  query::Progress progress) {
  auto& l = ledger();
  std::lock_guard lock(l.mutex);
  ++l.issues;
  if (l.issues <= 128) {
    ++l.events;
    event(q, begin ? "begin" : "end", 0, result, 0, progress,
          async && q.backendFence().valid());
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
}

}  // namespace dxmt9::core::querydiag
