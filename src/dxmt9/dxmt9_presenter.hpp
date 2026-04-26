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
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace dxmt9 {

class Device;

inline constexpr uint32_t kDefaultMetalDrawableCount = 3;

class PresentDrawableToken {
 public:
  PresentDrawableToken() = default;
  explicit PresentDrawableToken(WMT::Reference<WMT::MetalDrawable> drawable)
      : drawable_(std::move(drawable)), ready_(true) {}

  void complete(WMT::Reference<WMT::MetalDrawable> drawable);
  void fail();
  WMT::MetalDrawable waitDrawable();

 private:
  mutable std::mutex mutex_{};
  std::condition_variable cv_{};
  WMT::Reference<WMT::MetalDrawable> drawable_{};
  bool ready_ = false;
};

class Presenter {
 public:
  struct AcquireParams {
    uint32_t width = 0;
    uint32_t height = 0;
    bool displaySyncEnabled = true;
    double contentsScale = 1.0;
    uint32_t maxDrawableCount = kDefaultMetalDrawableCount;
    uint64_t seqId = 0;
  };

  struct EncodeParams {
    WMT::Texture source;                 // backbuffer texture (resolve target for MSAA)
    uint32_t width = 0;                  // drawable width
    uint32_t height = 0;                 // drawable height
    bool displaySyncEnabled = true;      // vsync
    double contentsScale = 1.0;          // CAMetalLayer.contentsScale
    double minimumPresentDuration = 0.0; // presentDrawableAfterMinimumDuration
    uint32_t maxDrawableCount = kDefaultMetalDrawableCount;  // CAMetalLayer.maximumDrawableCount
    bool opaqueAlpha = false;            // X8R8G8B8/X8B8G8R8 → force alpha=1
    uint64_t seqId = 0;                  // for trace events
    std::shared_ptr<PresentDrawableToken> drawableToken{};
    bool drawableTokenRequired = false;
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

  std::shared_ptr<PresentDrawableToken> acquireDrawable(const AcquireParams& params);
  std::shared_ptr<PresentDrawableToken> beginAcquireDrawable(const AcquireParams& params);
  EncodeResult encodeCommands(WMT::CommandBuffer& commandBuffer, const EncodeParams& params);
  void preAcquireNextDrawable(uint64_t seqId);

 private:
  // Return the pipeline future for the requested variant, kicking off the
  // build if this is the first request.
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>>&
      pipelineFor(bool opaqueAlpha);
  void configureLayer(const AcquireParams& params);
  WMT::Reference<WMT::MetalDrawable> takePrefetchedDrawable();
  void discardPrefetchedDrawable();
  void finishAsyncAcquireToken();
  void runAsyncAcquireLoop();
  void runPreAcquireLoop();

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
  std::mutex stateMutex_{};
  WMTLayerProps cachedLayerProps_{};
  uint32_t cachedMaxDrawableCount_ = 0;
  bool cachedLayerPropsValid_ = false;
  std::mutex preAcquireMutex_{};
  std::condition_variable preAcquireCv_{};
  std::thread preAcquireThread_{};
  WMT::Reference<WMT::MetalDrawable> prefetchedDrawable_{};
  uint64_t preAcquireSeqId_ = 0;
  uint64_t preAcquireGeneration_ = 0;
  bool preAcquireStop_ = false;
  bool preAcquireRequested_ = false;
  bool preAcquireInFlight_ = false;

  struct AsyncAcquireRequest {
    AcquireParams params{};
    std::shared_ptr<PresentDrawableToken> token{};
  };
  std::mutex asyncAcquireMutex_{};
  std::condition_variable asyncAcquireCv_{};
  std::thread asyncAcquireThread_{};
  std::deque<AsyncAcquireRequest> asyncAcquireRequests_{};
  bool asyncAcquireDrawableHeld_ = false;
  bool asyncAcquireStop_ = false;

  // Borrowed from DeviceImpl; used by the async pipeline builder.
  WMT::Reference<WMT::BinaryArchive>* archive_ = nullptr;
  const std::string* archivePath_ = nullptr;
};

// Orchestrates a present: resolves the source surface via `pool`, applies
// the DXMT_FORCE_PRESENT_TEXTURE_HANDLE override (debug-only), builds
// EncodeParams from `present`, calls Presenter::encodeCommands, and fires
// present.notifyPresentationStatus based on the nextDrawable outcome.
// Returns true if the presenter drew (caller may then set a
// discard-after-present flag).
//
// Extracted out of MetalBackendDevice::encodePresent so that present policy
// lives with the Presenter rather than the Renderer.
bool encodePresent(WMT::CommandBuffer& commandBuffer,
                   resources::Pool& pool,
                   const core::SwapDesc& present,
                   core::Handle sourceHandle,
                   std::uint64_t seqId);

}  // namespace dxmt9
