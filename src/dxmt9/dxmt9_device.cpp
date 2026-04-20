#include "dxmt9/dxmt9_device.hpp"
#include "backend_metal.hpp"

#include <utility>

namespace dxmt9 {

namespace {

class DeviceImpl final : public Device {
 public:
  explicit DeviceImpl(const DEVICE_DESC& desc)
      : wmt_device_(desc.device),
        queue_(wmt_device_),
        limits_(desc.limits),
        backend_(core::makeMetalBackendDevice(limits_, wmt_device_, queue_)) {}

  WMT::Device wmtDevice() override { return wmt_device_; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<core::BackendDevice> backend() override { return backend_; }

  bool ready() const noexcept { return static_cast<bool>(backend_); }

 private:
  WMT::Reference<WMT::Device> wmt_device_;
  CommandQueue queue_;
  core::BackendLimits limits_{};
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
