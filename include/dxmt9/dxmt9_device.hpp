#pragma once

// Upper-runtime dxmt9::Device — mirrors dxmt's dxmt::Device.
//
// The COM factory, D3D9 device, and swap chains are **consumers** of this
// object: Factory(std::unique_ptr<Device>) takes ownership; child D3D9 objects
// hold a shared_ptr to it. The Device owns the WMT Metal device, the command
// queue, and the backend implementation (built around WMT wrappers).

#include "../../src/winemetal/Metal.hpp"
#include "core.hpp"

#include <memory>

namespace dxmt9 {

class Device {
 public:
  virtual ~Device() = default;

  // The retained WMT Metal device.
  virtual WMT::Device wmtDevice() = 0;

  // Capability limits queried at construction.
  virtual const core::BackendLimits& limits() const = 0;

  // Shared backend implementation — for now, the concrete MetalBackendDevice.
  // Downstream core::Factory / core::Device consume this while the full
  // dissolution into Renderer/CommandQueue/Presenter is completed in
  // subsequent passes.
  virtual std::shared_ptr<core::BackendDevice> backend() = 0;
};

struct DEVICE_DESC {
  WMT::Device device;  // Metal device chosen by the caller (typically the first
                       // device returned by WMT::CopyAllDevices()).
  core::BackendLimits limits{};
};

std::unique_ptr<Device> CreateDXMT9Device(const DEVICE_DESC& desc);

}  // namespace dxmt9
