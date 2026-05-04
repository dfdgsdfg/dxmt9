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

DrawUniforms buildDrawUniforms(const core::DrawDesc& desc) {
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
  uniforms.halfPixelFixup = core::halfPixelFixup(desc.viewport.viewport);
  uniforms.viewportOrigin = {static_cast<f32>(desc.viewport.viewport.x),
                             static_cast<f32>(desc.viewport.viewport.y)};
  uniforms.viewportSize = {static_cast<f32>(std::max(1u, desc.viewport.viewport.width)),
                           static_cast<f32>(std::max(1u, desc.viewport.viewport.height))};
  if (desc.rs.values.contains(RS_TEXTURE_FACTOR)) {
    const u32 raw = desc.rs.values.at(RS_TEXTURE_FACTOR);
    uniforms.textureFactor = {
        static_cast<f32>((raw >> 16) & 0xffu) / 255.0f,
        static_cast<f32>((raw >> 8) & 0xffu) / 255.0f,
        static_cast<f32>(raw & 0xffu) / 255.0f,
        static_cast<f32>((raw >> 24) & 0xffu) / 255.0f,
    };
  }
  uniforms.vertexStreamOffset = desc.vertexDecl.streams[0].offset;
  uniforms.vertexStreamStride = desc.vertexDecl.streams[0].stride;
  uniforms.vertexBaseIndex = 0;
  uniforms.clipPlaneMask = desc.clipPlaneMask;
  uniforms.alphaTestEnable = !debug::disableAlphaTest() &&
                             desc.rs.values.contains(RS_ALPHA_TEST_ENABLE) &&
                             desc.rs.values.at(RS_ALPHA_TEST_ENABLE) != 0;
  uniforms.alphaTestFunc = desc.rs.values.contains(RS_ALPHA_FUNC)
                               ? desc.rs.values.at(RS_ALPHA_FUNC)
                               : static_cast<u32>(CompareFunc::Always);
  uniforms.fogMode = desc.rs.values.contains(RS_FOG_TABLE_MODE)
                         ? desc.rs.values.at(RS_FOG_TABLE_MODE)
                         : static_cast<u32>(FogMode::None);
  uniforms.alphaRef = desc.rs.values.contains(RS_ALPHA_REF)
                          ? static_cast<f32>(desc.rs.values.at(RS_ALPHA_REF)) / 255.0f
                          : 0.0f;
  uniforms.fogStart = desc.rs.values.contains(RS_FOG_START)
                          ? std::bit_cast<f32>(desc.rs.values.at(RS_FOG_START))
                          : 1.0f;
  uniforms.fogEnd = desc.rs.values.contains(RS_FOG_END)
                        ? std::bit_cast<f32>(desc.rs.values.at(RS_FOG_END))
                        : 1.0f;
  uniforms.fogDensity = desc.rs.values.contains(RS_FOG_DENSITY)
                            ? std::bit_cast<f32>(desc.rs.values.at(RS_FOG_DENSITY))
                            : 1.0f;
  for (std::size_t i = 0; i < core::kMaxClipPlanes; ++i) {
    uniforms.clipPlanes[i] = desc.clipPlanes[i];
  }
  return uniforms;
}

pipeline::DepthStencilKey makeDepthStencilKey(const core::DrawDesc& desc) {
  pipeline::DepthStencilKey key;
  if (debug::forceVisibleDraw()) {
    return key;
  }
  const auto it = desc.rs.values.find(RS_Z_ENABLE);
  key.depthEnable = it != desc.rs.values.end() && it->second != 0;
  const auto writeIt = desc.rs.values.find(RS_Z_WRITE_ENABLE);
  key.depthWrite = key.depthEnable && writeIt != desc.rs.values.end() && writeIt->second != 0;
  const auto funcIt = desc.rs.values.find(RS_Z_FUNC);
  key.depthFunc = key.depthEnable && funcIt != desc.rs.values.end()
                      ? funcIt->second
                      : static_cast<u32>(CompareFunc::Always);
  const auto stencilEnableIt = desc.rs.values.find(RS_STENCIL_ENABLE);
  key.front.enabled = stencilEnableIt != desc.rs.values.end() && stencilEnableIt->second != 0;
  const auto stencilFuncIt = desc.rs.values.find(RS_STENCIL_FUNC);
  key.front.compareFunction = stencilFuncIt != desc.rs.values.end() ? stencilFuncIt->second
                                                                    : static_cast<u32>(CompareFunc::Always);
  const auto stencilFailIt = desc.rs.values.find(RS_STENCIL_FAIL);
  key.front.failureOperation = stencilFailIt != desc.rs.values.end() ? stencilFailIt->second
                                                                     : static_cast<u32>(StencilOp::Keep);
  const auto stencilZFailIt = desc.rs.values.find(RS_STENCIL_ZFAIL);
  key.front.depthFailureOperation = stencilZFailIt != desc.rs.values.end() ? stencilZFailIt->second
                                                                           : static_cast<u32>(StencilOp::Keep);
  const auto stencilPassIt = desc.rs.values.find(RS_STENCIL_PASS);
  key.front.passOperation = stencilPassIt != desc.rs.values.end() ? stencilPassIt->second
                                                                  : static_cast<u32>(StencilOp::Keep);
  const auto stencilMaskIt = desc.rs.values.find(RS_STENCIL_MASK);
  key.front.readMask = stencilMaskIt != desc.rs.values.end() ? stencilMaskIt->second : 0xffu;
  const auto stencilWriteMaskIt = desc.rs.values.find(RS_STENCIL_WRITEMASK);
  key.front.writeMask = stencilWriteMaskIt != desc.rs.values.end() ? stencilWriteMaskIt->second : 0xffu;
  const auto ccwFuncIt = desc.rs.values.find(RS_STENCIL_CCW_FUNC);
  key.back.compareFunction = ccwFuncIt != desc.rs.values.end() ? ccwFuncIt->second : key.front.compareFunction;
  const auto ccwFailIt = desc.rs.values.find(RS_STENCIL_CCW_FAIL);
  key.back.failureOperation = ccwFailIt != desc.rs.values.end() ? ccwFailIt->second : key.front.failureOperation;
  const auto ccwZFailIt = desc.rs.values.find(RS_STENCIL_CCW_ZFAIL);
  key.back.depthFailureOperation = ccwZFailIt != desc.rs.values.end() ? ccwZFailIt->second
                                                                       : key.front.depthFailureOperation;
  const auto ccwPassIt = desc.rs.values.find(RS_STENCIL_CCW_PASS);
  key.back.passOperation = ccwPassIt != desc.rs.values.end() ? ccwPassIt->second : key.front.passOperation;
  const auto ccwMaskIt = desc.rs.values.find(RS_STENCIL_CCW_MASK);
  key.back.readMask = ccwMaskIt != desc.rs.values.end() ? ccwMaskIt->second : key.front.readMask;
  const auto ccwWriteMaskIt = desc.rs.values.find(RS_STENCIL_CCW_WRITEMASK);
  key.back.writeMask = ccwWriteMaskIt != desc.rs.values.end() ? ccwWriteMaskIt->second : key.front.writeMask;
  key.back.enabled = key.front.enabled;
  return key;
}

}  // namespace dxmt9::state
