#pragma once

// Conversion helpers from core D3D9-style enums/types to WMT equivalents.
// Pure functions with no state — lifted out of backend_metal.mm's anonymous
// namespace so they can be reused by any WMT-oriented module (PipelineCache,
// ResourcePool, Renderer) without pulling the backend TU in.

#include "dxmt9/core.hpp"
#include "../winemetal/Metal.hpp"

#include <cstdint>

namespace dxmt9::convert {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Core format → WMT pixel format. Depth formats consult BackendLimits to pick
// between Depth24Stencil8 / Depth32FloatStencil8 based on device support.
WMTPixelFormat toPixelFormat(core::Format format, const core::BackendLimits& limits);
WMTPixelFormat toPixelFormat(core::Format format, const core::BackendLimits& limits, bool srgb);
WMTPixelFormat toSrgbPixelFormat(WMTPixelFormat format);

bool formatHasStencilAspect(core::Format format);
bool formatHasDepthAspect(core::Format format);

WMTTextureType toTextureType(core::TextureType type, bool multisample);
bool formatNeedsShaderReadSwizzle(core::Format format);
bool suppressRenderTargetPixelFormatViewEnabled();
bool suppressX8RenderTargetPixelFormatViewEnabled();
bool textureNeedsShaderReadView(const core::TextureDesc& desc,
                                bool suppressRenderTargetPixelFormatView,
                                bool suppressX8RenderTargetPixelFormatView);
bool textureNeedsShaderReadView(const core::TextureDesc& desc,
                                bool suppressRenderTargetPixelFormatView);
bool textureNeedsShaderReadView(const core::TextureDesc& desc);
WMTTextureSwizzleChannels toShaderReadSwizzle(core::Format format);
std::uint16_t toShaderReadViewSliceCount(core::TextureType type);
// R-BACK-5.7: Pool/usage → Metal storage mode mapping. `hasUnifiedMemory`
// must come from a single MTLDevice.hasUnifiedMemory() probe cached at
// device init (do not call per-resource). On unified-memory devices
// (Apple Silicon), `D3DPOOL_MANAGED` resources stay on
// `MTLStorageModeShared` with no staging copy — both CPU and GPU view the
// same physical memory. On discrete-style devices the `MANAGED` pool maps
// to `MTLStorageModeManaged`, which still requires a staging blit upload
// path; that path is gated separately in the texture upload code and is
// counted via `perf::countManagedTextureUploadBlit`.
WMTResourceOptions toResourceOptions(core::Pool pool, u32 usage, bool hasUnifiedMemory);
WMTTextureUsage toTextureUsage(const core::SurfaceDesc& desc);
WMTTextureUsage toTextureUsage(const core::TextureDesc& desc,
                               bool suppressRenderTargetPixelFormatView,
                               bool suppressX8RenderTargetPixelFormatView);
WMTTextureUsage toTextureUsage(const core::TextureDesc& desc,
                               bool suppressRenderTargetPixelFormatView);
WMTTextureUsage toTextureUsage(const core::TextureDesc& desc);

WMTPrimitiveType toPrimitiveType(core::PrimitiveType type);
WMTIndexType toIndexType(core::IndexType type);

WMTCompareFunction toCompareFunction(u32 value);
WMTBlendOperation toBlendOperation(u32 value);
WMTBlendFactor toBlendFactor(u32 value, bool alphaLane = false);
WMTCullMode toCullMode(u32 value);
WMTStencilOperation toStencilOperation(u32 value);

// D3D9-style color-write-mask bits (RGBA = 1|2|4|8) → WMT color-write-mask.
std::uint8_t toColorWriteMask(u32 value);

}  // namespace dxmt9::convert
