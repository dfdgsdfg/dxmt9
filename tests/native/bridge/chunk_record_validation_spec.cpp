#include "device_c_chunk_validate.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using dxmt9::d3d9::ImportedChunkView;
using dxmt9::d3d9::CommandChunkEnvelope;
using dxmt9::d3d9::CommandChunkValidationScratch;
using dxmt9::d3d9::CommandChunkValidationStatus;
using dxmt9::d3d9::importPrevalidatedCommandChunk;
using dxmt9::d3d9::validateSegmentedCommandChunk;
using dxmt9::d3d9::validateSegmentedCommandChunkEnvelope;
using dxmt9::d3d9::validateCommandChunk;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

template <typename T>
T& at(std::vector<std::byte>& bytes, std::size_t offset) {
  return *reinterpret_cast<T*>(bytes.data() + offset);
}

struct RecordSpec {
  std::uint32_t type = 0u;
  std::vector<std::byte> payload;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
};

struct ChunkFixture {
  std::vector<std::byte> bytes;
  CommandChunkEnvelope envelope{};

  D9CCommandChunkWireHeader& header() {
    return at<D9CCommandChunkWireHeader>(bytes, 0u);
  }

  const D9CCommandChunkWireHeader& header() const {
    return *reinterpret_cast<const D9CCommandChunkWireHeader*>(bytes.data());
  }

  D9CCommandChunkWireRecordHeader& record(std::size_t index) {
    return at<D9CCommandChunkWireRecordHeader>(
        bytes, header().recordTableOffset +
                   index * sizeof(D9CCommandChunkWireRecordHeader));
  }

  D9CCommandChunkWireHandleEntry& handle(std::size_t index) {
    return at<D9CCommandChunkWireHandleEntry>(
        bytes, header().handleTableOffset +
                   index * sizeof(D9CCommandChunkWireHandleEntry));
  }

  std::byte& payloadByte(std::size_t offset) {
    return bytes[header().payloadArenaOffset + offset];
  }

  auto validate(ImportedChunkView* view = nullptr) const {
    return validateCommandChunk(bytes, envelope, view);
  }
};

ChunkFixture makeChunk(std::span<const RecordSpec> specs) {
  ChunkFixture fixture;
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  std::vector<std::byte> payload;
  records.reserve(specs.size());

  for (const auto& spec : specs) {
    const auto* rule = dxmt9::d3d9::recordRule(spec.type);
    check(rule != nullptr, "fixture record type exists in canonical schema");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back(D9CCommandChunkWireRecordHeader{
        .type = spec.type,
        .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
        .reserved0 = 0u,
        .reserved1 = 0u,
    });
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
  }

  D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleTableOffset = 0u,
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = 0u,
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  header.handleTableOffset = static_cast<std::uint32_t>(alignUp(
      header.recordTableOffset + records.size() * sizeof(records[0]),
      alignof(D9CCommandChunkWireHandleEntry)));
  header.payloadArenaOffset = static_cast<std::uint32_t>(alignUp(
      header.handleTableOffset + handles.size() * sizeof(handles[0]),
      alignof(std::uint32_t)));
  fixture.bytes.resize(header.payloadArenaOffset + payload.size());
  std::memcpy(fixture.bytes.data(), &header, sizeof(header));
  if (!records.empty()) {
    std::memcpy(fixture.bytes.data() + header.recordTableOffset, records.data(),
                records.size() * sizeof(records[0]));
  }
  if (!handles.empty()) {
    std::memcpy(fixture.bytes.data() + header.handleTableOffset, handles.data(),
                handles.size() * sizeof(handles[0]));
  }
  if (!payload.empty()) {
    std::memcpy(fixture.bytes.data() + header.payloadArenaOffset,
                payload.data(), payload.size());
  }
  fixture.envelope = CommandChunkEnvelope{
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
  };
  return fixture;
}

ChunkFixture makeChunk(const RecordSpec& spec) {
  return makeChunk(std::span<const RecordSpec>(&spec, 1u));
}

D9CCommandChunkWireHandleEntry handle(std::uint32_t kind,
                                        std::uint64_t id,
                                        std::uint32_t generation = 1u) {
  return D9CCommandChunkWireHandleEntry{
      .kind = kind,
      .generation = generation,
      .objectId = id,
  };
}

struct SectionSpec {
  std::uint16_t kind = 0u;
  std::uint32_t count = 0u;
  std::vector<std::byte> bytes;
};

std::vector<std::byte> makeDrawPayload(
    D9CCommandChunkWireDrawHeader draw,
    std::span<const SectionSpec> sectionSpecs) {
  draw.sectionCount = static_cast<std::uint32_t>(sectionSpecs.size());
  draw.sectionTableOffset = sizeof(draw);
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sectionSpecs.size() *
                         sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t)));
  std::vector<std::byte> payload(draw.sectionPayloadOffset);
  std::vector<D9CCommandChunkWireSectionDesc> descriptors;
  descriptors.reserve(sectionSpecs.size());
  for (const auto& section : sectionSpecs) {
    const auto* rule = dxmt9::d3d9::sectionRule(section.kind);
    check(rule != nullptr, "fixture section kind exists in canonical schema");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    descriptors.push_back(D9CCommandChunkWireSectionDesc{
        .kind = section.kind,
        .elementSize = rule->elementSize,
        .count = section.count,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .byteSize = static_cast<std::uint32_t>(section.bytes.size()),
    });
    payload.insert(payload.end(), section.bytes.begin(), section.bytes.end());
  }
  std::memcpy(payload.data(), &draw, sizeof(draw));
  if (!descriptors.empty()) {
    std::memcpy(payload.data() + draw.sectionTableOffset, descriptors.data(),
                descriptors.size() * sizeof(descriptors[0]));
  }
  return payload;
}

D9CCommandChunkWireDrawHeader directDrawHeader() {
  return D9CCommandChunkWireDrawHeader{
      .flags = 0u,
      .primitiveType = 4u,
      .baseVertex = 0,
      .minVertex = 0u,
      .numVertices = 0u,
      .startVertex = 3u,
      .startIndex = 0u,
      .primitiveCount = 1u,
      .stride = 0u,
      .indexFormat = 0u,
      .sectionCount = 0u,
      .sectionTableOffset = 0u,
      .sectionPayloadOffset = 0u,
      .reserved0 = 0u,
  };
}

void expectStatus(const ChunkFixture& fixture, CommandChunkValidationStatus status,
                  std::string_view message) {
  const auto result = fixture.validate();
  check(result.status == status, message);
}

void testEmptyAndFixedRecordViews() {
  const auto empty = makeChunk(std::span<const RecordSpec>{});
  ImportedChunkView emptyView;
  check(empty.validate(&emptyView).valid(), "canonical empty chunk validates");
  check(emptyView.empty() && emptyView.handles.empty(),
        "empty view owns only bounded empty spans");

  const D9CCommandChunkWireUpdateTexture update{
      .srcHandleIndex = 0u,
      .dstHandleIndex = 1u,
  };
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x100000001ull),
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x100000002ull),
      },
  };
  const auto fixture = makeChunk(spec);
  ImportedChunkView view;
  check(fixture.validate(&view).valid(),
        "fixed canonical record with exact handle slice validates");
  const auto record = view.record(0u);
  check(record.header.type == D9C_COMMAND_RECORD_UPDATE_TEXTURE &&
            record.payload.size() == sizeof(update) &&
            record.handles.size() == 2u,
        "validated fixed record exposes bounded payload and handle spans");
}

void testSparseDrawAndUpData() {
  const D9CCommandChunkWireTextureBinding texture{
      .slot = 2u,
      .valid = 1u,
      .handleIndex = 0u,
      .reserved0 = 0u,
  };
  const SectionSpec textureSection{
      .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
      .count = 1u,
      .bytes = bytesOf(texture),
  };
  const RecordSpec drawSpec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = makeDrawPayload(directDrawHeader(),
                                 std::span(&textureSection, 1u)),
      .handles = {handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x200000001ull)},
  };
  const auto drawFixture = makeChunk(drawSpec);
  ImportedChunkView drawView;
  check(drawFixture.validate(&drawView).valid(),
        "sorted sparse draw section validates");
  const auto drawRecord = drawView.record(0u);
  check(drawRecord.sparseState() && drawRecord.sections.size() == 1u &&
            drawRecord.section(0u).payload.size() == sizeof(texture),
        "draw view exposes bounded typed section descriptors");

  auto upHeader = directDrawHeader();
  upHeader.startVertex = 0u;
  upHeader.stride = 4u;
  std::vector<std::byte> vertices(12u, std::byte{0x2a});
  const SectionSpec vertexSection{
      .kind = D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
      .count = 12u,
      .bytes = vertices,
  };
  const RecordSpec upSpec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
      .payload = makeDrawPayload(upHeader, std::span(&vertexSection, 1u)),
  };
  check(makeChunk(upSpec).validate().valid(),
        "direct-UP vertex byte section matches primitive and stride");
}

void testHeaderTableAndSliceRejects() {
  D9CCommandChunkWireUpdateTexture update{0u, 1u};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x300000001ull),
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x300000002ull),
      },
  };

  auto wrongOuter = makeChunk(spec);
  wrongOuter.envelope.version = 1u;
  expectStatus(wrongOuter, CommandChunkValidationStatus::OuterVersionMismatch,
               "outer numeric version 1 rejects before effects");

  auto reserved = makeChunk(spec);
  reserved.header().reserved0 = 1u;
  expectStatus(reserved, CommandChunkValidationStatus::NonZeroReserved,
               "nonzero header reserved field rejects");

  const auto aligned = makeChunk(spec);
  std::vector<std::byte> shifted(aligned.bytes.size() + 1u);
  std::memcpy(shifted.data() + 1u, aligned.bytes.data(), aligned.bytes.size());
  const auto unalignedResult = validateCommandChunk(
      std::span<const std::byte>(shifted).subspan(1u), aligned.envelope);
  check(unalignedResult.status == CommandChunkValidationStatus::InvalidAlignment,
        "misaligned chunk base rejects before typed views are formed");

  auto tableGap = makeChunk(spec);
  tableGap.header().recordTableOffset += 4u;
  expectStatus(tableGap, CommandChunkValidationStatus::NonCanonicalChunkLayout,
               "noncanonical table start rejects");

  auto sliceGap = makeChunk(spec);
  sliceGap.record(0u).firstHandle = 1u;
  expectStatus(sliceGap, CommandChunkValidationStatus::NonCanonicalHandleSlice,
               "noncanonical record handle slice rejects");

  auto zeroGeneration = makeChunk(spec);
  zeroGeneration.handle(0u).generation = 0u;
  expectStatus(zeroGeneration, CommandChunkValidationStatus::InvalidHandleEntry,
               "zero generation rejects before replay");

  auto fullWidthGeneration = makeChunk(spec);
  fullWidthGeneration.handle(0u).generation = 0x01000000u;
  check(fullWidthGeneration.validate().valid(),
        "nonzero generation above 24 bits is canonical");

  auto duplicate = makeChunk(spec);
  duplicate.handle(1u) = duplicate.handle(0u);
  expectStatus(duplicate, CommandChunkValidationStatus::InvalidHandleEntry,
               "duplicate identity in one record slice rejects");
}

void testBidirectionalHandleRejects() {
  D9CCommandChunkWireUpdateTexture update{0u, 1u};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x400000001ull),
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x400000002ull),
      },
  };

  auto outOfSlice = makeChunk(spec);
  auto& outPayload = at<D9CCommandChunkWireUpdateTexture>(
      outOfSlice.bytes, outOfSlice.header().payloadArenaOffset);
  outPayload.dstHandleIndex = 2u;
  expectStatus(outOfSlice, CommandChunkValidationStatus::InvalidHandleReference,
               "payload index outside record slice rejects");

  auto wrongKind = makeChunk(spec);
  wrongKind.handle(1u).kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
  expectStatus(wrongKind, CommandChunkValidationStatus::InvalidHandleReference,
               "payload schema kind mismatch rejects");

  auto orphan = makeChunk(spec);
  auto& orphanPayload = at<D9CCommandChunkWireUpdateTexture>(
      orphan.bytes, orphan.header().payloadArenaOffset);
  orphanPayload.dstHandleIndex = 0u;
  expectStatus(orphan, CommandChunkValidationStatus::HandleSliceMismatch,
               "unreferenced handle-table entry rejects");

  const D9CCommandChunkWireGenerateMipmaps generate{0u};
  const RecordSpec generateSpec{
      .type = D9C_COMMAND_RECORD_GENERATE_MIPMAPS,
      .payload = bytesOf(generate),
      .handles = {
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x400000003ull),
      },
  };
  check(makeChunk(generateSpec).validate().valid(),
        "GenerateMipmaps accepts one exact texture identity");
  auto generateWrongKind = makeChunk(generateSpec);
  generateWrongKind.handle(0u).kind = D9C_CHUNK_HANDLE_KIND_SURFACE;
  expectStatus(generateWrongKind,
               CommandChunkValidationStatus::InvalidHandleReference,
               "GenerateMipmaps rejects a surface-qualified identity");
}

void testSparseSectionRejects() {
  const D9CCommandChunkWireTextureBinding texture{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = 0u,
      .reserved0 = 0u,
  };
  const SectionSpec textureSection{
      .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
      .count = 1u,
      .bytes = bytesOf(texture),
  };
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = makeDrawPayload(directDrawHeader(),
                                 std::span(&textureSection, 1u)),
      .handles = {handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x500000001ull)},
  };

  auto elementSize = makeChunk(spec);
  auto& elementDesc = at<D9CCommandChunkWireSectionDesc>(
      elementSize.bytes,
      elementSize.header().payloadArenaOffset + sizeof(directDrawHeader()));
  ++elementDesc.elementSize;
  expectStatus(elementSize, CommandChunkValidationStatus::InvalidSectionSchema,
               "wrong section element size rejects");

  auto wrongOffset = makeChunk(spec);
  auto& offsetDesc = at<D9CCommandChunkWireSectionDesc>(
      wrongOffset.bytes,
      wrongOffset.header().payloadArenaOffset + sizeof(directDrawHeader()));
  offsetDesc.payloadOffset += 4u;
  expectStatus(wrongOffset, CommandChunkValidationStatus::InvalidSectionRange,
               "noncanonical section payload offset rejects");

  const std::array duplicateSections = {textureSection, textureSection};
  RecordSpec duplicateSpec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = makeDrawPayload(directDrawHeader(), duplicateSections),
      .handles = {handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x500000002ull)},
  };
  expectStatus(makeChunk(duplicateSpec), CommandChunkValidationStatus::InvalidSectionOrder,
               "duplicate section kinds reject");

  auto fullHeader = directDrawHeader();
  fullHeader.flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT;
  const RecordSpec fullSpec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = makeDrawPayload(fullHeader,
                                 std::span<const SectionSpec>{}),
  };
  expectStatus(makeChunk(fullSpec), CommandChunkValidationStatus::InvalidFullSnapshot,
               "full snapshot missing texture and stream slots rejects");
}

void testConstantUpAndPaddingRejects() {
  D9CCommandChunkWireConstantRange constantRange{
      .startRegister = D9C_DRAW_PACKET_MAX_CONST_VS_F - 1u,
      .registerCount = 2u,
  };
  auto constantBytes = bytesOf(constantRange);
  constantBytes.resize(sizeof(constantRange) + 2u * 16u);
  const SectionSpec constantSection{
      .kind = D9C_COMMAND_CHUNK_SECTION_VS_CONST_F,
      .count = 2u,
      .bytes = constantBytes,
  };
  D9CCommandChunkWireDrawHeader apply{};
  const RecordSpec constantSpec{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = makeDrawPayload(apply, std::span(&constantSection, 1u)),
  };
  expectStatus(makeChunk(constantSpec),
               CommandChunkValidationStatus::InvalidConstantRange,
               "constant register overflow rejects");

  auto upHeader = directDrawHeader();
  upHeader.startVertex = 0u;
  upHeader.stride = 4u;
  const SectionSpec shortVertices{
      .kind = D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
      .count = 8u,
      .bytes = std::vector<std::byte>(8u),
  };
  const RecordSpec upSpec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
      .payload = makeDrawPayload(upHeader, std::span(&shortVertices, 1u)),
  };
  expectStatus(makeChunk(upSpec), CommandChunkValidationStatus::InvalidUpData,
               "short UP vertex range rejects");

  D9CCommandChunkWireSetConst boolConst{0u, 1u};
  auto boolPayload = bytesOf(boolConst);
  boolPayload.resize(sizeof(boolConst) + sizeof(std::uint32_t));
  D9CCommandChunkWirePresent present{};
  const std::array records = {
      RecordSpec{.type = D9C_COMMAND_RECORD_SET_VS_CONST_B,
                 .payload = boolPayload},
      RecordSpec{.type = D9C_COMMAND_RECORD_PRESENT,
                 .payload = bytesOf(present)},
  };
  auto padding = makeChunk(records);
  check(padding.record(1u).payloadOffset > padding.record(0u).payloadSize,
        "fixture has inter-record alignment padding");
  padding.payloadByte(padding.record(0u).payloadSize) = std::byte{1};
  expectStatus(padding, CommandChunkValidationStatus::NonZeroPadding,
               "nonzero inter-record padding rejects");
}

void testFailedValidationDoesNotPublishViewOrAllocatePerRecord() {
  D9CCommandChunkWireUpdateTexture update{0u, 0u};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x600000001ull)},
  };
  auto fixture = makeChunk(spec);
  ImportedChunkView view;
  CommandChunkValidationScratch scratch;
  check(fixture.validate(&view).valid(), "warm-up validation succeeds");
  const auto capacity = scratch.referencedHandles.capacity();
  const auto result = validateCommandChunk(fixture.bytes, fixture.envelope,
                                             &view, scratch);
  check(result.valid(), "explicit reusable scratch validates");
  const auto warmedCapacity = scratch.referencedHandles.capacity();
  check(warmedCapacity >= capacity, "scratch capacity is retained");
  check(validateCommandChunk(fixture.bytes, fixture.envelope, &view, scratch)
            .valid() &&
            scratch.referencedHandles.capacity() == warmedCapacity,
        "warm validation reuses one chunk-level scratch allocation");

  auto invalid = fixture;
  invalid.handle(0u).generation = 0u;
  check(!invalid.validate(&view).valid() && view.records.empty() &&
            view.handles.empty() && view.payloadArena.empty(),
        "failed whole-chunk validation publishes no partial view");
}

void testPrevalidatedViewReconstruction() {
  D9CCommandChunkWirePresent present{};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_PRESENT,
      .payload = bytesOf(present),
  };
  const auto fixture = makeChunk(spec);
  ImportedChunkView validated;
  ImportedChunkView reconstructed;
  check(fixture.validate(&validated).valid(),
        "prevalidated-view fixture validates");
  check(importPrevalidatedCommandChunk(
            fixture.bytes, fixture.envelope, reconstructed),
        "validated immutable blob reconstructs a view");
  check(reconstructed.records.data() == validated.records.data() &&
            reconstructed.records.size() == validated.records.size() &&
            reconstructed.handles.data() == validated.handles.data() &&
            reconstructed.handles.size() == validated.handles.size() &&
            reconstructed.payloadArena.data() == validated.payloadArena.data() &&
            reconstructed.payloadArena.size() == validated.payloadArena.size(),
        "reconstructed view aliases the same validated wire ranges");

  auto wrongEnvelope = fixture.envelope;
  ++wrongEnvelope.recordCount;
  check(!importPrevalidatedCommandChunk(
            fixture.bytes, wrongEnvelope, reconstructed) &&
            reconstructed.empty(),
        "outer count drift cannot reconstruct a prevalidated view");
  check(!importPrevalidatedCommandChunk(
            std::span<const std::byte>(fixture.bytes).first(
                fixture.bytes.size() - 1u),
            fixture.envelope, reconstructed) &&
            reconstructed.empty(),
        "truncated storage cannot reconstruct a prevalidated view");
}

D9CWireHandle wireHandle(const void* value) {
  const auto bits = static_cast<std::uint64_t>(
      reinterpret_cast<std::uintptr_t>(value));
  return D9CWireHandle{static_cast<std::uint32_t>(bits),
                       static_cast<std::uint32_t>(bits >> 32u)};
}

void makeSegmentedParts(
    const ChunkFixture& contiguous,
    std::vector<std::byte>& records,
    std::vector<std::byte>& handles,
    std::vector<std::byte>& payload,
    D9CCommandChunkSegmentedTransportV1& transport) {
  const auto& header = contiguous.header();
  const auto recordBytes = static_cast<std::size_t>(header.recordCount) *
                           sizeof(D9CCommandChunkWireRecordHeader);
  const auto handleBytes = static_cast<std::size_t>(header.handleCount) *
                           sizeof(D9CCommandChunkWireHandleEntry);
  records.resize(recordBytes);
  handles.resize(handleBytes);
  payload.resize(header.payloadArenaSize);
  if (!records.empty()) {
    std::memcpy(records.data(), contiguous.bytes.data() +
                                      header.recordTableOffset,
                records.size());
  }
  if (!handles.empty()) {
    std::memcpy(handles.data(), contiguous.bytes.data() +
                                      header.handleTableOffset,
                handles.size());
  }
  if (!payload.empty()) {
    std::memcpy(payload.data(), contiguous.bytes.data() +
                                      header.payloadArenaOffset,
                payload.size());
  }
  transport = {};
  transport.header = header;
  transport.records = wireHandle(records.data());
  transport.recordBytes = static_cast<std::uint32_t>(records.size());
  transport.handles = wireHandle(handles.data());
  transport.handleBytes = static_cast<std::uint32_t>(handles.size());
  transport.payload = wireHandle(payload.data());
  transport.payloadBytes = static_cast<std::uint32_t>(payload.size());
}

void testSegmentedTransportSharesCanonicalValidation() {
  const D9CCommandChunkWireUpdateTexture update{0u, 1u};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x810000001ull),
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x810000002ull),
      },
  };
  const auto contiguous = makeChunk(spec);
  std::vector<std::byte> records;
  std::vector<std::byte> handles;
  std::vector<std::byte> payload;
  D9CCommandChunkSegmentedTransportV1 segmented{};
  makeSegmentedParts(contiguous, records, handles, payload, segmented);
  segmented.renderTapeCaptureToken = 0x1020304050607080ull;
  segmented.renderTapeEventOrdinal = 7u;

  const auto recordBytes = std::span<const std::byte>(records);
  const auto handleBytes = std::span<const std::byte>(handles);
  const auto payloadBytes = std::span<const std::byte>(payload);
  ImportedChunkView view;
  check(validateSegmentedCommandChunk(
            segmented, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope, &view)
            .valid(),
        "segmented regions share canonical semantic validation");
  check(view.records.size() == 1u && view.handles.size() == 2u &&
            view.record(0u).payload.size() == sizeof(update),
        "segmented view exposes typed records, handles, and payload");

  auto wrongBytes = segmented;
  ++wrongBytes.recordBytes;
  check(validateSegmentedCommandChunk(
            wrongBytes, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented live table byte count is exact");
  auto wrongOffset = segmented;
  ++wrongOffset.header.payloadArenaOffset;
  check(validateSegmentedCommandChunk(
            wrongOffset, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented virtual offsets retain canonical alignment gaps");
}

void testSegmentedEnvelopeValidation() {
  const D9CCommandChunkWireUpdateTexture update{0u, 1u};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x820000001ull),
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x820000002ull),
      },
  };
  const auto contiguous = makeChunk(spec);
  std::vector<std::byte> records;
  std::vector<std::byte> handles;
  std::vector<std::byte> payload;
  D9CCommandChunkSegmentedTransportV1 segmented{};
  makeSegmentedParts(contiguous, records, handles, payload, segmented);
  const auto recordBytes = std::span<const std::byte>(records);
  const auto handleBytes = std::span<const std::byte>(handles);
  const auto payloadBytes = std::span<const std::byte>(payload);
  check(validateSegmentedCommandChunkEnvelope(
            segmented, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .valid(),
        "segmented envelope accepts canonical metadata and live spans");

  auto wrongRecordCount = segmented;
  ++wrongRecordCount.header.recordCount;
  check(validateSegmentedCommandChunkEnvelope(
            wrongRecordCount, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks the wire record count");
  auto wrongHandleCount = segmented;
  ++wrongHandleCount.header.handleCount;
  check(validateSegmentedCommandChunkEnvelope(
            wrongHandleCount, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks the wire handle count");
  auto wrongEnvelopeRecordCount = segmented;
  auto wrongEnvelope = contiguous.envelope;
  ++wrongEnvelope.recordCount;
  check(validateSegmentedCommandChunkEnvelope(
            wrongEnvelopeRecordCount, recordBytes, handleBytes, payloadBytes,
            wrongEnvelope)
            .status == CommandChunkValidationStatus::OuterCountMismatch,
        "segmented envelope checks the producer envelope record count");
  auto wrongEnvelopeHandleCount = segmented;
  auto handleCountEnvelope = contiguous.envelope;
  ++handleCountEnvelope.handleCount;
  check(validateSegmentedCommandChunkEnvelope(
            wrongEnvelopeHandleCount, recordBytes, handleBytes, payloadBytes,
            handleCountEnvelope)
            .status == CommandChunkValidationStatus::OuterCountMismatch,
        "segmented envelope checks the producer envelope handle count");
  auto wrongRecordBytes = segmented;
  ++wrongRecordBytes.recordBytes;
  check(validateSegmentedCommandChunkEnvelope(
            wrongRecordBytes, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks the record role extent");
  auto wrongHandleBytes = segmented;
  ++wrongHandleBytes.handleBytes;
  check(validateSegmentedCommandChunkEnvelope(
            wrongHandleBytes, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks the handle role extent");
  auto wrongPayloadBytes = segmented;
  ++wrongPayloadBytes.payloadBytes;
  check(validateSegmentedCommandChunkEnvelope(
            wrongPayloadBytes, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks the payload role extent");
  auto wrongPayloadArenaSize = segmented;
  ++wrongPayloadArenaSize.header.payloadArenaSize;
  check(validateSegmentedCommandChunkEnvelope(
            wrongPayloadArenaSize, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks the payload arena size");

  const auto shortRecords = recordBytes.first(recordBytes.size() - 1u);
  check(validateSegmentedCommandChunkEnvelope(
            segmented, shortRecords, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope rejects a short record span");
  const auto shortHandles = handleBytes.first(handleBytes.size() - 1u);
  check(validateSegmentedCommandChunkEnvelope(
            segmented, recordBytes, shortHandles, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope rejects a short handle span");
  const auto shortPayload = payloadBytes.first(payloadBytes.size() - 1u);
  check(validateSegmentedCommandChunkEnvelope(
            segmented, recordBytes, handleBytes, shortPayload,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope rejects a short payload span");

  auto wrongHeaderOffset = segmented;
  ++wrongHeaderOffset.header.handleTableOffset;
  check(validateSegmentedCommandChunkEnvelope(
            wrongHeaderOffset, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks the canonical handle-table offset");
  auto wrongPayloadOffset = segmented;
  ++wrongPayloadOffset.header.payloadArenaOffset;
  check(validateSegmentedCommandChunkEnvelope(
            wrongPayloadOffset, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks the canonical payload offset");
  auto wrongHeaderSize = segmented;
  ++wrongHeaderSize.header.headerSize;
  check(validateSegmentedCommandChunkEnvelope(
            wrongHeaderSize, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks the header size");
  auto wrongRecordHeaderSize = segmented;
  ++wrongRecordHeaderSize.header.recordHeaderSize;
  check(validateSegmentedCommandChunkEnvelope(
            wrongRecordHeaderSize, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::InvalidHeader,
        "segmented envelope checks the record-header size");
  auto wrongHandleEntrySize = segmented;
  ++wrongHandleEntrySize.header.handleEntrySize;
  check(validateSegmentedCommandChunkEnvelope(
            wrongHandleEntrySize, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::InvalidHeader,
        "segmented envelope checks the handle-entry size");
  auto wrongWireVersion = segmented;
  wrongWireVersion.header.version = 1u;
  check(validateSegmentedCommandChunkEnvelope(
            wrongWireVersion, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::OuterVersionMismatch,
        "segmented envelope checks the wire version");
  auto wrongEnvelopeVersion = segmented;
  wrongEnvelopeVersion.header.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
  auto versionEnvelope = contiguous.envelope;
  versionEnvelope.version = 1u;
  check(validateSegmentedCommandChunkEnvelope(
            wrongEnvelopeVersion, recordBytes, handleBytes, payloadBytes,
            versionEnvelope)
            .status == CommandChunkValidationStatus::OuterVersionMismatch,
        "segmented envelope checks the envelope version");

  auto wrongHeaderReserved = segmented;
  wrongHeaderReserved.header.reserved0 = 1u;
  check(validateSegmentedCommandChunkEnvelope(
            wrongHeaderReserved, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonZeroReserved,
        "segmented envelope checks header reserved fields");
  auto wrongRoleReserved = segmented;
  wrongRoleReserved.payloadReserved = 1u;
  check(validateSegmentedCommandChunkEnvelope(
            wrongRoleReserved, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope checks role reserved fields");

  struct MetadataMutation {
    std::string_view name;
    void (*mutate)(D9CCommandChunkSegmentedTransportV1&,
                   CommandChunkEnvelope&);
    CommandChunkValidationStatus status;
  };
  const std::array<MetadataMutation, 8u> metadataMutations{{
      {"envelope-record-count",
       [](D9CCommandChunkSegmentedTransportV1&,
          CommandChunkEnvelope& value) { ++value.recordCount; },
       CommandChunkValidationStatus::OuterCountMismatch},
      {"envelope-handle-count",
       [](D9CCommandChunkSegmentedTransportV1&,
          CommandChunkEnvelope& value) { ++value.handleCount; },
       CommandChunkValidationStatus::OuterCountMismatch},
      {"header-reserved-0",
       [](D9CCommandChunkSegmentedTransportV1& value,
          CommandChunkEnvelope&) { value.header.reserved0 = 1u; },
       CommandChunkValidationStatus::NonZeroReserved},
      {"header-reserved-1",
       [](D9CCommandChunkSegmentedTransportV1& value,
          CommandChunkEnvelope&) { value.header.reserved1 = 1u; },
       CommandChunkValidationStatus::NonZeroReserved},
      {"record-reserved",
       [](D9CCommandChunkSegmentedTransportV1& value,
          CommandChunkEnvelope&) { value.recordReserved = 1u; },
       CommandChunkValidationStatus::NonCanonicalChunkLayout},
      {"handle-reserved",
       [](D9CCommandChunkSegmentedTransportV1& value,
          CommandChunkEnvelope&) { value.handleReserved = 1u; },
       CommandChunkValidationStatus::NonCanonicalChunkLayout},
      {"payload-reserved",
       [](D9CCommandChunkSegmentedTransportV1& value,
          CommandChunkEnvelope&) { value.payloadReserved = 1u; },
       CommandChunkValidationStatus::NonCanonicalChunkLayout},
      {"record-table-offset",
       [](D9CCommandChunkSegmentedTransportV1& value,
          CommandChunkEnvelope&) { ++value.header.recordTableOffset; },
       CommandChunkValidationStatus::NonCanonicalChunkLayout},
  }};
  for (const auto& mutation : metadataMutations) {
    auto candidate = segmented;
    auto candidateEnvelope = contiguous.envelope;
    mutation.mutate(candidate, candidateEnvelope);
    check(validateSegmentedCommandChunkEnvelope(
              candidate, recordBytes, handleBytes, payloadBytes,
              candidateEnvelope)
              .status == mutation.status,
          mutation.name);
  }

  auto qualifiedIdentity = segmented;
  qualifiedIdentity.producerIdentity = {
      .firstEventOrdinal = 11u,
      .lastEventOrdinal = 11u,
      .firstSourceOrdinal = 17u,
      .lastSourceOrdinal = 17u,
  };
  auto qualifiedEnvelope = contiguous.envelope;
  qualifiedEnvelope.producerIdentity = qualifiedIdentity.producerIdentity;
  check(validateSegmentedCommandChunkEnvelope(
            qualifiedIdentity, recordBytes, handleBytes, payloadBytes,
            qualifiedEnvelope)
            .valid(),
        "segmented envelope accepts a matching source-qualified identity");

  const D9CCommandChunkWireClear clear{
      .flags = 1u,
      .colorARGB = 0xff123456u,
      .z = 1.0f,
      .stencil = 0u,
      .rectCount = 0u,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  const auto zeroRoleChunk = makeChunk(RecordSpec{
      .type = D9C_COMMAND_RECORD_CLEAR,
      .payload = bytesOf(clear),
  });
  std::vector<std::byte> zeroRecords;
  std::vector<std::byte> zeroHandles;
  std::vector<std::byte> zeroPayload;
  D9CCommandChunkSegmentedTransportV1 zeroRoleTransport{};
  makeSegmentedParts(zeroRoleChunk, zeroRecords, zeroHandles, zeroPayload,
                     zeroRoleTransport);
  zeroRoleTransport.handles = {};
  zeroRoleTransport.handleBytes = 0u;
  check(validateSegmentedCommandChunkEnvelope(
            zeroRoleTransport, std::span<const std::byte>(zeroRecords),
            std::span<const std::byte>{}, std::span<const std::byte>(zeroPayload),
            zeroRoleChunk.envelope)
            .valid(),
        "segmented envelope accepts a null token for a zero-length role");

  auto invalidIdentity = segmented;
  invalidIdentity.producerIdentity.firstEventOrdinal = 1u;
  check(validateSegmentedCommandChunkEnvelope(
            invalidIdentity, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::InvalidSourceIdentity,
        "segmented envelope rejects an incomplete producer identity");
  auto mismatchedIdentity = segmented;
  mismatchedIdentity.producerIdentity = {
      .firstEventOrdinal = 1u,
      .lastEventOrdinal = 1u,
      .firstSourceOrdinal = 1u,
      .lastSourceOrdinal = 1u,
  };
  auto identityEnvelope = contiguous.envelope;
  identityEnvelope.producerIdentity = {};
  check(validateSegmentedCommandChunkEnvelope(
            mismatchedIdentity, recordBytes, handleBytes, payloadBytes,
            identityEnvelope)
            .status == CommandChunkValidationStatus::InvalidSourceIdentity,
        "segmented envelope rejects a producer identity mismatch");

  auto unalignedStorage = std::vector<std::byte>(records.size() + 1u);
  std::memcpy(unalignedStorage.data() + 1u, records.data(), records.size());
  check(validateSegmentedCommandChunkEnvelope(
            segmented,
            std::span<const std::byte>(unalignedStorage).subspan(1u),
            handleBytes, payloadBytes, contiguous.envelope)
            .status == CommandChunkValidationStatus::InvalidAlignment,
        "segmented envelope checks live record-span alignment");
  auto unalignedHandles = std::vector<std::byte>(handles.size() + 1u);
  std::memcpy(unalignedHandles.data() + 1u, handles.data(), handles.size());
  check(validateSegmentedCommandChunkEnvelope(
            segmented, recordBytes,
            std::span<const std::byte>(unalignedHandles).subspan(1u),
            payloadBytes, contiguous.envelope)
            .status == CommandChunkValidationStatus::InvalidAlignment,
        "segmented envelope checks live handle-span alignment");
  auto unalignedPayload = std::vector<std::byte>(payload.size() + 1u);
  std::memcpy(unalignedPayload.data() + 1u, payload.data(), payload.size());
  check(validateSegmentedCommandChunkEnvelope(
            segmented, recordBytes, handleBytes,
            std::span<const std::byte>(unalignedPayload).subspan(1u),
            contiguous.envelope)
            .status == CommandChunkValidationStatus::InvalidAlignment,
        "segmented envelope checks live payload-span alignment");
  auto oversized = segmented;
  oversized.header.payloadArenaSize =
      D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES;
  oversized.payloadBytes = oversized.header.payloadArenaSize;
  const std::vector<std::byte> oversizedPayloadStorage(
      static_cast<std::size_t>(oversized.payloadBytes));
  const auto oversizedPayload = std::span<const std::byte>(
      oversizedPayloadStorage.data(), oversizedPayloadStorage.size());
  check(validateSegmentedCommandChunkEnvelope(
            oversized, recordBytes, handleBytes, oversizedPayload,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::NonCanonicalChunkLayout,
        "segmented envelope bounds the checked virtual total");

  auto malformedBody = segmented;
  D9CCommandChunkWireUpdateTexture malformedUpdate{};
  std::memcpy(&malformedUpdate, payload.data(), sizeof(malformedUpdate));
  malformedUpdate.dstHandleIndex = 2u;
  std::memcpy(payload.data(), &malformedUpdate, sizeof(malformedUpdate));
  check(validateSegmentedCommandChunkEnvelope(
            malformedBody, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .valid(),
        "segmented envelope does not inspect semantic record bodies");
  check(validateSegmentedCommandChunk(
            malformedBody, recordBytes, handleBytes, payloadBytes,
            contiguous.envelope)
            .status == CommandChunkValidationStatus::InvalidHandleReference,
        "full segmented validation rejects the malformed record body");
}

void testSegmentedEnvelopeDoesNotReadRoleSpans() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#elif !defined(MAP_ANONYMOUS) && !defined(MAP_ANON)
  return;
#else
  const auto pageSizeValue = sysconf(_SC_PAGESIZE);
  check(pageSizeValue > 0, "POSIX page size is available");
  const auto pageSize = static_cast<std::size_t>(pageSizeValue);
  struct NoAccessPage {
    std::size_t size = 0u;
    void* address = MAP_FAILED;

    explicit NoAccessPage(std::size_t pageSizeIn) : size(pageSizeIn) {
#if defined(MAP_ANONYMOUS)
      address = mmap(nullptr, size, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
      address = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
      check(address != MAP_FAILED, "no-access role page maps");
    }

    ~NoAccessPage() {
      if (address != MAP_FAILED) (void)munmap(address, size);
    }
  };

  const D9CCommandChunkWireUpdateTexture update{0u, 1u};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x830000001ull),
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x830000002ull),
      },
  };
  const auto contiguous = makeChunk(spec);
  const auto& header = contiguous.header();
  const auto recordBytes = static_cast<std::size_t>(header.recordCount) *
                           sizeof(D9CCommandChunkWireRecordHeader);
  const auto handleBytes = static_cast<std::size_t>(header.handleCount) *
                           sizeof(D9CCommandChunkWireHandleEntry);
  check(recordBytes <= pageSize && handleBytes <= pageSize &&
            header.payloadArenaSize <= pageSize,
        "role extents fit in protected pages");
  NoAccessPage recordsPage(pageSize);
  NoAccessPage handlesPage(pageSize);
  NoAccessPage payloadPage(pageSize);
  const auto records = std::span<const std::byte>(
      static_cast<const std::byte*>(recordsPage.address), recordBytes);
  const auto handles = std::span<const std::byte>(
      static_cast<const std::byte*>(handlesPage.address), handleBytes);
  const auto payload = std::span<const std::byte>(
      static_cast<const std::byte*>(payloadPage.address),
      header.payloadArenaSize);
  D9CCommandChunkSegmentedTransportV1 transport{};
  transport.header = header;
  transport.records = wireHandle(recordsPage.address);
  transport.recordBytes = static_cast<std::uint32_t>(recordBytes);
  transport.handles = wireHandle(handlesPage.address);
  transport.handleBytes = static_cast<std::uint32_t>(handleBytes);
  transport.payload = wireHandle(payloadPage.address);
  transport.payloadBytes = header.payloadArenaSize;
  check(validateSegmentedCommandChunkEnvelope(
            transport, records, handles, payload, contiguous.envelope)
            .valid(),
        "segmented envelope validates protected role spans without reading them");
#endif
}

struct RejectObservers {
  std::uint32_t registryRetains = 0u;
  std::uint32_t stateMutations = 0u;
  std::uint32_t submissions = 0u;
  std::uint32_t queryCallbacks = 0u;
  std::uint32_t refcountChanges = 0u;

  bool untouched() const noexcept {
    return registryRetains == 0u && stateMutations == 0u &&
           submissions == 0u && queryCallbacks == 0u &&
           refcountChanges == 0u;
  }
};

struct MutationCase {
  std::string_view name;
  CommandChunkValidationStatus status;
  std::function<void(ChunkFixture&)> mutate;
};

void expectRejectedWithoutSideEffects(const ChunkFixture& fixture,
                                      const MutationCase& mutation,
                                      std::uint32_t seed,
                                      std::uint32_t ordinal) {
  RejectObservers observers;
  ImportedChunkView view;
  const auto result = fixture.validate(&view);
  if (result.valid()) {
    ++observers.registryRetains;
    ++observers.stateMutations;
    ++observers.submissions;
    ++observers.queryCallbacks;
    ++observers.refcountChanges;
  }
  if (result.status != mutation.status || !observers.untouched() ||
      !view.records.empty() || !view.handles.empty() ||
      !view.payloadArena.empty()) {
    throw TestFailure(
        "mutation reject mismatch: seed=" + std::to_string(seed) +
        " ordinal=" + std::to_string(ordinal) +
        " case=" + std::string(mutation.name) +
        " expected=" +
        std::to_string(static_cast<std::uint32_t>(mutation.status)) +
        " actual=" +
        std::to_string(static_cast<std::uint32_t>(result.status)));
  }
}

void testTableAndSeededMalformedPropertyCorpus() {
  const D9CCommandChunkWireTextureBinding texture{
      .slot = 2u,
      .valid = 1u,
      .handleIndex = 0u,
      .reserved0 = 0u,
  };
  const SectionSpec textureSection{
      .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
      .count = 1u,
      .bytes = bytesOf(texture),
  };
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = makeDrawPayload(directDrawHeader(),
                                 std::span(&textureSection, 1u)),
      .handles = {handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x700000001ull)},
  };
  const auto valid = makeChunk(spec);
  check(valid.validate().valid(), "property corpus base chunk validates");

  const auto drawAt = [](ChunkFixture& fixture)
      -> D9CCommandChunkWireDrawHeader& {
    return at<D9CCommandChunkWireDrawHeader>(
        fixture.bytes,
        fixture.header().payloadArenaOffset + fixture.record(0u).payloadOffset);
  };
  const auto descAt = [](ChunkFixture& fixture)
      -> D9CCommandChunkWireSectionDesc& {
    return at<D9CCommandChunkWireSectionDesc>(
        fixture.bytes,
        fixture.header().payloadArenaOffset + fixture.record(0u).payloadOffset +
            sizeof(D9CCommandChunkWireDrawHeader));
  };
  const auto textureAt = [](ChunkFixture& fixture)
      -> D9CCommandChunkWireTextureBinding& {
    const auto payloadStart = fixture.header().payloadArenaOffset +
                              fixture.record(0u).payloadOffset;
    const auto offset = at<D9CCommandChunkWireSectionDesc>(
                            fixture.bytes,
                            payloadStart +
                                sizeof(D9CCommandChunkWireDrawHeader))
                            .payloadOffset;
    return at<D9CCommandChunkWireTextureBinding>(fixture.bytes,
                                                   payloadStart + offset);
  };

  const std::array<MutationCase, 34> mutations = {{
      {"outer-version", CommandChunkValidationStatus::OuterVersionMismatch,
       [](ChunkFixture& value) { value.envelope.version = 1u; }},
      {"outer-record-count", CommandChunkValidationStatus::OuterCountMismatch,
       [](ChunkFixture& value) { ++value.envelope.recordCount; }},
      {"outer-handle-count", CommandChunkValidationStatus::OuterCountMismatch,
       [](ChunkFixture& value) { ++value.envelope.handleCount; }},
      {"wire-version", CommandChunkValidationStatus::OuterVersionMismatch,
       [](ChunkFixture& value) { value.header().version = 1u; }},
      {"header-size", CommandChunkValidationStatus::InvalidHeader,
       [](ChunkFixture& value) { ++value.header().headerSize; }},
      {"record-header-size", CommandChunkValidationStatus::InvalidHeader,
       [](ChunkFixture& value) { ++value.header().recordHeaderSize; }},
      {"handle-entry-size", CommandChunkValidationStatus::InvalidHeader,
       [](ChunkFixture& value) { ++value.header().handleEntrySize; }},
      {"record-table-gap", CommandChunkValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) { value.header().recordTableOffset += 4u; }},
      {"record-count-overflow", CommandChunkValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) {
         value.header().recordCount = std::numeric_limits<std::uint32_t>::max();
         value.envelope.recordCount = value.header().recordCount;
       }},
      {"handle-table-gap", CommandChunkValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) { value.header().handleTableOffset += 4u; }},
      {"payload-arena-gap", CommandChunkValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) { value.header().payloadArenaOffset += 4u; }},
      {"payload-arena-tail", CommandChunkValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) { ++value.header().payloadArenaSize; }},
      {"header-reserved-0", CommandChunkValidationStatus::NonZeroReserved,
       [](ChunkFixture& value) { value.header().reserved0 = 1u; }},
      {"header-reserved-1", CommandChunkValidationStatus::NonZeroReserved,
       [](ChunkFixture& value) { value.header().reserved1 = 1u; }},
      {"record-type", CommandChunkValidationStatus::InvalidRecordType,
       [](ChunkFixture& value) { value.record(0u).type = 0u; }},
      {"record-flags", CommandChunkValidationStatus::InvalidRecordFlags,
       [](ChunkFixture& value) { value.record(0u).flags = 1u; }},
      {"record-payload-offset", CommandChunkValidationStatus::NonCanonicalPayloadLayout,
       [](ChunkFixture& value) { value.record(0u).payloadOffset = 4u; }},
      {"record-payload-size", CommandChunkValidationStatus::InvalidSectionRange,
       [](ChunkFixture& value) { --value.record(0u).payloadSize; }},
      {"record-first-handle", CommandChunkValidationStatus::NonCanonicalHandleSlice,
       [](ChunkFixture& value) { value.record(0u).firstHandle = 1u; }},
      {"record-handle-count", CommandChunkValidationStatus::InvalidHandleReference,
       [](ChunkFixture& value) { value.record(0u).handleCount = 0u; }},
      {"record-reserved-0", CommandChunkValidationStatus::NonZeroReserved,
       [](ChunkFixture& value) { value.record(0u).reserved0 = 1u; }},
      {"record-reserved-1", CommandChunkValidationStatus::NonZeroReserved,
       [](ChunkFixture& value) { value.record(0u).reserved1 = 1u; }},
      {"handle-kind", CommandChunkValidationStatus::InvalidHandleEntry,
       [](ChunkFixture& value) {
         value.handle(0u).kind = D9C_CHUNK_HANDLE_KIND_QUERY + 1u;
       }},
      {"handle-generation", CommandChunkValidationStatus::InvalidHandleEntry,
       [](ChunkFixture& value) { value.handle(0u).generation = 0u; }},
      {"handle-object-id", CommandChunkValidationStatus::InvalidHandleEntry,
       [](ChunkFixture& value) { value.handle(0u).objectId = 0u; }},
      {"draw-reserved", CommandChunkValidationStatus::InvalidDrawHeader,
       [drawAt](ChunkFixture& value) { drawAt(value).reserved0 = 1u; }},
      {"section-table-offset", CommandChunkValidationStatus::InvalidSectionTable,
       [drawAt](ChunkFixture& value) { ++drawAt(value).sectionTableOffset; }},
      {"section-payload-offset", CommandChunkValidationStatus::InvalidSectionTable,
       [drawAt](ChunkFixture& value) { drawAt(value).sectionPayloadOffset += 4u; }},
      {"descriptor-kind", CommandChunkValidationStatus::InvalidSectionOrder,
       [descAt](ChunkFixture& value) { descAt(value).kind = 0u; }},
      {"descriptor-element-size", CommandChunkValidationStatus::InvalidSectionSchema,
       [descAt](ChunkFixture& value) { ++descAt(value).elementSize; }},
      {"descriptor-count", CommandChunkValidationStatus::InvalidSectionSchema,
       [descAt](ChunkFixture& value) { descAt(value).count = 0u; }},
      {"descriptor-offset", CommandChunkValidationStatus::InvalidSectionRange,
       [descAt](ChunkFixture& value) { descAt(value).payloadOffset += 4u; }},
      {"texture-valid", CommandChunkValidationStatus::InvalidSectionSchema,
       [textureAt](ChunkFixture& value) { textureAt(value).valid = 2u; }},
      {"texture-index", CommandChunkValidationStatus::InvalidHandleReference,
       [textureAt](ChunkFixture& value) { textureAt(value).handleIndex = 1u; }},
  }};

  constexpr std::uint32_t seed = 0xc2f34a91u;
  std::uint32_t ordinal = 0u;
  for (const auto& mutation : mutations) {
    auto fixture = valid;
    mutation.mutate(fixture);
    expectRejectedWithoutSideEffects(fixture, mutation, seed, ordinal++);
  }

  std::uint32_t random = seed;
  for (std::uint32_t i = 0u; i < 256u; ++i) {
    random ^= random << 13u;
    random ^= random >> 17u;
    random ^= random << 5u;
    const auto& mutation = mutations[random % mutations.size()];
    auto fixture = valid;
    mutation.mutate(fixture);
    expectRejectedWithoutSideEffects(fixture, mutation, seed, ordinal++);
  }
}

}  // namespace

int main() {
  try {
    testEmptyAndFixedRecordViews();
    testSparseDrawAndUpData();
    testHeaderTableAndSliceRejects();
    testBidirectionalHandleRejects();
    testSparseSectionRejects();
    testConstantUpAndPaddingRejects();
    testFailedValidationDoesNotPublishViewOrAllocatePerRecord();
    testPrevalidatedViewReconstruction();
    testSegmentedTransportSharesCanonicalValidation();
    testSegmentedEnvelopeValidation();
    testSegmentedEnvelopeDoesNotReadRoleSpans();
    testTableAndSeededMalformedPropertyCorpus();
  } catch (const TestFailure& error) {
    std::cerr << "chunk_record_validation_spec failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_validation_spec passed\n";
  return EXIT_SUCCESS;
}
