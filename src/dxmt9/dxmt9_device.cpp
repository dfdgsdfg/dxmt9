#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_shader_archive.hpp"
#include "../winemetal/Metal.hpp"

#include <memory>
#include <mutex>
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

class DeviceImpl final : public Device {
 public:
  explicit DeviceImpl(const DEVICE_DESC& desc)
      : wmt_device_(desc.device),
        metalVersion_(selectMetalVersion(wmt_device_)),
        limits_(finalizeLimits(desc.limits, wmt_device_)),
        shaderArchive_(wmt_device_, resolveShaderCachePath()),
        pool_{},
        pipelineCache_{},
        allocators_{},
        queue_(wmt_device_, CommandQueueServices{
                                .limits = limits_,
                                .pool = pool_,
                                .cache = pipelineCache_,
                                .allocators = allocators_,
                                .archive = shaderArchive_,
                                .upperDevice = *this,
                            }) {}

  // queue_ destructs first (last-declared) — joins worker threads and
  // persists the shader archive while pool_/pipelineCache_/allocators_
  // are still live. No explicit body needed.
  ~DeviceImpl() override = default;

  WMT::Device wmtDevice() override { return wmt_device_; }
  WMTMetalVersion metalVersion() const override { return metalVersion_; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  // backend() stays nullptr in production. Tests observe through
  // StubDxmt9Device + MockBackendDevice.
  std::shared_ptr<core::BackendDevice> backend() override { return nullptr; }
  WMT::Reference<WMT::BinaryArchive>* shaderArchive() override { return &shaderArchive_.reference(); }
  const std::string* shaderArchivePath() override { return &shaderArchive_.path(); }

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

  core::BufferHandle createBuffer(const core::BufferDesc& desc) override {
    std::lock_guard lock(queue_.mutex_);
    return pool_.createBuffer(wmt_device_, desc);
  }
  core::TextureHandle createTexture(const core::TextureDesc& desc) override {
    std::lock_guard lock(queue_.mutex_);
    return pool_.createTexture(wmt_device_, limits_, desc);
  }
  core::SurfaceHandle createSurface(const core::SurfaceDesc& desc) override {
    std::lock_guard lock(queue_.mutex_);
    return pool_.createSurface(wmt_device_, limits_, desc);
  }
  core::SurfaceHandle createSurfaceForTexture(core::TextureHandle handle, std::uint32_t level,
                                                const core::SurfaceDesc& desc) override {
    std::lock_guard lock(queue_.mutex_);
    return pool_.createSurfaceForTexture(handle, level, desc);
  }
  void destroyBuffer(core::BufferHandle handle) override {
    std::lock_guard lock(queue_.mutex_);
    pool_.markDestroyAndGc(pool_.buffers, handle.value, queue_.completedSeqId_);
  }
  void destroyTexture(core::TextureHandle handle) override {
    std::lock_guard lock(queue_.mutex_);
    pool_.markDestroyAndGc(pool_.textures, handle.value, queue_.completedSeqId_);
  }
  void destroySurface(core::SurfaceHandle handle) override {
    std::lock_guard lock(queue_.mutex_);
    pool_.markDestroyAndGc(pool_.surfaces, handle.value, queue_.completedSeqId_);
  }
  void unmapBuffer(core::BufferHandle) override {}
  void uploadBufferData(core::BufferHandle handle, std::span<const std::uint8_t> bytes) override {
    std::lock_guard lock(queue_.mutex_);
    pool_.uploadBufferData(handle.value, bytes.data(), bytes.size());
  }
  void* mapBuffer(core::BufferHandle handle, std::uint32_t flags) override {
    return queue_.mapBuffer(handle, flags);
  }
  void uploadTextureLevel(core::TextureHandle handle, std::uint32_t level,
                           std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                           std::span<const std::uint8_t> bytes) override {
    queue_.uploadTextureLevel(handle, level, width, height, pitch, bytes);
  }

  void submitDraw(const core::DrawDesc& desc) override { queue_.submitDraw(pool_, desc); }
  void submitClear(const core::ClearDesc& desc) override { queue_.submitClear(pool_, desc); }
  void submitSurfaceCopy(const core::SurfaceCopyDesc& desc) override {
    queue_.submitSurfaceCopy(pool_, desc);
  }
  void submitStretchRect(const core::StretchRectDesc& desc) override {
    queue_.submitStretchRect(pool_, desc);
  }
  void submitReadback(const core::ReadbackDesc& desc) override {
    queue_.submitReadback(pool_, desc);
  }
  void submitColorFill(const core::ColorFillDesc& desc) override {
    queue_.submitColorFill(pool_, desc);
  }
  void present(const core::SwapDesc& desc) override { queue_.submitPresent(pool_, desc); }
  void flush() override { queue_.submitFlush(pool_); }
  core::HResult waitForVBlank(const core::SwapDesc&) override { return queue_.waitForVBlank(pool_); }
  bool readbackSurface(const core::ReadbackDesc& desc, core::ReadbackPixels& pixels) override {
    return queue_.readbackSurface(desc, pixels);
  }

  bool ready() const noexcept { return queue_.started(); }

 private:
  // Declaration order matters: queue_ is declared LAST so its destructor
  // runs FIRST — joining worker threads and persisting the archive
  // before pool_/pipelineCache_/allocators_ tear down.
  WMT::Reference<WMT::Device> wmt_device_;
  WMTMetalVersion metalVersion_ = WMTMetalVersionMax;
  core::BackendLimits limits_{};
  shaders::Archive shaderArchive_{};
  core::BackendDevice::DeviceLostObserver deviceLostObserver_{};
  core::BackendDevice::PresentationStatusObserver presentationStatusObserver_{};
  std::uint32_t maxFrameLatency_ = 3;
  resources::Pool pool_{};
  pipeline::Cache pipelineCache_{};
  scratch::FrameAllocators allocators_{};
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
