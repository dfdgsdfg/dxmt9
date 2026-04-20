#include "dxmt9_presenter.hpp"
#include "backend_metal.hpp"

#include <algorithm>

namespace dxmt9 {

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
    slot = core::buildPresentPipeline(WMT::Reference<WMT::Device>{device_.handle},
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
    MetalLayer_setProps(layer_.handle, &props);
    MetalLayer_setMaximumDrawableCount(layer_.handle,
                                        std::clamp<uint32_t>(params.maxDrawableCount, 1u, 3u));
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
  auto drawable = layer_.nextDrawable();
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

}  // namespace dxmt9
