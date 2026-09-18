#pragma once

#include "core_query_observer.hpp"
#include "util/log/log.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace dxmt9::d3d9::query_drain_diag {

using EndRecord = ::dxmt9::core::querydiag::Ledger::EndRecord;

// This view mirrors only the queue facts useful for attribution. In
// particular, a reserved FIFO slot has no exposed ordinal and remains
// explicitly unknown.
struct QueueSnapshot {
  std::size_t queuedCount = 0;
  std::uint64_t frontOrdinal = 0;
  std::uint64_t backOrdinal = 0;
  bool frontOrdinalKnown = false;
  bool backOrdinalKnown = false;
  bool frontPlaceholder = false;
  bool backPlaceholder = false;
  bool unknownOrdinal = false;
  bool inFlight = false;
};

enum class ResultClass : std::uint8_t {
  NoKnownEnd,
  MissingBeforeAfterEnd,
  EndChanged,
  EndKnownNoRawOrdinal,
  QueuedTailBeforeDrain,
  QueuedTailAfterDrainOnly,
  KnownEndNoQueuedTail,
  FailedDrain,
};

struct Detail {
  std::uint64_t queryIdentity = 0;
  ResultClass result = ResultClass::NoKnownEnd;
  EndRecord beforeEnd{};
  EndRecord afterEnd{};
  QueueSnapshot beforeQueue{};
  QueueSnapshot afterQueue{};
  std::uint64_t drainNanoseconds = 0;
  bool drained = false;
  bool queuedTailBeforeDrain = false;
  bool queuedTailAfterDrainOnly = false;
  bool unknownInflight = false;
};

struct Totals {
  std::uint64_t drains = 0;
  std::uint64_t failedDrains = 0;
  std::uint64_t omittedDetails = 0;
  std::uint64_t drainNanoseconds = 0;
  std::uint64_t beforeQueueBusy = 0;
  std::uint64_t beforeQueueEmpty = 0;
  std::uint64_t beforeQueueBusyNanoseconds = 0;
  std::uint64_t beforeQueueEmptyNanoseconds = 0;
  std::uint64_t queuedTailBeforeDrain = 0;
  std::uint64_t queuedTailBeforeDrainNanoseconds = 0;
  std::uint64_t queuedTailAfterDrainOnly = 0;
  std::uint64_t queuedTailAfterDrainOnlyNanoseconds = 0;
  std::uint64_t unknownInflight = 0;
  std::uint64_t unknownInflightNanoseconds = 0;
  std::array<std::uint64_t, 8> classes{};
  std::array<std::uint64_t, 8> classDrainNanoseconds{};
};

inline bool enabled() noexcept {
  static const bool value = [] {
    const char* setting = std::getenv("DXMT9_QUERY_OBSERVER");
    return setting && setting[0] && setting[0] != '0';
  }();
  return value;
}

inline bool sameEnd(const EndRecord& lhs, const EndRecord& rhs) noexcept {
  return lhs.valid == rhs.valid && lhs.identity == rhs.identity &&
         lhs.issue == rhs.issue && lhs.epoch == rhs.epoch &&
         lhs.backendQueue == rhs.backendQueue &&
         lhs.backendSequence == rhs.backendSequence &&
         lhs.rawOrdinal == rhs.rawOrdinal;
}

inline bool queueHasTailAfter(const QueueSnapshot& queue,
                              std::uint64_t ordinal) noexcept {
  return queue.backOrdinalKnown && queue.backOrdinal > ordinal;
}

inline ResultClass classifyRecords(std::uint64_t identity,
                                   const EndRecord* before,
                                   const EndRecord* after,
                                   const QueueSnapshot& beforeQueue,
                                   const QueueSnapshot& afterQueue,
                                   bool drained) noexcept {
  if (!drained) return ResultClass::FailedDrain;
  const auto matchesIdentity = [identity](const EndRecord* record) {
    return record && record->valid && record->identity == identity;
  };
  if (!matchesIdentity(after)) return ResultClass::NoKnownEnd;
  if (!matchesIdentity(before)) return ResultClass::MissingBeforeAfterEnd;
  if (!sameEnd(*before, *after)) return ResultClass::EndChanged;
  if (after->rawOrdinal == 0) return ResultClass::EndKnownNoRawOrdinal;
  if (queueHasTailAfter(beforeQueue, after->rawOrdinal))
    return ResultClass::QueuedTailBeforeDrain;
  if (queueHasTailAfter(afterQueue, after->rawOrdinal))
    return ResultClass::QueuedTailAfterDrainOnly;
  return ResultClass::KnownEndNoQueuedTail;
}

class Ledger final {
 public:
  static constexpr std::size_t kDetailCapacity = 32;
  static constexpr std::size_t kBusyDetailCapacity = 16;

  explicit Ledger(bool logging = true) noexcept : logging_(logging) {}
  Ledger(const Ledger&) = delete;
  Ledger& operator=(const Ledger&) = delete;
  ~Ledger() { report(); }

  void record(Detail detail) noexcept {
    std::lock_guard lock(mutex_);
    ++totals_.drains;
    if (!detail.drained) ++totals_.failedDrains;
    const auto bucket = static_cast<std::size_t>(detail.result);
    totals_.drainNanoseconds += detail.drainNanoseconds;
    if (detail.beforeQueue.queuedCount != 0 || detail.beforeQueue.inFlight) {
      ++totals_.beforeQueueBusy;
      totals_.beforeQueueBusyNanoseconds += detail.drainNanoseconds;
    } else {
      ++totals_.beforeQueueEmpty;
      totals_.beforeQueueEmptyNanoseconds += detail.drainNanoseconds;
    }
    if (detail.queuedTailBeforeDrain) {
      ++totals_.queuedTailBeforeDrain;
      totals_.queuedTailBeforeDrainNanoseconds += detail.drainNanoseconds;
    }
    if (detail.queuedTailAfterDrainOnly) {
      ++totals_.queuedTailAfterDrainOnly;
      totals_.queuedTailAfterDrainOnlyNanoseconds += detail.drainNanoseconds;
    }
    if (detail.unknownInflight) {
      ++totals_.unknownInflight;
      totals_.unknownInflightNanoseconds += detail.drainNanoseconds;
    }
    if (bucket < totals_.classes.size()) {
      ++totals_.classes[bucket];
      totals_.classDrainNanoseconds[bucket] += detail.drainNanoseconds;
    }
    const bool busy = detail.beforeQueue.queuedCount != 0 ||
                      detail.beforeQueue.inFlight ||
                      detail.queuedTailBeforeDrain;
    if (busy) {
      if (busySize_ < kBusyDetailCapacity) {
        details_[busySize_++] = detail;
      } else {
        ++totals_.omittedDetails;
      }
    } else if (genericSize_ < kDetailCapacity - kBusyDetailCapacity) {
      details_[kBusyDetailCapacity + genericSize_++] = detail;
    } else {
      ++totals_.omittedDetails;
    }
  }

  Totals totals() const noexcept {
    std::lock_guard lock(mutex_);
    return totals_;
  }

  std::array<Detail, kDetailCapacity> details() const noexcept {
    std::lock_guard lock(mutex_);
    return details_;
  }

  std::size_t detailCount() const noexcept {
    std::lock_guard lock(mutex_);
    return busySize_ + genericSize_;
  }

  void report() noexcept {
    std::lock_guard lock(mutex_);
    if (!logging_ || reported_) return;
    reported_ = true;
    util::logf(util::LogLevel::Info, "dxmt9-query-replay-drain-summary",
               "drains=%llu failed=%llu omitted=%llu drain_ns=%llu "
               "before_busy_count=%llu before_busy_ns=%llu "
               "before_empty_count=%llu before_empty_ns=%llu "
               "tail_before_drain=%llu tail_before_drain_ns=%llu "
               "tail_after_drain_only=%llu tail_after_drain_only_ns=%llu "
               "unknown_inflight=%llu unknown_inflight_ns=%llu no_end=%llu "
               "missing_before=%llu changed=%llu no_raw=%llu "
               "tail_before_class=%llu tail_after_only_class=%llu "
               "known_no_tail=%llu failed_class=%llu "
               "class_ns=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
               (unsigned long long)totals_.drains,
               (unsigned long long)totals_.failedDrains,
               (unsigned long long)totals_.omittedDetails,
               (unsigned long long)totals_.drainNanoseconds,
               (unsigned long long)totals_.beforeQueueBusy,
               (unsigned long long)totals_.beforeQueueBusyNanoseconds,
               (unsigned long long)totals_.beforeQueueEmpty,
               (unsigned long long)totals_.beforeQueueEmptyNanoseconds,
               (unsigned long long)totals_.queuedTailBeforeDrain,
               (unsigned long long)totals_.queuedTailBeforeDrainNanoseconds,
               (unsigned long long)totals_.queuedTailAfterDrainOnly,
               (unsigned long long)totals_.queuedTailAfterDrainOnlyNanoseconds,
               (unsigned long long)totals_.unknownInflight,
               (unsigned long long)totals_.unknownInflightNanoseconds,
               (unsigned long long)totals_.classes[0],
               (unsigned long long)totals_.classes[1],
               (unsigned long long)totals_.classes[2],
               (unsigned long long)totals_.classes[3],
               (unsigned long long)totals_.classes[4],
               (unsigned long long)totals_.classes[5],
               (unsigned long long)totals_.classes[6],
               (unsigned long long)totals_.classes[7],
               (unsigned long long)totals_.classDrainNanoseconds[0],
               (unsigned long long)totals_.classDrainNanoseconds[1],
               (unsigned long long)totals_.classDrainNanoseconds[2],
               (unsigned long long)totals_.classDrainNanoseconds[3],
               (unsigned long long)totals_.classDrainNanoseconds[4],
               (unsigned long long)totals_.classDrainNanoseconds[5],
               (unsigned long long)totals_.classDrainNanoseconds[6],
               (unsigned long long)totals_.classDrainNanoseconds[7]);
    for (std::size_t i = 0; i < busySize_; ++i) {
      reportDetail(details_[i]);
    }
    for (std::size_t i = 0; i < genericSize_; ++i) {
      reportDetail(details_[kBusyDetailCapacity + i]);
    }
  }

 private:
  void reportDetail(const Detail& d) noexcept {
    util::logf(util::LogLevel::Info, "dxmt9-query-replay-drain",
                 "query=%llu class=%u drained=%u drain_ns=%llu "
                 "before_issue=%llu before_epoch=%llu before_raw=%llu "
                 "after_issue=%llu after_epoch=%llu after_raw=%llu "
                 "queued_before=%zu queued_after=%zu front_ordinal=%llu "
                 "front_known=%u back_ordinal=%llu back_known=%u "
                 "in_flight=%u placeholders_before=%u placeholders_after=%u "
                 "unknown=%u tail_before=%u tail_after_only=%u",
                 (unsigned long long)d.queryIdentity,
                 static_cast<unsigned>(d.result), unsigned(d.drained),
                 (unsigned long long)d.drainNanoseconds,
                 (unsigned long long)d.beforeEnd.issue,
                 (unsigned long long)d.beforeEnd.epoch,
                 (unsigned long long)d.beforeEnd.rawOrdinal,
                 (unsigned long long)d.afterEnd.issue,
                 (unsigned long long)d.afterEnd.epoch,
                 (unsigned long long)d.afterEnd.rawOrdinal,
                 d.beforeQueue.queuedCount, d.afterQueue.queuedCount,
                 (unsigned long long)d.beforeQueue.frontOrdinal,
                 unsigned(d.beforeQueue.frontOrdinalKnown),
                 (unsigned long long)d.beforeQueue.backOrdinal,
                 unsigned(d.beforeQueue.backOrdinalKnown),
                 unsigned(d.beforeQueue.inFlight || d.afterQueue.inFlight),
                 unsigned(d.beforeQueue.frontPlaceholder ||
                          d.beforeQueue.backPlaceholder),
                 unsigned(d.afterQueue.frontPlaceholder ||
                          d.afterQueue.backPlaceholder),
                 unsigned(d.beforeQueue.unknownOrdinal ||
                          d.afterQueue.unknownOrdinal),
                 unsigned(d.queuedTailBeforeDrain),
                 unsigned(d.queuedTailAfterDrainOnly));
  }
  mutable std::mutex mutex_;
  bool logging_ = true;
  bool reported_ = false;
  std::array<Detail, kDetailCapacity> details_{};
  std::size_t busySize_ = 0;
  std::size_t genericSize_ = 0;
  Totals totals_{};
};

inline Ledger& ledger() {
  static Ledger value;
  return value;
}

inline std::uint64_t nowNs() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace dxmt9::d3d9::query_drain_diag
