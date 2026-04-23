#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_shader_sources.hpp"
#include "dxmt9_transfers.hpp"
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

    // Previously MetalBackendDevice bound this and started the threads;
    // DeviceImpl owns the lifecycle directly now (Step 3e).
    queue_.queueLifecycle_.bindTrackedSubmissionState({
        .writingSlot = &queue_.writingSlot_,
        .writeIndex = &queue_.writeIndex_,
        .nextSeqId = &queue_.nextSeqId_,
        .readySlots = &queue_.readySlots_,
        .completedSeqQueue = &queue_.completedSeqQueue_,
        .inflightCount = &queue_.inflightCount_,
        .completedSeqId = &queue_.completedSeqId_,
        .lastCommittedSeqId = &queue_.lastCommittedSeqId_,
        .slots = std::span<core::ChunkSlot>(queue_.slots_.data(), queue_.slots_.size()),
        .mutex = &queue_.mutex_,
        .writeCv = &queue_.writeCv_,
        .encodeCv = &queue_.encodeCv_,
        .finishCv = &queue_.finishCv_,
        .stop = &queue_.stop_,
        .submissionDiagnostics = &queue_.submissionDiagnostics_,
        .resolveSurfaceFlags = [this](core::Handle handle) {
          return compatFlagsForSurface(handle);
        },
    });

    queue_.startThreads(
        [this] { encodeLoop(); },
        [this] { queue_.runFinishLoop(pool_, allocators_); },
        [this] { queue_.runCompletionWatcherLoop(); });
    ready_ = true;
  }

  ~DeviceImpl() override {
    // Stop threads before tearing down state that they reference (pool,
    // allocators, shader archive). CommandQueue::stopThreads joins.
    queue_.stopThreads();
    if (shaderArchive_) {
      std::lock_guard lock(queue_.mutex_);
      shaders::persistShaderArchive(shaderArchive_, shaderArchivePath_);
    }
    pool_.purgeAll();
  }

  WMT::Device wmtDevice() override { return wmt_device_; }
  WMTMetalVersion metalVersion() const override { return metalVersion_; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  // backend() no longer surfaces a concrete MetalBackendDevice — tests
  // that want polymorphic BackendDevice access go through StubDxmt9Device.
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

  // Resource lifecycle. Pool lives on *this; queue_.mutex_ is the
  // protecting mutex.
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
  // Encode-thread main loop. Pulls chunks off the queue and encodes each via
  // dxmt9::encoders::encodeChunk(ctx, ...). @autoreleasepool scopes the ObjC
  // objects allocated per chunk; that's why this file is .mm.
  void encodeLoop() {
    @autoreleasepool {
      while (true) {
        std::unique_lock lock(queue_.mutex_);
        if (!queue_.queueLifecycle_.runEncodeIteration(
                lock,
                [this](std::size_t slotIndex, const core::ChunkSlot& slot) {
                  encoders::EncodeContext ctx{
                      wmt_device_, limits_, pool_, pipelineCache_, allocators_,
                      &shaderArchive_, &shaderArchivePath_, queue_, this,
                  };
                  return encoders::encodeChunk(ctx, slotIndex, slot);
                },
                [this](std::uint64_t) {
                  allocators_.reclaim(queue_.completedSeqId_);
                })) {
          return;
        }
      }
    }
  }

  // Surface-format compat bit query for the queueLifecycle binding's
  // resolveSurfaceFlags hook. Previously MetalBackendDevice::compatFlagsForSurfaceUnlocked.
  std::uint32_t compatFlagsForSurface(core::Handle handle) const {
    if (!handle) {
      return 0;
    }
    const auto* surface = pool_.findSurface(handle.value);
    if (!surface) {
      return 0;
    }
    return core::metalcompat::isFloatRenderTargetFormat(surface->desc.format)
               ? static_cast<std::uint32_t>(core::metalcompat::CompatFlagBits::CompatFlagFp16)
               : 0u;
  }

  WMT::Reference<WMT::Device> wmt_device_;
  WMTMetalVersion metalVersion_ = WMTMetalVersionMax;
  CommandQueue queue_;
  core::BackendLimits limits_{};
  std::string shaderArchivePath_{};
  WMT::Reference<WMT::BinaryArchive> shaderArchive_{};
  core::BackendDevice::DeviceLostObserver deviceLostObserver_{};
  core::BackendDevice::PresentationStatusObserver presentationStatusObserver_{};
  std::uint32_t maxFrameLatency_ = 3;
  // Pool / Cache / Allocators are owned here. Declared after queue_ so
  // queue_.stopThreads() in the dtor runs before they destruct (the threads
  // reach into these structs while running).
  resources::Pool pool_{};
  pipeline::Cache pipelineCache_{};
  scratch::FrameAllocators allocators_{};
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
