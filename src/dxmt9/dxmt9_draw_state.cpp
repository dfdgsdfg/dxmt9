#include "dxmt9_draw_state.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9/assert.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace dxmt9::state {

using core::CompareFunc;
using core::FogMode;
using core::StencilOp;

using core::RS_ALPHA_FUNC;
using core::RS_ALPHA_REF;
using core::RS_ALPHA_TEST_ENABLE;
using core::RS_FOG_COLOR;
using core::RS_FOG_DENSITY;
using core::RS_FOG_END;
using core::RS_FOG_ENABLE;
using core::RS_FOG_FROM_VERTEX;
using core::RS_FOG_START;
using core::RS_FOG_TABLE_MODE;
using core::RS_POINTSCALE_A;
using core::RS_POINTSCALE_B;
using core::RS_POINTSCALE_C;
using core::RS_POINTSIZE;
using core::RS_POINTSIZE_MAX;
using core::RS_POINTSIZE_MIN;
using core::RS_RANGE_FOG;
using core::RS_STENCIL_CCW_FAIL;
using core::RS_STENCIL_CCW_FUNC;
using core::RS_STENCIL_CCW_MASK;
using core::RS_STENCIL_CCW_PASS;
using core::RS_STENCIL_CCW_WRITEMASK;
using core::RS_STENCIL_CCW_ZFAIL;
using core::RS_STENCIL_ENABLE;
using core::RS_TWO_SIDED_STENCIL_MODE;
using core::RS_STENCIL_FAIL;
using core::RS_STENCIL_FUNC;
using core::RS_STENCIL_MASK;
using core::RS_STENCIL_PASS;
using core::RS_STENCIL_REF;
using core::RS_STENCIL_WRITEMASK;
using core::RS_STENCIL_ZFAIL;
using core::RS_TEXTURE_FACTOR;
using core::RS_Z_ENABLE;
using core::RS_Z_FUNC;
using core::RS_Z_WRITE_ENABLE;

namespace {

std::array<f32, 4> normalizedD3DColor(u32 raw) {
  return {
      static_cast<f32>((raw >> 16) & 0xffu) / 255.0f,
      static_cast<f32>((raw >> 8) & 0xffu) / 255.0f,
      static_cast<f32>(raw & 0xffu) / 255.0f,
      static_cast<f32>((raw >> 24) & 0xffu) / 255.0f,
  };
}

std::array<f32, 4> colorToArray(const core::ColorRGBA& color) {
  return {color.r, color.g, color.b, color.a};
}

}  // namespace

VsConsts buildVsConsts(core::FlatDrawStateView state) {
  DXMT_ASSERT(state.hasUniformPayload());
  const auto& vsConst = state.uniformPayload().vsConst;
  VsConsts out;
  out.vsFloatConst = vsConst.float4;
  out.vsIntConst = vsConst.int4;
  for (std::size_t i = 0; i < core::kMaxBoolConstants; ++i) {
    out.vsBoolConst[i] = vsConst.bools[i] ? 1u : 0u;
  }
  return out;
}

PsConsts buildPsConsts(core::FlatDrawStateView state) {
  DXMT_ASSERT(state.hasUniformPayload());
  const auto& psConst = state.uniformPayload().psConst;
  PsConsts out;
  out.psFloatConst = psConst.float4;
  out.psIntConst = psConst.int4;
  for (std::size_t i = 0; i < core::kMaxBoolConstants; ++i) {
    out.psBoolConst[i] = psConst.bools[i] ? 1u : 0u;
  }
  return out;
}

FfpVsConsts buildFfpVsConsts(core::FlatDrawStateView state) {
  const auto& hot = *state.hot;
  DXMT_ASSERT(state.hasUniformPayload());
  const auto& payload = state.uniformPayload();
  FfpVsConsts out;
  for (std::size_t row = 0; row < 4; ++row) {
    for (std::size_t col = 0; col < 4; ++col) {
      out.ffpWorldViewProj[row][col] = payload.worldViewProj.m[row * 4 + col];
      out.ffpWorldView[row][col] = payload.ffpWorldView.m[row * 4 + col];
      out.ffpNormalMatrix[row][col] = payload.ffpNormalMatrix.m[row * 4 + col];
    }
  }
  out.materialEmissive = colorToArray(payload.material.emissive);
  out.materialAmbient = colorToArray(payload.material.ambient);
  out.materialDiffuse = colorToArray(payload.material.diffuse);
  out.materialSpecular = colorToArray(payload.material.specular);
  out.globalAmbient =
      normalizedD3DColor(core::flatStateOr(hot.renderStates, core::RS_AMBIENT, 0u));
  out.materialPower = {payload.material.power, 0.0f, 0.0f, 0.0f};
  for (std::size_t i = 0; i < core::kMaxLights; ++i) {
    const auto& light = payload.lights[i];
    out.lightDiffuse[i] = colorToArray(light.diffuse);
    out.lightSpecular[i] = colorToArray(light.specular);
    out.lightAmbient[i] = colorToArray(light.ambient);
    out.lightDirection[i] = {light.direction[0], light.direction[1],
                             light.direction[2], 0.0f};
    // D3D9 §B.5: position/range/atten/falloff/theta/phi for Point/Spot.
    // For Directional lights the shader skips these fields entirely
    // (see emitLightingBlock branch on key.lightType[i]); we still emit
    // a defined value so the uniform layout is fully initialized.
    out.lightPosition[i] = {light.position[0], light.position[1],
                            light.position[2], light.range};
    out.lightAttenuation[i] = {light.attenuation0, light.attenuation1,
                               light.attenuation2, light.falloff};
    // Precompute the spot half-angle cosines. D3D9 stores theta/phi as
    // full cone angles in radians; the per-vertex spot factor compares
    // dot(L, -SpotDir) to cos(angle/2). Outer (phi) defines the zero-
    // intensity boundary, inner (theta) defines the full-intensity
    // plateau.
    const f32 cosOuter = std::cos(0.5f * light.phi);
    const f32 cosInner = std::cos(0.5f * light.theta);
    out.lightSpotCone[i] = {cosInner, cosOuter, 0.0f, 0.0f};
  }
  for (std::size_t matrix = 0; matrix < out.ffpBlendWorldViewProj.size(); ++matrix) {
    for (std::size_t row = 0; row < 4; ++row) {
      for (std::size_t col = 0; col < 4; ++col) {
        out.ffpBlendWorldViewProj[matrix][row][col] =
            payload.ffpBlendWorldViewProj[matrix].m[row * 4 + col];
      }
    }
  }
  for (std::size_t stage = 0; stage < core::kMaxTextureStages; ++stage) {
    for (std::size_t row = 0; row < 4; ++row) {
      for (std::size_t col = 0; col < 4; ++col) {
        out.ffpTextureTransforms[stage][row][col] =
            payload.textureTransforms[stage].m[row * 4 + col];
      }
    }
  }
  for (std::size_t i = 0; i < core::kMaxClipPlanes; ++i) {
    out.clipPlanes[i] = payload.clipPlanes[i];
  }
  out.halfPixelFixup = core::halfPixelFixup(hot.viewport.viewport);
  out.viewportOrigin = {static_cast<f32>(hot.viewport.viewport.x),
                        static_cast<f32>(hot.viewport.viewport.y)};
  out.viewportSize = {static_cast<f32>(std::max(1u, hot.viewport.viewport.width)),
                      static_cast<f32>(std::max(1u, hot.viewport.viewport.height))};
  out.fogStart =
      std::bit_cast<f32>(core::flatStateOr(hot.renderStates, RS_FOG_START, std::bit_cast<u32>(1.0f)));
  out.fogEnd =
      std::bit_cast<f32>(core::flatStateOr(hot.renderStates, RS_FOG_END, std::bit_cast<u32>(1.0f)));
  out.fogDensity =
      std::bit_cast<f32>(core::flatStateOr(hot.renderStates, RS_FOG_DENSITY, std::bit_cast<u32>(1.0f)));
  out.fogMode = core::flatStateOr(hot.renderStates, RS_FOG_FROM_VERTEX,
                                  static_cast<u32>(core::FogMode::None));
  out.rangeFog = core::flatStateOr(hot.renderStates, RS_RANGE_FOG, 0u) != 0 ? 1u : 0u;
  out.clipPlaneMask = hot.clipPlaneMask;
  // Point-size family: stored as DWORD but reinterpret-cast to float, same
  // pattern as fog start/end/density and D3DSAMP_MIPMAPLODBIAS.
  out.pointSize = std::bit_cast<f32>(
      core::flatStateOr(hot.renderStates, RS_POINTSIZE, std::bit_cast<u32>(1.0f)));
  out.pointSizeMin = std::bit_cast<f32>(
      core::flatStateOr(hot.renderStates, RS_POINTSIZE_MIN, std::bit_cast<u32>(1.0f)));
  out.pointSizeMax = std::bit_cast<f32>(
      core::flatStateOr(hot.renderStates, RS_POINTSIZE_MAX, std::bit_cast<u32>(64.0f)));
  out.pointScaleA = std::bit_cast<f32>(
      core::flatStateOr(hot.renderStates, RS_POINTSCALE_A, std::bit_cast<u32>(1.0f)));
  out.pointScaleB = std::bit_cast<f32>(
      core::flatStateOr(hot.renderStates, RS_POINTSCALE_B, std::bit_cast<u32>(0.0f)));
  out.pointScaleC = std::bit_cast<f32>(
      core::flatStateOr(hot.renderStates, RS_POINTSCALE_C, std::bit_cast<u32>(0.0f)));
  return out;
}

FfpPsConsts buildFfpPsConsts(core::FlatDrawStateView state) {
  const auto& rs = state.hot->renderStates;
  FfpPsConsts out;
  if (const auto* textureFactor = core::findFlatState(rs, RS_TEXTURE_FACTOR)) {
    out.textureFactor = normalizedD3DColor(textureFactor->value);
  }
  out.alphaTestEnable = !debug::disableAlphaTest() &&
                        core::flatStateOr(rs, RS_ALPHA_TEST_ENABLE, 0u) != 0;
  out.alphaTestFunc =
      core::flatStateOr(rs, RS_ALPHA_FUNC, static_cast<u32>(CompareFunc::Always));
  const bool fogEnabled = core::flatStateOr(rs, RS_FOG_ENABLE, 0u) != 0;
  out.fogMode = fogEnabled
                    ? core::flatStateOr(rs, RS_FOG_TABLE_MODE,
                                        static_cast<u32>(FogMode::None))
                    : static_cast<u32>(FogMode::None);
  if (fogEnabled && out.fogMode == static_cast<u32>(FogMode::None)) {
    out.fogMode = core::flatStateOr(rs, RS_FOG_FROM_VERTEX,
                                    static_cast<u32>(FogMode::None));
    out.fogSource = out.fogMode != static_cast<u32>(FogMode::None) ? 1u : 0u;
  }
  out.fogColor = normalizedD3DColor(core::flatStateOr(rs, RS_FOG_COLOR, 0u));
  out.alphaRef =
      static_cast<f32>(core::flatStateOr(rs, RS_ALPHA_REF, 0u)) / 255.0f;
  out.fogStart =
      std::bit_cast<f32>(core::flatStateOr(rs, RS_FOG_START, std::bit_cast<u32>(1.0f)));
  out.fogEnd =
      std::bit_cast<f32>(core::flatStateOr(rs, RS_FOG_END, std::bit_cast<u32>(1.0f)));
  out.fogDensity =
      std::bit_cast<f32>(core::flatStateOr(rs, RS_FOG_DENSITY, std::bit_cast<u32>(1.0f)));
  for (u32 stage = 0; stage < core::kMaxTextureStages; ++stage) {
    const auto& tss = state.hot->textureStageStates[stage];
    out.stageConstants[stage] =
        normalizedD3DColor(core::flatStateOr(tss, core::TSS_CONSTANT, 0u));
    out.bumpEnvMat[stage] = {
        std::bit_cast<f32>(core::flatStateOr(tss, core::TSS_BUMPENVMAT00, 0u)),
        std::bit_cast<f32>(core::flatStateOr(tss, core::TSS_BUMPENVMAT01, 0u)),
        std::bit_cast<f32>(core::flatStateOr(tss, core::TSS_BUMPENVMAT10, 0u)),
        std::bit_cast<f32>(core::flatStateOr(tss, core::TSS_BUMPENVMAT11, 0u)),
    };
    out.bumpEnvLum[stage] = {
        std::bit_cast<f32>(core::flatStateOr(tss, core::TSS_BUMPENVLSCALE, 0u)),
        std::bit_cast<f32>(core::flatStateOr(tss, core::TSS_BUMPENVLOFFSET, 0u)),
    };
  }
  return out;
}

SamplerLodBias buildSamplerLodBias(core::FlatDrawStateView state) {
  // D3DSAMP_MIPMAPLODBIAS (=8) is stored as a DWORD but is an IEEE-754 float
  // (same DWORD-reinterpret pattern as fog start/end/density and point size).
  // Read it per sampler stage and bit-cast to the float the shader threads
  // into bias(). Default 0.0 keeps the common no-bias case a no-op.
  SamplerLodBias out;
  for (u32 stage = 0; stage < core::kMaxTextureStages; ++stage) {
    out.bias[stage] = std::bit_cast<f32>(core::flatStateOr(
        state.hot->samplerStates[stage], core::SAMP_MIPMAP_LOD_BIAS,
        std::bit_cast<u32>(0.0f)));
  }
  return out;
}

bool anySamplerLodBiasNonzero(core::FlatDrawStateView state) {
  // PSO-variant gate predicate (gap_d3d9 B.3): single source of truth shared by
  // the PSO key (emit) and the slot-4 encoder bind. True iff some sampler
  // stage's SAMP_MIPMAP_LOD_BIAS DWORD bit-casts to a non-zero float. Reads the
  // exact same flat state slots buildSamplerLodBias uses so the key bit and the
  // bind decision can never disagree. The common no-bias draw returns false,
  // dropping the slot-4 SamplerLodBias param + bind + per-sample bias() to the
  // pre-feature plain-sample form.
  for (u32 stage = 0; stage < core::kMaxTextureStages; ++stage) {
    const f32 bias = std::bit_cast<f32>(core::flatStateOr(
        state.hot->samplerStates[stage], core::SAMP_MIPMAP_LOD_BIAS,
        std::bit_cast<u32>(0.0f)));
    if (bias != 0.0f) {
      return true;
    }
  }
  return false;
}

DrawVolatile buildDrawVolatile(i32 vertexBaseIndex, u32 vertexStreamOffset,
                               u32 vertexStreamStride) {
  DrawVolatile out;
  out.vertexBaseIndex = vertexBaseIndex;
  out.vertexStreamOffset = vertexStreamOffset;
  out.vertexStreamStride = vertexStreamStride;
  return out;
}

pipeline::DepthStencilKey makeDepthStencilKey(core::FlatDrawStateView state) {
  const auto& rs = state.hot->renderStates;
  pipeline::DepthStencilKey key;
  if (debug::forceVisibleDraw()) {
    return key;
  }
  key.depthEnable = core::flatStateOr(rs, RS_Z_ENABLE, 0u) != 0;
  key.depthWrite = key.depthEnable && !debug::probeDisableDepthWrite() &&
                   core::flatStateOr(rs, RS_Z_WRITE_ENABLE, 0u) != 0;
  key.depthFunc =
      key.depthEnable && !debug::probeDepthFuncAlways()
          ? core::flatStateOr(rs, RS_Z_FUNC, static_cast<u32>(CompareFunc::Always))
          : static_cast<u32>(CompareFunc::Always);
  key.front.enabled = core::flatStateOr(rs, RS_STENCIL_ENABLE, 0u) != 0;
  key.front.compareFunction =
      core::flatStateOr(rs, RS_STENCIL_FUNC, static_cast<u32>(CompareFunc::Always));
  key.front.failureOperation =
      core::flatStateOr(rs, RS_STENCIL_FAIL, static_cast<u32>(StencilOp::Keep));
  key.front.depthFailureOperation =
      core::flatStateOr(rs, RS_STENCIL_ZFAIL, static_cast<u32>(StencilOp::Keep));
  key.front.passOperation =
      core::flatStateOr(rs, RS_STENCIL_PASS, static_cast<u32>(StencilOp::Keep));
  key.front.readMask = core::flatStateOr(rs, RS_STENCIL_MASK, 0xffu);
  key.front.writeMask = core::flatStateOr(rs, RS_STENCIL_WRITEMASK, 0xffu);
  // D3DRS_TWOSIDEDSTENCILMODE (185): when FALSE (default) back faces use the
  // same ops/func as front; when TRUE back faces use the CCW family
  // (D3DRS_CCW_STENCILFAIL/ZFAIL/PASS/FUNC, slots 186-189). Metal exposes
  // these via MTLDepthStencilDescriptor.backFaceStencil. The stencil
  // reference (computeStencilRef) and read/write masks are single in D3D9 —
  // there is no D3DRS_CCW_STENCILREF / CCW mask — so they stay shared.
  const bool twoSided = core::flatStateOr(rs, RS_TWO_SIDED_STENCIL_MODE, 0u) != 0;
  if (twoSided) {
    key.back.compareFunction =
        core::flatStateOr(rs, RS_STENCIL_CCW_FUNC, key.front.compareFunction);
    key.back.failureOperation =
        core::flatStateOr(rs, RS_STENCIL_CCW_FAIL, key.front.failureOperation);
    key.back.depthFailureOperation = core::flatStateOr(
        rs, RS_STENCIL_CCW_ZFAIL, key.front.depthFailureOperation);
    key.back.passOperation =
        core::flatStateOr(rs, RS_STENCIL_CCW_PASS, key.front.passOperation);
  } else {
    key.back.compareFunction = key.front.compareFunction;
    key.back.failureOperation = key.front.failureOperation;
    key.back.depthFailureOperation = key.front.depthFailureOperation;
    key.back.passOperation = key.front.passOperation;
  }
  // Masks are shared across both faces in D3D9 regardless of mode.
  key.back.readMask = key.front.readMask;
  key.back.writeMask = key.front.writeMask;
  key.back.enabled = key.front.enabled;
  return key;
}

// D3DRS_STENCILREF (=57) is a DWORD but Metal stencil compares are 8-bit, so
// truncate to the low byte. D3D9 has no D3DRS_CCW_STENCILREF — Wine's
// `wined3d_device_apply_stencil_ref` applies the same `state->stencil_ref`
// to both faces regardless of D3DRS_TWOSIDEDSTENCILMODE; WMT exposes only a
// single-ref setter (`setStencilReferenceValue`) which Metal applies to
// front and back together. Front-and-back symmetry is therefore correct and
// matches the Wine oracle exactly.
std::uint8_t computeStencilRef(core::FlatDrawStateView state) {
  const auto& rs = state.hot->renderStates;
  return static_cast<std::uint8_t>(core::flatStateOr(rs, RS_STENCIL_REF, 0u) & 0xffu);
}

}  // namespace dxmt9::state
