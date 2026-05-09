#include "chunk_record_import_spec_fixtures.hpp"

namespace {

using namespace dxmt9::d3d9::devicec::spec;

void testImportedMultiRecordIteration() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 2u);

  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "first imported record exists");
  check(first->valid(), "first imported record validates");
  check(first->record == bytes.data(), "first imported record points at stream start");
  checkEq(first->offset, 0u, "first imported record offset");
  checkEq(first->index, 0u, "first imported record index");
  checkEq(first->header.type, static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          "first imported record type");
  checkEq(first->header.size, static_cast<std::uint32_t>(sizeof(present)),
          "first imported record size");

  const auto second = nextImportedRecord(chunk, first->nextOffset(), first->nextIndex());
  check(second.has_value(), "second imported record exists");
  check(second->valid(), "second imported record validates");
  check(second->record == bytes.data() + sizeof(present),
        "second imported record points at second byte range");
  checkEq(second->offset, static_cast<std::uint32_t>(sizeof(present)),
          "second imported record offset");
  checkEq(second->index, 1u, "second imported record index");
  checkEq(second->header.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          "second imported record type");

  const auto end = nextImportedRecord(chunk, second->nextOffset(), second->nextIndex());
  check(!end.has_value(), "imported record iteration stops at chunk end");

  const auto validation = validateImportedChunk(chunk);
  check(validation.valid(), "valid imported chunk validates");
  checkEq(validation.consumedBytes, static_cast<std::uint32_t>(bytes.size()),
          "valid imported chunk consumes all bytes");
  checkEq(validation.parsedRecordCount, 2u, "valid imported chunk parses count");
}

void testImportedWireRecordHandleRangesSelectSubsets() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);

  std::vector<std::uint8_t> arena;
  appendRecord(arena, present);
  appendRecord(arena, draw);

  std::vector<D9CCommandChunkWireHandleEntry> handles{
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x1000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x2000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x3000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  };

  std::vector<D9CCommandChunkWireRecordHeader> records{
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = 0u,
          .payloadSize = static_cast<std::uint32_t>(sizeof(present)),
          .firstHandle = 1u,
          .handleCount = 1u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = static_cast<std::uint32_t>(sizeof(present)),
          .payloadSize = static_cast<std::uint32_t>(sizeof(draw)),
          .firstHandle = 2u,
          .handleCount = 1u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  };

  const auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), arena.data(),
      static_cast<std::uint32_t>(arena.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  check(validateImportedWireChunk(wire).valid(),
        "wire chunk with per-record handle subsets validates");

  const auto firstRange = makeImportedWireRecordHandleView(wire, 0u);
  check(firstRange.handles == handles.data() + 1u,
        "first wire record handle range starts at selected table entry");
  checkEq(firstRange.handleCount, 1u,
          "first wire record handle range exposes selected count");

  ImportedChunkHandleSet firstHandles;
  check(collectImportedWireRecordHandles(wire, 0u, firstHandles),
        "first wire record handle subset collects");
  const auto firstEntries = makeImportedChunkHandleEntries(firstHandles);
  checkEq(firstEntries.size(), static_cast<std::size_t>(1),
          "first wire record collects only its handle range");
  check(containsHandle(firstEntries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2000u),
        "first wire record selected the buffer handle");

  ImportedChunkHandleSet secondHandles;
  check(collectImportedWireRecordHandles(wire, 1u, secondHandles),
        "second wire record handle subset collects");
  const auto secondEntries = makeImportedChunkHandleEntries(secondHandles);
  checkEq(secondEntries.size(), static_cast<std::size_t>(1),
          "second wire record collects only its handle range");
  check(containsHandle(secondEntries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3000u),
        "second wire record selected the surface handle");

  ImportedChunkHandleSet chunkHandles;
  check(collectImportedWireChunkHandles(wire, chunkHandles),
        "wire chunk handle collection walks per-record ranges");
  const auto chunkEntries = makeImportedChunkHandleEntries(chunkHandles);
  checkEq(chunkEntries.size(), static_cast<std::size_t>(2),
          "wire chunk collection ignores unreferenced table entries");
  check(!containsHandle(chunkEntries, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1000u),
        "wire chunk collection excludes handles outside all record ranges");
}

void testImportedWireIterationBuildsLegacyRecordViews() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  std::vector<D9CCommandChunkWireRecordHeader> records{
      wireRecordHeader(D9C_COMMAND_RECORD_PRESENT, 0u,
                       static_cast<std::uint32_t>(sizeof(present))),
      wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                       static_cast<std::uint32_t>(sizeof(present)),
                       static_cast<std::uint32_t>(sizeof(draw))),
  };
  const auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), nullptr, 0u);

  const auto first = nextImportedRecord(wire, 0u);
  check(first.has_value(), "first wire imported record exists");
  check(first->valid(), "first wire imported record validates as legacy record");
  check(first->record == bytes.data(),
        "first wire imported record points at legacy payload arena");
  checkEq(first->offset, 0u, "first wire imported record offset");
  checkEq(first->index, 0u, "first wire imported record index");
  checkEq(first->header.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          "first wire imported record type");
  checkEq(first->header.size, static_cast<std::uint32_t>(sizeof(present)),
          "first wire imported record size");

  const auto second = nextImportedRecord(wire, first->nextIndex());
  check(second.has_value(), "second wire imported record exists");
  check(second->valid(), "second wire imported record validates as legacy record");
  check(second->record == bytes.data() + sizeof(present),
        "second wire imported record points at legacy payload arena");
  checkEq(second->offset, static_cast<std::uint32_t>(sizeof(present)),
          "second wire imported record offset");
  checkEq(second->index, 1u, "second wire imported record index");
  checkEq(second->header.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          "second wire imported record type");

  const auto end = nextImportedRecord(wire, second->nextIndex());
  check(!end.has_value(), "wire imported record iteration stops at record count");
}

void testImportedWireBlobViewUsesHeaderTablesAndArena() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(11u, 2u);

  constexpr std::uint32_t drawPayloadOffset = 8u;
  const auto presentPayloadOffset =
      static_cast<std::uint32_t>(drawPayloadOffset + sizeof(draw) + 32u);
  std::vector<std::uint8_t> arena(presentPayloadOffset + sizeof(present));
  writeObject(arena, drawPayloadOffset, draw);
  writeObject(arena, presentPayloadOffset, present);

  std::vector<D9CCommandChunkWireRecordHeader> records{
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = presentPayloadOffset,
          .payloadSize = static_cast<std::uint32_t>(sizeof(present)),
          .firstHandle = 1u,
          .handleCount = 1u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = drawPayloadOffset,
          .payloadSize = static_cast<std::uint32_t>(sizeof(draw)),
          .firstHandle = 0u,
          .handleCount = 1u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  };
  std::vector<D9CCommandChunkWireHandleEntry> handles{
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x1000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x3000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  };

  const auto recordTableOffset =
      static_cast<std::uint32_t>(sizeof(D9CCommandChunkWireHeader));
  const auto recordTableBytes =
      static_cast<std::uint32_t>(records.size() * sizeof(records[0]));
  const auto handleTableOffset = recordTableOffset + recordTableBytes;
  const auto handleTableBytes =
      static_cast<std::uint32_t>(handles.size() * sizeof(handles[0]));
  const auto payloadArenaOffset = handleTableOffset + handleTableBytes + 16u;

  D9CCommandChunkWireHeader header{};
  header.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
  header.headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  header.recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  header.handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  header.recordTableOffset = recordTableOffset;
  header.recordCount = static_cast<std::uint32_t>(records.size());
  header.handleTableOffset = handleTableOffset;
  header.handleCount = static_cast<std::uint32_t>(handles.size());
  header.payloadArenaOffset = payloadArenaOffset;
  header.payloadArenaSize = static_cast<std::uint32_t>(arena.size());

  std::vector<std::uint8_t> blob(payloadArenaOffset + arena.size());
  writeObject(blob, 0u, header);
  std::memcpy(blob.data() + recordTableOffset, records.data(), recordTableBytes);
  std::memcpy(blob.data() + handleTableOffset, handles.data(), handleTableBytes);
  std::memcpy(blob.data() + payloadArenaOffset, arena.data(), arena.size());

  const auto decoded = makeImportedWireChunkBlobView(
      blob.data(), static_cast<std::uint32_t>(blob.size()));
  check(decoded.wireHeaderCandidate(), "wire blob decoder recognizes DOD header");
  check(decoded.valid(), "wire blob decoder validates header tables and arena");
  checkEq(decoded.chunk.recordCount, 2u, "wire blob decoder exposes record table count");
  checkEq(decoded.chunk.handleCount, 2u, "wire blob decoder exposes handle table count");

  const auto first = nextImportedRecord(decoded.chunk, 0u);
  check(first.has_value(), "wire blob first record exists");
  check(first->valid(), "wire blob first record validates");
  check(first->record == blob.data() + payloadArenaOffset + presentPayloadOffset,
        "wire blob first record follows record table order, not payload offset order");
  checkEq(first->header.type, static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          "wire blob first record type");

  const auto second = nextImportedRecord(decoded.chunk, first->nextIndex());
  check(second.has_value(), "wire blob second record exists");
  check(second->valid(), "wire blob second record validates");
  D9CCommandRecordDrawPrimitive decodedDraw{};
  std::memcpy(&decodedDraw, second->record, sizeof(decodedDraw));
  checkEq(decodedDraw.packet.startVertex, 11u,
          "wire blob second record reads lower non-contiguous payload");

  ImportedChunkHandleSet firstHandles;
  check(collectImportedWireRecordHandles(decoded.chunk, 0u, firstHandles),
        "wire blob first record handle subset collects");
  const auto firstEntries = makeImportedChunkHandleEntries(firstHandles);
  checkEq(firstEntries.size(), static_cast<std::size_t>(1),
          "wire blob first record selects one handle");
  check(containsHandle(firstEntries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3000u),
        "wire blob first record selects handle range from table");

  auto badBlob = blob;
  header.reserved0 = 1u;
  writeObject(badBlob, 0u, header);
  const auto badHeader = makeImportedWireChunkBlobView(
      badBlob.data(), static_cast<std::uint32_t>(badBlob.size()));
  checkEq(static_cast<int>(badHeader.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidChunkHeader),
          "wire blob decoder rejects nonzero chunk header reserved fields");
}

void testImportedRecordCountMismatch() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(0u, 1u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 1u);
  const auto validation = validateImportedChunk(chunk);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedChunkValidationStatus::RecordCountMismatch),
          "record count mismatch is reported");
  checkEq(validation.parsedRecordCount, 2u,
          "record count mismatch reports parsed records");
  checkEq(validation.consumedBytes, static_cast<std::uint32_t>(bytes.size()),
          "record count mismatch still consumes valid byte stream");
}

void testImportedTruncatedTail() {
  const auto present = makePresentRecord();

  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.rectCount = 1u;
  clear.rectOffset = sizeof(D9CCommandRecordClear);
  clear.header.size = sizeof(D9CCommandRecordClear) + sizeof(D9CRect);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, clear, clear.header.size - 1u);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 2u);
  const auto validation = validateImportedChunk(chunk);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedChunkValidationStatus::InvalidRecord),
          "truncated tail marks chunk invalid");
  checkEq(validation.parsedRecordCount, 1u,
          "truncated tail reports records before failure");
  checkEq(validation.consumedBytes, static_cast<std::uint32_t>(sizeof(present)),
          "truncated tail reports failure offset");
  checkEq(validation.failedRecord.offset, static_cast<std::uint32_t>(sizeof(present)),
          "truncated tail failed record offset");
  checkEq(validation.failedRecord.index, 1u,
          "truncated tail failed record index");
  checkEq(static_cast<int>(validation.failedRecord.validation.status),
          static_cast<int>(D9CCommandRecordValidationStatus::TruncatedRecord),
          "truncated tail preserves record validation status");
}

void testImportedChunkHandleAppendRejectsInvalidAndDuplicateHandles() {
  ImportedChunkHandleSet handles;
  check(!appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0u),
        "zero handle is ignored");
  check(!appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL + 1u, 0x1u),
        "unknown handle kind is ignored");
  check(appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x7000u),
        "first valid handle is appended");
  check(!appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x7000u),
        "duplicate handle is ignored");

  const auto entries = makeImportedChunkHandleEntries(handles);
  checkEq(entries.size(), static_cast<std::size_t>(1), "only one valid handle retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x7000u),
        "valid handle entry retained");

  D9CCommandChunkWireHandleEntry wireEntry{};
  wireEntry.kind = entries[0].kind;
  wireEntry.opaqueHandle = entries[0].handle;
  checkEq(wireEntry.kind, D9C_CHUNK_HANDLE_KIND_BUFFER,
          "DOD wire handle entry preserves imported handle kind");
  checkEq(wireEntry.opaqueHandle, 0x7000u,
          "DOD wire handle entry preserves imported opaque handle");
  checkEq(wireEntry.generation, D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          "DOD wire handle generation defaults to none for imported legacy handles");
  checkEq(wireEntry.reserved0, 0u, "DOD wire handle reserved0 defaults to zero");
  checkEq(wireEntry.reserved1, 0u, "DOD wire handle reserved1 defaults to zero");
}

}  // namespace

int main() {
  try {
    testImportedMultiRecordIteration();
    testImportedWireRecordHandleRangesSelectSubsets();
    testImportedWireIterationBuildsLegacyRecordViews();
    testImportedWireBlobViewUsesHeaderTablesAndArena();
    testImportedRecordCountMismatch();
    testImportedTruncatedTail();
    testImportedChunkHandleAppendRejectsInvalidAndDuplicateHandles();
  } catch (const dxmt9::d3d9::devicec::spec::TestFailure& e) {
    std::cerr << "chunk_record_import_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
