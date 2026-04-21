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

bool formatHasStencilAspect(core::Format format);
bool formatHasDepthAspect(core::Format format);

WMTTextureType toTextureType(core::TextureType type, bool multisample);
WMTResourceOptions toResourceOptions(core::Pool pool, u32 usage);
WMTTextureUsage toTextureUsage(const core::SurfaceDesc& desc);
WMTTextureUsage toTextureUsage(const core::TextureDesc& desc);

WMTPrimitiveType toPrimitiveType(core::PrimitiveType type);
WMTIndexType toIndexType(core::IndexType type);

WMTCompareFunction toCompareFunction(u32 value);
WMTBlendOperation toBlendOperation(u32 value);
WMTBlendFactor toBlendFactor(u32 value);
WMTCullMode toCullMode(u32 value);
WMTStencilOperation toStencilOperation(u32 value);

// D3D9-style color-write-mask bits (RGBA = 1|2|4|8) → WMT color-write-mask.
// Returns WMTColorWriteMaskAll if the input is 0 (matches prior behavior).
std::uint8_t toColorWriteMask(u32 value);

}  // namespace dxmt9::convert
