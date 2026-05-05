#include "dxmt9_draw_state.hpp"
#include "dxmt9_debug_trace.hpp"

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

DrawUniforms buildDrawUniforms(core::FlatDrawStateView state) {
  const auto& desc = state.desc();
  const auto& hot = *state.hot;
  const auto& rs = hot.renderStates;
  DrawUniforms uniforms;
  uniforms.vsFloatConst = desc.vsConst.float4;
  uniforms.vsIntConst = desc.vsConst.int4;
  for (std::size_t i = 0; i < core::kMaxBoolConstants; ++i) {
    uniforms.vsBoolConst[i] = desc.vsConst.bools[i] ? 1u : 0u;
  }
  for (std::size_t row = 0; row < 4; ++row) {
    for (std::size_t col = 0; col < 4; ++col) {
      uniforms.ffpWorldViewProj[row][col] = desc.worldViewProj.m[row * 4 + col];
    }
  }
  for (std::size_t stage = 0; stage < core::kMaxTextureStages; ++stage) {
    for (std::size_t row = 0; row < 4; ++row) {
      for (std::size_t col = 0; col < 4; ++col) {
        uniforms.ffpTextureTransforms[stage][row][col] =
            desc.textureTransforms[stage].m[row * 4 + col];
      }
    }
  }
  uniforms.psFloatConst = desc.psConst.float4;
  uniforms.psIntConst = desc.psConst.int4;
  for (std::size_t i = 0; i < core::kMaxBoolConstants; ++i) {
    uniforms.psBoolConst[i] = desc.psConst.bools[i] ? 1u : 0u;
  }
  uniforms.halfPixelFixup = core::halfPixelFixup(hot.viewport.viewport);
  uniforms.viewportOrigin = {static_cast<f32>(hot.viewport.viewport.x),
                             static_cast<f32>(hot.viewport.viewport.y)};
  uniforms.viewportSize = {static_cast<f32>(std::max(1u, hot.viewport.viewport.width)),
                           static_cast<f32>(std::max(1u, hot.viewport.viewport.height))};
  if (const auto* textureFactor = core::findFlatState(rs, RS_TEXTURE_FACTOR)) {
    const u32 raw = textureFactor->value;
    uniforms.textureFactor = {
        static_cast<f32>((raw >> 16) & 0xffu) / 255.0f,
        static_cast<f32>((raw >> 8) & 0xffu) / 255.0f,
        static_cast<f32>(raw & 0xffu) / 255.0f,
        static_cast<f32>((raw >> 24) & 0xffu) / 255.0f,
    };
  }
  uniforms.vertexStreamOffset = hot.streamOffsets[0];
  uniforms.vertexStreamStride = hot.streamStrides[0];
  uniforms.vertexBaseIndex = 0;
  uniforms.clipPlaneMask = hot.clipPlaneMask;
  uniforms.alphaTestEnable = !debug::disableAlphaTest() &&
                             core::flatStateOr(rs, RS_ALPHA_TEST_ENABLE, 0u) != 0;
  uniforms.alphaTestFunc =
      core::flatStateOr(rs, RS_ALPHA_FUNC, static_cast<u32>(CompareFunc::Always));
  uniforms.fogMode =
      core::flatStateOr(rs, RS_FOG_TABLE_MODE, static_cast<u32>(FogMode::None));
  uniforms.alphaRef =
      static_cast<f32>(core::flatStateOr(rs, RS_ALPHA_REF, 0u)) / 255.0f;
  uniforms.fogStart =
      std::bit_cast<f32>(core::flatStateOr(rs, RS_FOG_START, std::bit_cast<u32>(1.0f)));
  uniforms.fogEnd =
      std::bit_cast<f32>(core::flatStateOr(rs, RS_FOG_END, std::bit_cast<u32>(1.0f)));
  uniforms.fogDensity =
      std::bit_cast<f32>(core::flatStateOr(rs, RS_FOG_DENSITY, std::bit_cast<u32>(1.0f)));
  for (std::size_t i = 0; i < core::kMaxClipPlanes; ++i) {
    uniforms.clipPlanes[i] = desc.clipPlanes[i];
  }
  return uniforms;
}

DrawUniforms buildDrawUniforms(const core::DrawDesc& desc) {
  const auto hot = core::makeFlatDrawStateRecord(desc);
  return buildDrawUniforms(core::FlatDrawStateView{.hot = &hot, .coldDesc = &desc});
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

pipeline::DepthStencilKey makeDepthStencilKey(const core::DrawDesc& desc) {
  const auto hot = core::makeFlatDrawStateRecord(desc);
  return makeDepthStencilKey(core::FlatDrawStateView{.hot = &hot, .coldDesc = &desc});
}

}  // namespace dxmt9::state
