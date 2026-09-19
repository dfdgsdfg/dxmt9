#pragma once
#include "dxmt9_query_timeline_observer.hpp"
#include "dxmt9_perf_counters.hpp"
#include "util/log/log.hpp"
#include <array>
#include <cstdlib>
#include <mutex>

namespace dxmt9::query::splitdiag {
inline bool enabled() noexcept {
  static const bool value = [] {
    const char* s = std::getenv("DXMT9_QUERY_SPLIT_OBSERVER");
    return s && s[0] && s[0] != '0';
  }();
  return value;
}
enum class Kind { EmptyEnd, EagerEnd, DeferredNew, DeferredShared, FlushPublish,
                  EagerCapacity, MarkerBound, Publication, CommandBuffer,
                  ScopeNonempty, ScopeEmpty, Count };
inline constexpr std::array names{"empty_end", "eager_end", "deferred_new",
    "deferred_shared", "flush_publish", "eager_capacity", "marker_bound",
    "publication", "command_buffer", "scope_nonempty", "scope_empty"};
struct Ledger {
  std::mutex mutex;
  std::array<std::uint64_t, names.size()> count{}, extent{};
  std::array<std::uint64_t, 16> publicationReasons{};
  std::uint64_t events = 0;
  std::uint64_t presents = 0;
  Ledger() {
    util::logf(util::LogLevel::Info, "dxmt9-query-split",
        "enabled: extent=commands for publication, sources for CB; no timing attribution");
  }
  ~Ledger() {
    for (std::size_t i = 0; i != names.size(); ++i)
      util::logf(util::LogLevel::Info, "dxmt9-query-split-summary",
          "kind=%s count=%llu extent=%llu", names[i],
          (unsigned long long)count[i], (unsigned long long)extent[i]);
    for (std::size_t i = 0; i != publicationReasons.size(); ++i)
      if (publicationReasons[i])
        util::logf(util::LogLevel::Info, "dxmt9-query-split-reason",
            "reason=%zu count=%llu", i, (unsigned long long)publicationReasons[i]);
  }
};
// No clock, diagnostic lock, storage initialization or payload walk while off.
inline void record(Kind kind, std::uint64_t queue, std::uint64_t sequence,
                   std::uint64_t extent, unsigned reason) {
  if (!enabled()) return;
  static Ledger ledger;
  std::lock_guard lock(ledger.mutex);
  const auto index = static_cast<std::size_t>(kind);
  ++ledger.count[index];
  ledger.extent[index] += extent;
  if (kind == Kind::Publication && reason < ledger.publicationReasons.size())
    ++ledger.publicationReasons[reason];
  // Cumulative snapshots survive normal or abnormal teardown and retain late
  // intervals after the small event store fills. These are publication-clock
  // populations, not encode-frame IDs or exclusive command-buffer causes.
  if (kind == Kind::Publication &&
      reason == unsigned(perf::ChunkPublishReason::Present) &&
      ++ledger.presents <= 4096) {
    for (std::size_t i = 0; i < names.size(); ++i)
      util::logf(util::LogLevel::Info, "dxmt9-query-split-present",
          "present=%llu queue=%llu seq=%llu kind=%s count=%llu extent=%llu",
          (unsigned long long)ledger.presents, (unsigned long long)queue,
          (unsigned long long)sequence, names[i],
          (unsigned long long)ledger.count[i],
          (unsigned long long)ledger.extent[i]);
    for (std::size_t i = 0; i < ledger.publicationReasons.size(); ++i)
      if (ledger.publicationReasons[i])
        util::logf(util::LogLevel::Info, "dxmt9-query-split-present-reason",
            "present=%llu queue=%llu seq=%llu reason=%zu count=%llu",
            (unsigned long long)ledger.presents, (unsigned long long)queue,
            (unsigned long long)sequence, i,
            (unsigned long long)ledger.publicationReasons[i]);
  }
  if (ledger.events++ < 128)
    util::logf(util::LogLevel::Info, "dxmt9-query-split-event",
        "kind=%s queue=%llu seq=%llu extent=%llu reason=%u", names[index],
        (unsigned long long)queue, (unsigned long long)sequence,
        (unsigned long long)extent, reason);
}

inline void note(Kind kind, std::uint64_t queue, std::uint64_t sequence,
                 std::uint64_t extent = 0, unsigned reason = 0) {
  if (kind == Kind::Publication)
    timeline_diag::publication(queue, sequence);
  record(kind, queue, sequence, extent, reason);
}

inline void notePublication(std::uint64_t queue, std::uint64_t sequence,
                            std::uint64_t extent, unsigned reason,
                            timeline_diag::MarkerWitness marker) {
  timeline_diag::publication(queue, sequence, marker);
  record(Kind::Publication, queue, sequence, extent, reason);
}
} // namespace dxmt9::query::splitdiag
