#pragma once

#include "device_c_render_tape.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_descriptors.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace dxmt9::d3d9::test {

enum class CompletionTapeFixtureMode : std::uint8_t {
  PlainSingleMutation,
  DynamicTwoMutation,
  ManagedAllocationFailure,
  CapacityContinuation,
};

struct CompletionTapeFixture {
  CompletionTapeFixtureMode mode =
      CompletionTapeFixtureMode::PlainSingleMutation;
  D9CWireObjectIdentity bufferIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
      .generation = 1u,
      .objectId = 1u};
  D9CWireObjectIdentity outputIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = 2u};
  std::vector<std::byte> tape;
  std::vector<std::byte> blob;
  RenderTapeDigest blobDigest{};
  std::vector<std::byte> blobB;
  RenderTapeDigest blobBDigest{};
};

inline std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

inline std::vector<std::byte> singleRecordChunk(
    std::uint32_t type, std::span<const std::byte> payload,
    std::span<const D9CCommandChunkWireHandleEntry> handles = {}) {
  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handleTableOffset = alignUp(
      recordTableOffset + sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset = alignUp(
      handleTableOffset +
          handles.size() * sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = type,
      .payloadSize = static_cast<std::uint32_t>(payload.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
  };
  std::vector<std::byte> bytes(payloadArenaOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordTableOffset, &record, sizeof(record));
  if (!handles.empty()) {
    std::memcpy(bytes.data() + handleTableOffset, handles.data(),
                handles.size_bytes());
  }
  std::memcpy(bytes.data() + payloadArenaOffset, payload.data(), payload.size());
  return bytes;
}

inline std::vector<std::byte> fullStateChunk(
    bool draw, D9CWireObjectIdentity identity = {}) {
  std::array<D9CCommandChunkWireTextureBinding,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = {
        .slot = slot,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  std::array<D9CCommandChunkWireStreamBinding,
             D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = {
        .slot = slot,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  const bool hasBuffer = identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER;
  if (hasBuffer) {
    streams[0].handleIndex = 0u;
    streams[0].stride = 16u;
  }

  constexpr std::uint32_t sectionCount = 2u;
  constexpr std::size_t textureBytes =
      sizeof(D9CCommandChunkWireTextureBinding) * D9C_DRAW_PACKET_MAX_TEXTURES;
  constexpr std::size_t streamBytes =
      sizeof(D9CCommandChunkWireStreamBinding) * D9C_DRAW_PACKET_MAX_STREAMS;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset + sectionCount *
                                sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamOffset = sectionPayloadOffset + textureBytes;
  const D9CCommandChunkWireDrawHeader drawHeader{
      .flags = draw ? 0u : D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT,
      .primitiveType = draw ? 4u : 0u,
      .primitiveCount = draw ? 1u : 0u,
      .sectionCount = sectionCount,
      .sectionTableOffset = static_cast<std::uint32_t>(sectionTableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
  };
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(D9CCommandChunkWireTextureBinding),
          .count = static_cast<std::uint32_t>(textures.size()),
          .payloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
          .byteSize = static_cast<std::uint32_t>(textureBytes)},
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
          .count = static_cast<std::uint32_t>(streams.size()),
          .payloadOffset = static_cast<std::uint32_t>(streamOffset),
          .byteSize = static_cast<std::uint32_t>(streamBytes)}};
  std::vector<std::byte> payload(streamOffset + streamBytes);
  std::memcpy(payload.data(), &drawHeader, sizeof(drawHeader));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(),
              textureBytes);
  std::memcpy(payload.data() + streamOffset, streams.data(), streamBytes);
  const auto handle = D9CCommandChunkWireHandleEntry{
      .kind = identity.kind,
      .generation = identity.generation,
      .objectId = identity.objectId};
  return singleRecordChunk(
      draw ? D9C_COMMAND_RECORD_DRAW_PRIMITIVE
           : D9C_COMMAND_RECORD_APPLY_STATE,
      payload, hasBuffer
                    ? std::span<const D9CCommandChunkWireHandleEntry>(&handle,
                                                                        1u)
                    : std::span<const D9CCommandChunkWireHandleEntry>{});
}

inline std::vector<std::byte> drawIslandChunk(
    std::size_t drawCount, D9CWireObjectIdentity identity,
    std::uint32_t firstVertex) {
  if (drawCount == 0u || drawCount > 128u) {
    throw std::runtime_error("invalid draw-island size");
  }
  const auto one = fullStateChunk(true, identity);
  D9CCommandChunkWireHeader sourceHeader{};
  std::memcpy(&sourceHeader, one.data(), sizeof(sourceHeader));
  D9CCommandChunkWireRecordHeader sourceRecord{};
  std::memcpy(&sourceRecord,
              one.data() + sourceHeader.recordTableOffset,
              sizeof(sourceRecord));
  const auto sourcePayload = std::span<const std::byte>(
      one.data() + sourceHeader.payloadArenaOffset +
          sourceRecord.payloadOffset,
      sourceRecord.payloadSize);

  std::vector<D9CCommandChunkWireRecordHeader> records;
  records.reserve(drawCount);
  const auto handle = D9CCommandChunkWireHandleEntry{
      .kind = identity.kind,
      .generation = identity.generation,
      .objectId = identity.objectId};
  // Canonical wire partitions the handle table into record-local slices.
  // Repeated identities may not alias another record's table interval.
  const std::vector<D9CCommandChunkWireHandleEntry> handles(drawCount, handle);
  std::vector<std::byte> payload;
  payload.reserve(drawCount * sourcePayload.size());
  for (std::size_t index = 0u; index < drawCount; ++index) {
    const auto offset = alignUp(payload.size(), alignof(std::uint32_t));
    payload.resize(offset);
    const auto payloadOffset = payload.size();
    payload.insert(payload.end(), sourcePayload.begin(), sourcePayload.end());
    D9CCommandChunkWireDrawHeader draw{};
    std::memcpy(&draw, payload.data() + payloadOffset, sizeof(draw));
    draw.startVertex = firstVertex + static_cast<std::uint32_t>(index);
    std::memcpy(payload.data() + payloadOffset, &draw, sizeof(draw));
    D9CCommandChunkWireSectionDesc streamSection{};
    std::memcpy(&streamSection,
                payload.data() + payloadOffset + draw.sectionTableOffset +
                    sizeof(D9CCommandChunkWireSectionDesc),
                sizeof(streamSection));
    D9CCommandChunkWireStreamBinding stream{};
    std::memcpy(&stream,
                payload.data() + payloadOffset + streamSection.payloadOffset,
                sizeof(stream));
    // References use table-global indices inside each record's slice.
    stream.handleIndex = static_cast<std::uint32_t>(index);
    std::memcpy(payload.data() + payloadOffset + streamSection.payloadOffset,
                &stream, sizeof(stream));
    records.push_back({
        .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
        .flags = 0u,
        .payloadOffset = static_cast<std::uint32_t>(payloadOffset),
        .payloadSize = static_cast<std::uint32_t>(sourcePayload.size()),
        .firstHandle = static_cast<std::uint32_t>(index),
        .handleCount = 1u,
        .reserved0 = 0u,
        .reserved1 = 0u});
  }
  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handleTableOffset = alignUp(
      recordTableOffset + records.size() * sizeof(records[0]),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset = alignUp(
      handleTableOffset + handles.size() * sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
      .reserved0 = 0u,
      .reserved1 = 0u};
  std::vector<std::byte> result(payloadArenaOffset + payload.size(),
                                std::byte{0});
  std::memcpy(result.data(), &header, sizeof(header));
  std::memcpy(result.data() + recordTableOffset, records.data(),
              records.size() * sizeof(records[0]));
  std::memcpy(result.data() + handleTableOffset, handles.data(),
              handles.size() * sizeof(handles[0]));
  std::memcpy(result.data() + payloadArenaOffset, payload.data(),
              payload.size());
  return result;
}

inline std::vector<std::byte> presentChunk() {
  const D9CCommandChunkWirePresent present{};
  return singleRecordChunk(D9C_COMMAND_RECORD_PRESENT,
                           std::as_bytes(std::span(&present, 1u)));
}

inline CompletionTapeFixture makeCompletionTapeFixture(
    CompletionTapeFixtureMode mode, bool includeUnbind = true) {
  CompletionTapeFixture fixture;
  fixture.mode = mode;
  const bool dynamic = mode == CompletionTapeFixtureMode::DynamicTwoMutation;
  const bool managedAllocationFailure =
      mode == CompletionTapeFixtureMode::ManagedAllocationFailure;
  const bool capacityContinuation = mode == CompletionTapeFixtureMode::CapacityContinuation;
  fixture.blob.resize(256u);
  for (std::size_t index = 0u; index < fixture.blob.size(); ++index) {
    fixture.blob[index] = static_cast<std::byte>(index & 0xffu);
  }
  fixture.blobDigest = RenderTapeCaptureSession::sha256(
      std::span<const std::byte>(fixture.blob));
  if (dynamic) {
    fixture.blobB.resize(fixture.blob.size());
    for (std::size_t index = 0u; index < fixture.blobB.size(); ++index) {
      fixture.blobB[index] = static_cast<std::byte>((0xa5u + index) & 0xffu);
    }
    fixture.blobBDigest = RenderTapeCaptureSession::sha256(
        std::span<const std::byte>(fixture.blobB));
  }
  // D3DUSAGE_DYNAMIC and D3DPOOL_DEFAULT are wire-level constants here so
  // the native fixture does not depend on the Windows SDK.
  constexpr std::uint32_t kD3DUsageDynamic = 0x200u;
  const D9CBufferDesc descriptor{
      .size = 256u,
      .usage = dynamic ? kD3DUsageDynamic : 0u,
      .pool = managedAllocationFailure ? 1u : 0u};
  const RenderTapeSurfaceDescriptorV2 outputDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = D9CSurfaceDesc{
          .format = 21u,
          .resourceType = 1u,
          .usage = 1u,
          .pool = 0u,
          .width = 4u,
          .height = 4u,
          .depth = 1u}};
  const RenderTapeOracleAttachment oracle{
      .identity = fixture.outputIdentity,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface)};
  const auto bootstrap = fullStateChunk(false);
  const auto draw = fullStateChunk(true, fixture.bufferIdentity);
  const auto drawIslandA = drawIslandChunk(7u, fixture.bufferIdentity, 0u);
  const auto drawIslandB = drawIslandChunk(2u, fixture.bufferIdentity, 7u);
  const auto unbind = fullStateChunk(false);
  const auto present = presentChunk();
  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrap);
  builder.appendObjectDefine(
      fixture.bufferIdentity,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
      std::as_bytes(std::span(&descriptor, 1u)), 0u, {},
      fixture.blob.size(), 1u);
  builder.appendObjectDefine(
      fixture.outputIdentity,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  builder.appendResourceMutation(
      fixture.bufferIdentity, RenderTapeMutationKind::CpuUnlock, 0u, 0u,
      fixture.blob.size(), fixture.blobDigest,
      RenderTapeBufferMutationDisposition::Plain);
  if (capacityContinuation) {
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 7u, .handleCount = 7u},
        drawIslandA);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 2u, .handleCount = 2u},
        drawIslandB);
  } else {
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, draw);
  }
  if (dynamic) {
    builder.appendResourceMutation(
        fixture.bufferIdentity, RenderTapeMutationKind::CpuUnlock, 0u, 0u,
        fixture.blobB.size(), fixture.blobBDigest,
        RenderTapeBufferMutationDisposition::Discard);
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, draw);
  }
  if (includeUnbind) {
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, unbind);
  }
  builder.appendObjectDestroy(fixture.bufferIdentity);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  builder.appendPresentComplete(
      capacityContinuation
          ? (includeUnbind ? 9u : 8u)
          : (dynamic ? (includeUnbind ? 10u : 9u)
                     : (includeUnbind ? 8u : 7u)),
      1u,
      RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  fixture.tape = builder.seal();
  RenderTapeBlobCatalogue catalogue;
  catalogue.blobs.push_back({.digest = fixture.blobDigest,
                             .size = fixture.blob.size(),
                             .verified = 1u});
  if (dynamic) {
    catalogue.blobs.push_back({.digest = fixture.blobBDigest,
                               .size = fixture.blobB.size(),
                               .verified = 1u});
  }
  ImportedRenderTapeView imported;
  const auto validation =
      validateRenderTape(fixture.tape, catalogue, &imported);
  if (!validation.valid()) {
    throw std::runtime_error(
        std::string("completion fixture validation: ") +
        renderTapeValidationStatusName(validation.status) +
        " event=" + std::to_string(validation.failedEventIndex));
  }
  return fixture;
}

inline CompletionTapeFixture makeCompletionTapeFixture(bool includeUnbind = true) {
  return makeCompletionTapeFixture(CompletionTapeFixtureMode::PlainSingleMutation,
                                   includeUnbind);
}

inline CompletionTapeFixture makeDynamicCompletionTapeFixture(
    bool includeUnbind = true) {
  return makeCompletionTapeFixture(CompletionTapeFixtureMode::DynamicTwoMutation,
                                   includeUnbind);
}

inline CompletionTapeFixture makeManagedAllocationFailureTapeFixture(
    bool includeUnbind = true) {
  return makeCompletionTapeFixture(
      CompletionTapeFixtureMode::ManagedAllocationFailure, includeUnbind);
}

inline CompletionTapeFixture makeCapacityContinuationTapeFixture(
    bool includeUnbind = true) {
  return makeCompletionTapeFixture(CompletionTapeFixtureMode::CapacityContinuation,
                                   includeUnbind);
}

inline void writeCapacityContinuationTapeFixture(
    const std::filesystem::path& directory, bool includeUnbind = true) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) throw std::runtime_error("cannot create capacity fixture directory");
  const auto fixture = makeCapacityContinuationTapeFixture(includeUnbind);
  std::ofstream events(directory / "events.bin", std::ios::binary);
  std::ofstream blob(directory / "blob.bin", std::ios::binary);
  if (!events || !blob) throw std::runtime_error("cannot open capacity fixture");
  events.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  blob.write(reinterpret_cast<const char*>(fixture.blob.data()),
             static_cast<std::streamsize>(fixture.blob.size()));
  if (!events || !blob) throw std::runtime_error("cannot write capacity fixture");
}

inline void writeCompletionTapeFixture(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) throw std::runtime_error("cannot create completion fixture directory");
  const auto fixture = makeCompletionTapeFixture();
  std::ofstream events(directory / "events.bin", std::ios::binary);
  std::ofstream blob(directory / "blob.bin", std::ios::binary);
  if (!events || !blob) throw std::runtime_error("cannot open completion fixture");
  events.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  blob.write(reinterpret_cast<const char*>(fixture.blob.data()),
             static_cast<std::streamsize>(fixture.blob.size()));
  if (!events || !blob) throw std::runtime_error("cannot write completion fixture");
}

inline void writeDynamicCompletionTapeFixture(
    const std::filesystem::path& directory, bool includeUnbind = true) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) throw std::runtime_error("cannot create dynamic fixture directory");
  const auto fixture = makeDynamicCompletionTapeFixture(includeUnbind);
  std::ofstream events(directory / "events.bin", std::ios::binary);
  std::ofstream blobA(directory / "blob_a.bin", std::ios::binary);
  std::ofstream blobB(directory / "blob_b.bin", std::ios::binary);
  if (!events || !blobA || !blobB) {
    throw std::runtime_error("cannot open dynamic completion fixture");
  }
  events.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  blobA.write(reinterpret_cast<const char*>(fixture.blob.data()),
              static_cast<std::streamsize>(fixture.blob.size()));
  blobB.write(reinterpret_cast<const char*>(fixture.blobB.data()),
              static_cast<std::streamsize>(fixture.blobB.size()));
  if (!events || !blobA || !blobB) {
    throw std::runtime_error("cannot write dynamic completion fixture");
  }
}

inline void writeManagedAllocationFailureTapeFixture(
    const std::filesystem::path& directory, bool includeUnbind = true) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) throw std::runtime_error("cannot create managed fault fixture directory");
  const auto fixture = makeManagedAllocationFailureTapeFixture(includeUnbind);
  std::ofstream events(directory / "events.bin", std::ios::binary);
  std::ofstream blob(directory / "blob.bin", std::ios::binary);
  if (!events || !blob) throw std::runtime_error("cannot open managed fault fixture");
  events.write(reinterpret_cast<const char*>(fixture.tape.data()),
               static_cast<std::streamsize>(fixture.tape.size()));
  blob.write(reinterpret_cast<const char*>(fixture.blob.data()),
             static_cast<std::streamsize>(fixture.blob.size()));
  if (!events || !blob) throw std::runtime_error("cannot write managed fault fixture");
}

}  // namespace dxmt9::d3d9::test
