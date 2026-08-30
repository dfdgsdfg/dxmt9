#pragma once

#include "dxmt9/device_c.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dxmt9::d3d9 {

static_assert(std::endian::native == std::endian::little,
              "command chunk wire canonical requires a little-endian target");
static_assert(std::is_standard_layout_v<D9CCommandChunkSegmentedTransportV1>);
static_assert(std::is_trivially_copyable_v<
              D9CCommandChunkSegmentedTransportV1>);
static_assert(sizeof(D9CCommandChunkSegmentedTransportV1) ==
              D9C_COMMAND_CHUNK_SEGMENTED_TRANSPORT_V1_SIZE);
static_assert(alignof(D9CCommandChunkSegmentedTransportV1) == 8u);
static_assert(offsetof(D9CCommandChunkSegmentedTransportV1, header) == 0u);
static_assert(offsetof(D9CCommandChunkSegmentedTransportV1, records) == 48u);
static_assert(offsetof(D9CCommandChunkSegmentedTransportV1, recordBytes) == 56u);
static_assert(offsetof(D9CCommandChunkSegmentedTransportV1, handles) == 64u);
static_assert(offsetof(D9CCommandChunkSegmentedTransportV1, handleBytes) == 72u);
static_assert(offsetof(D9CCommandChunkSegmentedTransportV1, payload) == 80u);
static_assert(offsetof(D9CCommandChunkSegmentedTransportV1, payloadBytes) == 88u);
static_assert(offsetof(D9CCommandChunkSegmentedTransportV1,
                       renderTapeCaptureToken) == 96u);
static_assert(offsetof(D9CCommandChunkSegmentedTransportV1,
                       renderTapeEventOrdinal) == 104u);

enum RecordRuleFlags : std::uint32_t {
  RecordRuleNone = 0u,
  RecordRuleVariableTail = 1u << 0,
  RecordRuleDraw = 1u << 1,
  RecordRuleOrderingBoundary = 1u << 2,
  RecordRuleSynchronousBoundary = 1u << 3,
  RecordRuleSparseState = 1u << 4,
  RecordRuleHandleRefs = 1u << 5,
};

struct RecordRule {
  std::uint32_t type;
  std::uint32_t fixedPayloadSize;
  std::uint32_t payloadAlignment;
  std::uint32_t allowedRecordFlags;
  std::uint32_t ruleFlags;
};

inline constexpr std::array<RecordRule, 21> kRecordRules = {{
    {D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
     sizeof(D9CCommandChunkWireDrawHeader), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleVariableTail | RecordRuleDraw |
         RecordRuleSparseState | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
     sizeof(D9CCommandChunkWireDrawHeader), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleVariableTail | RecordRuleDraw |
         RecordRuleSparseState | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
     sizeof(D9CCommandChunkWireDrawHeader), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleVariableTail | RecordRuleDraw |
         RecordRuleSparseState | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
     sizeof(D9CCommandChunkWireDrawHeader), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleVariableTail | RecordRuleDraw |
         RecordRuleSparseState | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_SET_VS_CONST_F,
     sizeof(D9CCommandChunkWireSetConst), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_VS_CONST_I,
     sizeof(D9CCommandChunkWireSetConst), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_VS_CONST_B,
     sizeof(D9CCommandChunkWireSetConst), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_PS_CONST_F,
     sizeof(D9CCommandChunkWireSetConst), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_PS_CONST_I,
     sizeof(D9CCommandChunkWireSetConst), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_SET_PS_CONST_B,
     sizeof(D9CCommandChunkWireSetConst), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, RecordRuleVariableTail},
    {D9C_COMMAND_RECORD_CLEAR, sizeof(D9CCommandChunkWireClear), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleVariableTail | RecordRuleOrderingBoundary},
    {D9C_COMMAND_RECORD_PRESENT, sizeof(D9CCommandChunkWirePresent), 8u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleOrderingBoundary | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_STRETCH_RECT,
     sizeof(D9CCommandChunkWireStretchRect), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleOrderingBoundary | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_COLOR_FILL,
     sizeof(D9CCommandChunkWireColorFill), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleOrderingBoundary | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_UPDATE_TEXTURE,
     sizeof(D9CCommandChunkWireUpdateTexture), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleOrderingBoundary | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_UPDATE_SURFACE,
     sizeof(D9CCommandChunkWireUpdateSurface), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleOrderingBoundary | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_QUERY_ISSUE,
     sizeof(D9CCommandChunkWireQueryIssue), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleOrderingBoundary | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_READBACK, sizeof(D9CCommandChunkWireReadback), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleOrderingBoundary | RecordRuleSynchronousBoundary |
         RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_APPLY_STATE,
     sizeof(D9CCommandChunkWireDrawHeader), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleVariableTail | RecordRuleSparseState |
         RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
     sizeof(D9CCommandChunkWireReszDepthResolve), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleOrderingBoundary | RecordRuleHandleRefs},
    {D9C_COMMAND_RECORD_GENERATE_MIPMAPS,
     sizeof(D9CCommandChunkWireGenerateMipmaps), 4u,
     D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
     RecordRuleOrderingBoundary | RecordRuleHandleRefs},
}};

enum SectionRuleFlags : std::uint32_t {
  SectionRuleNone = 0u,
  SectionRuleSingle = 1u << 0,
  SectionRuleHandleRefs = 1u << 1,
  SectionRuleConstantRange = 1u << 2,
  SectionRuleRawBytes = 1u << 3,
  SectionRuleFullSnapshotSlots = 1u << 4,
};

inline constexpr std::uint32_t kNoHandleKind =
    std::numeric_limits<std::uint32_t>::max();

struct SectionRule {
  std::uint16_t kind;
  std::uint16_t elementSize;
  std::uint32_t payloadAlignment;
  std::uint32_t maxCount;
  std::uint32_t ruleFlags;
  std::uint32_t handleKind;
};

struct RecordHandleFieldRule {
  std::uint32_t recordType;
  std::uint32_t payloadOffset;
  std::uint32_t handleKind;
  bool nullable;
};

inline constexpr std::array<RecordHandleFieldRule, 14>
    kRecordHandleFieldRules = {{
        {D9C_COMMAND_RECORD_PRESENT,
         offsetof(D9CCommandChunkWirePresent, sourceHandleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, false},
        {D9C_COMMAND_RECORD_STRETCH_RECT,
         offsetof(D9CCommandChunkWireStretchRect, srcHandleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, false},
        {D9C_COMMAND_RECORD_STRETCH_RECT,
         offsetof(D9CCommandChunkWireStretchRect, dstHandleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, false},
        {D9C_COMMAND_RECORD_COLOR_FILL,
         offsetof(D9CCommandChunkWireColorFill, surfaceHandleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, false},
        {D9C_COMMAND_RECORD_UPDATE_TEXTURE,
         offsetof(D9CCommandChunkWireUpdateTexture, srcHandleIndex),
         D9C_CHUNK_HANDLE_KIND_TEXTURE, false},
        {D9C_COMMAND_RECORD_UPDATE_TEXTURE,
         offsetof(D9CCommandChunkWireUpdateTexture, dstHandleIndex),
         D9C_CHUNK_HANDLE_KIND_TEXTURE, false},
        {D9C_COMMAND_RECORD_UPDATE_SURFACE,
         offsetof(D9CCommandChunkWireUpdateSurface, srcHandleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, false},
        {D9C_COMMAND_RECORD_UPDATE_SURFACE,
         offsetof(D9CCommandChunkWireUpdateSurface, dstHandleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, false},
        {D9C_COMMAND_RECORD_QUERY_ISSUE,
         offsetof(D9CCommandChunkWireQueryIssue, queryHandleIndex),
         D9C_CHUNK_HANDLE_KIND_QUERY, false},
        {D9C_COMMAND_RECORD_READBACK,
         offsetof(D9CCommandChunkWireReadback, srcHandleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, false},
        {D9C_COMMAND_RECORD_READBACK,
         offsetof(D9CCommandChunkWireReadback, dstHandleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, false},
        {D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
         offsetof(D9CCommandChunkWireReszDepthResolve,
                  msaaDepthHandleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, false},
        {D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
         offsetof(D9CCommandChunkWireReszDepthResolve,
                  intzDestHandleIndex),
         D9C_CHUNK_HANDLE_KIND_TEXTURE, false},
        {D9C_COMMAND_RECORD_GENERATE_MIPMAPS,
         offsetof(D9CCommandChunkWireGenerateMipmaps, textureHandleIndex),
         D9C_CHUNK_HANDLE_KIND_TEXTURE, false},
    }};

struct SectionHandleFieldRule {
  std::uint16_t sectionKind;
  std::uint16_t payloadOffset;
  std::uint32_t handleKind;
  bool nullable;
};

inline constexpr std::array<SectionHandleFieldRule, 7>
    kSectionHandleFieldRules = {{
        {D9C_COMMAND_CHUNK_SECTION_TEXTURE,
         offsetof(D9CCommandChunkWireTextureBinding, handleIndex),
         D9C_CHUNK_HANDLE_KIND_TEXTURE, true},
        {D9C_COMMAND_CHUNK_SECTION_STREAM,
         offsetof(D9CCommandChunkWireStreamBinding, handleIndex),
         D9C_CHUNK_HANDLE_KIND_BUFFER, true},
        {D9C_COMMAND_CHUNK_SECTION_SHADER,
         offsetof(D9CCommandChunkWireShaderBinding, handleIndex),
         D9C_CHUNK_HANDLE_KIND_SHADER, true},
        {D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
         offsetof(D9CCommandChunkWireVertexInput, handleIndex),
         D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, true},
        {D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
         offsetof(D9CCommandChunkWireIndexBinding, handleIndex),
         D9C_CHUNK_HANDLE_KIND_BUFFER, true},
        {D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
         offsetof(D9CCommandChunkWireRenderTargetBinding, handleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, true},
        {D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
         offsetof(D9CCommandChunkWireDepthStencilBinding, handleIndex),
         D9C_CHUNK_HANDLE_KIND_SURFACE, true},
    }};

inline constexpr std::array<SectionRule,
                            D9C_COMMAND_CHUNK_SECTION_COUNT>
    kSectionRules = {{
        {D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
         sizeof(D9CCommandChunkWireRenderState), 4u,
         D9C_DRAW_PACKET_MAX_RENDER_STATES, SectionRuleNone,
         kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_TEXTURE,
         sizeof(D9CCommandChunkWireTextureBinding), 4u,
         D9C_DRAW_PACKET_MAX_TEXTURES,
         SectionRuleHandleRefs | SectionRuleFullSnapshotSlots,
         D9C_CHUNK_HANDLE_KIND_TEXTURE},
        {D9C_COMMAND_CHUNK_SECTION_STREAM,
         sizeof(D9CCommandChunkWireStreamBinding), 4u,
         D9C_DRAW_PACKET_MAX_STREAMS,
         SectionRuleHandleRefs | SectionRuleFullSnapshotSlots,
         D9C_CHUNK_HANDLE_KIND_BUFFER},
        {D9C_COMMAND_CHUNK_SECTION_SHADER,
         sizeof(D9CCommandChunkWireShaderBinding), 4u, 2u,
         SectionRuleHandleRefs, D9C_CHUNK_HANDLE_KIND_SHADER},
        {D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
         sizeof(D9CCommandChunkWireVertexInput), 4u, 1u,
         SectionRuleSingle | SectionRuleHandleRefs,
         D9C_CHUNK_HANDLE_KIND_VERTEX_DECL},
        {D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
         sizeof(D9CCommandChunkWireIndexBinding), 4u, 1u,
         SectionRuleSingle | SectionRuleHandleRefs,
         D9C_CHUNK_HANDLE_KIND_BUFFER},
        {D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
         sizeof(D9CCommandChunkWireRenderTargetBinding), 4u,
         D9C_DRAW_PACKET_MAX_RENDER_TARGETS, SectionRuleHandleRefs,
         D9C_CHUNK_HANDLE_KIND_SURFACE},
        {D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
         sizeof(D9CCommandChunkWireDepthStencilBinding), 4u, 1u,
         SectionRuleSingle | SectionRuleHandleRefs,
         D9C_CHUNK_HANDLE_KIND_SURFACE},
        {D9C_COMMAND_CHUNK_SECTION_VIEWPORT, sizeof(D9CViewport), 4u, 1u,
         SectionRuleSingle, kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_SCISSOR, sizeof(D9CRect), 4u, 1u,
         SectionRuleSingle, kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_MATERIAL, sizeof(D9CMaterial), 4u, 1u,
         SectionRuleSingle, kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE,
         sizeof(D9CCommandChunkWireClipPlane), 4u, 6u, SectionRuleNone,
         kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
         sizeof(D9CDrawPacketTextureStageState), 4u, D9C_DRAW_PACKET_MAX_TSS,
         SectionRuleNone, kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
         sizeof(D9CDrawPacketSamplerState), 4u, D9C_DRAW_PACKET_MAX_SAMPLER,
         SectionRuleNone, kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_TRANSFORM,
         sizeof(D9CDrawPacketTransform), 4u, D9C_DRAW_PACKET_MAX_TRANSFORMS,
         SectionRuleNone, kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_LIGHT,
         sizeof(D9CCommandChunkWireLight), 4u, D9C_DRAW_PACKET_MAX_LIGHTS,
         SectionRuleNone, kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
         sizeof(D9CCommandChunkWireLightEnable), 4u,
         D9C_DRAW_PACKET_MAX_LIGHTS, SectionRuleNone, kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_VS_CONST_F, 16u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_VS_F, SectionRuleConstantRange,
         kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_VS_CONST_I, 16u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_VS_I, SectionRuleConstantRange,
         kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_VS_CONST_B, 4u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_VS_B, SectionRuleConstantRange,
         kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_PS_CONST_F, 16u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_PS_F, SectionRuleConstantRange,
         kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_PS_CONST_I, 16u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_PS_I, SectionRuleConstantRange,
         kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_PS_CONST_B, 4u, 4u,
         D9C_DRAW_PACKET_MAX_CONST_PS_B, SectionRuleConstantRange,
         kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA, 1u, 4u,
         std::numeric_limits<std::uint32_t>::max(), SectionRuleRawBytes,
         kNoHandleKind},
        {D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA, 1u, 4u,
         std::numeric_limits<std::uint32_t>::max(), SectionRuleRawBytes,
         kNoHandleKind},
    }};

constexpr const RecordRule* recordRule(std::uint32_t type) {
  for (const auto& rule : kRecordRules) {
    if (rule.type == type) {
      return &rule;
    }
  }
  return nullptr;
}

constexpr const SectionRule* sectionRule(std::uint16_t kind) {
  for (const auto& rule : kSectionRules) {
    if (rule.kind == kind) {
      return &rule;
    }
  }
  return nullptr;
}

constexpr const SectionHandleFieldRule* sectionHandleFieldRule(
    std::uint16_t kind) {
  for (const auto& rule : kSectionHandleFieldRules) {
    if (rule.sectionKind == kind) {
      return &rule;
    }
  }
  return nullptr;
}

constexpr bool recordSchemaComplete() {
  constexpr std::array<std::uint32_t, 21> expected = {{
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
      D9C_COMMAND_RECORD_GENERATE_MIPMAPS,
  }};

  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (kRecordRules[i].type != expected[i]) {
      return false;
    }
    for (std::size_t j = i + 1; j < kRecordRules.size(); ++j) {
      if (kRecordRules[i].type == kRecordRules[j].type) {
        return false;
      }
    }
  }
  return true;
}

constexpr bool sectionSchemaComplete() {
  for (std::size_t i = 0; i < kSectionRules.size(); ++i) {
    if (kSectionRules[i].kind != i + 1u ||
        kSectionRules[i].elementSize == 0u ||
        kSectionRules[i].payloadAlignment == 0u ||
        kSectionRules[i].maxCount == 0u) {
      return false;
    }
  }
  return true;
}

constexpr bool handleFieldSchemaComplete() {
  for (const auto& recordRule : kRecordRules) {
    if ((recordRule.ruleFlags & RecordRuleHandleRefs) == 0u ||
        (recordRule.ruleFlags & RecordRuleSparseState) != 0u) {
      continue;
    }
    bool found = false;
    for (const auto& field : kRecordHandleFieldRules) {
      if (field.recordType != recordRule.type) {
        continue;
      }
      found = true;
      if (field.payloadOffset + sizeof(std::uint32_t) >
              recordRule.fixedPayloadSize ||
          field.handleKind > D9C_CHUNK_HANDLE_KIND_QUERY) {
        return false;
      }
    }
    if (!found) {
      return false;
    }
  }
  for (const auto& field : kSectionHandleFieldRules) {
    const auto* section = sectionRule(field.sectionKind);
    if (!section ||
        (section->ruleFlags & SectionRuleHandleRefs) == 0u ||
        field.payloadOffset + sizeof(std::uint32_t) > section->elementSize ||
        field.handleKind != section->handleKind) {
      return false;
    }
  }
  return true;
}

static_assert(recordSchemaComplete());
static_assert(sectionSchemaComplete());
static_assert(handleFieldSchemaComplete());
static_assert(sizeof(D9CCommandChunkWireHeader) ==
              D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE);
static_assert(alignof(D9CCommandChunkWireHeader) == 4u);
static_assert(sizeof(D9CCommandChunkWireRecordHeader) ==
              D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE);
static_assert(alignof(D9CCommandChunkWireRecordHeader) == 4u);
static_assert(sizeof(D9CCommandChunkWireHandleEntry) ==
              D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE);
static_assert(alignof(D9CCommandChunkWireHandleEntry) == 8u);
static_assert(sizeof(D9CCommandChunkWireSectionDesc) ==
              D9C_COMMAND_CHUNK_WIRE_SECTION_DESC_SIZE);
static_assert(alignof(D9CCommandChunkWireSectionDesc) == 4u);
static_assert(sizeof(D9CCommandChunkWireDrawHeader) ==
              D9C_COMMAND_CHUNK_WIRE_DRAW_HEADER_SIZE);
static_assert(alignof(D9CCommandChunkWireDrawHeader) == 4u);

}  // namespace dxmt9::d3d9
