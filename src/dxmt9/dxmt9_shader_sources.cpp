#include "dxmt9_shader_sources.hpp"

#include "dxmt9/core.hpp"
#include "dxmt9_archive_prewarm.hpp"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

namespace dxmt9::shaders {

bool vsoutTrimEnabled() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT9_TRIM_UNUSED_VARYINGS");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return value;
}

bool vsoutProbeDropPointSizeEnabled() {
  const char* env = std::getenv("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE");
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

bool vsoutProbePositionOnlyEnabled() {
  const char* env = std::getenv("DXMT9_PROBE_POSITION_ONLY_VSOUT");
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

bool vsoutProbeHalfEnabled() {
  const char* env = std::getenv("DXMT9_PROBE_HALF_VSOUT");
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

VSOutLayout minimalVSOutLayout() {
  VSOutLayout layout{};
  layout.texcoordMask = 0x1u;
  layout.color = false;
  layout.secondaryColor = false;
  layout.fogFactor = false;
  layout.pointSize = false;
  return layout;
}

VSOutLayout positionOnlyVSOutLayout() {
  VSOutLayout layout{};
  layout.texcoordMask = 0u;
  layout.color = false;
  layout.secondaryColor = false;
  layout.fogFactor = false;
  layout.pointSize = false;
  return layout;
}

VSOutLayout applyVSOutProbeOverrides(VSOutLayout layout) {
  if (vsoutProbePositionOnlyEnabled()) {
    return positionOnlyVSOutLayout();
  }
  if (vsoutProbeDropPointSizeEnabled()) {
    layout.pointSize = false;
  }
  return layout;
}

std::uint32_t vsoutLayoutKey(const VSOutLayout& layout) {
  std::uint32_t key = layout.texcoordMask & 0xffu;
  if (layout.color) key |= 1u << 8;
  if (layout.secondaryColor) key |= 1u << 9;
  if (layout.fogFactor) key |= 1u << 10;
  if (layout.pointSize) key |= 1u << 11;
  return key;
}

bool vsoutEmitTexcoord(const VSOutLayout& layout, std::size_t index) {
  return index < core::kMaxTextureStages &&
         ((layout.texcoordMask & (1u << index)) != 0u);
}

bool vsoutEmitColor(const VSOutLayout& layout) { return layout.color; }

bool vsoutEmitSecondaryColor(const VSOutLayout& layout) {
  return layout.secondaryColor;
}

bool vsoutEmitFogFactor(const VSOutLayout& layout) { return layout.fogFactor; }

bool vsoutEmitPointSize(const VSOutLayout& layout) { return layout.pointSize; }

std::string centroidAttribute(bool enabled) {
  return enabled ? " [[centroid_perspective]]" : "";
}

const char* vsoutVectorType(const ShaderPreludeOptions& options) {
  return options.halfVSOut ? "half4" : "float4";
}

const char* vsoutScalarType(const ShaderPreludeOptions& options) {
  return options.halfVSOut ? "half" : "float";
}

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

std::string makeGammaApplyFragmentSource(u64 variantHash, bool forceOpaqueAlpha) {
  // The GammaUniforms layout mirrors core::GammaRamp / D3DGAMMARAMP byte-
  // for-byte (3 * 256 * u16, total 1.5 KB). setFragmentBytes copies the
  // POD straight from SwapDesc::gammaRamp — no MTLBuffer needed. The 8-bit
  // quantize is intentional: D3D9 gamma ramps are documented as a 256-entry
  // LUT indexed by the source channel scaled to 0..255, so a deeper sample
  // doesn't gain precision (the upper 8 bits already pick the entry).
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  out << "struct VSOut { float4 position [[position]]; float2 uv; };\n";
  out << "struct GammaUniforms {\n";
  out << "  ushort red[" << core::kMaxGammaRampEntries << "];\n";
  out << "  ushort green[" << core::kMaxGammaRampEntries << "];\n";
  out << "  ushort blue[" << core::kMaxGammaRampEntries << "];\n";
  out << "};\n";
  out << "fragment float4 dxmt9_fs(VSOut in [[stage_in]],\n";
  out << "                          texture2d<float> tex0 [[texture(0)]],\n";
  out << "                          sampler samp0 [[sampler(0)]],\n";
  out << "                          constant GammaUniforms& ramp [[buffer(0)]]) {\n";
  out << "  float4 color = tex0.sample(samp0, in.uv);\n";
  out << "  uint ri = uint(saturate(color.r) * 255.0 + 0.5);\n";
  out << "  uint gi = uint(saturate(color.g) * 255.0 + 0.5);\n";
  out << "  uint bi = uint(saturate(color.b) * 255.0 + 0.5);\n";
  out << "  float4 mapped;\n";
  out << "  mapped.r = float(ramp.red[ri])   * (1.0 / 65535.0);\n";
  out << "  mapped.g = float(ramp.green[gi]) * (1.0 / 65535.0);\n";
  out << "  mapped.b = float(ramp.blue[bi])  * (1.0 / 65535.0);\n";
  if (forceOpaqueAlpha) {
    out << "  mapped.a = 1.0;\n";
  } else {
    out << "  mapped.a = color.a;\n";
  }
  out << "  return mapped;\n";
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
  // R-BACK-3.10 / specs/backend/spec.md §6.1 — writers acquire LOCK_EX
  // before serializeToURL: so a concurrent dxmt9 process's LOCK_SH
  // reader (dxmt9_archive_prewarm.cpp's acquireSharedLock) never
  // observes a torn write. serializeToURL:'s own temp-file-plus-rename
  // behavior stays the atomicity mechanism for the file content itself;
  // this lock only adds the missing cross-process mutual exclusion
  // around when that rename may happen. Best-effort: if the lock can't
  // be acquired within the bounded retry window, skip this save attempt
  // rather than block indefinitely — a later save (mid-session or the
  // next process's shutdown) will retry.
  const int lockFd = archive_prewarm::acquireArchiveWriteLock(path);
  if (lockFd < 0) {
    return;
  }
  WMT::Error err{};
  archive.serialize(path.c_str(), err);
  archive_prewarm::releaseArchiveWriteLock(lockFd);
}

std::string makeShaderPrelude(const ShaderPreludeOptions& options) {
  using namespace dxmt9::core;
  std::ostringstream out;
  out << "#include <metal_stdlib>\nusing namespace metal;\n";
  // Per-category uniform structs. Layouts are routed by stage and update
  // cadence: see specs/backend/draw-uniforms.
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
  out << "  float4 ffpWorldView[4];\n";
  out << "  float4 ffpNormalMatrix[4];\n";
  out << "  float4 materialEmissive;\n";
  out << "  float4 materialAmbient;\n";
  out << "  float4 materialDiffuse;\n";
  out << "  float4 materialSpecular;\n";
  out << "  float4 globalAmbient;\n";
  out << "  float4 materialPower;\n";
  out << "  float4 lightDiffuse[" << kMaxLights << "];\n";
  out << "  float4 lightSpecular[" << kMaxLights << "];\n";
  out << "  float4 lightAmbient[" << kMaxLights << "];\n";
  out << "  float4 lightDirection[" << kMaxLights << "];\n";
  // Point/Spot: xyz=position, w=range. Spot: lightDirection holds the
  // spot-cone axis (D3D9 reuses the directional `Direction` field).
  out << "  float4 lightPosition[" << kMaxLights << "];\n";
  // Point/Spot atten poly + spot falloff exponent: x=A0, y=A1, z=A2,
  // w=falloff.
  out << "  float4 lightAttenuation[" << kMaxLights << "];\n";
  // Spot only: x=cos(theta/2), y=cos(phi/2), zw=reserved.
  out << "  float4 lightSpotCone[" << kMaxLights << "];\n";
  out << "  float4 ffpBlendWorldViewProj[4][4];\n";
  out << "  float4 ffpTextureTransforms[" << kMaxTextureStages << "][4];\n";
  out << "  float4 clipPlanes[" << kMaxClipPlanes << "];\n";
  out << "  float2 halfPixelFixup;\n";
  out << "  float2 viewportOrigin;\n";
  out << "  float2 viewportSize;\n";
  out << "  float fogStart;\n";
  out << "  float fogEnd;\n";
  out << "  float fogDensity;\n";
  out << "  uint fogMode;\n";
  out << "  uint rangeFog;\n";
  out << "  uint clipPlaneMask;\n";
  out << "  float pointSize;\n";
  out << "  float pointSizeMin;\n";
  out << "  float pointSizeMax;\n";
  out << "  float pointScaleA;\n";
  out << "  float pointScaleB;\n";
  out << "  float pointScaleC;\n";
  out << "};\n";
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
  out << "inline float dxmt9_compute_fog_factor(uint fogMode, float fogDepth,\n";
  out << "                                      float fogStart, float fogEnd,\n";
  out << "                                      float fogDensity) {\n";
  out << "  float fog = 1.0f;\n";
  out << "  if (fogMode == 1u) {\n";
  out << "    fog = clamp((fogEnd - fogDepth) / max(fogEnd - fogStart, 1.0e-6f),\n";
  out << "                0.0f, 1.0f);\n";
  out << "  } else if (fogMode == 2u) {\n";
  out << "    fog = clamp(exp(-fogDensity * fogDepth), 0.0f, 1.0f);\n";
  out << "  } else if (fogMode == 3u) {\n";
  out << "    float d = fogDensity * fogDepth;\n";
  out << "    fog = clamp(exp(-(d * d)), 0.0f, 1.0f);\n";
  out << "  }\n";
  out << "  return fog;\n";
  out << "}\n";
  out << "inline float4 dxmt9_apply_fog(float4 color, constant FfpPsConsts& ffpPs,\n";
  out << "                              float fogDepth, float vertexFog) {\n";
  out << "  float fog = ffpPs.fogSource != 0u ? clamp(vertexFog, 0.0f, 1.0f) :\n";
  out << "      dxmt9_compute_fog_factor(ffpPs.fogMode, fogDepth,\n";
  out << "                               ffpPs.fogStart, ffpPs.fogEnd,\n";
  out << "                               ffpPs.fogDensity);\n";
  out << "  return float4(mix(ffpPs.fogColor.rgb, color.rgb, fog), color.a);\n";
  out << "}\n";
  out << "struct DrawVolatile {\n";
  out << "  int vertexBaseIndex;\n";
  out << "  uint vertexStreamOffset;\n";
  out << "  uint vertexStreamStride;\n";
  out << "  uint _pad;\n";
  out << "};\n";
  // H228 — per-draw fragment alpha-test immediate (host struct
  // dxmt9::state::FsVolatile, setFragmentBytes at fragment buffer 5). The
  // alpha-test tail is a single shader variant reading this at runtime;
  // alphaTest is 0 for off, else the D3DCMPFUNC (1..8).
  out << "struct FsVolatile {\n";
  out << "  uint alphaTest;\n";
  out << "  float alphaRef;\n";
  out << "};\n";
  out << "struct VSOut {\n";
  out << "  float4 position [[position]];\n";
  if (vsoutEmitColor(options.vsOutLayout)) {
    out << "  " << vsoutVectorType(options) << " color"
        << centroidAttribute(options.centroidColor) << ";\n";
  }
  if (vsoutEmitSecondaryColor(options.vsOutLayout)) {
    out << "  " << vsoutVectorType(options) << " secondaryColor"
        << centroidAttribute(options.centroidSecondaryColor) << ";\n";
  }
  for (size_t i = 0; i < core::kMaxTextureStages; ++i) {
    if (!vsoutEmitTexcoord(options.vsOutLayout, i)) {
      continue;
    }
    const bool centroid = (options.centroidTexcoordMask & (1u << i)) != 0u;
    out << "  " << vsoutVectorType(options) << " texcoord" << i
        << centroidAttribute(centroid) << ";\n";
  }
  if (vsoutEmitFogFactor(options.vsOutLayout)) {
    out << "  " << vsoutScalarType(options) << " fogFactor"
        << centroidAttribute(options.centroidFogFactor) << ";\n";
  }
  if (vsoutEmitPointSize(options.vsOutLayout)) {
    out << "  float pointSize [[point_size]];\n";
  }
  if (options.withClipDistances) {
    // Apple Metal (Apple7+) only honours a single `[[clip_distance]]`
    // declaration per vertex output. Both the array form
    // (`float clipDistance [[clip_distance]] [N]`) and multiple
    // separately-attributed scalars (`float a [[clip_distance(0)]];
    // float b [[clip_distance(1)]];`) silently clip every fragment
    // regardless of the per-slot value. The only working form is one
    // scalar `[[clip_distance]]` field per vertex output.
    //
    // D3D9 supports up to 6 clip planes via D3DRS_CLIPPLANEENABLE.
    // dxmt9 collapses the per-plane half-space tests into the single
    // Apple-supported slot by writing `min(d_i)` over the enabled
    // planes — a fragment is clipped iff ANY plane's dot is < 0, which
    // is equivalent to `min(d_i) < 0`. See `makeFfpVertexSource` /
    // `dxmt9_shader_metal_ir.cpp` for the runtime min-fold. Wine
    // `clip_planes_test` regression coverage:
    // tests/shader_runner/corpus/render_state/dxmt9_clip_plane_halfspace_readback.shader_test
    out << "  float clipDistance0 [[clip_distance]];\n";
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
  out << "                                       float4 temp, float4 stageConstant) {\n";
  out << "  float4 value = current;\n";
  out << "  switch (arg & 0x0fu) {\n";
  out << "    case 0u: value = diffuse; break;\n";
  out << "    case 1u: value = current; break;\n";
  out << "    case 2u: value = texture; break;\n";
  out << "    case 3u: value = tfactor; break;\n";
  out << "    case 4u: value = specular; break;\n";
  out << "    case 5u: value = temp; break;\n";
  out << "    case 6u: value = stageConstant; break;\n";
  out << "    default: value = current; break;\n";
  out << "  }\n";
  out << "  return dxmt9_apply_texture_arg_flags(value, arg);\n";
  out << "}\n";
  out << "inline float4 dxmt9_select_texcoord(VSOut in, uint index) {\n";
  out << "  switch (index) {\n";
  bool hasTexcoord0 = false;
  for (size_t i = 0; i < core::kMaxTextureStages; ++i) {
    if (!vsoutEmitTexcoord(options.vsOutLayout, i)) {
      continue;
    }
    hasTexcoord0 = hasTexcoord0 || i == 0u;
    out << "    case " << i << "u: return "
        << (options.halfVSOut ? "float4(" : "") << "in.texcoord" << i
        << (options.halfVSOut ? ")" : "") << ";\n";
  }
  if (hasTexcoord0) {
    out << "    default: return " << (options.halfVSOut ? "float4(" : "")
        << "in.texcoord0" << (options.halfVSOut ? ")" : "") << ";\n";
  } else {
    out << "    default: return float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
  }
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
  out << "    case 24u: {\n";
  out << "      float value = clamp(dot(arg1.rgb * 2.0f - 1.0f, arg2.rgb * 2.0f - 1.0f), 0.0f, 1.0f);\n";
  out << "      return float4(value, value, value, value);\n";
  out << "    }\n";
  out << "    case 26u: return mix(arg2, arg1, current);\n";
  out << "    default: return arg1;\n";
  out << "  }\n";
  out << "}\n";
  out << "inline float2 dxmt9_apply_texture_transform(float4 coord,\n";
  out << "                                            constant FfpVsConsts& ffpVs,\n";
  out << "                                            uint stage,\n";
  out << "                                            uint flags) {\n";
  out << "  const uint count = flags & 0xffu;\n";
  out << "  if (count == 0u) {\n";
  out << "    return coord.xy;\n";
  out << "  }\n";
  out << "  float4 transformed = float4(dot(ffpVs.ffpTextureTransforms[stage][0], coord),\n";
  out << "                              dot(ffpVs.ffpTextureTransforms[stage][1], coord),\n";
  out << "                              dot(ffpVs.ffpTextureTransforms[stage][2], coord),\n";
  out << "                              dot(ffpVs.ffpTextureTransforms[stage][3], coord));\n";
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

std::string makeShaderPrelude(bool withClipDistances) {
  ShaderPreludeOptions options;
  options.withClipDistances = withClipDistances;
  return makeShaderPrelude(options);
}

std::string makeShaderPreludeArgbufHybrid(const ShaderPreludeOptions& options) {
  using namespace dxmt9::core;
  // Reuse the Stage 1 prelude as the canonical source of truth for the
  // five per-category uniform struct layouts + helper inlines. Append
  // the Stage 2 ArgbufLayout wrapper struct so shaders read from the
  // slot-30 argument buffer instead of dedicated slots 0/3. The
  // Stage 1 struct definitions remain visible (FfpVsConsts is
  // referenced by helpers like dxmt9_apply_texture_transform that take
  // it by `constant FfpVsConsts&`) — the wrapper only changes the
  // entry-point binding shape.
  std::ostringstream out;
  out << makeShaderPrelude(options);
  // ArgbufLayout mirrors the per-encoder argbuf shape (spec.md §11.2).
  // [[id(N)]] attributes pin the descriptor indices so the host-side
  // MTLArgumentEncoder layout stays compatible across MSL versions.
  // The argbuf carries only the four per-frequency constant-buffer
  // pointers — texture and sampler resources continue to use the direct
  // [[texture(N)]] / [[sampler(N)]] binding lane (the validated Stage 1
  // resource path).
  out << "struct ArgbufLayout {\n";
  out << "  constant VsConsts*    vsConsts [[id(0)]];\n";
  out << "  constant FfpVsConsts* ffpVs    [[id(1)]];\n";
  out << "  constant PsConsts*    psConsts [[id(2)]];\n";
  out << "  constant FfpPsConsts* ffpPs    [[id(3)]];\n";
  out << "};\n";
  return out.str();
}

std::string makeShaderPreludeArgbufHybrid(bool withClipDistances) {
  ShaderPreludeOptions options;
  options.withClipDistances = withClipDistances;
  return makeShaderPreludeArgbufHybrid(options);
}

bool argbufResourceArrayEnabled() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT9_ARGBUF_RESOURCE_ARRAY");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return value;
}

std::string makeShaderPreludeArgbufResourceArray(const ShaderPreludeOptions& options) {
  // Reuse the constants-only Stage 2 prelude verbatim for the five uniform
  // struct definitions + helper inlines, then re-emit the ArgbufLayout
  // struct EXTENDED with the texture/sampler arrays. We intentionally do
  // not call makeShaderPreludeArgbufHybrid (which already emits a 4-pointer
  // ArgbufLayout) — a single MSL TU cannot declare `struct ArgbufLayout`
  // twice. Instead we emit makeShaderPrelude (no ArgbufLayout) and then the
  // extended struct here.
  std::ostringstream out;
  out << makeShaderPrelude(options);
  // ArgbufLayout mirrors the per-encoder argbuf shape with texture/sampler
  // arrays appended (spec.md §11.2, resource-array sub-mode). [[id(N)]]
  // attributes pin the descriptor indices so the host MTLArgumentEncoder
  // descriptor table (buildResourceArrayArgumentDescriptors) and the MSL
  // struct stay layout-compatible. The texture array is homogeneously typed
  // `texture2d<float>`; per-shader cube/volume aliasing is emitted by the
  // entry-point alias block (the gpuResourceID is type-agnostic on the wire).
  out << "struct ArgbufLayout {\n";
  out << "  constant VsConsts*    vsConsts [[id(0)]];\n";
  out << "  constant FfpVsConsts* ffpVs    [[id(1)]];\n";
  out << "  constant PsConsts*    psConsts [[id(2)]];\n";
  out << "  constant FfpPsConsts* ffpPs    [[id(3)]];\n";
  out << "  array<texture2d<float>, " << kArgbufResourceArrayStageCount
      << "> textures [[id(" << kArgbufResourceArrayTextureBaseId << ")]];\n";
  out << "  array<sampler, " << kArgbufResourceArrayStageCount
      << "> samplers [[id(" << kArgbufResourceArraySamplerBaseId << ")]];\n";
  out << "};\n";
  return out.str();
}

std::string makeShaderPreludeArgbufResourceArray(bool withClipDistances) {
  ShaderPreludeOptions options;
  options.withClipDistances = withClipDistances;
  return makeShaderPreludeArgbufResourceArray(options);
}

}  // namespace dxmt9::shaders
