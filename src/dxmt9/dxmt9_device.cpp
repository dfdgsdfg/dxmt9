#include "dxmt9_device.hpp"
#include "../winemetal/Metal.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace dxmt9 {

namespace {

WMTMetalVersion selectMetalVersion(WMT::Device device) {
  // Conservative default without an OS-version probe: Apple GPUs get
  // Metal 3.2, everything else caps at 3.1 (matching dxmt's fallback for
  // non-Apple-family devices where 3.2 features aren't available).
  if (device && device.supportsFamily(WMTGPUFamilyApple7)) {
    return WMTMetal320;
  }
  return WMTMetal310;
}

core::BackendLimits finalizeLimits(core::BackendLimits base, WMT::Device device) {
  if (device) {
    base.supportsDepth24Stencil8 = device.supportsDepth24Stencil8();
  }
  return base;
}

std::optional<std::uint32_t> envU32(const char* name) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value) {
    return std::nullopt;
  }
  if (parsed > std::numeric_limits<std::uint32_t>::max()) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return static_cast<std::uint32_t>(parsed);
}

std::uint32_t normalizeMaxFrameLatency(std::uint32_t latency) {
  return latency == 0 ? core::kDefaultFrameLatency
                      : std::min(latency, core::kMaxFrameLatency);
}

std::uint32_t effectiveMaxFrameLatency(std::uint32_t latency) {
  static const std::optional<std::uint32_t> override = [] {
    const auto value = envU32("DXMT9_MAX_FRAME_LATENCY");
    if (!value) {
      return std::optional<std::uint32_t>{};
    }
    return std::optional<std::uint32_t>{normalizeMaxFrameLatency(*value)};
  }();
  return override.value_or(normalizeMaxFrameLatency(latency));
}

class DeviceImpl final : public Device {
 public:
  explicit DeviceImpl(const DEVICE_DESC& desc)
      : wmt_device_(desc.device),
        metalVersion_(selectMetalVersion(wmt_device_)),
        limits_(finalizeLimits(desc.limits, wmt_device_)),
        queue_(wmt_device_, limits_) {}

  // queue_ destructs first (last-declared) — joins worker threads,
  // then queue-owned pool/cache/archive destruct in member-reverse
  // order (archive persists to disk last).
  ~DeviceImpl() override = default;

  WMT::Device wmtDevice() override { return wmt_device_; }
  WMTMetalVersion metalVersion() const override { return metalVersion_; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  // backend() stays nullptr in production. Tests observe through
  // StubDxmt9Device + MockBackendDevice.
  std::shared_ptr<core::BackendDevice> backend() override { return nullptr; }
  WMT::Reference<WMT::BinaryArchive>* shaderArchive() override {
    return &queue_.shaderArchive().reference();
  }
  const std::string* shaderArchivePath() override {
    return &queue_.shaderArchive().path();
  }
  resources::Pool* pool() override { return &queue_.pool(); }
  pipeline::Cache* pipelineCache() override { return &queue_.pipelineCache(); }

  void setDeviceLostObserver(core::BackendDevice::DeviceLostObserver observer) override {
    deviceLostObserver_ = std::move(observer);
  }
  void setPresentationStatusObserver(core::BackendDevice::PresentationStatusObserver observer) override {
    presentationStatusObserver_ = std::move(observer);
  }
  void notifyDeviceLost(bool lost) override {
    if (deviceLostObserver_) deviceLostObserver_(lost);
  }
  void notifyPresentationStatus(bool occluded) override {
    if (presentationStatusObserver_) presentationStatusObserver_(occluded);
  }
  void setMaxFrameLatency(std::uint32_t latency) override {
    maxFrameLatency_ = effectiveMaxFrameLatency(latency);
  }
  std::uint32_t maxFrameLatency() const override { return maxFrameLatency_; }

  core::BufferHandle createBuffer(const core::BufferDesc& desc) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().createBuffer(wmt_device_, desc);
  }
  core::TextureHandle createTexture(const core::TextureDesc& desc) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().createTexture(wmt_device_, limits_, desc);
  }
  core::SurfaceHandle createSurface(const core::SurfaceDesc& desc) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().createSurface(wmt_device_, limits_, desc);
  }
  core::SurfaceHandle createSurfaceForTexture(core::TextureHandle handle, std::uint32_t level,
                                                const core::SurfaceDesc& desc) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().createSurfaceForTexture(handle, level, desc);
  }
  void destroyBuffer(core::BufferHandle handle) override {
    std::lock_guard lock(queue_.mutex_);
    queue_.pool().markBufferDestroyAndGc(handle.value, queue_.completedSeqId_);
  }
  void destroyTexture(core::TextureHandle handle) override {
    std::lock_guard lock(queue_.mutex_);
    queue_.pool().markTextureDestroyAndGc(handle.value, queue_.completedSeqId_);
  }
  void destroySurface(core::SurfaceHandle handle) override {
    std::lock_guard lock(queue_.mutex_);
    queue_.pool().markSurfaceDestroyAndGc(handle.value, queue_.completedSeqId_);
  }
  void unmapBuffer(core::BufferHandle) override {}
  void uploadBufferData(core::BufferHandle handle, std::span<const std::uint8_t> bytes) override {
    std::lock_guard lock(queue_.mutex_);
    queue_.pool().uploadBufferData(handle.value, bytes.data(), bytes.size());
  }
  void* mapBuffer(core::BufferHandle handle, std::uint32_t flags) override {
    return queue_.mapBuffer(handle, flags);
  }
  void uploadTextureLevel(core::TextureHandle handle, std::uint32_t level,
                           std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                           std::span<const std::uint8_t> bytes) override {
    queue_.uploadTextureLevel(handle, level, width, height, pitch, bytes);
  }

  void submitDrawRun(core::CanonicalDrawState state,
                     const core::DrawUniformPayload& uniforms,
                     std::span<const core::DrawParam> draws,
                     std::span<const core::DrawParamPayloadView> payloads) override {
    queue_.submitDrawRun(std::move(state), uniforms, draws, payloads);
  }
  void markChunkResources(std::span<const core::ChunkHandleEntry> entries) override {
    queue_.markChunkResources(entries);
  }
  void setSkipDrawResourceMarking(bool skip) override {
    queue_.setSkipDrawResourceMarking(skip);
  }
  void submitClear(const core::ClearDesc& desc) override { queue_.submitClear(desc); }
  void submitSurfaceCopy(const core::SurfaceCopyDesc& desc) override {
    queue_.submitSurfaceCopy(desc);
  }
  void submitStretchRect(const core::StretchRectDesc& desc) override {
    queue_.submitStretchRect(desc);
  }
  void submitReadback(const core::ReadbackDesc& desc) override {
    queue_.submitReadback(desc);
  }
  void submitColorFill(const core::ColorFillDesc& desc) override {
    queue_.submitColorFill(desc);
  }
  void present(const core::SwapDesc& desc) override {
    // Inject the per-present back-channels the queue's encode thread
    // would otherwise need a Device* to reach. Lifetime is safe: queue_
    // is the last-declared member, so its dtor (which joins the encode
    // thread) runs before `this` is destroyed.
    core::SwapDesc augmented = desc;
    augmented.maxFrameLatency = maxFrameLatency_;
    augmented.notifyPresentationStatus = [this](bool occluded) {
      notifyPresentationStatus(occluded);
    };
    queue_.submitPresent(augmented);
  }
  void flush() override { queue_.submitFlush(); }
  core::HResult waitForVBlank(const core::SwapDesc&) override { return queue_.waitForVBlank(); }
  bool readbackSurface(const core::ReadbackDesc& desc, core::ReadbackPixels& pixels) override {
    return queue_.readbackSurface(desc, pixels);
  }
  bool supportsGpuReadback() const override { return true; }

  bool ready() const noexcept { return queue_.started(); }

 private:
  // Pool / pipeline cache / shader archive / allocators all live INSIDE
  // CommandQueue (matches upstream dxmt). DeviceImpl forwards the
  // virtual accessors above to queue_'s queue-owned members.
  WMT::Reference<WMT::Device> wmt_device_;
  WMTMetalVersion metalVersion_ = WMTMetalVersionMax;
  core::BackendLimits limits_{};
  core::BackendDevice::DeviceLostObserver deviceLostObserver_{};
  core::BackendDevice::PresentationStatusObserver presentationStatusObserver_{};
  std::uint32_t maxFrameLatency_ = core::kDefaultFrameLatency;
  CommandQueue queue_;
};

}  // namespace

std::unique_ptr<Device> CreateDXMT9Device(const DEVICE_DESC& desc) {
  auto device = std::make_unique<DeviceImpl>(desc);
  if (!device->ready()) {
    return nullptr;
  }
  return device;
}

}  // namespace dxmt9
