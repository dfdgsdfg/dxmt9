#pragma once

#include <cstdint>

namespace dxmt9::shader {

// R-BACK-4.10: cold library creation is independent of the main executable SDK.
struct CompileProfile {
  std::uint32_t version;
  std::uint32_t languageVersion;
  std::uint32_t mathMode;
  std::uint32_t mathFunctions;
  bool preserveInvariance;
};

inline constexpr CompileProfile kCompileProfile{1u, (2u << 16u) + 4u, 2u, 0u, false};

}  // namespace dxmt9::shader
