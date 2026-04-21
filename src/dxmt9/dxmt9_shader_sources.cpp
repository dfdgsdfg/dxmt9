#include "dxmt9_shader_sources.hpp"

#include "dxmt9/core.hpp"

#include <sstream>
#include <utility>

namespace dxmt9::shaders {

u64 makeHash(const std::string& source) {
  return core::hashString(source);
}

std::string makeGenericVertexSource(u64 variantHash) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; };\n";
  out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]]) {\n";
  out << "  VSOut out;\n";
  out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
  out << "  out.position = float4(p[vid % 3], 0.0, 1.0);\n";
  out << "  return out;\n";
  out << "}\n";
  out << "// variant " << variantHash << "\n";
  return out.str();
}

std::string makeGenericFragmentSource(const core::ColorRGBA& color, u64 variantHash) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; };\n";
  out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]]) {\n";
  out << "  (void)in;\n";
  out << "  return float4(" << color.r << "f, " << color.g << "f, " << color.b << "f, " << color.a << "f);\n";
  out << "}\n";
  out << "// variant " << variantHash << "\n";
  return out.str();
}

std::string makeTexturedVertexSource(u64 variantHash) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; float2 uv; };\n";
  out << "vertex VSOut dxmt9_vs(uint vid [[vertex_id]]) {\n";
  out << "  VSOut out;\n";
  out << "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n";
  out << "  float2 uv[3] = { float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0) };\n";
  out << "  out.position = float4(p[vid % 3], 0.0, 1.0);\n";
  out << "  out.uv = uv[vid % 3];\n";
  out << "  return out;\n";
  out << "}\n";
  out << "// variant " << variantHash << "\n";
  return out.str();
}

std::string makeTexturedFragmentSource(u64 variantHash, bool forceOpaqueAlpha) {
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; float2 uv; };\n";
  out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]], texture2d<float> tex0 [[texture(0)]], sampler samp0 [[sampler(0)]]) {\n";
  out << "  float4 color = tex0.sample(samp0, in.uv);\n";
  if (forceOpaqueAlpha) {
    out << "  color.a = 1.0;\n";
  }
  out << "  return color;\n";
  out << "}\n";
  out << "// variant " << variantHash << "\n";
  return out.str();
}

WMT::Reference<WMT::Library> makeLibrary(WMT::Device& device, const std::string& source) {
  WMT::Error error{};
  auto lib = device.newLibraryFromSource(source.c_str(), error);
  if (!lib) {
    return {};
  }
  return lib;
}

void initShaderArchive(WMT::Device& device, const std::string& path,
                       WMT::Reference<WMT::BinaryArchive>& archiveOut) {
  WMT::Error err{};
  auto archive = device.newBinaryArchive(path.c_str(), err);
  archiveOut = std::move(archive);
}

void persistShaderArchive(WMT::BinaryArchive& archive, const std::string& path) {
  if (!archive || path.empty()) {
    return;
  }
  WMT::Error err{};
  archive.serialize(path.c_str(), err);
}

}  // namespace dxmt9::shaders
