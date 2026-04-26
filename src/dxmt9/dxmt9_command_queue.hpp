#pragma once

// Upper-runtime CommandQueue — the execution service and a self-hosting
// runtime node. Owns the WMT::CommandQueue handle, the chunk ring
// state, the three worker threads (encode / finish / completion), the
// per-frame scratch allocators, the queue-owned ResourceInitializer
// for deferred texture uploads, the queueLifecycle_ binding, the
// queue-owned transfer paths (mapBuffer + readbackSurface), and the
// encode-context assembly path that feeds encoders::encodeChunk.
//
// Pool, pipeline cache, shader archive, limits, frame scratch, and the
// deferred ResourceInitializer are queue-owned. DeviceImpl only passes
// the WMT device plus finalized limits and carries D3D9-facing policy
// through per-submit descriptors.
//
// CommandQueue does NOT persist the shader archive (that's
// dxmt9::shaders::Archive's dtor) and does NOT run under an
// @autoreleasepool itself (the encode chunk scopes its own).

#include "../winemetal/Metal.hpp"
#include "dxmt9_backend_types.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_hud.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_shader_archive.hpp"

#include <array>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace dxmt9 {

class Device;
class CommandQueue;

namespace encoders { struct EncodeContext; }
namespace resources { class Initializer; }

// Chunk-ring size + in-flight cap. Match upstream dxmt's kCommandChunkCount:
// the ring may queue many chunks, while submitPresent() enforces present
// frame-latency through queue-owned present tokens.
inline constexpr size_t kCommandChunkCount = 32;
inline constexpr size_t kMaxQueuedChunks = kCommandChunkCount - 1;

class CommandQueue {
 public:
  // Full execution-service constructor. Allocates the WMT::CommandQueue,
  // constructs the queue-owned pool / pipeline cache / shader archive /
  // ResourceInitializer, binds queueLifecycle_ to own state, and spawns
  // the three worker threads. `limits` is the caller's final BackendLimits
  // (DeviceImpl finalizes desc.limits against device caps before passing
  // it in) and is stored verbatim — no post-construction back-channel.
  // A null device or queue-allocation failure leaves the object inert
  // (valid() == false, threadsStarted_ == false) but still safely
  // destructible.
  //
  // Presentation back-channels (maxFrameLatency + notifyPresentationStatus)
  // ride on each core::SwapDesc — DeviceImpl::present() fills them per
  // submission. The queue holds no Device* pointer.
  //
  // dxmt9-specific divergence from upstream's single-arg ctor: dxmt9's
  // encoders consume BackendLimits in the encode path (depth24/sampleN
  // policy, max texture size, etc.), so the queue snapshots it at
  // construction. Upstream dxmt has no BackendLimits.
  CommandQueue(WMT::Device device, core::BackendLimits limits);

  // Joins worker threads (if started). Archive persistence is not a
  // queue responsibility — it runs from shaders::Archive's dtor.
  ~CommandQueue();
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;

  // True if the full-service ctor succeeded in spawning the worker
  // threads. Drives DeviceImpl::ready().
  bool started() const noexcept { return threadsStarted_; }

  // True if a WMT::CommandQueue handle was allocated. A non-null
  // handle with !started() means only the test ctor ran.
  bool valid() const noexcept { return static_cast<bool>(queue_); }

  // Queue-owned resource initializer API.
  void uploadTextureLevel(core::TextureHandle handle,
                          std::uint32_t level,
                          std::uint32_t width,
                          std::uint32_t height,
                          std::uint32_t pitch,
                          std::span<const std::uint8_t> bytes);

  // Flush any pending deferred uploads. Returned (event, value) is what
  // the render command buffer must wait on; value==0 means nothing to
  // wait for. Invoked at the head of each chunk's command buffer by
  // encoders::encodeChunk.
  struct InitializerFlush {
    WMT::Event event{};
    std::uint64_t value = 0;
  };
  InitializerFlush flushInitializerUploads();

  // Submission / resource-marking surface. Each call acquires mutex_
  // internally; Pool access goes through pool_ (snapshotted at
  // construction).
  void submitDraw(const core::DrawDesc& desc);
  void submitClear(const core::ClearDesc& desc);
  void submitSurfaceCopy(const core::SurfaceCopyDesc& desc);
  void submitStretchRect(const core::StretchRectDesc& desc);
  void submitReadback(const core::ReadbackDesc& desc);
  void submitColorFill(const core::ColorFillDesc& desc);
  std::uint64_t submitPresent(const core::SwapDesc& desc);
  void presentBoundary(std::uint64_t presentSeqId, std::uint32_t maxFrameLatency);
  void submitFlush();
  core::HResult waitForVBlank();

  // Queue-owned transfer paths. mapBuffer orchestrates Pool storage +
  // queue's wait-for-sequence rule under one mutex acquisition;
  // readbackSurface routes through encoders::readbackSurface using the
  // queue's own device + limits + pool refs.
  void* mapBuffer(core::BufferHandle handle, std::uint32_t flags);
  bool readbackSurface(const core::ReadbackDesc& desc, core::ReadbackPixels& pixels);

  // Command-buffer issuance. Callers that need a WMT::CommandBuffer
  // (encoders, transfers, readback) get an owning Reference via
  // newCommandBuffer; raw() exposes the non-owning handle view.
  WMT::Reference<WMT::CommandBuffer> newCommandBuffer();
  WMT::CommandQueue& raw() noexcept { return queueView_; }
  const WMT::CommandQueue& raw() const noexcept { return queueView_; }

  struct TransientBufferSlice {
    WMT::Buffer buffer{};
    std::uint64_t offset = 0;
    std::size_t size = 0;

    explicit operator bool() const noexcept { return static_cast<bool>(buffer); }
  };

  TransientBufferSlice uploadTransientBuffer(std::span<const std::byte> bytes,
                                             std::size_t alignment,
                                             std::uint64_t seqId);

  // The WMT::Device this queue was built on.
  WMT::Device device() const noexcept { return device_; }

  // Access the queue-owned per-frame scratch allocators. Exposed for
  // encoders::encodeChunk which places DrawUniforms on the argument
  // bump ring.
  scratch::FrameAllocators& allocators() noexcept { return allocators_; }

  // Queue-owned resource registries. DeviceImpl forwards its public
  // accessors to these — pool/pipeline-cache/shader-archive live here
  // in upstream-dxmt style.
  resources::Pool& pool() noexcept { return pool_; }
  pipeline::Cache& pipelineCache() noexcept { return pipelineCache_; }
  shaders::Archive& shaderArchive() noexcept { return shaderArchive_; }

  // ─── Mostly-internal: worker-thread bodies + lifecycle binding ─────
  // Exposed so CommandQueue's constructor can wire its own runtime loops.
  // External callers should not use these.
  using EncodeChunkFn =
      std::function<std::optional<core::metalqueue::QueueSubmissionRecord>(
          std::size_t slotIndex, const core::ChunkSlot& slot)>;
  using OnSubmittedFn = std::function<void(std::uint64_t completedSeqId)>;
  void runEncodeLoop(EncodeChunkFn encodeChunk, OnSubmittedFn onSubmitted);
  void runFinishLoop();
  void runCompletionWatcherLoop();
  void notePresentDequeued(std::uint64_t seqId);
  using ResolveSurfaceFlagsFn = std::function<std::uint32_t(core::Handle)>;
  void bindSelfLifecycle(ResolveSurfaceFlagsFn resolveSurfaceFlags);
  void startThreads(std::function<void()> encodeLoop,
                    std::function<void()> finishLoop,
                    std::function<void()> completionLoop);
  void stopThreads();

  // ─── Chunk-ring + sync state (public for QueueLifecycleController) ─
  // These are raw-pointer-bound into queueLifecycle_ via bindSelfLifecycle.
  // Callers that need to read completedSeqId_ (e.g., DeviceImpl's
  // mapBuffer wait rule) treat them as read-only data guarded by mutex_.
  std::uint64_t nextSeqId_ = 1;           // next seq to allocate
  std::uint64_t completedSeqId_ = 0;      // gpu-completed watermark
  std::uint64_t lastCommittedSeqId_ = 0;  // cpu-committed watermark
  std::uint64_t presentDequeuedSeqId_ = 0; // encode worker reached present
  std::uint64_t presentCompletedSeqId_ = 0; // present-bearing command buffer completed

  std::array<core::ChunkSlot, kCommandChunkCount> slots_{};
  std::optional<size_t> writingSlot_{};
  size_t writeIndex_ = 0;
  size_t inflightCount_ = 0;
  std::deque<size_t> readySlots_{};
  std::deque<std::uint64_t> completedSeqQueue_{};
  std::condition_variable presentDequeuedCv_{};

  // Last destination handle for a color-write. submitPresent only uses
  // this as a fallback when SwapDesc::sourceSurface is absent; normal
  // D3D9 presents carry the swapchain backbuffer explicitly. Also read
  // by encoders::beginRenderPass to decide whether a post-present
  // Discard load action is safe.
  core::Handle currentBackBuffer_{};
  bool backBufferDiscardAfterPresent_ = false;

  core::metalqueue::QueueLifecycleController queueLifecycle_{};
  core::metalhud::SubmissionDiagnosticsController submissionDiagnostics_{};

  // Thread-coordination primitives. Worker threads owned by *this
  // (see private section); encoders and DeviceImpl acquire mutex_
  // directly when they need to read chunk-ring state.
  std::mutex mutex_{};
  std::condition_variable writeCv_{};
  std::condition_variable encodeCv_{};
  std::condition_variable finishCv_{};
  std::condition_variable presentCompletedCv_{};
  bool stop_ = true;

 private:
  // Assemble the EncodeContext handed to encoders::encodeChunk. Uses
  // queue-owned state only (device_, limits_, allocators_, pool_,
  // pipelineCache_, shaderArchive_, *this). No upper-Device pointer —
  // presentation back-channels ride on SwapDesc per submission.
  encoders::EncodeContext makeEncodeContext();

  WMT::Device device_{};
  WMT::Reference<WMT::CommandQueue> queue_{};
  WMT::CommandQueue queueView_{};  // non-owning view of queue_

  std::thread encodeThread_{};
  std::thread finishThread_{};
  std::thread completionThread_{};
  bool threadsStarted_ = false;

  // Queue-owned runtime node state. Pool / cache / archive / limits
  // all live HERE (matches upstream dxmt's CommandQueue). Order matters:
  // pool_ must be constructed before initializer_ (which borrows it).
  // All queue-owned state outlives the worker threads because threads
  // are joined first via stopThreads() in ~CommandQueue.
  core::BackendLimits limits_{};
  scratch::FrameAllocators allocators_{};
  resources::Pool pool_{};
  pipeline::Cache pipelineCache_{};
  shaders::Archive shaderArchive_{};
  std::unique_ptr<resources::Initializer> initializer_;

  struct TransientBufferAllocation {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::uint64_t seqId = 0;
  };

  struct RetainedTransientBuffer {
    WMT::Reference<WMT::Buffer> buffer{};
    std::uint64_t seqId = 0;
  };

  void reclaimTransientBuffersUnlocked(std::uint64_t completedSeqId);
  bool ensureTransientBufferUnlocked(std::size_t minimumCapacity);
  bool rotateTransientBufferUnlocked(std::size_t minimumCapacity, std::uint64_t seqId);

  std::mutex transientBufferMutex_{};
  WMT::Reference<WMT::Buffer> transientBuffer_{};
  std::byte* transientBufferContents_ = nullptr;
  std::size_t transientBufferCapacity_ = 0;
  std::size_t transientBufferCursor_ = 0;
  std::deque<TransientBufferAllocation> transientBufferAllocations_{};
  std::deque<RetainedTransientBuffer> retainedTransientBuffers_{};

 public:
  // Limits accessor — value member, not a borrowed pointer.
  const core::BackendLimits& limits() const noexcept { return limits_; }
};

}  // namespace dxmt9
