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
inline constexpr u32 kD3DDeclTypeUByte4 = 5u;
inline constexpr u32 kD3DDeclTypeShort2 = 6u;
inline constexpr u32 kD3DDeclTypeShort4 = 7u;
inline constexpr u32 kD3DDeclTypeUByte4N = 8u;
inline constexpr u32 kD3DDeclTypeShort2N = 9u;
inline constexpr u32 kD3DDeclTypeShort4N = 10u;
inline constexpr u32 kD3DDeclTypeUShort2N = 11u;
inline constexpr u32 kD3DDeclTypeUShort4N = 12u;
inline constexpr u32 kD3DDeclTypeUDec3 = 13u;
inline constexpr u32 kD3DDeclTypeDec3N = 14u;
inline constexpr u32 kD3DDeclTypeFloat16_2 = 15u;
inline constexpr u32 kD3DDeclTypeFloat16_4 = 16u;
inline constexpr u32 kD3DDeclUsagePosition = 0u;
inline constexpr u32 kD3DDeclUsagePSize = 4u;
inline constexpr u32 kD3DDeclUsageTexcoord = 5u;
inline constexpr u32 kD3DDeclUsagePositionT = 9u;
inline constexpr u32 kD3DDeclUsageColor = 10u;
inline constexpr u32 kD3DDeclUsageFog = 11u;

// Decoded vertex layout (per-element offsets into the single-stream buffer)
// used by the FFP generator to emit attribute loads. Keyed by a vertex
// declaration snapshot or FVF code.
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

// Decode a vertex declaration into a FixedFunctionVertexLayout.
// Returns nullopt if the declaration doesn't describe a position attribute
// recognizable as FFP (e.g., it's a programmable-pipeline decl).
std::optional<FixedFunctionVertexLayout> decodeFixedFunctionVertexLayout(const core::VertexDeclSnapshot& decl);

// Generate Metal Shading Language source for the FFP vertex / pixel
// shaders keyed by (key, ShaderSourceContext). Outputs a complete standalone MSL
// translation unit that can be passed to WMT::Device::newLibraryFromSource.
std::string makeFfpVertexSource(const core::FfpVertexKey& key,
                                const drawshader::ShaderSourceContext& context);
std::string makeFfpPixelSource(const core::FfpPixelKey& key,
                               const drawshader::ShaderSourceContext& context);

// R-BACK-13.* — Apple-Silicon-only tile-stage FFP source generator.
// Mirrors makeFfpPixelSource but emits a tile-stage `[[kernel]]` that
// reads attachment color via `imageblock<>` (programmable blending) and
// writes back, executing fog blend / alpha-test / A2C without a fragment
// dispatch. R-BACK-13.7: FFP arithmetic is typed `float`, not `half`,
// to keep bit-identity with the portable path. The imageblock element
// type is selected by `colorAttachmentPixelFormat` — `half4` is permitted
// only for 8-bpc unorm formats whose attachment quantization already
// discards precision below `half`; wider formats use `float4`.
std::string makeFfpTilePixelSource(const core::FfpPixelKey& key,
                                    const drawshader::ShaderSourceContext& context,
                                    std::uint32_t colorAttachmentPixelFormat);

// Selector classification + counter wiring helpers (R-BACK-13.1..13.6).
// Pure value transforms; tested via tile_ffp_selector_spec without standing
// up a Metal device. Eligibility is observable as a discriminated value so
// the encoder can map it directly to the right counter.
enum class TileFfpEligibility : std::uint8_t {
  Eligible,
  IneligiblePrecision,
  IneligibleUnsupportedState,
};

// Classify an FFPKeyPS for tile-stage execution. Returns the reason class
// (R-BACK-13.3 conformance boundary). The selector lives in pure code so
// the encoder can reuse it on encoder-open and on mid-pass eligibility
// re-checks (R-BACK-13.6) without duplicating the rules.
TileFfpEligibility classifyTileFfpEligibility(const core::FfpPixelKey& key,
                                              float alphaTestRefNormalized,
                                              bool alphaToCoverageEnabled);

// True iff `format` is an 8-bit-per-channel attachment whose quantization
// already discards precision below `half`. R-BACK-13.7 permits `half4`
// imageblock declarations only on these formats.
bool tileFfpAttachmentAcceptsHalf(std::uint32_t pixelFormat);

}  // namespace dxmt9::ffp
