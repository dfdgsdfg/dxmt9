#pragma once

#import <Foundation/Foundation.h>

#include "dxmt9_compat.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9_queue.hpp"

#include <string>
#include <condition_variable>
#include <functional>
#include <mutex>
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
  bool inspect(id<MTLCommandBuffer> commandBuffer,
               const metalqueue::CommandBufferDiagnostics& diagnostics,
               const char* context);
  void attachQueueSubmission(id<MTLCommandBuffer> commandBuffer,
                             const metalqueue::CommandBufferDiagnostics& diagnostics,
                             const std::function<void(const metalqueue::CommandBufferDiagnostics&)>& onCompletion,
                             const char* context = "queue");
  template <typename ReadyContainer, typename CompletedContainer, typename SlotContainer>
  void attachTrackedQueueSubmission(
      id<MTLCommandBuffer> commandBuffer,
      const metalqueue::CommandBufferDiagnostics& diagnostics,
      const metalqueue::QueueLifecycleController& queueLifecycle,
      size_t slotIndex,
      dxmt9::core::metalqueue::u64 seqId,
      std::optional<size_t>& writingSlot,
      size_t writeIndex,
      const ReadyContainer& readySlots,
      CompletedContainer& completedSeqQueue,
      size_t& inflightCount,
      dxmt9::core::metalqueue::u64& completedSeqId,
      dxmt9::core::metalqueue::u64& lastCommittedSeqId,
      const SlotContainer& slots,
      std::mutex& mutex,
      std::condition_variable& finishCv,
      const char* context = "queue") {
    queueLifecycle.noteEncodeCommit(slotIndex, seqId, writingSlot, writeIndex, readySlots,
                                    completedSeqQueue, inflightCount, completedSeqId,
                                    lastCommittedSeqId, slots);
    attachQueueSubmission(
        commandBuffer,
        diagnostics,
        [&queueLifecycle, slotIndex, seqId, &writingSlot, writeIndex, &readySlots,
         &completedSeqQueue, &inflightCount, &completedSeqId, &lastCommittedSeqId, &slots,
         &mutex, &finishCv](const metalqueue::CommandBufferDiagnostics&) {
          std::lock_guard completionLock(mutex);
          completedSeqQueue.push_back(seqId);
          queueLifecycle.noteGpuComplete(slotIndex, seqId, writingSlot, writeIndex, readySlots,
                                         completedSeqQueue, inflightCount, completedSeqId,
                                         lastCommittedSeqId, slots);
          finishCv.notify_all();
        },
        context);
  }
  const metalqueue::CompletionTracker& completionTracker() const noexcept { return completionTracker_; }

 private:
  metalqueue::CompletionTracker completionTracker_{};
  DeveloperHudController hudController_{};
};

}  // namespace dxmt9::core::metalhud
