#include "dxmt9_command_queue.hpp"
#include "dxmt9_queue_mutex_diag.hpp"
#include "render/backend_factory.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_archive_prewarm.hpp"
#include "dxmt9_debug_alloc_guard.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_device.hpp"
#include "dxmt9_blit_encoders.hpp"
#include "dxmt9_compat.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_state.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_presenter.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_resource_initializer.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "framegraph/fg_builder.hpp"
#include "util/log/log.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace dxmt9 {

namespace {

void recordCpuReadyTapeStats(const core::CpuReadyTape& tape) {
  const auto& stats = tape.stats();
  perf::recordCpuReadyTapeStats(
      stats.residentSources, stats.residentPages, stats.readyFifoEntries,
      stats.admissionCloses, stats.admissionReopens, stats.wrapPaddingPages);
}

}  // namespace

// Per-call-site attribution for CommandQueue::mutex_ contention
// (DXMT9_PERF_QUEUE_MUTEX_SPLIT). GT2 measurement showed the producer's
// commit-time mark phase alone spends 38.7us/call (0.617ms/present) just
// ACQUIRING mutex_ (commit_chunk_phase_mark_lock_cpu_ms); mutex_ is shared
// by the game thread (mark/capture, ~16 calls/present), the replay offload
// worker (submit* et al., hundreds of calls/present), the encode thread, and
// GPU completion handlers. This diagnostic buckets both acquire-wait and
// hold duration by call site so a fix can be targeted instead of guessed.
//
// Follows the same discipline as the drain-fence-site sink
// (DXMT9_PERF_DRAIN_FENCE_SITES, src/d3d9/device_c_replay_offload.cpp):
// bucket on the `site` string-literal POINTER, never hash contents — every
// caller here passes a string literal, so identity comparison is exact and
// costs one compare. A site that overflows the fixed table folds into an
// overflow row instead of being silently dropped.
//
// Hold-time policy: many CommandQueue::mutex_ sites hand their
// std::unique_lock to a condition_variable::wait(), or to a helper
// (ensureWritingSlotUnlocked / maybeCommitDrawPayloadArenaUnlocked /
// maybeCommitDrawChunkUnlocked / QueueLifecycleController methods) that may
// unlock/relock it an unbounded number of times before returning. For those
// sites "hold time" has no single meaningful duration, so this guard
// records ONLY the acquire-wait for the very first lock() and skips hold
// accounting entirely (skipHold=true) rather than guess at a number. Sites
// whose entire critical section is a single unbroken RAII scope (no manual
// unlock()/lock(), no cv wait, no lock handed to another function) get full
// acquire-wait + hold accounting.
namespace {

// Raised 64 -> 96 alongside the SEGMENT-HOLD extension below: the acquire-side
// sites already used ~30 of the original 64 slots, and per-segment tagging
// (e.g. "run_finish_loop/dequeue", "run_finish_loop/retire",
// "run_encode_loop/dequeue") adds several more distinct site strings without
// removing any existing row.
constexpr std::size_t kQueueMutexSiteSlots = 96;

struct QueueMutexSiteTable {
  std::mutex tableMutex;
  const char* names[kQueueMutexSiteSlots]{};
  std::uint64_t acquires[kQueueMutexSiteSlots]{};
  std::uint64_t acquireWaitNanos[kQueueMutexSiteSlots]{};
  std::uint64_t holdSamples[kQueueMutexSiteSlots]{};
  std::uint64_t holdNanos[kQueueMutexSiteSlots]{};
  std::uint64_t maxHoldNanos[kQueueMutexSiteSlots]{};
  std::size_t used = 0;
  std::uint64_t overflowAcquires = 0;
  std::uint64_t overflowAcquireWaitNanos = 0;
};

QueueMutexSiteTable& queueMutexSiteTable() {
  static QueueMutexSiteTable table;
  return table;
}

}  // namespace

// queueMutexSplitEnabled() / noteQueueMutexSite() / queueMutexProbeNanos()
// have external (dxmt9::) linkage -- not anonymous-namespace-local -- because
// dxmt9_queue.cpp (QueueLifecycleController, a different translation unit)
// also needs to record SEGMENT-HOLD samples at the interior unlock/relock and
// cv-wait boundaries it owns. See dxmt9_queue_mutex_diag.hpp for the shared
// declarations and the QueueMutexSegmentScope/noteQueueMutexSegmentIfEnabled
// helpers built on top of these three primitives.
bool queueMutexSplitEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_QUEUE_MUTEX_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

void noteQueueMutexSite(const char* site, std::uint64_t acquireWaitNs,
                        bool haveHold, std::uint64_t holdNs) {
  auto& t = queueMutexSiteTable();
  std::lock_guard tableLock(t.tableMutex);
  for (std::size_t i = 0; i < t.used; ++i) {
    if (t.names[i] == site) {
      ++t.acquires[i];
      t.acquireWaitNanos[i] += acquireWaitNs;
      if (haveHold) {
        ++t.holdSamples[i];
        t.holdNanos[i] += holdNs;
        if (holdNs > t.maxHoldNanos[i]) t.maxHoldNanos[i] = holdNs;
      }
      return;
    }
  }
  if (t.used < kQueueMutexSiteSlots) {
    t.names[t.used] = site;
    t.acquires[t.used] = 1;
    t.acquireWaitNanos[t.used] = acquireWaitNs;
    if (haveHold) {
      t.holdSamples[t.used] = 1;
      t.holdNanos[t.used] = holdNs;
      t.maxHoldNanos[t.used] = holdNs;
    }
    ++t.used;
    return;
  }
  ++t.overflowAcquires;
  t.overflowAcquireWaitNanos += acquireWaitNs;
}

std::uint64_t queueMutexProbeNanos(std::chrono::steady_clock::duration d) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}

namespace {

// Token produced immediately before a site's real std::lock_guard /
// std::unique_lock construction (or, for a std::defer_lock site, before its
// explicit lock() call). Deliberately a plain trivially-copyable struct, not
// a clock read wrapped in a branch every time: when the split is disabled
// `enabled` is false and no clock is ever touched, matching the "near-zero
// cost when off -- one branch per site, no clock" requirement.
struct QueueMutexBeginToken {
  bool enabled = false;
  std::chrono::steady_clock::time_point t0{};
};

QueueMutexBeginToken queueMutexProbeBegin() {
  if (!queueMutexSplitEnabled()) {
    return {};
  }
  return {true, std::chrono::steady_clock::now()};
}

// RAII probe placed immediately AFTER the site's real lock object has
// acquired the mutex (i.e. right after `std::lock_guard lock(mutex_);` /
// `std::unique_lock lock(mutex_);` / the explicit `lock.lock()` on a
// std::defer_lock site). It never touches mutex_ itself -- the real lock
// object still owns and releases it exactly as before.
//
// Declared textually AFTER the real lock in the same scope, this probe is
// destroyed BEFORE the real lock object at every scope exit (including
// early returns), by ordinary C++ reverse-construction-order destruction.
// So for a simple site (skipHold=false, no manual unlock()/lock(), no cv
// wait, lock never hand to another function) the probe's destructor fires
// an instant before the real unlock -- close enough to the true hold
// duration for contention triage. Sites that violate that assumption are
// marked skipHold=true at construction and this probe then records only the
// acquire-wait already captured at construction time.
class QueueMutexProbeScope {
 public:
  QueueMutexProbeScope(QueueMutexBeginToken begin, const char* site,
                       bool skipHold = false)
      : site_(site), skipHold_(skipHold), enabled_(begin.enabled) {
    if (!enabled_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    acquireWaitNs_ = queueMutexProbeNanos(now - begin.t0);
    holdStart_ = now;
  }

  QueueMutexProbeScope(const QueueMutexProbeScope&) = delete;
  QueueMutexProbeScope& operator=(const QueueMutexProbeScope&) = delete;

  ~QueueMutexProbeScope() {
    if (!enabled_) {
      return;
    }
    if (skipHold_) {
      noteQueueMutexSite(site_, acquireWaitNs_, /*haveHold=*/false, 0);
    } else {
      const auto holdNs =
          queueMutexProbeNanos(std::chrono::steady_clock::now() - holdStart_);
      noteQueueMutexSite(site_, acquireWaitNs_, /*haveHold=*/true, holdNs);
    }
  }

 private:
  const char* site_;
  bool skipHold_;
  bool enabled_;
  std::uint64_t acquireWaitNs_ = 0;
  std::chrono::steady_clock::time_point holdStart_{};
};

}  // namespace

void logQueueMutexSites(std::uint64_t presents) {
  if (!queueMutexSplitEnabled()) {
    return;
  }
  auto& t = queueMutexSiteTable();
  std::lock_guard tableLock(t.tableMutex);
  const double p = presents ? static_cast<double>(presents) : 1.0;
  for (std::size_t i = 0; i < t.used; ++i) {
    dxmt9::util::logf(
        dxmt9::util::LogLevel::Info, "dxmt9-queue-mutex",
        "site=%s acquires=%llu acquires_per_present=%.3f "
        "acquire_wait_ms=%.3f acquire_wait_ms_per_present=%.4f "
        "hold_ms=%.3f hold_samples=%llu max_hold_us=%.1f",
        t.names[i] ? t.names[i] : "untagged",
        static_cast<unsigned long long>(t.acquires[i]),
        static_cast<double>(t.acquires[i]) / p,
        static_cast<double>(t.acquireWaitNanos[i]) / 1.0e6,
        static_cast<double>(t.acquireWaitNanos[i]) / 1.0e6 / p,
        static_cast<double>(t.holdNanos[i]) / 1.0e6,
        static_cast<unsigned long long>(t.holdSamples[i]),
        t.holdSamples[i] ? static_cast<double>(t.maxHoldNanos[i]) / 1.0e3
                         : 0.0);
  }
  if (t.overflowAcquires) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-queue-mutex",
                      "site=<overflow> acquires=%llu acquire_wait_ms=%.3f "
                      "(raise kQueueMutexSiteSlots)",
                      static_cast<unsigned long long>(t.overflowAcquires),
                      static_cast<double>(t.overflowAcquireWaitNanos) / 1.0e6);
  }
}

namespace {
// Tiny helper that uploads a printf-formatted label to the bridge as an
// autoreleased NSString, then returns a non-owning WMT::String view. The
// NSString lives on the autoreleasepool of the encoding thread — long
// enough for the setLabel: selector to retain it, but never longer.
template <std::size_t Cap = 96>
WMT::String makeLabelStringFmt(const char* fmt, ...) {
  char buf[Cap];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return WMT::String::string(buf, WMTUTF8StringEncoding);
}

std::atomic_uint64_t gCommandBufferLabelCounter{0};

[[noreturn]] void abortDceLookaheadPendingFailOpen(const char* reason) {
  std::fprintf(stderr,
               "[dxmt9-queue] fatal: encoded DCE lookahead prefix could not "
               "be finalized (%s)\n",
               reason ? reason : "unknown");
  std::abort();
}

bool drawRunGroupByGenerationLaneEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_DRAWRUN_GROUP_BY_GEN_LANE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool renderTapeCaptureEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_RENDER_TAPE_CAPTURE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool encodeSlotPsoPrefetchDisabled() {
  static const bool disabled = [] {
    const char* env = std::getenv("DXMT9_DISABLE_ENCODE_SLOT_PSO_PREFETCH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return disabled;
}

bool encodeSlotPsoPrefetchEnabled() {
  return !encodeSlotPsoPrefetchDisabled();
}

bool unpublishedSlotPsoPrefetchEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PREFETCH_UNPUBLISHED_SLOT_PSO");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

void traceEncodeFnStage(const char* stage,
                        std::size_t slotIndex,
                        const core::ChunkSlot& slot) {
  if (!core::metalqueue::queueTraceEnabled()) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-encodefn]"
      << " stage=" << stage
      << " seq=" << static_cast<unsigned long long>(slot.seqId)
      << " slot=" << slotIndex
      << " commands=" << slot.commandCount()
      << " prefetch_sealed=" << (slot.prefetchedPipelinesSealed() ? 1 : 0);
  core::metalqueue::emitQueueTraceLine(out.str());
}

void tracePsoPrefetchStage(const char* stage,
                           const core::ChunkSlot& slot,
                           std::size_t commandIndex = std::numeric_limits<std::size_t>::max()) {
  if (!core::metalqueue::queueTraceEnabled()) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-pso-prefetch]"
      << " stage=" << stage
      << " seq=" << static_cast<unsigned long long>(slot.seqId)
      << " commands=" << slot.commandCount();
  if (commandIndex != std::numeric_limits<std::size_t>::max()) {
    out << " command=" << commandIndex;
  }
  core::metalqueue::emitQueueTraceLine(out.str());
}

bool encodeSlotPsoProbeKeyMemoDisabled() {
  static const bool disabled = [] {
    const char* env = std::getenv("DXMT9_DISABLE_ENCODE_SLOT_PSO_PROBE_KEY_MEMO");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return disabled;
}

bool encodeSlotPsoSemanticMemoDisabled() {
  static const bool disabled = [] {
    const char* env = std::getenv("DXMT9_DISABLE_ENCODE_SLOT_PSO_SEMANTIC_MEMO");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return disabled;
}

bool encodeSlotPsoSemanticSplitEnabled() {
  static const bool enabled = [] {
    const char* env =
        std::getenv("DXMT9_PERF_ENCODE_SLOT_PSO_SEMANTIC_SPLIT");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool encodeSlotPsoSemanticMissSplitEnabled() {
  static const bool enabled = [] {
    const char* env =
        std::getenv("DXMT9_PERF_ENCODE_SLOT_PSO_SEMANTIC_MISS_SPLIT");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool encodeSlotPsoResourceShapeOpportunityEnabled() {
  static const bool enabled = [] {
    const char* env =
        std::getenv("DXMT9_PERF_ENCODE_SLOT_PSO_RESOURCE_SHAPE_OPPORTUNITY");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool encodeSlotPsoResourceShapeMemoEnabled() {
  static const bool enabled = [] {
    const char* disableEnv =
        std::getenv("DXMT9_DISABLE_ENCODE_SLOT_PSO_RESOURCE_SHAPE_MEMO");
    return !(disableEnv && disableEnv[0] != '\0' &&
             std::strcmp(disableEnv, "0") != 0);
  }();
  return enabled;
}

enum class QueueWorkerRole {
  Encode,
  Finish,
  Completion,
};

#if defined(__APPLE__)
struct QueueWorkerThreadProfile {
  const char* name;
  qos_class_t qos;
};

QueueWorkerThreadProfile queueWorkerThreadProfile(QueueWorkerRole role) noexcept {
  switch (role) {
    case QueueWorkerRole::Encode:
      return {.name = "dxmt9-encode", .qos = QOS_CLASS_USER_INITIATED};
    case QueueWorkerRole::Finish:
      return {.name = "dxmt9-finish", .qos = QOS_CLASS_DEFAULT};
    case QueueWorkerRole::Completion:
      return {.name = "dxmt9-completion", .qos = QOS_CLASS_UTILITY};
  }
  return {.name = "dxmt9-worker", .qos = QOS_CLASS_DEFAULT};
}
#endif

void configureQueueWorkerThread(QueueWorkerRole role) noexcept {
#if defined(__APPLE__)
  const auto profile = queueWorkerThreadProfile(role);
  pthread_setname_np(profile.name);
  (void)pthread_set_qos_class_self_np(profile.qos, 0);
#else
  (void)role;
#endif
}

std::function<void()> makeQueueWorkerLoop(QueueWorkerRole role,
                                          std::function<void()> loop) {
  return [role, loop = std::move(loop)]() mutable {
    configureQueueWorkerThread(role);
    loop();
  };
}

// R-BACK-3.7 / R-BACK-3.8 / R-BACK-4.8 — archive path resolution moved
// into archive_prewarm. The path now embeds the dxmt9 archive ABI
// version and the sanitized GPU family token so cross-process readers
// never observe an archive that was serialized against an incompatible
// emitter / variant-key encoding (design §6.1).
std::string resolveShaderCachePath(WMT::Device device) {
  const auto mode = archive_prewarm::resolveMode();
  return archive_prewarm::resolveArchivePath(device, mode);
}

bool surfaceAliasesTracedTexture(resources::Pool& pool, core::Handle handle) {
  const auto wanted = debug::traceTextureHandle();
  if (wanted == 0 || !handle) {
    return false;
  }
  const auto* surface = pool.findSurface(handle.value);
  return surface && surface->aliasTexture.value == wanted;
}

void traceTextureSurfaceOp(resources::Pool& pool, const char* op,
                           core::Handle a = {}, core::Handle b = {},
                           core::Handle c = {}) {
  if (!op ||
      (!surfaceAliasesTracedTexture(pool, a) &&
       !surfaceAliasesTracedTexture(pool, b) &&
       !surfaceAliasesTracedTexture(pool, c))) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-texture] surface-op " << op;
  if (a) out << " a=0x" << std::hex << a.value << std::dec;
  if (b) out << " b=0x" << std::hex << b.value << std::dec;
  if (c) out << " c=0x" << std::hex << c.value << std::dec;
  core::metalqueue::emitTextureTraceLine(out.str());
}

void traceTextureClear(resources::Pool& pool, const core::ClearDesc& desc) {
  bool tracesTexture = false;
  for (const auto& attachment : desc.colorAttachments) {
    tracesTexture = tracesTexture || surfaceAliasesTracedTexture(pool, attachment.handle);
  }
  tracesTexture = tracesTexture || surfaceAliasesTracedTexture(pool, desc.depthStencil.handle);
  if (!tracesTexture) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-texture] clear flags color=" << (desc.clearColor ? 1 : 0)
      << " depth=" << (desc.clearDepth ? 1 : 0)
      << " stencil=" << (desc.clearStencil ? 1 : 0)
      << " rgba=(" << desc.color.r << "," << desc.color.g << ","
      << desc.color.b << "," << desc.color.a << ")"
      << " depthValue=" << desc.depth
      << " stencilValue=" << desc.stencil
      << " rects=" << desc.rects.size();
  core::metalqueue::emitTextureTraceLine(out.str());
}

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

CommandQueue::CommandQueue(WMT::Device device, core::BackendLimits limits,
                           bool cpuReadySessionLaneEnabled,
                           bool renderTapePublisherCaptureEnabled,
                           render::RenderPartitionConfig renderPartitionConfig)
    : cpuReadyTape_(renderTapePublisherCaptureEnabled
                        ? core::CpuReadyTapeConfig::queueCaptureStreaming(
                              kCommandChunkCount)
                    : cpuReadySessionLaneEnabled
                        ? core::CpuReadyTapeConfig::queueSessionStreaming(
                              kCommandChunkCount)
                        : core::CpuReadyTapeConfig::queueCompatibility(
                              kCommandChunkCount)),
      device_(device),
      cpuReadySessionLaneEnabled_(cpuReadySessionLaneEnabled),
      renderPartitionConfig_(renderPartitionConfig),
      limits_(limits),
      // R-BACK-3.9 — Mode::Full defers the archive load: shaderArchive_
      // starts default-constructed (empty, zero I/O) here and the real
      // load is kicked off asynchronously in the ctor body below. Lazy
      // and Disabled keep the original synchronous ctor call verbatim
      // ("device init behavior with DXMT9_PREWARM=disabled|lazy
      // unchanged"). The ternary only evaluates the selected branch, so
      // resolveShaderCachePath() (which itself calls resolveMode()) is
      // not invoked at all for Full mode.
      shaderArchive_(archive_prewarm::resolveMode() == archive_prewarm::Mode::Full
                         ? shaders::Archive()
                         : shaders::Archive(device, resolveShaderCachePath(device))) {
  setRenderTapeExactAttachmentPreservation(renderTapeCaptureEnabled());
  if (!device_) {
    return;
  }
  // R-BACK-3.7 / R-BACK-3.8 / R-BACK-3.9 — drive the prewarm step now that
  // the archive instance is constructed. Lazy/Disabled run the original
  // synchronous §6.1 failure-mode table (missing / lock_busy / entries /
  // bytes / load_ns counters), unchanged. Full mode instead begins the
  // async load: shaderArchive_ stays empty — pipeline-cache builders
  // already treat an empty archive as "compile fresh, no archive attach"
  // — until the background thread attaches it; archive-add side effects
  // that race the load are queued and replayed on attach
  // (dxmt9_pipeline_cache.cpp's submitPipelineBuild /
  // dxmt9_archive_prewarm.cpp's queueArchiveBackfill). Neither path
  // blocks queue init beyond a single bounded flock retry budget /
  // a stat() call.
  {
    const auto prewarmMode = archive_prewarm::resolveMode();
    if (prewarmMode == archive_prewarm::Mode::Full) {
      shaderArchive_.beginAsyncFullLoad(
          device, archive_prewarm::resolveArchivePath(device, prewarmMode));
    } else {
      archive_prewarm::run(device, shaderArchive_.path(), prewarmMode);
    }
    // R-BACK-3.11 — a non-default shader debug-env key (the
    // DXMT_DISABLE_*/DXMT_FORCE_*/DXMT9_PROBE_* classifier family) means
    // this session's compiled shader variants must not land in the
    // shared production archive. Gate save only; load above still
    // proceeds normally so a probe session still benefits from a warm
    // cache built by prior, non-diagnostic sessions.
    if (!pipeline::shaderSourceDebugEnvIsDefault()) {
      shaderArchive_.poisonSave();
    }
  }
  queue_ = device_.newCommandQueue(0);
  if (!queue_) {
    return;
  }
  queueView_ = WMT::CommandQueue{queue_.handle};
  // M1: name the queue so frame captures don't show a generic
  // <CAMetalQueue: 0x...>. deviceId is the underlying MTLDevice handle —
  // good enough to disambiguate multi-GPU configs in Xcode captures.
  queueView_.setLabel(makeLabelStringFmt("dxmt9-q-0x%llx",
      static_cast<unsigned long long>(device_.handle)));
  transientArena_.init(device_);

  // R-BACK-5.7: probe `MTLDevice.hasUnifiedMemory` ONCE at queue/device
  // init and cache the result on the resource pool. Per-resource code
  // (createTexture, stageTextureUpload, …) reads `pool_.hasUnifiedMemory_`
  // — never re-queries Metal — so the storage-mode and staging-blit
  // decisions stay one-shot per resource and consistent for that
  // resource's lifetime.
  pool_.setHasUnifiedMemory(device_.hasUnifiedMemory());
  // R-BACK-13.* — cache Apple3 GPU family support so the tile-shader FFP
  // selector can gate on a single bool. WMTGPUFamilyApple3 is the floor
  // for `MTLTileRenderPipelineDescriptor` and programmable blending.
  pool_.setSupportsApple3(device_.supportsFamily(WMTGPUFamilyApple3));
  // R-BACK-12.22 — Stage 2 argument-buffer hybrid capability gate. Enable
  // only when the device supports Tier-2 argbufs AND lives on Apple3+.
  // Probed once and cached on the pool so per-encoder selection is a
  // single bool read with no Metal traffic.
  // DXMT9_DISABLE_ARGBUF_HYBRID escape hatch: forces the gate off so a
  // suspected Stage 2 regression can be tested in isolation against the
  // Stage 1 baseline without rebuilding.
  {
    const auto tier = device_.argumentBuffersSupport();
    const bool tierOk = tier >= WMTArgumentBuffersTier2;
    const char* disableEnv = std::getenv("DXMT9_DISABLE_ARGBUF_HYBRID");
    const bool disabled =
        disableEnv && disableEnv[0] != '\0' && std::strcmp(disableEnv, "0") != 0;
    pool_.setArgbufHybridEnabled(!disabled && tierOk && pool_.supportsApple3());
  }
  // R-BACK-12.22 / 12.24 — build the queue-owned MTLArgumentEncoder when
  // the capability gate held. The encoder is shared across every render
  // pass on this queue; per-pass `openArgbuf` retargets it onto the
  // freshly reserved transient storage. When the gate failed, the
  // resource stays uninitialized and `openArgbuf` returns an empty
  // handle — the Stage 1 binding path then runs unchanged. Skip on a
  // sentinel-null device (test/fake-backend fixtures); ArgbufEncoderResource::init
  // tolerates a null handle but the gate above already short-circuits.
  if (pool_.argbufHybridEnabled() && device_) {
    argbufEncoderResource_.init(device_);
  }
  // R-BACK-12.22..12.26 (resource-array sub-mode) — opt-in second lane.
  // Build the 20-entry resource-array encoder ONLY when the constants-only
  // Stage 2 gate held AND the DXMT9_ARGBUF_RESOURCE_ARRAY env flag is set.
  // Default off: resourceArrayLaneActive_ stays false, the second encoder
  // stays uninitialized, and every Stage 2 pass uses the byte-identical
  // constants-only encoder. When on, the encoder thread selects this
  // encoder + the resource-array PSO bit per pass.
  resourceArrayLaneActive_ =
      pool_.argbufHybridEnabled() && shaders::argbufResourceArrayEnabled();
  if (resourceArrayLaneActive_ && device_) {
    resourceArrayEncoderResource_.initResourceArray(device_);
    // If the extended encoder failed to build (driver rejected the 20-entry
    // table), fall back to the constants-only lane rather than half-enable.
    if (!resourceArrayEncoderResource_.initialized()) {
      resourceArrayLaneActive_ = false;
    }
  }
  // R-BACK-14.* — bind the small-resource heap manager to the same
  // WMT::Device + unified-memory probe used by the pool's storage-mode
  // selectors. Init must run before initializer_ / encode loops because
  // the very first createTexture / createBuffer can reach the heap path.
  pool_.heapManager().init(device_, pool_.hasUnifiedMemory_);

  initializer_ = std::make_unique<resources::Initializer>(*this, pool_, device_);

  // Bind queueLifecycle_ to our own state + a pool-based surface-compat
  // hook. CommandQueue is its own lifecycle root.
  bindSelfLifecycle([this](core::Handle h) -> std::uint32_t {
    if (!h) return 0;
    auto* surface = pool_.findSurface(h.value);
    if (!surface) return 0;
    return core::metalcompat::isFloatRenderTargetFormat(surface->desc.format)
        ? static_cast<std::uint32_t>(core::metalcompat::CompatFlagBits::CompatFlagFp16)
        : 0u;
  });

  // R-BACK-31.7 — construct the render backend before the worker threads
  // start, so backend_ is non-null the first time the encode loop runs.
  // With DXMT9_RENDER_MODE unset this resolves to the promoted
  // FrameGraphBackend passcoalesce lane. Explicit "traditional" retains the
  // source-order byte-identical rollback.
  backend_ = render::createBackendFromEnv();

  // Spawn the three worker threads. Threads block on writeCv_ until the
  // first submit; no race with DeviceImpl's still-completing ctor because
  // submits can only happen after CreateDXMT9Device returns.
  startThreads(
      [this] {
        auto encodeSingleSource = [this](
                                          const core::metalqueue::ReadySlotSnapshot& source,
                                          const core::SourcePayloadView& payload) {
              const auto* legacySlot = payload.legacyPayload();
              if (!legacySlot) {
                auto ctx = makeEncodeContext();
                encoders::EncodeChunkOptions options{};
                options.partitionExecutionMode =
                    renderPartitionConfig_.resolved;
                options.partitionSource = core::CpuReadyTape::SourceRef{
                    .id = source.sourceId,
                    .storage = source.storage,
                };
                return backend_->onSourceReady(
                    ctx, source.slotIndex, payload, source.seqId,
                    std::move(options));
              }
              auto& slot = *const_cast<core::ChunkSlot*>(legacySlot);
              const std::size_t slotIndex = source.slotIndex;
              traceEncodeFnStage("entry", slotIndex, slot);
              if (encodeSlotPsoPrefetchEnabled() &&
                  !slot.prefetchedPipelinesSealed()) {
                traceEncodeFnStage("before-pso-prefetch", slotIndex, slot);
                PerfScope scope(perf::countEncodeSlotPsoPrefetchCpuTime);
                prefetchSlotPipelines(slot);
                traceEncodeFnStage("after-pso-prefetch", slotIndex, slot);
              }
              traceEncodeFnStage("before-make-context", slotIndex, slot);
              auto ctx = makeEncodeContext();
              traceEncodeFnStage("after-make-context", slotIndex, slot);
              traceEncodeFnStage("before-backend-onChunkReady", slotIndex, slot);
              encoders::EncodeChunkOptions options{};
              options.partitionExecutionMode =
                  renderPartitionConfig_.resolved;
              options.partitionSource = core::CpuReadyTape::SourceRef{
                  .id = source.sourceId,
                  .storage = source.storage,
              };
              auto submission = backend_->onChunkReady(
                  ctx, slotIndex, slot, std::move(options));
              traceEncodeFnStage(submission.has_value()
                                     ? "after-backend-onChunkReady-submission"
                                     : "after-backend-onChunkReady-inline",
                                 slotIndex,
                                 slot);
              return submission;
            };
        if (backend_->wantsNextChunkLookahead()) {
          runDceChunkLookaheadEncodeLoop(
              [this](std::uint64_t) { allocators_.reclaim(completedSeqId_); });
          return;
        }
        if (cpuReadySessionLaneEnabled_) {
          runCpuReadySessionEncodeLoop(
              [this](std::uint64_t) { allocators_.reclaim(completedSeqId_); });
          return;
        }
        runEncodeLoop(
            encodeSingleSource,
            [this](std::uint64_t) { allocators_.reclaim(completedSeqId_); });
      },
      [this] { runFinishLoop(); },
      [this] { runCompletionWatcherLoop(); });
}

CommandQueue::CommandQueue(InertTestQueueTag,
                           WMT::Reference<WMT::CommandQueue> queue,
                           core::BackendLimits limits)
    : cpuReadySessionLaneEnabled_(false),
      queue_(std::move(queue)),
      queueView_(queue_.handle),
      limits_(limits) {}

CommandQueue::CommandQueue(ArenaLeaseTestQueueTag,
                           core::BackendLimits limits,
                           WMT::Reference<WMT::CommandQueue> queue)
    : cpuReadyTape_(core::CpuReadyTapeConfig::queueSessionStreaming(
          kCommandChunkCount)),
      cpuReadySessionLaneEnabled_(false), queue_(std::move(queue)),
      queueView_(queue_.handle), limits_(limits) {
  bindSelfLifecycle([](core::Handle) -> std::uint32_t { return 0; });
  stop_ = false;
}

encoders::EncodeContext CommandQueue::makeEncodeContext() {
  // Snapshot + reset the chunk-import dirty accumulator. The dirty
  // bits and high-water counters move to the freshly-built
  // EncodeContext; the queue's pendingDirty_ resets to all-clean so the
  // next chunk's records start fresh. C2 still calls markAllDirty(...)
  // at encoder init per R-BACK-12.12 to fold in the implicit
  // "everything could have changed since last encode" semantic.
  // Note: device_ may be a sentinel-null handle for fake-backend test
  // fixtures (see tests/native/backend/resource_hazard_spec.cpp). The
  // real Metal-side dereference happens deep inside encoders::encodeChunk
  // / encoders::encodeDraw; assertion belongs there, not at context
  // creation.
  std::uint64_t transientCompletedSeqId = 0;
  {
    const auto qmxBegin = queueMutexProbeBegin();
    std::lock_guard lock(mutex_);
    QueueMutexProbeScope qmxScope(qmxBegin, "make_encode_context");
    transientCompletedSeqId = completedSeqId_;
  }
  auto ctx = encoders::EncodeContext{
      device_, limits_, pool_, pipelineCache_, allocators_,
      &shaderArchive_.reference(), &shaderArchive_.path(),
      *this,
      consumePendingDirty(),
  };
  ctx.transientCompletedSeqId = transientCompletedSeqId;
  ctx.drawRecorder = testOnlyDrawRecorder_;
  return ctx;
}

void CommandQueue::prefetchSlotPipelines(core::ChunkSlot& slot, bool seal) {
  DXMT_ASSERT(!slot.prefetchedPipelinesSealed() &&
              "prefetchSlotPipelines called after slot prefetch seal");
  tracePsoPrefetchStage("begin", slot);
  if (!device_) {
    if (seal) {
      slot.sealPrefetchedPipelines();
    }
    tracePsoPrefetchStage("no-device-sealed", slot);
    return;
  }
  const auto beginCommand = slot.prefetchedPipelineCommandCursor();
  const auto commandCount = slot.commandCount();
  if (beginCommand >= commandCount) {
    if (seal) {
      slot.sealPrefetchedPipelines();
    }
    tracePsoPrefetchStage(seal ? "end-sealed" : "end-partial", slot);
    return;
  }
  perf::countEncodeSlotPsoPrefetchCommands(
      static_cast<std::uint64_t>(commandCount - beginCommand));
  static constexpr std::size_t kDrawHandleReuseTableCapacity = 2048;
  static_assert((kDrawHandleReuseTableCapacity &
                 (kDrawHandleReuseTableCapacity - 1)) == 0,
                "draw handle reuse table capacity must be a power of two");
  static constexpr std::size_t kDrawProbeKeyMemoTableCapacity = 512;
  static_assert((kDrawProbeKeyMemoTableCapacity &
                 (kDrawProbeKeyMemoTableCapacity - 1)) == 0,
                "draw probe-key memo table capacity must be a power of two");
  static constexpr std::size_t kDrawSemanticMemoTableCapacity = 2048;
  static_assert((kDrawSemanticMemoTableCapacity &
                 (kDrawSemanticMemoTableCapacity - 1)) == 0,
                "draw semantic memo table capacity must be a power of two");
  static constexpr std::size_t kDrawResourceShapeMemoTableCapacity = 2048;
  static_assert((kDrawResourceShapeMemoTableCapacity &
                 (kDrawResourceShapeMemoTableCapacity - 1)) == 0,
                "draw resource-shape memo table capacity must be a power of two");
  struct DrawSemanticMemoKey {
    std::uint64_t fingerprint = 0;
    bool argbufHybridMode = false;
    bool argbufResourceArray = false;
    bool argbufDirectCbufMode = false;

    constexpr bool operator==(const DrawSemanticMemoKey&) const = default;
  };
  struct DrawSemanticMemoEntry {
    DrawSemanticMemoKey key{};
    const core::FlatDrawStateRecord* hot = nullptr;
    const core::DrawShaderLayoutContext* shaderLayout = nullptr;
    core::PsoHandle handle{};
    std::uint64_t epoch = 0;
  };
  struct DrawSemanticMemoLookup {
    core::PsoHandle handle{};
    std::size_t index = 0;
    bool hit = false;
    bool overflow = false;
  };
  struct DrawResourceShapeMemoEntry {
    DrawSemanticMemoKey key{};
    const core::FlatDrawStateRecord* hot = nullptr;
    const core::DrawShaderLayoutContext* shaderLayout = nullptr;
    pipeline::ShaderVariantKey probeKey{};
    core::PsoHandle handle{};
    std::uint64_t epoch = 0;
  };
  struct DrawResourceShapeMemoLookup {
    const DrawResourceShapeMemoEntry* entry = nullptr;
    std::size_t index = 0;
    bool hit = false;
    bool overflow = false;
  };
  struct DrawProbeKeyMemoEntry {
    pipeline::ShaderVariantKey probeKey{};
    DrawSemanticMemoKey semanticKey{};
    const core::FlatDrawStateRecord* hot = nullptr;
    const core::DrawShaderLayoutContext* shaderLayout = nullptr;
    core::PsoHandle handle{};
    std::uint64_t epoch = 0;
  };
  struct DrawProbeKeyMemoLookup {
    core::PsoHandle handle{};
    const DrawProbeKeyMemoEntry* entry = nullptr;
    std::size_t index = 0;
    bool hit = false;
    bool overflow = false;
  };
  struct DrawHandleReuseEntry {
    core::PsoHandle handle{};
    std::uint64_t epoch = 0;
  };
  static thread_local std::array<DrawHandleReuseEntry,
                                 kDrawHandleReuseTableCapacity>
      seenDrawPsoHandles{};
  static thread_local std::array<DrawSemanticMemoEntry,
                                 kDrawSemanticMemoTableCapacity>
      drawSemanticMemo{};
  static thread_local std::array<DrawProbeKeyMemoEntry,
                                 kDrawProbeKeyMemoTableCapacity>
      drawProbeKeyMemo{};
  static thread_local std::uint64_t drawPsoMemoNextEpoch = 0;
  std::uint64_t drawPsoMemoEpoch = ++drawPsoMemoNextEpoch;
  if (drawPsoMemoEpoch == 0) {
    for (auto& entry : seenDrawPsoHandles) {
      entry.epoch = 0;
    }
    for (auto& entry : drawSemanticMemo) {
      entry.epoch = 0;
    }
    for (auto& entry : drawProbeKeyMemo) {
      entry.epoch = 0;
    }
    drawPsoMemoEpoch = ++drawPsoMemoNextEpoch;
  }
  core::PsoHandle previousDrawPsoHandle{};
  bool hasPreviousDrawPsoHandle = false;
  const bool drawSemanticMemoEnabled = !encodeSlotPsoSemanticMemoDisabled();
  const bool drawProbeKeyMemoEnabled = !encodeSlotPsoProbeKeyMemoDisabled();
  const bool drawSemanticSplitEnabled =
      drawSemanticMemoEnabled && encodeSlotPsoSemanticSplitEnabled();
  const bool drawSemanticMissSplitEnabled =
      encodeSlotPsoSemanticMissSplitEnabled();
  const bool drawResourceShapeOpportunityEnabled =
      drawSemanticMemoEnabled && encodeSlotPsoResourceShapeOpportunityEnabled();
  const bool drawResourceShapeBehaviorEnabled =
      drawSemanticMemoEnabled && encodeSlotPsoResourceShapeMemoEnabled();
  const bool drawResourceShapeMemoEnabled =
      drawResourceShapeOpportunityEnabled || drawResourceShapeBehaviorEnabled;
  std::uint64_t drawResourceShapeMemoEpoch = 0;
  std::array<DrawResourceShapeMemoEntry,
             kDrawResourceShapeMemoTableCapacity>* drawResourceShapeMemo =
      nullptr;
  if (drawResourceShapeMemoEnabled) {
    static thread_local std::array<DrawResourceShapeMemoEntry,
                                   kDrawResourceShapeMemoTableCapacity>
        drawResourceShapeMemoScratch{};
    static thread_local std::uint64_t drawResourceShapeMemoNextEpoch = 0;
    drawResourceShapeMemo = &drawResourceShapeMemoScratch;
    drawResourceShapeMemoEpoch = ++drawResourceShapeMemoNextEpoch;
    if (drawResourceShapeMemoEpoch == 0) {
      for (auto& entry : drawResourceShapeMemoScratch) {
        entry.epoch = 0;
      }
      drawResourceShapeMemoEpoch = ++drawResourceShapeMemoNextEpoch;
    }
  }
  auto mixDrawSemanticHash = [](std::uint64_t seed, std::uint64_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6u) +
                   (seed >> 2u));
  };
  auto mixDrawSemanticHandle =
      [mixDrawSemanticHash](std::uint64_t seed, core::Handle handle) {
    return mixDrawSemanticHash(seed, handle.value);
  };
  auto mixDrawSemanticAttachment =
      [mixDrawSemanticHash, mixDrawSemanticHandle](
          std::uint64_t seed, const core::RenderTargetAttachment& attachment) {
    seed = mixDrawSemanticHandle(seed, attachment.handle);
    seed = mixDrawSemanticHash(seed, attachment.level);
    seed = mixDrawSemanticHash(seed, attachment.sampleCount);
    return seed;
  };
  auto vertexDeclPsoShapeEqual =
      [](const core::VertexDeclSnapshot& lhs,
         const core::VertexDeclSnapshot& rhs) {
    if (lhs.fvf != rhs.fvf || !(lhs.elements == rhs.elements)) {
      return false;
    }
    for (std::size_t i = 0; i < core::kMaxStreams; ++i) {
      if (lhs.streams[i].offset != rhs.streams[i].offset ||
          lhs.streams[i].stride != rhs.streams[i].stride) {
        return false;
      }
    }
    return true;
  };
  auto drawSemanticMemoEquivalent =
      [vertexDeclPsoShapeEqual](
          const DrawSemanticMemoEntry& entry,
          const DrawSemanticMemoKey& key,
          const core::FlatDrawStateRecord& hot,
          const core::DrawShaderLayoutContext& shaderLayout) {
    if (entry.key != key || !entry.hot || !entry.shaderLayout) {
      return false;
    }
    const auto& prevHot = *entry.hot;
    const auto& prevLayout = *entry.shaderLayout;
    const auto& prevKey = prevHot.key;
    const auto& currentKey = hot.key;
    return prevKey.fvf == currentKey.fvf &&
           prevKey.vertexElementCount == currentKey.vertexElementCount &&
           prevKey.vertexShaderKind == currentKey.vertexShaderKind &&
           prevKey.pixelShaderKind == currentKey.pixelShaderKind &&
           prevKey.vertexShaderHash == currentKey.vertexShaderHash &&
           prevKey.pixelShaderHash == currentKey.pixelShaderHash &&
           prevKey.renderStateHash == currentKey.renderStateHash &&
           prevKey.textureMask == currentKey.textureMask &&
           prevKey.samplerStateMask == currentKey.samplerStateMask &&
           prevKey.renderTargetMask == currentKey.renderTargetMask &&
           prevKey.clipPlaneMask == currentKey.clipPlaneMask &&
           prevKey.clipPlanesHash == currentKey.clipPlanesHash &&
           prevKey.textures == currentKey.textures &&
           prevKey.textureLods == currentKey.textureLods &&
           prevKey.textureStageStateHashes ==
               currentKey.textureStageStateHashes &&
           prevKey.samplerStateHashes == currentKey.samplerStateHashes &&
           prevKey.colorAttachments == currentKey.colorAttachments &&
           prevKey.depthStencil == currentKey.depthStencil &&
           prevLayout.vertexShader.kind == shaderLayout.vertexShader.kind &&
           prevLayout.vertexShader.hash == shaderLayout.vertexShader.hash &&
           prevLayout.vertexShader.bytecode.hash ==
               shaderLayout.vertexShader.bytecode.hash &&
           prevLayout.pixelShader.kind == shaderLayout.pixelShader.kind &&
           prevLayout.pixelShader.hash == shaderLayout.pixelShader.hash &&
           prevLayout.pixelShader.bytecode.hash ==
               shaderLayout.pixelShader.bytecode.hash &&
           prevLayout.vertexConstantUsage ==
               shaderLayout.vertexConstantUsage &&
           prevLayout.pixelConstantUsage == shaderLayout.pixelConstantUsage &&
           prevLayout.clipPlaneMask == shaderLayout.clipPlaneMask &&
           vertexDeclPsoShapeEqual(prevLayout.vertexDecl,
                                   shaderLayout.vertexDecl);
  };
  auto drawResourceShapeMemoEquivalent =
      [vertexDeclPsoShapeEqual](
          const DrawResourceShapeMemoEntry& entry,
          const DrawSemanticMemoKey& key,
          const core::FlatDrawStateRecord& hot,
          const core::DrawShaderLayoutContext& shaderLayout) {
    if (entry.key != key || !entry.hot || !entry.shaderLayout) {
      return false;
    }
    const auto& prevHot = *entry.hot;
    const auto& prevLayout = *entry.shaderLayout;
    const auto& prevKey = prevHot.key;
    const auto& currentKey = hot.key;
    return prevKey.fvf == currentKey.fvf &&
           prevKey.vertexElementCount == currentKey.vertexElementCount &&
           prevKey.vertexShaderKind == currentKey.vertexShaderKind &&
           prevKey.pixelShaderKind == currentKey.pixelShaderKind &&
           prevKey.vertexShaderHash == currentKey.vertexShaderHash &&
           prevKey.pixelShaderHash == currentKey.pixelShaderHash &&
           prevKey.renderStateHash == currentKey.renderStateHash &&
           prevKey.textureMask == currentKey.textureMask &&
           prevKey.samplerStateMask == currentKey.samplerStateMask &&
           prevKey.renderTargetMask == currentKey.renderTargetMask &&
           prevKey.clipPlaneMask == currentKey.clipPlaneMask &&
           prevKey.clipPlanesHash == currentKey.clipPlanesHash &&
           prevKey.textureLods == currentKey.textureLods &&
           prevKey.textureStageStateHashes ==
               currentKey.textureStageStateHashes &&
           prevKey.samplerStateHashes == currentKey.samplerStateHashes &&
           prevKey.colorAttachments == currentKey.colorAttachments &&
           prevKey.depthStencil == currentKey.depthStencil &&
           prevLayout.vertexShader.kind == shaderLayout.vertexShader.kind &&
           prevLayout.vertexShader.hash == shaderLayout.vertexShader.hash &&
           prevLayout.vertexShader.bytecode.hash ==
               shaderLayout.vertexShader.bytecode.hash &&
           prevLayout.pixelShader.kind == shaderLayout.pixelShader.kind &&
           prevLayout.pixelShader.hash == shaderLayout.pixelShader.hash &&
           prevLayout.pixelShader.bytecode.hash ==
               shaderLayout.pixelShader.bytecode.hash &&
           prevLayout.vertexConstantUsage ==
               shaderLayout.vertexConstantUsage &&
           prevLayout.pixelConstantUsage == shaderLayout.pixelConstantUsage &&
           prevLayout.clipPlaneMask == shaderLayout.clipPlaneMask &&
           vertexDeclPsoShapeEqual(prevLayout.vertexDecl,
                                   shaderLayout.vertexDecl);
  };
  auto makeDrawSemanticMemoKey =
      [mixDrawSemanticHash, mixDrawSemanticHandle,
       mixDrawSemanticAttachment](const core::FlatDrawStateView& drawState,
                                  bool argbufHybridMode,
                                  bool argbufResourceArray,
                                  bool argbufDirectCbufMode) {
    const auto& hot = *drawState.hot;
    const auto& key = hot.key;
    const auto& shaderLayout = drawState.shaderContext();
    std::uint64_t hash = 1469598103934665603ull;
    hash = mixDrawSemanticHash(hash, key.fvf);
    hash = mixDrawSemanticHash(hash, key.vertexElementCount);
    hash = mixDrawSemanticHash(hash, key.vertexDeclHash);
    hash = mixDrawSemanticHash(
        hash, ffp::hashVertexDeclaration(shaderLayout.vertexDecl));
    hash = mixDrawSemanticHash(
        hash, static_cast<std::uint64_t>(key.vertexShaderKind));
    hash = mixDrawSemanticHash(
        hash, static_cast<std::uint64_t>(key.pixelShaderKind));
    hash = mixDrawSemanticHash(hash, key.vertexShaderHash);
    hash = mixDrawSemanticHash(hash, key.pixelShaderHash);
    hash = mixDrawSemanticHash(hash, shaderLayout.vertexShader.bytecode.hash);
    hash = mixDrawSemanticHash(hash, shaderLayout.pixelShader.bytecode.hash);
    hash = mixDrawSemanticHash(hash, key.renderStateHash);
    hash = mixDrawSemanticHash(hash, key.textureMask);
    hash = mixDrawSemanticHash(hash, key.samplerStateMask);
    hash = mixDrawSemanticHash(hash, key.renderTargetMask);
    hash = mixDrawSemanticHash(hash, key.clipPlaneMask);
    hash = mixDrawSemanticHash(hash, key.clipPlanesHash);
    for (const auto texture : key.textures) {
      hash = mixDrawSemanticHandle(hash, texture);
    }
    for (const auto lod : key.textureLods) {
      hash = mixDrawSemanticHash(hash, lod);
    }
    for (const auto stageHash : key.textureStageStateHashes) {
      hash = mixDrawSemanticHash(hash, stageHash);
    }
    for (const auto samplerHash : key.samplerStateHashes) {
      hash = mixDrawSemanticHash(hash, samplerHash);
    }
    for (const auto attachment : key.colorAttachments) {
      hash = mixDrawSemanticAttachment(hash, attachment);
    }
    hash = mixDrawSemanticAttachment(hash, key.depthStencil);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.floatCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.intCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.boolCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.indexedFloat);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.indexedInt);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.indexedBool);
    hash = mixDrawSemanticHash(hash, shaderLayout.vertexConstantUsage.unknown);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.floatCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.intCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.boolCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.indexedFloat);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.indexedInt);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.indexedBool);
    hash = mixDrawSemanticHash(hash, shaderLayout.pixelConstantUsage.unknown);
    hash = mixDrawSemanticHash(hash, shaderLayout.clipPlaneMask);
    return DrawSemanticMemoKey{
        .fingerprint = hash,
        .argbufHybridMode = argbufHybridMode,
        .argbufResourceArray = argbufResourceArray,
        .argbufDirectCbufMode = argbufDirectCbufMode,
    };
  };
  auto makeDrawResourceShapeMemoKey =
      [mixDrawSemanticHash,
       mixDrawSemanticAttachment](const core::FlatDrawStateView& drawState,
                                  bool argbufHybridMode,
                                  bool argbufResourceArray,
                                  bool argbufDirectCbufMode) {
    const auto& hot = *drawState.hot;
    const auto& key = hot.key;
    const auto& shaderLayout = drawState.shaderContext();
    std::uint64_t hash = 1469598103934665603ull;
    hash = mixDrawSemanticHash(hash, key.fvf);
    hash = mixDrawSemanticHash(hash, key.vertexElementCount);
    hash = mixDrawSemanticHash(hash, key.vertexDeclHash);
    hash = mixDrawSemanticHash(
        hash, ffp::hashVertexDeclaration(shaderLayout.vertexDecl));
    hash = mixDrawSemanticHash(
        hash, static_cast<std::uint64_t>(key.vertexShaderKind));
    hash = mixDrawSemanticHash(
        hash, static_cast<std::uint64_t>(key.pixelShaderKind));
    hash = mixDrawSemanticHash(hash, key.vertexShaderHash);
    hash = mixDrawSemanticHash(hash, key.pixelShaderHash);
    hash = mixDrawSemanticHash(hash, shaderLayout.vertexShader.bytecode.hash);
    hash = mixDrawSemanticHash(hash, shaderLayout.pixelShader.bytecode.hash);
    hash = mixDrawSemanticHash(hash, key.renderStateHash);
    hash = mixDrawSemanticHash(hash, key.textureMask);
    hash = mixDrawSemanticHash(hash, key.samplerStateMask);
    hash = mixDrawSemanticHash(hash, key.renderTargetMask);
    hash = mixDrawSemanticHash(hash, key.clipPlaneMask);
    hash = mixDrawSemanticHash(hash, key.clipPlanesHash);
    for (const auto lod : key.textureLods) {
      hash = mixDrawSemanticHash(hash, lod);
    }
    for (const auto stageHash : key.textureStageStateHashes) {
      hash = mixDrawSemanticHash(hash, stageHash);
    }
    for (const auto samplerHash : key.samplerStateHashes) {
      hash = mixDrawSemanticHash(hash, samplerHash);
    }
    for (const auto attachment : key.colorAttachments) {
      hash = mixDrawSemanticAttachment(hash, attachment);
    }
    hash = mixDrawSemanticAttachment(hash, key.depthStencil);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.floatCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.intCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.boolCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.indexedFloat);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.indexedInt);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.vertexConstantUsage.indexedBool);
    hash = mixDrawSemanticHash(hash, shaderLayout.vertexConstantUsage.unknown);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.floatCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.intCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.boolCount);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.indexedFloat);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.indexedInt);
    hash = mixDrawSemanticHash(hash,
                               shaderLayout.pixelConstantUsage.indexedBool);
    hash = mixDrawSemanticHash(hash, shaderLayout.pixelConstantUsage.unknown);
    hash = mixDrawSemanticHash(hash, shaderLayout.clipPlaneMask);
    return DrawSemanticMemoKey{
        .fingerprint = hash,
        .argbufHybridMode = argbufHybridMode,
        .argbufResourceArray = argbufResourceArray,
        .argbufDirectCbufMode = argbufDirectCbufMode,
    };
  };
  auto hashDrawSemanticMemoKey = [](const DrawSemanticMemoKey& key) {
    auto hash = key.fingerprint;
    hash ^= key.argbufHybridMode ? 0x6a09e667f3bcc909ull : 0u;
    hash ^= key.argbufResourceArray ? 0xbb67ae8584caa73bull : 0u;
    hash ^= key.argbufDirectCbufMode ? 0x3c6ef372fe94f82bull : 0u;
    return static_cast<std::size_t>(hash);
  };
  auto probeDrawSemanticMemo =
      [drawPsoMemoEpoch, hashDrawSemanticMemoKey, drawSemanticMemoEquivalent](
          const DrawSemanticMemoKey& key,
          const core::FlatDrawStateRecord& hot,
          const core::DrawShaderLayoutContext& shaderLayout)
      -> DrawSemanticMemoLookup {
    std::size_t index =
        hashDrawSemanticMemoKey(key) & (kDrawSemanticMemoTableCapacity - 1);
    for (std::size_t probe = 0; probe < kDrawSemanticMemoTableCapacity;
         ++probe) {
      const auto& entry = drawSemanticMemo[index];
      if (entry.epoch != drawPsoMemoEpoch) {
        return DrawSemanticMemoLookup{.index = index};
      }
      if (drawSemanticMemoEquivalent(entry, key, hot, shaderLayout)) {
        return DrawSemanticMemoLookup{
            .handle = entry.handle,
            .index = index,
            .hit = true,
        };
      }
      index = (index + 1) & (kDrawSemanticMemoTableCapacity - 1);
    }
    return DrawSemanticMemoLookup{.overflow = true};
  };
  auto storeDrawSemanticMemo =
      [drawPsoMemoEpoch](
          const DrawSemanticMemoLookup& memo,
          const DrawSemanticMemoKey& key,
          const core::FlatDrawStateRecord& hot,
          const core::DrawShaderLayoutContext& shaderLayout,
          core::PsoHandle handle) {
    if (memo.hit || memo.overflow || !handle.valid()) {
      return;
    }
    auto& entry = drawSemanticMemo[memo.index];
    entry.key = key;
    entry.hot = &hot;
    entry.shaderLayout = &shaderLayout;
    entry.handle = handle;
    entry.epoch = drawPsoMemoEpoch;
  };
  auto probeDrawResourceShapeMemo =
      [drawResourceShapeMemo, drawResourceShapeMemoEpoch,
       hashDrawSemanticMemoKey,
       drawResourceShapeMemoEquivalent](
          const DrawSemanticMemoKey& key,
          const core::FlatDrawStateRecord& hot,
          const core::DrawShaderLayoutContext& shaderLayout)
      -> DrawResourceShapeMemoLookup {
    if (!drawResourceShapeMemo) {
      return DrawResourceShapeMemoLookup{};
    }
    std::size_t index =
        hashDrawSemanticMemoKey(key) &
        (kDrawResourceShapeMemoTableCapacity - 1);
    for (std::size_t probe = 0; probe < kDrawResourceShapeMemoTableCapacity;
         ++probe) {
      const auto& entry = (*drawResourceShapeMemo)[index];
      if (entry.epoch != drawResourceShapeMemoEpoch) {
        return DrawResourceShapeMemoLookup{.index = index};
      }
      if (drawResourceShapeMemoEquivalent(entry, key, hot, shaderLayout)) {
        return DrawResourceShapeMemoLookup{
            .entry = &entry,
            .index = index,
            .hit = true,
        };
      }
      index = (index + 1) & (kDrawResourceShapeMemoTableCapacity - 1);
    }
    return DrawResourceShapeMemoLookup{.overflow = true};
  };
  auto storeDrawResourceShapeMemo =
      [drawResourceShapeMemo, drawResourceShapeMemoEpoch](
          const DrawResourceShapeMemoLookup& memo,
          const DrawSemanticMemoKey& key,
          const core::FlatDrawStateRecord& hot,
          const core::DrawShaderLayoutContext& shaderLayout,
          const pipeline::ShaderVariantKey& probeKey,
          core::PsoHandle handle) {
    if (!drawResourceShapeMemo || memo.hit || memo.overflow || !handle.valid()) {
      return;
    }
    auto& entry = (*drawResourceShapeMemo)[memo.index];
    entry.key = key;
    entry.hot = &hot;
    entry.shaderLayout = &shaderLayout;
    entry.probeKey = probeKey;
    entry.handle = handle;
    entry.epoch = drawResourceShapeMemoEpoch;
    perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoStores();
  };
  auto probeDrawPsoKeyMemo =
      [drawPsoMemoEpoch](const pipeline::ShaderVariantKey& probeKey)
      -> DrawProbeKeyMemoLookup {
    std::size_t index =
        pipeline::ShaderVariantKeyHash{}(probeKey) &
        (kDrawProbeKeyMemoTableCapacity - 1);
    for (std::size_t probe = 0; probe < kDrawProbeKeyMemoTableCapacity;
         ++probe) {
      const auto& entry = drawProbeKeyMemo[index];
      if (entry.epoch != drawPsoMemoEpoch) {
        return DrawProbeKeyMemoLookup{.index = index};
      }
      if (entry.probeKey == probeKey) {
        return DrawProbeKeyMemoLookup{
            .handle = entry.handle,
            .entry = &entry,
            .index = index,
            .hit = true,
        };
      }
      index = (index + 1) & (kDrawProbeKeyMemoTableCapacity - 1);
    }
    return DrawProbeKeyMemoLookup{.overflow = true};
  };
  auto storeDrawPsoKeyMemo = [drawPsoMemoEpoch](
                                 const DrawProbeKeyMemoLookup& memo,
                                 const pipeline::ShaderVariantKey& probeKey,
                                 const DrawSemanticMemoKey& semanticKey,
                                 const core::FlatDrawStateRecord& hot,
                                 const core::DrawShaderLayoutContext& shaderLayout,
                                 core::PsoHandle handle) {
    if (memo.hit || memo.overflow || !handle.valid()) {
      return;
    }
    auto& entry = drawProbeKeyMemo[memo.index];
    entry.probeKey = probeKey;
    entry.semanticKey = semanticKey;
    entry.hot = &hot;
    entry.shaderLayout = &shaderLayout;
    entry.handle = handle;
    entry.epoch = drawPsoMemoEpoch;
  };
  auto recordDrawSemanticMissProbeKeyCollapse =
      [drawSemanticMissSplitEnabled, vertexDeclPsoShapeEqual](
          const DrawProbeKeyMemoEntry& entry,
          const DrawSemanticMemoKey& semanticKey,
          const core::FlatDrawStateRecord& hot,
          const core::DrawShaderLayoutContext& shaderLayout) {
    if (!drawSemanticMissSplitEnabled) {
      return;
    }
    perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyHits();
    if (!entry.hot || !entry.shaderLayout) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffUnknown();
      return;
    }

    std::uint32_t diffFieldCount = 0;
    bool textureHandleDiff = false;
    const auto& prevHot = *entry.hot;
    const auto& prevLayout = *entry.shaderLayout;
    const auto& prevKey = prevHot.key;
    const auto& currentKey = hot.key;

    if (entry.semanticKey.argbufHybridMode != semanticKey.argbufHybridMode ||
        entry.semanticKey.argbufResourceArray !=
            semanticKey.argbufResourceArray ||
        entry.semanticKey.argbufDirectCbufMode !=
            semanticKey.argbufDirectCbufMode) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffArgbufSelector();
      ++diffFieldCount;
    }
    if (prevKey.fvf != currentKey.fvf ||
        prevKey.vertexElementCount != currentKey.vertexElementCount ||
        prevKey.vertexDeclHash != currentKey.vertexDeclHash ||
        !vertexDeclPsoShapeEqual(prevLayout.vertexDecl,
                                 shaderLayout.vertexDecl)) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffVertexDecl();
      ++diffFieldCount;
    }
    if (prevKey.vertexShaderKind != currentKey.vertexShaderKind ||
        prevKey.pixelShaderKind != currentKey.pixelShaderKind ||
        prevKey.vertexShaderHash != currentKey.vertexShaderHash ||
        prevKey.pixelShaderHash != currentKey.pixelShaderHash ||
        prevLayout.vertexShader.kind != shaderLayout.vertexShader.kind ||
        prevLayout.vertexShader.hash != shaderLayout.vertexShader.hash ||
        prevLayout.vertexShader.bytecode.hash !=
            shaderLayout.vertexShader.bytecode.hash ||
        prevLayout.pixelShader.kind != shaderLayout.pixelShader.kind ||
        prevLayout.pixelShader.hash != shaderLayout.pixelShader.hash ||
        prevLayout.pixelShader.bytecode.hash !=
            shaderLayout.pixelShader.bytecode.hash) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffShader();
      ++diffFieldCount;
    }
    if (prevKey.renderStateHash != currentKey.renderStateHash) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffRenderState();
      ++diffFieldCount;
    }
    if (prevKey.textureMask != currentKey.textureMask ||
        prevKey.textures != currentKey.textures) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandles();
      textureHandleDiff = true;
      ++diffFieldCount;
    }
    if (prevKey.textureLods != currentKey.textureLods) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureLod();
      ++diffFieldCount;
    }
    if (prevKey.textureStageStateHashes !=
        currentKey.textureStageStateHashes) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureStage();
      ++diffFieldCount;
    }
    if (prevKey.samplerStateMask != currentKey.samplerStateMask ||
        prevKey.samplerStateHashes != currentKey.samplerStateHashes) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSampler();
      ++diffFieldCount;
    }
    if (prevKey.renderTargetMask != currentKey.renderTargetMask ||
        prevKey.colorAttachments != currentKey.colorAttachments ||
        prevKey.depthStencil != currentKey.depthStencil) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffAttachment();
      ++diffFieldCount;
    }
    if (prevKey.clipPlaneMask != currentKey.clipPlaneMask ||
        prevKey.clipPlanesHash != currentKey.clipPlanesHash ||
        prevLayout.clipPlaneMask != shaderLayout.clipPlaneMask) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffClipPlane();
      ++diffFieldCount;
    }
    if (prevLayout.vertexConstantUsage != shaderLayout.vertexConstantUsage ||
        prevLayout.pixelConstantUsage != shaderLayout.pixelConstantUsage) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffConstantUsage();
      ++diffFieldCount;
    }
    if (diffFieldCount == 0) {
      if (entry.semanticKey == semanticKey) {
        perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeySameSemantic();
      } else {
        perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffHashOnly();
      }
      return;
    }
    if (diffFieldCount == 1) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffSingleField();
      if (textureHandleDiff) {
        perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesOnly();
      }
      return;
    }
    perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffMultiField();
    if (textureHandleDiff) {
      perf::countEncodeSlotPsoPrefetchDrawSemanticMissProbeKeyDiffTextureHandlesWithOthers();
    }
  };
  auto recordDrawResourceShapeProbeMismatch =
      [](const pipeline::ShaderVariantKey& previous,
         const pipeline::ShaderVariantKey& current) {
    bool classified = false;
    if (previous.textureMask != current.textureMask) {
      perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureMask();
      classified = true;
    }
    if (previous.textureTypes != current.textureTypes) {
      perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchTextureTypes();
      classified = true;
    }
    if (previous.x8AlphaOneTextureMask != current.x8AlphaOneTextureMask) {
      perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchX8Alpha();
      classified = true;
    }
    if (previous.sampleCount != current.sampleCount ||
        previous.colorFormats != current.colorFormats ||
        previous.blend != current.blend ||
        previous.depthFormat != current.depthFormat ||
        previous.stencilFormat != current.stencilFormat) {
      perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchAttachment();
      classified = true;
    }
    if (previous.samplerLodBias != current.samplerLodBias) {
      perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchSamplerLodBias();
      classified = true;
    }
    if (previous.vsOutLayoutKey != current.vsOutLayoutKey) {
      perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchVsOut();
      classified = true;
    }
    if (!classified) {
      perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoMismatchOther();
    }
  };
  auto recordDrawPsoHandleReuseOpportunity = [drawPsoMemoEpoch,
                                               &previousDrawPsoHandle,
                                               &hasPreviousDrawPsoHandle](
                                                  core::PsoHandle handle) {
    if (!handle.valid()) {
      return;
    }
    if (hasPreviousDrawPsoHandle) {
      perf::countEncodeSlotPsoPrefetchDrawHandleAdjacentCandidates();
      if (previousDrawPsoHandle == handle) {
        perf::countEncodeSlotPsoPrefetchDrawHandleAdjacentHits();
      }
    }

    const auto key =
        (static_cast<std::uint32_t>(handle.slot) << 16) |
        static_cast<std::uint32_t>(handle.generation);
    std::size_t index =
        (key * 2654435761u) & (kDrawHandleReuseTableCapacity - 1);
    for (std::size_t probe = 0; probe < kDrawHandleReuseTableCapacity;
         ++probe) {
      auto& seen = seenDrawPsoHandles[index];
      if (seen.epoch != drawPsoMemoEpoch) {
        seen.handle = handle;
        seen.epoch = drawPsoMemoEpoch;
        perf::countEncodeSlotPsoPrefetchDrawHandleSlotUnique();
        previousDrawPsoHandle = handle;
        hasPreviousDrawPsoHandle = true;
        return;
      }
      if (seen.handle == handle) {
        perf::countEncodeSlotPsoPrefetchDrawHandleSlotRepeatHits();
        previousDrawPsoHandle = handle;
        hasPreviousDrawPsoHandle = true;
        return;
      }
      index = (index + 1) & (kDrawHandleReuseTableCapacity - 1);
    }

    perf::countEncodeSlotPsoPrefetchDrawHandleSlotOverflow();
    previousDrawPsoHandle = handle;
    hasPreviousDrawPsoHandle = true;
  };
  for (std::size_t i = beginCommand; i < commandCount; ++i) {
    tracePsoPrefetchStage("command.begin", slot, i);
    const auto command = slot.commandAt(i);
    if (command.kind != core::MetalCommandKind::DrawRun ||
        !command.drawPsoSubview ||
        !command.drawPsoSubview->hasShaderContext ||
        !command.drawState.hot ||
        !command.drawState.hasShaderContext()) {
      tracePsoPrefetchStage("command.skip", slot, i);
      continue;
    }
    perf::countEncodeSlotPsoPrefetchCandidates();
    tracePsoPrefetchStage("draw.candidate", slot, i);

    // DrawPsoSubview is the PE/draw-run-side summary that proves this run has
    // PSO-bearing state. Final key completion still happens below because RT/DS
    // formats and tile/argbuf selectors are Metal-device/pool dependent.
    core::FlatDrawStateView drawState{};
    {
      PerfScope stageScope(perf::countEncodeSlotPsoPrefetchStateCopyCpuTime);
      drawState = command.drawState;
      if (!command.drawRunRecord) {
        continue;
      }
    }
    pipeline::DepthStencilLookup depthLookup{};
    {
      PerfScope stageScope(perf::countEncodeSlotPsoPrefetchDepthLookupCpuTime);
      depthLookup = pipelineCache_.depthStencilStateHandleFor(
          device_, state::makeDepthStencilKey(drawState));
    }
    slot.setDrawRunDepthStencilHandle(i, depthLookup.handle);
    pipeline::TileFfpSelection tileSelection{};
    {
      PerfScope stageScope(perf::countEncodeSlotPsoPrefetchTileSelectCpuTime);
      tileSelection =
          pipeline::selectTileFfpForPass(drawState, pool_.supportsApple3());
    }
    const bool tileFfpMode =
        tileSelection.decision == pipeline::TileFfpDecision::Tile;
    if (tileFfpMode) {
      perf::countEncodeSlotPsoPrefetchTileCandidates();
      pipeline::DrawPipelineLookup baseLookup{};
      {
        PerfScope stageScope(
            perf::countEncodeSlotPsoPrefetchTileBaseLookupCpuTime);
        tracePsoPrefetchStage("tile.before-base-lookup", slot, i);
        baseLookup =
            pipelineCache_.getOrBuildTileFfpBaseColorPipelineHandleForState(
                device_, limits_, pool_, drawState, &shaderArchive_.reference(),
                &shaderArchive_.path());
        tracePsoPrefetchStage("tile.after-base-lookup", slot, i);
      }
      pipeline::DrawPipelineLookup tileLookup{};
      {
        PerfScope stageScope(
            perf::countEncodeSlotPsoPrefetchTileDrawLookupCpuTime);
        tracePsoPrefetchStage("tile.before-draw-lookup", slot, i);
        tileLookup = pipelineCache_.getOrBuildDrawPipelineHandleForState(
            device_, limits_, pool_, drawState, &shaderArchive_.reference(),
            &shaderArchive_.path(), /*tileFfpMode=*/true,
            /*argbufHybridMode=*/false, /*argbufResourceArray=*/false);
        tracePsoPrefetchStage("tile.after-draw-lookup", slot, i);
      }
      slot.setDrawRunPsoHandles(i, baseLookup.handle, tileLookup.handle);
      continue;
    }

    bool argbufHybridMode = false;
    {
      PerfScope stageScope(perf::countEncodeSlotPsoPrefetchArgbufSelectCpuTime);
      argbufHybridMode =
          pipeline::selectArgbufHybridForPass(drawState, pool_.argbufHybridEnabled()) ==
          pipeline::ArgbufHybridDecision::Stage2;
    }
    if (argbufHybridMode) {
      perf::countEncodeSlotPsoPrefetchArgbufStage2Candidates();
    }
    const bool argbufResourceArray =
        argbufHybridMode && resourceArrayLaneActive_ &&
        resourceArrayEncoderResource_.initialized();
    if (argbufResourceArray) {
      perf::countEncodeSlotPsoPrefetchArgbufResourceArrayCandidates();
    }
    const bool argbufDirectCbufMode =
        argbufHybridMode && !argbufResourceArray &&
        pipeline::argbufDirectCbufEnabled();
    DrawSemanticMemoKey semanticMemoKey{};
    {
      PerfScope stageScope(
          drawSemanticSplitEnabled
              ? perf::countEncodeSlotPsoPrefetchDrawSemanticKeyCpuTime
              : nullptr);
      semanticMemoKey =
          makeDrawSemanticMemoKey(drawState, argbufHybridMode,
                                  argbufResourceArray,
                                  argbufDirectCbufMode);
    }
    DrawSemanticMemoLookup semanticMemo{};
    if (drawSemanticMemoEnabled) {
      {
        PerfScope stageScope(
            drawSemanticSplitEnabled
                ? perf::countEncodeSlotPsoPrefetchDrawSemanticProbeCpuTime
                : nullptr);
        semanticMemo = probeDrawSemanticMemo(
            semanticMemoKey, *drawState.hot, drawState.shaderContext());
      }
      if (semanticMemo.hit) {
        perf::countEncodeSlotPsoPrefetchDrawSemanticMemoHits();
        recordDrawPsoHandleReuseOpportunity(semanticMemo.handle);
        slot.setDrawRunPsoHandles(i, semanticMemo.handle);
        continue;
      }
      if (semanticMemo.overflow) {
        perf::countEncodeSlotPsoPrefetchDrawSemanticMemoOverflow();
      } else {
        perf::countEncodeSlotPsoPrefetchDrawSemanticMemoMisses();
      }
    }
    DrawSemanticMemoKey resourceShapeMemoKey{};
    DrawResourceShapeMemoLookup resourceShapeMemo{};
    const bool resourceShapeOpportunityForDraw =
        drawResourceShapeMemoEnabled && drawSemanticMemoEnabled &&
        !semanticMemo.hit && !semanticMemo.overflow;
    if (resourceShapeOpportunityForDraw) {
      perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoCandidates();
      resourceShapeMemoKey =
          makeDrawResourceShapeMemoKey(drawState, argbufHybridMode,
                                       argbufResourceArray,
                                       argbufDirectCbufMode);
      resourceShapeMemo = probeDrawResourceShapeMemo(
          resourceShapeMemoKey, *drawState.hot, drawState.shaderContext());
      if (resourceShapeMemo.hit) {
        perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoHits();
      } else if (resourceShapeMemo.overflow) {
        perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoOverflow();
      } else {
        perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoMisses();
      }
      if (drawResourceShapeBehaviorEnabled &&
          !drawResourceShapeOpportunityEnabled && resourceShapeMemo.hit &&
          resourceShapeMemo.entry && resourceShapeMemo.entry->handle.valid()) {
        recordDrawPsoHandleReuseOpportunity(resourceShapeMemo.entry->handle);
        if (drawSemanticMemoEnabled) {
          PerfScope stageScope(
              drawSemanticSplitEnabled
                  ? perf::countEncodeSlotPsoPrefetchDrawSemanticStoreCpuTime
                  : nullptr);
          storeDrawSemanticMemo(semanticMemo, semanticMemoKey, *drawState.hot,
                                drawState.shaderContext(),
                                resourceShapeMemo.entry->handle);
        }
        slot.setDrawRunPsoHandles(i, resourceShapeMemo.entry->handle);
        continue;
      }
    }
    auto resolved = [&] {
      PerfScope stageScope(
          perf::countEncodeSlotPsoPrefetchDrawKeyResolveCpuTime);
      return pipelineCache_.resolveDrawPipelineState(
          limits_, pool_, drawState, /*tileFfpMode=*/false, argbufHybridMode,
          argbufResourceArray, argbufDirectCbufMode);
    }();
    const auto probeKey = pipeline::makeShaderVariantProbeKey(resolved.key);
    if (resourceShapeOpportunityForDraw && resourceShapeMemo.hit &&
        resourceShapeMemo.entry) {
      if (resourceShapeMemo.entry->probeKey == probeKey) {
        perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoValidatedHits();
      } else {
        perf::countEncodeSlotPsoPrefetchDrawResourceShapeMemoValidatedMisses();
        recordDrawResourceShapeProbeMismatch(resourceShapeMemo.entry->probeKey,
                                             probeKey);
      }
    }
    pipeline::DrawPipelineLookup lookup{};
    if (drawProbeKeyMemoEnabled) {
      const auto memo = probeDrawPsoKeyMemo(probeKey);
      if (memo.hit) {
        perf::countEncodeSlotPsoPrefetchDrawProbeKeyMemoHits();
        if (drawSemanticMemoEnabled && !semanticMemo.hit &&
            !semanticMemo.overflow && memo.entry) {
          recordDrawSemanticMissProbeKeyCollapse(
              *memo.entry, semanticMemoKey, *drawState.hot,
              drawState.shaderContext());
        }
        lookup.handle = memo.handle;
      } else {
        if (memo.overflow) {
          perf::countEncodeSlotPsoPrefetchDrawProbeKeyMemoOverflow();
        } else {
          perf::countEncodeSlotPsoPrefetchDrawProbeKeyMemoMisses();
        }
        {
          PerfScope stageScope(
              perf::countEncodeSlotPsoPrefetchDrawLookupCpuTime);
          tracePsoPrefetchStage("draw.before-lookup", slot, i);
          lookup = pipelineCache_.getOrBuildDrawPipelineHandle(
              device_, resolved.key, std::move(resolved.shaderSource),
              &shaderArchive_.reference(), &shaderArchive_.path());
          tracePsoPrefetchStage("draw.after-lookup", slot, i);
        }
        storeDrawPsoKeyMemo(memo, probeKey, semanticMemoKey, *drawState.hot,
                            drawState.shaderContext(), lookup.handle);
      }
    } else {
      {
        PerfScope stageScope(perf::countEncodeSlotPsoPrefetchDrawLookupCpuTime);
        tracePsoPrefetchStage("draw.before-lookup", slot, i);
        lookup = pipelineCache_.getOrBuildDrawPipelineHandle(
            device_, resolved.key, std::move(resolved.shaderSource),
            &shaderArchive_.reference(), &shaderArchive_.path());
        tracePsoPrefetchStage("draw.after-lookup", slot, i);
      }
    }
    if (drawSemanticMemoEnabled) {
      PerfScope stageScope(
          drawSemanticSplitEnabled
              ? perf::countEncodeSlotPsoPrefetchDrawSemanticStoreCpuTime
              : nullptr);
      storeDrawSemanticMemo(semanticMemo, semanticMemoKey, *drawState.hot,
                            drawState.shaderContext(), lookup.handle);
    }
    if (resourceShapeOpportunityForDraw) {
      storeDrawResourceShapeMemo(resourceShapeMemo, resourceShapeMemoKey,
                                 *drawState.hot, drawState.shaderContext(),
                                 probeKey, lookup.handle);
    }
    recordDrawPsoHandleReuseOpportunity(lookup.handle);
    slot.setDrawRunPsoHandles(i, lookup.handle);
    tracePsoPrefetchStage("draw.done", slot, i);
  }
  slot.setPrefetchedPipelineCommandCursor(commandCount);
  if (seal) {
    slot.sealPrefetchedPipelines();
  }
  tracePsoPrefetchStage(seal ? "end-sealed" : "end-partial", slot);
}

uniform::DirtyState CommandQueue::consumePendingDirty() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::DirtyState snapshot = pendingDirty_;
  pendingDirty_ = uniform::DirtyState{};
  return snapshot;
}

void CommandQueue::applyDirtyConstantSetVsF(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetVsF(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetVsI(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetVsI(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetVsB(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetVsB(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetPsF(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetPsF(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetPsI(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetPsI(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyConstantSetPsB(std::uint32_t startReg, std::uint32_t count) {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyConstantSetPsB(pendingDirty_, startReg, count);
}

void CommandQueue::applyDirtyTransformChange() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyTransformChange(pendingDirty_);
}

void CommandQueue::applyDirtyClipPlaneChange() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyClipPlaneChange(pendingDirty_);
}

void CommandQueue::applyDirtyViewportChange() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyViewportChange(pendingDirty_);
}

void CommandQueue::applyDirtyRenderStateFog() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyRenderStateFog(pendingDirty_);
}

void CommandQueue::applyDirtyRenderStateAlpha() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyRenderStateAlpha(pendingDirty_);
}

void CommandQueue::applyDirtyRenderStateTexFactor() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyRenderStateTexFactor(pendingDirty_);
}

void CommandQueue::applyDirtyTextureStageConstant() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::applyTextureStageConstant(pendingDirty_);
}

// R-BACK-15.4 / 15.5 / 15.6: touched color attachment set. Single-thread
// access (encoder thread); no mutex. Null/zero handles are no-ops on
// every entry-point so callers don't have to guard.
bool CommandQueue::isColorHandleTouched(core::Handle handle) const {
  if (!handle) return false;
  return touchedColorHandles_.find(handle.value) != touchedColorHandles_.end();
}

void CommandQueue::markColorHandleTouched(core::Handle handle) {
  if (!handle) return;
  touchedColorHandles_.insert(handle.value);
}

void CommandQueue::invalidateColorHandle(core::Handle handle) {
  if (!handle) return;
  touchedColorHandles_.erase(handle.value);
}

void CommandQueue::clearAllTouchedColorHandles() {
  touchedColorHandles_.clear();
}

void CommandQueue::markPendingDirtyAll() {
  std::lock_guard<std::mutex> guard(dirtyMutex_);
  uniform::markAllDirty(pendingDirty_);
}

CommandQueue::~CommandQueue() {
  if (threadsStarted_) {
    stopThreads();
  }
  // Archive persist lives on dxmt9::shaders::Archive's dtor — not here.
}

void CommandQueue::uploadTextureLevel(core::TextureHandle handle,
                                        std::uint32_t level,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        std::uint32_t depth,
                                        std::uint32_t pitch,
                                        std::uint32_t slicePitch,
                                        std::span<const std::uint8_t> bytes) {
  if (initializer_) {
    initializer_->uploadTextureLevel(handle, level, width, height, depth, pitch,
                                     slicePitch, bytes);
  }
}

void CommandQueue::initializeTextureZero(core::TextureHandle handle) {
  if (initializer_) {
    initializer_->initializeTextureZero(handle);
  }
}

core::HResult CommandQueue::generateTextureMipSublevels(core::TextureHandle handle) {
  if (!handle || !queue_) {
    return core::D3DERR_INVALIDCALL;
  }

  const auto flushResult = flushInitializerUploads();
  if (flushResult.event && flushResult.value > 0) {
    WMT::SharedEvent{flushResult.event.handle}.waitUntilSignaledValue(
        flushResult.value, /*timeout-ms*/ 1000);
  }

  WMT::Reference<WMT::Texture> texture;
  WMT::Heap heap{};
  bool isHeapBacked = false;
  {
    const auto qmxBegin = queueMutexProbeBegin();
    std::lock_guard lock(mutex_);
    QueueMutexProbeScope qmxScope(qmxBegin, "generate_texture_mip_sublevels");
    auto* record = pool_.findTexture(handle.value);
    if (!record || !record->texture || record->desc.levels <= 1) {
      return record ? core::D3D_OK : core::D3DERR_INVALIDCALL;
    }
    texture = record->texture;
    heap = record->heap;
    isHeapBacked = record->isHeapBacked;
  }

  auto commandBuffer = newCommandBuffer();
  if (!commandBuffer) {
    return core::D3DERR_INVALIDCALL;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return core::D3DERR_INVALIDCALL;
  }
  if (isHeapBacked && heap.handle != 0) {
    blit.useHeap(heap);
    perf::countUseHeap();
  }
  blit.generateMipmaps(WMT::Texture{texture.handle});
  blit.endEncoding();
  commandBuffer.commit();
  const auto started = std::chrono::steady_clock::now();
  commandBuffer.waitUntilCompleted();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  perf::countSyncWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  return core::D3D_OK;
}

CommandQueue::InitializerFlush CommandQueue::flushInitializerUploads() {
  if (!initializer_) {
    return {};
  }
  auto result = initializer_->flushToWait();
  return {result.event, result.value, result.didFlush};
}

WMT::Reference<WMT::CommandBuffer> CommandQueue::newCommandBuffer() {
  if (!queue_) {
    return {};
  }
  PerfScope scope(perf::countCommandBufferCreateCpuTime);
  auto commandBuffer = queue_.commandBuffer();
  if (commandBuffer) {
    perf::countCommandBuffer();
    // M1: monotonic counter — newCommandBuffer is called from multiple
    // sites (chunk encode, transfers, present split). Mixing a sequence
    // ID would be more meaningful but it isn't available at this site.
    const auto id = gCommandBufferLabelCounter.fetch_add(1, std::memory_order_relaxed);
    WMT::CommandBuffer view{commandBuffer.handle};
    view.setLabel(makeLabelStringFmt("cb_seq_%llu",
        static_cast<unsigned long long>(id)));
  }
  return commandBuffer;
}

CommandQueue::TransientBufferSlice CommandQueue::uploadTransientBuffer(
    std::span<const std::byte> bytes,
    std::size_t alignment,
    std::uint64_t seqId) {
  std::uint64_t completedSeqId = 0;
  {
    const auto qmxBegin = queueMutexProbeBegin();
    std::lock_guard lock(mutex_);
    QueueMutexProbeScope qmxScope(qmxBegin, "upload_transient_buffer");
    completedSeqId = completedSeqId_;
  }
  return uploadTransientBufferWithCompletedSeqId(
      bytes, alignment, seqId, completedSeqId);
}

CommandQueue::TransientBufferSlice
CommandQueue::uploadTransientBufferWithCompletedSeqId(
    std::span<const std::byte> bytes,
    std::size_t alignment,
    std::uint64_t seqId,
    std::uint64_t completedSeqId) {
  return transientArena_.uploadBuffer(bytes, alignment, seqId, completedSeqId);
}

std::vector<CommandQueue::TransientBufferSlice> CommandQueue::uploadTransientBufferBatch(
    std::span<const std::span<const std::byte>> payloads,
    std::size_t alignment,
    std::uint64_t seqId) {
  std::uint64_t completedSeqId = 0;
  {
    const auto qmxBegin = queueMutexProbeBegin();
    std::lock_guard lock(mutex_);
    QueueMutexProbeScope qmxScope(qmxBegin, "upload_transient_buffer_batch");
    completedSeqId = completedSeqId_;
  }
  return uploadTransientBufferBatchWithCompletedSeqId(
      payloads, alignment, seqId, completedSeqId);
}

std::vector<CommandQueue::TransientBufferSlice>
CommandQueue::uploadTransientBufferBatchWithCompletedSeqId(
    std::span<const std::span<const std::byte>> payloads,
    std::size_t alignment,
    std::uint64_t seqId,
    std::uint64_t completedSeqId) {
  return transientArena_.uploadBufferBatch(payloads, alignment, seqId, completedSeqId);
}

void CommandQueue::retainSamplerForSeq(WMT::Reference<WMT::SamplerState> sampler,
                                       std::uint64_t seqId) {
  transientArena_.retainSamplerForSeq(std::move(sampler), seqId);
}

CommandQueue::TransientBufferReservation CommandQueue::reserveTransientBuffer(
    std::size_t size,
    std::size_t alignment,
    std::uint64_t seqId) {
  std::uint64_t completedSeqId = 0;
  {
    const auto qmxBegin = queueMutexProbeBegin();
    std::lock_guard lock(mutex_);
    QueueMutexProbeScope qmxScope(qmxBegin, "reserve_transient_buffer");
    completedSeqId = completedSeqId_;
  }
  return reserveTransientBufferWithCompletedSeqId(
      size, alignment, seqId, completedSeqId);
}

CommandQueue::TransientBufferReservation
CommandQueue::reserveTransientBufferWithCompletedSeqId(
    std::size_t size,
    std::size_t alignment,
    std::uint64_t seqId,
    std::uint64_t completedSeqId) {
  return transientArena_.reserveBuffer(size, alignment, seqId, completedSeqId);
}

resources::ReorderedIndexBufferLookup CommandQueue::findReorderedIndexBuffer(
    core::Handle sourceHandle,
    resources::ReorderedIndexBufferCacheKey key,
    std::uint64_t seqId) {
  const auto qmxBegin = queueMutexProbeBegin();
  std::lock_guard lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "find_reordered_index_buffer");
  return pool_.findReorderedIndexBuffer(
      sourceHandle.value,
      key,
      seqId,
      completedSeqId_);
}

bool CommandQueue::rememberRejectedReorderedIndexBuffer(
    core::Handle sourceHandle,
    resources::ReorderedIndexBufferCacheKey key,
    std::uint64_t seqId) {
  const auto qmxBegin = queueMutexProbeBegin();
  std::lock_guard lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "remember_rejected_reordered_index_buffer");
  return pool_.rememberRejectedReorderedIndexBuffer(
      sourceHandle.value,
      key,
      seqId,
      completedSeqId_);
}

resources::ReorderedIndexBufferLookup CommandQueue::getOrCreateReorderedIndexBuffer(
    core::Handle sourceHandle,
    resources::ReorderedIndexBufferCacheKey key,
    std::span<const std::uint8_t> bytes,
    std::uint64_t seqId) {
  const auto qmxBegin = queueMutexProbeBegin();
  std::lock_guard lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "get_or_create_reordered_index_buffer");
  return pool_.getOrCreateReorderedIndexBuffer(
      device_,
      sourceHandle.value,
      key,
      bytes,
      seqId,
      completedSeqId_);
}

void CommandQueue::startThreads(std::function<void()> encodeLoop,
                                 std::function<void()> finishLoop,
                                 std::function<void()> completionLoop) {
  if (threadsStarted_) {
    return;
  }
  stop_ = false;
  queueLifecycle_.resetPendingCompletionStop();
  encodeThread_ = std::thread(
      makeQueueWorkerLoop(QueueWorkerRole::Encode, std::move(encodeLoop)));
  finishThread_ = std::thread(
      makeQueueWorkerLoop(QueueWorkerRole::Finish, std::move(finishLoop)));
  completionThread_ = std::thread(
      makeQueueWorkerLoop(QueueWorkerRole::Completion, std::move(completionLoop)));
  threadsStarted_ = true;
}

void CommandQueue::stopThreads() {
  if (!threadsStarted_) {
    return;
  }
  {
    const auto qmxBegin = queueMutexProbeBegin();
    std::lock_guard lock(mutex_);
    QueueMutexProbeScope qmxScope(qmxBegin, "stop_threads");
    requestSchedulingStopLocked();
  }
  if (encodeThread_.joinable()) encodeThread_.join();
  if (completionThread_.joinable()) completionThread_.join();
  if (finishThread_.joinable()) finishThread_.join();
  schedulingProgressWatchdog_.stop();
  threadsStarted_ = false;
}

// --- Chunk-ring submission (Step 3b migration from MetalBackendDevice) --

namespace {

core::ChunkSlot& currentSlotUnlocked(CommandQueue& q) {
  // TLA+: RingSafety — caller holds q.mutex_ and has ensured a writing slot.
  auto& control = q.slots_[*q.writingSlot_];
  auto* payload = q.cpuReadyTape_.resolveForWrite(
      core::CpuReadyPublicationTicket{
          .id = control.sourceId,
          .storage = control.storage,
      });
  DXMT_ASSERT(payload != nullptr && payload == control.payload);
  if (!payload || payload != control.payload) {
    // This helper's reference return cannot represent a stale locator. Stop
    // every producer/consumer surface and terminate before any release-build
    // dereference rather than returning an invalid reference.
    q.cpuReadyTape_.stopAdmission();
    q.stop_ = true;
    q.writeCv_.notify_all();
    q.encodeCv_.notify_all();
    q.finishCv_.notify_all();
    q.presentCompletedCv_.notify_all();
    std::terminate();
  }
  return *payload;
}

void ensureWritingSlotUnlocked(CommandQueue& q, std::unique_lock<std::mutex>& lock) {
  (void)q.queueLifecycle_.ensureWriterSlot(lock, kMaxQueuedChunks);
}

std::uint64_t seqIdForMark(CommandQueue& q, std::uint64_t seqId) {
  return seqId == 0 ? q.nextSeqId_ : seqId;
}

void markChunkResourcesWithExactSeq(
    resources::Pool& pool,
    std::span<const core::ChunkHandleEntry> entries,
    std::uint64_t seqId) {
  for (const auto& entry : entries) {
    switch (entry.kind) {
    case core::ChunkHandleKind::Texture:
      pool.markTextureUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Surface:
      pool.markSurfaceUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Buffer:
      pool.markBufferUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Shader:
    case core::ChunkHandleKind::VertexDecl:
      break;
    }
  }
}

std::uint64_t steadyClockNanoseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

void noteCurrentSlotCommandAppendStartedUnlocked(CommandQueue& q) {
  if (!q.writingSlot_) {
    return;
  }
  const std::size_t slotIndex = *q.writingSlot_;
  if (slotIndex >= q.slotFirstCommandSteadyNs_.size()) {
    return;
  }
  if (!q.slots_[slotIndex].commandsEmpty()) {
    return;
  }
  if (q.slotFirstCommandSteadyNs_[slotIndex] == 0) {
    q.slotFirstCommandSteadyNs_[slotIndex] = steadyClockNanoseconds();
  }
}

std::uint64_t recordCurrentSlotPublishResidencyUnlocked(
    CommandQueue& q,
    perf::ChunkPublishReason reason) {
  if (!q.writingSlot_) {
    return 0;
  }
  const std::size_t slotIndex = *q.writingSlot_;
  if (slotIndex >= q.slotFirstCommandSteadyNs_.size()) {
    return 0;
  }
  const std::uint64_t started = q.slotFirstCommandSteadyNs_[slotIndex];
  q.slotFirstCommandSteadyNs_[slotIndex] = 0;
  if (started == 0) {
    return 0;
  }
  const std::uint64_t now = steadyClockNanoseconds();
  if (now >= started) {
    const std::uint64_t elapsed = now - started;
    perf::countChunkPublishSlotResidency(reason, elapsed);
    return elapsed;
  }
  return 0;
}

std::size_t drawChunkCommandLimit() {
  static const std::size_t limit = [] {
    const char* env = std::getenv("DXMT9_DRAW_CHUNK_COMMAND_LIMIT");
    if (!env || env[0] == '\0') {
      return std::size_t{0};
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    if (end == env || parsed == 0) {
      return std::size_t{0};
    }
    if (parsed > std::numeric_limits<std::size_t>::max()) {
      return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(parsed);
  }();
  return limit;
}

std::size_t drawPayloadArenaLimitBytes() {
  static const std::size_t limit = [] {
    const char* env = std::getenv("DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES");
    if (!env || env[0] == '\0') {
      return std::size_t{0};
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    if (end == env || parsed == 0) {
      return std::size_t{0};
    }
    return static_cast<std::size_t>(parsed);
  }();
  return limit;
}

std::size_t drawRunPayloadBytes(
    std::span<const core::DrawParam> draws,
    std::span<const core::DrawParamPayloadView> payloads) {
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < draws.size(); ++i) {
    const core::DrawParamPayloadView payload =
        i < payloads.size() ? payloads[i] : core::DrawParamPayloadView{};
    if (payload.userIndexData.size() >
        std::numeric_limits<std::size_t>::max() - payload.userVertexData.size()) {
      return std::numeric_limits<std::size_t>::max();
    }
    const std::size_t payloadBytes =
        payload.userVertexData.size() + payload.userIndexData.size();
    if (payload.bindingOverrideData.size() >
        std::numeric_limits<std::size_t>::max() - payloadBytes) {
      return std::numeric_limits<std::size_t>::max();
    }
    const std::size_t payloadAndBindingBytes =
        payloadBytes + payload.bindingOverrideData.size();
    if (payload.bindingSnapshotData.size() >
        std::numeric_limits<std::size_t>::max() - payloadAndBindingBytes) {
      return std::numeric_limits<std::size_t>::max();
    }
    const std::size_t totalPayloadBytes =
        payloadAndBindingBytes + payload.bindingSnapshotData.size();
    if (totalPayloadBytes > std::numeric_limits<std::size_t>::max() - bytes) {
      return std::numeric_limits<std::size_t>::max();
    }
    bytes += totalPayloadBytes;
  }
  return bytes;
}

template <typename Submission>
std::size_t drawRunSubmissionPayloadBytes(
    std::span<Submission> submissions) {
  std::size_t bytes = 0;
  for (const auto& submission : submissions) {
    const auto& payload = submission.payload;
    if (payload.userIndexData.size() >
        std::numeric_limits<std::size_t>::max() - payload.userVertexData.size()) {
      return std::numeric_limits<std::size_t>::max();
    }
    const std::size_t payloadBytes =
        payload.userVertexData.size() + payload.userIndexData.size();
    if (payload.bindingOverrideData.size() >
        std::numeric_limits<std::size_t>::max() - payloadBytes) {
      return std::numeric_limits<std::size_t>::max();
    }
    const std::size_t payloadAndBindingBytes =
        payloadBytes + payload.bindingOverrideData.size();
    if (payload.bindingSnapshotData.size() >
        std::numeric_limits<std::size_t>::max() - payloadAndBindingBytes) {
      return std::numeric_limits<std::size_t>::max();
    }
    const std::size_t totalPayloadBytes =
        payloadAndBindingBytes + payload.bindingSnapshotData.size();
    if (totalPayloadBytes > std::numeric_limits<std::size_t>::max() - bytes) {
      return std::numeric_limits<std::size_t>::max();
    }
    bytes += totalPayloadBytes;
  }
  return bytes;
}

template <typename Submission>
bool drawSubmissionStatesCompatible(
    const Submission& a,
    const Submission& b) noexcept {
  const bool sameGenerationLane =
      core::drawRunSubmissionSameStateGenerationLane(a, b);
  if (drawRunGroupByGenerationLaneEnabled()) {
    perf::countSubmitDrawRunBatchCompatPair(sameGenerationLane,
                                            sameGenerationLane);
    return sameGenerationLane;
  }
  if (sameGenerationLane) {
#ifndef NDEBUG
    if (a.stateMaterialized && b.stateMaterialized) {
      DXMT_ASSERT(core::drawRunSubmissionStatesCompatibleForBatch(a, b));
    }
#endif
    perf::countSubmitDrawRunBatchCompatPair(true, true);
    return true;
  }
  DXMT_ASSERT(a.stateMaterialized && b.stateMaterialized);
  const bool compatible = core::drawRunSubmissionStatesCompatibleForBatch(a, b);
  perf::countSubmitDrawRunBatchCompatPair(false, compatible);
  if (!compatible) {
    const auto& aState = a.materializedState();
    const auto& bState = b.materializedState();
    const auto incompat = core::classifyDrawRunBatchIncompatibility(
        aState.hot, bState.hot, aState.shaderLayout, bState.shaderLayout);
    perf::countSubmitDrawRunBatchIncompat(
        static_cast<std::uint8_t>(incompat.firstDiff), incompat.textureOnly);
    if (incompat.renderStateDiff !=
        core::DrawRunBatchRenderStateDiffClass::None) {
      perf::countSubmitDrawRunBatchIncompatRenderState(
          static_cast<std::uint8_t>(incompat.renderStateDiff));
    }
  }
  return compatible;
}

template <typename Submission>
bool drawSubmissionStatesCompatibleWithAcceptedPrevious(
    const Submission& base,
    const Submission& previous,
    const Submission& candidate) noexcept {
  if (drawRunGroupByGenerationLaneEnabled()) {
    return drawSubmissionStatesCompatible(base, candidate);
  }

  const bool sameBaseGenerationLane =
      core::drawRunSubmissionSameStateGenerationLane(base, candidate);
  if (sameBaseGenerationLane) {
#ifndef NDEBUG
    if (base.stateMaterialized && candidate.stateMaterialized) {
      DXMT_ASSERT(core::drawRunSubmissionStatesCompatibleForBatch(base,
                                                                  candidate));
    }
#endif
    perf::countSubmitDrawRunBatchCompatPair(true, true);
    return true;
  }

  if (core::drawRunSubmissionUsesAcceptedPreviousStateGenerationLane(
          base, previous, candidate)) {
    // The previous submission is already in this batch; sharing its producer
    // stamp makes the elided candidate compatible by transitivity.
    perf::countSubmitDrawRunBatchCompatPair(false, true);
    return true;
  }

  DXMT_ASSERT(candidate.stateMaterialized);
  const bool compatible =
      core::drawRunSubmissionStatesCompatibleForBatch(base, candidate);
  perf::countSubmitDrawRunBatchCompatPair(false, compatible);
  if (!compatible) {
    const auto& baseState = base.materializedState();
    const auto& candidateState = candidate.materializedState();
    const auto incompat = core::classifyDrawRunBatchIncompatibility(
        baseState.hot, candidateState.hot, baseState.shaderLayout,
        candidateState.shaderLayout);
    perf::countSubmitDrawRunBatchIncompat(
        static_cast<std::uint8_t>(incompat.firstDiff), incompat.textureOnly);
    if (incompat.renderStateDiff !=
        core::DrawRunBatchRenderStateDiffClass::None) {
      perf::countSubmitDrawRunBatchIncompatRenderState(
          static_cast<std::uint8_t>(incompat.renderStateDiff));
    }
  }
  return compatible;
}

template <typename Submission>
void countDrawSubmissionAdjacentStateGenerations(
    std::span<Submission> submissions) noexcept {
  for (std::size_t i = 1; i < submissions.size(); ++i) {
    perf::countSubmitDrawRunBatchSubmissionAdjacent(
        core::drawRunSubmissionSameStateGenerationLane(submissions[i - 1],
                                                       submissions[i]));
  }
}

template <typename Submission>
void countDrawRunBatchDiscardedMaterializedStates(
    std::span<Submission> submissions) noexcept {
  if (submissions.size() <= 1u) {
    return;
  }
  std::uint64_t records = 0;
  for (std::size_t i = 1; i < submissions.size(); ++i) {
    if (submissions[i].stateMaterialized) {
      ++records;
    }
  }
  if (records == 0) {
    return;
  }
  perf::countSubmitDrawRunBatchDiscardedState(
      records, records * core::drawRunSubmissionStateCopyBytes());
}

struct DrawSubmitScratch {
  std::vector<core::DrawBindingSnapshot> bindingSnapshots;
  std::vector<core::DrawParamPayloadView> snapshotPayloads;
  std::vector<core::DrawRunSubmission> arenaSubmissions;
  bool inUse = false;
};

DrawSubmitScratch& drawSubmitScratch() {
  static thread_local DrawSubmitScratch scratch;
  return scratch;
}

class ScopedDrawSubmitScratchUse {
public:
  explicit ScopedDrawSubmitScratchUse(DrawSubmitScratch& scratch) noexcept
      : scratch_(scratch) {
    DXMT_ASSERT(!scratch_.inUse);
    scratch_.inUse = true;
  }

  ~ScopedDrawSubmitScratchUse() {
    scratch_.bindingSnapshots.clear();
    scratch_.snapshotPayloads.clear();
    scratch_.arenaSubmissions.clear();
    scratch_.inUse = false;
  }

  ScopedDrawSubmitScratchUse(const ScopedDrawSubmitScratchUse&) = delete;
  ScopedDrawSubmitScratchUse& operator=(const ScopedDrawSubmitScratchUse&) = delete;

private:
  DrawSubmitScratch& scratch_;
};

template <typename Submission>
void prepareDrawRunBatchBindingOverrides(
    std::span<Submission> submissions) noexcept {
  if (submissions.empty()) {
    return;
  }
  const auto& base = submissions.front();
  DXMT_ASSERT(base.stateMaterialized);
  for (auto& submission : submissions) {
    if (!submission.stateMaterialized) {
      if (core::drawRunSubmissionHasExternalBindingOverride(submission)) {
        core::ensureDrawRunSubmissionBindingOverridePayload(submission);
      } else {
        submission.bindingOverride = {};
      }
      continue;
    }
    core::prepareDrawRunSubmissionBindingOverride(base, submission);
  }
}

core::DrawParamPayloadView drawPayloadAt(
    std::span<const core::DrawParamPayloadView> payloads,
    std::size_t index) noexcept {
  return index < payloads.size() ? payloads[index] : core::DrawParamPayloadView{};
}

bool copyDrawBindingOverride(std::span<const core::u8> bytes,
                             core::DrawBindingOverride& out) noexcept {
  if (bytes.size() != sizeof(core::DrawBindingOverride)) {
    out = {};
    return false;
  }
  std::memcpy(&out, bytes.data(), sizeof(out));
  return !core::drawBindingOverrideEmpty(out);
}

bool copyDrawBindingSnapshot(std::span<const core::u8> bytes,
                             core::DrawBindingSnapshot& out) noexcept {
  if (bytes.size() != sizeof(core::DrawBindingSnapshot)) {
    out = {};
    return false;
  }
  std::memcpy(&out, bytes.data(), sizeof(out));
  return !core::drawBindingSnapshotEmpty(out);
}

void addDynamicBufferSnapshots(
    resources::Pool& pool,
    const core::FlatDrawStateRecord& hot,
    const core::DrawParam& draw,
    const core::DrawParamPayloadView& payload,
    core::DrawBindingSnapshot& out,
    bool& addedSnapshots) {
  std::array<core::Handle, core::kMaxStreams> streamBuffers = hot.streamBuffers;
  std::array<core::u32, core::kMaxStreams> streamOffsets = hot.streamOffsets;
  std::array<core::u32, core::kMaxStreams> streamStrides = hot.streamStrides;
  core::Handle indexBuffer = hot.indexBuffer;
  core::IndexType indexType = draw.indexType;

  core::DrawBindingOverride existing{};
  copyDrawBindingOverride(payload.bindingOverrideData, existing);
  out = {};

  for (core::u32 stream = 0; stream < core::kMaxStreams; ++stream) {
    if ((existing.streamMask & (1u << stream)) == 0u) {
      continue;
    }
    streamBuffers[stream] = existing.streams[stream].buffer;
    streamOffsets[stream] = existing.streams[stream].offset;
    streamStrides[stream] = existing.streams[stream].stride;
  }
  if (existing.indexBufferValid) {
    indexBuffer = existing.indexBuffer;
    indexType = existing.indexType;
  }

  for (core::u32 stream = 0; stream < core::kMaxStreams; ++stream) {
    if (!streamBuffers[stream]) {
      continue;
    }
    const auto snapshot = pool.snapshotBufferBinding(streamBuffers[stream]);
    if (!snapshot.valid()) {
      continue;
    }
    out.streamMask |= 1u << stream;
    out.streams[stream].buffer = streamBuffers[stream];
    out.streams[stream].offset = streamOffsets[stream];
    out.streams[stream].stride = streamStrides[stream];
    out.streams[stream].snapshot = snapshot;
    addedSnapshots = true;
  }

  if (draw.indexed && indexBuffer) {
    const auto snapshot = pool.snapshotBufferBinding(indexBuffer);
    if (snapshot.valid()) {
      out.indexBuffer = indexBuffer;
      out.indexType = indexType;
      out.indexSnapshot = snapshot;
      out.indexSnapshotValid = true;
      addedSnapshots = true;
    }
  }
}

std::span<const core::DrawParamPayloadView> snapshotDrawRunBindingPayloads(
    resources::Pool& pool,
    const core::FlatDrawStateRecord& hot,
    std::span<const core::DrawParam> draws,
    std::span<const core::DrawParamPayloadView> payloads,
    std::vector<core::DrawBindingSnapshot>& bindingSnapshots,
    std::vector<core::DrawParamPayloadView>& snapshotPayloads) {
  bindingSnapshots.clear();
  snapshotPayloads.clear();
  bindingSnapshots.reserve(draws.size());
  snapshotPayloads.reserve(draws.size());

  bool anyPayloadChanged = false;
  for (std::size_t i = 0; i < draws.size(); ++i) {
    const auto payload = drawPayloadAt(payloads, i);
    if (!payload.bindingSnapshotData.empty()) {
      snapshotPayloads.push_back(payload);
      continue;
    }
    core::DrawBindingSnapshot snapshot{};
    bool addedSnapshots = false;
    addDynamicBufferSnapshots(pool, hot, draws[i], payload, snapshot, addedSnapshots);
    if (addedSnapshots) {
      bindingSnapshots.push_back(snapshot);
      snapshotPayloads.push_back(core::DrawParamPayloadView{
          .userVertexData = payload.userVertexData,
          .userIndexData = payload.userIndexData,
          .bindingOverrideData = payload.bindingOverrideData,
          .bindingSnapshotData = core::drawBindingSnapshotBytes(bindingSnapshots.back()),
      });
      anyPayloadChanged = true;
    } else {
      snapshotPayloads.push_back(payload);
    }
  }

  if (!anyPayloadChanged) {
    bindingSnapshots.clear();
    snapshotPayloads.clear();
    return payloads;
  }
  return snapshotPayloads;
}

template <typename Submission>
void snapshotDrawSubmissionBindingPayloads(
    resources::Pool& pool,
    std::span<Submission> submissions,
    std::vector<core::DrawBindingSnapshot>& bindingSnapshots) {
  bindingSnapshots.clear();
  bindingSnapshots.reserve(submissions.size());
  if (submissions.empty()) {
    return;
  }
  DXMT_ASSERT(submissions.front().stateMaterialized);
  const auto& frontHot = submissions.front().materializedState().hot;

  for (auto& submission : submissions) {
    core::DrawParamPayloadView payload = submission.payload;
    if (!payload.bindingSnapshotData.empty()) {
      continue;
    }
    core::DrawBindingSnapshot snapshot{};
    bool addedSnapshots = false;
    const auto& hot = submission.stateMaterialized
        ? submission.materializedState().hot
        : frontHot;
    addDynamicBufferSnapshots(pool,
                              hot,
                              submission.draw,
                              payload,
                              snapshot,
                              addedSnapshots);
    if (!addedSnapshots) {
      continue;
    }
    bindingSnapshots.push_back(snapshot);
    submission.payload.bindingSnapshotData =
        core::drawBindingSnapshotBytes(bindingSnapshots.back());
  }
}

void markDrawBindingOverrideResource(resources::Pool& pool,
                                     const core::DrawBindingOverride& binding,
                                     std::uint64_t seqId) {
  for (std::uint32_t stream = 0; stream < core::kMaxStreams; ++stream) {
    if ((binding.streamMask & (1u << stream)) == 0) {
      continue;
    }
    pool.markBufferUse(binding.streams[stream].buffer, seqId);
  }
  if (binding.indexBufferValid) {
    pool.markBufferUse(binding.indexBuffer, seqId);
  }
}

void markDrawBindingSnapshotResource(resources::Pool& pool,
                                     const core::DrawBindingSnapshot& binding,
                                     std::uint64_t seqId) {
  for (std::uint32_t stream = 0; stream < core::kMaxStreams; ++stream) {
    if ((binding.streamMask & (1u << stream)) == 0) {
      continue;
    }
    pool.markBufferSnapshotUse(binding.streams[stream].buffer,
                               binding.streams[stream].snapshot,
                               seqId);
  }
  if (binding.indexSnapshotValid) {
    pool.markBufferSnapshotUse(binding.indexBuffer,
                               binding.indexSnapshot,
                               seqId);
  }
}

void markDrawBindingOverrideResources(
    resources::Pool& pool,
    std::span<const core::DrawParamPayloadView> payloads,
    std::uint64_t seqId) {
  for (const auto& payload : payloads) {
    if (payload.bindingOverrideData.size() == sizeof(core::DrawBindingOverride)) {
      core::DrawBindingOverride binding{};
      std::memcpy(&binding, payload.bindingOverrideData.data(), sizeof(binding));
      markDrawBindingOverrideResource(pool, binding, seqId);
    }
  }
}

void markDrawBindingSnapshotResources(
    resources::Pool& pool,
    std::span<const core::DrawParamPayloadView> payloads,
    std::uint64_t seqId) {
  for (const auto& payload : payloads) {
    if (payload.bindingSnapshotData.size() == sizeof(core::DrawBindingSnapshot)) {
      core::DrawBindingSnapshot snapshot{};
      if (copyDrawBindingSnapshot(payload.bindingSnapshotData, snapshot)) {
        markDrawBindingSnapshotResource(pool, snapshot, seqId);
      }
    }
  }
}

void markDrawRunPayloadResources(resources::Pool& pool,
                                 const core::MetalCommandView& command,
                                 std::uint64_t seqId) {
  const auto arena = core::drawRunPayloadBytes(command);
  for (const auto& param : command.drawParams) {
    const auto bytes = core::drawRunPayloadBytes(param.bindingOverrideRange, arena);
    if (bytes.size() == sizeof(core::DrawBindingOverride)) {
      core::DrawBindingOverride binding{};
      std::memcpy(&binding, bytes.data(), sizeof(binding));
      markDrawBindingOverrideResource(pool, binding, seqId);
    }
    const auto snapshotBytes =
        core::drawRunPayloadBytes(param.bindingSnapshotRange, arena);
    if (snapshotBytes.size() != sizeof(core::DrawBindingSnapshot)) {
      continue;
    }
    core::DrawBindingSnapshot snapshot{};
    if (copyDrawBindingSnapshot(snapshotBytes, snapshot)) {
      markDrawBindingSnapshotResource(pool, snapshot, seqId);
    }
  }
}

void markArenaSourceResources(resources::Pool& pool,
                              const core::SourcePayloadView& payload,
                              std::uint64_t seqId) {
  for (std::size_t i = 0; i < payload.commandCount(); ++i) {
    const auto sourceCommand = payload.commandAt(i);
    const auto command = sourceCommand.command;
    switch (command.kind) {
    case core::MetalCommandKind::DrawRun:
      if (command.drawState.hot) {
        pool.markDrawResources(*command.drawState.hot, seqId);
      }
      markDrawRunPayloadResources(pool, command, seqId);
      break;
    case core::MetalCommandKind::Clear:
      if (sourceCommand.clear) {
        if (sourceCommand.clear->clearColor) {
          for (const auto& attachment : sourceCommand.clear->colorAttachments) {
            pool.markSurfaceUse(attachment.handle, seqId);
          }
        }
        if (sourceCommand.clear->clearDepth ||
            sourceCommand.clear->clearStencil) {
          pool.markSurfaceUse(sourceCommand.clear->depthStencil.handle, seqId);
        }
      }
      break;
    case core::MetalCommandKind::SurfaceCopy:
      if (command.surfaceCopy) {
        pool.markSurfaceCopyResources(*command.surfaceCopy, seqId);
      }
      break;
    case core::MetalCommandKind::StretchRect:
      if (command.stretchRect) {
        pool.markStretchResources(*command.stretchRect, seqId);
      }
      break;
    case core::MetalCommandKind::Readback:
      if (command.readback) {
        pool.markReadbackResources(*command.readback, seqId);
      }
      break;
    case core::MetalCommandKind::ColorFill:
      if (command.colorFill) {
        pool.markColorFillResources(*command.colorFill, seqId);
      }
      break;
    case core::MetalCommandKind::DepthResolve:
      if (command.depthResolve) {
        pool.markDepthResolveResources(*command.depthResolve, seqId);
      }
      break;
    case core::MetalCommandKind::Present:
      if (command.present && command.present->presentSource) {
        pool.markSurfaceUse(command.present->presentSource, seqId);
      }
      break;
    }
  }
}

bool validateArenaDrawAdmissionSnapshots(
    const resources::Pool& pool,
    const core::SourcePayloadView& payload) noexcept {
  const auto validateBinding = [&](core::Handle handle,
                                   const core::DrawBufferBindingSnapshot& value) {
    return pool.validateCapturedBufferSnapshot(handle, value) !=
           resources::CapturedBufferSnapshotStatus::Invalid;
  };

  for (std::size_t i = 0; i < payload.commandCount(); ++i) {
    const auto command = payload.commandAt(i).command;
    if (command.kind != core::MetalCommandKind::DrawRun ||
        !command.drawState.hot) {
      continue;
    }
    const auto& hot = *command.drawState.hot;
    const auto arena = core::drawRunPayloadBytes(command);
    for (const auto& param : command.drawParams) {
      core::DrawBindingOverride override{};
      const auto overrideBytes =
          core::drawRunPayloadBytes(param.bindingOverrideRange, arena);
      if (!overrideBytes.empty() &&
          !copyDrawBindingOverride(overrideBytes, override)) {
        return false;
      }
      core::DrawBindingSnapshot snapshot{};
      const auto snapshotBytes =
          core::drawRunPayloadBytes(param.bindingSnapshotRange, arena);
      if (!snapshotBytes.empty() &&
          !copyDrawBindingSnapshot(snapshotBytes, snapshot)) {
        return false;
      }

      const std::uint32_t streamMask = hot.streamMask | override.streamMask;
      for (std::uint32_t stream = 0; stream < core::kMaxStreams; ++stream) {
        if ((streamMask & (1u << stream)) == 0) {
          continue;
        }
        const core::Handle handle =
            (override.streamMask & (1u << stream)) != 0
                ? override.streams[stream].buffer
                : hot.streamBuffers[stream];
        const bool snapshotMatches =
            (snapshot.streamMask & (1u << stream)) != 0 &&
            snapshot.streams[stream].buffer == handle;
        if (!validateBinding(
                handle, snapshotMatches
                            ? snapshot.streams[stream].snapshot
                            : core::DrawBufferBindingSnapshot{})) {
          return false;
        }
      }

      if (!param.indexed) {
        continue;
      }
      const core::Handle indexBuffer = override.indexBufferValid
          ? override.indexBuffer
          : hot.indexBuffer;
      const bool snapshotMatches = snapshot.indexSnapshotValid &&
                                   snapshot.indexBuffer == indexBuffer;
      if (!validateBinding(
              indexBuffer, snapshotMatches
                               ? snapshot.indexSnapshot
                               : core::DrawBufferBindingSnapshot{})) {
        return false;
      }
    }
  }
  return true;
}

bool splitStretchChunk() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_SPLIT_STRETCH_CHUNK");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

// Boundary-wait policy — resolved once per process via
// dxmt9::resolveBoundaryPolicyFromEnv() (see dxmt9_presenter.hpp).
// The boundary enum collapses the previous trio of env-parsing
// lambdas (DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION /
// DXMT9_PRESENT_BOUNDARY_COMPLETION / DXMT9_DISABLE_PRESENT_BOUNDARY)
// into a single switch — see resolveBoundaryPolicy() doc-comment for
// the priority ordering. AfterAcquire is observationally a no-op on
// the wait branch here; its effect lives in dxmt9_draw_encoder_chunk.mm,
// which also reads through resolveBoundaryPolicyFromEnv().
//
// capFrameLatencyToBackBuffers() / backBufferLatencyCap() /
// presentBoundaryLatency() / PresentBoundaryAction /
// resolvePresentBoundaryAction() / offloadCommitReplayEnabled() now live in
// dxmt9_command_queue.hpp (R-BACK-2.51) so the commit-replay offload's
// present-ordinal boundary (CommandQueue::waitPresentOrdinalBoundary) shares
// the exact same cap math and per-present decision truth table instead of
// duplicating them, and so both are unit-testable from
// present_ordinal_boundary_spec.cpp without a live queue.

std::uint64_t presentBoundaryTargetSeqId(std::uint64_t presentSeqId,
                                         std::uint32_t maxFrameLatency) {
  if (presentSeqId == 0) {
    return 0;
  }
  maxFrameLatency = std::clamp<std::uint32_t>(
      maxFrameLatency, 1u, kMaxQueuedChunks);
  if (presentSeqId <= maxFrameLatency) {
    return 0;
  }
  return presentSeqId - maxFrameLatency;
}

std::uint64_t deferredPresentBoundaryTargetSeqId(std::uint64_t presentSeqId,
                                                 std::uint32_t maxFrameLatency) {
  if (presentSeqId == std::numeric_limits<std::uint64_t>::max()) {
    return presentBoundaryTargetSeqId(presentSeqId, maxFrameLatency);
  }
  return presentBoundaryTargetSeqId(presentSeqId + 1, maxFrameLatency);
}

BoundaryPolicy presentBoundaryWaitPolicy(BoundaryPolicy policy) {
  if (policy == BoundaryPolicy::DeferredPresentCompletion) {
    return BoundaryPolicy::PresentCompletion;
  }
  return policy;
}

Presenter::AcquireParams makePresentAcquireParams(const core::SwapDesc& desc) {
  return Presenter::AcquireParams{
      .width = desc.width,
      .height = desc.height,
      .displaySyncEnabled = desc.displaySyncEnabled,
      .contentsScale = 1.0,
      .maxDrawableCount = kDefaultMetalDrawableCount,
  };
}

void markSlotResourcesUnlocked(resources::Pool& pool, const core::ChunkSlot& slot) {
  for (std::size_t i = 0; i < slot.commandCount(); ++i) {
    const auto command = slot.commandAt(i);
    switch (command.kind) {
      case core::MetalCommandKind::DrawRun:
        if (command.drawState.hot) pool.markDrawResources(*command.drawState.hot, slot.seqId);
        markDrawRunPayloadResources(pool, command, slot.seqId);
        break;
      case core::MetalCommandKind::Clear:
        if (command.clear) pool.markClearResources(*command.clear, slot.seqId);
        break;
      case core::MetalCommandKind::SurfaceCopy:
        if (command.surfaceCopy) pool.markSurfaceCopyResources(*command.surfaceCopy, slot.seqId);
        break;
      case core::MetalCommandKind::StretchRect:
        if (command.stretchRect) pool.markStretchResources(*command.stretchRect, slot.seqId);
        break;
      case core::MetalCommandKind::Readback:
        if (command.readback) pool.markReadbackResources(*command.readback, slot.seqId);
        break;
      case core::MetalCommandKind::ColorFill:
        if (command.colorFill) pool.markColorFillResources(*command.colorFill, slot.seqId);
        break;
      case core::MetalCommandKind::DepthResolve:
        if (command.depthResolve) pool.markDepthResolveResources(*command.depthResolve, slot.seqId);
        break;
      case core::MetalCommandKind::Present:
        if (command.present && command.present->presentSource) {
          pool.markSurfaceUse(command.present->presentSource, slot.seqId);
        }
        break;
    }
  }
}

struct PresentPrePresentOpportunity {
  std::uint64_t commandCount = 0;
  std::uint64_t drawRunCount = 0;
  std::uint64_t drawItemCount = 0;
  std::uint64_t nonDrawCommandCount = 0;
  perf::ChunkPublishTailCommandKind tailKind =
      perf::ChunkPublishTailCommandKind::Empty;
  bool drawOnly = false;
  bool presentIsTail = false;

  bool valid() const noexcept {
    return commandCount != 0;
  }
};

perf::ChunkPublishTailCommandKind chunkPublishTailCommandKind(
    core::MetalCommandKind kind) {
  switch (kind) {
  case core::MetalCommandKind::DrawRun:
    return perf::ChunkPublishTailCommandKind::DrawRun;
  case core::MetalCommandKind::Clear:
    return perf::ChunkPublishTailCommandKind::Clear;
  case core::MetalCommandKind::SurfaceCopy:
    return perf::ChunkPublishTailCommandKind::SurfaceCopy;
  case core::MetalCommandKind::StretchRect:
    return perf::ChunkPublishTailCommandKind::StretchRect;
  case core::MetalCommandKind::Readback:
    return perf::ChunkPublishTailCommandKind::Readback;
  case core::MetalCommandKind::ColorFill:
    return perf::ChunkPublishTailCommandKind::ColorFill;
  case core::MetalCommandKind::DepthResolve:
    return perf::ChunkPublishTailCommandKind::DepthResolve;
  case core::MetalCommandKind::Present:
    return perf::ChunkPublishTailCommandKind::Present;
  }
  return perf::ChunkPublishTailCommandKind::Empty;
}

PresentPrePresentOpportunity analyzePresentPrePresentOpportunity(
    const core::ChunkSlot& slot) {
  PresentPrePresentOpportunity stats{};
  const auto commandCount = slot.commandHeaders.size();
  std::size_t firstPresent = commandCount;
  for (std::size_t i = 0; i < commandCount; ++i) {
    if (slot.commandHeaders[i].kind == core::MetalCommandKind::Present) {
      firstPresent = i;
      break;
    }
  }
  if (firstPresent == 0 || firstPresent == commandCount) {
    return stats;
  }

  stats.commandCount = firstPresent;
  stats.tailKind =
      chunkPublishTailCommandKind(slot.commandHeaders[firstPresent - 1].kind);
  stats.presentIsTail = (firstPresent + 1 == commandCount);
  for (std::size_t i = 0; i < firstPresent; ++i) {
    const auto& header = slot.commandHeaders[i];
    if (header.kind == core::MetalCommandKind::DrawRun) {
      ++stats.drawRunCount;
      const std::uint32_t drawRunIndex = header.payloadIndex.value;
      if (drawRunIndex < slot.drawRunRecords.size()) {
        stats.drawItemCount += slot.drawRunRecords[drawRunIndex].paramCount;
      }
    } else {
      ++stats.nonDrawCommandCount;
    }
  }
  stats.drawOnly = stats.nonDrawCommandCount == 0;
  return stats;
}

void prepareSlotForPublish(CommandQueue& q,
                           resources::Pool& pool,
                           core::ChunkSlot& slot,
                           perf::ChunkPublishReason reason) {
  slot.publishReason = reason;
  const std::uint64_t residencyNs =
      recordCurrentSlotPublishResidencyUnlocked(q, reason);
  perf::countChunkPublishReason(reason, slot.commandCount());
  if (reason == perf::ChunkPublishReason::Present) {
    const auto stats = analyzePresentPrePresentOpportunity(slot);
    if (stats.valid()) {
      perf::countChunkPublishPresentPrePresentOpportunity(
          stats.commandCount, stats.drawRunCount, stats.drawItemCount,
          stats.nonDrawCommandCount, slot.drawPayloadArena.size(), residencyNs,
          stats.presentIsTail);
      perf::countChunkPublishPresentPrePresentOpportunityTail(stats.tailKind,
                                                              stats.drawOnly);
    }
  }
  PerfScope scope(perf::countPrepareSlotForPublishCpuTime);
  {
    PerfScope stageScope(perf::countPrepareSlotResourceMarkCpuTime);
    markSlotResourcesUnlocked(pool, slot);
  }
}

bool maybeCommitDrawChunkUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock) {
  const std::size_t limit = drawChunkCommandLimit();
  if (limit == 0 || !q.writingSlot_) {
    return false;
  }
  if (currentSlotUnlocked(q).commandCount() < limit) {
    return false;
  }
  const bool committed = q.queueLifecycle_.commitCurrentChunk(
      lock, kMaxQueuedChunks, [&q, &pool](core::ChunkSlot& slot) {
        prepareSlotForPublish(q, pool, slot,
                              perf::ChunkPublishReason::DrawLimit);
      });
  if (committed && q.skipDrawResourceMarking_) {
    q.forceDrawResourceMarkingAfterSplit_ = true;
  }
  return committed;
}

bool maybeCommitDrawPayloadArenaUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock,
    std::size_t pendingPayloadBytes) {
  const std::size_t limit = drawPayloadArenaLimitBytes();
  if (limit == 0 || pendingPayloadBytes == 0 || !q.writingSlot_) {
    return false;
  }

  auto& slot = currentSlotUnlocked(q);
  if (slot.commandsEmpty()) {
    return false;
  }
  if (slot.drawPayloadArena.size() <= limit &&
      pendingPayloadBytes <= limit - slot.drawPayloadArena.size()) {
    return false;
  }

  const bool committed = q.queueLifecycle_.commitCurrentChunk(
      lock, kMaxQueuedChunks, [&q, &pool](core::ChunkSlot& publishSlot) {
        prepareSlotForPublish(q, pool, publishSlot,
                              perf::ChunkPublishReason::PayloadLimit);
      });
  if (committed) {
    if (q.skipDrawResourceMarking_) {
      q.forceDrawResourceMarkingAfterSplit_ = true;
    }
    ensureWritingSlotUnlocked(q, lock);
  }
  return committed;
}

}  // namespace

void CommandQueue::setSkipDrawResourceMarking(bool skip) {
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "set_skip_draw_resource_marking");
  skipDrawResourceMarking_ = skip;
  forceDrawResourceMarkingAfterSplit_ = false;
}

void CommandQueue::noteCommitChunkEntryForCompletionGap() {
  queueLifecycle_.recordCompletionWaitCommitChunkEntry();
  queueLifecycle_.recordNoEnqueueWaitGapToCommitChunkEntry();
}

void CommandQueue::noteCommitChunkReplayStartForCompletionGap() {
  queueLifecycle_.recordCompletionWaitCommitChunkReplayStart();
  queueLifecycle_.recordNoEnqueueWaitGapToCommitChunkReplayStart();
}

void CommandQueue::noteCommitChunkReplayEndForCompletionGap(
    std::uint64_t replayNanoseconds) {
  queueLifecycle_.recordCompletionWaitCommitChunkReplayEnd(replayNanoseconds);
  queueLifecycle_.recordNoEnqueueWaitGapToCommitChunkReplayEnd();
}

void CommandQueue::noteCommitChunkReplayCpuBeforePublish(
    std::uint64_t nanoseconds) {
  queueLifecycle_.recordNoEnqueueCommitChunkReplayCpuBeforePublish(nanoseconds);
}

void CommandQueue::noteCommitChunkActiveReplayCpuBeforePublish(
    std::uint64_t nanoseconds) {
  queueLifecycle_.recordNoEnqueueCommitChunkActiveReplayCpuBeforePublish(nanoseconds);
}

void CommandQueue::noteCommitChunkRecordShapeForCompletionGap(
    const core::metalqueue::NoEnqueueCommitChunkRecordShape& shape) {
  queueLifecycle_.recordNoEnqueueCommitChunkRecordShapeBeforePublish(shape);
}

void CommandQueue::prefetchCurrentWritingSlotPipelines() {
  if (!unpublishedSlotPsoPrefetchEnabled()) {
    return;
  }

  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "prefetch_current_writing_slot_pipelines");
  if (!writingSlot_) {
    return;
  }
  auto& slot = currentSlotUnlocked(*this);
  if (slot.commandsEmpty() || slot.prefetchedPipelinesSealed()) {
    return;
  }

  PerfScope stageScope(perf::countUnpublishedSlotPsoPrefetchCpuTime);
  prefetchSlotPipelines(slot, /*seal=*/false);
}

namespace {
// Splits the queue-side mark call's cost into "waiting for the queue mutex"
// and "marking". The distinction decides what the remaining commit_chunk
// cost IS: the body below is a short loop over unique handles, so if the
// ~19us this call costs per commit (state-churn-encode-append-decomposition.06)
// is the acquire, the producer is contending with the encode thread rather
// than doing work, and that is a different problem with a different fix.
// Same env gate as the other commit_chunk phases (PE-side
// commitChunkPhaseSplitEnabled() in device_c_chunk_replay.cpp); resolved once
// per TU since the two live on opposite sides of the PE/unix boundary.
bool markChunkPhaseSplitEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}
}  // namespace

void CommandQueue::markChunkResources(std::span<const core::ChunkHandleEntry> entries) {
  if (entries.empty()) {
    return;
  }
  const bool phaseSplit = markChunkPhaseSplitEnabled();
  const auto lockWaitStart = phaseSplit ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "mark_chunk_resources");
  if (phaseSplit) {
    perf::countCommitChunkPhaseMarkLockCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - lockWaitStart).count()));
  }
  // Single seqId snapshot for the whole bulk-mark — the importer is
  // about to emit Draw* records onto the same chunk, so all resources
  // get pinned to the chunk's nextSeqId together.
  const std::uint64_t seqId = seqIdForMark(*this, 0);
  for (const auto& entry : entries) {
    switch (entry.kind) {
    case core::ChunkHandleKind::Texture:
      pool_.markTextureUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Surface:
      pool_.markSurfaceUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Buffer:
      pool_.markBufferUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Shader:
    case core::ChunkHandleKind::VertexDecl:
      // No pool table for these yet — kinds reserved for future use.
      break;
    }
  }
}

core::ChunkBufferBindingCaptureResult
CommandQueue::markChunkResourcesAndCaptureBufferBindings(
    std::span<const core::ChunkHandleEntry> entries,
    std::vector<core::ChunkBufferBindingSnapshot>& snapshots) {
  snapshots.clear();
  snapshots.reserve(entries.size());
  if (entries.empty()) {
    return core::ChunkBufferBindingCaptureResult::Complete;
  }
  // See markChunkResources above: same env-gated acquire/marking split, and
  // this is the default (non-legacy) path most commits actually take, which
  // previously left commit_chunk_phase_mark_lock_cpu_ms at 0 on every run.
  const bool phaseSplit = markChunkPhaseSplitEnabled();
  const auto lockWaitStart = phaseSplit ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  QueueMutexProbeScope qmxScope(
      qmxBegin, "mark_chunk_resources_and_capture_buffer_bindings");
  if (phaseSplit) {
    perf::countCommitChunkPhaseMarkLockCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - lockWaitStart).count()));
  }
  const std::uint64_t seqId = seqIdForMark(*this, 0);
  bool missingRequired = false;
  for (const auto& entry : entries) {
    switch (entry.kind) {
    case core::ChunkHandleKind::Texture:
      pool_.markTextureUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Surface:
      pool_.markSurfaceUse(entry.handle, seqId);
      break;
    case core::ChunkHandleKind::Buffer:
      pool_.markBufferUse(entry.handle, seqId);
      snapshots.push_back(pool_.captureChunkBufferBinding(entry.handle));
      missingRequired |= snapshots.back().requiresCapturedBacking &&
                         !snapshots.back().snapshot.valid();
      break;
    case core::ChunkHandleKind::Shader:
    case core::ChunkHandleKind::VertexDecl:
      break;
    }
  }
  return missingRequired
             ? core::ChunkBufferBindingCaptureResult::MissingRequired
             : core::ChunkBufferBindingCaptureResult::Complete;
}

core::ChunkBufferBindingCaptureResult
CommandQueue::captureChunkBufferBindings(
    std::span<const core::ChunkHandleEntry> entries,
    std::vector<core::ChunkBufferBindingSnapshot>& snapshots) {
  snapshots.clear();
  snapshots.reserve(entries.size());
  if (entries.empty()) {
    return core::ChunkBufferBindingCaptureResult::Complete;
  }
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "capture_chunk_buffer_bindings");
  bool missingRequired = false;
  for (const auto& entry : entries) {
    if (entry.kind != core::ChunkHandleKind::Buffer) {
      continue;
    }
    snapshots.push_back(pool_.captureChunkBufferBinding(entry.handle));
    missingRequired |= snapshots.back().requiresCapturedBacking &&
                       !snapshots.back().snapshot.valid();
  }
  return missingRequired
             ? core::ChunkBufferBindingCaptureResult::MissingRequired
             : core::ChunkBufferBindingCaptureResult::Complete;
}

void CommandQueue::submitDrawRun(core::CanonicalDrawState state,
                                 const core::DrawUniformPayload& uniforms,
                                 std::span<const core::DrawParam> draws,
                                 std::span<const core::DrawParamPayloadView> payloads) {
  for (std::size_t i = 0; i < draws.size(); ++i) {
    perf::countSubmitDraw();
  }
  PerfScope scope(perf::countSubmitDrawCpuTime);
  if (appendActiveArenaDrawRun(state, uniforms, draws, payloads) !=
      ActiveArenaAppendResult::Inactive) {
    return;
  }
  if (draws.empty()) {
    return;
  }
  auto& scratch = drawSubmitScratch();
  ScopedDrawSubmitScratchUse scratchUse(scratch);
  scratch.bindingSnapshots.reserve(draws.size());
  scratch.snapshotPayloads.reserve(draws.size());
  // Per-draw-run hot entry. Heap-allocation invariant per
  // codebase_conventions.rules.md; debug-only guard, no-op unless
  // DXMT_DEBUG_NO_PER_DRAW_ALLOC=1 build flag and env are both set.
  DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE("submitDrawRun");
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold: `lock` is handed by reference to ensureWritingSlotUnlocked /
  // maybeCommitDrawPayloadArenaUnlocked / maybeCommitDrawChunkUnlocked
  // below, which may unlock/relock it (via QueueLifecycleController, a
  // different file) an unbounded number of times before returning, so a
  // single "hold" duration would not be meaningful. Only the acquire-wait
  // for this initial lock is recorded.
  QueueMutexProbeScope qmxScope(qmxBegin, "submit_draw_run", /*skipHold=*/true);
  std::span<const core::DrawParamPayloadView> effectivePayloads{};
  {
    PerfScope stageScope(perf::countSubmitDrawRunBindingSnapshotCpuTime);
    effectivePayloads =
        snapshotDrawRunBindingPayloads(pool_,
                                       state.hot,
                                       draws,
                                       payloads,
                                       scratch.bindingSnapshots,
                                       scratch.snapshotPayloads);
  }
  std::size_t pendingPayloadBytes = 0;
  {
    PerfScope stageScope(perf::countSubmitDrawRunPayloadBytesCpuTime);
    pendingPayloadBytes = drawRunPayloadBytes(draws, effectivePayloads);
  }
  {
    PerfScope stageScope(perf::countSubmitDrawRunSlotPrepareCpuTime);
    ensureWritingSlotUnlocked(*this, lock);
    maybeCommitDrawPayloadArenaUnlocked(*this, pool_, lock, pendingPayloadBytes);
  }
  {
    PerfScope stageScope(perf::countSubmitDrawRunResourceMarkCpuTime);
    const std::uint64_t seqId = seqIdForMark(*this, 0);
    // Chunk handle tables retain the logical BufferHandle once per chunk, but
    // a DYNAMIC + DISCARD buffer can rotate through several concrete Metal
    // backings inside that chunk. Always stamp the per-draw snapshot backing
    // before replay reaches a later DISCARD, even when logical per-draw
    // marking is suppressed by the chunk importer.
    markDrawBindingSnapshotResources(pool_, effectivePayloads, seqId);
    if (!skipDrawResourceMarking_ || forceDrawResourceMarkingAfterSplit_) {
      pool_.markDrawResources(state.hot, seqId);
      markDrawBindingOverrideResources(pool_, effectivePayloads, seqId);
    }
  }
  currentBackBuffer_ = state.hot.colorAttachments[0].handle;
  {
    PerfScope stageScope(perf::countSubmitDrawRunAppendCpuTime);
    noteCurrentSlotCommandAppendStartedUnlocked(*this);
    currentSlotUnlocked(*this).appendDrawRun(
        std::move(state), uniforms, draws, effectivePayloads);
  }
  {
    PerfScope stageScope(perf::countSubmitDrawRunChunkCommitCpuTime);
    (void)maybeCommitDrawChunkUnlocked(*this, pool_, lock);
  }
}

void submitDrawRunBatchImpl(CommandQueue& queue,
                            resources::Pool& pool,
                            std::mutex& mutex,
                            core::Handle& currentBackBuffer,
                            bool skipDrawResourceMarking,
                            bool forceDrawResourceMarkingAfterSplit,
                            std::span<core::DrawRunSubmission> submissions) {
  if (submissions.empty()) {
    return;
  }
  auto& scratch = drawSubmitScratch();
  ScopedDrawSubmitScratchUse scratchUse(scratch);
  scratch.bindingSnapshots.reserve(submissions.size());
  DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE("submitDrawRunBatch");
  countDrawSubmissionAdjacentStateGenerations(submissions);
  std::unique_lock lock(mutex, std::defer_lock);
  {
    PerfScope stageScope(perf::countSubmitDrawRunBatchQueueLockCpuTime);
    const auto qmxBegin = queueMutexProbeBegin();
    lock.lock();
    // skipHold: `lock` is handed by reference to ensureWritingSlotUnlocked /
    // maybeCommitDrawPayloadArenaUnlocked / maybeCommitDrawChunkUnlocked
    // below (per batch iteration), which may unlock/relock it an unbounded
    // number of times, so only the acquire-wait for this initial lock() is
    // recorded. This site is not among the 41 acquisitions that spell the
    // member name `mutex_` (it locks the same std::mutex via the `mutex`
    // reference parameter passed from CommandQueue::submitDrawRunBatch), but
    // it is the hot per-draw-run-batch acquisition called by the replay
    // offload worker hundreds of times per present, so it is included here.
    QueueMutexProbeScope qmxScope(
        qmxBegin, "submit_draw_run_batch_impl", /*skipHold=*/true);
  }
  std::size_t batchStart = 0;
  while (batchStart < submissions.size()) {
    std::size_t batchEnd = batchStart + 1u;
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchCompatScanCpuTime);
      while (batchEnd < submissions.size() &&
             drawSubmissionStatesCompatibleWithAcceptedPrevious(
                 submissions[batchStart], submissions[batchEnd - 1u],
                 submissions[batchEnd])) {
        ++batchEnd;
      }
    }
    auto batch = submissions.subspan(batchStart, batchEnd - batchStart);
    countDrawRunBatchDiscardedMaterializedStates(batch);
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchBindingOverrideCpuTime);
      prepareDrawRunBatchBindingOverrides(batch);
    }
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchBindingSnapshotCpuTime);
      snapshotDrawSubmissionBindingPayloads(pool, batch, scratch.bindingSnapshots);
    }
    std::size_t pendingPayloadBytes = 0;
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchPayloadBytesCpuTime);
      pendingPayloadBytes = drawRunSubmissionPayloadBytes(batch);
    }
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchSlotPrepareCpuTime);
      ensureWritingSlotUnlocked(queue, lock);
      maybeCommitDrawPayloadArenaUnlocked(queue, pool, lock, pendingPayloadBytes);
      DXMT_ASSERT(batch.front().stateMaterialized);
    }
    // SEGMENT-HOLD: from here through the append below, `lock` is
    // continuously held with no interior unlock/relock (the two helpers that
    // may unlock -- ensureWritingSlotUnlocked and
    // maybeCommitDrawPayloadArenaUnlocked -- already returned above, and
    // maybeCommitDrawChunkUnlocked below has not been called yet). This is
    // the per-batch resource-marking + append body the module comment above
    // calls out as the priority segment (22.6 calls/present).
    {
      QueueMutexSegmentScope qmxAppendSegment("submit_draw_run_batch_impl/append");
      {
        PerfScope stageScope(perf::countSubmitDrawRunBatchResourceMarkCpuTime);
        const std::uint64_t seqId = seqIdForMark(queue, 0);
        if (!skipDrawResourceMarking || forceDrawResourceMarkingAfterSplit) {
          pool.markDrawResources(batch.front().materializedState().hot, seqId);
        }
        for (auto& submission : batch) {
          std::span<const core::DrawParamPayloadView> payloads{};
          if (!submission.payload.userVertexData.empty() ||
              !submission.payload.userIndexData.empty() ||
              !submission.payload.bindingOverrideData.empty() ||
              !submission.payload.bindingSnapshotData.empty()) {
            payloads = std::span<const core::DrawParamPayloadView>(&submission.payload, 1);
          }
          markDrawBindingSnapshotResources(pool, payloads, seqId);
          if (!skipDrawResourceMarking || forceDrawResourceMarkingAfterSplit) {
            markDrawBindingOverrideResources(pool, payloads, seqId);
          }
        }
      }
      DXMT_ASSERT(batch.front().stateMaterialized);
      currentBackBuffer =
          batch.front().materializedState().hot.colorAttachments[0].handle;

      perf::countSubmitDrawRunBatchGroup(static_cast<std::uint32_t>(batch.size()));
      {
        PerfScope stageScope(perf::countSubmitDrawRunBatchAppendCpuTime);
        noteCurrentSlotCommandAppendStartedUnlocked(queue);
        currentSlotUnlocked(queue).appendDrawRunBatch(batch);
      }
    }
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchChunkCommitCpuTime);
      const bool committed = maybeCommitDrawChunkUnlocked(queue, pool, lock);
      forceDrawResourceMarkingAfterSplit =
          forceDrawResourceMarkingAfterSplit ||
          (committed && skipDrawResourceMarking);
    }
    batchStart = batchEnd;
  }
}

CommandQueue::CpuReadyArenaBuildLease::~CpuReadyArenaBuildLease() {
  abort();
}

CommandQueue::CpuReadyArenaBuildLease::CpuReadyArenaBuildLease(
    CpuReadyArenaBuildLease&& other) noexcept
    : queue_(other.queue_),
      ticket_(other.ticket_),
      controlIndex_(other.controlIndex_) {
  other.queue_ = nullptr;
  other.ticket_ = {};
  other.controlIndex_ = std::numeric_limits<std::size_t>::max();
}

CommandQueue::CpuReadyArenaBuildLease&
CommandQueue::CpuReadyArenaBuildLease::operator=(
    CpuReadyArenaBuildLease&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  abort();
  queue_ = other.queue_;
  ticket_ = other.ticket_;
  controlIndex_ = other.controlIndex_;
  other.queue_ = nullptr;
  other.ticket_ = {};
  other.controlIndex_ = std::numeric_limits<std::size_t>::max();
  return *this;
}

bool CommandQueue::CpuReadyArenaBuildLease::publish(
    std::span<const core::ChunkHandleEntry> resources,
    CpuReadyCaptureIdentity* captureIdentity) noexcept {
  if (!queue_) {
    return false;
  }
  auto* queue = queue_;
  const auto ticket = ticket_;
  if (!queue->publishCpuReadyArenaSource(
          ticket, controlIndex_, resources, captureIdentity)) {
    queue_ = nullptr;
    ticket_ = {};
    controlIndex_ = std::numeric_limits<std::size_t>::max();
    return false;
  }
  queue_ = nullptr;
  ticket_ = {};
  controlIndex_ = std::numeric_limits<std::size_t>::max();
  return true;
}

bool CommandQueue::CpuReadyArenaBuildLease::beginCaptureIdentity(
    std::uint32_t recordCount) noexcept {
  if (!queue_ || recordCount == 0u) {
    return false;
  }
  auto* context = queue_->activeArenaBuild_.load(std::memory_order_acquire);
  if (!context || context->reservation.ticket != ticket_ ||
      context->controlIndex != controlIndex_ ||
      context->ownerThread != std::this_thread::get_id() ||
      context->captureEnabled() ||
      context->failed.load(std::memory_order_acquire)) {
    return false;
  }
  try {
    context->captureCommandAnchors.reserve(recordCount);
    context->captureNextRawRecords.reserve(recordCount);
  } catch (...) {
    return false;
  }
  context->captureRecordCount = recordCount;
  return true;
}

bool CommandQueue::CpuReadyArenaBuildLease::captureNextCommandRecord(
    std::uint32_t recordIndex) noexcept {
  const std::array records{recordIndex};
  return captureNextDrawRecords(records);
}

bool CommandQueue::CpuReadyArenaBuildLease::captureNextDrawRecords(
    std::span<const std::uint32_t> recordIndices) noexcept {
  if (!queue_) {
    return false;
  }
  auto* context = queue_->activeArenaBuild_.load(std::memory_order_acquire);
  return context && context->reservation.ticket == ticket_ &&
         context->controlIndex == controlIndex_ &&
         context->ownerThread == std::this_thread::get_id() &&
         !context->failed.load(std::memory_order_acquire) &&
         context->setCaptureNextRawRecords(recordIndices);
}

bool CommandQueue::CpuReadyArenaBuildLease::selectSegment(
    std::size_t segmentIndex) noexcept {
  return queue_ && queue_->selectCpuReadyArenaSegment(
                       ticket_, controlIndex_, segmentIndex);
}

void CommandQueue::CpuReadyArenaBuildLease::abort() noexcept {
  if (!queue_) {
    return;
  }
  queue_->abortCpuReadyArenaSource(ticket_, controlIndex_);
  queue_ = nullptr;
  ticket_ = {};
  controlIndex_ = std::numeric_limits<std::size_t>::max();
}

CommandQueue::CpuReadyArenaBeginResult
CommandQueue::beginCpuReadyArenaSource(
    std::uint64_t rawOrdinal,
    const core::ArenaSourcePayloadLayout& layout) noexcept {
  if (rawOrdinal == 0 || !layout.valid()) {
    return {.status = CpuReadyArenaBeginStatus::Invalid};
  }
  for (std::size_t i = 0; i < layout.segmentCount; ++i) {
    if (layout.segments[i].layout.requiredBaseAlignment >
        core::kCpuReadyPageArenaAlignment) {
      return {.status = CpuReadyArenaBeginStatus::Invalid};
    }
  }
  if (layout.pageCount > cpuReadyTape_.config().values().maxPagesPerSource) {
    return {.status = CpuReadyArenaBeginStatus::Invalid};
  }
  if (arenaBuildPoisoned_.load(std::memory_order_acquire)) {
    return {.status = CpuReadyArenaBeginStatus::Corrupt};
  }

  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold: `lock` is later handed to queueLifecycle_.commitCurrentChunk()
  // (may unlock/relock), and this function also explicitly calls
  // lock.unlock() followed by more work before returning (see below), so a
  // single "hold" duration would not be meaningful.
  QueueMutexProbeScope qmxScope(
      qmxBegin, "begin_cpu_ready_arena_source", /*skipHold=*/true);
  if (stop_) {
    return {.status = CpuReadyArenaBeginStatus::Stopped};
  }
  if (arenaAdmissionActive_.load(std::memory_order_relaxed) ||
      arenaBuildContext_.has_value()) {
    return {.status = CpuReadyArenaBeginStatus::TemporaryPressure};
  }
  if (writingSlot_) {
    (void)queueLifecycle_.commitCurrentChunk(
        lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot,
                                perf::ChunkPublishReason::SemanticBoundary);
        });
    if (stop_) {
      return {.status = CpuReadyArenaBeginStatus::Stopped};
    }
    if (writingSlot_) {
      return {.status = CpuReadyArenaBeginStatus::TemporaryPressure};
    }
  }
  if (writeIndex_ >= slots_.size()) {
    return {.status = CpuReadyArenaBeginStatus::Corrupt};
  }
  if (slots_[writeIndex_].state != core::ChunkSlot::State::Free) {
    return {.status = CpuReadyArenaBeginStatus::TemporaryPressure};
  }
  const auto probe = cpuReadyTape_.probeArenaReserve(layout);
  switch (probe) {
  case core::CpuReadyTape::ReserveProbe::Ready:
    break;
  case core::CpuReadyTape::ReserveProbe::TemporaryPressure:
    return {.status = CpuReadyArenaBeginStatus::TemporaryPressure};
  case core::CpuReadyTape::ReserveProbe::Stopped:
    return {.status = CpuReadyArenaBeginStatus::Stopped};
  case core::CpuReadyTape::ReserveProbe::Corrupt:
    return {.status = CpuReadyArenaBeginStatus::Corrupt};
  case core::CpuReadyTape::ReserveProbe::InvalidRequest:
    return {.status = CpuReadyArenaBeginStatus::Invalid};
  }
  if (nextSeqId_ == 0 ||
      nextSeqId_ == std::numeric_limits<std::uint64_t>::max()) {
    return {.status = CpuReadyArenaBeginStatus::Corrupt};
  }

  const std::uint64_t seqId = nextSeqId_;
  const std::uint64_t buildGeneration = nextArenaBuildGeneration_++;
  const core::CpuReadyAdmissionIdentity identity{
      .rawOrdinal = rawOrdinal,
      .sourceOrdinal = seqId,
      .seqId = seqId,
      .buildGeneration = buildGeneration == 0 ? nextArenaBuildGeneration_++
                                             : buildGeneration,
  };
  auto reservation = cpuReadyTape_.reserve(layout, identity);
  if (!reservation) {
    return {.status = CpuReadyArenaBeginStatus::Corrupt};
  }
  recordCpuReadyTapeStats(cpuReadyTape_);
  schedulingProgressWatchdog_.noteAccepted(seqId, false);

  const std::size_t controlIndex = writeIndex_;
  auto& control = slots_[controlIndex];
  control.state = core::ChunkSlot::State::Writing;
  control.seqId = 0;
  control.sourceId = reservation->id;
  control.storage = reservation->storage;
  control.payload = nullptr;
  ++nextSeqId_;
  arenaAdmissionActive_.store(true, std::memory_order_release);
  arenaBuildContext_.emplace(
      *reservation, controlIndex, layout, cpuReadyTape_, currentBackBuffer_);
  auto* context = &*arenaBuildContext_;
  if (context->failed.load(std::memory_order_relaxed)) {
    context->failed.store(true, std::memory_order_release);
    lock.unlock();
    abortCpuReadyArenaSource(reservation->ticket, controlIndex);
    return {.status = CpuReadyArenaBeginStatus::Corrupt};
  }
  activeArenaBuild_.store(context, std::memory_order_release);
  lock.unlock();
  return {
      .status = CpuReadyArenaBeginStatus::Ready,
      .lease = CpuReadyArenaBuildLease(
          *this, reservation->ticket, controlIndex),
  };
}

bool CommandQueue::waitForCpuReadyArenaAdmission(
    const core::ArenaSourcePayloadLayout& layout) noexcept {
  arenaAdmissionWaiterCount_.fetch_add(1, std::memory_order_acq_rel);
  const auto waitStarted = std::chrono::steady_clock::now();
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold ("-cv"): `lock` is handed to writeCv_.wait(lock) below, which
  // releases and reacquires the mutex an unbounded number of times while
  // parked, so only the acquire-wait for this initial lock is recorded.
  QueueMutexProbeScope qmxScope(
      qmxBegin, "wait_for_cpu_ready_arena_admission-cv", /*skipHold=*/true);
  // Wake a parked Tape-gated encode session for deterministic re-evaluation.
  // This live pressure observation carries no release fence.
  encodeCv_.notify_one();
  while (true) {
    const bool controlSlotFree = writeIndex_ < slots_.size() &&
        slots_[writeIndex_].state == core::ChunkSlot::State::Free;
    bool reserveStillPressured = true;
    if (!arenaAdmissionActive_.load(std::memory_order_relaxed) &&
        !arenaBuildContext_.has_value() && controlSlotFree) {
      reserveStillPressured =
          cpuReadyTape_.probeArenaReserve(layout) ==
          core::CpuReadyTape::ReserveProbe::TemporaryPressure;
      recordCpuReadyTapeStats(cpuReadyTape_);
    }
    const auto action = render::classifyCpuReadyAdmissionGate({
        .stopped = stop_,
        .poisoned = arenaBuildPoisoned_.load(std::memory_order_acquire),
        .arenaBuildActive =
            arenaAdmissionActive_.load(std::memory_order_relaxed),
        .arenaBuildContextPresent = arenaBuildContext_.has_value(),
        .controlSlotFree = controlSlotFree,
        .reserveStillPressured = reserveStillPressured,
    });
    if (action != render::CpuReadyAdmissionAction::Wait) {
      break;
    }
    writeCv_.wait(lock);
  }
  const bool admitted = !stop_ &&
      !arenaBuildPoisoned_.load(std::memory_order_acquire);
  arenaAdmissionWaiterCount_.fetch_sub(1, std::memory_order_acq_rel);
  perf::countCpuReadyTapeAdmissionWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - waitStarted).count()));
  return admitted;
}

bool CommandQueue::selectCpuReadyArenaSegment(
    core::CpuReadyPublicationTicket ticket,
    std::size_t controlIndex,
    std::size_t segmentIndex) noexcept {
  auto* context = activeArenaBuild_.load(std::memory_order_acquire);
  if (!context || context->reservation.ticket != ticket ||
      context->controlIndex != controlIndex ||
      context->ownerThread != std::this_thread::get_id() ||
      context->failed.load(std::memory_order_acquire) ||
      context->publishing.load(std::memory_order_acquire) ||
      segmentIndex >= context->layout.segmentCount ||
      (segmentIndex != context->activeSegment &&
       segmentIndex != context->activeSegment + 1u) ||
      !context->builders[segmentIndex] ||
      !context->assemblers[segmentIndex]) {
    if (context) {
      context->failed.store(true, std::memory_order_release);
    }
    arenaBuildPoisoned_.store(true, std::memory_order_release);
    return false;
  }
  context->activeSegment = segmentIndex;
  return true;
}

bool CommandQueue::ArenaBuildContext::setCaptureNextRawRecords(
    std::span<const std::uint32_t> records) noexcept {
  if (!captureEnabled() || records.empty() ||
      !captureNextRawRecords.empty()) {
    return false;
  }
  std::uint32_t previous = 0u;
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (records[i] >= captureRecordCount ||
        (i != 0u && records[i] <= previous)) {
      return false;
    }
    previous = records[i];
  }
  try {
    captureNextRawRecords.assign(records.begin(), records.end());
  } catch (...) {
    return false;
  }
  return true;
}

bool CommandQueue::ArenaBuildContext::captureSingleCommand() noexcept {
  if (!captureEnabled()) {
    return true;
  }
  if (captureNextRawRecords.size() != 1u) {
    return false;
  }
  try {
    captureCommandAnchors.push_back(CaptureCommandAnchor{
        .firstRecord = captureNextRawRecords.front(),
        .lastRecord = captureNextRawRecords.front(),
    });
  } catch (...) {
    return false;
  }
  captureNextRawRecords.clear();
  return true;
}

bool CommandQueue::ArenaBuildContext::captureDrawCommand(
    std::size_t first, std::size_t count) noexcept {
  if (!captureEnabled()) {
    return true;
  }
  if (count == 0u || first > captureNextRawRecords.size() ||
      count > captureNextRawRecords.size() - first) {
    return false;
  }
  try {
    captureCommandAnchors.push_back(CaptureCommandAnchor{
        .firstRecord = captureNextRawRecords[first],
        .lastRecord = captureNextRawRecords[first + count - 1u],
    });
  } catch (...) {
    return false;
  }
  return true;
}

CommandQueue::ActiveArenaAppendResult CommandQueue::rejectIfActiveArena()
    noexcept {
  if (!arenaAdmissionActive_.load(std::memory_order_acquire)) {
    return ActiveArenaAppendResult::Inactive;
  }
  auto* context = activeArenaBuild_.load(std::memory_order_acquire);
  if (context) {
    context->failed.store(true, std::memory_order_release);
  }
  arenaBuildPoisoned_.store(true, std::memory_order_release);
  return ActiveArenaAppendResult::Failed;
}

void CommandQueue::rejectActiveCpuReadyArenaSource() noexcept {
  (void)rejectIfActiveArena();
}

CommandQueue::ActiveArenaAppendResult
CommandQueue::appendActiveArenaDrawRunBatch(
    std::span<core::DrawRunSubmission> submissions) noexcept {
  return appendActiveArena([&](ArenaBuildContext& context) {
    auto* assembler = context.activeAssembler();
    if (submissions.empty() || !submissions.front().stateMaterialized) {
      return false;
    }
    countDrawSubmissionAdjacentStateGenerations(submissions);

    std::size_t batchStart = 0;
    while (batchStart < submissions.size()) {
      std::size_t batchEnd = batchStart + 1u;
      while (batchEnd < submissions.size() &&
             drawSubmissionStatesCompatibleWithAcceptedPrevious(
                 submissions[batchStart], submissions[batchEnd - 1u],
                 submissions[batchEnd])) {
        ++batchEnd;
      }
      auto batch = submissions.subspan(batchStart, batchEnd - batchStart);
      countDrawRunBatchDiscardedMaterializedStates(batch);
      prepareDrawRunBatchBindingOverrides(batch);
      // Direct replay payloads already carry the app-admission concrete
      // backing snapshot. Never recapture from Pool on the replay thread:
      // doing so would substitute a later rename generation. Publish validates
      // every versioned binding through Pool's own synchronization before
      // marking it, outside the queue scheduling mutex.
      if (!assembler || !assembler->tryAppendDrawRunBatch(batch)) {
        return false;
      }
      if (!context.captureDrawCommand(batchStart, batch.size())) {
        return false;
      }
      context.pendingBackBuffer = batch.front()
                                      .materializedState()
                                      .hot.colorAttachments[0].handle;
      context.updatesBackBuffer = true;
      perf::countSubmitDrawRunBatchGroup(
          static_cast<std::uint32_t>(batch.size()));
      batchStart = batchEnd;
    }
    if (context.captureEnabled()) {
      context.captureNextRawRecords.clear();
    }
    return true;
  });
}

CommandQueue::ActiveArenaAppendResult
CommandQueue::appendActiveArenaDrawRun(
    core::CanonicalDrawState& state,
    const core::DrawUniformPayload& uniforms,
    std::span<const core::DrawParam> draws,
    std::span<const core::DrawParamPayloadView> payloads) noexcept {
  return appendActiveArena([&](ArenaBuildContext& context) {
    auto* assembler = context.activeAssembler();
    if (draws.empty()) {
      return true;
    }
    auto& scratch = drawSubmitScratch();
    ScopedDrawSubmitScratchUse scratchUse(scratch);
    scratch.arenaSubmissions.clear();
    scratch.arenaSubmissions.reserve(draws.size());
    const auto backBuffer = state.hot.colorAttachments[0].handle;
    for (std::size_t i = 0; i < draws.size(); ++i) {
      auto& submission = scratch.arenaSubmissions.emplace_back();
      submission.draw = draws[i];
      submission.payload = drawPayloadAt(payloads, i);
      submission.stateGeneration = 1;
      submission.uniformGeneration = 1;
      submission.stateLane = core::DrawRunSubmissionStateLane::BindingAgnostic;
      if (i == 0) {
        submission.state.emplace(std::move(state));
        submission.uniforms.emplace(uniforms);
        submission.stateMaterialized = true;
      } else {
        submission.stateMaterialized = false;
      }
    }
    if (!assembler || !assembler->tryAppendDrawRunBatch(
            scratch.arenaSubmissions)) {
      return false;
    }
    if (!context.captureSingleCommand()) {
      return false;
    }
    context.pendingBackBuffer = backBuffer;
    context.updatesBackBuffer = true;
    return true;
  });
}

CommandQueue::ActiveArenaAppendResult CommandQueue::appendActiveArenaClear(
    const core::ClearDesc& value) noexcept {
  return appendActiveArena([&](ArenaBuildContext& context) {
    auto* assembler = context.activeAssembler();
    if (!assembler || !assembler->tryAppendClear(value)) {
      return false;
    }
    if (!context.captureSingleCommand()) {
      return false;
    }
    if (value.colorAttachments[0].handle) {
      context.pendingBackBuffer = value.colorAttachments[0].handle;
      context.updatesBackBuffer = true;
    }
    return true;
  });
}

CommandQueue::ActiveArenaAppendResult
CommandQueue::appendActiveArenaSurfaceCopy(
    const core::SurfaceCopyDesc& value) noexcept {
  return appendActiveArena([&](ArenaBuildContext& context) {
    auto* assembler = context.activeAssembler();
    if (!assembler || !assembler->tryAppendSurfaceCopy(value)) {
      return false;
    }
    if (!context.captureSingleCommand()) {
      return false;
    }
    context.pendingBackBuffer = value.destination;
    context.updatesBackBuffer = true;
    return true;
  });
}

CommandQueue::ActiveArenaAppendResult
CommandQueue::appendActiveArenaStretchRect(
    const core::StretchRectDesc& value) noexcept {
  return appendActiveArena([&](ArenaBuildContext& context) {
    auto* assembler = context.activeAssembler();
    if (!assembler || !assembler->tryAppendStretchRect(value)) {
      return false;
    }
    if (!context.captureSingleCommand()) {
      return false;
    }
    context.pendingBackBuffer = value.destination;
    context.updatesBackBuffer = true;
    return true;
  });
}

CommandQueue::ActiveArenaAppendResult CommandQueue::appendActiveArenaColorFill(
    const core::ColorFillDesc& value) noexcept {
  return appendActiveArena([&](ArenaBuildContext& context) {
    auto* assembler = context.activeAssembler();
    if (!assembler || !assembler->tryAppendColorFill(value)) {
      return false;
    }
    if (!context.captureSingleCommand()) {
      return false;
    }
    context.pendingBackBuffer = value.destination;
    context.updatesBackBuffer = true;
    return true;
  });
}

CommandQueue::ActiveArenaAppendResult
CommandQueue::appendActiveArenaDepthResolve(
    const core::DepthResolveDesc& value) noexcept {
  return appendActiveArena([&](ArenaBuildContext& context) {
    auto* assembler = context.activeAssembler();
    return assembler && assembler->tryAppendDepthResolve(value) &&
           context.captureSingleCommand();
  });
}

CommandQueue::ActiveArenaAppendResult CommandQueue::appendActiveArenaPresent(
    core::SwapDesc value, BoundaryPolicy boundaryPolicy,
    bool tokenStashed) noexcept {
  return appendActiveArena([&](ArenaBuildContext& context) {
    auto* builder = context.activeBuilder();
    if (!builder || context.presentAppended) {
      return false;
    }
    const core::Handle fallback = context.updatesBackBuffer
        ? context.pendingBackBuffer
        : context.initialBackBuffer;
    const core::Handle sourceHandle =
        core::metalqueue::selectPresentSourceHandle(value, fallback);
    perf::countPresentSourceSelection(
        static_cast<bool>(value.sourceSurface),
        sourceHandle.value != 0 && sourceHandle == fallback);

    // Record cleanup responsibility before the append: if capacity or command
    // validation fails, lease abort remains the single token-removal path.
    context.pendingPresentId = value.presentId;
    context.pendingPresentDesc = value;
    context.pendingPresentBoundaryPolicy = boundaryPolicy;
    context.presentAppended = true;
    context.presentTokenStashed = tokenStashed;
    return builder->tryAppendPresentCommand(core::PresentCommandRecord{
        .present = std::move(value),
        .presentSource = sourceHandle,
    }) && context.captureSingleCommand();
  });
}

CommandQueue::ActiveArenaAppendResult
CommandQueue::deferActiveArenaFlush() noexcept {
  return appendActiveArena([](ArenaBuildContext& context) {
    if (!context.presentAppended) {
      return false;
    }
    context.flushAfterPublication = true;
    return true;
  });
}

bool CommandQueue::publishCpuReadyArenaSource(
    core::CpuReadyPublicationTicket ticket,
    std::size_t controlIndex,
    std::span<const core::ChunkHandleEntry> resources,
    CpuReadyCaptureIdentity* captureIdentity) noexcept {
  auto* context = activeArenaBuild_.load(std::memory_order_acquire);
  if (!context || context->reservation.ticket != ticket ||
      context->ownerThread != std::this_thread::get_id() ||
      context->failed.load(std::memory_order_acquire) ||
      context->controlIndex != controlIndex ||
      context->activeSegment + 1u != context->layout.segmentCount) {
    arenaBuildPoisoned_.store(true, std::memory_order_release);
    abortCpuReadyArenaSource(ticket, controlIndex);
    return false;
  }
  if (captureIdentity) {
    *captureIdentity = {};
  }
  for (std::size_t i = 0; i < context->layout.segmentCount; ++i) {
    if (!context->builders[i] || !context->builders[i]->publish()) {
      arenaBuildPoisoned_.store(true, std::memory_order_release);
      abortCpuReadyArenaSource(ticket, controlIndex);
      return false;
    }
  }

  // Capture attribution is built from the exact immutable blocks owned by
  // this ticket after their builders publish, but before Ready visibility lets
  // the encode thread reorder or reclaim them. Failure invalidates only the
  // diagnostic sidecar; it never changes rendering or publication.
  CpuReadyCaptureIdentity captured{};
  bool capturedValid = captureIdentity == nullptr;
  if (captureIdentity && context->captureEnabled() &&
      context->captureNextRawRecords.empty()) {
    try {
      std::array<const core::ArenaSourcePayloadBlock*,
                 core::kMaxArenaSourcePayloadSegments> blocks{};
      for (std::size_t i = 0; i < context->layout.segmentCount; ++i) {
        blocks[i] = context->reservation.arenaPayloads[i];
      }
      core::ArenaSourcePayloadChain chain;
      capturedValid = chain.initialize(
          std::span(blocks).first(context->layout.segmentCount));
      const core::SourcePayloadView payload(chain);
      capturedValid = capturedValid && payload.valid() &&
          payload.commandCount() == context->captureCommandAnchors.size();
      framegraph::FrameGraph graph;
      if (capturedValid) {
        framegraph::buildFrameGraph(payload, ticket.sourceOrdinal, graph);
      }
      std::vector<std::uint32_t> commandPass;
      if (capturedValid) {
        commandPass.assign(payload.commandCount(),
                           std::numeric_limits<std::uint32_t>::max());
        for (std::size_t passIndex = 0; passIndex < graph.passes.size();
             ++passIndex) {
          const auto& pass = graph.passes[passIndex];
          if (pass.commands.first > graph.commands.size() ||
              pass.commands.count >
                  graph.commands.size() - pass.commands.first) {
            capturedValid = false;
            break;
          }
          for (std::size_t local = 0; local < pass.commands.count; ++local) {
            const auto commandIndex =
                graph.commands[pass.commands.first + local].command_index;
            if (commandIndex >= commandPass.size() ||
                commandPass[commandIndex] !=
                    std::numeric_limits<std::uint32_t>::max()) {
              capturedValid = false;
              break;
            }
            commandPass[commandIndex] = static_cast<std::uint32_t>(passIndex);
          }
          if (!capturedValid) {
            break;
          }
        }
        capturedValid = capturedValid && std::none_of(
            commandPass.begin(), commandPass.end(), [](std::uint32_t value) {
              return value == std::numeric_limits<std::uint32_t>::max();
            });
      }
      if (capturedValid) {
        captured.sourceOrdinal = ticket.sourceOrdinal;
        captured.seqId = ticket.seqId;
        captured.recordCount = context->captureRecordCount;
        std::uint32_t nextRecord = 0u;
        for (std::size_t commandIndex = 0;
             commandIndex < context->captureCommandAnchors.size();
             ++commandIndex) {
          const auto& anchor = context->captureCommandAnchors[commandIndex];
          const auto passIndex = commandPass[commandIndex];
          const bool last =
              commandIndex + 1u == context->captureCommandAnchors.size();
          const std::uint32_t endRecord = last
              ? context->captureRecordCount
              : anchor.lastRecord + 1u;
          if (anchor.firstRecord < nextRecord ||
              anchor.lastRecord < anchor.firstRecord ||
              endRecord <= nextRecord ||
              endRecord > context->captureRecordCount ||
              passIndex >= graph.passes.size()) {
            capturedValid = false;
            break;
          }
          const std::uint32_t passKind =
              static_cast<std::uint32_t>(graph.passes[passIndex].kind) + 1u;
          if (!captured.ranges.empty() &&
              captured.ranges.back().dagPassIndex == passIndex &&
              captured.ranges.back().passKind == passKind &&
              captured.ranges.back().firstRecord +
                      captured.ranges.back().recordCount ==
                  nextRecord) {
            captured.ranges.back().recordCount += endRecord - nextRecord;
          } else {
            captured.ranges.push_back(CpuReadyCapturePassRange{
                .firstRecord = nextRecord,
                .recordCount = endRecord - nextRecord,
                .dagPassIndex = passIndex,
                .passKind = passKind,
            });
          }
          nextRecord = endRecord;
        }
        capturedValid = capturedValid && nextRecord == captured.recordCount &&
                        captured.valid();
      }
    } catch (...) {
      capturedValid = false;
    }
  }

  // Phase 1 fixes the exact transaction and immutable published payload while
  // holding the scheduling mutex. `publishing` keeps every producer path out
  // until phase 3 seals or fail-stops this same capability.
  {
    const auto qmxBegin = queueMutexProbeBegin();
    std::unique_lock lock(mutex_);
    // skipHold: two explicit lock.unlock() calls below are each followed by
    // more work (abortCpuReadyArenaSource()) before returning, so a single
    // "hold" duration measured from acquisition to this scope's exit would
    // overcount past the real unlock on those paths.
    QueueMutexProbeScope qmxScope(
        qmxBegin, "publish_cpu_ready_arena_source_phase1", /*skipHold=*/true);
    if (controlIndex >= slots_.size() || !arenaBuildContext_ ||
        &*arenaBuildContext_ != context ||
        activeArenaBuild_.load(std::memory_order_relaxed) != context ||
        context->reservation.ticket != ticket ||
        context->controlIndex != controlIndex ||
        context->failed.load(std::memory_order_acquire)) {
      lock.unlock();
      abortCpuReadyArenaSource(ticket, controlIndex);
      return false;
    }
    const auto& control = slots_[controlIndex];
    if (control.state != core::ChunkSlot::State::Writing ||
        control.sourceId != ticket.id || control.storage != ticket.storage) {
      lock.unlock();
      abortCpuReadyArenaSource(ticket, controlIndex);
      return false;
    }
    context->publishing.store(true, std::memory_order_release);
  }

  // Resource retention is deliberately outside the scheduling lock (§5.4).
  // The arena owner and view remain pinned by the active strict transaction.
  for (std::size_t i = 0; i < context->layout.segmentCount; ++i) {
    const core::SourcePayloadView payloadView(
        *context->reservation.arenaPayloads[i]);
    if (!payloadView.valid() ||
        !validateArenaDrawAdmissionSnapshots(pool_, payloadView)) {
      arenaBuildPoisoned_.store(true, std::memory_order_release);
      abortCpuReadyArenaSource(ticket, controlIndex);
      return false;
    }
  }
  markChunkResourcesWithExactSeq(pool_, resources, ticket.seqId);
  for (std::size_t i = 0; i < context->layout.segmentCount; ++i) {
    const core::SourcePayloadView payloadView(
        *context->reservation.arenaPayloads[i]);
    markArenaSourceResources(pool_, payloadView, ticket.seqId);
  }

  // Phase 3 revalidates the same ticket/context/control after marking. No
  // writer can advance either ring while arenaAdmissionActive_ remains set.
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold: explicit lock.unlock() calls below (both the early-fail paths
  // and the success path) are each followed by more work before returning,
  // so a single "hold" duration would not be meaningful.
  QueueMutexProbeScope qmxScope(
      qmxBegin, "publish_cpu_ready_arena_source_phase3", /*skipHold=*/true);
  if (controlIndex >= slots_.size() || !arenaBuildContext_ ||
      &*arenaBuildContext_ != context ||
      activeArenaBuild_.load(std::memory_order_relaxed) != context ||
      context->reservation.ticket != ticket ||
      context->controlIndex != controlIndex ||
      !context->publishing.load(std::memory_order_acquire) ||
      context->failed.load(std::memory_order_acquire)) {
    lock.unlock();
    arenaBuildPoisoned_.store(true, std::memory_order_release);
    abortCpuReadyArenaSource(ticket, controlIndex);
    return false;
  }
  auto& control = slots_[controlIndex];
  if (control.state != core::ChunkSlot::State::Writing ||
      control.sourceId != ticket.id || control.storage != ticket.storage ||
      !cpuReadyTape_.sealAndPublish(ticket, controlIndex,
                                   context->layout.usedBytes)) {
    lock.unlock();
    arenaBuildPoisoned_.store(true, std::memory_order_release);
    abortCpuReadyArenaSource(ticket, controlIndex);
    return false;
  }
  control.seqId = ticket.seqId;
  control.state = core::ChunkSlot::State::Pending;
  lastCommittedSeqId_ = ticket.seqId;
  ++inflightCount_;
  writeIndex_ = (controlIndex + 1) % slots_.size();
  if (context->updatesBackBuffer) {
    currentBackBuffer_ = context->pendingBackBuffer;
  }
  const bool hasPublishedPresent = context->presentAppended;
  const auto publishedPresentDesc = context->pendingPresentDesc;
  const auto publishedPresentBoundaryPolicy =
      context->pendingPresentBoundaryPolicy;
  const bool flushAfterPublication = context->flushAfterPublication;
  // Ready publication transfers any stashed drawable token's cleanup
  // obligation from the build context to normal encoded-Present consumption.
  context->presentTokenStashed = false;
  activeArenaBuild_.store(nullptr, std::memory_order_release);
  arenaBuildContext_.reset();
  arenaAdmissionActive_.store(false, std::memory_order_release);
  recordCpuReadyTapeStats(cpuReadyTape_);
  queueLifecycle_.noteCpuReadyCapacityProgress();
  schedulingProgressWatchdog_.notePublished(ticket.seqId,
                                             hasPublishedPresent);
  lock.unlock();
  writeCv_.notify_all();
  if (hasPublishedPresent) {
    if (publishedPresentBoundaryPolicy ==
        BoundaryPolicy::DeferredPresentCompletion) {
      PerfScope stageScope(perf::countSubmitPresentBoundaryCpuTime);
      drainDeferredPresentBoundary();
    }
    applyPublishedPresentBoundary(ticket.seqId, publishedPresentDesc,
                                  publishedPresentBoundaryPolicy);
  }
  if (flushAfterPublication) {
    submitFlush();
  }
  if (captureIdentity && capturedValid) {
    *captureIdentity = std::move(captured);
  }
  return true;
}

void CommandQueue::abortCpuReadyArenaSource(
    core::CpuReadyPublicationTicket ticket,
    std::size_t controlIndex) noexcept {
  core::PresentId stashedPresentId{};
  bool removeStashedPresentToken = false;
  auto owner = [&]() {
    const auto qmxBegin = queueMutexProbeBegin();
    std::unique_lock lock(mutex_);
    QueueMutexProbeScope qmxScope(qmxBegin, "abort_cpu_ready_arena_source_detach");
    auto detached = controlIndex < slots_.size()
        ? cpuReadyTape_.beginArenaAbort(ticket)
        : std::optional<core::CpuReadyTape::DetachedArenaOwner>{};
    if (controlIndex < slots_.size()) {
      if (arenaBuildContext_ &&
          arenaBuildContext_->reservation.ticket == ticket &&
          arenaBuildContext_->presentTokenStashed) {
        stashedPresentId = arenaBuildContext_->pendingPresentId;
        removeStashedPresentToken = true;
        arenaBuildContext_->presentTokenStashed = false;
      }
      activeArenaBuild_.store(nullptr, std::memory_order_release);
      arenaBuildContext_.reset();
    }
    stop_ = true;
    cpuReadyTape_.stopAdmission();
    arenaBuildPoisoned_.store(true, std::memory_order_release);
    return detached;
  }();
  if (owner) {
    owner->destroy();
  }
  if (removeStashedPresentToken) {
    (void)takeDrawableToken(stashedPresentId);
  }
  {
    const auto qmxBegin = queueMutexProbeBegin();
    std::lock_guard lock(mutex_);
    QueueMutexProbeScope qmxScope(qmxBegin, "abort_cpu_ready_arena_source_finish");
    if (owner && cpuReadyTape_.finishArenaAbort(ticket, std::move(*owner)) &&
        controlIndex < slots_.size()) {
      slots_[controlIndex] = {};
      queueLifecycle_.noteCpuReadyCapacityProgress();
    }
    arenaAdmissionActive_.store(false, std::memory_order_release);
    recordCpuReadyTapeStats(cpuReadyTape_);
  }
  notifySchedulingTerminalWaiters(
      render::SchedulingTerminalDisposition::DeviceLoss);
}

void CommandQueue::submitDrawRunBatch(
    std::span<core::DrawRunSubmission> submissions) {
  for (std::size_t i = 0; i < submissions.size(); ++i) {
    perf::countSubmitDraw();
  }
  PerfScope scope(perf::countSubmitDrawCpuTime);
  if (appendActiveArenaDrawRunBatch(submissions) !=
      ActiveArenaAppendResult::Inactive) {
    return;
  }
  submitDrawRunBatchImpl(*this, pool_, mutex_, currentBackBuffer_,
                         skipDrawResourceMarking_,
                         forceDrawResourceMarkingAfterSplit_,
                         submissions);
}

void CommandQueue::submitClear(const core::ClearDesc& desc) {
  perf::countSubmitClear();
  if (appendActiveArenaClear(desc) != ActiveArenaAppendResult::Inactive) {
    return;
  }
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold: `lock` is handed to ensureWritingSlotUnlocked below, which may
  // unlock/relock it via QueueLifecycleController (a different file).
  QueueMutexProbeScope qmxScope(qmxBegin, "submit_clear", /*skipHold=*/true);
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureClear(pool_, desc);
  for (const auto& attachment : desc.colorAttachments) {
    traceTextureSurfaceOp(pool_, "Clear", attachment.handle, desc.depthStencil.handle);
  }
  noteCurrentSlotCommandAppendStartedUnlocked(*this);
  currentSlotUnlocked(*this).appendClear(desc);
  if (desc.colorAttachments[0].handle) {
    currentBackBuffer_ = desc.colorAttachments[0].handle;
  }
  pool_.markClearResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitSurfaceCopy(const core::SurfaceCopyDesc& desc) {
  if (appendActiveArenaSurfaceCopy(desc) !=
      ActiveArenaAppendResult::Inactive) {
    return;
  }
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold: `lock` is handed to ensureWritingSlotUnlocked below, which may
  // unlock/relock it via QueueLifecycleController (a different file).
  QueueMutexProbeScope qmxScope(qmxBegin, "submit_surface_copy", /*skipHold=*/true);
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureSurfaceOp(pool_, "SurfaceCopy", desc.source, desc.destination);
  noteCurrentSlotCommandAppendStartedUnlocked(*this);
  currentSlotUnlocked(*this).appendSurfaceCopy(desc);
  currentBackBuffer_ = desc.destination;
  pool_.markSurfaceCopyResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitStretchRect(const core::StretchRectDesc& desc) {
  perf::countSubmitStretch();
  if (appendActiveArenaStretchRect(desc) !=
      ActiveArenaAppendResult::Inactive) {
    return;
  }
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold: `lock` is handed to queueLifecycle_.commitCurrentChunk() and
  // ensureWritingSlotUnlocked below, both of which may unlock/relock it via
  // QueueLifecycleController (a different file).
  QueueMutexProbeScope qmxScope(qmxBegin, "submit_stretch_rect", /*skipHold=*/true);
  if (splitStretchChunk()) {
    queueLifecycle_.commitCurrentChunk(
        lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot,
                                perf::ChunkPublishReason::StretchSplit);
        });
  }
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureSurfaceOp(pool_, "StretchRect", desc.source, desc.destination);
  noteCurrentSlotCommandAppendStartedUnlocked(*this);
  currentSlotUnlocked(*this).appendStretchRect(desc);
  currentBackBuffer_ = desc.destination;
  pool_.markStretchResources(desc, seqIdForMark(*this, 0));
  if (splitStretchChunk()) {
    queueLifecycle_.commitCurrentChunk(
        lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot,
                                perf::ChunkPublishReason::StretchSplit);
        });
  }
}

void CommandQueue::submitReadback(const core::ReadbackDesc& desc) {
  if (rejectIfActiveArena() != ActiveArenaAppendResult::Inactive) {
    return;
  }
  const auto qmxBegin = queueMutexProbeBegin();
  std::lock_guard lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "submit_readback");
  // Readback is satisfied synchronously in CommandQueue::readbackSurface.
  // Still mark resources so NoUseAfterFree remains meaningful.
  traceTextureSurfaceOp(pool_, "Readback", desc.source, desc.destination);
  pool_.markReadbackResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitColorFill(const core::ColorFillDesc& desc) {
  if (appendActiveArenaColorFill(desc) !=
      ActiveArenaAppendResult::Inactive) {
    return;
  }
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold: `lock` is handed to ensureWritingSlotUnlocked below, which may
  // unlock/relock it via QueueLifecycleController (a different file).
  QueueMutexProbeScope qmxScope(qmxBegin, "submit_color_fill", /*skipHold=*/true);
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureSurfaceOp(pool_, "ColorFill", desc.destination);
  noteCurrentSlotCommandAppendStartedUnlocked(*this);
  currentSlotUnlocked(*this).appendColorFill(desc);
  currentBackBuffer_ = desc.destination;
  pool_.markColorFillResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitDepthResolve(const core::DepthResolveDesc& desc) {
  if (appendActiveArenaDepthResolve(desc) !=
      ActiveArenaAppendResult::Inactive) {
    return;
  }
  // R-FORMAT-11 — RESZ MSAA depth resolve. Fire-and-forget surface op:
  // append the command + mark both endpoints, mirroring submitColorFill.
  // The destination is the INTZ depth texture, not the present back buffer,
  // so currentBackBuffer_ is left untouched.
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold: `lock` is handed to ensureWritingSlotUnlocked below, which may
  // unlock/relock it via QueueLifecycleController (a different file).
  QueueMutexProbeScope qmxScope(qmxBegin, "submit_depth_resolve", /*skipHold=*/true);
  ensureWritingSlotUnlocked(*this, lock);
  traceTextureSurfaceOp(pool_, "DepthResolve", desc.msaaDepth, desc.intzDest);
  noteCurrentSlotCommandAppendStartedUnlocked(*this);
  currentSlotUnlocked(*this).appendDepthResolve(desc);
  pool_.markDepthResolveResources(desc, seqIdForMark(*this, 0));
}

std::uint64_t CommandQueue::submitPresent(const core::SwapDesc& desc) {
  // Cumulative per-site DXMT9_PERF_QUEUE_MUTEX_SPLIT emission, once every 60
  // presents. Periodic rather than at teardown, matching the drain-fence-site
  // sink (DXMT9_PERF_DRAIN_FENCE_SITES): 3DMark05 never releases the device,
  // so a destructor-emitted report would never fire. Gated on
  // queueMutexSplitEnabled() so the counter/branch cost is paid only when the
  // split is actually enabled.
  if (queueMutexSplitEnabled()) {
    static std::atomic<std::uint64_t> queueMutexSitePresentTally{0};
    const std::uint64_t presentOrdinal =
        queueMutexSitePresentTally.fetch_add(1, std::memory_order_relaxed) + 1;
    if (presentOrdinal % 60 == 0) {
      logQueueMutexSites(presentOrdinal);
    }
  }
  const bool arenaPresentActive =
      arenaAdmissionActive_.load(std::memory_order_acquire);
  std::uint64_t arenaPresentSeqId = 0;
  if (arenaPresentActive) {
    auto* context = activeArenaBuild_.load(std::memory_order_acquire);
    if (!context || context->ownerThread != std::this_thread::get_id() ||
        context->failed.load(std::memory_order_acquire) ||
        context->publishing.load(std::memory_order_acquire)) {
      // Validate the strict transaction before acquiring or stashing a
      // drawable token. A failed active build is the sole owner of abort-side
      // token cleanup, so no Presenter side effect may precede this check.
      (void)rejectIfActiveArena();
      return 0;
    }
    arenaPresentSeqId = context->reservation.ticket.seqId;
  }
  // TLA+: PresentFrameLatency / CommitPresent.
  perf::countSubmitPresent();
  PerfScope scope(perf::countSubmitPresentCpuTime);
  // R-BACK-3.10 — milestone-save decision only runs at present
  // boundaries (never per-draw). Cheap in the common case: a handful of
  // integer compares under a mutex; the actual serialize only fires at
  // most twice per process, off this thread (Archive::triggerSave spawns
  // a background thread).
  shaderArchive_.notePresent();
  const BoundaryPolicy boundaryPolicy = resolveBoundaryPolicyFromEnv();
  // R-BACK-2.51(g) — drainDeferredPresentBoundary() is a no-op when nothing
  // is deferred (deferredPresentBoundaryTargetSeqId_ == 0), so it is safe to
  // evaluate unconditionally here. It must NOT be gated on the global
  // offloadCommitReplayEnabled() flag: an earlier *unpaced* present in this
  // process (a direct COM caller, or one replayed by the synchronous
  // non-offload chunk path) may have deferred a target even while offload
  // is globally enabled for other (paced) presents.
  if (!arenaPresentActive &&
      boundaryPolicy == BoundaryPolicy::DeferredPresentCompletion) {
    PerfScope stageScope(perf::countSubmitPresentBoundaryCpuTime);
    drainDeferredPresentBoundary();
  }
  core::SwapDesc queuedDesc = desc;
  bool presentTokenStashed = false;
  // Resolve the queue-local Presenter binding once. Stale ids (swapchain
  // destroyed between snapshotSwapDesc and submitPresent) resolve to
  // nullptr; the legacy raw-pointer code already tolerated that path so
  // the rest of this function preserves the same control flow.
  Presenter* presenter = lookupPresenter(queuedDesc.presentId);
  if (presenter) {
    PerfScope stageScope(perf::countSubmitPresentAcquireCpuTime);
    switch (presenter->acquirePolicy()) {
      case AcquirePolicy::Async: {
        perf::countPresentAsyncAcquireRequest();
        auto token = presenter->beginAcquireDrawable(makePresentAcquireParams(queuedDesc));
        if (token) {
          perf::countPresentAsyncAcquireIssued();
          queuedDesc.drawableTokenRequired = true;
        } else {
          perf::countPresentAsyncAcquireFallback();
          queuedDesc.drawableTokenRequired = false;
        }
        presentTokenStashed = static_cast<bool>(token);
        stashDrawableToken(queuedDesc.presentId, std::move(token));
        break;
      }
      case AcquirePolicy::SyncOnSubmit: {
        if (!arenaPresentActive) {
          const auto qmxBegin = queueMutexProbeBegin();
          std::unique_lock lock(mutex_);
          // skipHold: `lock` is handed to queueLifecycle_.commitCurrentChunk()
          // below, which may unlock/relock it via QueueLifecycleController
          // (a different file).
          QueueMutexProbeScope qmxScope(
              qmxBegin, "submit_present_sync_on_submit", /*skipHold=*/true);
          queueLifecycle_.commitCurrentChunk(
              lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
                prepareSlotForPublish(*this, pool_, slot,
                                      perf::ChunkPublishReason::PresentAcquire);
              });
        }
        auto token = presenter->acquireDrawable(makePresentAcquireParams(queuedDesc));
        queuedDesc.drawableTokenRequired = true;
        presentTokenStashed = static_cast<bool>(token);
        stashDrawableToken(queuedDesc.presentId, std::move(token));
        break;
      }
      case AcquirePolicy::Sync:
      case AcquirePolicy::PreAcquire:
        // Sync acquires inline in encodeCommands; PreAcquire feeds the
        // prefetched-drawable cache from the encode thread.
        break;
    }
  }

  if (arenaPresentActive) {
    return appendActiveArenaPresent(std::move(queuedDesc), boundaryPolicy,
                                    presentTokenStashed) ==
                   ActiveArenaAppendResult::Appended
        ? arenaPresentSeqId
        : 0;
  }

  std::uint64_t presentSeqId = 0;
  {
    PerfScope stageScope(perf::countSubmitPresentCommitCpuTime);
    const auto qmxBegin = queueMutexProbeBegin();
    std::unique_lock lock(mutex_);
    // skipHold: `lock` is handed to queueLifecycle_.presentAndCommit() below,
    // which may unlock/relock it via QueueLifecycleController (a different
    // file).
    QueueMutexProbeScope qmxScope(
        qmxBegin, "submit_present_commit", /*skipHold=*/true);
    const core::Handle sourceHandle =
        core::metalqueue::selectPresentSourceHandle(queuedDesc, currentBackBuffer_);
    perf::countPresentSourceSelection(static_cast<bool>(queuedDesc.sourceSurface),
                                      sourceHandle.value != 0 &&
                                          sourceHandle.value == currentBackBuffer_.value);
    queueLifecycle_.presentAndCommit(
        lock, kMaxQueuedChunks, queuedDesc, sourceHandle,
        [this](core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot,
                                perf::ChunkPublishReason::Present);
        });
    presentSeqId = lastCommittedSeqId_;
  }

  // R-BACK-2.51(g) — per-present decision, not a global offload-enabled
  // gate: only the specific present whose SwapDesc was marked
  // pacedByPresentOrdinal (set exclusively by the D3D9 chunk-replay path,
  // and only when dxmt9::d3d9::offloadCommitReplayEnabled() was on) skips
  // the inline boundary below. Any other present — a direct COM caller, or
  // one replayed by the synchronous non-offload chunk path — keeps
  // participating in the inline boundary even while
  // DXMT9_OFFLOAD_COMMIT_REPLAY is globally enabled for other presents in
  // this process.
  applyPublishedPresentBoundary(presentSeqId, queuedDesc, boundaryPolicy);
  return presentSeqId;
}

void CommandQueue::applyPublishedPresentBoundary(
    std::uint64_t presentSeqId, const core::SwapDesc& desc,
    BoundaryPolicy boundaryPolicy) {
  DXMT_ASSERT(!desc.pacedByPresentOrdinal || offloadCommitReplayEnabled());
  switch (resolvePresentBoundaryAction(desc.pacedByPresentOrdinal,
                                       boundaryPolicy)) {
    case PresentBoundaryAction::SkipPacedByOffloadOrdinal:
      // dxmt9::Device::waitPresentOrdinalBoundary already paced this
      // present before submitPresent was called; applying the inline
      // seqId-based boundary here too would double-wait on the same
      // present token.
      perf::countPresentBoundarySkipped();
      break;
    case PresentBoundaryAction::Defer:
      perf::countPresentBoundaryApplied();
      deferPresentBoundary(presentSeqId, presentBoundaryLatency(desc));
      break;
    case PresentBoundaryAction::ApplyInline: {
      PerfScope stageScope(perf::countSubmitPresentBoundaryCpuTime);
      perf::countPresentBoundaryApplied();
      presentBoundary(presentSeqId, presentBoundaryLatency(desc));
      break;
    }
    case PresentBoundaryAction::SkipDisabled:
      perf::countPresentBoundarySkipped();
      break;
  }
}

void CommandQueue::presentBoundary(std::uint64_t presentSeqId, std::uint32_t maxFrameLatency) {
  // TLA+: PresentFrameLatency / BeginPresentWait + CommitPendingPresent.
  const std::uint64_t targetSeqId =
      presentBoundaryTargetSeqId(presentSeqId, maxFrameLatency);
  if (targetSeqId == 0) {
    return;
  }
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold ("-cv"): `lock` is handed to one of the cv.wait(lock, ...) calls
  // below, which release and reacquire the mutex an unbounded number of
  // times while parked, so only the acquire-wait for this initial lock is
  // recorded.
  QueueMutexProbeScope qmxScope(qmxBegin, "present_boundary-cv", /*skipHold=*/true);
  // R-BACK / PresentFrameLatency — branch on the unified
  // BoundaryPolicy resolved once at process init. Disabled is
  // filtered earlier by resolvePresentBoundaryAction (only
  // ApplyInline/Defer reach presentBoundary/deferPresentBoundary) and
  // never reaches here; AfterAcquire shares the Default wait branch (the
  // position of notePresentDequeued is the only observable difference).
  const BoundaryPolicy policy =
      presentBoundaryWaitPolicy(resolveBoundaryPolicyFromEnv());
  const auto reachedBoundary = [&] {
    switch (policy) {
      case BoundaryPolicy::DeferredPresentCompletion:
      case BoundaryPolicy::PresentCompletion:
        return presentCompletedSeqId_ >= targetSeqId;
      case BoundaryPolicy::Completion:
        return completedSeqId_ >= targetSeqId;
      case BoundaryPolicy::Default:
      case BoundaryPolicy::AfterAcquire:
        return presentDequeuedSeqId_ >= targetSeqId;
      case BoundaryPolicy::Disabled:
        return true;
    }
    return true;
  };
  if (reachedBoundary()) {
    return;
  }
  const auto waitStarted = std::chrono::steady_clock::now();
  switch (policy) {
    case BoundaryPolicy::DeferredPresentCompletion:
    case BoundaryPolicy::PresentCompletion:
      presentCompletedCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
      break;
    case BoundaryPolicy::Completion:
      finishCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
      break;
    case BoundaryPolicy::Default:
    case BoundaryPolicy::AfterAcquire:
      presentDequeuedCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
      break;
    case BoundaryPolicy::Disabled:
      break;
  }
  const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
  // TLA+: PresentFrameLatency / AppWaitReturnSafe
  DXMT_ASSERT(stop_ || reachedBoundary());
  // TLA+: PresentFrameLatency / PresentCompletionSafety
  DXMT_ASSERT(presentCompletedSeqId_ <= completedSeqId_);
  perf::countPresentBoundaryWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
}

void CommandQueue::deferPresentBoundary(std::uint64_t presentSeqId,
                                        std::uint32_t maxFrameLatency) {
  const std::uint64_t targetSeqId =
      deferredPresentBoundaryTargetSeqId(presentSeqId, maxFrameLatency);
  if (targetSeqId == 0) {
    return;
  }
  const auto qmxBegin = queueMutexProbeBegin();
  std::lock_guard lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "defer_present_boundary");
  deferredPresentBoundaryTargetSeqId_ =
      std::max(deferredPresentBoundaryTargetSeqId_, targetSeqId);
  perf::countPresentBoundaryDeferred();
}

void CommandQueue::drainDeferredPresentBoundary() {
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold ("-cv"): `lock` is handed to presentCompletedCv_.wait(lock, ...)
  // below, which releases and reacquires the mutex an unbounded number of
  // times while parked.
  QueueMutexProbeScope qmxScope(
      qmxBegin, "drain_deferred_present_boundary-cv", /*skipHold=*/true);
  const std::uint64_t targetSeqId = deferredPresentBoundaryTargetSeqId_;
  if (targetSeqId == 0) {
    return;
  }
  const auto reachedBoundary = [&] {
    return presentCompletedSeqId_ >= targetSeqId;
  };
  if (reachedBoundary()) {
    deferredPresentBoundaryTargetSeqId_ = 0;
    return;
  }
  perf::countPresentBoundaryDeferredWait();
  const auto waitStarted = std::chrono::steady_clock::now();
  presentCompletedCv_.wait(lock, [&] { return stop_ || reachedBoundary(); });
  const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
  DXMT_ASSERT(stop_ || reachedBoundary());
  DXMT_ASSERT(presentCompletedSeqId_ <= completedSeqId_);
  if (reachedBoundary()) {
    deferredPresentBoundaryTargetSeqId_ = 0;
  }
  perf::countPresentBoundaryWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
}

// Commit-replay offload present-ordinal boundary. Mirrors presentBoundary /
// deferPresentBoundary above, but paces on
// presentOrdinalGate_.completedOrdinal (a count of retired presents) rather
// than a chunk seqId, since the offload path's caller (src/d3d9) tracks its
// own present ordinal instead of a queue seqId.
// TLA+: PresentFrameLatency ordinal variant (WaitOrdinal /
// PresentOrdinalWaitIsomorphism in PresentFrameLatency.tla).
//
// The policy/target mapping itself is the pure planPresentOrdinalWait()
// (dxmt9_command_queue.hpp) so it is unit-testable without a live queue;
// the wait-state mechanics themselves are the pure PresentOrdinalGate
// (also dxmt9_command_queue.hpp, unit-tested by
// present_ordinal_boundary_spec.cpp); this method only owns the mutex +
// condition-variable mechanics around them.
//
// R-BACK-2.51(h) / R-BACK-6.10: `backBufferCount` and
// `displaySyncEnabled` are folded into the effective latency via the same
// resolvedPresentFrameLatency() helper presentBoundaryLatency() uses for the
// inline seqId-based boundary. The optional back-buffer cap and the
// Immediate-present low-latency default therefore apply identically regardless
// of which boundary mechanism paces a given present.
void CommandQueue::waitPresentOrdinalBoundary(std::uint64_t presentOrdinal,
                                              std::uint32_t maxFrameLatency,
                                              std::uint32_t backBufferCount,
                                              bool displaySyncEnabled) {
  const BoundaryPolicy policy = resolveBoundaryPolicyFromEnv();
  const std::uint32_t effectiveLatency = resolvedPresentFrameLatency(
      maxFrameLatency, backBufferCount, displaySyncEnabled,
      capFrameLatencyToBackBuffers());
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold ("-cv"): `lock` is handed to presentCompletedCv_.wait(lock, ...)
  // below, which releases and reacquires the mutex an unbounded number of
  // times while parked.
  QueueMutexProbeScope qmxScope(
      qmxBegin, "wait_present_ordinal_boundary-cv", /*skipHold=*/true);
  const PresentOrdinalWaitPlan plan = planPresentOrdinalWait(
      policy, presentOrdinal, effectiveLatency, presentOrdinalGate_.deferredTarget);
  // Ordinals are produced by the single app-thread commit path (strictly
  // increasing), so the stored deferred target only ever needs to move
  // forward — no reset-to-0 case exists here, unlike a completion
  // watermark that can be consumed and re-armed.
  presentOrdinalGate_.deferredTarget = plan.storedDeferredTarget;
  const std::uint64_t targetOrdinal = plan.waitTargetOrdinal;
  if (!presentOrdinalGate_.needsWait(targetOrdinal)) {
    // presentOrdinalGate_.aborted: a dead offload worker can never advance
    // completedOrdinal again (see abortPresentOrdinalWaits() doc), so
    // return immediately instead of counting/starting a wait that would
    // otherwise never be satisfied by real progress.
    return;
  }
  perf::countPresentOrdinalBoundaryWait();
  const auto waitStarted = std::chrono::steady_clock::now();
  presentCompletedCv_.wait(lock, [&] {
    return presentOrdinalGate_.waitDone(targetOrdinal, stop_);
  });
  DXMT_ASSERT(presentOrdinalGate_.waitDone(targetOrdinal, stop_));
  perf::countPresentOrdinalBoundaryWaitNs(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - waitStarted).count()));
}

void CommandQueue::abortPresentOrdinalWaits() {
  const auto qmxBegin = queueMutexProbeBegin();
  std::lock_guard lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "abort_present_ordinal_waits");
  presentOrdinalGate_.aborted = true;
  presentCompletedCv_.notify_all();
}

void CommandQueue::notePresentDequeued(std::uint64_t seqId) {
  const auto qmxBegin = queueMutexProbeBegin();
  std::lock_guard lock(mutex_);
  QueueMutexProbeScope qmxScope(qmxBegin, "note_present_dequeued");
  presentDequeuedSeqId_ = std::max(presentDequeuedSeqId_, seqId);
  presentDequeuedCv_.notify_all();
}

void CommandQueue::noteSchedulingPresentDisposition(
    std::uint64_t seqId, bool published) noexcept {
  schedulingProgressWatchdog_.notePresentDisposition(seqId, published);
}

std::optional<core::metalcapture::MetalCaptureRequest>
CommandQueue::metalCaptureForPresentChunk(std::uint64_t seqId) {
  return metalCapture_.maybeCapturePresentChunk(seqId);
}

std::optional<core::metalcapture::MetalCaptureRequest>
CommandQueue::metalCaptureForChunkBegin(std::uint64_t seqId) {
  return metalCapture_.maybeCaptureAtChunkBegin(seqId);
}

bool CommandQueue::metalCaptureEnabled() const noexcept {
  return metalCapture_.enabled();
}

std::optional<core::metalcapture::MetalCaptureRequest>
CommandQueue::notePresentChunkForCapture(std::uint64_t seqId) {
  return metalCapture_.maybePresentChunkClosesSession(seqId);
}

void CommandQueue::submitFlush() {
  perf::countSubmitFlush();
  if (deferActiveArenaFlush() != ActiveArenaAppendResult::Inactive) {
    return;
  }
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold: `lock` is handed to queueLifecycle_.flushAndWait() below, which
  // may unlock/relock it (and cv-wait) via QueueLifecycleController (a
  // different file).
  QueueMutexProbeScope qmxScope(qmxBegin, "submit_flush", /*skipHold=*/true);
  queueLifecycle_.flushAndWait(
      lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
        prepareSlotForPublish(*this, pool_, slot,
                              perf::ChunkPublishReason::Flush);
      });
}

bool CommandQueue::releaseCpuReadySessionBeforeOrderedControl(
    core::metalqueue::SessionReleaseReason reason,
    core::metalqueue::SessionReleaseAction action,
    std::uint64_t fenceRawOrdinal) {
  if (!cpuReadySessionLaneEnabled_) {
    return true;
  }

  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  // skipHold ("-cv"): `lock` is handed to queueLifecycle_.commitCurrentChunk()
  // and to two sessionReleaseCv_.wait(lock, ...) calls below, plus
  // queueLifecycle_.waitForSequence(); all may unlock/relock the mutex an
  // unbounded number of times.
  QueueMutexProbeScope qmxScope(
      qmxBegin, "release_cpu_ready_session-cv", /*skipHold=*/true);
  if (stop_) {
    return false;
  }
  (void)queueLifecycle_.commitCurrentChunk(
      lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
        prepareSlotForPublish(*this, pool_, slot,
                              perf::ChunkPublishReason::SemanticBoundary);
      });
  const std::uint64_t fenceSeqId = lastCommittedSeqId_;

  core::metalqueue::SessionReleasePostResult posted{};
  while (!stop_) {
    posted = sessionReleaseState_.tryPostOrdered(
        reason, action, fenceRawOrdinal, fenceSeqId);
    if (posted.accepted()) {
      break;
    }
    if (posted.status !=
        core::metalqueue::SessionReleasePostStatus::Full) {
      return false;
    }
    sessionReleaseCv_.wait(lock, [this] {
      return stop_ || !sessionReleaseState_.orderedFull();
    });
  }
  if (stop_ || !posted.accepted()) {
    return false;
  }

  encodeCv_.notify_one();
  const std::uint64_t eventOrdinal = posted.snapshot.event.ordinal;
  sessionReleaseCv_.wait(lock, [this, eventOrdinal] {
    return stop_ ||
           sessionReleaseState_.acknowledgedOrdinal() >= eventOrdinal;
  });
  if (stop_) {
    return false;
  }
  if (action == core::metalqueue::SessionReleaseAction::SubmitAndWait &&
      fenceSeqId > completedSeqId_) {
    queueLifecycle_.waitForSequence(lock, fenceSeqId);
  }
  return !stop_;
}

core::HResult CommandQueue::waitForVBlank() {
  submitFlush();
  return core::HResult{0};
}

void* CommandQueue::mapBuffer(core::BufferHandle handle, std::uint32_t flags) {
  // Pool storage + queue's wait-for-sequence rule under one mutex.
  const auto totalStart = std::chrono::steady_clock::now();
  const auto qmxBegin = queueMutexProbeBegin();
  std::unique_lock lock(mutex_);
  const auto lockAcquired = std::chrono::steady_clock::now();
  // skipHold: `lock` is handed to queueLifecycle_.commitCurrentChunk() and
  // queueLifecycle_.waitForSequence() below, both of which may unlock/relock
  // it via QueueLifecycleController (a different file).
  QueueMutexProbeScope qmxScope(qmxBegin, "map_buffer", /*skipHold=*/true);
  std::uint64_t waitSeq = 0;
  {
    // SEGMENT-HOLD: pool_.mapWaitSeqId() never touches `lock` -- this is
    // real, previously-invisible hold time between the outer skipHold=true
    // acquire probe and the first potential unlock/relock helper call below.
    QueueMutexSegmentScope qmxPreCommitSegment("map_buffer/pre_commit");
    waitSeq = pool_.mapWaitSeqId(handle, flags);
  }
  const bool hasWaitSeq = waitSeq != 0;
  const auto waitStart = std::chrono::steady_clock::now();
  // Wine writeonly_vertex_buffer_readback_policy (#66): a Draw followed
  // by a Lock without an intervening Present must still observe the
  // draw on read. The drawn slot's seqId is set as soon as the draw is
  // appended to the current chunk, but the chunk hasn't been committed
  // to Metal yet — without committing, completedSeqId_ can never reach
  // waitSeq and the wait below would block forever. Drive the pending
  // chunk into the submit pipeline first.
  //
  // SEGMENT-HOLD: commitCurrentChunk() and waitForSequence() each own their
  // own interior segment-hold instrumentation (see dxmt9_queue.cpp) around
  // their respective cv waits, so no additional bracketing is added here --
  // wrapping this whole if/if pair would double-count hold time already
  // attributed to "commit_current_chunk/*" below.
  if (waitSeq > lastCommittedSeqId_) {
    queueLifecycle_.commitCurrentChunk(
        lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot,
                                perf::ChunkPublishReason::MapWait);
        });
  }
  // Resource pre-marking may stamp the queue's next sequence even when the
  // replayed chunk emits no backend commands. An empty commit cannot publish
  // that sequence, so waiting for it would have no producer and never finish.
  const std::uint64_t waitTarget =
      core::metalqueue::committedSequenceWaitTarget(waitSeq,
                                                     lastCommittedSeqId_);
  if (waitTarget > completedSeqId_) {
    queueLifecycle_.waitForSequence(lock, waitTarget);
  }
  const auto waitEnd = std::chrono::steady_clock::now();
  void* result = nullptr;
  {
    // SEGMENT-HOLD: from here to return, `lock` is held continuously (no
    // further unlock/relock helper calls below) through the accounting call
    // and pool_.finalizeBufferMap() itself -- this closes out map_buffer's
    // previously entirely-invisible hold time.
    QueueMutexSegmentScope qmxFinalizeSegment("map_buffer/finalize");
    perf::countMapBufferWait(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            waitEnd - totalStart).count()),
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            lockAcquired - totalStart).count()),
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            waitEnd - waitStart).count()),
        flags,
        hasWaitSeq);
    // R-BACK-5.8 — pass the GPU completion watermark and the device
    // reference so the rename ring can rotate / fresh-allocate on
    // DISCARD without blocking on prior completion. Non-DYNAMIC paths
    // ignore both arguments.
    result = pool_.finalizeBufferMap(device_, handle, flags, completedSeqId_);
  }
  return result;
}

bool CommandQueue::readbackSurface(const core::ReadbackDesc& desc, core::ReadbackPixels& pixels) {
  return encoders::readbackSurface(*this, pool_, device_, limits_, desc, pixels);
}

void CommandQueue::runEncodeLoop(EncodeChunkFn encodeChunk, OnSubmittedFn onSubmitted) {
  while (true) {
    const auto qmxBegin = queueMutexProbeBegin();
    std::unique_lock lock(mutex_);
    // skipHold: `lock` is handed to queueLifecycle_.runEncodeIteration()
    // below, which may unlock/relock it (and cv-wait) via
    // QueueLifecycleController (a different file). One acquire-wait sample
    // is recorded per loop iteration.
    QueueMutexProbeScope qmxScope(qmxBegin, "run_encode_loop", /*skipHold=*/true);
    if (!queueLifecycle_.runEncodeIteration(lock, encodeChunk, onSubmitted)) {
      return;
    }
  }
}

void CommandQueue::runDceChunkLookaheadEncodeLoop(
    OnSubmittedFn onSubmitted) {
  // TLA+: DceChunkLookahead. One held Encoding source plus the FIFO ready
  // queue refines HeldIsNext / ReadyIsFollowingPrefix; submitting current
  // before carrying next refines SubmittedPrefix. A prior validated proof may
  // place the ready-FIFO sample after a prefix of the optimized replay
  // permutation: that prefix is encoded into an unsubmitted EncodeSession,
  // while only the freshly selected successor can authorize omissions in the
  // appended suffix.
  using core::metalqueue::QueueSubmissionRecord;
  using core::metalqueue::ReadySlotSnapshot;
  using core::metalqueue::ResolvedPublishedSource;

  std::optional<ReadySlotSnapshot> held;
  while (true) {
    const auto qmxBegin = queueMutexProbeBegin();
    std::unique_lock lock(mutex_);
    // skipHold: this loop body manually unlock()s/lock()s `lock` many times
    // (queueLifecycle_ calls, backend_->onSourceReady/onChunkReady, and the
    // post-commit callback drain), so only the acquire-wait for the lock
    // taken at the top of each loop iteration is recorded.
    QueueMutexProbeScope qmxScope(
        qmxBegin, "run_dce_chunk_lookahead_encode_loop", /*skipHold=*/true);
    if (!held.has_value()) {
      ReadySlotSnapshot source{};
      if (!queueLifecycle_.dequeueReadySlot(lock, source)) {
        return;
      }
      held = source;
    }

    ReadySlotSnapshot current = *held;
    held.reset();
    std::optional<encoders::EncodeContext> ctx;
    std::vector<std::uint32_t> predictedPrefix;
    bool encodedPrefix = false;
    std::optional<QueueSubmissionRecord> prefixSubmission;
    encoders::EncodeChunkSession prefixSession;
    {
      const ResolvedPublishedSource resolved =
          queueLifecycle_.resolveRepresentedSource(current);
      if (!resolved.valid()) {
        queueLifecycle_.poisonTapeFailureLocked();
        return;
      }
      if (!resolved.slot) {
        auto arenaCtx = makeEncodeContext();
        encoders::EncodeChunkOptions arenaOptions{};
        arenaOptions.partitionExecutionMode =
            renderPartitionConfig_.resolved;
        arenaOptions.partitionSource = resolved.source;
        lock.unlock();
        auto arenaSubmission = backend_->onSourceReady(
            arenaCtx, current.slotIndex, resolved.payload, current.seqId,
            std::move(arenaOptions));
        if (arenaSubmission.has_value() &&
            !arenaSubmission->commandBuffer) {
          arenaSubmission.reset();
        }
        lock.lock();
        if (arenaSubmission.has_value()) {
          if (!core::metalqueue::assignOrValidateSingleCompletionSource(
                  *arenaSubmission, current)) {
            abortDceLookaheadPendingFailOpen(
                "arena completion source retention failed");
          }
          if (!queueLifecycle_.submitEncodedSubmission(lock,
                                                       *arenaSubmission)) {
            abortDceLookaheadPendingFailOpen(
                "arena submission preflight failed");
          }
          auto callbacks = std::move(arenaSubmission->postCommitCallbacks);
          lock.unlock();
          for (auto& callback : callbacks) {
            if (callback) {
              callback();
            }
          }
          lock.lock();
        } else {
          if (!queueLifecycle_.completeInlineChunk(
                  lock, current.slotIndex, current.seqId)) {
            return;
          }
          if (onSubmitted) {
            onSubmitted(current.seqId);
          }
        }
        continue;
      }
      auto& slot = *resolved.slot;
      lock.unlock();

      traceEncodeFnStage("entry", current.slotIndex, slot);
      traceEncodeFnStage("before-make-context", current.slotIndex, slot);
      ctx.emplace(makeEncodeContext());
      traceEncodeFnStage("after-make-context", current.slotIndex, slot);

      predictedPrefix =
          backend_->dceLookaheadReplayPrefix(*ctx, current.slotIndex, slot);
      if (encodeSlotPsoPrefetchEnabled() &&
          !slot.prefetchedPipelinesSealed()) {
        traceEncodeFnStage("before-pso-prefetch", current.slotIndex, slot);
        PerfScope scope(perf::countEncodeSlotPsoPrefetchCpuTime);
        prefetchSlotPipelines(const_cast<core::ChunkSlot&>(slot));
        traceEncodeFnStage("after-pso-prefetch", current.slotIndex, slot);
      }

      bool predictedPrefixValid =
          !predictedPrefix.empty() &&
          predictedPrefix.size() < slot.commandCount();
      std::vector<bool> predictedCommands(slot.commandCount(), false);
      for (const std::uint32_t commandIndex : predictedPrefix) {
        if (commandIndex >= slot.commandCount() ||
            predictedCommands[commandIndex] ||
            slot.commandHeaders[commandIndex].kind ==
                core::MetalCommandKind::Present) {
          predictedPrefixValid = false;
          break;
        }
        predictedCommands[commandIndex] = true;
      }
      if (!predictedPrefixValid) {
        predictedPrefix.clear();
      }

      if (!predictedPrefix.empty()) {
        prefixSession = encoders::makeEncodeChunkSession();
        encoders::EncodeChunkOptions prefixOptions{};
        prefixOptions.partitionExecutionMode =
            renderPartitionConfig_.resolved;
        prefixOptions.allowInjectedCommandBufferMidChunkCommits = true;
        prefixOptions.session = prefixSession.get();
        prefixOptions.deferSessionFinalization = true;
        prefixOptions.replayCommandPlanActive = true;
        prefixOptions.replayCommandOrder = predictedPrefix;
        prefixOptions.skipBackendPlanning = true;
        prefixOptions.partitionSource = resolved.source;
        traceEncodeFnStage("before-dce-lookahead-prefix",
                           current.slotIndex, slot);
        prefixSubmission = backend_->onChunkReady(
            *ctx, current.slotIndex, slot, std::move(prefixOptions));
        traceEncodeFnStage(prefixSubmission.has_value()
                               ? "after-dce-lookahead-prefix"
                               : "after-dce-lookahead-prefix-null",
                           current.slotIndex, slot);
        if (prefixSubmission.has_value() &&
            prefixSubmission->commandBuffer) {
          encodedPrefix = true;
          perf::countFramegraphDceLookaheadPrefix(predictedPrefix.size());
        } else {
          prefixSubmission.reset();
          prefixSession.reset();
          predictedPrefix.clear();
        }
      }
    }

    lock.lock();
    ReadySlotSnapshot next{};
    bool hasNext = false;
    const DceChunkLookaheadAction action =
        resolveDceChunkLookaheadAction(!cpuReadyTape_.readyEmpty());
    if (action == DceChunkLookaheadAction::UseReady) {
      hasNext = queueLifecycle_.dequeueReadySlot(lock, next);
      DXMT_ASSERT(hasNext);
      // TLA+: DceChunkLookahead / HeldIsNext.
      DXMT_ASSERT(!hasNext || next.seqId == current.seqId + 1u);
      perf::countFramegraphDceLookaheadSelected();
    } else {
      perf::countFramegraphDceLookaheadFailOpen();
    }

    std::array<ResolvedPublishedSource, 2> selected{};
    selected[0] = queueLifecycle_.resolveRepresentedSource(current);
    if (hasNext) {
      selected[1] = queueLifecycle_.resolveRepresentedSource(next);
    }
    const DceChunkLookaheadSourceAction sourceAction =
        resolveDceChunkLookaheadSourceAction(
            selected[0].valid(), selected[0].slot != nullptr, hasNext,
            selected[1].valid(), selected[1].slot != nullptr);
    if (sourceAction == DceChunkLookaheadSourceAction::Poison) {
      queueLifecycle_.poisonTapeFailureLocked();
      return;
    }
    encoders::EncodeChunkOptions options{};
    options.partitionExecutionMode = renderPartitionConfig_.resolved;
    options.partitionSource = selected[0].source;
    if (sourceAction ==
        DceChunkLookaheadSourceAction::EncodeCurrentExposeLegacyLookahead) {
      options.sessionLookaheadSources =
          std::span<const ResolvedPublishedSource>(selected);
    }
    if (encodedPrefix) {
      options.allowInjectedCommandBufferMidChunkCommits = true;
      options.commandBuffer = std::move(prefixSubmission->commandBuffer);
      options.session = prefixSession.get();
      options.sessionSource =
          core::metalqueue::completionSourceForReadySlot(current);
      options.replayCommandsAlreadyEncoded = predictedPrefix;
    }

    std::optional<QueueSubmissionRecord> submission;
    {
      const auto& slot = *selected[0].slot;
      lock.unlock();
      traceEncodeFnStage("before-backend-onChunkReady",
                         current.slotIndex, slot);
      submission = backend_->onChunkReady(
          *ctx, current.slotIndex, slot, std::move(options));
      if (submission.has_value() && !submission->commandBuffer) {
        submission.reset();
      }
      if (encodedPrefix) {
        if (!submission.has_value()) {
          abortDceLookaheadPendingFailOpen(
              "suffix encode returned no command buffer");
        }
        const std::uint64_t prefixChainLength =
            std::max<std::uint64_t>(
                1u, prefixSubmission->commandBufferChainLength);
        const std::uint64_t suffixChainLength =
            std::max<std::uint64_t>(
                1u, submission->commandBufferChainLength);
        submission->commandBufferChainLength =
            prefixChainLength + suffixChainLength - 1u;
        if (!encoders::retainEncodeChunkSessionUntilSubmissionComplete(
                std::move(prefixSession), *submission)) {
          abortDceLookaheadPendingFailOpen("session retain failed");
        }
      }
      traceEncodeFnStage(submission.has_value()
                             ? "after-backend-onChunkReady-submission"
                             : "after-backend-onChunkReady-inline",
                         current.slotIndex, slot);
    }
    selected = {};
    lock.lock();

    if (submission.has_value()) {
      if (submission->fixedCompletionSources.empty()) {
        const std::array completionSources{
            core::metalqueue::completionSourceForReadySlot(current),
        };
        if (!submission->assignFixedCompletionSources(completionSources)) {
          abortDceLookaheadPendingFailOpen(
              "completion source retention failed");
        }
      }
      if (!queueLifecycle_.submitEncodedSubmission(lock, *submission)) {
        abortDceLookaheadPendingFailOpen("submission preflight failed");
      }
      auto callbacks = std::move(submission->postCommitCallbacks);
      lock.unlock();
      for (auto& callback : callbacks) {
        if (callback) {
          callback();
        }
      }
      lock.lock();
    } else {
      if (!queueLifecycle_.completeInlineChunk(
              lock, current.slotIndex, current.seqId)) {
        return;
      }
      if (onSubmitted) {
        onSubmitted(current.seqId);
      }
    }

    if (hasNext) {
      held = next;
      // TLA+: DceChunkLookahead / BoundedHold.
      DXMT_ASSERT(held->sourceId.valid());
    }
  }
}

std::optional<core::metalqueue::QueueSubmissionRecord>
CommandQueue::encodeCpuReadySessionSource(
    const core::metalqueue::ResolvedPublishedSource& source,
    encoders::EncodeChunkOptions options) {
  options.partitionExecutionMode = renderPartitionConfig_.resolved;
  std::optional<core::metalqueue::QueueSubmissionRecord> submission;
  if (const auto* legacy = source.payload.legacyPayload()) {
    auto& slot = *legacy;
    traceEncodeFnStage("entry", source.slotIndex, slot);
    if (encodeSlotPsoPrefetchEnabled() &&
        !slot.prefetchedPipelinesSealed()) {
      PerfScope scope(perf::countEncodeSlotPsoPrefetchCpuTime);
      prefetchSlotPipelines(const_cast<core::ChunkSlot&>(slot));
    }
    auto ctx = makeEncodeContext();
    submission = backend_->onChunkReady(ctx, source.slotIndex, slot,
                                        std::move(options));
  } else {
    auto ctx = makeEncodeContext();
    submission = backend_->onSourceReady(ctx, source.slotIndex,
                                         source.payload, source.seqId,
                                         std::move(options));
  }
  if (submission.has_value() && !submission->commandBuffer &&
      !submission->testOnlyAllowNullCommandBuffer) {
    submission.reset();
  }
  return submission;
}

void CommandQueue::bindSelfLifecycle(ResolveSurfaceFlagsFn resolveSurfaceFlags) {
  queueLifecycle_.bindTrackedSubmissionState({
      .writingSlot = &writingSlot_,
      .writeIndex = &writeIndex_,
      .nextSeqId = &nextSeqId_,
      .completedSeqQueue = &completedSeqQueue_,
      .completedPresentSeqQueue = &completedPresentSeqQueue_,
      .inflightCount = &inflightCount_,
      .completedSeqId = &completedSeqId_,
      .presentCompletedSeqId = &presentCompletedSeqId_,
      .completedPresentOrdinal = &presentOrdinalGate_.completedOrdinal,
      .lastCommittedSeqId = &lastCommittedSeqId_,
      .slots = std::span<core::ChunkSlotControl>(slots_.data(), slots_.size()),
      .cpuReadyTape = &cpuReadyTape_,
      .mutex = &mutex_,
      .writeCv = &writeCv_,
      .encodeCv = &encodeCv_,
      .finishCv = &finishCv_,
      .presentCompletedCv = &presentCompletedCv_,
      .presentDequeuedCv = &presentDequeuedCv_,
      .sessionReleaseCv = &sessionReleaseCv_,
      .stop = &stop_,
      .submissionDiagnostics = &submissionDiagnostics_,
      .schedulingProgressWatchdog = &schedulingProgressWatchdog_,
      .resolveSurfaceFlags = std::move(resolveSurfaceFlags),
  });
}

void CommandQueue::noteInitializerPendingUploads() noexcept {
  encodeCv_.notify_all();
}

void CommandQueue::requestSchedulingStopLocked() noexcept {
  stop_ = true;
  cpuReadyTape_.stopAdmission();
  notifySchedulingTerminalWaiters(
      render::SchedulingTerminalDisposition::Stop);
}

void CommandQueue::notifySchedulingTerminalWaiters(
    render::SchedulingTerminalDisposition disposition) noexcept {
  schedulingProgressWatchdog_.noteTerminal(
      disposition == render::SchedulingTerminalDisposition::DeviceLoss);
  const auto wake = render::planSchedulingTerminalWake(disposition);
  if (wake.wakes(render::SchedulingWakeWriter)) writeCv_.notify_all();
  if (wake.wakes(render::SchedulingWakeEncoder)) encodeCv_.notify_all();
  if (wake.wakes(render::SchedulingWakeFinish)) finishCv_.notify_all();
  if (wake.wakes(render::SchedulingWakePresentCompleted)) {
    presentCompletedCv_.notify_all();
  }
  if (wake.wakes(render::SchedulingWakePresentDequeued)) {
    presentDequeuedCv_.notify_all();
  }
  if (wake.wakes(render::SchedulingWakeSessionRelease)) {
    sessionReleaseCv_.notify_all();
  }
  if (wake.wakes(render::SchedulingWakePendingCompletion)) {
    queueLifecycle_.requestPendingCompletionStop();
  }
}

void CommandQueue::runFinishLoop() {
  while (true) {
    const auto qmxBegin = queueMutexProbeBegin();
    std::unique_lock lock(mutex_);
    // skipHold: `lock` is handed to queueLifecycle_.runFinishIteration()
    // below, which may unlock/relock it (and cv-wait) via
    // QueueLifecycleController (a different file). One acquire-wait sample
    // is recorded per loop iteration.
    QueueMutexProbeScope qmxScope(qmxBegin, "run_finish_loop", /*skipHold=*/true);
    if (!queueLifecycle_.runFinishIteration(lock, [this](std::uint64_t) {
          allocators_.reclaim(completedSeqId_);
          pool_.reclaimCompleted(completedSeqId_);
          transientArena_.reclaim(completedSeqId_);
        })) {
      return;
    }
  }
}

void CommandQueue::runCompletionWatcherLoop() {
  while (queueLifecycle_.processOnePendingCompletion()) {
    // continue until processOnePendingCompletion returns false (stop)
  }
}

core::PresentId CommandQueue::registerPresenter(Presenter* presenter) {
  if (!presenter) {
    return {};
  }
  std::lock_guard lock(presenterRegistryMutex_);
  std::uint32_t slotIndex = 0;
  if (presenterFreeHead_ >= 0) {
    slotIndex = static_cast<std::uint32_t>(presenterFreeHead_);
    presenterFreeHead_ = presenterSlots_[slotIndex].nextFree;
    presenterSlots_[slotIndex].nextFree = -1;
  } else {
    slotIndex = static_cast<std::uint32_t>(presenterSlots_.size());
    presenterSlots_.emplace_back();
  }
  auto& slot = presenterSlots_[slotIndex];
  slot.presenter = presenter;
  slot.pendingToken.reset();
  // Generation is bumped on free; the current value is what the caller
  // observes for this allocation lifetime.
  return core::PresentId{encodePresentId(slotIndex, slot.generation)};
}

void CommandQueue::unregisterPresenter(core::PresentId id) {
  if (!id) {
    return;
  }
  std::lock_guard lock(presenterRegistryMutex_);
  const std::uint32_t slotIndex = decodePresentIdSlot(id);
  if (slotIndex >= presenterSlots_.size()) {
    return;
  }
  auto& slot = presenterSlots_[slotIndex];
  if (slot.generation != decodePresentIdGeneration(id)) {
    return;
  }
  slot.presenter = nullptr;
  slot.pendingToken.reset();
  // Bump generation so any in-flight PresentId carrying the old value
  // resolves to nullptr in lookupPresenter — the encoder will skip the
  // present rather than reach into a destroyed Presenter.
  ++slot.generation;
  slot.nextFree = presenterFreeHead_;
  presenterFreeHead_ = static_cast<std::int32_t>(slotIndex);
}

Presenter* CommandQueue::lookupPresenter(core::PresentId id) const {
  if (!id) {
    return nullptr;
  }
  std::lock_guard lock(presenterRegistryMutex_);
  const std::uint32_t slotIndex = decodePresentIdSlot(id);
  if (slotIndex >= presenterSlots_.size()) {
    return nullptr;
  }
  const auto& slot = presenterSlots_[slotIndex];
  if (slot.generation != decodePresentIdGeneration(id)) {
    return nullptr;
  }
  return slot.presenter;
}

void CommandQueue::stashDrawableToken(core::PresentId id,
                                       std::shared_ptr<PresentDrawableToken> token) {
  if (!id) {
    return;
  }
  std::lock_guard lock(presenterRegistryMutex_);
  const std::uint32_t slotIndex = decodePresentIdSlot(id);
  if (slotIndex >= presenterSlots_.size()) {
    return;
  }
  auto& slot = presenterSlots_[slotIndex];
  if (slot.generation != decodePresentIdGeneration(id)) {
    return;
  }
  slot.pendingToken = std::move(token);
}

std::shared_ptr<PresentDrawableToken>
CommandQueue::takeDrawableToken(core::PresentId id) {
  if (!id) {
    return {};
  }
  std::lock_guard lock(presenterRegistryMutex_);
  const std::uint32_t slotIndex = decodePresentIdSlot(id);
  if (slotIndex >= presenterSlots_.size()) {
    return {};
  }
  auto& slot = presenterSlots_[slotIndex];
  if (slot.generation != decodePresentIdGeneration(id)) {
    return {};
  }
  return std::exchange(slot.pendingToken, {});
}

}  // namespace dxmt9
