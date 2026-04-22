#include "dxmt9/dxmt9_device.hpp"
#include "backend_metal.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
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
                                             shaderArchive_, shaderArchivePath_, *this,
                                             pool_, pipelineCache_, allocators_);
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

  // Resource lifecycle. Called directly from core::Device (and from
  // MetalBackendDevice's BackendDevice overrides during the transition).
  // The pool lives on *this; commandQueue_.mutex_ is the protecting mutex.
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
  // mapBuffer + uploadTextureLevel stay on the backend: they need the
  // wait-for-seq hook / texture trace sink resident on MetalBackendDevice.
  // Forward through backend_ which inherits from BackendDevice and owns
  // the concrete methods.
  void* mapBuffer(core::BufferHandle handle, std::uint32_t flags) override {
    return backend_ ? backend_->mapBuffer(handle, flags) : nullptr;
  }
  void uploadTextureLevel(core::TextureHandle handle, std::uint32_t level,
                           std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                           std::span<const std::uint8_t> bytes) override {
    if (backend_) backend_->uploadTextureLevel(handle, level, width, height, pitch, bytes);
  }

  // Submit / present / flush — forward to backend while the encoder lives
  // there. Step 3 will move these onto a RenderContext that DeviceImpl owns.
  void submitDraw(const core::DrawDesc& desc) override {
    if (backend_) backend_->submitDraw(desc);
  }
  void submitClear(const core::ClearDesc& desc) override {
    if (backend_) backend_->submitClear(desc);
  }
  void submitSurfaceCopy(const core::SurfaceCopyDesc& desc) override {
    if (backend_) backend_->submitSurfaceCopy(desc);
  }
  void submitStretchRect(const core::StretchRectDesc& desc) override {
    if (backend_) backend_->submitStretchRect(desc);
  }
  void submitReadback(const core::ReadbackDesc& desc) override {
    if (backend_) backend_->submitReadback(desc);
  }
  void submitColorFill(const core::ColorFillDesc& desc) override {
    if (backend_) backend_->submitColorFill(desc);
  }
  void present(const core::SwapDesc& desc) override {
    if (backend_) backend_->present(desc);
  }
  void flush() override {
    if (backend_) backend_->flush();
  }
  core::HResult waitForVBlank(const core::SwapDesc& desc) override {
    return backend_ ? backend_->waitForVBlank(desc) : core::HResult{0};
  }

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
  // Pool / Cache / Allocators are owned here (Step 1 of dxmt-alignment).
  // Declared before backend_ so they outlive it (backend destructs first
  // because it holds pointers into these).
  resources::Pool pool_{};
  pipeline::Cache pipelineCache_{};
  scratch::FrameAllocators allocators_{};
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
