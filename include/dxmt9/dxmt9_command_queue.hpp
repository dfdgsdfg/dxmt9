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
#include <mutex>
#include <optional>

namespace dxmt9 {

// Size of the chunk ring. Matches upstream dxmt's kCommandChunkCount.
inline constexpr size_t kCommandChunkCount = 32;

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
};

}  // namespace dxmt9
