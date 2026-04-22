#pragma once

// Blit-only command encoders. Extracted from backend_metal.mm as the first
// slice of the encoder split — these encoders don't need the pipeline cache,
// allocators, or queue state; just a command buffer, the resource pool, and
// the command descriptor. Larger draw/fill/stretch encoders stay in
// backend_metal.mm for now because they pull in more backend state.

#include "dxmt9/core.hpp"
#include "dxmt9_resource_pool.hpp"
#include "../winemetal/Metal.hpp"

namespace dxmt9::encoders {

// Record a GPU→GPU texture copy for a CopyResource-style readback. The
// source's resolve texture is used when present (MSAA resolve path).
void encodeReadback(WMT::CommandBuffer& commandBuffer,
                    resources::Pool& pool,
                    const core::ReadbackDesc& readback);

}  // namespace dxmt9::encoders
