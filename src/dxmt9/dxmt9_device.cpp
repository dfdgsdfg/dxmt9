#include "dxmt9/dxmt9_device.hpp"
#include "backend_metal.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_shader_sources.hpp"
#include "dxmt9_transfers.hpp"
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
    if (!wmt_device_ || !queue_.valid()) {
      return;
    }
    limits_.supportsDepth24Stencil8 = wmt_device_.supportsDepth24Stencil8();

    // Thin Metal-specific glue. Provides the per-chunk encode callback
    // and the surface-compat hook.
    metalBackend_ = std::make_unique<MetalBackendDevice>(
        wmt_device_, limits_, queue_, pool_, pipelineCache_, allocators_,
        shaderArchive_, shaderArchivePath_, *this);

    // Bind queueLifecycle_ to CommandQueue's own state + the compat hook.
    queue_.bindSelfLifecycle(
        [backend = metalBackend_.get()](core::Handle h) {
          return backend->compatFlagsForSurface(h);
        });

    // Spawn the 3 worker threads. Encode loop supplies the chunk
    // callback (with @autoreleasepool inside); finish/completion are
    // pure C++ CommandQueue methods.
    queue_.startThreads(
        [this] {
          queue_.runEncodeLoop(
              [backend = metalBackend_.get()](std::size_t slotIndex,
                                               const core::ChunkSlot& slot) {
                return backend->encodeChunk(slotIndex, slot);
              },
              [this](std::uint64_t) {
                allocators_.reclaim(queue_.completedSeqId_);
              });
        },
        [this] { queue_.runFinishLoop(pool_, allocators_); },
        [this] { queue_.runCompletionWatcherLoop(); });
    ready_ = true;
  }

  ~DeviceImpl() override {
    // Join threads first — they reach into pool/cache/allocators/backend.
    queue_.stopThreads();
    // Persist the compiled-shader archive now that no encode is in flight.
    if (shaderArchive_) {
      std::lock_guard lock(queue_.mutex_);
      shaders::persistShaderArchive(shaderArchive_, shaderArchivePath_);
    }
    metalBackend_.reset();
    pool_.purgeAll();
  }

  WMT::Device wmtDevice() override { return wmt_device_; }
  WMTMetalVersion metalVersion() const override { return metalVersion_; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  // backend() stays nullptr in production. Tests observe through
  // StubDxmt9Device + MockBackendDevice.
  std::shared_ptr<core::BackendDevice> backend() override { return nullptr; }
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
    return transfers::mapBuffer(queue_, pool_, handle, flags);
  }
  void uploadTextureLevel(core::TextureHandle handle, std::uint32_t level,
                           std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                           std::span<const std::uint8_t> bytes) override {
    transfers::uploadTextureLevel(queue_, pool_, wmt_device_, handle, level,
                                    width, height, pitch, bytes);
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
    return transfers::readbackSurface(queue_, pool_, wmt_device_, limits_, desc, pixels);
  }

  bool ready() const noexcept { return ready_; }

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
  resources::Pool pool_{};
  pipeline::Cache pipelineCache_{};
  scratch::FrameAllocators allocators_{};
  // metalBackend_ declared after the data it borrows — destructs first
  // in reverse member order. The dtor explicitly calls queue_.stopThreads()
  // before backend/pool teardown to avoid a race with worker threads.
  std::unique_ptr<MetalBackendDevice> metalBackend_;
  bool ready_ = false;
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
