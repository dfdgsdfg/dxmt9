#include "device_c_chunk_v2_validate.hpp"

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

namespace {

using dxmt9::d3d9::ImportedChunkV2View;
using dxmt9::d3d9::V2ChunkEnvelope;
using dxmt9::d3d9::V2ValidationScratch;
using dxmt9::d3d9::V2ValidationStatus;
using dxmt9::d3d9::importPrevalidatedCommandChunkV2;
using dxmt9::d3d9::validateCommandChunkV2;

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
  std::vector<D9CCommandChunkWireHandleEntryV2> handles;
};

struct ChunkFixture {
  std::vector<std::byte> bytes;
  V2ChunkEnvelope envelope{};

  D9CCommandChunkWireHeaderV2& header() {
    return at<D9CCommandChunkWireHeaderV2>(bytes, 0u);
  }

  D9CCommandChunkWireRecordHeaderV2& record(std::size_t index) {
    return at<D9CCommandChunkWireRecordHeaderV2>(
        bytes, header().recordTableOffset +
                   index * sizeof(D9CCommandChunkWireRecordHeaderV2));
  }

  D9CCommandChunkWireHandleEntryV2& handle(std::size_t index) {
    return at<D9CCommandChunkWireHandleEntryV2>(
        bytes, header().handleTableOffset +
                   index * sizeof(D9CCommandChunkWireHandleEntryV2));
  }

  std::byte& payloadByte(std::size_t offset) {
    return bytes[header().payloadArenaOffset + offset];
  }

  auto validate(ImportedChunkV2View* view = nullptr) const {
    return validateCommandChunkV2(bytes, envelope, view);
  }
};

ChunkFixture makeChunk(std::span<const RecordSpec> specs) {
  ChunkFixture fixture;
  std::vector<D9CCommandChunkWireRecordHeaderV2> records;
  std::vector<D9CCommandChunkWireHandleEntryV2> handles;
  std::vector<std::byte> payload;
  records.reserve(specs.size());

  for (const auto& spec : specs) {
    const auto* rule = dxmt9::d3d9::v2RecordRule(spec.type);
    check(rule != nullptr, "fixture record type exists in V2 schema");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back(D9CCommandChunkWireRecordHeaderV2{
        .type = spec.type,
        .flags = D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
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

  D9CCommandChunkWireHeaderV2 header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION_V2,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_V2_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_V2_SIZE,
      .recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
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
      alignof(D9CCommandChunkWireHandleEntryV2)));
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
  fixture.envelope = V2ChunkEnvelope{
      .version = D9C_COMMAND_CHUNK_VERSION_V2,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
  };
  return fixture;
}

ChunkFixture makeChunk(const RecordSpec& spec) {
  return makeChunk(std::span<const RecordSpec>(&spec, 1u));
}

D9CCommandChunkWireHandleEntryV2 handle(std::uint32_t kind,
                                        std::uint64_t id,
                                        std::uint32_t generation = 1u) {
  return D9CCommandChunkWireHandleEntryV2{
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
    D9CCommandChunkWireDrawHeaderV2 draw,
    std::span<const SectionSpec> sectionSpecs) {
  draw.sectionCount = static_cast<std::uint32_t>(sectionSpecs.size());
  draw.sectionTableOffset = sizeof(draw);
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sectionSpecs.size() *
                         sizeof(D9CCommandChunkWireSectionDescV2),
      alignof(std::uint32_t)));
  std::vector<std::byte> payload(draw.sectionPayloadOffset);
  std::vector<D9CCommandChunkWireSectionDescV2> descriptors;
  descriptors.reserve(sectionSpecs.size());
  for (const auto& section : sectionSpecs) {
    const auto* rule = dxmt9::d3d9::v2SectionRule(section.kind);
    check(rule != nullptr, "fixture section kind exists in V2 schema");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    descriptors.push_back(D9CCommandChunkWireSectionDescV2{
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

D9CCommandChunkWireDrawHeaderV2 directDrawHeader() {
  return D9CCommandChunkWireDrawHeaderV2{
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

void expectStatus(const ChunkFixture& fixture, V2ValidationStatus status,
                  std::string_view message) {
  const auto result = fixture.validate();
  check(result.status == status, message);
}

void testEmptyAndFixedRecordViews() {
  const auto empty = makeChunk(std::span<const RecordSpec>{});
  ImportedChunkV2View emptyView;
  check(empty.validate(&emptyView).valid(), "canonical empty chunk validates");
  check(emptyView.empty() && emptyView.handles.empty(),
        "empty view owns only bounded empty spans");

  const D9CCommandChunkWireUpdateTextureV2 update{
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
  ImportedChunkV2View view;
  check(fixture.validate(&view).valid(),
        "fixed V2 record with exact handle slice validates");
  const auto record = view.record(0u);
  check(record.header.type == D9C_COMMAND_RECORD_UPDATE_TEXTURE &&
            record.payload.size() == sizeof(update) &&
            record.handles.size() == 2u,
        "validated fixed record exposes bounded payload and handle spans");
}

void testSparseDrawAndUpData() {
  const D9CCommandChunkWireTextureBindingV2 texture{
      .slot = 2u,
      .valid = 1u,
      .handleIndex = 0u,
      .reserved0 = 0u,
  };
  const SectionSpec textureSection{
      .kind = D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE,
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
  ImportedChunkV2View drawView;
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
      .kind = D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA,
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
  D9CCommandChunkWireUpdateTextureV2 update{0u, 1u};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x300000001ull),
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x300000002ull),
      },
  };

  auto wrongOuter = makeChunk(spec);
  wrongOuter.envelope.version = D9C_COMMAND_CHUNK_VERSION;
  expectStatus(wrongOuter, V2ValidationStatus::OuterVersionMismatch,
               "mixed outer V1 / inner V2 rejects");

  auto reserved = makeChunk(spec);
  reserved.header().reserved0 = 1u;
  expectStatus(reserved, V2ValidationStatus::NonZeroReserved,
               "nonzero header reserved field rejects");

  const auto aligned = makeChunk(spec);
  std::vector<std::byte> shifted(aligned.bytes.size() + 1u);
  std::memcpy(shifted.data() + 1u, aligned.bytes.data(), aligned.bytes.size());
  const auto unalignedResult = validateCommandChunkV2(
      std::span<const std::byte>(shifted).subspan(1u), aligned.envelope);
  check(unalignedResult.status == V2ValidationStatus::InvalidAlignment,
        "misaligned chunk base rejects before typed views are formed");

  auto tableGap = makeChunk(spec);
  tableGap.header().recordTableOffset += 4u;
  expectStatus(tableGap, V2ValidationStatus::NonCanonicalChunkLayout,
               "noncanonical table start rejects");

  auto sliceGap = makeChunk(spec);
  sliceGap.record(0u).firstHandle = 1u;
  expectStatus(sliceGap, V2ValidationStatus::NonCanonicalHandleSlice,
               "noncanonical record handle slice rejects");

  auto zeroGeneration = makeChunk(spec);
  zeroGeneration.handle(0u).generation = 0u;
  expectStatus(zeroGeneration, V2ValidationStatus::InvalidHandleEntry,
               "zero generation rejects before replay");

  auto duplicate = makeChunk(spec);
  duplicate.handle(1u) = duplicate.handle(0u);
  expectStatus(duplicate, V2ValidationStatus::InvalidHandleEntry,
               "duplicate identity in one record slice rejects");
}

void testBidirectionalHandleRejects() {
  D9CCommandChunkWireUpdateTextureV2 update{0u, 1u};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x400000001ull),
          handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x400000002ull),
      },
  };

  auto outOfSlice = makeChunk(spec);
  auto& outPayload = at<D9CCommandChunkWireUpdateTextureV2>(
      outOfSlice.bytes, outOfSlice.header().payloadArenaOffset);
  outPayload.dstHandleIndex = 2u;
  expectStatus(outOfSlice, V2ValidationStatus::InvalidHandleReference,
               "payload index outside record slice rejects");

  auto wrongKind = makeChunk(spec);
  wrongKind.handle(1u).kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
  expectStatus(wrongKind, V2ValidationStatus::InvalidHandleReference,
               "payload schema kind mismatch rejects");

  auto orphan = makeChunk(spec);
  auto& orphanPayload = at<D9CCommandChunkWireUpdateTextureV2>(
      orphan.bytes, orphan.header().payloadArenaOffset);
  orphanPayload.dstHandleIndex = 0u;
  expectStatus(orphan, V2ValidationStatus::HandleSliceMismatch,
               "unreferenced handle-table entry rejects");
}

void testSparseSectionRejects() {
  const D9CCommandChunkWireTextureBindingV2 texture{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = 0u,
      .reserved0 = 0u,
  };
  const SectionSpec textureSection{
      .kind = D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE,
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
  auto& elementDesc = at<D9CCommandChunkWireSectionDescV2>(
      elementSize.bytes,
      elementSize.header().payloadArenaOffset + sizeof(directDrawHeader()));
  ++elementDesc.elementSize;
  expectStatus(elementSize, V2ValidationStatus::InvalidSectionSchema,
               "wrong section element size rejects");

  auto wrongOffset = makeChunk(spec);
  auto& offsetDesc = at<D9CCommandChunkWireSectionDescV2>(
      wrongOffset.bytes,
      wrongOffset.header().payloadArenaOffset + sizeof(directDrawHeader()));
  offsetDesc.payloadOffset += 4u;
  expectStatus(wrongOffset, V2ValidationStatus::InvalidSectionRange,
               "noncanonical section payload offset rejects");

  const std::array duplicateSections = {textureSection, textureSection};
  RecordSpec duplicateSpec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = makeDrawPayload(directDrawHeader(), duplicateSections),
      .handles = {handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x500000002ull)},
  };
  expectStatus(makeChunk(duplicateSpec), V2ValidationStatus::InvalidSectionOrder,
               "duplicate section kinds reject");

  auto fullHeader = directDrawHeader();
  fullHeader.flags = D9C_COMMAND_CHUNK_V2_DRAW_FLAG_FULL_SNAPSHOT;
  const RecordSpec fullSpec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = makeDrawPayload(fullHeader,
                                 std::span<const SectionSpec>{}),
  };
  expectStatus(makeChunk(fullSpec), V2ValidationStatus::InvalidFullSnapshot,
               "full snapshot missing texture and stream slots rejects");
}

void testConstantUpAndPaddingRejects() {
  D9CCommandChunkWireConstantRangeV2 constantRange{
      .startRegister = D9C_DRAW_PACKET_MAX_CONST_VS_F - 1u,
      .registerCount = 2u,
  };
  auto constantBytes = bytesOf(constantRange);
  constantBytes.resize(sizeof(constantRange) + 2u * 16u);
  const SectionSpec constantSection{
      .kind = D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_F,
      .count = 2u,
      .bytes = constantBytes,
  };
  D9CCommandChunkWireDrawHeaderV2 apply{};
  const RecordSpec constantSpec{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = makeDrawPayload(apply, std::span(&constantSection, 1u)),
  };
  expectStatus(makeChunk(constantSpec),
               V2ValidationStatus::InvalidConstantRange,
               "constant register overflow rejects");

  auto upHeader = directDrawHeader();
  upHeader.startVertex = 0u;
  upHeader.stride = 4u;
  const SectionSpec shortVertices{
      .kind = D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA,
      .count = 8u,
      .bytes = std::vector<std::byte>(8u),
  };
  const RecordSpec upSpec{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
      .payload = makeDrawPayload(upHeader, std::span(&shortVertices, 1u)),
  };
  expectStatus(makeChunk(upSpec), V2ValidationStatus::InvalidUpData,
               "short UP vertex range rejects");

  D9CCommandChunkWireSetConstV2 boolConst{0u, 1u};
  auto boolPayload = bytesOf(boolConst);
  boolPayload.resize(sizeof(boolConst) + sizeof(std::uint32_t));
  D9CCommandChunkWirePresentV2 present{};
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
  expectStatus(padding, V2ValidationStatus::NonZeroPadding,
               "nonzero inter-record padding rejects");
}

void testFailedValidationDoesNotPublishViewOrAllocatePerRecord() {
  D9CCommandChunkWireUpdateTextureV2 update{0u, 0u};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payload = bytesOf(update),
      .handles = {handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x600000001ull)},
  };
  auto fixture = makeChunk(spec);
  ImportedChunkV2View view;
  V2ValidationScratch scratch;
  check(fixture.validate(&view).valid(), "warm-up validation succeeds");
  const auto capacity = scratch.referencedHandles.capacity();
  const auto result = validateCommandChunkV2(fixture.bytes, fixture.envelope,
                                             &view, scratch);
  check(result.valid(), "explicit reusable scratch validates");
  const auto warmedCapacity = scratch.referencedHandles.capacity();
  check(warmedCapacity >= capacity, "scratch capacity is retained");
  check(validateCommandChunkV2(fixture.bytes, fixture.envelope, &view, scratch)
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
  D9CCommandChunkWirePresentV2 present{};
  const RecordSpec spec{
      .type = D9C_COMMAND_RECORD_PRESENT,
      .payload = bytesOf(present),
  };
  const auto fixture = makeChunk(spec);
  ImportedChunkV2View validated;
  ImportedChunkV2View reconstructed;
  check(fixture.validate(&validated).valid(),
        "prevalidated-view fixture validates");
  check(importPrevalidatedCommandChunkV2(
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
  check(!importPrevalidatedCommandChunkV2(
            fixture.bytes, wrongEnvelope, reconstructed) &&
            reconstructed.empty(),
        "outer count drift cannot reconstruct a prevalidated view");
  check(!importPrevalidatedCommandChunkV2(
            std::span<const std::byte>(fixture.bytes).first(
                fixture.bytes.size() - 1u),
            fixture.envelope, reconstructed) &&
            reconstructed.empty(),
        "truncated storage cannot reconstruct a prevalidated view");
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
  V2ValidationStatus status;
  std::function<void(ChunkFixture&)> mutate;
};

void expectRejectedWithoutSideEffects(const ChunkFixture& fixture,
                                      const MutationCase& mutation,
                                      std::uint32_t seed,
                                      std::uint32_t ordinal) {
  RejectObservers observers;
  ImportedChunkV2View view;
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
  const D9CCommandChunkWireTextureBindingV2 texture{
      .slot = 2u,
      .valid = 1u,
      .handleIndex = 0u,
      .reserved0 = 0u,
  };
  const SectionSpec textureSection{
      .kind = D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE,
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
      -> D9CCommandChunkWireDrawHeaderV2& {
    return at<D9CCommandChunkWireDrawHeaderV2>(
        fixture.bytes,
        fixture.header().payloadArenaOffset + fixture.record(0u).payloadOffset);
  };
  const auto descAt = [](ChunkFixture& fixture)
      -> D9CCommandChunkWireSectionDescV2& {
    return at<D9CCommandChunkWireSectionDescV2>(
        fixture.bytes,
        fixture.header().payloadArenaOffset + fixture.record(0u).payloadOffset +
            sizeof(D9CCommandChunkWireDrawHeaderV2));
  };
  const auto textureAt = [](ChunkFixture& fixture)
      -> D9CCommandChunkWireTextureBindingV2& {
    const auto payloadStart = fixture.header().payloadArenaOffset +
                              fixture.record(0u).payloadOffset;
    const auto offset = at<D9CCommandChunkWireSectionDescV2>(
                            fixture.bytes,
                            payloadStart +
                                sizeof(D9CCommandChunkWireDrawHeaderV2))
                            .payloadOffset;
    return at<D9CCommandChunkWireTextureBindingV2>(fixture.bytes,
                                                   payloadStart + offset);
  };

  const std::array<MutationCase, 34> mutations = {{
      {"outer-version", V2ValidationStatus::OuterVersionMismatch,
       [](ChunkFixture& value) { value.envelope.version = 1u; }},
      {"outer-record-count", V2ValidationStatus::OuterCountMismatch,
       [](ChunkFixture& value) { ++value.envelope.recordCount; }},
      {"outer-handle-count", V2ValidationStatus::OuterCountMismatch,
       [](ChunkFixture& value) { ++value.envelope.handleCount; }},
      {"wire-version", V2ValidationStatus::OuterVersionMismatch,
       [](ChunkFixture& value) { value.header().version = 1u; }},
      {"header-size", V2ValidationStatus::InvalidHeader,
       [](ChunkFixture& value) { ++value.header().headerSize; }},
      {"record-header-size", V2ValidationStatus::InvalidHeader,
       [](ChunkFixture& value) { ++value.header().recordHeaderSize; }},
      {"handle-entry-size", V2ValidationStatus::InvalidHeader,
       [](ChunkFixture& value) { ++value.header().handleEntrySize; }},
      {"record-table-gap", V2ValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) { value.header().recordTableOffset += 4u; }},
      {"record-count-overflow", V2ValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) {
         value.header().recordCount = std::numeric_limits<std::uint32_t>::max();
         value.envelope.recordCount = value.header().recordCount;
       }},
      {"handle-table-gap", V2ValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) { value.header().handleTableOffset += 4u; }},
      {"payload-arena-gap", V2ValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) { value.header().payloadArenaOffset += 4u; }},
      {"payload-arena-tail", V2ValidationStatus::NonCanonicalChunkLayout,
       [](ChunkFixture& value) { ++value.header().payloadArenaSize; }},
      {"header-reserved-0", V2ValidationStatus::NonZeroReserved,
       [](ChunkFixture& value) { value.header().reserved0 = 1u; }},
      {"header-reserved-1", V2ValidationStatus::NonZeroReserved,
       [](ChunkFixture& value) { value.header().reserved1 = 1u; }},
      {"record-type", V2ValidationStatus::InvalidRecordType,
       [](ChunkFixture& value) { value.record(0u).type = 0u; }},
      {"record-flags", V2ValidationStatus::InvalidRecordFlags,
       [](ChunkFixture& value) { value.record(0u).flags = 1u; }},
      {"record-payload-offset", V2ValidationStatus::NonCanonicalPayloadLayout,
       [](ChunkFixture& value) { value.record(0u).payloadOffset = 4u; }},
      {"record-payload-size", V2ValidationStatus::InvalidSectionRange,
       [](ChunkFixture& value) { --value.record(0u).payloadSize; }},
      {"record-first-handle", V2ValidationStatus::NonCanonicalHandleSlice,
       [](ChunkFixture& value) { value.record(0u).firstHandle = 1u; }},
      {"record-handle-count", V2ValidationStatus::InvalidHandleReference,
       [](ChunkFixture& value) { value.record(0u).handleCount = 0u; }},
      {"record-reserved-0", V2ValidationStatus::NonZeroReserved,
       [](ChunkFixture& value) { value.record(0u).reserved0 = 1u; }},
      {"record-reserved-1", V2ValidationStatus::NonZeroReserved,
       [](ChunkFixture& value) { value.record(0u).reserved1 = 1u; }},
      {"handle-kind", V2ValidationStatus::InvalidHandleEntry,
       [](ChunkFixture& value) {
         value.handle(0u).kind = D9C_CHUNK_HANDLE_KIND_QUERY + 1u;
       }},
      {"handle-generation", V2ValidationStatus::InvalidHandleEntry,
       [](ChunkFixture& value) { value.handle(0u).generation = 0u; }},
      {"handle-object-id", V2ValidationStatus::InvalidHandleEntry,
       [](ChunkFixture& value) { value.handle(0u).objectId = 0u; }},
      {"draw-reserved", V2ValidationStatus::InvalidDrawHeader,
       [drawAt](ChunkFixture& value) { drawAt(value).reserved0 = 1u; }},
      {"section-table-offset", V2ValidationStatus::InvalidSectionTable,
       [drawAt](ChunkFixture& value) { ++drawAt(value).sectionTableOffset; }},
      {"section-payload-offset", V2ValidationStatus::InvalidSectionTable,
       [drawAt](ChunkFixture& value) { drawAt(value).sectionPayloadOffset += 4u; }},
      {"descriptor-kind", V2ValidationStatus::InvalidSectionOrder,
       [descAt](ChunkFixture& value) { descAt(value).kind = 0u; }},
      {"descriptor-element-size", V2ValidationStatus::InvalidSectionSchema,
       [descAt](ChunkFixture& value) { ++descAt(value).elementSize; }},
      {"descriptor-count", V2ValidationStatus::InvalidSectionSchema,
       [descAt](ChunkFixture& value) { descAt(value).count = 0u; }},
      {"descriptor-offset", V2ValidationStatus::InvalidSectionRange,
       [descAt](ChunkFixture& value) { descAt(value).payloadOffset += 4u; }},
      {"texture-valid", V2ValidationStatus::InvalidSectionSchema,
       [textureAt](ChunkFixture& value) { textureAt(value).valid = 2u; }},
      {"texture-index", V2ValidationStatus::InvalidHandleReference,
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
    testTableAndSeededMalformedPropertyCorpus();
  } catch (const TestFailure& error) {
    std::cerr << "chunk_record_v2_validation_spec failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_v2_validation_spec passed\n";
  return EXIT_SUCCESS;
}
