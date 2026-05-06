#include "dxmt9_presenter.hpp"
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

bool presentPreAcquireEnabled() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT9_PRESENT_PREACQUIRE");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return value;
}

bool presentAcquireOnSubmitEnabled() {
  static const bool value = [] {
    const char* syncEnv = std::getenv("DXMT9_PRESENT_ACQUIRE_ON_SUBMIT");
    const char* asyncEnv = std::getenv("DXMT9_PRESENT_ASYNC_ACQUIRE");
    const bool syncEnabled = syncEnv && syncEnv[0] != '\0' && std::strcmp(syncEnv, "0") != 0;
    const bool asyncEnabled = asyncEnv && asyncEnv[0] != '\0' && std::strcmp(asyncEnv, "0") != 0;
    return syncEnabled || asyncEnabled;
  }();
  return value;
}

bool layerDisplaySyncEnabled() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT9_LAYER_DISPLAY_SYNC");
    return env && env[0] != '\0' && env[0] != '0';
  }();
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

Presenter::Presenter(WMT::Device device, uint64_t hwnd, uint64_t seqId,
                     WMT::Reference<WMT::BinaryArchive>* archive,
                     const std::string* archivePath)
    : device_(device), hwnd_(hwnd),
      acquisition_(presentimpl::acquireLayerForHwnd(hwnd, seqId)),
      layer_(acquisition_.layerHandle),
      archive_(archive), archivePath_(archivePath) {}

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
Presenter::pipelineFor(bool opaqueAlpha) {
  auto& slot = opaqueAlpha ? pipelineOpaque_ : pipelineAlpha_;
  if (!slot.valid()) {
    slot = pipeline::buildPresentPipeline(WMT::Reference<WMT::Device>{device_.handle},
                                            opaqueAlpha, archive_, archivePath_);
  }
  return slot;
}

void Presenter::configureLayer(const AcquireParams& params) {
  WMTLayerProps props{};
  props.device = device_.handle;
  props.pixel_format = WMTPixelFormatBGRA8Unorm;
  props.opaque = true;
  props.framebuffer_only = false;
  props.drawable_width = std::max(1u, params.width);
  props.drawable_height = std::max(1u, params.height);
  // Match upstream dxmt: keep CAMetalLayer out of present pacing by
  // default, and let the D3D9/queue layer own latency boundaries.
  props.display_sync_enabled = layerDisplaySyncEnabled() && params.displaySyncEnabled;
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
  if (!presentPreAcquireEnabled() || !layer_) {
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
  while (true) {
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
  if (!layer_ || !commandBuffer) {
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
  if (presentAcquireOnSubmitEnabled()) {
    std::lock_guard lock(stateMutex_);
    configureLayer(acquireParams);
  } else {
    configureLayer(acquireParams);
  }

  auto pipelineFuture = pipelineFor(params.opaqueAlpha);
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
  if (params.drawableToken) {
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
  if (!drawable) {
    prefetchedDrawable = takeOrWaitForPrefetchedDrawable();
  }
  if (!drawable && prefetchedDrawable) {
    perf::countPresentPreAcquireHit();
    drawable = WMT::MetalDrawable{prefetchedDrawable.handle};
  } else {
    if (!drawable && presentPreAcquireEnabled()) {
      perf::countPresentPreAcquireMiss();
    }
    if (!drawable) {
      drawable = layer_.nextDrawable();
    }
  }
  const auto acquireElapsed = std::chrono::steady_clock::now() - acquireStarted;
  if (!params.drawableToken) {
    perf::countPresentAcquireWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(acquireElapsed).count()));
  }
  if (!drawable) {
    presentimpl::traceEvent("nextDrawable.nil", params.seqId, hwnd_);
    return result;
  }
  result.acquired = true;
  presentimpl::traceEvent("nextDrawable.ok", params.seqId, hwnd_);

  auto drawableTex = drawable.texture();
  WMTRenderPassInfo passInfo{};
  passInfo.colors[0].texture = drawableTex.handle;
  passInfo.colors[0].load_action = WMTLoadActionDontCare;
  passInfo.colors[0].store_action = WMTStoreActionStore;

  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) {
    presentimpl::traceEvent("encoder.nil", params.seqId, hwnd_);
    return result;
  }

  encoder.setRenderPipelineState(pipeline);
  encoder.setFragmentTexture(params.source, 0);
  if (sampler_) {
    encoder.setFragmentSamplerState(sampler_, 0);
  }

  const auto drawableWidth = drawableTex.width();
  const auto drawableHeight = drawableTex.height();
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
  if (params.minimumPresentDuration > 0.0) {
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
                   const core::SwapDesc& present,
                   core::Handle sourceHandle,
                   std::uint64_t seqId) {
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

  // The originating core::SwapChain owns the Presenter and passes it via
  // SwapDesc. Missing presenter = no layer available (hwnd=0 or failed
  // acquisition in SwapChain::ensurePresenter).
  Presenter* presenter = present.presenter;
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
  params.minimumPresentDuration =
      !layerDisplaySyncEnabled() && present.displaySyncEnabled
          ? minimumPresentDuration(present.interval)
          : 0.0;
  params.maxDrawableCount = kDefaultMetalDrawableCount;
  params.opaqueAlpha = opaqueAlpha;
  params.seqId = seqId;
  params.drawableToken = present.drawableToken;
  params.drawableTokenRequired = present.drawableTokenRequired;

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
