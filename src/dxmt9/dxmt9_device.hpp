#pragma once

// Upper-runtime dxmt9::Device — mirrors dxmt's dxmt::Device.
//
// The COM factory, D3D9 device, and swap chains are **consumers** of this
// object: Factory(std::unique_ptr<Device>) takes ownership; child D3D9 objects
// hold a shared_ptr to it. The Device owns the WMT Metal device, the command
// queue, and the backend implementation (built around WMT wrappers).

#include "../winemetal/Metal.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9_command_queue.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace dxmt9 {

namespace resources { struct Pool; }
namespace pipeline { class Cache; }

class Device {
 public:
  virtual ~Device() = default;

  // The retained WMT Metal device.
  virtual WMT::Device wmtDevice() = 0;

  // Metal shading language version selected for this device. Mirrors
  // dxmt::Device::metalVersion(); determined at construction from the
  // macOS version + device family. Consumers (shader translator, pipeline
  // builders) use this to gate feature availability.
  virtual WMTMetalVersion metalVersion() const { return WMTMetalVersionMax; }

  // The upper-runtime CommandQueue (owns the WMT::CommandQueue handle).
  // Matches dxmt::Device::queue() in public shape. Returns a null-valued
  // CommandQueue on test paths where no Metal device is available.
  virtual CommandQueue& queue() = 0;

  // Capability limits queried at construction.
  virtual const core::BackendLimits& limits() const = 0;

  // Shared backend implementation — for now, the concrete MetalBackendDevice.
  // Downstream core::Factory / core::Device consume this while the full
  // dissolution into Renderer/CommandQueue/Presenter is completed in
  // subsequent passes.
  virtual std::shared_ptr<core::BackendDevice> backend() = 0;

  // Shader archive accessors — borrowed pointers used by Presenter to build
  // its present pipeline with cache persistence. May return nullptr on test
  // paths (StubDxmt9Device) or when the archive was not initialized.
  virtual WMT::Reference<WMT::BinaryArchive>* shaderArchive() { return nullptr; }
  virtual const std::string* shaderArchivePath() { return nullptr; }

  // Resource pool + pipeline cache accessors. CommandQueue reads these
  // through the upper Device at construction so its ctor signature
  // stays narrow. Default to nullptr for test paths (StubDxmt9Device)
  // that don't construct a real CommandQueue.
  virtual resources::Pool* pool() { return nullptr; }
  virtual pipeline::Cache* pipelineCache() { return nullptr; }

  // Device-level observers and frame-latency governor. Previously on
  // BackendDevice; migrated up so the backend is a pure Renderer.
  // DeviceImpl stores them; the backend invokes the notify* variants.
  virtual void setDeviceLostObserver(core::BackendDevice::DeviceLostObserver) {}
  virtual void setPresentationStatusObserver(core::BackendDevice::PresentationStatusObserver) {}
  virtual void notifyDeviceLost(bool /*lost*/) {}
  virtual void notifyPresentationStatus(bool /*occluded*/) {}
  virtual void setMaxFrameLatency(std::uint32_t /*latency*/) {}
  virtual std::uint32_t maxFrameLatency() const { return 3; }

  // Resource lifecycle — previously on BackendDevice. Promoted so
  // core::Device can talk to dxmt9::Device directly instead of going
  // through the BackendDevice indirection. Default impls return empty
  // handles for test paths (StubDxmt9Device).
  virtual core::BufferHandle createBuffer(const core::BufferDesc&) { return {}; }
  virtual core::TextureHandle createTexture(const core::TextureDesc&) { return {}; }
  virtual core::SurfaceHandle createSurface(const core::SurfaceDesc&) { return {}; }
  virtual core::SurfaceHandle createSurfaceForTexture(core::TextureHandle, std::uint32_t,
                                                       const core::SurfaceDesc&) {
    return {};
  }
  virtual void destroyBuffer(core::BufferHandle) {}
  virtual void destroyTexture(core::TextureHandle) {}
  virtual void destroySurface(core::SurfaceHandle) {}
  virtual void* mapBuffer(core::BufferHandle, std::uint32_t /*flags*/) { return nullptr; }
  virtual void unmapBuffer(core::BufferHandle) {}
  virtual void uploadBufferData(core::BufferHandle, std::span<const std::uint8_t>) {}
  virtual void uploadTextureLevel(core::TextureHandle, std::uint32_t /*level*/,
                                    std::uint32_t /*width*/, std::uint32_t /*height*/,
                                    std::uint32_t /*pitch*/,
                                    std::span<const std::uint8_t> /*bytes*/) {}

  // Encode + submit commands. Previously on BackendDevice. Promoted here
  // so core::Device can dispatch through the upper Device. DeviceImpl
  // forwards to backend_ while the encoder still lives there; Step 3
  // moves the encoder onto a RenderContext sibling and DeviceImpl
  // implements these directly.
  virtual void submitDraw(const core::DrawDesc&) {}
  virtual void submitClear(const core::ClearDesc&) {}
  virtual void submitSurfaceCopy(const core::SurfaceCopyDesc&) {}
  virtual void submitStretchRect(const core::StretchRectDesc&) {}
  virtual void submitReadback(const core::ReadbackDesc&) {}
  virtual void submitColorFill(const core::ColorFillDesc&) {}
  virtual void present(const core::SwapDesc&) {}
  virtual void flush() {}
  virtual core::HResult waitForVBlank(const core::SwapDesc&) { return core::HResult{0}; }
  virtual bool readbackSurface(const core::ReadbackDesc&, core::ReadbackPixels&) { return false; }
};

struct DEVICE_DESC {
  WMT::Device device;  // Metal device chosen by the caller (typically the first
                       // device returned by WMT::CopyAllDevices()).
  core::BackendLimits limits{};
};

std::unique_ptr<Device> CreateDXMT9Device(const DEVICE_DESC& desc);

}  // namespace dxmt9
