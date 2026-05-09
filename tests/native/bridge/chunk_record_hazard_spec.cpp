#include "chunk_record_import_spec_fixtures.hpp"

namespace {

using namespace dxmt9::d3d9::devicec::spec;

void testResourceRetentionDerivationFromDrawRecords() {
  D9CCommandRecordDrawIndexedPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.state.textureMask = 0x3u;
  draw.packet.state.textures[0] = wireHandle(0x1000u);
  draw.packet.state.textures[1] = wireHandle(0x1000u);
  draw.packet.state.streamSourceMask = 0x3u;
  draw.packet.state.streamSources[0].buffer = wireHandle(0x2000u);
  draw.packet.state.streamSources[1].buffer = wireHandle(0x2008u);
  draw.packet.state.rtMask = 0x3u;
  draw.packet.state.rtHandles[0] = wireHandle(0x3000u);
  draw.packet.state.rtHandles[1] = wireHandle(0x3008u);
  draw.packet.state.dsValid = 1u;
  draw.packet.state.dsHandle = wireHandle(0x3010u);
  draw.packet.ibValid = 1u;
  draw.packet.ibHandle = wireHandle(0x2010u);

  const auto bytes = recordBytes(draw);
  const auto chunk = makeImportedChunkView(bytes.data(), static_cast<std::uint32_t>(bytes.size()), 1u);
  const auto record = nextImportedRecord(chunk, 0u, 0u);
  check(record.has_value(), "draw retention record exists");
  check(record->valid(), "draw retention record validates");

  ImportedChunkHandleSet handles;
  collectImportedRecordResourceHandles(*record, handles);
  const auto entries = makeImportedChunkHandleEntries(handles);

  checkEq(entries.size(), static_cast<std::size_t>(7), "draw retention set deduplicates handles");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1000u),
        "draw retention includes texture handle");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2000u),
        "draw retention includes stream 0 buffer");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2008u),
        "draw retention includes stream 1 buffer");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2010u),
        "draw retention includes index buffer");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3000u),
        "draw retention includes RT0");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3008u),
        "draw retention includes RT1");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3010u),
        "draw retention includes depth-stencil");
}

void testResourceRetentionDerivationFromSurfaceAndReadbackRecords() {
  D9CCommandRecordStretchRect stretch{};
  stretch.header.type = D9C_COMMAND_RECORD_STRETCH_RECT;
  stretch.header.size = sizeof(stretch);
  stretch.srcWire = 0x4000u;
  stretch.dstWire = 0x4008u;

  D9CCommandRecordUpdateTexture updateTexture{};
  updateTexture.header.type = D9C_COMMAND_RECORD_UPDATE_TEXTURE;
  updateTexture.header.size = sizeof(updateTexture);
  updateTexture.srcWire = 0x5000u;
  updateTexture.dstWire = 0x5008u;

  D9CCommandRecordReadback readback{};
  readback.header.type = D9C_COMMAND_RECORD_READBACK;
  readback.header.size = sizeof(readback);
  readback.srcWire = 0x6000u;
  readback.dstWire = 0x6008u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, stretch);
  appendRecord(bytes, updateTexture);
  appendRecord(bytes, readback);

  const auto chunk = makeImportedChunkView(bytes.data(), static_cast<std::uint32_t>(bytes.size()), 3u);
  ImportedChunkHandleSet handles;
  std::uint32_t offset = 0;
  std::uint32_t index = 0;
  while (auto record = nextImportedRecord(chunk, offset, index)) {
    check(record->valid(), "surface/readback retention record validates");
    collectImportedRecordResourceHandles(*record, handles);
    const auto info = replayInfoForImportedRecord(*record);
    check(info.barrier, "surface/readback record is an ordering barrier");
    offset = record->nextOffset();
    index = record->nextIndex();
  }
  checkEq(index, 3u, "surface/readback retention parsed all records");

  const auto entries = makeImportedChunkHandleEntries(handles);
  checkEq(entries.size(), static_cast<std::size_t>(6), "surface/readback retention includes all unique resources");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x4000u), "stretch source retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x4008u), "stretch destination retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x5000u), "update texture source retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x5008u), "update texture destination retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x6000u), "readback source retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x6008u), "readback destination retained");
}

void testImportedRecordHazardsSeparateReadsAndWrites() {
  auto draw = makeHazardDrawRecord(0x3000u, 0x1000u, 0x2000u);
  D9CCommandRecordDrawIndexedPrimitive indexed{};
  indexed.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  indexed.header.size = sizeof(indexed);
  indexed.packet.state = draw.packet;
  indexed.packet.ibValid = 1u;
  indexed.packet.ibHandle = wireHandle(0x2010u);

  const auto bytes = recordBytes(indexed);
  const auto chunk = makeImportedChunkView(bytes.data(), static_cast<std::uint32_t>(bytes.size()), 1u);
  const auto record = nextImportedRecord(chunk, 0u, 0u);
  check(record.has_value(), "hazard record exists");
  check(record->valid(), "hazard record validates");

  dxmt9::d3d9::devicec::ImportedRecordResourceHazards hazards;
  collectImportedRecordResourceHazards(*record, hazards);
  const auto reads = makeImportedChunkHandleEntries(hazards.reads);
  const auto writes = makeImportedChunkHandleEntries(hazards.writes);

  checkEq(reads.size(), static_cast<std::size_t>(3), "draw hazards track read handles");
  check(containsHandle(reads, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1000u),
        "draw hazard reads texture");
  check(containsHandle(reads, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2000u),
        "draw hazard reads stream buffer");
  check(containsHandle(reads, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2010u),
        "indexed draw hazard reads index buffer");
  checkEq(writes.size(), static_cast<std::size_t>(1), "draw hazards track write handles");
  check(containsHandle(writes, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3000u),
        "draw hazard writes render target");
}

void testImportedReplayOrderingDecisionsAreDataDriven() {
  const auto firstDraw = makeHazardDrawRecord(0x3000u, 0x1000u, 0x2000u);
  const auto cleanDraw = makeHazardDrawRecord(0x3008u, 0x1008u, 0x2008u);
  const auto overlappingDraw = makeHazardDrawRecord(0x3000u, 0x1010u, 0x2010u);

  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.header.size = sizeof(clear);
  clear.rectOffset = sizeof(D9CCommandRecordClear);

  D9CCommandRecordReadback readback{};
  readback.header.type = D9C_COMMAND_RECORD_READBACK;
  readback.header.size = sizeof(readback);
  readback.srcWire = 0x3000u;
  readback.dstWire = 0x3010u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, cleanDraw);
  appendRecord(bytes, overlappingDraw);
  appendRecord(bytes, clear);
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, readback);

  const auto chunk = makeImportedChunkView(bytes.data(), static_cast<std::uint32_t>(bytes.size()), 6u);
  ImportedReplayHazardState state;
  std::uint32_t offset = 0;
  std::uint32_t index = 0;

  const auto nextDecision = [&]() {
    const auto record = nextImportedRecord(chunk, offset, index);
    check(record.has_value(), "ordering test record exists");
    check(record->valid(), "ordering test record validates");
    const auto decision = evaluateImportedReplayOrdering(*record, state);
    state = nextImportedReplayHazardState(state, decision);
    offset = record->nextOffset();
    index = record->nextIndex();
    return decision;
  };

  auto decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::Continue),
          "first draw starts active hazard tracking");
  check(!decision.readAfterWrite && !decision.writeAfterRead && !decision.writeAfterWrite,
        "first draw has no prior hazards");
  check(state.active, "first draw leaves active hazards");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::Continue),
          "disjoint draw continues without a boundary");
  check(!decision.hazardBoundary(), "disjoint draw is not a hazard boundary");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::HazardBoundary),
          "overlapping render-target write starts a hazard boundary");
  check(decision.writeAfterWrite, "overlapping draw reports WAW");
  check(decision.hazardBoundary(), "overlapping draw exposes hazard boundary helper");
  check(state.active, "hazard boundary starts a new active hazard scope");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::BarrierBoundary),
          "clear is an explicit replay barrier");
  check(decision.barrierBoundary(), "clear exposes barrier boundary helper");
  check(decision.resetsActiveHazards, "barrier resets active hazards");
  check(!state.active, "barrier clears active hazard scope");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::Continue),
          "draw after barrier does not inherit prior hazards");
  check(!decision.writeAfterWrite, "barrier prevents stale WAW carry-over");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::SynchronousReadBoundary),
          "readback is a synchronous read boundary");
  check(decision.barrierBoundary(), "readback is also a barrier boundary");
  check(decision.resetsActiveHazards, "readback resets active hazards");
  check(!state.active, "readback clears active hazard scope");
  checkEq(index, 6u, "ordering decisions consumed all records");
}

}  // namespace

int main() {
  try {
    testResourceRetentionDerivationFromDrawRecords();
    testResourceRetentionDerivationFromSurfaceAndReadbackRecords();
    testImportedRecordHazardsSeparateReadsAndWrites();
    testImportedReplayOrderingDecisionsAreDataDriven();
  } catch (const dxmt9::d3d9::devicec::spec::TestFailure& e) {
    std::cerr << "chunk_record_hazard_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
