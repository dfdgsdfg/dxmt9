#pragma once

#include "dxmt9/core.hpp"
#include "../winemetal/winemetal.h"

#include <optional>
#include <span>
#include <string>

namespace WMT {
class Device;
}

namespace dxmt9::core::metalcapture {

u64 gpuDumpTextureHandle();
const char* gpuDumpTexturePath();
bool writeTextureBmp(const std::string& path, Format format, u32 width, u32 height, u32 pitch,
                     std::span<const u8> bytes);

struct MetalCaptureConfig {
  u64 targetFrame = 0;
  std::string path;
  WMTCaptureDestination destination = WMTCaptureDestinationGPUTraceDocument;

  bool enabled() const noexcept { return targetFrame != 0; }
};

struct MetalCaptureRequest {
  u64 frame = 0;
  u64 seqId = 0;
  std::string path;
  WMTCaptureDestination destination = WMTCaptureDestinationGPUTraceDocument;
};

MetalCaptureConfig metalCaptureConfigFromEnv();
std::string defaultMetalCapturePath(u64 frame, u64 seqId);

class MetalCaptureController {
 public:
  MetalCaptureController();
  explicit MetalCaptureController(MetalCaptureConfig config);

  bool enabled() const noexcept { return config_.enabled(); }
  u64 targetFrame() const noexcept { return config_.targetFrame; }
  u64 observedPresentFrames() const noexcept { return observedPresentFrames_; }

  // Fires on the first chunk seen *after* the (targetFrame - 1)-th Present
  // — i.e. the first chunk of the target frame. Returns nullopt thereafter
  // until reset. The returned request is also recorded as the "active
  // session" so `maybePresentChunkClosesSession` can later identify the
  // chunk that should stop the capture.
  std::optional<MetalCaptureRequest> maybeCaptureAtChunkBegin(u64 seqId);

  // Called when a chunk containing a Present command is being encoded.
  // Bumps `observedPresentFrames_`. Returns the active capture request if
  // this Present is the target frame's Present (i.e. this is the chunk
  // whose commit should stop the capture). Otherwise returns nullopt.
  std::optional<MetalCaptureRequest> maybePresentChunkClosesSession(u64 seqId);

  // Legacy: equivalent to bumping the frame counter and (for the original
  // present-chunk-only capture) returning the request if the target hits.
  // Kept for callers that haven't migrated to the begin+close pair.
  std::optional<MetalCaptureRequest> maybeCapturePresentChunk(u64 seqId);

 private:
  MetalCaptureConfig config_{};
  u64 observedPresentFrames_ = 0;
  bool requested_ = false;
  // After (targetFrame - 1) presents have been observed, the next
  // chunk-begin is the first chunk of the target frame; arm capture.
  bool armedForChunkBegin_ = false;
  // Active capture request once chunk-begin starts the session. Cleared
  // when `maybePresentChunkClosesSession` consumes it.
  std::optional<MetalCaptureRequest> activeSession_{};
};

// Per-draw Metal debug groups ("Draw[seq=,prim=]") are read only inside a GPU
// capture, but cost a string format, a bridged WMT::String allocation, and a
// pushDebugGroup/popDebugGroup pair on EVERY draw -- 2.7ms/present on GT2
// (state-churn-encode-append-decomposition.16). So they are emitted only when
// this process is set up to capture, or when explicitly forced with
// DXMT9_PER_DRAW_DEBUG_GROUPS.
//
// Per-render-pass, blit and present debug groups are NOT gated: there are a few
// per frame rather than ~1,690, so they are not worth the risk. (They are not
// what xctrace joins on -- summarize_xctrace_metal_intervals.py matches the
// encoder LABEL set by setLabel, not any debug group. Nothing in the repo joins
// on a debug group.)
bool perDrawDebugGroupsEnabled();

bool startMetalCapture(const WMT::Device& device, const MetalCaptureRequest& request);
void stopMetalCapture(const MetalCaptureRequest& request);

}  // namespace dxmt9::core::metalcapture
