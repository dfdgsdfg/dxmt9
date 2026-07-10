#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_archive_prewarm.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_draw_state.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_metal_labels.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_shader_sources.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dxmt9::encoders {
WMTSamplerInfo makeSamplerInfo(const core::FlatStateSet<core::kMaxSamplerStates>& states,
                               float lodMinClamp);
}

namespace dxmt9::pipeline {

namespace {
constexpr u64 kFnvOffset = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;
constexpr u64 kFillPipelineKeyTag = 0x66696c6c5f70736full; // "fill_pso"

inline u64 mix(u64 hash, u64 value) {
  hash ^= value;
  hash *= kFnvPrime;
  return hash;
}

// Mirror of backend_metal.mm's file-local debugForceVisibleDraw. Keeps this
// translation unit self-contained so Cache doesn't depend on backend state.
bool debugForceVisibleDraw() {
  const char* env = std::getenv("DXMT9_DEBUG_FORCE_VISIBLE_DRAW");
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

bool envFlag(const char* name) noexcept {
  const char* env = std::getenv(name);
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

bool synchronousPsoCompileEnabled() noexcept {
  static const bool enabled =
      envFlag("DXMT9_SYNC_PSO_COMPILE") ||
      envFlag("MTL_CAPTURE_ENABLED") ||
      envFlag("METAL_CAPTURE_ENABLED");
  return enabled;
}

bool pipelineBuildTraceEnabled() noexcept {
  static const bool enabled =
      envFlag("DXMT9_TRACE_PIPELINE_BUILD") ||
      envFlag("DXMT_TRACE_QUEUE");
  return enabled;
}

bool argbufDirectCbufEnvEnabled() noexcept {
  static const bool enabled = envFlag("DXMT9_ARGBUF_DIRECT_CBUF");
  return enabled;
}

void traceDrawPipelineBuild(const char* stage,
                            const ShaderVariantKey& key,
                            u64 sourceHash) {
  if (!pipelineBuildTraceEnabled()) {
    return;
  }
  util::logf(util::LogLevel::Info, "dxmt9-pipeline-cache",
             "draw-pso %s variant=0x%llx source=0x%llx tile=%u tile_base=%u "
             "argbuf=%u argbuf_direct_cbuf=%u resource_array=%u fragmentless=%u sample=%u "
             "color0=%u depth=%u stencil=%u vsout=0x%x alpha_to_coverage=%u",
             stage ? stage : "unknown",
             static_cast<unsigned long long>(key.hash),
             static_cast<unsigned long long>(sourceHash),
             key.tileFfpMode ? 1u : 0u,
             key.tileFfpBaseColor ? 1u : 0u,
             key.argbufHybridMode ? 1u : 0u,
             key.argbufDirectCbufMode ? 1u : 0u,
             key.argbufResourceArray ? 1u : 0u,
             key.fragmentlessDepthOnly ? 1u : 0u,
             static_cast<unsigned>(std::max(1u, key.sampleCount)),
             static_cast<unsigned>(key.colorFormats[0]),
             static_cast<unsigned>(key.depthFormat),
             static_cast<unsigned>(key.stencilFormat),
             static_cast<unsigned>(key.vsOutLayoutKey),
             key.alphaToCoverage ? 1u : 0u);
}

class ScopedPerfCounter {
 public:
  explicit ScopedPerfCounter(void (*record)(std::uint64_t))
      : record_(perf::enabled() ? record : nullptr) {
    if (record_) {
      started_ = std::chrono::steady_clock::now();
    }
  }

  ~ScopedPerfCounter() {
    if (!record_) {
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    record_(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  }

  ScopedPerfCounter(const ScopedPerfCounter&) = delete;
  ScopedPerfCounter& operator=(const ScopedPerfCounter&) = delete;

 private:
  void (*record_)(std::uint64_t) = nullptr;
  std::chrono::steady_clock::time_point started_{};
};

bool x8ShaderAlphaFillEnabled() {
  static const bool enabled = envFlag("DXMT9_X8_SHADER_ALPHA_FILL");
  return enabled;
}

bool probeDisableAlphaBlendEnabled() {
  static const bool enabled = envFlag("DXMT9_PROBE_DISABLE_ALPHA_BLEND");
  return enabled;
}

bool isX8Format(core::Format format) noexcept {
  return format == core::Format::X8R8G8B8 ||
         format == core::Format::X8B8G8R8;
}

u32 x8AlphaOneTextureMask(resources::Pool& pool,
                          const core::FlatDrawStateRecord& hot,
                          u32 activeFragmentTextureMask) {
  if (!x8ShaderAlphaFillEnabled()) {
    return 0;
  }
  u32 mask = 0;
  for (u32 stage = 0; stage < core::kMaxFragmentSamplers; ++stage) {
    if ((activeFragmentTextureMask & (1u << stage)) == 0u) {
      continue;
    }
    const auto textureHandle = hot.textures[stage];
    if (!textureHandle) {
      continue;
    }
    const auto* texture = pool.findTexture(textureHandle.value);
    if (texture && isX8Format(texture->desc.format)) {
      mask |= 1u << stage;
    }
  }
  return mask;
}

void stampX8AlphaOneTextureMask(ShaderVariantKey& key,
                                drawshader::ShaderSourceContext& shaderSource,
                                u32 mask) noexcept {
  key.x8AlphaOneTextureMask = mask;
  shaderSource.x8AlphaOneTextureMask = mask;
  if (mask != 0u) {
    key.hash = mix(mix(key.hash, 0x78385f616c706861ull), mask);
  }
}

// R-BACK-13.* tile-FFP ↔ portable equality escape hatch. Mirrors the
// DXMT9_DISABLE_ARGBUF_HYBRID one-shot-static pattern: read once at first
// use (process-init semantics — env changes after dxmt9 load do not take
// effect, like the other DXMT9 knobs).
//   off   — never take the tile path; always route the genuine portable lane.
//           THIS IS THE DEFAULT (value unset / empty / unrecognized) as an
//           interim safety measure: the two-stage encode now issues the
//           base-colour draw followed by the tile post-pass, but tile-FFP
//           still needs eligible-draw equality and perf validation before it
//           can become a production default.
//   auto  — explicit opt-in to the selectTileFfpForPass heuristic (the path
//           validated behind this flag; the default can flip back to auto once
//           equality and useful coverage hold).
//   force — take the tile path whenever the genuine eligibility gates still
//           pass (FFP shape, precision, A2C). NEVER forces an ineligible
//           draw (non-FFP, textured, vertex-blended, precision-unsafe, A2C),
//           which would mis-emit; those keep falling back to portable.
enum class TileFfpModeOverride : std::uint8_t { Auto, Off, Force };

TileFfpModeOverride tileFfpModeOverride() noexcept {
  static const TileFfpModeOverride mode = [] {
    const char* env = std::getenv("DXMT9_TILE_FFP");
    if (env) {
      if (std::strcmp(env, "auto") == 0) return TileFfpModeOverride::Auto;
      if (std::strcmp(env, "force") == 0) return TileFfpModeOverride::Force;
      // "off" and any unrecognized value fall through to the default.
    }
    return TileFfpModeOverride::Off;  // interim safety default — see comment above.
  }();
  return mode;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
makeReadyPipelineFuture(WMT::Reference<WMT::RenderPipelineState> value = {}) {
  std::promise<WMT::Reference<WMT::RenderPipelineState>> promise;
  promise.set_value(std::move(value));
  return promise.get_future().share();
}

const char* lookupRole(HandleLookupContext context) noexcept {
  return context.role ? context.role : "unknown";
}

void logStaleDrawHandle(core::PsoHandle handle,
                        HandleLookupContext context,
                        const char* reason,
                        std::size_t snapshotSize,
                        const PsoSlot* slot = nullptr) {
  static std::atomic<std::uint64_t> logged{0};
  if (!handle.valid() || logged.fetch_add(1, std::memory_order_relaxed) >= 32u) {
    return;
  }
  const u32 actualGeneration = slot ? slot->generation : 0u;
  const u64 keyHash = slot && slot->occupied ? slot->key.hash : 0u;
  util::logf(util::LogLevel::Warn, "dxmt9-pipeline-cache",
             "stale draw PSO handle role=%s reason=%s chunk=%llu command=%u "
             "slot=%u generation=%u actual_generation=%u snapshot_size=%zu key_hash=0x%llx",
             lookupRole(context), reason,
             static_cast<unsigned long long>(context.chunkSeqId),
             static_cast<unsigned>(context.commandIndex),
             static_cast<unsigned>(handle.slot),
             static_cast<unsigned>(handle.generation),
             static_cast<unsigned>(actualGeneration),
             snapshotSize,
             static_cast<unsigned long long>(keyHash));
}

void logStaleDepthHandle(core::DepthStencilHandle handle,
                         HandleLookupContext context,
                         const char* reason,
                         std::size_t snapshotSize,
                         const DepthStencilSlot* slot = nullptr) {
  static std::atomic<std::uint64_t> logged{0};
  if (!handle.valid() || logged.fetch_add(1, std::memory_order_relaxed) >= 32u) {
    return;
  }
  const u32 actualGeneration = slot ? slot->generation : 0u;
  const u64 keyHash = slot && slot->occupied
      ? static_cast<u64>(DepthStencilKeyHash{}(slot->key))
      : 0u;
  util::logf(util::LogLevel::Warn, "dxmt9-pipeline-cache",
             "stale depth/stencil handle role=%s reason=%s chunk=%llu command=%u "
             "slot=%u generation=%u actual_generation=%u snapshot_size=%zu key_hash=0x%llx",
             lookupRole(context), reason,
             static_cast<unsigned long long>(context.chunkSeqId),
             static_cast<unsigned>(context.commandIndex),
             static_cast<unsigned>(handle.slot),
             static_cast<unsigned>(handle.generation),
             static_cast<unsigned>(actualGeneration),
             snapshotSize,
             static_cast<unsigned long long>(keyHash));
}

core::PsoHandle internDrawHandleLocked(Cache& cache,
                                       const ShaderVariantKey& key,
                                       const Entry& entry) {
  if (auto it = cache.drawHandles.find(key); it != cache.drawHandles.end()) {
    return it->second;
  }
  if (cache.drawSlots.size() >= core::PsoHandle::kInvalidSlot) {
    perf::countDrawPsoSlotExhausted();
    util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
               "draw PSO handle table exhausted slots=%llu variant=0x%llx",
               static_cast<unsigned long long>(cache.drawSlots.size()),
               static_cast<unsigned long long>(key.hash));
    return {};
  }
  const core::PsoHandle handle{
      .slot = static_cast<std::uint16_t>(cache.drawSlots.size()),
      .generation = static_cast<std::uint16_t>(1u),
  };
  cache.drawSlots.push_back(PsoSlot{
      .generation = handle.generation,
      .key = key,
      .entry = entry,
      .occupied = true,
  });
  perf::recordDrawPsoSlotCount(cache.drawSlots.size());
  if (key.argbufHybridMode) {
    perf::countDrawPsoVariantArgbufStage2();
  }
  if (key.tileFfpMode) {
    perf::countDrawPsoVariantTileFfp();
  }
  cache.drawHandles.emplace(key, handle);
  std::atomic_store_explicit(
      &cache.drawSlotSnapshot,
      std::shared_ptr<const std::vector<PsoSlot>>(
          std::make_shared<std::vector<PsoSlot>>(cache.drawSlots)),
      std::memory_order_release);
  return handle;
}

core::DepthStencilHandle internDepthStencilHandleLocked(
    Cache& cache,
    const DepthStencilKey& key,
    WMT::Reference<WMT::DepthStencilState> state) {
  if (auto it = cache.depthHandles.find(key); it != cache.depthHandles.end()) {
    return it->second;
  }
  if (cache.depthSlots.size() >= core::DepthStencilHandle::kInvalidSlot) {
    return {};
  }
  const core::DepthStencilHandle handle{
      .slot = static_cast<std::uint16_t>(cache.depthSlots.size()),
      .generation = static_cast<std::uint16_t>(1u),
  };
  cache.depthSlots.push_back(DepthStencilSlot{
      .generation = handle.generation,
      .key = key,
      .state = state,
      .occupied = true,
  });
  cache.depthHandles.emplace(key, handle);
  std::atomic_store_explicit(
      &cache.depthSlotSnapshot,
      std::shared_ptr<const std::vector<DepthStencilSlot>>(
          std::make_shared<std::vector<DepthStencilSlot>>(cache.depthSlots)),
      std::memory_order_release);
  return handle;
}

template <typename Result>
class CompileQueue {
 public:
  CompileQueue() {
    std::size_t workers = 0;
    if (const char* env = std::getenv("DXMT9_PSO_COMPILE_THREADS");
        env && env[0] != '\0') {
      char* end = nullptr;
      const auto parsed = std::strtoull(env, &end, 10);
      if (end != env) {
        workers = static_cast<std::size_t>(parsed);
      }
    }
    if (workers == 0) {
      const auto hw = std::thread::hardware_concurrency();
      workers = hw == 0 ? 2u : std::min<std::size_t>(4u, std::max<std::size_t>(2u, hw / 2u));
    }
    workers = std::clamp<std::size_t>(workers, 1u, 8u);
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
      workers_.emplace_back([this] { run(); });
    }
  }

  ~CompileQueue() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  CompileQueue(const CompileQueue&) = delete;
  CompileQueue& operator=(const CompileQueue&) = delete;

  template <typename Fn>
  std::shared_future<Result> submit(Fn&& fn) {
    std::packaged_task<Result()> task(std::forward<Fn>(fn));
    auto future = task.get_future().share();
    {
      std::lock_guard lock(mutex_);
      jobs_.push_back(std::move(task));
    }
    cv_.notify_one();
    return future;
  }

 private:
  void run() {
    while (true) {
      std::packaged_task<Result()> task;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (stop_ && jobs_.empty()) {
          return;
        }
        task = std::move(jobs_.front());
        jobs_.pop_front();
      }
      task();
    }
  }

  std::mutex mutex_{};
  std::condition_variable cv_{};
  bool stop_ = false;
  std::deque<std::packaged_task<Result()>> jobs_{};
  std::vector<std::thread> workers_{};
};

using PipelineCompileQueue = CompileQueue<WMT::Reference<WMT::RenderPipelineState>>;
using LibraryCompileQueue = CompileQueue<WMT::Reference<WMT::Library>>;

LibraryCompileQueue& libraryCompileQueue() {
  static LibraryCompileQueue queue;
  return queue;
}

PipelineCompileQueue& pipelineCompileQueue() {
  (void)libraryCompileQueue();
  static PipelineCompileQueue queue;
  return queue;
}

template <typename Fn>
std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
submitPipelineBuild(Fn&& fn) {
  if (synchronousPsoCompileEnabled()) {
    std::promise<WMT::Reference<WMT::RenderPipelineState>> promise;
    try {
      promise.set_value(std::forward<Fn>(fn)());
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
    return promise.get_future().share();
  }
  // R-BACK-3.9 / R-BACK-3.10 — every submission here is a pipeline-cache
  // compile-miss (the Cache::getOrBuild* callers only reach this after a
  // cache-map miss), so it is exactly the R-BACK-3.10 "new archive entry"
  // activity signal regardless of whether this particular job's archive
  // pointer ends up empty (pending load) or attached.
  archive_prewarm::noteArchiveEntryCompiled();
  if (archive_prewarm::archiveLoadInFlight()) {
    // `fn` may read an empty (not-yet-attached) archive handle inside its
    // body and silently skip the addRenderPipelineFunctionsWithDescriptor:
    // side effect (see the `if (archive && *archive)` guards throughout
    // this file). Queue a copy to replay once the archive attaches so
    // that compiled PSO is still preserved into it (R-BACK-3.9). The
    // replay recompiles (discarding the resulting PSO) purely for the
    // archive-add side effect — no cache map is touched by `fn` itself,
    // only by the caller after this function returns, so re-running `fn`
    // standalone cannot double-insert anything.
    if constexpr (std::is_copy_constructible_v<std::decay_t<Fn>>) {
      auto replay = fn;
      archive_prewarm::queueArchiveBackfill(
          [replay]() mutable { replay(); });
    }
  }
  return pipelineCompileQueue().submit(std::forward<Fn>(fn));
}

template <typename Fn>
std::shared_future<WMT::Reference<WMT::Library>>
submitLibraryBuild(Fn&& fn) {
  if (synchronousPsoCompileEnabled()) {
    std::promise<WMT::Reference<WMT::Library>> promise;
    try {
      promise.set_value(std::forward<Fn>(fn)());
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
    return promise.get_future().share();
  }
  return libraryCompileQueue().submit(std::forward<Fn>(fn));
}

std::shared_future<WMT::Reference<WMT::Library>>
getOrSubmitSourceLibraryLocked(Cache& cache,
                               WMT::Reference<WMT::Device> device,
                               u64 sourceHash,
                               const std::string& source) {
  if (auto it = cache.sourceLibraries.find(sourceHash);
      it != cache.sourceLibraries.end()) {
    return it->second.future;
  }

  auto future = submitLibraryBuild(
      [device, source]() mutable {
        return shaders::makeLibrary(device, source);
      });
  cache.sourceLibraries.emplace(sourceHash, SourceLibraryEntry{future});
  perf::recordSourceLibraryEntryCount(cache.sourceLibraries.size());
  return future;
}

enum class PipelineBuildFailureStage : std::uint8_t {
  Library,
  Function,
  Pso,
};

void countPipelineBuildFailure(PipelineBuildFailureStage stage) {
  perf::countPipelineBuildFailDraw();
  switch (stage) {
    case PipelineBuildFailureStage::Library:
      perf::countPipelineBuildFailLibrary();
      break;
    case PipelineBuildFailureStage::Function:
      perf::countPipelineBuildFailFunction();
      break;
    case PipelineBuildFailureStage::Pso:
      perf::countPipelineBuildFailPso();
      break;
  }
}

void logPipelineBuildFailureOnce(const char* stage,
                                 const ShaderVariantKey& key,
                                 u64 sourceHash) {
  static std::mutex mutex;
  static std::unordered_set<u64> logged;
  const u64 stageHash = core::hashString(stage ? std::string_view(stage) : std::string_view{});
  const u64 logKey = key.hash ^ (sourceHash << 1u) ^ (stageHash << 2u);
  bool shouldLog = false;
  {
    std::lock_guard lock(mutex);
    shouldLog = logged.size() < 64u && logged.insert(logKey).second;
  }
  if (!shouldLog) {
    return;
  }
  util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
             "draw pipeline build failed: stage=%s variant=0x%llx source=0x%llx tile=%u tile_base=%u argbuf=%u argbuf_direct_cbuf=%u resource_array=%u color0fmt=%u depthfmt=%u stencilfmt=%u",
             stage ? stage : "unknown",
             static_cast<unsigned long long>(key.hash),
             static_cast<unsigned long long>(sourceHash),
             key.tileFfpMode ? 1u : 0u,
             key.tileFfpBaseColor ? 1u : 0u,
             key.argbufHybridMode ? 1u : 0u,
             key.argbufDirectCbufMode ? 1u : 0u,
             key.argbufResourceArray ? 1u : 0u,
             static_cast<unsigned>(key.colorFormats[0]),
             static_cast<unsigned>(key.depthFormat),
             static_cast<unsigned>(key.stencilFormat));
}

}  // namespace

u64 makeShaderSourceDebugEnvKey(bool trimUnusedVaryings,
                                bool trimVertexTemps,
                                bool trimVsOutputScratch,
                                bool forceFullscreenVertex,
                                bool flipTranslatedVertexY,
                                bool forceFragmentShaderColor,
                                bool disableAlphaTest,
                                bool disableFog,
                                bool forceTextureWhite,
                                std::string_view fragmentMode,
                                bool forcePixelVFlip,
                                bool debugFfpUv,
                                bool debugFfpTexture,
                                bool debugFfpAlpha,
                                bool probeDropVSOutPointSize,
                                bool probePositionOnlyVSOut,
                                bool probeHalfVSOut,
                                bool probeFragmentlessKeepVSOut) noexcept {
  u64 hash = kFnvOffset;
  hash = mix(hash, kShaderDebugEnvSchemaVersion);
  hash = mix(hash, static_cast<u64>(trimUnusedVaryings));
  hash = mix(hash, static_cast<u64>(trimVertexTemps));
  hash = mix(hash, static_cast<u64>(trimVsOutputScratch));
  hash = mix(hash, static_cast<u64>(forceFullscreenVertex));
  hash = mix(hash, static_cast<u64>(flipTranslatedVertexY));
  hash = mix(hash, static_cast<u64>(forceFragmentShaderColor));
  hash = mix(hash, static_cast<u64>(disableAlphaTest));
  hash = mix(hash, static_cast<u64>(disableFog));
  hash = mix(hash, static_cast<u64>(forceTextureWhite));
  hash = mix(hash, core::hashString(fragmentMode));
  hash = mix(hash, static_cast<u64>(forcePixelVFlip));
  hash = mix(hash, static_cast<u64>(debugFfpUv));
  hash = mix(hash, static_cast<u64>(debugFfpTexture));
  hash = mix(hash, static_cast<u64>(debugFfpAlpha));
  if (probeDropVSOutPointSize) {
    hash = mix(hash, 0x9f3b7c2d4a11e905ull);
  }
  if (probePositionOnlyVSOut) {
    hash = mix(hash, 0x3b08d0a9d08c0fb1ull);
  }
  if (probeHalfVSOut) {
    hash = mix(hash, 0xe14f9d92a8c61d37ull);
  }
  if (probeFragmentlessKeepVSOut) {
    hash = mix(hash, 0x2849ee34d4c1c0e7ull);
  }
  return hash;
}

u64 currentShaderSourceDebugEnvKey(
    std::optional<bool> forceTextureWhiteOverride) noexcept {
  const char* fragmentMode = std::getenv("DXMT_DEBUG_FRAGMENT_MODE");
  return makeShaderSourceDebugEnvKey(
      envFlag("DXMT9_TRIM_UNUSED_VARYINGS"),
      envFlag("DXMT9_TRIM_VERTEX_TEMPS"),
      envFlag("DXMT9_TRIM_VS_OUTPUT_SCRATCH"),
      envFlag("DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX"),
      envFlag("DXMT_DEBUG_FLIP_VERTEX_Y"),
      envFlag("DXMT_DEBUG_FORCE_FRAGMENT_COLOR"),
      envFlag("DXMT_DISABLE_ALPHA_TEST"),
      envFlag("DXMT_DISABLE_FOG"),
      forceTextureWhiteOverride.value_or(envFlag("DXMT_FORCE_TEXTURE_WHITE")),
      fragmentMode ? std::string_view(fragmentMode) : std::string_view{},
      envFlag("DXMT_DEBUG_FORCE_PIXEL_V_FLIP"),
      envFlag("DXMT_DEBUG_FFP_UV"),
      envFlag("DXMT_DEBUG_FFP_TEXTURE"),
      envFlag("DXMT_DEBUG_FFP_ALPHA"),
      envFlag("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE"),
      envFlag("DXMT9_PROBE_POSITION_ONLY_VSOUT"),
      envFlag("DXMT9_PROBE_HALF_VSOUT"),
      debug::probeFragmentlessDepthOnlyKeepVSOut());
}

u64 currentShaderSourceDebugEnvKey() noexcept {
  return currentShaderSourceDebugEnvKey(std::nullopt);
}

bool shaderSourceDebugEnvIsDefault() noexcept {
  static const u64 kDefaultKey = makeShaderSourceDebugEnvKey(
      /*trimUnusedVaryings=*/false, /*trimVertexTemps=*/false,
      /*trimVsOutputScratch=*/false,
      /*forceFullscreenVertex=*/false, /*flipTranslatedVertexY=*/false,
      /*forceFragmentShaderColor=*/false, /*disableAlphaTest=*/false,
      /*disableFog=*/false, /*forceTextureWhite=*/false,
      /*fragmentMode=*/std::string_view{}, /*forcePixelVFlip=*/false,
      /*debugFfpUv=*/false, /*debugFfpTexture=*/false,
      /*debugFfpAlpha=*/false, /*probeDropVSOutPointSize=*/false,
      /*probePositionOnlyVSOut=*/false, /*probeHalfVSOut=*/false,
      /*probeFragmentlessKeepVSOut=*/false);
  return currentShaderSourceDebugEnvKey() == kDefaultKey;
}

ShaderVariantKey makeShaderVariantProbeKey(ShaderVariantKey key) noexcept {
  key.vertexSourceHash = 0;
  key.fragmentSourceHash = 0;
  key.tileSourceHash = 0;
  return key;
}

bool fragmentlessDepthOnlyShaderSafe(const drawshader::ShaderSourceContext& shaderSource) {
  try {
    auto fragment = drawshader::makeDrawShaderSource(shaderSource, false);
    return fragment.find("discard_fragment()") == std::string::npos &&
           fragment.find("[[depth") == std::string::npos;
  } catch (const std::exception& ex) {
    util::logf(util::LogLevel::Warn, "dxmt9-pipeline-cache",
               "fragmentless depth-only probe rejected: fragment safety scan failed: %s",
               ex.what());
  } catch (...) {
    util::logf(util::LogLevel::Warn, "dxmt9-pipeline-cache",
               "fragmentless depth-only probe rejected: fragment safety scan failed");
  }
  return false;
}

std::optional<detail::DrawShaderSources>
detail::makeContainedDrawShaderSources(const drawshader::ShaderSourceContext& shaderSource,
                                       u64 variantHash) {
  auto shaderHash = [](const core::ShaderRef& shader) {
    return shader.hash != 0 ? shader.hash : shader.bytecode.hash;
  };

  try {
    auto vertex = drawshader::makeDrawShaderSource(shaderSource, true);
    const u64 vertexHash = shaders::makeHash(vertex);
    if (shaderSource.fragmentlessDepthOnly) {
      return detail::DrawShaderSources{
          .vertex = std::move(vertex),
          .fragment = {},
          .vertexHash = vertexHash,
          .fragmentHash = 0,
      };
    }
    auto fragment = drawshader::makeDrawShaderSource(shaderSource, false);
    const u64 fragmentHash = shaders::makeHash(fragment);
    return detail::DrawShaderSources{
        .vertex = std::move(vertex),
        .fragment = std::move(fragment),
        .vertexHash = vertexHash,
        .fragmentHash = fragmentHash,
    };
  } catch (const std::exception& ex) {
    util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
               "draw shader source generation failed: %s variant=0x%llx vs=0x%llx ps=0x%llx",
               ex.what(),
               static_cast<unsigned long long>(variantHash),
               static_cast<unsigned long long>(shaderHash(shaderSource.vertexShader)),
               static_cast<unsigned long long>(shaderHash(shaderSource.pixelShader)));
  } catch (...) {
    util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
               "draw shader source generation failed: unknown exception variant=0x%llx vs=0x%llx ps=0x%llx",
               static_cast<unsigned long long>(variantHash),
               static_cast<unsigned long long>(shaderHash(shaderSource.vertexShader)),
               static_cast<unsigned long long>(shaderHash(shaderSource.pixelShader)));
  }

  return std::nullopt;
}

std::array<BlendAttachmentKey, core::kMaxRenderTargets>
detail::makeBlendAttachmentKeys(core::FlatDrawStateView state,
                                bool forceVisibleDraw,
                                bool disableAlphaBlend) {
  std::array<BlendAttachmentKey, core::kMaxRenderTargets> blendAttachments{};
  const auto& rs = state.hot->renderStates;
  const bool blendEnabled =
      !forceVisibleDraw &&
      !(probeDisableAlphaBlendEnabled() || disableAlphaBlend) &&
      core::flatStateOr(rs, core::RS_ALPHABLEND_ENABLE, 0u) != 0;
  const bool separateAlphaBlend =
      core::flatStateOr(rs, core::RS_SEPARATE_ALPHA_BLEND_ENABLE, 0u) != 0;
  const u32 rgbBlendOperation =
      core::flatStateOr(rs, core::RS_BLEND_OP, static_cast<u32>(core::BlendOp::Add));
  const u32 alphaBlendOperation =
      separateAlphaBlend ? core::flatStateOr(rs, core::RS_BLEND_OP_ALPHA, rgbBlendOperation)
                         : rgbBlendOperation;
  u32 sourceRGBBlendFactor =
      core::flatStateOr(rs, core::RS_SRC_BLEND, static_cast<u32>(core::BlendFactor::One));
  u32 destinationRGBBlendFactor =
      core::flatStateOr(rs, core::RS_DEST_BLEND, static_cast<u32>(core::BlendFactor::Zero));
  auto fixLegacyBothSrcAlpha = [](u32& src, u32& dst) {
    if (src == static_cast<u32>(core::BlendFactor::BothSrcAlpha)) {
      src = static_cast<u32>(core::BlendFactor::SrcAlpha);
      dst = static_cast<u32>(core::BlendFactor::InvSrcAlpha);
    } else if (src == static_cast<u32>(core::BlendFactor::BothInvSrcAlpha)) {
      src = static_cast<u32>(core::BlendFactor::InvSrcAlpha);
      dst = static_cast<u32>(core::BlendFactor::SrcAlpha);
    }
  };
  fixLegacyBothSrcAlpha(sourceRGBBlendFactor, destinationRGBBlendFactor);
  u32 sourceAlphaBlendFactor = sourceRGBBlendFactor;
  u32 destinationAlphaBlendFactor = destinationRGBBlendFactor;
  if (separateAlphaBlend) {
    sourceAlphaBlendFactor =
        core::flatStateOr(rs, core::RS_SRC_BLEND_ALPHA, sourceAlphaBlendFactor);
    destinationAlphaBlendFactor =
        core::flatStateOr(rs, core::RS_DEST_BLEND_ALPHA, destinationAlphaBlendFactor);
    fixLegacyBothSrcAlpha(sourceAlphaBlendFactor, destinationAlphaBlendFactor);
  }
  // D3D9 exposes one color-write mask per render target: RS_COLOR_WRITE_ENABLE
  // drives RT0, ...ENABLE1/2/3 drive RT1/2/3. Each mask defaults independently
  // to all channels (0xf) and per-RT slots do NOT inherit RT0's mask, matching
  // the reset() seeding so single-RT apps (which only touch slot 168) and unset
  // MRT slots both resolve to all-channels.
  constexpr std::array<u32, core::kMaxRenderTargets> kColorWriteSlots = {
      core::RS_COLOR_WRITE_ENABLE,
      core::RS_COLOR_WRITE_ENABLE1,
      core::RS_COLOR_WRITE_ENABLE2,
      core::RS_COLOR_WRITE_ENABLE3,
  };

  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    auto& blend = blendAttachments[i];
    blend.blendingEnabled = blendEnabled;
    blend.rgbBlendOperation = rgbBlendOperation;
    blend.alphaBlendOperation = alphaBlendOperation;
    blend.sourceRGBBlendFactor = sourceRGBBlendFactor;
    blend.destinationRGBBlendFactor = destinationRGBBlendFactor;
    blend.sourceAlphaBlendFactor = sourceAlphaBlendFactor;
    blend.destinationAlphaBlendFactor = destinationAlphaBlendFactor;
    blend.colorWriteMask =
        forceVisibleDraw ? 0xfu : core::flatStateOr(rs, kColorWriteSlots[i], 0xfu);
  }
  return blendAttachments;
}

ShaderVariantKey detail::makeFillPipelineKey(const core::ColorRGBA& color,
                                             u32 pixelFormat) noexcept {
  ShaderVariantKey key{};
  key.hash = kFnvOffset;
  key.hash = mix(key.hash, kFillPipelineKeyTag);
  key.hash = mix(key.hash, static_cast<u64>(std::bit_cast<u32>(color.r)));
  key.hash = mix(key.hash, static_cast<u64>(std::bit_cast<u32>(color.g)));
  key.hash = mix(key.hash, static_cast<u64>(std::bit_cast<u32>(color.b)));
  key.hash = mix(key.hash, static_cast<u64>(std::bit_cast<u32>(color.a)));
  key.hash = mix(key.hash, pixelFormat);
  key.colorFormats[0] = pixelFormat;
  key.blend[0].pixelFormat = pixelFormat;
  return key;
}

std::size_t BlendAttachmentKeyHash::operator()(const BlendAttachmentKey& key) const noexcept {
  u64 hash = kFnvOffset;
  hash = mix(hash, static_cast<u64>(key.blendingEnabled));
  hash = mix(hash, key.rgbBlendOperation);
  hash = mix(hash, key.alphaBlendOperation);
  hash = mix(hash, key.sourceRGBBlendFactor);
  hash = mix(hash, key.destinationRGBBlendFactor);
  hash = mix(hash, key.sourceAlphaBlendFactor);
  hash = mix(hash, key.destinationAlphaBlendFactor);
  hash = mix(hash, key.colorWriteMask);
  hash = mix(hash, key.pixelFormat);
  return static_cast<std::size_t>(hash);
}

std::size_t StencilFaceKeyHash::operator()(const StencilFaceKey& key) const noexcept {
  u64 hash = kFnvOffset;
  hash = mix(hash, static_cast<u64>(key.enabled));
  hash = mix(hash, key.compareFunction);
  hash = mix(hash, key.failureOperation);
  hash = mix(hash, key.depthFailureOperation);
  hash = mix(hash, key.passOperation);
  hash = mix(hash, key.readMask);
  hash = mix(hash, key.writeMask);
  return static_cast<std::size_t>(hash);
}

std::size_t ShaderVariantKeyHash::operator()(const ShaderVariantKey& key) const noexcept {
  const bool measure = perf::enabled();
  const auto started = measure ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
  u64 hash = key.hash;
  hash = mix(hash, key.vertexSourceHash);
  hash = mix(hash, key.fragmentSourceHash);
  hash = mix(hash, key.tileSourceHash);
  hash = mix(hash, key.emitterVersion);
  hash = mix(hash, key.sourceLayoutVersion);
  hash = mix(hash, key.debugEnvSchemaVersion);
  hash = mix(hash, key.debugEnvKey);
  hash = mix(hash, static_cast<u64>(key.textured));
  hash = mix(hash, key.textureMask);
  hash = mix(hash, static_cast<u64>(key.linear));
  hash = mix(hash, static_cast<u64>(key.clipPlanes));
  hash = mix(hash, static_cast<u64>(key.alphaTest));
  hash = mix(hash, static_cast<u64>(key.alphaToCoverage));
  // R-BACK-13.3: tile_ffp_mode participates in the PSO key hash so the
  // fragment-stage and tile-stage variants of the same FFPKeyPS hit
  // distinct cache entries.
  hash = mix(hash, static_cast<u64>(key.tileFfpMode));
  // R-BACK-13.1: the tile-FFP base-colour sub-key participates in the hash so
  // the base-colour render PSO (fog/alpha-test/A2C stripped) never collides
  // with the portable fragment PSO of the same FFPKeyPS.
  hash = mix(hash, static_cast<u64>(key.tileFfpBaseColor));
  // R-BACK-12.22 / 12.23: argbuf-hybrid mode participates in the key
  // hash so Stage 1 and Stage 2 PSOs of the same shader live in
  // distinct cache slots.
  hash = mix(hash, static_cast<u64>(key.argbufHybridMode));
  hash = mix(hash, static_cast<u64>(key.argbufDirectCbufMode));
  // R-BACK-12.22..12.26 (resource-array sub-mode): the texture/sampler
  // resource-array sub-bit participates in the key hash so a Stage 2
  // constants-only PSO and a Stage 2 resource-array PSO of the same
  // shader live in distinct cache slots — keeps the default lane
  // byte-identical.
  hash = mix(hash, static_cast<u64>(key.argbufResourceArray));
  // gap_d3d9 B.3: the LOD-bias gate bit participates in the key hash so the
  // bias-on (slot-4 + bias()) and bias-off (plain sample) variants of the same
  // shader hit distinct cache slots.
  hash = mix(hash, static_cast<u64>(key.samplerLodBias));
  // Diagnostic depth-only backend-shape probe: fragmentless and ordinary
  // draw PSOs must never alias even when their shader/input state matches.
  hash = mix(hash, static_cast<u64>(key.fragmentlessDepthOnly));
  hash = mix(hash, key.vsOutLayoutKey);
  hash = mix(hash, key.x8AlphaOneTextureMask);
  hash = mix(hash, key.sampleCount);
  for (auto type : key.textureTypes) {
    hash = mix(hash, type);
  }
  for (auto fmt : key.colorFormats) {
    hash = mix(hash, fmt);
  }
  for (const auto& b : key.blend) {
    hash = mix(hash, static_cast<u64>(b.blendingEnabled));
    hash = mix(hash, b.rgbBlendOperation);
    hash = mix(hash, b.alphaBlendOperation);
    hash = mix(hash, b.sourceRGBBlendFactor);
    hash = mix(hash, b.destinationRGBBlendFactor);
    hash = mix(hash, b.sourceAlphaBlendFactor);
    hash = mix(hash, b.destinationAlphaBlendFactor);
    hash = mix(hash, b.colorWriteMask);
    hash = mix(hash, b.pixelFormat);
  }
  hash = mix(hash, key.depthFormat);
  hash = mix(hash, key.stencilFormat);
  if (measure) {
    perf::countShaderVariantKeyHashCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count()));
  }
  return static_cast<std::size_t>(hash);
}

void logFragmentlessDepthOnlyProbeDecision(const ShaderVariantKey& key,
                                           bool accepted,
                                           const char* reason) {
  static std::mutex mutex;
  static std::unordered_set<u64> logged;
  const auto keyHash = static_cast<u64>(ShaderVariantKeyHash{}(key));
  const u64 logKey = (keyHash << 1u) ^ (accepted ? 1u : 0u);
  {
    std::lock_guard lock(mutex);
    if (!logged.insert(logKey).second) {
      return;
    }
  }
  util::logf(util::LogLevel::Warn,
             "dxmt9-pipeline-cache",
             "fragmentless depth-only probe %s: key=0x%llx reason=%s vsout=0x%x alpha_test=%u alpha_to_coverage=%u tile_ffp=%u",
             accepted ? "accepted" : "rejected",
             static_cast<unsigned long long>(keyHash),
             reason ? reason : "unknown",
             key.vsOutLayoutKey,
             key.alphaTest ? 1u : 0u,
             key.alphaToCoverage ? 1u : 0u,
             key.tileFfpMode ? 1u : 0u);
}

std::size_t DepthStencilKeyHash::operator()(const DepthStencilKey& key) const noexcept {
  u64 hash = kFnvOffset;
  hash = mix(hash, static_cast<u64>(key.depthEnable));
  hash = mix(hash, static_cast<u64>(key.depthWrite));
  hash = mix(hash, key.depthFunc);
  StencilFaceKeyHash faceHash{};
  hash = mix(hash, static_cast<u64>(faceHash(key.front)));
  hash = mix(hash, static_cast<u64>(faceHash(key.back)));
  return static_cast<std::size_t>(hash);
}

std::size_t SamplerKeyHash::operator()(const SamplerKey& key) const noexcept {
  u64 hash = kFnvOffset;
  hash = mix(hash, key.states.hash);
  hash = mix(hash, key.states.count);
  hash = mix(hash, static_cast<u64>(key.states.overflow));
  const auto count = std::min<std::size_t>(key.states.count, key.states.entries.size());
  for (std::size_t i = 0; i < count; ++i) {
    hash = mix(hash, key.states.entries[i].state);
    hash = mix(hash, key.states.entries[i].value);
  }
  hash = mix(hash, key.lodMinClampBits);
  hash = mix(hash, static_cast<u64>(key.supportArgumentBuffers));
  return static_cast<std::size_t>(hash);
}

WMT::Reference<WMT::DepthStencilState> Cache::depthStencilStateFor(WMT::Device& device,
                                                                     const DepthStencilKey& key) {
  return depthStencilStateHandleFor(device, key).state;
}

DepthStencilLookup Cache::depthStencilStateHandleFor(WMT::Device& device,
                                                     const DepthStencilKey& key) {
  std::lock_guard lock(mutex);
  if (auto it = depth.find(key); it != depth.end()) {
    return DepthStencilLookup{
        .state = it->second,
        .handle = internDepthStencilHandleLocked(*this, key, it->second),
    };
  }
  WMTDepthStencilInfo info{};
  info.depth_compare_function =
      static_cast<WMTCompareFunction>(convert::toCompareFunction(key.depthFunc));
  info.depth_write_enabled = key.depthEnable && key.depthWrite;
  auto applyFace = [](WMTStencilInfo& stencilInfo, const StencilFaceKey& face) {
    stencilInfo.enabled = face.enabled;
    stencilInfo.stencil_compare_function =
        static_cast<WMTCompareFunction>(convert::toCompareFunction(face.compareFunction));
    stencilInfo.stencil_fail_op =
        static_cast<WMTStencilOperation>(convert::toStencilOperation(face.failureOperation));
    stencilInfo.depth_fail_op =
        static_cast<WMTStencilOperation>(convert::toStencilOperation(face.depthFailureOperation));
    stencilInfo.depth_stencil_pass_op =
        static_cast<WMTStencilOperation>(convert::toStencilOperation(face.passOperation));
    stencilInfo.read_mask = static_cast<uint8_t>(face.readMask);
    stencilInfo.write_mask = static_cast<uint8_t>(face.writeMask);
  };
  if (key.front.enabled || key.back.enabled) {
    applyFace(info.front_stencil, key.front);
    applyFace(info.back_stencil, key.back.enabled ? key.back : key.front);
  }
  auto state = device.newDepthStencilState(info);
  auto [it, inserted] = depth.emplace(key, state);
  (void)inserted;
  return DepthStencilLookup{
      .state = it->second,
      .handle = internDepthStencilHandleLocked(*this, key, it->second),
  };
}

WMT::Reference<WMT::DepthStencilState>
Cache::depthStencilStateForHandle(core::DepthStencilHandle handle,
                                  HandleLookupContext context) {
  auto snapshot = std::atomic_load_explicit(&depthSlotSnapshot,
                                            std::memory_order_acquire);
  if (!handle.valid()) {
    return {};
  }
  if (!snapshot) {
    logStaleDepthHandle(handle, context, "no-snapshot", 0u);
    return {};
  }
  if (handle.slot >= snapshot->size()) {
    logStaleDepthHandle(handle, context, "slot-out-of-range", snapshot->size());
    return {};
  }
  const auto& slot = (*snapshot)[handle.slot];
  if (!slot.occupied || slot.generation != handle.generation) {
    logStaleDepthHandle(handle, context,
                        slot.occupied ? "generation-mismatch" : "empty-slot",
                        snapshot->size(), &slot);
    return {};
  }
  return slot.state;
}

WMT::Reference<WMT::SamplerState>
Cache::samplerStateFor(WMT::Reference<WMT::Device> device,
                       const core::FlatStateSet<core::kMaxSamplerStates>& states,
                       float lodMinClamp,
                       bool supportArgumentBuffers) {
  SamplerKey key{
      .states = states,
      .lodMinClampBits = std::bit_cast<u32>(lodMinClamp),
      .supportArgumentBuffers = supportArgumentBuffers,
  };
  std::lock_guard lock(mutex);
  if (auto it = sampler.find(key); it != sampler.end()) {
    return it->second;
  }
  auto info = dxmt9::encoders::makeSamplerInfo(states, lodMinClamp);
  info.support_argument_buffers = supportArgumentBuffers;
  DXMT_ASSERT(device && "samplerStateFor called with stale/null Metal device handle");
  auto state = device.newSamplerState(info);
  sampler.emplace(std::move(key), state);
  return state;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildFillPipeline(WMT::Reference<WMT::Device> device,
                                const core::ColorRGBA& color,
                                u32 pixelFormat,
                                WMT::Reference<WMT::BinaryArchive>* archive,
                                const std::string* archivePath) {
  (void)archivePath;
  const ShaderVariantKey key = detail::makeFillPipelineKey(color, pixelFormat);
  std::lock_guard lock(mutex);
  if (auto it = fill.find(key); it != fill.end()) {
    perf::countPipelineCacheHit(perf::PipelineKind::Fill);
    return it->second.future;
  }
  perf::countPipelineCacheMiss(perf::PipelineKind::Fill);
  auto shared = submitPipelineBuild([device, color, pixelFormat, archive]() mutable {
    auto vsLib = shaders::makeLibrary(device, shaders::makeGenericVertexSource(shaders::makeHash("fill")));
    auto fsLib = shaders::makeLibrary(device, shaders::makeGenericFragmentSource(color, shaders::makeHash("fill")));
    if (!vsLib || !fsLib) {
      return WMT::Reference<WMT::RenderPipelineState>{};
    }
    auto vs = vsLib.newFunction("dxmt9_vs");
    auto fs = fsLib.newFunction("dxmt9_fs");
    WMTRenderPipelineInfo info{};
    info.max_tessellation_factor = 1;
    info.vertex_function = vs.handle;
    info.fragment_function = fs.handle;
    info.colors[0].pixel_format = static_cast<WMTPixelFormat>(pixelFormat);
    info.colors[0].write_mask = WMTColorWriteMaskAll;
    info.rasterization_enabled = true;
    info.raster_sample_count = 1;
    if (archive && *archive) info.binary_archive_for_serialization = (*archive).handle;
    WMT::Error err{};
    perf::countPipelineBuild(perf::PipelineKind::Fill);
    auto pso = device.newRenderPipelineState(info, err);
    if (pso) {
      const unsigned colorPacked =
          (static_cast<unsigned>(std::clamp(color.r, 0.0f, 1.0f) * 255) << 24) |
          (static_cast<unsigned>(std::clamp(color.g, 0.0f, 1.0f) * 255) << 16) |
          (static_cast<unsigned>(std::clamp(color.b, 0.0f, 1.0f) * 255) <<  8) |
          (static_cast<unsigned>(std::clamp(color.a, 0.0f, 1.0f) * 255));
      pso.setLabel(labels::makeLabelStringFmt("pso_fill_fmt%u_rgba0x%08x",
                                                static_cast<unsigned>(pixelFormat),
                                                colorPacked));
    }
    return pso;
  });
  fill.emplace(key, Entry{shared});
  return shared;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildStretchPipeline(WMT::Reference<WMT::Device> device,
                                   const core::StretchRectDesc& stretch,
                                   u32 pixelFormat,
                                   WMT::Reference<WMT::BinaryArchive>* archive,
                                   const std::string* archivePath) {
  (void)archivePath;
  ShaderVariantKey key{};
  key.hash = stretch.linear ? 1u : 0u;
  key.textured = true;
  key.linear = stretch.linear;
  key.sampleCount = std::max(1u, stretch.destinationSampleCount);
  key.colorFormats[0] = pixelFormat;
  key.blend[0].pixelFormat = pixelFormat;
  std::lock_guard lock(mutex);
  if (auto it = this->stretch.find(key); it != this->stretch.end()) {
    perf::countPipelineCacheHit(perf::PipelineKind::Stretch);
    return it->second.future;
  }
  perf::countPipelineCacheMiss(perf::PipelineKind::Stretch);
  const u32 sampleCountVal = key.sampleCount;
  auto shared = submitPipelineBuild([device, sampleCountVal, pixelFormat, archive]() mutable {
    auto vsLib = shaders::makeLibrary(device, shaders::makeTexturedVertexSource(shaders::makeHash("stretch")));
    auto fsLib = shaders::makeLibrary(device, shaders::makeTexturedFragmentSource(shaders::makeHash("stretch")));
    if (!vsLib || !fsLib) return WMT::Reference<WMT::RenderPipelineState>{};
    auto vs = vsLib.newFunction("dxmt9_vs");
    auto fs = fsLib.newFunction("dxmt9_fs");
    WMTRenderPipelineInfo info{};
    info.max_tessellation_factor = 1;
    info.vertex_function = vs.handle;
    info.fragment_function = fs.handle;
    info.raster_sample_count = sampleCountVal;
    info.colors[0].pixel_format = static_cast<WMTPixelFormat>(pixelFormat);
    info.colors[0].write_mask = WMTColorWriteMaskAll;
    info.rasterization_enabled = true;
    if (archive && *archive) info.binary_archive_for_serialization = (*archive).handle;
    WMT::Error err{};
    perf::countPipelineBuild(perf::PipelineKind::Stretch);
    auto pso = device.newRenderPipelineState(info, err);
    if (pso) {
      pso.setLabel(labels::makeLabelStringFmt(
          "pso_stretch_fmt%u_msaa%u",
          static_cast<unsigned>(pixelFormat),
          static_cast<unsigned>(sampleCountVal)));
    }
    return pso;
  });
  this->stretch.emplace(key, Entry{shared});
  return shared;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildDrawPipeline(WMT::Reference<WMT::Device> device,
                                const ShaderVariantKey& key,
                                drawshader::ShaderSourceContext shaderSource,
                                WMT::Reference<WMT::BinaryArchive>* archive,
                                const std::string* archivePath) {
  return getOrBuildDrawPipelineHandle(device, key, std::move(shaderSource),
                                      archive, archivePath).future;
}

DrawPipelineLookup
Cache::getOrBuildDrawPipelineHandle(WMT::Reference<WMT::Device> device,
                                    const ShaderVariantKey& key,
                                    drawshader::ShaderSourceContext shaderSource,
                                    WMT::Reference<WMT::BinaryArchive>* archive,
                                    const std::string* archivePath) {
  (void)archivePath;
  const ShaderVariantKey probeKey = makeShaderVariantProbeKey(key);
  {
    std::lock_guard lock(mutex);
    if (auto probe = this->drawProbe.find(probeKey); probe != this->drawProbe.end()) {
      if (auto it = this->draw.find(probe->second); it != this->draw.end()) {
        perf::countPipelineCacheHit(perf::PipelineKind::Draw);
        return DrawPipelineLookup{
            .future = it->second.future,
            .handle = internDrawHandleLocked(*this, probe->second, it->second),
        };
      }
    }
  }

  ShaderVariantKey sourceKey = key;
  // R-BACK-13.3 / 13.6: when the variant key carries tile_ffp_mode = true
  // the cache builds an MTLTileRenderPipelineState via
  // makeFfpTilePixelSource() instead of the standard fragment PSO. The
  // resulting Reference<RenderPipelineState> is opaque to callers; the
  // encoder switches between setRenderPipelineState() and
  // setTileRenderPipelineState() based on the same bit on the key.
  if (key.tileFfpMode) {
    core::FfpPixelKey psKey{};
    if (shaderSource.pixelShader.kind == core::ShaderRef::Kind::FixedFunctionPixel &&
        shaderSource.pixelShader.pixelKey.has_value()) {
      psKey = *shaderSource.pixelShader.pixelKey;
    }
    std::string tileSource;
    try {
      tileSource = ffp::makeFfpTilePixelSource(psKey, shaderSource, key.colorFormats[0]);
    } catch (const std::exception& ex) {
      util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
                 "tile shader source generation failed: %s variant=0x%llx",
                 ex.what(),
                 static_cast<unsigned long long>(key.hash));
      perf::countPipelineBuildFailDraw();
      return DrawPipelineLookup{.future = makeReadyPipelineFuture(), .handle = {}};
    } catch (...) {
      util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
                 "tile shader source generation failed: unknown exception variant=0x%llx",
                 static_cast<unsigned long long>(key.hash));
      perf::countPipelineBuildFailDraw();
      return DrawPipelineLookup{.future = makeReadyPipelineFuture(), .handle = {}};
    }
    sourceKey.tileSourceHash = shaders::makeHash(tileSource);

    std::lock_guard lock(mutex);
    if (auto it = this->draw.find(sourceKey); it != this->draw.end()) {
      this->drawProbe[probeKey] = sourceKey;
      perf::countPipelineCacheHit(perf::PipelineKind::Draw);
      return DrawPipelineLookup{
          .future = it->second.future,
          .handle = internDrawHandleLocked(*this, sourceKey, it->second),
      };
    }
    auto tileLibrary =
        getOrSubmitSourceLibraryLocked(*this, device, sourceKey.tileSourceHash, tileSource);
    perf::countPipelineCacheMiss(perf::PipelineKind::Draw);
    auto shared = submitPipelineBuild(
        [device, sourceKey, tileLibrary, archive]() mutable {
          auto tileLib = tileLibrary.get();
          if (!tileLib) {
            countPipelineBuildFailure(PipelineBuildFailureStage::Library);
            logPipelineBuildFailureOnce("tile-library", sourceKey, sourceKey.tileSourceHash);
            return WMT::Reference<WMT::RenderPipelineState>{};
          }
          auto tileFn = tileLib.newFunction("ffp_tile");
          if (!tileFn) {
            countPipelineBuildFailure(PipelineBuildFailureStage::Function);
            logPipelineBuildFailureOnce("tile-function", sourceKey, sourceKey.tileSourceHash);
            return WMT::Reference<WMT::RenderPipelineState>{};
          }
          WMTTileRenderPipelineDescriptor desc{};
          desc.tile_function = tileFn.handle;
          desc.raster_sample_count = std::max(1u, sourceKey.sampleCount);
          desc.threadgroup_size_matches_tile_size = 1u;
          desc.max_total_threads_per_threadgroup = 0u;
          std::uint32_t attachmentCount = 0;
          for (std::size_t i = 0; i < core::kMaxRenderTargets && i < 8; ++i) {
            desc.color_attachment_pixel_formats[i] =
                static_cast<WMTPixelFormat>(sourceKey.colorFormats[i]);
            if (sourceKey.colorFormats[i] != 0u) {
              attachmentCount = static_cast<std::uint32_t>(i + 1u);
            }
          }
          for (std::size_t i = core::kMaxRenderTargets; i < 8; ++i) {
            desc.color_attachment_pixel_formats[i] = WMTPixelFormatInvalid;
          }
          desc.color_attachment_count = attachmentCount;
          WMT::Error err{};
          perf::countPipelineBuild(perf::PipelineKind::Draw);
          perf::countColdCompileAfterWarm();
          traceDrawPipelineBuild("before-tile-pso",
                                 sourceKey,
                                 sourceKey.tileSourceHash);
          auto pso = device.newRenderPipelineState(desc, err);
          traceDrawPipelineBuild(pso ? "after-tile-pso-ok" : "after-tile-pso-fail",
                                 sourceKey,
                                 sourceKey.tileSourceHash);
          if (!pso) {
            countPipelineBuildFailure(PipelineBuildFailureStage::Pso);
            logPipelineBuildFailureOnce("tile-pso", sourceKey, sourceKey.tileSourceHash);
          }
          if (pso) {
            pso.setLabel(labels::makeLabelStringFmt(
                "pso_tile_ffp_h0x%llx_color0fmt%u",
                static_cast<unsigned long long>(sourceKey.hash),
                static_cast<unsigned>(sourceKey.colorFormats[0])));
          }
          (void)archive;
          return pso;
        });
    auto [it, inserted] = this->draw.emplace(sourceKey, Entry{shared});
    (void)inserted;
    this->drawProbe[probeKey] = sourceKey;
    return DrawPipelineLookup{
        .future = shared,
        .handle = internDrawHandleLocked(*this, sourceKey, it->second),
    };
  }
  auto sources = detail::makeContainedDrawShaderSources(shaderSource, key.hash);
  if (!sources) {
    perf::countPipelineBuildFailDraw();
    return DrawPipelineLookup{.future = makeReadyPipelineFuture(), .handle = {}};
  }
  sourceKey.vertexSourceHash = sources->vertexHash;
  sourceKey.fragmentSourceHash = sources->fragmentHash;
  {
    std::lock_guard lock(mutex);
    if (auto it = this->draw.find(sourceKey); it != this->draw.end()) {
      this->drawProbe[probeKey] = sourceKey;
      perf::countPipelineCacheHit(perf::PipelineKind::Draw);
      return DrawPipelineLookup{
          .future = it->second.future,
          .handle = internDrawHandleLocked(*this, sourceKey, it->second),
      };
    }
    auto vertexLibrary =
        getOrSubmitSourceLibraryLocked(*this, device, sources->vertexHash, sources->vertex);
    std::shared_future<WMT::Reference<WMT::Library>> fragmentLibrary;
    if (!sourceKey.fragmentlessDepthOnly) {
      fragmentLibrary =
          getOrSubmitSourceLibraryLocked(*this, device, sources->fragmentHash, sources->fragment);
    }
    perf::countPipelineCacheMiss(perf::PipelineKind::Draw);
    auto shared = submitPipelineBuild(
        [device, sourceKey, vertexLibrary, fragmentLibrary, archive]() mutable {
          auto vsLib = vertexLibrary.get();
          WMT::Reference<WMT::Library> fsLib{};
          if (!sourceKey.fragmentlessDepthOnly) {
            fsLib = fragmentLibrary.get();
          }
          if (!vsLib || (!sourceKey.fragmentlessDepthOnly && !fsLib)) {
            countPipelineBuildFailure(PipelineBuildFailureStage::Library);
            logPipelineBuildFailureOnce(!vsLib ? "vertex-library" : "fragment-library",
                                        sourceKey,
                                        !vsLib ? sourceKey.vertexSourceHash
                                               : sourceKey.fragmentSourceHash);
            return WMT::Reference<WMT::RenderPipelineState>{};
          }
          auto vs = vsLib.newFunction("dxmt9_vs");
          WMT::Reference<WMT::Function> fs{};
          if (!sourceKey.fragmentlessDepthOnly) {
            fs = fsLib.newFunction("dxmt9_fs");
          }
          if (!vs || (!sourceKey.fragmentlessDepthOnly && !fs)) {
            countPipelineBuildFailure(PipelineBuildFailureStage::Function);
            logPipelineBuildFailureOnce(!vs ? "vertex-function" : "fragment-function",
                                        sourceKey,
                                        !vs ? sourceKey.vertexSourceHash
                                            : sourceKey.fragmentSourceHash);
            return WMT::Reference<WMT::RenderPipelineState>{};
          }
          WMTRenderPipelineInfo info{};
          info.max_tessellation_factor = 1;
          info.vertex_function = vs.handle;
          info.fragment_function =
              sourceKey.fragmentlessDepthOnly ? NULL_OBJECT_HANDLE : fs.handle;
          info.raster_sample_count = std::max(1u, sourceKey.sampleCount);
          info.alpha_to_coverage_enabled = sourceKey.alphaToCoverage;
          info.depth_pixel_format = static_cast<WMTPixelFormat>(sourceKey.depthFormat);
          info.stencil_pixel_format = static_cast<WMTPixelFormat>(sourceKey.stencilFormat);
          info.rasterization_enabled = true;
          if (archive && *archive) {
            info.binary_archive_for_serialization = (*archive).handle;
          }
          for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
            auto& ca = info.colors[i];
            ca.pixel_format = static_cast<WMTPixelFormat>(sourceKey.colorFormats[i]);
            ca.blending_enabled = sourceKey.blend[i].blendingEnabled;
            ca.rgb_blend_operation = convert::toBlendOperation(sourceKey.blend[i].rgbBlendOperation);
            ca.alpha_blend_operation =
                convert::toBlendOperation(sourceKey.blend[i].alphaBlendOperation);
            ca.src_rgb_blend_factor =
                convert::toBlendFactor(sourceKey.blend[i].sourceRGBBlendFactor);
            ca.dst_rgb_blend_factor =
                convert::toBlendFactor(sourceKey.blend[i].destinationRGBBlendFactor);
            ca.src_alpha_blend_factor =
                convert::toBlendFactor(sourceKey.blend[i].sourceAlphaBlendFactor, true);
            ca.dst_alpha_blend_factor =
                convert::toBlendFactor(sourceKey.blend[i].destinationAlphaBlendFactor, true);
            ca.write_mask = convert::toColorWriteMask(sourceKey.blend[i].colorWriteMask);
          }
          WMT::Error err{};
          perf::countPipelineBuild(perf::PipelineKind::Draw);
          // R-BACK-3.8 — every Draw PSO that lands in this closure is a
          // miss against the prewarmed archive. The counter starts at zero
          // after a successful Full prewarm and stays low when the archive
          // is hot; a high value indicates the prewarm path missed (file
          // missing, schema drift, lock_busy, or content mismatch with the
          // current emitter / variant key).
          perf::countColdCompileAfterWarm();
          traceDrawPipelineBuild("before-render-pso",
                                 sourceKey,
                                 sourceKey.vertexSourceHash ^
                                     (sourceKey.fragmentSourceHash << 1u));
          auto pso = device.newRenderPipelineState(info, err);
          traceDrawPipelineBuild(pso ? "after-render-pso-ok" : "after-render-pso-fail",
                                 sourceKey,
                                 sourceKey.vertexSourceHash ^
                                     (sourceKey.fragmentSourceHash << 1u));
          if (!pso) {
            countPipelineBuildFailure(PipelineBuildFailureStage::Pso);
            logPipelineBuildFailureOnce("render-pso", sourceKey,
                                        sourceKey.vertexSourceHash ^
                                            (sourceKey.fragmentSourceHash << 1u));
          }
          if (pso) {
            pso.setLabel(labels::makeLabelStringFmt(
                "pso_draw_h0x%llx_vso0x%03x_msaa%u_color0fmt%u",
                static_cast<unsigned long long>(sourceKey.hash),
                static_cast<unsigned>(sourceKey.vsOutLayoutKey),
                static_cast<unsigned>(std::max(1u, sourceKey.sampleCount)),
                static_cast<unsigned>(sourceKey.colorFormats[0])));
          }
          return pso;
        });
    auto [it, inserted] = this->draw.emplace(sourceKey, Entry{shared});
    (void)inserted;
    this->drawProbe[probeKey] = sourceKey;
    return DrawPipelineLookup{
        .future = shared,
        .handle = internDrawHandleLocked(*this, sourceKey, it->second),
    };
  }
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::drawPipelineForHandle(core::PsoHandle handle,
                             HandleLookupContext context) {
  auto snapshot = std::atomic_load_explicit(&drawSlotSnapshot,
                                            std::memory_order_acquire);
  if (!handle.valid()) {
    return makeReadyPipelineFuture();
  }
  if (!snapshot) {
    logStaleDrawHandle(handle, context, "no-snapshot", 0u);
    return makeReadyPipelineFuture();
  }
  if (handle.slot >= snapshot->size()) {
    logStaleDrawHandle(handle, context, "slot-out-of-range", snapshot->size());
    return makeReadyPipelineFuture();
  }
  const auto& slot = (*snapshot)[handle.slot];
  if (!slot.occupied || slot.generation != handle.generation) {
    logStaleDrawHandle(handle, context,
                       slot.occupied ? "generation-mismatch" : "empty-slot",
                       snapshot->size(), &slot);
    return makeReadyPipelineFuture();
  }
  perf::countPipelineCacheHit(perf::PipelineKind::Draw);
  return slot.entry.future;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
buildPresentPipeline(WMT::Reference<WMT::Device> device, bool opaqueAlpha,
                     WMT::Reference<WMT::BinaryArchive>* archive,
                     const std::string* archivePath) {
  (void)archivePath;
  return submitPipelineBuild([device, opaqueAlpha, archive]() mutable {
    auto vsLib = shaders::makeLibrary(device, shaders::makeTexturedVertexSource(shaders::makeHash("present")));
    auto fsLib = shaders::makeLibrary(device, shaders::makeTexturedFragmentSource(
                                          shaders::makeHash(opaqueAlpha ? "present-opaque" : "present"),
                                          opaqueAlpha));
    if (!vsLib || !fsLib) return WMT::Reference<WMT::RenderPipelineState>{};
    auto vs = vsLib.newFunction("dxmt9_vs");
    auto fs = fsLib.newFunction("dxmt9_fs");
    WMTRenderPipelineInfo info{};
    info.max_tessellation_factor = 1;
    info.vertex_function = vs.handle;
    info.fragment_function = fs.handle;
    info.raster_sample_count = 1;
    info.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
    info.colors[0].write_mask = WMTColorWriteMaskAll;
    info.rasterization_enabled = true;
    if (archive && *archive) info.binary_archive_for_serialization = (*archive).handle;
    WMT::Error err{};
    perf::countPipelineBuild(perf::PipelineKind::Present);
    auto pso = device.newRenderPipelineState(info, err);
    if (pso) {
      pso.setLabel(labels::makeLabelStringFmt("pso_present_%s",
                                                opaqueAlpha ? "opaque" : "alpha"));
    }
    return pso;
  });
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
buildGammaApplyPresentPipeline(WMT::Reference<WMT::Device> device, bool opaqueAlpha,
                               WMT::Reference<WMT::BinaryArchive>* archive,
                               const std::string* archivePath) {
  (void)archivePath;
  return submitPipelineBuild([device, opaqueAlpha, archive]() mutable {
    auto vsLib = shaders::makeLibrary(device, shaders::makeTexturedVertexSource(shaders::makeHash("present-gamma")));
    auto fsLib = shaders::makeLibrary(device, shaders::makeGammaApplyFragmentSource(
                                          shaders::makeHash(opaqueAlpha ? "present-gamma-opaque" : "present-gamma"),
                                          opaqueAlpha));
    if (!vsLib || !fsLib) return WMT::Reference<WMT::RenderPipelineState>{};
    auto vs = vsLib.newFunction("dxmt9_vs");
    auto fs = fsLib.newFunction("dxmt9_fs");
    WMTRenderPipelineInfo info{};
    info.max_tessellation_factor = 1;
    info.vertex_function = vs.handle;
    info.fragment_function = fs.handle;
    info.raster_sample_count = 1;
    info.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
    info.colors[0].write_mask = WMTColorWriteMaskAll;
    info.rasterization_enabled = true;
    if (archive && *archive) info.binary_archive_for_serialization = (*archive).handle;
    WMT::Error err{};
    perf::countPipelineBuild(perf::PipelineKind::Present);
    auto pso = device.newRenderPipelineState(info, err);
    if (pso) {
      pso.setLabel(labels::makeLabelStringFmt("pso_present_gamma_%s",
                                                opaqueAlpha ? "opaque" : "alpha"));
    }
    return pso;
  });
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildDrawPipelineForState(WMT::Reference<WMT::Device> device,
                                      const core::BackendLimits& limits,
                                      resources::Pool& pool,
                                      core::FlatDrawStateView state,
                                      WMT::Reference<WMT::BinaryArchive>* archive,
                                      const std::string* archivePath,
                                      bool tileFfpMode,
                                      bool argbufHybridMode,
                                      bool argbufResourceArray,
                                      bool argbufDirectCbufMode,
                                      bool disableAlphaBlend,
                                      std::optional<bool> forceTextureWhiteOverride,
                                      bool fragmentlessDepthOnly) {
  return getOrBuildDrawPipelineHandleForState(
      device, limits, pool, state, archive, archivePath, tileFfpMode,
      argbufHybridMode, argbufResourceArray, argbufDirectCbufMode,
      disableAlphaBlend,
      forceTextureWhiteOverride, fragmentlessDepthOnly).future;
}

ResolvedDrawPipelineState
Cache::resolveDrawPipelineState(const core::BackendLimits& limits,
                                resources::Pool& pool,
                                core::FlatDrawStateView state,
                                bool tileFfpMode,
                                bool argbufHybridMode,
                                bool argbufResourceArray,
                                bool argbufDirectCbufMode,
                                bool disableAlphaBlend,
                                std::optional<bool> forceTextureWhiteOverride,
                                bool fragmentlessDepthOnly) {
  std::array<u32, core::kMaxRenderTargets> colorFormats{};
  std::array<BlendAttachmentKey, core::kMaxRenderTargets> blendAttachments{};
  u32 depthFormat = 0;
  u32 stencilFormat = 0;
  {
    ScopedPerfCounter scope(
        perf::countEncodeSlotPsoPrefetchDrawResolveFormatCpuTime);
    const bool srgbWrite =
        core::flatStateOr(state.hot->renderStates, core::RS_SRGB_WRITE_ENABLE, 0u) != 0;
    auto resolvePixelFormat = [&](core::Handle handle) -> u32 {
      if (!handle) {
        return 0;
      }
      if (auto* surface = pool.findSurface(handle.value); surface) {
        return static_cast<u32>(
            dxmt9::convert::toPixelFormat(surface->desc.format, limits, srgbWrite));
      }
      return 0;
    };

    blendAttachments =
        detail::makeBlendAttachmentKeys(state, debugForceVisibleDraw(), disableAlphaBlend);
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      colorFormats[i] = resolvePixelFormat(state.hot->colorAttachments[i].handle);
      blendAttachments[i].pixelFormat = colorFormats[i];
    }
    if (state.hot->depthStencil.handle) {
      if (auto* surface = pool.findSurface(state.hot->depthStencil.handle.value);
          surface && surface->desc.depthStencil) {
        const auto pixelFormat =
            static_cast<u32>(dxmt9::convert::toPixelFormat(surface->desc.format, limits));
        depthFormat =
            dxmt9::convert::formatHasDepthAspect(surface->desc.format) ? pixelFormat : 0u;
        stencilFormat =
            dxmt9::convert::formatHasStencilAspect(surface->desc.format) ? pixelFormat : 0u;
      }
    }
  }
  bool forceTextureWhiteForDebug = false;
  ShaderVariantKey key{};
  {
    ScopedPerfCounter scope(
        perf::countEncodeSlotPsoPrefetchDrawResolveVariantKeyCpuTime);
    forceTextureWhiteForDebug =
        forceTextureWhiteOverride.value_or(envFlag("DXMT_FORCE_TEXTURE_WHITE"));
    key = makeShaderVariantKey(
        state, colorFormats, blendAttachments, depthFormat, stencilFormat,
        forceTextureWhiteOverride);
    // R-BACK-13.3: stamp the tile-FFP-mode bit onto the variant key so the
    // tile-stage and fragment-stage variants share an FFPKeyPS but land in
    // distinct cache entries.
    key.tileFfpMode = tileFfpMode;
    // R-BACK-12.22 / 12.23: stamp the argbuf-hybrid-mode bit so Stage 1
    // and Stage 2 variants of the same shader compile separate PSOs.
    key.argbufHybridMode = argbufHybridMode;
    // R-BACK-12.22..12.26 (resource-array sub-mode): stamp the sub-bit only
    // alongside argbufHybridMode so a stray true can never select the
    // resource-array prelude on a Stage 1 PSO.
    key.argbufResourceArray = argbufHybridMode && argbufResourceArray;
    key.argbufDirectCbufMode =
        argbufHybridMode && !key.argbufResourceArray && argbufDirectCbufMode;
  }
  drawshader::ShaderSourceContext shaderSource{};
  {
    ScopedPerfCounter scope(
        perf::countEncodeSlotPsoPrefetchDrawResolveShaderContextCpuTime);
    shaderSource =
        drawshader::makeShaderSourceContext(state.shaderContext(), *state.hot);
    // R-BACK-12.22..12.26 MSL routing — propagate the variant key bit into
    // the source-emitter context so FFP and DXBC->MSL bodies read uniforms
    // through `ArgbufLayout` at slot 30 instead of slots 0/3.
    shaderSource.argbufHybridMode = argbufHybridMode;
    shaderSource.argbufDirectCbufMode = key.argbufDirectCbufMode;
    // R-BACK-12.22..12.26 (resource-array sub-mode): propagate the sub-bit so
    // the emitters route texture/sampler reads through the slot-30 argbuf
    // arrays + alias block. Mirrors the key bit exactly.
    shaderSource.argbufResourceArray = key.argbufResourceArray;
    // gap_d3d9 B.3: propagate the LOD-bias gate bit so the FFP and DXBC->MSL
    // emitters declare the slot-4 SamplerLodBias param + thread bias() only when
    // a sampler carries a non-zero LOD bias. makeShaderVariantKey already
    // computed key.samplerLodBias from the same predicate the encoder bind reads.
    shaderSource.samplerLodBias = key.samplerLodBias;
    shaderSource.stripAlphaTestForDebug = envFlag("DXMT_DISABLE_ALPHA_TEST");
    shaderSource.stripFogForDebug = envFlag("DXMT_DISABLE_FOG");
    shaderSource.forceTextureWhiteForDebug = forceTextureWhiteForDebug;
  }
  {
    ScopedPerfCounter scope(
        perf::countEncodeSlotPsoPrefetchDrawResolveX8AlphaCpuTime);
    stampX8AlphaOneTextureMask(
        key, shaderSource,
        x8AlphaOneTextureMask(pool, *state.hot, key.textureMask));
  }
  {
    ScopedPerfCounter scope(
        perf::countEncodeSlotPsoPrefetchDrawResolveVsoutLayoutCpuTime);
    shaderSource.vsOutLayout = drawshader::resolveVSOutLayoutForShaderPair(shaderSource);
    key.vsOutLayoutKey = shaders::vsoutLayoutKey(shaderSource.vsOutLayout);
  }
  if (fragmentlessDepthOnly) {
    ScopedPerfCounter scope(
        perf::countEncodeSlotPsoPrefetchDrawResolveFragmentlessCpuTime);
    const char* rejectReason = nullptr;
    if (tileFfpMode) {
      rejectReason = "tile-ffp-mode";
    } else if (key.alphaTest) {
      rejectReason = "alpha-test";
    } else if (key.alphaToCoverage) {
      rejectReason = "alpha-to-coverage";
    } else if (!fragmentlessDepthOnlyShaderSafe(shaderSource)) {
      rejectReason = "fragment-side-effect";
    }
    fragmentlessDepthOnly = rejectReason == nullptr;
    if (fragmentlessDepthOnly &&
        !debug::probeFragmentlessDepthOnlyKeepVSOut()) {
      shaderSource.vsOutLayout = shaders::positionOnlyVSOutLayout();
      key.vsOutLayoutKey = shaders::vsoutLayoutKey(shaderSource.vsOutLayout);
    }
    key.fragmentlessDepthOnly = fragmentlessDepthOnly;
    logFragmentlessDepthOnlyProbeDecision(
        key, fragmentlessDepthOnly,
        fragmentlessDepthOnly
            ? (debug::probeFragmentlessDepthOnlyKeepVSOut()
                   ? "accepted-keep-vsout"
                   : "accepted")
            : rejectReason);
  }
  key.fragmentlessDepthOnly = fragmentlessDepthOnly;
  shaderSource.fragmentlessDepthOnly = fragmentlessDepthOnly;
  return ResolvedDrawPipelineState{
      .key = key,
      .shaderSource = std::move(shaderSource),
  };
}

DrawPipelineLookup
Cache::getOrBuildDrawPipelineHandleForState(WMT::Reference<WMT::Device> device,
                                            const core::BackendLimits& limits,
                                            resources::Pool& pool,
                                            core::FlatDrawStateView state,
                                            WMT::Reference<WMT::BinaryArchive>* archive,
                                            const std::string* archivePath,
                                            bool tileFfpMode,
                                            bool argbufHybridMode,
                                            bool argbufResourceArray,
                                            bool argbufDirectCbufMode,
                                            bool disableAlphaBlend,
                                            std::optional<bool> forceTextureWhiteOverride,
                                            bool fragmentlessDepthOnly) {
  auto resolved = resolveDrawPipelineState(
      limits, pool, state, tileFfpMode, argbufHybridMode, argbufResourceArray,
      argbufDirectCbufMode, disableAlphaBlend, forceTextureWhiteOverride,
      fragmentlessDepthOnly);
  return getOrBuildDrawPipelineHandle(device, resolved.key,
                                      std::move(resolved.shaderSource), archive,
                                      archivePath);
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildTileFfpBaseColorPipelineForState(WMT::Reference<WMT::Device> device,
                                                  const core::BackendLimits& limits,
                                                  resources::Pool& pool,
                                                  core::FlatDrawStateView state,
                                                  WMT::Reference<WMT::BinaryArchive>* archive,
                                                  const std::string* archivePath,
                                                  std::optional<bool> forceTextureWhiteOverride) {
  return getOrBuildTileFfpBaseColorPipelineHandleForState(
      device, limits, pool, state, archive, archivePath,
      forceTextureWhiteOverride).future;
}

DrawPipelineLookup
Cache::getOrBuildTileFfpBaseColorPipelineHandleForState(
    WMT::Reference<WMT::Device> device,
    const core::BackendLimits& limits,
    resources::Pool& pool,
    core::FlatDrawStateView state,
    WMT::Reference<WMT::BinaryArchive>* archive,
    const std::string* archivePath,
    std::optional<bool> forceTextureWhiteOverride) {
  // R-BACK-13.1 — assemble the same render-pipeline key as the portable
  // fragment path, then re-stamp it for the base-colour tile-FFP sub-variant:
  //   * tileFfpBaseColor = true   -> distinct cache slot + source-strip gate
  //   * tileFfpMode       = false -> this is an ordinary fragment PSO, not the
  //                                  tile-descriptor build
  //   * alphaToCoverage   = false -> the base draw must write every covered
  //                                  fragment so the tile kernel can post-
  //                                  process it; A2C belongs to the tile pass
  // and turn on stripFogAlphaTestForTileBase in the emitter context so
  // makeFfpPixelSource drops the fog blend and the alpha-test discard.
  const bool srgbWrite =
      core::flatStateOr(state.hot->renderStates, core::RS_SRGB_WRITE_ENABLE, 0u) != 0;
  auto resolvePixelFormat = [&](core::Handle handle) -> u32 {
    if (!handle) {
      return 0;
    }
    if (auto* surface = pool.findSurface(handle.value); surface) {
      return static_cast<u32>(
          dxmt9::convert::toPixelFormat(surface->desc.format, limits, srgbWrite));
    }
    return 0;
  };

  std::array<u32, core::kMaxRenderTargets> colorFormats{};
  auto blendAttachments = detail::makeBlendAttachmentKeys(state, debugForceVisibleDraw());
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    colorFormats[i] = resolvePixelFormat(state.hot->colorAttachments[i].handle);
    blendAttachments[i].pixelFormat = colorFormats[i];
  }
  u32 depthFormat = 0;
  u32 stencilFormat = 0;
  if (state.hot->depthStencil.handle) {
    if (auto* surface = pool.findSurface(state.hot->depthStencil.handle.value);
        surface && surface->desc.depthStencil) {
      const auto pixelFormat =
          static_cast<u32>(dxmt9::convert::toPixelFormat(surface->desc.format, limits));
      depthFormat = dxmt9::convert::formatHasDepthAspect(surface->desc.format) ? pixelFormat : 0u;
      stencilFormat =
          dxmt9::convert::formatHasStencilAspect(surface->desc.format) ? pixelFormat : 0u;
    }
  }
  const bool forceTextureWhiteForDebug =
      forceTextureWhiteOverride.value_or(envFlag("DXMT_FORCE_TEXTURE_WHITE"));
  auto key = makeShaderVariantKey(
      state, colorFormats, blendAttachments, depthFormat, stencilFormat,
      forceTextureWhiteOverride);
  key.tileFfpMode = false;
  key.tileFfpBaseColor = true;
  // A2C is applied by the tile pass (or, where the tile kernel cannot, the
  // draw stays portable via selectTileFfpForPass). The base draw must not
  // alpha-to-coverage, so force the descriptor bit off for this sub-variant.
  key.alphaToCoverage = false;
  drawshader::ShaderSourceContext shaderSource =
      drawshader::makeShaderSourceContext(state.shaderContext(), *state.hot);
  shaderSource.samplerLodBias = key.samplerLodBias;
  shaderSource.stripAlphaTestForDebug = envFlag("DXMT_DISABLE_ALPHA_TEST");
  shaderSource.stripFogForDebug = envFlag("DXMT_DISABLE_FOG");
  shaderSource.forceTextureWhiteForDebug = forceTextureWhiteForDebug;
  stampX8AlphaOneTextureMask(
      key, shaderSource,
      x8AlphaOneTextureMask(pool, *state.hot, key.textureMask));
  // R-BACK-13.1: drop fog blend + alpha-test discard from the emitted FFP
  // fragment; the tile kernel re-applies them over the rasterized base colour.
  shaderSource.stripFogAlphaTestForTileBase = true;
  shaderSource.vsOutLayout = drawshader::resolveVSOutLayoutForShaderPair(shaderSource);
  key.vsOutLayoutKey = shaders::vsoutLayoutKey(shaderSource.vsOutLayout);
  return getOrBuildDrawPipelineHandle(device, key, std::move(shaderSource),
                                      archive, archivePath);
}

TileFfpSelection classifyTileFfpForPass(core::FlatDrawStateView state, bool supportsApple3) {
  // R-BACK-13.5: tile-FFP is unconditionally off on non-Apple3.
  if (!supportsApple3) {
    return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::GpuFamily};
  }
  // Tile path only applies when the pixel shader is the fixed-function
  // FFP. A programmable shader has no FFP key to translate; portable.
  if (!state.hasShaderContext()) {
    return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::NotFfp};
  }
  const auto& shader = state.shaderContext();
  if (shader.pixelShader.kind != core::ShaderRef::Kind::FixedFunctionPixel ||
      !shader.pixelShader.pixelKey.has_value()) {
    return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::NotFfp};
  }
  if (shader.vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex &&
      shader.vertexShader.vertexKey.has_value() &&
      (shader.vertexShader.vertexKey->vertexBlend != 0 ||
       shader.vertexShader.vertexKey->indexedVertexBlend)) {
    return TileFfpSelection{
        TileFfpDecision::Portable,
        TileFfpFallbackReason::UnsupportedState,
    };
  }
  for (const auto& texture : state.hot->textures) {
    if (texture) {
      // The current tile-FFP kernel is a tile-memory alpha/fog transform,
      // not a replacement for a full fragment shader with interpolated
      // texcoords and texture sampling. Keep textured FFP draws on the
      // portable fragment path until tile/portable readback equality exists.
      return TileFfpSelection{
          TileFfpDecision::Portable,
          TileFfpFallbackReason::UnsupportedState,
      };
    }
  }
  const auto& key = *shader.pixelShader.pixelKey;
  const auto& rs = state.hot->renderStates;
  // RS_ALPHA_REF holds a 0..255 D3D9 byte. Normalize so the precision
  // boundary check uses the same float space as the shader.
  const u32 alphaRefRaw = core::flatStateOr(rs, core::RS_ALPHA_REF, 0u);
  const float alphaRefNorm = static_cast<float>(alphaRefRaw & 0xffu) / 255.0f;
  // R-BACK-13.7: A2C with PS-emitted alpha-test cannot be replicated
  // tile-side. Pull the bit from the same render-state slot the
  // ShaderVariantKey uses so the selector stays consistent with the PSO
  // key.
  // R-FORMAT-13: A2C is driven by the D3DRS_ADAPTIVETESS_Y FOURCC hack. Read
  // the same render-state slot makeShaderVariantKey uses so the tile-FFP
  // selector stays consistent with the PSO key: the ATOC token and the ATI
  // A2M1 alias enable it; 0 / A2M0 / any other value leave it disabled.
  const u32 adaptiveTessY = core::flatStateOr(rs, core::RS_ADAPTIVETESS_Y, 0u);
  const bool a2c =
      adaptiveTessY == core::kFourCcAtoc || adaptiveTessY == core::kFourCcA2M1;
  auto eligibility = ffp::classifyTileFfpEligibility(key, alphaRefNorm, a2c);
  switch (eligibility) {
    case ffp::TileFfpEligibility::Eligible:
      // The genuinely-eligible draw takes the tile lane under both `auto`
      // and `force` (`mode` is intentionally not re-checked here — there is
      // no soft heuristic left to override at this point).
      return TileFfpSelection{TileFfpDecision::Tile, TileFfpFallbackReason::None};
    case ffp::TileFfpEligibility::IneligiblePrecision:
      // R-BACK-13.* `force` deliberately does NOT promote a precision-unsafe
      // draw (alpha-ref out of [0,1] / Exp|Exp2 fog) to the tile lane — the
      // tile kernel cannot reproduce it bit-identically, so forcing it would
      // mis-emit. Stay portable even under DXMT9_TILE_FFP=force.
      return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::Precision};
    case ffp::TileFfpEligibility::IneligibleUnsupportedState:
      // Likewise A2C-with-alpha-test cannot be replicated tile-side; not
      // forceable.
      return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::UnsupportedState};
  }
  // Unreachable: classifyTileFfpEligibility is total over the enum.
  return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::Precision};
}

TileFfpSelection selectTileFfpForPass(core::FlatDrawStateView state, bool supportsApple3) {
  // R-BACK-13.* DXMT9_TILE_FFP escape hatch (off|force|auto). `off` is a
  // clean opt-out that routes every draw down the portable lane regardless
  // of eligibility — keep it ahead of the genuine gates so an A/B probe can
  // diff the two lanes for the SAME eligible draw. `force` deliberately does
  // NOT bypass the genuine ineligibility gates (GpuFamily / NotFfp /
  // textured / vertex-blend / precision / A2C): forcing the tile kernel onto
  // any of those would mis-emit. `force` therefore only guarantees the tile
  // lane for a draw that genuinely passes every gate — which is what makes it
  // the Tile half of the A/B against `off`'s Portable half for an eligible
  // non-textured FFP draw.
  const TileFfpModeOverride mode = tileFfpModeOverride();
  if (mode == TileFfpModeOverride::Off) {
    return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::None};
  }
  return classifyTileFfpForPass(state, supportsApple3);
}

ArgbufHybridDecision selectArgbufHybridForPass(core::FlatDrawStateView,
                                                 bool argbufHybridEnabled) {
  // Tier-2 argbuf + Apple3 is cached as a single bool on the pool. When
  // the gate fails the pass commits to Stage 1 and never switches
  // (R-BACK-12.22). When the gate holds, texture-free and texture-bound
  // draws use Stage 2 for uniform bindings. Texture/sampler resources stay
  // on the direct render-encoder lane.
  if (!argbufHybridEnabled) {
    return ArgbufHybridDecision::Stage1;
  }
  return ArgbufHybridDecision::Stage2;
}

bool argbufDirectCbufEnabled() noexcept {
  return argbufDirectCbufEnvEnabled();
}

ShaderVariantKey makeShaderVariantKey(core::FlatDrawStateView state,
                                       std::span<const u32> colorFormats,
                                       std::span<const BlendAttachmentKey> blendAttachments,
                                       u32 depthFormat,
                                       u32 stencilFormat,
                                       std::optional<bool> forceTextureWhiteOverride) {
  const auto& hot = *state.hot;
  const auto& shader = state.shaderContext();
  const auto& vertexDecl = shader.vertexDecl;
  const auto& vertexShader = shader.vertexShader;
  const auto& pixelShader = shader.pixelShader;
  ShaderVariantKey key{};
  key.emitterVersion = kShaderEmitterVersion;
  key.sourceLayoutVersion = kShaderSourceLayoutVersion;
  key.debugEnvSchemaVersion = kShaderDebugEnvSchemaVersion;
  key.debugEnvKey = currentShaderSourceDebugEnvKey(forceTextureWhiteOverride);
  const auto layout =
      vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex
          ? ffp::decodeFixedFunctionVertexLayout(vertexDecl)
          : std::optional<ffp::FixedFunctionVertexLayout>{};
  const u64 layoutHash = layout ? layout->hash : ffp::hashVertexDeclaration(vertexDecl);
  key.hash = vertexShader.hash ^ (pixelShader.hash << 1) ^ hot.clipPlaneMask ^ depthFormat ^
             (stencilFormat << 1) ^ (layoutHash << 1) ^ vertexDecl.fvf;
  key.textureMask =
      drawshader::activeFragmentTextureMaskForShader(pixelShader, hot.textureMask);
  key.textured = key.textureMask != 0;
  key.linear =
      core::flatStateOr(hot.samplerStates[0], core::SAMP_MIN_FILTER, 0u) == 2u ||
      core::flatStateOr(hot.samplerStates[0], core::SAMP_MAG_FILTER, 0u) == 2u;
  key.clipPlanes = hot.clipPlaneMask != 0;
  key.alphaTest = core::flatStateOr(hot.renderStates, core::RS_ALPHA_TEST_ENABLE, 0u) != 0;
  // R-FORMAT-13: alpha-to-coverage is driven by the cross-vendor render-state
  // hack on D3DRS_ADAPTIVETESS_Y. Writing the ATOC FOURCC (or the ATI A2M1
  // alias) enables it; 0, the A2M0 alias, or any other value leaves it
  // disabled and reverts the render state to its ordinary meaning. The bit is
  // mixed into ShaderVariantKeyHash, so enabled vs disabled draws hit distinct
  // PSO cache slots, and the backend stamps Metal alphaToCoverageEnabled from
  // it (the getOrBuildDrawPipeline build site sets info.alpha_to_coverage_enabled).
  const u32 adaptiveTessY = core::flatStateOr(hot.renderStates, core::RS_ADAPTIVETESS_Y, 0u);
  key.alphaToCoverage =
      adaptiveTessY == core::kFourCcAtoc || adaptiveTessY == core::kFourCcA2M1;
  // gap_d3d9 B.3: PSO-variant gate for D3DSAMP_MIPMAPLODBIAS. Set from the
  // single predicate so bias-on and bias-off draws hash to distinct PSOs and
  // the slot-4 SamplerLodBias param + bias() sampling are emitted only when a
  // sampler actually carries a non-zero LOD bias. The same predicate gates the
  // encoder's slot-4 bind, keeping declaration and binding in lockstep.
  key.samplerLodBias = state::anySamplerLodBiasNonzero(state);
  key.sampleCount = std::max(1u, hot.colorAttachments[0].sampleCount);
  for (std::size_t i = 0; i < core::kMaxTextureStages; ++i) {
    key.textureTypes[i] =
        core::flatStateOr(hot.textureStageStates[i], core::TSS_TEXTURE_TYPE,
                          static_cast<u32>(core::TextureType::TwoD));
  }
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    key.colorFormats[i] = i < colorFormats.size() ? colorFormats[i] : 0u;
    if (i < blendAttachments.size()) {
      key.blend[i] = blendAttachments[i];
    }
  }
  key.depthFormat = depthFormat;
  key.stencilFormat = stencilFormat;
  return key;
}

}  // namespace dxmt9::pipeline
