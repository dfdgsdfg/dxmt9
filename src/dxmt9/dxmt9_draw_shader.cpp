#include "dxmt9_draw_shader.hpp"

#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_shader_sources.hpp"
#include "dxmt9_shader_translator.hpp"

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

}  // namespace

std::string makeDrawShaderSource(const DrawDesc& desc, bool vertex) {
  if (vertex) {
    if (desc.vertexShader.kind == ShaderRef::Kind::Bytecode) {
      auto source = translator::makeTranslatedVertexSource(desc.vertexShader, desc);
      maybeDumpShaderSource("translated-vs", source);
      return source;
    }
    if (desc.vertexShader.kind == ShaderRef::Kind::FixedFunctionVertex && desc.vertexShader.vertexKey) {
      auto source = ffp::makeFfpVertexSource(*desc.vertexShader.vertexKey, desc);
      maybeDumpShaderSource("ffp-vs", source);
      return source;
    }
    const u64 variantHash = desc.vertexShader.hash ^ desc.clipPlaneMask ^ desc.rts.color[0].sampleCount;
    auto source = desc.textures[0].handle ? shaders::makeTexturedVertexSource(variantHash)
                                          : shaders::makeGenericVertexSource(variantHash);
    maybeDumpShaderSource("builtin-vs", source);
    return source;
  }

  if (desc.pixelShader.kind == ShaderRef::Kind::Bytecode) {
    auto source = translator::makeTranslatedFragmentSource(desc.pixelShader, desc);
    maybeDumpShaderSource("translated-fs", source);
    return source;
  }
  if (desc.pixelShader.kind == ShaderRef::Kind::FixedFunctionPixel && desc.pixelShader.pixelKey) {
    auto source = ffp::makeFfpPixelSource(*desc.pixelShader.pixelKey, desc);
    maybeDumpShaderSource("ffp-fs", source);
    return source;
  }
  const u64 variantHash = desc.pixelShader.hash ^ desc.clipPlaneMask ^ desc.rts.color[0].sampleCount;
  auto source = desc.textures[0].handle ? shaders::makeTexturedFragmentSource(variantHash)
                                        : shaders::makeGenericFragmentSource(ColorRGBA{1.0f, 1.0f, 1.0f, 1.0f},
                                                                              variantHash);
  maybeDumpShaderSource("builtin-fs", source);
  return source;
}

}  // namespace dxmt9::drawshader
