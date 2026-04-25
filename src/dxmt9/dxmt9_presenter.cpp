#include "dxmt9_presenter.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_device.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace dxmt9 {

namespace {

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

Presenter::Presenter(WMT::Device device, uint64_t hwnd, uint64_t seqId,
                     WMT::Reference<WMT::BinaryArchive>* archive,
                     const std::string* archivePath)
    : device_(device), hwnd_(hwnd),
      acquisition_(presentimpl::acquireLayerForHwnd(hwnd, seqId)),
      layer_(acquisition_.layerHandle),
      archive_(archive), archivePath_(archivePath) {}

Presenter::~Presenter() {
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

Presenter::EncodeResult Presenter::encodeCommands(WMT::CommandBuffer& commandBuffer,
                                                   const EncodeParams& params) {
  EncodeResult result{};
  if (!layer_ || !commandBuffer) {
    return result;
  }

  {
    WMTLayerProps props{};
    props.device = device_.handle;
    props.pixel_format = WMTPixelFormatBGRA8Unorm;
    props.opaque = true;
    props.framebuffer_only = false;
    props.drawable_width = std::max(1u, params.width);
    props.drawable_height = std::max(1u, params.height);
    props.display_sync_enabled = params.displaySyncEnabled;
    props.contents_scale = params.contentsScale;
    const auto maxDrawableCount = std::clamp<uint32_t>(params.maxDrawableCount, 1u, 3u);
    if (!cachedLayerPropsValid_ ||
        !sameLayerProps(cachedLayerProps_, props) ||
        cachedMaxDrawableCount_ != maxDrawableCount) {
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
  auto drawable = layer_.nextDrawable();
  const auto acquireElapsed = std::chrono::steady_clock::now() - acquireStarted;
  perf::countPresentAcquireWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(acquireElapsed).count()));
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

  const double width = std::max<uint32_t>(1u, params.width);
  const double height = std::max<uint32_t>(1u, params.height);
  encoder.setViewport(WMTViewport{0.0, 0.0, width, height, 0.0, 1.0});
  encoder.setScissorRect(WMTScissorRect{0, 0, static_cast<uint64_t>(width), static_cast<uint64_t>(height)});
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  encoder.endEncoding();
  commandBuffer.presentDrawable(drawable);

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
  if (!source || !source->texture) {
    perf::countPresentSkipped();
    presentimpl::traceEvent("missing-source", seqId, present.window.value);
    return false;
  }
  obj_handle_t sourceTextureHandle =
      source->resolveTexture ? source->resolveTexture.handle : source->texture.handle;
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
  params.maxDrawableCount = present.maxFrameLatency;
  params.opaqueAlpha = opaqueAlpha;
  params.seqId = seqId;

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
