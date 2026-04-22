#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_shader_sources.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace dxmt9::ffp {

using namespace dxmt9::core;
// The FVF / D3D-decl constants are already in this namespace (defined in the
// header). The `using namespace dxmt9::core;` above brings in DrawDesc,
// TextureOp, FogMode, Handle, and the kMax* constants.

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
      return 4;
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
  hash ^= layout.stride;
  hash *= 1099511628211ull;
  hash ^= layout.positionOffset;
  hash *= 1099511628211ull;
  hash ^= layout.diffuseOffset;
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

std::optional<FixedFunctionVertexLayout> decodeFixedFunctionVertexLayout(const DrawDesc& desc) {
  FixedFunctionVertexLayout layout;
  if (!desc.vertexDecl.elements.empty()) {
    u32 computedStride = 0;
    for (const auto& element : desc.vertexDecl.elements) {
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
      } else if (element.usage == kD3DDeclUsageTexcoord && element.usageIndex < kMaxTextureStages &&
                 element.type == kD3DDeclTypeFloat2) {
        layout.hasTexcoord[element.usageIndex] = true;
        layout.texcoordOffset[element.usageIndex] = element.offset;
      }
    }
    layout.stride = desc.vertexDecl.streams[0].stride ? desc.vertexDecl.streams[0].stride : computedStride;
    if (layout.valid) {
      layout.hash = hashFixedFunctionLayout(layout);
      return layout;
    }
    return std::nullopt;
  }

  const u32 fvf = desc.vertexDecl.fvf;
  const u32 position = fvf & kFvfPositionMask;
  if (position != kFvfXyzrhw && position != kFvfXyz) {
    return std::nullopt;
  }

  layout.valid = true;
  layout.preTransformed = position == kFvfXyzrhw;
  layout.positionComponents = layout.preTransformed ? 4u : 3u;
  u32 offset = 0;
  layout.positionOffset = offset;
  offset += layout.preTransformed ? 16u : 12u;

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

  layout.stride = desc.vertexDecl.streams[0].stride ? desc.vertexDecl.streams[0].stride : offset;
  layout.hash = hashFixedFunctionLayout(layout);
  return layout;
}

std::string makeFfpVertexSource(const FfpVertexKey& key, const DrawDesc& desc) {
  std::ostringstream out;
  const auto layout = decodeFixedFunctionVertexLayout(desc);
  constexpr u32 kTciIndexMask = 0x0000ffffu;
  constexpr u32 kTciGenMask = 0xffff0000u;
  constexpr u32 kTciCameraSpacePosition = 0x00020000u;
  const auto emitStageTexcoords = [&](std::ostringstream& shader, const char* positionExpr) {
    for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
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
               << ", uniforms, " << stage << "u, " << key.texTransformFlags[stage]
               << "u), dxmt9_texcoord" << stage << ".zw);\n";
      }
    }
  };
  out << shaders::makeShaderPrelude(key.clipPlaneMask != 0);
  if (layout && layout->preTransformed) {
    out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]], "
           "device const uchar* stream0 [[buffer(1)]]) {\n";
    out << "  VSOut out;\n";
    out << "  const uint stride = uniforms.vertexStreamStride != 0u ? uniforms.vertexStreamStride : "
        << layout->stride << "u;\n";
    out << "  const int vertexIndex = max(0, int(vid) + uniforms.vertexBaseIndex);\n";
    out << "  const uint base = uniforms.vertexStreamOffset + uint(vertexIndex) * stride;\n";
    out << "  float4 inPosition = dxmt9_load_f32x4(stream0, base + " << layout->positionOffset << "u);\n";
    out << "  float clipW = fabs(inPosition.w) > 1.0e-8f ? (1.0f / inPosition.w) : 1.0f;\n";
    out << "  float2 viewportSize = max(uniforms.viewportSize, float2(1.0f));\n";
    out << "  float2 ndc = float2(((inPosition.x - uniforms.viewportOrigin.x) / viewportSize.x) * 2.0f - 1.0f,\n";
    out << "                     1.0f - ((inPosition.y - uniforms.viewportOrigin.y) / viewportSize.y) * 2.0f);\n";
    out << "  out.position = float4(ndc * clipW, inPosition.z * clipW, clipW);\n";
    out << "  out.position.xy += uniforms.halfPixelFixup * out.position.w;\n";
    if (layout->hasDiffuse) {
      out << "  out.color = dxmt9_load_d3dcolor(stream0, base + " << layout->diffuseOffset << "u);\n";
    } else {
      out << "  out.color = float4(1.0);\n";
    }
    out << "  out.secondaryColor = float4(0.0);\n";
    emitStageTexcoords(out, "inPosition");
    out << "  out.fogFactor = 1.0;\n";
    out << "  out.pointSize = 1.0;\n";
  } else if (layout) {
    out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]], "
           "device const uchar* stream0 [[buffer(1)]]) {\n";
    out << "  VSOut out;\n";
    out << "  const uint stride = uniforms.vertexStreamStride != 0u ? uniforms.vertexStreamStride : "
        << layout->stride << "u;\n";
    out << "  const int vertexIndex = max(0, int(vid) + uniforms.vertexBaseIndex);\n";
    out << "  const uint base = uniforms.vertexStreamOffset + uint(vertexIndex) * stride;\n";
    if (layout->positionComponents == 4) {
      out << "  float4 inPosition = dxmt9_load_f32x4(stream0, base + " << layout->positionOffset << "u);\n";
    } else {
      out << "  float4 inPosition = float4(dxmt9_load_f32x3(stream0, base + " << layout->positionOffset
          << "u), 1.0f);\n";
    }
    out << "  float4 clip;\n";
    out << "  bool identityWvp = all(uniforms.ffpWorldViewProj[0] == float4(1.0, 0.0, 0.0, 0.0)) &&\n";
    out << "                     all(uniforms.ffpWorldViewProj[1] == float4(0.0, 1.0, 0.0, 0.0)) &&\n";
    out << "                     all(uniforms.ffpWorldViewProj[2] == float4(0.0, 0.0, 1.0, 0.0)) &&\n";
    out << "                     all(uniforms.ffpWorldViewProj[3] == float4(0.0, 0.0, 0.0, 1.0));\n";
    out << "  bool pixelSpacePosition = identityWvp && (fabs(inPosition.x) > 2.0f || fabs(inPosition.y) > 2.0f);\n";
    out << "  if (pixelSpacePosition) {\n";
    out << "    float2 viewportSize = max(uniforms.viewportSize, float2(1.0f));\n";
    out << "    float2 ndc = float2(((inPosition.x - uniforms.viewportOrigin.x) / viewportSize.x) * 2.0f - 1.0f,\n";
    out << "                       1.0f - ((inPosition.y - uniforms.viewportOrigin.y) / viewportSize.y) * 2.0f);\n";
    out << "    clip = float4(ndc, inPosition.z, 1.0f);\n";
    out << "  } else {\n";
    out << "    clip.x = dot(float4(uniforms.ffpWorldViewProj[0].x, uniforms.ffpWorldViewProj[1].x,\n";
    out << "                           uniforms.ffpWorldViewProj[2].x, uniforms.ffpWorldViewProj[3].x), inPosition);\n";
    out << "    clip.y = dot(float4(uniforms.ffpWorldViewProj[0].y, uniforms.ffpWorldViewProj[1].y,\n";
    out << "                           uniforms.ffpWorldViewProj[2].y, uniforms.ffpWorldViewProj[3].y), inPosition);\n";
    out << "    clip.z = dot(float4(uniforms.ffpWorldViewProj[0].z, uniforms.ffpWorldViewProj[1].z,\n";
    out << "                           uniforms.ffpWorldViewProj[2].z, uniforms.ffpWorldViewProj[3].z), inPosition);\n";
    out << "    clip.w = dot(float4(uniforms.ffpWorldViewProj[0].w, uniforms.ffpWorldViewProj[1].w,\n";
    out << "                           uniforms.ffpWorldViewProj[2].w, uniforms.ffpWorldViewProj[3].w), inPosition);\n";
    out << "  }\n";
    out << "  out.position = clip;\n";
    out << "  out.position.xy += uniforms.halfPixelFixup * out.position.w;\n";
    if (layout->hasDiffuse) {
      out << "  out.color = dxmt9_load_d3dcolor(stream0, base + " << layout->diffuseOffset << "u);\n";
    } else {
      out << "  out.color = float4(1.0);\n";
    }
    out << "  out.secondaryColor = float4(0.0);\n";
    emitStageTexcoords(out, "inPosition");
    out << "  out.fogFactor = 1.0;\n";
    out << "  out.pointSize = 1.0;\n";
  } else {
    out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant DrawUniforms& uniforms [[buffer(0)]]) {\n";
    out << "  VSOut out;\n";
    out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
    out << "  out.position = float4(p[vid % 3], 0.0, 1.0);\n";
    out << "  out.position.xy += uniforms.halfPixelFixup * out.position.w;\n";
    out << "  out.color = float4(1.0);\n";
    out << "  out.secondaryColor = float4(0.0);\n";
    out << "  out.texcoord0 = float4(float2(vid & 1u, (vid >> 1u) & 1u), 0.0f, 1.0f);\n";
    for (size_t i = 1; i < kMaxTextureStages; ++i) {
      out << "  out.texcoord" << i << " = out.texcoord0;\n";
    }
    out << "  out.fogFactor = 1.0;\n";
    out << "  out.pointSize = 1.0;\n";
  }
  out << "  if (" << (key.lightingEnabled ? "true" : "false") << ") {\n";
  out << "    out.color.rgb *= 1.0;\n";
  out << "  }\n";
  if (key.clipPlaneMask != 0 || desc.clipPlaneMask != 0) {
    out << "  for (uint i = 0; i < 6; ++i) {\n";
    out << "    if ((uniforms.clipPlaneMask & (1u << i)) != 0u) {\n";
      out << "      out.clipDistance[i] = dot(uniforms.clipPlanes[i], out.position);\n";
    out << "    }\n";
    out << "  }\n";
  }
  out << "  return out;\n";
  out << "}\n";
  out << "// ffp vertex hash " << key.hash << "\n";
  return out.str();
}

std::string makeFfpPixelSource(const FfpPixelKey& key, const DrawDesc& desc) {
  std::ostringstream out;
  std::vector<size_t> activeStages;
  activeStages.reserve(kMaxTextureStages);
  for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    const bool stageEnabled =
        key.stages[stage].colorOp != static_cast<u32>(TextureOp::Disable) ||
        key.stages[stage].alphaOp != static_cast<u32>(TextureOp::Disable);
    if (stageEnabled && desc.textures[stage].handle != Handle{}) {
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
  out << shaders::makeShaderPrelude(desc.clipPlaneMask != 0);
  if (textured) {
    out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]], ";
    for (size_t i = 0; i < activeStages.size(); ++i) {
      const size_t stage = activeStages[i];
      if (i != 0) {
        out << ", ";
      }
      out << "texture2d<float> tex" << stage << " [[texture(" << stage << ")]], sampler samp" << stage
          << " [[sampler(" << stage << ")]]";
    }
    out << ") {\n";
  } else {
    out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], constant DrawUniforms& uniforms [[buffer(0)]]) {\n";
  }
  out << "  float4 color = in.color;\n";
  out << "  float4 current = color;\n";
  out << "  float4 diffuse = in.color;\n";
  out << "  float4 specular = in.secondaryColor;\n";
  out << "  float4 tfactor = uniforms.textureFactor;\n";
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
      const bool hasTexture = desc.textures[stage].handle != Handle{};
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
    out << "  switch (uniforms.alphaTestFunc) {\n";
    out << "    case 2u: pass = color.a < uniforms.alphaRef; break;\n";
    out << "    case 3u: pass = color.a == uniforms.alphaRef; break;\n";
    out << "    case 4u: pass = color.a <= uniforms.alphaRef; break;\n";
    out << "    case 5u: pass = color.a > uniforms.alphaRef; break;\n";
    out << "    case 6u: pass = color.a != uniforms.alphaRef; break;\n";
    out << "    case 7u: pass = color.a >= uniforms.alphaRef; break;\n";
    out << "    case 8u: pass = true; break;\n";
    out << "    default: pass = true; break;\n";
    out << "  }\n";
    out << "  if (!pass) { discard_fragment(); }\n";
  }
  if (key.fogMode != FogMode::None) {
    out << "  float fog = clamp(in.fogFactor, 0.0, 1.0);\n";
    out << "  float4 fogColor = float4(0.5, 0.5, 0.5, 1.0);\n";
    out << "  color = mix(fogColor, color, fog);\n";
  }
  out << "  return color;\n";
  out << "}\n";
  out << "// ffp pixel hash " << key.hash << "\n";
  return out.str();
}

u32 computeVertexDeclStride(const DrawDesc& desc) {
  if (desc.vertexDecl.streams[0].stride != 0) {
    return desc.vertexDecl.streams[0].stride;
  }
  u32 computedStride = 0;
  for (const auto& element : desc.vertexDecl.elements) {
    if (element.stream != 0) {
      continue;
    }
    computedStride = std::max(computedStride, static_cast<u32>(element.offset + declTypeSize(element.type)));
  }
  return computedStride;
}

u64 hashVertexDeclaration(const VertexDeclSnapshot& decl) {
  u64 hash = 1469598103934665603ull;
  hash ^= decl.fvf;
  hash *= 1099511628211ull;
  hash ^= decl.streams[0].stride;
  hash *= 1099511628211ull;
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
  for (const auto& input : layout.inputs) {
    hash ^= static_cast<u64>(input.valid);
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
