#pragma once

#import <Foundation/Foundation.h>

#include "dxmt9_compat.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9_queue.hpp"

#include <string>
#include <functional>
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
  metalqueue::CommandBufferDiagnostics prepareForSubmission(
      u64 seqId,
      size_t slotIndex,
      std::span<const MetalCommandRecord> commands,
      const std::function<u32(Handle)>& resolveSurfaceFlags);
  metalqueue::CommandBufferDiagnostics prepareForSubmission(metalqueue::CommandBufferDiagnostics diagnostics);
  void attachCompletionHandler(
      id<MTLCommandBuffer> commandBuffer,
      u64 seqId,
      size_t slotIndex,
      std::span<const MetalCommandRecord> commands,
      const std::function<u32(Handle)>& resolveSurfaceFlags,
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

}  // namespace dxmt9::core::metalhud
