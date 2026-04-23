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
#include <mutex>
#include <optional>
#include <thread>

namespace dxmt9 {

namespace resources { struct Pool; }
namespace scratch { struct FrameAllocators; }

// Size of the chunk ring. Matches upstream dxmt's kCommandChunkCount.
inline constexpr size_t kCommandChunkCount = 32;

// Maximum chunks the writer is allowed to hold while waiting for GPU
// completion. Previously a file-local constant inside backend_metal.mm.
inline constexpr size_t kMaxInflight = 3;

class CommandQueue {
 public:
  // Construct by creating a new WMT::CommandQueue on the given device.
  explicit CommandQueue(WMT::Device device);
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;

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

  // Thread main-loop bodies. encodeLoop still lives on the backend (depends
  // on encodeChunk + encodeDraw which Steps 3c-follow-up / 3d will migrate).
  // finish + completion loops are thin wrappers over queueLifecycle_ and
  // move here cleanly.
  void runFinishLoop(resources::Pool& pool, scratch::FrameAllocators& allocators);
  void runCompletionWatcherLoop();

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

  // Worker threads (C7c). Owned here so CommandQueue's dtor can deterministically
  // join them; backend is responsible for signalling stop via stopThreads()
  // while its own state is still valid.
  std::thread encodeThread_{};
  std::thread finishThread_{};
  std::thread completionThread_{};
  bool threadsStarted_ = false;
};

}  // namespace dxmt9
