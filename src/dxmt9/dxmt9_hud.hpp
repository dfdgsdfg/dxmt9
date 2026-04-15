#pragma once

#import <Foundation/Foundation.h>

#include "dxmt9/core.hpp"

#include <string>
#include <vector>

namespace dxmt9::core::metalhud {

enum CompatFlagBits : u32 {
  CompatFlagFp16 = 1u << 0,
  CompatFlagMrt = 1u << 1,
  CompatFlagSrgb = 1u << 2,
  CompatFlagProjected = 1u << 3,
  CompatFlagMsaa = 1u << 4,
  CompatFlagQuery = 1u << 5,
};

bool compatHudEnabled();
bool isFloatRenderTargetFormat(Format format);
bool matrixIsIdentity(const Matrix4x4& matrix);
std::string formatCompatFlags(u32 flags);

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

}  // namespace dxmt9::core::metalhud
