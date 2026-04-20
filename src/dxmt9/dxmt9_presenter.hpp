#pragma once

// Upper-runtime Presenter — per-window wrapper around a WMT::MetalLayer.
//
// Mirrors dxmt's class Presenter (dxmt/src/dxmt/dxmt_presenter.hpp). Owns a
// handle to the CAMetalLayer created by the macdrv bridge, applies layer
// properties, acquires the next drawable, and encodes the present blit into
// a caller-provided WMT::CommandBuffer. Does NOT own the layer's lifetime:
// the CAMetalLayer is held by the HWND's NSView via macdrv, so the Presenter
// only keeps a weak handle.

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
  };

  struct EncodeResult {
    bool acquired = false;               // nextDrawable succeeded
    bool encoded = false;                // render encoder was opened + committed
  };

  Presenter(WMT::Device device, WMT::MetalLayer layer);

  EncodeResult encodeCommands(WMT::CommandBuffer& commandBuffer, const EncodeParams& params);

  WMT::MetalLayer layer() const noexcept { return layer_; }

 private:
  WMT::Device device_;
  WMT::MetalLayer layer_;
};

}  // namespace dxmt9
