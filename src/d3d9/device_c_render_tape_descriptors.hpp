#pragma once

#include "dxmt9/device_c.h"

#include <cstdint>

namespace dxmt9::d3d9 {

// Bounded, pointer-free descriptor payloads shared by PE capture and unix
// provider replay. These are tape schema payloads, not bridge ABI structs.
struct RenderTapeTextureDescriptor {
  D9CSurfaceDesc level0{};
  std::uint32_t levelCount = 0u;
};

struct RenderTapeVertexDeclDescriptor {
  std::uint32_t elementCount = 0u;
  std::uint32_t elementBytes = 0u;
};

struct RenderTapeShaderDescriptor {
  std::uint32_t stage = 0u;
  std::uint32_t bytecodeBytes = 0u;
};

struct RenderTapeQueryDescriptor {
  std::uint32_t type = 0u;
  std::uint32_t dataBytes = 0u;
};

static_assert(sizeof(RenderTapeTextureDescriptor) == sizeof(D9CSurfaceDesc) + 4u);
static_assert(sizeof(RenderTapeVertexDeclDescriptor) == 8u);
static_assert(sizeof(RenderTapeShaderDescriptor) == 8u);
static_assert(sizeof(RenderTapeQueryDescriptor) == 8u);

} // namespace dxmt9::d3d9
