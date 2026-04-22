#include "dxmt9/dxmt9_device.hpp"
#include "backend_metal.hpp"
#include "dxmt9_shader_sources.hpp"
#include "../winemetal/Metal.hpp"

#include <string>
#include <utility>

namespace dxmt9 {

namespace {

std::string resolveShaderCachePath() {
  char buf[4096]{};
  WMTGetShaderCachePath(buf, sizeof(buf));
  return std::string(buf);
}

WMTMetalVersion selectMetalVersion(WMT::Device device) {
  // Conservative default without an OS-version probe: Apple GPUs get
  // Metal 3.2, everything else caps at 3.1 (matching dxmt's fallback for
  // non-Apple-family devices where 3.2 features aren't available).
  // A WMTGetOSVersion-gated refinement can replace this once that entry
  // point is wired through winemetal.
  if (device && device.supportsFamily(WMTGPUFamilyApple7)) {
    return WMTMetal320;
  }
  return WMTMetal310;
}

class DeviceImpl final : public Device {
 public:
  explicit DeviceImpl(const DEVICE_DESC& desc)
      : wmt_device_(desc.device),
        metalVersion_(selectMetalVersion(wmt_device_)),
        queue_(wmt_device_),
        limits_(desc.limits),
        shaderArchivePath_(resolveShaderCachePath()) {
    if (wmt_device_) {
      shaders::initShaderArchive(wmt_device_, shaderArchivePath_, shaderArchive_);
    }
    backend_ = core::makeMetalBackendDevice(limits_, wmt_device_, queue_,
                                             shaderArchive_, shaderArchivePath_, *this);
  }

  ~DeviceImpl() override {
    // Destroy backend first (joins threads), then persist the archive one last
    // time. Pipeline-builder tasks that persist mid-run already handle their
    // own writes; this catches any final state.
    backend_.reset();
    if (shaderArchive_) {
      shaders::persistShaderArchive(shaderArchive_, shaderArchivePath_);
    }
  }

  WMT::Device wmtDevice() override { return wmt_device_; }
  WMTMetalVersion metalVersion() const override { return metalVersion_; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<core::BackendDevice> backend() override { return backend_; }
  WMT::Reference<WMT::BinaryArchive>* shaderArchive() override { return &shaderArchive_; }
  const std::string* shaderArchivePath() override { return &shaderArchivePath_; }

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
    maxFrameLatency_ = latency == 0 ? 1u : (latency > 3u ? 3u : latency);
  }
  std::uint32_t maxFrameLatency() const override { return maxFrameLatency_; }

  bool ready() const noexcept { return static_cast<bool>(backend_); }

 private:
  WMT::Reference<WMT::Device> wmt_device_;
  WMTMetalVersion metalVersion_ = WMTMetalVersionMax;
  CommandQueue queue_;
  core::BackendLimits limits_{};
  std::string shaderArchivePath_{};
  WMT::Reference<WMT::BinaryArchive> shaderArchive_{};
  core::BackendDevice::DeviceLostObserver deviceLostObserver_{};
  core::BackendDevice::PresentationStatusObserver presentationStatusObserver_{};
  std::uint32_t maxFrameLatency_ = 3;
  // backend_ is declared last so it destructs first; see ~DeviceImpl().
  std::shared_ptr<core::BackendDevice> backend_;
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
