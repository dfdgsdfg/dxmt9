#pragma once

// Upper-runtime Presenter — per-window wrapper around a WMT::MetalLayer.
//
// Mirrors dxmt's class Presenter (dxmt/src/dxmt/dxmt_presenter.hpp). Each
// Presenter owns the per-hwnd macdrv resources (CAMetalLayer, MetalView,
// macdrv MetalDevice) acquired at construction and released in the
// destructor. Callers supply the present pipeline + sampler (still cached
// device-wide) as EncodeParams.

#include "dxmt9_presenter_macdrv.hpp"
#include "../winemetal/Metal.hpp"

#include <cstdint>

namespace dxmt9 {

class Presenter {
 public:
  struct EncodeParams {
    WMT::Texture source;                 // backbuffer texture (resolve target for MSAA)
    uint32_t width = 0;                  // drawable width
    uint32_t height = 0;                 // drawable height
    bool displaySyncEnabled = true;      // vsync
    double contentsScale = 1.0;          // CAMetalLayer.contentsScale
    uint32_t maxDrawableCount = 3;       // CAMetalLayer.maximumDrawableCount
    WMT::RenderPipelineState pipeline{}; // textured-blit pipeline (not owned)
    WMT::SamplerState sampler{};         // source sampler (not owned)
    uint64_t seqId = 0;                  // for trace events
  };

  struct EncodeResult {
    bool acquired = false;               // nextDrawable succeeded
    bool encoded = false;                // render encoder was opened + committed
  };

  // Acquire the hwnd's CAMetalLayer up-front; on failure valid() is false.
  Presenter(WMT::Device device, uint64_t hwnd, uint64_t seqId);
  ~Presenter();

  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  bool valid() const noexcept { return acquisition_.valid(); }
  uint64_t hwnd() const noexcept { return hwnd_; }
  WMT::MetalLayer layer() const noexcept { return layer_; }

  EncodeResult encodeCommands(WMT::CommandBuffer& commandBuffer, const EncodeParams& params);

 private:
  WMT::Device device_{};
  uint64_t hwnd_ = 0;
  presentimpl::LayerAcquisition acquisition_{};
  WMT::MetalLayer layer_{};
};

}  // namespace dxmt9
