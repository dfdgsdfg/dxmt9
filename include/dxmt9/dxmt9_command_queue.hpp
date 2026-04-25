#pragma once

// Upper-runtime CommandQueue — the execution service and a self-hosting
// runtime node. Owns the WMT::CommandQueue handle, the chunk ring
// state, the three worker threads (encode / finish / completion), the
// per-frame scratch allocators, the queue-owned ResourceInitializer
// for deferred texture uploads, the queueLifecycle_ binding, the
// queue-owned transfer paths (mapBuffer + readbackSurface), and the
// encode-context assembly path that feeds encoders::encodeChunk.
//
// External services (pool, cache, archive, upper Device, limits) are
// snapshotted at construction time as raw pointers; CommandQueue reads
// through them on hot paths without virtual dispatch. DeviceImpl
// guarantees lifetime via member declaration order — queue_ is
// declared last and destructs first.
//
// CommandQueue does NOT persist the shader archive (that's
// dxmt9::shaders::Archive's dtor) and does NOT run under an
// @autoreleasepool itself (the encode chunk scopes its own).

#include "../../src/winemetal/Metal.hpp"
#include "../../src/dxmt9/dxmt9_backend_types.hpp"
#include "../../src/dxmt9/dxmt9_queue.hpp"
#include "../../src/dxmt9/dxmt9_hud.hpp"
#include "../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../src/dxmt9/dxmt9_ring_arena.hpp"
#include "../../src/dxmt9/dxmt9_shader_archive.hpp"

#include <array>
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

// Chunk-ring size + in-flight cap. Match upstream dxmt's kCommandChunkCount.
inline constexpr size_t kCommandChunkCount = 32;
inline constexpr size_t kMaxInflight = 3;

class CommandQueue {
 public:
  // Full execution-service constructor. Allocates the WMT::CommandQueue,
  // constructs the ResourceInitializer, binds queueLifecycle_ to own
  // state, and spawns the three worker threads. Pool / pipeline cache /
  // shader archive / limits are read through the upper Device at
  // construction and snapshotted internally — DeviceImpl guarantees
  // they outlive the queue. A null device or queue-allocation failure
  // leaves the object inert (valid() == false, threadsStarted_ ==
  // false) but still safely destructible.
  CommandQueue(WMT::Device device, Device& upperDevice);

  // Minimal ctor for StubDxmt9Device's null-device test path. Allocates
  // only the WMT::CommandQueue handle when the device is non-null; no
  // threads, no initializer, no lifecycle binding.
  explicit CommandQueue(WMT::Device device);

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
  void submitPresent(const core::SwapDesc& desc);
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
  // Exposed so the services-ctor's own initialization can invoke them.
  // External callers should not use these.
  using EncodeChunkFn =
      std::function<std::optional<core::metalqueue::QueueSubmissionRecord>(
          std::size_t slotIndex, const core::ChunkSlot& slot)>;
  using OnSubmittedFn = std::function<void(std::uint64_t completedSeqId)>;
  void runEncodeLoop(EncodeChunkFn encodeChunk, OnSubmittedFn onSubmitted);
  void runFinishLoop();
  void runCompletionWatcherLoop();
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

  std::array<core::ChunkSlot, kCommandChunkCount> slots_{};
  std::optional<size_t> writingSlot_{};
  size_t writeIndex_ = 0;
  size_t inflightCount_ = 0;
  std::deque<size_t> readySlots_{};
  std::deque<std::uint64_t> completedSeqQueue_{};

  // Last destination handle for a color-write. Drives submitPresent's
  // source selection; read by encoders::beginRenderPass to decide
  // whether a post-present Discard load action is safe.
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
  bool stop_ = true;

 private:
  // Assemble the EncodeContext handed to encoders::encodeChunk. Uses
  // queue-owned (device_, allocators_, *this) + borrowed services
  // (cache_, archive_, upperDevice_, limits_, pool_).
  encoders::EncodeContext makeEncodeContext();

  WMT::Device device_{};
  WMT::Reference<WMT::CommandQueue> queue_{};
  WMT::CommandQueue queueView_{};  // non-owning view of queue_

  std::thread encodeThread_{};
  std::thread finishThread_{};
  std::thread completionThread_{};
  bool threadsStarted_ = false;

  // Snapshotted from upper Device at construction (small, immutable).
  // upperDevice_ is the back-channel for notify callbacks invoked from
  // the encode thread (presentation status, device lost).
  const core::BackendLimits* limits_ = nullptr;
  Device* upperDevice_ = nullptr;

  // Queue-owned runtime node state. Pool / cache / archive live HERE
  // (matches upstream dxmt's CommandQueue), not on DeviceImpl. Order
  // matters: pool_ must be constructed before initializer_ (which
  // borrows it). All queue-owned state outlives the worker threads
  // because threads are joined first (declaration-order: encodeThread_
  // before pool_ — but stop() drains threads before the dtor reaches
  // the pool members).
  scratch::FrameAllocators allocators_{};
  resources::Pool pool_{};
  pipeline::Cache pipelineCache_{};
  shaders::Archive shaderArchive_{};
  std::unique_ptr<resources::Initializer> initializer_;
};

}  // namespace dxmt9
