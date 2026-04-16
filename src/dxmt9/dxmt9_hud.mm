#import <objc/message.h>
#import <objc/runtime.h>

#include "dxmt9_hud.hpp"

#include "util/config/config.hpp"

#include <cmath>
#include <sstream>

namespace dxmt9::core::metalhud {

bool compatHudEnabled() {
  static const bool enabled = dxmt9::util::getenvFlag("DXMT_COMPAT_HUD");
  return enabled;
}

bool isFloatRenderTargetFormat(Format format) {
  switch (format) {
    case Format::A16B16G16R16F:
    case Format::A32B32G32R32F:
    case Format::G16R16F:
    case Format::R16F:
    case Format::G32R32F:
    case Format::R32F:
      return true;
    default:
      return false;
  }
}

bool matrixIsIdentity(const Matrix4x4& matrix) {
  for (u32 row = 0; row < 4; ++row) {
    for (u32 col = 0; col < 4; ++col) {
      const float expected = row == col ? 1.0f : 0.0f;
      if (std::fabs(matrix.m[row * 4 + col] - expected) > 1.0e-6f) {
        return false;
      }
    }
  }
  return true;
}

std::string formatCompatFlags(u32 flags) {
  std::ostringstream out;
  const auto appendFlag = [&](u32 bit, const char* text) {
    if ((flags & bit) == 0) {
      return;
    }
    if (out.tellp() > 0) {
      out << ' ';
    }
    out << text;
  };
  appendFlag(CompatFlagFp16, "F16");
  appendFlag(CompatFlagMrt, "MRT");
  appendFlag(CompatFlagSrgb, "SRG");
  appendFlag(CompatFlagProjected, "PJT");
  appendFlag(CompatFlagMsaa, "MSA");
  appendFlag(CompatFlagQuery, "QRY");
  if (out.tellp() == 0) {
    out << '-';
  }
  return out.str();
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
  updateLine(1, "compat " + formatCompatFlags(flags));
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

}  // namespace dxmt9::core::metalhud
