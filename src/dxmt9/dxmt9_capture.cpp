#include "dxmt9_capture.hpp"

#include "../winemetal/Metal.hpp"
#include "util/util_bmp.hpp"
#include "util/log/log.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace dxmt9::core::metalcapture {

namespace {

std::optional<u64> parseEnvU64(const char* name) {
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return std::nullopt;
  }
  errno = 0;
  char* end = nullptr;
  const auto value = std::strtoull(env, &end, 0);
  if (errno != 0 || end == env || (end && *end != '\0')) {
    return std::nullopt;
  }
  return static_cast<u64>(value);
}

std::string optionalEnvString(const char* name) {
  const char* env = std::getenv(name);
  return env && env[0] != '\0' ? std::string(env) : std::string{};
}

}  // namespace

u64 gpuDumpTextureHandle() {
  static const u64 handle = [] {
    const char* env = std::getenv("DXMT_DUMP_GPU_TEXTURE_HANDLE");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return static_cast<u64>(std::strtoull(env, nullptr, 0));
  }();
  return handle;
}

const char* gpuDumpTexturePath() {
  static const char* path = std::getenv("DXMT_DUMP_GPU_TEXTURE_PATH");
  return path && path[0] != '\0' ? path : nullptr;
}

bool writeTextureBmp(const std::string& path, Format format, u32 width, u32 height, u32 pitch,
                     std::span<const u8> bytes) {
  return dxmt9::util::writeBmp(path, format, width, height, pitch, bytes);
}

MetalCaptureConfig metalCaptureConfigFromEnv() {
  MetalCaptureConfig config;
  config.targetFrame = parseEnvU64("DXMT_METAL_CAPTURE_FRAME").value_or(0);
  config.path = optionalEnvString("DXMT_METAL_CAPTURE_PATH");
  return config;
}

std::string defaultMetalCapturePath(u64 frame, u64 seqId) {
  std::ostringstream name;
  name << "dxmt9_frame_" << frame << "_seq_" << seqId << ".gputrace";
#ifndef _WIN32
  char cwd[4096]{};
  if (getcwd(cwd, sizeof(cwd)) && cwd[0] != '\0') {
    return std::string(cwd) + "/" + name.str();
  }
#endif
  return name.str();
}

MetalCaptureController::MetalCaptureController()
    : MetalCaptureController(metalCaptureConfigFromEnv()) {}

MetalCaptureController::MetalCaptureController(MetalCaptureConfig config)
    : config_(std::move(config)) {}

std::optional<MetalCaptureRequest> MetalCaptureController::maybeCapturePresentChunk(u64 seqId) {
  if (!enabled() || requested_) {
    return std::nullopt;
  }
  ++observedPresentFrames_;
  if (observedPresentFrames_ != config_.targetFrame) {
    return std::nullopt;
  }
  requested_ = true;
  return MetalCaptureRequest{
      .frame = observedPresentFrames_,
      .seqId = seqId,
      .path = config_.path.empty() ? defaultMetalCapturePath(observedPresentFrames_, seqId)
                                   : config_.path,
  };
}

bool startMetalCapture(const WMT::Device& device, const MetalCaptureRequest& request) {
  if (!device || request.path.empty()) {
    return false;
  }

  auto captureManager = WMT::CaptureManager::sharedCaptureManager();
  if (!captureManager) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Warn, "dxmt9-capture",
                      "Metal capture frame=%llu seq=%llu unavailable: no capture manager",
                      static_cast<unsigned long long>(request.frame),
                      static_cast<unsigned long long>(request.seqId));
    return false;
  }

  WMTCaptureInfo info{};
  info.capture_object = device.handle;
  info.destination = WMTCaptureDestinationGPUTraceDocument;
  info.output_url.set(request.path.c_str());
  const bool started = captureManager.startCapture(info);
  dxmt9::util::logf(started ? dxmt9::util::LogLevel::Info : dxmt9::util::LogLevel::Warn,
                    "dxmt9-capture",
                    "Metal capture frame=%llu seq=%llu %s path=%s",
                    static_cast<unsigned long long>(request.frame),
                    static_cast<unsigned long long>(request.seqId),
                    started ? "started" : "failed",
                    request.path.c_str());
  return started;
}

void stopMetalCapture(const MetalCaptureRequest& request) {
  auto captureManager = WMT::CaptureManager::sharedCaptureManager();
  if (!captureManager) {
    return;
  }
  captureManager.stopCapture();
  dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-capture",
                    "Metal capture frame=%llu seq=%llu stopped path=%s",
                    static_cast<unsigned long long>(request.frame),
                    static_cast<unsigned long long>(request.seqId),
                    request.path.c_str());
}

}  // namespace dxmt9::core::metalcapture
