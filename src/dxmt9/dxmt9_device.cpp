#include "dxmt9_device.hpp"
#include "../winemetal/Metal.hpp"
#include "dxmt9_archive_prewarm.hpp"
#include "dxmt9_blit_encoders.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_render_scheduling.hpp"
#include "util/log/log.hpp"

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

bool shouldInitializeTextureWithZero(const core::TextureDesc& desc) {
  return desc.pool == core::Pool::Default &&
         (desc.usage & core::UsageRenderTarget) != 0u &&
         (desc.usage & core::UsageDepthStencil) == 0u;
}

bool cpuReadyTapeDirectReplayEnabled() noexcept {
  static const bool enabled = [] {
    return resolveCpuReadyTapeDirectReplayEnabled(
        std::getenv("DXMT9_CPU_READY_TAPE"),
        std::getenv("DXMT9_RENDER_TAPE_CAPTURE"),
        std::getenv("DXMT9_RENDER_TAPE_OUTPUT_ROOT"));
  }();
  return enabled;
}

bool renderTapePublisherCaptureEnabled() noexcept {
  static const bool enabled = resolveRenderTapePublisherCaptureEnabled(
      std::getenv("DXMT9_RENDER_TAPE_CAPTURE"),
      std::getenv("DXMT9_RENDER_TAPE_OUTPUT_ROOT"));
  return enabled;
}

render::RenderPartitionConfig renderPartitionConfig() noexcept {
  const char* value = std::getenv("DXMT9_RENDER_PARTITION_MODE");
  return render::resolveRenderPartitionConfig(value);
}

// M6 — sampled once at device init and logged so triage on a bug report
// can immediately see which Metal counter / family features the running
// device exposes. Cheap: ~10 selector dispatches at process start.
DeviceCapabilities probeCapabilities(WMT::Device device) {
  DeviceCapabilities caps{};
  if (!device) {
    return caps;
  }
  caps.supportsApple7 = device.supportsFamily(WMTGPUFamilyApple7);
  caps.supportsApple8 = device.supportsFamily(WMTGPUFamilyApple8);
  caps.supportsApple9 = device.supportsFamily(WMTGPUFamilyApple9);
  caps.counterSamplingAtStageBoundary =
      device.supportsCounterSampling(WMTCounterSamplingPointAtStageBoundary);
  caps.counterSamplingAtDrawBoundary =
      device.supportsCounterSampling(WMTCounterSamplingPointAtDrawBoundary);
  caps.counterSamplingAtBlitBoundary =
      device.supportsCounterSampling(WMTCounterSamplingPointAtBlitBoundary);
  caps.counterSamplingAtDispatchBoundary =
      device.supportsCounterSampling(WMTCounterSamplingPointAtDispatchBoundary);
  caps.counterSamplingAtTileDispatchBoundary =
      device.supportsCounterSampling(WMTCounterSamplingPointAtTileDispatchBoundary);
  return caps;
}

void logCapabilities(const DeviceCapabilities& caps) {
  dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-device",
                    "capabilities: family={apple7=%d apple8=%d apple9=%d} "
                    "counter_sampling={stage=%d draw=%d blit=%d "
                    "dispatch=%d tile=%d}",
                    caps.supportsApple7 ? 1 : 0,
                    caps.supportsApple8 ? 1 : 0,
                    caps.supportsApple9 ? 1 : 0,
                    caps.counterSamplingAtStageBoundary ? 1 : 0,
                    caps.counterSamplingAtDrawBoundary ? 1 : 0,
                    caps.counterSamplingAtBlitBoundary ? 1 : 0,
                    caps.counterSamplingAtDispatchBoundary ? 1 : 0,
                    caps.counterSamplingAtTileDispatchBoundary ? 1 : 0);
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
        capabilities_(probeCapabilities(wmt_device_)),
        renderTapePublisherCaptureEnabled_(
            renderTapePublisherCaptureEnabled()),
        cpuReadyTapeDirectReplayEnabled_(cpuReadyTapeDirectReplayEnabled()),
        renderPartitionConfig_(renderPartitionConfig()),
        queue_(wmt_device_, limits_, cpuReadyTapeDirectReplayEnabled_,
               renderTapePublisherCaptureEnabled_, renderPartitionConfig_) {
    const auto level = renderPartitionConfig_.fallback ==
            render::PartitionModeFallback::None
        ? util::LogLevel::Info
        : util::LogLevel::Warn;
    util::logf(level, "dxmt9-device",
               "render partition requested=%s resolved=%s fallback=%u",
               render::partitionModeRequestName(
                   renderPartitionConfig_.requested),
               render::partitionModeName(renderPartitionConfig_.resolved),
               static_cast<unsigned>(renderPartitionConfig_.fallback));
    if (perf::enabled()) {
      perf::countRenderPartitionProvider(
          renderPartitionConfig_.requested, renderPartitionConfig_.resolved);
    }
  }

  // queue_ destructs first (last-declared) — joins worker threads,
  // then queue-owned pool/cache/archive destruct in member-reverse
  // order (archive persists to disk last).
  ~DeviceImpl() override = default;

  WMT::Device wmtDevice() override { return wmt_device_; }
  WMTMetalVersion metalVersion() const override { return metalVersion_; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  const DeviceCapabilities& capabilities() const override { return capabilities_; }
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
    core::TextureHandle handle{};
    {
      std::lock_guard lock(queue_.mutex_);
      handle = queue_.pool().createTexture(wmt_device_, limits_, desc);
    }
    if (handle && shouldInitializeTextureWithZero(desc)) {
      queue_.initializeTextureZero(handle);
    }
    return handle;
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
  bool exportSharedBuffer(core::BufferHandle handle, SharedBufferBacking& out) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().exportSharedBuffer(handle, out);
  }
  core::BufferHandle importSharedBuffer(const core::BufferDesc& desc,
                                        const SharedBufferBacking& backing) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().importSharedBuffer(desc, backing);
  }
  bool exportSharedTexture(core::TextureHandle handle, SharedTextureBacking& out) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().exportSharedTexture(handle, out);
  }
  core::TextureHandle importSharedTexture(const core::TextureDesc& desc,
                                          const SharedTextureBacking& backing) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().importSharedTexture(desc, backing);
  }
  bool exportSharedSurface(core::SurfaceHandle handle, SharedSurfaceBacking& out) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().exportSharedSurface(handle, out);
  }
  core::SurfaceHandle importSharedSurface(const core::SurfaceDesc& desc,
                                          const SharedSurfaceBacking& backing) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().importSharedSurface(desc, backing);
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
    queue_.pool().uploadBufferData(wmt_device_, handle.value, bytes.data(),
                                   bytes.size(), queue_.completedSeqId_);
  }
  void* mapBuffer(core::BufferHandle handle, std::uint32_t flags) override {
    return queue_.mapBuffer(handle, flags);
  }
  void uploadTextureLevel(core::TextureHandle handle, std::uint32_t level,
                           std::uint32_t width, std::uint32_t height,
                           std::uint32_t depth, std::uint32_t pitch,
                           std::uint32_t slicePitch,
                           std::span<const std::uint8_t> bytes) override {
    queue_.uploadTextureLevel(handle, level, width, height, depth, pitch,
                              slicePitch, bytes);
  }
  core::HResult generateTextureMipSublevels(core::TextureHandle handle) override {
    return queue_.generateTextureMipSublevels(handle);
  }

  void submitDrawRun(core::CanonicalDrawState state,
                     const core::DrawUniformPayload& uniforms,
                     std::span<const core::DrawParam> draws,
                     std::span<const core::DrawParamPayloadView> payloads) override {
    queue_.submitDrawRun(std::move(state), uniforms, draws, payloads);
  }
  void submitDrawRunBatch(std::span<core::DrawRunSubmission> submissions) override {
    queue_.submitDrawRunBatch(submissions);
  }
  void markChunkResources(std::span<const core::ChunkHandleEntry> entries) override {
    queue_.markChunkResources(entries);
  }
  core::ChunkBufferBindingCaptureResult
  markChunkResourcesAndCaptureBufferBindings(
      std::span<const core::ChunkHandleEntry> entries,
      std::vector<core::ChunkBufferBindingSnapshot>& snapshots) override {
    return queue_.markChunkResourcesAndCaptureBufferBindings(entries,
                                                             snapshots);
  }
  core::ChunkBufferBindingCaptureResult captureChunkBufferBindings(
      std::span<const core::ChunkHandleEntry> entries,
      std::vector<core::ChunkBufferBindingSnapshot>& snapshots) override {
    return queue_.captureChunkBufferBindings(entries, snapshots);
  }
  bool supportsCpuReadyArenaReplay() const noexcept override {
    return cpuReadyTapeDirectReplayEnabled_;
  }
  bool dynamicBufferRenameEnabled() const noexcept override {
    return resources::dynamicBufferRenameEnabled();
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
  void submitDepthResolve(const core::DepthResolveDesc& desc) override {
    queue_.submitDepthResolve(desc);
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
  void waitPresentOrdinalBoundary(std::uint64_t ordinal,
                                  std::uint32_t backBufferCount,
                                  bool displaySyncEnabled) override {
    queue_.waitPresentOrdinalBoundary(
        ordinal, maxFrameLatency_, backBufferCount, displaySyncEnabled);
  }
  void abortPresentOrdinalWaits() override { queue_.abortPresentOrdinalWaits(); }
  void flush() override { queue_.submitFlush(); }
  core::HResult waitForVBlank(const core::SwapDesc&) override { return queue_.waitForVBlank(); }
  bool readbackSurface(const core::ReadbackDesc& desc, core::ReadbackPixels& pixels) override {
    return queue_.readbackSurface(desc, pixels);
  }
  bool captureCanonicalD24X8Depth(
      core::SurfaceHandle source,
      core::CanonicalD24X8Depth& depth) override {
    return encoders::captureCanonicalD24X8Depth(
        queue_, queue_.pool(), wmt_device_, limits_, source, depth);
  }
  bool seedCanonicalD24X8Depth(
      core::SurfaceHandle destination,
      const core::CanonicalD24X8Depth& depth) override {
    return encoders::seedCanonicalD24X8Depth(
        queue_, queue_.pool(), wmt_device_, limits_, destination, depth);
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
  DeviceCapabilities capabilities_{};
  core::BackendDevice::DeviceLostObserver deviceLostObserver_{};
  core::BackendDevice::PresentationStatusObserver presentationStatusObserver_{};
  std::uint32_t maxFrameLatency_ = core::kDefaultFrameLatency;
  bool renderTapePublisherCaptureEnabled_ = false;
  bool cpuReadyTapeDirectReplayEnabled_ = false;
  render::RenderPartitionConfig renderPartitionConfig_{};
  CommandQueue queue_;
};

}  // namespace

std::unique_ptr<Device> CreateDXMT9Device(const DEVICE_DESC& desc) {
  auto device = std::make_unique<DeviceImpl>(desc);
  if (!device->ready()) {
    return nullptr;
  }
  // M6: log the already-probed capability snapshot once at init so bug
  // reports include device family + counter-sampling support without
  // requiring the reporter to enable trace logging.
  logCapabilities(device->capabilities());

  // R-BACK-3.8 — surface the resolved archive path and prewarm mode in
  // present diagnostics so support reports can confirm prewarm hit the
  // expected file. The path comes from the queue's already-constructed
  // shader archive (it was resolved through archive_prewarm::resolveArchivePath
  // during CommandQueue's ctor). Empty path means archive disabled at
  // either the env-flag layer or the cache-root layer.
  if (auto* path = device->shaderArchivePath()) {
    const auto mode = archive_prewarm::resolveMode();
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-archive",
                      "prewarm mode=%s archive=\"%s\"",
                      archive_prewarm::modeName(mode),
                      path->c_str());
  }
  return device;
}

}  // namespace dxmt9
