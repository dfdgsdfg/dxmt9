#include "dxmt9_ffp_shaders.hpp"
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
  hash ^= layout.blendWeightOffset;
  hash *= 1099511628211ull;
  hash ^= layout.blendWeightComponents;
  hash *= 1099511628211ull;
  hash ^= layout.blendIndicesOffset;
  hash *= 1099511628211ull;
  for (size_t i = 0; i < layout.hasTexcoord.size(); ++i) {
    hash ^= static_cast<u64>(layout.hasTexcoord[i]);
    hash *= 1099511628211ull;
    hash ^= layout.texcoordOffset[i];
    hash *= 1099511628211ull;
  }
  return hash;
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
      } else if (element.usage == kD3DDeclUsageBlendWeight && element.usageIndex == 0 &&
                 element.type <= kD3DDeclTypeFloat4) {
        layout.hasBlendWeight = true;
        layout.blendWeightOffset = element.offset;
        layout.blendWeightComponents = element.type + 1u;
      } else if (element.usage == kD3DDeclUsageBlendIndices && element.usageIndex == 0 &&
                 element.type == kD3DDeclTypeUByte4) {
        layout.hasBlendIndices = true;
        layout.blendIndicesOffset = element.offset;
      } else if (element.usage == kD3DDeclUsageTexcoord && element.usageIndex < kMaxTextureStages &&
                 element.type == kD3DDeclTypeFloat2) {
        layout.hasTexcoord[element.usageIndex] = true;
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
  if (position != kFvfXyzrhw && position != kFvfXyz &&
      position != kFvfXyzB1 && position != kFvfXyzB2 &&
      position != kFvfXyzB3 && position != kFvfXyzB4 &&
      position != kFvfXyzB5) {
    return std::nullopt;
  }

  layout.valid = true;
  layout.preTransformed = position == kFvfXyzrhw;
  layout.positionComponents = layout.preTransformed ? 4u : 3u;
  u32 offset = 0;
  layout.positionOffset = offset;
  offset += layout.preTransformed ? 16u : 12u;
  if (!layout.preTransformed && position > kFvfXyz) {
    layout.hasBlendWeight = true;
    layout.blendWeightOffset = offset;
    layout.blendWeightComponents = std::min<u32>((position - kFvfXyzrhw) / 2u, 4u);
    offset += layout.blendWeightComponents * 4u;
  }

  if ((fvf & kFvfNormal) != 0) {
    offset += 12u;
  }

  if ((fvf & kFvfDiffuse) != 0) {
    layout.hasDiffuse = true;
    layout.diffuseOffset = offset;
    offset += 4;
  }
  if ((fvf & kFvfSpecular) != 0) {
    offset += 4;
  }

  const u32 texCount = (fvf & kFvfTexCountMask) >> kFvfTexCountShift;
  if (texCount > 0) {
    for (u32 i = 0; i < std::min<u32>(texCount, kMaxTextureStages); ++i) {
      if (fvfTexcoordSize(fvf, i) >= 2u) {
        layout.hasTexcoord[i] = true;
        layout.texcoordOffset[i] = offset;
      }
      offset += fvfTexcoordSize(fvf, i) * 4u;
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
  constexpr u32 kTciCameraSpacePosition = 0x00020000u;
  const auto maxTexOut = shaders::vsoutMaxTexcoord();
  const auto emitStageTexcoords = [&](std::ostringstream& shader, const char* positionExpr) {
    for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
      // DXMT9_TRIM_UNUSED_VARYINGS: skip stages that the trimmed VSOut
      // doesn't declare; the local `dxmt9_texcoordN` would still compute
      // but the `out.texcoordN` write would target an undefined field.
      if (stage >= maxTexOut) continue;
      const u32 texCoordIndex = key.texCoordGen[stage] & kTciIndexMask;
      const u32 texCoordGen = key.texCoordGen[stage] & kTciGenMask;
      shader << "  float4 dxmt9_texcoord" << stage << " = float4(0.0f, 0.0f, 1.0f, 1.0f);\n";
      if (layout && texCoordIndex < layout->hasTexcoord.size() && layout->hasTexcoord[texCoordIndex]) {
        shader << "  dxmt9_texcoord" << stage << " = float4(dxmt9_load_f32x2(stream0, base + "
               << layout->texcoordOffset[texCoordIndex] << "u), 1.0f, 1.0f);\n";
      }
      if (texCoordGen == kTciCameraSpacePosition && !(layout && layout->preTransformed)) {
        shader << "  dxmt9_texcoord" << stage << " = float4(" << positionExpr << ".xyz, 1.0f);\n";
      }
      if (layout && layout->preTransformed) {
        shader << "  out.texcoord" << stage << " = dxmt9_texcoord" << stage << ";\n";
      } else {
        shader << "  out.texcoord" << stage << " = float4(dxmt9_apply_texture_transform(dxmt9_texcoord" << stage
               << ", ffpVs, " << stage << "u, " << key.texTransformFlags[stage]
               << "u), dxmt9_texcoord" << stage << ".zw);\n";
      }
    }
  };
  if (argbufHybrid) {
    out << shaders::makeShaderPreludeArgbufHybrid(key.clipPlaneMask != 0);
  } else {
    out << shaders::makeShaderPrelude(key.clipPlaneMask != 0);
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
             "constant ArgbufLayout const* abuf [[buffer("
          << shaders::kArgbufHybridBindSlot << ")]], ";
      if (withStream) {
        out << "device const uchar* stream0 [[buffer(1)]], ";
      }
      out << "constant DrawVolatile& drawVolatile [[buffer(5)]]) {\n";
      out << "  constant VsConsts& vsConsts = *abuf->vsConsts;\n";
      out << "  constant FfpVsConsts& ffpVs = *abuf->ffpVs;\n";
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
    out << "  out.position.xy += ffpVs.halfPixelFixup * out.position.w;\n";
    if (layout->hasDiffuse) {
      out << "  out.color = dxmt9_load_d3dcolor(stream0, base + " << layout->diffuseOffset << "u);\n";
    } else {
      out << "  out.color = float4(1.0);\n";
    }
    out << "  out.secondaryColor = float4(0.0);\n";
    emitStageTexcoords(out, "inPosition");
    if (shaders::vsoutEmitFogFactor()) out << "  out.fogFactor = 1.0;\n";
    if (shaders::vsoutEmitPointSize()) out << "  out.pointSize = 1.0;\n";
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
    } else {
      out << "  float4 inPosition = float4(dxmt9_load_f32x3(stream0, base + " << layout->positionOffset
          << "u), 1.0f);\n";
    }
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
    if (layout->hasDiffuse) {
      out << "  out.color = dxmt9_load_d3dcolor(stream0, base + " << layout->diffuseOffset << "u);\n";
    } else {
      out << "  out.color = float4(1.0);\n";
    }
    out << "  out.secondaryColor = float4(0.0);\n";
    emitStageTexcoords(out, "inPosition");
    if (shaders::vsoutEmitFogFactor()) out << "  out.fogFactor = 1.0;\n";
    if (shaders::vsoutEmitPointSize()) out << "  out.pointSize = 1.0;\n";
  } else {
    emitVertexSig(/*withStream=*/false);
    out << "  (void)vsConsts; (void)drawVolatile;\n";
    out << "  VSOut out;\n";
    out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
    out << "  out.position = float4(p[vid % 3], 0.0, 1.0);\n";
    out << "  out.position.xy += ffpVs.halfPixelFixup * out.position.w;\n";
    out << "  out.color = float4(1.0);\n";
    out << "  out.secondaryColor = float4(0.0);\n";
    out << "  out.texcoord0 = float4(float2(vid & 1u, (vid >> 1u) & 1u), 0.0f, 1.0f);\n";
    for (size_t i = 1; i < maxTexOut; ++i) {
      out << "  out.texcoord" << i << " = out.texcoord0;\n";
    }
    if (shaders::vsoutEmitFogFactor()) out << "  out.fogFactor = 1.0;\n";
    if (shaders::vsoutEmitPointSize()) out << "  out.pointSize = 1.0;\n";
  }
  out << "  if (" << (key.lightingEnabled ? "true" : "false") << ") {\n";
  out << "    out.color.rgb *= 1.0;\n";
  out << "  }\n";
  if (key.clipPlaneMask != 0 || context.clipPlaneMask != 0) {
    out << "  for (uint i = 0; i < 6; ++i) {\n";
    out << "    if ((ffpVs.clipPlaneMask & (1u << i)) != 0u) {\n";
      out << "      out.clipDistance[i] = dot(ffpVs.clipPlanes[i], out.position);\n";
    out << "    }\n";
    out << "  }\n";
  }
  out << "  return out;\n";
  out << "}\n";
  out << "// ffp vertex hash " << key.hash << "\n";
  return out.str();
}

std::string makeFfpPixelSource(const FfpPixelKey& key,
                               const drawshader::ShaderSourceContext& context) {
  std::ostringstream out;
  std::vector<size_t> activeStages;
  activeStages.reserve(kMaxTextureStages);
  for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    const bool stageEnabled =
        key.stages[stage].colorOp != static_cast<u32>(TextureOp::Disable) ||
        key.stages[stage].alphaOp != static_cast<u32>(TextureOp::Disable);
    if (stageEnabled && context.textures[stage]) {
      activeStages.push_back(stage);
    }
  }
  const bool textured = !activeStages.empty();
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
  if (argbufHybrid) {
    out << shaders::makeShaderPreludeArgbufHybrid(context.clipPlaneMask != 0);
  } else {
    out << shaders::makeShaderPrelude(context.clipPlaneMask != 0);
  }
  // R-BACK-12.22..12.26 MSL routing — when argbufHybrid is set, the
  // fragment entry point takes a single argument buffer at slot 30 and
  // reads `psConsts`/`ffpPs` plus textures/samplers off the argbuf.
  // The body code references `texN`/`sampN` locals so we materialize
  // each active stage's texture and sampler off `abuf->textures2d[N]` /
  // `abuf->samplers[N]` at function entry.
  if (textured) {
    if (argbufHybrid) {
      out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], "
             "constant ArgbufLayout const* abuf [[buffer("
          << shaders::kArgbufHybridBindSlot << ")]]) {\n";
      out << "  constant PsConsts& psConsts = *abuf->psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf->ffpPs;\n";
      for (size_t i = 0; i < activeStages.size(); ++i) {
        const size_t stage = activeStages[i];
        out << "  texture2d<float> tex" << stage << " = abuf->textures2d[" << stage << "];\n";
        out << "  sampler samp" << stage << " = abuf->samplers[" << stage << "];\n";
      }
    } else {
      out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], "
             "constant PsConsts& psConsts [[buffer(0)]], "
             "constant FfpPsConsts& ffpPs [[buffer(3)]], ";
      for (size_t i = 0; i < activeStages.size(); ++i) {
        const size_t stage = activeStages[i];
        if (i != 0) {
          out << ", ";
        }
        out << "texture2d<float> tex" << stage << " [[texture(" << stage << ")]], sampler samp" << stage
            << " [[sampler(" << stage << ")]]";
      }
      out << ") {\n";
    }
    out << "  (void)psConsts;\n";
  } else {
    if (argbufHybrid) {
      out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], "
             "constant ArgbufLayout const* abuf [[buffer("
          << shaders::kArgbufHybridBindSlot << ")]]) {\n";
      out << "  constant PsConsts& psConsts = *abuf->psConsts;\n";
      out << "  constant FfpPsConsts& ffpPs = *abuf->ffpPs;\n";
    } else {
      out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], "
             "constant PsConsts& psConsts [[buffer(0)]], "
             "constant FfpPsConsts& ffpPs [[buffer(3)]]) {\n";
    }
    out << "  (void)psConsts;\n";
  }
  out << "  float4 color = in.color;\n";
  out << "  float4 current = color;\n";
  out << "  float4 diffuse = in.color;\n";
  out << "  float4 specular = in.secondaryColor;\n";
  out << "  float4 tfactor = ffpPs.textureFactor;\n";
  out << "  float4 temp = float4(0.0);\n";
  if (textured) {
    if (debugFfpUv) {
      out << "  return float4(fract(in.texcoord0.x), fract(in.texcoord0.y), 0.0, 1.0);\n";
      out << "}\n";
      out << "// ffp pixel hash " << key.hash << "\n";
      return out.str();
    }
    if (debugFfpTexture) {
      const size_t stage = activeStages.front();
      const u32 coordIndex = key.stages[stage].texCoordIndex & 0xffffu;
      out << "  return tex" << stage << ".sample(samp" << stage
          << ", dxmt9_select_texcoord(in, " << coordIndex << "u).xy);\n";
      out << "}\n";
      out << "// ffp pixel hash " << key.hash << "\n";
      return out.str();
    }
    if (debugFfpAlpha) {
      const size_t stage = activeStages.front();
      const u32 coordIndex = key.stages[stage].texCoordIndex & 0xffffu;
      out << "  float alpha = tex" << stage << ".sample(samp" << stage
          << ", dxmt9_select_texcoord(in, " << coordIndex << "u).xy).a;\n";
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
      const u32 coordIndex = stageKey.texCoordIndex & 0xffffu;
      if (hasTexture) {
        out << "  float4 texColor" << stage << " = tex" << stage << ".sample(samp" << stage
            << ", dxmt9_select_texcoord(in, " << coordIndex << "u).xy);\n";
      } else {
        out << "  float4 texColor" << stage << " = float4(1.0f);\n";
      }
      out << "  float4 colorArg1_" << stage << " = dxmt9_select_texture_arg(" << stageKey.colorArg1
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp);\n";
      out << "  float4 colorArg2_" << stage << " = dxmt9_select_texture_arg(" << stageKey.colorArg2
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp);\n";
      out << "  float4 stageResult" << stage << " = dxmt9_apply_texture_op(" << stageKey.colorOp
          << "u, colorArg1_" << stage << ", colorArg2_" << stage << ", current);\n";
      out << "  float4 alphaArg1_" << stage << " = dxmt9_select_texture_arg(" << stageKey.alphaArg1
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp);\n";
      out << "  float4 alphaArg2_" << stage << " = dxmt9_select_texture_arg(" << stageKey.alphaArg2
          << "u, current, diffuse, specular, texColor" << stage << ", tfactor, temp);\n";
      out << "  stageResult" << stage << ".a = dxmt9_apply_texture_op(" << stageKey.alphaOp
          << "u, alphaArg1_" << stage << ", alphaArg2_" << stage << ", current).a;\n";
      out << "  current = stageResult" << stage << ";\n";
      if (stageKey.resultArg == 5u) {
        out << "  temp = stageResult" << stage << ";\n";
      }
    }
    out << "  color = current;\n";
  }
  if (key.alphaTestEnable) {
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
  if (key.fogMode != FogMode::None) {
    out << "  float fogDepth = color.a;\n";
    out << "  float fog = 1.0f;\n";
    out << "  if (ffpPs.fogMode == 1u) {\n";
    out << "    fog = clamp((ffpPs.fogEnd - fogDepth) /\n";
    out << "                max(ffpPs.fogEnd - ffpPs.fogStart, 1.0e-6f),\n";
    out << "                0.0f, 1.0f);\n";
    out << "  } else if (ffpPs.fogMode == 2u) {\n";
    out << "    fog = clamp(exp(-ffpPs.fogDensity * fogDepth), 0.0f, 1.0f);\n";
    out << "  } else if (ffpPs.fogMode == 3u) {\n";
    out << "    float d = ffpPs.fogDensity * fogDepth;\n";
    out << "    fog = clamp(exp(-(d * d)), 0.0f, 1.0f);\n";
    out << "  }\n";
    out << "  float4 fogColor = float4(0.5, 0.5, 0.5, 1.0);\n";
    out << "  color = mix(fogColor, color, fog);\n";
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
  out << "  float alphaRef;\n";
  out << "  float fogStart;\n";
  out << "  float fogEnd;\n";
  out << "  float fogDensity;\n";
  out << "  uint alphaTestEnable;\n";
  out << "  uint alphaTestFunc;\n";
  out << "  uint fogMode;\n";
  out << "  uint _pad;\n";
  out << "  float4 bumpEnvMat[" << kMaxTextureStages << "];\n";
  out << "  float2 bumpEnvLum[" << kMaxTextureStages << "];\n";
  out << "};\n";
  if (argbufHybrid) {
    // R-BACK-12.22..12.26 MSL routing — tile kernel reads `ffpPs`
    // through the argument buffer at slot 30 instead of buffer(3).
    // The remaining fields (vsConsts, psConsts, textures, samplers)
    // are unused by the tile path but must be declared for the
    // argbuf layout to match the host-side encoder.
    out << "struct VsConsts;\n";
    out << "struct PsConsts;\n";
    out << "struct FfpVsConsts;\n";
    out << "struct ArgbufLayout {\n";
    out << "  constant VsConsts*    vsConsts [[id(0)]];\n";
    out << "  constant FfpVsConsts* ffpVs    [[id(1)]];\n";
    out << "  constant PsConsts*    psConsts [[id(2)]];\n";
    out << "  constant FfpPsConsts* ffpPs    [[id(3)]];\n";
    out << "  array<texture2d<float>, " << shaders::kArgbufHybridTextureSlotCount
        << "> textures [[id(" << shaders::kArgbufHybridConstantBufferCount << ")]];\n";
    out << "  array<sampler, " << shaders::kArgbufHybridSamplerSlotCount
        << "> samplers [[id("
        << (shaders::kArgbufHybridConstantBufferCount + shaders::kArgbufHybridTextureSlotCount)
        << ")]];\n";
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
    out << "    constant ArgbufLayout const* abuf [[buffer("
        << shaders::kArgbufHybridBindSlot << ")]]) {\n";
    out << "  constant FfpPsConsts& ffpPs = *abuf->ffpPs;\n";
  } else {
    out << "[[kernel]] void ffp_tile(\n";
    out << "    imageblock<TileColorData, imageblock_layout_implicit> imageblock_data,\n";
    out << "    ushort2 tid [[thread_position_in_threadgroup]],\n";
    out << "    constant FfpPsConsts& ffpPs [[buffer(3)]]) {\n";
  }
  out << "  threadgroup_imageblock TileColorData* slot = imageblock_data.data(tid);\n";
  // R-BACK-13.7: even when the imageblock element is `half4`, FFP
  // arithmetic must be carried in `float` to preserve bit-identity with
  // the portable path. We promote on read and demote on write-back.
  out << "  float4 color = float4(slot->color);\n";
  if (key.alphaTestEnable) {
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
    out << "  float fog = clamp((ffpPs.fogEnd - color.a) /\n";
    out << "                    max(ffpPs.fogEnd - ffpPs.fogStart, 1.0e-6f),\n";
    out << "                    0.0f, 1.0f);\n";
    out << "  float4 fogColor = float4(0.5f, 0.5f, 0.5f, 1.0f);\n";
    out << "  color = mix(fogColor, color, fog);\n";
  }
  // Demote back to imageblock element type on write.
  if (useHalf) {
    out << "  slot->color = half4(color);\n";
  } else {
    out << "  slot->color = color;\n";
  }
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
