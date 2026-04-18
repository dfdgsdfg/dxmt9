#pragma once

#import <Foundation/Foundation.h>

#include "dxmt9_compat.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9_queue.hpp"

#include <string>
#include <deque>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace dxmt9::core::metalhud {

bool compatHudEnabled();

class DeveloperHudState {
 public:
  DeveloperHudState();
  ~DeveloperHudState();

  void update(u32 frame, u64 seqId, u32 flags, const std::string& errorSummary);

 private:
  bool ensureInitialized();
  void addLabel(const char* label, const char* after);
  void updateLine(size_t index, const std::string& value);

  bool initialized_ = false;
  bool available_ = false;
  id hud_ = nil;
  std::vector<NSString*> labels_{};
};

class DeveloperHudController {
 public:
  metalqueue::CommandBufferDiagnostics prepareForSubmission(metalqueue::CommandBufferDiagnostics diagnostics);
  void attachCompletionHandler(
      id<MTLCommandBuffer> commandBuffer,
      const metalqueue::CommandBufferDiagnostics& diagnostics,
      metalqueue::CompletionTracker& completionTracker,
      const std::function<void(const metalqueue::CommandBufferDiagnostics&)>& onCompletion,
      const char* context = "queue");
  bool observeCompletion(id<MTLCommandBuffer> commandBuffer,
                         const metalqueue::CommandBufferDiagnostics& diagnostics,
                         metalqueue::CompletionTracker& completionTracker,
                         const char* context = "queue");
  void completeSubmission(const metalqueue::CommandBufferDiagnostics& diagnostics,
                          const metalqueue::CompletionTracker& completionTracker);

 private:
  DeveloperHudState state_{};
  u32 presentedFrame_ = 0;
  u32 lastCompatFlags_ = 0;
};

class SubmissionDiagnosticsController {
 public:
  struct TrackedQueueSubmissionState {
    size_t slotIndex = 0;
    metalqueue::u64 seqId = 0;
    const metalqueue::QueueLifecycleController* queueLifecycle = nullptr;
    std::optional<size_t>* writingSlot = nullptr;
    size_t* writeIndex = nullptr;
    std::deque<size_t>* readySlots = nullptr;
    std::deque<metalqueue::u64>* completedSeqQueue = nullptr;
    size_t* inflightCount = nullptr;
    metalqueue::u64* completedSeqId = nullptr;
    metalqueue::u64* lastCommittedSeqId = nullptr;
    std::span<const ChunkSlot> slots;
    std::mutex* mutex = nullptr;
    std::condition_variable* finishCv = nullptr;
  };

  bool inspect(id<MTLCommandBuffer> commandBuffer,
               const metalqueue::CommandBufferDiagnostics& diagnostics,
               const char* context);
  void attachQueueSubmission(id<MTLCommandBuffer> commandBuffer,
                             const metalqueue::CommandBufferDiagnostics& diagnostics,
                             const std::function<void(const metalqueue::CommandBufferDiagnostics&)>& onCompletion,
                             const char* context = "queue");
  void attachTrackedQueueSubmission(id<MTLCommandBuffer> commandBuffer,
                                    const metalqueue::CommandBufferDiagnostics& diagnostics,
                                    const TrackedQueueSubmissionState& state,
                                    const char* context = "queue");
  const metalqueue::CompletionTracker& completionTracker() const noexcept { return completionTracker_; }

 private:
  metalqueue::CompletionTracker completionTracker_{};
  DeveloperHudController hudController_{};
};

}  // namespace dxmt9::core::metalhud
