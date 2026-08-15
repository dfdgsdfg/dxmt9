#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_identity.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_render_tape_capture.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_presenter.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

static_assert(sizeof(D9CRenderTapePresentCaptureResult) == 56u);
static_assert(alignof(D9CRenderTapePresentCaptureResult) == alignof(uint64_t));
static_assert(std::is_standard_layout_v<D9CRenderTapePresentCaptureResult>);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, status) == 0u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, width) == 4u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, height) == 8u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, format) == 12u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, byteCount) == 16u);
static_assert(offsetof(D9CRenderTapePresentCaptureResult, sha256) == 24u);
static_assert(sizeof(D9CRenderTapeIdentityCaptureResult) == 32u);
static_assert(sizeof(D9CRenderTapeIdentitySourceEntry) == 48u);
static_assert(sizeof(D9CRenderTapeIdentityRangeEntry) == 48u);
static_assert(std::is_standard_layout_v<D9CRenderTapeIdentityCaptureResult>);
static_assert(std::is_standard_layout_v<D9CRenderTapeIdentitySourceEntry>);
static_assert(std::is_standard_layout_v<D9CRenderTapeIdentityRangeEntry>);

using namespace dxmt9::d3d9;

using dxmt9::d3d9::pe::CommandChunkBuilder;
using dxmt9::d3d9::pe::PeWireObjectRef;

struct RecorderSurface {
  std::uint32_t refs = 1u;
};

extern "C" void dxmt9c_surface_addref(D9CSurface* value) {
  ++reinterpret_cast<RecorderSurface*>(value)->refs;
}

extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) {
  return --reinterpret_cast<RecorderSurface*>(value)->refs;
}

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
  std::vector<std::byte> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

std::vector<std::byte> singleRecordChunk(
    std::uint32_t type, std::span<const std::byte> payload) {
  const auto recordsOffset = sizeof(D9CCommandChunkWireHeader);
  const auto payloadOffset = alignUp(
      recordsOffset + sizeof(D9CCommandChunkWireRecordHeader),
      alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordsOffset),
      .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(payloadOffset),
      .handleCount = 0u,
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = type,
      .payloadSize = static_cast<std::uint32_t>(payload.size()),
      .handleCount = 0u,
  };
  std::vector<std::byte> result(payloadOffset + payload.size());
  std::memcpy(result.data(), &header, sizeof(header));
  std::memcpy(result.data() + recordsOffset, &record, sizeof(record));
  std::memcpy(result.data() + payloadOffset, payload.data(), payload.size());
  return result;
}

std::vector<std::byte> singleSurfaceColorFillChunk(
    const D9CWireObjectIdentity& identity) {
  const auto recordsOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handlesOffset = alignUp(
      recordsOffset + sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadOffset = alignUp(
      handlesOffset + sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordsOffset),
      .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(handlesOffset),
      .handleCount = 1u,
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaSize = sizeof(D9CCommandChunkWireColorFill),
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = D9C_COMMAND_RECORD_COLOR_FILL,
      .payloadSize = sizeof(D9CCommandChunkWireColorFill),
      .firstHandle = 0u,
      .handleCount = 1u,
  };
  const D9CCommandChunkWireHandleEntry handle{
      .kind = identity.kind,
      .generation = identity.generation,
      .objectId = identity.objectId,
  };
  const D9CCommandChunkWireColorFill fill{
      .surfaceHandleIndex = 0u,
  };
  std::vector<std::byte> result(
      payloadOffset + sizeof(D9CCommandChunkWireColorFill));
  std::memcpy(result.data(), &header, sizeof(header));
  std::memcpy(result.data() + recordsOffset, &record, sizeof(record));
  std::memcpy(result.data() + handlesOffset, &handle, sizeof(handle));
  std::memcpy(result.data() + payloadOffset, &fill, sizeof(fill));
  return result;
}

std::vector<std::byte> singleUpdateTextureChunk(
    const D9CWireObjectIdentity& source,
    const D9CWireObjectIdentity& destination) {
  const auto recordsOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handlesOffset = alignUp(
      recordsOffset + sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadOffset = alignUp(
      handlesOffset + 2u * sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordsOffset),
      .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(handlesOffset),
      .handleCount = 2u,
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaSize = sizeof(D9CCommandChunkWireUpdateTexture),
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .payloadSize = sizeof(D9CCommandChunkWireUpdateTexture),
      .firstHandle = 0u,
      .handleCount = 2u,
  };
  const std::array handles{
      D9CCommandChunkWireHandleEntry{
          .kind = source.kind,
          .generation = source.generation,
          .objectId = source.objectId,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = destination.kind,
          .generation = destination.generation,
          .objectId = destination.objectId,
      },
  };
  const D9CCommandChunkWireUpdateTexture update{
      .srcHandleIndex = 0u,
      .dstHandleIndex = 1u,
  };
  std::vector<std::byte> result(
      payloadOffset + sizeof(D9CCommandChunkWireUpdateTexture));
  std::memcpy(result.data(), &header, sizeof(header));
  std::memcpy(result.data() + recordsOffset, &record, sizeof(record));
  std::memcpy(result.data() + handlesOffset, handles.data(), sizeof(handles));
  std::memcpy(result.data() + payloadOffset, &update, sizeof(update));
  return result;
}

std::vector<std::byte> bootstrapChunk() {
  std::array<D9CCommandChunkWireTextureBinding,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t i = 0u; i < textures.size(); ++i) {
    textures[i] = D9CCommandChunkWireTextureBinding{
        .slot = i,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  for (std::uint32_t i = 0u; i < streams.size(); ++i) {
    streams[i] = D9CCommandChunkWireStreamBinding{
        .slot = i,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  constexpr std::uint32_t sectionCount = 2u;
  constexpr std::size_t textureBytes =
      sizeof(D9CCommandChunkWireTextureBinding) * D9C_DRAW_PACKET_MAX_TEXTURES;
  constexpr std::size_t streamBytes =
      sizeof(D9CCommandChunkWireStreamBinding) * D9C_DRAW_PACKET_MAX_STREAMS;
  const auto tableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto payloadOffset = alignUp(
      tableOffset + sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  const auto streamsOffset = payloadOffset + textureBytes;
  const D9CCommandChunkWireDrawHeader draw{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT,
      .sectionCount = sectionCount,
      .sectionTableOffset = static_cast<std::uint32_t>(tableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(payloadOffset),
  };
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(D9CCommandChunkWireTextureBinding),
          .count = D9C_DRAW_PACKET_MAX_TEXTURES,
          .payloadOffset = static_cast<std::uint32_t>(payloadOffset),
          .byteSize = static_cast<std::uint32_t>(textureBytes),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
          .count = D9C_DRAW_PACKET_MAX_STREAMS,
          .payloadOffset = static_cast<std::uint32_t>(streamsOffset),
          .byteSize = static_cast<std::uint32_t>(streamBytes),
      },
  };
  std::vector<std::byte> payload(streamsOffset + streamBytes);
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + tableOffset, sections.data(), sizeof(sections));
  std::memcpy(payload.data() + payloadOffset, textures.data(), textureBytes);
  std::memcpy(payload.data() + streamsOffset, streams.data(), streamBytes);
  return singleRecordChunk(D9C_COMMAND_RECORD_APPLY_STATE, payload);
}

std::vector<std::byte> presentChunk() {
  const D9CCommandChunkWirePresent present{};
  return singleRecordChunk(D9C_COMMAND_RECORD_PRESENT,
                           std::as_bytes(std::span(&present, 1u)));
}

constexpr D9CWireObjectIdentity kProducedTexture{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 9u,
    .objectId = 43u,
};
constexpr D9CWireObjectIdentity kProducedAlias{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 7u,
    .objectId = 41u,
};
constexpr D9CWireObjectIdentity kProducedOutput{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 7u,
    .objectId = 40u,
};
constexpr D9CWireObjectIdentity kProducedDepth{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 7u,
    .objectId = 45u,
};

RenderTapeSurfaceDescriptorV2 producedDepthDescriptor() {
  return {
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::Standalone),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedByCapturedPass),
      .surface = D9CSurfaceDesc{
          .format = 77u, .resourceType = 1u, .usage = 2u, .pool = 0u,
          .width = 4u, .height = 4u, .depth = 1u,
      },
  };
}

std::vector<std::byte> producedTextureDescriptor() {
  RenderTapeTextureDescriptorV2 header{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u,
      .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedByCapturedPass),
  };
  const D9CSurfaceDesc level{
      .format = 22u,
      .resourceType = 3u,
      .usage = 1u,
      .pool = 0u,
      .width = 4u,
      .height = 4u,
      .depth = 1u,
  };
  std::vector<std::byte> descriptor(sizeof(header) + sizeof(level));
  std::memcpy(descriptor.data(), &header, sizeof(header));
  std::memcpy(descriptor.data() + sizeof(header), &level, sizeof(level));
  return descriptor;
}

std::vector<std::byte> producedAliasDescriptor() {
  const RenderTapeSurfaceDescriptorV2 alias{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = kProducedTexture,
      .surface = D9CSurfaceDesc{
          .format = 22u,
          .resourceType = 1u,
          .usage = 1u,
          .pool = 0u,
          .width = 4u,
          .height = 4u,
          .depth = 1u,
      },
  };
  return std::vector<std::byte>(
      reinterpret_cast<const std::byte*>(&alias),
      reinterpret_cast<const std::byte*>(&alias) + sizeof(alias));
}

struct ProducedRecord {
  std::uint32_t type = 0u;
  std::vector<std::byte> payload{};
  std::vector<D9CCommandChunkWireHandleEntry> handles{};
};

std::vector<std::byte> makeProducedChunk(
    std::span<const ProducedRecord> specs) {
  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  std::size_t handleCount = 0u;
  for (const auto& spec : specs) handleCount += spec.handles.size();
  const auto handleTableOffset = alignUp(
      recordTableOffset + specs.size() * sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset = alignUp(
      handleTableOffset + handleCount * sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  std::vector<std::byte> payload;
  for (const auto& spec : specs) {
    const auto* rule = recordRule(spec.type);
    if (!rule) return {};
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back(D9CCommandChunkWireRecordHeader{
        .type = spec.type,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
    });
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
  }
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
  };
  std::vector<std::byte> bytes(payloadArenaOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordTableOffset, records.data(),
              records.size() * sizeof(records[0]));
  if (!handles.empty()) {
    std::memcpy(bytes.data() + handleTableOffset, handles.data(),
                handles.size() * sizeof(handles[0]));
  }
  std::memcpy(bytes.data() + payloadArenaOffset, payload.data(), payload.size());
  return bytes;
}

std::vector<std::byte> producedFullClearChunk(bool partial = false,
                                              bool drawFirst = false,
                                              bool wrongIdentity = false,
                                              bool omitAlias = false,
                                              D9CWireObjectIdentity aliasIdentity =
                                                  kProducedAlias,
                                              bool includeDepth = false,
                                              D9CWireObjectIdentity depthIdentity =
                                                  kProducedDepth,
                                              bool omitPresent = false,
                                              bool bindParentTexture = false,
                                              bool clearDepthAspect = true) {
  const bool bindAlias = !omitAlias;
  const auto target = wrongIdentity
      ? D9CWireObjectIdentity{.kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
                              .generation = 7u,
                              .objectId = 99u}
      : aliasIdentity;
  const D9CCommandChunkWireRenderTargetBinding renderTarget{
      .slot = 0u,
      .valid = bindAlias ? 1u : 0u,
      .handleIndex = bindAlias ? 0u : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  const D9CCommandChunkWireDepthStencilBinding depthStencil{
      .valid = includeDepth ? 1u : 0u,
      .handleIndex = includeDepth ? 1u
                                  : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
  };
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = {.slot = slot, .valid = 1u,
                      .handleIndex = bindParentTexture && slot == 0u
                          ? 1u
                          : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = {.slot = slot, .valid = 1u,
                     .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  }
  constexpr std::uint32_t sectionCount = 4u;
  const D9CCommandChunkWireDrawHeader apply{
      .flags = 0u,
      .sectionCount = sectionCount,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
          sizeof(D9CCommandChunkWireDrawHeader) +
              sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
          alignof(std::uint32_t))),
  };
  const auto streamOffset = apply.sectionPayloadOffset + sizeof(textures);
  const auto renderTargetOffset = streamOffset + sizeof(streams);
  const auto depthStencilOffset = renderTargetOffset + sizeof(renderTarget);
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(textures[0]),
          .count = static_cast<std::uint32_t>(textures.size()),
          .payloadOffset = apply.sectionPayloadOffset,
          .byteSize = sizeof(textures),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(streams[0]),
          .count = static_cast<std::uint32_t>(streams.size()),
          .payloadOffset = static_cast<std::uint32_t>(streamOffset),
          .byteSize = sizeof(streams),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
          .elementSize = sizeof(renderTarget),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(renderTargetOffset),
          .byteSize = sizeof(renderTarget),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
          .elementSize = sizeof(depthStencil),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(depthStencilOffset),
          .byteSize = sizeof(depthStencil),
      },
  };
  std::vector<std::byte> applyPayload(
      depthStencilOffset + sizeof(depthStencil));
  std::memcpy(applyPayload.data(), &apply, sizeof(apply));
  std::memcpy(applyPayload.data() + apply.sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(applyPayload.data() + apply.sectionPayloadOffset, textures.data(),
              sizeof(textures));
  std::memcpy(applyPayload.data() + streamOffset, streams.data(),
              sizeof(streams));
  std::memcpy(applyPayload.data() + renderTargetOffset, &renderTarget,
              sizeof(renderTarget));
  std::memcpy(applyPayload.data() + depthStencilOffset, &depthStencil,
              sizeof(depthStencil));
  const D9CCommandChunkWireClear clear{
      .flags = includeDepth && clearDepthAspect ? 3u : 1u,
      .colorARGB = 0xff204060u,
      .z = 1.0f,
      .rectCount = partial ? 1u : 0u,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  const D9CRect rect{0, 0, 2, 2};
  std::vector<std::byte> clearPayload = bytesOf(clear);
  if (partial) {
    const auto rectBytes = std::as_bytes(std::span(&rect, 1u));
    clearPayload.insert(clearPayload.end(), rectBytes.begin(), rectBytes.end());
  }
  auto drawPayload = applyPayload;
  D9CCommandChunkWireDrawHeader drawHeader{};
  std::memcpy(&drawHeader, drawPayload.data(), sizeof(drawHeader));
  drawHeader.primitiveType = 4u;
  drawHeader.primitiveCount = 1u;
  std::memcpy(drawPayload.data(), &drawHeader, sizeof(drawHeader));
  const std::array clearFirstRecords{
      ProducedRecord{.type = D9C_COMMAND_RECORD_APPLY_STATE,
                     .payload = applyPayload,
                     .handles = bindAlias
                         ? includeDepth
                               ? std::vector<D9CCommandChunkWireHandleEntry>{
                                     {target.kind, target.generation,
                                      target.objectId},
                                     {depthIdentity.kind,
                                      depthIdentity.generation,
                                      depthIdentity.objectId}}
                               : bindParentTexture
                                   ? std::vector<
                                         D9CCommandChunkWireHandleEntry>{
                                         {target.kind, target.generation,
                                          target.objectId},
                                         {kProducedTexture.kind,
                                          kProducedTexture.generation,
                                          kProducedTexture.objectId}}
                               : std::vector<D9CCommandChunkWireHandleEntry>{
                                     {target.kind, target.generation,
                                      target.objectId}}
                         : std::vector<D9CCommandChunkWireHandleEntry>{}},
      ProducedRecord{.type = D9C_COMMAND_RECORD_CLEAR,
                     .payload = clearPayload},
      ProducedRecord{.type = D9C_COMMAND_RECORD_PRESENT,
                     .payload = bytesOf(D9CCommandChunkWirePresent{})},
  };
  if (!drawFirst) {
    return makeProducedChunk(
        omitPresent
            ? std::span<const ProducedRecord>(clearFirstRecords).first(2u)
            : std::span<const ProducedRecord>(clearFirstRecords));
  }
  const std::array drawFirstRecords{
      ProducedRecord{.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                     .payload = drawPayload,
                     .handles = bindAlias
                         ? std::vector<D9CCommandChunkWireHandleEntry>{
                               {target.kind, target.generation, target.objectId}}
                         : std::vector<D9CCommandChunkWireHandleEntry>{}},
      ProducedRecord{.type = D9C_COMMAND_RECORD_CLEAR,
                     .payload = clearPayload},
      ProducedRecord{.type = D9C_COMMAND_RECORD_PRESENT,
                     .payload = bytesOf(D9CCommandChunkWirePresent{})},
  };
  return makeProducedChunk(drawFirstRecords);
}

constexpr D9CWireObjectIdentity kSurface{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 2u,
    .objectId = 17u,
};

RenderTapeDigest digest() {
  RenderTapeDigest value{};
  for (std::size_t i = 0u; i < value.size(); ++i) {
    value[i] = static_cast<std::byte>(i + 1u);
  }
  return value;
}

RenderTapeBlob blob() {
  return RenderTapeBlob{.digest = digest(), .size = 4u, .verified = 1u};
}

RenderTapeOrderedControlHeader flushControl() {
  return RenderTapeOrderedControlHeader{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::FlushWait),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = sizeof(RenderTapeFlushWaitControl),
      .completionOrdinal = 10u,
  };
}

RenderTapeOracleAttachment oracle() {
  return RenderTapeOracleAttachment{
      .identity = kSurface,
      .descriptorKind = static_cast<std::uint32_t>(
      RenderTapeDescriptorKind::Surface)};
}

RenderTapeSurfaceDescriptorV2 outputSurfaceDescriptor();

void testProducedByCapturedPassCaptureEndToEnd() {
  const auto outputDescriptorBytes = outputSurfaceDescriptor();
  const auto textureDescriptorBytes = producedTextureDescriptor();
  const auto aliasDescriptorBytes = producedAliasDescriptor();
  const auto flush = flushControl();
  const RenderTapeFlushWaitControl wait{.waitedSeqId = 9u};
  const RenderTapeOracleAttachment outputOracle{
      .identity = kProducedOutput,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };

  auto capture = [&](std::span<const std::byte> chunk,
                     std::span<const std::byte> textureDescriptor,
                     bool secondAlias = false,
                     bool secondProduced = false,
                     std::uint32_t chunkHandleCount = 1u,
                     bool standaloneDepth = false,
                     RenderTapeValidationResult* validationOut = nullptr,
                     std::uint32_t secondAliasSubresource = 0u,
                     std::span<const std::byte> primaryAliasDescriptor = {},
                     std::uint32_t chunkRecordCount = 3u) {
    const auto selectedAliasDescriptor = primaryAliasDescriptor.empty()
        ? std::span<const std::byte>(aliasDescriptorBytes)
        : primaryAliasDescriptor;
    RenderTapeCaptureSession session(true);
    check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() ==
                  RenderTapeCaptureStatus::Accepted,
          "ProducedByCapturedPass capture fixture starts");
    check(session.objectDefine(
              kProducedOutput,
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
              std::as_bytes(std::span(&outputDescriptorBytes, 1u)), 0u,
              {}) == RenderTapeCaptureStatus::Accepted,
          "ProducedByCapturedPass capture defines output");
    check(session.objectDefine(
              kProducedTexture,
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
              textureDescriptor, 0u, {}, 0u, 0u) ==
              RenderTapeCaptureStatus::Accepted,
          "ProducedByCapturedPass capture defines zero-seed texture");
    check(session.objectDefine(
              kProducedAlias,
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
              selectedAliasDescriptor, 0u, {}, 0u, 0u) ==
              RenderTapeCaptureStatus::Accepted,
          "ProducedByCapturedPass capture defines texture alias");
    if (secondAlias) {
      const D9CWireObjectIdentity alias2{
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = 7u,
          .objectId = 42u,
      };
      std::vector<std::byte> alias2Descriptor(selectedAliasDescriptor.begin(),
                                              selectedAliasDescriptor.end());
      RenderTapeSurfaceDescriptorV2 alias2Value{};
      std::memcpy(&alias2Value, alias2Descriptor.data(), sizeof(alias2Value));
      alias2Value.parentTexture = kProducedTexture;
      alias2Value.subresource = secondAliasSubresource;
      std::memcpy(alias2Descriptor.data(), &alias2Value, sizeof(alias2Value));
      const auto aliasStatus = session.objectDefine(
          alias2, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
          alias2Descriptor, 0u, {}, 0u, 0u);
      if (aliasStatus != RenderTapeCaptureStatus::Accepted)
        return std::pair{aliasStatus, session.sealedArtifact().empty()};
    }
    if (secondProduced) {
      const D9CWireObjectIdentity texture2{
          .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
          .generation = 9u,
          .objectId = 44u,
      };
      check(session.objectDefine(
                texture2,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
                textureDescriptor, 0u, {}, 0u, 0u) ==
                RenderTapeCaptureStatus::Accepted,
            "multiple Produced fixture defines the second texture");
    }
    if (standaloneDepth) {
      const auto depth = producedDepthDescriptor();
      check(session.objectDefine(
                kProducedDepth,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                std::as_bytes(std::span(&depth, 1u)), 0u, {}, 0u, 0u) ==
                RenderTapeCaptureStatus::Accepted,
            "multiple Produced fixture defines standalone D24X8");
    }
    if (!chunk.empty()) {
      ImportedChunkView imported;
      const auto chunkValidation = validateCommandChunk(
          chunk, CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                      .recordCount = chunkRecordCount,
                                      .handleCount = chunkHandleCount},
          &imported);
      check(chunkValidation.valid(),
            "ProducedByCapturedPass fixture chunk is canonically encoded");
      check(session.commandChunk(
                CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                     .recordCount = chunkRecordCount,
                                     .handleCount = chunkHandleCount},
                chunk) == RenderTapeCaptureStatus::Accepted,
            "ProducedByCapturedPass capture retains the command chunk");
    }
    check(session.orderedControl(flush, std::as_bytes(std::span(&wait, 1u))) ==
              RenderTapeCaptureStatus::Accepted,
          "ProducedByCapturedPass capture records completion fence");
    const auto status = session.completePresent(
        standaloneDepth || secondAlias || secondProduced ? 6u : 5u, 11u,
        RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&outputOracle, 1u)));
    if (validationOut)
      *validationOut = session.validationResult();
    return std::pair{status, session.sealedArtifact().empty()};
  };

  const auto positive = capture(producedFullClearChunk(), textureDescriptorBytes);
  check(positive.first == RenderTapeCaptureStatus::Complete && !positive.second,
        "capture publishes Produced texture, alias, and the same full-clear chunk");
  RenderTapeCaptureSession later2d(true);
  check(later2d.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            later2d.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted &&
            later2d.objectDefine(
                kProducedOutput,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                std::as_bytes(std::span(&outputDescriptorBytes, 1u)), 0u,
                {}) == RenderTapeCaptureStatus::Accepted &&
            later2d.objectDefine(
                kProducedTexture,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
                textureDescriptorBytes, 0u, {}, 0u, 0u) ==
                RenderTapeCaptureStatus::Accepted &&
            later2d.objectDefine(
                kProducedAlias,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                aliasDescriptorBytes, 0u, {}, 0u, 0u) ==
                RenderTapeCaptureStatus::Accepted,
        "later-access 2D fixture defines the exact one-subresource closure");
  const auto first2d = producedFullClearChunk(
      false, false, false, false, kProducedAlias, false, kProducedDepth, true);
  const auto laterParent2d = producedFullClearChunk(
      false, false, false, false, kProducedAlias, false, kProducedDepth, false,
      true);
  check(later2d.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 2u, .handleCount = 1u},
            first2d) == RenderTapeCaptureStatus::Accepted &&
            later2d.commandChunk(
                CommandChunkEnvelope{
                    .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                    .recordCount = 3u, .handleCount = 2u},
                laterParent2d) == RenderTapeCaptureStatus::Accepted &&
            later2d.orderedControl(
                flush, std::as_bytes(std::span(&wait, 1u))) ==
                RenderTapeCaptureStatus::Accepted &&
            later2d.completePresent(
                6u, 11u, RenderTapeDigestValidity::NotCaptured, {},
                std::as_bytes(std::span(&outputOracle, 1u))) ==
                RenderTapeCaptureStatus::Complete,
        "a fully-produced 2D subresource permits later direct parent sampling");
  RenderTapeValidationResult multiValidation{};
  const auto multiObligation = capture(
      producedFullClearChunk(false, false, false, false, kProducedAlias, true),
      textureDescriptorBytes, false, false, 2u, true, &multiValidation);
  check(multiObligation.first == RenderTapeCaptureStatus::Complete &&
            !multiObligation.second,
        std::string("one clear resolves texture color and standalone D24X8 obligations: ") +
            renderTapeValidationStatusName(multiValidation.status));

  auto partial = capture(producedFullClearChunk(true), textureDescriptorBytes);
  auto drawFirst = capture(producedFullClearChunk(false, true),
                           textureDescriptorBytes);
  auto identityMismatch = capture(
      producedFullClearChunk(false, false, true), textureDescriptorBytes);
  auto unresolved = capture(producedFullClearChunk(false, false, false, true),
                            textureDescriptorBytes, false, false, 0u);
  check(partial.first != RenderTapeCaptureStatus::Complete && partial.second &&
            drawFirst.first != RenderTapeCaptureStatus::Complete &&
            drawFirst.second &&
            identityMismatch.first != RenderTapeCaptureStatus::Complete &&
            identityMismatch.second &&
            unresolved.first != RenderTapeCaptureStatus::Complete &&
            unresolved.second,
        "partial, draw-first, identity-mismatch, and unresolved Produced captures publish nothing");

  const auto multipleAlias = capture(producedFullClearChunk(),
                                     textureDescriptorBytes, true);
  const auto multipleProduced = capture(producedFullClearChunk(),
                                        textureDescriptorBytes, false, true);
  check(multipleAlias.first != RenderTapeCaptureStatus::Complete &&
            multipleAlias.second &&
            multipleProduced.first != RenderTapeCaptureStatus::Complete &&
            multipleProduced.second,
        "duplicate aliases and multiple Produced parents both reject");

  RenderTapeTextureDescriptorV2 cubeHeader{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Cube),
      .mipLevelCount = 1u,
      .subresourceCount = 6u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedByCapturedPass),
  };
  const D9CSurfaceDesc cubeFace{
      .format = 114u, .resourceType = 5u, .usage = 1u, .pool = 0u,
      .width = 4u, .height = 4u, .depth = 1u,
  };
  std::vector<std::byte> cubeDescriptor(
      sizeof(cubeHeader) + 6u * sizeof(cubeFace));
  std::memcpy(cubeDescriptor.data(), &cubeHeader, sizeof(cubeHeader));
  for (std::uint32_t face = 0u; face < 6u; ++face) {
    std::memcpy(cubeDescriptor.data() + sizeof(cubeHeader) +
                    face * sizeof(cubeFace),
                &cubeFace, sizeof(cubeFace));
  }
  auto cubeAliasDescriptor = aliasDescriptorBytes;
  RenderTapeSurfaceDescriptorV2 cubeAlias{};
  std::memcpy(&cubeAlias, cubeAliasDescriptor.data(), sizeof(cubeAlias));
  cubeAlias.surface.format = 114u;
  std::memcpy(cubeAliasDescriptor.data(), &cubeAlias, sizeof(cubeAlias));
  const auto twoFaceClearChunk = [&] {
    const auto face0 = producedFullClearChunk(
        false, false, false, false, kProducedAlias, false, kProducedDepth,
        true);
    const auto face1 = producedFullClearChunk(
        false, false, false, false,
        D9CWireObjectIdentity{.kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
                              .generation = 7u, .objectId = 42u});
    ImportedChunkView first{};
    ImportedChunkView second{};
    check(importPrevalidatedCommandChunk(
              face0,
              CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                   .recordCount = 2u, .handleCount = 1u},
              first) &&
              importPrevalidatedCommandChunk(
                  face1,
                  CommandChunkEnvelope{
                      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                      .recordCount = 3u, .handleCount = 1u},
                  second),
          "two-face Produced fixture imports both source chunks");
    std::vector<ProducedRecord> records;
    const auto append = [&](const ImportedChunkView& source,
                            std::uint32_t handleOffset) {
      for (std::size_t index = 0u; index < source.records.size(); ++index) {
        const auto record = source.record(index);
        ProducedRecord out{
            .type = record.header.type,
            .payload = std::vector<std::byte>(record.payload.begin(),
                                              record.payload.end()),
            .handles = std::vector<D9CCommandChunkWireHandleEntry>(
                source.handles.begin() + record.header.firstHandle,
                source.handles.begin() + record.header.firstHandle +
                    record.header.handleCount),
        };
        if (handleOffset != 0u && record.sparseState()) {
          D9CCommandChunkWireDrawHeader draw{};
          std::memcpy(&draw, out.payload.data(), sizeof(draw));
          for (std::uint32_t sectionIndex = 0u;
               sectionIndex < draw.sectionCount; ++sectionIndex) {
            D9CCommandChunkWireSectionDesc section{};
            std::memcpy(&section,
                        out.payload.data() + draw.sectionTableOffset +
                            sectionIndex * sizeof(section),
                        sizeof(section));
            if (section.kind != D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET)
              continue;
            D9CCommandChunkWireRenderTargetBinding binding{};
            std::memcpy(&binding, out.payload.data() + section.payloadOffset,
                        sizeof(binding));
            if (binding.valid != 0u)
              binding.handleIndex += handleOffset;
            std::memcpy(out.payload.data() + section.payloadOffset, &binding,
                        sizeof(binding));
          }
        }
        records.push_back(std::move(out));
      }
    };
    append(first, 0u);
    append(second, 1u);
    return makeProducedChunk(records);
  }();
  RenderTapeValidationResult cubeValidation{};
  const auto cubeFace0 = capture(producedFullClearChunk(), cubeDescriptor,
                                 false, false, 1u, false, &cubeValidation, 0u,
                                 cubeAliasDescriptor);
  check(cubeFace0.first == RenderTapeCaptureStatus::Complete &&
            !cubeFace0.second,
        std::string("one-mip cube Produced capture admits exact fully-cleared face zero: ") +
            renderTapeValidationStatusName(cubeValidation.status));

  auto multiMip2d = textureDescriptorBytes;
  RenderTapeTextureDescriptorV2 multiMip2dHeader{};
  std::memcpy(&multiMip2dHeader, multiMip2d.data(),
              sizeof(multiMip2dHeader));
  multiMip2dHeader.mipLevelCount = 2u;
  multiMip2dHeader.subresourceCount = 2u;
  const D9CSurfaceDesc mip1{
      .format = 22u, .resourceType = 3u, .usage = 1u, .pool = 0u,
      .width = 2u, .height = 2u, .depth = 1u,
  };
  multiMip2d.resize(sizeof(multiMip2dHeader) + 2u * sizeof(mip1));
  std::memcpy(multiMip2d.data(), &multiMip2dHeader,
              sizeof(multiMip2dHeader));
  std::memcpy(multiMip2d.data() + sizeof(multiMip2dHeader) + sizeof(mip1),
              &mip1, sizeof(mip1));
  const auto multiMip2dResult = capture(producedFullClearChunk(), multiMip2d);
  check(multiMip2dResult.first != RenderTapeCaptureStatus::Complete &&
            multiMip2dResult.second,
        "multi-mip 2D Produced capture remains fail-closed");

  const D9CWireObjectIdentity siblingAlias{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 7u,
      .objectId = 42u,
  };
  RenderTapeValidationResult siblingValidation{};
  const auto siblingFace = capture(
      twoFaceClearChunk,
      cubeDescriptor, true, false, 2u, false, &siblingValidation, 1u,
      cubeAliasDescriptor, 5u);
  check(siblingFace.first == RenderTapeCaptureStatus::Complete &&
            !siblingFace.second,
        std::string("same-chunk sibling face is admitted only by its independent full clear: ") +
            renderTapeValidationStatusName(siblingValidation.status));

  const auto laterAccess = [&](bool sibling, bool proveSibling,
                               RenderTapeValidationResult* validation) {
    RenderTapeCaptureSession session(true);
    check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() ==
                  RenderTapeCaptureStatus::Accepted &&
              session.objectDefine(
                  kProducedOutput,
                  static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                  std::as_bytes(std::span(&outputDescriptorBytes, 1u)), 0u,
                  {}) == RenderTapeCaptureStatus::Accepted &&
              session.objectDefine(
                  kProducedTexture,
                  static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
                  cubeDescriptor, 0u, {}, 0u, 0u) ==
                  RenderTapeCaptureStatus::Accepted &&
              session.objectDefine(
                  kProducedAlias,
                  static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                  cubeAliasDescriptor, 0u, {}, 0u, 0u) ==
                  RenderTapeCaptureStatus::Accepted,
          "later-access cube fixture defines the exact face-zero closure");
    if (sibling) {
      auto face1Descriptor = cubeAliasDescriptor;
      RenderTapeSurfaceDescriptorV2 face1{};
      std::memcpy(&face1, face1Descriptor.data(), sizeof(face1));
      face1.subresource = 1u;
      std::memcpy(face1Descriptor.data(), &face1, sizeof(face1));
      check(session.objectDefine(
                siblingAlias,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                face1Descriptor, 0u, {}, 0u, 0u) ==
                RenderTapeCaptureStatus::Accepted,
            "later-access cube fixture defines sibling face one");
    }
    const auto first = producedFullClearChunk(
        false, false, false, false, kProducedAlias, false, kProducedDepth,
        true);
    const auto second = sibling
        ? producedFullClearChunk(false, !proveSibling, false, false,
                                 siblingAlias)
        : producedFullClearChunk(false, false, false, false, kProducedAlias,
                                 false, kProducedDepth, false, true);
    check(session.commandChunk(
              CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                   .recordCount = 2u, .handleCount = 1u},
              first) == RenderTapeCaptureStatus::Accepted &&
              session.commandChunk(
                  CommandChunkEnvelope{
                      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                      .recordCount = 3u,
                      .handleCount = sibling ? 1u : 2u},
                  second) == RenderTapeCaptureStatus::Accepted &&
              session.orderedControl(
                  flush, std::as_bytes(std::span(&wait, 1u))) ==
                  RenderTapeCaptureStatus::Accepted,
          "later-access cube fixture records proof then forbidden access");
    const auto status = session.completePresent(
        sibling ? 7u : 6u, 11u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&outputOracle, 1u)));
    if (validation)
      *validation = session.validationResult();
    return std::pair{status, session.sealedArtifact().empty()};
  };
  RenderTapeValidationResult laterSiblingValidation{};
  RenderTapeValidationResult laterUnprovedValidation{};
  RenderTapeValidationResult laterParentValidation{};
  const auto laterSibling = laterAccess(true, true, &laterSiblingValidation);
  const auto laterUnprovedSibling =
      laterAccess(true, false, &laterUnprovedValidation);
  const auto laterParent = laterAccess(false, true, &laterParentValidation);
  check(laterSibling.first == RenderTapeCaptureStatus::Complete &&
            !laterSibling.second &&
            laterUnprovedSibling.first != RenderTapeCaptureStatus::Complete &&
            laterUnprovedSibling.second &&
            laterParent.first != RenderTapeCaptureStatus::Complete &&
            laterParent.second,
        std::string("later face requires its own full clear and direct-parent access always rejects: ") +
            renderTapeValidationStatusName(laterSiblingValidation.status) +
            "," + renderTapeValidationStatusName(
                        laterUnprovedValidation.status) +
            "," + renderTapeValidationStatusName(laterParentValidation.status));

  auto mismatchedAlias = cubeAliasDescriptor;
  RenderTapeSurfaceDescriptorV2 mismatchedAliasValue{};
  std::memcpy(&mismatchedAliasValue, mismatchedAlias.data(),
              sizeof(mismatchedAliasValue));
  mismatchedAliasValue.surface.width = 2u;
  std::memcpy(mismatchedAlias.data(), &mismatchedAliasValue,
              sizeof(mismatchedAliasValue));
  RenderTapeCaptureSession mismatchSession(true);
  check(mismatchSession.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            mismatchSession.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted &&
            mismatchSession.objectDefine(
                kProducedOutput,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                std::as_bytes(std::span(&outputDescriptorBytes, 1u)), 0u, {}) ==
                RenderTapeCaptureStatus::Accepted &&
            mismatchSession.objectDefine(
                kProducedTexture,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
                cubeDescriptor, 0u, {}, 0u, 0u) ==
                RenderTapeCaptureStatus::Accepted &&
            mismatchSession.objectDefine(
                kProducedAlias,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                mismatchedAlias, 0u, {}, 0u, 0u) ==
                RenderTapeCaptureStatus::Accepted,
        "mismatched Produced alias fixture is journaled for tape-level proof");
  check(mismatchSession.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 3u, .handleCount = 1u},
            producedFullClearChunk()) == RenderTapeCaptureStatus::Accepted &&
            mismatchSession.orderedControl(
                flush, std::as_bytes(std::span(&wait, 1u))) ==
                RenderTapeCaptureStatus::Accepted &&
            mismatchSession.completePresent(
                5u, 11u, RenderTapeDigestValidity::NotCaptured, {},
                std::as_bytes(std::span(&outputOracle, 1u))) !=
                RenderTapeCaptureStatus::Complete &&
            mismatchSession.sealedArtifact().empty(),
        "extent-mismatched Produced alias aborts without publication");
}

void testProducedDefinitionTemporalOrderAndAliasJournal() {
  const auto outputDescriptorBytes = outputSurfaceDescriptor();
  const auto textureDescriptorBytes = producedTextureDescriptor();
  const auto aliasDescriptorBytes = producedAliasDescriptor();
  const RenderTapeOracleAttachment outputOracle{
      .identity = kProducedOutput,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };

  RenderTapeBuilder temporal;
  temporal.appendBootstrapState(bootstrapChunk());
  // This command precedes all Produced definitions. It is deliberately
  // handle-free: the validator must not try to prove a future obligation here.
  temporal.appendCommandChunk(
      CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                           .recordCount = 1u,
                           .handleCount = 0u},
      bootstrapChunk());
  temporal.appendObjectDefine(
      kProducedOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptorBytes, 1u)), 0u, {});
  temporal.appendObjectDefine(
      kProducedTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptorBytes, 0u, {}, 0u, 0u);
  temporal.appendObjectDefine(
      kProducedAlias, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      aliasDescriptorBytes, 0u, {}, 0u, 0u);
  temporal.appendCommandChunk(
      CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                           .recordCount = 3u,
                           .handleCount = 1u},
      producedFullClearChunk());
  temporal.appendPresentComplete(
      6u, 11u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&outputOracle, 1u)));
  check(validateRenderTape(temporal.seal(), {}).valid(),
        "Produced proof ignores command chunks before its definitions");

  const D9CWireObjectIdentity alias2{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 7u,
      .objectId = 42u,
  };
  RenderTapeBuilder duplicateAlias;
  duplicateAlias.appendBootstrapState(bootstrapChunk());
  duplicateAlias.appendObjectDefine(
      kProducedOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptorBytes, 1u)), 0u, {});
  duplicateAlias.appendObjectDefine(
      kProducedTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptorBytes, 0u, {}, 0u, 0u);
  duplicateAlias.appendObjectDefine(
      kProducedAlias, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      aliasDescriptorBytes, 0u, {}, 0u, 0u);
  duplicateAlias.appendObjectDestroy(kProducedAlias);
  duplicateAlias.appendObjectDefine(
      alias2, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      aliasDescriptorBytes, 0u, {}, 0u, 0u);
  duplicateAlias.appendCommandChunk(
      CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                           .recordCount = 3u,
                           .handleCount = 1u},
      producedFullClearChunk(false, false, false, false, alias2));
  duplicateAlias.appendPresentComplete(
      7u, 11u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&outputOracle, 1u)));
  check(validateRenderTape(duplicateAlias.seal(), {}).valid(),
        "a retired alias wrapper does not make its live replacement ambiguous");
}

RenderTapeSurfaceDescriptorV2 outputSurfaceDescriptor() {
  return RenderTapeSurfaceDescriptorV2{
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
          .depth = 1u,
      },
  };
}

RenderTapeSurfaceDescriptorV2 standaloneSurfaceDescriptor() {
  auto descriptor = outputSurfaceDescriptor();
  descriptor.storage =
      static_cast<std::uint32_t>(RenderTapeSurfaceStorage::Standalone);
  descriptor.initialContentDisposition = static_cast<std::uint32_t>(
      RenderTapeInitialContentDisposition::CompleteSeed);
  return descriptor;
}

std::vector<std::byte> texture2DDescriptor(
    RenderTapeInitialContentDisposition disposition =
        RenderTapeInitialContentDisposition::CompleteSeed) {
  const RenderTapeTextureDescriptorV2 header{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u,
      .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(disposition),
  };
  const D9CSurfaceDesc level{
      .format = 21u,
      .resourceType = 3u,
      .pool = 0u,
      .width = 4u,
      .height = 4u,
      .depth = 1u,
  };
  std::vector<std::byte> bytes(sizeof(header) + sizeof(level));
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), &level, sizeof(level));
  return bytes;
}

void testCaptureOffPreservesBytes() {
  auto chunk = presentChunk();
  const auto original = chunk;
  RenderTapeCaptureSession session(false);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Disabled,
        "capture-off arm is inert");
  check(session.state() == RenderTapeCaptureState::Disabled,
        "capture-off state remains disabled");
  check(chunk == original, "capture-off leaves canonical bytes unchanged");
}

void testDescriptorKindAxisTruthTable() {
  constexpr std::array identityKinds{
      D9C_CHUNK_HANDLE_KIND_TEXTURE, D9C_CHUNK_HANDLE_KIND_SURFACE,
      D9C_CHUNK_HANDLE_KIND_BUFFER, D9C_CHUNK_HANDLE_KIND_SHADER,
      D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, D9C_CHUNK_HANDLE_KIND_QUERY};
  constexpr std::array expectedDescriptorKinds{
      RenderTapeDescriptorKind::Texture, RenderTapeDescriptorKind::Surface,
      RenderTapeDescriptorKind::Buffer, RenderTapeDescriptorKind::Shader,
      RenderTapeDescriptorKind::VertexDeclaration,
      RenderTapeDescriptorKind::Query};
  constexpr std::array<std::byte, 8u> descriptor{};
  const auto surfaceDescriptor = outputSurfaceDescriptor();
  const auto textureDescriptor = texture2DDescriptor();
  for (std::size_t i = 0u; i < identityKinds.size(); ++i) {
    const D9CWireObjectIdentity identity{
        .kind = identityKinds[i], .generation = 1u,
        .objectId = static_cast<std::uint64_t>(100u + i)};
    check(renderTapeDescriptorKindForObject(identity.kind) ==
              expectedDescriptorKinds[i],
          "identity kind maps to its stable descriptor schema tag");
    RenderTapeCaptureSession session(true);
    check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() ==
                  RenderTapeCaptureStatus::Accepted,
          "descriptor-kind truth-table fixture starts");
    const std::span<const std::byte> descriptorBytes =
        identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE
            ? std::span<const std::byte>(textureDescriptor)
            : identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE
                  ? std::as_bytes(std::span(&surfaceDescriptor, 1u))
                  : std::span<const std::byte>(descriptor);
    check(session.objectDefine(
              identity, static_cast<std::uint32_t>(expectedDescriptorKinds[i]),
              descriptorBytes, 0u, {},
              identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE ? 4u : 0u,
              identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE ? 1u : 0u) ==
              RenderTapeCaptureStatus::Accepted,
          "all object categories accept their non-zero descriptor schema tag");
  }
  check(renderTapeDescriptorKindForObject(99u) ==
            RenderTapeDescriptorKind::Invalid,
        "unknown identity kinds map to the invalid descriptor schema tag");
}

void testKindZeroIntervalDefineUsesNonZeroDescriptorTag() {
  constexpr D9CWireObjectIdentity kindZero{
      .kind = 0u, .generation = 7u, .objectId = 700u};
  constexpr std::array<std::byte, 8u> descriptor{};
  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "kind-zero interval define fixture starts");
  const auto descriptorKind = renderTapeDescriptorKindForObject(kindZero.kind);
  check(descriptorKind != RenderTapeDescriptorKind::Invalid &&
            static_cast<std::uint32_t>(descriptorKind) != 0u,
        "kind-zero identity maps to a non-zero descriptor tag");
  check(session.objectDefine(
            kindZero, static_cast<std::uint32_t>(descriptorKind), descriptor,
            0u, {}) == RenderTapeCaptureStatus::Accepted,
        "kind-zero interval ObjectDefine accepts the mapped descriptor tag");
}

std::vector<std::byte> recorderPresentChunk();

bool unusedProductionProducer(RenderTapeCaptureBootstrapSeed&) { return true; }

bool unusedProductionPublisher(const RenderTapePublicationBundle&) {
  return true;
}

void testProductionHookGateTruthTable() {
  using Producer = D3D9PeRenderTapeBootstrapProducer;
  using Publisher = D3D9PeRenderTapeArtifactPublisher;
  struct GateCase {
    bool enabled;
    Producer producer;
    Publisher publisher;
    bool expected;
  };
  const auto producer = &unusedProductionProducer;
  const auto publisher = &unusedProductionPublisher;
  const std::array<GateCase, 6u> cases{
      GateCase{false, producer, publisher, false},
      GateCase{false, producer, nullptr, false},
      GateCase{false, nullptr, publisher, false},
      GateCase{true, producer, publisher, true},
      GateCase{true, producer, nullptr, false},
      GateCase{true, nullptr, publisher, true},
  };
  for (const auto& testCase : cases) {
    check(dxmt9PeRenderTapeCaptureCallbacksInstalled(
              testCase.enabled, testCase.producer, testCase.publisher) ==
              testCase.expected,
          "production capture gate truth table is stable");
  }
  auto bytes = recorderPresentChunk();
  const auto original = bytes;
  check(bytes == original,
        "production capture gate does not mutate canonical recorder bytes");
}

void testProfileSelectionTruthTable() {
  check(dxmt9PeRenderTapeProfileFromText("") == kRenderTapeProfileFrame &&
            dxmt9PeRenderTapeProfileFromText("frame-tape") ==
                kRenderTapeProfileFrame &&
            dxmt9PeRenderTapeProfileFromText("sequence-tape") ==
                kRenderTapeProfileSequence &&
            dxmt9PeRenderTapeProfileFromText("bogus") == 0u,
        "capture profile selection accepts only explicit bounded names");
}

std::vector<std::byte> recorderPresentChunk() {
  CommandChunkBuilder recorder;
  RecorderSurface surface;
  const PeWireObjectRef source{
      .identity = D9CWireObjectIdentity{
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = 2u,
          .objectId = 17u,
      },
      .object = &surface,
  };
  check(dxmt9::d3d9::pe::appendPresent(
            recorder, D9CCommandChunkWirePresent{}, source),
        "PE recorder appends Present");
  const auto sealed = recorder.seal();
  check(sealed.valid(), "PE recorder seals Present");
  return std::vector<std::byte>(sealed.blob.begin(), sealed.blob.end());
}

void writeAtomically(const std::filesystem::path& path,
                     std::span<const std::byte> bytes) {
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  check(output.good(), "fixture opens temporary output");
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  check(output.good(), "fixture writes temporary output");
  output.close();
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  check(!error, "fixture publishes output atomically");
}

void writeProductionFixture(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory / "blobs", error);
  check(!error, "fixture creates output directory");

  const auto bootstrap = bootstrapChunk();
  const auto present = recorderPresentChunk();
  const auto mutationBytes = std::array<std::byte, 4u>{
      std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}, std::byte{0xdd}};
  const RenderTapeCaptureBlob verifiedBlob{
      .bytes = std::vector<std::byte>(mutationBytes.begin(), mutationBytes.end())};
  RenderTapeCaptureSession session(true);
  check(session.armWithBlobs(bootstrap, std::span(&verifiedBlob, 1u)) ==
            RenderTapeCaptureStatus::Accepted,
        "production fixture arms capture owner");
  check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "production fixture starts one Present interval");
  const auto outputDescriptor = outputSurfaceDescriptor();
  check(session.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "production fixture journals mapped Present output");
  constexpr D9CWireObjectIdentity seedSurface{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 18u,
  };
  const auto seedDescriptor = standaloneSurfaceDescriptor();
  check(session.objectDefine(
            seedSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&seedDescriptor, 1u)), 0u, {}, 4u, 1u) ==
            RenderTapeCaptureStatus::Accepted,
        "production fixture journals independent seeded surface");
  check(session.resourceMutationBytes(
            seedSurface, RenderTapeMutationKind::Upload, 0u, 0u,
            mutationBytes) == RenderTapeCaptureStatus::Accepted,
        "production fixture journals resource mutation");
  check(session.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 1u, .handleCount = 1u},
            present) == RenderTapeCaptureStatus::Accepted,
        "production fixture copies canonical PE recorder bytes once");
  const RenderTapeFlushWaitControl wait{.waitedSeqId = 9u};
  check(session.orderedControl(
            RenderTapeOrderedControlHeader{
                .kind = static_cast<std::uint32_t>(RenderTapeControlKind::FlushWait),
                .disposition = static_cast<std::uint32_t>(
                    RenderTapeControlDisposition::Completed),
                .controlBytes = sizeof(wait),
                .completionOrdinal = 10u},
            std::as_bytes(std::span(&wait, 1u))) ==
            RenderTapeCaptureStatus::Accepted,
        "production fixture journals ordered control");
  const RenderTapeOracleAttachment attachment = oracle();
  check(session.completePresent(
            5u, 11u, RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::Complete,
        "production fixture seals Present transactionally");
  check(session.validationStatus() == RenderTapeValidationStatus::Valid,
        "production fixture validates before publish");

  const auto eventsPath = directory / "events.bin";
  const auto blobPath = directory / "blobs" /
                        "8d70d691c822d55638b6e7fd54cd94170c87d19eb1f628b757506ede5688d297.bin";
  check(session.publicationBundle().events == session.sealedArtifact(),
        "production fixture publishes the session bundle events");
  check(session.publicationBundle().blobs.size() == 1u &&
            session.publicationBundle().blobs[0].bytes ==
                std::vector<std::byte>(mutationBytes.begin(), mutationBytes.end()),
        "production fixture publishes verified blob bytes from the bundle");
  writeAtomically(eventsPath, session.publicationBundle().events);
  writeAtomically(blobPath, session.publicationBundle().blobs[0].bytes);
}

void testProductionFixtureUsesRecorderAndPublishesBundle(
    const std::filesystem::path& directory) {
  const auto recorderBytes = recorderPresentChunk();
  const auto original = recorderBytes;
  RenderTapeCaptureSession disabled(false);
  check(disabled.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Disabled,
        "production capture-off arm is inert");
  check(recorderBytes == original,
        "production capture-off preserves recorder bytes");
  writeProductionFixture(directory);
  check(std::filesystem::is_regular_file(directory / "events.bin"),
        "production fixture publishes events.bin");
  check(std::filesystem::is_regular_file(
            directory / "blobs" /
            "8d70d691c822d55638b6e7fd54cd94170c87d19eb1f628b757506ede5688d297.bin"),
        "production fixture publishes digest-named blob");
}

void testCompletePresentPublishesExactlyOneTape() {
  const auto bootstrap = bootstrapChunk();
  auto present = presentChunk();
  const auto resource = blob();
  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrap, std::span(&resource, 1u)) ==
            RenderTapeCaptureStatus::Accepted,
        "complete capture arms");
  check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "complete capture starts one interval");
  auto descriptor = standaloneSurfaceDescriptor();
  const auto expectedDescriptor = descriptor;
  check(session.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&descriptor, 1u)), 0u, {}, 4u, 1u) ==
            RenderTapeCaptureStatus::Accepted,
        "object definition is journaled");
  descriptor.schemaVersion = 0u;
  const auto mutationDigestValue = digest();
  check(session.resourceMutation(
            kSurface, RenderTapeMutationKind::Upload, 0u, 0u, 4u,
            std::span<const std::byte, kRenderTapeDigestSize>(
                mutationDigestValue)) == RenderTapeCaptureStatus::Accepted,
        "verified initial mutation is journaled");
  check(session.commandChunk(
            CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
            present) == RenderTapeCaptureStatus::Accepted,
        "canonical Present chunk is copied once");
  present[0] = std::byte{0xff};
  const RenderTapeFlushWaitControl wait{.waitedSeqId = 9u};
  check(session.orderedControl(flushControl(),
                               std::as_bytes(std::span(&wait, 1u))) ==
            RenderTapeCaptureStatus::Accepted,
        "true bypass control is journaled");
  const auto attachment = oracle();
  check(session.completePresent(
            4u, 11u, RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::Complete,
        "PresentComplete validates and publishes");
  check(session.state() == RenderTapeCaptureState::Sealed,
        "successful Present seals the owner");
  const RenderTapeIdentitySource identitySource{
      .eventOrdinal = 4u,
      .sourceOrdinal = 1u,
      .seqId = 1u,
      .captureToken = 7u,
      .recordCount = 1u,
      .firstRange = 0u,
      .rangeCount = 1u,
  };
  const RenderTapeIdentityRange identityRange{
      .eventOrdinal = 4u,
      .sourceOrdinal = 1u,
      .seqId = 1u,
      .logicalPassId = 1u,
      .firstRecord = 0u,
      .recordCount = 1u,
      .dagPassIndex = 0u,
      .passKind = 4u,
  };
  check(session.attachCaptureIdentity(
            7u, 4u, std::span(&identitySource, 1u),
            std::span(&identityRange, 1u)) ==
            RenderTapeCaptureStatus::Complete &&
            !session.publicationBundle().identity.empty(),
        "sealed capture attaches one validated authoritative identity sidecar");
  check(!session.sealedArtifact().empty(), "sealed artifact is published");
  ImportedRenderTapeView imported;
  const RenderTapeBlobCatalogue catalogue{.blobs = {resource}};
  check(validateRenderTape(session.sealedArtifact(), catalogue, &imported)
            .valid(),
        "active-created identity fixture remains canonical");
  check(validateRenderTapeIdentity(
            session.sealedArtifact(), catalogue,
            session.publicationBundle().identity).valid(),
        "attached capture identity validates against the sealed events digest");
  RenderTapeObjectDefineHeader define{};
  const auto defineEvent = imported.event(1u);
  std::memcpy(&define, defineEvent.payload.data(), sizeof(define));
  check(define.identity.generation == kSurface.generation &&
            define.identity.objectId == kSurface.objectId &&
            defineEvent.payload.size() ==
                sizeof(define) + sizeof(expectedDescriptor) &&
            std::memcmp(defineEvent.payload.data() + sizeof(define),
                        &expectedDescriptor, sizeof(expectedDescriptor)) == 0,
        "active-created identity preserves exact V2 descriptor bytes and generation");
  check(session.completePresent(
            4u, 12u, RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::InvalidState,
        "second Present cannot publish another interval");
}

void testSequenceCaptureDefersSealUntilSecondPresent() {
  constexpr std::array<std::byte, 4u> firstBytes{
      std::byte{0x10u}, std::byte{0x11u}, std::byte{0x12u}, std::byte{0x13u}};
  constexpr std::array<std::byte, 4u> secondBytes{
      std::byte{0x20u}, std::byte{0x21u}, std::byte{0x22u}, std::byte{0x23u}};
  const std::array<RenderTapeCaptureBlob, 2u> blobs{
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(
          firstBytes.begin(), firstBytes.end())},
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(
          secondBytes.begin(), secondBytes.end())}};
  RenderTapeCaptureSession session(true, {}, kRenderTapeProfileSequence);
  check(session.profile() == kRenderTapeProfileSequence &&
            session.armWithBlobs(bootstrapChunk(), blobs) ==
                RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted,
        "sequence capture arms the explicit two-interval profile");
  const auto descriptor = standaloneSurfaceDescriptor();
  check(session.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&descriptor, 1u)), 0u, {}, 4u, 1u) ==
            RenderTapeCaptureStatus::Accepted,
        "sequence capture journals the initial output seed definition");
  const auto firstPresent = presentChunk();
  check(session.resourceMutationBytes(
            kSurface, RenderTapeMutationKind::Upload, 0u, 0u,
            firstBytes) == RenderTapeCaptureStatus::Accepted &&
            session.commandChunk(
                CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
                firstPresent) == RenderTapeCaptureStatus::Accepted,
        "sequence capture records interval one in journal order");
  const auto attachment = oracle();
  check(session.completePresent(
            4u, 1u, RenderTapeDigestValidity::Sha256, digest(),
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::Accepted &&
            session.presentCompletionCount() == 1u &&
            session.state() == RenderTapeCaptureState::Capturing &&
            session.sealedArtifact().empty(),
        "sequence capture does not seal or validate after interval one");
  const auto secondPresent = presentChunk();
  check(session.resourceMutationBytes(
            kSurface, RenderTapeMutationKind::Upload, 0u, 0u,
            secondBytes) == RenderTapeCaptureStatus::Accepted &&
            session.commandChunk(
                CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
                secondPresent) == RenderTapeCaptureStatus::Accepted,
        "sequence capture preserves the between-Present mutation");
  check(session.completePresent(
            7u, 2u, RenderTapeDigestValidity::Sha256, digest(),
            std::as_bytes(std::span(&attachment, 1u))) ==
            RenderTapeCaptureStatus::Complete &&
            session.presentCompletionCount() == 2u &&
            session.state() == RenderTapeCaptureState::Sealed &&
            !session.sealedArtifact().empty(),
        "sequence capture seals only after interval two validation");
  RenderTapeBlobCatalogue catalogue;
  for (const auto& blob : session.publicationBundle().blobs) {
    catalogue.blobs.push_back(RenderTapeBlob{
        .digest = blob.digest, .size = blob.bytes.size(), .verified = 1u});
  }
  check(validateRenderTape(session.sealedArtifact(), catalogue).valid(),
        "sequence capture publishes a structurally valid final tape");
}

void testValidationFailurePreservesEventAndChunkLocation() {
  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "validation-location fixture arms");
  check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "validation-location fixture starts");
  const auto present = presentChunk();
  check(session.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 1u, .handleCount = 0u},
            present) == RenderTapeCaptureStatus::Accepted,
        "validation-location fixture records Present chunk");
  check(session.completePresent(
            1u, 1u, RenderTapeDigestValidity::NotCaptured, {}) ==
            RenderTapeCaptureStatus::ValidationFailed,
        "missing oracle attachment fails at final validation");
  const auto &validation = session.validationResult();
  check(validation.status == RenderTapeValidationStatus::InvalidPresentComplete &&
            validation.failedEventIndex == 2u &&
            validation.chunkStatus == CommandChunkValidationStatus::Valid,
        "validation failure preserves final event index and chunk status");
}

void testObjectDefineValidationDetailTruthTable() {
  constexpr std::array<std::byte, 8u> genericDescriptor{};
  const auto descriptor = outputSurfaceDescriptor();
  const auto descriptorBytes = std::as_bytes(std::span(&descriptor, 1u));
  RenderTapeObjectDefineHeader fixed{
      .identity = kSurface,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
      .descriptorBytes = static_cast<std::uint32_t>(descriptorBytes.size()),
  };
  auto detail = renderTapeClassifyObjectDefineValidation(fixed, descriptorBytes);
  check(!detail.valid() && detail.identity.objectId == kSurface.objectId &&
            detail.descriptorBytes == descriptorBytes.size() &&
            detail.descriptorPayloadBytes == descriptorBytes.size(),
        "valid ObjectDefine has no validation detail but preserves its values");

  fixed.descriptorKind = static_cast<std::uint32_t>(
      RenderTapeDescriptorKind::Texture);
  detail = renderTapeClassifyObjectDefineValidation(fixed, descriptorBytes);
  check(detail.subreason ==
            RenderTapeObjectDefineValidationSubreason::DescriptorKindMismatch,
        "ObjectDefine detail names descriptor-kind mismatch");
  fixed.descriptorKind = static_cast<std::uint32_t>(
      RenderTapeDescriptorKind::Surface);
  fixed.expectedContentBytes = 4u;
  detail = renderTapeClassifyObjectDefineValidation(fixed, descriptorBytes);
  check(detail.subreason ==
            RenderTapeObjectDefineValidationSubreason::ExpectedContentPair,
        "ObjectDefine detail names an incomplete expected-content pair");
  fixed.expectedContentBytes = 0u;
  fixed.payloadValidity = 99u;
  detail = renderTapeClassifyObjectDefineValidation(fixed, descriptorBytes);
  check(detail.subreason ==
            RenderTapeObjectDefineValidationSubreason::PayloadValidity,
        "ObjectDefine detail names invalid payload validity");
  fixed.payloadValidity = 0u;
  fixed.descriptorBytes = 4u;
  detail = renderTapeClassifyObjectDefineValidation(fixed, descriptorBytes);
  check(detail.subreason ==
            RenderTapeObjectDefineValidationSubreason::DescriptorExtent,
        "ObjectDefine detail names descriptor extent mismatch");

  constexpr D9CWireObjectIdentity shader{
      .kind = D9C_CHUNK_HANDLE_KIND_SHADER, .generation = 1u, .objectId = 91u};
  fixed = RenderTapeObjectDefineHeader{
      .identity = shader,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Shader),
      .descriptorBytes = genericDescriptor.size(),
  };
  detail = renderTapeClassifyObjectDefineValidation(fixed, genericDescriptor);
  check(detail.subreason == RenderTapeObjectDefineValidationSubreason::
                            ImmutablePayloadRequired,
        "ObjectDefine detail names a missing required immutable payload");

  constexpr D9CWireObjectIdentity parent{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 4u,
      .objectId = 92u,
  };
  const RenderTapeTextureDescriptorV2 texture{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u,
      .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  const D9CSurfaceDesc textureSurface{.format = render_tape_d3d_format::A8R8G8B8,
                                      .resourceType = 3u,
                                      .width = 64u,
                                      .height = 32u,
                                      .depth = 1u};
  std::vector<std::byte> textureDescriptor(sizeof(texture) +
                                            sizeof(textureSurface));
  std::memcpy(textureDescriptor.data(), &texture, sizeof(texture));
  std::memcpy(textureDescriptor.data() + sizeof(texture), &textureSurface,
              sizeof(textureSurface));
  fixed = RenderTapeObjectDefineHeader{
      .identity = parent,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Texture),
      .descriptorBytes = static_cast<std::uint32_t>(textureDescriptor.size()),
      .expectedContentBytes = 4u,
      .expectedContentCount = 1u,
  };
  detail = renderTapeClassifyObjectDefineValidation(fixed, textureDescriptor);
  check(!detail.valid() && detail.descriptorDimension ==
                                  static_cast<std::uint32_t>(
                                      RenderTapeTextureDimension::Texture2D) &&
            detail.descriptorExpectedExtentBytes == textureDescriptor.size(),
        "ObjectDefine detail decodes versioned texture extent metadata");
  auto invalidTexture = texture;
  invalidTexture.dimension = 99u;
  std::memcpy(textureDescriptor.data(), &invalidTexture, sizeof(invalidTexture));
  detail = renderTapeClassifyObjectDefineValidation(fixed, textureDescriptor);
  check(detail.subreason == RenderTapeObjectDefineValidationSubreason::
                            TextureDescriptorDimension,
        "ObjectDefine detail names invalid texture dimension");
  auto unavailableTexture = texture;
  unavailableTexture.initialContentDisposition = static_cast<std::uint32_t>(
      RenderTapeInitialContentDisposition::Unavailable);
  std::memcpy(textureDescriptor.data(), &unavailableTexture,
              sizeof(unavailableTexture));
  fixed.expectedContentBytes = 0u;
  fixed.expectedContentCount = 0u;
  detail = renderTapeClassifyObjectDefineValidation(fixed, textureDescriptor);
  check(detail.subreason == RenderTapeObjectDefineValidationSubreason::
                                TextureDescriptorDisposition,
        "texture V2 rejects Unavailable instead of weakening seed closure");
  fixed.expectedContentBytes = 4u;
  fixed.expectedContentCount = 1u;

  const RenderTapeSurfaceDescriptorV2 alias{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = parent,
      .surface = D9CSurfaceDesc{
          .format = textureSurface.format,
          .resourceType = 1u,
          .width = textureSurface.width,
          .height = textureSurface.height,
          .depth = textureSurface.depth,
      },
  };
  fixed = RenderTapeObjectDefineHeader{
      .identity = kSurface,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
      .descriptorBytes = sizeof(alias),
  };
  const auto aliasBytes = std::as_bytes(std::span(&alias, 1u));
  detail = renderTapeClassifyObjectDefineValidation(fixed, aliasBytes);
  check(!detail.valid() && detail.parentTexture.objectId == parent.objectId &&
            detail.descriptorStorage ==
                static_cast<std::uint32_t>(
                    RenderTapeSurfaceStorage::TextureSubresource),
        "ObjectDefine detail decodes versioned surface parent metadata");
  const D9CSurfaceDesc legacySurface{
      .format = render_tape_d3d_format::A8R8G8B8,
      .resourceType = 1u,
      .width = 64u,
      .height = 32u,
      .depth = 1u,
  };
  fixed.descriptorBytes = sizeof(legacySurface);
  detail = renderTapeClassifyObjectDefineValidation(
      fixed, std::as_bytes(std::span(&legacySurface, 1u)));
  check(detail.subreason == RenderTapeObjectDefineValidationSubreason::
                                SurfaceDescriptorExtent,
        "raw D9CSurfaceDesc is retired from the surface grammar");
  fixed.descriptorBytes = sizeof(alias);
  auto invalidStorage = alias;
  invalidStorage.storage = 99u;
  detail = renderTapeClassifyObjectDefineValidation(
      fixed, std::as_bytes(std::span(&invalidStorage, 1u)));
  check(detail.subreason == RenderTapeObjectDefineValidationSubreason::
                                SurfaceDescriptorStorage,
        "surface V2 rejects unknown storage");
  auto invalidDisposition = alias;
  invalidDisposition.initialContentDisposition = static_cast<std::uint32_t>(
      RenderTapeInitialContentDisposition::CompleteSeed);
  detail = renderTapeClassifyObjectDefineValidation(
      fixed, std::as_bytes(std::span(&invalidDisposition, 1u)));
  check(detail.subreason == RenderTapeObjectDefineValidationSubreason::
                                SurfaceDescriptorDisposition,
        "surface V2 rejects storage/disposition mismatches");
  auto invalidAlias = alias;
  invalidAlias.parentTexture.kind = D9C_CHUNK_HANDLE_KIND_SURFACE;
  detail = renderTapeClassifyObjectDefineValidation(
      fixed, std::as_bytes(std::span(&invalidAlias, 1u)));
  check(detail.subreason ==
            RenderTapeObjectDefineValidationSubreason::SurfaceDescriptorParent,
        "ObjectDefine detail names invalid surface parent identity");

  constexpr std::array<std::byte, sizeof(D9CSurfaceDesc) + sizeof(std::uint32_t)>
      legacyTexture{};
  fixed = RenderTapeObjectDefineHeader{
      .identity = parent,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Texture),
      .descriptorBytes = static_cast<std::uint32_t>(legacyTexture.size()),
  };
  detail = renderTapeClassifyObjectDefineValidation(fixed, legacyTexture);
  check(detail.subreason == RenderTapeObjectDefineValidationSubreason::
                                TextureDescriptorSchema,
        "legacy level0-plus-count texture descriptor is retired");
}

void testPresentCompleteOracleTargetTruthTable() {
  constexpr D9CWireObjectIdentity buffer{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 1u, .objectId = 99u};
  constexpr std::array<std::byte, 8u> descriptor{};
  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "oracle-target truth-table fixture starts");
  check(session.objectDefine(buffer,
                             static_cast<std::uint32_t>(
                                 RenderTapeDescriptorKind::Buffer),
                             descriptor, 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "oracle-target truth-table admits a live buffer control case");
  const auto present = presentChunk();
  check(session.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 1u, .handleCount = 0u},
            present) == RenderTapeCaptureStatus::Accepted,
        "oracle-target truth-table records Present");
  const RenderTapeOracleAttachment wrongTarget{
      .identity = buffer,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Buffer)};
  check(session.completePresent(
            1u, 1u, RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&wrongTarget, 1u))) ==
            RenderTapeCaptureStatus::ValidationFailed,
        "PresentComplete rejects a non-surface oracle target");
  check(session.validationResult().status ==
            RenderTapeValidationStatus::InvalidPresentComplete,
        "oracle-target rejection is attributed to PresentComplete");
}

void testObjectExpectedContentContractTruthTable() {
  struct ExtentCase {
    std::uint64_t bytes;
    std::uint32_t count;
    RenderTapeCaptureStatus expected;
  };
  constexpr std::array cases{
      ExtentCase{0u, 0u, RenderTapeCaptureStatus::Accepted},
      ExtentCase{4u, 0u, RenderTapeCaptureStatus::InvalidInput},
      ExtentCase{0u, 1u, RenderTapeCaptureStatus::InvalidInput},
      ExtentCase{4u, 1u, RenderTapeCaptureStatus::Accepted},
  };
  const auto outputDescriptor = outputSurfaceDescriptor();
  const auto standaloneDescriptor = standaloneSurfaceDescriptor();
  for (const auto& testCase : cases) {
    RenderTapeCaptureSession session(true);
    check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() ==
                  RenderTapeCaptureStatus::Accepted,
          "expected-content truth-table fixture starts");
    const auto descriptor = testCase.bytes == 4u && testCase.count == 1u
                                ? standaloneDescriptor
                                : outputDescriptor;
    check(session.objectDefine(
              kSurface,
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
              std::as_bytes(std::span(&descriptor, 1u)), 0u, {},
                               testCase.bytes, testCase.count) ==
              testCase.expected,
          "ObjectDefine expected extent/count pair truth table is stable");
  }
}

std::vector<std::byte> versionedTextureDescriptor(
    RenderTapeTextureDimension dimension, std::uint32_t mipLevelCount,
    std::uint32_t format, std::uint32_t width, std::uint32_t height) {
  const auto subresourceCount = dimension == RenderTapeTextureDimension::Cube
                                    ? mipLevelCount * 6u
                                    : mipLevelCount;
  const RenderTapeTextureDescriptorV2 fixed{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(dimension),
      .mipLevelCount = mipLevelCount,
      .subresourceCount = subresourceCount,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  const auto resourceType = dimension == RenderTapeTextureDimension::Texture2D
                                ? 3u
                                : dimension == RenderTapeTextureDimension::Cube
                                      ? 5u
                                      : 4u;
  std::vector<std::byte> descriptor(
      sizeof(fixed) + static_cast<std::size_t>(subresourceCount) *
                           sizeof(D9CSurfaceDesc));
  std::memcpy(descriptor.data(), &fixed, sizeof(fixed));
  for (std::uint32_t i = 0u; i < subresourceCount; ++i) {
    const D9CSurfaceDesc surface{
        .format = format,
        .resourceType = resourceType,
        .width = width,
        .height = height,
        .depth = dimension == RenderTapeTextureDimension::Volume ? width : 1u,
    };
    std::memcpy(descriptor.data() + sizeof(fixed) +
                    static_cast<std::size_t>(i) * sizeof(surface),
                &surface, sizeof(surface));
  }
  return descriptor;
}

void testMissingSeedDescriptorAndProvenanceTruthTable() {
  const D9CWireObjectIdentity texture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 9u, .objectId = 77u};
  const RenderTapeReferenceProvenance provenance{
      .handleIndex = 12u, .recordIndex = 34u,
      .recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE};
  auto descriptor = texture2DDescriptor();
  RenderTapeTextureDescriptorV2 header{};
  std::memcpy(&header, descriptor.data(), sizeof(header));
  D9CSurfaceDesc surface{};
  std::memcpy(&surface, descriptor.data() + sizeof(header), sizeof(surface));
  surface.usage = 0x1234u;
  surface.pool = 2u;
  surface.multiSampleType = 3u;
  surface.multiSampleQuality = 4u;
  std::memcpy(descriptor.data() + sizeof(header), &surface, sizeof(surface));

  const auto detail = renderTapeDescribeMissingSeed(
      texture, descriptor, 0u, provenance);
  check(detail.descriptorStatus ==
                RenderTapeMissingSeedDescriptorStatus::Accepted &&
            detail.expectedContentStatus ==
                RenderTapeExpectedContentStatus::Accepted &&
            detail.textureDimension == RenderTapeTextureDimension::Texture2D &&
            detail.mipLevelCount == 1u && detail.subresourceCount == 1u &&
            detail.missingSurface.usage == 0x1234u &&
            detail.missingSurface.resourceType == 3u &&
            detail.missingSurface.pool == 2u &&
            detail.missingSurface.format == 21u &&
            detail.missingSurface.width == 4u &&
            detail.missingSurface.height == 4u &&
            detail.missingSurface.multiSampleType == 3u &&
            detail.missingSurface.multiSampleQuality == 4u &&
            detail.expectedTightBytesValid && detail.expectedTightBytes == 64u,
        "missing-seed descriptor extracts V2 metadata and exact tight bytes");
  check(detail.identity.kind == texture.kind &&
            detail.identity.generation == texture.generation &&
            detail.identity.objectId == texture.objectId &&
            detail.provenance.handleIndex == provenance.handleIndex &&
            detail.provenance.recordIndex == provenance.recordIndex &&
            detail.provenance.recordType == provenance.recordType,
        "missing-seed diagnostic preserves original identity and command provenance");

  auto malformed = descriptor;
  malformed.resize(sizeof(RenderTapeTextureDescriptorV2));
  const auto malformedDetail = renderTapeDescribeMissingSeed(
      texture, malformed, 0u, provenance);
  check(malformedDetail.descriptorStatus ==
                RenderTapeMissingSeedDescriptorStatus::InvalidDescriptor &&
            malformedDetail.expectedContentStatus ==
                RenderTapeExpectedContentStatus::InvalidDescriptor &&
            !malformedDetail.expectedTightBytesValid &&
            malformedDetail.provenance.recordIndex == provenance.recordIndex,
        "malformed V2 descriptor fails closed without losing provenance");

  const auto missingDetail = renderTapeDescribeMissingSeed(
      texture, descriptor, 1u, provenance);
  check(missingDetail.descriptorStatus ==
                RenderTapeMissingSeedDescriptorStatus::MissingSubresource &&
            missingDetail.expectedContentStatus ==
                RenderTapeExpectedContentStatus::InvalidExtent &&
            missingDetail.subresourceCount == 1u &&
            missingDetail.provenance.handleIndex == provenance.handleIndex,
        "out-of-range missing subresource remains typed and provenance-qualified");

  check(std::string_view(renderTapeMissingSeedDescriptorStatusName(
            RenderTapeMissingSeedDescriptorStatus::InvalidDescriptor)) ==
            "invalid_descriptor",
        "missing-seed descriptor status names are stable");
}

void testArmBoundaryTransitionTruthTable() {
  using Phase = RenderTapeArmBoundaryPhase;
  constexpr std::array phases{
      Phase::Disabled, Phase::PresentFlushed, Phase::SnapshotComplete,
      Phase::Armed, Phase::FirstCapturedChunk};
  for (std::size_t from = 0u; from < phases.size(); ++from) {
    for (std::size_t to = 0u; to < phases.size(); ++to) {
      const auto transition = renderTapeAdvanceArmBoundary(
          phases[from], phases[to]);
      const bool expected = to == from + 1u;
      check(transition.accepted == expected &&
                transition.next == (expected ? phases[to] : phases[from]),
            "Present flush -> snapshot -> arm -> first-chunk truth table");
    }
  }

  const auto armA = renderTapeNextArmSnapshotEpoch(0u);
  const std::array<std::byte, 4u> durableBase{
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x3f}};
  const std::array<std::byte, 4u> snapshotA{
      std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3e}};
  const auto armB = renderTapeNextArmSnapshotEpoch(armA.ordinal);
  const std::array<std::byte, 4u> snapshotB{
      std::byte{0x00}, std::byte{0x00}, std::byte{0x40}, std::byte{0x3f}};
  const std::array<std::byte, 1u> durableDescriptor{std::byte{0x01}};
  const std::array<std::byte, 1u> snapshotDescriptor{std::byte{0x02}};
  const std::vector<std::vector<std::byte>> durableContent{
      std::vector<std::byte>(durableBase.begin(), durableBase.end())};
  const std::vector<std::vector<std::byte>> snapshotAContent{
      std::vector<std::byte>(snapshotA.begin(), snapshotA.end())};
  const std::vector<std::vector<std::byte>> snapshotBContent{
      std::vector<std::byte>(snapshotB.begin(), snapshotB.end())};
  const auto staleA = renderTapeSelectArmObjectSnapshotOverlay(
      durableDescriptor, durableContent, snapshotDescriptor, snapshotAContent,
      armA.ordinal, armB.ordinal);
  const auto currentB = renderTapeSelectArmObjectSnapshotOverlay(
      durableDescriptor, durableContent, snapshotDescriptor, snapshotBContent,
      armB.ordinal, armB.ordinal);
  const auto baseOnly = renderTapeSelectArmObjectSnapshotOverlay(
      durableDescriptor, durableContent, {}, {}, 0u, armB.ordinal);
  const auto activePresentOutput = renderTapeSelectArmObjectSnapshotOverlay(
      durableDescriptor, {}, snapshotDescriptor, snapshotBContent,
      armB.ordinal, armB.ordinal,
      RenderTapeArmObjectSnapshotOverlayPolicy::PresentOutput);
  check(armA.valid && armB.valid && snapshotA != snapshotB &&
            staleA.source == RenderTapeArmSnapshotOverlaySource::StaleArm &&
            staleA.descriptor.empty() && staleA.content.empty() &&
            currentB.source ==
                RenderTapeArmSnapshotOverlaySource::CurrentArm &&
            currentB.descriptor.size() == snapshotDescriptor.size() &&
            currentB.content.size() == 1u &&
            std::equal(currentB.content[0].begin(), currentB.content[0].end(),
                       snapshotB.begin(), snapshotB.end()) &&
            baseOnly.source ==
                RenderTapeArmSnapshotOverlaySource::DurableBase &&
            baseOnly.descriptor.size() == durableDescriptor.size() &&
            baseOnly.content.size() == 1u &&
            std::equal(baseOnly.content[0].begin(), baseOnly.content[0].end(),
                       durableBase.begin(), durableBase.end()) &&
            activePresentOutput.source ==
                RenderTapeArmSnapshotOverlaySource::DurableBase &&
            activePresentOutput.descriptor.size() == durableDescriptor.size() &&
            activePresentOutput.content.empty() &&
            activePresentOutput.descriptor[0] == durableDescriptor[0] &&
            snapshotBContent.size() == 1u &&
            std::equal(snapshotBContent[0].begin(), snapshotBContent[0].end(),
                       snapshotB.begin(), snapshotB.end()) &&
            durableBase == std::array<std::byte, 4u>{
                std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                std::byte{0x3f}},
        "production arm overlay selects B, rejects stale A, and leaves the durable base unchanged");
  check(activePresentOutput.source ==
                RenderTapeArmSnapshotOverlaySource::DurableBase &&
            activePresentOutput.content.empty() &&
            snapshotBContent[0] ==
                std::vector<std::byte>(snapshotB.begin(), snapshotB.end()),
        "active PresentOutput preserves zero/zero role content and leaves its displaced arm snapshot intact");
  const D9CSurfaceDesc backbuffer{
      .format = 22u,
      .resourceType = 1u,
      .usage = 1u,
      .pool = 0u,
      .width = 4u,
      .height = 4u,
      .depth = 1u,
  };
  const RenderTapeSurfaceDescriptorV2 producedOutput{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = backbuffer,
  };
  auto seededOutput = producedOutput;
  seededOutput.initialContentDisposition = static_cast<std::uint32_t>(
      RenderTapeInitialContentDisposition::CompleteSeed);
  const std::vector<std::vector<std::byte>> seededOutputContent{
      std::vector<std::byte>(4u * 4u * 4u, std::byte{0x5au})};
  const auto selectedOutputSeed = renderTapeSelectArmObjectSnapshotOverlay(
      std::as_bytes(std::span(&producedOutput, 1u)), {},
      std::as_bytes(std::span(&seededOutput, 1u)), seededOutputContent,
      armB.ordinal, armB.ordinal,
      RenderTapeArmObjectSnapshotOverlayPolicy::PresentOutput);
  check(selectedOutputSeed.source ==
                RenderTapeArmSnapshotOverlaySource::CurrentArm &&
            selectedOutputSeed.content.size() == 1u &&
            selectedOutputSeed.content[0] == seededOutputContent[0],
        "PresentOutput selects an exact current-arm CompleteSeed over its zero-content output role");
  check(renderTapeArmObjectSnapshotContentComplete(
            0u, false,
            RenderTapeArmObjectSnapshotOverlayPolicy::PresentOutput,
            selectedOutputSeed.source, selectedOutputSeed.content) &&
            !renderTapeArmObjectSnapshotContentComplete(
                0u, false,
                RenderTapeArmObjectSnapshotOverlayPolicy::PresentOutput,
                RenderTapeArmSnapshotOverlaySource::DurableBase,
                selectedOutputSeed.content) &&
            renderTapeArmObjectSnapshotContentComplete(
                7u, true,
                RenderTapeArmObjectSnapshotOverlayPolicy::Ordinary,
                RenderTapeArmSnapshotOverlaySource::Missing, {}),
        "current-arm PresentOutput CompleteSeed overrides its durable zero-content count while fallback and aliases stay exact");
  const auto overflow = renderTapeNextArmSnapshotEpoch(
      std::numeric_limits<std::uint64_t>::max());
  check(!overflow.valid && overflow.ordinal == 0u,
        "arm snapshot epoch overflow fails closed");
  check(renderTapeArmSnapshotCompletionAction(false) ==
                RenderTapeArmSnapshotCompletionAction::RetainForNextInterval &&
            renderTapeArmSnapshotCompletionAction(true) ==
                RenderTapeArmSnapshotCompletionAction::Clear,
        "sequence Accepted retains the arm overlay and final Complete clears it");
}

void testArmColorSnapshotDescriptorTruthTable() {
  const auto make = [](RenderTapeTextureDimension dimension,
                       std::uint32_t format) {
    const std::uint32_t count =
        dimension == RenderTapeTextureDimension::Cube ? 6u : 1u;
    const RenderTapeTextureDescriptorV2 header{
        .schemaVersion = kRenderTapeTextureDescriptorVersion2,
        .dimension = static_cast<std::uint32_t>(dimension),
        .mipLevelCount = 1u,
        .subresourceCount = count,
        .initialContentDisposition = static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::CompleteSeed),
    };
    std::vector<std::byte> descriptor(
        sizeof(header) + count * sizeof(D9CSurfaceDesc));
    std::memcpy(descriptor.data(), &header, sizeof(header));
    for (std::uint32_t face = 0u; face < count; ++face) {
      const D9CSurfaceDesc surface{
          .format = format,
          .resourceType = dimension == RenderTapeTextureDimension::Cube ? 5u
                                                                         : 3u,
          .usage = 1u,
          .pool = 0u,
          .multiSampleType = 0u,
          .multiSampleQuality = 0u,
          .width = 32u,
          .height = 32u,
          .depth = 1u,
      };
      std::memcpy(descriptor.data() + sizeof(header) +
                      face * sizeof(surface),
                  &surface, sizeof(surface));
    }
    return descriptor;
  };
  const auto texture2d = make(RenderTapeTextureDimension::Texture2D, 22u);
  const auto cube = make(RenderTapeTextureDimension::Cube, 114u);
  check(renderTapeArmColorSnapshotTextureSupported(texture2d) &&
            renderTapeArmColorSnapshotTextureSupported(cube),
        "arm snapshots accept exact X8R8G8B8 2D and all-face R32F cube shapes");
  D9CSurfaceDesc standalone{
      .format = 22u,
      .resourceType = 1u,
      .usage = 1u,
      .pool = 0u,
      .width = 32u,
      .height = 32u,
      .depth = 1u,
  };
  check(renderTapeArmColorSnapshotStandaloneSurfaceSupported(standalone),
        "arm snapshots accept exact standalone X8R8G8B8 render targets");
  standalone.format = 21u;
  check(!renderTapeArmColorSnapshotStandaloneSurfaceSupported(standalone),
        "standalone color snapshots reject formats outside exact GT2 shape");
  standalone.format = 22u;
  standalone.multiSampleType = 2u;
  check(!renderTapeArmColorSnapshotStandaloneSurfaceSupported(standalone),
        "standalone multisampled color snapshots remain fail-closed");

  auto badFace = cube;
  D9CSurfaceDesc face5{};
  std::memcpy(&face5,
              badFace.data() + sizeof(RenderTapeTextureDescriptorV2) +
                  5u * sizeof(face5),
              sizeof(face5));
  face5.format = 22u;
  std::memcpy(badFace.data() + sizeof(RenderTapeTextureDescriptorV2) +
                  5u * sizeof(face5),
              &face5, sizeof(face5));
  check(!renderTapeArmColorSnapshotTextureSupported(badFace),
        "one mismatched cube face rejects the complete snapshot shape");

  auto msaa = texture2d;
  D9CSurfaceDesc level{};
  std::memcpy(&level, msaa.data() + sizeof(RenderTapeTextureDescriptorV2),
              sizeof(level));
  level.multiSampleType = 2u;
  std::memcpy(msaa.data() + sizeof(RenderTapeTextureDescriptorV2), &level,
              sizeof(level));
  check(!renderTapeArmColorSnapshotTextureSupported(msaa),
        "MSAA color snapshots remain fail-closed");

  auto wrongShape = cube;
  RenderTapeTextureDescriptorV2 header{};
  std::memcpy(&header, wrongShape.data(), sizeof(header));
  header.mipLevelCount = 2u;
  std::memcpy(wrongShape.data(), &header, sizeof(header));
  check(!renderTapeArmColorSnapshotTextureSupported(wrongShape),
        "multi-mip cube snapshots remain fail-closed");
}

void testExpectedContentContractDerivation() {
  constexpr auto texture = D9C_CHUNK_HANDLE_KIND_TEXTURE;
  constexpr auto surface = D9C_CHUNK_HANDLE_KIND_SURFACE;
  constexpr auto buffer = D9C_CHUNK_HANDLE_KIND_BUFFER;
  const auto cube = versionedTextureDescriptor(
      RenderTapeTextureDimension::Cube, 1u, render_tape_d3d_format::DXT1, 4u,
      4u);
  std::array<std::uint64_t, 6u> cubeExtents{};
  auto contract = renderTapeDeriveExpectedContentContract(
      texture, cube, cubeExtents);
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.bytes == 6u * 8u && contract.count == 6u,
        "V2 cube CompleteSeed derives six exact DXT1 subresource extents");
  check(std::all_of(cubeExtents.begin(), cubeExtents.end(),
                    [](std::uint64_t bytes) { return bytes == 8u; }),
        "V2 cube exposes one exact tight extent per face");
  auto wrongCubeExtents = cubeExtents;
  wrongCubeExtents.front() = 0u;
  wrongCubeExtents.back() = 16u;
  check(renderTapeValidateExpectedContentExtents(texture, cube,
                                                 wrongCubeExtents) ==
            RenderTapeExpectedContentStatus::InvalidExtent,
        "wrong per-subresource distribution is rejected despite matching total");

  const auto r32fCube = versionedTextureDescriptor(
      RenderTapeTextureDimension::Cube, 1u,
      render_tape_d3d_format::R32F, 512u, 512u);
  std::array<std::uint64_t, 6u> r32fCubeExtents{};
  contract = renderTapeDeriveExpectedContentContract(
      texture, r32fCube, r32fCubeExtents);
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.bytes == 6u * 512u * 512u * 4u &&
            contract.count == 6u &&
            std::all_of(r32fCubeExtents.begin(), r32fCubeExtents.end(),
                        [](std::uint64_t bytes) {
                          return bytes == 512u * 512u * 4u;
                        }),
        "GT2 R32F cube derives six exact tight float32 face extents");

  const auto dxt2d = versionedTextureDescriptor(
      RenderTapeTextureDimension::Texture2D, 1u,
      render_tape_d3d_format::DXT3, 7u, 5u);
  contract = renderTapeDeriveExpectedContentContract(texture, dxt2d);
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.bytes == 2u * 2u * 16u && contract.count == 1u,
        "V2 2D CompleteSeed derives the exact compressed extent");

  const D9CSurfaceDesc standaloneSurfaceDesc{
      .format = render_tape_d3d_format::A8R8G8B8,
      .resourceType = 1u,
      .width = 4u,
      .height = 2u,
      .depth = 1u,
  };
  const auto standaloneSurface = RenderTapeSurfaceDescriptorV2{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::Standalone),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
      .surface = standaloneSurfaceDesc,
  };
  contract = renderTapeDeriveExpectedContentContract(
      surface, std::as_bytes(std::span(&standaloneSurface, 1u)));
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.bytes == 32u && contract.count == 1u,
        "V2 standalone surface derives its tight uncompressed extent");

  RenderTapeSurfaceDescriptorV2 snapshotDepth{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::Standalone),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteDepthFloat32V1),
      .surface = D9CSurfaceDesc{
          .format = render_tape_d3d_format::D24X8,
          .resourceType = 1u,
          .usage = 2u,
          .pool = 0u,
          .multiSampleType = 0u,
          .multiSampleQuality = 0u,
          .width = 4u,
          .height = 2u,
          .depth = 1u,
      },
  };
  contract = renderTapeDeriveExpectedContentContract(
      surface, std::as_bytes(std::span(&snapshotDepth, 1u)));
  check(renderTapeSnapshotStandaloneD24X8Supported(snapshotDepth.surface) &&
            contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.bytes == 32u && contract.count == 1u,
        "standalone D24X8 snapshot derives exact float32-v1 bytes");
  snapshotDepth.surface.format = render_tape_d3d_format::D24S8;
  check(!renderTapeSnapshotStandaloneD24X8Supported(snapshotDepth.surface),
        "D24S8 remains explicitly fail-closed");
  snapshotDepth.surface.format = render_tape_d3d_format::D24X8;
  snapshotDepth.surface.multiSampleType = 4u;
  check(!renderTapeSnapshotStandaloneD24X8Supported(snapshotDepth.surface),
        "multisampled D24X8 remains fail-closed");
  snapshotDepth.surface.multiSampleType = 0u;
  snapshotDepth.surface.usage = 3u;
  check(!renderTapeSnapshotStandaloneD24X8Supported(snapshotDepth.surface),
        "ambiguous D24X8 usage remains fail-closed");

  const D9CBufferDesc vb{.size = 257u};
  contract = renderTapeDeriveExpectedContentContract(
      buffer, std::as_bytes(std::span(&vb, 1u)));
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.bytes == 257u && contract.count == 1u,
        "D9CBufferDesc derives exact VB/IB bytes and one subresource");

  constexpr std::array<std::byte, 8u> immutableDescriptor{};
  contract = renderTapeDeriveExpectedContentContract(
      D9C_CHUNK_HANDLE_KIND_SHADER, immutableDescriptor);
  check(contract.status == RenderTapeExpectedContentStatus::NotRequired &&
            contract.bytes == 0u && contract.count == 0u,
        "immutable shader content remains zero/zero");

  RenderTapeSurfaceDescriptorV2 alias{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .parentTexture = D9CWireObjectIdentity{.kind = texture,
                                             .generation = 1u,
                                             .objectId = 2u},
      .surface = standaloneSurfaceDesc,
  };
  contract = renderTapeDeriveExpectedContentContract(
      surface, std::as_bytes(std::span(&alias, 1u)));
  check(contract.status == RenderTapeExpectedContentStatus::NotRequired &&
            contract.bytes == 0u && contract.count == 0u,
        "texture-derived aliases remain zero/zero");

  const auto malformed = versionedTextureDescriptor(
      RenderTapeTextureDimension::Texture2D, 1u, 0xdeadbeefu, 4u, 4u);
  contract = renderTapeDeriveExpectedContentContract(texture, malformed);
  check(contract.status == RenderTapeExpectedContentStatus::UnsupportedFormat,
        "unknown CompleteSeed format is rejected before ObjectDefine");
  const std::array<std::byte, sizeof(std::uint32_t)> shortV2 = [&] {
    std::array<std::byte, sizeof(std::uint32_t)> bytes{};
    const auto schema = kRenderTapeTextureDescriptorVersion2;
    std::memcpy(bytes.data(), &schema, sizeof(schema));
    return bytes;
  }();
  contract = renderTapeDeriveExpectedContentContract(texture, shortV2);
  check(contract.status == RenderTapeExpectedContentStatus::InvalidDescriptor,
        "short V2 descriptor is rejected before fixed-layout copy");
  auto overflow = versionedTextureDescriptor(
      RenderTapeTextureDimension::Texture2D, 1u,
      render_tape_d3d_format::A8R8G8B8, std::numeric_limits<std::uint32_t>::max(),
      2u);
  contract = renderTapeDeriveExpectedContentContract(texture, overflow);
  check(contract.status == RenderTapeExpectedContentStatus::Overflow,
        "overflowing CompleteSeed extent is rejected");
  auto incomplete = cube;
  incomplete.resize(incomplete.size() - sizeof(D9CSurfaceDesc));
  contract = renderTapeDeriveExpectedContentContract(texture, incomplete);
  check(contract.status == RenderTapeExpectedContentStatus::InvalidDescriptor,
        "malformed CompleteSeed descriptor is rejected");
  auto volume = versionedTextureDescriptor(
      RenderTapeTextureDimension::Volume, 1u,
      render_tape_d3d_format::A8R8G8B8, 4u, 4u);
  contract = renderTapeDeriveExpectedContentContract(
      texture, volume);
  check(contract.status == RenderTapeExpectedContentStatus::UnsupportedDimension,
        "V2 volume is rejected before ObjectDefine because seed closure is unsupported");
  const RenderTapeSurfaceDescriptorV2 depthSurface{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::Standalone),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
      .surface = D9CSurfaceDesc{.format = render_tape_d3d_format::D24S8,
                                .resourceType = 1u,
                                .width = 4u,
                                .height = 2u,
                                .depth = 1u},
  };
  contract = renderTapeDeriveExpectedContentContract(
      surface, std::as_bytes(std::span(&depthSurface, 1u)));
  check(contract.status == RenderTapeExpectedContentStatus::UnsupportedFormat,
        "depth/render-target format is classified unsupported by the linear table");
}

void testUpdateTextureClosureTruthTable() {
  auto sourceDescriptor = versionedTextureDescriptor(
      RenderTapeTextureDimension::Texture2D, 1u,
      render_tape_d3d_format::A8R8G8B8, 2u, 2u);
  auto destinationDescriptor = sourceDescriptor;
  const auto setPool = [](std::vector<std::byte>& descriptor,
                          std::uint32_t pool) {
    RenderTapeTextureDescriptorV2 fixed{};
    std::memcpy(&fixed, descriptor.data(), sizeof(fixed));
    for (std::uint32_t index = 0u; index < fixed.subresourceCount; ++index) {
      D9CSurfaceDesc desc{};
      const auto offset = sizeof(fixed) +
                          static_cast<std::size_t>(index) * sizeof(desc);
      std::memcpy(&desc, descriptor.data() + offset, sizeof(desc));
      desc.pool = pool;
      std::memcpy(descriptor.data() + offset, &desc, sizeof(desc));
    }
  };
  setPool(sourceDescriptor, 2u);       // D3DPOOL_SYSTEMMEM
  setPool(destinationDescriptor, 0u);  // D3DPOOL_DEFAULT
  const std::vector<std::byte> sourceBytes(16u, std::byte{0x5a});
  std::vector<std::vector<std::byte>> completeSource{sourceBytes};
  {
    std::vector<std::vector<std::byte>> destination(1u);
    check(applyRenderTapeUpdateTextureClosure(
              sourceDescriptor, completeSource, destinationDescriptor,
              destination) == RenderTapeUpdateTextureStatus::Accepted &&
              destination[0] == sourceBytes,
          "UpdateTexture establishes an exact empty destination seed");
    completeSource[0][0] = std::byte{0x7f};
    check(destination[0][0] == std::byte{0x5a},
          "UpdateTexture destination owns a copy independent of source mutation");
  }
  {
    std::vector<std::vector<std::byte>> destination{
        std::vector<std::byte>(16u, std::byte{0x22})};
    check(applyRenderTapeUpdateTextureClosure(
              sourceDescriptor, completeSource, destinationDescriptor,
              destination) == RenderTapeUpdateTextureStatus::Accepted &&
              destination[0] == completeSource[0],
          "UpdateTexture overwrites an already-seeded destination exactly");
  }
  {
    const std::vector<std::vector<std::byte>> incompleteSource{
        std::vector<std::byte>(4u, std::byte{0x11})};
    std::vector<std::vector<std::byte>> destination(1u);
    check(applyRenderTapeUpdateTextureClosure(
              sourceDescriptor, incompleteSource, destinationDescriptor,
              destination) == RenderTapeUpdateTextureStatus::IncompleteSource &&
              destination[0].empty(),
          "UpdateTexture leaves the destination unknown for an incomplete source");
  }
  {
    auto mismatchDescriptor = versionedTextureDescriptor(
        RenderTapeTextureDimension::Texture2D, 1u,
        render_tape_d3d_format::A8R8G8B8, 4u, 2u);
    setPool(mismatchDescriptor, 0u);
    std::vector<std::vector<std::byte>> destination{
        std::vector<std::byte>(8u, std::byte{0x22})};
    const auto prior = destination[0];
    check(applyRenderTapeUpdateTextureClosure(
              sourceDescriptor, completeSource, mismatchDescriptor,
              destination) == RenderTapeUpdateTextureStatus::
                                 DescriptorMismatch &&
              destination[0] == prior,
          "UpdateTexture rejects extent mismatch without changing destination");
  }
  {
    auto cubeSourceDescriptor = versionedTextureDescriptor(
        RenderTapeTextureDimension::Cube, 2u,
        render_tape_d3d_format::DXT1, 4u, 4u);
    auto cubeDestinationDescriptor = cubeSourceDescriptor;
    for (std::uint32_t face = 0u; face < 6u; ++face) {
      D9CSurfaceDesc mipOne{};
      const auto offset = sizeof(RenderTapeTextureDescriptorV2) +
                          static_cast<std::size_t>(face * 2u + 1u) *
                              sizeof(mipOne);
      std::memcpy(&mipOne, cubeSourceDescriptor.data() + offset,
                  sizeof(mipOne));
      mipOne.width = 2u;
      mipOne.height = 2u;
      std::memcpy(cubeSourceDescriptor.data() + offset, &mipOne,
                  sizeof(mipOne));
      std::memcpy(cubeDestinationDescriptor.data() + offset, &mipOne,
                  sizeof(mipOne));
    }
    setPool(cubeSourceDescriptor, 2u);
    setPool(cubeDestinationDescriptor, 0u);
    std::vector<std::vector<std::byte>> cubeSource(
        12u);
    std::vector<std::vector<std::byte>> cubeDestination(
        12u);
    for (std::uint32_t index = 0u; index < 12u; ++index) {
      cubeSource[index].assign(8u,
                               std::byte{static_cast<unsigned char>(0x30u + index)});
      cubeDestination[index].assign(8u, std::byte{0x44});
    }
    check(applyRenderTapeUpdateTextureClosure(
              cubeSourceDescriptor, cubeSource, cubeDestinationDescriptor,
              cubeDestination) == RenderTapeUpdateTextureStatus::Accepted &&
              cubeDestination == cubeSource,
          "UpdateTexture copies all cube face subresources in face-major order");
  }
  {
    auto autogenDescriptor = destinationDescriptor;
    D9CSurfaceDesc desc{};
    std::memcpy(&desc, autogenDescriptor.data() +
                              sizeof(RenderTapeTextureDescriptorV2),
                sizeof(desc));
    desc.usage = 0x00000400u;
    std::memcpy(autogenDescriptor.data() + sizeof(RenderTapeTextureDescriptorV2),
                &desc, sizeof(desc));
    std::vector<std::vector<std::byte>> destination(1u);
    check(applyRenderTapeUpdateTextureClosure(
              sourceDescriptor, completeSource, autogenDescriptor,
              destination) == RenderTapeUpdateTextureStatus::UnsupportedFormat &&
              destination[0].empty(),
          "UpdateTexture rejects autogen-mipmap closure");
  }
  {
    auto palettizedSourceDescriptor = versionedTextureDescriptor(
        RenderTapeTextureDimension::Texture2D, 1u,
        render_tape_d3d_format::P8, 2u, 2u);
    auto palettizedDestinationDescriptor = palettizedSourceDescriptor;
    setPool(palettizedSourceDescriptor, 2u);
    setPool(palettizedDestinationDescriptor, 0u);
    std::vector<std::vector<std::byte>> destination(1u);
    check(applyRenderTapeUpdateTextureClosure(
              palettizedSourceDescriptor,
              std::vector<std::vector<std::byte>>{
                  std::vector<std::byte>(4u, std::byte{0x11})},
              palettizedDestinationDescriptor, destination) ==
              RenderTapeUpdateTextureStatus::UnsupportedFormat,
          "UpdateTexture rejects palettized closure without palette proof");
  }
  {
    const auto volumeDescriptor = versionedTextureDescriptor(
        RenderTapeTextureDimension::Volume, 1u,
        render_tape_d3d_format::A8R8G8B8, 2u, 2u);
    std::vector<std::vector<std::byte>> destination(1u);
    check(applyRenderTapeUpdateTextureClosure(
              volumeDescriptor, completeSource, volumeDescriptor,
              destination) == RenderTapeUpdateTextureStatus::
                                 UnsupportedDimension,
          "UpdateTexture rejects volume closure");
  }
}

void testExpectedContentValidatorOrdering() {
  constexpr D9CWireObjectIdentity textureA{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 1u, .objectId = 301u};
  constexpr D9CWireObjectIdentity textureB{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 1u, .objectId = 302u};
  const auto descriptor = versionedTextureDescriptor(
      RenderTapeTextureDimension::Texture2D, 1u,
      render_tape_d3d_format::A8R8G8B8, 4u, 1u);
  const auto partial = std::vector<std::byte>(8u, std::byte{0x11});
  const auto complete = std::vector<std::byte>(16u, std::byte{0x22});
  const std::array<RenderTapeCaptureBlob, 2u> partialBlobs{
      RenderTapeCaptureBlob{.bytes = partial},
      RenderTapeCaptureBlob{.bytes = complete}};
  const auto run = [&](std::span<const RenderTapeCaptureBlob> blobs,
                       bool fullSeeds) {
    RenderTapeCaptureSession session(true);
    check(session.armWithBlobs(bootstrapChunk(), blobs) ==
              RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() ==
                  RenderTapeCaptureStatus::Accepted,
          "expected-content validator fixture starts");
    const auto surfaceDescriptor = outputSurfaceDescriptor();
    check(session.objectDefine(
              kSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
              std::as_bytes(std::span(&surfaceDescriptor, 1u)), 0u, {}) ==
                  RenderTapeCaptureStatus::Accepted,
          "validator fixture admits Present target");
    check(session.objectDefine(
              textureA, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
              descriptor, 0u, {}, 16u, 1u) == RenderTapeCaptureStatus::Accepted &&
              session.objectDefine(
                  textureB,
                  static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
                  descriptor, 0u, {}, 16u, 1u) ==
                  RenderTapeCaptureStatus::Accepted,
          "validator fixture journals dynamic texture contracts");
    const auto first = fullSeeds ? complete : partial;
    check(session.resourceMutationBytes(
              textureA, RenderTapeMutationKind::Upload, 0u, 0u, first) ==
              RenderTapeCaptureStatus::Accepted,
          "validator fixture records the first unique subresource mutation");
    check(session.resourceMutationBytes(
              textureB, RenderTapeMutationKind::Upload, 0u, 0u, complete) ==
              RenderTapeCaptureStatus::Accepted,
          "validator fixture records the second unique subresource mutation");
    const auto update = singleUpdateTextureChunk(textureA, textureB);
    check(session.commandChunk(
              CommandChunkEnvelope{.recordCount = 1u, .handleCount = 2u}, update) ==
              RenderTapeCaptureStatus::Accepted,
          "validator fixture admits the command-reference event to the journal");
    const auto present = presentChunk();
    check(session.commandChunk(
              CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present) ==
              RenderTapeCaptureStatus::Accepted,
          "validator fixture records the canonical Present event");
    const auto attachment = oracle();
    const auto status = session.completePresent(
        8u, 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&attachment, 1u)));
    return std::pair{status, session.validationResult().status};
  };
  const auto partialResult = run(partialBlobs, false);
  check(partialResult.first == RenderTapeCaptureStatus::ValidationFailed &&
            partialResult.second == RenderTapeValidationStatus::IncompleteFrame,
        "partial seed fails closed before first command reference");
  const std::array<RenderTapeCaptureBlob, 2u> completeBlobs{
      RenderTapeCaptureBlob{.bytes = complete},
      RenderTapeCaptureBlob{.bytes = complete}};
  const auto completeResult = run(completeBlobs, true);
  check(completeResult.first == RenderTapeCaptureStatus::Complete &&
            completeResult.second == RenderTapeValidationStatus::Valid,
        "unique complete seeds allow command reference and final validation");
}

void testFailureBeforePublishAndBoundedBackpressure() {
  RenderTapeCaptureSession bounded(
      true, RenderTapeCaptureLimits{.maxEvents = 1u});
  check(bounded.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "bounded capture arms at its event limit");
  check(bounded.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "bounded capture starts");
  const auto descriptor = outputSurfaceDescriptor();
  check(bounded.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&descriptor, 1u)), 0u, {}) ==
            RenderTapeCaptureStatus::CapacityExceeded,
        "bounded owner rejects before growing the journal");
  check(bounded.state() == RenderTapeCaptureState::Aborted,
        "backpressure aborts before publishing a partial artifact");
  check(bounded.sealedArtifact().empty(), "backpressure has no artifact");
  check(bounded.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            bounded.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted,
        "an aborted capture lifecycle can be re-armed independently");

  RenderTapeCaptureSession failed(true);
  check(failed.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "failure fixture arms");
  check(failed.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "failure fixture starts");
  check(failed.completePresent(
            1u, 1u, RenderTapeDigestValidity::NotCaptured, {}) ==
            RenderTapeCaptureStatus::InvalidInput,
        "missing Present chunk fails before publish");
  check(failed.state() == RenderTapeCaptureState::Aborted,
        "failed sealing aborts the owner");
  check(failed.sealedArtifact().empty(), "failed sealing has no partial tape");
}

void testBoundedBlobBytesAndDeduplication() {
  const RenderTapeCaptureLimits limits{.maxBlobBytes = 4u};
  const std::array<RenderTapeCaptureBlob, 2u> overflowing{
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(3u, std::byte{1})},
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(2u, std::byte{2})}};
  RenderTapeCaptureSession armOverflow(true, limits);
  check(armOverflow.armWithBlobs(bootstrapChunk(), overflowing) ==
            RenderTapeCaptureStatus::CapacityExceeded,
        "arm rejects the total blob-byte limit before retaining payloads");
  check(armOverflow.ownedBlobBytes() == 0u &&
            armOverflow.state() == RenderTapeCaptureState::Disabled,
        "failed arm leaves the blob-byte invariant untouched");

  RenderTapeCaptureSession incremental(true, limits);
  check(incremental.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "incremental blob fixture arms");
  check(incremental.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "incremental blob fixture starts");
  const std::array<std::byte, 3u> first{
      std::byte{0x10}, std::byte{0x11}, std::byte{0x12}};
  const std::array<std::byte, 2u> tooMuch{std::byte{0x20}, std::byte{0x21}};
  const std::array<std::byte, 1u> last{std::byte{0x30}};
  check(incremental.registerBlobBytes(first) ==
            RenderTapeCaptureStatus::Accepted &&
            incremental.ownedBlobBytes() == 3u,
        "incremental blob registration charges accepted bytes");
  check(incremental.registerBlobBytes(tooMuch) ==
            RenderTapeCaptureStatus::CapacityExceeded &&
            incremental.ownedBlobBytes() == 3u,
        "incremental overflow preserves the owned-byte count");
  check(incremental.registerBlobBytes(last) ==
            RenderTapeCaptureStatus::Accepted &&
            incremental.ownedBlobBytes() == 4u,
        "the exact incremental byte bound is accepted");
  check(incremental.registerBlobBytes(first) ==
            RenderTapeCaptureStatus::Accepted &&
            incremental.ownedBlobBytes() == 4u,
        "deduplicated bytes do not double-charge the bound");

  const std::array<RenderTapeCaptureBlob, 2u> exact{
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(2u, std::byte{3})},
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(2u, std::byte{4})}};
  RenderTapeCaptureSession exactArm(true, limits);
  check(exactArm.armWithBlobs(bootstrapChunk(), exact) ==
            RenderTapeCaptureStatus::Accepted &&
            exactArm.ownedBlobBytes() == 4u,
        "arm accepts the exact total blob-byte bound");
  const auto duplicate = exact[0].bytes;
  check(exactArm.registerBlobBytes(duplicate) ==
            RenderTapeCaptureStatus::Accepted &&
            exactArm.ownedBlobBytes() == 4u,
        "arm-owned duplicate does not double-charge bytes");

  const std::array<RenderTapeCaptureBlob, 2u> duplicateSeeds{
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(
                                3u, std::byte{0x55})},
      RenderTapeCaptureBlob{.bytes = std::vector<std::byte>(
                                3u, std::byte{0x55})},
  };
  RenderTapeCaptureSession duplicateArm(true,
                                        RenderTapeCaptureLimits{
                                            .maxBlobEntries = 1u,
                                            .maxBlobBytes = 3u});
  check(duplicateArm.armWithBlobs(bootstrapChunk(), duplicateSeeds) ==
            RenderTapeCaptureStatus::Accepted &&
            duplicateArm.ownedBlobBytes() == 3u,
        "duplicate bootstrap blobs consume one entry and one byte charge");
}

void testRenderTapeBlobCapacityResolverTruthTable() {
  struct ResolverCase {
    std::string_view value;
    std::uint64_t expected;
  };
  constexpr std::array cases{
      ResolverCase{"", kRenderTapeDefaultMaxBlobBytes},
      ResolverCase{"invalid", kRenderTapeDefaultMaxBlobBytes},
      ResolverCase{"0", kRenderTapeDefaultMaxBlobBytes},
      ResolverCase{"000", kRenderTapeDefaultMaxBlobBytes},
      ResolverCase{"+1", kRenderTapeDefaultMaxBlobBytes},
      ResolverCase{"1x", kRenderTapeDefaultMaxBlobBytes},
      ResolverCase{"268435456", 268435456u},
      ResolverCase{"1073741824", kRenderTapeHardMaxBlobBytes},
      ResolverCase{"1073741825", kRenderTapeHardMaxBlobBytes},
      ResolverCase{"18446744073709551616", kRenderTapeDefaultMaxBlobBytes},
  };
  for (const auto& testCase : cases) {
    check(dxmt9PeRenderTapeMaxBlobBytesFromText(testCase.value) ==
              testCase.expected,
          "Render Tape blob capacity resolver truth table is stable");
  }
}

void testProductionBlobDefaultIsCaptureBounded() {
  constexpr std::uint64_t gt2R7RequiredBytes = 67371903u;
  const RenderTapeCaptureLimits limits{};
  check(limits.maxBlobBytes == kRenderTapeDefaultMaxBlobBytes &&
            limits.maxBlobBytes >= gt2R7RequiredBytes,
        "the capture-only blob default covers the measured GT2 r7 lower bound");
  check(kRenderTapeDefaultMaxBlobBytes < kRenderTapeHardMaxBlobBytes,
        "the capture-only blob default remains below the hard ceiling");
  const RenderTapeCaptureLimits overMax{
      .maxBlobBytes = kRenderTapeHardMaxBlobBytes + 1u};
  RenderTapeCaptureSession clamped(true, overMax);
  check(clamped.limits().maxBlobBytes == kRenderTapeHardMaxBlobBytes,
        "the capture session enforces the hard blob ceiling");
}

void testObjectLifetimeAndTerminalControls() {
  const auto descriptor = outputSurfaceDescriptor();
  const auto descriptorBytes = std::as_bytes(std::span(&descriptor, 1u));
  RenderTapeCaptureSession lifetime(true);
  check(lifetime.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "lifetime fixture arms");
  check(lifetime.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "lifetime fixture starts");
  check(lifetime.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "lifetime definition succeeds");
  check(lifetime.objectDestroy(kSurface) == RenderTapeCaptureStatus::Accepted,
        "lifetime destroy succeeds");
  auto newerSurface = kSurface;
  ++newerSurface.generation;
  check(lifetime.objectDefine(
            newerSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}) == RenderTapeCaptureStatus::Accepted,
        "capture session admits a strictly newer destroyed-slot generation");
  auto overlappingSurface = newerSurface;
  ++overlappingSurface.generation;
  RenderTapeObjectDefineDisposition defineDisposition{};
  check(lifetime.objectDefine(
            overlappingSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &defineDisposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::OverlappingLiveGeneration,
        "capture session rejects overlapping live slot generations");
  check(lifetime.objectDestroy(newerSurface) ==
            RenderTapeCaptureStatus::Accepted,
        "newer lifetime generation retires exactly");
  check(lifetime.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &defineDisposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::ExactIdentityConflict &&
            lifetime.objectDefine(
                newerSurface,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                descriptorBytes, 0u, {}, 0u, 0u, &defineDisposition) ==
                RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::ExactIdentityConflict,
        "capture session keeps exact definitions globally unique after destroy");
  auto futureSurface = newerSurface;
  futureSurface.generation += 2u;
  check(lifetime.objectDefine(
            futureSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}) == RenderTapeCaptureStatus::Accepted &&
            lifetime.objectDestroy(futureSurface) ==
                RenderTapeCaptureStatus::Accepted,
        "an unseen monotone generation advances a destroyed ordinary slot");
  auto staleSurface = newerSurface;
  ++staleSurface.generation;
  check(lifetime.objectDefine(
            staleSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &defineDisposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::StaleOrEqualGeneration,
        "an unseen stale generation cannot re-enter a destroyed slot");
  const auto value = digest();
  check(lifetime.resourceMutation(
            newerSurface, RenderTapeMutationKind::Upload, 0u, 0u, 4u,
            std::span<const std::byte, kRenderTapeDigestSize>(value)) ==
            RenderTapeCaptureStatus::InvalidInput,
        "retired object cannot receive a mutation");

  constexpr D9CWireObjectIdentity parentTexture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 4u,
      .objectId = 900u,
  };
  constexpr D9CSurfaceDesc aliasSurface{
      .format = render_tape_d3d_format::A8R8G8B8,
      .resourceType = 1u,
      .width = 64u,
      .height = 32u,
      .depth = 1u,
  };
  const RenderTapeTextureDescriptorV2 parentDescriptor{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u,
      .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  auto parentLevel = aliasSurface;
  parentLevel.resourceType = 3u;
  std::array<std::byte, sizeof(parentDescriptor) + sizeof(parentLevel)>
      parentDescriptorBytes{};
  std::memcpy(parentDescriptorBytes.data(), &parentDescriptor,
              sizeof(parentDescriptor));
  std::memcpy(parentDescriptorBytes.data() + sizeof(parentDescriptor),
              &parentLevel, sizeof(parentLevel));
  const RenderTapeSurfaceDescriptorV2 aliasDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = parentTexture,
      .surface = aliasSurface,
  };
  RenderTapeCaptureSession aliasLifetime(true);
  check(aliasLifetime.arm(bootstrapChunk()) ==
            RenderTapeCaptureStatus::Accepted &&
            aliasLifetime.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted,
        "surface-alias lifetime fixture starts");
  check(aliasLifetime.objectDefine(
            parentTexture,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
            parentDescriptorBytes, 0u, {}, 4u, 1u) ==
            RenderTapeCaptureStatus::Accepted &&
            aliasLifetime.objectDefine(
                kSurface,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                std::as_bytes(std::span(&aliasDescriptor, 1u)), 0u, {}, 0u,
                0u, &defineDisposition) == RenderTapeCaptureStatus::Accepted,
        "surface alias follows its exact parent definition with no seed extent");
  const auto aliasEventCount = aliasLifetime.eventCount();
  check(aliasLifetime.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&aliasDescriptor, 1u)), 0u, {}, 0u, 0u,
            &defineDisposition) == RenderTapeCaptureStatus::Accepted &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::IdempotentSurfaceAlias &&
            aliasLifetime.eventCount() == aliasEventCount &&
            std::string_view(renderTapeObjectDefineDispositionName(
                defineDisposition)) == "idempotent_surface_alias",
        "an exact lazy surface wrapper is idempotent and emits no redefinition");
  auto conflictingAlias = aliasDescriptor;
  conflictingAlias.subresource = 1u;
  check(aliasLifetime.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&conflictingAlias, 1u)), 0u, {}, 0u, 0u,
            &defineDisposition) == RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::ExactIdentityConflict,
        "an exact identity with conflicting alias metadata still rejects");
  auto secondAliasIdentity = kSurface;
  ++secondAliasIdentity.generation;
  check(aliasLifetime.objectDefine(
            secondAliasIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&conflictingAlias, 1u)), 0u, {}, 0u, 0u,
            &defineDisposition) == RenderTapeCaptureStatus::Accepted,
        "one wire object id admits a distinct texture subresource alias");
  auto overlappingAliasIdentity = secondAliasIdentity;
  ++overlappingAliasIdentity.generation;
  check(aliasLifetime.objectDefine(
            overlappingAliasIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&conflictingAlias, 1u)), 0u, {}, 0u, 0u,
            &defineDisposition) == RenderTapeCaptureStatus::InvalidInput &&
            defineDisposition ==
                RenderTapeObjectDefineDisposition::OverlappingLiveGeneration,
        "one logical alias slot still rejects overlapping live generations");
  auto wrongDestroyIdentity = secondAliasIdentity;
  ++wrongDestroyIdentity.generation;
  check(aliasLifetime.objectDestroy(wrongDestroyIdentity) ==
            RenderTapeCaptureStatus::InvalidInput &&
            aliasLifetime.objectDestroy(secondAliasIdentity) ==
                RenderTapeCaptureStatus::Accepted,
        "surface alias destruction resolves the exact identity");
  auto ordinarySurfaceIdentity = kSurface;
  ordinarySurfaceIdentity.generation += 18u;
  const auto ordinarySurfaceDescriptor = outputSurfaceDescriptor();
  check(aliasLifetime.objectDefine(
            ordinarySurfaceIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&ordinarySurfaceDescriptor, 1u)), 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "an ordinary surface may share an alias wrapper wire object id");

  RenderTapeCaptureSession reset(true);
  check(reset.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "reset fixture arms");
  check(reset.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "reset fixture starts");
  const RenderTapeResetControl resetPayload{};
  const RenderTapeOrderedControlHeader resetHeader{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::Reset),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Terminal),
      .controlBytes = sizeof(resetPayload),
      .completionOrdinal = 3u,
  };
  check(reset.orderedControl(
            resetHeader, std::as_bytes(std::span(&resetPayload, 1u))) ==
            RenderTapeCaptureStatus::Terminal,
        "successful Reset terminates the interval");
  check(reset.state() == RenderTapeCaptureState::Aborted &&
            reset.sealedArtifact().empty(),
        "Reset aborts without a partial artifact");

  RenderTapeCaptureSession lost(true);
  check(lost.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "device-lost fixture arms");
  check(lost.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "device-lost fixture starts");
  const RenderTapeDeviceLostControl lostPayload{.hrCode = 0x88760868u};
  const RenderTapeOrderedControlHeader lostHeader{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::DeviceLost),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Terminal),
      .resultCode = static_cast<std::int32_t>(lostPayload.hrCode),
      .controlBytes = sizeof(lostPayload),
      .completionOrdinal = 4u,
  };
  check(lost.orderedControl(
            lostHeader, std::as_bytes(std::span(&lostPayload, 1u))) ==
            RenderTapeCaptureStatus::Terminal,
        "device lost terminates the interval");
  check(lost.state() == RenderTapeCaptureState::Aborted &&
            lost.sealedArtifact().empty(),
        "device lost has no partial artifact");
}

void testPendingChunkLifetimeTruthTable() {
  RenderTapeSurfaceAliasLifetime ordinary;
  check(ordinary.acquire(), "ordinary wrapper acquires");
  check(ordinary.retainPendingChunk() && ordinary.pendingChunkRefs == 1u,
        "pending chunk ownership is bounded and retained");
  check(!ordinary.releaseWrapper() && ordinary.wrapperRefs == 0u &&
            ordinary.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::RetainedChunk,
        "last wrapper release transfers ordinary identity to pending chunk");
  check(ordinary.releasePendingChunk() && ordinary.pendingChunkRefs == 0u &&
            ordinary.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::Retired,
        "ordinary pending release retires exactly at the drain point");
  check(!ordinary.releasePendingChunk() && !ordinary.acquire(),
        "drained pending ownership cannot be released or resurrected");

  RenderTapeSurfaceAliasLifetime reacquired;
  check(reacquired.acquire() && reacquired.retainPendingChunk() &&
            !reacquired.releaseWrapper() && reacquired.acquire() &&
            !reacquired.releaseWrapper() && reacquired.pendingChunkRefs == 1u &&
            !reacquired.retainPendingChunk(),
        "reacquire/release before one builder drain stays at one pending ref");
  check(reacquired.releasePendingChunk() &&
            reacquired.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::Retired,
        "one pending drain retires the reacquired wrapper identity");

  RenderTapeSurfaceAliasLifetime alias;
  alias.textureAlias = true;
  check(alias.acquire() && alias.retainPendingChunk() &&
            !alias.releaseWrapper() && alias.wrapperRefs == 0u &&
            alias.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::RetainedChunk,
        "alias keeps both parent and pending lifetime axes independent");
  check(!alias.releasePendingChunk() &&
            alias.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::RetainedAlias,
        "alias pending drain preserves identity until parent retirement");
  check(alias.retireParent() &&
            alias.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::Retired,
        "alias parent retirement is the final destroy transition");

  RenderTapeSurfaceAliasLifetime parentRetiresWhilePending;
  parentRetiresWhilePending.textureAlias = true;
  check(parentRetiresWhilePending.acquire() &&
            parentRetiresWhilePending.retainPendingChunk() &&
            !parentRetiresWhilePending.releaseWrapper() &&
            !parentRetiresWhilePending.retireParent() &&
            parentRetiresWhilePending.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::RetainedChunk,
        "parent retirement waits for the pending alias command");
  check(parentRetiresWhilePending.releasePendingChunk() &&
            parentRetiresWhilePending.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::Retired,
        "pending alias command retires after an already-retired parent");
}

void testGammaRampOrderedControlIsCaptured() {
  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
        "gamma capture fixture arms");
  check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "gamma capture fixture starts one interval");
  std::array<std::uint16_t, kRenderTapeGammaRampBytes / sizeof(std::uint16_t)>
      ramp{};
  for (std::size_t index = 0u; index < ramp.size(); ++index) {
    ramp[index] = static_cast<std::uint16_t>((index % 256u) << 8u);
    if (index == 17u) ramp[index] ^= 0x0100u;
  }
  const RenderTapeOrderedControlHeader header{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::GammaRampSet),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = kRenderTapeGammaRampBytes,
      .completionOrdinal = 1u,
  };
  check(session.orderedControl(header, std::as_bytes(std::span(ramp))) ==
            RenderTapeCaptureStatus::Accepted,
        "GammaRampSet is admitted as an ordered capture control");
}

// R-RT-CAP-9.4: the PresentOutput role is capture-owned and single-holder.
// GT2 frame-tape retries (artifact
// experiments/output/app-d3d9-3dmark05-gt2-frame-tape-exact-closure-r6-20260814)
// re-admitted a fresh back-buffer wrapper per attempt while the previous
// holder stayed live and roled: the arm saw `present_output_count count=2..8`,
// and a recycled wire object id then met the stale entry in the logical-slot
// scan and rejected as `prior_not_retained_alias`, marking the registry
// invalid for the rest of the process.
void testPresentOutputRoleOwnershipTruthTable() {
  constexpr D9CWireObjectIdentity held{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 0x10000009bull,
  };
  auto recycled = held;
  recycled.generation = 10u;
  D9CWireObjectIdentity other = held;
  other.objectId = held.objectId + 1u;

  using Transition = RenderTapePresentOutputRoleTransition;
  const RenderTapePresentOutputRole unheld{};
  check(renderTapePresentOutputRoleTransition(unheld, &other, true, 1u) ==
            Transition::None,
        "an unheld role has nothing to hand back");

  const RenderTapePresentOutputRole captureOwned{
      .identity = held, .held = true, .captureOwned = true};
  check(renderTapePresentOutputRoleTransition(captureOwned, &held, true, 1u) ==
            Transition::Retained,
        "re-admitting the same exact identity keeps the role in place");
  check(renderTapePresentOutputRoleTransition(captureOwned, nullptr, true,
                                              1u) == Transition::Retire,
        "a capture-owned holder with only the admission reference retires");
  check(renderTapePresentOutputRoleTransition(captureOwned, &other, true, 1u) ==
            Transition::Retire,
        "naming a different holder retires the capture-owned entry");
  check(renderTapePresentOutputRoleTransition(captureOwned, &recycled, true,
                                              1u) == Transition::Retire,
        "a recycled wire object id retires the stale holder before "
        "registration sees it");
  check(renderTapePresentOutputRoleTransition(captureOwned, &other, false,
                                              0u) == Transition::None,
        "a holder already gone from the live registry needs no transition");
  check(renderTapePresentOutputRoleTransition(captureOwned, &held, false, 0u) ==
            Transition::None,
        "re-admitting an identity that left the registry is a fresh admission, "
        "not a retained role");
  check(renderTapePresentOutputRoleTransition(captureOwned, &other, true, 2u) ==
            Transition::Demote,
        "a surviving app wrapper demotes instead of retiring");

  // Retirement is scoped to the swap-chain output handoff. Anything the
  // capture only re-roled stays registered, so the alias rules keep owning it.
  auto nonSurface = held;
  nonSurface.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
  const RenderTapePresentOutputRole captureOwnedTexture{
      .identity = nonSurface, .held = true, .captureOwned = true};
  check(renderTapePresentOutputRoleTransition(captureOwnedTexture, &other, true,
                                              1u) == Transition::Demote,
        "only a surface holder takes the swap-chain output retirement");

  const RenderTapePresentOutputRole appOwned{
      .identity = held, .held = true, .captureOwned = false};
  check(renderTapePresentOutputRoleTransition(appOwned, &other, true, 1u) ==
            Transition::Demote,
        "an app-owned holder is only re-roled, so it is only demoted");
  check(renderTapePresentOutputRoleTransition(appOwned, nullptr, true, 0u) ==
            Transition::Demote,
        "an app-owned holder without wrapper references still demotes");

  // The transition never inspects generations. Monotonicity stays owned by
  // registration, so handing the role back cannot admit an identity that
  // registration would reject.
  auto stale = held;
  stale.generation = 1u;
  check(renderTapePresentOutputRoleTransition(captureOwned, &stale, true, 1u) ==
            Transition::Retire,
        "a stale generation is retired here and rejected by registration");

  check(std::string_view(renderTapePresentOutputRoleTransitionName(
            Transition::Retire)) == "retire" &&
            std::string_view(renderTapePresentOutputRoleTransitionName(
                Transition::Demote)) == "demote" &&
            std::string_view(renderTapePresentOutputRoleTransitionName(
                Transition::Retained)) == "retained" &&
            std::string_view(renderTapePresentOutputRoleTransitionName(
                Transition::None)) == "none",
        "every present output transition has a stable log name");
}

void testStandaloneSurfaceIdentityClosureTruthTable() {
  check(renderTapeSurfaceRegistrationRoute(false) ==
            RenderTapeSurfaceRegistrationRoute::Standalone &&
            renderTapeSurfaceRegistrationRoute(true) ==
                RenderTapeSurfaceRegistrationRoute::TextureParentAlias,
        "standalone wrappers register independently while texture levels use the parent");

  constexpr D9CWireObjectIdentity surface{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 4u,
      .objectId = 0x100000041ull,
  };
  check(renderTapePresentOutputIdentityMatchesCommand(surface, surface),
        "PresentOutput is pinned to the command-visible cached backbuffer identity");
  auto secondWrapper = surface;
  ++secondWrapper.objectId;
  check(!renderTapePresentOutputIdentityMatchesCommand(surface, secondWrapper),
        "a separately wrapped backbuffer identity cannot become the oracle target");
  auto descriptor = standaloneSurfaceDescriptor();
  descriptor.surface.width = 640u;
  descriptor.surface.height = 480u;
  const auto descriptorBytes = std::as_bytes(std::span(&descriptor, 1u));
  const auto slot = renderTapeLogicalObjectSlot(surface, descriptorBytes);
  check(!slot.textureSubresourceAlias && !slot.malformedSurfaceDescriptor,
        "a standalone surface keeps an independent logical slot");

  RenderTapeCaptureSession session(true);
  RenderTapeObjectDefineDisposition disposition{};
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            session.objectDefine(
                surface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                descriptorBytes, 0u, {}, 4u, 1u) ==
                RenderTapeCaptureStatus::Accepted,
        "a standalone wrapper identity is admitted with its exact descriptor");
  check(session.objectDefine(
            surface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 4u, 1u, &disposition) ==
                RenderTapeCaptureStatus::InvalidInput &&
            disposition == RenderTapeObjectDefineDisposition::ExactIdentityConflict,
        "the tape validator rejects a repeated standalone identity as a conflicting definition");

  auto stale = surface;
  --stale.generation;
  check(session.objectDestroy(surface) == RenderTapeCaptureStatus::Accepted &&
            session.objectDefine(
                stale, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                descriptorBytes, 0u, {}, 4u, 1u, &disposition) ==
                RenderTapeCaptureStatus::InvalidInput &&
            disposition == RenderTapeObjectDefineDisposition::StaleOrEqualGeneration,
        "standalone surface reuse remains exact-generation fail-closed");

  RenderTapeSurfaceAliasLifetime standalone;
  check(standalone.acquire() && standalone.acquire() &&
            standalone.wrapperRefs == 2u && !standalone.releaseWrapper() &&
            standalone.wrapperRefs == 1u && standalone.releaseWrapper() &&
            standalone.wrapperRefs == 0u &&
            standalone.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::Retired,
        "one standalone logical slot tracks two wrapper refs and retires after both release");
}

void testSurfaceAliasGenerationReplacementTransition() {
  constexpr D9CWireObjectIdentity priorIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 0x100000002ull,
  };
  constexpr D9CWireObjectIdentity nextIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 3u,
      .objectId = priorIdentity.objectId,
  };
  constexpr D9CWireObjectIdentity parentIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 7u,
      .objectId = 0x100000009ull,
  };
  constexpr D9CSurfaceDesc surface{
      .format = render_tape_d3d_format::A8R8G8B8,
      .resourceType = 1u,
      .width = 128u,
      .height = 32u,
      .depth = 1u,
  };
  const RenderTapeSurfaceDescriptorV2 descriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = parentIdentity,
      .surface = surface,
  };
  const auto descriptorBytes = std::as_bytes(std::span(&descriptor, 1u));
  RenderTapeSurfaceAliasLifetime retained{
      .wrapperRefs = 0u,
      .textureAlias = true,
      .disposition =
          RenderTapeSurfaceAliasLifetime::Disposition::RetainedAlias,
  };
  using Status = RenderTapeSurfaceAliasReplacementStatus;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            descriptorBytes) == Status::Accepted,
        "a retained alias admits a semantically identical newer generation");

  auto pending = retained;
  pending.pendingChunkRefs = 1u;
  pending.disposition =
      RenderTapeSurfaceAliasLifetime::Disposition::RetainedChunk;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, pending, descriptorBytes, nextIdentity,
            descriptorBytes) == Status::PendingChunkRequiresFlush,
        "a pending alias generation must flush before logical-slot replacement");

  auto staleIdentity = nextIdentity;
  staleIdentity.generation = priorIdentity.generation;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, staleIdentity,
            descriptorBytes) == Status::NonMonotoneGeneration,
        "one wire object id rejects equal and stale alias generations");

  auto crossObjectEqual = nextIdentity;
  ++crossObjectEqual.objectId;
  crossObjectEqual.generation = priorIdentity.generation;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, crossObjectEqual,
            descriptorBytes) == Status::Accepted,
        "event order admits an equal generation from a distinct wire object id");
  auto crossObjectLower = crossObjectEqual;
  ++crossObjectLower.objectId;
  crossObjectLower.generation = 1u;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, crossObjectLower,
            descriptorBytes) == Status::Accepted,
        "event order admits a lower generation from a distinct wire object id");

  auto live = retained;
  live.wrapperRefs = 1u;
  live.disposition = RenderTapeSurfaceAliasLifetime::Disposition::Live;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, live, descriptorBytes, nextIdentity,
            descriptorBytes) == Status::LiveWrapper,
        "a live alias wrapper prevents generation replacement");

  auto standalone = retained;
  standalone.textureAlias = false;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, standalone, descriptorBytes, nextIdentity,
            descriptorBytes) == Status::PriorNotRetainedAlias,
        "a standalone surface never enters alias generation replacement");
  const auto ordinarySurface = outputSurfaceDescriptor();
  const auto ordinaryDescriptor =
      std::as_bytes(std::span(&ordinarySurface, 1u));
  check(renderTapeLogicalSlotRelation(
            renderTapeLogicalObjectSlot(priorIdentity, descriptorBytes),
            renderTapeLogicalObjectSlot(nextIdentity, ordinaryDescriptor)) ==
            RenderTapeLogicalSlotRelation::Different,
        "an alias and ordinary surface never collide by shared wire object id");
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            ordinaryDescriptor) == Status::DifferentLogicalSlot,
        "mixed alias and ordinary surfaces coexist instead of replacing");

  auto differentParent = descriptor;
  ++differentParent.parentTexture.generation;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            std::as_bytes(std::span(&differentParent, 1u))) ==
            Status::DifferentLogicalSlot,
        "a different exact parent names a distinct logical alias slot");

  auto differentSubresource = descriptor;
  ++differentSubresource.subresource;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            std::as_bytes(std::span(&differentSubresource, 1u))) ==
            Status::DifferentLogicalSlot,
        "a different parent subresource names a distinct logical alias slot");
  check(renderTapeLogicalSlotRelation(
            renderTapeLogicalObjectSlot(priorIdentity, descriptorBytes),
            renderTapeLogicalObjectSlot(
                nextIdentity,
                std::as_bytes(std::span(&differentSubresource, 1u)))) ==
            RenderTapeLogicalSlotRelation::Different,
        "shared wire object ids do not merge distinct alias subresources");

  auto differentSurface = descriptor;
  ++differentSurface.surface.width;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            std::as_bytes(std::span(&differentSurface, 1u))) ==
            Status::SurfaceMismatch,
        "alias generation replacement preserves the semantic surface descriptor");

  auto invalidDescriptor = descriptor;
  invalidDescriptor.parentTexture.generation = 0u;
  check(renderTapeSurfaceAliasReplacementStatus(
            priorIdentity, retained, descriptorBytes, nextIdentity,
            std::as_bytes(std::span(&invalidDescriptor, 1u))) ==
            Status::InvalidDescriptor,
        "malformed alias metadata prevents generation replacement");

  RenderTapeCaptureSession orderedAlias(true);
  RenderTapeObjectDefineDisposition disposition{};
  check(orderedAlias.arm(bootstrapChunk()) ==
            RenderTapeCaptureStatus::Accepted &&
            orderedAlias.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted &&
            orderedAlias.objectDefine(
                priorIdentity,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                descriptorBytes, 0u, {}) == RenderTapeCaptureStatus::Accepted &&
            orderedAlias.objectDestroy(priorIdentity) ==
                RenderTapeCaptureStatus::Accepted,
        "ordered alias replacement fixture retires its first exact identity");
  check(orderedAlias.objectDefine(
            priorIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &disposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            disposition ==
                RenderTapeObjectDefineDisposition::ExactIdentityConflict,
        "a retired exact alias identity cannot be defined again");
  auto sameObjectStale = priorIdentity;
  sameObjectStale.generation = 1u;
  check(orderedAlias.objectDefine(
            sameObjectStale,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &disposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            disposition ==
                RenderTapeObjectDefineDisposition::StaleOrEqualGeneration,
        "a retired alias wire object rejects an unseen lower generation");
  auto lowerCrossObject = priorIdentity;
  ++lowerCrossObject.objectId;
  lowerCrossObject.generation = 1u;
  check(orderedAlias.objectDefine(
            lowerCrossObject,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &disposition) ==
            RenderTapeCaptureStatus::Accepted,
        "a lower generation from a new wire object follows ordered destroy");
  auto overlappingCrossObject = lowerCrossObject;
  ++overlappingCrossObject.objectId;
  check(orderedAlias.objectDefine(
            overlappingCrossObject,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}, 0u, 0u, &disposition) ==
            RenderTapeCaptureStatus::InvalidInput &&
            disposition ==
                RenderTapeObjectDefineDisposition::OverlappingLiveGeneration,
        "a second live cross-object alias generation remains rejected");

  RenderTapeCaptureSession missingDestroy(true);
  check(missingDestroy.arm(bootstrapChunk()) ==
            RenderTapeCaptureStatus::Accepted &&
            missingDestroy.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted &&
            missingDestroy.objectDestroy(priorIdentity) ==
                RenderTapeCaptureStatus::InvalidInput,
        "a missing exact capture identity makes the ordered destroy fail closed");
}

void testPendingAliasFlushBeforeReplacementSequence() {
  constexpr D9CWireObjectIdentity oldIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = 0x100000132ull,
  };
  constexpr D9CWireObjectIdentity newIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = 0x100000135ull,
  };
  constexpr D9CWireObjectIdentity parentIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 4u,
      .objectId = 0x100000120ull,
  };
  constexpr D9CSurfaceDesc surface{
      .format = render_tape_d3d_format::A8R8G8B8,
      .resourceType = 1u,
      .width = 64u,
      .height = 64u,
      .depth = 1u,
  };
  const RenderTapeSurfaceDescriptorV2 descriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = parentIdentity,
      .surface = surface,
  };
  const auto descriptorBytes = std::as_bytes(std::span(&descriptor, 1u));
  const RenderTapeTextureDescriptorV2 parentDescriptor{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(
          RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u,
      .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  auto parentSurface = surface;
  parentSurface.resourceType = 3u;
  std::vector<std::byte> parentDescriptorBytes(sizeof(parentDescriptor) +
                                               sizeof(parentSurface));
  std::memcpy(parentDescriptorBytes.data(), &parentDescriptor,
              sizeof(parentDescriptor));
  std::memcpy(parentDescriptorBytes.data() + sizeof(parentDescriptor),
              &parentSurface, sizeof(parentSurface));
  RenderTapeSurfaceAliasLifetime lifetime;
  lifetime.textureAlias = true;
  check(lifetime.acquire() && lifetime.retainPendingChunk() &&
            !lifetime.releaseWrapper() &&
            lifetime.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::RetainedChunk,
        "pending replacement fixture transfers the old wrapper to its chunk");
  using Status = RenderTapeSurfaceAliasReplacementStatus;
  check(renderTapeSurfaceAliasReplacementStatus(
            oldIdentity, lifetime, descriptorBytes, newIdentity,
            descriptorBytes) == Status::PendingChunkRequiresFlush,
        "the production predicate blocks direct replacement of the old identity");

  RenderTapeCaptureSession session(true);
  check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted,
        "pending replacement fixture starts one frame interval");
  check(session.objectDefine(
            parentIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
            parentDescriptorBytes, 0u, {}, 4u, 1u) ==
            RenderTapeCaptureStatus::Accepted,
        "flush fixture materializes the live alias parent first");
  const std::array<std::byte, 4u> parentSeed{
      std::byte{1u}, std::byte{2u}, std::byte{3u}, std::byte{4u}};
  check(session.resourceMutationBytes(
            parentIdentity, RenderTapeMutationKind::Upload, 0u, 0u,
            parentSeed) == RenderTapeCaptureStatus::Accepted,
        "flush fixture closes the canonical parent texture seed");
  check(session.objectDefine(
            oldIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}) == RenderTapeCaptureStatus::Accepted,
        "flush materializes the old exact generation before its command");
  const auto oldCommand = singleSurfaceColorFillChunk(oldIdentity);
  const auto beforeCommand = session.eventCount();
  check(session.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 1u, .handleCount = 1u},
            oldCommand) == RenderTapeCaptureStatus::Accepted &&
            session.eventCount() == beforeCommand + 1u &&
            session.hasLiveObject(oldIdentity),
        "forced flush records the old command while its exact identity is live");
  check(!lifetime.releasePendingChunk() &&
            lifetime.disposition ==
                RenderTapeSurfaceAliasLifetime::Disposition::RetainedAlias,
        "successful command capture drains pending ownership before replacement");
  const auto beforeDestroy = session.eventCount();
  check(session.objectDestroy(oldIdentity) ==
                RenderTapeCaptureStatus::Accepted &&
            session.eventCount() == beforeDestroy + 1u &&
            !session.hasLiveObject(oldIdentity),
        "replacement journals the old destroy after the old command");
  check(renderTapeSurfaceAliasReplacementStatus(
            oldIdentity, lifetime, descriptorBytes, newIdentity,
            descriptorBytes) == Status::Accepted,
        "drained retained alias is replaceable after ordered destroy");
  const auto beforeNew = session.eventCount();
  check(session.objectDefine(
            newIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            descriptorBytes, 0u, {}) == RenderTapeCaptureStatus::Accepted &&
            session.eventCount() == beforeNew + 1u &&
            session.hasLiveObject(newIdentity),
        "new registration follows old materialize, command, drain, and destroy");
  const D9CWireObjectIdentity outputIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = newIdentity.objectId + 1u,
  };
  const auto outputDescriptor = outputSurfaceDescriptor();
  check(session.objectDefine(
            outputIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {}) ==
            RenderTapeCaptureStatus::Accepted,
        "flush fixture materializes a canonical Present output");
  const auto present = presentChunk();
  check(session.commandChunk(
            CommandChunkEnvelope{.version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                                 .recordCount = 1u, .handleCount = 0u},
            present) == RenderTapeCaptureStatus::Accepted,
        "flush fixture records the required Present event");
  const RenderTapeOracleAttachment output{
      .identity = outputIdentity,
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface)};
  const auto completion = session.completePresent(
      session.eventCount(), 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&output, 1u)));
  check(completion == RenderTapeCaptureStatus::Complete &&
            session.state() == RenderTapeCaptureState::Sealed,
        std::string("pending alias replacement completion status=") +
            std::to_string(static_cast<unsigned>(completion)) + "/" +
            renderTapeValidationStatusName(session.validationStatus()));
  RenderTapeBlobCatalogue catalogue;
  for (const auto& captured : session.publicationBundle().blobs) {
    catalogue.blobs.push_back(RenderTapeBlob{
        .digest = captured.digest,
        .size = captured.bytes.size(),
        .verified = 1u,
    });
  }
  check(validateRenderTape(session.sealedArtifact(), catalogue).valid(),
        "pending alias replacement sequence passes final canonical validation");
}

void testPresentCaptureResultAbiAndOneShotCancellation() {
  D9CRenderTapePresentCaptureResult result{};
  check(result.status == D9C_RENDER_TAPE_PRESENT_CAPTURE_NONE &&
            result.width == 0u && result.height == 0u && result.format == 0u &&
            result.byteCount == 0u,
        "capture-only result is fixed POD with a zero failure baseline");
  check(std::all_of(std::begin(result.sha256), std::end(result.sha256),
                    [](std::uint8_t value) { return value == 0u; }),
        "zeroed capture-only result exposes no partial digest");

  constexpr std::array<std::byte, 3> abc{
      std::byte{0x61u}, std::byte{0x62u}, std::byte{0x63u}};
  constexpr std::array<std::byte, 32> abcSha256{
      std::byte{0xbau}, std::byte{0x78u}, std::byte{0x16u}, std::byte{0xbfu},
      std::byte{0x8fu}, std::byte{0x01u}, std::byte{0xcfu}, std::byte{0xeau},
      std::byte{0x41u}, std::byte{0x41u}, std::byte{0x40u}, std::byte{0xdeu},
      std::byte{0x5du}, std::byte{0xaeu}, std::byte{0x22u}, std::byte{0x23u},
      std::byte{0xb0u}, std::byte{0x03u}, std::byte{0x61u}, std::byte{0xa3u},
      std::byte{0x96u}, std::byte{0x17u}, std::byte{0x7au}, std::byte{0x9cu},
      std::byte{0xb4u}, std::byte{0x10u}, std::byte{0xffu}, std::byte{0x61u},
      std::byte{0xf2u}, std::byte{0x00u}, std::byte{0x15u}, std::byte{0xadu},
  };
  const auto digest = RenderTapeCaptureSession::sha256(abc);
  check(digest == abcSha256,
        "capture-only digest is SHA-256 over tightly packed presentation bytes");
  auto mismatchedBytes = abc;
  mismatchedBytes[2] = std::byte{0x64u};
  check(RenderTapeCaptureSession::sha256(mismatchedBytes) != digest,
        "one changed presentation byte must reject the expected digest");

  dxmt9::PresentMirrorTicket cancelled;
  check(cancelled.cancel() && !cancelled.markEncoded() && !cancelled.encoded(),
        "cancelled mirror ticket cannot be consumed by a later Present");

  dxmt9::PresentMirrorTicket consumed;
  check(consumed.markEncoded() && consumed.encoded() && !consumed.cancel(),
        "encoded mirror ticket is consumed exactly once and is not cancelled");
}

void testProductionIdentityLedgerIsTokenBoundAndNoClobber() {
  RenderTapeProductionIdentityLedger ledger;
  const std::array ranges{
      RenderTapeProductionPassRange{
          .firstRecord = 0u, .recordCount = 2u,
          .dagPassIndex = 0u, .passKind = 1u},
      RenderTapeProductionPassRange{
          .firstRecord = 2u, .recordCount = 3u,
          .dagPassIndex = 1u, .passKind = 4u},
  };
  check(ledger.append(7u, 11u, 21u, 31u, 5u, ranges),
        "identity ledger accepts one exact source coverage");

  D9CRenderTapeIdentityCaptureResult query{};
  check(ledger.copy(7u, query, {}) &&
            query.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE &&
            query.sourceCount == 1u && query.rangeCount == 2u &&
            query.captureToken == 7u &&
            query.byteCount == sizeof(D9CRenderTapeIdentitySourceEntry) +
                                   2u * sizeof(D9CRenderTapeIdentityRangeEntry),
        "zero-capacity query reports the exact immutable table extent");
  std::vector<std::byte> bytes(query.byteCount);
  D9CRenderTapeIdentityCaptureResult copied{};
  check(ledger.copy(7u, copied, bytes) && copied.byteCount == bytes.size(),
        "exact-capacity copy succeeds without consuming the ledger");
  D9CRenderTapeIdentitySourceEntry source{};
  std::array<D9CRenderTapeIdentityRangeEntry, 2> copiedRanges{};
  std::memcpy(&source, bytes.data(), sizeof(source));
  std::memcpy(copiedRanges.data(), bytes.data() + sizeof(source),
              sizeof(copiedRanges));
  check(source.eventOrdinal == 11u && source.sourceOrdinal == 21u &&
            source.seqId == 31u && source.captureToken == 7u &&
            source.recordCount == 5u && source.firstRange == 0u &&
            source.rangeCount == 2u,
        "copied source keeps the production event/source/seq join");
  check(copiedRanges[0].logicalPassId != 0u &&
            copiedRanges[1].logicalPassId != 0u &&
            copiedRanges[0].logicalPassId != copiedRanges[1].logicalPassId &&
            copiedRanges[0].firstRecord == 0u &&
            copiedRanges[1].firstRecord == 2u,
        "ledger assigns non-zero pass identities while preserving DAG ranges");

  std::array<std::byte, 1> tooSmall{};
  D9CRenderTapeIdentityCaptureResult rejected{};
  check(!ledger.copy(7u, rejected, tooSmall) &&
            rejected.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED,
        "non-exact output capacity fails without a partial copy");
  ledger.fail(7u);
  check(!ledger.copy(7u, rejected, {}),
        "a failed join cannot be mistaken for an authoritative sidecar");
  check(ledger.append(8u, 12u, 22u, 32u, 5u, ranges) &&
            ledger.copy(8u, query, {}),
        "a newer capture token resets stale failed state deterministically");
}

void testBlockCompressedLockCaptureLayouts() {
  constexpr std::uint32_t dxt1 = render_tape_d3d_format::DXT1;
  constexpr std::uint32_t dxt3 = render_tape_d3d_format::DXT3;
  constexpr std::uint32_t dxt5 = render_tape_d3d_format::DXT5;
  for (const auto [format, blockBytes] :
       std::array<std::pair<std::uint32_t, std::uint32_t>, 3>{
           {{dxt1, 8u}, {dxt3, 16u}, {dxt5, 16u}}}) {
    D9CSurfaceDesc desc{.format = format, .width = 8u, .height = 8u};
    RenderTapeBlockLockLayout layout{};
    const std::int32_t pitch = static_cast<std::int32_t>(blockBytes * 2u + 8u);
    check(renderTapeBlockLockLayout(desc, pitch, nullptr, layout) ==
              RenderTapeBlockLayoutStatus::Accepted &&
              layout.fullSubresource && layout.blockBytes == blockBytes &&
              layout.fullRowBytes == blockBytes * 2u && layout.fullRows == 2u &&
              layout.rowBytes == blockBytes * 2u && layout.rows == 2u &&
              layout.pitch == static_cast<std::uint32_t>(pitch) &&
              layout.tightBytes == blockBytes * 4u,
          "DXT1/DXT3/DXT5 full-face layout uses block rows and tight bytes");

    std::vector<std::byte> pitched(static_cast<std::size_t>(pitch) * 2u,
                                  std::byte{0xeeu});
    for (std::uint32_t i = 0u; i < layout.rowBytes; ++i) {
      pitched[i] = static_cast<std::byte>(i);
      pitched[static_cast<std::size_t>(pitch) + i] =
          static_cast<std::byte>(0x40u + i);
    }
    std::vector<std::byte> tight;
    check(copyRenderTapeBlockRows(pitched.data(), layout, tight) &&
              tight.size() == layout.tightBytes &&
              tight[layout.rowBytes] == std::byte{0x40u},
          "block capture honors source pitch without persisting padding");
    std::vector<std::byte> content;
    check(applyRenderTapeBlockMutation(layout, tight, content) ==
              RenderTapeBlockMutationStatus::Accepted &&
              content == tight,
          "full block lock establishes a complete immutable seed");
  }
}

void testBlockCompressedSubrectMipAndOddExtentLayouts() {
  constexpr std::uint32_t dxt1 = render_tape_d3d_format::DXT1;
  constexpr std::uint32_t dxt5 = render_tape_d3d_format::DXT5;
  D9CSurfaceDesc desc{.format = dxt5, .width = 16u, .height = 12u};
  RenderTapeBlockLockLayout full{};
  check(renderTapeBlockLockLayout(desc, 80, nullptr, full) ==
            RenderTapeBlockLayoutStatus::Accepted &&
            full.fullRowBytes == 64u && full.fullRows == 3u &&
            full.tightBytes == 192u,
        "full mip layout rounds to complete block rows independently of pitch");
  std::vector<std::byte> content(192u, std::byte{0x11u});
  const auto priorImmutableContent = content;
  const auto priorDigest = RenderTapeCaptureSession::sha256(content);

  const RenderTapeLockRect rect{4, 4, 12, 12};
  RenderTapeBlockLockLayout partial{};
  check(renderTapeBlockLockLayout(desc, 80, &rect, partial) ==
            RenderTapeBlockLayoutStatus::Accepted &&
            !partial.fullSubresource && partial.blockLeft == 1u &&
            partial.blockTop == 1u && partial.rowBytes == 32u &&
            partial.rows == 2u && partial.tightBytes == 64u,
        "aligned DXT subrect records exact block coordinates and mip layout");
  std::vector<std::byte> patchBytes(64u, std::byte{0x7au});
  check(applyRenderTapeBlockMutation(partial, patchBytes, content) ==
            RenderTapeBlockMutationStatus::Accepted &&
            content[0] == std::byte{0x11u} &&
            content[80u] == std::byte{0x7au} &&
            content[144u] == std::byte{0x7au} &&
            priorImmutableContent ==
                std::vector<std::byte>(192u, std::byte{0x11u}) &&
            RenderTapeCaptureSession::sha256(content) != priorDigest,
        "aligned subrect creates the next full immutable mutation in order");

  D9CSurfaceDesc odd{.format = dxt1, .width = 7u, .height = 5u};
  RenderTapeBlockLockLayout oddFull{};
  check(renderTapeBlockLockLayout(odd, 24, nullptr, oddFull) ==
            RenderTapeBlockLayoutStatus::Accepted &&
            oddFull.fullRowBytes == 16u && oddFull.fullRows == 2u &&
            oddFull.tightBytes == 32u,
        "odd DXT dimensions round width and height independently to blocks");
  const RenderTapeLockRect oddEdge{4, 4, 7, 5};
  RenderTapeBlockLockLayout oddPartial{};
  check(renderTapeBlockLockLayout(odd, 24, &oddEdge, oddPartial) ==
            RenderTapeBlockLayoutStatus::Accepted &&
            oddPartial.blockLeft == 1u && oddPartial.blockTop == 1u &&
            oddPartial.rowBytes == 8u && oddPartial.rows == 1u,
        "edge-aligned odd mip subrect admits a non-multiple terminal edge");
}

void testBlockCompressedCaptureRejectsInvalidLayouts() {
  constexpr std::uint32_t dxt1 = render_tape_d3d_format::DXT1;
  constexpr std::uint32_t dxt2 = render_tape_d3d_format::DXT2;
  D9CSurfaceDesc desc{.format = dxt1, .width = 16u, .height = 16u};
  RenderTapeBlockLockLayout layout{};
  check(std::string_view(renderTapeCaptureRejectionReasonName(
            RenderTapeCaptureRejectionReason::IncompleteSubresourceSeed)) ==
            "incomplete_subresource_seed" &&
            std::string_view(renderTapeCaptureRejectionReasonName(
                RenderTapeCaptureRejectionReason::InvalidBlockAlignment)) ==
                "invalid_block_alignment" &&
            std::string_view(renderTapeCaptureRejectionReasonName(
                RenderTapeCaptureRejectionReason::UnmaterializedPreArmObject)) ==
                "unmaterialized_pre_arm_object",
        "typed first-rejection diagnostics have stable observable names");
  D9CSurfaceDesc unsupported{.format = dxt2, .width = 16u, .height = 16u};
  check(renderTapeBlockLockLayout(unsupported, 64, nullptr, layout) ==
            RenderTapeBlockLayoutStatus::UnsupportedFormat,
        "DXT2 remains outside the narrowly admitted capture family");
  const RenderTapeLockRect badLeft{2, 0, 8, 8};
  const RenderTapeLockRect badRight{4, 4, 10, 12};
  check(renderTapeBlockLockLayout(desc, 32, &badLeft, layout) ==
            RenderTapeBlockLayoutStatus::InvalidAlignment &&
            renderTapeBlockLockLayout(desc, 32, &badRight, layout) ==
                RenderTapeBlockLayoutStatus::InvalidAlignment,
        "non-edge DXT lock bounds must be block aligned");
  const RenderTapeLockRect full{0, 0, 16, 16};
  check(renderTapeBlockLockLayout(desc, 31, &full, layout) ==
            RenderTapeBlockLayoutStatus::InvalidPitch,
        "pitch shorter than one block row rejects before copying");
  const RenderTapeLockRect oneBlock{4, 4, 8, 8};
  check(renderTapeBlockLockLayout(desc, 8, &oneBlock, layout) ==
            RenderTapeBlockLayoutStatus::InvalidPitch,
        "subrect pitch must still span the complete compressed mip row");
  D9CSurfaceDesc huge{.format = render_tape_d3d_format::DXT5,
                      .width = 0xffffffffu,
                      .height = 4u};
  check(renderTapeBlockLockLayout(huge, 0x7fffffff, nullptr, layout) ==
            RenderTapeBlockLayoutStatus::Overflow,
        "block-row arithmetic overflow is rejected");

  RenderTapeBlockLockLayout partial{};
  const RenderTapeLockRect aligned{4, 4, 8, 8};
  check(renderTapeBlockLockLayout(desc, 32, &aligned, partial) ==
            RenderTapeBlockLayoutStatus::Accepted,
        "aligned partial fixture is valid");
  std::vector<std::byte> patchBytes(partial.tightBytes, std::byte{0x33u});
  std::vector<std::byte> unavailable;
  check(applyRenderTapeBlockMutation(partial, patchBytes, unavailable) ==
            RenderTapeBlockMutationStatus::IncompleteSeed &&
            unavailable.empty(),
        "partial block write cannot manufacture unavailable initial bytes");
}

void testLinearLockCaptureLayouts() {
  constexpr std::uint32_t argb = render_tape_d3d_format::A8R8G8B8;
  check(render_tape_d3d_format::X4R4G4B4 == 30u,
        "X4R4G4B4 pins the authoritative public D3DFORMAT value");
  check(render_tape_d3d_format::L16 == 81u,
        "L16 pins the authoritative public D3DFORMAT value");
  check(render_tape_d3d_format::R32F == 114u,
        "R32F pins the authoritative public D3DFORMAT value");
  constexpr std::array supportedFormats{
      std::pair{render_tape_d3d_format::R8G8B8, 3u},
      std::pair{render_tape_d3d_format::A8R8G8B8, 4u},
      std::pair{render_tape_d3d_format::X8R8G8B8, 4u},
      std::pair{render_tape_d3d_format::R32F, 4u},
      std::pair{render_tape_d3d_format::R5G6B5, 2u},
      std::pair{render_tape_d3d_format::X1R5G5B5, 2u},
      std::pair{render_tape_d3d_format::A1R5G5B5, 2u},
      std::pair{render_tape_d3d_format::A4R4G4B4, 2u},
      std::pair{render_tape_d3d_format::X4R4G4B4, 2u},
      std::pair{render_tape_d3d_format::A8, 1u},
      std::pair{render_tape_d3d_format::A8B8G8R8, 4u},
      std::pair{render_tape_d3d_format::X8B8G8R8, 4u},
      std::pair{render_tape_d3d_format::A8P8, 2u},
      std::pair{render_tape_d3d_format::P8, 1u},
      std::pair{render_tape_d3d_format::L8, 1u},
      std::pair{render_tape_d3d_format::A8L8, 2u},
      std::pair{render_tape_d3d_format::V8U8, 2u},
      std::pair{render_tape_d3d_format::L16, 2u},
  };
  for (const auto [format, bytesPerPixel] : supportedFormats) {
    const D9CSurfaceDesc formatDesc{
        .format = format, .width = 3u, .height = 2u};
    RenderTapeLinearLockLayout formatLayout{};
    check(renderTapeLinearBytesPerPixel(format) == bytesPerPixel &&
              renderTapeLinearLockLayout(
                  formatDesc,
                  static_cast<std::int32_t>(3u * bytesPerPixel + 4u), nullptr,
                  formatLayout) == RenderTapeLinearLayoutStatus::Accepted &&
              formatLayout.bytesPerPixel == bytesPerPixel &&
              formatLayout.tightBytes == 6u * bytesPerPixel,
          "every admitted linear format uses its authoritative layout");
  }
  check(renderTapeLinearBytesPerPixel(render_tape_d3d_format::DXT1) == 0u,
        "block-compressed formats do not enter the linear layout path");
  const D9CSurfaceDesc l16Desc{
      .format = render_tape_d3d_format::L16,
      .width = 1024u,
      .height = 64u,
  };
  RenderTapeLinearLockLayout l16Layout{};
  check(renderTapeLinearLockLayout(l16Desc, 2048, nullptr, l16Layout) ==
            RenderTapeLinearLayoutStatus::Accepted &&
            l16Layout.bytesPerPixel == 2u &&
            l16Layout.fullRowBytes == 2048u && l16Layout.fullRows == 64u &&
            l16Layout.tightBytes == 131072u &&
            l16Layout.sourceBytes == 131072u && l16Layout.fullSubresource,
        "Firefly Forest L16 full lock closes to exactly 131072 tight bytes");
  const D9CSurfaceDesc desc{.format = argb, .width = 4u, .height = 3u};
  RenderTapeLinearLockLayout full{};
  check(renderTapeLinearLockLayout(desc, 24, nullptr, full) ==
            RenderTapeLinearLayoutStatus::Accepted &&
            full.fullRowBytes == 16u && full.rows == 3u &&
            full.tightBytes == 48u && full.sourceBytes == 64u &&
            full.fullSubresource,
        "linear full LockRect keeps descriptor extent and actual pitch");

  std::vector<std::byte> source(24u * 3u, std::byte{0});
  for (std::uint32_t row = 0u; row < 3u; ++row) {
    for (std::uint32_t byte = 0u; byte < 16u; ++byte) {
      source[row * 24u + byte] =
          static_cast<std::byte>(row * 16u + byte + 1u);
    }
  }
  std::vector<std::byte> copied;
  check(copyRenderTapeLinearRows(source.data(), full, copied) &&
            copied.size() == 48u && copied[23] == std::byte{24u} &&
            copied[24] == std::byte{25u},
        "linear full copy strips only row padding");
  std::vector<std::byte> content;
  check(applyRenderTapeLinearMutation(full, copied, content) ==
            RenderTapeBlockMutationStatus::Accepted &&
            content == copied,
        "linear full mutation establishes the complete seed");

  std::vector<std::byte> providerDestination(24u * 3u, std::byte{0xeeu});
  check(writeRenderTapeLinearRows(copied, providerDestination.data(), full) &&
            providerDestination[15] == copied[15] &&
            providerDestination[16] == std::byte{0xeeu} &&
            providerDestination[24] == copied[16],
        "provider upload restores tight rows without overwriting lock padding");

  const RenderTapeLockRect partialRect{1, 1, 3, 3};
  RenderTapeLinearLockLayout partial{};
  check(renderTapeLinearLockLayout(desc, 24, &partialRect, partial) ==
            RenderTapeLinearLayoutStatus::Accepted &&
            partial.destinationByteOffset == 4u && partial.top == 1u &&
            partial.rowBytes == 8u && partial.rows == 2u &&
            partial.tightBytes == 16u && partial.sourceBytes == 32u &&
            partial.subresourceSourceOffset == 28u &&
            partial.subresourceSourceBytes == 60u &&
            !partial.fullSubresource,
        "linear partial LockRect records exact rectangle extent");
  std::vector<std::byte> partialSource(24u * 2u, std::byte{0});
  for (std::uint32_t row = 0u; row < 2u; ++row) {
    for (std::uint32_t byte = 0u; byte < 8u; ++byte)
      partialSource[row * 24u + byte] = static_cast<std::byte>(
          0xa0u + row * 8u + byte);
  }
  std::vector<std::byte> partialBytes;
  check(copyRenderTapeLinearRows(partialSource.data(), partial, partialBytes) &&
            applyRenderTapeLinearMutation(partial, partialBytes, content) ==
                RenderTapeBlockMutationStatus::Accepted &&
            content[16u + 4u] == std::byte{0xa0u} &&
            content[32u + 4u] == std::byte{0xa8u},
        "linear partial mutation merges rows at descriptor coordinates");

  std::vector<std::byte> fullSubresourceSource(24u * 3u, std::byte{0u});
  std::copy(partialSource.begin(), partialSource.begin() + 8u,
            fullSubresourceSource.begin() + 28u);
  std::copy(partialSource.begin() + 24u, partialSource.begin() + 32u,
            fullSubresourceSource.begin() + 52u);
  std::vector<std::byte> userMemoryBytes;
  check(copyRenderTapeLinearRows(
            fullSubresourceSource.data(), partial, userMemoryBytes,
            RenderTapeLockBitsOrigin::Subresource) &&
            userMemoryBytes == partialBytes,
        "user-memory LockRect applies the checked rectangle offset to base pBits");

  std::vector<std::byte> unavailable;
  check(applyRenderTapeLinearMutation(partial, partialBytes, unavailable) ==
            RenderTapeBlockMutationStatus::IncompleteSeed,
        "linear partial mutation cannot manufacture an unavailable seed");
  check(renderTapeLinearLockLayout(desc, 15, nullptr, full) ==
            RenderTapeLinearLayoutStatus::InvalidPitch,
        "linear pitch shorter than descriptor row rejects fail-closed");
  check(renderTapeUserMemoryFullSeedLayout(
            D9CSurfaceDesc{.format = argb, .width = 128u, .height = 32u},
            128u * 4u, full) == RenderTapeLinearLayoutStatus::Accepted &&
            full.fullSubresource && full.destinationByteOffset == 0u &&
            full.top == 0u && full.rowBytes == 128u * 4u && full.rows == 32u &&
            full.tightBytes == 128u * 32u * 4u,
        "user-memory full seed uses the exact 128x32 subresource extent");
  std::vector<std::byte> userMemorySeed(
      static_cast<std::size_t>(128u * 32u * 4u), std::byte{0x11u});
  const RenderTapeLockRect userMemoryPatch{7, 5, 23, 13};
  RenderTapeLinearLockLayout userMemoryPartial{};
  check(renderTapeLinearLockLayout(
            D9CSurfaceDesc{.format = argb, .width = 128u, .height = 32u},
            128u * 4u, &userMemoryPatch, userMemoryPartial) ==
            RenderTapeLinearLayoutStatus::Accepted &&
            !userMemoryPartial.fullSubresource,
        "user-memory fixture begins with a bounded partial rectangle");
  for (std::uint32_t row = userMemoryPatch.top;
       row < static_cast<std::uint32_t>(userMemoryPatch.bottom); ++row) {
    for (std::uint32_t byte =
             static_cast<std::uint32_t>(userMemoryPatch.left) * 4u;
         byte < static_cast<std::uint32_t>(userMemoryPatch.right) * 4u;
         ++byte)
      userMemorySeed[static_cast<std::size_t>(row) * 128u * 4u + byte] =
          static_cast<std::byte>((row * 17u + byte) & 0xffu);
  }
  std::vector<std::byte> partialUserMemoryBytes;
  std::vector<std::byte> unavailableSeed;
  check(copyRenderTapeLinearRows(
            userMemorySeed.data(), userMemoryPartial, partialUserMemoryBytes,
            RenderTapeLockBitsOrigin::Subresource) &&
            applyRenderTapeLinearMutation(userMemoryPartial,
                                          partialUserMemoryBytes,
                                          unavailableSeed) ==
                RenderTapeBlockMutationStatus::IncompleteSeed,
        "the first user-memory partial write remains incomplete by itself");
  std::vector<std::byte> userMemoryTight;
  std::vector<std::byte> completeSeed;
  check(copyRenderTapeLinearRows(
            userMemorySeed.data(), full, userMemoryTight,
            RenderTapeLockBitsOrigin::Subresource) &&
            userMemoryTight.size() == 128u * 32u * 4u &&
            applyRenderTapeLinearMutation(full, userMemoryTight,
                                          completeSeed) ==
                RenderTapeBlockMutationStatus::Accepted &&
            completeSeed == userMemoryTight &&
            completeSeed[5u * 128u * 4u + 7u * 4u] != std::byte{0x11u} &&
            completeSeed[12u * 128u * 4u + 22u * 4u + 3u] !=
                std::byte{0x11u},
        "a 128x32 user-memory partial-first-write closure publishes a complete seed");
  check(renderTapeUserMemoryFullSeedLayout(
            D9CSurfaceDesc{.format = argb, .width = 128u, .height = 32u},
            128u * 4u - 1u, full) == RenderTapeLinearLayoutStatus::InvalidPitch &&
            renderTapeUserMemoryFullSeedLayout(
                D9CSurfaceDesc{.format = argb, .width = 0u, .height = 32u},
                128u * 4u, full) == RenderTapeLinearLayoutStatus::InvalidExtent,
        "user-memory full seed rejects invalid pitch and descriptor extent");
  D9CSurfaceDesc huge{.format = argb,
                      .width = 0xffffffffu,
                      .height = 2u};
  check(renderTapeLinearLockLayout(huge, 0x7fffffff, nullptr, full) ==
            RenderTapeLinearLayoutStatus::Overflow,
        "linear descriptor conversion and byte ranges reject overflow");
}

void testFullSnapshotClosureTruthTable() {
  constexpr std::uint64_t fullBytes = 128u * 32u * 4u;
  const std::array<std::byte, 8u> bytes{};
  check(renderTapeClassifySnapshot(true, true, true, true, 0u, fullBytes) ==
            RenderTapeFullSnapshotStatus::Required &&
            renderTapeClassifySnapshot(true, true, true, true, fullBytes,
                                       fullBytes) ==
                RenderTapeFullSnapshotStatus::NotRequired,
        "snapshot decision distinguishes unseeded partial and seeded overlay");
  check(renderTapeClassifySnapshot(false, true, true, true, 0u, fullBytes) ==
            RenderTapeFullSnapshotStatus::NotRequired &&
            renderTapeClassifySnapshot(true, false, true, true, 0u,
                                       fullBytes) ==
                RenderTapeFullSnapshotStatus::InvalidIdentity &&
            renderTapeClassifySnapshot(true, true, false, true, 0u,
                                       fullBytes) ==
                RenderTapeFullSnapshotStatus::InvalidExtent &&
            renderTapeClassifySnapshot(true, true, true, true, 7u,
                                       fullBytes) ==
                RenderTapeFullSnapshotStatus::InvalidExtent,
        "capture-off, stale identity, and malformed extent are fail-closed");
  check(renderTapeClassifyUserMemorySeedRoute(
            RenderTapeFullSnapshotStatus::Required) ==
                RenderTapeUserMemorySeedRoute::FullOnly &&
            renderTapeClassifyUserMemorySeedRoute(
                RenderTapeFullSnapshotStatus::NotRequired) ==
                RenderTapeUserMemorySeedRoute::PartialOnly &&
            renderTapeClassifyUserMemorySeedRoute(
                RenderTapeFullSnapshotStatus::InvalidIdentity) ==
                RenderTapeUserMemorySeedRoute::Reject &&
            renderTapeClassifyUserMemorySeedRoute(
                RenderTapeFullSnapshotStatus::InvalidExtent) ==
                RenderTapeUserMemorySeedRoute::Reject,
        "user-memory closure routes full-only, partial-only, or reject");
  check(renderTapeUserMemoryLockRequiresFlush(true) &&
            !renderTapeUserMemoryLockRequiresFlush(false),
        "capture-tracked user-memory locks flush before mutation or CPU reads while capture-off stays fast");
  check(renderTapeClassifySnapshot(true, true, true, false, 0u, fullBytes) ==
            RenderTapeFullSnapshotStatus::NotRequired,
        "full locks never resnapshot");
  constexpr std::uint64_t fullBufferBytes = 4096u;
  std::vector<std::byte> bufferContent(
      static_cast<std::size_t>(fullBufferBytes), std::byte{0x11});
  const std::array<std::byte, 4u> bufferPatch{
      std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};
  check(applyRenderTapeBufferMutation(fullBufferBytes, 0u, bufferPatch,
                                      bufferContent) ==
                RenderTapeBlockMutationStatus::Accepted &&
            bufferContent.size() == fullBufferBytes &&
            bufferContent[0] == std::byte{0x21} &&
            bufferContent[3] == std::byte{0x24} &&
            bufferContent[4] == std::byte{0x11},
        "offset-zero partial buffer writes overlay an existing complete seed");
  std::vector<std::byte> missingBufferSeed;
  check(applyRenderTapeBufferMutation(fullBufferBytes, 0u, bufferPatch,
                                      missingBufferSeed) ==
            RenderTapeBlockMutationStatus::IncompleteSeed,
        "offset-zero partial buffer writes cannot manufacture a seed");
  check(renderTapeClassifyBufferSnapshot(
            true, true, true, true, 0u, fullBufferBytes) ==
            RenderTapeFullSnapshotStatus::Required &&
            renderTapeClassifyBufferSnapshot(
                true, true, true, true, fullBufferBytes, fullBufferBytes) ==
                RenderTapeFullSnapshotStatus::NotRequired &&
            renderTapeClassifyBufferSnapshot(
                false, true, true, true, 0u, fullBufferBytes) ==
                RenderTapeFullSnapshotStatus::NotRequired,
        "buffer snapshot closure only relocks an unseeded partial writable range");
  check(renderTapeClassifyBufferSnapshot(
            true, false, true, true, 0u, fullBufferBytes) ==
            RenderTapeFullSnapshotStatus::InvalidIdentity &&
            renderTapeClassifyBufferSnapshot(
                true, true, false, true, 0u, fullBufferBytes) ==
                RenderTapeFullSnapshotStatus::InvalidExtent &&
            renderTapeClassifyBufferSnapshot(
                true, true, true, false, 0u, fullBufferBytes) ==
                RenderTapeFullSnapshotStatus::NotRequired,
        "buffer identity, extent, and full-lock failures remain typed and fail-closed");
  check(renderTapeValidateFullSnapshot(true, fullBytes,
                                      std::span<const std::byte>(
                                          bytes.data(), bytes.size())) ==
            RenderTapeFullSnapshotStatus::InvalidBytes,
        "a short full snapshot is rejected");
  std::vector<std::byte> exact(static_cast<std::size_t>(fullBytes));
  check(renderTapeValidateFullSnapshot(true, fullBytes, exact) ==
            RenderTapeFullSnapshotStatus::Accepted,
        "an exact full snapshot is accepted");
  exact.push_back(std::byte{0});
  check(renderTapeValidateFullSnapshot(true, fullBytes, exact) ==
            RenderTapeFullSnapshotStatus::InvalidBytes,
        "a long full snapshot is rejected");
  check(renderTapeValidateFullSnapshot(false, fullBytes, exact) ==
            RenderTapeFullSnapshotStatus::InvalidExtent,
        "a malformed partial full-lock proof is rejected");
  check(RenderTapeFullSnapshotStatus::InvalidIdentity !=
            RenderTapeFullSnapshotStatus::Accepted,
        "stale identity remains a typed rejection state");
  check(renderTapeClassifySurfaceSnapshotRoute(
            false, true, true, true, true) ==
            RenderTapeSurfaceSnapshotRoute::NotRequired &&
            renderTapeClassifySurfaceSnapshotRoute(
                true, false, false, true, true) ==
                RenderTapeSurfaceSnapshotRoute::StandaloneSurface &&
            renderTapeClassifySurfaceSnapshotRoute(
                true, true, false, true, true) ==
                RenderTapeSurfaceSnapshotRoute::InvalidIdentity &&
            renderTapeClassifySurfaceSnapshotRoute(
                true, true, true, true, true) ==
                RenderTapeSurfaceSnapshotRoute::TextureDerived,
        "surface snapshot route only admits an exact 2D texture alias");
  check(renderTapeClassifySurfaceSnapshotRoute(
            true, true, true, false, true) ==
            RenderTapeSurfaceSnapshotRoute::NotRequired &&
            renderTapeClassifySurfaceSnapshotRoute(
                true, true, true, true, false) ==
                RenderTapeSurfaceSnapshotRoute::NotRequired,
        "full or byte-less surface mutations do not trigger a snapshot");
}

void testBootstrapClosureTruthTable() {
  check(!renderTapeBootstrapRequiresAllLiveObjects(kRenderTapeProfileFrame) &&
            renderTapeBootstrapRequiresAllLiveObjects(
                kRenderTapeProfileSequence),
        "frame tapes use exact closure while sequence tapes retain all live objects");
  const D9CWireObjectIdentity texture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 1u, .objectId = 41u};
  const D9CWireObjectIdentity staleTexture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 2u, .objectId = 41u};
  const D9CWireObjectIdentity alias{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 1u, .objectId = 42u};
  const D9CWireObjectIdentity output{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 1u, .objectId = 43u};
  const std::array<RenderTapeBootstrapClosureObject, 3u> complete{
      RenderTapeBootstrapClosureObject{.identity = texture, .complete = true},
      RenderTapeBootstrapClosureObject{
          .identity = alias,
          .complete = true,
          .hasDescriptorDependency = true,
          .descriptorDependency = texture},
      RenderTapeBootstrapClosureObject{.identity = output, .complete = true},
  };
  std::vector<D9CWireObjectIdentity> closure;
  check(renderTapeBuildBootstrapClosure(std::span<const D9CWireObjectIdentity>{},
                                        output, complete, closure) ==
            RenderTapeBootstrapClosureStatus::Accepted &&
            closure.size() == 1u && closure[0].objectId == output.objectId,
        "bootstrap closure always includes only the required Present output");
  const std::array<D9CWireObjectIdentity, 1u> roots{alias};
  const std::array<D9CWireObjectIdentity, 1u> textureRoot{texture};
  check(renderTapeBuildBootstrapClosure(roots, output, complete, closure) ==
            RenderTapeBootstrapClosureStatus::Accepted &&
            closure.size() == 3u &&
            renderTapeBootstrapClosureContains(closure, texture),
        "bootstrap closure unions overlay roots, Present output, and alias parent");

  auto incomplete = complete;
  incomplete[0].complete = false;
  const auto incompleteRoot = renderTapeBuildBootstrapClosureAttributed(
      textureRoot, output, incomplete, closure);
  check(incompleteRoot.status ==
            RenderTapeBootstrapClosureStatus::ReferencedObjectIncomplete &&
            incompleteRoot.hasOffendingIdentity &&
            incompleteRoot.offendingIdentity.objectId == texture.objectId &&
            !incompleteRoot.hasDependencyIdentity,
        "referenced incomplete seed names its exact root");
  incomplete[0].producedByCapturedPassCandidate = true;
  check(renderTapeBuildBootstrapClosure(textureRoot, output, incomplete,
                                        closure) ==
            RenderTapeBootstrapClosureStatus::Accepted &&
            renderTapeBootstrapClosureContains(closure, texture),
        "descriptor-qualified incomplete bootstrap root remains a temporal obligation");
  incomplete = complete;
  incomplete[1].complete = false;
  check(renderTapeBuildBootstrapClosure(roots, output, incomplete, closure) ==
            RenderTapeBootstrapClosureStatus::ReferencedObjectIncomplete,
        "referenced incomplete alias is rejected");
  incomplete = complete;
  incomplete[0].complete = false;
  const auto incompleteDependency = renderTapeBuildBootstrapClosureAttributed(
      roots, output, incomplete, closure);
  check(incompleteDependency.status ==
            RenderTapeBootstrapClosureStatus::DescriptorDependencyIncomplete &&
            incompleteDependency.hasOffendingIdentity &&
            incompleteDependency.offendingIdentity.objectId == alias.objectId &&
            incompleteDependency.hasDependencyIdentity &&
            incompleteDependency.dependencyIdentity.objectId == texture.objectId,
        "incomplete texture-derived surface names alias and parent");
  incomplete = complete;
  const std::array<D9CWireObjectIdentity, 1u> staleRoot{staleTexture};
  check(renderTapeBuildBootstrapClosure(staleRoot, output, incomplete, closure) ==
            RenderTapeBootstrapClosureStatus::ReferencedObjectMissing,
        "stale generation is not satisfied by a live prior generation");
  const std::array<RenderTapeBootstrapClosureObject, 4u> withUnreferenced{
      complete[0], complete[1], complete[2],
      RenderTapeBootstrapClosureObject{
          .identity = D9CWireObjectIdentity{.kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
                                             .generation = 1u,
                                             .objectId = 99u},
          .complete = false},
  };
  check(renderTapeBuildBootstrapClosure({}, output, withUnreferenced, closure) ==
            RenderTapeBootstrapClosureStatus::Accepted &&
            !renderTapeBootstrapClosureContains(
                closure, withUnreferenced.back().identity),
        "unreferenced incomplete live objects are pruned");
}

void testCommandAdmissionTruthTable() {
  struct Row {
    RenderTapeCommandAdmissionFacts facts{};
    RenderTapeCommandAdmissionStatus expected =
        RenderTapeCommandAdmissionStatus::OriginRejected;
    bool materializes = false;
    bool produced = false;
  };
  const std::array rows{
      Row{{}, RenderTapeCommandAdmissionStatus::OriginRejected},
      Row{{.originAccepted = true},
          RenderTapeCommandAdmissionStatus::RegistryMissing},
      Row{{.originAccepted = true, .registryPresent = true},
          RenderTapeCommandAdmissionStatus::ObjectMissing},
      Row{{.originAccepted = true, .registryPresent = true,
           .deadObject = true},
          RenderTapeCommandAdmissionStatus::DeadObject},
      Row{{.originAccepted = true, .registryPresent = true,
           .liveObject = true, .admitted = true},
          RenderTapeCommandAdmissionStatus::Accepted},
      Row{{.originAccepted = true, .registryPresent = true,
           .liveObject = true, .aliasDependencyAccepted = false,
           .textureAlias = true},
          RenderTapeCommandAdmissionStatus::AliasDependencyRejected, true},
      Row{{.originAccepted = true, .registryPresent = true,
           .liveObject = true, .contentComplete = true},
          RenderTapeCommandAdmissionStatus::Accepted, true},
      Row{{.originAccepted = true, .registryPresent = true,
           .liveObject = true, .textureAlias = true},
          RenderTapeCommandAdmissionStatus::Accepted, true},
      Row{{.originAccepted = true, .registryPresent = true,
           .liveObject = true},
          RenderTapeCommandAdmissionStatus::UnsupportedProducedDescriptor,
          true},
      Row{{.originAccepted = true, .registryPresent = true,
           .liveObject = true, .producedDescriptorSupported = true},
          RenderTapeCommandAdmissionStatus::ProducedProofRejected, true},
      Row{{.originAccepted = true, .registryPresent = true,
           .liveObject = true, .producedDescriptorSupported = true,
           .producedProofAccepted = true},
          RenderTapeCommandAdmissionStatus::Accepted, true, true},
  };
  for (const auto& row : rows) {
    const auto result = renderTapeClassifyCommandAdmission(row.facts);
    check(result.status == row.expected &&
              result.requiresMaterialization == row.materializes &&
              result.usesProducedProof == row.produced,
          "command admission truth table row");
    check(std::string_view(renderTapeCommandAdmissionStatusName(result.status)) !=
              "unknown",
          "command admission status has stable text");
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--write-production-fixture") {
      testProductionFixtureUsesRecorderAndPublishesBundle(argv[2]);
      return 0;
    }
    check(argc == 1,
          "usage: render_tape_capture_spec [--write-production-fixture dir]");
    testCaptureOffPreservesBytes();
    testDescriptorKindAxisTruthTable();
    testKindZeroIntervalDefineUsesNonZeroDescriptorTag();
    testProductionHookGateTruthTable();
    testProfileSelectionTruthTable();
    testProducedByCapturedPassCaptureEndToEnd();
    testProducedDefinitionTemporalOrderAndAliasJournal();
    testCompletePresentPublishesExactlyOneTape();
    testSequenceCaptureDefersSealUntilSecondPresent();
    testValidationFailurePreservesEventAndChunkLocation();
    testObjectDefineValidationDetailTruthTable();
    testPresentCompleteOracleTargetTruthTable();
    testFailureBeforePublishAndBoundedBackpressure();
    testBoundedBlobBytesAndDeduplication();
    testRenderTapeBlobCapacityResolverTruthTable();
    testProductionBlobDefaultIsCaptureBounded();
    testObjectLifetimeAndTerminalControls();
    testPendingChunkLifetimeTruthTable();
    testGammaRampOrderedControlIsCaptured();
    testPresentOutputRoleOwnershipTruthTable();
    testStandaloneSurfaceIdentityClosureTruthTable();
    testSurfaceAliasGenerationReplacementTransition();
    testPendingAliasFlushBeforeReplacementSequence();
    testPresentCaptureResultAbiAndOneShotCancellation();
    testProductionIdentityLedgerIsTokenBoundAndNoClobber();
    testBlockCompressedLockCaptureLayouts();
    testBlockCompressedSubrectMipAndOddExtentLayouts();
    testBlockCompressedCaptureRejectsInvalidLayouts();
    testLinearLockCaptureLayouts();
    testFullSnapshotClosureTruthTable();
    testBootstrapClosureTruthTable();
    testCommandAdmissionTruthTable();
    testObjectExpectedContentContractTruthTable();
    testMissingSeedDescriptorAndProvenanceTruthTable();
    testArmBoundaryTransitionTruthTable();
    testArmColorSnapshotDescriptorTruthTable();
    testExpectedContentContractDerivation();
    testUpdateTextureClosureTruthTable();
    testExpectedContentValidatorOrdering();
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "render tape capture spec failed: " << failure.what() << '\n';
    return 1;
  }
}
