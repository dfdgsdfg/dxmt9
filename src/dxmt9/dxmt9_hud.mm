#import <objc/message.h>
#import <objc/runtime.h>

#include "dxmt9_hud.hpp"

#include "util/config/config.hpp"

#include <sstream>

namespace dxmt9::core::metalhud {

namespace {

metalqueue::QueueControllerState makeQueueControllerState(
    const SubmissionDiagnosticsController::TrackedQueueSubmissionState& state) {
  return metalqueue::QueueControllerState{
      .writingSlot = state.writingSlot ? *state.writingSlot : std::optional<size_t>{},
      .writeIndex = state.writeIndex ? *state.writeIndex : 0,
      .readyCount = state.readySlots ? state.readySlots->size() : 0,
      .completedQueueCount = state.completedSeqQueue ? state.completedSeqQueue->size() : 0,
      .inflightCount = state.inflightCount ? *state.inflightCount : 0,
      .completedSeqId = state.completedSeqId ? *state.completedSeqId : 0,
      .lastCommittedSeqId = state.lastCommittedSeqId ? *state.lastCommittedSeqId : 0,
      .slots = state.slots,
  };
}

}  // namespace

bool compatHudEnabled() {
  static const bool enabled = dxmt9::util::getenvFlag("DXMT_COMPAT_HUD");
  return enabled;
}

DeveloperHudState::DeveloperHudState() = default;

DeveloperHudState::~DeveloperHudState() {
  @autoreleasepool {
    if (hud_) {
      for (NSString* label : labels_) {
        [label release];
      }
      labels_.clear();
      [hud_ release];
      hud_ = nil;
    }
  }
}

void DeveloperHudState::update(u32 frame, u64 seqId, u32 flags, const std::string& errorSummary) {
  if (!ensureInitialized()) {
    return;
  }

  std::ostringstream heading;
  heading << "dxmt9 frame=" << frame << " seq=" << seqId;
  updateLine(0, heading.str());
  updateLine(1, "compat " + metalcompat::formatCompatFlags(flags));
  updateLine(2, errorSummary.empty() ? std::string("last-error -") : std::string("last-error ") + errorSummary);
}

bool DeveloperHudState::ensureInitialized() {
  if (initialized_) {
    return available_;
  }
  initialized_ = true;
  if (!compatHudEnabled()) {
    return false;
  }

  @autoreleasepool {
    Class hudClass = objc_lookUpClass("_CADeveloperHUDProperties");
    if (!hudClass) {
      return false;
    }
    const auto instanceFn = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    hud_ = [instanceFn(reinterpret_cast<id>(hudClass), @selector(instance)) retain];
    if (!hud_) {
      return false;
    }

    addLabel("com.github.3shain.dxmt9-heading", "com.apple.hud-graph.default");
    addLabel("com.github.3shain.dxmt9-flags", "com.github.3shain.dxmt9-heading");
    addLabel("com.github.3shain.dxmt9-error", "com.github.3shain.dxmt9-flags");
    available_ = true;
  }
  return available_;
}

void DeveloperHudState::addLabel(const char* label, const char* after) {
  NSString* labelString = [[NSString alloc] initWithUTF8String:label];
  NSString* afterString = [NSString stringWithUTF8String:after];
  const auto addFn = reinterpret_cast<BOOL (*)(id, SEL, id, id)>(objc_msgSend);
  (void)addFn(hud_, @selector(addLabel:after:), labelString, afterString);
  labels_.push_back(labelString);
}

void DeveloperHudState::updateLine(size_t index, const std::string& value) {
  if (index >= labels_.size()) {
    return;
  }
  NSString* valueString = [NSString stringWithUTF8String:value.c_str()];
  const auto updateFn = reinterpret_cast<void (*)(id, SEL, id, id)>(objc_msgSend);
  updateFn(hud_, @selector(updateLabel:value:), labels_[index], valueString);
}

metalqueue::CommandBufferDiagnostics
DeveloperHudController::prepareForSubmission(metalqueue::CommandBufferDiagnostics diagnostics) {
  if (!diagnostics.hasPresent) {
    return diagnostics;
  }
  presentedFrame_ += 1;
  lastCompatFlags_ = diagnostics.compatFlags;
  diagnostics.frame = presentedFrame_;
  return diagnostics;
}

void DeveloperHudController::attachCompletionHandler(
    id<MTLCommandBuffer> commandBuffer,
    const metalqueue::CommandBufferDiagnostics& diagnostics,
    metalqueue::CompletionTracker& completionTracker,
    const std::function<void(const metalqueue::CommandBufferDiagnostics&)>& onCompletion,
    const char* context) {
  if (!commandBuffer) {
    return;
  }
  const auto preparedDiagnostics = prepareForSubmission(diagnostics);
  auto* self = this;
  auto* tracker = &completionTracker;
  const auto completion = onCompletion;
  const std::string contextValue = context ? context : "queue";
  [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
    (void)self->observeCompletion(buffer, preparedDiagnostics, *tracker, contextValue.c_str());
    if (completion) {
      completion(preparedDiagnostics);
    }
  }];
}

bool DeveloperHudController::observeCompletion(id<MTLCommandBuffer> commandBuffer,
                                               const metalqueue::CommandBufferDiagnostics& diagnostics,
                                               metalqueue::CompletionTracker& completionTracker,
                                               const char* context) {
  if (!completionTracker.inspect(commandBuffer, diagnostics, context)) {
    return false;
  }
  completeSubmission(diagnostics, completionTracker);
  return true;
}

void DeveloperHudController::completeSubmission(const metalqueue::CommandBufferDiagnostics& diagnostics,
                                                const metalqueue::CompletionTracker& completionTracker) {
  const u32 frame = diagnostics.frame != 0 ? diagnostics.frame : presentedFrame_;
  const u32 flags = diagnostics.compatFlags != 0 ? diagnostics.compatFlags : lastCompatFlags_;
  state_.update(frame, diagnostics.seqId, flags, completionTracker.lastErrorSummary());
}

bool SubmissionDiagnosticsController::inspect(id<MTLCommandBuffer> commandBuffer,
                                              const metalqueue::CommandBufferDiagnostics& diagnostics,
                                              const char* context) {
  return completionTracker_.inspect(commandBuffer, diagnostics, context);
}

void SubmissionDiagnosticsController::attachQueueSubmission(
    id<MTLCommandBuffer> commandBuffer,
    const metalqueue::CommandBufferDiagnostics& diagnostics,
    const std::function<void(const metalqueue::CommandBufferDiagnostics&)>& onCompletion,
    const char* context) {
  hudController_.attachCompletionHandler(commandBuffer, diagnostics, completionTracker_, onCompletion, context);
}

void SubmissionDiagnosticsController::attachTrackedQueueSubmission(
    id<MTLCommandBuffer> commandBuffer,
    const metalqueue::CommandBufferDiagnostics& diagnostics,
    const TrackedQueueSubmissionState& state,
    const char* context) {
  if (state.queueLifecycle) {
    state.queueLifecycle->noteEncodeCommit(makeQueueControllerState(state), state.slotIndex, state.seqId);
  }
  attachQueueSubmission(
      commandBuffer,
      diagnostics,
      [state](const metalqueue::CommandBufferDiagnostics&) {
        if (!state.mutex || !state.completedSeqQueue) {
          return;
        }
        std::lock_guard completionLock(*state.mutex);
        state.completedSeqQueue->push_back(state.seqId);
        if (state.queueLifecycle) {
          state.queueLifecycle->noteGpuComplete(makeQueueControllerState(state), state.slotIndex, state.seqId);
        }
        if (state.finishCv) {
          state.finishCv->notify_all();
        }
      },
      context);
}

}  // namespace dxmt9::core::metalhud
