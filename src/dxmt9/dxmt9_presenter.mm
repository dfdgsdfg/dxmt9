#include "dxmt9_presenter.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9_metal_labels.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_device.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>

namespace dxmt9 {

namespace {

void attachCounterSampleBuffers(
    WMTRenderPassInfo& passInfo,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  constexpr std::size_t kMaxSampleBufferAttachments =
      sizeof(passInfo.sample_buffer_attachments) /
      sizeof(passInfo.sample_buffer_attachments[0]);
  const auto attachmentCount =
      std::min(sampleBufferAttachments.size(), kMaxSampleBufferAttachments);
  for (std::size_t i = 0; i < attachmentCount; ++i) {
    passInfo.sample_buffer_attachments[i] = sampleBufferAttachments[i];
  }
  passInfo.num_sample_buffer_attachments =
      static_cast<std::uint8_t>(attachmentCount);
}

bool envFlagSet(const char* env) {
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

bool layerDisplaySyncEnabled() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT9_LAYER_DISPLAY_SYNC");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return value;
}

// DXMT9_DISABLE_VSYNC=1 forces every present to use
// minimumPresentDuration=0 and CAMetalLayer.displaySyncEnabled=false,
// regardless of the D3D9 PresentationInterval the app requested. This is
// the production-side counterpart to setting D3D9 PresentationInterval=
// IMMEDIATE per-swapchain — a runtime override for benchmarking, perf
// triage, or user-controlled "vsync off" without modifying the D3D9 app.
// Default off. Set to "1" / "yes" / non-empty non-"0" to enable.
bool disableVsyncEnv() {
  static const bool value =
      ::dxmt9::resolveDisableVsync(std::getenv("DXMT9_DISABLE_VSYNC"));
  return value;
}

double presentRefreshHz() {
  static const double value = [] {
    const char* env = std::getenv("DXMT9_PRESENT_REFRESH_HZ");
    if (!env || env[0] == '\0') {
      return 60.0;
    }
    char* end = nullptr;
    const double parsed = std::strtod(env, &end);
    return end != env && parsed > 0.0 ? parsed : 60.0;
  }();
  return value;
}

double minimumPresentDuration(core::PresentInterval interval) {
  switch (interval) {
    case core::PresentInterval::Two:
      return 2.0 / presentRefreshHz();
    case core::PresentInterval::Default:
      return 1.0 / presentRefreshHz();
    case core::PresentInterval::Immediate:
      return 0.0;
  }
  return 0.0;
}

bool sameLayerProps(const WMTLayerProps& a, const WMTLayerProps& b) {
  return a.device == b.device &&
         a.contents_scale == b.contents_scale &&
         a.drawable_width == b.drawable_width &&
         a.drawable_height == b.drawable_height &&
         a.opaque == b.opaque &&
         a.display_sync_enabled == b.display_sync_enabled &&
         a.framebuffer_only == b.framebuffer_only &&
         a.pixel_format == b.pixel_format;
}

}  // namespace

AcquirePolicy resolveAcquirePolicy(const char* asyncEnv,
                                   const char* onSubmitEnv,
                                   const char* preAcquireEnv) {
  // Priority order — see AcquirePolicy doc-comment. Each branch is
  // checked independently so that callers can pass arbitrary
  // combinations.
  if (envFlagSet(asyncEnv)) {
    return AcquirePolicy::Async;
  }
  if (envFlagSet(onSubmitEnv)) {
    return AcquirePolicy::SyncOnSubmit;
  }
  if (envFlagSet(preAcquireEnv)) {
    return AcquirePolicy::PreAcquire;
  }
  return AcquirePolicy::Sync;
}

AcquirePolicy resolveAcquirePolicyFromEnv() {
  static const AcquirePolicy value = resolveAcquirePolicy(
      std::getenv("DXMT9_PRESENT_ASYNC_ACQUIRE"),
      std::getenv("DXMT9_PRESENT_ACQUIRE_ON_SUBMIT"),
      std::getenv("DXMT9_PRESENT_PREACQUIRE"));
  return value;
}

namespace {
// DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION historically defaulted to
// true: when the env var was unset the boundary waited on
// presentCompletedSeqId_. Treat a null / empty string as "set" while
// keeping a literal "0" as the explicit opt-out.
bool presentCompletionEnvDefaultOn(const char* env) {
  if (env == nullptr) {
    return true;
  }
  if (env[0] == '\0') {
    return true;
  }
  return std::strcmp(env, "0") != 0;
}
}  // namespace

BoundaryPolicy resolveBoundaryPolicy(const char* disableEnv,
                                     const char* deferredEnv,
                                     const char* presentCompletionEnv,
                                     const char* completionEnv,
                                     const char* afterAcquireEnv) {
  // Priority order — see BoundaryPolicy doc-comment. Disabled
  // short-circuits the whole boundary, so it is consulted first. The
  // deferred branch keeps the same present-completion target but moves
  // the wait to the next Present, so it must outrank the historical
  // default-on PresentCompletion branch.
  if (envFlagSet(disableEnv)) {
    return BoundaryPolicy::Disabled;
  }
  if (envFlagSet(deferredEnv)) {
    return BoundaryPolicy::DeferredPresentCompletion;
  }
  // The default-on PresentCompletion branch matches the historical
  // `if (!env) return true;` behavior of the legacy lambda; an
  // explicit "0" demotes us to the Completion / AfterAcquire /
  // Default chain.
  if (presentCompletionEnvDefaultOn(presentCompletionEnv)) {
    return BoundaryPolicy::PresentCompletion;
  }
  if (envFlagSet(completionEnv)) {
    return BoundaryPolicy::Completion;
  }
  if (envFlagSet(afterAcquireEnv)) {
    return BoundaryPolicy::AfterAcquire;
  }
  return BoundaryPolicy::Default;
}

BoundaryPolicy resolveBoundaryPolicyFromEnv() {
  static const BoundaryPolicy value = resolveBoundaryPolicy(
      std::getenv("DXMT9_DISABLE_PRESENT_BOUNDARY"),
      std::getenv("DXMT9_PRESENT_BOUNDARY_DEFERRED"),
      std::getenv("DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION"),
      std::getenv("DXMT9_PRESENT_BOUNDARY_COMPLETION"),
      std::getenv("DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE"));
  return value;
}

bool resolveDisableVsync(const char* env) {
  return env != nullptr && env[0] != '\0' && env[0] != '0';
}

void PresentDrawableToken::complete(WMT::Reference<WMT::MetalDrawable> drawable) {
  {
    std::lock_guard lock(mutex_);
    drawable_ = std::move(drawable);
    ready_ = true;
  }
  cv_.notify_all();
}

void PresentDrawableToken::fail() {
  {
    std::lock_guard lock(mutex_);
    drawable_ = nullptr;
    ready_ = true;
  }
  cv_.notify_all();
}

WMT::MetalDrawable PresentDrawableToken::waitDrawable() {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return ready_; });
  return WMT::MetalDrawable{drawable_.handle};
}

PresentOutputTarget OffscreenPresentOutput::acquire(std::uint64_t) noexcept {
  return target_;
}

void OffscreenPresentOutput::schedule(WMT::CommandBuffer&,
                                      std::uint64_t) noexcept {
  scheduledCount_.fetch_add(1u, std::memory_order_release);
}

Presenter::Presenter(WMT::Device device, uint64_t hwnd, uint64_t seqId,
                     WMT::Reference<WMT::BinaryArchive>* archive,
                     const std::string* archivePath)
    : device_(device), hwnd_(hwnd),
      acquisition_(presentimpl::acquireLayerForHwnd(hwnd, seqId)),
      layer_(acquisition_.layerHandle),
      policy_(resolveAcquirePolicyFromEnv()),
      archive_(archive), archivePath_(archivePath) {}

Presenter::Presenter(WMT::Device device, std::shared_ptr<PresentOutput> output,
                     WMT::Reference<WMT::BinaryArchive>* archive,
                     const std::string* archivePath)
    : device_(device), output_(std::move(output)),
      policy_(AcquirePolicy::Sync), archive_(archive), archivePath_(archivePath) {}

Presenter::~Presenter() {
  {
    std::lock_guard lock(asyncAcquireMutex_);
    asyncAcquireStop_ = true;
    for (auto& request : asyncAcquireRequests_) {
      if (request.token) {
        request.token->fail();
      }
    }
    asyncAcquireRequests_.clear();
    asyncAcquireDrawableHeld_ = false;
  }
  asyncAcquireCv_.notify_all();
  if (asyncAcquireThread_.joinable()) {
    asyncAcquireThread_.join();
  }

  {
    std::lock_guard lock(preAcquireMutex_);
    preAcquireStop_ = true;
    preAcquireRequested_ = false;
    ++preAcquireGeneration_;
  }
  preAcquireCv_.notify_all();
  if (preAcquireThread_.joinable()) {
    preAcquireThread_.join();
  }
  discardPrefetchedDrawable();
  presentimpl::releaseLayerAcquisition(acquisition_);
  layer_ = WMT::MetalLayer{};
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>&
Presenter::pipelineFor(bool opaqueAlpha, bool applyGamma) {
  auto& slot = applyGamma
                   ? (opaqueAlpha ? pipelineGammaOpaque_ : pipelineGammaAlpha_)
                   : (opaqueAlpha ? pipelineOpaque_ : pipelineAlpha_);
  if (!slot.valid()) {
    slot = applyGamma
               ? pipeline::buildGammaApplyPresentPipeline(
                     WMT::Reference<WMT::Device>{device_.handle},
                     opaqueAlpha, archive_, archivePath_)
               : pipeline::buildPresentPipeline(
                     WMT::Reference<WMT::Device>{device_.handle},
                     opaqueAlpha, archive_, archivePath_);
  }
  return slot;
}

void Presenter::configureLayer(const AcquireParams& params) {
  WMTLayerProps props{};
  props.device = device_.handle;
  props.pixel_format = WMTPixelFormatBGRA8Unorm;
  props.opaque = true;
  // `framebuffer_only=false` disables Apple's tile-only fast path on
  // CAMetalLayer. With false, the drawable backing is system-memory
  // resident (so D3D9 Lock() / GetRenderTargetData() on the backbuffer
  // can read it) and WindowServer compositing reads from system memory
  // — measured at ~15.8 s GPU per 20 s xctrace window on SFIV (alongside
  // SFIV's own 19.9 s fragment, the compositor multiplies on-die GPU
  // pressure). `DXMT9_LAYER_FRAMEBUFFER_ONLY=1` flips it to the
  // direct-display fast path: Apple can keep the drawable in tile
  // memory and skip the system-memory round-trip. Apps that rely on
  // backbuffer readback will break, so default stays false.
  static const bool fbOnly = [] {
    const char* env = std::getenv("DXMT9_LAYER_FRAMEBUFFER_ONLY");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  props.framebuffer_only = fbOnly;
  props.drawable_width = std::max(1u, params.width);
  props.drawable_height = std::max(1u, params.height);
  // Match upstream dxmt: keep CAMetalLayer out of present pacing by
  // default, and let the D3D9/queue layer own latency boundaries.
  // DXMT9_DISABLE_VSYNC=1 forces the layer path off independently of the
  // env-var that controls the layer-side opt-in.
  props.display_sync_enabled = !disableVsyncEnv()
                               && layerDisplaySyncEnabled()
                               && params.displaySyncEnabled;
  props.contents_scale = params.contentsScale;
  const auto maxDrawableCount = std::clamp<uint32_t>(params.maxDrawableCount, 1u, 3u);
  if (!cachedLayerPropsValid_ ||
      !sameLayerProps(cachedLayerProps_, props) ||
      cachedMaxDrawableCount_ != maxDrawableCount) {
    discardPrefetchedDrawable();
    const auto propsStarted = std::chrono::steady_clock::now();
    MetalLayer_setProps(layer_.handle, &props);
    MetalLayer_setMaximumDrawableCount(layer_.handle, maxDrawableCount);
    const auto propsElapsed = std::chrono::steady_clock::now() - propsStarted;
    perf::countPresentSetPropsWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(propsElapsed).count()));
    cachedLayerProps_ = props;
    cachedMaxDrawableCount_ = maxDrawableCount;
    cachedLayerPropsValid_ = true;
  }
}

std::shared_ptr<PresentDrawableToken> Presenter::acquireDrawable(const AcquireParams& params) {
  if (!layer_) {
    return {};
  }

  {
    std::lock_guard lock(stateMutex_);
    configureLayer(params);
  }

  presentimpl::traceEvent("nextDrawable.submit.begin", params.seqId, hwnd_);
  const auto acquireStarted = std::chrono::steady_clock::now();
  auto drawable = layer_.nextDrawableRetained();
  const auto acquireElapsed = std::chrono::steady_clock::now() - acquireStarted;
  const auto acquireNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(acquireElapsed).count());
  perf::countPresentAcquireWait(acquireNs);
  if (!drawable) {
    presentimpl::traceEvent("nextDrawable.submit.nil", params.seqId, hwnd_);
    return {};
  }
  presentimpl::traceEvent("nextDrawable.submit.ok", params.seqId, hwnd_);
  return std::make_shared<PresentDrawableToken>(std::move(drawable));
}

std::shared_ptr<PresentDrawableToken> Presenter::beginAcquireDrawable(const AcquireParams& params) {
  if (!layer_) {
    return {};
  }
  auto token = std::make_shared<PresentDrawableToken>();
  {
    std::lock_guard lock(asyncAcquireMutex_);
    if (asyncAcquireStop_) {
      token->fail();
      return token;
    }
    if (!asyncAcquireThread_.joinable()) {
      asyncAcquireThread_ = std::thread([this] { runAsyncAcquireLoop(); });
    }
    asyncAcquireRequests_.push_back(AsyncAcquireRequest{
        .params = params,
        .token = token,
    });
  }
  asyncAcquireCv_.notify_one();
  return token;
}

WMT::Reference<WMT::MetalDrawable> Presenter::takeOrWaitForPrefetchedDrawable() {
  std::unique_lock lock(preAcquireMutex_);
  if (!prefetchedDrawable_ && preAcquireInFlight_) {
    preAcquireCv_.wait(lock, [this] {
      return preAcquireStop_ || prefetchedDrawable_ || !preAcquireInFlight_;
    });
  }
  WMT::Reference<WMT::MetalDrawable> drawable = std::move(prefetchedDrawable_);
  return drawable;
}

void Presenter::discardPrefetchedDrawable() {
  std::lock_guard lock(preAcquireMutex_);
  prefetchedDrawable_ = nullptr;
  ++preAcquireGeneration_;
}

void Presenter::finishAsyncAcquireToken() {
  {
    std::lock_guard lock(asyncAcquireMutex_);
    asyncAcquireDrawableHeld_ = false;
  }
  asyncAcquireCv_.notify_one();
}

void Presenter::preAcquireNextDrawable(uint64_t seqId) {
  if (policy_ != AcquirePolicy::PreAcquire || !layer_) {
    return;
  }
  {
    std::lock_guard lock(preAcquireMutex_);
    if (prefetchedDrawable_ || preAcquireRequested_ || preAcquireInFlight_ || preAcquireStop_) {
      return;
    }
    if (!preAcquireThread_.joinable()) {
      preAcquireThread_ = std::thread([this] { runPreAcquireLoop(); });
    }
    preAcquireSeqId_ = seqId;
    preAcquireRequested_ = true;
    perf::countPresentPreAcquireRequest();
  }
  preAcquireCv_.notify_one();
}

void Presenter::runAsyncAcquireLoop() {
  // Worker thread has no AppKit run loop, so it owns no top-level
  // autorelease pool. `layer_.nextDrawableRetained()` crosses into
  // the unix .mm provider where `[CAMetalLayer nextDrawable]` and any
  // QuartzCore/Foundation temporaries it spawns would otherwise
  // accumulate until thread exit. Wrap each iteration's acquire body
  // in @autoreleasepool to drain those temporaries per-frame. See
  // agents/rules/codebase_conventions.rules.md "Metal / ObjC++
  // Runtime" — Presenter is the runtime-side drawable owner.
  while (true) {
    @autoreleasepool {
      AsyncAcquireRequest request{};
      {
        std::unique_lock lock(asyncAcquireMutex_);
        asyncAcquireCv_.wait(lock, [this] {
          return asyncAcquireStop_ ||
                 (!asyncAcquireRequests_.empty() && !asyncAcquireDrawableHeld_);
        });
        if (asyncAcquireStop_ && asyncAcquireRequests_.empty()) {
          return;
        }
        if (asyncAcquireStop_) {
          return;
        }
        request = std::move(asyncAcquireRequests_.front());
        asyncAcquireRequests_.pop_front();
        asyncAcquireDrawableHeld_ = true;
      }

      if (!request.token) {
        finishAsyncAcquireToken();
        continue;
      }
      if (!layer_) {
        request.token->fail();
        finishAsyncAcquireToken();
        continue;
      }

      {
        std::lock_guard lock(stateMutex_);
        configureLayer(request.params);
      }

      presentimpl::traceEvent("asyncAcquire.begin", request.params.seqId, hwnd_);
      const auto acquireStarted = std::chrono::steady_clock::now();
      auto drawable = layer_.nextDrawableRetained();
      const auto acquireElapsed = std::chrono::steady_clock::now() - acquireStarted;
      const auto acquireNs = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(acquireElapsed).count());
      perf::countPresentAcquireWait(acquireNs);
      perf::countPresentAsyncAcquireWait(acquireNs);
      if (!drawable) {
        presentimpl::traceEvent("asyncAcquire.nil", request.params.seqId, hwnd_);
        request.token->fail();
        finishAsyncAcquireToken();
        continue;
      }
      presentimpl::traceEvent("asyncAcquire.ok", request.params.seqId, hwnd_);
      request.token->complete(std::move(drawable));
    }  // @autoreleasepool
  }
}

void Presenter::runPreAcquireLoop() {
  while (true) {
    uint64_t seqId = 0;
    uint64_t generation = 0;
    {
      std::unique_lock lock(preAcquireMutex_);
      preAcquireCv_.wait(lock, [this] {
        return preAcquireStop_ || preAcquireRequested_;
      });
      if (preAcquireStop_) {
        return;
      }
      seqId = preAcquireSeqId_;
      generation = preAcquireGeneration_;
      preAcquireRequested_ = false;
      preAcquireInFlight_ = true;
    }

    presentimpl::traceEvent("preAcquire.begin", seqId, hwnd_);
    const auto acquireStarted = std::chrono::steady_clock::now();
    auto drawable = layer_.nextDrawableRetained();
    const auto acquireElapsed = std::chrono::steady_clock::now() - acquireStarted;
    perf::countPresentPreAcquireWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(acquireElapsed).count()));
    if (!drawable) {
      presentimpl::traceEvent("preAcquire.nil", seqId, hwnd_);
    } else {
      std::lock_guard lock(preAcquireMutex_);
      if (!preAcquireStop_ && generation == preAcquireGeneration_ && !prefetchedDrawable_) {
        prefetchedDrawable_ = std::move(drawable);
        presentimpl::traceEvent("preAcquire.ok", seqId, hwnd_);
      }
    }

    {
      std::lock_guard lock(preAcquireMutex_);
      preAcquireInFlight_ = false;
    }
    preAcquireCv_.notify_all();
  }
}

Presenter::EncodeResult Presenter::encodeCommands(WMT::CommandBuffer& commandBuffer,
                                                   const EncodeParams& params) {
  EncodeResult result{};
  if ((!layer_ && !output_) || !commandBuffer) {
    return result;
  }

  const AcquireParams acquireParams{
      .width = params.width,
      .height = params.height,
      .displaySyncEnabled = params.displaySyncEnabled,
      .contentsScale = params.contentsScale,
      .maxDrawableCount = params.maxDrawableCount,
      .seqId = params.seqId,
  };
  // Lock the layer-config path only when a concurrent thread can be
  // calling configureLayer: the async-acquire worker
  // (runAsyncAcquireLoop) or the queue-side sync-on-submit acquire.
  // The Sync and PreAcquire policies have configureLayer as the only
  // caller and don't need the lock.
  if (!output_) {
    switch (policy_) {
      case AcquirePolicy::SyncOnSubmit:
      case AcquirePolicy::Async: {
        std::lock_guard lock(stateMutex_);
        configureLayer(acquireParams);
        break;
      }
      case AcquirePolicy::Sync:
      case AcquirePolicy::PreAcquire:
        configureLayer(acquireParams);
        break;
    }
  }

  // G2-B Option A — non-identity ramp opts into the gamma-apply PSO and
  // pays one extra setFragmentBytes(1.5 KB) per present. Identity ramps
  // (the default for apps that never call SetGammaRamp, plus the explicit
  // reset to identity baseline) stay on the validated textured-blit path.
  const bool applyGamma = !params.gammaRampIsIdentity && params.gammaRamp != nullptr;
  auto pipelineFuture = pipelineFor(params.opaqueAlpha, applyGamma);
  auto pipeline = pipelineFuture.get();
  if (!pipeline) {
    presentimpl::traceEvent("pipeline.nil", params.seqId, hwnd_);
    return result;
  }

  if (!sampler_) {
    WMTSamplerInfo info{};
    info.min_filter = WMTSamplerMinMagFilterLinear;
    info.mag_filter = WMTSamplerMinMagFilterLinear;
    info.mip_filter = WMTSamplerMipFilterNotMipmapped;
    info.s_address_mode = WMTSamplerAddressModeClampToEdge;
    info.t_address_mode = WMTSamplerAddressModeClampToEdge;
    info.r_address_mode = WMTSamplerAddressModeClampToEdge;
    info.max_anisotroy = 1;
    info.compare_function = WMTCompareFunctionNever;
    info.lod_max_clamp = 1e9f;
    info.normalized_coords = true;
    sampler_ = WMT::Reference<WMT::Device>{device_.handle}.newSamplerState(info);
  }

  presentimpl::traceEvent("nextDrawable.begin", params.seqId, hwnd_);
  const auto acquireStarted = std::chrono::steady_clock::now();
  WMT::MetalDrawable drawable{};
  PresentOutputTarget outputTarget{};
  if (output_) {
    outputTarget = output_->acquire(params.seqId);
    if (!outputTarget.texture) {
      presentimpl::traceEvent("presentOutput.nil", params.seqId, hwnd_);
      return result;
    }
    result.acquired = true;
  } else if (params.drawableToken) {
    const auto tokenWaitStarted = std::chrono::steady_clock::now();
    drawable = params.drawableToken->waitDrawable();
    const auto tokenWaitElapsed = std::chrono::steady_clock::now() - tokenWaitStarted;
    perf::countPresentTokenWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tokenWaitElapsed).count()));
    finishAsyncAcquireToken();
    if (!drawable) {
      presentimpl::traceEvent("nextDrawable.token-missing", params.seqId, hwnd_);
      return result;
    }
    result.acquired = true;
    presentimpl::traceEvent("nextDrawable.token", params.seqId, hwnd_);
  } else if (params.drawableTokenRequired) {
    perf::countPresentAcquireWait(0);
    presentimpl::traceEvent("nextDrawable.token-missing", params.seqId, hwnd_);
    return result;
  }
  auto prefetchedDrawable = WMT::Reference<WMT::MetalDrawable>{};
  if (!output_) {
    if (!drawable) {
      prefetchedDrawable = takeOrWaitForPrefetchedDrawable();
    }
    if (!drawable && prefetchedDrawable) {
      perf::countPresentPreAcquireHit();
      drawable = WMT::MetalDrawable{prefetchedDrawable.handle};
    } else {
      if (!drawable && policy_ == AcquirePolicy::PreAcquire) {
        perf::countPresentPreAcquireMiss();
      }
      if (!drawable) {
        drawable = layer_.nextDrawable();
      }
    }
  }
  const auto acquireElapsed = std::chrono::steady_clock::now() - acquireStarted;
  if (!params.drawableToken) {
    perf::countPresentAcquireWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(acquireElapsed).count()));
  }
  if (!output_ && !drawable) {
    presentimpl::traceEvent("nextDrawable.nil", params.seqId, hwnd_);
    return result;
  }
  result.acquired = true;
  presentimpl::traceEvent("nextDrawable.ok", params.seqId, hwnd_);

  DXMT_ASSERT((outputTarget.texture || drawable) &&
              "Presenter::encodeCommands reached Metal encode with null target");
  auto drawableTex = output_ ? outputTarget.texture : drawable.texture();
  WMTRenderPassInfo passInfo{};
  passInfo.colors[0].texture = drawableTex.handle;
  passInfo.colors[0].load_action = WMTLoadActionDontCare;
  passInfo.colors[0].store_action = WMTStoreActionStore;
  attachCounterSampleBuffers(passInfo, params.sampleBufferAttachments);

  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) {
    presentimpl::traceEvent("encoder.nil", params.seqId, hwnd_);
    return result;
  }
  encoder.setLabel(labels::makeLabelStringFmt(
      "Present[seq=%llu]", static_cast<unsigned long long>(params.seqId)));

  encoder.setRenderPipelineState(pipeline);
  encoder.setFragmentTexture(params.source, 0);
  if (sampler_) {
    encoder.setFragmentSamplerState(sampler_, 0);
  }
  if (applyGamma) {
    // setFragmentBytes is bounded to 4 KB inline; the 1.5 KB ramp fits
    // comfortably and the payload lives on the encoder's command buffer
    // for the encoder's lifetime, so we can borrow SwapDesc::gammaRamp
    // directly without owning a copy.
    encoder.setFragmentBytes(params.gammaRamp, sizeof(core::GammaRamp), 0);
  }

  const auto drawableWidth = output_ ? outputTarget.width : drawableTex.width();
  const auto drawableHeight = output_ ? outputTarget.height : drawableTex.height();
  const auto fallbackWidth =
      static_cast<std::uint64_t>(std::max<uint32_t>(1u, params.width));
  const auto fallbackHeight =
      static_cast<std::uint64_t>(std::max<uint32_t>(1u, params.height));
  const auto targetWidth = drawableWidth ? drawableWidth : fallbackWidth;
  const auto targetHeight = drawableHeight ? drawableHeight : fallbackHeight;
  const double width = static_cast<double>(targetWidth);
  const double height = static_cast<double>(targetHeight);
  encoder.setViewport(WMTViewport{0.0, 0.0, width, height, 0.0, 1.0});
  encoder.setScissorRect(WMTScissorRect{0, 0, targetWidth, targetHeight});
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  encoder.endEncoding();
  perf::countPresentPass(params.width, params.height, targetWidth, targetHeight);
  perf::countPresentSchedule(params.displaySyncEnabled, params.minimumPresentDuration);
  if (output_) {
    output_->schedule(commandBuffer, params.seqId);
  } else if (params.minimumPresentDuration > 0.0) {
    commandBuffer.presentDrawableAfterMinimumDuration(drawable, params.minimumPresentDuration);
  } else {
    commandBuffer.presentDrawable(drawable);
  }

  result.encoded = true;
  presentimpl::traceEvent("scheduled", params.seqId, hwnd_);
  return result;
}

namespace {

std::uint64_t forcedPresentTextureHandle() {
  static const std::uint64_t value = [] {
    const char* env = std::getenv("DXMT_FORCE_PRESENT_TEXTURE_HANDLE");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 0);
    if (end == env) {
      return 0ull;
    }
    return parsed;
  }();
  return value;
}

}  // namespace

bool encodePresent(WMT::CommandBuffer& commandBuffer,
                   resources::Pool& pool,
                   Presenter* presenter,
                   std::shared_ptr<PresentDrawableToken> drawableToken,
                   const core::SwapDesc& present,
                   core::Handle sourceHandle,
                   std::uint64_t seqId,
                   std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  using namespace dxmt9::core::metalqueue;
  presentimpl::traceEvent("begin", seqId, present.window.value);
  if (queueTraceEnabled()) {
    std::ostringstream out;
    out << "[dxmt9-present] source"
        << " seq=" << static_cast<unsigned long long>(seqId)
        << " hwnd=" << static_cast<unsigned long long>(present.window.value)
        << " handle=0x" << std::hex << static_cast<unsigned long long>(sourceHandle.value) << std::dec;
    emitQueueTraceLine(out.str());
  }
  auto* source = pool.findSurface(sourceHandle.value);
  if (!source) {
    perf::countPresentSourceResolved(false, false, false, true, 0, 0, 0, 0,
                                     sourceHandle.value, 0);
    perf::countPresentSkipped();
    presentimpl::traceEvent("missing-source", seqId, present.window.value);
    return false;
  }
  obj_handle_t sourceTextureHandle =
      source->texture
          ? (source->resolveTexture ? source->resolveTexture.handle : source->texture.handle)
          : 0;
  if (!source->texture) {
    const std::uint32_t sampleCount =
        source->desc.multiSampleType == core::MultiSampleType::None
            ? 1u
            : core::sampleCount(source->desc.multiSampleType);
    perf::countPresentSourceResolved(true,
                                     false,
                                     static_cast<bool>(source->resolveTexture),
                                     true,
                                     source->desc.width,
                                     source->desc.height,
                                     static_cast<std::uint32_t>(source->desc.format),
                                     sampleCount,
                                     sourceHandle.value,
                                     0);
    perf::countPresentSkipped();
    presentimpl::traceEvent("missing-source", seqId, present.window.value);
    return false;
  }
  if (!present.windowed) {
    perf::countPresentFullscreen();
  }
  const std::uint64_t forcedTextureHandle = forcedPresentTextureHandle();
  if (forcedTextureHandle != 0ull) {
    if (auto* forced = pool.findTexture(forcedTextureHandle); forced && forced->texture) {
      sourceTextureHandle = forced->texture.handle;
      if (queueTraceEnabled()) {
        std::ostringstream out;
        out << "[dxmt9-present] force-texture"
            << " seq=" << static_cast<unsigned long long>(seqId)
            << " hwnd=" << static_cast<unsigned long long>(present.window.value)
            << " handle=0x" << std::hex << forcedTextureHandle << std::dec
            << " size=" << forced->desc.width << "x" << forced->desc.height
            << " fmt=" << static_cast<unsigned>(forced->desc.format);
        emitQueueTraceLine(out.str());
      }
    } else if (queueTraceEnabled()) {
      std::ostringstream out;
      out << "[dxmt9-present] force-texture-missing"
          << " seq=" << static_cast<unsigned long long>(seqId)
          << " hwnd=" << static_cast<unsigned long long>(present.window.value)
          << " handle=0x" << std::hex << forcedTextureHandle << std::dec;
      emitQueueTraceLine(out.str());
    }
  }
  if (queueTraceEnabled()) {
    std::ostringstream out;
    out << "[dxmt9-present] source.info"
        << " seq=" << static_cast<unsigned long long>(seqId)
        << " hwnd=" << static_cast<unsigned long long>(present.window.value)
        // Present-source diagnostic (R-BACK debug): the D3D9 surface handle the
        // app presented, the resolved Metal texture actually blitted to the
        // drawable, and whether the MSAA resolve target was used. Correlate with
        // a captured frame to test whether an intermittent visual anomaly is a
        // present-source race (texture handle changes) vs a render/blend bug
        // (handle stable but contents wrong).
        << " srcSurface=0x" << std::hex
        << static_cast<unsigned long long>(sourceHandle.value)
        << " srcTex=0x" << static_cast<unsigned long long>(sourceTextureHandle)
        << std::dec << " usedResolve=" << (source->resolveTexture ? 1 : 0)
        << " size=" << source->desc.width << "x" << source->desc.height
        << " fmt=" << static_cast<unsigned>(source->desc.format)
        << " sampleCount="
        << (source->desc.multiSampleType == core::MultiSampleType::None
                ? 1u
                : core::sampleCount(source->desc.multiSampleType));
    emitQueueTraceLine(out.str());
  }
  const std::uint32_t sourceSampleCount =
      source->desc.multiSampleType == core::MultiSampleType::None
          ? 1u
          : core::sampleCount(source->desc.multiSampleType);
  perf::countPresentSourceResolved(true,
                                   true,
                                   static_cast<bool>(source->resolveTexture),
                                   source->desc.width == 0 || source->desc.height == 0 ||
                                       sourceTextureHandle == 0,
                                   source->desc.width,
                                   source->desc.height,
                                   static_cast<std::uint32_t>(source->desc.format),
                                   sourceSampleCount,
                                   sourceHandle.value,
                                   sourceTextureHandle);

  // The originating core::SwapChain owns the Presenter; the queue
  // resolved the SwapDesc::presentId to a non-owning pointer before
  // calling encodePresent. Missing presenter = no layer available
  // (hwnd=0 or failed acquisition in SwapChain::ensurePresenter), or a
  // stale id from a destroyed swapchain.
  if (!presenter) {
    perf::countPresentSkipped();
    presentimpl::traceEvent("missing-layer", seqId, present.window.value);
    return false;
  }

  const bool opaqueAlpha = source->desc.format == core::Format::X8R8G8B8 ||
                            source->desc.format == core::Format::X8B8G8R8;

  Presenter::EncodeParams params{};
  params.source = WMT::Texture{sourceTextureHandle};
  params.width = present.width;
  params.height = present.height;
  params.displaySyncEnabled = present.displaySyncEnabled;
  params.contentsScale = 1.0;
  // DXMT9_DISABLE_VSYNC=1 forces minimumPresentDuration to 0 regardless
  // of the D3D9 PresentationInterval — the runtime-side counterpart to
  // an app requesting D3DPRESENT_INTERVAL_IMMEDIATE per swap chain.
  params.minimumPresentDuration =
      !disableVsyncEnv()
              && !layerDisplaySyncEnabled()
              && present.displaySyncEnabled
          ? minimumPresentDuration(present.interval)
          : 0.0;
  params.maxDrawableCount = kDefaultMetalDrawableCount;
  params.opaqueAlpha = opaqueAlpha;
  params.seqId = seqId;
  params.drawableToken = std::move(drawableToken);
  params.drawableTokenRequired = present.drawableTokenRequired;
  // SwapDesc::gammaRamp is a POD on the same struct we're iterating; it
  // stays valid until the encoder finishes encoding, so a borrowed
  // pointer is safe here. The identity flag is the fast-path selector.
  params.gammaRamp = &present.gammaRamp;
  params.gammaRampIsIdentity = present.gammaRampIsIdentity;
  params.sampleBufferAttachments = sampleBufferAttachments;

  const auto presentResult = presenter->encodeCommands(commandBuffer, params);
  if (presentResult.encoded) {
    perf::countPresentEncoded();
  } else {
    perf::countPresentSkipped();
  }
  if (!presentResult.acquired) {
    if (present.notifyPresentationStatus) present.notifyPresentationStatus(true);
    return false;
  }
  if (present.notifyPresentationStatus) present.notifyPresentationStatus(false);
  return presentResult.encoded;
}

}  // namespace dxmt9
