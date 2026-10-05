#pragma once

// Resource upload service. Owns a WMT::SharedEvent to coordinate
// deferred staging uploads with the render queue: callers enqueue
// texture-level uploads (shared-mode bypasses the event; private-mode
// queues a staging→private blit) and the encode thread flushes the
// batch before each chunk via flushToWait(), which returns the
// (event, value) pair for the render command buffer to wait on.

#include "../winemetal/Metal.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/query_upload_tracker.hpp"
#include "dxmt9_resource_pool.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dxmt9 {

class CommandQueue;
struct SchedulingProgressTestAccess;

namespace resources {

class Initializer {
 public:
  Initializer(CommandQueue& queue, Pool& pool, WMT::Reference<WMT::Device> device);
  Initializer(const Initializer&) = delete;
  Initializer& operator=(const Initializer&) = delete;

  // Upload a texture level. Shared-mode textures get an immediate
  // CPU-direct replaceRegion; private-mode textures get a staging
  // upload queued for the next flushToWait() call.
  void uploadTextureLevel(core::TextureHandle handle,
                           std::uint32_t level,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::uint32_t depth,
                           std::uint32_t pitch,
                           std::uint32_t slicePitch,
                           std::span<const std::uint8_t> bytes);

  // Queue an initial zero fill for backend-owned render target textures.
  // Private-mode textures use the same deferred staging path as app uploads;
  // shared-mode textures are written immediately by Pool::stageTextureUpload.
  void initializeTextureZero(core::TextureHandle handle);

  // Result of flushing any pending deferred uploads.
  //   event: the SharedEvent that will be signaled by our command
  //          buffer when the uploads finish on the GPU. Empty if the
  //          queue was constructed on a null device.
  //   value: the signal value produced by this flush (equal to our
  //          monotonic counter after any new flush, or the last
  //          previously-signaled value if no new work). 0 means no
  //          flush has ever happened — the caller should skip the
  //          wait-for-event encode.
  //   didFlush: true only when this call encoded and committed new
  //             staging work. Render encoders should wait only for new
  //             work; synchronous callers may still wait on value to
  //             join a previously flushed initializer command buffer.
  struct FlushResult {
    WMT::Event event{};
    std::uint64_t value = 0;
    bool didFlush = false;
  };
  FlushResult flushToWait();

  // Caller must hold CommandQueue::mutex_. This is a queue-side boundary
  // predicate for EncodeSession append decisions; it does not flush work.
  bool hasPendingUploadsUnlocked() const noexcept;
  // Query owns a prefix of this separate submission stream as well as the
  // ordinary queue. Callers hold CommandQueue::mutex_; no completion wait.
  std::optional<std::uint64_t> publishQueryPrefixUnlocked();
  struct QueryProgress {
    std::uint64_t published = 0, completed = 0;
    bool failed = false;
  };
  QueryProgress queryProgressUnlocked() noexcept;
  // Capture-finish only; the caller owns the queue mutex and matching sink.
  // Polls retained actual command buffers without publishing or waiting.
  bool refreshTapeRuntimeCompletionsUnlocked() noexcept;

 private:
  FlushResult flushToWaitUnlocked();
  void failTextureUploadUnlocked() noexcept;
  void enqueuePendingUploadUnlocked(Pool::StagingCopy staging);

  friend struct dxmt9::SchedulingProgressTestAccess;

  CommandQueue* queue_;
  Pool* pool_;
  WMT::Reference<WMT::Device> device_;

  // Created with a real device; absent only on the null-device test path.
  WMT::Reference<WMT::SharedEvent> event_;
  std::uint64_t nextEventValue_ = 1;
  std::uint64_t lastSignaledValue_ = 0;
  std::vector<Pool::StagingCopy> pendingUploads_;
  // Async query mode retains a bounded FIFO of upload submissions so
  // shared-event signaling cannot hide an errored command buffer. No callback
  // captures Initializer/Query lifetime and polling never waits for the GPU.
  bool queryTrackingEnabled_ = false;
  query::UploadTracker<WMT::Reference<WMT::CommandBuffer>, 64> queryUploads_;
  struct TapeUpload {
    WMT::Reference<WMT::CommandBuffer> buffer{};
    std::uint64_t sequence = 0u;
  };
  // Capture-only status ownership; it never changes submission, upload
  // tracking policy, shared-event behavior or the app's synchronous waits.
  std::array<TapeUpload, 64> tapeUploads_{};
  std::size_t tapeUploadHead_ = 0u;
  std::size_t tapeUploadCount_ = 0u;
  bool tapeUploadFailed_ = false;
};

}  // namespace resources
}  // namespace dxmt9
