#include "dxmt9/dxmt9_device.hpp"
#include "backend_metal.hpp"
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

void initShaderArchive(WMT::Device& device, const std::string& path,
                       WMT::Reference<WMT::BinaryArchive>& archiveOut) {
  WMT::Error err{};
  auto archive = device.newBinaryArchive(path.c_str(), err);
  archiveOut = std::move(archive);
}

void persistShaderArchive(WMT::BinaryArchive& archive, const std::string& path) {
  if (!archive || path.empty()) {
    return;
  }
  WMT::Error err{};
  archive.serialize(path.c_str(), err);
}

class DeviceImpl final : public Device {
 public:
  explicit DeviceImpl(const DEVICE_DESC& desc)
      : wmt_device_(desc.device),
        queue_(wmt_device_),
        limits_(desc.limits),
        shaderArchivePath_(resolveShaderCachePath()) {
    if (wmt_device_) {
      initShaderArchive(wmt_device_, shaderArchivePath_, shaderArchive_);
    }
    backend_ = core::makeMetalBackendDevice(limits_, wmt_device_, queue_,
                                             shaderArchive_, shaderArchivePath_);
  }

  ~DeviceImpl() override {
    // Destroy backend first (joins threads), then persist the archive one last
    // time. Pipeline-builder tasks that persist mid-run already handle their
    // own writes; this catches any final state.
    backend_.reset();
    if (shaderArchive_) {
      persistShaderArchive(shaderArchive_, shaderArchivePath_);
    }
  }

  WMT::Device wmtDevice() override { return wmt_device_; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<core::BackendDevice> backend() override { return backend_; }

  bool ready() const noexcept { return static_cast<bool>(backend_); }

 private:
  WMT::Reference<WMT::Device> wmt_device_;
  CommandQueue queue_;
  core::BackendLimits limits_{};
  std::string shaderArchivePath_{};
  WMT::Reference<WMT::BinaryArchive> shaderArchive_{};
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
