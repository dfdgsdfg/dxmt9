#include "dxmt9/core.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <unordered_set>

namespace dxmt9::core {

// Split from core.cpp; keep this unit private to the D3D9 frontend.
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;

u64 hashCombineForState(u64 seed, u64 value) {
  seed ^= value;
  seed *= kFnvPrime;
  return seed;
}

u64 hashFfpVertexKeyForState(const FfpVertexKey &key) {
  u64 hash = kFnvOffset;
  hash = hashCombineForState(hash, static_cast<u64>(key.lightingEnabled));
  hash = hashCombineForState(hash, static_cast<u64>(key.specularEnabled));
  hash = hashCombineForState(hash, static_cast<u64>(key.normalizeNormals));
  for (bool enabled : key.lightEnabled) {
    hash = hashCombineForState(hash, static_cast<u64>(enabled));
  }
  for (u32 type : key.lightType) {
    hash = hashCombineForState(hash, type);
  }
  for (u32 mode : key.colorMaterialMode) {
    hash = hashCombineForState(hash, mode);
  }
  hash = hashCombineForState(hash, static_cast<u64>(key.fogMode));
  hash = hashCombineForState(hash, static_cast<u64>(key.fogFromVertex));
  hash = hashCombineForState(hash, static_cast<u64>(key.rangeFog));
  for (u32 value : key.texCoordGen) {
    hash = hashCombineForState(hash, value);
  }
  for (u32 value : key.texTransformFlags) {
    hash = hashCombineForState(hash, value);
  }
  hash = hashCombineForState(hash, key.vertexBlend);
  hash = hashCombineForState(hash, static_cast<u64>(key.indexedVertexBlend));
  hash = hashCombineForState(hash, key.clipPlaneMask);
  return hash;
}

u64 hashFfpPixelKeyForState(const FfpPixelKey &key) {
  u64 hash = kFnvOffset;
  for (const auto &stage : key.stages) {
    hash = hashCombineForState(hash, stage.colorOp);
    hash = hashCombineForState(hash, stage.colorArg1);
    hash = hashCombineForState(hash, stage.colorArg2);
    hash = hashCombineForState(hash, stage.colorArg0);
    hash = hashCombineForState(hash, stage.alphaOp);
    hash = hashCombineForState(hash, stage.alphaArg1);
    hash = hashCombineForState(hash, stage.alphaArg2);
    hash = hashCombineForState(hash, stage.alphaArg0);
    hash = hashCombineForState(hash, stage.resultArg);
    hash = hashCombineForState(hash, stage.texType);
    hash = hashCombineForState(hash, stage.texCoordIndex);
  }
  hash = hashCombineForState(hash, static_cast<u64>(key.fogMode));
  hash = hashCombineForState(hash, static_cast<u64>(key.alphaTestEnable));
  hash = hashCombineForState(hash, key.alphaTestFunc);
  return hash;
}

u64 hashShaderRefForState(const ShaderRef &ref) {
  switch (ref.kind) {
  case ShaderRef::Kind::Bytecode:
    return ref.hash != 0
               ? ref.hash
               : hashBytes(std::as_bytes(std::span<const u8>(
                     ref.bytecode.bytes.data(), ref.bytecode.bytes.size())));
  case ShaderRef::Kind::FixedFunctionVertex:
    return ref.vertexKey ? (ref.vertexKey->hash
                                ? ref.vertexKey->hash
                                : hashFfpVertexKeyForState(*ref.vertexKey))
                         : 0;
  case ShaderRef::Kind::FixedFunctionPixel:
    return ref.pixelKey
               ? (ref.pixelKey->hash ? ref.pixelKey->hash
                                     : hashFfpPixelKeyForState(*ref.pixelKey))
               : 0;
  case ShaderRef::Kind::None:
    return 0;
  }
  return 0;
}

} // namespace

void DeviceState::reset() {
  viewport = {};
  scissorRect = {};
  scissorEnabled = false;
  renderStates.clear();
  for (auto &stage : textureStageStates) {
    stage.clear();
  }
  for (auto &sampler : samplerStates) {
    sampler.clear();
  }
  transforms.clear();
  lights = {};
  lightEnabled.fill(false);
  material = {};
  streamBuffers.fill(nullptr);
  streamOffsets.fill(0);
  streamStrides.fill(0);
  indexBuffer.reset();
  indexType = IndexType::UInt16;
  vertexDecl = {};
  fvf = 0;
  vertexShader = {};
  pixelShader = {};
  vsConst = {};
  psConst = {};
  clipPlanes = {};
  textures.fill(nullptr);
  renderTargets = {};
  depthStencil = {};
  inScene = false;

  renderStates.set(RS_LIGHTING, 1);
  // Wine d3d9: RS_SHADEMODE default is D3DSHADE_GOURAUD (2);
  // RS_MULTISAMPLEMASK = 0xffffffff; RS_MULTISAMPLEANTIALIAS = TRUE.
  // visual_shademode_render_state_policy /
  // visual_sample_mask_render_state_policy.
  renderStates.set(9u /*RS_SHADEMODE*/, 2u);
  renderStates.set(161u /*RS_MULTISAMPLEANTIALIAS*/, 1u);
  renderStates.set(162u /*RS_MULTISAMPLEMASK*/, 0xffffffffu);
  renderStates.set(RS_SPECULAR_ENABLE, 0);
  renderStates.set(RS_NORMALIZE_NORMALS, 0);
  renderStates.set(RS_FOG_TABLE_MODE, static_cast<u32>(FogMode::None));
  renderStates.set(RS_FOG_FROM_VERTEX, 1);
  renderStates.set(RS_RANGE_FOG, 0);
  renderStates.set(RS_ALPHA_TEST_ENABLE, 0);
  renderStates.set(RS_ALPHA_FUNC, static_cast<u32>(CompareFunc::Always));
  renderStates.set(RS_ALPHA_REF, 0);
  renderStates.set(RS_FOG_ENABLE, 0);
  renderStates.set(RS_FOG_COLOR, 0);
  renderStates.set(RS_FOG_START, std::bit_cast<u32>(1.0f));
  renderStates.set(RS_FOG_END, std::bit_cast<u32>(1.0f));
  renderStates.set(RS_FOG_DENSITY, std::bit_cast<u32>(1.0f));
  renderStates.set(RS_AMBIENT, 0);
  renderStates.set(RS_DIFFUSE_MATERIAL_SOURCE, 1);
  renderStates.set(RS_SPECULAR_MATERIAL_SOURCE, 2);
  renderStates.set(RS_AMBIENT_MATERIAL_SOURCE, 0);
  renderStates.set(RS_EMISSIVE_MATERIAL_SOURCE, 0);
  renderStates.set(RS_VERTEX_BLEND, 0);
  renderStates.set(RS_CLIP_PLANE_ENABLE, 0);
  renderStates.set(RS_POINT_SPRITE_ENABLE, 0);
  renderStates.set(RS_POINT_SCALE_ENABLE, 0);
  renderStates.set(RS_CULL_MODE, static_cast<u32>(CullMode::Ccw));
  renderStates.set(RS_Z_WRITE_ENABLE, 1);
  renderStates.set(RS_Z_FUNC, static_cast<u32>(CompareFunc::LessEqual));
  renderStates.set(RS_SRC_BLEND, static_cast<u32>(BlendFactor::One));
  renderStates.set(RS_DEST_BLEND, static_cast<u32>(BlendFactor::Zero));
  renderStates.set(RS_BLEND_OP, static_cast<u32>(BlendOp::Add));
  renderStates.set(RS_COLOR_WRITE_ENABLE, 0xf);
  // Per-RT color-write masks default to all channels so MRT slots 1..3 match
  // RT0's default and single-RT apps (which only touch slot 168) are unaffected.
  renderStates.set(RS_COLOR_WRITE_ENABLE1, 0xf);
  renderStates.set(RS_COLOR_WRITE_ENABLE2, 0xf);
  renderStates.set(RS_COLOR_WRITE_ENABLE3, 0xf);
  renderStates.set(RS_Z_ENABLE, 1);
  renderStates.set(RS_ALPHABLEND_ENABLE, 0);
  renderStates.set(RS_BLEND_FACTOR, 0xffffffffu);
  renderStates.set(RS_SEPARATE_ALPHA_BLEND_ENABLE, 0);
  renderStates.set(RS_SRC_BLEND_ALPHA, static_cast<u32>(BlendFactor::One));
  renderStates.set(RS_DEST_BLEND_ALPHA, static_cast<u32>(BlendFactor::Zero));
  renderStates.set(RS_BLEND_OP_ALPHA, static_cast<u32>(BlendOp::Add));
  renderStates.set(RS_STENCIL_ENABLE, 0);
  renderStates.set(RS_STENCIL_FUNC, static_cast<u32>(CompareFunc::Always));
  renderStates.set(RS_STENCIL_FAIL, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_ZFAIL, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_PASS, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_REF, 0);
  renderStates.set(RS_STENCIL_MASK, 0xffu);
  renderStates.set(RS_STENCIL_WRITEMASK, 0xffu);
  renderStates.set(RS_STENCIL_CCW_FUNC, static_cast<u32>(CompareFunc::Always));
  renderStates.set(RS_STENCIL_CCW_FAIL, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_CCW_ZFAIL, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_CCW_PASS, static_cast<u32>(StencilOp::Keep));
  renderStates.set(RS_STENCIL_CCW_REF, 0);
  renderStates.set(RS_STENCIL_CCW_MASK, 0xffu);
  renderStates.set(RS_STENCIL_CCW_WRITEMASK, 0xffu);

  for (size_t stageIndex = 0; stageIndex < textureStageStates.size();
       ++stageIndex) {
    auto &stage = textureStageStates[stageIndex];
    stage.set(TSS_COLOR_OP,
              static_cast<u32>(stageIndex == 0 ? TextureOp::Modulate
                                               : TextureOp::Disable));
    stage.set(TSS_COLOR_ARG1, 2); // D3DTA_TEXTURE
    stage.set(TSS_COLOR_ARG2, 1); // D3DTA_CURRENT
    stage.set(TSS_ALPHA_OP,
              static_cast<u32>(stageIndex == 0 ? TextureOp::SelectArg1
                                               : TextureOp::Disable));
    stage.set(TSS_ALPHA_ARG1, 2); // D3DTA_TEXTURE
    stage.set(TSS_ALPHA_ARG2, 1); // D3DTA_CURRENT
    stage.set(TSS_RESULT_ARG, 1); // D3DTA_CURRENT
    stage.set(TSS_TEXCOORD_INDEX, static_cast<u32>(stageIndex));
    stage.set(TSS_TEXTURE_TRANSFORM_FLAGS, 0);
    stage.set(TSS_CONSTANT, 0);
    stage.set(TSS_TEXTURE_TYPE, 0);
  }

  for (auto &sampler : samplerStates) {
    sampler.set(SAMP_MIN_FILTER, 1);
    sampler.set(SAMP_MAG_FILTER, 1);
    sampler.set(SAMP_MIP_FILTER, 0);
    sampler.set(SAMP_ADDRESS_U, 1);
    sampler.set(SAMP_ADDRESS_V, 1);
    sampler.set(SAMP_ADDRESS_W, 1);
    sampler.set(SAMP_MAX_ANISOTROPY, 1);
    sampler.set(SAMP_MIPMAP_LOD_BIAS, 0);
    sampler.set(SAMP_BORDER_COLOR, 0);
  }

  material.diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
}

namespace {

template <typename Map, typename Key, typename Value>
void setMapValue(Map &dst, const Key &key, const Value &value) {
  dst[key] = value;
}

template <std::size_t MaxEntries>
void setMapValue(StateValueTable<MaxEntries> &dst, u32 key, u32 value) {
  dst.set(key, value);
}

template <typename Map>
void applyMapDelta(Map &dst, const Map &before, const Map &after) {
  for (const auto &entry : after) {
    const auto key = entry.first;
    const auto &value = entry.second;
    const auto beforeIt = before.find(key);
    if (beforeIt == before.end() || beforeIt->second != value) {
      setMapValue(dst, key, value);
    }
  }
  for (const auto &entry : before) {
    const auto key = entry.first;
    if (!after.contains(key)) {
      dst.erase(key);
    }
  }
}

template <typename Map>
void captureMapDelta(Map &snapshot, const Map &before, const Map &recorded,
                     const Map &current) {
  for (const auto &entry : recorded) {
    const auto key = entry.first;
    const auto &value = entry.second;
    const auto beforeIt = before.find(key);
    if (beforeIt == before.end() || beforeIt->second != value) {
      const auto currentIt = current.find(key);
      if (currentIt != current.end()) {
        setMapValue(snapshot, key, currentIt->second);
      } else {
        snapshot.erase(key);
      }
    }
  }
  for (const auto &entry : before) {
    const auto key = entry.first;
    const auto &value = entry.second;
    if (!recorded.contains(key)) {
      const auto currentIt = current.find(key);
      if (currentIt == current.end() || currentIt->second != value) {
        if (currentIt != current.end()) {
          setMapValue(snapshot, key, currentIt->second);
        } else {
          snapshot.erase(key);
        }
      }
    }
  }
}

template <typename T>
void applyIfChanged(T &dst, const T &before, const T &after) {
  if (!(before == after)) {
    dst = after;
  }
}

template <typename T>
void captureIfRecorded(T &snapshot, const T &before, const T &recorded,
                       const T &current) {
  if (!(before == recorded)) {
    snapshot = current;
  }
}

template <size_t FloatCount>
void applyShaderConstantsDelta(
    ShaderConstantSnapshot<FloatCount> &dst,
    const ShaderConstantSnapshot<FloatCount> &before,
    const ShaderConstantSnapshot<FloatCount> &after) {
  for (size_t i = 0; i < after.float4.size(); ++i) {
    if (before.float4[i] != after.float4[i]) {
      dst.float4[i] = after.float4[i];
    }
  }
  for (size_t i = 0; i < after.int4.size(); ++i) {
    if (before.int4[i] != after.int4[i]) {
      dst.int4[i] = after.int4[i];
    }
  }
  for (size_t i = 0; i < after.bools.size(); ++i) {
    if (before.bools[i] != after.bools[i]) {
      dst.bools[i] = after.bools[i];
    }
  }
}

template <size_t FloatCount>
void captureShaderConstantsDelta(
    ShaderConstantSnapshot<FloatCount> &snapshot,
    const ShaderConstantSnapshot<FloatCount> &before,
    const ShaderConstantSnapshot<FloatCount> &recorded,
    const ShaderConstantSnapshot<FloatCount> &current) {
  for (size_t i = 0; i < recorded.float4.size(); ++i) {
    if (before.float4[i] != recorded.float4[i]) {
      snapshot.float4[i] = current.float4[i];
    }
  }
  for (size_t i = 0; i < recorded.int4.size(); ++i) {
    if (before.int4[i] != recorded.int4[i]) {
      snapshot.int4[i] = current.int4[i];
    }
  }
  for (size_t i = 0; i < recorded.bools.size(); ++i) {
    if (before.bools[i] != recorded.bools[i]) {
      snapshot.bools[i] = current.bools[i];
    }
  }
}

constexpr u32 kRsFillMode = 8;
constexpr u32 kRsShadeMode = 9;
constexpr u32 kRsLastPixel = 16;
constexpr u32 kRsDitherEnable = 26;
constexpr u32 kRsWrap0 = 128;
constexpr u32 kRsWrap1 = 129;
constexpr u32 kRsWrap2 = 130;
constexpr u32 kRsWrap3 = 131;
constexpr u32 kRsWrap4 = 132;
constexpr u32 kRsWrap5 = 133;
constexpr u32 kRsWrap6 = 134;
constexpr u32 kRsWrap7 = 135;
constexpr u32 kRsClipping = 136;
constexpr u32 kRsColorVertex = 141;
constexpr u32 kRsLocalViewer = 142;
constexpr u32 kRsMultisampleAntialias = 161;
constexpr u32 kRsMultisampleMask = 162;
constexpr u32 kRsPatchEdgeStyle = 163;
constexpr u32 kRsIndexedVertexBlendEnable = 167;
constexpr u32 kRsTweenFactor = 170;
constexpr u32 kRsPositionDegree = 172;
constexpr u32 kRsNormalDegree = 173;
constexpr u32 kRsSlopeScaleDepthBias = 175;
constexpr u32 kRsAntialiasedLineEnable = 176;
constexpr u32 kRsMinTessellationLevel = 178;
constexpr u32 kRsMaxTessellationLevel = 179;
constexpr u32 kRsAdaptiveTessX = 180;
constexpr u32 kRsAdaptiveTessY = 181;
constexpr u32 kRsAdaptiveTessZ = 182;
constexpr u32 kRsAdaptiveTessW = 183;
constexpr u32 kRsEnableAdaptiveTessellation = 184;
constexpr u32 kRsTwoSidedStencilMode = 185;
constexpr u32 kRsColorWriteEnable1 = 190;
constexpr u32 kRsColorWriteEnable2 = 191;
constexpr u32 kRsColorWriteEnable3 = 192;
constexpr u32 kRsDepthBias = 195;
constexpr u32 kRsWrap8 = 198;
constexpr u32 kRsWrap9 = 199;
constexpr u32 kRsWrap10 = 200;
constexpr u32 kRsWrap11 = 201;
constexpr u32 kRsWrap12 = 202;
constexpr u32 kRsWrap13 = 203;
constexpr u32 kRsWrap14 = 204;
constexpr u32 kRsWrap15 = 205;

constexpr auto kPixelStateRenderStates = std::to_array<u32>({
    RS_ALPHABLEND_ENABLE,
    RS_ALPHA_FUNC,
    RS_ALPHA_REF,
    RS_ALPHA_TEST_ENABLE,
    kRsAntialiasedLineEnable,
    RS_BLEND_FACTOR,
    RS_BLEND_OP,
    RS_BLEND_OP_ALPHA,
    RS_STENCIL_CCW_FAIL,
    RS_STENCIL_CCW_FUNC,
    RS_STENCIL_CCW_PASS,
    RS_STENCIL_CCW_ZFAIL,
    RS_COLOR_WRITE_ENABLE,
    kRsColorWriteEnable1,
    kRsColorWriteEnable2,
    kRsColorWriteEnable3,
    kRsDepthBias,
    RS_DEST_BLEND,
    RS_DEST_BLEND_ALPHA,
    kRsDitherEnable,
    kRsFillMode,
    RS_FOG_DENSITY,
    RS_FOG_END,
    RS_FOG_START,
    kRsLastPixel,
    RS_SCISSOR_TEST_ENABLE,
    RS_SEPARATE_ALPHA_BLEND_ENABLE,
    kRsShadeMode,
    kRsSlopeScaleDepthBias,
    RS_SRC_BLEND,
    RS_SRC_BLEND_ALPHA,
    RS_SRGB_WRITE_ENABLE,
    RS_STENCIL_ENABLE,
    RS_STENCIL_FAIL,
    RS_STENCIL_FUNC,
    RS_STENCIL_MASK,
    RS_STENCIL_PASS,
    RS_STENCIL_REF,
    RS_STENCIL_WRITEMASK,
    RS_STENCIL_ZFAIL,
    RS_TEXTURE_FACTOR,
    kRsTwoSidedStencilMode,
    kRsWrap0,
    kRsWrap1,
    kRsWrap10,
    kRsWrap11,
    kRsWrap12,
    kRsWrap13,
    kRsWrap14,
    kRsWrap15,
    kRsWrap2,
    kRsWrap3,
    kRsWrap4,
    kRsWrap5,
    kRsWrap6,
    kRsWrap7,
    kRsWrap8,
    kRsWrap9,
    RS_Z_ENABLE,
    RS_Z_FUNC,
    RS_Z_WRITE_ENABLE,
});

constexpr auto kVertexStateRenderStates = std::to_array<u32>({
    kRsAdaptiveTessW,
    kRsAdaptiveTessX,
    kRsAdaptiveTessY,
    kRsAdaptiveTessZ,
    RS_AMBIENT,
    RS_AMBIENT_MATERIAL_SOURCE,
    kRsClipping,
    RS_CLIP_PLANE_ENABLE,
    kRsColorVertex,
    RS_CULL_MODE,
    RS_DIFFUSE_MATERIAL_SOURCE,
    RS_EMISSIVE_MATERIAL_SOURCE,
    kRsEnableAdaptiveTessellation,
    RS_FOG_COLOR,
    RS_FOG_DENSITY,
    RS_FOG_ENABLE,
    RS_FOG_END,
    RS_FOG_START,
    RS_FOG_TABLE_MODE,
    RS_FOG_FROM_VERTEX,
    kRsIndexedVertexBlendEnable,
    RS_LIGHTING,
    kRsLocalViewer,
    kRsMaxTessellationLevel,
    kRsMinTessellationLevel,
    kRsMultisampleAntialias,
    kRsMultisampleMask,
    kRsNormalDegree,
    RS_NORMALIZE_NORMALS,
    kRsPatchEdgeStyle,
    RS_POINTSCALE_A,
    RS_POINTSCALE_B,
    RS_POINTSCALE_C,
    RS_POINT_SCALE_ENABLE,
    RS_POINTSIZE,
    RS_POINTSIZE_MAX,
    RS_POINTSIZE_MIN,
    RS_POINT_SPRITE_ENABLE,
    kRsPositionDegree,
    RS_RANGE_FOG,
    kRsShadeMode,
    RS_SPECULAR_ENABLE,
    RS_SPECULAR_MATERIAL_SOURCE,
    kRsTweenFactor,
    RS_VERTEX_BLEND,
});

// Wine oracle: dlls/wined3d/stateblock.c vertex_states_texture[] (lines 254-258).
// D3DSBT_VERTEXSTATE captures these per-stage TSS keys; pixel TSS is the
// superset, so the PixelState branch keeps its whole-table copy.
constexpr auto kVertexStateTextureStageStates = std::to_array<u32>({
    TSS_TEXCOORD_INDEX,
    TSS_TEXTURE_TRANSFORM_FLAGS,
});

template <size_t N>
void copyRenderStates(DeviceState &dst, const DeviceState &src,
                      const std::array<u32, N> &keys) {
  for (u32 key : keys) {
    const auto it = src.renderStates.find(key);
    if (it != src.renderStates.end()) {
      dst.renderStates.set(key, it->second);
    } else {
      dst.renderStates.erase(key);
    }
  }
}

template <size_t N>
void copyTextureStageStateSlice(
    std::array<TextureStageStateTable, kMaxTextureStages> &dst,
    const std::array<TextureStageStateTable, kMaxTextureStages> &src,
    const std::array<u32, N> &keys) {
  for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    for (u32 key : keys) {
      const auto it = src[stage].find(key);
      if (it != src[stage].end()) {
        dst[stage].set(key, it->second);
      } else {
        dst[stage].erase(key);
      }
    }
  }
}

void syncRenderStateDerived(DeviceState &state) {
  const auto scissorIt = state.renderStates.find(RS_SCISSOR_TEST_ENABLE);
  state.scissorEnabled =
      scissorIt != state.renderStates.end() && scissorIt->second != 0;
}

void applyFullSnapshotState(DeviceState &dst, const DeviceState &src,
                            StateBlockType type) {
  switch (type) {
  case StateBlockType::All:
  case StateBlockType::Recorded: {
    const bool inScene = dst.inScene;
    dst = src;
    dst.inScene = inScene;
    return;
  }
  case StateBlockType::PixelState:
    copyRenderStates(dst, src, kPixelStateRenderStates);
    dst.textureStageStates = src.textureStageStates;
    dst.samplerStates = src.samplerStates;
    dst.pixelShader = src.pixelShader;
    dst.psConst = src.psConst;
    syncRenderStateDerived(dst);
    return;
  case StateBlockType::VertexState:
    copyRenderStates(dst, src, kVertexStateRenderStates);
    copyTextureStageStateSlice(dst.textureStageStates, src.textureStageStates,
                               kVertexStateTextureStageStates);
    dst.vertexDecl = src.vertexDecl;
    dst.fvf = src.fvf;
    dst.vertexShader = src.vertexShader;
    dst.vsConst = src.vsConst;
    dst.lights = src.lights;
    dst.lightEnabled = src.lightEnabled;
    syncRenderStateDerived(dst);
    return;
  }
}

} // namespace

void StateBlock::capture(const DeviceState &state) {
  if (mode_ == CaptureMode::FullSnapshot) {
    snapshot_ = state;
    baseline_ = {};
    recordedRenderStates_.clear();
    return;
  }

  const DeviceState recorded = snapshot_;
  captureIfRecorded(snapshot_.viewport, baseline_.viewport, recorded.viewport,
                    state.viewport);
  captureIfRecorded(snapshot_.scissorRect, baseline_.scissorRect,
                    recorded.scissorRect, state.scissorRect);
  captureIfRecorded(snapshot_.scissorEnabled, baseline_.scissorEnabled,
                    recorded.scissorEnabled, state.scissorEnabled);
  if (!recordedRenderStates_.empty()) {
    for (u32 key : recordedRenderStates_) {
      const auto currentIt = state.renderStates.find(key);
      if (currentIt != state.renderStates.end()) {
        snapshot_.renderStates.set(key, currentIt->second);
      } else {
        snapshot_.renderStates.erase(key);
      }
    }
  } else {
    captureMapDelta(snapshot_.renderStates, baseline_.renderStates,
                    recorded.renderStates, state.renderStates);
  }
  for (size_t i = 0; i < snapshot_.textureStageStates.size(); ++i) {
    captureMapDelta(
        snapshot_.textureStageStates[i], baseline_.textureStageStates[i],
        recorded.textureStageStates[i], state.textureStageStates[i]);
  }
  for (size_t i = 0; i < snapshot_.samplerStates.size(); ++i) {
    captureMapDelta(snapshot_.samplerStates[i], baseline_.samplerStates[i],
                    recorded.samplerStates[i], state.samplerStates[i]);
  }
  captureMapDelta(snapshot_.transforms, baseline_.transforms,
                  recorded.transforms, state.transforms);
  for (size_t i = 0; i < snapshot_.lights.size(); ++i) {
    captureIfRecorded(snapshot_.lights[i], baseline_.lights[i],
                      recorded.lights[i], state.lights[i]);
    captureIfRecorded(snapshot_.lightEnabled[i], baseline_.lightEnabled[i],
                      recorded.lightEnabled[i], state.lightEnabled[i]);
  }
  captureIfRecorded(snapshot_.material, baseline_.material, recorded.material,
                    state.material);
  for (size_t i = 0; i < snapshot_.streamBuffers.size(); ++i) {
    if (baseline_.streamBuffers[i] != recorded.streamBuffers[i] ||
        baseline_.streamOffsets[i] != recorded.streamOffsets[i] ||
        baseline_.streamStrides[i] != recorded.streamStrides[i]) {
      snapshot_.streamBuffers[i] = state.streamBuffers[i];
      snapshot_.streamOffsets[i] = state.streamOffsets[i];
      snapshot_.streamStrides[i] = state.streamStrides[i];
    }
  }
  if (baseline_.indexBuffer != recorded.indexBuffer ||
      baseline_.indexType != recorded.indexType) {
    snapshot_.indexBuffer = state.indexBuffer;
    snapshot_.indexType = state.indexType;
  }
  captureIfRecorded(snapshot_.vertexDecl, baseline_.vertexDecl,
                    recorded.vertexDecl, state.vertexDecl);
  captureIfRecorded(snapshot_.fvf, baseline_.fvf, recorded.fvf, state.fvf);
  captureIfRecorded(snapshot_.vertexShader, baseline_.vertexShader,
                    recorded.vertexShader, state.vertexShader);
  captureIfRecorded(snapshot_.pixelShader, baseline_.pixelShader,
                    recorded.pixelShader, state.pixelShader);
  captureShaderConstantsDelta(snapshot_.vsConst, baseline_.vsConst,
                              recorded.vsConst, state.vsConst);
  captureShaderConstantsDelta(snapshot_.psConst, baseline_.psConst,
                              recorded.psConst, state.psConst);
  for (size_t i = 0; i < snapshot_.textures.size(); ++i) {
    captureIfRecorded(snapshot_.textures[i], baseline_.textures[i],
                      recorded.textures[i], state.textures[i]);
  }
  for (size_t i = 0; i < snapshot_.renderTargets.size(); ++i) {
    captureIfRecorded(snapshot_.renderTargets[i], baseline_.renderTargets[i],
                      recorded.renderTargets[i], state.renderTargets[i]);
  }
  captureIfRecorded(snapshot_.depthStencil, baseline_.depthStencil,
                    recorded.depthStencil, state.depthStencil);
  for (size_t i = 0; i < snapshot_.clipPlanes.size(); ++i) {
    captureIfRecorded(snapshot_.clipPlanes[i], baseline_.clipPlanes[i],
                      recorded.clipPlanes[i], state.clipPlanes[i]);
  }
}

void StateBlock::captureDelta(const DeviceState &before,
                              const DeviceState &after) {
  mode_ = CaptureMode::Delta;
  type_ = StateBlockType::Recorded;
  baseline_ = before;
  snapshot_ = after;
  recordedRenderStates_.clear();
}

void StateBlock::captureDelta(
    const DeviceState &before, const DeviceState &after,
    const std::unordered_set<u32> &recordedRenderStates) {
  mode_ = CaptureMode::Delta;
  type_ = StateBlockType::Recorded;
  baseline_ = before;
  snapshot_ = after;
  recordedRenderStates_ = recordedRenderStates;
}

void StateBlock::apply(Device &device) const {
  auto &state = device.mutableState();
  if (mode_ == CaptureMode::FullSnapshot) {
    applyFullSnapshotState(state, snapshot_, type_);
    return;
  }

  applyIfChanged(state.viewport, baseline_.viewport, snapshot_.viewport);
  applyIfChanged(state.scissorRect, baseline_.scissorRect,
                 snapshot_.scissorRect);
  applyIfChanged(state.scissorEnabled, baseline_.scissorEnabled,
                 snapshot_.scissorEnabled);
  if (!recordedRenderStates_.empty()) {
    for (u32 key : recordedRenderStates_) {
      const auto snapshotIt = snapshot_.renderStates.find(key);
      if (snapshotIt != snapshot_.renderStates.end()) {
        state.renderStates.set(key, snapshotIt->second);
      } else {
        state.renderStates.erase(key);
      }
    }
  } else {
    applyMapDelta(state.renderStates, baseline_.renderStates,
                  snapshot_.renderStates);
  }
  syncRenderStateDerived(state);
  for (size_t i = 0; i < state.textureStageStates.size(); ++i) {
    applyMapDelta(state.textureStageStates[i], baseline_.textureStageStates[i],
                  snapshot_.textureStageStates[i]);
  }
  for (size_t i = 0; i < state.samplerStates.size(); ++i) {
    applyMapDelta(state.samplerStates[i], baseline_.samplerStates[i],
                  snapshot_.samplerStates[i]);
  }
  applyMapDelta(state.transforms, baseline_.transforms, snapshot_.transforms);
  for (size_t i = 0; i < state.lights.size(); ++i) {
    applyIfChanged(state.lights[i], baseline_.lights[i], snapshot_.lights[i]);
    applyIfChanged(state.lightEnabled[i], baseline_.lightEnabled[i],
                   snapshot_.lightEnabled[i]);
  }
  applyIfChanged(state.material, baseline_.material, snapshot_.material);
  for (size_t i = 0; i < state.streamBuffers.size(); ++i) {
    if (baseline_.streamBuffers[i] != snapshot_.streamBuffers[i] ||
        baseline_.streamOffsets[i] != snapshot_.streamOffsets[i] ||
        baseline_.streamStrides[i] != snapshot_.streamStrides[i]) {
      state.streamBuffers[i] = snapshot_.streamBuffers[i];
      state.streamOffsets[i] = snapshot_.streamOffsets[i];
      state.streamStrides[i] = snapshot_.streamStrides[i];
    }
  }
  if (baseline_.indexBuffer != snapshot_.indexBuffer ||
      baseline_.indexType != snapshot_.indexType) {
    state.indexBuffer = snapshot_.indexBuffer;
    state.indexType = snapshot_.indexType;
  }
  applyIfChanged(state.vertexDecl, baseline_.vertexDecl, snapshot_.vertexDecl);
  applyIfChanged(state.fvf, baseline_.fvf, snapshot_.fvf);
  applyIfChanged(state.vertexShader, baseline_.vertexShader,
                 snapshot_.vertexShader);
  applyIfChanged(state.pixelShader, baseline_.pixelShader,
                 snapshot_.pixelShader);
  applyShaderConstantsDelta(state.vsConst, baseline_.vsConst,
                            snapshot_.vsConst);
  applyShaderConstantsDelta(state.psConst, baseline_.psConst,
                            snapshot_.psConst);
  for (size_t i = 0; i < state.textures.size(); ++i) {
    applyIfChanged(state.textures[i], baseline_.textures[i],
                   snapshot_.textures[i]);
  }
  for (size_t i = 0; i < state.renderTargets.size(); ++i) {
    applyIfChanged(state.renderTargets[i], baseline_.renderTargets[i],
                   snapshot_.renderTargets[i]);
  }
  applyIfChanged(state.depthStencil, baseline_.depthStencil,
                 snapshot_.depthStencil);
  for (size_t i = 0; i < state.clipPlanes.size(); ++i) {
    applyIfChanged(state.clipPlanes[i], baseline_.clipPlanes[i],
                   snapshot_.clipPlanes[i]);
  }
}

std::shared_ptr<StateBlock>
Device::createStateBlock(StateBlockType type) const {
  auto block = std::make_shared<StateBlock>(type);
  block->capture(state_);
  return block;
}

std::shared_ptr<StateBlock> Device::captureStateBlock() const {
  return createStateBlock();
}

HResult Device::applyStateBlock(const StateBlock &block) {
  block.apply(*this);
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setRenderState(u32 key, u32 value) {
  state_.renderStates.set(key, value);
  if (key == RS_SCISSOR_TEST_ENABLE) {
    state_.scissorEnabled = value != 0;
  }
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setRenderStateFloat(u32 key, f32 value) {
  state_.renderStates.set(key, std::bit_cast<u32>(value));
  invalidateDrawStateCache();
  return D3D_OK;
}

u32 Device::getRenderState(u32 key) const {
  if (auto it = state_.renderStates.find(key);
      it != state_.renderStates.end()) {
    return it->second;
  }
  return 0;
}

f32 Device::getRenderStateFloat(u32 key, f32 defaultValue) const {
  if (auto it = state_.renderStates.find(key);
      it != state_.renderStates.end()) {
    return std::bit_cast<f32>(it->second);
  }
  return defaultValue;
}

HResult Device::setTextureStageState(u32 stage, u32 key, u32 value) {
  stage = std::min<u32>(stage, kMaxTextureStages - 1);
  key = std::min<u32>(key, kMaxTextureStageStates - 1);
  state_.textureStageStates[stage].set(key, value);
  invalidateDrawStateCache();
  return D3D_OK;
}

u32 Device::getTextureStageState(u32 stage, u32 key) const {
  if (stage >= kMaxTextureStages) {
    return 0;
  }
  const auto &map = state_.textureStageStates[stage];
  if (auto it = map.find(key); it != map.end()) {
    return it->second;
  }
  return 0;
}

HResult Device::setSamplerState(u32 sampler, u32 key, u32 value) {
  if (sampler >= kMaxSamplers) {
    return D3DERR_INVALIDCALL;
  }
  state_.samplerStates[sampler].set(key, value);
  invalidateDrawStateCache();
  return D3D_OK;
}

u32 Device::getSamplerState(u32 sampler, u32 key) const {
  if (sampler >= kMaxSamplers) {
    return 0;
  }
  const auto &map = state_.samplerStates[sampler];
  if (auto it = map.find(key); it != map.end()) {
    return it->second;
  }
  return 0;
}

void Device::notifyTextureLodChanged(const Texture &texture) {
  const auto handle = texture.handle();
  for (const auto &bound : state_.textures) {
    if (bound && bound->handle() == handle) {
      invalidateDrawStateCache();
      return;
    }
  }
}

HResult Device::setTransform(u32 key, const Matrix4x4 &matrix) {
  state_.transforms.set(key, matrix);
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setLight(u32 index, const Light &light) {
  if (index >= kMaxLights) {
    return D3DERR_INVALIDCALL;
  }
  state_.lights[index] = light;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::lightEnable(u32 index, bool enable) {
  if (index >= kMaxLights) {
    return D3DERR_INVALIDCALL;
  }
  state_.lightEnabled[index] = enable;
  state_.lights[index].enabled = enable;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setMaterial(const Material &material) {
  state_.material = material;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setTexture(u32 stage, std::shared_ptr<Texture> texture) {
  if (stage >= kMaxTextures) {
    return D3DERR_INVALIDCALL;
  }
  if (stage < kMaxTextureStages) {
    const u32 type = texture ? static_cast<u32>(texture->desc().type) : 0u;
    state_.textureStageStates[stage].set(TSS_TEXTURE_TYPE, type);
  }
  state_.textures[stage] = std::move(texture);
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setStreamSource(u32 stream, std::shared_ptr<Buffer> buffer,
                                u32 offset, u32 stride) {
  if (stream >= kMaxStreams) {
    return D3DERR_INVALIDCALL;
  }
  state_.streamBuffers[stream] = std::move(buffer);
  state_.streamOffsets[stream] = offset;
  state_.streamStrides[stream] = stride;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setIndices(std::shared_ptr<Buffer> buffer,
                           IndexType indexType) {
  state_.indexBuffer = std::move(buffer);
  state_.indexType = indexType;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setFVF(u32 fvf) {
  state_.fvf = fvf;
  state_.vertexDecl.fvf = fvf;
  state_.vertexDecl.elements.clear();
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setVertexDeclaration(std::vector<VertexElement> elements) {
  state_.vertexDecl.elements = std::move(elements);
  state_.vertexDecl.fvf = state_.fvf;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setVertexShader(const ShaderRef &shader) {
  state_.vertexShader = shader;
  if (state_.vertexShader.hash == 0) {
    state_.vertexShader.hash = hashShaderRefForState(state_.vertexShader);
  }
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setPixelShader(const ShaderRef &shader) {
  state_.pixelShader = shader;
  if (state_.pixelShader.hash == 0) {
    state_.pixelShader.hash = hashShaderRefForState(state_.pixelShader);
  }
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setClipPlane(u32 index, const ClipPlane &plane) {
  if (index >= kMaxClipPlanes) {
    return D3DERR_INVALIDCALL;
  }
  state_.clipPlanes[index] = plane;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setViewport(const Viewport &viewport) {
  if (viewport.width == 0 || viewport.height == 0 ||
      !std::isfinite(viewport.minZ) || !std::isfinite(viewport.maxZ) ||
      viewport.minZ < 0.0f || viewport.maxZ > 1.0f ||
      viewport.minZ > viewport.maxZ) {
    return D3DERR_INVALIDCALL;
  }
  state_.viewport = viewport;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setScissorRect(const Rect &rect) {
  state_.scissorRect = rect;
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setRenderTarget(u32 index, std::shared_ptr<Surface> surface) {
  if (index >= kMaxRenderTargets) {
    return D3DERR_INVALIDCALL;
  }
  state_.renderTargets[index] =
      surface ? RenderTargetAttachment{surface->handle(), surface->level(),
                                       surface->multiSampleCount()}
              : RenderTargetAttachment{};
  if (index == 0 && surface) {
    const auto &desc = surface->desc();
    state_.viewport = {
        0, 0, std::max(1u, desc.width), std::max(1u, desc.height), 0.0f, 1.0f};
  }
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::setDepthStencilSurface(std::shared_ptr<Surface> surface) {
  state_.depthStencil =
      surface ? RenderTargetAttachment{surface->handle(), surface->level(),
                                       surface->multiSampleCount()}
              : RenderTargetAttachment{};
  invalidateDrawStateCache();
  return D3D_OK;
}

HResult Device::beginScene() {
  if (state_.inScene) {
    return D3DERR_INVALIDCALL;
  }
  state_.inScene = true;
  inScene_ = true;
  return D3D_OK;
}

HResult Device::endScene() {
  if (!state_.inScene) {
    return D3DERR_INVALIDCALL;
  }
  state_.inScene = false;
  inScene_ = false;
  return D3D_OK;
}

void Device::resetState() {
  state_.reset();
  invalidateDrawStateCache();
}

} // namespace dxmt9::core
