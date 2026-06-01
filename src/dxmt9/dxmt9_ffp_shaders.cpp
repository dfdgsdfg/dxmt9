#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_shader_sources.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace dxmt9::ffp {

using namespace dxmt9::core;
// The FVF / D3D-decl constants are already in this namespace (defined in the
// header). The `using namespace dxmt9::core;` above brings in TextureOp,
// FogMode, Handle, and the kMax* constants.

u32 declTypeSize(u32 type) {
  switch (type) {
    case kD3DDeclTypeFloat1:
      return 4;
    case kD3DDeclTypeFloat2:
      return 8;
    case kD3DDeclTypeFloat3:
      return 12;
    case kD3DDeclTypeFloat4:
      return 16;
    case kD3DDeclTypeD3DColor:
    case kD3DDeclTypeUByte4:
    case kD3DDeclTypeUByte4N:
    case kD3DDeclTypeShort2:
    case kD3DDeclTypeShort2N:
    case kD3DDeclTypeUShort2N:
    case kD3DDeclTypeUDec3:
    case kD3DDeclTypeDec3N:
    case kD3DDeclTypeFloat16_2:
      return 4;
    case kD3DDeclTypeShort4:
    case kD3DDeclTypeShort4N:
    case kD3DDeclTypeUShort4N:
    case kD3DDeclTypeFloat16_4:
      return 8;
    default:
      return 0;
  }
}

namespace {

u32 fvfTexcoordSize(u32 fvf, u32 index) {
  const u32 code = (fvf >> (16u + index * 2u)) & 0x3u;
  switch (code) {
    case 1u:
      return 3;
    case 2u:
      return 4;
    case 3u:
      return 1;
    default:
      return 2;
  }
}

u64 hashFixedFunctionLayout(const FixedFunctionVertexLayout& layout) {
  u64 hash = 1469598103934665603ull;
  hash ^= static_cast<u64>(layout.valid);
  hash *= 1099511628211ull;
  hash ^= static_cast<u64>(layout.preTransformed);
  hash *= 1099511628211ull;
  hash ^= layout.positionComponents;
  hash *= 1099511628211ull;
  hash ^= static_cast<u64>(layout.hasDiffuse);
  hash *= 1099511628211ull;
  hash ^= static_cast<u64>(layout.hasSpecular);
  hash *= 1099511628211ull;
  hash ^= static_cast<u64>(layout.hasNormal);
  hash *= 1099511628211ull;
  hash ^= static_cast<u64>(layout.hasPointSize);
  hash *= 1099511628211ull;
  hash ^= static_cast<u64>(layout.hasBlendWeight);
  hash *= 1099511628211ull;
  hash ^= static_cast<u64>(layout.hasBlendIndices);
  hash *= 1099511628211ull;
  hash ^= layout.stride;
  hash *= 1099511628211ull;
  hash ^= layout.positionOffset;
  hash *= 1099511628211ull;
  hash ^= layout.diffuseOffset;
  hash *= 1099511628211ull;
  hash ^= layout.specularOffset;
  hash *= 1099511628211ull;
  hash ^= layout.normalOffset;
  hash *= 1099511628211ull;
  hash ^= layout.pointSizeOffset;
  hash *= 1099511628211ull;
  hash ^= layout.blendWeightOffset;
  hash *= 1099511628211ull;
  hash ^= layout.blendWeightComponents;
  hash *= 1099511628211ull;
  hash ^= layout.blendIndicesOffset;
  hash *= 1099511628211ull;
  for (size_t i = 0; i < layout.hasTexcoord.size(); ++i) {
    hash ^= static_cast<u64>(layout.hasTexcoord[i]);
    hash *= 1099511628211ull;
    hash ^= layout.texcoordComponents[i];
    hash *= 1099511628211ull;
    hash ^= layout.texcoordOffset[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

u32 floatDeclTypeComponents(u32 type) {
  switch (type) {
    case kD3DDeclTypeFloat1:
      return 1;
    case kD3DDeclTypeFloat2:
      return 2;
    case kD3DDeclTypeFloat3:
      return 3;
    case kD3DDeclTypeFloat4:
      return 4;
    default:
      return 0;
  }
}

std::string texcoordLoadExpression(u32 offset, u32 components) {
  switch (components) {
    case 1:
      return "float4(dxmt9_load_f32(stream0, base + " +
             std::to_string(offset) + "u), 0.0f, 1.0f, 1.0f)";
    case 3:
      return "float4(dxmt9_load_f32x3(stream0, base + " +
             std::to_string(offset) + "u), 1.0f)";
    case 4:
      return "dxmt9_load_f32x4(stream0, base + " +
             std::to_string(offset) + "u)";
    case 2:
    default:
      return "float4(dxmt9_load_f32x2(stream0, base + " +
             std::to_string(offset) + "u), 1.0f, 1.0f)";
  }
}

std::string materialSourceExpr(u32 mode, const char* materialField) {
  switch (mode) {
    case 1u:
      return "dxmt9_vertexDiffuse";
    case 2u:
      return "dxmt9_vertexSpecular";
    default:
      return std::string("ffpVs.") + materialField;
  }
}

void emitLightingBlock(std::ostringstream& shader, const FfpVertexKey& key) {
  if (!key.lightingEnabled) {
    return;
  }
  bool hasSupportedLight = false;
  for (u32 i = 0; i < kMaxLights; ++i) {
    if (!key.lightEnabled[i]) {
      continue;
    }
    const u32 type = key.lightType[i];
    if (type == static_cast<u32>(LightType::Directional) ||
        type == static_cast<u32>(LightType::Point) ||
        type == static_cast<u32>(LightType::Spot)) {
      hasSupportedLight = true;
      break;
    }
  }
  if (!hasSupportedLight) {
    return;
  }

  shader << "  float4 dxmt9_vertexDiffuse = out.color;\n";
  shader << "  float4 dxmt9_vertexSpecular = out.secondaryColor;\n";
  shader << "  float4 dxmt9_materialEmissive = "
         << materialSourceExpr(key.colorMaterialMode[0], "materialEmissive") << ";\n";
  shader << "  float4 dxmt9_materialAmbient = "
         << materialSourceExpr(key.colorMaterialMode[1], "materialAmbient") << ";\n";
  shader << "  float4 dxmt9_materialDiffuse = "
         << materialSourceExpr(key.colorMaterialMode[2], "materialDiffuse") << ";\n";
  shader << "  float4 dxmt9_materialSpecular = "
         << materialSourceExpr(key.colorMaterialMode[3], "materialSpecular") << ";\n";
  shader << "  float3 dxmt9_litNormal = normalize(dxmt9_lightingNormal);\n";
  shader << "  float3 dxmt9_diffuseAccum = dxmt9_materialEmissive.rgb + "
            "dxmt9_materialAmbient.rgb * ffpVs.globalAmbient.rgb;\n";
  shader << "  float3 dxmt9_specularAccum = float3(0.0f);\n";
  for (u32 i = 0; i < kMaxLights; ++i) {
    if (!key.lightEnabled[i]) {
      continue;
    }
    const u32 type = key.lightType[i];
    const bool isDirectional = type == static_cast<u32>(LightType::Directional);
    const bool isPoint = type == static_cast<u32>(LightType::Point);
    const bool isSpot = type == static_cast<u32>(LightType::Spot);
    if (!isDirectional && !isPoint && !isSpot) {
      continue;
    }

    // D3D9 ambient contribution from per-light ambient is applied
    // before attenuation gating in the SDK reference; we keep that
    // ordering identical for Directional but for Point/Spot we
    // multiply by the attenuation * spot factor so a light that is
    // out-of-range or outside its cone contributes nothing.
    if (isDirectional) {
      shader << "  dxmt9_diffuseAccum += dxmt9_materialAmbient.rgb * ffpVs.lightAmbient["
             << i << "].rgb;\n";
      shader << "  float3 dxmt9_lightVec" << i << " = -ffpVs.lightDirection[" << i << "].xyz;\n";
      shader << "  if (length(dxmt9_lightVec" << i << ") <= 1.0e-8f) "
             << "dxmt9_lightVec" << i << " = float3(0.0f, 0.0f, 1.0f);\n";
      shader << "  dxmt9_lightVec" << i << " = normalize(dxmt9_lightVec" << i << ");\n";
      shader << "  float dxmt9_ndotl" << i
             << " = max(dot(dxmt9_litNormal, dxmt9_lightVec" << i << "), 0.0f);\n";
      shader << "  dxmt9_diffuseAccum += dxmt9_materialDiffuse.rgb * ffpVs.lightDiffuse["
             << i << "].rgb * dxmt9_ndotl" << i << ";\n";
      if (key.specularEnabled) {
        shader << "  float3 dxmt9_halfVec" << i
               << " = normalize(dxmt9_lightVec" << i << " + float3(0.0f, 0.0f, 1.0f));\n";
        shader << "  float dxmt9_specFactor" << i
               << " = dxmt9_ndotl" << i << " > 0.0f ? pow(max(dot(dxmt9_litNormal, dxmt9_halfVec"
               << i << "), 0.0f), max(ffpVs.materialPower.x, 1.0f)) : 0.0f;\n";
        shader << "  dxmt9_specularAccum += dxmt9_materialSpecular.rgb * ffpVs.lightSpecular["
               << i << "].rgb * dxmt9_specFactor" << i << ";\n";
      }
    } else {
      // Point / Spot — D3D9 §B.5 evaluation.
      // L = lightPos - vertexPos (camera space). Distance gate at Range.
      shader << "  float3 dxmt9_lightDelta" << i
             << " = ffpVs.lightPosition[" << i << "].xyz - dxmt9_cameraPosition.xyz;\n";
      shader << "  float dxmt9_lightDist" << i
             << " = length(dxmt9_lightDelta" << i << ");\n";
      shader << "  float dxmt9_lightRange" << i
             << " = ffpVs.lightPosition[" << i << "].w;\n";
      shader << "  float3 dxmt9_lightVec" << i << " = dxmt9_lightDist" << i
             << " > 1.0e-8f ? dxmt9_lightDelta" << i << " / dxmt9_lightDist" << i
             << " : float3(0.0f, 0.0f, 1.0f);\n";
      // attenuation = 1 / (A0 + A1*d + A2*d^2). Saturated to [0,1] per
      // D3D9 reference. Beyond Range the light is fully attenuated to 0.
      shader << "  float dxmt9_attenDenom" << i
             << " = ffpVs.lightAttenuation[" << i << "].x"
             << " + ffpVs.lightAttenuation[" << i << "].y * dxmt9_lightDist" << i
             << " + ffpVs.lightAttenuation[" << i << "].z * dxmt9_lightDist" << i
             << " * dxmt9_lightDist" << i << ";\n";
      shader << "  float dxmt9_attenuation" << i
             << " = dxmt9_lightDist" << i << " > dxmt9_lightRange" << i
             << " ? 0.0f : saturate(1.0f / max(dxmt9_attenDenom" << i
             << ", 1.0e-8f));\n";
      if (isSpot) {
        // rho = dot(-SpotDir, L). Spot factor:
        //   inside inner cone (rho >= cosTheta/2) -> 1
        //   outside outer cone (rho <= cosPhi/2)  -> 0
        //   between                               -> ((rho - cosPhi)/(cosTheta - cosPhi))^falloff
        shader << "  float3 dxmt9_spotDir" << i
               << " = -ffpVs.lightDirection[" << i << "].xyz;\n";
        shader << "  if (length(dxmt9_spotDir" << i << ") <= 1.0e-8f) "
               << "dxmt9_spotDir" << i << " = float3(0.0f, 0.0f, 1.0f);\n";
        shader << "  dxmt9_spotDir" << i << " = normalize(dxmt9_spotDir" << i << ");\n";
        shader << "  float dxmt9_spotRho" << i
               << " = dot(dxmt9_spotDir" << i << ", dxmt9_lightVec" << i << ");\n";
        shader << "  float dxmt9_spotCosInner" << i
               << " = ffpVs.lightSpotCone[" << i << "].x;\n";
        shader << "  float dxmt9_spotCosOuter" << i
               << " = ffpVs.lightSpotCone[" << i << "].y;\n";
        shader << "  float dxmt9_spotFalloff" << i
               << " = ffpVs.lightAttenuation[" << i << "].w;\n";
        shader << "  float dxmt9_spotDenom" << i
               << " = max(dxmt9_spotCosInner" << i << " - dxmt9_spotCosOuter" << i
               << ", 1.0e-6f);\n";
        shader << "  float dxmt9_spotFactor" << i
               << " = dxmt9_spotRho" << i << " <= dxmt9_spotCosOuter" << i
               << " ? 0.0f : (dxmt9_spotRho" << i << " >= dxmt9_spotCosInner" << i
               << " ? 1.0f : pow(saturate((dxmt9_spotRho" << i
               << " - dxmt9_spotCosOuter" << i << ") / dxmt9_spotDenom" << i
               << "), max(dxmt9_spotFalloff" << i << ", 0.0f)));\n";
        shader << "  dxmt9_attenuation" << i
               << " *= dxmt9_spotFactor" << i << ";\n";
      }
      // Per-light ambient is gated by the same attenuation/spot factor
      // for Point/Spot — outside the cone or beyond range contributes
      // nothing.
      shader << "  dxmt9_diffuseAccum += dxmt9_materialAmbient.rgb"
             << " * ffpVs.lightAmbient[" << i << "].rgb"
             << " * dxmt9_attenuation" << i << ";\n";
      shader << "  float dxmt9_ndotl" << i
             << " = max(dot(dxmt9_litNormal, dxmt9_lightVec" << i << "), 0.0f);\n";
      shader << "  dxmt9_diffuseAccum += dxmt9_materialDiffuse.rgb"
             << " * ffpVs.lightDiffuse[" << i << "].rgb * dxmt9_ndotl" << i
             << " * dxmt9_attenuation" << i << ";\n";
      if (key.specularEnabled) {
        // Half-vector uses local-eye approximation matching the
        // directional path (eye = +Z in camera space).
        shader << "  float3 dxmt9_halfVec" << i
               << " = normalize(dxmt9_lightVec" << i << " + float3(0.0f, 0.0f, 1.0f));\n";
        shader << "  float dxmt9_specFactor" << i
               << " = dxmt9_ndotl" << i << " > 0.0f ? pow(max(dot(dxmt9_litNormal, dxmt9_halfVec"
               << i << "), 0.0f), max(ffpVs.materialPower.x, 1.0f)) : 0.0f;\n";
        shader << "  dxmt9_specularAccum += dxmt9_materialSpecular.rgb"
               << " * ffpVs.lightSpecular[" << i << "].rgb * dxmt9_specFactor" << i
               << " * dxmt9_attenuation" << i << ";\n";
      }
    }
  }
  shader << "  out.color = saturate(float4(dxmt9_diffuseAccum, dxmt9_materialDiffuse.a));\n";
  if (key.specularEnabled) {
    shader << "  out.secondaryColor = float4(dxmt9_specularAccum, 0.0f);\n";
  }
}

void emitFfpTextureOpHelper(std::ostringstream& out) {
  out << "inline float4 dxmt9_apply_ffp_texture_op(uint op, float4 arg1, float4 arg2,\n";
  out << "                                          float4 arg0, float4 current,\n";
  out << "                                          float4 diffuse, float4 texture,\n";
  out << "                                          float4 tfactor) {\n";
  out << "  switch (op) {\n";
  out << "    case 12u: return mix(arg2, arg1, diffuse.a);\n";
  out << "    case 13u: return mix(arg2, arg1, texture.a);\n";
  out << "    case 14u: return mix(arg2, arg1, tfactor.a);\n";
  // D3DTOP_BUMPENVMAP (22) and D3DTOP_BUMPENVMAPLUMINANCE (23): the
  // perturbation + luminance scale has already been baked into the
  // stage's `texColor` (and therefore into `texture` here) by the
  // FFP shader generator's per-stage prelude. The texop itself just
  // returns the (already-perturbed, optionally-scaled) sample so
  // chained stages can read it via `current` or `D3DTA_TEXTURE`.
  out << "    case 22u: return texture;\n";
  out << "    case 23u: return texture;\n";
  // D3DTOP_MULTIPLYADD (25): saturate(arg1 * arg2 + arg0) — the third
  // explicit argument (D3DTSS_COLORARG0 / ALPHAARG0) is the accumulate
  // term, not the prior-stage `current`. Matches wined3d ffp_hlsl.c
  // WINED3D_TOP_MULTIPLY_ADD.
  out << "    case 25u: return saturate(arg1 * arg2 + arg0);\n";
  // D3DTOP_LERP (26): lerp(arg2, arg1, arg0) component-wise — arg0 is the
  // blend factor (arg0=0 -> arg2, arg0=1 -> arg1). Matches wined3d
  // ffp_hlsl.c WINED3D_TOP_LERP (`lerp(arg2, arg1, arg0)`); MSL `mix`
  // shares HLSL `lerp` operand order.
  out << "    case 26u: return mix(arg2, arg1, arg0);\n";
  out << "    default: return dxmt9_apply_texture_op(op, arg1, arg2, current);\n";
  out << "  }\n";
  out << "}\n";
}

}  // namespace

std::optional<FixedFunctionVertexLayout> decodeFixedFunctionVertexLayout(const VertexDeclSnapshot& decl) {
  FixedFunctionVertexLayout layout;
  if (!decl.elements.empty()) {
    u32 computedStride = 0;
    for (const auto& element : decl.elements) {
      if (element.stream != 0) {
        continue;
      }
      const u32 size = declTypeSize(element.type);
      if (size == 0) {
        continue;
      }
      computedStride = std::max(computedStride, static_cast<u32>(element.offset + size));
      if (element.usage == kD3DDeclUsagePositionT && element.usageIndex == 0 &&
          element.type == kD3DDeclTypeFloat4) {
        layout.valid = true;
        layout.preTransformed = true;
        layout.positionComponents = 4;
        layout.positionOffset = element.offset;
      } else if (element.usage == kD3DDeclUsagePosition && element.usageIndex == 0 &&
                 (element.type == kD3DDeclTypeFloat3 || element.type == kD3DDeclTypeFloat4)) {
        layout.valid = true;
        layout.preTransformed = false;
        layout.positionComponents = element.type == kD3DDeclTypeFloat4 ? 4u : 3u;
        layout.positionOffset = element.offset;
      } else if (element.usage == kD3DDeclUsageColor && element.usageIndex == 0 &&
                 element.type == kD3DDeclTypeD3DColor) {
        layout.hasDiffuse = true;
        layout.diffuseOffset = element.offset;
      } else if (element.usage == kD3DDeclUsageColor && element.usageIndex == 1 &&
                 element.type == kD3DDeclTypeD3DColor) {
        layout.hasSpecular = true;
        layout.specularOffset = element.offset;
      } else if (element.usage == kD3DDeclUsageNormal && element.usageIndex == 0 &&
                 (element.type == kD3DDeclTypeFloat3 || element.type == kD3DDeclTypeFloat4)) {
        layout.hasNormal = true;
        layout.normalOffset = element.offset;
      } else if (element.usage == kD3DDeclUsagePSize && element.usageIndex == 0 &&
                 element.type == kD3DDeclTypeFloat1) {
        layout.hasPointSize = true;
        layout.pointSizeOffset = element.offset;
      } else if (element.usage == kD3DDeclUsageBlendWeight && element.usageIndex == 0 &&
                 element.type <= kD3DDeclTypeFloat4) {
        layout.hasBlendWeight = true;
        layout.blendWeightOffset = element.offset;
        layout.blendWeightComponents = element.type + 1u;
      } else if (element.usage == kD3DDeclUsageBlendIndices && element.usageIndex == 0 &&
                 element.type == kD3DDeclTypeUByte4) {
        layout.hasBlendIndices = true;
        layout.blendIndicesOffset = element.offset;
      } else if (element.usage == kD3DDeclUsageTexcoord && element.usageIndex < kMaxTextureStages) {
        const u32 components = floatDeclTypeComponents(element.type);
        if (components == 0) {
          continue;
        }
        layout.hasTexcoord[element.usageIndex] = true;
        layout.texcoordComponents[element.usageIndex] = components;
        layout.texcoordOffset[element.usageIndex] = element.offset;
      }
    }
    layout.stride = decl.streams[0].stride ? decl.streams[0].stride : computedStride;
    if (layout.valid) {
      layout.hash = hashFixedFunctionLayout(layout);
      return layout;
    }
    return std::nullopt;
  }

  const u32 fvf = decl.fvf;
  const u32 position = fvf & kFvfPositionMask;
  if (position != kFvfXyzrhw && position != kFvfXyz && position != kFvfXyzw &&
      position != kFvfXyzB1 && position != kFvfXyzB2 &&
      position != kFvfXyzB3 && position != kFvfXyzB4 &&
      position != kFvfXyzB5) {
    return std::nullopt;
  }

  layout.valid = true;
  layout.preTransformed = position == kFvfXyzrhw;
  // D3DFVF_XYZW (Wine `test_ffp_w`, visual.c:28095) declares a 4-component
  // input but is NOT pre-transformed; the W is forced to 1.0 in the
  // transformed FFP VS body. Keep this branch separate from `preTransformed`
  // so the lighting / camera-space texgen still runs.
  const bool xyzw = (position == kFvfXyzw);
  layout.positionComponents = (layout.preTransformed || xyzw) ? 4u : 3u;
  u32 offset = 0;
  layout.positionOffset = offset;
  offset += (layout.preTransformed || xyzw) ? 16u : 12u;
  if (!layout.preTransformed && !xyzw && position > kFvfXyz) {
    layout.hasBlendWeight = true;
    layout.blendWeightOffset = offset;
    layout.blendWeightComponents = std::min<u32>((position - kFvfXyzrhw) / 2u, 4u);
    offset += layout.blendWeightComponents * 4u;
  }

  if ((fvf & kFvfNormal) != 0) {
    layout.hasNormal = true;
    layout.normalOffset = offset;
    offset += 12u;
  }

  if ((fvf & kFvfPSize) != 0) {
    layout.hasPointSize = true;
    layout.pointSizeOffset = offset;
    offset += 4u;
  }

  if ((fvf & kFvfDiffuse) != 0) {
    layout.hasDiffuse = true;
    layout.diffuseOffset = offset;
    offset += 4;
  }
  if ((fvf & kFvfSpecular) != 0) {
    layout.hasSpecular = true;
    layout.specularOffset = offset;
    offset += 4;
  }

  const u32 texCount = (fvf & kFvfTexCountMask) >> kFvfTexCountShift;
  if (texCount > 0) {
    for (u32 i = 0; i < std::min<u32>(texCount, kMaxTextureStages); ++i) {
      const u32 components = fvfTexcoordSize(fvf, i);
      if (components >= 1u) {
        layout.hasTexcoord[i] = true;
        layout.texcoordComponents[i] = components;
        layout.texcoordOffset[i] = offset;
      }
      offset += components * 4u;
    }
  } else {
    for (u32 i = 0; i < texCount; ++i) {
      offset += fvfTexcoordSize(fvf, i) * 4u;
    }
  }
  for (u32 i = std::min<u32>(texCount, kMaxTextureStages); i < texCount; ++i) {
    offset += fvfTexcoordSize(fvf, i) * 4u;
  }

  layout.stride = decl.streams[0].stride ? decl.streams[0].stride : offset;
  layout.hash = hashFixedFunctionLayout(layout);
  return layout;
}

std::string makeFfpVertexSource(const FfpVertexKey& key,
                                const drawshader::ShaderSourceContext& context) {
  std::ostringstream out;
  const bool argbufHybrid = context.argbufHybridMode;
  const auto layout = decodeFixedFunctionVertexLayout(context.vertexDecl);
  constexpr u32 kTciIndexMask = 0x0000ffffu;
  constexpr u32 kTciGenMask = 0xffff0000u;
  constexpr u32 kTciCameraSpaceNormal = 0x00010000u;
  constexpr u32 kTciCameraSpacePosition = 0x00020000u;
  constexpr u32 kTciCameraSpaceReflection = 0x00030000u;
  constexpr u32 kTciSphereMap = 0x00040000u;
  const auto emitStageTexcoords = [&](std::ostringstream& shader) {
    for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
      if (!shaders::vsoutEmitTexcoord(context.vsOutLayout, stage)) continue;
      const u32 texCoordIndex = key.texCoordGen[stage] & kTciIndexMask;
      const u32 texCoordGen = key.texCoordGen[stage] & kTciGenMask;
      shader << "  float4 dxmt9_texcoord" << stage << " = float4(0.0f, 0.0f, 1.0f, 1.0f);\n";
      if (layout && texCoordIndex < layout->hasTexcoord.size() && layout->hasTexcoord[texCoordIndex]) {
        shader << "  dxmt9_texcoord" << stage << " = "
               << texcoordLoadExpression(layout->texcoordOffset[texCoordIndex],
                                          layout->texcoordComponents[texCoordIndex])
               << ";\n";
      }
      if (texCoordGen == kTciCameraSpacePosition && !(layout && layout->preTransformed)) {
        shader << "  dxmt9_texcoord" << stage << " = float4(dxmt9_cameraPosition.xyz, 1.0f);\n";
      } else if (texCoordGen == kTciCameraSpaceNormal && !(layout && layout->preTransformed)) {
        shader << "  dxmt9_texcoord" << stage << " = float4(dxmt9_cameraNormal, 1.0f);\n";
      } else if (texCoordGen == kTciCameraSpaceReflection && !(layout && layout->preTransformed)) {
        shader << "  float3 dxmt9_eye" << stage << " = normalize(-dxmt9_cameraPosition.xyz);\n";
        shader << "  if (all(fabs(dxmt9_cameraPosition.xyz) < float3(1.0e-8f))) "
               << "dxmt9_eye" << stage << " = float3(0.0f, 0.0f, 1.0f);\n";
        shader << "  dxmt9_texcoord" << stage << " = float4(reflect(-dxmt9_eye" << stage
               << ", dxmt9_cameraUnitNormal), 1.0f);\n";
      } else if (texCoordGen == kTciSphereMap && !(layout && layout->preTransformed)) {
        shader << "  float3 dxmt9_eye" << stage << " = normalize(-dxmt9_cameraPosition.xyz);\n";
        shader << "  if (all(fabs(dxmt9_cameraPosition.xyz) < float3(1.0e-8f))) "
               << "dxmt9_eye" << stage << " = float3(0.0f, 0.0f, 1.0f);\n";
        shader << "  float3 dxmt9_reflect" << stage << " = reflect(-dxmt9_eye" << stage
               << ", dxmt9_cameraUnitNormal);\n";
        shader << "  float dxmt9_sphereM" << stage
               << " = 2.0f * sqrt(dot(dxmt9_reflect" << stage << ".xy, dxmt9_reflect" << stage
               << ".xy) + (dxmt9_reflect" << stage << ".z + 1.0f) * "
               << "(dxmt9_reflect" << stage << ".z + 1.0f));\n";
        shader << "  float2 dxmt9_sphereUv" << stage << " = dxmt9_sphereM" << stage
               << " > 1.0e-8f ? float2(dxmt9_reflect" << stage
               << ".x / dxmt9_sphereM" << stage << " + 0.5f, -dxmt9_reflect" << stage
               << ".y / dxmt9_sphereM" << stage << " + 0.5f) : float2(0.5f, 0.5f);\n";
        shader << "  dxmt9_texcoord" << stage << " = float4(dxmt9_sphereUv" << stage
               << ", dxmt9_reflect" << stage << ".z, 1.0f);\n";
      }
      shader << "  out.texcoord" << stage << " = float4(dxmt9_apply_texture_transform(dxmt9_texcoord" << stage
             << ", ffpVs, " << stage << "u, " << key.texTransformFlags[stage]
             << "u), dxmt9_texcoord" << stage << ".zw);\n";
    }
  };
  shaders::ShaderPreludeOptions preludeOptions{};
  preludeOptions.withClipDistances = key.clipPlaneMask != 0;
  preludeOptions.vsOutLayout = context.vsOutLayout;
  if (argbufHybrid) {
    out << shaders::makeShaderPreludeArgbufHybrid(preludeOptions);
  } else {
    out << shaders::makeShaderPrelude(preludeOptions);
  }
  // R-BACK-12.22..12.26 MSL routing — when argbufHybrid is set the entry
  // point takes a single argument buffer at slot 30 instead of dedicated
  // slots 0/3. Vertex stream stays at slot 1 and DrawVolatile at slot 5
  // (design.md §11.4). Body code re-aliases `vsConsts`/`ffpVs` references
  // off the argbuf so the existing reads (e.g. `ffpVs.halfPixelFixup`,
  // `ffpVs.ffpWorldViewProj[0]`, helper calls taking
  // `constant FfpVsConsts&`) compile unchanged.
  const auto emitVertexSig = [&](bool withStream) {
    if (argbufHybrid) {
      out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], "
             "constant ArgbufLayout& abuf [[buffer("
          << shaders::kArgbufHybridBindSlot << ")]], ";
      if (withStream) {
        out << "device const uchar* stream0 [[buffer(1)]], ";
      }
      out << "constant DrawVolatile& drawVolatile [[buffer(5)]]) {\n";
      out << "  constant VsConsts& vsConsts = *abuf.vsConsts;\n";
      out << "  constant FfpVsConsts& ffpVs = *abuf.ffpVs;\n";
    } else {
      out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], "
             "constant VsConsts& vsConsts [[buffer(0)]], ";
      if (withStream) {
        out << "device const uchar* stream0 [[buffer(1)]], ";
      }
      out << "constant FfpVsConsts& ffpVs [[buffer(3)]], "
             "constant DrawVolatile& drawVolatile [[buffer(5)]]) {\n";
    }
  };
  if (layout && layout->preTransformed) {
    emitVertexSig(/*withStream=*/true);
    out << "  (void)vsConsts;\n";
    out << "  VSOut out;\n";
    out << "  const uint stride = drawVolatile.vertexStreamStride != 0u ? drawVolatile.vertexStreamStride : "
        << layout->stride << "u;\n";
    out << "  const int vertexIndex = max(0, int(vid) + drawVolatile.vertexBaseIndex);\n";
    out << "  const uint base = drawVolatile.vertexStreamOffset + uint(vertexIndex) * stride;\n";
    out << "  float4 inPosition = dxmt9_load_f32x4(stream0, base + " << layout->positionOffset << "u);\n";
    out << "  float clipW = fabs(inPosition.w) > 1.0e-8f ? (1.0f / inPosition.w) : 1.0f;\n";
    out << "  float2 viewportSize = max(ffpVs.viewportSize, float2(1.0f));\n";
    out << "  float2 ndc = float2(((inPosition.x - ffpVs.viewportOrigin.x) / viewportSize.x) * 2.0f - 1.0f,\n";
    out << "                     1.0f - ((inPosition.y - ffpVs.viewportOrigin.y) / viewportSize.y) * 2.0f);\n";
    out << "  out.position = float4(ndc * clipW, inPosition.z * clipW, clipW);\n";
    if (shaders::vsoutEmitColor(context.vsOutLayout) && layout->hasDiffuse) {
      out << "  out.color = dxmt9_load_d3dcolor(stream0, base + " << layout->diffuseOffset << "u);\n";
    } else if (shaders::vsoutEmitColor(context.vsOutLayout)) {
      out << "  out.color = float4(1.0);\n";
    }
    if (shaders::vsoutEmitSecondaryColor(context.vsOutLayout) && layout->hasSpecular) {
      out << "  out.secondaryColor = dxmt9_load_d3dcolor(stream0, base + "
          << layout->specularOffset << "u);\n";
    } else if (shaders::vsoutEmitSecondaryColor(context.vsOutLayout)) {
      out << "  out.secondaryColor = float4(0.0);\n";
    }
    out << "  float3 dxmt9_lightingNormal = float3(0.0f, 0.0f, 1.0f);\n";
    // Point/Spot lighting (D3D9 §B.5) references dxmt9_cameraPosition;
    // pre-transformed vertices have no transformable camera-space
    // position, so we emit a zero stand-in. D3D9 apps that ship XYZRHW
    // geometry typically disable lighting, but the body must still
    // compile when key.lightingEnabled is set.
    out << "  float4 dxmt9_cameraPosition = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
    emitStageTexcoords(out);
    if (shaders::vsoutEmitFogFactor(context.vsOutLayout)) out << "  out.fogFactor = 1.0;\n";
    if (shaders::vsoutEmitPointSize(context.vsOutLayout)) {
      if (layout->hasPointSize) {
        out << "  float dxmt9_pointSize = dxmt9_load_f32(stream0, base + "
            << layout->pointSizeOffset << "u);\n";
      } else {
        out << "  float dxmt9_pointSize = ffpVs.pointSize;\n";
      }
      // Pre-transformed verts have no view-space; distance attenuation is
      // a no-op in this branch even when POINTSCALEENABLE is set. The
      // clamp by [POINTSIZE_MIN, POINTSIZE_MAX] still applies.
      out << "  out.pointSize = clamp(dxmt9_pointSize, ffpVs.pointSizeMin, ffpVs.pointSizeMax);\n";
    }
  } else if (layout) {
    emitVertexSig(/*withStream=*/true);
    out << "  (void)vsConsts;\n";
    out << "  VSOut out;\n";
    out << "  const uint stride = drawVolatile.vertexStreamStride != 0u ? drawVolatile.vertexStreamStride : "
        << layout->stride << "u;\n";
    out << "  const int vertexIndex = max(0, int(vid) + drawVolatile.vertexBaseIndex);\n";
    out << "  const uint base = drawVolatile.vertexStreamOffset + uint(vertexIndex) * stride;\n";
    if (layout->positionComponents == 4) {
      out << "  float4 inPosition = dxmt9_load_f32x4(stream0, base + " << layout->positionOffset << "u);\n";
      // Wine `test_ffp_w` (visual.c:28095) proves D3DFVF_XYZW / FLOAT4
      // POSITION inputs feed the FFP transform with W forced to 1.0, not
      // the buffer value. The XYZ stay verbatim, only the homogeneous W is
      // overridden before the WorldViewProjection multiply. This path is
      // never taken for pre-transformed POSITIONT — that hits the
      // `preTransformed` branch above.
      out << "  inPosition.w = 1.0f;\n";
    } else {
      out << "  float4 inPosition = float4(dxmt9_load_f32x3(stream0, base + " << layout->positionOffset
          << "u), 1.0f);\n";
    }
    if (layout->hasNormal) {
      out << "  float3 inNormal = dxmt9_load_f32x3(stream0, base + " << layout->normalOffset << "u);\n";
    } else {
      out << "  float3 inNormal = float3(0.0f, 0.0f, 1.0f);\n";
    }
    out << "  float inNormalLength = length(inNormal);\n";
    out << "  float3 unitNormal = inNormalLength > 1.0e-8f ? inNormal / inNormalLength : float3(0.0f, 0.0f, 1.0f);\n";
    if (key.normalizeNormals) {
      out << "  inNormal = unitNormal;\n";
    }
    out << "  float4 dxmt9_cameraPosition;\n";
    out << "  dxmt9_cameraPosition.x = dot(float4(ffpVs.ffpWorldView[0].x, ffpVs.ffpWorldView[1].x,\n";
    out << "                                      ffpVs.ffpWorldView[2].x, ffpVs.ffpWorldView[3].x), inPosition);\n";
    out << "  dxmt9_cameraPosition.y = dot(float4(ffpVs.ffpWorldView[0].y, ffpVs.ffpWorldView[1].y,\n";
    out << "                                      ffpVs.ffpWorldView[2].y, ffpVs.ffpWorldView[3].y), inPosition);\n";
    out << "  dxmt9_cameraPosition.z = dot(float4(ffpVs.ffpWorldView[0].z, ffpVs.ffpWorldView[1].z,\n";
    out << "                                      ffpVs.ffpWorldView[2].z, ffpVs.ffpWorldView[3].z), inPosition);\n";
    out << "  dxmt9_cameraPosition.w = dot(float4(ffpVs.ffpWorldView[0].w, ffpVs.ffpWorldView[1].w,\n";
    out << "                                      ffpVs.ffpWorldView[2].w, ffpVs.ffpWorldView[3].w), inPosition);\n";
    out << "  float3 dxmt9_cameraNormal = float3(\n";
    out << "      dot(float3(ffpVs.ffpNormalMatrix[0].x, ffpVs.ffpNormalMatrix[1].x, ffpVs.ffpNormalMatrix[2].x), inNormal),\n";
    out << "      dot(float3(ffpVs.ffpNormalMatrix[0].y, ffpVs.ffpNormalMatrix[1].y, ffpVs.ffpNormalMatrix[2].y), inNormal),\n";
    out << "      dot(float3(ffpVs.ffpNormalMatrix[0].z, ffpVs.ffpNormalMatrix[1].z, ffpVs.ffpNormalMatrix[2].z), inNormal));\n";
    out << "  float dxmt9_cameraNormalLength = length(dxmt9_cameraNormal);\n";
    out << "  float3 dxmt9_cameraUnitNormal = dxmt9_cameraNormalLength > 1.0e-8f ? dxmt9_cameraNormal / dxmt9_cameraNormalLength : float3(0.0f, 0.0f, 1.0f);\n";
    const bool emitVertexBlend =
        key.vertexBlend > 0 && key.vertexBlend <= 3 && layout->hasBlendWeight;
    out << "  float4 clip;\n";
    out << "  float4 blendedClip = float4(0.0f);\n";
    out << "  bool identityWvp = all(ffpVs.ffpWorldViewProj[0] == float4(1.0, 0.0, 0.0, 0.0)) &&\n";
    out << "                     all(ffpVs.ffpWorldViewProj[1] == float4(0.0, 1.0, 0.0, 0.0)) &&\n";
    out << "                     all(ffpVs.ffpWorldViewProj[2] == float4(0.0, 0.0, 1.0, 0.0)) &&\n";
    out << "                     all(ffpVs.ffpWorldViewProj[3] == float4(0.0, 0.0, 0.0, 1.0));\n";
    if (emitVertexBlend) {
      out << "  float4 blendWeights = float4(0.0f);\n";
      out << "  uint4 blendMatrixIndices = uint4(0u, 1u, 2u, 3u);\n";
      if (layout->blendWeightComponents == 1) {
        out << "  blendWeights.x = dxmt9_load_f32(stream0, base + " << layout->blendWeightOffset << "u);\n";
      } else if (layout->blendWeightComponents == 2) {
        out << "  blendWeights.xy = dxmt9_load_f32x2(stream0, base + " << layout->blendWeightOffset << "u);\n";
      } else if (layout->blendWeightComponents == 3) {
        out << "  blendWeights.xyz = dxmt9_load_f32x3(stream0, base + " << layout->blendWeightOffset << "u);\n";
      } else {
        out << "  blendWeights = dxmt9_load_f32x4(stream0, base + " << layout->blendWeightOffset << "u);\n";
      }
      if (key.indexedVertexBlend && layout->hasBlendIndices) {
        out << "  float4 rawBlendIndices = dxmt9_load_u8x4(stream0, base + "
            << layout->blendIndicesOffset << "u);\n";
        out << "  blendMatrixIndices = uint4(uint(rawBlendIndices.x), uint(rawBlendIndices.y),\n";
        out << "                             uint(rawBlendIndices.z), uint(rawBlendIndices.w));\n";
      }
      out << "  blendWeights = clamp(blendWeights, float4(0.0f), float4(1.0f));\n";
      out << "  float4 matrixWeight = float4(0.0f);\n";
      for (u32 i = 0; i < key.vertexBlend && i < 3u; ++i) {
        out << "  matrixWeight[" << i << "] = blendWeights[" << i << "];\n";
      }
      out << "  matrixWeight[" << std::min<u32>(key.vertexBlend, 3u)
          << "] = max(0.0f, 1.0f - (matrixWeight.x + matrixWeight.y + matrixWeight.z));\n";
      for (u32 matrix = 0; matrix <= std::min<u32>(key.vertexBlend, 3u); ++matrix) {
        if (key.indexedVertexBlend && layout->hasBlendIndices) {
          out << "  uint blendMatrix" << matrix << " = min(blendMatrixIndices[" << matrix << "], 3u);\n";
        } else {
          out << "  constexpr uint blendMatrix" << matrix << " = " << matrix << "u;\n";
        }
        out << "  float4 blendClip" << matrix << ";\n";
        out << "  blendClip" << matrix << ".x = dot(float4(ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][0].x, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][1].x, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][2].x, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix << "][3].x), inPosition);\n";
        out << "  blendClip" << matrix << ".y = dot(float4(ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][0].y, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][1].y, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][2].y, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix << "][3].y), inPosition);\n";
        out << "  blendClip" << matrix << ".z = dot(float4(ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][0].z, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][1].z, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][2].z, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix << "][3].z), inPosition);\n";
        out << "  blendClip" << matrix << ".w = dot(float4(ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][0].w, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][1].w, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix
            << "][2].w, ffpVs.ffpBlendWorldViewProj[blendMatrix" << matrix << "][3].w), inPosition);\n";
        out << "  blendedClip += blendClip" << matrix << " * matrixWeight[" << matrix << "];\n";
      }
    }
    out << "  bool useVertexBlend = " << (emitVertexBlend ? "true" : "false") << ";\n";
    out << "  bool pixelSpacePosition = !useVertexBlend && identityWvp && "
           "(fabs(inPosition.x) > 2.0f || fabs(inPosition.y) > 2.0f);\n";
    out << "  if (pixelSpacePosition) {\n";
    out << "    float2 viewportSize = max(ffpVs.viewportSize, float2(1.0f));\n";
    out << "    float2 ndc = float2(((inPosition.x - ffpVs.viewportOrigin.x) / viewportSize.x) * 2.0f - 1.0f,\n";
    out << "                       1.0f - ((inPosition.y - ffpVs.viewportOrigin.y) / viewportSize.y) * 2.0f);\n";
    out << "    clip = float4(ndc, inPosition.z, 1.0f);\n";
    out << "  } else if (useVertexBlend) {\n";
    out << "    clip = blendedClip;\n";
    out << "  } else {\n";
    out << "    clip.x = dot(float4(ffpVs.ffpWorldViewProj[0].x, ffpVs.ffpWorldViewProj[1].x,\n";
    out << "                           ffpVs.ffpWorldViewProj[2].x, ffpVs.ffpWorldViewProj[3].x), inPosition);\n";
    out << "    clip.y = dot(float4(ffpVs.ffpWorldViewProj[0].y, ffpVs.ffpWorldViewProj[1].y,\n";
    out << "                           ffpVs.ffpWorldViewProj[2].y, ffpVs.ffpWorldViewProj[3].y), inPosition);\n";
    out << "    clip.z = dot(float4(ffpVs.ffpWorldViewProj[0].z, ffpVs.ffpWorldViewProj[1].z,\n";
    out << "                           ffpVs.ffpWorldViewProj[2].z, ffpVs.ffpWorldViewProj[3].z), inPosition);\n";
    out << "    clip.w = dot(float4(ffpVs.ffpWorldViewProj[0].w, ffpVs.ffpWorldViewProj[1].w,\n";
    out << "                           ffpVs.ffpWorldViewProj[2].w, ffpVs.ffpWorldViewProj[3].w), inPosition);\n";
    out << "  }\n";
    out << "  out.position = clip;\n";
    out << "  out.position.xy += ffpVs.halfPixelFixup * out.position.w;\n";
    if (shaders::vsoutEmitColor(context.vsOutLayout) && layout->hasDiffuse) {
      out << "  out.color = dxmt9_load_d3dcolor(stream0, base + " << layout->diffuseOffset << "u);\n";
    } else if (shaders::vsoutEmitColor(context.vsOutLayout)) {
      out << "  out.color = float4(1.0);\n";
    }
    if (shaders::vsoutEmitSecondaryColor(context.vsOutLayout) && layout->hasSpecular) {
      out << "  out.secondaryColor = dxmt9_load_d3dcolor(stream0, base + "
          << layout->specularOffset << "u);\n";
    } else if (shaders::vsoutEmitSecondaryColor(context.vsOutLayout)) {
      out << "  out.secondaryColor = float4(0.0);\n";
    }
    out << "  float3 dxmt9_lightingNormal = dxmt9_cameraUnitNormal;\n";
    emitStageTexcoords(out);
    if (shaders::vsoutEmitFogFactor(context.vsOutLayout)) {
      out << "  float dxmt9_fogDepth = ffpVs.rangeFog != 0u ? length(dxmt9_cameraPosition.xyz) : fabs(dxmt9_cameraPosition.z);\n";
      out << "  out.fogFactor = dxmt9_compute_fog_factor(ffpVs.fogMode, dxmt9_fogDepth,\n";
      out << "                                           ffpVs.fogStart, ffpVs.fogEnd,\n";
      out << "                                           ffpVs.fogDensity);\n";
    }
    if (shaders::vsoutEmitPointSize(context.vsOutLayout)) {
      if (layout->hasPointSize) {
        out << "  float dxmt9_pointSize = dxmt9_load_f32(stream0, base + "
            << layout->pointSizeOffset << "u);\n";
      } else {
        out << "  float dxmt9_pointSize = ffpVs.pointSize;\n";
      }
      if (key.pointScaleEnable) {
        // size = pointSize * rsqrt(A + B*d + C*d^2), d = length(view-space pos)
        out << "  float dxmt9_pointDist = length(dxmt9_cameraPosition.xyz);\n";
        out << "  float dxmt9_pointAtten = ffpVs.pointScaleA + ffpVs.pointScaleB * dxmt9_pointDist + ffpVs.pointScaleC * dxmt9_pointDist * dxmt9_pointDist;\n";
        out << "  dxmt9_pointSize = dxmt9_pointSize * rsqrt(max(dxmt9_pointAtten, 1.0e-6f));\n";
      }
      out << "  out.pointSize = clamp(dxmt9_pointSize, ffpVs.pointSizeMin, ffpVs.pointSizeMax);\n";
    }
  } else {
    emitVertexSig(/*withStream=*/false);
    out << "  (void)vsConsts; (void)drawVolatile;\n";
    out << "  VSOut out;\n";
    out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
    out << "  out.position = float4(p[vid % 3], 0.0, 1.0);\n";
    out << "  out.position.xy += ffpVs.halfPixelFixup * out.position.w;\n";
    if (shaders::vsoutEmitColor(context.vsOutLayout)) {
      out << "  out.color = float4(1.0);\n";
    }
    if (shaders::vsoutEmitSecondaryColor(context.vsOutLayout)) {
      out << "  out.secondaryColor = float4(0.0);\n";
    }
    out << "  float3 dxmt9_lightingNormal = float3(0.0f, 0.0f, 1.0f);\n";
    // Fallback (no vertex layout) path: emit a zero camera position so
    // Point/Spot lighting (D3D9 §B.5) compiles even when no real
    // geometry stream is bound.
    out << "  float4 dxmt9_cameraPosition = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
    if (shaders::vsoutEmitTexcoord(context.vsOutLayout, 0u)) {
      out << "  out.texcoord0 = float4(float2(vid & 1u, (vid >> 1u) & 1u), 0.0f, 1.0f);\n";
    }
    for (size_t i = 1; i < kMaxTextureStages; ++i) {
      if (!shaders::vsoutEmitTexcoord(context.vsOutLayout, i)) continue;
      if (shaders::vsoutEmitTexcoord(context.vsOutLayout, 0u)) {
        out << "  out.texcoord" << i << " = out.texcoord0;\n";
      } else {
        out << "  out.texcoord" << i << " = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
      }
    }
    if (shaders::vsoutEmitFogFactor(context.vsOutLayout)) out << "  out.fogFactor = 1.0;\n";
    if (shaders::vsoutEmitPointSize(context.vsOutLayout)) {
      out << "  out.pointSize = clamp(ffpVs.pointSize, ffpVs.pointSizeMin, ffpVs.pointSizeMax);\n";
    }
  }
  emitLightingBlock(out, key);
  if (key.clipPlaneMask != 0 || context.clipPlaneMask != 0) {
    // Apple Metal limits us to a single `[[clip_distance]]` per VS
    // output (see comment in `dxmt9_shader_sources.cpp`). Collapse the
    // up-to-six D3D9 clip planes into one slot by writing the minimum
    // of the enabled per-plane half-space tests: a fragment is clipped
    // iff ANY plane's dot is < 0, which is equivalent to `min(d_i) < 0`
    // for slots enabled in `ffpVs.clipPlaneMask`.
    out << "  float dxmt9_minClip = 1.0f;\n";
    out << "  for (uint i = 0u; i < 6u; ++i) {\n";
    out << "    if ((ffpVs.clipPlaneMask & (1u << i)) != 0u) {\n";
    out << "      const float d = dot(ffpVs.clipPlanes[i], out.position);\n";
    out << "      dxmt9_minClip = min(dxmt9_minClip, d);\n";
    out << "    }\n";
    out << "  }\n";
    out << "  out.clipDistance0 = dxmt9_minClip;\n";
  }
  out << "  return out;\n";
  out << "}\n";
  out << "// ffp vertex hash " << key.hash << "\n";
  return out.str();
}

std::string makeFfpPixelSource(const FfpPixelKey& key,
                               const drawshader::ShaderSourceContext& context) {
  std::ostringstream out;
  std::vector<size_t> sampledStages;
  sampledStages.reserve(kMaxTextureStages);
  bool hasEnabledStages = false;
  for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    const bool stageEnabled =
        key.stages[stage].colorOp != static_cast<u32>(TextureOp::Disable) ||
        key.stages[stage].alphaOp != static_cast<u32>(TextureOp::Disable);
    if (stageEnabled) {
      hasEnabledStages = true;
      if (context.textures[stage]) {
        sampledStages.push_back(stage);
      }
    }
  }
  const bool hasTextureParams = !sampledStages.empty();
  // D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3) PSO-variant gate: only when the variant
  // key flagged a non-zero sampler LOD bias do we declare the slot-4
  // SamplerLodBias uniform and thread bias() through the stage samples. When
  // clear (the common case) the emitted MSL is byte-identical to the
  // pre-MIPMAPLODBIAS plain-sample form, and the encoder skips the slot-4 bind
  // on the same predicate. Only meaningful on a textured fragment.
  const bool emitLodBias = context.samplerLodBias && hasTextureParams;
  // Per-stage bias() argument appended to a sample() call, or empty when the
  // gate is clear. Keeping this in one place keeps every sample site in sync
  // with the slot-4 param emit decision.
  const auto biasArg = [&](size_t stage) -> std::string {
    if (!emitLodBias) {
      return std::string();
    }
    return ", bias(samplerLodBias.bias[" + std::to_string(stage) + "])";
  };
  const bool debugFfpUv = [] {
    const char* env = std::getenv("DXMT_DEBUG_FFP_UV");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  const bool debugFfpTexture = [] {
    const char* env = std::getenv("DXMT_DEBUG_FFP_TEXTURE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  const bool debugFfpAlpha = [] {
    const char* env = std::getenv("DXMT_DEBUG_FFP_ALPHA");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  const bool argbufHybrid = context.argbufHybridMode;
  // R-BACK-12.22..12.26 (resource-array sub-mode) — only meaningful when
  // Stage 2 hybrid is also on AND the fragment actually samples a texture.
  // When set, the texture/sampler resources ride the slot-30 argbuf arrays
  // and the direct [[texture(N)]] / [[sampler(N)]] params are dropped; an
  // alias block re-binds the `tex<stage>` / `samp<stage>` names off the
  // argbuf so every downstream sample site is byte-identical.
  const bool argbufResourceArray =
      argbufHybrid && context.argbufResourceArray && hasTextureParams;
  shaders::ShaderPreludeOptions preludeOptions{};
  preludeOptions.withClipDistances = context.clipPlaneMask != 0;
  preludeOptions.vsOutLayout = context.vsOutLayout;
  if (argbufResourceArray) {
    out << shaders::makeShaderPreludeArgbufResourceArray(preludeOptions);
  } else if (argbufHybrid) {
    out << shaders::makeShaderPreludeArgbufHybrid(preludeOptions);
  } else {
    out << shaders::makeShaderPrelude(preludeOptions);
  }
  emitFfpTextureOpHelper(out);
  // D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3): per-sampler mip LOD bias is applied
  // at sample time via `texture.sample(..., bias(b))`; Metal samplers carry no
  // LOD-bias field. The bias rides a dedicated fragment uniform bound at slot 4
  // (host struct dxmt9::state::SamplerLodBias). Declared inline here — byte
  // identical float[8] — so the shared prelude stays untouched. PSO-variant
  // gated: only emitted when some active sampler carries a non-zero LOD bias.
  if (emitLodBias) {
    out << "struct SamplerLodBias {\n";
    out << "  float bias[" << kMaxTextureStages << "];\n";
    out << "};\n";
  }
  if (context.x8AlphaOneTextureMask != 0u) {
    out << "inline float4 dxmt9_x8_alpha_one(float4 color) {\n";
    out << "  color.a = 1.0f;\n";
    out << "  return color;\n";
    out << "}\n";
  }
  // R-BACK-12.22..12.26 MSL routing — when argbufHybrid is set, the
  // fragment entry point takes a single argument buffer at slot 30 for
  // `psConsts`/`ffpPs`. Texture/sampler parameters remain direct so
  // texture-bound Stage 2 draws share the proven Stage 1 resource lane.
  // POINTSPRITEENABLE: per D3D9, the FFP point primitive emits an
  // auto-generated [0,1]² texcoord at every fragment. Metal exposes the
  // same value via the [[point_coord]] attribute. We bind it as a
  // separate FS input only when the variant key calls for it -- adding
  // an unused [[point_coord]] would cost a function-constant slot
  // without value.
  const auto emitPointCoordParam = [&]() {
    if (key.pointSpriteEnable) {
      out << ", float2 dxmt9_pointCoord [[point_coord]]";
    }
  };
  if (hasTextureParams) {
    if (argbufResourceArray) {
      // R-BACK-12.22..12.26 (resource-array sub-mode) — texture/sampler
      // resources ride the slot-30 argbuf arrays; the entry point declares
      // NO [[texture(N)]] / [[sampler(N)]] params. The alias block below
      // re-binds `tex<stage>` / `samp<stage>` off the argbuf so the FFP
      // sample sites (which reference tex<stage>.sample(samp<stage>, ...))
      // are byte-identical to the direct lane.
      out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], "
             "constant ArgbufLayout& abuf [[buffer("
          << shaders::kArgbufHybridBindSlot << ")]]";
      emitPointCoordParam();
      if (emitLodBias) {
        out << ", constant SamplerLodBias& samplerLodBias [[buffer(4)]]";
      }
      out << ") {\n";
      out << "  constant PsConsts& psConsts = *abuf.psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf.ffpPs;\n";
      // FFP textures are always texture2d<float>; alias each active stage.
      for (size_t i = 0; i < sampledStages.size(); ++i) {
        const size_t stage = sampledStages[i];
        out << "  texture2d<float> tex" << stage << " = abuf.textures["
            << stage << "];\n";
        out << "  sampler samp" << stage << " = abuf.samplers[" << stage
            << "];\n";
      }
    } else if (argbufHybrid) {
      out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], "
             "constant ArgbufLayout& abuf [[buffer("
          << shaders::kArgbufHybridBindSlot << ")]]";
      emitPointCoordParam();
      out << ", ";
      for (size_t i = 0; i < sampledStages.size(); ++i) {
        const size_t stage = sampledStages[i];
        if (i != 0) {
          out << ", ";
        }
        out << "texture2d<float> tex" << stage << " [[texture(" << stage
            << ")]], sampler samp" << stage << " [[sampler(" << stage << ")]]";
      }
      if (emitLodBias) {
        out << ", constant SamplerLodBias& samplerLodBias [[buffer(4)]]";
      }
      out << ") {\n";
      out << "  constant PsConsts& psConsts = *abuf.psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf.ffpPs;\n";
    } else {
      out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], "
             "constant PsConsts& psConsts [[buffer(0)]], "
             "constant FfpPsConsts& ffpPs [[buffer(3)]]";
      emitPointCoordParam();
      out << ", ";
      for (size_t i = 0; i < sampledStages.size(); ++i) {
        const size_t stage = sampledStages[i];
        if (i != 0) {
          out << ", ";
        }
        out << "texture2d<float> tex" << stage << " [[texture(" << stage << ")]], sampler samp" << stage
            << " [[sampler(" << stage << ")]]";
      }
      if (emitLodBias) {
        out << ", constant SamplerLodBias& samplerLodBias [[buffer(4)]]";
      }
      out << ") {\n";
    }
    out << "  (void)psConsts;\n";
  } else {
    if (argbufHybrid) {
      out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], "
             "constant ArgbufLayout& abuf [[buffer("
          << shaders::kArgbufHybridBindSlot << ")]]";
      emitPointCoordParam();
      out << ") {\n";
      out << "  constant PsConsts& psConsts = *abuf.psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf.ffpPs;\n";
    } else {
      out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], "
             "constant PsConsts& psConsts [[buffer(0)]], "
             "constant FfpPsConsts& ffpPs [[buffer(3)]]";
      emitPointCoordParam();
      out << ") {\n";
    }
    out << "  (void)psConsts;\n";
    if (key.pointSpriteEnable) {
      // Untextured sprite-enabled FS never reads point_coord; keep the
      // attribute on the entry-point signature for variant-key parity
      // with the textured form and suppress the unused-param warning.
      out << "  (void)dxmt9_pointCoord;\n";
    }
  }
  if (shaders::vsoutProbePositionOnlyEnabled() ||
      ::dxmt9::debug::forceFragmentShaderColor()) {
    out << "  return float4(1.0f, 0.0f, 1.0f, 1.0f);\n";
    out << "}\n";
    out << "// ffp pixel hash " << key.hash << "\n";
    return out.str();
  }
  out << "  float4 color = in.color;\n";
  out << "  float4 current = color;\n";
  out << "  float4 diffuse = in.color;\n";
  out << "  float4 specular = in.secondaryColor;\n";
  out << "  float4 tfactor = ffpPs.textureFactor;\n";
  out << "  float4 temp = float4(0.0);\n";
  // POINTSPRITEENABLE: every active stage's UV source becomes the
  // auto-generated [[point_coord]] regardless of TSS_TEXCOORD_INDEX.
  // Wine's `state_pointsprite` mirrors the same override at the fixed
  // function pixel pipeline.
  const auto sampleCoord = [&](u32 coordIndex) -> std::string {
    if (key.pointSpriteEnable) {
      return "float2(dxmt9_pointCoord)";
    }
    std::ostringstream s;
    s << "dxmt9_select_texcoord(in, " << coordIndex << "u).xy";
    return s.str();
  };
  const auto sampleExpr = [&](size_t stage, std::string coord) {
    std::string sample = "tex" + std::to_string(stage) + ".sample(samp" +
                         std::to_string(stage) + ", " + coord +
                         biasArg(stage) + ")";
    if ((context.x8AlphaOneTextureMask & (1u << stage)) != 0u) {
      sample = "dxmt9_x8_alpha_one(" + sample + ")";
    }
    return sample;
  };
  if (hasEnabledStages) {
    if (debugFfpUv) {
      if (key.pointSpriteEnable) {
        out << "  return float4(dxmt9_pointCoord.x, dxmt9_pointCoord.y, 0.0, 1.0);\n";
      } else {
        out << "  return float4(fract(in.texcoord0.x), fract(in.texcoord0.y), 0.0, 1.0);\n";
      }
      out << "}\n";
      out << "// ffp pixel hash " << key.hash << "\n";
      return out.str();
    }
    if (debugFfpTexture && hasTextureParams) {
      const size_t stage = sampledStages.front();
      // FFP texcoord routing: the FFP vertex shader has already resolved
      // TSS_TEXCOORDINDEX *and* applied the per-stage texture-transform
      // matrix into `out.texcoord<stage>` (see `makeFfpVertexSource`).
      // The fragment shader samples stage N at `in.texcoord<stage>` --
      // routing by `stages[stage].texCoordIndex` here would silently
      // bypass the matrix whenever TCI != stage (Wine `test_texcoordindex`
      // + a TEXTURE0 transform). conf-d3d9-intent-probe regression coverage:
      // `texcoord-index-matrix`.
      const u32 coordIndex = static_cast<u32>(stage);
      out << "  return " << sampleExpr(stage, sampleCoord(coordIndex)) << ";\n";
      out << "}\n";
      out << "// ffp pixel hash " << key.hash << "\n";
      return out.str();
    }
    if (debugFfpAlpha && hasTextureParams) {
      const size_t stage = sampledStages.front();
      // Same per-stage routing as the debugFfpTexture branch above:
      // the FFP VS already wrote the transformed coord into
      // `out.texcoord<stage>`.
      const u32 coordIndex = static_cast<u32>(stage);
      out << "  float alpha = " << sampleExpr(stage, sampleCoord(coordIndex)) << ".a;\n";
      out << "  return float4(alpha, alpha, alpha, 1.0);\n";
      out << "}\n";
      out << "// ffp pixel hash " << key.hash << "\n";
      return out.str();
    }
    for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
      const auto& stageKey = key.stages[stage];
      const bool stageEnabled =
          stageKey.colorOp != static_cast<u32>(TextureOp::Disable) ||
          stageKey.alphaOp != static_cast<u32>(TextureOp::Disable);
      if (!stageEnabled) {
        continue;
      }
      const bool hasTexture = context.textures[stage];
      // FFP texcoord routing: the FFP vertex shader has already resolved
      // TSS_TEXCOORDINDEX *and* applied the per-stage texture-transform
      // matrix into `out.texcoord<stage>`. The fragment shader therefore
      // samples stage N at `in.texcoord<stage>` -- routing by
      // `stageKey.texCoordIndex` here would silently bypass the matrix
      // whenever TCI != stage (e.g. Wine `test_texcoordindex` /
      // conf-d3d9-intent-probe `texcoord-index-matrix`: TCI=1 + TEXTURE0
      // transform on stage 0). The vertex shader also enforces "matrix
      // off => identity passthrough", so the FS read collapses to the
      // selected input slot when TTFF=DISABLE (the 14 non-matrix probe
      // cases that were already passing).
      const u32 coordIndex = static_cast<u32>(stage);
      // Wine fixed_function_bumpmap_test (D3DTOP_BUMPENVMAP = 22,
      // D3DTOP_BUMPENVMAPLUMINANCE = 23): when this stage's colorOp is
      // a bump-env op, the *current* stage's texture is sampled at
      // coords perturbed by the PREVIOUS stage's RG output via
      // BUMPENVMAT, then optionally scaled by the bump luminance
      // formula for op 23. The perturbed sample replaces the standard
      // texCoord lookup before any texop dispatch runs.
      const bool isBumpEnv = stageKey.colorOp == 22u;
      const bool isBumpEnvLum = stageKey.colorOp == 23u;
      if (hasTexture) {
        if ((isBumpEnv || isBumpEnvLum) && stage > 0u) {
          // BUMPENVMAP perturbs the stage's own texcoord by the prior
          // stage's RG output via BUMPENVMAT — independent of point-sprite
          // mode. With point sprites the base UV would normally come from
          // `[[point_coord]]`; bump-env is stage-specific and rarely
          // combined with sprites, so we keep the per-vertex texcoord
          // here. (Wine wined3d does the same.)
          const auto bumpStage = std::to_string(stage - 1u);
          const auto s = std::to_string(stage);
          out << "  float2 baseUV" << stage << " = dxmt9_select_texcoord(in, "
              << coordIndex << "u).xy;\n";
          out << "  float2 bumpDelta" << stage
              << " = float2(ffpPs.bumpEnvMat[" << s << "].x * (texColor"
              << bumpStage << ".r - 0.5f) + ffpPs.bumpEnvMat[" << s
              << "].z * (texColor" << bumpStage << ".g - 0.5f), ffpPs.bumpEnvMat["
              << s << "].y * (texColor" << bumpStage
              << ".r - 0.5f) + ffpPs.bumpEnvMat[" << s << "].w * (texColor"
              << bumpStage << ".g - 0.5f));\n";
          out << "  float4 texColor" << stage << " = "
              << sampleExpr(stage, std::string("baseUV") + std::to_string(stage) +
                                       " + bumpDelta" + std::to_string(stage))
              << ";\n";
          if (isBumpEnvLum) {
            out << "  float bumpLum" << stage
                << " = saturate(texColor" << bumpStage << ".b * ffpPs.bumpEnvLum["
                << s << "].x + ffpPs.bumpEnvLum[" << s << "].y);\n";
            out << "  texColor" << stage << ".rgb *= bumpLum" << stage << ";\n";
          }
        } else {
          out << "  float4 texColor" << stage << " = "
              << sampleExpr(stage, sampleCoord(coordIndex)) << ";\n";
        }
      } else {
        out << "  float4 texColor" << stage << " = float4(1.0f);\n";
      }
      out << "  float4 colorArg1_" << stage << " = dxmt9_select_texture_arg(" << stageKey.colorArg1
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp, "
          << "ffpPs.stageConstants[" << stage << "]);\n";
      out << "  float4 colorArg2_" << stage << " = dxmt9_select_texture_arg(" << stageKey.colorArg2
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp, "
          << "ffpPs.stageConstants[" << stage << "]);\n";
      // Third (triadic) argument — read by D3DTOP_MULTIPLYADD / D3DTOP_LERP.
      out << "  float4 colorArg0_" << stage << " = dxmt9_select_texture_arg(" << stageKey.colorArg0
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp, "
          << "ffpPs.stageConstants[" << stage << "]);\n";
      out << "  float4 stageResult" << stage << " = dxmt9_apply_ffp_texture_op(" << stageKey.colorOp
          << "u, colorArg1_" << stage << ", colorArg2_" << stage << ", colorArg0_" << stage
          << ", current, diffuse, texColor" << stage << ", tfactor);\n";
      out << "  float4 alphaArg1_" << stage << " = dxmt9_select_texture_arg(" << stageKey.alphaArg1
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp, "
          << "ffpPs.stageConstants[" << stage << "]);\n";
      out << "  float4 alphaArg2_" << stage << " = dxmt9_select_texture_arg(" << stageKey.alphaArg2
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp, "
          << "ffpPs.stageConstants[" << stage << "]);\n";
      out << "  float4 alphaArg0_" << stage << " = dxmt9_select_texture_arg(" << stageKey.alphaArg0
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp, "
          << "ffpPs.stageConstants[" << stage << "]);\n";
      out << "  stageResult" << stage << ".a = dxmt9_apply_ffp_texture_op(" << stageKey.alphaOp
          << "u, alphaArg1_" << stage << ", alphaArg2_" << stage << ", alphaArg0_" << stage
          << ", current, diffuse, texColor" << stage << ", tfactor).a;\n";
      out << "  current = stageResult" << stage << ";\n";
      if (stageKey.resultArg == 5u) {
        out << "  temp = stageResult" << stage << ";\n";
      }
    }
    out << "  color = current;\n";
  }
  // R-BACK-13.1 — when this is the tile-FFP base-colour variant, the fog
  // blend and the alpha-test discard are NOT emitted here; the tile kernel
  // (makeFfpTilePixelSource) applies them over the rasterized base colour in
  // the imageblock. Emitting them here too would double-apply fog and would
  // discard fragments the tile kernel is supposed to alpha-test (the tile
  // kernel reads back the imageblock color, so a discarded base fragment
  // would never reach it).
  const bool emitFogAlphaTest = !context.stripFogAlphaTestForTileBase;
  const bool emitAlphaTest = emitFogAlphaTest && !context.stripAlphaTestForDebug;
  if (emitAlphaTest && key.alphaTestEnable) {
    out << "  bool pass = true;\n";
    out << "  switch (ffpPs.alphaTestFunc) {\n";
    out << "    case 2u: pass = color.a < ffpPs.alphaRef; break;\n";
    out << "    case 3u: pass = color.a == ffpPs.alphaRef; break;\n";
    out << "    case 4u: pass = color.a <= ffpPs.alphaRef; break;\n";
    out << "    case 5u: pass = color.a > ffpPs.alphaRef; break;\n";
    out << "    case 6u: pass = color.a != ffpPs.alphaRef; break;\n";
    out << "    case 7u: pass = color.a >= ffpPs.alphaRef; break;\n";
    out << "    case 8u: pass = true; break;\n";
    out << "    default: pass = true; break;\n";
    out << "  }\n";
    out << "  if (!pass) { discard_fragment(); }\n";
  }
  if (emitFogAlphaTest && key.fogMode != FogMode::None) {
    out << "  float fogDepth = color.a;\n";
    out << "  color = dxmt9_apply_fog(color, ffpPs, fogDepth, in.fogFactor);\n";
  }
  out << "  return color;\n";
  out << "}\n";
  out << "// ffp pixel hash " << key.hash << "\n";
  return out.str();
}

bool tileFfpAttachmentAcceptsHalf(u32 pixelFormat) {
  // R-BACK-13.7: only 8-bpc unorm attachments accept `half4` imageblock
  // declarations because the attachment quantization already discards
  // precision below `half`. Wider formats keep `float4` to preserve
  // bit-identity with the portable path.
  switch (pixelFormat) {
    case static_cast<u32>(WMTPixelFormatRGBA8Unorm):
    case static_cast<u32>(WMTPixelFormatRGBA8Unorm_sRGB):
    case static_cast<u32>(WMTPixelFormatRGBA8Snorm):
    case static_cast<u32>(WMTPixelFormatBGRA8Unorm):
    case static_cast<u32>(WMTPixelFormatBGRA8Unorm_sRGB):
    case static_cast<u32>(WMTPixelFormatBGRX8Unorm):
    case static_cast<u32>(WMTPixelFormatBGRX8Unorm_sRGB):
      return true;
    default:
      return false;
  }
}

TileFfpEligibility classifyTileFfpEligibility(const FfpPixelKey& key,
                                              float alphaTestRefNormalized,
                                              bool alphaToCoverageEnabled) {
  // R-BACK-13.3 conformance boundary:
  //   - alpha-test reference outside [0.0, 1.0] -> precision fallback
  //   - non-linear fog at high z (Exp / Exp2) -> precision fallback
  //   - alpha-to-coverage with PS-emitted alpha-test -> unsupported_state
  if (key.alphaTestEnable) {
    if (!(alphaTestRefNormalized >= 0.0f && alphaTestRefNormalized <= 1.0f)) {
      return TileFfpEligibility::IneligiblePrecision;
    }
    if (alphaToCoverageEnabled) {
      // Programmable-blend interaction with PS-emitted alpha-to-coverage
      // cannot be replicated bit-identical at the tile stage.
      return TileFfpEligibility::IneligibleUnsupportedState;
    }
  }
  if (key.fogMode == FogMode::Exp || key.fogMode == FogMode::Exp2) {
    return TileFfpEligibility::IneligiblePrecision;
  }
  return TileFfpEligibility::Eligible;
}

std::string makeFfpTilePixelSource(const FfpPixelKey& key,
                                    const drawshader::ShaderSourceContext& context,
                                    u32 colorAttachmentPixelFormat) {
  std::ostringstream out;
  // Tile pipelines never carry the VS->PS interpolation prelude. Emit a
  // minimal MSL translation unit with the FfpPsConsts structure mirrored
  // from the portable path so binding indices line up.
  const bool useHalf = tileFfpAttachmentAcceptsHalf(colorAttachmentPixelFormat);
  const char* tileColorType = useHalf ? "half4" : "float4";
  const bool argbufHybrid = context.argbufHybridMode;
  out << "#include <metal_stdlib>\n";
  out << "using namespace metal;\n";
  // Mirror the portable FFP PS consts struct so the same per-frequency
  // UBO layout flows through the tile binding (FfpPsConsts buffer slot).
  out << "struct FfpPsConsts {\n";
  out << "  float4 textureFactor;\n";
  out << "  float4 stageConstants[" << kMaxTextureStages << "];\n";
  out << "  float alphaRef;\n";
  out << "  float fogStart;\n";
  out << "  float fogEnd;\n";
  out << "  float fogDensity;\n";
  out << "  uint alphaTestEnable;\n";
  out << "  uint alphaTestFunc;\n";
  out << "  uint fogMode;\n";
  out << "  uint fogSource;\n";
  out << "  float4 bumpEnvMat[" << kMaxTextureStages << "];\n";
  out << "  float2 bumpEnvLum[" << kMaxTextureStages << "];\n";
  out << "  float4 fogColor;\n";
  out << "};\n";
  if (argbufHybrid) {
    // R-BACK-12.22..12.26 MSL routing — tile kernel reads `ffpPs`
    // through the constants-only argument buffer at slot 30 instead of
    // buffer(3). The other constant-buffer fields (vsConsts/psConsts/
    // ffpVs) are unused by the tile path but stay declared so the
    // argbuf layout matches the host-side encoder.
    out << "struct VsConsts;\n";
    out << "struct PsConsts;\n";
    out << "struct FfpVsConsts;\n";
    out << "struct ArgbufLayout {\n";
    out << "  constant VsConsts*    vsConsts [[id(0)]];\n";
    out << "  constant FfpVsConsts* ffpVs    [[id(1)]];\n";
    out << "  constant PsConsts*    psConsts [[id(2)]];\n";
    out << "  constant FfpPsConsts* ffpPs    [[id(3)]];\n";
    out << "};\n";
  }
  out << "struct TileColorData {\n";
  out << "  " << tileColorType << " color [[color(0)]];\n";
  out << "};\n";
  // Tile kernel signature: imageblock<> reads/writes the attachment
  // value in tile memory; thread_position_in_threadgroup picks the
  // per-pixel slot. R-BACK-13.5 specifies threadgroupSizeMatchesTileSize
  // so one thread maps to one tile lane.
  if (argbufHybrid) {
    out << "[[kernel]] void ffp_tile(\n";
    out << "    imageblock<TileColorData, imageblock_layout_implicit> imageblock_data,\n";
    out << "    ushort2 tid [[thread_position_in_threadgroup]],\n";
    out << "    constant ArgbufLayout& abuf [[buffer("
        << shaders::kArgbufHybridBindSlot << ")]]) {\n";
    out << "  constant FfpPsConsts& ffpPs = *abuf.ffpPs;\n";
  } else {
    out << "[[kernel]] void ffp_tile(\n";
    out << "    imageblock<TileColorData, imageblock_layout_implicit> imageblock_data,\n";
    out << "    ushort2 tid [[thread_position_in_threadgroup]],\n";
    out << "    constant FfpPsConsts& ffpPs [[buffer(3)]]) {\n";
  }
  // R-BACK-13.5/13.7: `imageblock_layout_implicit` imageblocks expose the
  // per-thread slot via read(tid)/write(value, tid) (the `.data(tid)`
  // pointer accessor requires the explicit-layout form and does not compile
  // here). Read the attachment color by value, run the FFP arithmetic in
  // `float` (never `half`) to preserve bit-identity with the portable path,
  // then write back the modified slot.
  out << "  TileColorData tileSlot = imageblock_data.read(tid);\n";
  out << "  float4 color = float4(tileSlot.color);\n";
  if (!context.stripAlphaTestForDebug && key.alphaTestEnable) {
    out << "  if (ffpPs.alphaTestEnable != 0u) {\n";
    out << "    bool pass = true;\n";
    out << "    switch (ffpPs.alphaTestFunc) {\n";
    out << "      case 2u: pass = color.a < ffpPs.alphaRef; break;\n";
    out << "      case 3u: pass = color.a == ffpPs.alphaRef; break;\n";
    out << "      case 4u: pass = color.a <= ffpPs.alphaRef; break;\n";
    out << "      case 5u: pass = color.a > ffpPs.alphaRef; break;\n";
    out << "      case 6u: pass = color.a != ffpPs.alphaRef; break;\n";
    out << "      case 7u: pass = color.a >= ffpPs.alphaRef; break;\n";
    out << "      case 8u: pass = true; break;\n";
    out << "      default: pass = true; break;\n";
    out << "    }\n";
    // Tile-stage equivalent of `discard_fragment()` is to bypass the
    // write-back: the prior tile color stays. R-BACK-13.5 explicitly
    // forbids discard_fragment() in the tile kernel.
    out << "    if (!pass) { return; }\n";
    out << "  }\n";
  }
  if (key.fogMode != FogMode::None) {
    // Fog blend: linear over [fogStart, fogEnd]. Computed in float
    // (R-BACK-13.7) regardless of attachment format.
    //
    // Runtime-gate on `ffpPs.fogMode` exactly as the portable path does:
    // `makeFfpPixelKey` bakes a non-None fogMode whenever a fog table/vertex
    // mode render-state is set, but `buildFfpPsConsts` writes
    // `ffpPs.fogMode = 0` whenever D3DRS_FOGENABLE is off. The portable
    // `dxmt9_apply_fog` consults `ffpPs.fogMode` at runtime (fog factor 1.0 =>
    // no blend) so a fog-keyed-but-fog-disabled draw is a no-op there. Without
    // this guard the tile kernel would blend toward a zeroed fogColor and turn
    // a fog-disabled draw black (plain-quad regression).
    out << "  if (ffpPs.fogMode != 0u) {\n";
    out << "    float fog = clamp((ffpPs.fogEnd - color.a) /\n";
    out << "                      max(ffpPs.fogEnd - ffpPs.fogStart, 1.0e-6f),\n";
    out << "                      0.0f, 1.0f);\n";
    out << "    color = float4(mix(ffpPs.fogColor.rgb, color.rgb, fog), color.a);\n";
    out << "  }\n";
  }
  // Demote back to the imageblock element type and write the slot back.
  if (useHalf) {
    out << "  tileSlot.color = half4(color);\n";
  } else {
    out << "  tileSlot.color = color;\n";
  }
  out << "  imageblock_data.write(tileSlot, tid);\n";
  out << "}\n";
  out << "// ffp tile pixel hash " << key.hash << " fmt " << colorAttachmentPixelFormat << "\n";
  (void)context;
  return out.str();
}

u32 computeVertexDeclStreamStride(const VertexDeclSnapshot& decl, u32 stream) {
  if (stream >= decl.streams.size()) {
    return 0;
  }
  if (decl.streams[stream].stride != 0) {
    return decl.streams[stream].stride;
  }
  u32 computedStride = 0;
  for (const auto& element : decl.elements) {
    if (element.stream != stream) {
      continue;
    }
    computedStride = std::max(computedStride, static_cast<u32>(element.offset + declTypeSize(element.type)));
  }
  return computedStride;
}

u32 computeVertexDeclStride(const VertexDeclSnapshot& decl) {
  return computeVertexDeclStreamStride(decl, 0);
}

u32 vertexShaderStreamBufferSlot(u32 stream) {
  return stream == 0 ? 1u : 5u + stream;
}

u64 hashVertexDeclaration(const VertexDeclSnapshot& decl) {
  u64 hash = 1469598103934665603ull;
  hash ^= decl.fvf;
  hash *= 1099511628211ull;
  for (const auto& stream : decl.streams) {
    hash ^= stream.offset;
    hash *= 1099511628211ull;
    hash ^= stream.stride;
    hash *= 1099511628211ull;
  }
  for (const auto& element : decl.elements) {
    hash ^= element.stream;
    hash *= 1099511628211ull;
    hash ^= element.offset;
    hash *= 1099511628211ull;
    hash ^= element.type;
    hash *= 1099511628211ull;
    hash ^= element.method;
    hash *= 1099511628211ull;
    hash ^= element.usage;
    hash *= 1099511628211ull;
    hash ^= element.usageIndex;
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 hashVertexShaderInputLayout(const VertexShaderInputLayout& layout) {
  u64 hash = 1469598103934665603ull;
  hash ^= layout.stride;
  hash *= 1099511628211ull;
  hash ^= layout.streamMask;
  hash *= 1099511628211ull;
  for (const auto stride : layout.streamStrides) {
    hash ^= stride;
    hash *= 1099511628211ull;
  }
  for (const auto& input : layout.inputs) {
    hash ^= static_cast<u64>(input.valid);
    hash *= 1099511628211ull;
    hash ^= input.stream;
    hash *= 1099511628211ull;
    hash ^= input.offset;
    hash *= 1099511628211ull;
    hash ^= input.type;
    hash *= 1099511628211ull;
    hash ^= input.usage;
    hash *= 1099511628211ull;
    hash ^= input.usageIndex;
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace dxmt9::ffp
