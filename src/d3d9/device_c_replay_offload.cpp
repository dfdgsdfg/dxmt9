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
#include <limits>

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

std::shared_ptr<ReplayDrainTarget>
ReplayDrainLedger::targetForCoreBuffer(std::uint64_t handleValue) {
  std::lock_guard lock(mutex_);
  if (handleValue != 0u) {
    const auto found = bufferTargets_.find(handleValue);
    if (found != bufferTargets_.end()) {
      if (auto target = found->second.lock()) {
        return target;
      }
      bufferTargets_.erase(found);
    }
  }
  auto target = std::make_shared<ReplayDrainTarget>();
  if (handleValue != 0u) {
    if (bufferTargets_.size() >= nextBufferTargetSweep_ - 1u) {
      std::erase_if(bufferTargets_, [](const auto& entry) {
        return entry.second.expired();
      });
      constexpr std::size_t kMinimumSweepThreshold = 64u;
      constexpr std::size_t kMaximumSize =
          std::numeric_limits<std::size_t>::max();
      nextBufferTargetSweep_ =
          bufferTargets_.size() >= (kMaximumSize / 2u) - 1u
              ? kMaximumSize
              : std::max(kMinimumSweepThreshold,
                         2u * (bufferTargets_.size() + 1u));
    }
    bufferTargets_[handleValue] = target;
  }
  return target;
}

void ReplayDrainLedger::publishAcceptedLocked(RawCommandChunk& chunk) noexcept {
  chunk.replaySeq = nextSeq_++;
  for (auto* target : chunk.ledgerTargets) {
    if (target) {
      target->lastQueuedSeq = chunk.replaySeq;
    }
  }
  cv_.notify_all();
}

bool ReplayDrainLedger::publishInline(RawCommandChunk& chunk) noexcept {
  std::lock_guard lock(mutex_);
  if (!accepting_ || terminal()) {
    return false;
  }
  publishAcceptedLocked(chunk);
  return true;
}

void ReplayDrainLedger::publishReplayed(
    const RawCommandChunk& chunk) noexcept {
  std::lock_guard lock(mutex_);
  for (auto* target : chunk.ledgerTargets) {
    if (target) {
      target->lastReplayedSeq = std::max(
          target->lastReplayedSeq, chunk.replaySeq);
    }
  }
  cv_.notify_all();
}

ReplayDrainResult ReplayDrainLedger::wait(
    ReplayDrainTarget& target) noexcept {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [&] {
    return poisoned_ || stopping_ ||
           target.lastQueuedSeq <= target.lastReplayedSeq;
  });
  if (poisoned_) {
    return ReplayDrainResult::Poisoned;
  }
  if (stopping_) {
    return ReplayDrainResult::Stopped;
  }
  return ReplayDrainResult::CaughtUp;
}

bool ReplayDrainLedger::pending(
    const ReplayDrainTarget& target) const noexcept {
  std::lock_guard lock(mutex_);
  return target.lastQueuedSeq > target.lastReplayedSeq;
}

void ReplayDrainLedger::stop() noexcept {
  auto expected = ReplayTerminalState::Running;
  terminalState_.compare_exchange_strong(
      expected, ReplayTerminalState::Stopped,
      std::memory_order_release, std::memory_order_relaxed);
  std::lock_guard lock(mutex_);
  accepting_ = false;
  stopping_ = true;
  cv_.notify_all();
}

void ReplayDrainLedger::publishFailure() noexcept {
  terminalState_.store(ReplayTerminalState::Failed,
                       std::memory_order_release);
}

void ReplayDrainLedger::poison() noexcept {
  publishFailure();
  std::lock_guard lock(mutex_);
  accepting_ = false;
  poisoned_ = true;
  cv_.notify_all();
}

ReplayTerminalState ReplayDrainLedger::terminalState() const noexcept {
  return terminalState_.load(std::memory_order_acquire);
}

bool ReplayDrainLedger::terminal() const noexcept {
  return terminalState() != ReplayTerminalState::Running;
}

bool ReplayDrainLedger::stopped() const noexcept {
  return terminalState() == ReplayTerminalState::Stopped;
}

bool ReplayDrainLedger::poisoned() const noexcept {
  return terminalState() == ReplayTerminalState::Failed;
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
    candidate.ledgerTargets.reserve(envelope.handleCount);
    candidate.bufferSnapshots.reserve(envelope.handleCount);
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
  owner_ = device;
  thread_ = std::thread([this, device] { run(device); });
}

void ReplayOffloadWorker::stop() {
  if (owner_) {
    owner_->replayDrainLedger.stop();
  }
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
    const int32_t hr = replay_ ? replay_(device, chunk)
                               : replayRawChunk(device, chunk);
    if (hr < 0) {
      // Fail-stop is visible before any completion notification. A producer
      // linearized after poison() observes the same terminal state under the
      // queue -> ledger admission order and cannot enqueue a doomed chunk.
      device->replayDrainLedger.publishFailure();
      failed_.store(true, std::memory_order_release);
      device->replayDrainLedger.poison();
      queue_.stop();
      if (!replay_) {
        DXMT_ASSERT(false && "deferred commit replay failed");
      }
      if (failureHook_) {
        failureHook_(failureHookContext_);
      }
      // Release-build safety net: abort any app thread waiting on an ordinal
      // this now-dead worker can never retire. Native failure-injection tests
      // construct a device without a provider interface, hence the null guard.
      if (device->iface) {
        if (auto upper = device->dev().upperDevice()) {
          upper->abortPresentOrdinalWaits();
        }
      }
      chunk.bufferSnapshots.clear();
      releaseRetainedWrappers(chunk);
      queue_.markReplayDone();
      break;
    }
    device->replayDrainLedger.publishReplayed(chunk);
    // Raw-entry backing leases are needed only until replay has consumed the
    // immutable table. Release them before the worker can idle on its next pop;
    // GPU reuse is protected from here by the normal per-draw seqId marks.
    chunk.bufferSnapshots.clear();
    // Raw entries hold wrapper references, and each wrapper shares ownership
    // of its core-buffer-identity ledger target. Publish completion before
    // releasing those references.
    releaseRetainedWrappers(chunk);
    queue_.markReplayDone();
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

// Class histogram of the locks that actually blocked. Flags and pool follow the
// same encoding the d3d9_buffer_lock_* counters use (pool 0=DEFAULT, 1=MANAGED,
// 2=SYSTEMMEM, 3=SCRATCH), so the blocked subset can be compared directly
// against the all-locks population without a second convention.
constexpr std::uint32_t kD3DLockReadOnly = 0x00000010u;
constexpr std::uint32_t kD3DLockNoOverwrite = 0x00001000u;
constexpr std::uint32_t kD3DLockDiscard = 0x00002000u;

struct BlockedLockClasses {
  std::mutex mutex;
  std::uint64_t readOnly = 0;
  std::uint64_t discard = 0;
  std::uint64_t noOverwrite = 0;
  std::uint64_t plain = 0;
  std::uint64_t pool[4]{};
  std::uint64_t poolOther = 0;
  std::uint64_t readOnlyNs = 0;
  std::uint64_t discardNs = 0;
  std::uint64_t noOverwriteNs = 0;
  std::uint64_t plainNs = 0;
};

struct DrainFenceModes {
  std::mutex mutex;
  std::uint64_t globalWait = 0;
  std::uint64_t scopedWait = 0;
  std::uint64_t scopedClear = 0;
  std::uint64_t scopedUnrelated = 0;
  std::uint64_t bypassDiscard = 0;
  std::uint64_t bypassNoOverwrite = 0;
  std::uint64_t terminal = 0;
  std::uint64_t globalWaitNs = 0;
  std::uint64_t scopedWaitNs = 0;
};

enum class DrainFenceMode {
  GlobalWait,
  ScopedWait,
  ScopedClear,
  ScopedUnrelated,
  BypassDiscard,
  BypassNoOverwrite,
  Terminal,
};

DrainFenceModes& drainFenceModes() {
  static DrainFenceModes modes;
  return modes;
}

void noteDrainFenceMode(DrainFenceMode mode,
                        std::uint64_t nanoseconds = 0) {
  if (!drainSiteAttributionEnabled()) {
    return;
  }
  auto& modes = drainFenceModes();
  std::lock_guard lock(modes.mutex);
  switch (mode) {
  case DrainFenceMode::GlobalWait:
    ++modes.globalWait;
    modes.globalWaitNs += nanoseconds;
    break;
  case DrainFenceMode::ScopedWait:
    ++modes.scopedWait;
    modes.scopedWaitNs += nanoseconds;
    break;
  case DrainFenceMode::ScopedClear:
    ++modes.scopedClear;
    break;
  case DrainFenceMode::ScopedUnrelated:
    ++modes.scopedUnrelated;
    break;
  case DrainFenceMode::BypassDiscard:
    ++modes.bypassDiscard;
    break;
  case DrainFenceMode::BypassNoOverwrite:
    ++modes.bypassNoOverwrite;
    break;
  case DrainFenceMode::Terminal:
    ++modes.terminal;
    break;
  }
}

BlockedLockClasses& blockedLockClasses() {
  static BlockedLockClasses classes;
  return classes;
}

void noteBlockedLockClass(std::uint32_t flags, std::uint32_t pool,
                          std::uint64_t nanoseconds) {
  auto& c = blockedLockClasses();
  std::lock_guard lock(c.mutex);
  const bool discard = (flags & kD3DLockDiscard) != 0;
  const bool noOverwrite = (flags & kD3DLockNoOverwrite) != 0;
  if ((flags & kD3DLockReadOnly) != 0) { ++c.readOnly; c.readOnlyNs += nanoseconds; }
  if (discard) { ++c.discard; c.discardNs += nanoseconds; }
  if (noOverwrite) { ++c.noOverwrite; c.noOverwriteNs += nanoseconds; }
  if (!discard && !noOverwrite) { ++c.plain; c.plainNs += nanoseconds; }
  if (pool < 4u) ++c.pool[pool]; else ++c.poolOther;
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
  {
    auto& modes = drainFenceModes();
    std::lock_guard lock(modes.mutex);
    if (modes.globalWait || modes.scopedWait || modes.scopedClear ||
        modes.scopedUnrelated || modes.bypassDiscard ||
        modes.bypassNoOverwrite || modes.terminal) {
      dxmt9::util::logf(
          dxmt9::util::LogLevel::Info, "dxmt9-drain-site",
          "fence_mode global_wait=%llu/%.3fms scoped_wait=%llu/%.3fms "
          "scoped_clear=%llu scoped_unrelated=%llu bypass_discard=%llu "
          "bypass_nooverwrite=%llu terminal=%llu",
          static_cast<unsigned long long>(modes.globalWait),
          modes.globalWaitNs / 1.0e6,
          static_cast<unsigned long long>(modes.scopedWait),
          modes.scopedWaitNs / 1.0e6,
          static_cast<unsigned long long>(modes.scopedClear),
          static_cast<unsigned long long>(modes.scopedUnrelated),
          static_cast<unsigned long long>(modes.bypassDiscard),
          static_cast<unsigned long long>(modes.bypassNoOverwrite),
          static_cast<unsigned long long>(modes.terminal));
    }
  }
  {
    auto& c = blockedLockClasses();
    std::lock_guard lock(c.mutex);
    if (c.readOnly || c.discard || c.noOverwrite || c.plain) {
      dxmt9::util::logf(
          dxmt9::util::LogLevel::Info, "dxmt9-drain-site",
          "blocked_lock_class readonly=%llu/%.3fms discard=%llu/%.3fms "
          "nooverwrite=%llu/%.3fms plain=%llu/%.3fms "
          "pool_default=%llu pool_managed=%llu pool_sysmem=%llu "
          "pool_scratch=%llu pool_other=%llu",
          static_cast<unsigned long long>(c.readOnly), c.readOnlyNs / 1.0e6,
          static_cast<unsigned long long>(c.discard), c.discardNs / 1.0e6,
          static_cast<unsigned long long>(c.noOverwrite), c.noOverwriteNs / 1.0e6,
          static_cast<unsigned long long>(c.plain), c.plainNs / 1.0e6,
          static_cast<unsigned long long>(c.pool[0]),
          static_cast<unsigned long long>(c.pool[1]),
          static_cast<unsigned long long>(c.pool[2]),
          static_cast<unsigned long long>(c.pool[3]),
          static_cast<unsigned long long>(c.poolOther));
    }
  }
  if (t.overflowCount) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-drain-site",
                      "site=<overflow> waits=%llu ms_total=%.3f "
                      "(raise kDrainSiteSlots)",
                      static_cast<unsigned long long>(t.overflowCount),
                      static_cast<double>(t.overflowNanos) / 1.0e6);
  }
}

bool drainDeferredReplay(D9CDevice* d, const char* site) {
  if (!d) {
    return true;
  }
  if (d->replayDrainLedger.terminal()) {
    noteDrainFenceMode(DrainFenceMode::Terminal);
    return false;
  }
  if (!d->replayOffload) {
    return true;
  }
  auto& queue = d->replayOffload->queue();
  if (queue.depth() == 0) {
    return !queue.stopped() && !d->replayOffload->failed();
  }
  dxmt9::perf::countOffloadDrainFenceWait();
  const auto waitStart = std::chrono::steady_clock::now();
  queue.waitDrained();
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - waitStart).count());
  dxmt9::perf::countOffloadDrainFenceCpuTime(elapsed);
  noteDrainFenceMode(DrainFenceMode::GlobalWait, elapsed);
  if (drainSiteAttributionEnabled()) {
    noteDrainSite(site, elapsed);
  }
  const bool caughtUp = !queue.stopped() &&
                        !d->replayOffload->failed() &&
                        !d->replayDrainLedger.terminal();
  if (!caughtUp) {
    noteDrainFenceMode(DrainFenceMode::Terminal);
  }
  return caughtUp;
}

bool replayTerminal(D9CDevice* d) noexcept {
  return d && d->replayDrainLedger.terminal();
}

bool replayTerminal(D9CBuffer* b) noexcept {
  return replayTerminal(b ? b->device : nullptr);
}

bool replayTerminal(D9CSwapChain* s) noexcept {
  return replayTerminal(s ? s->owner : nullptr);
}

bool replayTerminal(D9CTexture* t) noexcept {
  return replayTerminal(t ? t->device : nullptr);
}

bool replayTerminal(D9CSurface* s) noexcept {
  return replayTerminal(s ? s->device : nullptr);
}

bool replayTerminal(D9CShader* s) noexcept {
  return replayTerminal(s ? s->device : nullptr);
}

bool replayTerminal(D9CVertexDecl* d) noexcept {
  return replayTerminal(d ? d->device : nullptr);
}

bool replayTerminal(D9CQuery* q) noexcept {
  return replayTerminal(q ? q->device : nullptr);
}

bool bufferLockClassBypassesReplay(const D9CBufferDesc& desc,
                                   std::uint32_t lockFlags,
                                   bool dynamicRenameEnabled) noexcept {
  constexpr std::uint32_t kD3DUsageDynamic = 0x00000200u;
  constexpr std::uint32_t kD3DPoolDefault = 0u;
  const bool noOverwrite = (lockFlags & kD3DLockNoOverwrite) != 0u;
  const bool validRename = dynamicRenameEnabled &&
      (lockFlags & kD3DLockDiscard) != 0u &&
      desc.pool == kD3DPoolDefault &&
      (desc.usage & kD3DUsageDynamic) != 0u;
  return noOverwrite || validRename;
}

bool drainDeferredReplayForBufferLock(D9CBuffer* b,
                                      std::uint32_t lockFlags) {
  if (!b || !b->device) {
    return true;
  }
  auto& ledger = b->device->replayDrainLedger;
  if (ledger.terminal()) {
    noteDrainFenceMode(DrainFenceMode::Terminal);
    drainDeferredReplay(b->device, "dxmt9c_buffer_lock_fallback");
    return false;
  }
  const auto upper = b->device->dev().upperDevice();
  const bool noOverwrite = (lockFlags & kD3DLockNoOverwrite) != 0u;
  const bool noWaitClass = bufferLockClassBypassesReplay(
      b->desc, lockFlags,
      upper && upper->dynamicBufferRenameEnabled());
  if (noWaitClass) {
    noteDrainFenceMode(noOverwrite ? DrainFenceMode::BypassNoOverwrite
                                   : DrainFenceMode::BypassDiscard);
    return true;
  }
  if (!ledger.pending(*b->replayDrainTarget)) {
    const bool unrelatedWork =
        b->device->replayOffload &&
        b->device->replayOffload->queue().depth() != 0u;
    noteDrainFenceMode(unrelatedWork ? DrainFenceMode::ScopedUnrelated
                                     : DrainFenceMode::ScopedClear);
    return true;
  }
  dxmt9::perf::countOffloadDrainFenceWait();
  const auto waitStart = std::chrono::steady_clock::now();
  const auto result = ledger.wait(*b->replayDrainTarget);
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - waitStart).count());
  dxmt9::perf::countOffloadDrainFenceCpuTime(elapsed);
  noteDrainFenceMode(DrainFenceMode::ScopedWait, elapsed);
  if (drainSiteAttributionEnabled()) {
    noteDrainSite("dxmt9c_buffer_lock", elapsed);
    noteBlockedLockClass(lockFlags, b->desc.pool, elapsed);
  }
  if (result != ReplayDrainResult::CaughtUp) {
    noteDrainFenceMode(DrainFenceMode::Terminal);
    drainDeferredReplay(b->device, "dxmt9c_buffer_lock_fallback");
    return false;
  }
  return true;
}

bool drainDeferredReplayForBufferUnlock(D9CBuffer* b) {
  if (!b || !b->device) {
    return true;
  }
  auto& ledger = b->device->replayDrainLedger;
  if (ledger.terminal()) {
    noteDrainFenceMode(DrainFenceMode::Terminal);
    drainDeferredReplay(b->device, "dxmt9c_buffer_unlock_fallback");
    return false;
  }
  const bool noOverwrite =
      (b->lastLockFlags & kD3DLockNoOverwrite) != 0u;
  const auto upper = b->device->dev().upperDevice();
  const bool noWaitClass =
      b->lastLockSucceeded && bufferLockClassBypassesReplay(
          b->desc, b->lastLockFlags,
          upper && upper->dynamicBufferRenameEnabled());
  if (noWaitClass) {
    noteDrainFenceMode(noOverwrite ? DrainFenceMode::BypassNoOverwrite
                                   : DrainFenceMode::BypassDiscard);
    return true;
  }
  if (!ledger.pending(*b->replayDrainTarget)) {
    const bool unrelatedWork =
        b->device->replayOffload &&
        b->device->replayOffload->queue().depth() != 0u;
    noteDrainFenceMode(unrelatedWork ? DrainFenceMode::ScopedUnrelated
                                     : DrainFenceMode::ScopedClear);
    return true;
  }
  dxmt9::perf::countOffloadDrainFenceWait();
  const auto waitStart = std::chrono::steady_clock::now();
  const auto result = ledger.wait(*b->replayDrainTarget);
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - waitStart).count());
  dxmt9::perf::countOffloadDrainFenceCpuTime(elapsed);
  noteDrainFenceMode(DrainFenceMode::ScopedWait, elapsed);
  if (drainSiteAttributionEnabled()) {
    noteDrainSite("dxmt9c_buffer_unlock", elapsed);
  }
  if (result != ReplayDrainResult::CaughtUp) {
    noteDrainFenceMode(DrainFenceMode::Terminal);
    drainDeferredReplay(b->device, "dxmt9c_buffer_unlock_fallback");
    return false;
  }
  return true;
}

bool drainDeferredReplay(D9CBuffer* b, const char* site) {
  if (!b) {
    return true;
  }
  return drainDeferredReplay(b->device, site);
}

bool drainDeferredReplay(D9CSwapChain* s, const char* site) {
  if (!s) {
    return true;
  }
  return drainDeferredReplay(s->owner, site);
}

bool drainDeferredReplay(D9CTexture* t, const char* site) {
  if (!t) {
    return true;
  }
  return drainDeferredReplay(t->device, site);
}

bool drainDeferredReplay(D9CSurface* s, const char* site) {
  if (!s) {
    return true;
  }
  return drainDeferredReplay(s->device, site);
}

bool drainDeferredReplay(D9CQuery* q, const char* site) {
  if (!q) {
    return true;
  }
  return drainDeferredReplay(q->device, site);
}

bool drainDeferredReplay(D9CStateBlock* sb, const char* site) {
  if (!sb) {
    return true;
  }
  return drainDeferredReplay(sb->device, site);
}

}  // namespace dxmt9::d3d9
