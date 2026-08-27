#pragma once

// Blit-only command encoders. Extracted from backend_metal.mm as the first
// slice of the encoder split — these encoders don't need the pipeline cache,
// allocators, or queue state; just a command buffer, the resource pool, and
// the command descriptor. Larger draw/fill/stretch encoders stay in
// backend_metal.mm for now because they pull in more backend state.

#include "dxmt9/core.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_source_payload.hpp"
#include "../winemetal/Metal.hpp"

#include <string>

namespace dxmt9::encoders {

using u32 = std::uint32_t;

constexpr bool canonicalD24X8PhysicalFormatSupported(
    WMTPixelFormat format) noexcept {
  return format == WMTPixelFormatDepth24Unorm_Stencil8 ||
         format == WMTPixelFormatDepth32Float_Stencil8 ||
         format == WMTPixelFormatDepth32Float;
}

constexpr bool canonicalD24X8ReplayPhysicalFormatsCompatible(
    WMTPixelFormat captured, WMTPixelFormat replayed) noexcept {
  return canonicalD24X8PhysicalFormatSupported(captured) &&
         canonicalD24X8PhysicalFormatSupported(replayed);
}

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
                       const core::SurfaceCopyDesc& copy,
                       std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments = {});

// Textured-blit clear-less rect; builds (or reuses) a stretch pipeline.
void encodeStretchRect(WMT::CommandBuffer& commandBuffer,
                        resources::Pool& pool,
                        pipeline::Cache& pipelineCache,
                        WMT::Reference<WMT::Device> device,
                        const core::BackendLimits& limits,
                        WMT::Reference<WMT::BinaryArchive>* archive,
                        const std::string* archivePath,
                        const core::StretchRectDesc& stretch,
                        std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments = {});

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
                      const core::ColorFillDesc& fill,
                      std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments = {});

// Render-pass with LoadActionClear that targets color attachment 0 (and its
// resolve if present). Used for D3DClear-style non-scissored clears.
void encodeClearPass(WMT::CommandBuffer& commandBuffer,
                      resources::Pool& pool,
                      const core::ClearDesc& clear,
                      std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments = {});
void encodeClearPass(WMT::CommandBuffer& commandBuffer,
                     resources::Pool& pool,
                     const core::ClearCommandView& clear,
                     std::span<const WMTSampleBufferAttachmentInfo>
                         sampleBufferAttachments = {});

// R-FORMAT-11 — RESZ multisample depth resolve. Resolves the bound
// multisampled depth surface (`msaaDepthSource`) into the bound INTZ depth
// texture (`intzDestination`) via a render pass whose depth attachment uses
// store=MultisampleResolve + resolve_texture + filter=Sample. The DEPTH twin
// of the color MSAA resolve already wired in encodeStretchRect /
// encodeColorFill / encodeClearPass. No-op when either surface is missing or
// lacks a depth aspect.
void encodeDepthResolve(WMT::CommandBuffer& commandBuffer,
                        resources::Pool& pool,
                        core::Handle msaaDepthSource,
                        core::Handle intzDestination,
                        std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments = {});

bool encodeGenerateMipmaps(WMT::CommandBuffer& commandBuffer,
                           resources::Pool& pool,
                           const core::GenerateMipmapsDesc& generate);

}  // namespace dxmt9::encoders

// Forward decl for readbackSurface.
namespace dxmt9 { class CommandQueue; }

namespace dxmt9::encoders {

// Synchronous GetRenderTargetData-style readback. Creates a staging
// texture, blits source → staging, then copies staging → staging buffer,
// both on ephemeral command buffers awaited with waitUntilCompleted.
// Populates `pixels.bytes` + `pixels.pitch`. Returns false if the source
// surface is missing or the format has no CPU-readable layout.
bool readbackSurface(CommandQueue& queue,
                      resources::Pool& pool,
                      WMT::Reference<WMT::Device> device,
                      const core::BackendLimits& limits,
                      const core::ReadbackDesc& desc,
                      core::ReadbackPixels& pixels);

// Capture-only D24X8 conversion. Both directions use an explicit shader
// conversion to/from canonical float32 depth and validate the exact physical
// texture format selected from BackendLimits.
bool captureCanonicalD24X8Depth(CommandQueue& queue,
                                resources::Pool& pool,
                                WMT::Reference<WMT::Device> device,
                                const core::BackendLimits& limits,
                                core::SurfaceHandle source,
                                core::CanonicalD24X8Depth& depth);
bool seedCanonicalD24X8Depth(CommandQueue& queue,
                             resources::Pool& pool,
                             WMT::Reference<WMT::Device> device,
                             const core::BackendLimits& limits,
                             core::SurfaceHandle destination,
                             const core::CanonicalD24X8Depth& depth);

}  // namespace dxmt9::encoders
