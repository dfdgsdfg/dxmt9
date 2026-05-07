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
  archiveOut = initShaderArchive(device, path);
}

WMT::Reference<WMT::BinaryArchive> initShaderArchive(WMT::Device device, const std::string& path) {
  if (!device) {
    return {};
  }
  WMT::Error err{};
  return device.newBinaryArchive(path.c_str(), err);
}

void persistShaderArchive(WMT::BinaryArchive& archive, const std::string& path) {
  if (!archive || path.empty()) {
    return;
  }
  WMT::Error err{};
  archive.serialize(path.c_str(), err);
}

std::string makeShaderPrelude(bool withClipDistances) {
  using namespace dxmt9::core;
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct DrawUniforms {\n";
  out << "  float4 vsFloatConst[" << kMaxVertexConstants << "];\n";
  out << "  int4 vsIntConst[" << kMaxIntegerConstants << "];\n";
  out << "  uint vsBoolConst[" << kMaxBoolConstants << "];\n";
  out << "  float4 ffpWorldViewProj[4];\n";
  out << "  float4 ffpTextureTransforms[" << kMaxTextureStages << "][4];\n";
  out << "  float4 psFloatConst[" << kMaxPixelConstants << "];\n";
  out << "  int4 psIntConst[" << kMaxIntegerConstants << "];\n";
  out << "  uint psBoolConst[" << kMaxBoolConstants << "];\n";
  out << "  float4 clipPlanes[6];\n";
  out << "  float2 halfPixelFixup;\n";
  out << "  float2 viewportOrigin;\n";
  out << "  float2 viewportSize;\n";
  out << "  float4 textureFactor;\n";
  out << "  float alphaRef;\n";
  out << "  float fogStart;\n";
  out << "  float fogEnd;\n";
  out << "  float fogDensity;\n";
  out << "  uint vertexStreamOffset;\n";
  out << "  uint vertexStreamStride;\n";
  out << "  int vertexBaseIndex;\n";
  out << "  uint clipPlaneMask;\n";
  out << "  uint alphaTestEnable;\n";
  out << "  uint alphaTestFunc;\n";
  out << "  uint fogMode;\n";
  out << "};\n";
  // New per-category uniform structs. Layouts are routed by stage and update
  // cadence: see specs/backend/draw-uniforms. The legacy DrawUniforms struct
  // above stays defined while the encoder still binds it; the translated draw
  // shaders below reference these category structs instead.
  out << "struct VsConsts {\n";
  out << "  float4 vsFloatConst[" << kMaxVertexConstants << "];\n";
  out << "  int4 vsIntConst[" << kMaxIntegerConstants << "];\n";
  out << "  uint vsBoolConst[" << kMaxBoolConstants << "];\n";
  out << "};\n";
  out << "struct PsConsts {\n";
  out << "  float4 psFloatConst[" << kMaxPixelConstants << "];\n";
  out << "  int4 psIntConst[" << kMaxIntegerConstants << "];\n";
  out << "  uint psBoolConst[" << kMaxBoolConstants << "];\n";
  out << "};\n";
  out << "struct FfpVsConsts {\n";
  out << "  float4 ffpWorldViewProj[4];\n";
  out << "  float4 ffpTextureTransforms[" << kMaxTextureStages << "][4];\n";
  out << "  float4 clipPlanes[" << kMaxClipPlanes << "];\n";
  out << "  float2 halfPixelFixup;\n";
  out << "  float2 viewportOrigin;\n";
  out << "  float2 viewportSize;\n";
  out << "  uint clipPlaneMask;\n";
  out << "};\n";
  out << "struct FfpPsConsts {\n";
  out << "  float4 textureFactor;\n";
  out << "  float alphaRef;\n";
  out << "  float fogStart;\n";
  out << "  float fogEnd;\n";
  out << "  float fogDensity;\n";
  out << "  uint alphaTestEnable;\n";
  out << "  uint alphaTestFunc;\n";
  out << "  uint fogMode;\n";
  out << "};\n";
  out << "struct DrawVolatile {\n";
  out << "  int vertexBaseIndex;\n";
  out << "  uint vertexStreamOffset;\n";
  out << "  uint vertexStreamStride;\n";
  out << "  uint _pad;\n";
  out << "};\n";
  out << "struct VSOut {\n";
  out << "  float4 position [[position]];\n";
  out << "  float4 color;\n";
  out << "  float4 secondaryColor;\n";
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    out << "  float4 texcoord" << i << ";\n";
  }
  out << "  float fogFactor;\n";
  out << "  float pointSize [[point_size]];\n";
  if (withClipDistances) {
    out << "  float clipDistance [[clip_distance]] [6];\n";
  }
  out << "};\n";
  out << "inline float4 dxmt9_merge(float4 current, float4 next, uint mask) {\n";
  out << "  return float4((mask & 1u) != 0u ? next.x : current.x,\n";
  out << "                  (mask & 2u) != 0u ? next.y : current.y,\n";
  out << "                  (mask & 4u) != 0u ? next.z : current.z,\n";
  out << "                  (mask & 8u) != 0u ? next.w : current.w);\n";
  out << "}\n";
  out << "inline float dxmt9_load_f32(const device uchar* base, uint offset) {\n";
  out << "  return as_type<float>(*reinterpret_cast<const device uint*>(base + offset));\n";
  out << "}\n";
  out << "inline float2 dxmt9_load_f32x2(const device uchar* base, uint offset) {\n";
  out << "  return float2(dxmt9_load_f32(base, offset), dxmt9_load_f32(base, offset + 4u));\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_f32x4(const device uchar* base, uint offset) {\n";
  out << "  return float4(dxmt9_load_f32(base, offset), dxmt9_load_f32(base, offset + 4u),\n";
  out << "                dxmt9_load_f32(base, offset + 8u), dxmt9_load_f32(base, offset + 12u));\n";
  out << "}\n";
  out << "inline float3 dxmt9_load_f32x3(const device uchar* base, uint offset) {\n";
  out << "  return float3(dxmt9_load_f32(base, offset), dxmt9_load_f32(base, offset + 4u),\n";
  out << "                dxmt9_load_f32(base, offset + 8u));\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_d3dcolor(const device uchar* base, uint offset) {\n";
  out << "  const uint raw = *reinterpret_cast<const device uint*>(base + offset);\n";
  out << "  return float4(float((raw >> 16) & 0xffu), float((raw >> 8) & 0xffu), float(raw & 0xffu),\n";
  out << "                float((raw >> 24) & 0xffu)) / 255.0f;\n";
  out << "}\n";
  out << "inline float2 dxmt9_load_i16x2(const device uchar* base, uint offset) {\n";
  out << "  const device short* p = reinterpret_cast<const device short*>(base + offset);\n";
  out << "  return float2(float(p[0]), float(p[1]));\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_i16x4(const device uchar* base, uint offset) {\n";
  out << "  const device short* p = reinterpret_cast<const device short*>(base + offset);\n";
  out << "  return float4(float(p[0]), float(p[1]), float(p[2]), float(p[3]));\n";
  out << "}\n";
  out << "inline float2 dxmt9_load_i16x2_snorm(const device uchar* base, uint offset) {\n";
  out << "  return clamp(dxmt9_load_i16x2(base, offset) / 32767.0f, float2(-1.0f), float2(1.0f));\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_i16x4_snorm(const device uchar* base, uint offset) {\n";
  out << "  return clamp(dxmt9_load_i16x4(base, offset) / 32767.0f, float4(-1.0f), float4(1.0f));\n";
  out << "}\n";
  out << "inline float2 dxmt9_load_u16x2_unorm(const device uchar* base, uint offset) {\n";
  out << "  const device ushort* p = reinterpret_cast<const device ushort*>(base + offset);\n";
  out << "  return float2(float(p[0]), float(p[1])) / 65535.0f;\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_u16x4_unorm(const device uchar* base, uint offset) {\n";
  out << "  const device ushort* p = reinterpret_cast<const device ushort*>(base + offset);\n";
  out << "  return float4(float(p[0]), float(p[1]), float(p[2]), float(p[3])) / 65535.0f;\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_u8x4(const device uchar* base, uint offset) {\n";
  out << "  return float4(float(base[offset + 0u]), float(base[offset + 1u]),\n";
  out << "                float(base[offset + 2u]), float(base[offset + 3u]));\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_u8x4_unorm(const device uchar* base, uint offset) {\n";
  out << "  return dxmt9_load_u8x4(base, offset) / 255.0f;\n";
  out << "}\n";
  out << "inline float dxmt9_load_f16(const device uchar* base, uint offset) {\n";
  out << "  return float(as_type<half>(*reinterpret_cast<const device ushort*>(base + offset)));\n";
  out << "}\n";
  out << "inline float2 dxmt9_load_f16x2(const device uchar* base, uint offset) {\n";
  out << "  return float2(dxmt9_load_f16(base, offset), dxmt9_load_f16(base, offset + 2u));\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_f16x4(const device uchar* base, uint offset) {\n";
  out << "  return float4(dxmt9_load_f16(base, offset), dxmt9_load_f16(base, offset + 2u),\n";
  out << "                dxmt9_load_f16(base, offset + 4u), dxmt9_load_f16(base, offset + 6u));\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_udec3(const device uchar* base, uint offset) {\n";
  out << "  const uint raw = *reinterpret_cast<const device uint*>(base + offset);\n";
  out << "  return float4(float(raw & 0x3ffu), float((raw >> 10) & 0x3ffu), float((raw >> 20) & 0x3ffu), 1.0f);\n";
  out << "}\n";
  out << "inline float dxmt9_snorm10(uint raw) {\n";
  out << "  int value = int(raw & 0x3ffu);\n";
  out << "  if (value >= 512) value -= 1024;\n";
  out << "  return clamp(float(value) / 511.0f, -1.0f, 1.0f);\n";
  out << "}\n";
  out << "inline float4 dxmt9_load_dec3n(const device uchar* base, uint offset) {\n";
  out << "  const uint raw = *reinterpret_cast<const device uint*>(base + offset);\n";
  out << "  return float4(dxmt9_snorm10(raw), dxmt9_snorm10(raw >> 10), dxmt9_snorm10(raw >> 20), 1.0f);\n";
  out << "}\n";
  out << "inline float4 dxmt9_apply_texture_arg_flags(float4 value, uint arg) {\n";
  out << "  if ((arg & 0x20u) != 0u) value = value.aaaa;\n";
  out << "  if ((arg & 0x10u) != 0u) value = float4(1.0f) - value;\n";
  out << "  return value;\n";
  out << "}\n";
  out << "inline float4 dxmt9_select_texture_arg(uint arg, float4 current, float4 diffuse,\n";
  out << "                                       float4 specular, float4 texture, float4 tfactor,\n";
  out << "                                       float4 temp) {\n";
  out << "  float4 value = current;\n";
  out << "  switch (arg & 0x0fu) {\n";
  out << "    case 0u: value = diffuse; break;\n";
  out << "    case 1u: value = current; break;\n";
  out << "    case 2u: value = texture; break;\n";
  out << "    case 3u: value = tfactor; break;\n";
  out << "    case 4u: value = specular; break;\n";
  out << "    case 5u: value = temp; break;\n";
  out << "    default: value = current; break;\n";
  out << "  }\n";
  out << "  return dxmt9_apply_texture_arg_flags(value, arg);\n";
  out << "}\n";
  out << "inline float4 dxmt9_select_texcoord(VSOut in, uint index) {\n";
  out << "  switch (index) {\n";
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    out << "    case " << i << "u: return in.texcoord" << i << ";\n";
  }
  out << "    default: return in.texcoord0;\n";
  out << "  }\n";
  out << "}\n";
  out << "inline float4 dxmt9_apply_texture_op(uint op, float4 arg1, float4 arg2, float4 current) {\n";
  out << "  switch (op) {\n";
  out << "    case 1u: return current;\n";
  out << "    case 2u: return arg1;\n";
  out << "    case 3u: return arg2;\n";
  out << "    case 4u: return arg1 * arg2;\n";
  out << "    case 5u: return saturate(arg1 * arg2 * 2.0f);\n";
  out << "    case 6u: return saturate(arg1 * arg2 * 4.0f);\n";
  out << "    case 7u: return saturate(arg1 + arg2);\n";
  out << "    case 8u: return saturate(arg1 + arg2 - float4(0.5f));\n";
  out << "    case 9u: return saturate((arg1 + arg2 - float4(0.5f)) * 2.0f);\n";
  out << "    case 10u: return saturate(arg1 - arg2);\n";
  out << "    case 11u: return saturate(arg1 + arg2 - arg1 * arg2);\n";
  out << "    case 26u: return mix(arg2, arg1, current);\n";
  out << "    default: return arg1;\n";
  out << "  }\n";
  out << "}\n";
  out << "inline float2 dxmt9_apply_texture_transform(float4 coord,\n";
  out << "                                            constant DrawUniforms& uniforms,\n";
  out << "                                            uint stage,\n";
  out << "                                            uint flags) {\n";
  out << "  const uint count = flags & 0xffu;\n";
  out << "  if (count == 0u) {\n";
  out << "    return coord.xy;\n";
  out << "  }\n";
  out << "  float4 transformed = float4(dot(uniforms.ffpTextureTransforms[stage][0], coord),\n";
  out << "                              dot(uniforms.ffpTextureTransforms[stage][1], coord),\n";
  out << "                              dot(uniforms.ffpTextureTransforms[stage][2], coord),\n";
  out << "                              dot(uniforms.ffpTextureTransforms[stage][3], coord));\n";
  out << "  if ((flags & 0x100u) != 0u && count >= 2u) {\n";
  out << "    const uint divisorIndex = min(count - 1u, 3u);\n";
  out << "    const float q = transformed[divisorIndex];\n";
  out << "    if (fabs(q) > 1.0e-8f) {\n";
  out << "      transformed.xy /= q;\n";
  out << "    } else {\n";
  out << "      transformed.xy = float2(0.0f);\n";
  out << "    }\n";
  out << "  }\n";
  out << "  if (count == 1u) {\n";
  out << "    return float2(transformed.x, 0.0f);\n";
  out << "  }\n";
  out << "  return transformed.xy;\n";
  out << "}\n";
  return out.str();
}

}  // namespace dxmt9::shaders
