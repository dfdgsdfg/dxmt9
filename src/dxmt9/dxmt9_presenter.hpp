#pragma once

// Upper-runtime Presenter — per-window wrapper around a WMT::MetalLayer.
//
// Mirrors dxmt's class Presenter (dxmt/src/dxmt/dxmt_presenter.hpp). Each
// Presenter owns the per-hwnd macdrv resources (CAMetalLayer, MetalView,
// macdrv MetalDevice) acquired at construction and released in the
// destructor. The present pipeline + sampler are owned here too — dxmt's
// "fatter" Presenter shape.

#include "dxmt9_presenter_macdrv.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9/core.hpp"
#include "../winemetal/Metal.hpp"

#include <cstdint>
#include <future>
#include <string>

namespace dxmt9 {

class Device;

class Presenter {
 public:
  struct EncodeParams {
    WMT::Texture source;                 // backbuffer texture (resolve target for MSAA)
    uint32_t width = 0;                  // drawable width
    uint32_t height = 0;                 // drawable height
    bool displaySyncEnabled = true;      // vsync
    double contentsScale = 1.0;          // CAMetalLayer.contentsScale
    uint32_t maxDrawableCount = 3;       // CAMetalLayer.maximumDrawableCount
    bool opaqueAlpha = false;            // X8R8G8B8/X8B8G8R8 → force alpha=1
    uint64_t seqId = 0;                  // for trace events
  };

  struct EncodeResult {
    bool acquired = false;               // nextDrawable succeeded
    bool encoded = false;                // render encoder was opened + committed
  };

  // Acquire the hwnd's CAMetalLayer up-front; on failure valid() is false.
  // archive + archivePath are borrowed pointers (owned by DeviceImpl) used
  // for pipeline persistence — pass nullptr to skip.
  Presenter(WMT::Device device, uint64_t hwnd, uint64_t seqId,
            WMT::Reference<WMT::BinaryArchive>* archive,
            const std::string* archivePath);
  ~Presenter();

  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  bool valid() const noexcept { return acquisition_.valid(); }
  uint64_t hwnd() const noexcept { return hwnd_; }
  WMT::MetalLayer layer() const noexcept { return layer_; }

  EncodeResult encodeCommands(WMT::CommandBuffer& commandBuffer, const EncodeParams& params);

 private:
  // Return the pipeline future for the requested variant, kicking off the
  // build if this is the first request.
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>>&
      pipelineFor(bool opaqueAlpha);

  WMT::Device device_{};
  uint64_t hwnd_ = 0;
  presentimpl::LayerAcquisition acquisition_{};
  WMT::MetalLayer layer_{};
  // Pipeline cache — two variants (alpha-preserving + alpha-forced-to-1).
  // std::shared_future lets concurrent encodeCommands calls share the result
  // of a single async build.
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineAlpha_{};
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineOpaque_{};
  WMT::Reference<WMT::SamplerState> sampler_{};
  // Borrowed from DeviceImpl; used by the async pipeline builder.
  WMT::Reference<WMT::BinaryArchive>* archive_ = nullptr;
  const std::string* archivePath_ = nullptr;
};

// Orchestrates a present: resolves the source surface via `pool`, applies
// the DXMT_FORCE_PRESENT_TEXTURE_HANDLE override (debug-only), builds
// EncodeParams from `present`, calls Presenter::encodeCommands, and fires
// upperDevice->notifyPresentationStatus based on the nextDrawable outcome.
// Returns true if the presenter drew (caller may then set a
// discard-after-present flag).
//
// Extracted out of MetalBackendDevice::encodePresent so that present policy
// lives with the Presenter rather than the Renderer.
bool encodePresent(WMT::CommandBuffer& commandBuffer,
                   resources::Pool& pool,
                   Device* upperDevice,
                   const core::SwapDesc& present,
                   core::Handle sourceHandle,
                   std::uint64_t seqId);

}  // namespace dxmt9
