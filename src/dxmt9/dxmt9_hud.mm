#include "dxmt9_hud.hpp"

#include "util/config/config.hpp"

#include <sstream>

namespace dxmt9::core::metalhud {

bool compatHudEnabled() {
  static const bool enabled = dxmt9::util::getenvFlag("DXMT_COMPAT_HUD");
  return enabled;
}

DeveloperHudState::DeveloperHudState() = default;

DeveloperHudState::~DeveloperHudState() = default;

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

  hud_ = WMT::DeveloperHUDProperties::instance();
  if (!hud_) {
    return false;
  }

  addLabel("com.github.3shain.dxmt9-heading", "com.apple.hud-graph.default");
  addLabel("com.github.3shain.dxmt9-flags", "com.github.3shain.dxmt9-heading");
  addLabel("com.github.3shain.dxmt9-error", "com.github.3shain.dxmt9-flags");
  available_ = true;
  return available_;
}

void DeveloperHudState::addLabel(const char* label, const char* after) {
  auto labelString = WMT::MakeString(label, WMTUTF8StringEncoding);
  auto afterString = WMT::MakeString(after, WMTUTF8StringEncoding);
  (void)hud_.addLabel(labelString, afterString);
  labels_.push_back(std::move(labelString));
}

void DeveloperHudState::updateLine(size_t index, const std::string& value) {
  if (index >= labels_.size()) {
    return;
  }
  auto valueString = WMT::MakeString(value.c_str(), WMTUTF8StringEncoding);
  hud_.updateLabel(labels_[index], valueString);
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

metalqueue::CommandBufferDiagnostics SubmissionDiagnosticsController::prepareQueueSubmission(
    metalqueue::CommandBufferDiagnostics diagnostics) {
  return hudController_.prepareForSubmission(diagnostics);
}

bool SubmissionDiagnosticsController::inspect(obj_handle_t commandBuffer,
                                              const metalqueue::CommandBufferDiagnostics& diagnostics,
                                              const char* context) {
  return completionTracker_.inspect((id<MTLCommandBuffer>)commandBuffer, diagnostics, context);
}

bool SubmissionDiagnosticsController::observeQueueSubmission(
    obj_handle_t commandBuffer,
    const metalqueue::CommandBufferDiagnostics& diagnostics,
    const char* context) {
  return hudController_.observeCompletion((id<MTLCommandBuffer>)commandBuffer, diagnostics,
                                         completionTracker_, context);
}

}  // namespace dxmt9::core::metalhud
