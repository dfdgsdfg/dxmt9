#pragma once

// Blit-only command encoders. Extracted from backend_metal.mm as the first
// slice of the encoder split — these encoders don't need the pipeline cache,
// allocators, or queue state; just a command buffer, the resource pool, and
// the command descriptor. Larger draw/fill/stretch encoders stay in
// backend_metal.mm for now because they pull in more backend state.

#include "dxmt9/core.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_pool.hpp"
#include "../winemetal/Metal.hpp"

#include <string>

namespace dxmt9::encoders {

using u32 = std::uint32_t;

// Record a GPU→GPU texture copy for a CopyResource-style readback. The
// source's resolve texture is used when present (MSAA resolve path).
void encodeReadback(WMT::CommandBuffer& commandBuffer,
                    resources::Pool& pool,
                    const core::ReadbackDesc& readback);

// Same-size GPU→GPU texture copy with optional fallback to a stretch blit
// when source/destination rects have different sizes. `pipelineCache` +
// device + archive/path are threaded through to the stretch fallback's
// pipeline builder.
void encodeSurfaceCopy(WMT::CommandBuffer& commandBuffer,
                       resources::Pool& pool,
                       pipeline::Cache& pipelineCache,
                       WMT::Reference<WMT::Device> device,
                       const core::BackendLimits& limits,
                       WMT::Reference<WMT::BinaryArchive>* archive,
                       const std::string* archivePath,
                       const core::SurfaceCopyDesc& copy);

// Textured-blit clear-less rect; builds (or reuses) a stretch pipeline.
void encodeStretchRect(WMT::CommandBuffer& commandBuffer,
                        resources::Pool& pool,
                        pipeline::Cache& pipelineCache,
                        WMT::Reference<WMT::Device> device,
                        const core::BackendLimits& limits,
                        WMT::Reference<WMT::BinaryArchive>* archive,
                        const std::string* archivePath,
                        const core::StretchRectDesc& stretch);

// Solid-color fill. If `fill.hasRect`, builds (or reuses) a fill pipeline
// and draws a scissored fullscreen triangle; otherwise just sets the clear
// color on the render pass.
void encodeColorFill(WMT::CommandBuffer& commandBuffer,
                      resources::Pool& pool,
                      pipeline::Cache& pipelineCache,
                      WMT::Reference<WMT::Device> device,
                      const core::BackendLimits& limits,
                      WMT::Reference<WMT::BinaryArchive>* archive,
                      const std::string* archivePath,
                      const core::ColorFillDesc& fill);

// Render-pass with LoadActionClear that targets color attachment 0 (and its
// resolve if present). Used for D3DClear-style non-scissored clears.
void encodeClearPass(WMT::CommandBuffer& commandBuffer,
                      resources::Pool& pool,
                      const core::ClearDesc& clear);

}  // namespace dxmt9::encoders
