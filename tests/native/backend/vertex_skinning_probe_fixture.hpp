#pragma once

#include <string>

// Synthetic two-bone program with independently populated identity matrices.
// This is not copied application shader input and is not an original oracle.
inline std::string vertexSkinningProbeFixture() {
  return R"MSL(#include <metal_stdlib>
using namespace metal;
struct VSOut { float4 position [[position]]; };
struct Ffp { float2 halfPixelFixup; };
vertex VSOut dxmt9_vs(uint vid [[vertex_id]], constant float4* cFloat [[buffer(0)]],
                     const device float4* vertices [[buffer(1)]], constant Ffp& ffpVs [[buffer(3)]]) {
  VSOut out;
  float4 vin0 = vertices[vid];
  float4 vin1 = float4(0.25, 0.5, 0, 1);
  float4 vin2 = float4(0, 1, 0, 0);
  float4 vin3 = float4(0.25, 0.75, 0, 1);
  int4 a0 = int4(vin2 * 3.0f);
  uint stride = 32, vertexIndex = vid, base = vid * stride;
  const float4 dxmt9_cdef196 = float4(3,0,0,0);
  float4 r[3];
  // mul r0, c0, v3
  r[0] = (((clamp(a0.y + 0, 0, 255)) == 196 ? dxmt9_cdef196 : cFloat[clamp(a0.y + 0, 0, 255)]) * vin3.y);
  r[0] = (((clamp(a0.x + 0, 0, 255)) == 196 ? dxmt9_cdef196 : cFloat[clamp(a0.x + 0, 0, 255)]) * vin3.x + r[0]);
  // dp4 r0, v0, r0
  r[0].x = dot(vin0, r[0]);
  r[1] = (((clamp(a0.y + 1, 0, 255)) == 196 ? dxmt9_cdef196 : cFloat[clamp(a0.y + 1, 0, 255)]) * vin3.y);
  r[2] = (((clamp(a0.x + 1, 0, 255)) == 196 ? dxmt9_cdef196 : cFloat[clamp(a0.x + 1, 0, 255)]) * vin3.x + r[1]);
  r[0].y = dot(vin0, r[2]);
  r[1] = (((clamp(a0.y + 2, 0, 255)) == 196 ? dxmt9_cdef196 : cFloat[clamp(a0.y + 2, 0, 255)]) * vin3.y);
  r[1] = (((clamp(a0.x + 2, 0, 255)) == 196 ? dxmt9_cdef196 : cFloat[clamp(a0.x + 2, 0, 255)]) * vin3.x + r[1]);
  // dp4 r0, v0, r1
  r[0].z = dot(vin0, r[1]);
  r[0].w = vin0.w;
  float4 outPosition;
  outPosition.x = dot(r[0], cFloat[192]);
  outPosition.y = dot(r[0], cFloat[193]);
  outPosition.z = dot(r[0], cFloat[194]);
  outPosition.w = dot(r[0], cFloat[195]);
  out.position = outPosition;
  out.position.xy += ffpVs.halfPixelFixup * out.position.w;
  return out;
}
)MSL";
}
