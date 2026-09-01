#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../src/dxmt9/dxmt9_backend_types.hpp"
#include "../../../src/dxmt9/dxmt9_compat.hpp"
#include "../../../src/dxmt9/dxmt9_queue.hpp"
#include "dxmt9/core.hpp"

namespace {

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;

namespace metalcompat = dxmt9::core::metalcompat;
namespace metalqueue = dxmt9::core::metalqueue;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A &left, const B &right, std::string_view message) {
  if (!(left == right)) {
    fail(std::string(message));
  }
}

CanonicalDrawState makeCanonicalDrawStateForTest(const DrawDesc &desc) {
  auto hot = makeFlatDrawStateRecord(desc);
  auto shaderLayout = makeDrawShaderLayoutContext(desc);
  auto debug = makeDrawDebugSnapshot(desc, hot);
  return CanonicalDrawState{std::move(hot), std::move(shaderLayout),
                            std::move(debug)};
}

DrawParam makeDrawParam(std::uint32_t primitiveCount,
                        std::uint32_t startVertex) {
  DrawParam param{};
  param.primitiveCount = primitiveCount;
  param.startVertex = startVertex;
  return param;
}

Matrix4x4 projectedTextureTransform() {
  Matrix4x4 matrix{};
  matrix.m[0] = 2.0f;
  matrix.m[5] = 1.0f;
  matrix.m[10] = 1.0f;
  matrix.m[15] = 1.0f;
  return matrix;
}

enum class QueueReplayCategory {
  Draw,
  Blit,
  Present,
};

struct QueueReplayObservation {
  MetalCommandKind kind = MetalCommandKind::DrawRun;
  QueueReplayCategory category = QueueReplayCategory::Draw;
  Handle primaryHandle{};
  std::size_t drawCount = 0;
  std::size_t payloadSize = 0;
  bool orderingBoundary = false;
  bool synchronousReadBoundary = false;
};

std::vector<QueueReplayObservation> observeQueueReplay(const ChunkSlot &slot) {
  std::vector<QueueReplayObservation> observations;
  observations.reserve(slot.commandCount());

  for (std::size_t i = 0; i < slot.commandCount(); ++i) {
    const auto command = slot.commandAt(i);
    QueueReplayObservation observation{.kind = command.kind};
    switch (command.kind) {
    case MetalCommandKind::DrawRun:
      observation.category = QueueReplayCategory::Draw;
      observation.primaryHandle =
          command.drawState.hot
              ? command.drawState.hot->colorAttachments[0].handle
              : Handle{};
      observation.drawCount = drawRunDrawCount(command);
      observation.payloadSize = drawRunPayloadSize(command);
      break;
    case MetalCommandKind::Clear:
      observation.category = QueueReplayCategory::Draw;
      observation.primaryHandle =
          command.clear ? command.clear->colorAttachments[0].handle : Handle{};
      observation.orderingBoundary = true;
      break;
    case MetalCommandKind::SurfaceCopy:
      observation.category = QueueReplayCategory::Blit;
      observation.primaryHandle =
          command.surfaceCopy ? command.surfaceCopy->destination : Handle{};
      observation.orderingBoundary = true;
      break;
    case MetalCommandKind::StretchRect:
      observation.category = QueueReplayCategory::Blit;
      observation.primaryHandle =
          command.stretchRect ? command.stretchRect->destination : Handle{};
      observation.orderingBoundary = true;
      break;
    case MetalCommandKind::Readback:
      observation.category = QueueReplayCategory::Blit;
      observation.primaryHandle =
          command.readback ? command.readback->destination : Handle{};
      observation.orderingBoundary = true;
      observation.synchronousReadBoundary = true;
      break;
    case MetalCommandKind::ColorFill:
      observation.category = QueueReplayCategory::Draw;
      observation.primaryHandle =
          command.colorFill ? command.colorFill->destination : Handle{};
      observation.orderingBoundary = true;
      break;
    case MetalCommandKind::DepthResolve:
      observation.category = QueueReplayCategory::Blit;
      observation.primaryHandle =
          command.depthResolve ? command.depthResolve->intzDest : Handle{};
      observation.orderingBoundary = true;
      break;
    case MetalCommandKind::GenerateMipmaps:
      observation.category = QueueReplayCategory::Blit;
      observation.primaryHandle =
          command.generateMipmaps ? command.generateMipmaps->texture : Handle{};
      observation.orderingBoundary = true;
      break;
    case MetalCommandKind::Present:
      observation.category = QueueReplayCategory::Present;
      observation.primaryHandle =
          command.present ? command.present->presentSource : Handle{};
      observation.orderingBoundary = true;
      break;
    }
    observations.push_back(observation);
  }

  return observations;
}

struct ChunkSlotCapacitySnapshot {
  std::size_t commandHeaders = 0;
  std::size_t drawHotStates = 0;
  std::size_t drawShaderLayouts = 0;
  std::size_t drawDebugSnapshots = 0;
  std::size_t drawUniformFixedPayloads = 0;
  std::size_t drawUniformVertexConstants = 0;
  std::size_t drawUniformVertexConstantBytes = 0;
  std::size_t drawUniformPixelConstants = 0;
  std::size_t drawUniformPixelConstantBytes = 0;
  std::size_t drawUniformPayloads = 0;
  std::size_t drawUniformPayloadLookupHeads = 0;
  std::size_t drawUniformPayloadLookupTails = 0;
  std::size_t drawUniformPayloadLookupNext = 0;
  std::size_t drawUniformVertexConstantsLookupHeads = 0;
  std::size_t drawUniformVertexConstantsLookupTails = 0;
  std::size_t drawUniformVertexConstantsLookupNext = 0;
  std::size_t drawUniformPixelConstantsLookupHeads = 0;
  std::size_t drawUniformPixelConstantsLookupTails = 0;
  std::size_t drawUniformPixelConstantsLookupNext = 0;
  std::size_t drawParams = 0;
  std::size_t drawPayloadArena = 0;
  std::size_t drawRunRecords = 0;

  friend bool operator==(const ChunkSlotCapacitySnapshot &,
                         const ChunkSlotCapacitySnapshot &) = default;
};

ChunkSlotCapacitySnapshot capacitySnapshot(const ChunkSlot &slot) {
  return ChunkSlotCapacitySnapshot{
      .commandHeaders = slot.commandHeaders.capacity(),
      .drawHotStates = slot.drawHotStates.capacity(),
      .drawShaderLayouts = slot.drawShaderLayouts.capacity(),
      .drawDebugSnapshots = slot.drawDebugSnapshots.capacity(),
      .drawUniformFixedPayloads = slot.drawUniformFixedPayloads.capacity(),
      .drawUniformVertexConstants = slot.drawUniformVertexConstants.capacity(),
      .drawUniformVertexConstantBytes =
          slot.drawUniformVertexConstantBytes.capacity(),
      .drawUniformPixelConstants = slot.drawUniformPixelConstants.capacity(),
      .drawUniformPixelConstantBytes =
          slot.drawUniformPixelConstantBytes.capacity(),
      .drawUniformPayloads = slot.drawUniformPayloads.capacity(),
      .drawUniformPayloadLookupHeads =
          slot.drawUniformPayloadLookupHeads.capacity(),
      .drawUniformPayloadLookupTails =
          slot.drawUniformPayloadLookupTails.capacity(),
      .drawUniformPayloadLookupNext =
          slot.drawUniformPayloadLookupNext.capacity(),
      .drawUniformVertexConstantsLookupHeads =
          slot.drawUniformVertexConstantsLookupHeads.capacity(),
      .drawUniformVertexConstantsLookupTails =
          slot.drawUniformVertexConstantsLookupTails.capacity(),
      .drawUniformVertexConstantsLookupNext =
          slot.drawUniformVertexConstantsLookupNext.capacity(),
      .drawUniformPixelConstantsLookupHeads =
          slot.drawUniformPixelConstantsLookupHeads.capacity(),
      .drawUniformPixelConstantsLookupTails =
          slot.drawUniformPixelConstantsLookupTails.capacity(),
      .drawUniformPixelConstantsLookupNext =
          slot.drawUniformPixelConstantsLookupNext.capacity(),
      .drawParams = slot.drawParams.capacity(),
      .drawPayloadArena = slot.drawPayloadArena.capacity(),
      .drawRunRecords = slot.drawRunRecords.capacity(),
  };
}

void testChunkSlotReplayObserverAndQueueSummarySeeSameCategories() {
  DrawDesc desc{};
  desc.primitiveCount = 1u;
  desc.rts.color[0] = RenderTargetAttachment{
      .handle = Handle{0x3000u}, .level = 0u, .sampleCount = 4u};
  desc.rts.color[1] = RenderTargetAttachment{
      .handle = Handle{0x3001u}, .level = 0u, .sampleCount = 1u};
  desc.textures[0].handle = Handle{0x1000u};
  desc.textureTransforms[0] = projectedTextureTransform();
  desc.rs.values[RS_SRGB_WRITE_ENABLE] = 1u;

  const auto uniforms = makeDrawUniformPayload(desc);
  const std::array<DrawParam, 2> draws{
      makeDrawParam(1u, 0u),
      makeDrawParam(2u, 3u),
  };
  const std::array<u8, 4> firstVertexPayload{0x10u, 0x20u, 0x30u, 0x40u};
  const std::array<u8, 2> firstIndexPayload{0x50u, 0x60u};
  const std::array<u8, 3> secondVertexPayload{0x70u, 0x80u, 0x90u};
  const std::array<DrawParamPayloadView, 2> payloads{
      DrawParamPayloadView{
          .userVertexData = std::span<const u8>(firstVertexPayload.data(),
                                                firstVertexPayload.size()),
          .userIndexData = std::span<const u8>(firstIndexPayload.data(),
                                               firstIndexPayload.size()),
      },
      DrawParamPayloadView{
          .userVertexData = std::span<const u8>(secondVertexPayload.data(),
                                                secondVertexPayload.size()),
      },
  };

  ChunkSlot slot{};
  slot.seqId = 42u;
  slot.appendDrawRun(
      makeCanonicalDrawStateForTest(desc), uniforms,
      std::span<const DrawParam>(draws.data(), draws.size()),
      std::span<const DrawParamPayloadView>(payloads.data(), payloads.size()));
  // Queue diagnostics must read projected-texture compatibility from the
  // compact hot state, not by re-materializing draw-run uniform payloads.
  slot.drawUniformPayloads.clear();
  slot.drawUniformFixedPayloads.clear();
  slot.drawUniformVertexConstants.clear();
  slot.drawUniformVertexConstantBytes.clear();
  slot.drawUniformPixelConstants.clear();
  slot.drawUniformPixelConstantBytes.clear();

  ClearDesc clear{};
  clear.colorAttachments[0] = RenderTargetAttachment{
      .handle = Handle{0x3000u}, .level = 0u, .sampleCount = 4u};
  clear.clearColor = true;
  clear.color = ColorRGBA{0.1f, 0.2f, 0.3f, 1.0f};
  slot.appendClear(clear);

  SurfaceCopyDesc copy{};
  copy.source = Handle{0x4000u};
  copy.destination = Handle{0x4008u};
  copy.sourceRect = Rect{1, 2, 3, 4};
  copy.destinationRect = Rect{5, 6, 7, 8};
  slot.appendSurfaceCopy(copy);

  ReadbackDesc readback{};
  readback.source = Handle{0x5000u};
  readback.destination = Handle{0x5008u};
  readback.sourceRect = Rect{9, 10, 11, 12};
  slot.appendReadback(readback);

  SwapDesc present{};
  present.window = Handle{0x6000u};
  present.sourceSurface = Handle{0x7000u};
  present.multiSampleType = MultiSampleType::Four;
  slot.appendPresent(present, present.sourceSurface);

  const auto observed = observeQueueReplay(slot);
  checkEq(observed.size(), std::size_t{5},
          "queue observer sees every chunk-slot command header");
  checkEq(observed[0].kind, MetalCommandKind::DrawRun,
          "queue observer records draw-run first");
  checkEq(observed[0].category, QueueReplayCategory::Draw,
          "queue observer classifies draw-run as draw");
  checkEq(observed[0].primaryHandle, Handle{0x3000u},
          "queue observer reports draw primary render target");
  checkEq(observed[0].drawCount, std::size_t{2},
          "queue observer reports draw-run param count");
  checkEq(observed[0].payloadSize, std::size_t{9},
          "queue observer reports draw-run payload span");
  check(!observed[0].orderingBoundary, "draw-run is not an ordering boundary");

  checkEq(observed[1].kind, MetalCommandKind::Clear,
          "queue observer preserves clear order");
  checkEq(observed[1].category, QueueReplayCategory::Draw,
          "queue observer classifies clear with draw work");
  check(observed[1].orderingBoundary,
        "queue observer marks clear as a replay boundary");
  checkEq(observed[2].category, QueueReplayCategory::Blit,
          "queue observer classifies surface copy as blit work");
  checkEq(observed[3].kind, MetalCommandKind::Readback,
          "queue observer preserves readback order");
  check(observed[3].orderingBoundary,
        "queue observer marks readback as a replay boundary");
  check(observed[3].synchronousReadBoundary,
        "queue observer marks readback as synchronous");
  checkEq(observed[4].category, QueueReplayCategory::Present,
          "queue observer classifies present separately");
  checkEq(observed[4].primaryHandle, Handle{0x7000u},
          "queue observer reports present source handle");

  const auto diagnostics =
      metalqueue::summarizeCommands(42u, 3u, slot, [](Handle handle) -> u32 {
        return handle == Handle{0x3000u} ? metalcompat::CompatFlagFp16 : 0u;
      });
  checkEq(diagnostics.seqId, 42u, "queue diagnostics preserve seq id");
  checkEq(diagnostics.slotIndex, std::size_t{3},
          "queue diagnostics preserve slot index");
  check(diagnostics.hasDraw, "queue diagnostics see draw-class work");
  check(diagnostics.hasBlit, "queue diagnostics see blit-class work");
  check(diagnostics.hasPresent, "queue diagnostics see present-class work");
  check(!diagnostics.hasStretchRect,
        "queue diagnostics do not infer stretch work from generic blit work");

  const std::array<metalqueue::ChunkObservation, 1> stretchObservations{{
      metalqueue::ChunkObservation{
          .kind = metalqueue::ChunkObservationKind::StretchRect,
          .compatFlags = 0u,
      },
  }};
  const auto stretchDiagnostics =
      metalqueue::summarizeChunk(43u, 4u, std::span<const metalqueue::ChunkObservation>(
                                             stretchObservations.data(),
                                             stretchObservations.size()));
  check(stretchDiagnostics.hasBlit,
        "queue diagnostics keep stretch-rect in the blit-class bucket");
  check(stretchDiagnostics.hasStretchRect,
        "queue diagnostics expose stretch-rect work for completion wait splits");

  const u32 expectedFlags =
      metalcompat::CompatFlagFp16 | metalcompat::CompatFlagMrt |
      metalcompat::CompatFlagSrgb | metalcompat::CompatFlagProjected |
      metalcompat::CompatFlagMsaa;
  checkEq(diagnostics.compatFlags & expectedFlags, expectedFlags,
          "queue diagnostics aggregate draw/present compatibility flags");
}

void testChunkSlotDrawRunSoAReuseKeepsReservedCapacitiesStable() {
  DrawDesc desc{};
  desc.primitiveCount = 1u;
  desc.rts.color[0] = RenderTargetAttachment{.handle = Handle{0x3000u}};
  const auto uniforms = makeDrawUniformPayload(desc);
  const DrawParam draw = makeDrawParam(1u, 0u);
  const std::array<u8, 4> vertexPayload{0xa0u, 0xb0u, 0xc0u, 0xd0u};
  const DrawParamPayloadView payload{
      .userVertexData =
          std::span<const u8>(vertexPayload.data(), vertexPayload.size()),
  };

  ChunkSlot slot{};
  slot.commandHeaders.reserve(4u);
  slot.drawRunRecords.reserve(4u);
  slot.reserveDrawStateStorage(4u);
  slot.drawParams.reserve(4u);
  slot.drawPayloadArena.reserve(16u);
  slot.drawUniformFixedPayloads.reserve(1u);
  slot.drawUniformVertexConstants.reserve(1u);
  slot.drawUniformVertexConstantBytes.reserve(sizeof(VertexShaderConstants));
  slot.drawUniformPixelConstants.reserve(1u);
  slot.drawUniformPixelConstantBytes.reserve(sizeof(PixelShaderConstants));
  slot.drawUniformPayloads.reserve(1u);

  slot.appendDrawRun(makeCanonicalDrawStateForTest(desc), uniforms,
                     std::span<const DrawParam>(&draw, 1u),
                     std::span<const DrawParamPayloadView>(&payload, 1u));
  const auto firstUniformHandle = slot.drawRunRecords.front().uniformHandle;
  const auto stableCapacity = capacitySnapshot(slot);

  for (std::uint32_t i = 1; i < 4u; ++i) {
    DrawParam repeated = makeDrawParam(1u, i * 3u);
    slot.appendDrawRun(makeCanonicalDrawStateForTest(desc), uniforms,
                       std::span<const DrawParam>(&repeated, 1u),
                       std::span<const DrawParamPayloadView>(&payload, 1u),
                       firstUniformHandle);
  }

  checkEq(capacitySnapshot(slot), stableCapacity,
          "reserved SoA storage does not grow for reused-uniform draw runs");
  checkEq(slot.commandCount(), std::size_t{4},
          "draw-run hot path appends one command header per run");
  checkEq(slot.drawRunRecords.size(), std::size_t{4},
          "draw-run hot path stores one compact record per run");
  checkEq(slot.drawHotStates.size(), std::size_t{4},
          "draw-run hot path stores one hot state per run");
  checkEq(slot.drawParams.size(), std::size_t{4},
          "draw-run hot path stores one draw param per run");
  checkEq(slot.drawPayloadArena.size(), std::size_t{16},
          "draw-run hot path stores payload bytes in one arena");
  checkEq(slot.drawUniformPayloads.size(), std::size_t{1},
          "draw-run hot path reuses the interned uniform payload");
  checkEq(slot.drawUniformFixedPayloads.size(), std::size_t{1},
          "draw-run hot path reuses the interned fixed uniform payload");
  for (const auto &record : slot.drawRunRecords) {
    checkEq(record.uniformHandle, firstUniformHandle,
            "reused draw runs point at the same uniform handle");
  }

  slot.clearCommands();
  check(slot.commandsEmpty(), "clearCommands removes command headers");
  checkEq(capacitySnapshot(slot), stableCapacity,
          "clearCommands preserves SoA capacities for queue-local reuse");
}


void testChunkSlotUniformPayloadLookupHashCollisionKeepsDistinctPayloads() {
  DrawDesc desc{};
  desc.primitiveCount = 1u;
  desc.rts.color[0] = RenderTargetAttachment{.handle = Handle{0x3000u}};

  DrawDesc firstDesc = desc;
  DrawDesc secondDesc = desc;
  firstDesc.vsConst.float4[0][0] = 11.0f;
  secondDesc.vsConst.float4[0][0] = 22.0f;

  auto firstUniformPayload = makeDrawUniformPayload(firstDesc);
  auto secondUniformPayload = makeDrawUniformPayload(secondDesc);
  firstUniformPayload.hash = 0x12345678ull;
  secondUniformPayload.hash = firstUniformPayload.hash;

  const DrawParam draw = makeDrawParam(1u, 0u);
  ChunkSlot slot{};
  slot.appendDrawRun(makeCanonicalDrawStateForTest(desc), firstUniformPayload,
                     std::span<const DrawParam>(&draw, 1u),
                     std::span<const DrawParamPayloadView>{});
  const auto firstHandle = slot.drawRunRecords.back().uniformHandle;

  slot.appendDrawRun(makeCanonicalDrawStateForTest(desc), secondUniformPayload,
                     std::span<const DrawParam>(&draw, 1u),
                     std::span<const DrawParamPayloadView>{});
  const auto secondHandle = slot.drawRunRecords.back().uniformHandle;

  checkEq(slot.drawUniformPayloads.size(), std::size_t{2},
          "same lookup hash does not merge different uniform payloads");
  check(firstHandle.hash == secondHandle.hash,
        "test payloads intentionally share the lookup hash");
  check(!(firstHandle == secondHandle),
        "distinct payloads keep distinct uniform handles under hash collision");

  slot.appendDrawRun(makeCanonicalDrawStateForTest(desc), firstUniformPayload,
                     std::span<const DrawParam>(&draw, 1u),
                     std::span<const DrawParamPayloadView>{});
  const auto reusedHandle = slot.drawRunRecords.back().uniformHandle;
  checkEq(slot.drawUniformPayloads.size(), std::size_t{2},
          "bucket lookup reuses the matching colliding payload");
  checkEq(reusedHandle, firstHandle,
          "hash collision lookup returns the payload whose full contents match");
}

void testChunkSlotUniformStageConstantsUseCompactByteArena() {
  DrawDesc desc{};
  desc.primitiveCount = 1u;
  desc.rts.color[0] = RenderTargetAttachment{.handle = Handle{0x3000u}};
  desc.vsConst.float4[0][0] = 42.0f;
  desc.vsConst.float4[10][0] = 99.0f;

  auto uniforms = makeDrawUniformPayload(desc);
  uniforms.vertexFloatConstantCount = 1u;
  uniforms.vertexIntConstantCount = 0u;
  uniforms.vertexBoolConstantCount = 0u;

  const DrawParam draw = makeDrawParam(1u, 0u);
  ChunkSlot slot{};
  slot.appendDrawRun(makeCanonicalDrawStateForTest(desc), uniforms,
                     std::span<const DrawParam>(&draw, 1u),
                     std::span<const DrawParamPayloadView>{});

  checkEq(slot.drawUniformVertexConstants.size(), std::size_t{1},
          "compact stage storage appends one VS record");
  checkEq(slot.drawUniformVertexConstantBytes.size(),
          sizeof(std::array<f32, 4>),
          "compact stage storage stores only the live VS float prefix");
  check(slot.drawUniformVertexConstantBytes.size() < sizeof(VertexShaderConstants),
        "compact stage storage is narrower than the full VS constant block");

  const auto command = slot.drawRunCommandAt(0u);
  DrawUniformPayload scratch{};
  const auto* materialized =
      drawRunUniformPayloadForParam(command, command.drawParams[0], scratch);
  check(materialized != nullptr,
        "compact stage storage materializes a legacy uniform payload view");
  check(materialized->vsConst.float4[0][0] == 42.0f,
        "compact stage storage preserves the stored live VS constant");
  check(materialized->vsConst.float4[10][0] == 0.0f,
        "compact stage storage zero-fills constants outside the stored prefix");
}

void testChunkSlotUniformStageConstantsPreserveAbiPrefixForBoolUpload() {
  DrawDesc desc{};
  desc.primitiveCount = 1u;
  desc.rts.color[0] = RenderTargetAttachment{.handle = Handle{0x3000u}};
  desc.vsConst.float4[0][0] = 42.0f;
  desc.vsConst.float4[10][0] = 99.0f;
  desc.vsConst.int4[2][0] = 77;
  desc.vsConst.bools[0] = true;
  desc.psConst.float4[0][0] = 24.0f;
  desc.psConst.float4[7][0] = 66.0f;
  desc.psConst.int4[1][0] = 55;
  desc.psConst.bools[0] = true;

  auto uniforms = makeDrawUniformPayload(desc);
  uniforms.vertexFloatConstantCount = 1u;
  uniforms.vertexIntConstantCount = 1u;
  uniforms.vertexBoolConstantCount = 1u;
  uniforms.pixelFloatConstantCount = 1u;
  uniforms.pixelIntConstantCount = 1u;
  uniforms.pixelBoolConstantCount = 1u;

  const DrawParam draw = makeDrawParam(1u, 0u);
  ChunkSlot slot{};
  slot.appendDrawRun(makeCanonicalDrawStateForTest(desc), uniforms,
                     std::span<const DrawParam>(&draw, 1u),
                     std::span<const DrawParamPayloadView>{});

  const auto expectedVsBytes =
      uniforms.vsConst.float4.size() * sizeof(uniforms.vsConst.float4[0]) +
      uniforms.vsConst.int4.size() * sizeof(uniforms.vsConst.int4[0]) +
      sizeof(uniforms.vsConst.bools[0]);
  const auto expectedPsBytes =
      uniforms.psConst.float4.size() * sizeof(uniforms.psConst.float4[0]) +
      uniforms.psConst.int4.size() * sizeof(uniforms.psConst.int4[0]) +
      sizeof(uniforms.psConst.bools[0]);
  checkEq(slot.drawUniformVertexConstantBytes.size(), expectedVsBytes,
          "bool VS constants preserve the ABI prefix before bool storage");
  checkEq(slot.drawUniformPixelConstantBytes.size(), expectedPsBytes,
          "bool PS constants preserve the ABI prefix before bool storage");

  const auto command = slot.drawRunCommandAt(0u);
  DrawUniformPayload scratch{};
  const auto* materialized =
      drawRunUniformPayloadForParam(command, command.drawParams[0], scratch);
  check(materialized != nullptr,
        "ABI-prefix stage storage materializes a uniform payload view");
  check(materialized->vsConst.float4[10][0] == 99.0f,
        "ABI-prefix stage storage preserves VS float prefix values");
  check(materialized->vsConst.int4[2][0] == 77,
        "ABI-prefix stage storage preserves VS int prefix values");
  check(materialized->vsConst.bools[0],
        "ABI-prefix stage storage preserves VS bool values");
  check(materialized->psConst.float4[7][0] == 66.0f,
        "ABI-prefix stage storage preserves PS float prefix values");
  check(materialized->psConst.int4[1][0] == 55,
        "ABI-prefix stage storage preserves PS int prefix values");
  check(materialized->psConst.bools[0],
        "ABI-prefix stage storage preserves PS bool values");
}

void testChunkSlotUniformStageConstantsPreserveAbiPrefixForIntUpload() {
  DrawDesc desc{};
  desc.primitiveCount = 1u;
  desc.rts.color[0] = RenderTargetAttachment{.handle = Handle{0x3001u}};
  desc.vsConst.float4[0][0] = 12.0f;
  desc.vsConst.float4[10][0] = 34.0f;
  desc.vsConst.int4[0][0] = 56;
  desc.psConst.float4[0][0] = 78.0f;
  desc.psConst.float4[7][0] = 90.0f;
  desc.psConst.int4[0][0] = 123;

  auto uniforms = makeDrawUniformPayload(desc);
  uniforms.vertexFloatConstantCount = 1u;
  uniforms.vertexIntConstantCount = 1u;
  uniforms.vertexBoolConstantCount = 0u;
  uniforms.pixelFloatConstantCount = 1u;
  uniforms.pixelIntConstantCount = 1u;
  uniforms.pixelBoolConstantCount = 0u;

  const DrawParam draw = makeDrawParam(1u, 0u);
  ChunkSlot slot{};
  slot.appendDrawRun(makeCanonicalDrawStateForTest(desc), uniforms,
                     std::span<const DrawParam>(&draw, 1u),
                     std::span<const DrawParamPayloadView>{});

  const auto expectedVsBytes =
      uniforms.vsConst.float4.size() * sizeof(uniforms.vsConst.float4[0]) +
      sizeof(uniforms.vsConst.int4[0]);
  const auto expectedPsBytes =
      uniforms.psConst.float4.size() * sizeof(uniforms.psConst.float4[0]) +
      sizeof(uniforms.psConst.int4[0]);
  checkEq(slot.drawUniformVertexConstantBytes.size(), expectedVsBytes,
          "int VS constants preserve the ABI prefix before int storage");
  checkEq(slot.drawUniformPixelConstantBytes.size(), expectedPsBytes,
          "int PS constants preserve the ABI prefix before int storage");

  const auto command = slot.drawRunCommandAt(0u);
  DrawUniformPayload scratch{};
  const auto* materialized =
      drawRunUniformPayloadForParam(command, command.drawParams[0], scratch);
  check(materialized != nullptr,
        "int ABI-prefix stage storage materializes a uniform payload view");
  check(materialized->vsConst.float4[10][0] == 34.0f,
        "int ABI-prefix stage storage preserves VS float prefix values");
  check(materialized->vsConst.int4[0][0] == 56,
        "int ABI-prefix stage storage preserves VS int values");
  check(materialized->psConst.float4[7][0] == 90.0f,
        "int ABI-prefix stage storage preserves PS float prefix values");
  check(materialized->psConst.int4[0][0] == 123,
        "int ABI-prefix stage storage preserves PS int values");
}
} // namespace

int main() {
  try {
    testChunkSlotReplayObserverAndQueueSummarySeeSameCategories();
    testChunkSlotDrawRunSoAReuseKeepsReservedCapacitiesStable();
    testChunkSlotUniformPayloadLookupHashCollisionKeepsDistinctPayloads();
    testChunkSlotUniformStageConstantsUseCompactByteArena();
    testChunkSlotUniformStageConstantsPreserveAbiPrefixForBoolUpload();
    testChunkSlotUniformStageConstantsPreserveAbiPrefixForIntUpload();
  } catch (const TestFailure &e) {
    std::cerr << "dod_replay_observer_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception &e) {
    std::cerr << "dod_replay_observer_spec unexpected exception: " << e.what()
              << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "dod_replay_observer_spec passed\n";
  return EXIT_SUCCESS;
}
