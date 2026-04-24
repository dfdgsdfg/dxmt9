#pragma once

// Upper-runtime CommandQueue — the execution service and a self-hosting
// runtime node. Owns the WMT::CommandQueue handle, the chunk ring state,
// the three worker threads (encode / finish / completion), the per-frame
// scratch allocators, the queue-owned ResourceInitializer for deferred
// texture uploads, the queueLifecycle_ binding, and the encode-context
// assembly path that feeds encoders::encodeChunk.
//
// Dependencies are passed in via a CommandQueueDeps bundle at
// construction: just the external services CommandQueue reads through
// (pool, cache, archive, upper Device, limits). The queue constructs
// the EncodeContext per chunk from its own internal state plus these
// borrowed refs.
//
// CommandQueue does NOT persist the shader archive (that's
// dxmt9::shaders::Archive's dtor), does NOT expose mapBuffer/readback
// (those belong on the Device orchestration surface), and does NOT run
// under an @autoreleasepool itself (the encode chunk wraps its own).

#include "../../src/winemetal/Metal.hpp"
#include "../../src/dxmt9/dxmt9_backend_types.hpp"
#include "../../src/dxmt9/dxmt9_queue.hpp"
#include "../../src/dxmt9/dxmt9_hud.hpp"
#include "../../src/dxmt9/dxmt9_ring_arena.hpp"

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
namespace pipeline { class Cache; }
namespace resources { struct Pool; class Initializer; }
namespace shaders { class Archive; }

// External services CommandQueue reads through. All refs must outlive
// the queue (DeviceImpl guarantees this via member declaration order —
// queue_ is declared last, destructs first).
//
// Everything else the queue needs (allocators, initializer, threads,
// queueLifecycle) is queue-owned.
struct CommandQueueDeps {
  const core::BackendLimits& limits;
  resources::Pool& pool;
  pipeline::Cache& cache;
  shaders::Archive& archive;
  Device& upperDevice;
};

// Chunk-ring size + in-flight cap. Match upstream dxmt's kCommandChunkCount.
inline constexpr size_t kCommandChunkCount = 32;
inline constexpr size_t kMaxInflight = 3;

class CommandQueue {
 public:
  // Full execution-service constructor. Allocates the WMT::CommandQueue,
  // constructs the ResourceInitializer, binds queueLifecycle_ to own
  // state, and spawns the three worker threads. A null device or a
  // queue-allocation failure leaves the object inert (valid() == false,
  // threadsStarted_ == false) but still safely destructible.
  CommandQueue(WMT::Device device, const CommandQueueDeps& deps);

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

  // Submission surface. Each call acquires mutex_ internally. Pool is
  // passed in so GC resource-marking flows through the caller (keeps
  // Pool ownership on DeviceImpl).
  void submitDraw(resources::Pool& pool, const core::DrawDesc& desc);
  void submitClear(resources::Pool& pool, const core::ClearDesc& desc);
  void submitSurfaceCopy(resources::Pool& pool, const core::SurfaceCopyDesc& desc);
  void submitStretchRect(resources::Pool& pool, const core::StretchRectDesc& desc);
  void submitReadback(resources::Pool& pool, const core::ReadbackDesc& desc);
  void submitColorFill(resources::Pool& pool, const core::ColorFillDesc& desc);
  void submitPresent(resources::Pool& pool, const core::SwapDesc& desc);
  void submitFlush(resources::Pool& pool);
  core::HResult waitForVBlank(resources::Pool& pool);

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

  // Borrowed-from-DeviceImpl services, used by submit / compat / encode
  // context assembly. Raw pointers; lifetime enforced by DeviceImpl's
  // member declaration order (queue_ declared last ⇒ destructs first).
  const core::BackendLimits* limits_ = nullptr;
  resources::Pool* pool_ = nullptr;
  pipeline::Cache* cache_ = nullptr;
  shaders::Archive* archive_ = nullptr;
  Device* upperDevice_ = nullptr;

  // Queue-owned runtime node state: per-frame allocators + the
  // ResourceInitializer upload service. Allocators are a direct member
  // so they share the queue's lifetime (thread teardown happens first,
  // then allocators destruct — the encode loop has stopped by then).
  scratch::FrameAllocators allocators_{};
  std::unique_ptr<resources::Initializer> initializer_;
};

}  // namespace dxmt9
