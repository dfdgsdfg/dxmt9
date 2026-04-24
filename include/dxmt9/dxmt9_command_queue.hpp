#pragma once

// Upper-runtime CommandQueue — owns the WMT::CommandQueue handle + the chunk
// ring state. Mirrors dxmt's class CommandQueue (dxmt/src/dxmt/dxmt_command_queue.hpp).
// The worker threads (encode/finish/completion) still live on
// MetalBackendDevice for now; they access queue state through this object.

#include "../../src/winemetal/Metal.hpp"
#include "../../src/dxmt9/dxmt9_backend_types.hpp"
#include "../../src/dxmt9/dxmt9_queue.hpp"
#include "../../src/dxmt9/dxmt9_hud.hpp"

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
namespace resources { struct Pool; class Initializer; }
namespace scratch { struct FrameAllocators; }
namespace pipeline { class Cache; }

// Size of the chunk ring. Matches upstream dxmt's kCommandChunkCount.
inline constexpr size_t kCommandChunkCount = 32;

// Maximum chunks the writer is allowed to hold while waiting for GPU
// completion. Previously a file-local constant inside backend_metal.mm.
inline constexpr size_t kMaxInflight = 3;

class CommandQueue {
 public:
  // All-in-one constructor: allocate the WMT::CommandQueue, bind
  // queueLifecycle_ to own state, construct the ResourceInitializer,
  // and spawn the three worker threads. If `device` is null or queue
  // allocation fails, leaves the object in a ready-to-destruct state
  // with valid() == false and threadsStarted_ == false.
  CommandQueue(WMT::Device device,
               const core::BackendLimits& limits,
               resources::Pool& pool,
               pipeline::Cache& cache,
               scratch::FrameAllocators& allocators,
               WMT::Reference<WMT::BinaryArchive>& shaderArchive,
               const std::string& shaderArchivePath,
               Device& upperDevice);

  // Minimal test-only constructor. Allocates just the WMT::CommandQueue
  // handle (if device is non-null) and leaves all other state empty —
  // no threads, no initializer, no lifecycle binding. Used by
  // StubDxmt9Device on the null-device test path.
  explicit CommandQueue(WMT::Device device);

  ~CommandQueue();
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;

  bool started() const noexcept { return threadsStarted_; }

  // Upload a texture level via the queue-owned ResourceInitializer.
  // Thin forwarder; Pool + device refs come from start().
  void uploadTextureLevel(core::TextureHandle handle,
                          std::uint32_t level,
                          std::uint32_t width,
                          std::uint32_t height,
                          std::uint32_t pitch,
                          std::span<const std::uint8_t> bytes);

  // True if a WMT::CommandQueue was successfully allocated.
  bool valid() const noexcept { return static_cast<bool>(queue_); }

  // Access the underlying WMT::CommandQueue. Callers that need to issue
  // command buffers use newCommandBuffer() below.
  WMT::CommandQueue& raw() noexcept { return queueView_; }
  const WMT::CommandQueue& raw() const noexcept { return queueView_; }

  // Issue a new command buffer from the queue. Returns an owning reference
  // (released via RAII when the caller's Reference goes out of scope or is
  // moved into a CommandChunk).
  WMT::Reference<WMT::CommandBuffer> newCommandBuffer();

  // The WMT::Device this queue was created on.
  WMT::Device device() const noexcept { return device_; }

  // Thread management — owned by CommandQueue (C7c). The backend supplies
  // the loop bodies at startup and MUST call stopThreads() before tearing
  // down its state so the threads (which call back into backend methods)
  // are joined while that state is still valid.
  void startThreads(std::function<void()> encodeLoop,
                     std::function<void()> finishLoop,
                     std::function<void()> completionLoop);
  void stopThreads();

  // Chunk-ring submission. Previously on MetalBackendDevice. Pool is
  // passed in so GC resource marking can flow through the dxmt9::Device
  // owner. Acquires mutex_ internally.
  void submitDraw(resources::Pool& pool, const core::DrawDesc& desc);
  void submitClear(resources::Pool& pool, const core::ClearDesc& desc);
  void submitSurfaceCopy(resources::Pool& pool, const core::SurfaceCopyDesc& desc);
  void submitStretchRect(resources::Pool& pool, const core::StretchRectDesc& desc);
  void submitReadback(resources::Pool& pool, const core::ReadbackDesc& desc);
  void submitColorFill(resources::Pool& pool, const core::ColorFillDesc& desc);
  void submitPresent(resources::Pool& pool, const core::SwapDesc& desc);
  void submitFlush(resources::Pool& pool);
  core::HResult waitForVBlank(resources::Pool& pool);

  // All three worker-thread bodies now live here. runEncodeLoop is pure
  // C++ — the @autoreleasepool scoping is the caller's responsibility
  // (wrap it inside the encodeChunk callback).
  using EncodeChunkFn =
      std::function<std::optional<core::metalqueue::QueueSubmissionRecord>(
          std::size_t slotIndex, const core::ChunkSlot& slot)>;
  using OnSubmittedFn = std::function<void(std::uint64_t completedSeqId)>;
  void runEncodeLoop(EncodeChunkFn encodeChunk, OnSubmittedFn onSubmitted);
  void runFinishLoop(resources::Pool& pool, scratch::FrameAllocators& allocators);
  void runCompletionWatcherLoop();

  // Wire queueLifecycle_ to own state + caller-supplied surface-flags
  // hook. Called once by DeviceImpl during construction.
  using ResolveSurfaceFlagsFn = std::function<std::uint32_t(core::Handle)>;
  void bindSelfLifecycle(ResolveSurfaceFlagsFn resolveSurfaceFlags);

  // Sequence counters + chunk-ring state. Guarded externally by the owning
  // backend's mutex (the binding into QueueLifecycleController takes raw
  // pointers to these). Ownership is on CommandQueue; the mutex migration
  // happens in a later phase along with the worker threads.
  std::uint64_t nextSeqId_ = 1;           // next seq to allocate
  std::uint64_t completedSeqId_ = 0;      // gpu-completed watermark
  std::uint64_t lastCommittedSeqId_ = 0;  // cpu-committed watermark

  std::array<core::ChunkSlot, kCommandChunkCount> slots_{};
  std::optional<size_t> writingSlot_{};
  size_t writeIndex_ = 0;
  size_t inflightCount_ = 0;
  std::deque<size_t> readySlots_{};
  std::deque<std::uint64_t> completedSeqQueue_{};

  // Last destination handle for a color-write (draw/clear/colorFill). Drives
  // the presentation source in submitPresent. Previously backend-resident.
  core::Handle currentBackBuffer_{};

  // Set by encodePresent, consumed by the next draw to the same RT so the
  // encoder can choose DontCare over Load. Tied to presentation lifecycle.
  bool backBufferDiscardAfterPresent_ = false;

  core::metalqueue::QueueLifecycleController queueLifecycle_{};
  core::metalhud::SubmissionDiagnosticsController submissionDiagnostics_{};

  // Synchronization. The backend's worker threads wait on these; backend
  // still owns the std::thread objects so it can join them in its dtor
  // before CommandQueue tears down (destruction order safety).
  std::mutex mutex_{};
  std::condition_variable writeCv_{};
  std::condition_variable encodeCv_{};
  std::condition_variable finishCv_{};
  bool stop_ = true;

 private:
  WMT::Device device_{};
  WMT::Reference<WMT::CommandQueue> queue_{};
  // Non-owning view exposed via raw() — kept in sync with queue_.
  WMT::CommandQueue queueView_{};

  // Worker threads. Owned here; joined in stop() + dtor. Destruction
  // order inside CommandQueue::~CommandQueue guarantees threads are
  // joined before the initializer pointer (which threads reach through)
  // is destroyed.
  std::thread encodeThread_{};
  std::thread finishThread_{};
  std::thread completionThread_{};
  bool threadsStarted_ = false;

  // Dependencies borrowed via start(). Non-owning pointers; DeviceImpl
  // guarantees lifetime through the stop-then-destruct pattern.
  const core::BackendLimits* limits_ = nullptr;
  resources::Pool* pool_ = nullptr;
  pipeline::Cache* cache_ = nullptr;
  scratch::FrameAllocators* allocators_ = nullptr;
  WMT::Reference<WMT::BinaryArchive>* shaderArchive_ = nullptr;
  const std::string* shaderArchivePath_ = nullptr;
  Device* upperDevice_ = nullptr;

  // ResourceInitializer owned by the queue (upload service). Constructed
  // in start(); destroyed in ~CommandQueue after threads have joined.
  std::unique_ptr<resources::Initializer> initializer_;
};

}  // namespace dxmt9
