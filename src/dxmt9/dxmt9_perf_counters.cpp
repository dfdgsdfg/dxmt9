#include "dxmt9_perf_counters.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdio>

namespace dxmt9::perf {
namespace {

struct Counters {
  std::atomic<std::uint64_t> submitDraw{0};
  std::atomic<std::uint64_t> submitClear{0};
  std::atomic<std::uint64_t> submitStretch{0};
  std::atomic<std::uint64_t> submitPresent{0};
  std::atomic<std::uint64_t> submitFlush{0};
  std::atomic<std::uint64_t> commandBuffers{0};
  std::atomic<std::uint64_t> metalBuffers{0};
  std::atomic<std::uint64_t> metalBufferBytes{0};
  std::atomic<std::uint64_t> pipelineBuilds{0};
  std::atomic<std::uint64_t> completionWaits{0};
  std::atomic<std::uint64_t> completionWaitNs{0};
  std::atomic<std::uint64_t> completionWaitMaxNs{0};
  std::atomic<std::uint64_t> completionPresentWaits{0};
  std::atomic<std::uint64_t> completionPresentWaitNs{0};
  std::atomic<std::uint64_t> completionPresentWaitMaxNs{0};
  std::atomic<std::uint64_t> completionDrawWaits{0};
  std::atomic<std::uint64_t> completionDrawWaitNs{0};
  std::atomic<std::uint64_t> completionDrawWaitMaxNs{0};
  std::atomic<std::uint64_t> completionBlitWaits{0};
  std::atomic<std::uint64_t> completionBlitWaitNs{0};
  std::atomic<std::uint64_t> completionBlitWaitMaxNs{0};
  std::atomic<std::uint64_t> completionOtherWaits{0};
  std::atomic<std::uint64_t> completionOtherWaitNs{0};
  std::atomic<std::uint64_t> completionOtherWaitMaxNs{0};
  std::atomic<std::uint64_t> syncWaits{0};
  std::atomic<std::uint64_t> syncWaitNs{0};
  std::atomic<std::uint64_t> syncWaitMaxNs{0};
  std::atomic<std::uint64_t> presentEncoded{0};
  std::atomic<std::uint64_t> presentSkipped{0};
  std::atomic<std::uint64_t> presentAcquireWaits{0};
  std::atomic<std::uint64_t> presentAcquireWaitNs{0};
  std::atomic<std::uint64_t> presentSetPropsWaits{0};
  std::atomic<std::uint64_t> presentSetPropsWaitNs{0};
};

Counters& counters() {
  static Counters value;
  return value;
}

bool enabledFlag() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT_PERF_COUNTERS");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return value;
}

std::uint64_t load(const std::atomic<std::uint64_t>& value) {
  return value.load(std::memory_order_relaxed);
}

void report() {
  if (!enabledFlag()) {
    return;
  }
  const Counters& c = counters();
  std::fprintf(
      stderr,
      "[dxmt9-perf] submit_draw=%llu submit_clear=%llu submit_stretch=%llu "
      "submit_present=%llu submit_flush=%llu command_buffers=%llu "
      "metal_buffers=%llu metal_buffer_bytes=%llu pipeline_builds=%llu "
      "completion_waits=%llu completion_wait_ms=%.3f completion_wait_max_ms=%.3f "
      "completion_present_waits=%llu completion_present_wait_ms=%.3f completion_present_wait_max_ms=%.3f "
      "completion_draw_waits=%llu completion_draw_wait_ms=%.3f completion_draw_wait_max_ms=%.3f "
      "completion_blit_waits=%llu completion_blit_wait_ms=%.3f completion_blit_wait_max_ms=%.3f "
      "completion_other_waits=%llu completion_other_wait_ms=%.3f completion_other_wait_max_ms=%.3f "
      "sync_waits=%llu sync_wait_ms=%.3f sync_wait_max_ms=%.3f "
      "present_encoded=%llu present_skipped=%llu present_acquire_waits=%llu "
      "present_acquire_wait_ms=%.3f present_set_props_waits=%llu present_set_props_wait_ms=%.3f\n",
      static_cast<unsigned long long>(load(c.submitDraw)),
      static_cast<unsigned long long>(load(c.submitClear)),
      static_cast<unsigned long long>(load(c.submitStretch)),
      static_cast<unsigned long long>(load(c.submitPresent)),
      static_cast<unsigned long long>(load(c.submitFlush)),
      static_cast<unsigned long long>(load(c.commandBuffers)),
      static_cast<unsigned long long>(load(c.metalBuffers)),
      static_cast<unsigned long long>(load(c.metalBufferBytes)),
      static_cast<unsigned long long>(load(c.pipelineBuilds)),
      static_cast<unsigned long long>(load(c.completionWaits)),
      static_cast<double>(load(c.completionWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionWaitMaxNs)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionPresentWaits)),
      static_cast<double>(load(c.completionPresentWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionPresentWaitMaxNs)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionDrawWaits)),
      static_cast<double>(load(c.completionDrawWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionDrawWaitMaxNs)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionBlitWaits)),
      static_cast<double>(load(c.completionBlitWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionBlitWaitMaxNs)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionOtherWaits)),
      static_cast<double>(load(c.completionOtherWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionOtherWaitMaxNs)) / 1000000.0,
      static_cast<unsigned long long>(load(c.syncWaits)),
      static_cast<double>(load(c.syncWaitNs)) / 1000000.0,
      static_cast<double>(load(c.syncWaitMaxNs)) / 1000000.0,
      static_cast<unsigned long long>(load(c.presentEncoded)),
      static_cast<unsigned long long>(load(c.presentSkipped)),
      static_cast<unsigned long long>(load(c.presentAcquireWaits)),
      static_cast<double>(load(c.presentAcquireWaitNs)) / 1000000.0,
      static_cast<unsigned long long>(load(c.presentSetPropsWaits)),
      static_cast<double>(load(c.presentSetPropsWaitNs)) / 1000000.0);
}

void ensureRegistered() {
  static const bool registered = [] {
    if (enabledFlag()) {
      std::atexit(report);
    }
    return true;
  }();
  (void)registered;
}

void add(std::atomic<std::uint64_t>& counter, std::uint64_t value = 1) {
  if (!enabled()) {
    return;
  }
  counter.fetch_add(value, std::memory_order_relaxed);
}

void updateMax(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
  if (!enabled()) {
    return;
  }
  auto current = counter.load(std::memory_order_relaxed);
  while (current < value &&
         !counter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

}  // namespace

bool enabled() {
  ensureRegistered();
  return enabledFlag();
}

void countSubmitDraw() {
  add(counters().submitDraw);
}

void countSubmitClear() {
  add(counters().submitClear);
}

void countSubmitStretch() {
  add(counters().submitStretch);
}

void countSubmitPresent() {
  add(counters().submitPresent);
}

void countSubmitFlush() {
  add(counters().submitFlush);
}

void countCommandBuffer() {
  add(counters().commandBuffers);
}

void countMetalBuffer(std::size_t bytes) {
  add(counters().metalBuffers);
  add(counters().metalBufferBytes, static_cast<std::uint64_t>(bytes));
}

void countPipelineBuild() {
  add(counters().pipelineBuilds);
}

void countCompletionWait(std::uint64_t nanoseconds, bool hasDraw, bool hasPresent, bool hasBlit) {
  add(counters().completionWaits);
  add(counters().completionWaitNs, nanoseconds);
  updateMax(counters().completionWaitMaxNs, nanoseconds);
  if (hasPresent) {
    add(counters().completionPresentWaits);
    add(counters().completionPresentWaitNs, nanoseconds);
    updateMax(counters().completionPresentWaitMaxNs, nanoseconds);
  } else if (hasDraw) {
    add(counters().completionDrawWaits);
    add(counters().completionDrawWaitNs, nanoseconds);
    updateMax(counters().completionDrawWaitMaxNs, nanoseconds);
  } else if (hasBlit) {
    add(counters().completionBlitWaits);
    add(counters().completionBlitWaitNs, nanoseconds);
    updateMax(counters().completionBlitWaitMaxNs, nanoseconds);
  } else {
    add(counters().completionOtherWaits);
    add(counters().completionOtherWaitNs, nanoseconds);
    updateMax(counters().completionOtherWaitMaxNs, nanoseconds);
  }
}

void countSyncWait(std::uint64_t nanoseconds) {
  add(counters().syncWaits);
  add(counters().syncWaitNs, nanoseconds);
  updateMax(counters().syncWaitMaxNs, nanoseconds);
}

void countPresentEncoded() {
  add(counters().presentEncoded);
}

void countPresentSkipped() {
  add(counters().presentSkipped);
}

void countPresentAcquireWait(std::uint64_t nanoseconds) {
  add(counters().presentAcquireWaits);
  add(counters().presentAcquireWaitNs, nanoseconds);
}

void countPresentSetPropsWait(std::uint64_t nanoseconds) {
  add(counters().presentSetPropsWaits);
  add(counters().presentSetPropsWaitNs, nanoseconds);
}

}  // namespace dxmt9::perf
