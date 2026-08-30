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
#include <utility>
#include <vector>

namespace dxmt9 {

namespace resources { struct Pool; }
namespace pipeline { class Cache; }

// M6 — device capability snapshot. Computed once at device construction
// from MTL probes (supportsFamily / supportsCounterSampling). Cached on
// the Device so callers don't re-probe per-call. counterSampling* gates
// MTL counter sample buffer integrations; supportsAppleN gates GPU-
// family-specific code paths in the encoder and pipeline cache. All
// fields default to false on the test/no-device path.
struct DeviceCapabilities {
  bool counterSamplingAtStageBoundary = false;
  bool counterSamplingAtDrawBoundary = false;
  bool counterSamplingAtBlitBoundary = false;
  bool counterSamplingAtDispatchBoundary = false;
  bool counterSamplingAtTileDispatchBoundary = false;
  bool supportsApple7 = false;
  bool supportsApple8 = false;
  bool supportsApple9 = false;
};

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

  // M6 — device capability snapshot, computed once at construction.
  // Test/stub paths return a zero-initialized DeviceCapabilities.
  virtual const DeviceCapabilities& capabilities() const {
    static const DeviceCapabilities kEmpty{};
    return kEmpty;
  }

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
  virtual std::uint32_t maxFrameLatency() const { return core::kDefaultFrameLatency; }

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
  virtual bool exportSharedBuffer(core::BufferHandle, SharedBufferBacking&) { return false; }
  virtual core::BufferHandle importSharedBuffer(const core::BufferDesc&,
                                                 const SharedBufferBacking&) { return {}; }
  virtual bool exportSharedTexture(core::TextureHandle, SharedTextureBacking&) { return false; }
  virtual core::TextureHandle importSharedTexture(const core::TextureDesc&,
                                                   const SharedTextureBacking&) { return {}; }
  virtual bool exportSharedSurface(core::SurfaceHandle, SharedSurfaceBacking&) { return false; }
  virtual core::SurfaceHandle importSharedSurface(const core::SurfaceDesc&,
                                                   const SharedSurfaceBacking&) { return {}; }
  virtual void destroyBuffer(core::BufferHandle) {}
  virtual void destroyTexture(core::TextureHandle) {}
  virtual void destroySurface(core::SurfaceHandle) {}
  virtual void* mapBuffer(core::BufferHandle, std::uint32_t /*flags*/) { return nullptr; }
  virtual void unmapBuffer(core::BufferHandle) {}
  virtual void uploadBufferData(core::BufferHandle, std::span<const std::uint8_t>) {}
  virtual void uploadBufferDataRange(core::BufferHandle handle,
                                     std::span<const std::uint8_t> fullBytes,
                                     std::uint64_t offset,
                                     std::uint64_t byteCount) {
    (void)offset;
    (void)byteCount;
    uploadBufferData(handle, fullBytes);
  }
  // R-BACK-44.x — Managed buffer mutation offload. Defaulted to "not
  // available" so every stub/test backend keeps the synchronous upload path:
  // `bufferHasVersionedBacking` returning false makes the shared admission
  // predicate reject, which is the same answer the mode-off rollback gives.
  virtual bool bufferHasVersionedBacking(core::BufferHandle) { return false; }
  virtual resources::ManagedBufferMutationLease
  rotateManagedBufferForMutation(core::BufferHandle) {
    return {};
  }
  virtual resources::ManagedBufferMutationApplyResult
  applyManagedBufferMutation(core::BufferHandle,
                             const resources::ManagedBufferMutationLease&,
                             std::uint64_t /*offset*/,
                             std::span<const std::uint8_t> /*bytes*/) {
    return {};
  }
  virtual void uploadTextureLevel(core::TextureHandle, std::uint32_t /*level*/,
                                    std::uint32_t /*width*/, std::uint32_t /*height*/,
                                    std::uint32_t /*depth*/, std::uint32_t /*pitch*/,
                                    std::uint32_t /*slicePitch*/,
                                    std::span<const std::uint8_t> /*bytes*/) {}
  virtual bool captureCanonicalD24X8Depth(
      core::SurfaceHandle, core::CanonicalD24X8Depth&) { return false; }
  virtual bool seedCanonicalD24X8Depth(
      core::SurfaceHandle, const core::CanonicalD24X8Depth&) { return false; }
  virtual core::HResult generateTextureMipSublevels(core::TextureHandle) {
    return core::D3D_OK;
  }

  // Encode + submit commands. core::Device dispatches compact draw runs
  // through the upper runtime; DeviceImpl forwards to CommandQueue.
  // Bulk resource retention — chunk-importer-supplied handle set.
  // Default no-op for stub backends (pool lifetime tracking is a
  // production-only concern). DeviceImpl forwards to
  // CommandQueue::markChunkResources.
  virtual void markChunkResources(std::span<const core::ChunkHandleEntry> /*entries*/) {}
  virtual core::ChunkBufferBindingCaptureResult
  markChunkResourcesAndCaptureBufferBindings(
      std::span<const core::ChunkHandleEntry> entries,
      std::vector<core::ChunkBufferBindingSnapshot>& snapshots) {
    snapshots.clear();
    markChunkResources(entries);
    return core::ChunkBufferBindingCaptureResult::Unsupported;
  }
  virtual core::ChunkBufferBindingCaptureResult captureChunkBufferBindings(
      std::span<const core::ChunkHandleEntry> entries,
      std::vector<core::ChunkBufferBindingSnapshot>& snapshots) {
    // Compatibility backends may only implement the historical combined
    // hook. Production DeviceImpl overrides this with capture-only behavior.
    return markChunkResourcesAndCaptureBufferBindings(entries, snapshots);
  }
  virtual bool supportsCpuReadyArenaReplay() const noexcept { return false; }
  // The ordinary synchronous replay lane may bypass DrawRunSubmission only
  // when the backend advertises a transactional final-ChunkSlot destination.
  virtual bool supportsDirectChunkSlotReplay() const noexcept { return false; }
  virtual bool dynamicBufferRenameEnabled() const noexcept { return false; }
  // Phase 14: chunk importer toggles per-draw markDrawResources off
  // around the record-iteration block so bulk markChunkResources is the
  // sole retention path. Stub backends ignore — they have no Pool.
  virtual void setSkipDrawResourceMarking(bool /*skip*/) {}
  // Immediate compact draw-run ingress (BaseDrawState + N DrawParam).
  // Borrowed spans are valid for the call only; implementations must copy to
  // their queue/storage before returning.
  virtual void submitDrawRun(core::CanonicalDrawState,
                             const core::DrawUniformPayload&,
                             std::span<const core::DrawParam>,
                             std::span<const core::DrawParamPayloadView>) {}
  // Direct replay borrows Device cache values synchronously and materializes
  // them once in the final Arena/ChunkSlot destination before returning.
  virtual core::DirectReplayDrawDisposition submitDirectReplayDraw(
      const core::DirectReplayDrawInput&) noexcept {
    return core::DirectReplayDrawDisposition::LegacyUnsupported;
  }
  virtual void submitDrawRunBatch(std::span<core::DrawRunSubmission> submissions) {
    if (submissions.empty()) {
      return;
    }
    const auto& frontState = submissions.front().materializedState();
    const core::DrawUniformPayload* previousUniform = nullptr;
    std::uint64_t previousUniformGeneration = 0;
    for (auto& submission : submissions) {
      const std::span<const core::DrawParam> draws(&submission.draw, 1);
      std::span<const core::DrawParamPayloadView> payloads{};
      if (!submission.payload.userVertexData.empty() ||
          !submission.payload.userIndexData.empty() ||
          !submission.payload.bindingOverrideData.empty() ||
          !submission.payload.bindingSnapshotData.empty()) {
        payloads = std::span<const core::DrawParamPayloadView>(&submission.payload, 1);
      }
      if (submission.uniforms.has_value()) {
        previousUniform = &submission.uniformPayload();
        previousUniformGeneration = submission.uniformGeneration;
      } else {
        const bool sameUniformGeneration =
            submission.uniformGeneration == previousUniformGeneration;
        DXMT_ASSERT(previousUniform && sameUniformGeneration);
        (void)sameUniformGeneration;
      }
      submitDrawRun(submission.stateMaterialized
                        ? submission.materializedState()
                        : frontState,
                    *previousUniform, draws, payloads);
    }
  }
  virtual void submitClear(const core::ClearDesc&) {}
  virtual void submitSurfaceCopy(const core::SurfaceCopyDesc&) {}
  virtual void submitStretchRect(const core::StretchRectDesc&) {}
  virtual void submitReadback(const core::ReadbackDesc&) {}
  virtual void submitColorFill(const core::ColorFillDesc&) {}
  virtual void submitDepthResolve(const core::DepthResolveDesc&) {}
  virtual void submitGenerateMipmaps(const core::GenerateMipmapsDesc&) {}
  virtual void present(const core::SwapDesc&) {}
  // Commit-replay offload present-ordinal boundary. `ordinal` counts
  // present-bearing commits 1,2,3... from the caller's own tracking (not a
  // queue seqId); paced through CommandQueue::waitPresentOrdinalBoundary
  // using this device's maxFrameLatency, the committing swapchain's
  // backBufferCount, and whether the present is synchronized, the same way
  // the inline seqId-based boundary's presentBoundaryLatency() resolves the
  // optional back-buffer cap and Immediate low-latency default
  // (R-BACK-2.51, R-BACK-6.10). src/d3d9's
  // offload path calls this once per present-bearing commit
  // (dxmt9c_device_commit_chunk). See core::SwapDesc::pacedByPresentOrdinal
  // for the separate per-present flag that controls whether
  // submitPresent()'s own inline boundary is skipped for the specific
  // present this ordinal wait paces.
  virtual void waitPresentOrdinalBoundary(std::uint64_t, std::uint32_t, bool) {}
  // Sticky release valve for waitPresentOrdinalBoundary waiters. Called by
  // ReplayOffloadWorker's fail-stop path (device_c_replay_offload.cpp) once
  // a deferred commit-replay failure means no further present ordinal can
  // ever be retired on this device, so any thread parked (or about to park)
  // in waitPresentOrdinalBoundary must be released instead of hanging.
  virtual void abortPresentOrdinalWaits() {}
  virtual void flush() {}
  virtual void observePipelineControl(
      queue::PipelineControl, queue::PipelineDisposition) noexcept {}
  virtual wsi::QuiescenceDisposition beginWsiQuiescence() noexcept {
    return wsi::QuiescenceDisposition::QueueStopped;
  }
  virtual wsi::QuiescenceDisposition beginFinalWsiQuiescence() noexcept {
    return wsi::QuiescenceDisposition::QueueStopped;
  }
  virtual void endWsiQuiescence() noexcept {}
  virtual core::HResult waitForVBlank(const core::SwapDesc&) { return core::HResult{0}; }
  virtual bool readbackSurface(const core::ReadbackDesc&, core::ReadbackPixels&) { return false; }

  // True for real GPU devices where render-target/depth contents can be
  // recovered through readbackSurface(); false for recording/null test stubs
  // that still need core's CPU shadow path for assertions.
  virtual bool supportsGpuReadback() const { return false; }
};

struct DEVICE_DESC {
  WMT::Device device;  // Metal device chosen by the caller (typically the first
                       // device returned by WMT::CopyAllDevices()).
  core::BackendLimits limits{};
};

constexpr bool resolveCpuReadyTapeDirectReplayEnabled(
    const char* value) noexcept {
  return value && value[0] != '\0' &&
         !(value[0] == '0' && value[1] == '\0');
}

// Isolated ordinary replay-to-ChunkSlot production selector. Keep this
// provider configuration transform out of the D3D9 replay planner so the
// Metal/backend device layer never depends on a frontend-private header.
constexpr bool resolveDirectChunkSlotReplayEnabled(
    const char* value, bool traceRender) noexcept {
  return !traceRender && resolveCpuReadyTapeDirectReplayEnabled(value);
}

constexpr bool resolveRenderTapePublisherCaptureEnabled(
    const char* renderTapeCapture,
    const char* renderTapeOutputRoot) noexcept {
  return resolveCpuReadyTapeDirectReplayEnabled(renderTapeCapture) &&
         renderTapeOutputRoot && renderTapeOutputRoot[0] != '\0';
}

constexpr bool resolveCpuReadyTapeDirectReplayEnabled(
    const char* value, const char* renderTapeCapture,
    const char* renderTapeOutputRoot) noexcept {
  return resolveCpuReadyTapeDirectReplayEnabled(value) ||
         resolveRenderTapePublisherCaptureEnabled(renderTapeCapture,
                                                  renderTapeOutputRoot);
}

std::unique_ptr<Device> CreateDXMT9Device(const DEVICE_DESC& desc);

}  // namespace dxmt9
