// ReplayOffloadWorker: device-owned background thread draining the Task 2
// ReplayOffloadQueue. Kept in its own TU so device_c_common.hpp only needs a
// forward declaration of ReplayOffloadWorker for the D9CDevice member (the
// full definition here, plus D9CDevice's own destructor defined out-of-line
// in device_c_state.cpp, is what actually needs ReplayOffloadWorker
// complete).

#include "device_c_replay_offload.hpp"

#include "device_c_common.hpp"
// Need the full dxmt9::Device type to call abortPresentOrdinalWaits() on
// the upperDevice shared_ptr from the fail-stop path below (mirrors the
// same include-for-the-same-reason comment in device_c_chunk_replay.cpp).
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_perf_counters.hpp"

#include "dxmt9/assert.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace dxmt9::d3d9 {

namespace {

// Same file-local RAII CPU-time scope pattern used across the codebase
// (e.g. src/d3d9/core_draw.cpp, src/dxmt9/dxmt9_command_queue.cpp): there is
// no shared dxmt9::perf::PerfScope type, each TU that needs one defines its
// own thin wrapper around a `count*(nanoseconds)` function pointer.
class PerfScope {
 public:
  explicit PerfScope(void (*record)(std::uint64_t)) : record_(record) {}
  ~PerfScope() {
    if (!record_) {
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    record_(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  }

  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  void (*record_)(std::uint64_t) = nullptr;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

}  // namespace

bool offloadCommitReplayEnabled() {
  // Engine default flipped to ON (R-BACK-2.51 promotion, 2026-07-10) after
  // the per-present boundary suppression + ordinal latency cap landed and
  // the offload-forced native spec variants went green. Explicit "0" is the
  // opt-out; any other value (or unset) enables the offload.
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_OFFLOAD_COMMIT_REPLAY");
    if (!value || value[0] == '\0') {
      return true;
    }
    return !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

namespace {

std::size_t offloadQueueSizeFromEnv(const char* name, std::size_t fallback) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || parsed == 0ull) {
    return fallback;
  }
  return static_cast<std::size_t>(parsed);
}

}  // namespace

std::size_t offloadQueueMaxChunks() {
  static const std::size_t chunks =
      offloadQueueSizeFromEnv("DXMT9_OFFLOAD_QUEUE_CHUNKS", 64u);
  return chunks;
}

std::size_t offloadQueueMaxBytes() {
  static const std::size_t bytes =
      offloadQueueSizeFromEnv("DXMT9_OFFLOAD_QUEUE_BYTES", 8u << 20);
  return bytes;
}

void notePushBackpressureWait(std::uint64_t nanoseconds) {
  dxmt9::perf::countOffloadPushBackpressureWait();
  dxmt9::perf::countOffloadPushBackpressureWaitNs(nanoseconds);
}

void noteWorkerIdleWait(std::uint64_t nanoseconds) {
  dxmt9::perf::countOffloadWorkerIdleWaitNs(nanoseconds);
}

bool prepareV2OffloadChunk(
    std::span<const std::byte> blob,
    const V2ChunkEnvelope& envelope,
    const WireObjectRegistry& registry,
    WireObjectRegistry::RetainFn retain,
    RawCommandChunk& out) noexcept {
  if (!out.retainedWrappers.empty() || !out.resolvedObjects.empty()) {
    return false;
  }

  RawCommandChunk candidate;
  try {
    candidate.recordBlob.resize(blob.size());
    if (!blob.empty()) {
      std::memcpy(candidate.recordBlob.data(), blob.data(), blob.size());
    }
    candidate.resolvedObjects.resize(envelope.handleCount);
    candidate.retainedWrappers.resize(envelope.handleCount);
  } catch (...) {
    return false;
  }

  ImportedChunkV2View view;
  const auto ownedBytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(candidate.recordBlob.data()),
      candidate.recordBlob.size());
  if (!validateCommandChunkV2(ownedBytes, envelope, &view).valid() ||
      !registry.resolveAndRetain(view.handles, candidate.resolvedObjects,
                                retain)) {
    return false;
  }

  for (std::size_t i = 0u; i < view.handles.size(); ++i) {
    candidate.retainedWrappers[i] = RetainedWireHandle{
        .kind = view.handles[i].kind,
        .ptr = candidate.resolvedObjects[i],
    };
  }
  candidate.wireVersion = D9C_COMMAND_CHUNK_VERSION_V2;
  candidate.recordCount = envelope.recordCount;
  candidate.recordBytes = static_cast<std::uint32_t>(blob.size());
  candidate.handleCount = envelope.handleCount;
  candidate.preflightValidated = true;
  candidate.hasPresent = std::any_of(
      view.records.begin(), view.records.end(), [](const auto& record) {
        return record.type == D9C_COMMAND_RECORD_PRESENT;
      });
  out = std::move(candidate);
  return true;
}

void ReplayOffloadWorker::start(D9CDevice* device) {
  thread_ = std::thread([this, device] { run(device); });
}

void ReplayOffloadWorker::stop() {
  queue_.stop();
  if (thread_.joinable()) {
    thread_.join();
  }
  // Defensive drain: by the time join() returns, run() has already fully
  // exited (including its own epilogue drain below), so this is
  // single-threaded and normally finds nothing left. Kept here in case a
  // future change manages to leave a chunk queued across a stop() that
  // did not go through the fail-stop path.
  RawCommandChunk drained;
  while (queue_.pop(drained)) {
    releaseRetainedWrappers(drained);
    queue_.markReplayDone();
  }
}

void ReplayOffloadWorker::run(D9CDevice* device) {
  RawCommandChunk chunk;
  while (queue_.pop(chunk)) {
    const int32_t hr = replayRawChunk(device, chunk);
    queue_.markReplayDone();
    if (hr < 0) {
      failed_.store(true, std::memory_order_release);
      DXMT_ASSERT(false && "deferred commit replay failed");
      queue_.stop();
      // Release-build safety net: DXMT_ASSERT above does not abort outside
      // debug builds, so without this an app thread already parked in
      // CommandQueue::waitPresentOrdinalBoundary on an ordinal this now-dead
      // worker can never retire would hang forever. abortPresentOrdinalWaits()
      // wakes any such waiter so it observes the abort and returns instead.
      if (auto upper = device->dev().upperDevice()) {
        upper->abortPresentOrdinalWaits();
      }
      break;
    }
  }
  // Drain epilogue: release wrapper retention for any chunks left queued
  // when the loop above exited. Reached either via the fail-stop `break`
  // (the remaining queued chunks were never popped/replayed, so their
  // retained wrappers would otherwise leak) or via a normal stop()-with-
  // empty-queue exit (a no-op drain, since ReplayOffloadQueue::pop() already
  // returns queued items after stop() -- only returning false once stopped
  // *and* empty -- so the loop above already replayed everything in that
  // case). Single-threaded: this is still the worker thread itself, and no
  // other thread calls pop()/markReplayDone() on this queue.
  RawCommandChunk drained;
  while (queue_.pop(drained)) {
    releaseRetainedWrappers(drained);
    queue_.markReplayDone();
  }
}

// Per-call-site attribution for the drain fence
// (DXMT9_PERF_DRAIN_FENCE_SITES). The aggregate counters say the producer
// blocks ~10.6 times per present for ~2.2ms total on GT2, but not which of the
// ~85 bridge entry points is doing it, which is what decides whether the fence
// can be narrowed. Recorded only when a drain actually blocks, so the cost is
// ~10 table probes per present, not one per bridge call.
//
// Buckets on the `site` POINTER, not its content: every caller passes a string
// literal, so identity comparison is exact and costs one compare. A site that
// overflows the table is folded into an overflow row rather than silently
// dropped -- a missing row would read as "this call never blocks".
namespace {

constexpr std::size_t kDrainSiteSlots = 48;

struct DrainSiteTable {
  std::mutex mutex;
  const char* names[kDrainSiteSlots]{};
  std::uint64_t counts[kDrainSiteSlots]{};
  std::uint64_t nanos[kDrainSiteSlots]{};
  std::size_t used = 0;
  std::uint64_t overflowCount = 0;
  std::uint64_t overflowNanos = 0;
};

DrainSiteTable& drainSiteTable() {
  static DrainSiteTable table;
  return table;
}

bool drainSiteAttributionEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_DRAIN_FENCE_SITES");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

void noteDrainSite(const char* site, std::uint64_t nanoseconds) {
  auto& t = drainSiteTable();
  std::lock_guard lock(t.mutex);
  for (std::size_t i = 0; i < t.used; ++i) {
    if (t.names[i] == site) {
      ++t.counts[i];
      t.nanos[i] += nanoseconds;
      return;
    }
  }
  if (t.used < kDrainSiteSlots) {
    t.names[t.used] = site;
    t.counts[t.used] = 1;
    t.nanos[t.used] = nanoseconds;
    ++t.used;
    return;
  }
  ++t.overflowCount;
  t.overflowNanos += nanoseconds;
}

}  // namespace

void logDrainFenceSites(std::uint64_t presents) {
  if (!drainSiteAttributionEnabled()) {
    return;
  }
  auto& t = drainSiteTable();
  std::lock_guard lock(t.mutex);
  const double p = presents ? static_cast<double>(presents) : 1.0;
  for (std::size_t i = 0; i < t.used; ++i) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-drain-site",
                      "site=%s waits=%llu waits_per_present=%.3f "
                      "ms_total=%.3f ms_per_present=%.4f us_per_wait=%.1f",
                      t.names[i] ? t.names[i] : "untagged",
                      static_cast<unsigned long long>(t.counts[i]),
                      static_cast<double>(t.counts[i]) / p,
                      static_cast<double>(t.nanos[i]) / 1.0e6,
                      static_cast<double>(t.nanos[i]) / 1.0e6 / p,
                      t.counts[i] ? static_cast<double>(t.nanos[i]) /
                                        static_cast<double>(t.counts[i]) / 1.0e3
                                  : 0.0);
  }
  if (t.overflowCount) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-drain-site",
                      "site=<overflow> waits=%llu ms_total=%.3f "
                      "(raise kDrainSiteSlots)",
                      static_cast<unsigned long long>(t.overflowCount),
                      static_cast<double>(t.overflowNanos) / 1.0e6);
  }
}

void drainDeferredReplay(D9CDevice* d, const char* site) {
  if (!d || !d->replayOffload) {
    return;
  }
  auto& queue = d->replayOffload->queue();
  if (queue.depth() == 0) {
    return;
  }
  dxmt9::perf::countOffloadDrainFenceWait();
  const auto waitStart = std::chrono::steady_clock::now();
  queue.waitDrained();
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - waitStart).count());
  dxmt9::perf::countOffloadDrainFenceCpuTime(elapsed);
  if (drainSiteAttributionEnabled()) {
    noteDrainSite(site, elapsed);
  }
}

void drainDeferredReplay(D9CBuffer* b, const char* site) {
  if (!b) {
    return;
  }
  drainDeferredReplay(b->device, site);
}

void drainDeferredReplay(D9CSwapChain* s, const char* site) {
  if (!s) {
    return;
  }
  drainDeferredReplay(s->owner, site);
}

void drainDeferredReplay(D9CTexture* t, const char* site) {
  if (!t) {
    return;
  }
  drainDeferredReplay(t->device, site);
}

void drainDeferredReplay(D9CSurface* s, const char* site) {
  if (!s) {
    return;
  }
  drainDeferredReplay(s->device, site);
}

}  // namespace dxmt9::d3d9
