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

void maybeDumpShaderSource(const char* label, const std::string& source) {
  const char* dir = shaderDumpDir();
  if (!dir || !label) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const u64 hash = hashBytes(std::as_bytes(std::span(source)));
  const auto path = std::filesystem::path(dir) /
                    (std::string(label) + "-" + std::to_string(hash) + ".metal");
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

ShaderSourceContext makeShaderSourceContext(const DrawShaderLayoutContext& layout,
                                            const FlatDrawStateRecord& hot) {
  ShaderSourceContext context{};
  context.vertexDecl = layout.vertexDecl;
  context.vertexShader = layout.vertexShader;
  context.pixelShader = layout.pixelShader;
  for (std::size_t i = 0; i < context.textures.size(); ++i) {
    context.textures[i] = hot.textures[i] != Handle{};
    if (i < hot.textureStageStates.size()) {
      context.textureTypes[i] = textureTypeFromStateValue(
          flatStateOr(hot.textureStageStates[i], TSS_TEXTURE_TYPE, static_cast<u32>(TextureType::TwoD)));
    }
  }
  context.sampleCount = std::max(1u, hot.colorAttachments[0].sampleCount);
  context.clipPlaneMask = layout.clipPlaneMask;
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
  return context;
}

std::string makeDrawShaderSource(const ShaderSourceContext& context, bool vertex) {
  if (vertex) {
    if (context.vertexShader.kind == ShaderRef::Kind::Bytecode) {
      auto source = translator::makeTranslatedVertexSource(context.vertexShader, context);
      maybeDumpShaderSource("translated-vs", source);
      return source;
    }
    if (context.vertexShader.kind == ShaderRef::Kind::FixedFunctionVertex && context.vertexShader.vertexKey) {
      auto source = ffp::makeFfpVertexSource(*context.vertexShader.vertexKey, context);
      maybeDumpShaderSource("ffp-vs", source);
      return source;
    }
    const u64 variantHash = context.vertexShader.hash ^ context.clipPlaneMask ^ context.sampleCount;
    auto source = context.textures[0] ? shaders::makeTexturedVertexSource(variantHash)
                                      : shaders::makeGenericVertexSource(variantHash);
    maybeDumpShaderSource("builtin-vs", source);
    return source;
  }

  if (context.pixelShader.kind == ShaderRef::Kind::Bytecode) {
    auto source = translator::makeTranslatedFragmentSource(context.pixelShader, context);
    maybeDumpShaderSource("translated-fs", source);
    return source;
  }
  if (context.pixelShader.kind == ShaderRef::Kind::FixedFunctionPixel && context.pixelShader.pixelKey) {
    auto source = ffp::makeFfpPixelSource(*context.pixelShader.pixelKey, context);
    maybeDumpShaderSource("ffp-fs", source);
    return source;
  }
  const u64 variantHash = context.pixelShader.hash ^ context.clipPlaneMask ^ context.sampleCount;
  auto source = context.textures[0] ? shaders::makeTexturedFragmentSource(variantHash)
                                    : shaders::makeGenericFragmentSource(ColorRGBA{1.0f, 1.0f, 1.0f, 1.0f},
                                                                          variantHash);
  maybeDumpShaderSource("builtin-fs", source);
  return source;
}

}  // namespace dxmt9::drawshader
