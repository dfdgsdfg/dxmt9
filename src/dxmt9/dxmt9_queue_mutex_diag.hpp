#pragma once

// Shared segment-hold accounting primitives for DXMT9_PERF_QUEUE_MUTEX_SPLIT
// (see the full design comment in dxmt9_command_queue.cpp, near
// QueueMutexSiteTable). The acquire-side machinery (QueueMutexBeginToken,
// QueueMutexProbeScope, the site table itself) stays local to
// dxmt9_command_queue.cpp because every mutex_ acquisition site lives there.
//
// This header exists because a meaningful share of CommandQueue::mutex_'s
// hold time is spent inside QueueLifecycleController (dxmt9_queue.{hpp,cpp}),
// a different translation unit that receives the held std::unique_lock by
// reference and may unlock()/relock() it, or hand it to a
// condition_variable::wait(), an unbounded number of times before returning.
// Those interior unlock/relock and cv-wait boundaries are the only places a
// "segment" (a contiguous interval during which the caller actually owns the
// mutex) can be measured correctly, so the instrumentation has to live next
// to them, not at the outer CommandQueue call site. The outer sites keep
// their existing skipHold=true QueueMutexProbeScope (acquire-wait only); the
// segments recorded here are ADDITIONAL rows in the same
// [dxmt9-queue-mutex] table, not a replacement for it.
//
// Zero-cost-when-off contract: queueMutexSplitEnabled() is a single cached
// bool read (same underlying flag as dxmt9_command_queue.cpp's copy -- both
// read the same env var independently, so there is no cross-TU static-init
// ordering dependency). Callers are expected to branch on it themselves
// before touching a clock, exactly like QueueMutexBeginToken does; nothing
// here reads a clock unless the caller already checked the flag.

#include <chrono>
#include <cstdint>

namespace dxmt9 {

// Same predicate as dxmt9_command_queue.cpp's file-local
// queueMutexSplitEnabled(): true iff DXMT9_PERF_QUEUE_MUTEX_SPLIT is set to a
// non-empty, non-"0" value.
bool queueMutexSplitEnabled();

// Records one more sample against `site` (bucketed by string-literal pointer
// identity, exactly like the acquire-side sink). `haveHold`/`holdNs` follow
// the same meaning as the acquire-side noteQueueMutexSite: when true, one
// hold sample is added along with a max-hold update.
void noteQueueMutexSite(const char* site, std::uint64_t acquireWaitNs,
                        bool haveHold, std::uint64_t holdNs);

std::uint64_t queueMutexProbeNanos(std::chrono::steady_clock::duration d);

// Convenience for the common segment shape: "I have been continuously
// holding the mutex since `start`; note that as a zero-acquire-wait hold
// sample for `site`." Callers gate the (already cheap) clock read on their
// own cached `enabled` bool -- see the loop-friendly pattern below -- so this
// itself does not re-check queueMutexSplitEnabled(); it is meant to be
// called only when the caller has already confirmed `enabled`.
inline void noteQueueMutexSegment(const char* site,
                                  std::chrono::steady_clock::time_point start) {
  noteQueueMutexSite(site, /*acquireWaitNs=*/0, /*haveHold=*/true,
                     queueMutexProbeNanos(std::chrono::steady_clock::now() - start));
}

// Same as noteQueueMutexSegment, but no-ops (and touches no clock) unless
// `enabled` is true. Use this at call sites that computed `enabled` once
// (e.g. at the top of a loop iteration) instead of re-deriving it from
// queueMutexSplitEnabled() at every segment boundary.
inline void noteQueueMutexSegmentIfEnabled(
    const char* site, bool enabled,
    std::chrono::steady_clock::time_point start) {
  if (!enabled) {
    return;
  }
  noteQueueMutexSegment(site, start);
}

// RAII segment-hold probe for the common case: a single function scope that
// is known to hold the mutex for its entire body (no interior unlock/relock,
// no cv wait). Prefer this over the free-function pair above when the
// segment's start and end are the same lexical scope; use the free functions
// when a segment must be bracketed around an interior wait/unlock inside a
// loop or across an early-return.
class QueueMutexSegmentScope {
 public:
  explicit QueueMutexSegmentScope(const char* site)
      : site_(site), enabled_(queueMutexSplitEnabled()) {
    if (enabled_) {
      start_ = std::chrono::steady_clock::now();
    }
  }

  QueueMutexSegmentScope(const QueueMutexSegmentScope&) = delete;
  QueueMutexSegmentScope& operator=(const QueueMutexSegmentScope&) = delete;

  ~QueueMutexSegmentScope() {
    if (!enabled_) {
      return;
    }
    noteQueueMutexSegment(site_, start_);
  }

 private:
  const char* site_;
  bool enabled_;
  std::chrono::steady_clock::time_point start_{};
};

}  // namespace dxmt9
