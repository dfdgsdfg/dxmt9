#pragma once

#include "dxmt9/core.hpp"

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

  bool enabled() const noexcept { return targetFrame != 0; }
};

struct MetalCaptureRequest {
  u64 frame = 0;
  u64 seqId = 0;
  std::string path;
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

  std::optional<MetalCaptureRequest> maybeCapturePresentChunk(u64 seqId);

 private:
  MetalCaptureConfig config_{};
  u64 observedPresentFrames_ = 0;
  bool requested_ = false;
};

bool startMetalCapture(const WMT::Device& device, const MetalCaptureRequest& request);
void stopMetalCapture(const MetalCaptureRequest& request);

}  // namespace dxmt9::core::metalcapture
