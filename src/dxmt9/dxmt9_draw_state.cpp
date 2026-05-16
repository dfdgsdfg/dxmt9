#include "dxmt9_draw_state.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9/assert.hpp"

#include <algorithm>
#include <bit>

namespace dxmt9::state {

using core::CompareFunc;
using core::FogMode;
using core::StencilOp;

using core::RS_ALPHA_FUNC;
using core::RS_ALPHA_REF;
using core::RS_ALPHA_TEST_ENABLE;
using core::RS_FOG_DENSITY;
using core::RS_FOG_END;
using core::RS_FOG_START;
using core::RS_FOG_TABLE_MODE;
using core::RS_STENCIL_CCW_FAIL;
using core::RS_STENCIL_CCW_FUNC;
using core::RS_STENCIL_CCW_MASK;
using core::RS_STENCIL_CCW_PASS;
using core::RS_STENCIL_CCW_WRITEMASK;
using core::RS_STENCIL_CCW_ZFAIL;
using core::RS_STENCIL_ENABLE;
using core::RS_STENCIL_FAIL;
using core::RS_STENCIL_FUNC;
using core::RS_STENCIL_MASK;
using core::RS_STENCIL_PASS;
using core::RS_STENCIL_WRITEMASK;
using core::RS_STENCIL_ZFAIL;
using core::RS_TEXTURE_FACTOR;
using core::RS_Z_ENABLE;
using core::RS_Z_FUNC;
using core::RS_Z_WRITE_ENABLE;

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
    }
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
  out.clipPlaneMask = hot.clipPlaneMask;
  return out;
}

FfpPsConsts buildFfpPsConsts(core::FlatDrawStateView state) {
  const auto& rs = state.hot->renderStates;
  FfpPsConsts out;
  if (const auto* textureFactor = core::findFlatState(rs, RS_TEXTURE_FACTOR)) {
    const u32 raw = textureFactor->value;
    out.textureFactor = {
        static_cast<f32>((raw >> 16) & 0xffu) / 255.0f,
        static_cast<f32>((raw >> 8) & 0xffu) / 255.0f,
        static_cast<f32>(raw & 0xffu) / 255.0f,
        static_cast<f32>((raw >> 24) & 0xffu) / 255.0f,
    };
  }
  out.alphaTestEnable = !debug::disableAlphaTest() &&
                        core::flatStateOr(rs, RS_ALPHA_TEST_ENABLE, 0u) != 0;
  out.alphaTestFunc =
      core::flatStateOr(rs, RS_ALPHA_FUNC, static_cast<u32>(CompareFunc::Always));
  out.fogMode =
      core::flatStateOr(rs, RS_FOG_TABLE_MODE, static_cast<u32>(FogMode::None));
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
  key.depthWrite = key.depthEnable && core::flatStateOr(rs, RS_Z_WRITE_ENABLE, 0u) != 0;
  key.depthFunc = key.depthEnable
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
  key.back.compareFunction =
      core::flatStateOr(rs, RS_STENCIL_CCW_FUNC, key.front.compareFunction);
  key.back.failureOperation =
      core::flatStateOr(rs, RS_STENCIL_CCW_FAIL, key.front.failureOperation);
  key.back.depthFailureOperation =
      core::flatStateOr(rs, RS_STENCIL_CCW_ZFAIL, key.front.depthFailureOperation);
  key.back.passOperation =
      core::flatStateOr(rs, RS_STENCIL_CCW_PASS, key.front.passOperation);
  key.back.readMask = core::flatStateOr(rs, RS_STENCIL_CCW_MASK, key.front.readMask);
  key.back.writeMask = core::flatStateOr(rs, RS_STENCIL_CCW_WRITEMASK, key.front.writeMask);
  key.back.enabled = key.front.enabled;
  return key;
}

}  // namespace dxmt9::state
