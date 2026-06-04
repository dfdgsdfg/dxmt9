#include "dxmt9_draw_shader.hpp"

#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_shader_sources.hpp"
#include "dxmt9_shader_translator.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>

namespace dxmt9::drawshader {

using namespace ::dxmt9::core;

namespace {

const char* shaderDumpDir() {
  static const char* dir = std::getenv("DXMT_DUMP_SHADER_DIR");
  return dir;
}

void maybeDumpShaderSource(const char* label, u64 shaderHash, const std::string& source) {
  const char* dir = shaderDumpDir();
  if (!dir || !label) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const u64 sourceHash = hashBytes(std::as_bytes(std::span(source)));
  const auto path = std::filesystem::path(dir) /
                    (std::string(label) + "-shader-" + std::to_string(shaderHash) +
                     "-source-" + std::to_string(sourceHash) + ".metal");
  if (std::filesystem::exists(path, ec)) {
    return;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return;
  }
  out.write(source.data(), static_cast<std::streamsize>(source.size()));
}

TextureType textureTypeFromStateValue(u32 value) {
  switch (value) {
    case static_cast<u32>(TextureType::Cube):
    case 5u:  // D3DRTYPE_CUBETEXTURE
      return TextureType::Cube;
    case static_cast<u32>(TextureType::Volume):
    case 4u:  // D3DRTYPE_VOLUMETEXTURE
      return TextureType::Volume;
    case static_cast<u32>(TextureType::TwoD):
    case 3u:  // D3DRTYPE_TEXTURE
    default:
      return TextureType::TwoD;
  }
}

}  // namespace

std::uint32_t activeFragmentTextureMaskForShader(
    const ShaderRef& pixelShader,
    std::uint32_t textureMask) {
  constexpr std::uint32_t fragmentMask =
      (1u << core::kMaxFragmentSamplers) - 1u;
  std::uint32_t activeMask = textureMask & fragmentMask;
  if (pixelShader.kind != ShaderRef::Kind::FixedFunctionPixel ||
      !pixelShader.pixelKey.has_value()) {
    return activeMask;
  }

  std::uint32_t ffpMask = 0u;
  const auto& key = *pixelShader.pixelKey;
  for (std::size_t stage = 0; stage < core::kMaxTextureStages; ++stage) {
    const bool stageEnabled =
        key.stages[stage].colorOp != static_cast<u32>(TextureOp::Disable) ||
        key.stages[stage].alphaOp != static_cast<u32>(TextureOp::Disable);
    if (stageEnabled) {
      ffpMask |= 1u << stage;
    }
  }
  return activeMask & ffpMask;
}

namespace {

bool envFlag(const char* name) {
  const char* env = std::getenv(name);
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

void keepTexcoord(shaders::VSOutLayout& layout, std::uint32_t index) {
  if (index < core::kMaxTextureStages) {
    layout.texcoordMask |= 1u << index;
  }
}

shaders::VSOutLayout ffpPixelVaryingLiveness(const ShaderSourceContext& context) {
  auto layout = shaders::minimalVSOutLayout();
  keepTexcoord(layout, 0u);
  if (context.pixelShader.kind != ShaderRef::Kind::FixedFunctionPixel ||
      !context.pixelShader.pixelKey.has_value()) {
    return layout;
  }

  const auto& key = *context.pixelShader.pixelKey;
  // makeFfpPixelSource currently materializes these unconditionally as
  // `color`, `current`, `diffuse`, and `specular`.
  layout.color = true;
  layout.secondaryColor = true;
  if (!context.stripFogAlphaTestForTileBase && !context.stripFogForDebug &&
      key.fogMode != FogMode::None) {
    layout.fogFactor = true;
  }

  const bool debugFfpUv = envFlag("DXMT_DEBUG_FFP_UV");
  const bool debugFfpTexture = envFlag("DXMT_DEBUG_FFP_TEXTURE");
  const bool debugFfpAlpha = envFlag("DXMT_DEBUG_FFP_ALPHA");
  if (debugFfpUv) {
    keepTexcoord(layout, 0u);
  }

  for (std::size_t stage = 0; stage < core::kMaxTextureStages; ++stage) {
    const auto& stageKey = key.stages[stage];
    const bool stageEnabled =
        stageKey.colorOp != static_cast<u32>(TextureOp::Disable) ||
        stageKey.alphaOp != static_cast<u32>(TextureOp::Disable);
    if (!stageEnabled || !context.textures[stage]) {
      continue;
    }
    if (!key.pointSpriteEnable || debugFfpTexture || debugFfpAlpha ||
        ((stageKey.colorOp == 22u || stageKey.colorOp == 23u) && stage > 0u)) {
      keepTexcoord(layout, static_cast<std::uint32_t>(stage));
    }
  }
  return layout;
}

}  // namespace

shaders::VSOutLayout resolveVSOutLayoutForShaderPair(const ShaderSourceContext& context) {
  if (!shaders::vsoutTrimEnabled()) {
    return shaders::applyVSOutProbeOverrides(shaders::fullVSOutLayout());
  }

  if (context.pixelShader.kind == ShaderRef::Kind::Bytecode) {
    return shaders::applyVSOutProbeOverrides(
        translator::collectTranslatedFragmentVaryingLiveness(context.pixelShader, context));
  }
  if (context.pixelShader.kind == ShaderRef::Kind::FixedFunctionPixel &&
      context.pixelShader.pixelKey.has_value()) {
    return shaders::applyVSOutProbeOverrides(ffpPixelVaryingLiveness(context));
  }
  return shaders::applyVSOutProbeOverrides(shaders::fullVSOutLayout());
}

ShaderSourceContext makeShaderSourceContext(const DrawShaderLayoutContext& layout,
                                            const FlatDrawStateRecord& hot) {
  ShaderSourceContext context{};
  context.vertexDecl = layout.vertexDecl;
  context.vertexShader = layout.vertexShader;
  context.pixelShader = layout.pixelShader;
  const std::uint32_t activeFragmentTextureMask =
      activeFragmentTextureMaskForShader(layout.pixelShader, hot.textureMask);
  for (std::size_t i = 0; i < context.textures.size(); ++i) {
    const bool fragmentSlot = i < core::kMaxFragmentSamplers;
    context.textures[i] =
        hot.textures[i] != Handle{} &&
        (!fragmentSlot || ((activeFragmentTextureMask & (1u << i)) != 0u));
    if (i < hot.textureStageStates.size()) {
      context.textureTypes[i] = textureTypeFromStateValue(
          flatStateOr(hot.textureStageStates[i], TSS_TEXTURE_TYPE, static_cast<u32>(TextureType::TwoD)));
    }
  }
  context.sampleCount = std::max(1u, hot.colorAttachments[0].sampleCount);
  context.clipPlaneMask = layout.clipPlaneMask;
  context.unboundTextureFallback = true;
  context.enableHalfVSOut = shaders::vsoutProbeHalfEnabled();
  return context;
}

ShaderSourceContext makeShaderSourceContext(const fixture::DrawDesc& desc) {
  const auto layout = fixture::makeDrawShaderLayoutContext(desc);
  ShaderSourceContext context{};
  context.vertexDecl = layout.vertexDecl;
  context.vertexShader = layout.vertexShader;
  context.pixelShader = layout.pixelShader;
  for (std::size_t i = 0; i < context.textures.size(); ++i) {
    context.textures[i] = desc.textures[i].handle != Handle{};
    context.textureTypes[i] = textureTypeFromStateValue(
        desc.textures[i].stageStates.valueOr(TSS_TEXTURE_TYPE, static_cast<u32>(TextureType::TwoD)));
  }
  context.sampleCount = std::max(1u, desc.rts.color[0].sampleCount);
  context.clipPlaneMask = layout.clipPlaneMask;
  context.enableHalfVSOut = shaders::vsoutProbeHalfEnabled();
  return context;
}

std::string makeDrawShaderSource(const ShaderSourceContext& context, bool vertex) {
  if (vertex) {
    if (context.vertexShader.kind == ShaderRef::Kind::Bytecode) {
      auto source = translator::makeTranslatedVertexSource(context.vertexShader, context);
      maybeDumpShaderSource("translated-vs", context.vertexShader.hash, source);
      return source;
    }
    if (context.vertexShader.kind == ShaderRef::Kind::FixedFunctionVertex && context.vertexShader.vertexKey) {
      auto source = ffp::makeFfpVertexSource(*context.vertexShader.vertexKey, context);
      maybeDumpShaderSource("ffp-vs", context.vertexShader.hash, source);
      return source;
    }
    const u64 variantHash = context.vertexShader.hash ^ context.clipPlaneMask ^ context.sampleCount;
    auto source = context.textures[0] ? shaders::makeTexturedVertexSource(variantHash)
                                      : shaders::makeGenericVertexSource(variantHash);
    maybeDumpShaderSource("builtin-vs", variantHash, source);
    return source;
  }

  if (context.pixelShader.kind == ShaderRef::Kind::Bytecode) {
    auto source = translator::makeTranslatedFragmentSource(context.pixelShader, context);
    maybeDumpShaderSource("translated-fs", context.pixelShader.hash, source);
    return source;
  }
  if (context.pixelShader.kind == ShaderRef::Kind::FixedFunctionPixel && context.pixelShader.pixelKey) {
    auto source = ffp::makeFfpPixelSource(*context.pixelShader.pixelKey, context);
    maybeDumpShaderSource("ffp-fs", context.pixelShader.hash, source);
    return source;
  }
  const u64 variantHash = context.pixelShader.hash ^ context.clipPlaneMask ^ context.sampleCount;
  auto source = context.textures[0] ? shaders::makeTexturedFragmentSource(variantHash)
                                    : shaders::makeGenericFragmentSource(ColorRGBA{1.0f, 1.0f, 1.0f, 1.0f},
                                                                          variantHash);
  maybeDumpShaderSource("builtin-fs", variantHash, source);
  return source;
}

}  // namespace dxmt9::drawshader
