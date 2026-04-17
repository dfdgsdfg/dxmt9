#pragma once

#include "dxmt9/core.hpp"

#include <string>

namespace dxmt9::core::metalcompat {

enum CompatFlagBits : u32 {
  CompatFlagFp16 = 1u << 0,
  CompatFlagMrt = 1u << 1,
  CompatFlagSrgb = 1u << 2,
  CompatFlagProjected = 1u << 3,
  CompatFlagMsaa = 1u << 4,
  CompatFlagQuery = 1u << 5,
};

bool isFloatRenderTargetFormat(Format format);
bool matrixIsIdentity(const Matrix4x4& matrix);
std::string formatCompatFlags(u32 flags);

}  // namespace dxmt9::core::metalcompat
