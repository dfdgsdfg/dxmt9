#pragma once

#include "dxmt9/core_constants.hpp"

namespace dxmt9::d3d9 {

// Valid only for a known render-state setter, not a generic mutableState
// borrow. Other render states still invalidate hot state and shader layout.
constexpr bool renderStateAffectsNonconstantUniform(core::u32 key) noexcept {
  return key == core::RS_CLIPPING || key == core::RS_CLIP_PLANE_ENABLE;
}

} // namespace dxmt9::d3d9
