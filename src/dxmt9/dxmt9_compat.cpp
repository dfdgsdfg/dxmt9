#include "dxmt9_compat.hpp"

#include <cmath>
#include <sstream>

namespace dxmt9::core::metalcompat {

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

}  // namespace dxmt9::core::metalcompat
