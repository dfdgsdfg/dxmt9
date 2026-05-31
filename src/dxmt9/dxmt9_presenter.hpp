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
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace dxmt9 {

class Device;

inline constexpr uint32_t kDefaultMetalDrawableCount = 3;

// Present-drawable acquire policy. The four values collapse the
// previous if/else chain that walked
// DXMT9_PRESENT_ASYNC_ACQUIRE / DXMT9_PRESENT_ACQUIRE_ON_SUBMIT /
// DXMT9_PRESENT_PREACQUIRE in separate static-lambda env parsers
// spread across Presenter and CommandQueue. Resolution is done once
// per process via resolveAcquirePolicyFromEnv() and cached in
// Presenter::policy_.
//
// Priority when multiple env-vars are set simultaneously (highest
// first): Async > SyncOnSubmit > PreAcquire > Sync. This matches the
// pre-existing if/else order in CommandQueue::submitPresent (the
// async branch was checked before the sync-on-submit branch). For the
// common case where at most one env-var is set, behavior is
// unchanged.
//
// This is intentionally still a policy surface, not a hard-coded winner:
// drawable acquisition is workload/compositor sensitive. Keep alternate
// modes behind env selection and compare present_acquire_wait_*,
// present_token_wait_*, present_boundary_wait_*, and frame pacing counters
// before changing the default.
enum class AcquirePolicy : uint32_t {
  Sync = 0,        // default — acquire inline in encodeCommands.
  SyncOnSubmit,    // DXMT9_PRESENT_ACQUIRE_ON_SUBMIT — sync acquire at submit.
  PreAcquire,      // DXMT9_PRESENT_PREACQUIRE — prefetch via background thread.
  Async,           // DXMT9_PRESENT_ASYNC_ACQUIRE — async acquire at submit.
};

// Pure resolver — takes explicit env strings (nullptr / "" / "0"
// count as "not set"). Used by Presenter and the spec test.
AcquirePolicy resolveAcquirePolicy(const char* asyncEnv,
                                   const char* onSubmitEnv,
                                   const char* preAcquireEnv);

// Process-once env reader; reads DXMT9_PRESENT_{ASYNC_ACQUIRE,
// ACQUIRE_ON_SUBMIT, PREACQUIRE} via std::getenv on first call and
// caches the result.
AcquirePolicy resolveAcquirePolicyFromEnv();

// Present-boundary policy. The five values collapse the previous
// trio of static-lambda env parsers that walked
// DXMT9_DISABLE_PRESENT_BOUNDARY /
// DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION /
// DXMT9_PRESENT_BOUNDARY_COMPLETION in CommandQueue, plus the
// DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE bit consumed in the present
// encode path. Resolution is done once per process via
// resolveBoundaryPolicyFromEnv() and cached behind a process-wide
// reader; branch sites read this directly instead of going through
// scattered env-parsing lambdas.
//
// Priority when multiple env-vars are set simultaneously (highest
// first): Disabled > PresentCompletion > Completion > AfterAcquire >
// Default. This matches the pre-existing if/else order in
// CommandQueue::presentBoundary: DXMT9_DISABLE_PRESENT_BOUNDARY=1
// short-circuits the whole boundary; otherwise the default-true
// PresentCompletion branch wins over Completion, which in turn wins
// over the legacy Dequeued (presentDequeued CV) path. The
// AfterAcquire bit is observationally a no-op when the wait branch
// is PresentCompletion or Completion (those branches do not consult
// presentDequeuedSeqId_), so collapsing it into the lower-priority
// enum slot preserves behavior exactly.
//
// Like AcquirePolicy, this remains an A/B policy surface until a workload
// class has enough latency/stutter/drawable-wait data to justify a new
// default. Env overrides are the app-class escape hatch.
enum class BoundaryPolicy : uint32_t {
  Default = 0,         // wait on presentDequeuedSeqId_ — note dequeued before encode.
  AfterAcquire,        // DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE — same wait, note after encode.
  Completion,          // DXMT9_PRESENT_BOUNDARY_COMPLETION — wait on completedSeqId_.
  PresentCompletion,   // DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION (default on) — wait on presentCompletedSeqId_.
  Disabled,            // DXMT9_DISABLE_PRESENT_BOUNDARY — skip the boundary altogether.
};

// Pure resolver — takes explicit env strings (nullptr / "" / "0"
// count as "not set"). Used by CommandQueue and the spec test. Note
// the unset-presentCompletionEnv default: when the caller passes
// nullptr/empty for presentCompletionEnv it counts as set (matches
// the pre-normalization default-true behavior of the historical
// lambda).
BoundaryPolicy resolveBoundaryPolicy(const char* disableEnv,
                                     const char* presentCompletionEnv,
                                     const char* completionEnv,
                                     const char* afterAcquireEnv);

// Process-once env reader; reads DXMT9_DISABLE_PRESENT_BOUNDARY /
// DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION /
// DXMT9_PRESENT_BOUNDARY_COMPLETION /
// DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE via std::getenv on first call
// and caches the result.
BoundaryPolicy resolveBoundaryPolicyFromEnv();

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
    // G2-B Option A — per-present gamma ramp. When gammaRampIsIdentity is
    // true the encoder selects the standard textured-blit PSO (fast path,
    // matches the pre-G2-B present behavior); when false the encoder
    // selects the gamma-apply PSO variant and uploads the 1.5 KB ramp via
    // setFragmentBytes(buffer 0). The pointer is borrowed from
    // SwapDesc::gammaRamp; encoders::encodePresent populates both fields
    // from the SwapDesc snapshot.
    const core::GammaRamp* gammaRamp = nullptr;
    bool gammaRampIsIdentity = true;
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments{};
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
  AcquirePolicy acquirePolicy() const noexcept { return policy_; }

  std::shared_ptr<PresentDrawableToken> acquireDrawable(const AcquireParams& params);
  std::shared_ptr<PresentDrawableToken> beginAcquireDrawable(const AcquireParams& params);
  EncodeResult encodeCommands(WMT::CommandBuffer& commandBuffer, const EncodeParams& params);
  void preAcquireNextDrawable(uint64_t seqId);

 private:
  // Return the pipeline future for the requested variant, kicking off the
  // build if this is the first request. `applyGamma=true` selects the
  // gamma-apply PSO variant; the identity / non-applied fast-path remains
  // the standard textured-blit PSO so apps that never call SetGammaRamp
  // pay no extra cost.
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>>&
      pipelineFor(bool opaqueAlpha, bool applyGamma);
  void configureLayer(const AcquireParams& params);
  WMT::Reference<WMT::MetalDrawable> takeOrWaitForPrefetchedDrawable();
  void discardPrefetchedDrawable();
  void finishAsyncAcquireToken();
  void runAsyncAcquireLoop();
  void runPreAcquireLoop();

  WMT::Device device_{};
  uint64_t hwnd_ = 0;
  presentimpl::LayerAcquisition acquisition_{};
  WMT::MetalLayer layer_{};
  // Acquire policy is resolved once at construction from env (process-
  // wide cache). Branch sites read this directly instead of going
  // through scattered env-parsing lambdas.
  AcquirePolicy policy_ = AcquirePolicy::Sync;
  // Pipeline cache — four variants: alpha-preserving / alpha-forced-to-1
  // crossed with identity-gamma (default present) / gamma-apply (G2-B
  // Option A). std::shared_future lets concurrent encodeCommands calls
  // share the result of a single async build. Lazy: the gamma variants
  // are not constructed until the first non-identity SetGammaRamp present
  // selects them, so the steady-state cost for apps that never call
  // SetGammaRamp is exactly one pipeline build per opaqueAlpha flavor.
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineAlpha_{};
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineOpaque_{};
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineGammaAlpha_{};
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> pipelineGammaOpaque_{};
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
// `presenter` is the queue-resolved binding (CommandQueue::lookupPresenter
// on `present.presentId`); a null pointer means the swapchain either
// never had a layer (test path) or was destroyed between submission and
// encode — both fall through to a skipped present.
// `drawableToken` is the optional queue-stashed token from an
// acquire-before-present experiment (CommandQueue::takeDrawableToken on
// the same id). The legacy shared_ptr / Presenter* fields no longer
// live on the PE-visible core::SwapDesc.
bool encodePresent(WMT::CommandBuffer& commandBuffer,
                   resources::Pool& pool,
                   Presenter* presenter,
                   std::shared_ptr<PresentDrawableToken> drawableToken,
                   const core::SwapDesc& present,
                   core::Handle sourceHandle,
                   std::uint64_t seqId,
                   std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments = {});

}  // namespace dxmt9
