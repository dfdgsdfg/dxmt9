#pragma once

#include "dxmt9_compat.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9_queue.hpp"
#include "../winemetal/Metal.hpp"

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
  WMT::Reference<WMT::DeveloperHUDProperties> hud_{};
  std::vector<WMT::Reference<WMT::String>> labels_{};
};

class DeveloperHudController {
 public:
  metalqueue::CommandBufferDiagnostics prepareForSubmission(metalqueue::CommandBufferDiagnostics diagnostics);
  bool observeCompletion(obj_handle_t commandBuffer,
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
  metalqueue::CommandBufferDiagnostics prepareQueueSubmission(
      metalqueue::CommandBufferDiagnostics diagnostics);
  bool inspect(obj_handle_t commandBuffer,
               const metalqueue::CommandBufferDiagnostics& diagnostics,
               const char* context);
  bool observeQueueSubmission(obj_handle_t commandBuffer,
                              const metalqueue::CommandBufferDiagnostics& diagnostics,
                              const char* context = "queue");
  const metalqueue::CompletionTracker& completionTracker() const noexcept { return completionTracker_; }

 private:
  metalqueue::CompletionTracker completionTracker_{};
  DeveloperHudController hudController_{};
};

}  // namespace dxmt9::core::metalhud
