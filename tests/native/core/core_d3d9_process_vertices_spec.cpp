// Pure value-level, Wine-free spec for IDirect3DDevice9::ProcessVertices
// (src/d3d9/d3d9_pe_process_vertices.cpp).
//
// Why a value-level spec, and why this is the FIRST native coverage of
// ProcessVertices:
//   d3d9_pe_process_vertices.cpp is built only into dxmt9_pe_core_srcs
//   (src/d3d9/meson.build), which is only compiled
//   `if host_machine.system() == 'windows'` -- it is a Windows-only TU
//   (transitively pulls <windows.h>/<d3d9.h> through d3d9_pe.hpp) and cannot
//   be linked into a host (macOS) native test binary. The established
//   pattern for exactly this situation (core_d3d9_multiply_transform_spec.cpp,
//   core_d3d9_device_validation_spec.cpp, core_d3d9_recorder_lock_spec.cpp,
//   core_d3d9_shared_handle_spec.cpp) is to mirror the small, self-contained
//   PE-side pure logic here, byte-for-byte, and pin its observable contract
//   at value level. The implementation file remains the source of truth.
//
//   KNOW WHAT THIS DOES NOT DO: because the mirror is a copy rather than a
//   call, editing d3d9_pe_process_vertices.cpp does NOT fail this spec. It
//   cannot detect drift; it pins the contract in value terms and records the
//   arithmetic, so a reviewer reading both files can see a divergence, and a
//   future change that means to alter the contract has something concrete to
//   contradict. Treat a green run here as "the documented contract still
//   holds internally", never as "the shipped implementation still matches".
//
// CRITICAL PROVENANCE RULE FOLLOWED HERE: every `expected*` value below is
// derived by hand arithmetic written out in the test comments/code from the
// D3D9 contract (row-vector v' = v*M transform convention, viewport map,
// XYZRHW semantics) -- never by running dxmt9 and recording what it printed.
// Where dxmt9's ProcessVertices programmable-shader interpreter behavior is
// not part of the D3D9 contract itself (Wine nulls the vertex shader in
// wined3d_device_process_vertices and only ever runs a fixed-function
// WORLD*VIEW*PROJ transform -- real-D3D9-via-Wine has no opinion on the
// programmable path), the composition rule asserted is the one the
// implementation demonstrably follows by construction (cited by exact
// source location below), and the test additionally asserts the
// corresponding STRUCTURAL invariant rather than only a pinned number.
//
// Companion/oracle context: this investigation found that
// tests/conformance/d3d9/d3d9_conformance_visual_misc.c ::
// test_visual_process_vertices_xyzhw_policy's hardcoded `expected_*` arrays
// are wrong in at least four mutually incompatible ways, and that Wine
// cannot arbitrate the programmable-shader path at all. Those arrays are
// read here only as background, never as an oracle -- see the "AGREES /
// DISAGREES" summary in the investigation report, not reproduced in this
// file.

#include "core_spec_fixtures.hpp"

#include <array>
#include <cstdint>
#include <cstring>

using namespace dxmt9::core::spec;

namespace {

// ---------------------------------------------------------------------------
// Mirrors of src/d3d9/d3d9_pe_process_vertices.cpp's pure value-level core.
// ---------------------------------------------------------------------------

struct Mat4 {
  // Row-major, m[row*4 + col] -- identical layout to dxmt9::D9CMatrix and to
  // core_d3d9_multiply_transform_spec.cpp's Mat4.
  std::array<float, 16> m{};
};

Mat4 identityMat() {
  Mat4 r{};
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
  return r;
}

// Row-vector scale: v' = v * S. Diagonal (sx, sy, sz, 1) leaves w untouched.
Mat4 scaleMat(float sx, float sy, float sz) {
  Mat4 r{};
  r.m[0] = sx;
  r.m[5] = sy;
  r.m[10] = sz;
  r.m[15] = 1.0f;
  return r;
}

// Row-vector translate (D3D9 row-vector convention: translation lives in
// row 3, so v' = v * T adds (tx,ty,tz) to a w=1 point).
Mat4 translateMat(float tx, float ty, float tz) {
  Mat4 r = identityMat();
  r.m[12] = tx;
  r.m[13] = ty;
  r.m[14] = tz;
  return r;
}

// Mirrors multiplyTransformMatrix() (d3d9_pe_process_vertices.cpp:2255-2268):
//   result.m[row*4+col] = sum_k left.m[row*4+k] * right.m[k*4+col]
Mat4 multiplyMat(const Mat4& left, const Mat4& right) {
  Mat4 result{};
  for (unsigned row = 0; row < 4; ++row) {
    for (unsigned col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (unsigned k = 0; k < 4; ++k) {
        sum += left.m[row * 4 + k] * right.m[k * 4 + col];
      }
      result.m[row * 4 + col] = sum;
    }
  }
  return result;
}

// Mirrors worldViewProjectionTransform() (d3d9_pe_process_vertices.cpp:
// 2270-2277): WVP = (WORLD * VIEW) * PROJECTION, in that left-to-right
// composition order.
Mat4 worldViewProjection(const Mat4& world, const Mat4& view, const Mat4& proj) {
  return multiplyMat(multiplyMat(world, view), proj);
}

// Mirrors the lambda `transformPoint` used throughout processVertices()
// (d3d9_pe_process_vertices.cpp:2762-2771):
//   out[col] = sum_k position[k] * matrix.m[k*4 + col]
// This is the row-vector v' = v * M product against the row-major Mat4
// above (same convention pinned by core_d3d9_multiply_transform_spec.cpp).
std::array<float, 4> transformPoint(const std::array<float, 4>& position,
                                     const Mat4& matrix) {
  std::array<float, 4> out{};
  for (unsigned col = 0; col < 4; ++col) {
    out[col] = position[0] * matrix.m[col] + position[1] * matrix.m[4 + col] +
                position[2] * matrix.m[8 + col] +
                position[3] * matrix.m[12 + col];
  }
  return out;
}

struct Viewport {
  // Mirrors D9CViewport (include/dxmt9/device_c.h:52-55).
  uint32_t x = 0, y = 0, width = 0, height = 0;
  float minZ = 0.0f, maxZ = 1.0f;
};

struct ScreenVertex {
  float x = 0.0f, y = 0.0f, z = 0.0f, rhw = 0.0f;
};

// Mirrors the per-vertex viewport-map tail of processVertices()
// (d3d9_pe_process_vertices.cpp:2684-2689 for the scale/offset/zScale setup,
// and :3322-3338 for the per-vertex divide + map). Applied identically after
// EITHER the fixed-function `clip` (transformPoint(position, wvp, clip)) or
// the programmable-path `clip` (copied verbatim from the shader's oPos
// register, see below) -- the viewport map itself does not know or care
// which path produced `clip`.
ScreenVertex mapClipToScreen(const std::array<float, 4>& clip,
                             const Viewport& vp, bool clippingEnabled) {
  const float scaleX = static_cast<float>(vp.width) * 0.5f;
  const float scaleY = static_cast<float>(vp.height) * 0.5f;
  const float offsetX = static_cast<float>(vp.x) + scaleX;
  const float offsetY = static_cast<float>(vp.y) + scaleY;
  const float zScale = vp.maxZ - vp.minZ;
  // d3d9_pe_process_vertices.cpp:3322 -- w==0 does NOT divide by zero; it
  // is defined (in code, not documented anywhere) to fall back to invW=1.
  const float invW = clip[3] != 0.0f ? 1.0f / clip[3] : 1.0f;
  const float ndcX = clip[0] * invW;
  const float ndcY = clip[1] * invW;
  const float ndcZ = clip[2] * invW;
  float viewportZ = vp.minZ + ndcZ * zScale;
  if (!clippingEnabled) {
    // d3d9_pe_process_vertices.cpp:3327-3331 (D3DRS_CLIPPING == 0 branch).
    const float minDepth = std::min(vp.minZ, vp.maxZ);
    const float maxDepth = std::max(vp.minZ, vp.maxZ);
    viewportZ = std::clamp(viewportZ, minDepth, maxDepth);
  }
  ScreenVertex out;
  out.x = ndcX * scaleX + offsetX;
  // Note the deliberate Y flip (d3d9_pe_process_vertices.cpp:3334):
  // out.y = -ndcY * scaleY + offsetY.
  out.y = -ndcY * scaleY + offsetY;
  out.z = viewportZ;
  out.rhw = invW;
  return out;
}

// Mirrors decodeProcessDeclVector() (d3d9_pe_process_vertices.cpp:
// 1013-1117) for the three cases required by this spec: FLOATn (used for
// the "pure decode" identity check), SHORT2 (raw unnormalized), and
// UBYTE4N (normalized). D3DDECLTYPE_D3DCOLOR is deliberately NOT part of
// this function in the source (it is decoded separately by
// unpackD3DColor(), mirrored below) -- both are covered.
enum class DeclType { Float1, Float2, Float3, Float4, Short2, Ubyte4n };

std::array<float, 4> decodeDeclVector(const uint8_t* source, DeclType type) {
  std::array<float, 4> out{0.0f, 0.0f, 0.0f, 1.0f};  // line 1017 default.
  switch (type) {
    case DeclType::Float1:
    case DeclType::Float2:
    case DeclType::Float3:
    case DeclType::Float4: {
      const unsigned components = type == DeclType::Float1   ? 1
                                   : type == DeclType::Float2 ? 2
                                   : type == DeclType::Float3 ? 3
                                                               : 4;
      std::memcpy(out.data(), source, components * sizeof(float));
      break;
    }
    case DeclType::Short2: {
      int16_t in[2]{};
      std::memcpy(in, source, sizeof(in));
      out[0] = static_cast<float>(in[0]);
      out[1] = static_cast<float>(in[1]);
      break;
    }
    case DeclType::Ubyte4n: {
      uint8_t in[4]{};
      std::memcpy(in, source, sizeof(in));
      for (unsigned c = 0; c < 4u; ++c) out[c] = static_cast<float>(in[c]) / 255.0f;
      break;
    }
  }
  return out;
}

// Mirrors unpackD3DColor() (d3d9_pe_process_vertices.cpp:688-693): a
// D3DCOLOR DWORD is 0xAARRGGBB, and is unpacked to register order
// (R, G, B, A), each channel /255.
std::array<float, 4> unpackD3DColor(uint32_t color) {
  std::array<float, 4> out{};
  out[0] = static_cast<float>((color >> 16u) & 0xffu) / 255.0f;  // R
  out[1] = static_cast<float>((color >> 8u) & 0xffu) / 255.0f;   // G
  out[2] = static_cast<float>(color & 0xffu) / 255.0f;           // B
  out[3] = static_cast<float>((color >> 24u) & 0xffu) / 255.0f;  // A
  return out;
}

// ---------------------------------------------------------------------------
// Minimal interpreter mirror for the composition-rule tests: MOV (full
// write) and ADD (masked write) only, mirroring the D3DSIO_MOV case body
// (d3d9_pe_process_vertices.cpp:2036-2038), the D3DSIO_ADD case body
// (:2084-2086), and simpleVsWriteDest()'s per-component write-mask
// application (:1405-1450, specifically the mask loop at :1440-1444).
// Real D3D9 bytecode token parsing (analyzeSimpleProcessVertexShader /
// simpleProcessShaderReadOperands) is intentionally NOT reproduced here --
// this spec is not proving the parser, it is proving the composition rule:
// what (if any) transform is applied to the position input/output around
// shader execution. That rule lives entirely in processVertices() itself,
// not in the parser.
// ---------------------------------------------------------------------------

struct WriteMask {
  bool x = true, y = true, z = true, w = true;
};

std::array<float, 4> execMov(const std::array<float, 4>& priorDest,
                             const std::array<float, 4>& src,
                             WriteMask mask) {
  std::array<float, 4> out = priorDest;
  if (mask.x) out[0] = src[0];
  if (mask.y) out[1] = src[1];
  if (mask.z) out[2] = src[2];
  if (mask.w) out[3] = src[3];
  return out;
}

std::array<float, 4> execAdd(const std::array<float, 4>& priorDest,
                             const std::array<float, 4>& a,
                             const std::array<float, 4>& b,
                             WriteMask mask) {
  std::array<float, 4> out = priorDest;
  if (mask.x) out[0] = a[0] + b[0];
  if (mask.y) out[1] = a[1] + b[1];
  if (mask.z) out[2] = a[2] + b[2];
  if (mask.w) out[3] = a[3] + b[3];
  return out;
}

// ---------------------------------------------------------------------------
// 1. Fixed-function path: position * WORLD * VIEW * PROJ, then viewport map.
//    Non-identity WORLD catches a missing-or-doubled world multiply.
// ---------------------------------------------------------------------------

void testFixedFunctionWorldViewProjectionThenViewport() {
  // Hand-derived pipeline. Chosen so every stage does real, non-trivial
  // work and the matrices do not commute (order-sensitive):
  //   WORLD = scale(0.5, 0.5, 1)      -- object-space to world-space
  //   VIEW  = translate(-1, 0, 0)     -- world-space to view-space
  //   PROJ  = [x'=x, y'=y, z'=z, w'=z] -- view-space to clip-space, a
  //           minimal (non-D3D-standard, deliberately simple) projective
  //           matrix whose only job is to route view-space z into clip.w
  //           so the perspective divide below is non-trivial.
  const Mat4 world = scaleMat(0.5f, 0.5f, 1.0f);
  const Mat4 view = translateMat(-1.0f, 0.0f, 0.0f);
  Mat4 proj{};
  proj.m[0] = 1.0f;   // x' = x
  proj.m[5] = 1.0f;   // y' = y
  proj.m[10] = 1.0f;  // z' = z
  proj.m[11] = 1.0f;  // w' = z (column 3 of row 2)

  const std::array<float, 4> object = {2.0f, 4.0f, 10.0f, 1.0f};

  // Hand arithmetic, step by step (independent of the matrix-multiply
  // helper under test -- this is the cross-check path):
  //   world  = (2*0.5, 4*0.5, 10*1, 1) = (1, 2, 10, 1)
  //   view   = (1-1, 2, 10, 1)         = (0, 2, 10, 1)
  //   clip   = (0, 2, 10, 10)          -- w' = view.z = 10
  const std::array<float, 4> expectedWorld = {1.0f, 2.0f, 10.0f, 1.0f};
  const std::array<float, 4> expectedView = {0.0f, 2.0f, 10.0f, 1.0f};
  const std::array<float, 4> expectedClip = {0.0f, 2.0f, 10.0f, 10.0f};

  const std::array<float, 4> stepWorld = transformPoint(object, world);
  checkNear(stepWorld[0], expectedWorld[0], 1e-6f, "world.x");
  checkNear(stepWorld[1], expectedWorld[1], 1e-6f, "world.y");
  checkNear(stepWorld[2], expectedWorld[2], 1e-6f, "world.z");

  const std::array<float, 4> stepView = transformPoint(stepWorld, view);
  checkNear(stepView[0], expectedView[0], 1e-6f, "view.x");
  checkNear(stepView[2], expectedView[2], 1e-6f, "view.z");

  const std::array<float, 4> stepClip = transformPoint(stepView, proj);
  checkNear(stepClip[0], expectedClip[0], 1e-6f, "clip.x (step-by-step)");
  checkNear(stepClip[1], expectedClip[1], 1e-6f, "clip.y (step-by-step)");
  checkNear(stepClip[2], expectedClip[2], 1e-6f, "clip.z (step-by-step)");
  checkNear(stepClip[3], expectedClip[3], 1e-6f, "clip.w (step-by-step)");

  // The single WVP matrix (as processVertices() actually builds and applies
  // it) must agree with the independent step-by-step transform above --
  // this is the real regression guard: a swapped multiply order
  // (e.g. PROJ*VIEW*WORLD) or a doubled/dropped WORLD term would diverge
  // from the step-by-step path while possibly still looking plausible in
  // isolation.
  const Mat4 wvp = worldViewProjection(world, view, proj);
  const std::array<float, 4> clip = transformPoint(object, wvp);
  checkNear(clip[0], expectedClip[0], 1e-5f, "WVP clip.x matches step-by-step");
  checkNear(clip[1], expectedClip[1], 1e-5f, "WVP clip.y matches step-by-step");
  checkNear(clip[2], expectedClip[2], 1e-5f, "WVP clip.z matches step-by-step");
  checkNear(clip[3], expectedClip[3], 1e-5f, "WVP clip.w matches step-by-step");

  // Viewport map. vp = {x=0,y=0,width=640,height=480,minZ=0,maxZ=1}, a
  // typical D3DVIEWPORT9.
  //   invW  = 1/10 = 0.1
  //   ndc   = (0*0.1, 2*0.1, 10*0.1) = (0, 0.2, 1.0)
  //   scaleX = 640*0.5 = 320, offsetX = 0+320 = 320
  //   scaleY = 480*0.5 = 240, offsetY = 0+240 = 240
  //   zScale = 1-0 = 1
  //   out.x  = 0*320 + 320   = 320
  //   out.y  = -0.2*240+240  = -48+240 = 192   (Y flip)
  //   out.z  = 0 + 1.0*1     = 1.0
  //   out.rhw= invW          = 0.1
  Viewport vp;
  vp.x = 0;
  vp.y = 0;
  vp.width = 640;
  vp.height = 480;
  vp.minZ = 0.0f;
  vp.maxZ = 1.0f;
  const ScreenVertex screen = mapClipToScreen(clip, vp, /*clippingEnabled=*/true);
  checkNear(screen.x, 320.0f, 1e-4f, "viewport-mapped x");
  checkNear(screen.y, 192.0f, 1e-4f, "viewport-mapped y (note the Y flip)");
  checkNear(screen.z, 1.0f, 1e-5f, "viewport-mapped z");
  checkNear(screen.rhw, 0.1f, 1e-6f, "rhw == 1/clip.w");
}

void testWorldMultiplyAppliedExactlyOnce() {
  // WORLD=scale(3), VIEW=PROJ=identity so the only stage capable of
  // changing xyz is WORLD. If dxmt9 applied WORLD twice, x would come out
  // *9; if it dropped WORLD entirely (only VIEW*PROJ, i.e. identity), x
  // would come out unchanged (*1). Both are distinguishable from the
  // correct *3 result asserted below.
  const Mat4 world = scaleMat(3.0f, 3.0f, 3.0f);
  const Mat4 identity = identityMat();
  const std::array<float, 4> object = {2.0f, -1.0f, 5.0f, 1.0f};

  const Mat4 wvp = worldViewProjection(world, identity, identity);
  const std::array<float, 4> clip = transformPoint(object, wvp);

  checkNear(clip[0], 6.0f, 1e-6f, "world scale applied exactly once (x: 2*3=6, not 2*9=18 or 2)");
  checkNear(clip[1], -3.0f, 1e-6f, "world scale applied exactly once (y: -1*3=-3)");
  checkNear(clip[2], 15.0f, 1e-6f, "world scale applied exactly once (z: 5*3=15)");
  checkNear(clip[3], 1.0f, 1e-6f, "homogeneous w untouched by an xyz-only scale");
}

// ---------------------------------------------------------------------------
// 2. XYZRHW output contract: what lands in the destination vertex buffer,
//    and its relationship to clip-space w.
// ---------------------------------------------------------------------------

void testXyzrhwOutputContract() {
  Viewport vp;
  vp.x = 0;
  vp.y = 0;
  vp.width = 200;
  vp.height = 100;
  vp.minZ = 0.0f;
  vp.maxZ = 1.0f;

  // rhw == 1/clip.w for an ordinary nonzero w: clip=(20,10,4,4) ->
  // invW=0.25, ndc=(5, 2.5, 1) -> x = 5*100+100 = 600, y = -2.5*50+50 = -75,
  // z = 0 + 1*1 = 1, rhw = 0.25.
  {
    const std::array<float, 4> clip = {20.0f, 10.0f, 4.0f, 4.0f};
    const ScreenVertex screen = mapClipToScreen(clip, vp, /*clippingEnabled=*/true);
    checkNear(screen.rhw, 0.25f, 1e-6f, "rhw = 1/clip.w for nonzero w");
    checkNear(screen.x, 600.0f, 1e-4f, "x uses rhw-divided (perspective-correct) ndc");
    checkNear(screen.y, -75.0f, 1e-4f, "y uses rhw-divided ndc with the Y flip");
    checkNear(screen.z, 1.0f, 1e-6f, "z = minZ + (clip.z/clip.w)*(maxZ-minZ)");
  }

  // clip.w == 0: source (d3d9_pe_process_vertices.cpp:3322) defines invW as
  // exactly 1.0 rather than +inf/NaN -- this is dxmt9's own explicit
  // fallback (there is no division-by-zero contract in the D3D9 XYZRHW
  // documentation itself), asserted here as an observed-from-source rule.
  {
    const std::array<float, 4> clip = {3.0f, -2.0f, 5.0f, 0.0f};
    const ScreenVertex screen = mapClipToScreen(clip, vp, /*clippingEnabled=*/true);
    checkNear(screen.rhw, 1.0f, 1e-6f, "clip.w==0 falls back to rhw=1 (no divide-by-zero)");
    checkNear(screen.x, 3.0f * 100.0f + 100.0f, 1e-4f, "x with invW=1 fallback (ndc==clip)");
    checkNear(screen.z, 5.0f, 1e-6f, "z with invW=1 fallback");
  }

  // D3DRS_CLIPPING == 0 clamps viewport z into [min(minZ,maxZ), max(minZ,maxZ)]
  // (d3d9_pe_process_vertices.cpp:3327-3331); with clipping enabled the same
  // out-of-range z passes through unclamped.
  {
    // ndcZ = -2 (well outside [0,1]) via clip=(0,0,-2,1).
    const std::array<float, 4> clip = {0.0f, 0.0f, -2.0f, 1.0f};
    const ScreenVertex clipped = mapClipToScreen(clip, vp, /*clippingEnabled=*/true);
    checkNear(clipped.z, -2.0f, 1e-6f, "D3DRS_CLIPPING enabled: z passes through unclamped");
    const ScreenVertex unclipped = mapClipToScreen(clip, vp, /*clippingEnabled=*/false);
    checkNear(unclipped.z, 0.0f, 1e-6f, "D3DRS_CLIPPING disabled: z clamps to [minZ,maxZ]");
  }
}

// ---------------------------------------------------------------------------
// 3. Interpreter composition rule: what (if anything) transforms the
//    programmable-path position input/output.
//
// Structural fact this spec pins (verified by direct source inspection,
// not by execution): in processVertices()'s `if (programmable) { ... }`
// branch (d3d9_pe_process_vertices.cpp:2772-3226), the identifiers `wvp`
// and `viewProjection` -- the only WORLD/VIEW/PROJECTION-derived matrices
// computed in the function -- are NEVER referenced. `clip` is populated
// directly from the shader's own output-position register:
//     std::memcpy(clip, positionReg->data(), sizeof(clip));   // line 3184
// while the position INPUT register v0 is loaded straight from the raw
// vertex-declaration bytes via decodeProcessDeclVector (line 2783), with
// no matrix applied. Contrast the `else` (fixed-function) branch, which
// explicitly calls `transformPoint(position, wvp, clip)` (line 3319).
//
// COMPOSITION RULE (asserted below, both as pinned values and as a
// transform-invariance structural check):
//   dxmt9's simple-VS interpreter applies NO implicit WORLD/VIEW/PROJECTION
//   transform to either the input registers or the output position
//   register -- the shader's oPos is used directly as clip-space input to
//   the (path-agnostic) viewport map.
// This is a dxmt9-specific behavior with no D3D9 spec anchor (Wine nulls
// the vertex shader in wined3d_device_process_vertices and only ever runs
// fixed-function, so real D3D9 has no programmable-path contract to check
// against here) -- flagged per the task's ambiguity-disclosure rule.
// ---------------------------------------------------------------------------

void testInterpreterMovIsPureDecodePassthrough() {
  // v0 loaded via the FLOAT4 decode path (identical formula to the
  // fixed-function position decode -- decodeProcessDeclVector is shared by
  // both paths) from raw bytes (1, 2, 3, 4).
  const float rawBytes[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  const std::array<float, 4> v0 =
      decodeDeclVector(reinterpret_cast<const uint8_t*>(rawBytes), DeclType::Float4);
  checkNear(v0[0], 1.0f, 1e-6f, "v0.x raw decode");
  checkNear(v0[3], 4.0f, 1e-6f, "v0.w raw decode (FLOAT4 carries its own w, no default override)");

  // `mov o0, v0` -- full write mask, output register o0 starts zero
  // (SimpleVsRegisters value-initializes every register to {0,0,0,0},
  // d3d9_pe_process_vertices.cpp:1119-1132).
  const std::array<float, 4> o0Init = {0.0f, 0.0f, 0.0f, 0.0f};
  const std::array<float, 4> o0 = execMov(o0Init, v0, WriteMask{});
  checkEq(o0, v0, "mov o0, v0 reproduces its input exactly (pure passthrough)");

  const std::array<float, 4> clip = o0;  // memcpy(clip, positionReg->data(), ...)

  // Structural transform-invariance check, by CONTRAST with the
  // fixed-function path: take the same object-space position that
  // produced v0 (1,2,3) as xyz, and run it through the fixed-function
  // transformPoint() under two wildly different WORLD/VIEW/PROJECTION
  // states -- identity, and a large non-uniform scale. The fixed-function
  // result visibly moves with the transform (as it must). The
  // interpreter's `clip` above was computed with NO transform parameter in
  // scope at all (execMov/decodeDeclVector take no matrix argument, matching
  // processVertices()'s programmable branch, which never references `wvp`
  // or `viewProjection` -- d3d9_pe_process_vertices.cpp:2772-3226), so it
  // cannot move: it stays exactly v0 no matter what SetTransform() the app
  // last called.
  const std::array<float, 4> objectSpace = {v0[0], v0[1], v0[2], v0[3]};
  const Mat4 identityXform = identityMat();
  const Mat4 extremeXform = scaleMat(1000.0f, 1000.0f, 1000.0f);
  const std::array<float, 4> fixedFnIdentity = transformPoint(objectSpace, identityXform);
  const std::array<float, 4> fixedFnExtreme = transformPoint(objectSpace, extremeXform);
  check(std::fabs(fixedFnExtreme[0] - fixedFnIdentity[0]) > 1.0f,
        "sanity: fixed-function path DOES move under a different WORLD/VIEW/PROJ (contrast fixture)");
  checkEq(clip, v0, "interpreter's oPos-derived clip is untouched by any transform state (composition rule)");

  Viewport vp;
  vp.x = 10;
  vp.y = 20;
  vp.width = 800;
  vp.height = 600;
  vp.minZ = 0.0f;
  vp.maxZ = 1.0f;
  const ScreenVertex screen = mapClipToScreen(clip, vp, true);
  checkNear(screen.rhw, 0.25f, 1e-6f, "rhw = 1/clip.w = 1/4 -- unaffected by any WORLD/VIEW/PROJECTION state");
}

void testInterpreterAddWithPartialWriteMaskLeavesWAtZeroInit() {
  // v0 = POSITION, decoded FLOAT3 (2, 4, 10) -> default w=1 per
  // decodeProcessDeclVector's {0,0,0,1} initializer (only xyz overwritten).
  const float posBytes[3] = {2.0f, 4.0f, 10.0f};
  const std::array<float, 4> v0 =
      decodeDeclVector(reinterpret_cast<const uint8_t*>(posBytes), DeclType::Float3);
  checkNear(v0[3], 1.0f, 1e-6f, "FLOAT3 position decode defaults w=1");

  // v3 = NORMAL (explicit `dcl_normal v3` in the real shader), decoded
  // FLOAT4 (0.5, -0.5, 1.0, 1.0) via the identical FLOAT-path formula.
  const float normalBytes[4] = {0.5f, -0.5f, 1.0f, 1.0f};
  const std::array<float, 4> v3 =
      decodeDeclVector(reinterpret_cast<const uint8_t*>(normalBytes), DeclType::Float4);

  // `add o0.xyz, v0.xyz, v3.xyz` -- write mask xyz only, so o0.w is left at
  // its zero-init value (simpleVsWriteDest only assigns masked components,
  // d3d9_pe_process_vertices.cpp:1440-1444).
  const std::array<float, 4> o0Init = {0.0f, 0.0f, 0.0f, 0.0f};
  WriteMask xyzOnly;
  xyzOnly.w = false;
  const std::array<float, 4> o0 = execAdd(o0Init, v0, v3, xyzOnly);

  checkNear(o0[0], 2.5f, 1e-6f, "o0.x = v0.x + v3.x = 2 + 0.5");
  checkNear(o0[1], 3.5f, 1e-6f, "o0.y = v0.y + v3.y = 4 + (-0.5)");
  checkNear(o0[2], 11.0f, 1e-6f, "o0.z = v0.z + v3.z = 10 + 1.0");
  checkNear(o0[3], 0.0f, 1e-6f, "o0.w untouched by the xyz write mask, stays at zero-init");

  const std::array<float, 4> clip = o0;

  // clip.w == 0 hits the same invW=1 fallback pinned in
  // testXyzrhwOutputContract above -- proving the composition rule and the
  // XYZRHW contract compose correctly together, not just in isolation.
  Viewport vp;
  vp.x = 0;
  vp.y = 0;
  vp.width = 640;
  vp.height = 480;
  vp.minZ = 0.0f;
  vp.maxZ = 1.0f;
  const ScreenVertex screen = mapClipToScreen(clip, vp, /*clippingEnabled=*/true);
  checkNear(screen.rhw, 1.0f, 1e-6f, "invW=1 fallback (clip.w==0) reached via the interpreter path");
  // ndc == clip (invW=1): x = 2.5*320+320 = 1120, y = -3.5*240+240 = -600,
  // z = 0 + 11*1 = 11.
  checkNear(screen.x, 1120.0f, 1e-4f, "screen.x via interpreter clip + viewport map");
  checkNear(screen.y, -600.0f, 1e-4f, "screen.y via interpreter clip + viewport map (Y flip)");
  checkNear(screen.z, 11.0f, 1e-5f, "screen.z via interpreter clip + viewport map");
}

// ---------------------------------------------------------------------------
// 4. Declaration-decode formulas, independent of any transform.
// ---------------------------------------------------------------------------

void testDeclDecodeShort2Ubyte4nD3DColor() {
  // SHORT2 (d3d9_pe_process_vertices.cpp:1038-1044): raw signed 16-bit
  // integers, UNNORMALIZED (D3DDECLTYPE_SHORT2 is the non-N variant), cast
  // straight to float. Only x,y are written; z,w keep the {0,0,0,1}
  // default set at function entry (line 1017).
  {
    const int16_t raw[2] = {-100, 12345};
    const std::array<float, 4> decoded =
        decodeDeclVector(reinterpret_cast<const uint8_t*>(raw), DeclType::Short2);
    checkNear(decoded[0], -100.0f, 1e-6f, "SHORT2.x is the raw int16 value, not normalized");
    checkNear(decoded[1], 12345.0f, 1e-6f, "SHORT2.y is the raw int16 value, not normalized");
    checkNear(decoded[2], 0.0f, 1e-6f, "SHORT2 does not touch z (default 0)");
    checkNear(decoded[3], 1.0f, 1e-6f, "SHORT2 does not touch w (default 1)");
  }

  // UBYTE4N (d3d9_pe_process_vertices.cpp:1077-1084): unsigned bytes,
  // each divided by 255 to map [0,255] -> [0,1].
  {
    const uint8_t raw[4] = {0, 128, 255, 64};
    const std::array<float, 4> decoded =
        decodeDeclVector(raw, DeclType::Ubyte4n);
    checkNear(decoded[0], 0.0f / 255.0f, 1e-7f, "UBYTE4N 0 -> 0.0");
    checkNear(decoded[1], 128.0f / 255.0f, 1e-7f, "UBYTE4N 128 -> 128/255");
    checkNear(decoded[2], 255.0f / 255.0f, 1e-7f, "UBYTE4N 255 -> 1.0");
    checkNear(decoded[3], 64.0f / 255.0f, 1e-7f, "UBYTE4N 64 -> 64/255");
  }

  // D3DCOLOR (d3d9_pe_process_vertices.cpp:688-693, unpackD3DColor):
  // packed as 0xAARRGGBB, unpacked to register order (R,G,B,A), each /255.
  // Chosen bytes divide evenly by 255 so the expected values are exact:
  //   A=0x7F=127 -> 127/255 = 0.498039...
  //   R=0x33=51  -> 51/255  = 0.2  (51*5=255)
  //   G=0x66=102 -> 102/255 = 0.4  (102*2.5=255... use exact fraction 102/255=2/5)
  //   B=0xCC=204 -> 204/255 = 0.8  (204*1.25=255)
  {
    const uint32_t color = 0x7F3366CCu;
    const std::array<float, 4> decoded = unpackD3DColor(color);
    checkNear(decoded[0], 51.0f / 255.0f, 1e-7f, "D3DCOLOR -> register.x is R = (color>>16)&0xff");
    checkNear(decoded[1], 102.0f / 255.0f, 1e-7f, "D3DCOLOR -> register.y is G = (color>>8)&0xff");
    checkNear(decoded[2], 204.0f / 255.0f, 1e-7f, "D3DCOLOR -> register.z is B = color&0xff");
    checkNear(decoded[3], 127.0f / 255.0f, 1e-7f, "D3DCOLOR -> register.w is A = (color>>24)&0xff");
    checkNear(decoded[0], 0.2f, 1e-6f, "R sanity: 0x33=51, 51/255 == 0.2 exactly");
    checkNear(decoded[2], 0.8f, 1e-6f, "B sanity: 0xCC=204, 204/255 == 0.8 exactly");
  }
}

}  // namespace

int main() {
  try {
    testFixedFunctionWorldViewProjectionThenViewport();
    testWorldMultiplyAppliedExactlyOnce();
    testXyzrhwOutputContract();
    testInterpreterMovIsPureDecodePassthrough();
    testInterpreterAddWithPartialWriteMaskLeavesWAtZeroInit();
    testDeclDecodeShort2Ubyte4nD3DColor();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
