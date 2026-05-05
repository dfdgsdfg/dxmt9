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
#include "device_c_record_utils.hpp"
#include "dxmt9/core.hpp"

namespace {

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;

namespace devicec = dxmt9::d3d9::devicec;
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

template <typename T>
void appendRecord(std::vector<std::uint8_t> &bytes, const T &record) {
  const auto *begin = reinterpret_cast<const std::uint8_t *>(&record);
  bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

D9CWireHandle wireHandle(std::uint64_t value) {
  return D9CWireHandle{
      .lo = static_cast<std::uint32_t>(value),
      .hi = static_cast<std::uint32_t>(value >> 32),
  };
}

D9CCommandRecordDrawPrimitive makeHazardDrawRecord(std::uint64_t renderTarget,
                                                   std::uint64_t texture,
                                                   std::uint64_t vertexBuffer,
                                                   std::uint32_t startVertex) {
  D9CCommandRecordDrawPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.primitiveType = 4u;
  draw.packet.primitiveCount = 1u;
  draw.packet.startVertex = startVertex;
  draw.packet.textureMask = 0x1u;
  draw.packet.textures[0] = wireHandle(texture);
  draw.packet.streamSourceMask = 0x1u;
  draw.packet.streamSources[0].buffer = wireHandle(vertexBuffer);
  draw.packet.streamSources[0].stride = 16u;
  draw.packet.rtMask = 0x1u;
  draw.packet.rtHandles[0] = wireHandle(renderTarget);
  return draw;
}

D9CCommandRecordClear makeClearRecord() {
  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.header.size = sizeof(clear);
  clear.rectOffset = sizeof(D9CCommandRecordClear);
  clear.flags = 1u;
  clear.colorARGB = 0xff203040u;
  clear.z = 1.0f;
  return clear;
}

D9CCommandRecordStretchRect makeStretchRecord(std::uint64_t source,
                                              std::uint64_t destination) {
  D9CCommandRecordStretchRect stretch{};
  stretch.header.type = D9C_COMMAND_RECORD_STRETCH_RECT;
  stretch.header.size = sizeof(stretch);
  stretch.srcWire = source;
  stretch.dstWire = destination;
  return stretch;
}

D9CCommandRecordReadback makeReadbackRecord(std::uint64_t source,
                                            std::uint64_t destination) {
  D9CCommandRecordReadback readback{};
  readback.header.type = D9C_COMMAND_RECORD_READBACK;
  readback.header.size = sizeof(readback);
  readback.srcWire = source;
  readback.dstWire = destination;
  return readback;
}

D9CCommandRecordPresent makePresentRecord() {
  D9CCommandRecordPresent present{};
  present.header.type = D9C_COMMAND_RECORD_PRESENT;
  present.header.size = sizeof(present);
  return present;
}

bool containsHandle(const std::vector<D9CChunkHandleEntry> &entries,
                    std::uint32_t kind, std::uint64_t handle) {
  for (const auto &entry : entries) {
    if (entry.kind == kind && entry.handle == handle) {
      return true;
    }
  }
  return false;
}

struct ImportedReplayObservation {
  std::uint32_t recordIndex = 0;
  std::uint32_t recordType = 0;
  devicec::ImportedRecordReplayCategory category =
      devicec::ImportedRecordReplayCategory::Unknown;
  devicec::ImportedReplayOrderingAction action =
      devicec::ImportedReplayOrderingAction::InvalidRecord;
  bool hazardBoundary = false;
  bool barrierBoundary = false;
  bool synchronousReadBoundary = false;
};

struct ImportedReplayObserver {
  std::vector<ImportedReplayObservation> observations;
  devicec::ImportedChunkHandleSet retainedHandles{};

  void observe(const devicec::ImportedChunkView &chunk) {
    devicec::ImportedReplayHazardState hazardState{};
    std::uint32_t offset = 0;
    std::uint32_t index = 0;

    while (auto record = devicec::nextImportedRecord(chunk, offset, index)) {
      check(record->valid(), "imported observer sees only valid records");
      devicec::collectImportedRecordResourceHandles(*record, retainedHandles);

      const auto decision =
          devicec::evaluateImportedReplayOrdering(*record, hazardState);
      observations.push_back(ImportedReplayObservation{
          .recordIndex = record->index,
          .recordType = record->header.type,
          .category = decision.replayInfo.category,
          .action = decision.action,
          .hazardBoundary = decision.hazardBoundary(),
          .barrierBoundary = decision.barrierBoundary(),
          .synchronousReadBoundary =
              decision.replayInfo.synchronousReadBoundary,
      });

      hazardState =
          devicec::nextImportedReplayHazardState(hazardState, decision);
      offset = record->nextOffset();
      index = record->nextIndex();
    }
  }
};

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
  std::size_t drawUniformPayloads = 0;
  std::size_t drawUniformPayloadLookupHeads = 0;
  std::size_t drawUniformPayloadLookupTails = 0;
  std::size_t drawUniformPayloadLookupNext = 0;
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
      .drawUniformPayloads = slot.drawUniformPayloads.capacity(),
      .drawUniformPayloadLookupHeads =
          slot.drawUniformPayloadLookupHeads.capacity(),
      .drawUniformPayloadLookupTails =
          slot.drawUniformPayloadLookupTails.capacity(),
      .drawUniformPayloadLookupNext =
          slot.drawUniformPayloadLookupNext.capacity(),
      .drawParams = slot.drawParams.capacity(),
      .drawPayloadArena = slot.drawPayloadArena.capacity(),
      .drawRunRecords = slot.drawRunRecords.capacity(),
  };
}

void testImportedReplayObserverRetainsHandlesAndBoundariesInOrder() {
  const auto firstDraw = makeHazardDrawRecord(0x3000u, 0x1000u, 0x2000u, 0u);
  const auto disjointDraw = makeHazardDrawRecord(0x3008u, 0x1008u, 0x2008u, 3u);
  const auto overlappingDraw =
      makeHazardDrawRecord(0x3000u, 0x1010u, 0x2010u, 6u);
  const auto clear = makeClearRecord();
  const auto stretch = makeStretchRecord(0x4000u, 0x4008u);
  const auto readback = makeReadbackRecord(0x5000u, 0x5008u);
  const auto present = makePresentRecord();

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, disjointDraw);
  appendRecord(bytes, overlappingDraw);
  appendRecord(bytes, clear);
  appendRecord(bytes, stretch);
  appendRecord(bytes, readback);
  appendRecord(bytes, present);

  const auto chunk = devicec::makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 7u);
  ImportedReplayObserver observer;
  observer.observe(chunk);

  checkEq(observer.observations.size(), std::size_t{7},
          "imported observer records every replay record");
  for (std::size_t i = 0; i < observer.observations.size(); ++i) {
    checkEq(observer.observations[i].recordIndex, static_cast<std::uint32_t>(i),
            "imported observer preserves record-table order");
  }

  using Category = devicec::ImportedRecordReplayCategory;
  using Action = devicec::ImportedReplayOrderingAction;
  const std::array<Category, 7> categories{
      Category::Draw,    Category::Draw,      Category::Draw,
      Category::Clear,   Category::SurfaceOp, Category::Readback,
      Category::Present,
  };
  const std::array<Action, 7> actions{
      Action::Continue,        Action::Continue,
      Action::HazardBoundary,  Action::BarrierBoundary,
      Action::BarrierBoundary, Action::SynchronousReadBoundary,
      Action::BarrierBoundary,
  };
  for (std::size_t i = 0; i < categories.size(); ++i) {
    checkEq(observer.observations[i].category, categories[i],
            "imported observer records replay category");
    checkEq(observer.observations[i].action, actions[i],
            "imported observer records ordering action");
  }
  check(observer.observations[2].hazardBoundary,
        "overlapping draw is reported as a hazard boundary");
  check(observer.observations[3].barrierBoundary,
        "clear is reported as a barrier boundary");
  check(observer.observations[5].barrierBoundary,
        "readback is also reported as a barrier boundary");
  check(observer.observations[5].synchronousReadBoundary,
        "readback keeps its synchronous-read marker");

  const auto retained =
      devicec::makeImportedChunkHandleEntries(observer.retainedHandles);
  checkEq(retained.size(), std::size_t{12},
          "imported observer retains every unique resource handle");
  check(containsHandle(retained, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1000u),
        "retained handles include first draw texture");
  check(containsHandle(retained, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1010u),
        "retained handles include hazard draw texture");
  check(containsHandle(retained, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2008u),
        "retained handles include disjoint stream buffer");
  check(containsHandle(retained, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2010u),
        "retained handles include hazard stream buffer");
  check(containsHandle(retained, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3000u),
        "retained handles dedupe the overlapping render target");
  check(containsHandle(retained, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x4008u),
        "retained handles include stretch destination");
  check(containsHandle(retained, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x5008u),
        "retained handles include readback destination");
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
  for (const auto &record : slot.drawRunRecords) {
    checkEq(record.uniformHandle, firstUniformHandle,
            "reused draw runs point at the same uniform handle");
  }

  slot.clearCommands();
  check(slot.commandsEmpty(), "clearCommands removes command headers");
  checkEq(capacitySnapshot(slot), stableCapacity,
          "clearCommands preserves SoA capacities for queue-local reuse");
}

} // namespace

int main() {
  try {
    testImportedReplayObserverRetainsHandlesAndBoundariesInOrder();
    testChunkSlotReplayObserverAndQueueSummarySeeSameCategories();
    testChunkSlotDrawRunSoAReuseKeepsReservedCapacitiesStable();
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
