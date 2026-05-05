#pragma once

// D3D9 fixed-function pipeline (FFP) shader generation + vertex layout
// decoding. Lifted out of backend_metal.mm to give the FFP pipeline a named
// home. The generators produce Metal Shading Language source for the
// auto-generated vertex and pixel shaders that D3D9 requires when the app
// has not set explicit shaders.

#include "dxmt9/core.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace dxmt9::drawshader {
struct ShaderSourceContext;
}

namespace dxmt9::ffp {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// D3D9 FVF bits + vertex-declaration usage/type codes. Previously duplicated
// as constexpr inside backend_metal.mm's anonymous namespace; centralized
// here so the FFP layout decoder + encoders share the definitions.
inline constexpr u32 kFvfPositionMask = 0x000eu;
inline constexpr u32 kFvfXyz = 0x0002u;
inline constexpr u32 kFvfXyzrhw = 0x0004u;
inline constexpr u32 kFvfNormal = 0x0010u;
inline constexpr u32 kFvfDiffuse = 0x0040u;
inline constexpr u32 kFvfSpecular = 0x0080u;
inline constexpr u32 kFvfTexCountMask = 0x0f00u;
inline constexpr u32 kFvfTexCountShift = 8u;

inline constexpr u32 kD3DDeclTypeFloat1 = 0u;
inline constexpr u32 kD3DDeclTypeFloat2 = 1u;
inline constexpr u32 kD3DDeclTypeFloat3 = 2u;
inline constexpr u32 kD3DDeclTypeFloat4 = 3u;
inline constexpr u32 kD3DDeclTypeD3DColor = 4u;
inline constexpr u32 kD3DDeclUsagePosition = 0u;
inline constexpr u32 kD3DDeclUsagePSize = 4u;
inline constexpr u32 kD3DDeclUsageTexcoord = 5u;
inline constexpr u32 kD3DDeclUsagePositionT = 9u;
inline constexpr u32 kD3DDeclUsageColor = 10u;
inline constexpr u32 kD3DDeclUsageFog = 11u;

// Decoded vertex layout (per-element offsets into the single-stream buffer)
// used by the FFP generator to emit attribute loads. Keyed by DrawDesc's
// vertex declaration or FVF code.
struct FixedFunctionVertexLayout {
  bool valid = false;
  bool preTransformed = false;
  u32 positionComponents = 0;
  bool hasDiffuse = false;
  std::array<bool, core::kMaxTextureStages> hasTexcoord{};
  u32 stride = 0;
  u32 positionOffset = 0;
  u32 diffuseOffset = 0;
  std::array<u32, core::kMaxTextureStages> texcoordOffset{};
  u64 hash = 0;
};

// Size in bytes of a D3D9 vertex-declaration element type. Returns 0 for
// unknown types (caller typically treats that as "skip").
u32 declTypeSize(u32 type);

// Combined stride computation: returns the declared stream[0].stride if set,
// otherwise the maximum (offset + size) over all elements on stream[0].
u32 computeVertexDeclStride(const core::VertexDeclSnapshot& decl);
u32 computeVertexDeclStride(const core::DrawDesc& desc);

// Per-input-register binding for the translated programmable vertex path.
struct VertexInputBinding {
  bool valid = false;
  u32 offset = 0;
  u32 type = 0;
  u32 usage = 0;
  u32 usageIndex = 0;
};

// Decoded DCL declaration layout for a D3D9 programmable vertex shader.
// Populated by the shader translator; members indexed by D3DSPR_INPUT reg.
struct VertexShaderInputLayout {
  u32 stride = 0;
  std::array<VertexInputBinding, 16> inputs{};
  u64 hash = 0;
};

u64 hashVertexShaderInputLayout(const VertexShaderInputLayout& layout);

// Hash a D3D9 vertex declaration for variant-keying.
u64 hashVertexDeclaration(const core::VertexDeclSnapshot& decl);

// Decode a DrawDesc's vertex declaration into a FixedFunctionVertexLayout.
// Returns nullopt if the declaration doesn't describe a position attribute
// recognizable as FFP (e.g., it's a programmable-pipeline decl).
std::optional<FixedFunctionVertexLayout> decodeFixedFunctionVertexLayout(const core::VertexDeclSnapshot& decl);
std::optional<FixedFunctionVertexLayout> decodeFixedFunctionVertexLayout(const core::DrawDesc& desc);

// Generate Metal Shading Language source for the FFP vertex / pixel
// shaders keyed by (key, ShaderSourceContext). Outputs a complete standalone MSL
// translation unit that can be passed to WMT::Device::newLibraryFromSource.
std::string makeFfpVertexSource(const core::FfpVertexKey& key,
                                const drawshader::ShaderSourceContext& context);
std::string makeFfpPixelSource(const core::FfpPixelKey& key,
                               const drawshader::ShaderSourceContext& context);

}  // namespace dxmt9::ffp
