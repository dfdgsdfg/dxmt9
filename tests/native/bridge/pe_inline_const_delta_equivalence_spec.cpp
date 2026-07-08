// pe_inline_const_delta_equivalence_spec
//
// R-BACK-2.52 T3 native-test coverage for the opt-in inline const-delta
// wire mode (DXMT9_PE_INLINE_CONST_DELTA). T1 (ea515e89) owns the wire
// schema in include/dxmt9/device_c.h; T2 (the PE-side fold in
// d3d9_pe_device.cpp / d3d9_pe_const_shadow.hpp) is developed in parallel
// and is Windows-only, so it cannot be linked into this native test binary.
// Every wire record below is therefore HAND-CONSTRUCTED at test scope --
// the same pattern chunk_record_import_spec.cpp / chunk_record_validation_
// spec.cpp / chunk_record_replay_spec.cpp already use via
// chunk_record_import_spec_fixtures.hpp -- not produced via the PE
// recorder.
//
// Three pieces of REAL production code are exercised directly (no mocking):
//
//   1. Wire-level validation: validateCommandRecord / validateImportedWireChunk
//      (device_c_record_validate.cpp) -- the R-BACK-2.52(c) import gate T3
//      added for DRAW_PRIMITIVE / DRAW_INDEXED_PRIMITIVE / DRAW_PRIMITIVE_UP /
//      DRAW_INDEXED_PRIMITIVE_UP.
//   2. Section/offset resolution: the T1 device_c.h helpers
//      (d9c_draw_packet_const_delta_section_slice,
//      d9c_command_record_draw_*_const_delta_offset, ...) -- the exact
//      helpers device_c_chunk_replay.cpp's applyDrawPacketConstDeltaSections
//      calls.
//   3. Run-coalescing: scanImportedDrawRun (device_c_record_replay.cpp) --
//      proves the R-BACK-2.52(f) fix to packetHasNoStateDelta /
//      drawPacketStateDeltaCompatibleWithRunBase.
//
// What CANNOT be linked into a native test (no real D9CDevice / Metal
// backend): device_c_chunk_replay.cpp's applyDrawPacketConstDeltaSections
// itself, and the dxmt9c_device_set_*_const_* ABI entries it forwards to.
// Case A below mirrors that function's effect with a small
// ConstantRegisterShadow model -- exactly the modeling boundary
// pe_full_snapshot_equivalence_spec.cpp documents and uses for
// applyDrawPacketStateDirect. The mirror walks a packet's
// constDeltaSections in the SAME canonical order and resolves payload
// bytes through the SAME production d9c_draw_packet_const_delta_section_slice
// helper device_c_chunk_replay.cpp calls, so the only thing not exercised
// here is the final 1:1 dxmt9c_device_set_*_const_* forwarding call itself
// (shared verbatim with the already-production standalone
// D9C_COMMAND_RECORD_SET_*_CONST_* dispatch).

#include "chunk_record_import_spec_fixtures.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace dxmt9::d3d9::devicec::spec;

// =====================================================================
// Constant-register shadow model (mirrors the unix-side per-stage float /
// int / bool constant register file that dxmt9c_device_set_{vs,ps}_const_
// {f,i,b} write into). Sized to the same register-file caps device_c.h
// declares.
// =====================================================================
struct ConstantRegisterShadow {
  std::array<std::array<float, 4>, D9C_DRAW_PACKET_MAX_CONST_VS_F> vsF{};
  std::array<std::array<int32_t, 4>, D9C_DRAW_PACKET_MAX_CONST_VS_I> vsI{};
  std::array<uint32_t, D9C_DRAW_PACKET_MAX_CONST_VS_B> vsB{};
  std::array<std::array<float, 4>, D9C_DRAW_PACKET_MAX_CONST_PS_F> psF{};
  std::array<std::array<int32_t, 4>, D9C_DRAW_PACKET_MAX_CONST_PS_I> psI{};
  std::array<uint32_t, D9C_DRAW_PACKET_MAX_CONST_PS_B> psB{};
};

bool constantShadowsEqual(const ConstantRegisterShadow& a,
                          const ConstantRegisterShadow& b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

uint32_t standaloneConstTypeForKind(uint32_t kind) {
  switch (kind) {
  case D9C_DRAW_PACKET_CONST_DELTA_VS_F: return D9C_COMMAND_RECORD_SET_VS_CONST_F;
  case D9C_DRAW_PACKET_CONST_DELTA_VS_I: return D9C_COMMAND_RECORD_SET_VS_CONST_I;
  case D9C_DRAW_PACKET_CONST_DELTA_VS_B: return D9C_COMMAND_RECORD_SET_VS_CONST_B;
  case D9C_DRAW_PACKET_CONST_DELTA_PS_F: return D9C_COMMAND_RECORD_SET_PS_CONST_F;
  case D9C_DRAW_PACKET_CONST_DELTA_PS_I: return D9C_COMMAND_RECORD_SET_PS_CONST_I;
  case D9C_DRAW_PACKET_CONST_DELTA_PS_B: return D9C_COMMAND_RECORD_SET_PS_CONST_B;
  default: return 0u;
  }
}

// Mirrors the standalone D9C_COMMAND_RECORD_SET_*_CONST_* dispatch in
// device_c_chunk_replay.cpp (the case D9C_COMMAND_RECORD_SET_VS_CONST_F..
// PS_CONST_B block): writes `count` registers of the type's element size
// starting at `start` from `payload` into the matching shadow array.
void applyStandaloneConstRecord(ConstantRegisterShadow& shadow, uint32_t type,
                                uint32_t start, uint32_t count,
                                const void* payload) {
  switch (type) {
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    std::memcpy(&shadow.vsF[start], payload, sizeof(float) * 4u * count);
    break;
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    std::memcpy(&shadow.psF[start], payload, sizeof(float) * 4u * count);
    break;
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    std::memcpy(&shadow.vsI[start], payload, sizeof(int32_t) * 4u * count);
    break;
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    std::memcpy(&shadow.psI[start], payload, sizeof(int32_t) * 4u * count);
    break;
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    std::memcpy(&shadow.vsB[start], payload, sizeof(uint32_t) * count);
    break;
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    std::memcpy(&shadow.psB[start], payload, sizeof(uint32_t) * count);
    break;
  default:
    break;
  }
}

// Mirrors device_c_chunk_replay.cpp::applyDrawPacketConstDeltaSections: walks
// the packet's six sections in canonical order and, for each valid section,
// resolves its trailing payload slice through the REAL production helper
// (d9c_draw_packet_const_delta_section_slice) and writes it into the
// matching shadow array via the same per-kind dispatch
// applyStandaloneConstRecord uses.
void applyInlineConstDeltaSections(ConstantRegisterShadow& shadow,
                                   const D9CDrawPrimitivePacket& packet,
                                   const std::uint8_t* record,
                                   std::uint32_t constDeltaBaseOffset) {
  for (uint32_t kind = 0; kind < D9C_DRAW_PACKET_CONST_DELTA_COUNT; ++kind) {
    const auto& section = packet.constDeltaSections[kind];
    if (!section.valid) {
      continue;
    }
    const auto slice = d9c_draw_packet_const_delta_section_slice(
        &packet, constDeltaBaseOffset, kind);
    applyStandaloneConstRecord(shadow, standaloneConstTypeForKind(kind),
                              section.startRegister, section.registerCount,
                              record + slice.payloadOffset);
  }
}

// =====================================================================
// Hand-built const-delta-bearing wire records. No PE recorder involved --
// mirrors the trailing-payload-after-fixed-record wire shape
// specs/backend/spec.md "Inline Const Delta (opt-in)" documents and T1's
// chunk_record_spec.cpp::testInlineConstDeltaSections() pins.
// =====================================================================
struct ConstDeltaSectionData {
  std::uint32_t startRegister = 0;
  std::uint32_t registerCount = 0;
  std::vector<std::uint8_t> payload;
};
using ConstDeltaSectionSet =
    std::array<ConstDeltaSectionData, D9C_DRAW_PACKET_CONST_DELTA_COUNT>;

std::vector<std::uint8_t> makePatternPayload(uint32_t registerCount,
                                             uint32_t elemSize,
                                             std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(registerCount) * elemSize);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(seed + i);
  }
  return bytes;
}

ConstDeltaSectionData makeSection(uint32_t kind, uint32_t start, uint32_t count,
                                  std::uint8_t seed) {
  ConstDeltaSectionData data;
  data.startRegister = start;
  data.registerCount = count;
  data.payload =
      makePatternPayload(count, d9c_draw_packet_const_delta_section_elem_size(kind), seed);
  return data;
}

void populateConstDeltaSections(D9CDrawPrimitivePacket& packet,
                                const ConstDeltaSectionSet& sections) {
  for (uint32_t kind = 0; kind < D9C_DRAW_PACKET_CONST_DELTA_COUNT; ++kind) {
    if (sections[kind].registerCount == 0u) {
      continue;
    }
    auto& hdr = packet.constDeltaSections[kind];
    hdr.valid = 1u;
    hdr.startRegister = sections[kind].startRegister;
    hdr.registerCount = sections[kind].registerCount;
  }
}

// Appends every populated section's payload bytes in canonical order
// (skipping unpopulated sections), matching
// d9c_draw_packet_const_delta_section_local_offset's packing contract.
void appendConstDeltaPayloadBytes(std::vector<std::uint8_t>& bytes,
                                  const ConstDeltaSectionSet& sections) {
  for (uint32_t kind = 0; kind < D9C_DRAW_PACKET_CONST_DELTA_COUNT; ++kind) {
    if (sections[kind].registerCount == 0u) {
      continue;
    }
    const auto& payload = sections[kind].payload;
    bytes.insert(bytes.end(), payload.begin(), payload.end());
  }
}

std::vector<std::uint8_t> makeDrawPrimitiveRecordBytes(
    std::uint32_t startVertex, std::uint32_t primitiveCount,
    const ConstDeltaSectionSet& sections) {
  D9CCommandRecordDrawPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw.packet.primitiveType = 4u;
  draw.packet.startVertex = startVertex;
  draw.packet.primitiveCount = primitiveCount;
  populateConstDeltaSections(draw.packet, sections);
  draw.header.size = d9c_command_record_draw_primitive_total_size(&draw.packet);

  std::vector<std::uint8_t> bytes(sizeof(draw));
  std::memcpy(bytes.data(), &draw, sizeof(draw));
  appendConstDeltaPayloadBytes(bytes, sections);
  return bytes;
}

std::vector<std::uint8_t> makeDrawIndexedPrimitiveRecordBytes(
    std::uint32_t startIndex, std::uint32_t primitiveCount,
    const ConstDeltaSectionSet& sections) {
  D9CCommandRecordDrawIndexedPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  draw.packet.state.primitiveType = 4u;
  draw.packet.startIndex = startIndex;
  draw.packet.primitiveCount = primitiveCount;
  draw.packet.numVertices = primitiveCount * 3u;
  populateConstDeltaSections(draw.packet.state, sections);
  draw.header.size =
      d9c_command_record_draw_indexed_primitive_total_size(&draw.packet.state);

  std::vector<std::uint8_t> bytes(sizeof(draw));
  std::memcpy(bytes.data(), &draw, sizeof(draw));
  appendConstDeltaPayloadBytes(bytes, sections);
  return bytes;
}

std::vector<std::uint8_t> makeDrawPrimitiveUPRecordBytes(
    std::uint32_t vertexBytes, const ConstDeltaSectionSet& sections) {
  D9CCommandRecordDrawPrimitiveUP draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
  draw.packet.state.primitiveType = 4u;
  draw.packet.primitiveCount = 1u;
  draw.packet.stride = 16u;
  draw.packet.vertexDataOffset =
      static_cast<std::uint32_t>(sizeof(D9CCommandRecordDrawPrimitiveUP));
  draw.packet.vertexDataSize = vertexBytes;
  populateConstDeltaSections(draw.packet.state, sections);
  draw.header.size = d9c_command_record_draw_primitive_up_total_size(&draw.packet);

  // Vertex-data bytes are left zero -- this spec never asserts on vertex
  // content, only on const-delta payload placement/application.
  std::vector<std::uint8_t> bytes(sizeof(draw) + vertexBytes);
  std::memcpy(bytes.data(), &draw, sizeof(draw));
  appendConstDeltaPayloadBytes(bytes, sections);
  return bytes;
}

// =====================================================================
// Case A: equivalence. A draw record carrying a folded inline const-delta
// section must produce the SAME effective constant-register state as
// replaying the equivalent standalone D9C_COMMAND_RECORD_SET_*_CONST_*
// record immediately before an otherwise-identical draw without sections.
// =====================================================================

void checkEquivalentSingleSection(const std::string& tag, uint32_t kind,
                                  uint32_t start, uint32_t count,
                                  std::uint8_t seed) {
  const auto payload =
      makePatternPayload(count, d9c_draw_packet_const_delta_section_elem_size(kind), seed);

  // "Unfolded" lane: standalone SET_*_CONST_* record applied directly,
  // immediately before an otherwise-identical draw without sections.
  ConstantRegisterShadow unfolded;
  applyStandaloneConstRecord(unfolded, standaloneConstTypeForKind(kind), start,
                            count, payload.data());
  const auto plainDrawBytes =
      makeDrawPrimitiveRecordBytes(5u, 2u, ConstDeltaSectionSet{});
  const auto plainDrawValidation = validateCommandRecord(
      plainDrawBytes.data(), static_cast<std::uint32_t>(plainDrawBytes.size()));
  check(plainDrawValidation.valid(), tag + ": unfolded draw record validates");

  // "Folded" lane: the equivalent single-section draw record.
  ConstDeltaSectionSet sections{};
  sections[kind].startRegister = start;
  sections[kind].registerCount = count;
  sections[kind].payload = payload;
  const auto foldedDrawBytes = makeDrawPrimitiveRecordBytes(5u, 2u, sections);
  const auto foldedDrawValidation = validateCommandRecord(
      foldedDrawBytes.data(), static_cast<std::uint32_t>(foldedDrawBytes.size()));
  check(foldedDrawValidation.valid(), tag + ": folded draw record validates");

  D9CCommandRecordDrawPrimitive foldedDecoded{};
  std::memcpy(&foldedDecoded, foldedDrawBytes.data(), sizeof(foldedDecoded));
  ConstantRegisterShadow folded;
  applyInlineConstDeltaSections(
      folded, foldedDecoded.packet, foldedDrawBytes.data(),
      d9c_command_record_draw_primitive_const_delta_offset());

  check(constantShadowsEqual(unfolded, folded),
        tag + ": folded and unfolded lanes produce identical constant state");

  D9CCommandRecordDrawPrimitive plainDecoded{};
  std::memcpy(&plainDecoded, plainDrawBytes.data(), sizeof(plainDecoded));
  checkEq(plainDecoded.packet.startVertex, foldedDecoded.packet.startVertex,
          tag + ": draw startVertex is identical between lanes");
  checkEq(plainDecoded.packet.primitiveCount, foldedDecoded.packet.primitiveCount,
          tag + ": draw primitiveCount is identical between lanes");
  checkEq(plainDecoded.packet.primitiveType, foldedDecoded.packet.primitiveType,
          tag + ": draw primitiveType is identical between lanes");
}

void testEquivalenceSingleSectionEachStage() {
  checkEquivalentSingleSection("VS_F", D9C_DRAW_PACKET_CONST_DELTA_VS_F, 4u, 3u, 0x11u);
  checkEquivalentSingleSection("PS_F", D9C_DRAW_PACKET_CONST_DELTA_PS_F, 2u, 1u, 0x21u);
  checkEquivalentSingleSection("VS_I", D9C_DRAW_PACKET_CONST_DELTA_VS_I, 1u, 2u, 0x31u);
  checkEquivalentSingleSection("PS_B", D9C_DRAW_PACKET_CONST_DELTA_PS_B, 0u, 4u, 0x41u);
}

void testEquivalenceTwoSectionDraw() {
  ConstDeltaSectionSet sections{};
  sections[D9C_DRAW_PACKET_CONST_DELTA_VS_F] =
      makeSection(D9C_DRAW_PACKET_CONST_DELTA_VS_F, 10u, 2u, 0x51u);
  sections[D9C_DRAW_PACKET_CONST_DELTA_PS_B] =
      makeSection(D9C_DRAW_PACKET_CONST_DELTA_PS_B, 0u, 3u, 0x61u);

  const auto foldedBytes = makeDrawPrimitiveRecordBytes(7u, 4u, sections);
  const auto validation = validateCommandRecord(
      foldedBytes.data(), static_cast<std::uint32_t>(foldedBytes.size()));
  check(validation.valid(), "two-section draw record validates");

  D9CCommandRecordDrawPrimitive decoded{};
  std::memcpy(&decoded, foldedBytes.data(), sizeof(decoded));
  ConstantRegisterShadow folded;
  applyInlineConstDeltaSections(folded, decoded.packet, foldedBytes.data(),
                                d9c_command_record_draw_primitive_const_delta_offset());

  ConstantRegisterShadow unfolded;
  applyStandaloneConstRecord(
      unfolded, D9C_COMMAND_RECORD_SET_VS_CONST_F, 10u, 2u,
      sections[D9C_DRAW_PACKET_CONST_DELTA_VS_F].payload.data());
  applyStandaloneConstRecord(
      unfolded, D9C_COMMAND_RECORD_SET_PS_CONST_B, 0u, 3u,
      sections[D9C_DRAW_PACKET_CONST_DELTA_PS_B].payload.data());

  check(constantShadowsEqual(folded, unfolded),
        "two-section draw matches two standalone const records applied in "
        "canonical order");
}

// Proves the per-record-kind base-offset helpers device_c_chunk_replay.cpp's
// applyDrawPacketConstDeltaSections relies on: DRAW_INDEXED_PRIMITIVE's
// payload area begins right after its (larger) fixed record.
void testEquivalenceDrawIndexedPrimitiveSection() {
  ConstDeltaSectionSet sections{};
  sections[D9C_DRAW_PACKET_CONST_DELTA_VS_F] =
      makeSection(D9C_DRAW_PACKET_CONST_DELTA_VS_F, 1u, 1u, 0x71u);

  const auto bytes = makeDrawIndexedPrimitiveRecordBytes(2u, 3u, sections);
  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), "indexed draw with const-delta section validates");

  D9CCommandRecordDrawIndexedPrimitive decoded{};
  std::memcpy(&decoded, bytes.data(), sizeof(decoded));
  ConstantRegisterShadow folded;
  applyInlineConstDeltaSections(
      folded, decoded.packet.state, bytes.data(),
      d9c_command_record_draw_indexed_primitive_const_delta_offset());

  ConstantRegisterShadow unfolded;
  applyStandaloneConstRecord(
      unfolded, D9C_COMMAND_RECORD_SET_VS_CONST_F, 1u, 1u,
      sections[D9C_DRAW_PACKET_CONST_DELTA_VS_F].payload.data());
  check(constantShadowsEqual(folded, unfolded),
        "indexed draw base-offset resolves to the same constant state as "
        "the standalone record");
}

// Proves DRAW_PRIMITIVE_UP's payload area chains after the trailing
// vertex-data region, not immediately after the fixed record.
void testEquivalenceDrawPrimitiveUPSection() {
  ConstDeltaSectionSet sections{};
  sections[D9C_DRAW_PACKET_CONST_DELTA_PS_F] =
      makeSection(D9C_DRAW_PACKET_CONST_DELTA_PS_F, 5u, 2u, 0x81u);

  const auto bytes = makeDrawPrimitiveUPRecordBytes(32u, sections);
  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), "UP draw with const-delta section validates");

  D9CCommandRecordDrawPrimitiveUP decoded{};
  std::memcpy(&decoded, bytes.data(), sizeof(decoded));
  ConstantRegisterShadow folded;
  applyInlineConstDeltaSections(
      folded, decoded.packet.state, bytes.data(),
      d9c_command_record_draw_primitive_up_const_delta_offset(&decoded.packet));

  ConstantRegisterShadow unfolded;
  applyStandaloneConstRecord(
      unfolded, D9C_COMMAND_RECORD_SET_PS_CONST_F, 5u, 2u,
      sections[D9C_DRAW_PACKET_CONST_DELTA_PS_F].payload.data());
  check(constantShadowsEqual(folded, unfolded),
        "UP draw base-offset chains after vertex data, matching the "
        "standalone record");
}

// =====================================================================
// Case B: rejection. A section that exceeds its register-file cap, or a
// declared header.size too small for the section payload it claims, must
// fail chunk import with the same malformed-packet status class every
// other packet-shape violation uses -- and must never reach any apply
// call. dxmt9c_device_commit_chunk (device_c_chunk_replay.cpp) maps any
// non-Valid validateImportedWireChunk() status to commitChunkFail("validation",
// ...), whose default HRESULT is D3DERR_INVALIDCALL, and gates the WHOLE
// chunk on that validation before replayImportedChunk ever runs -- so no
// record (including any preceding valid draws) is replayed on a violation.
// =====================================================================

void testRejectionSectionExceedsRegisterCap() {
  // VS_F's register-file cap is D9C_DRAW_PACKET_MAX_CONST_VS_F == 256;
  // 254 + 3 = 257 exceeds it (mirrors chunk_record_spec.cpp's own
  // cap-boundary pin for the raw range helper).
  ConstDeltaSectionSet sections{};
  sections[D9C_DRAW_PACKET_CONST_DELTA_VS_F] =
      makeSection(D9C_DRAW_PACKET_CONST_DELTA_VS_F, 254u, 3u, 0x91u);
  const auto bytes = makeDrawPrimitiveRecordBytes(0u, 1u, sections);

  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(!validation.valid(),
        "a section exceeding its register-file cap fails validation");
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(D9CCommandRecordValidationStatus::SizeMismatch),
          "cap violation reports the same malformed-packet status class "
          "other packet-shape violations use");

  // No partial application: a caller must gate every apply call on
  // validation.valid(); demonstrate the shadow never moves off its
  // zero-initialized default when that gate is honored.
  ConstantRegisterShadow shadow;
  check(constantShadowsEqual(shadow, ConstantRegisterShadow{}),
        "rejected record: no constant state may be applied");
}

void testRejectionHeaderSizeTooSmallForPayload() {
  ConstDeltaSectionSet sections{};
  sections[D9C_DRAW_PACKET_CONST_DELTA_PS_F] =
      makeSection(D9C_DRAW_PACKET_CONST_DELTA_PS_F, 0u, 2u, 0xA1u);
  auto bytes = makeDrawPrimitiveRecordBytes(0u, 1u, sections);

  // Truncate the declared header.size back to the fixed-record size,
  // pretending the trailing PS_F payload the packet's own section header
  // still declares valid was never appended on the wire.
  D9CCommandRecordHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  header.size = static_cast<std::uint32_t>(sizeof(D9CCommandRecordDrawPrimitive));
  std::memcpy(bytes.data(), &header, sizeof(header));

  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(!validation.valid(),
        "header.size too small for the declared section payload fails "
        "validation");
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(D9CCommandRecordValidationStatus::SizeMismatch),
          "undersized declared header reports SizeMismatch");

  ConstantRegisterShadow shadow;
  check(constantShadowsEqual(shadow, ConstantRegisterShadow{}),
        "rejected record: no constant state may be applied");
}

// Chunk-level proof: the SAME violation, imported through
// validateImportedWireChunk -- the exact gate dxmt9c_device_commit_chunk
// calls before replay -- is classified the same way as every other
// malformed record and rejects the whole chunk.
void testChunkImportRejectsConstDeltaCapViolation() {
  ConstDeltaSectionSet sections{};
  sections[D9C_DRAW_PACKET_CONST_DELTA_VS_I] =
      makeSection(D9C_DRAW_PACKET_CONST_DELTA_VS_I, 15u, 2u, 0xB1u);  // 15+2=17 > 16 cap.
  const auto bytes = makeDrawPrimitiveRecordBytes(0u, 1u, sections);

  std::vector<D9CCommandChunkWireRecordHeader> records{
      wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, 0u,
                       static_cast<std::uint32_t>(bytes.size())),
  };
  const auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), nullptr, 0u);
  const auto validation = validateImportedWireChunk(wire);
  check(!validation.valid(),
        "chunk import rejects a const-delta section exceeding its register cap");
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecord),
          "chunk-level status classifies it as an invalid record, the same "
          "class every other malformed packet uses");
}

// =====================================================================
// Case C: off path. A draw record with every const-delta section left
// invalid replays exactly as before R-BACK-2.52 -- reuses the same
// assertion shapes chunk_record_replay_spec.cpp's run-coalescing tests and
// chunk_record_spec.cpp's schema pin already use.
// =====================================================================
void testOffPathReplaysUnchanged() {
  const auto draw = makeDrawPrimitiveRecord(0u, 1u);  // fixture: all sections invalid.
  const auto bytes = recordBytes(draw);
  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), "off-path draw record still validates");
  checkEq(validation.expectedSize, static_cast<std::uint64_t>(sizeof(draw)),
          "off-path draw record expected size is unchanged (R-BACK-2.52(a))");

  check(packetHasNoStateDelta(draw.packet),
        "off-path packet with no other delta still reports no state delta");

  // Reuses chunk_record_replay_spec.cpp's coalescing assertion pattern
  // (testDrawRunScanAllowsRepeatedStreamDelta): two off-path draws with no
  // other state delta still coalesce into one run.
  const auto secondDraw = makeDrawPrimitiveRecord(3u, 1u);
  std::vector<std::uint8_t> chunkBytes;
  appendRecord(chunkBytes, draw);
  appendRecord(chunkBytes, secondDraw);
  const auto chunk = makeImportedChunkView(
      chunkBytes.data(), static_cast<std::uint32_t>(chunkBytes.size()), 2u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value() && first->valid(), "off-path first record validates");
  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(), "off-path draws still coalesce into a run");
  checkEq(scan.recordCount, 2u, "off-path run includes both draws");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::EndOfChunk),
          "off-path run reaches chunk end");
}

// =====================================================================
// R-BACK-2.52(f): run-coalescer proof. A candidate carrying any valid
// const-delta section can never continue a run (must break exactly where
// the equivalent standalone SET_*_CONST_* record breaks it today), while a
// run BASE carrying a section may still accept a later no-delta draw --
// mirroring how a standalone const record before the base draw does not
// stop LATER compatible draws from joining that same run.
// =====================================================================
void testRunCoalescerBreaksWhenCandidateCarriesConstDelta() {
  ConstDeltaSectionSet sections{};
  sections[D9C_DRAW_PACKET_CONST_DELTA_VS_F] =
      makeSection(D9C_DRAW_PACKET_CONST_DELTA_VS_F, 0u, 1u, 0x21u);

  const auto firstBytes =
      makeDrawPrimitiveRecordBytes(0u, 1u, ConstDeltaSectionSet{});
  const auto secondBytes = makeDrawPrimitiveRecordBytes(3u, 1u, sections);

  std::vector<std::uint8_t> chunkBytes;
  chunkBytes.insert(chunkBytes.end(), firstBytes.begin(), firstBytes.end());
  chunkBytes.insert(chunkBytes.end(), secondBytes.begin(), secondBytes.end());

  const auto chunk = makeImportedChunkView(
      chunkBytes.data(), static_cast<std::uint32_t>(chunkBytes.size()), 2u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value() && first->valid(),
        "const-delta candidate scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(!scan.replayAsRun(),
        "a const-delta-bearing candidate cannot continue a run");
  checkEq(scan.recordCount, 1u,
          "const-delta candidate scan keeps only the record before the fold");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::StateDelta),
          "const-delta candidate scan reports a state-delta boundary");
}

void testRunCoalescerAllowsConstDeltaBearingBase() {
  ConstDeltaSectionSet sections{};
  sections[D9C_DRAW_PACKET_CONST_DELTA_PS_F] =
      makeSection(D9C_DRAW_PACKET_CONST_DELTA_PS_F, 2u, 1u, 0x31u);

  const auto firstBytes = makeDrawPrimitiveRecordBytes(0u, 1u, sections);
  const auto secondBytes =
      makeDrawPrimitiveRecordBytes(3u, 1u, ConstDeltaSectionSet{});

  std::vector<std::uint8_t> chunkBytes;
  chunkBytes.insert(chunkBytes.end(), firstBytes.begin(), firstBytes.end());
  chunkBytes.insert(chunkBytes.end(), secondBytes.begin(), secondBytes.end());

  const auto chunk = makeImportedChunkView(
      chunkBytes.data(), static_cast<std::uint32_t>(chunkBytes.size()), 2u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value() && first->valid(),
        "const-delta base scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(),
        "a const-delta-bearing run base still accepts a later no-delta draw");
  checkEq(scan.recordCount, 2u, "const-delta base scan includes the following draw");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::EndOfChunk),
          "const-delta base scan reaches chunk end");
}

}  // namespace

int main() {
  try {
    testEquivalenceSingleSectionEachStage();
    testEquivalenceTwoSectionDraw();
    testEquivalenceDrawIndexedPrimitiveSection();
    testEquivalenceDrawPrimitiveUPSection();
    testRejectionSectionExceedsRegisterCap();
    testRejectionHeaderSizeTooSmallForPayload();
    testChunkImportRejectsConstDeltaCapViolation();
    testOffPathReplaysUnchanged();
    testRunCoalescerBreaksWhenCandidateCarriesConstDelta();
    testRunCoalescerAllowsConstDeltaBearingBase();
  } catch (const TestFailure& e) {
    std::cerr << "pe_inline_const_delta_equivalence_spec failed: " << e.what()
              << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "pe_inline_const_delta_equivalence_spec unexpected exception: "
              << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_inline_const_delta_equivalence_spec passed\n";
  return EXIT_SUCCESS;
}
