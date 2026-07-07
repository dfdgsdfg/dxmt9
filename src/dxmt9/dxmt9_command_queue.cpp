#include "dxmt9_command_queue.hpp"
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
#include "render/tail_present_batch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

[[noreturn]] void abortOpenCbPendingFailOpen(const char* reason) {
  std::fprintf(stderr,
               "[dxmt9-queue] fatal: encoded open-CB pending work could not "
               "fail-open by submit (%s)\n",
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

bool publishPsoPrefetchDisabled() {
  static const bool disabled = [] {
    const char* env = std::getenv("DXMT9_DISABLE_PUBLISH_PSO_PREFETCH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return disabled;
}

bool publishPsoPrefetchForced() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_ENABLE_PUBLISH_PSO_PREFETCH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool publishPsoPrefetchEnabled() {
  return publishPsoPrefetchForced() && !publishPsoPrefetchDisabled();
}

bool encodeSlotPsoPrefetchDisabled() {
  static const bool disabled = [] {
    const char* env = std::getenv("DXMT9_DISABLE_ENCODE_SLOT_PSO_PREFETCH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return disabled;
}

bool encodeSlotPsoPrefetchEnabled() {
  return !publishPsoPrefetchEnabled() && !encodeSlotPsoPrefetchDisabled();
}

bool encodeTailPresentBatchEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_ENCODE_TAIL_PRESENT_BATCH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool openCbPreencodeTailPresentEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool openCbRenderSessionCarryEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_OPEN_CB_CARRY_RENDER_SESSION");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool openCbSemanticBoundaryPublishEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled &&
         openCbPreencodeTailPresentEnabled() &&
         openCbRenderSessionCarryEnabled();
}

bool openCbWriterActiveCpuReadyPublishEnabled() {
  static const bool enabled = [] {
    const char* env =
        std::getenv("DXMT9_OPEN_CB_WRITER_ACTIVE_CPU_READY_PUBLISH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled && openCbSemanticBoundaryPublishEnabled();
}

bool openCbActiveWaitCpuReadyAppendEnabled() {
  static const bool enabled = [] {
    const char* env =
        std::getenv("DXMT9_OPEN_CB_ACTIVE_WAIT_CPU_READY_APPEND");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled && openCbSemanticBoundaryPublishEnabled();
}

bool openCbWaitStartCpuReadyPublishEnabled() {
  static const bool enabled = [] {
    const char* env =
        std::getenv("DXMT9_OPEN_CB_WAIT_START_CPU_READY_PUBLISH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled && openCbSemanticBoundaryPublishEnabled();
}

bool openCbDrawAttachmentBoundaryPublishEnabled() {
  static const bool enabled = [] {
    const char* env =
        std::getenv("DXMT9_OPEN_CB_DRAW_ATTACHMENT_BOUNDARY_PUBLISH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled && openCbSemanticBoundaryPublishEnabled();
}

bool openCbDrawContinuationBoundaryPublishEnabled() {
  static const bool enabled = [] {
    const char* env =
        std::getenv("DXMT9_OPEN_CB_DRAW_CONTINUATION_BOUNDARY_PUBLISH");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled &&
         openCbPreencodeTailPresentEnabled() &&
         openCbRenderSessionCarryEnabled();
}

std::size_t openCbDrawContinuationCommandLimit() {
  static const std::size_t limit = [] {
    const char* env =
        std::getenv("DXMT9_OPEN_CB_DRAW_CONTINUATION_COMMAND_LIMIT");
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
  return openCbDrawContinuationBoundaryPublishEnabled() ? limit
                                                        : std::size_t{0};
}

render::OpenCbSemanticBoundaryReleaseMode openCbSemanticBoundaryReleaseMode() {
  static const auto mode = [] {
    const char* env =
        std::getenv("DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_RELEASE_MODE");
    if (env && std::strcmp(env, "deterministic") == 0) {
      return render::OpenCbSemanticBoundaryReleaseMode::Deterministic;
    }
    return render::OpenCbSemanticBoundaryReleaseMode::CompletionWait;
  }();
  return mode;
}

std::chrono::microseconds openCbPendingTailWaitTimeout() {
  static const auto timeout = [] {
    const char* env = std::getenv("DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US");
    if (!env || env[0] == '\0') {
      return std::chrono::microseconds{0};
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    if (end == env || parsed == 0) {
      return std::chrono::microseconds{0};
    }
    const auto max =
        static_cast<unsigned long long>(
            std::chrono::microseconds::max().count());
    return std::chrono::microseconds{
        static_cast<std::chrono::microseconds::rep>(std::min(parsed, max))};
  }();
  return openCbPreencodeTailPresentEnabled() ? timeout
                                             : std::chrono::microseconds{0};
}

bool stageTailPresentChunkEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_STAGE_TAIL_PRESENT_CHUNK");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled && encodeTailPresentBatchEnabled();
}

std::size_t stagePrePresentCommandLimit() {
  static const std::size_t limit = [] {
    const char* env = std::getenv("DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT");
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
  return (encodeTailPresentBatchEnabled() || openCbPreencodeTailPresentEnabled())
      ? limit
      : std::size_t{0};
}

std::size_t openCbCpuReadyCommandLimit() {
  static const std::size_t limit = [] {
    const char* env = std::getenv("DXMT9_OPEN_CB_CPU_READY_COMMAND_LIMIT");
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
  return openCbSemanticBoundaryPublishEnabled() ? limit : std::size_t{0};
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

CommandQueue::CommandQueue(WMT::Device device, core::BackendLimits limits)
    : device_(device),
      limits_(limits),
      shaderArchive_(device, resolveShaderCachePath(device)) {
  if (!device_) {
    return;
  }
  // R-BACK-3.7 / R-BACK-3.8 — drive the prewarm step now that the
  // archive instance is constructed. Runs the §6.1 failure-mode table
  // and bumps the relevant perf counters (missing / lock_busy /
  // entries / bytes / load_ns). Non-fatal under all conditions; never
  // blocks queue init beyond a single bounded flock retry budget.
  {
    const auto prewarmMode = archive_prewarm::resolveMode();
    archive_prewarm::run(device, shaderArchive_.path(), prewarmMode);
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
  // With DXMT9_RENDER_MODE unset this resolves to TraditionalBackend, whose
  // onChunkReady forwards to encoders::encodeChunk — byte-identical baseline.
  backend_ = render::createBackendFromEnv();

  // Spawn the three worker threads. Threads block on writeCv_ until the
  // first submit; no race with DeviceImpl's still-completing ctor because
  // submits can only happen after CreateDXMT9Device returns.
  startThreads(
      [this] {
        auto encodeSingleSource = [this](std::size_t slotIndex,
                                          core::ChunkSlot& slot) {
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
              auto submission = backend_->onChunkReady(ctx, slotIndex, slot);
              traceEncodeFnStage(submission.has_value()
                                     ? "after-backend-onChunkReady-submission"
                                     : "after-backend-onChunkReady-inline",
                                 slotIndex,
                                 slot);
              return submission;
            };
        if (openCbPreencodeTailPresentEnabled()) {
          runOpenCbTailPresentEncodeLoop(
              [this](std::uint64_t) { allocators_.reclaim(completedSeqId_); });
          return;
        }
        if (encodeTailPresentBatchEnabled()) {
          std::array<core::metalqueue::ReadySlotSnapshot, kCommandChunkCount> scratch{};
          runEncodeBatchLoop(
              std::span<core::metalqueue::ReadySlotSnapshot>(scratch),
              [this, encodeSingleSource](
                  std::span<core::metalqueue::ReadySlotSnapshot> sources) mutable {
                if (sources.size() == 1u) {
                  DXMT_ASSERT(sources.front().slot != nullptr);
                  auto& slot = *sources.front().slot;
                  return encodeSingleSource(sources.front().slotIndex, slot);
                }
                for (const auto& source : sources) {
                  DXMT_ASSERT(source.slot != nullptr);
                  traceEncodeFnStage("batch-entry", source.slotIndex, *source.slot);
                }
                auto ctx = makeEncodeContext();
                auto submission = backend_->onChunkBatchReady(ctx, sources);
                DXMT_ASSERT(submission.has_value() &&
                            "tail-Present batch selector produced an unencodable batch");
                return submission;
              },
              [this](std::uint64_t) { allocators_.reclaim(completedSeqId_); },
              {},
              render::selectTailPresentBatchPrefix);
          return;
        }
        runEncodeLoop(
            encodeSingleSource,
            [this](std::uint64_t) { allocators_.reclaim(completedSeqId_); });
      },
      [this] { runFinishLoop(); },
      [this] { runCompletionWatcherLoop(); });
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
    std::lock_guard lock(mutex_);
    transientCompletedSeqId = completedSeqId_;
  }
  auto ctx = encoders::EncodeContext{
      device_, limits_, pool_, pipelineCache_, allocators_,
      &shaderArchive_.reference(), &shaderArchive_.path(),
      *this,
      consumePendingDirty(),
  };
  ctx.transientCompletedSeqId = transientCompletedSeqId;
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
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
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
  std::lock_guard lock(mutex_);
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
  std::lock_guard lock(mutex_);
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
  std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
    stop_ = true;
    encodeCv_.notify_all();
    finishCv_.notify_all();
    presentCompletedCv_.notify_all();
    presentDequeuedCv_.notify_all();
    writeCv_.notify_all();
  }
  queueLifecycle_.notifyPendingCompletionStop();
  if (encodeThread_.joinable()) encodeThread_.join();
  if (completionThread_.joinable()) completionThread_.join();
  if (finishThread_.joinable()) finishThread_.join();
  threadsStarted_ = false;
}

// --- Chunk-ring submission (Step 3b migration from MetalBackendDevice) --

namespace {

core::ChunkSlot& currentSlotUnlocked(CommandQueue& q) {
  // TLA+: RingSafety — caller holds q.mutex_ and has ensured a writing slot.
  return q.slots_[*q.writingSlot_];
}

void ensureWritingSlotUnlocked(CommandQueue& q, std::unique_lock<std::mutex>& lock) {
  (void)q.queueLifecycle_.ensureWriterSlot(lock, kMaxQueuedChunks);
}

std::uint64_t seqIdForMark(CommandQueue& q, std::uint64_t seqId) {
  return seqId == 0 ? q.nextSeqId_ : seqId;
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

bool splitPresentChunk() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_SPLIT_PRESENT_CHUNK");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
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
// the wait branch here; its effect lives in dxmt9_draw_encoder.mm,
// which also reads through resolveBoundaryPolicyFromEnv().

bool capFrameLatencyToBackBuffers() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

std::uint32_t backBufferLatencyCap(std::uint32_t backBufferCount) {
  const std::uint32_t normalized = std::max(1u, backBufferCount);
  if (normalized >= core::kMaxFrameLatency) {
    return core::kMaxFrameLatency;
  }
  return normalized + 1u;
}

std::uint32_t presentBoundaryLatency(const core::SwapDesc& desc) {
  if (capFrameLatencyToBackBuffers()) {
    return std::min(desc.maxFrameLatency, backBufferLatencyCap(desc.backBufferCount));
  }
  return desc.maxFrameLatency;
}

bool shouldApplyPresentBoundary(const core::SwapDesc&) {
  return resolveBoundaryPolicyFromEnv() != BoundaryPolicy::Disabled;
}

// DXMT9_OFFLOAD_COMMIT_REPLAY — the commit-replay offload path paces
// present frame-latency itself via CommandQueue::waitPresentOrdinalBoundary
// from the PE side, keyed on a present ordinal instead of a queue seqId.
// When enabled, submitPresent() must not also drain/apply the inline
// seqId-based boundary below, or the two mechanisms would double-wait on
// the same present token.
bool offloadCommitReplayEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_OFFLOAD_COMMIT_REPLAY");
    return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

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

perf::ChunkPublishTailCommandKind chunkPublishTailCommandKind(
    const core::ChunkSlot& slot) {
  if (slot.commandHeaders.empty()) {
    return perf::ChunkPublishTailCommandKind::Empty;
  }
  return chunkPublishTailCommandKind(slot.commandHeaders.back().kind);
}

void prepareSlotForPublish(CommandQueue& q,
                           resources::Pool& pool,
                           core::ChunkSlot& slot,
                           perf::ChunkPublishReason reason) {
  slot.publishReason = reason;
  const std::uint64_t residencyNs =
      recordCurrentSlotPublishResidencyUnlocked(q, reason);
  perf::countChunkPublishReason(reason, slot.commandCount());
  if (reason == perf::ChunkPublishReason::PresentSplitBefore) {
    perf::countChunkPublishPresentSplitBeforeTail(
        chunkPublishTailCommandKind(slot), slot.drawOnlyCommandStream());
  }
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
  {
    PerfScope stageScope(perf::countPrepareSlotPsoPrefetchCpuTime);
    if (publishPsoPrefetchEnabled()) {
      q.prefetchSlotPipelines(slot);
    }
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

bool commitAndStageCurrentPrePresentSlotUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock,
    std::size_t postCommitHeadroom) {
  if (!(encodeTailPresentBatchEnabled() ||
        openCbPreencodeTailPresentEnabled()) ||
      !q.writingSlot_ ||
      currentSlotUnlocked(q).commandsEmpty()) {
    return false;
  }
  if (q.inflightCount_ + 1u + postCommitHeadroom > kMaxQueuedChunks) {
    return false;
  }

  const bool committed = q.queueLifecycle_.commitCurrentChunk(
      lock, kMaxQueuedChunks, [&q, &pool](core::ChunkSlot& slot) {
        prepareSlotForPublish(q, pool, slot,
                              perf::ChunkPublishReason::PresentSplitBefore);
      });
  if (!committed || q.readySlots_.empty()) {
    return false;
  }

  if (openCbPreencodeTailPresentEnabled()) {
    if (q.skipDrawResourceMarking_) {
      q.forceDrawResourceMarkingAfterSplit_ = true;
    }
    return true;
  }

  const std::size_t stagedSlotIndex = q.readySlots_.back();
  if (!q.queueLifecycle_.stageLastReadySlot(
          lock, q.stagedTailPresentSlots_, stagedSlotIndex)) {
    return false;
  }
  if (q.skipDrawResourceMarking_) {
    q.forceDrawResourceMarkingAfterSplit_ = true;
  }
  return true;
}

bool maybeStagePrePresentChunkUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock) {
  const std::size_t limit = stagePrePresentCommandLimit();
  if (limit == 0 || !q.writingSlot_) {
    return false;
  }
  if (currentSlotUnlocked(q).commandCount() < limit) {
    return false;
  }
  const bool staged = commitAndStageCurrentPrePresentSlotUnlocked(
      q, pool, lock, /*postCommitHeadroom=*/2u);
  if (staged) {
    ensureWritingSlotUnlocked(q, lock);
  }
  return staged;
}

bool maybePublishSemanticBoundaryChunkUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock) {
  if (!openCbSemanticBoundaryPublishEnabled() || !q.writingSlot_) {
    return false;
  }

  auto& slot = currentSlotUnlocked(q);
  if (slot.commandsEmpty() || !slot.presentRecords.empty()) {
    return false;
  }

  const bool committed = q.queueLifecycle_.commitCurrentChunk(
      lock, kMaxQueuedChunks, [&q, &pool](core::ChunkSlot& publishSlot) {
        prepareSlotForPublish(q, pool, publishSlot,
                              perf::ChunkPublishReason::SemanticBoundary);
      });
  if (!committed) {
    return false;
  }
  if (q.skipDrawResourceMarking_) {
    q.forceDrawResourceMarkingAfterSplit_ = true;
  }
  ensureWritingSlotUnlocked(q, lock);
  return true;
}

bool maybePublishOpenCbCpuReadyCommandLimitUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock) {
  const std::size_t limit = openCbCpuReadyCommandLimit();
  if (limit == 0 || !q.writingSlot_) {
    return false;
  }

  auto& slot = currentSlotUnlocked(q);
  if (slot.commandsEmpty() ||
      !slot.presentRecords.empty() ||
      slot.commandCount() < limit) {
    return false;
  }
  if (q.inflightCount_ + 2u > kMaxQueuedChunks) {
    return false;
  }
  return maybePublishSemanticBoundaryChunkUnlocked(q, pool, lock);
}

const core::FlatDrawStateRecord* lastDrawTailState(
    const core::ChunkSlot& slot) noexcept {
  if (slot.commandHeaders.empty() ||
      slot.commandHeaders.back().kind != core::MetalCommandKind::DrawRun) {
    return nullptr;
  }
  const auto command = slot.drawRunCommandAt(slot.commandHeaders.size() - 1u);
  return command.drawState.hot;
}

bool maybePublishOpenCbDrawAttachmentBoundaryChunkUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock,
    const core::FlatDrawStateRecord& nextHot) {
  if (!openCbDrawAttachmentBoundaryPublishEnabled() || !q.writingSlot_) {
    return false;
  }

  auto& slot = currentSlotUnlocked(q);
  if (slot.commandsEmpty() || !slot.presentRecords.empty()) {
    return false;
  }

  const bool completionWaitActive =
      q.queueLifecycle_.completionWaitActive();
  perf::countOpenCbTailPresentDrawAttachmentBoundaryCandidate(
      completionWaitActive);

  const auto* tailHot = lastDrawTailState(slot);
  if (!tailHot) {
    perf::countOpenCbTailPresentDrawAttachmentBoundaryNoDrawTail();
    return false;
  }
  if (render::drawAttachmentKeysMatch(*tailHot, nextHot)) {
    perf::countOpenCbTailPresentDrawAttachmentBoundarySame();
    return false;
  }

  perf::countOpenCbTailPresentDrawAttachmentBoundaryChanged();
  if (q.inflightCount_ + 2u > kMaxQueuedChunks) {
    perf::countOpenCbTailPresentDrawAttachmentBoundaryBlockedHeadroom();
    return false;
  }
  const bool published = maybePublishSemanticBoundaryChunkUnlocked(q, pool, lock);
  if (published) {
    perf::countOpenCbTailPresentDrawAttachmentBoundaryPublished(
        completionWaitActive);
  }
  return published;
}

bool maybePublishOpenCbDrawContinuationBoundaryChunkUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock,
    const core::FlatDrawStateRecord& nextHot) {
  if (!openCbDrawContinuationBoundaryPublishEnabled() || !q.writingSlot_) {
    return false;
  }

  auto& slot = currentSlotUnlocked(q);
  const auto* tailHot = lastDrawTailState(slot);
  const bool hasHeadroom = q.inflightCount_ + 2u <= kMaxQueuedChunks;
  if (!render::openCbShouldPublishDrawContinuationBoundary(
          slot.commandsEmpty(),
          !slot.presentRecords.empty(),
          tailHot != nullptr,
          tailHot && render::drawAttachmentKeysMatch(*tailHot, nextHot),
          hasHeadroom,
          slot.commandCount(),
          openCbDrawContinuationCommandLimit())) {
    return false;
  }

  const bool committed = q.queueLifecycle_.commitCurrentChunk(
      lock, kMaxQueuedChunks, [&q, &pool](core::ChunkSlot& publishSlot) {
        prepareSlotForPublish(q, pool, publishSlot,
                              perf::ChunkPublishReason::DrawContinuation);
      });
  if (!committed) {
    return false;
  }
  if (q.skipDrawResourceMarking_) {
    q.forceDrawResourceMarkingAfterSplit_ = true;
  }
  ensureWritingSlotUnlocked(q, lock);
  return true;
}

bool maybePublishOpenCbDrawSourceBoundaryChunkUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock,
    const core::FlatDrawStateRecord& nextHot) {
  if (maybePublishOpenCbDrawContinuationBoundaryChunkUnlocked(
          q, pool, lock, nextHot)) {
    return true;
  }
  return maybePublishOpenCbDrawAttachmentBoundaryChunkUnlocked(
      q, pool, lock, nextHot);
}

bool maybePublishOpenCbWriterActiveCpuReadySlotUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock,
    bool pendingCanReleaseAtSemanticBoundary,
    render::OpenCbSemanticBoundaryReleaseMode semanticBoundaryReleaseMode,
    bool completionWaitActive,
    bool writerActive) {
  if (!openCbWriterActiveCpuReadyPublishEnabled() || !q.writingSlot_) {
    return false;
  }
  auto& slot = currentSlotUnlocked(q);
  if (!render::openCbPendingShouldCpuReadyPublishWriterActiveSlot(
          /*readySlotsEmpty=*/q.readySlots_.empty(),
          pendingCanReleaseAtSemanticBoundary,
          semanticBoundaryReleaseMode,
          completionWaitActive,
          writerActive,
          slot.commandsEmpty(),
          !slot.presentRecords.empty())) {
    return false;
  }
  if (q.inflightCount_ + 2u > kMaxQueuedChunks) {
    return false;
  }
  return maybePublishSemanticBoundaryChunkUnlocked(q, pool, lock);
}

bool maybePublishOpenCbActiveWaitCpuReadySlotUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock,
    bool pendingCanReleaseAtSemanticBoundary,
    render::OpenCbSemanticBoundaryReleaseMode semanticBoundaryReleaseMode,
    bool completionWaitActive,
    bool semanticReleaseAlreadyUsedDuringWait,
    bool writerActive) {
  if (!openCbActiveWaitCpuReadyAppendEnabled() || !q.writingSlot_) {
    return false;
  }
  auto& slot = currentSlotUnlocked(q);
  if (!render::openCbPendingShouldCpuReadyPublishActiveWaitSlot(
          /*readySlotsEmpty=*/q.readySlots_.empty(),
          pendingCanReleaseAtSemanticBoundary,
          semanticBoundaryReleaseMode,
          completionWaitActive,
          semanticReleaseAlreadyUsedDuringWait,
          writerActive,
          slot.commandsEmpty(),
          !slot.presentRecords.empty())) {
    return false;
  }
  if (q.inflightCount_ + 2u > kMaxQueuedChunks) {
    return false;
  }
  return maybePublishSemanticBoundaryChunkUnlocked(q, pool, lock);
}

bool maybePublishOpenCbWaitStartCpuReadySlotUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock,
    bool hasPendingRecord,
    bool completionWaitActive,
    bool writerActive) {
  if (!openCbWaitStartCpuReadyPublishEnabled() || !q.writingSlot_) {
    return false;
  }
  auto& slot = currentSlotUnlocked(q);
  const bool readySlotsEmpty = q.readySlots_.empty();
  const bool writingSlotEmpty = slot.commandsEmpty();
  const bool writingSlotHasPresent = !slot.presentRecords.empty();
  if (!readySlotsEmpty ||
      hasPendingRecord ||
      !completionWaitActive ||
      q.stop_ ||
      !writerActive) {
    return false;
  }
  perf::countOpenCbTailPresentWaitStartPublishCandidate();
  if (!render::openCbShouldCpuReadyPublishWaitStartSlot(
          readySlotsEmpty,
          hasPendingRecord,
          completionWaitActive,
          q.stop_,
          writerActive,
          writingSlotEmpty,
          writingSlotHasPresent)) {
    if (writingSlotEmpty) {
      perf::countOpenCbTailPresentWaitStartPublishSlotEmpty();
    } else if (writingSlotHasPresent) {
      perf::countOpenCbTailPresentWaitStartPublishSlotPresent();
    }
    return false;
  }
  if (q.inflightCount_ + 2u > kMaxQueuedChunks) {
    perf::countOpenCbTailPresentWaitStartPublishBlockedHeadroom();
    return false;
  }
  if (!maybePublishSemanticBoundaryChunkUnlocked(q, pool, lock)) {
    return false;
  }
  perf::countOpenCbTailPresentWaitStartPublished();
  return true;
}

bool maybePublishOpenCbProducerActiveWaitCpuReadySlotUnlocked(
    CommandQueue& q,
    resources::Pool& pool,
    std::unique_lock<std::mutex>& lock) {
  if (!openCbWaitStartCpuReadyPublishEnabled() || !q.writingSlot_) {
    return false;
  }

  auto& slot = currentSlotUnlocked(q);
  const bool readySlotsEmpty = q.readySlots_.empty();
  const bool completionWaitActive = q.queueLifecycle_.completionWaitActive();
  const bool writingSlotEmpty = slot.commandsEmpty();
  const bool writingSlotHasPresent = !slot.presentRecords.empty();
  if (!readySlotsEmpty || !completionWaitActive || q.stop_) {
    return false;
  }

  perf::countOpenCbTailPresentWaitStartPublishCandidate();
  perf::countOpenCbTailPresentWaitStartProducerPublishCandidate();
  if (!render::openCbShouldCpuReadyPublishWaitStartSlot(
          readySlotsEmpty,
          /*hasPendingRecord=*/false,
          completionWaitActive,
          q.stop_,
          /*writerActive=*/true,
          writingSlotEmpty,
          writingSlotHasPresent)) {
    if (writingSlotEmpty) {
      perf::countOpenCbTailPresentWaitStartPublishSlotEmpty();
    } else if (writingSlotHasPresent) {
      perf::countOpenCbTailPresentWaitStartPublishSlotPresent();
    }
    return false;
  }
  if (q.inflightCount_ + 2u > kMaxQueuedChunks) {
    perf::countOpenCbTailPresentWaitStartPublishBlockedHeadroom();
    return false;
  }
  if (!maybePublishSemanticBoundaryChunkUnlocked(q, pool, lock)) {
    return false;
  }
  perf::countOpenCbTailPresentWaitStartPublished();
  perf::countOpenCbTailPresentWaitStartProducerPublished();
  return true;
}

bool firstOpenCbReadySourceCanAppendToPendingUnlocked(
    const CommandQueue& q,
    bool carryRenderSession,
    bool hasPendingSession) {
  if (q.readySlots_.empty()) {
    return false;
  }
  const std::size_t slotIndex = q.readySlots_.front();
  if (slotIndex >= q.slots_.size()) {
    return false;
  }
  const auto& slot = q.slots_[slotIndex];
  return render::slotCanAppendToOpenCbPending(
      slot,
      carryRenderSession,
      hasPendingSession,
      /*tailReadyForCurrentHead=*/false);
}

}  // namespace

void CommandQueue::setSkipDrawResourceMarking(bool skip) {
  std::unique_lock lock(mutex_);
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

  std::unique_lock lock(mutex_);
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

void CommandQueue::markChunkResources(std::span<const core::ChunkHandleEntry> entries) {
  if (entries.empty()) {
    return;
  }
  std::unique_lock lock(mutex_);
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

void CommandQueue::submitDrawRun(core::CanonicalDrawState state,
                                 const core::DrawUniformPayload& uniforms,
                                 std::span<const core::DrawParam> draws,
                                 std::span<const core::DrawParamPayloadView> payloads) {
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
  for (std::size_t i = 0; i < draws.size(); ++i) {
    perf::countSubmitDraw();
  }
  PerfScope scope(perf::countSubmitDrawCpuTime);
  std::unique_lock lock(mutex_);
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
    (void)maybePublishOpenCbDrawSourceBoundaryChunkUnlocked(
        *this, pool_, lock, state.hot);
  }
  {
    PerfScope stageScope(perf::countSubmitDrawRunResourceMarkCpuTime);
    if (!skipDrawResourceMarking_ || forceDrawResourceMarkingAfterSplit_) {
      const std::uint64_t seqId = seqIdForMark(*this, 0);
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
    if (!maybePublishOpenCbProducerActiveWaitCpuReadySlotUnlocked(*this, pool_, lock) &&
        !maybePublishOpenCbCpuReadyCommandLimitUnlocked(*this, pool_, lock) &&
        !maybeStagePrePresentChunkUnlocked(*this, pool_, lock)) {
      (void)maybeCommitDrawChunkUnlocked(*this, pool_, lock);
    }
  }
}

template <typename Submission>
void submitDrawRunBatchImpl(CommandQueue& queue,
                            resources::Pool& pool,
                            std::mutex& mutex,
                            core::Handle& currentBackBuffer,
                            bool skipDrawResourceMarking,
                            bool forceDrawResourceMarkingAfterSplit,
                            std::span<Submission> submissions) {
  if (submissions.empty()) {
    return;
  }
  auto& scratch = drawSubmitScratch();
  ScopedDrawSubmitScratchUse scratchUse(scratch);
  scratch.bindingSnapshots.reserve(submissions.size());
  DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE("submitDrawRunBatch");
  for (std::size_t i = 0; i < submissions.size(); ++i) {
    perf::countSubmitDraw();
  }
  countDrawSubmissionAdjacentStateGenerations(submissions);
  PerfScope scope(perf::countSubmitDrawCpuTime);
  std::unique_lock lock(mutex, std::defer_lock);
  {
    PerfScope stageScope(perf::countSubmitDrawRunBatchQueueLockCpuTime);
    lock.lock();
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
      const bool sourceBoundaryPublished =
          maybePublishOpenCbDrawSourceBoundaryChunkUnlocked(
              queue, pool, lock, batch.front().materializedState().hot);
      forceDrawResourceMarkingAfterSplit =
          forceDrawResourceMarkingAfterSplit ||
          (sourceBoundaryPublished && skipDrawResourceMarking);
    }
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchResourceMarkCpuTime);
      if (!skipDrawResourceMarking || forceDrawResourceMarkingAfterSplit) {
        const std::uint64_t seqId = seqIdForMark(queue, 0);
        pool.markDrawResources(batch.front().materializedState().hot, seqId);
        for (auto& submission : batch) {
          std::span<const core::DrawParamPayloadView> payloads{};
          if (!submission.payload.userVertexData.empty() ||
              !submission.payload.userIndexData.empty() ||
              !submission.payload.bindingOverrideData.empty() ||
              !submission.payload.bindingSnapshotData.empty()) {
            payloads = std::span<const core::DrawParamPayloadView>(&submission.payload, 1);
          }
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
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchChunkCommitCpuTime);
      const bool cpuReadyPublished =
          maybePublishOpenCbProducerActiveWaitCpuReadySlotUnlocked(queue, pool, lock) ||
          maybePublishOpenCbCpuReadyCommandLimitUnlocked(queue, pool, lock);
      if (cpuReadyPublished) {
        forceDrawResourceMarkingAfterSplit =
            forceDrawResourceMarkingAfterSplit || skipDrawResourceMarking;
      } else if (!maybeStagePrePresentChunkUnlocked(queue, pool, lock)) {
        const bool committed = maybeCommitDrawChunkUnlocked(queue, pool, lock);
        forceDrawResourceMarkingAfterSplit =
            forceDrawResourceMarkingAfterSplit ||
            (committed && skipDrawResourceMarking);
      } else {
        forceDrawResourceMarkingAfterSplit =
            forceDrawResourceMarkingAfterSplit || skipDrawResourceMarking;
      }
    }
    batchStart = batchEnd;
  }
}

template <typename Submission>
void submitDrawRunBatchAndRunImpl(
    CommandQueue& queue,
    resources::Pool& pool,
    std::mutex& mutex,
    core::Handle& currentBackBuffer,
    bool skipDrawResourceMarking,
    bool forceDrawResourceMarkingAfterSplit,
    std::span<Submission> submissions,
    core::CanonicalDrawState state,
    const core::DrawUniformPayload& uniforms,
    std::span<const core::DrawParam> draws,
    std::span<const core::DrawParamPayloadView> payloads) {
  if (submissions.empty()) {
    queue.submitDrawRun(std::move(state), uniforms, draws, payloads);
    return;
  }
  if (draws.empty()) {
    submitDrawRunBatchImpl(queue, pool, mutex, currentBackBuffer,
                           skipDrawResourceMarking,
                           forceDrawResourceMarkingAfterSplit,
                           submissions);
    return;
  }

  auto& scratch = drawSubmitScratch();
  ScopedDrawSubmitScratchUse scratchUse(scratch);
  scratch.bindingSnapshots.reserve(
      std::max(submissions.size(), draws.size()));
  scratch.snapshotPayloads.reserve(draws.size());
  DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE("submitDrawRunBatchAndRun");
  for (std::size_t i = 0; i < submissions.size() + draws.size(); ++i) {
    perf::countSubmitDraw();
  }
  countDrawSubmissionAdjacentStateGenerations(submissions);
  PerfScope scope(perf::countSubmitDrawCpuTime);
  std::unique_lock lock(mutex, std::defer_lock);
  {
    PerfScope stageScope(perf::countSubmitDrawRunBatchQueueLockCpuTime);
    lock.lock();
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
      const bool sourceBoundaryPublished =
          maybePublishOpenCbDrawSourceBoundaryChunkUnlocked(
              queue, pool, lock, batch.front().materializedState().hot);
      forceDrawResourceMarkingAfterSplit =
          forceDrawResourceMarkingAfterSplit ||
          (sourceBoundaryPublished && skipDrawResourceMarking);
    }
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchResourceMarkCpuTime);
      if (!skipDrawResourceMarking || forceDrawResourceMarkingAfterSplit) {
        const std::uint64_t seqId = seqIdForMark(queue, 0);
        pool.markDrawResources(batch.front().materializedState().hot, seqId);
        for (auto& submission : batch) {
          std::span<const core::DrawParamPayloadView> submissionPayloads{};
          if (!submission.payload.userVertexData.empty() ||
              !submission.payload.userIndexData.empty() ||
              !submission.payload.bindingOverrideData.empty() ||
              !submission.payload.bindingSnapshotData.empty()) {
            submissionPayloads =
                std::span<const core::DrawParamPayloadView>(&submission.payload, 1);
          }
          markDrawBindingOverrideResources(pool, submissionPayloads, seqId);
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
    {
      PerfScope stageScope(perf::countSubmitDrawRunBatchChunkCommitCpuTime);
      if (maybePublishOpenCbProducerActiveWaitCpuReadySlotUnlocked(queue, pool, lock) ||
          maybePublishOpenCbCpuReadyCommandLimitUnlocked(queue, pool, lock)) {
        forceDrawResourceMarkingAfterSplit =
            forceDrawResourceMarkingAfterSplit || skipDrawResourceMarking;
      }
    }
    batchStart = batchEnd;
  }

  std::span<const core::DrawParamPayloadView> effectivePayloads{};
  {
    PerfScope stageScope(perf::countSubmitDrawRunBindingSnapshotCpuTime);
    effectivePayloads =
        snapshotDrawRunBindingPayloads(pool,
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
    ensureWritingSlotUnlocked(queue, lock);
    maybeCommitDrawPayloadArenaUnlocked(queue, pool, lock, pendingPayloadBytes);
    const bool sourceBoundaryPublished =
        maybePublishOpenCbDrawSourceBoundaryChunkUnlocked(
            queue, pool, lock, state.hot);
    forceDrawResourceMarkingAfterSplit =
        forceDrawResourceMarkingAfterSplit ||
        (sourceBoundaryPublished && skipDrawResourceMarking);
  }
  {
    PerfScope stageScope(perf::countSubmitDrawRunResourceMarkCpuTime);
    if (!skipDrawResourceMarking || forceDrawResourceMarkingAfterSplit) {
      const std::uint64_t seqId = seqIdForMark(queue, 0);
      pool.markDrawResources(state.hot, seqId);
      markDrawBindingOverrideResources(pool, effectivePayloads, seqId);
    }
  }
  currentBackBuffer = state.hot.colorAttachments[0].handle;
  {
    PerfScope stageScope(perf::countSubmitDrawRunAppendCpuTime);
    noteCurrentSlotCommandAppendStartedUnlocked(queue);
    currentSlotUnlocked(queue).appendDrawRun(
        std::move(state), uniforms, draws, effectivePayloads);
  }
  {
    PerfScope stageScope(perf::countSubmitDrawRunChunkCommitCpuTime);
    if (!maybePublishOpenCbProducerActiveWaitCpuReadySlotUnlocked(queue, pool, lock) &&
        !maybePublishOpenCbCpuReadyCommandLimitUnlocked(queue, pool, lock) &&
        !maybeStagePrePresentChunkUnlocked(queue, pool, lock)) {
      (void)maybeCommitDrawChunkUnlocked(queue, pool, lock);
    }
  }
}

void CommandQueue::submitDrawRunBatch(
    std::span<core::DrawRunSubmission> submissions) {
  submitDrawRunBatchImpl(*this, pool_, mutex_, currentBackBuffer_,
                         skipDrawResourceMarking_,
                         forceDrawResourceMarkingAfterSplit_,
                         submissions);
}

void CommandQueue::submitCompactDrawRunBatch(
    std::span<core::DrawRunCompactSubmission> submissions) {
  submitDrawRunBatchImpl(*this, pool_, mutex_, currentBackBuffer_,
                         skipDrawResourceMarking_,
                         forceDrawResourceMarkingAfterSplit_,
                         submissions);
}

void CommandQueue::submitDrawRunBatchWithResourceMarking(
    std::span<core::DrawRunSubmission> submissions) {
  submitDrawRunBatchImpl(*this, pool_, mutex_, currentBackBuffer_,
                         /*skipDrawResourceMarking=*/false,
                         /*forceDrawResourceMarkingAfterSplit=*/false,
                         submissions);
}

void CommandQueue::submitCompactDrawRunBatchWithResourceMarking(
    std::span<core::DrawRunCompactSubmission> submissions) {
  submitDrawRunBatchImpl(*this, pool_, mutex_, currentBackBuffer_,
                         /*skipDrawResourceMarking=*/false,
                         /*forceDrawResourceMarkingAfterSplit=*/false,
                         submissions);
}

void CommandQueue::submitDrawRunBatchAndRun(
    std::span<core::DrawRunSubmission> submissions,
    core::CanonicalDrawState state,
    const core::DrawUniformPayload& uniforms,
    std::span<const core::DrawParam> draws,
    std::span<const core::DrawParamPayloadView> payloads) {
  submitDrawRunBatchAndRunImpl(*this, pool_, mutex_, currentBackBuffer_,
                               skipDrawResourceMarking_,
                               forceDrawResourceMarkingAfterSplit_,
                               submissions, std::move(state), uniforms,
                               draws, payloads);
}

void CommandQueue::submitCompactDrawRunBatchAndRun(
    std::span<core::DrawRunCompactSubmission> submissions,
    core::CanonicalDrawState state,
    const core::DrawUniformPayload& uniforms,
    std::span<const core::DrawParam> draws,
    std::span<const core::DrawParamPayloadView> payloads) {
  submitDrawRunBatchAndRunImpl(*this, pool_, mutex_, currentBackBuffer_,
                               skipDrawResourceMarking_,
                               forceDrawResourceMarkingAfterSplit_,
                               submissions, std::move(state), uniforms,
                               draws, payloads);
}

void CommandQueue::submitDrawRunBatchAndRunWithResourceMarking(
    std::span<core::DrawRunSubmission> submissions,
    core::CanonicalDrawState state,
    const core::DrawUniformPayload& uniforms,
    std::span<const core::DrawParam> draws,
    std::span<const core::DrawParamPayloadView> payloads) {
  submitDrawRunBatchAndRunImpl(*this, pool_, mutex_, currentBackBuffer_,
                               /*skipDrawResourceMarking=*/false,
                               /*forceDrawResourceMarkingAfterSplit=*/false,
                               submissions, std::move(state), uniforms,
                               draws, payloads);
}

void CommandQueue::submitCompactDrawRunBatchAndRunWithResourceMarking(
    std::span<core::DrawRunCompactSubmission> submissions,
    core::CanonicalDrawState state,
    const core::DrawUniformPayload& uniforms,
    std::span<const core::DrawParam> draws,
    std::span<const core::DrawParamPayloadView> payloads) {
  submitDrawRunBatchAndRunImpl(*this, pool_, mutex_, currentBackBuffer_,
                               /*skipDrawResourceMarking=*/false,
                               /*forceDrawResourceMarkingAfterSplit=*/false,
                               submissions, std::move(state), uniforms,
                               draws, payloads);
}

void CommandQueue::submitClear(const core::ClearDesc& desc) {
  perf::countSubmitClear();
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  maybePublishSemanticBoundaryChunkUnlocked(*this, pool_, lock);
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
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  maybePublishSemanticBoundaryChunkUnlocked(*this, pool_, lock);
  traceTextureSurfaceOp(pool_, "SurfaceCopy", desc.source, desc.destination);
  noteCurrentSlotCommandAppendStartedUnlocked(*this);
  currentSlotUnlocked(*this).appendSurfaceCopy(desc);
  currentBackBuffer_ = desc.destination;
  pool_.markSurfaceCopyResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitStretchRect(const core::StretchRectDesc& desc) {
  perf::countSubmitStretch();
  std::unique_lock lock(mutex_);
  if (splitStretchChunk()) {
    queueLifecycle_.commitCurrentChunk(
        lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot,
                                perf::ChunkPublishReason::StretchSplit);
        });
  }
  ensureWritingSlotUnlocked(*this, lock);
  maybePublishSemanticBoundaryChunkUnlocked(*this, pool_, lock);
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
  std::lock_guard lock(mutex_);
  // Readback is satisfied synchronously in CommandQueue::readbackSurface.
  // Still mark resources so NoUseAfterFree remains meaningful.
  traceTextureSurfaceOp(pool_, "Readback", desc.source, desc.destination);
  pool_.markReadbackResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitColorFill(const core::ColorFillDesc& desc) {
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  maybePublishSemanticBoundaryChunkUnlocked(*this, pool_, lock);
  traceTextureSurfaceOp(pool_, "ColorFill", desc.destination);
  noteCurrentSlotCommandAppendStartedUnlocked(*this);
  currentSlotUnlocked(*this).appendColorFill(desc);
  currentBackBuffer_ = desc.destination;
  pool_.markColorFillResources(desc, seqIdForMark(*this, 0));
}

void CommandQueue::submitDepthResolve(const core::DepthResolveDesc& desc) {
  // R-FORMAT-11 — RESZ MSAA depth resolve. Fire-and-forget surface op:
  // append the command + mark both endpoints, mirroring submitColorFill.
  // The destination is the INTZ depth texture, not the present back buffer,
  // so currentBackBuffer_ is left untouched.
  std::unique_lock lock(mutex_);
  ensureWritingSlotUnlocked(*this, lock);
  maybePublishSemanticBoundaryChunkUnlocked(*this, pool_, lock);
  traceTextureSurfaceOp(pool_, "DepthResolve", desc.msaaDepth, desc.intzDest);
  noteCurrentSlotCommandAppendStartedUnlocked(*this);
  currentSlotUnlocked(*this).appendDepthResolve(desc);
  pool_.markDepthResolveResources(desc, seqIdForMark(*this, 0));
}

std::uint64_t CommandQueue::submitPresent(const core::SwapDesc& desc) {
  // TLA+: PresentFrameLatency / CommitPresent.
  perf::countSubmitPresent();
  PerfScope scope(perf::countSubmitPresentCpuTime);
  const BoundaryPolicy boundaryPolicy = resolveBoundaryPolicyFromEnv();
  if (!offloadCommitReplayEnabled() && boundaryPolicy == BoundaryPolicy::DeferredPresentCompletion) {
    PerfScope stageScope(perf::countSubmitPresentBoundaryCpuTime);
    drainDeferredPresentBoundary();
  }
  core::SwapDesc queuedDesc = desc;
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
        stashDrawableToken(queuedDesc.presentId, std::move(token));
        break;
      }
      case AcquirePolicy::SyncOnSubmit: {
        {
          std::unique_lock lock(mutex_);
          queueLifecycle_.commitCurrentChunk(
              lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
                prepareSlotForPublish(*this, pool_, slot,
                                      perf::ChunkPublishReason::PresentAcquire);
              });
        }
        auto token = presenter->acquireDrawable(makePresentAcquireParams(queuedDesc));
        queuedDesc.drawableTokenRequired = true;
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

  std::uint64_t presentSeqId = 0;
  {
    PerfScope stageScope(perf::countSubmitPresentCommitCpuTime);
    std::unique_lock lock(mutex_);
    const core::Handle sourceHandle =
        core::metalqueue::selectPresentSourceHandle(queuedDesc, currentBackBuffer_);
    perf::countPresentSourceSelection(static_cast<bool>(queuedDesc.sourceSurface),
                                      sourceHandle.value != 0 &&
                                          sourceHandle.value == currentBackBuffer_.value);
    const bool hasStagedPrePresent = !stagedTailPresentSlots_.empty();
    const bool hasCurrentPrePresentWork =
        writingSlot_.has_value() &&
        !currentSlotUnlocked(*this).commandsEmpty();
    const bool shouldSplitOpenCbPresentTail =
        render::openCbPresentTailNeedsPrePresentSplit(
            openCbPreencodeTailPresentEnabled(),
            hasCurrentPrePresentWork);
    const bool shouldUseTailPresentStaging =
        hasStagedPrePresent ||
        shouldSplitOpenCbPresentTail ||
        (stageTailPresentChunkEnabled() && hasCurrentPrePresentWork);
    if (shouldUseTailPresentStaging) {
      if (writingSlot_.has_value() &&
          !currentSlotUnlocked(*this).commandsEmpty()) {
        (void)commitAndStageCurrentPrePresentSlotUnlocked(
            *this, pool_, lock, /*postCommitHeadroom=*/1u);
      }
      ensureWritingSlotUnlocked(*this, lock);
      queueLifecycle_.appendPresentCommand(queuedDesc, sourceHandle);
      const bool tailCommitted = queueLifecycle_.commitCurrentChunk(
          lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
            prepareSlotForPublish(*this, pool_, slot,
                                  perf::ChunkPublishReason::Present);
          });
      if (tailCommitted) {
        presentSeqId = lastCommittedSeqId_;
      }
      if (tailCommitted && !readySlots_.empty()) {
        (void)queueLifecycle_.releaseStagedSlotsBeforeReadyTail(
            lock, stagedTailPresentSlots_, readySlots_.back());
      }
    } else if (splitPresentChunk()) {
      queueLifecycle_.commitCurrentChunk(
          lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
            prepareSlotForPublish(
                *this, pool_, slot,
                perf::ChunkPublishReason::PresentSplitBefore);
          });
      ensureWritingSlotUnlocked(*this, lock);
      queueLifecycle_.appendPresentCommand(queuedDesc, sourceHandle);
      queueLifecycle_.commitCurrentChunk(
          lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
            prepareSlotForPublish(*this, pool_, slot,
                                  perf::ChunkPublishReason::Present);
          });
      presentSeqId = lastCommittedSeqId_;
    } else {
      queueLifecycle_.presentAndCommit(
          lock, kMaxQueuedChunks, queuedDesc, sourceHandle,
          [this](core::ChunkSlot& slot) {
            prepareSlotForPublish(*this, pool_, slot,
                                  perf::ChunkPublishReason::Present);
          });
      presentSeqId = lastCommittedSeqId_;
    }
  }

  if (offloadCommitReplayEnabled()) {
    // The commit-replay offload path paces itself through
    // dxmt9::Device::waitPresentOrdinalBoundary from the PE side; skip the
    // inline seqId-based boundary here so the two mechanisms don't
    // double-wait on the same present token.
    perf::countPresentBoundarySkipped();
  } else if (boundaryPolicy == BoundaryPolicy::DeferredPresentCompletion) {
    perf::countPresentBoundaryApplied();
    deferPresentBoundary(presentSeqId, presentBoundaryLatency(queuedDesc));
  } else if (shouldApplyPresentBoundary(queuedDesc)) {
    PerfScope stageScope(perf::countSubmitPresentBoundaryCpuTime);
    perf::countPresentBoundaryApplied();
    presentBoundary(presentSeqId, presentBoundaryLatency(queuedDesc));
  } else {
    perf::countPresentBoundarySkipped();
  }
  return presentSeqId;
}

void CommandQueue::presentBoundary(std::uint64_t presentSeqId, std::uint32_t maxFrameLatency) {
  // TLA+: PresentFrameLatency / BeginPresentWait + CommitPendingPresent.
  const std::uint64_t targetSeqId =
      presentBoundaryTargetSeqId(presentSeqId, maxFrameLatency);
  if (targetSeqId == 0) {
    return;
  }
  std::unique_lock lock(mutex_);
  // R-BACK / PresentFrameLatency — branch on the unified
  // BoundaryPolicy resolved once at process init. Disabled is
  // filtered earlier by shouldApplyPresentBoundary and never reaches
  // here; AfterAcquire shares the Default wait branch (the position
  // of notePresentDequeued is the only observable difference).
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
  std::lock_guard lock(mutex_);
  deferredPresentBoundaryTargetSeqId_ =
      std::max(deferredPresentBoundaryTargetSeqId_, targetSeqId);
  perf::countPresentBoundaryDeferred();
}

void CommandQueue::drainDeferredPresentBoundary() {
  std::unique_lock lock(mutex_);
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
void CommandQueue::waitPresentOrdinalBoundary(std::uint64_t presentOrdinal,
                                              std::uint32_t maxFrameLatency) {
  const BoundaryPolicy policy = resolveBoundaryPolicyFromEnv();
  std::unique_lock lock(mutex_);
  const PresentOrdinalWaitPlan plan = planPresentOrdinalWait(
      policy, presentOrdinal, maxFrameLatency, presentOrdinalGate_.deferredTarget);
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
  std::lock_guard lock(mutex_);
  presentOrdinalGate_.aborted = true;
  presentCompletedCv_.notify_all();
}

void CommandQueue::notePresentDequeued(std::uint64_t seqId) {
  std::lock_guard lock(mutex_);
  presentDequeuedSeqId_ = std::max(presentDequeuedSeqId_, seqId);
  presentDequeuedCv_.notify_all();
}

std::optional<core::metalcapture::MetalCaptureRequest>
CommandQueue::metalCaptureForPresentChunk(std::uint64_t seqId) {
  return metalCapture_.maybeCapturePresentChunk(seqId);
}

std::optional<core::metalcapture::MetalCaptureRequest>
CommandQueue::metalCaptureForChunkBegin(std::uint64_t seqId) {
  return metalCapture_.maybeCaptureAtChunkBegin(seqId);
}

std::optional<core::metalcapture::MetalCaptureRequest>
CommandQueue::notePresentChunkForCapture(std::uint64_t seqId) {
  return metalCapture_.maybePresentChunkClosesSession(seqId);
}

void CommandQueue::submitFlush() {
  perf::countSubmitFlush();
  std::unique_lock lock(mutex_);
  queueLifecycle_.flushAndWait(
      lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
        prepareSlotForPublish(*this, pool_, slot,
                              perf::ChunkPublishReason::Flush);
      });
}

core::HResult CommandQueue::waitForVBlank() {
  submitFlush();
  return core::HResult{0};
}

void* CommandQueue::mapBuffer(core::BufferHandle handle, std::uint32_t flags) {
  // Pool storage + queue's wait-for-sequence rule under one mutex.
  const auto totalStart = std::chrono::steady_clock::now();
  std::unique_lock lock(mutex_);
  const auto lockAcquired = std::chrono::steady_clock::now();
  const std::uint64_t waitSeq = pool_.mapWaitSeqId(handle, flags);
  const bool hasWaitSeq = waitSeq != 0;
  const auto waitStart = std::chrono::steady_clock::now();
  // Wine writeonly_vertex_buffer_readback_policy (#66): a Draw followed
  // by a Lock without an intervening Present must still observe the
  // draw on read. The drawn slot's seqId is set as soon as the draw is
  // appended to the current chunk, but the chunk hasn't been committed
  // to Metal yet — without committing, completedSeqId_ can never reach
  // waitSeq and the wait below would block forever. Drive the pending
  // chunk into the submit pipeline first.
  if (waitSeq > lastCommittedSeqId_) {
    queueLifecycle_.commitCurrentChunk(
        lock, kMaxQueuedChunks, [this](core::ChunkSlot& slot) {
          prepareSlotForPublish(*this, pool_, slot,
                                perf::ChunkPublishReason::MapWait);
        });
  }
  if (waitSeq > completedSeqId_) {
    queueLifecycle_.waitForSequence(lock, waitSeq);
  }
  const auto waitEnd = std::chrono::steady_clock::now();
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
  return pool_.finalizeBufferMap(device_, handle, flags, completedSeqId_);
}

bool CommandQueue::readbackSurface(const core::ReadbackDesc& desc, core::ReadbackPixels& pixels) {
  return encoders::readbackSurface(*this, pool_, device_, limits_, desc, pixels);
}

void CommandQueue::runEncodeLoop(EncodeChunkFn encodeChunk, OnSubmittedFn onSubmitted) {
  while (true) {
    std::unique_lock lock(mutex_);
    if (!queueLifecycle_.runEncodeIteration(lock, encodeChunk, onSubmitted)) {
      return;
    }
  }
}

void CommandQueue::runOpenCbTailPresentEncodeLoop(OnSubmittedFn onSubmitted) {
  using core::metalqueue::EncodeSessionSourceList;
  using core::metalqueue::QueueCompletionSource;
  using core::metalqueue::QueueSubmissionRecord;
  using core::metalqueue::ReadySlotSnapshot;

  std::array<ReadySlotSnapshot, kCommandChunkCount> scratch{};
  std::optional<QueueSubmissionRecord> pendingRecord;
  EncodeSessionSourceList pendingSources;
  encoders::EncodeChunkSession pendingSession;
  bool pendingCanReleaseAtSemanticBoundary = false;
  bool semanticBoundaryReleaseUsedDuringWait = false;
  const bool carryRenderSession = openCbRenderSessionCarryEnabled();
  const auto semanticBoundaryReleaseMode =
      openCbSemanticBoundaryReleaseMode();
  const auto pendingTailWaitTimeout = openCbPendingTailWaitTimeout();
  const bool pendingTailTimeoutEnabled =
      pendingTailWaitTimeout.count() > 0;
  // Positive timeout remains a diagnostic release point. The production-shaped
  // path starts a pre-Present head under queue-local state and releases it on a
  // ready source, stop, or inactive writer rather than on wallclock time.
  const bool allowPendingHeadWithoutReadyTail = carryRenderSession;

  auto encodeSource = [this](const ReadySlotSnapshot& source,
                             encoders::EncodeChunkOptions options) {
    DXMT_ASSERT(source.slot != nullptr);
    auto& slot = *source.slot;
    traceEncodeFnStage("entry", source.slotIndex, slot);
    if (encodeSlotPsoPrefetchEnabled() &&
        !slot.prefetchedPipelinesSealed()) {
      traceEncodeFnStage("before-pso-prefetch", source.slotIndex, slot);
      PerfScope scope(perf::countEncodeSlotPsoPrefetchCpuTime);
      prefetchSlotPipelines(slot);
      traceEncodeFnStage("after-pso-prefetch", source.slotIndex, slot);
    }
    traceEncodeFnStage("before-make-context", source.slotIndex, slot);
    auto ctx = makeEncodeContext();
    traceEncodeFnStage("after-make-context", source.slotIndex, slot);
    traceEncodeFnStage("before-backend-onChunkReady", source.slotIndex, slot);
    auto submission = backend_->onChunkReady(ctx, source.slotIndex, slot,
                                             std::move(options));
    traceEncodeFnStage(submission.has_value()
                           ? "after-backend-onChunkReady-submission"
                           : "after-backend-onChunkReady-inline",
                       source.slotIndex,
                       slot);
    if (submission.has_value() && !submission->commandBuffer) {
      submission.reset();
    }
    return submission;
  };

  auto completeInlineSnapshot = [this, &onSubmitted](
                                    const ReadySlotSnapshot& source) {
    DXMT_ASSERT(source.slot != nullptr);
    const std::uint64_t seqId = source.seqId;
    queueLifecycle_.completeInlineChunk(source.slotIndex, seqId);
    if (onSubmitted) {
      onSubmitted(seqId);
    }
  };

  auto retainSource = [this](std::unique_lock<std::mutex>& lock,
                             const ReadySlotSnapshot& source,
                             QueueCompletionSource& retained) {
    std::array<QueueCompletionSource, 1> out{};
    const std::span<const ReadySlotSnapshot> sources(&source, 1u);
    const std::size_t count =
        queueLifecycle_.retainEncodedSourcesForPendingTail(
            lock, sources, std::span<QueueCompletionSource>(out));
    if (count != 1u) {
      return false;
    }
    retained = out.front();
    return true;
  };

  auto submitRecordLocked = [this](std::unique_lock<std::mutex>& lock,
                                   QueueSubmissionRecord& record) {
    auto callbacks = std::move(record.postCommitCallbacks);
    queueLifecycle_.submitEncodedSubmission(lock, record);
    lock.unlock();
    for (auto& callback : callbacks) {
      if (callback) {
        callback();
      }
    }
  };

  auto finalizePendingSessionForSubmitLocked =
      [this, &pendingRecord, &pendingSession](
          std::unique_lock<std::mutex>& lock) {
        if (!pendingSession) {
          return true;
        }
        if (!encoders::encodeChunkSessionHasDeferredSubmissionPayload(
                *pendingSession)) {
          pendingSession.reset();
          return true;
        }
        if (!pendingRecord.has_value()) {
          return false;
        }

        lock.unlock();
        auto ctx = makeEncodeContext();
        const bool finalized =
            encoders::finalizeEncodeChunkSessionIntoSubmission(
                ctx, *pendingSession, *pendingRecord);
        lock.lock();
        if (finalized) {
          if (!encoders::retainEncodeChunkSessionUntilSubmissionComplete(
                  std::move(pendingSession), *pendingRecord)) {
            return false;
          }
        }
        return finalized;
      };

  auto submitPendingRecordLocked =
      [&pendingRecord, &pendingSources, &pendingSession,
       &pendingCanReleaseAtSemanticBoundary,
       &finalizePendingSessionForSubmitLocked, &submitRecordLocked](
          std::unique_lock<std::mutex>& lock) {
        if (!pendingRecord.has_value()) {
          return false;
        }
        if (!finalizePendingSessionForSubmitLocked(lock)) {
          return false;
        }
        submitRecordLocked(lock, *pendingRecord);
        pendingRecord.reset();
        pendingSources.clear();
        pendingSession.reset();
        pendingCanReleaseAtSemanticBoundary = false;
        return true;
      };

  auto countCompletionWaitPendingState =
      [&pendingRecord, &pendingSession,
       &pendingCanReleaseAtSemanticBoundary,
       &semanticBoundaryReleaseUsedDuringWait](
          bool completionWaitActive,
          bool readySlotsEmpty) {
        if (!completionWaitActive || !pendingRecord.has_value()) {
          return;
        }
        perf::countOpenCbTailPresentCompletionWaitPendingState(
            pendingCanReleaseAtSemanticBoundary,
            semanticBoundaryReleaseUsedDuringWait,
            pendingSession &&
                encoders::encodeChunkSessionHasActiveRender(*pendingSession),
            readySlotsEmpty);
      };

  while (true) {
    std::unique_lock lock(mutex_);
    {
      const bool completionWaitActive =
          queueLifecycle_.completionWaitActive();
      const bool writerActive = writingSlot_.has_value();
      if (maybePublishOpenCbWaitStartCpuReadySlotUnlocked(
              *this, pool_, lock, pendingRecord.has_value(),
              completionWaitActive, writerActive)) {
        continue;
      }
      if (semanticBoundaryReleaseMode ==
              render::OpenCbSemanticBoundaryReleaseMode::CompletionWait &&
          !completionWaitActive) {
        semanticBoundaryReleaseUsedDuringWait = false;
      }
    }

    if (pendingRecord.has_value() && !readySlots_.empty()) {
      if (render::openCbPendingShouldSubmitForProducerSequenceWait(
              pendingRecord.has_value(),
              queueLifecycle_.producerSequenceWaitActive())) {
        if (submitPendingRecordLocked(lock)) {
          if (stop_) {
            return;
          }
          continue;
        }
        perf::countOpenCbTailPresentPendingAbandonedNoReady();
        abortOpenCbPendingFailOpen("producer sequence wait release before ready");
      }
      const bool completionWaitActive = queueLifecycle_.completionWaitActive();
      countCompletionWaitPendingState(
          completionWaitActive, /*readySlotsEmpty=*/false);
      if (render::openCbPendingReadySourceBlocksSemanticReleaseNoCompletionWait(
              /*readySlotsEmpty=*/false,
              pendingCanReleaseAtSemanticBoundary,
              semanticBoundaryReleaseMode,
              completionWaitActive)) {
        perf::countOpenCbTailPresentSemanticReleaseBlockedReadySourceNoCompletionWait();
      }
      if (pendingCanReleaseAtSemanticBoundary &&
          (completionWaitActive ||
           semanticBoundaryReleaseMode ==
               render::OpenCbSemanticBoundaryReleaseMode::Deterministic)) {
        perf::countOpenCbTailPresentSemanticReleaseCandidate();
        if (semanticBoundaryReleaseMode ==
                render::OpenCbSemanticBoundaryReleaseMode::CompletionWait &&
            semanticBoundaryReleaseUsedDuringWait) {
          perf::countOpenCbTailPresentSemanticReleaseBlockedAlreadyUsed();
        }
      }
      const bool appendReadyBeforeRelease =
          openCbActiveWaitCpuReadyAppendEnabled() &&
          render::openCbPendingShouldAppendReadySourceBeforeSemanticRelease(
              /*readySlotsEmpty=*/false,
              pendingCanReleaseAtSemanticBoundary,
              semanticBoundaryReleaseMode,
              completionWaitActive,
              semanticBoundaryReleaseUsedDuringWait,
              firstOpenCbReadySourceCanAppendToPendingUnlocked(
                  *this, carryRenderSession,
                  static_cast<bool>(pendingSession)));
      if (!appendReadyBeforeRelease &&
          render::openCbPendingShouldReleaseBeforeReadySource(
              /*readySlotsEmpty=*/false,
              pendingCanReleaseAtSemanticBoundary,
              semanticBoundaryReleaseMode,
              completionWaitActive,
              semanticBoundaryReleaseUsedDuringWait)) {
        if (submitPendingRecordLocked(lock)) {
          if (semanticBoundaryReleaseMode ==
              render::OpenCbSemanticBoundaryReleaseMode::CompletionWait) {
            semanticBoundaryReleaseUsedDuringWait = true;
          }
          perf::countOpenCbTailPresentSemanticReleaseSubmitted();
          if (stop_) {
            return;
          }
          continue;
        }
        perf::countOpenCbTailPresentSemanticReleaseFailed();
        perf::countOpenCbTailPresentPendingAbandonedNoReady();
        abortOpenCbPendingFailOpen("semantic boundary release before ready");
      }
    }

    if (pendingRecord.has_value() && readySlots_.empty()) {
      if (render::openCbPendingShouldSubmitForProducerSequenceWait(
              pendingRecord.has_value(),
              queueLifecycle_.producerSequenceWaitActive())) {
        if (submitPendingRecordLocked(lock)) {
          if (stop_) {
            return;
          }
          continue;
        }
        perf::countOpenCbTailPresentPendingAbandonedNoReady();
        abortOpenCbPendingFailOpen("producer sequence wait release");
      }
      const bool completionWaitActive = queueLifecycle_.completionWaitActive();
      const bool writerActive = writingSlot_.has_value();
      countCompletionWaitPendingState(
          completionWaitActive, /*readySlotsEmpty=*/true);
      if (pendingCanReleaseAtSemanticBoundary) {
        perf::countOpenCbTailPresentSemanticReleaseCandidate();
        const auto noWaitBlock =
            render::classifyOpenCbPendingSemanticReleaseNoCompletionWaitBlock(
                /*readySlotsEmpty=*/true,
                pendingCanReleaseAtSemanticBoundary,
                semanticBoundaryReleaseMode,
                completionWaitActive,
                writerActive);
        switch (noWaitBlock) {
          case render::OpenCbSemanticReleaseNoCompletionWaitBlock::None:
            if (semanticBoundaryReleaseMode ==
                    render::OpenCbSemanticBoundaryReleaseMode::CompletionWait &&
                completionWaitActive &&
                semanticBoundaryReleaseUsedDuringWait) {
              perf::countOpenCbTailPresentSemanticReleaseBlockedAlreadyUsed();
            }
            break;
          case render::OpenCbSemanticReleaseNoCompletionWaitBlock::WriterActive:
            perf::countOpenCbTailPresentSemanticReleaseBlockedNoCompletionWait();
            perf::countOpenCbTailPresentSemanticReleaseBlockedNoCompletionWaitWriterActive();
            {
              const auto& writerSlot = currentSlotUnlocked(*this);
              const auto writerSlotState =
                  render::classifyOpenCbPendingSemanticReleaseWriterActiveSlotState(
                      noWaitBlock,
                      writerSlot.commandsEmpty(),
                      !writerSlot.presentRecords.empty());
              switch (writerSlotState) {
              case render::OpenCbSemanticReleaseWriterActiveSlotState::None:
                break;
              case render::OpenCbSemanticReleaseWriterActiveSlotState::Empty:
                perf::countOpenCbTailPresentSemanticReleaseBlockedNoCompletionWaitWriterActiveSlotEmpty();
                break;
              case render::OpenCbSemanticReleaseWriterActiveSlotState::NonPresentWork:
                perf::countOpenCbTailPresentSemanticReleaseBlockedNoCompletionWaitWriterActiveSlotNonPresent();
                {
                  const auto shape =
                      core::metalqueue::summarizeNoEnqueueFirstPublishSlotShape(
                          writerSlot);
                  perf::countOpenCbTailPresentSemanticReleaseWriterActiveNonPresentSlotShape(
                      shape.commandCount,
                      shape.drawRunCommands,
                      shape.drawItems,
                      shape.nonDrawCommands,
                      shape.payloadBytes);
                  if (maybePublishOpenCbWriterActiveCpuReadySlotUnlocked(
                          *this, pool_, lock,
                          pendingCanReleaseAtSemanticBoundary,
                          semanticBoundaryReleaseMode,
                          completionWaitActive,
                          writerActive)) {
                    continue;
                  }
                }
                break;
              case render::OpenCbSemanticReleaseWriterActiveSlotState::PresentBearing:
                perf::countOpenCbTailPresentSemanticReleaseBlockedNoCompletionWaitWriterActiveSlotPresent();
                break;
              }
            }
            break;
          case render::OpenCbSemanticReleaseNoCompletionWaitBlock::WriterInactive:
            perf::countOpenCbTailPresentSemanticReleaseBlockedNoCompletionWait();
            perf::countOpenCbTailPresentSemanticReleaseBlockedNoCompletionWaitWriterInactive();
            break;
        }
      }
      if (semanticBoundaryReleaseMode ==
              render::OpenCbSemanticBoundaryReleaseMode::CompletionWait &&
          !completionWaitActive) {
        semanticBoundaryReleaseUsedDuringWait = false;
      }
      if (maybePublishOpenCbActiveWaitCpuReadySlotUnlocked(
              *this, pool_, lock,
              pendingCanReleaseAtSemanticBoundary,
              semanticBoundaryReleaseMode,
              completionWaitActive,
              semanticBoundaryReleaseUsedDuringWait,
              writerActive)) {
        continue;
      }
      if (render::openCbPendingCanReleaseAtSemanticBoundary(
              pendingCanReleaseAtSemanticBoundary,
              /*sourceHasFinalPresentTail=*/false,
              semanticBoundaryReleaseMode,
              completionWaitActive,
              semanticBoundaryReleaseUsedDuringWait)) {
        if (submitPendingRecordLocked(lock)) {
          if (semanticBoundaryReleaseMode ==
              render::OpenCbSemanticBoundaryReleaseMode::CompletionWait) {
            semanticBoundaryReleaseUsedDuringWait = true;
          }
          perf::countOpenCbTailPresentSemanticReleaseSubmitted();
          if (stop_) {
            return;
          }
          continue;
        }
        perf::countOpenCbTailPresentSemanticReleaseFailed();
        perf::countOpenCbTailPresentPendingAbandonedNoReady();
        abortOpenCbPendingFailOpen("semantic boundary release");
      }
      const auto waitAction =
          render::selectOpenCbPendingTailWaitAction(
              pendingRecord.has_value(), readySlots_.empty(), stop_,
              writerActive, pendingTailTimeoutEnabled);
      if (waitAction == render::OpenCbPendingTailWaitAction::SubmitPending) {
        if (submitPendingRecordLocked(lock)) {
          if (stop_) {
            return;
          }
          continue;
        }
        perf::countOpenCbTailPresentPendingAbandonedNoReady();
        abortOpenCbPendingFailOpen("deterministic tail release");
      }
      if (waitAction == render::OpenCbPendingTailWaitAction::WaitForReady) {
        bool readyOrStopped = false;
        const bool waitObservedCompletionWaitActive =
            queueLifecycle_.completionWaitActive();
        const auto pendingWakePredicate =
            [this,
             &pendingCanReleaseAtSemanticBoundary,
             &semanticBoundaryReleaseUsedDuringWait,
             semanticBoundaryReleaseMode,
             waitObservedCompletionWaitActive] {
          const bool completionWaitActive =
              queueLifecycle_.completionWaitActive();
          return stop_ || !readySlots_.empty() ||
                 !writingSlot_.has_value() ||
                 queueLifecycle_.producerSequenceWaitActive() ||
                 render::openCbPendingCompletionWaitTransitionNeedsRecheck(
                     completionWaitActive,
                     waitObservedCompletionWaitActive,
                     semanticBoundaryReleaseMode,
                     pendingCanReleaseAtSemanticBoundary,
                     semanticBoundaryReleaseUsedDuringWait);
        };
        if (pendingTailTimeoutEnabled) {
          readyOrStopped =
              encodeCv_.wait_for(lock, pendingTailWaitTimeout,
                                  pendingWakePredicate);
        } else {
          encodeCv_.wait(lock, pendingWakePredicate);
          readyOrStopped = stop_ || !readySlots_.empty() ||
                           !writingSlot_.has_value();
        }
        if (pendingRecord.has_value() && readySlots_.empty()) {
          if (render::openCbPendingShouldSubmitForProducerSequenceWait(
                  pendingRecord.has_value(),
                  queueLifecycle_.producerSequenceWaitActive())) {
            if (submitPendingRecordLocked(lock)) {
              if (stop_) {
                return;
              }
              continue;
            }
            perf::countOpenCbTailPresentPendingAbandonedNoReady();
            abortOpenCbPendingFailOpen("producer sequence wait wake release");
          }
          const bool completionWaitActiveAfterWake =
              queueLifecycle_.completionWaitActive();
          if (render::openCbPendingCompletionWaitTransitionNeedsRecheck(
                  completionWaitActiveAfterWake,
                  waitObservedCompletionWaitActive,
                  semanticBoundaryReleaseMode,
                  pendingCanReleaseAtSemanticBoundary,
                  semanticBoundaryReleaseUsedDuringWait)) {
            continue;
          }
        }
        if (readySlots_.empty() &&
            (!readyOrStopped || stop_ || !writingSlot_.has_value())) {
          if (pendingTailTimeoutEnabled && !readyOrStopped && !stop_) {
            perf::countOpenCbTailPresentPendingTailWaitTimeout();
          }
          if (submitPendingRecordLocked(lock)) {
            if (pendingTailTimeoutEnabled && !readyOrStopped && !stop_) {
              perf::countOpenCbTailPresentPendingTimeoutSubmitted();
            }
            if (stop_) {
              return;
            }
          } else {
            perf::countOpenCbTailPresentPendingAbandonedNoReady();
            abortOpenCbPendingFailOpen("tail wait release");
          }
          continue;
        }
      }
    }

    const std::size_t count =
        queueLifecycle_.dequeueReadySlotBatchPrefix(
            lock,
            std::span<ReadySlotSnapshot>(scratch),
            [carryRenderSession](
                const std::deque<std::size_t>& readySlots,
                std::span<const core::ChunkSlot> slots,
                std::size_t maxCount) noexcept -> std::size_t {
              if (carryRenderSession) {
                return render::selectOpenCbTailPresentBatchPrefix(
                    readySlots, slots, maxCount);
              }
              return maxCount == 0u ? std::size_t{0} : std::size_t{1};
            });
    if (count == 0) {
      if (pendingRecord.has_value()) {
        if (!submitPendingRecordLocked(lock)) {
          perf::countOpenCbTailPresentPendingAbandonedNoReady();
          abortOpenCbPendingFailOpen("queue drained");
        }
      }
      return;
    }

    const bool selectedTailReadyPrefix =
        carryRenderSession && count > 1u &&
        scratch[count - 1u].slot &&
        render::slotHasFinalPresentTail(*scratch[count - 1u].slot);
    const bool selectedOpenCbSessionPrefix =
        render::selectedOpenCbPrefixStartsSession(
            std::span<const ReadySlotSnapshot>(scratch.data(), count),
            carryRenderSession);
    const bool selectedSemanticStartPrefix =
        selectedOpenCbSessionPrefix && !selectedTailReadyPrefix &&
        scratch[0].slot &&
        scratch[0].slot->publishReason ==
            perf::ChunkPublishReason::SemanticBoundary;
    const bool selectorCompletionWaitActive =
        queueLifecycle_.completionWaitActive();
    if (selectedTailReadyPrefix) {
      perf::countOpenCbTailPresentSelectorTailPrefix(
          static_cast<std::uint64_t>(count),
          selectorCompletionWaitActive);
    } else if (selectedSemanticStartPrefix) {
      perf::countOpenCbTailPresentSelectorSemanticPrefix(
          static_cast<std::uint64_t>(count),
          selectorCompletionWaitActive);
    } else if (selectedOpenCbSessionPrefix) {
      perf::countOpenCbTailPresentSelectorOrdinaryPrefix(
          static_cast<std::uint64_t>(count),
          selectorCompletionWaitActive);
    }
    std::array<QueueCompletionSource, kCommandChunkCount>
        selectedCompletionSources{};
    bool selectedCompletionSourcesValid = false;
    if (selectedOpenCbSessionPrefix) {
      const std::size_t retainedCount =
          queueLifecycle_.retainEncodedSourcesForPendingTail(
              lock,
              std::span<const ReadySlotSnapshot>(scratch.data(), count),
              std::span<QueueCompletionSource>(
                  selectedCompletionSources.data(), count));
      selectedCompletionSourcesValid = retainedCount == count;
      DXMT_ASSERT(selectedCompletionSourcesValid);
    }
    auto retainedSourceForIndex =
        [&retainSource, &selectedCompletionSources,
         selectedCompletionSourcesValid](
            std::unique_lock<std::mutex>& lock,
            std::size_t sourceIndex,
            const ReadySlotSnapshot& source,
            QueueCompletionSource& retained) {
          if (selectedCompletionSourcesValid) {
            retained = selectedCompletionSources[sourceIndex];
            return true;
          }
          return retainSource(lock, source, retained);
        };
    for (std::size_t sourceIndex = 0; sourceIndex < count; ++sourceIndex) {
      if (!lock.owns_lock()) {
        lock.lock();
      }
      const ReadySlotSnapshot source = scratch[sourceIndex];
      DXMT_ASSERT(source.slot != nullptr);
      const bool sourceHasFinalPresentTail =
          render::slotHasFinalPresentTail(*source.slot);
      const bool sourceIsOpenHead =
          render::slotIsOpenCbPreencodeHead(*source.slot);
      const bool sourceIsSemanticBoundary =
          source.slot->publishReason ==
          perf::ChunkPublishReason::SemanticBoundary;
      const bool tailReadyForCurrentHead =
          selectedTailReadyPrefix && sourceIndex + 1u < count;
      const bool sourceCanStartSession =
          render::slotCanStartOpenCbPendingSession(
              *source.slot, carryRenderSession, tailReadyForCurrentHead);
      bool sourceCanAppendToPending =
          render::slotCanAppendToOpenCbPending(
              *source.slot, carryRenderSession,
              static_cast<bool>(pendingSession), tailReadyForCurrentHead);

      if (pendingRecord.has_value() &&
          !sourceCanAppendToPending) {
        perf::countOpenCbTailPresentPendingAbandonedNonAppendable();
        if (!submitPendingRecordLocked(lock)) {
          abortOpenCbPendingFailOpen("non-appendable source");
        }
        if (!lock.owns_lock()) {
          lock.lock();
        }
      }
      if (pendingRecord.has_value() &&
          render::openCbPendingShouldSubmitBeforeInitializerWait(
              sourceCanAppendToPending,
              pendingSession &&
                  encoders::encodeChunkSessionHasActiveRender(
                      *pendingSession),
              initializer_ &&
                  initializer_->hasPendingUploadsUnlocked())) {
        if (!submitPendingRecordLocked(lock)) {
          abortOpenCbPendingFailOpen("initializer wait boundary");
        }
        if (!lock.owns_lock()) {
          lock.lock();
        }
        sourceCanAppendToPending = false;
      }

      bool appendToPending =
          pendingRecord.has_value() && sourceCanAppendToPending;
      if (appendToPending) {
        const QueueCompletionSource candidate = selectedCompletionSourcesValid
            ? selectedCompletionSources[sourceIndex]
            : core::metalqueue::completionSourceForReadySlot(source);
        const bool queueSourcesCanAppend =
            pendingSources.canAppend(candidate);
        const bool sessionSourcesCanAppend =
            !pendingSession ||
            encoders::canAppendEncodeChunkSessionSource(
                *pendingSession, candidate);
        if (!queueSourcesCanAppend || !sessionSourcesCanAppend) {
          if (!submitPendingRecordLocked(lock)) {
            abortOpenCbPendingFailOpen("session source preflight failed");
          }
          if (!lock.owns_lock()) {
            lock.lock();
          }
          appendToPending = false;
        }
      }
      const bool suppressCarryHeadWithoutTail =
          carryRenderSession && !allowPendingHeadWithoutReadyTail &&
          !pendingRecord.has_value() && sourceIsOpenHead &&
          !tailReadyForCurrentHead;
      if (suppressCarryHeadWithoutTail) {
        perf::countOpenCbTailPresentPendingSuppressedNoTail();
      }
      bool startPending = !pendingRecord.has_value() && sourceCanStartSession &&
          !suppressCarryHeadWithoutTail;
      QueueCompletionSource appendRetained{};
      bool appendRetainedValid = false;
      if (appendToPending) {
        if (!retainedSourceForIndex(lock, sourceIndex, source,
                                    appendRetained)) {
          perf::countOpenCbTailPresentPendingAbandonedRetainFailed();
          if (!submitPendingRecordLocked(lock)) {
            abortOpenCbPendingFailOpen("append source retain failed");
          }
          if (!lock.owns_lock()) {
            lock.lock();
          }
          appendToPending = false;
          startPending = sourceCanStartSession && tailReadyForCurrentHead;
        } else {
          appendRetainedValid = true;
        }
      }
      QueueCompletionSource startRetained{};
      bool startRetainedValid = false;
      if (startPending) {
        if (!retainedSourceForIndex(lock, sourceIndex, source,
                                    startRetained)) {
          perf::countOpenCbTailPresentPendingAbandonedRetainFailed();
          startPending = false;
        } else {
          startRetainedValid = true;
        }
      }

      encoders::EncodeChunkOptions options{};
      if (appendToPending || startPending) {
        options.allowInjectedCommandBufferMidChunkCommits =
            render::openCbPendingAllowsSemanticMidChunkCommits(
                appendToPending);
        options.disablePresentAcquireSplit = true;
      }
      if (carryRenderSession && (appendToPending || startPending)) {
        if (!pendingSession) {
          pendingSession = encoders::makeEncodeChunkSession();
        }
        options.session = pendingSession.get();
        options.deferSessionFinalization = !sourceHasFinalPresentTail;
        options.sessionSource =
            appendToPending ? appendRetained : startRetained;
        if (selectedOpenCbSessionPrefix && selectedCompletionSourcesValid) {
          options.sessionLookaheadSources =
              std::span<const ReadySlotSnapshot>(scratch.data() + sourceIndex,
                                                 count - sourceIndex);
        }
      }
      if (appendToPending) {
        options.commandBuffer = pendingRecord->commandBuffer;
      }

      lock.unlock();
      auto submission = encodeSource(source, std::move(options));
      lock.lock();

      if (!submission.has_value()) {
        if (appendToPending) {
          perf::countOpenCbTailPresentPendingAbandonedEncodeNull();
          if (!submitPendingRecordLocked(lock)) {
            abortOpenCbPendingFailOpen("append encode returned null");
          }
          if (!lock.owns_lock()) {
            lock.lock();
          }
        }
        if (startPending) {
          pendingSession.reset();
        }
        completeInlineSnapshot(source);
        continue;
      }

      if (startPending) {
        if (!startRetainedValid) {
          perf::countOpenCbTailPresentPendingAbandonedRetainFailed();
          submitRecordLocked(lock, *submission);
          pendingSession.reset();
          pendingCanReleaseAtSemanticBoundary = false;
          continue;
        }
        pendingSources.clear();
        if (!pendingSources.append(startRetained)) {
          DXMT_ASSERT(false && "first encoded session source must be valid");
          perf::countOpenCbTailPresentPendingAbandonedRetainFailed();
          pendingRecord = std::move(*submission);
          if (!submitPendingRecordLocked(lock)) {
            abortOpenCbPendingFailOpen("initial session source rejected");
          }
          continue;
        }
        pendingRecord = std::move(*submission);
        pendingCanReleaseAtSemanticBoundary =
            sourceIsSemanticBoundary && !sourceHasFinalPresentTail;
        const bool pendingStartedDuringCompletionWait =
            queueLifecycle_.completionWaitActive();
        perf::countOpenCbTailPresentPendingStarted(
            pendingStartedDuringCompletionWait);
        if (tailReadyForCurrentHead) {
          perf::countOpenCbTailPresentPendingStartedTailReady(
              pendingStartedDuringCompletionWait);
        } else if (sourceIsSemanticBoundary) {
          perf::countOpenCbTailPresentPendingStartedSemantic(
              pendingStartedDuringCompletionWait);
        } else {
          perf::countOpenCbTailPresentPendingStartedOrdinary(
              pendingStartedDuringCompletionWait);
        }
        continue;
      }

      if (appendToPending) {
        const std::uint64_t appendChainLength =
            std::max<std::uint64_t>(
                1, submission->commandBufferChainLength);
        const bool appendCommittedPendingTail =
            appendRetainedValid &&
            pendingRecord.has_value() &&
            pendingRecord->commandBuffer &&
            submission->commandBuffer &&
            pendingRecord->commandBuffer.handle !=
                submission->commandBuffer.handle &&
            appendChainLength > 1u;
        EncodeSessionSourceList mergedSources;
        const bool merged =
            appendRetainedValid &&
            core::metalqueue::mergeEncodedPendingTailSubmission(
                *submission, *pendingRecord, pendingSources.span(),
                appendRetained, appendCommittedPendingTail,
                sourceHasFinalPresentTail ? nullptr : &mergedSources);
        if (!merged) {
          perf::countOpenCbTailPresentPendingMergeFailed();
          if (!appendRetainedValid || !submission->commandBuffer) {
            if (!submitPendingRecordLocked(lock)) {
              abortOpenCbPendingFailOpen("merge failed before append submit");
            }
            if (!lock.owns_lock()) {
              lock.lock();
            }
            if (submission->commandBuffer) {
              submitRecordLocked(lock, *submission);
            } else {
              completeInlineSnapshot(source);
            }
            continue;
          }
          abortOpenCbPendingFailOpen("pending append metadata merge failed");
        }

        pendingRecord.reset();
        pendingSources.clear();
        pendingCanReleaseAtSemanticBoundary = false;
        if (!sourceHasFinalPresentTail) {
          pendingSources = mergedSources;
          pendingRecord = std::move(*submission);
          pendingCanReleaseAtSemanticBoundary =
              sourceIsSemanticBoundary && !sourceHasFinalPresentTail;
          perf::countOpenCbTailPresentHeadAppended();
          if (sourceIsSemanticBoundary) {
            perf::countOpenCbTailPresentHeadAppendedSemantic();
          } else {
            perf::countOpenCbTailPresentHeadAppendedOrdinary();
          }
          continue;
        }

        perf::countOpenCbTailPresentTailAppended();
        if (pendingSession &&
            !encoders::retainEncodeChunkSessionUntilSubmissionComplete(
                std::move(pendingSession), *submission)) {
          perf::countOpenCbTailPresentPendingMergeFailed();
          abortOpenCbPendingFailOpen("merged tail session retain failed");
        }
        submitRecordLocked(lock, *submission);
        perf::countOpenCbTailPresentTailSubmitted();
        pendingSession.reset();
        pendingCanReleaseAtSemanticBoundary = false;
        continue;
      }

      submitRecordLocked(lock, *submission);
    }
  }
}

void CommandQueue::runEncodeBatchLoop(
    std::span<core::metalqueue::ReadySlotSnapshot> scratch,
    EncodeBatchFn encodeBatch,
    OnSubmittedFn onSubmitted,
    core::metalqueue::ReadySlotBatchAppendPredicate canAppend,
    core::metalqueue::ReadySlotBatchPrefixSelector selectPrefix) {
  while (true) {
    std::unique_lock lock(mutex_);
    if (!queueLifecycle_.runEncodeBatchIteration(
            lock, scratch, encodeBatch, onSubmitted, canAppend, selectPrefix)) {
      return;
    }
  }
}

void CommandQueue::bindSelfLifecycle(ResolveSurfaceFlagsFn resolveSurfaceFlags) {
  queueLifecycle_.bindTrackedSubmissionState({
      .writingSlot = &writingSlot_,
      .writeIndex = &writeIndex_,
      .nextSeqId = &nextSeqId_,
      .readySlots = &readySlots_,
      .completedSeqQueue = &completedSeqQueue_,
      .completedPresentSeqQueue = &completedPresentSeqQueue_,
      .inflightCount = &inflightCount_,
      .completedSeqId = &completedSeqId_,
      .presentCompletedSeqId = &presentCompletedSeqId_,
      .completedPresentOrdinal = &presentOrdinalGate_.completedOrdinal,
      .lastCommittedSeqId = &lastCommittedSeqId_,
      .slots = std::span<core::ChunkSlot>(slots_.data(), slots_.size()),
      .mutex = &mutex_,
      .writeCv = &writeCv_,
      .encodeCv = &encodeCv_,
      .finishCv = &finishCv_,
      .presentCompletedCv = &presentCompletedCv_,
      .stop = &stop_,
      .submissionDiagnostics = &submissionDiagnostics_,
      .resolveSurfaceFlags = std::move(resolveSurfaceFlags),
  });
}

void CommandQueue::runFinishLoop() {
  while (true) {
    std::unique_lock lock(mutex_);
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
  while (queueLifecycle_.processOnePendingCompletion(stop_)) {
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
