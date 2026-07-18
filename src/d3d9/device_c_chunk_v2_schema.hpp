#pragma once

#include "dxmt9/device_c.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt9::d3d9 {

static_assert(std::endian::native == std::endian::little,
              "command chunk wire V2 requires a little-endian target");

enum V2RecordRuleFlags : std::uint32_t {
  V2RecordRuleNone = 0u,
  V2RecordRuleVariableTail = 1u << 0,
  V2RecordRuleDraw = 1u << 1,
  V2RecordRuleOrderingBoundary = 1u << 2,
  V2RecordRuleSynchronousBoundary = 1u << 3,
  V2RecordRuleSparseState = 1u << 4,
  V2RecordRuleHandleRefs = 1u << 5,
};

struct V2RecordRule {
  std::uint32_t type;
  std::uint32_t fixedPayloadSize;
  std::uint32_t payloadAlignment;
  std::uint32_t allowedRecordFlags;
  std::uint32_t ruleFlags;
};

inline constexpr std::array<V2RecordRule, 20> kV2RecordRules = {{
    {D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
     sizeof(D9CCommandChunkWireDrawHeaderV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleVariableTail | V2RecordRuleDraw |
         V2RecordRuleSparseState | V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
     sizeof(D9CCommandChunkWireDrawHeaderV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleVariableTail | V2RecordRuleDraw |
         V2RecordRuleSparseState | V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
     sizeof(D9CCommandChunkWireDrawHeaderV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleVariableTail | V2RecordRuleDraw |
         V2RecordRuleSparseState | V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
     sizeof(D9CCommandChunkWireDrawHeaderV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleVariableTail | V2RecordRuleDraw |
         V2RecordRuleSparseState | V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_SET_VS_CONST_F,
     sizeof(D9CCommandChunkWireSetConstV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE, V2RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_VS_CONST_I,
     sizeof(D9CCommandChunkWireSetConstV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE, V2RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_VS_CONST_B,
     sizeof(D9CCommandChunkWireSetConstV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE, V2RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_PS_CONST_F,
     sizeof(D9CCommandChunkWireSetConstV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE, V2RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_PS_CONST_I,
     sizeof(D9CCommandChunkWireSetConstV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE, V2RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_PS_CONST_B,
     sizeof(D9CCommandChunkWireSetConstV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE, V2RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_CLEAR, sizeof(D9CCommandChunkWireClearV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleVariableTail | V2RecordRuleOrderingBoundary},
    {D9C_COMMAND_RECORD_PRESENT, sizeof(D9CCommandChunkWirePresentV2), 8u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE, V2RecordRuleOrderingBoundary},
    {D9C_COMMAND_RECORD_STRETCH_RECT,
     sizeof(D9CCommandChunkWireStretchRectV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleOrderingBoundary | V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_COLOR_FILL,
     sizeof(D9CCommandChunkWireColorFillV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleOrderingBoundary | V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_UPDATE_TEXTURE,
     sizeof(D9CCommandChunkWireUpdateTextureV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleOrderingBoundary | V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_UPDATE_SURFACE,
     sizeof(D9CCommandChunkWireUpdateSurfaceV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleOrderingBoundary | V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_QUERY_ISSUE,
     sizeof(D9CCommandChunkWireQueryIssueV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleOrderingBoundary | V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_READBACK, sizeof(D9CCommandChunkWireReadbackV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleOrderingBoundary | V2RecordRuleSynchronousBoundary |
         V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_APPLY_STATE,
     sizeof(D9CCommandChunkWireDrawHeaderV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleVariableTail | V2RecordRuleSparseState |
         V2RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
     sizeof(D9CCommandChunkWireReszDepthResolveV2), 4u,
     D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
     V2RecordRuleOrderingBoundary | V2RecordRuleHandleRefs},
}};

enum V2SectionRuleFlags : std::uint32_t {
  V2SectionRuleNone = 0u,
  V2SectionRuleSingle = 1u << 0,
  V2SectionRuleHandleRefs = 1u << 1,
  V2SectionRuleConstantRange = 1u << 2,
  V2SectionRuleRawBytes = 1u << 3,
  V2SectionRuleFullSnapshotSlots = 1u << 4,
};

inline constexpr std::uint32_t kV2NoHandleKind =
    std::numeric_limits<std::uint32_t>::max();

struct V2SectionRule {
  std::uint16_t kind;
  std::uint16_t elementSize;
  std::uint32_t payloadAlignment;
  std::uint32_t maxCount;
  std::uint32_t ruleFlags;
  std::uint32_t handleKind;
};

inline constexpr std::array<V2SectionRule,
                            D9C_COMMAND_CHUNK_V2_SECTION_COUNT>
    kV2SectionRules = {{
        {D9C_COMMAND_CHUNK_V2_SECTION_RENDER_STATE,
         sizeof(D9CCommandChunkWireRenderStateV2), 4u,
         D9C_DRAW_PACKET_MAX_RENDER_STATES, V2SectionRuleNone,
         kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE,
         sizeof(D9CCommandChunkWireTextureBindingV2), 4u,
         D9C_DRAW_PACKET_MAX_TEXTURES,
         V2SectionRuleHandleRefs | V2SectionRuleFullSnapshotSlots,
         D9C_CHUNK_HANDLE_KIND_TEXTURE},
        {D9C_COMMAND_CHUNK_V2_SECTION_STREAM,
         sizeof(D9CCommandChunkWireStreamBindingV2), 4u,
         D9C_DRAW_PACKET_MAX_STREAMS,
         V2SectionRuleHandleRefs | V2SectionRuleFullSnapshotSlots,
         D9C_CHUNK_HANDLE_KIND_BUFFER},
        {D9C_COMMAND_CHUNK_V2_SECTION_SHADER,
         sizeof(D9CCommandChunkWireShaderBindingV2), 4u, 2u,
         V2SectionRuleHandleRefs, D9C_CHUNK_HANDLE_KIND_SHADER},
        {D9C_COMMAND_CHUNK_V2_SECTION_VERTEX_INPUT,
         sizeof(D9CCommandChunkWireVertexInputV2), 4u, 1u,
         V2SectionRuleSingle | V2SectionRuleHandleRefs,
         D9C_CHUNK_HANDLE_KIND_VERTEX_DECL},
        {D9C_COMMAND_CHUNK_V2_SECTION_INDEX_BUFFER,
         sizeof(D9CCommandChunkWireIndexBindingV2), 4u, 1u,
         V2SectionRuleSingle | V2SectionRuleHandleRefs,
         D9C_CHUNK_HANDLE_KIND_BUFFER},
        {D9C_COMMAND_CHUNK_V2_SECTION_RENDER_TARGET,
         sizeof(D9CCommandChunkWireRenderTargetBindingV2), 4u,
         D9C_DRAW_PACKET_MAX_RENDER_TARGETS, V2SectionRuleHandleRefs,
         D9C_CHUNK_HANDLE_KIND_SURFACE},
        {D9C_COMMAND_CHUNK_V2_SECTION_DEPTH_STENCIL,
         sizeof(D9CCommandChunkWireDepthStencilBindingV2), 4u, 1u,
         V2SectionRuleSingle | V2SectionRuleHandleRefs,
         D9C_CHUNK_HANDLE_KIND_SURFACE},
        {D9C_COMMAND_CHUNK_V2_SECTION_VIEWPORT, sizeof(D9CViewport), 4u, 1u,
         V2SectionRuleSingle, kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_SCISSOR, sizeof(D9CRect), 4u, 1u,
         V2SectionRuleSingle, kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_MATERIAL, sizeof(D9CMaterial), 4u, 1u,
         V2SectionRuleSingle, kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_CLIP_PLANE,
         sizeof(D9CCommandChunkWireClipPlaneV2), 4u, 6u, V2SectionRuleNone,
         kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE_STAGE_STATE,
         sizeof(D9CDrawPacketTextureStageState), 4u, D9C_DRAW_PACKET_MAX_TSS,
         V2SectionRuleNone, kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_SAMPLER_STATE,
         sizeof(D9CDrawPacketSamplerState), 4u, D9C_DRAW_PACKET_MAX_SAMPLER,
         V2SectionRuleNone, kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_TRANSFORM,
         sizeof(D9CDrawPacketTransform), 4u, D9C_DRAW_PACKET_MAX_TRANSFORMS,
         V2SectionRuleNone, kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_LIGHT,
         sizeof(D9CCommandChunkWireLightV2), 4u, D9C_DRAW_PACKET_MAX_LIGHTS,
         V2SectionRuleNone, kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_LIGHT_ENABLE,
         sizeof(D9CCommandChunkWireLightEnableV2), 4u,
         D9C_DRAW_PACKET_MAX_LIGHTS, V2SectionRuleNone, kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_F, 16u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_VS_F, V2SectionRuleConstantRange,
         kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_I, 16u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_VS_I, V2SectionRuleConstantRange,
         kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_B, 4u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_VS_B, V2SectionRuleConstantRange,
         kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_F, 16u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_PS_F, V2SectionRuleConstantRange,
         kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_I, 16u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_PS_I, V2SectionRuleConstantRange,
         kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_B, 4u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_PS_B, V2SectionRuleConstantRange,
         kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_UP_INDEX_DATA, 1u, 4u,
         std::numeric_limits<std::uint32_t>::max(), V2SectionRuleRawBytes,
         kV2NoHandleKind},
        {D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA, 1u, 4u,
         std::numeric_limits<std::uint32_t>::max(), V2SectionRuleRawBytes,
         kV2NoHandleKind},
    }};

constexpr const V2RecordRule* v2RecordRule(std::uint32_t type) {
  for (const auto& rule : kV2RecordRules) {
    if (rule.type == type) {
      return &rule;
    }
  }
  return nullptr;
}

constexpr const V2SectionRule* v2SectionRule(std::uint16_t kind) {
  for (const auto& rule : kV2SectionRules) {
    if (rule.kind == kind) {
      return &rule;
    }
  }
  return nullptr;
}

constexpr bool v2RecordSchemaComplete() {
  constexpr std::array<std::uint32_t, 20> expected = {{
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
      D9C_COMMAND_RECORD_SET_VS_CONST_F,
      D9C_COMMAND_RECORD_SET_VS_CONST_I,
      D9C_COMMAND_RECORD_SET_VS_CONST_B,
      D9C_COMMAND_RECORD_SET_PS_CONST_F,
      D9C_COMMAND_RECORD_SET_PS_CONST_I,
      D9C_COMMAND_RECORD_SET_PS_CONST_B,
      D9C_COMMAND_RECORD_CLEAR,
      D9C_COMMAND_RECORD_PRESENT,
      D9C_COMMAND_RECORD_STRETCH_RECT,
      D9C_COMMAND_RECORD_COLOR_FILL,
      D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      D9C_COMMAND_RECORD_UPDATE_SURFACE,
      D9C_COMMAND_RECORD_QUERY_ISSUE,
      D9C_COMMAND_RECORD_READBACK,
      D9C_COMMAND_RECORD_APPLY_STATE,
      D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
  }};

  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (kV2RecordRules[i].type != expected[i]) {
      return false;
    }
    for (std::size_t j = i + 1; j < kV2RecordRules.size(); ++j) {
      if (kV2RecordRules[i].type == kV2RecordRules[j].type) {
        return false;
      }
    }
  }
  return true;
}

constexpr bool v2SectionSchemaComplete() {
  for (std::size_t i = 0; i < kV2SectionRules.size(); ++i) {
    if (kV2SectionRules[i].kind != i + 1u ||
        kV2SectionRules[i].elementSize == 0u ||
        kV2SectionRules[i].payloadAlignment == 0u ||
        kV2SectionRules[i].maxCount == 0u) {
      return false;
    }
  }
  return true;
}

static_assert(v2RecordSchemaComplete());
static_assert(v2SectionSchemaComplete());
static_assert(sizeof(D9CCommandChunkWireHeaderV2) ==
              D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE);
static_assert(alignof(D9CCommandChunkWireHeaderV2) == 4u);
static_assert(sizeof(D9CCommandChunkWireRecordHeaderV2) ==
              D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_V2_SIZE);
static_assert(alignof(D9CCommandChunkWireRecordHeaderV2) == 4u);
static_assert(sizeof(D9CCommandChunkWireHandleEntryV2) ==
              D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_V2_SIZE);
static_assert(alignof(D9CCommandChunkWireHandleEntryV2) == 8u);
static_assert(sizeof(D9CCommandChunkWireSectionDescV2) ==
              D9C_COMMAND_CHUNK_WIRE_SECTION_DESC_V2_SIZE);
static_assert(alignof(D9CCommandChunkWireSectionDescV2) == 4u);
static_assert(sizeof(D9CCommandChunkWireDrawHeaderV2) ==
              D9C_COMMAND_CHUNK_WIRE_DRAW_HEADER_V2_SIZE);
static_assert(alignof(D9CCommandChunkWireDrawHeaderV2) == 4u);

}  // namespace dxmt9::d3d9
