#include "chunk_record_import_spec_fixtures.hpp"
#include "device_c_common.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"

#include <cstdint>

namespace {

using namespace dxmt9::d3d9::devicec::spec;

void testFixedRecordValidation() {
  D9CCommandRecordPresent present{};
  present.header.type = D9C_COMMAND_RECORD_PRESENT;
  present.header.size = sizeof(present);

  auto bytes = recordBytes(present);
  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), "present fixed record validates");
  checkEq(validation.minimumSize, static_cast<std::uint32_t>(sizeof(present)),
          "present fixed minimum size");
  checkEq(validation.expectedSize, static_cast<std::uint64_t>(sizeof(present)),
          "present fixed expected size");

  present.header.size = sizeof(present) + 4u;
  checkStatus(recordBytes(present, present.header.size),
              D9CCommandRecordValidationStatus::SizeMismatch,
              "fixed records reject trailing bytes");
}

void testClearRectTailValidation() {
  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.rectCount = 2u;
  clear.rectOffset = sizeof(D9CCommandRecordClear);
  clear.header.size = sizeof(D9CCommandRecordClear) + sizeof(D9CRect) * clear.rectCount;

  auto bytes = recordBytes(clear, clear.header.size);
  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), "clear rect tail validates");
  checkEq(validation.expectedSize, static_cast<std::uint64_t>(clear.header.size),
          "clear rect expected size includes tail");

  clear.rectOffset = sizeof(D9CCommandRecordHeader);
  checkStatus(recordBytes(clear, clear.header.size),
              D9CCommandRecordValidationStatus::SizeMismatch,
              "clear rejects non-tail rect offset");
}

void testSetConstTailValidation() {
  D9CCommandRecordSetConst setConst{};
  setConst.header.type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
  setConst.count = 3u;
  setConst.header.size = sizeof(D9CCommandRecordSetConst) + sizeof(float) * 4u * setConst.count;

  auto bytes = recordBytes(setConst, setConst.header.size);
  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), "set const float tail validates");
  checkEq(validation.expectedSize, static_cast<std::uint64_t>(setConst.header.size),
          "set const float expected size includes vec4 tail");

  setConst.header.type = D9C_COMMAND_RECORD_SET_PS_CONST_B;
  setConst.count = 5u;
  setConst.header.size = sizeof(D9CCommandRecordSetConst) +
                         sizeof(std::uint32_t) * setConst.count;
  bytes = recordBytes(setConst, setConst.header.size);
  const auto boolValidation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(boolValidation.valid(), "set const bool tail validates");
  checkEq(boolValidation.expectedSize, static_cast<std::uint64_t>(setConst.header.size),
          "set const bool expected size uses scalar tail");

  setConst.header.type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
  setConst.count = 3u;
  setConst.header.size = sizeof(D9CCommandRecordSetConst) + sizeof(float) * 4u * 2u;
  checkStatus(recordBytes(setConst, setConst.header.size),
              D9CCommandRecordValidationStatus::SizeMismatch,
              "set const rejects truncated logical tail");
}

void testAllCommandIdsValidateWithExpectedRecordShapes() {
  const auto draw = makeDrawPrimitiveRecord(0u, 1u);
  checkValidRecordBytes(recordBytes(draw), D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                        sizeof(draw), sizeof(draw),
                        "DRAW_PRIMITIVE record");

  const auto indexed = makeDrawIndexedPrimitiveRecord(0u, 1u);
  checkValidRecordBytes(recordBytes(indexed),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
                        sizeof(indexed), sizeof(indexed),
                        "DRAW_INDEXED_PRIMITIVE record");

  const auto drawUp = makeDrawPrimitiveUPRecord(64u);
  checkValidRecordBytes(declaredRecordBytes(drawUp),
                        D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawPrimitiveUP),
                        drawUp.header.size,
                        "DRAW_PRIMITIVE_UP record");

  const auto indexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkValidRecordBytes(declaredRecordBytes(indexedUp),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawIndexedPrimitiveUP),
                        indexedUp.header.size,
                        "DRAW_INDEXED_PRIMITIVE_UP record");

  const auto vsFloat = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_F, 2u, sizeof(float) * 4u);
  checkValidRecordBytes(declaredRecordBytes(vsFloat),
                        D9C_COMMAND_RECORD_SET_VS_CONST_F,
                        sizeof(D9CCommandRecordSetConst),
                        vsFloat.header.size,
                        "SET_VS_CONST_F record");

  const auto vsInt = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_I, 2u, sizeof(std::int32_t) * 4u);
  checkValidRecordBytes(declaredRecordBytes(vsInt),
                        D9C_COMMAND_RECORD_SET_VS_CONST_I,
                        sizeof(D9CCommandRecordSetConst),
                        vsInt.header.size,
                        "SET_VS_CONST_I record");

  const auto vsBool = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_B, 3u, sizeof(std::uint32_t));
  checkValidRecordBytes(declaredRecordBytes(vsBool),
                        D9C_COMMAND_RECORD_SET_VS_CONST_B,
                        sizeof(D9CCommandRecordSetConst),
                        vsBool.header.size,
                        "SET_VS_CONST_B record");

  const auto psFloat = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_F, 2u, sizeof(float) * 4u);
  checkValidRecordBytes(declaredRecordBytes(psFloat),
                        D9C_COMMAND_RECORD_SET_PS_CONST_F,
                        sizeof(D9CCommandRecordSetConst),
                        psFloat.header.size,
                        "SET_PS_CONST_F record");

  const auto psInt = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_I, 2u, sizeof(std::int32_t) * 4u);
  checkValidRecordBytes(declaredRecordBytes(psInt),
                        D9C_COMMAND_RECORD_SET_PS_CONST_I,
                        sizeof(D9CCommandRecordSetConst),
                        psInt.header.size,
                        "SET_PS_CONST_I record");

  const auto psBool = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_B, 3u, sizeof(std::uint32_t));
  checkValidRecordBytes(declaredRecordBytes(psBool),
                        D9C_COMMAND_RECORD_SET_PS_CONST_B,
                        sizeof(D9CCommandRecordSetConst),
                        psBool.header.size,
                        "SET_PS_CONST_B record");

  const auto clear = makeClearRecord(2u);
  checkValidRecordBytes(declaredRecordBytes(clear), D9C_COMMAND_RECORD_CLEAR,
                        sizeof(D9CCommandRecordClear), clear.header.size,
                        "CLEAR record");

  const auto present = makePresentRecord();
  checkValidRecordBytes(recordBytes(present), D9C_COMMAND_RECORD_PRESENT,
                        sizeof(present), sizeof(present),
                        "PRESENT record");

  const auto stretch = makeStretchRectRecord();
  checkValidRecordBytes(recordBytes(stretch), D9C_COMMAND_RECORD_STRETCH_RECT,
                        sizeof(stretch), sizeof(stretch),
                        "STRETCH_RECT record");

  const auto color = makeColorFillRecord();
  checkValidRecordBytes(recordBytes(color), D9C_COMMAND_RECORD_COLOR_FILL,
                        sizeof(color), sizeof(color),
                        "COLOR_FILL record");

  const auto updateTexture = makeUpdateTextureRecord();
  checkValidRecordBytes(recordBytes(updateTexture),
                        D9C_COMMAND_RECORD_UPDATE_TEXTURE,
                        sizeof(updateTexture), sizeof(updateTexture),
                        "UPDATE_TEXTURE record");

  const auto updateSurface = makeUpdateSurfaceRecord();
  checkValidRecordBytes(recordBytes(updateSurface),
                        D9C_COMMAND_RECORD_UPDATE_SURFACE,
                        sizeof(updateSurface), sizeof(updateSurface),
                        "UPDATE_SURFACE record");

  const auto query = makeQueryIssueRecord();
  checkValidRecordBytes(recordBytes(query), D9C_COMMAND_RECORD_QUERY_ISSUE,
                        sizeof(query), sizeof(query),
                        "QUERY_ISSUE record");

  const auto readback = makeReadbackRecord();
  checkValidRecordBytes(recordBytes(readback), D9C_COMMAND_RECORD_READBACK,
                        sizeof(readback), sizeof(readback),
                        "READBACK record");

  const auto apply = makeApplyStateRecord();
  checkValidRecordBytes(recordBytes(apply), D9C_COMMAND_RECORD_APPLY_STATE,
                        sizeof(apply), sizeof(apply),
                        "APPLY_STATE record");
}

void testDrawUpValidationMatrix() {
  auto drawUp = makeDrawPrimitiveUPRecord(64u);
  checkValidRecordBytes(declaredRecordBytes(drawUp),
                        D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawPrimitiveUP),
                        drawUp.header.size,
                        "draw primitive UP with vertex tail");

  drawUp = makeDrawPrimitiveUPRecord(0u);
  checkValidRecordBytes(recordBytes(drawUp),
                        D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawPrimitiveUP),
                        sizeof(D9CCommandRecordDrawPrimitiveUP),
                        "draw primitive UP minimum record");

  auto tooSmallDrawUp = drawUp;
  tooSmallDrawUp.header.size =
      static_cast<std::uint32_t>(sizeof(D9CCommandRecordDrawPrimitiveUP) - 1u);
  checkStatus(recordBytes(tooSmallDrawUp, tooSmallDrawUp.header.size),
              D9CCommandRecordValidationStatus::SizeTooSmall,
              "draw primitive UP rejects undersized fixed header");

  auto truncatedDrawUp = makeDrawPrimitiveUPRecord(64u);
  checkStatus(recordBytes(truncatedDrawUp, truncatedDrawUp.header.size - 1u),
              D9CCommandRecordValidationStatus::TruncatedRecord,
              "draw primitive UP rejects unavailable declared tail");

  auto indexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkValidRecordBytes(declaredRecordBytes(indexedUp),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawIndexedPrimitiveUP),
                        indexedUp.header.size,
                        "draw indexed primitive UP with index and vertex tails");

  indexedUp = makeDrawIndexedPrimitiveUPRecord(0u, 0u);
  checkValidRecordBytes(recordBytes(indexedUp),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawIndexedPrimitiveUP),
                        sizeof(D9CCommandRecordDrawIndexedPrimitiveUP),
                        "draw indexed primitive UP minimum record");

  auto tooSmallIndexedUp = indexedUp;
  tooSmallIndexedUp.header.size = static_cast<std::uint32_t>(
      sizeof(D9CCommandRecordDrawIndexedPrimitiveUP) - 1u);
  checkStatus(recordBytes(tooSmallIndexedUp, tooSmallIndexedUp.header.size),
              D9CCommandRecordValidationStatus::SizeTooSmall,
              "draw indexed primitive UP rejects undersized fixed header");

  auto truncatedIndexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkStatus(recordBytes(truncatedIndexedUp, truncatedIndexedUp.header.size - 1u),
              D9CCommandRecordValidationStatus::TruncatedRecord,
              "draw indexed primitive UP rejects unavailable declared tail");
}

void testSurfaceQueryRecordValidationMatrix() {
  checkExactRecordSizeMatrix(makeQueryIssueRecord(), "QUERY_ISSUE");
  checkExactRecordSizeMatrix(makeColorFillRecord(), "COLOR_FILL");
  checkExactRecordSizeMatrix(makeUpdateSurfaceRecord(), "UPDATE_SURFACE");
  checkExactRecordSizeMatrix(makeUpdateTextureRecord(), "UPDATE_TEXTURE");
  checkExactRecordSizeMatrix(makeStretchRectRecord(), "STRETCH_RECT");
}

void testInvalidTruncatedAndUnknownRecords() {
  std::vector<std::uint8_t> shortHeader(sizeof(D9CCommandRecordHeader) - 1u);
  checkStatus(shortHeader, D9CCommandRecordValidationStatus::TruncatedHeader,
              "short header is rejected");

  D9CCommandRecordHeader tinySize{};
  tinySize.type = D9C_COMMAND_RECORD_PRESENT;
  tinySize.size = sizeof(D9CCommandRecordHeader) - 1u;
  checkStatus(recordBytes(tinySize), D9CCommandRecordValidationStatus::SizeTooSmall,
              "header size smaller than common header is rejected");

  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.rectCount = 1u;
  clear.rectOffset = sizeof(D9CCommandRecordClear);
  clear.header.size = sizeof(D9CCommandRecordClear) + sizeof(D9CRect);
  checkStatus(recordBytes(clear, clear.header.size - 1u),
              D9CCommandRecordValidationStatus::TruncatedRecord,
              "available bytes shorter than header size is rejected");

  D9CCommandRecordHeader unknown{};
  unknown.type = 0xffffu;
  unknown.size = sizeof(unknown);
  checkStatus(recordBytes(unknown), D9CCommandRecordValidationStatus::UnknownType,
              "unknown record type is rejected");
}

void testCommandChunkWireBlobRejectsLegacyRawRecordStream() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  const auto decoded = makeImportedWireChunkBlobView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(!decoded.valid(), "raw record stream is not accepted as a wire blob");
  check(!decoded.wireHeaderCandidate(),
        "raw record stream does not pass DOD wire header identity checks");
  checkEq(static_cast<int>(decoded.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidChunkHeader),
          "raw record stream fails as an invalid chunk header");

  const auto empty = makeImportedWireChunkBlobView(nullptr, 0u);
  checkEq(static_cast<int>(empty.status),
          static_cast<int>(ImportedWireChunkValidationStatus::MissingChunkHeader),
          "empty command chunk blob is rejected before import");
}

void testImportedWireChunkRejectsMalformedPayloadRange() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);
  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  const std::vector<D9CCommandChunkWireRecordHeader> validRecords{
      wireRecordHeader(D9C_COMMAND_RECORD_PRESENT, 0u,
                       static_cast<std::uint32_t>(sizeof(present))),
      wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                       static_cast<std::uint32_t>(sizeof(present)),
                       static_cast<std::uint32_t>(sizeof(draw))),
  };
  std::vector<D9CCommandChunkWireRecordHeader> records = validRecords;
  records[0].payloadOffset = static_cast<std::uint32_t>(bytes.size());
  records[0].payloadSize = 1u;

  auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), nullptr, 0u);
  const auto validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordRange),
          "wire payload range past arena is rejected");
  checkEq(validation.failedRecordIndex, 0u,
          "wire payload range reports failed record index");

  records = validRecords;
  records[0].payloadSize = static_cast<std::uint32_t>(bytes.size());
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), nullptr, 0u);
  const auto oversizedValidation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(oversizedValidation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecord),
          "wire payload range spanning extra record bytes is rejected");
  checkEq(static_cast<int>(oversizedValidation.failedRecord.validation.status),
          static_cast<int>(D9CCommandRecordValidationStatus::SizeMismatch),
          "wire oversized payload preserves legacy record validation status");
}

void testImportedWireChunkEnforcesHandleTableAndRanges() {
  const auto present = makePresentRecord();
  auto bytes = recordBytes(present);
  std::vector<D9CCommandChunkWireHandleEntry> handles{
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1000u),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2000u),
  };
  const std::vector<D9CCommandChunkWireRecordHeader> validRecords{
      wireRecordHeader(D9C_COMMAND_RECORD_PRESENT, 0u,
                       static_cast<std::uint32_t>(sizeof(present)), 0u,
                       static_cast<std::uint32_t>(handles.size())),
  };

  auto records = validRecords;
  records[0].firstHandle = 2u;
  records[0].handleCount = 1u;
  auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  auto validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordRange),
          "wire handle range past table is rejected");
  checkEq(validation.failedRecordIndex, 0u,
          "wire handle range reports failed record index");

  records = validRecords;
  records[0].flags = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordHeader),
          "wire record header rejects unsupported flags");

  records[0].flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE;
  records[0].reserved1 = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordHeader),
          "wire record header rejects nonzero reserved fields");

  records = validRecords;
  handles[1].kind = D9C_CHUNK_HANDLE_KIND_VERTEX_DECL + 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          "wire handle table rejects unknown handle kind");
  checkEq(validation.failedHandleIndex, 1u,
          "wire handle table reports failed handle index");

  handles[1].kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
  handles[1].reserved0 = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          "wire handle table rejects nonzero reserved fields");

  handles[1].reserved0 = 0u;
  handles[1].reserved1 = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          "wire handle table rejects nonzero trailing reserved fields");

  handles[1].reserved1 = 0u;
  // In-range stamped generation must now pass the structural validator —
  // the cross-side equality check moved to chunk replay where the wrapper
  // pointer can be dereferenced. See
  // `device_c_chunk_replay.cpp` "bad-handle-generation".
  handles[1].generation = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::Valid),
          "wire handle table accepts in-range stamped generations");

  // Generations outside the encoded domain (24 bits) are still structurally
  // invalid — a producer stamping 25+ bits of generation has corrupted the
  // encoding and the importer must reject the entry before any cross-side
  // lookup is attempted.
  handles[1].generation =
      D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_MASK + 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          "wire handle table rejects out-of-range stamped generations");
  checkEq(validation.failedHandleIndex, 1u,
          "wire handle table reports failed handle index for "
          "out-of-range generation");
  handles[1].generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE;
}

// Cross-side generation match: the helper used by the chunk-replay
// importer must accept the legacy NONE sentinel, accept stamped values
// that match the resolved core handle's generation, and reject every
// non-matching non-NONE stamp. This is the "zombie handle" gate that
// fires when a PE recorder's wrapper pointer is reused before the
// unix-side import resolves it — without the gate the importer would
// silently dispatch records against a freed resource slot.
void testWireHandleGenerationCrossSideEquality() {
  // Build a `core::Handle.value`-shaped uint64 with explicit generation
  // bits so the test does not depend on HandleArena internals being
  // visible to the bridge layer.
  const std::uint64_t handleWithGen5 =
      (static_cast<std::uint64_t>(5u) <<
       D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_SHIFT) |
      0x42u;
  const std::uint64_t handleWithGen6 =
      (static_cast<std::uint64_t>(6u) <<
       D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_SHIFT) |
      0x42u;

  checkEq(
      d9c_command_chunk_wire_handle_generation_from_handle(handleWithGen5),
      5u, "decoded generation matches encoded high bits");

  check(d9c_command_chunk_wire_handle_generation_matches(
            D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE, handleWithGen5) != 0,
        "legacy NONE sentinel accepts any resolved generation");

  check(d9c_command_chunk_wire_handle_generation_matches(5u, handleWithGen5) !=
            0,
        "stamped generation accepts matching resolved generation");

  check(d9c_command_chunk_wire_handle_generation_matches(6u, handleWithGen5) ==
            0,
        "stamped generation rejects mismatched resolved generation");

  check(d9c_command_chunk_wire_handle_generation_matches(5u, handleWithGen6) ==
            0,
        "decoded generation is sensitive to the high bits of the handle");

  check(d9c_command_chunk_wire_handle_generation_valid(
            D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE) != 0,
        "structural validator accepts NONE sentinel");
  check(d9c_command_chunk_wire_handle_generation_valid(
            D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_MASK) != 0,
        "structural validator accepts the maximum encoded generation");
  check(d9c_command_chunk_wire_handle_generation_valid(
            D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_MASK + 1u) == 0,
        "structural validator rejects generations past the encoded domain");
}

void testImportedWireAcceptsAllCommandIds() {
  const auto draw = makeDrawPrimitiveRecord(0u, 1u);
  checkWireAcceptsRecordBytes(recordBytes(draw),
                              D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                              "DRAW_PRIMITIVE");

  const auto indexed = makeDrawIndexedPrimitiveRecord(0u, 1u);
  checkWireAcceptsRecordBytes(recordBytes(indexed),
                              D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
                              "DRAW_INDEXED_PRIMITIVE");

  const auto drawUp = makeDrawPrimitiveUPRecord(64u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(drawUp),
                              D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                              "DRAW_PRIMITIVE_UP");

  const auto indexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(indexedUp),
                              D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                              "DRAW_INDEXED_PRIMITIVE_UP");

  const auto vsFloat = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_F, 2u, sizeof(float) * 4u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(vsFloat),
                              D9C_COMMAND_RECORD_SET_VS_CONST_F,
                              "SET_VS_CONST_F");

  const auto vsInt = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_I, 2u, sizeof(std::int32_t) * 4u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(vsInt),
                              D9C_COMMAND_RECORD_SET_VS_CONST_I,
                              "SET_VS_CONST_I");

  const auto vsBool = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_B, 3u, sizeof(std::uint32_t));
  checkWireAcceptsRecordBytes(declaredRecordBytes(vsBool),
                              D9C_COMMAND_RECORD_SET_VS_CONST_B,
                              "SET_VS_CONST_B");

  const auto psFloat = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_F, 2u, sizeof(float) * 4u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(psFloat),
                              D9C_COMMAND_RECORD_SET_PS_CONST_F,
                              "SET_PS_CONST_F");

  const auto psInt = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_I, 2u, sizeof(std::int32_t) * 4u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(psInt),
                              D9C_COMMAND_RECORD_SET_PS_CONST_I,
                              "SET_PS_CONST_I");

  const auto psBool = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_B, 3u, sizeof(std::uint32_t));
  checkWireAcceptsRecordBytes(declaredRecordBytes(psBool),
                              D9C_COMMAND_RECORD_SET_PS_CONST_B,
                              "SET_PS_CONST_B");

  const auto clear = makeClearRecord(2u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(clear), D9C_COMMAND_RECORD_CLEAR,
                              "CLEAR");

  const auto present = makePresentRecord();
  checkWireAcceptsRecordBytes(recordBytes(present), D9C_COMMAND_RECORD_PRESENT,
                              "PRESENT");

  const auto stretch = makeStretchRectRecord();
  checkWireAcceptsRecordBytes(recordBytes(stretch),
                              D9C_COMMAND_RECORD_STRETCH_RECT,
                              "STRETCH_RECT");

  const auto color = makeColorFillRecord();
  checkWireAcceptsRecordBytes(recordBytes(color), D9C_COMMAND_RECORD_COLOR_FILL,
                              "COLOR_FILL");

  const auto updateTexture = makeUpdateTextureRecord();
  checkWireAcceptsRecordBytes(recordBytes(updateTexture),
                              D9C_COMMAND_RECORD_UPDATE_TEXTURE,
                              "UPDATE_TEXTURE");

  const auto updateSurface = makeUpdateSurfaceRecord();
  checkWireAcceptsRecordBytes(recordBytes(updateSurface),
                              D9C_COMMAND_RECORD_UPDATE_SURFACE,
                              "UPDATE_SURFACE");

  const auto query = makeQueryIssueRecord();
  checkWireAcceptsRecordBytes(recordBytes(query), D9C_COMMAND_RECORD_QUERY_ISSUE,
                              "QUERY_ISSUE");

  const auto readback = makeReadbackRecord();
  checkWireAcceptsRecordBytes(recordBytes(readback), D9C_COMMAND_RECORD_READBACK,
                              "READBACK");

  const auto apply = makeApplyStateRecord();
  checkWireAcceptsRecordBytes(recordBytes(apply), D9C_COMMAND_RECORD_APPLY_STATE,
                              "APPLY_STATE");
}

void testImportedWireSpecialRecordValidationMatrix() {
  const auto drawUp = makeDrawPrimitiveUPRecord(64u);
  checkWireRecordMatrix(declaredRecordBytes(drawUp),
                        D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                        "DRAW_PRIMITIVE_UP");

  const auto indexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkWireRecordMatrix(declaredRecordBytes(indexedUp),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                        "DRAW_INDEXED_PRIMITIVE_UP");

  const auto query = makeQueryIssueRecord();
  checkWireRecordMatrix(recordBytes(query), D9C_COMMAND_RECORD_QUERY_ISSUE,
                        "QUERY_ISSUE");

  const auto color = makeColorFillRecord();
  checkWireRecordMatrix(recordBytes(color), D9C_COMMAND_RECORD_COLOR_FILL,
                        "COLOR_FILL");

  const auto updateSurface = makeUpdateSurfaceRecord();
  checkWireRecordMatrix(recordBytes(updateSurface),
                        D9C_COMMAND_RECORD_UPDATE_SURFACE,
                        "UPDATE_SURFACE");

  const auto updateTexture = makeUpdateTextureRecord();
  checkWireRecordMatrix(recordBytes(updateTexture),
                        D9C_COMMAND_RECORD_UPDATE_TEXTURE,
                        "UPDATE_TEXTURE");

  const auto stretch = makeStretchRectRecord();
  checkWireRecordMatrix(recordBytes(stretch),
                        D9C_COMMAND_RECORD_STRETCH_RECT,
                        "STRETCH_RECT");
}

}  // namespace

int main() {
  try {
    testFixedRecordValidation();
    testClearRectTailValidation();
    testSetConstTailValidation();
    testAllCommandIdsValidateWithExpectedRecordShapes();
    testDrawUpValidationMatrix();
    testSurfaceQueryRecordValidationMatrix();
    testInvalidTruncatedAndUnknownRecords();
    testCommandChunkWireBlobRejectsLegacyRawRecordStream();
    testImportedWireChunkRejectsMalformedPayloadRange();
    testImportedWireChunkEnforcesHandleTableAndRanges();
    testWireHandleGenerationCrossSideEquality();
    testImportedWireAcceptsAllCommandIds();
    testImportedWireSpecialRecordValidationMatrix();
  } catch (const dxmt9::d3d9::devicec::spec::TestFailure& e) {
    std::cerr << "chunk_record_validation_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
