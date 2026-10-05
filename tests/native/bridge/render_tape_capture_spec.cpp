#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_identity.hpp"
#include "render_tape_identity_v4_fixture.hpp"
#include "d3d9_pe_capture_state.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_render_tape_capture.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_presenter.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace capture_state_allocation_probe {

bool enabled = false;
std::size_t count = 0u;
bool failEnabled = false;
std::size_t failAfter = 0u;
std::exception_ptr injectedException{};
std::size_t watchSize = 0u;
std::size_t watchedAllocations = 0u;
void* watchedPointer = nullptr;
bool watchedFreed = false;
dxmt9::d3d9::RenderTapeCpuShadowContent* freezeDuringAllocation = nullptr;
bool activeFreezeRejected = false;
bool activeFreezeAccepted = false;

} // namespace capture_state_allocation_probe

void* operator new(std::size_t size) {
  if (capture_state_allocation_probe::freezeDuringAllocation) {
    auto* shadow = capture_state_allocation_probe::freezeDuringAllocation;
    capture_state_allocation_probe::freezeDuringAllocation = nullptr;
    try {
      (void)shadow->freeze();
      capture_state_allocation_probe::activeFreezeAccepted = true;
    } catch (const std::logic_error&) {
      capture_state_allocation_probe::activeFreezeRejected = true;
    }
  }
  if (capture_state_allocation_probe::failEnabled) {
    if (capture_state_allocation_probe::failAfter == 0u) {
      capture_state_allocation_probe::failEnabled = false;
      if (capture_state_allocation_probe::injectedException)
        std::rethrow_exception(capture_state_allocation_probe::injectedException);
      throw std::bad_alloc();
    }
    --capture_state_allocation_probe::failAfter;
  }
  if (capture_state_allocation_probe::enabled)
    ++capture_state_allocation_probe::count;
  if (void* value = std::malloc(size)) {
    if (size == capture_state_allocation_probe::watchSize) {
      ++capture_state_allocation_probe::watchedAllocations;
      capture_state_allocation_probe::watchedPointer = value;
      capture_state_allocation_probe::watchedFreed = false;
    }
    return value;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* value) noexcept {
  if (value && value == capture_state_allocation_probe::watchedPointer)
    capture_state_allocation_probe::watchedFreed = true;
  std::free(value);
}

void operator delete[](void* value) noexcept {
  ::operator delete(value);
}

void operator delete(void* value, std::size_t) noexcept {
  ::operator delete(value);
}

void operator delete[](void* value, std::size_t) noexcept {
  ::operator delete[](value);
}

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
static_assert(sizeof(D9CRenderTapeIdentityCaptureResult) == 80u);
static_assert(sizeof(D9CRenderTapeIdentitySourceEntry) == 48u);
static_assert(sizeof(D9CRenderTapeIdentityRangeEntry) == 48u);
static_assert(sizeof(D9CRenderTapeIdentitySettlementEntry) == 48u);
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

void testOptionalPeCaptureStateLifecycle() {
  capture_state_allocation_probe::count = 0u;
  capture_state_allocation_probe::enabled = true;
  auto disabled = makePeCaptureState(
      false, RenderTapeCaptureLimits{}, kRenderTapeProfileFrame, 7u);
  capture_state_allocation_probe::enabled = false;
  std::uint32_t callbackCount = 0u;
  const auto observer = [&](PeCaptureState&) { ++callbackCount; };
  if (disabled)
    observer(*disabled);
  check(!disabled && capture_state_allocation_probe::count == 0u &&
            callbackCount == 0u,
        "capture-off cold owner is null without allocation or callback");

  auto enabled = makePeCaptureState(
      true, RenderTapeCaptureLimits{}, kRenderTapeProfileFrame, 2u);
  check(enabled != nullptr &&
            enabled->renderTapeArmPresentSkipRemaining == 2u &&
            enabled->renderTapeCapture.state() ==
                RenderTapeCaptureState::Disabled,
        "capture-on cold owner constructs the complete idle lifecycle");
  auto& capture = *enabled;
  check(capture.renderTapeCapture.arm(bootstrapChunk()) ==
                RenderTapeCaptureStatus::Accepted &&
            capture.renderTapeCapture.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted,
        "capture-on cold owner arms and enters its first interval");
  capture.renderTapeActiveCaptureToken = 9u;
  capture.renderTapeArmSnapshotOrdinal = 3u;
  capture.renderTapeAbortReason = "test_abort";
  capture.renderTapeCapture.abort();
  check(enabled != nullptr &&
            capture.renderTapeCapture.state() == RenderTapeCaptureState::Aborted &&
            capture.renderTapeActiveCaptureToken == 9u &&
            capture.renderTapeArmSnapshotOrdinal == 3u &&
            std::string_view(capture.renderTapeAbortReason) == "test_abort",
        "capture abort preserves the cold owner and retry ledger");

  check(capture.renderTapeCapture.arm(bootstrapChunk()) ==
                RenderTapeCaptureStatus::Accepted &&
            capture.renderTapeCapture.beginPresentInterval() ==
                RenderTapeCaptureStatus::Accepted,
        "aborted cold owner re-arms for retry");
  const auto descriptor = standaloneSurfaceDescriptor();
  const auto resource = blob();
  const auto mutationDigest = digest();
  auto present = presentChunk();
  check(capture.renderTapeCapture.objectDefine(
            kSurface,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&descriptor, 1u)), 0u, {}, 4u, 1u) ==
                RenderTapeCaptureStatus::Accepted &&
            capture.renderTapeCapture.registerVerifiedBlob(
                std::span<const std::byte, kRenderTapeDigestSize>(
                    resource.digest),
                resource.size) == RenderTapeCaptureStatus::Accepted &&
            capture.renderTapeCapture.resourceMutation(
                kSurface, RenderTapeMutationKind::Upload, 0u, 0u, 4u,
                std::span<const std::byte, kRenderTapeDigestSize>(
                    mutationDigest)) == RenderTapeCaptureStatus::Accepted &&
            capture.renderTapeCapture.commandChunk(
                CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
                present) == RenderTapeCaptureStatus::Accepted,
        "retried cold owner retains the exact completion inputs");
  const auto attachment = oracle();
  check(capture.renderTapeCapture.completePresent(
            capture.renderTapeCapture.eventCount(), 1u,
            RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&attachment, 1u))) ==
                RenderTapeCaptureStatus::Complete &&
            enabled != nullptr &&
            capture.renderTapeCapture.state() == RenderTapeCaptureState::Sealed,
        "retried cold owner survives through completion");
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
  dxmt9::d3d9::pe::SurfaceRef source{};
  source.identity = D9CWireObjectIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 17u,
  };
  source.object = &surface;
  check(dxmt9::d3d9::pe::appendPresent(
            recorder, D9CCommandChunkWirePresent{}, source),
        "PE recorder appends Present");
  const auto sealed = recorder.seal();
  check(sealed.valid(), "PE recorder seals Present");
  return std::vector<std::byte>(sealed.blob.begin(), sealed.blob.end());
}

void writeAtomically(const std::filesystem::path& path,
                     RenderTapeByteView bytes) {
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  check(output.good(), "fixture opens temporary output");
  std::array<std::byte, kRenderTapePayloadWindowBytes> window{};
  for (std::size_t offset = 0u; offset < bytes.size();) {
    const auto count = std::min(window.size(), bytes.size() - offset);
    if (!bytes.readAt(offset, std::span(window).first(count)))
      throw RenderTapePayloadReadError();
    output.write(reinterpret_cast<const char*>(window.data()),
                 static_cast<std::streamsize>(count));
    check(output.good(), "fixture writes complete payload window");
    offset += count;
  }
  check(output.good(), "fixture writes temporary output");
  output.close();
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  check(!error, "fixture publishes output atomically");
}

void writeAtomically(const std::filesystem::path& path,
                     std::span<const std::byte> bytes) {
  writeAtomically(path, RenderTapeByteView(bytes));
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
      std::vector<std::byte>(mutationBytes.begin(), mutationBytes.end())};
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
  const std::array<std::byte, 4u> sourceOracle{
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
  check(session.completePresent(
            5u, 11u, RenderTapeDigestValidity::NotCaptured, {},
            std::as_bytes(std::span(&attachment, 1u)), {}, sourceOracle) ==
            RenderTapeCaptureStatus::Complete,
        "production fixture seals Present transactionally");
  check(session.validationStatus() == RenderTapeValidationStatus::Valid,
        "production fixture validates before publish");

  const auto eventsPath = directory / "events.bin";
  const auto blobPath = directory / "blobs" /
                        "8d70d691c822d55638b6e7fd54cd94170c87d19eb1f628b757506ede5688d297.bin";
  check(session.publicationBundle().events == session.sealedArtifact(),
        "production fixture publishes the session bundle events");
  check(session.publicationBundle().sourceOracle.size() == sourceOracle.size() &&
            session.publicationBundle().sourceOracle ==
                std::vector<std::byte>(sourceOracle.begin(), sourceOracle.end()),
        "production fixture publishes source-before-Present bytes");
  check(session.publicationBundle().blobs.size() == 1u &&
            session.publicationBundle().blobs[0].bytes ==
                std::vector<std::byte>(mutationBytes.begin(), mutationBytes.end()),
        "production fixture publishes verified blob bytes from the bundle");
  writeAtomically(eventsPath, session.publicationBundle().events);
  writeAtomically(blobPath, session.publicationBundle().blobs[0].bytes.readBytes());
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
  const D9CRenderTapeIdentitySettlementEntry settlementEntry{
      .eventOrdinal = 4u,
      .rawOrdinal = 4u,
      .buildGeneration = 1u,
      .firstSourceOrdinal = 1u,
      .tailSeqId = 1u,
      .sourceCount = 1u,
      .reserved0 = 0u,
  };
  check(session.attachCaptureIdentity(
            7u, 4u, std::span(&identitySource, 1u),
            std::span(&identityRange, 1u),
            RenderTapeIdentityEventSettlement{.eventOrdinal = 4u,
                                               .sourceOrdinal = 1u,
                                               .seqId = 1u,
                                               .count = 1u},
            std::span(&settlementEntry, 1u)) ==
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

constexpr RenderTapeIdentityV3CaptureKey kCaptureIdentityV3Key{7u, 8u};
constexpr std::uint64_t kCaptureIdentityV3PresentOrdinal = 6u;

std::vector<std::byte> sealedCaptureIdentityV3Fixture(
    RenderTapeCaptureSession& session, RenderTapeBlobCatalogue& catalogue) {
  const RenderTapeCaptureBlob resource{
      std::vector<std::byte>(64u, std::byte{0x31})};
  const auto resourceDigest = RenderTapeCaptureSession::sha256(resource.bytes);
  catalogue.blobs = {{.digest = resourceDigest,
                      .size = resource.bytes.size(), .verified = 1u}};
  const auto surface = standaloneSurfaceDescriptor();
  constexpr D9CWireObjectIdentity query{D9C_CHUNK_HANDLE_KIND_QUERY, 3u, 88u};
  const RenderTapeQueryDescriptorV2 queryDescriptor{.type = 8u, .dataBytes = 4u};
  check(session.armWithBlobs(bootstrapChunk(), std::span(&resource, 1u)) ==
            RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            session.objectDefine(kSurface,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                std::as_bytes(std::span(&surface, 1u)), 0u, {}, 64u, 1u) ==
                RenderTapeCaptureStatus::Accepted &&
            session.resourceMutation(kSurface, RenderTapeMutationKind::Upload,
                0u, 0u, 64u, resourceDigest) == RenderTapeCaptureStatus::Accepted &&
            session.objectDefine(query,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
                std::as_bytes(std::span(&queryDescriptor, 1u)), 0u, {}) ==
                RenderTapeCaptureStatus::Accepted &&
            session.commandChunk({.recordCount = 1u}, bootstrapChunk()) ==
                RenderTapeCaptureStatus::Accepted,
        "v3 capture fixture seeds verified contents and a source-free state event");
  const auto stateEventOrdinal = std::uint64_t{session.eventCount()};
  const auto presentEventOrdinal = stateEventOrdinal + 1u;
  check(presentEventOrdinal == kCaptureIdentityV3PresentOrdinal,
        "v3 capture fixture records the actual next command's Present ordinal");
  const std::array records{
      ProducedRecord{.type = D9C_COMMAND_RECORD_QUERY_ISSUE,
          .payload = bytesOf(D9CCommandChunkWireQueryIssue{0u, 1u}),
          .handles = {{query.kind, query.generation, query.objectId}}},
      ProducedRecord{.type = D9C_COMMAND_RECORD_PRESENT,
          .payload = bytesOf(D9CCommandChunkWirePresent{})},
  };
  const std::vector<std::byte> output(64u, std::byte{0x4a});
  const std::vector<std::byte> source(64u, std::byte{0x3f});
  const auto attachment = oracle();
  check(session.commandChunk({.recordCount = 2u, .handleCount = 1u},
                              makeProducedChunk(records)) ==
            RenderTapeCaptureStatus::Accepted &&
            session.completePresent(presentEventOrdinal, 11u,
                RenderTapeDigestValidity::Sha256,
                RenderTapeCaptureSession::sha256(output),
                std::as_bytes(std::span(&attachment, 1u)), output, source) ==
                RenderTapeCaptureStatus::Complete,
        "v3 capture fixture seals query END and Present with output/source bytes");

  RenderTapeIdentityV3View identity;
  identity.header.frameId = 9u;
  identity.header.presentOrdinal = presentEventOrdinal;
  identity.header.captureToken = kCaptureIdentityV3Key.token;
  identity.header.captureEpoch = kCaptureIdentityV3Key.epoch;
  identity.header.queue = 1u;
  identity.header.initializerEpoch = 1u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(
      RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  identity.sources = {{.sourceOrdinal = 1u, .seqId = 1u,
      .writerCaptureId = 1u, .writerGeneration = 2u, .storageGeneration = 3u,
      .publicationEdge = 4u, .submissionEdge = 5u, .completionEdge = 6u,
      .completedSequence = 1u, .cutEventOrdinal = presentEventOrdinal, .commandCount = 1u,
      .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)}};
  identity.commands = {{.sourceIndex = 0u,
      .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::Present),
      .membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::NoPassCommand)}};
  identity.dispositions = {
      {.eventOrdinal = stateEventOrdinal, .recordCount = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::StateOnly)},
      {.eventOrdinal = presentEventOrdinal, .recordCount = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::Coordinator),
       .coordinatorIndex = 0u},
      {.eventOrdinal = presentEventOrdinal, .firstRecord = 1u, .recordCount = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
       .commandIndex = 0u},
  };
  identity.coordinators = {{.eventOrdinal = presentEventOrdinal, .rawOrdinal = presentEventOrdinal,
      .buildGeneration = 1u, .dispatchEdge = 3u,
      .releaseOrdinal = 1u, .releaseGeneration = 1u, .queryIdentity = query,
      .issueCycle = 1u, .issueOrdinal = 1u, .deviceEpoch = 2u,
      .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE, .flags = 1u, .queryType = 8u,
      .prefixKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline)}};
  identity.settlements = {{stateEventOrdinal, stateEventOrdinal, 1u, 2u, 1u},
      {presentEventOrdinal, presentEventOrdinal, 1u, 7u, 2u}};
  RenderTapeIdentityV3Status status{};
  auto bytes = buildRenderTapeIdentityV3(
      session.sealedArtifact(), catalogue, identity, &status);
  check(status == RenderTapeIdentityV3Status::Valid && !bytes.empty(),
        "v3 fixture authenticates one actual source and independent replay settlements");
  return bytes;
}

void checkV3AttachmentAborted(const RenderTapeCaptureSession& session) {
  const auto& bundle = session.publicationBundle();
  check(session.state() == RenderTapeCaptureState::Aborted &&
            session.sealedArtifact().empty() && bundle.events.empty() &&
            bundle.blobs.empty() && bundle.identity.empty() &&
            bundle.identitySettlements.empty() && bundle.identitySettlement.count == 0u &&
            bundle.outputOracle.empty() && bundle.sourceOracle.empty() &&
            session.ownedBlobBytes() == 0u && session.ownedBlobEntries() == 0u,
        "v3 attachment rejection releases the entire publication and verified blob owner");
}

void testCompleteCaptureIdentityV3Attachment() {
  RenderTapeCaptureSession session(true);
  RenderTapeBlobCatalogue catalogue;
  auto identity = sealedCaptureIdentityV3Fixture(session, catalogue);
  const auto exactIdentity = identity;
  const auto exactEvents = session.publicationBundle().events;
  const auto exactOutput = session.publicationBundle().outputOracle;
  const auto exactSource = session.publicationBundle().sourceOracle;
  const auto* blobBacking = session.publicationBundle().blobs[0].bytes.backingIdentity();
  check(session.attachCaptureIdentityV3(kCaptureIdentityV3Key,
            kCaptureIdentityV3PresentOrdinal, identity) == RenderTapeCaptureStatus::Complete &&
            session.identityV3ValidationStatus() == RenderTapeIdentityV3Status::Valid,
        "complete capture attaches validated native v3 sidecar bytes");
  identity[0] = std::byte{0xff};
  const auto& bundle = session.publicationBundle();
  RenderTapeIdentityV3View imported;
  check(bundle.identity == exactIdentity && bundle.events == exactEvents &&
            bundle.outputOracle == exactOutput && bundle.sourceOracle == exactSource &&
            bundle.blobs.size() == 1u &&
            blobBacking && bundle.blobs[0].bytes.backingIdentity() == blobBacking &&
            bundle.identitySettlements.empty() && bundle.identitySettlement.count == 0u &&
            validateRenderTapeIdentityV3(bundle.events, catalogue, bundle.identity, &imported) ==
                RenderTapeIdentityV3Status::Valid &&
            imported.sources.size() == 1u && imported.coordinators.size() == 1u &&
            imported.settlements.size() == 2u,
        "v3 attachment owns exact bytes, preserves completed output, and does not coerce v2 rows");
  check(session.attachCaptureIdentityV3(kCaptureIdentityV3Key,
            kCaptureIdentityV3PresentOrdinal, exactIdentity) ==
            RenderTapeCaptureStatus::InvalidState && bundle.identity == exactIdentity &&
            session.identityV3ValidationStatus() == RenderTapeIdentityV3Status::Valid &&
            session.state() == RenderTapeCaptureState::Sealed,
        "a completed identity cannot be clobbered by a repeated v3 attachment");
  RenderTapeCaptureSession capturing(true);
  check(capturing.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            capturing.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            capturing.attachCaptureIdentityV3(kCaptureIdentityV3Key,
                kCaptureIdentityV3PresentOrdinal, exactIdentity) ==
                RenderTapeCaptureStatus::InvalidState &&
            capturing.state() == RenderTapeCaptureState::Capturing,
        "v3 attachment requires the capture's final sealed tape");
}

void testCaptureIdentityV3AttachmentRejections() {
  for (std::uint32_t mutation = 0u; mutation < 15u; ++mutation) {
    RenderTapeCaptureSession session(true);
    RenderTapeBlobCatalogue catalogue;
    auto bytes = sealedCaptureIdentityV3Fixture(session, catalogue);
    RenderTapeIdentityV3Header header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    auto expectedCapture = kCaptureIdentityV3Key;
    auto presentOrdinal = kCaptureIdentityV3PresentOrdinal;
    auto expected = RenderTapeIdentityV3Status::InvalidHeader;
    if (mutation == 0u) header.magic = 0u;
    if (mutation == 1u) header.version = kRenderTapeIdentityVersion;
    if (mutation == 2u) {
      ++header.sources.offset;
      expected = RenderTapeIdentityV3Status::InvalidLayout;
    }
    if (mutation == 3u) {
      header.eventsDigest[0] ^= std::byte{1};
      expected = RenderTapeIdentityV3Status::EventsMismatch;
    }
    if (mutation == 4u) {
      ++header.eventsBytes;
      expected = RenderTapeIdentityV3Status::EventsMismatch;
    }
    if (mutation == 5u) ++expectedCapture.token;
    if (mutation == 6u) ++expectedCapture.epoch;
    if (mutation == 7u) ++presentOrdinal;
    if (mutation == 8u) expectedCapture.token = 0u;
    if (mutation == 9u) expectedCapture.epoch = 0u;
    if (mutation == 10u) presentOrdinal = 0u;
    if (mutation == 11u) {
      RenderTapeIdentityV3Source source{};
      std::memcpy(&source, bytes.data() + header.sources.offset, sizeof(source));
      source.completionEdge = source.completedSequence = 0u;
      std::memcpy(bytes.data() + header.sources.offset, &source, sizeof(source));
      expected = RenderTapeIdentityV3Status::Incomplete;
    }
    if (mutation == 12u) {
      --header.settlements.count;
      bytes.resize(bytes.size() - sizeof(RenderTapeIdentityV3ReplaySettlement));
      expected = RenderTapeIdentityV3Status::Incomplete;
    }
    if (mutation == 13u || mutation == 14u) {
      RenderTapeIdentityV3Coordinator coordinator{};
      std::memcpy(&coordinator, bytes.data() + header.coordinators.offset, sizeof(coordinator));
      if (mutation == 13u) {
        coordinator.prefixKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::Unbound);
        coordinator.writerCaptureId = 1u;
        coordinator.writerGeneration = 2u;
        coordinator.storageGeneration = 3u;
        coordinator.commandEnd = 1u;
        coordinator.observation = static_cast<std::uint32_t>(RenderTapeIdentityV3Observation::Pending);
        expected = RenderTapeIdentityV3Status::Incomplete;
      } else {
        ++coordinator.queryIdentity.generation;
        expected = RenderTapeIdentityV3Status::InvalidCoordinator;
      }
      std::memcpy(bytes.data() + header.coordinators.offset, &coordinator, sizeof(coordinator));
    }
    std::memcpy(bytes.data(), &header, sizeof(header));
    check(session.attachCaptureIdentityV3(expectedCapture, presentOrdinal, bytes) ==
              RenderTapeCaptureStatus::ValidationFailed &&
              session.identityV3ValidationStatus() == expected,
          "corrupt, mismatched, stale, incomplete and unbound v3 attachments fail with precise status");
    checkV3AttachmentAborted(session);
  }
  RenderTapeCaptureSession empty(true);
  RenderTapeBlobCatalogue catalogue;
  (void)sealedCaptureIdentityV3Fixture(empty, catalogue);
  check(empty.attachCaptureIdentityV3(kCaptureIdentityV3Key,
            kCaptureIdentityV3PresentOrdinal, std::span<const std::byte>{}) ==
            RenderTapeCaptureStatus::ValidationFailed &&
            empty.identityV3ValidationStatus() == RenderTapeIdentityV3Status::InvalidHeader,
        "missing v3 bytes cannot produce an identity-qualified capture");
  checkV3AttachmentAborted(empty);
}

void testCaptureIdentityV3AttachmentAllocationFailures() {
  RenderTapeCaptureSession successful(true);
  RenderTapeBlobCatalogue catalogue;
  const auto bytes = sealedCaptureIdentityV3Fixture(successful, catalogue);
  capture_state_allocation_probe::count = 0u;
  capture_state_allocation_probe::enabled = true;
  const auto status = successful.attachCaptureIdentityV3(kCaptureIdentityV3Key,
      kCaptureIdentityV3PresentOrdinal, bytes);
  capture_state_allocation_probe::enabled = false;
  const auto allocations = capture_state_allocation_probe::count;
  check(status == RenderTapeCaptureStatus::Complete && allocations != 0u,
        "v3 attachment allocation probe observes validation and owned-byte admission");
  for (std::size_t allocation = 0u; allocation < allocations; ++allocation) {
    RenderTapeCaptureSession failed(true);
    RenderTapeBlobCatalogue failedCatalogue;
    const auto candidate = sealedCaptureIdentityV3Fixture(failed, failedCatalogue);
    capture_state_allocation_probe::failAfter = allocation;
    capture_state_allocation_probe::failEnabled = true;
    const auto failure = failed.attachCaptureIdentityV3(kCaptureIdentityV3Key,
        kCaptureIdentityV3PresentOrdinal, candidate);
    capture_state_allocation_probe::failEnabled = false;
    const auto detail = failed.identityV3ValidationStatus();
    // The v3 canonical-fact reader currently classifies a tape validator's
    // internal scratch-allocation rejection as InvalidTape.
    check((failure == RenderTapeCaptureStatus::CapacityExceeded &&
               detail == RenderTapeIdentityV3Status::AllocationFailed) ||
              (failure == RenderTapeCaptureStatus::ValidationFailed &&
               detail == RenderTapeIdentityV3Status::InvalidTape),
          "every v3 validator/copy allocation failure preserves its status and rejects publication");
    checkV3AttachmentAborted(failed);
  }
}

void testCaptureIdentityV3SnapshotAttachment() {
  for (std::uint32_t mutation = 0u; mutation < 4u; ++mutation) {
    RenderTapeCaptureSession session(true);
    RenderTapeBlobCatalogue catalogue;
    const auto bytes = sealedCaptureIdentityV3Fixture(session, catalogue);
    RenderTapeIdentityV3View snapshot;
    check(validateRenderTapeIdentityV3(session.sealedArtifact(), catalogue,
              bytes, &snapshot) == RenderTapeIdentityV3Status::Valid,
          "Unix snapshot fixture owns the complete v3 value grammar");
    snapshot.header.eventsBytes = 0u;
    snapshot.header.eventsDigest = {};
    if (mutation == 1u) ++snapshot.header.captureToken;
    if (mutation == 2u) ++snapshot.header.captureEpoch;
    if (mutation == 3u) {
      snapshot.sources[0].completionEdge = 0u;
      snapshot.sources[0].completedSequence = 0u;
    }
    const auto status = session.attachCaptureIdentityV3(kCaptureIdentityV3Key,
        kCaptureIdentityV3PresentOrdinal, snapshot);
    if (mutation == 0u) {
      check(status == RenderTapeCaptureStatus::Complete &&
                session.identityV3ValidationStatus() == RenderTapeIdentityV3Status::Valid &&
                session.publicationBundle().identity == bytes &&
                snapshot.header.eventsBytes == 0u,
            "capture binds an immutable Unix snapshot to its final tape without modifying the caller");
    } else {
      check(status == RenderTapeCaptureStatus::ValidationFailed &&
                session.identityV3ValidationStatus() == (mutation == 3u
                    ? RenderTapeIdentityV3Status::Incomplete
                    : RenderTapeIdentityV3Status::InvalidHeader),
            "snapshot attachment rejects stale trusted-key joins and incomplete publication evidence");
      checkV3AttachmentAborted(session);
    }
  }
}

struct CaptureIdentityV4Pair {
  RenderTapeIdentityV4View identity{};
  RenderTapeExecutionTopologyV1View topology{};
  std::vector<std::byte> identityBytes{};
  std::vector<std::byte> topologyBytes{};
};

RenderTapeActionWitnessV1 captureIdentityV4ActionWitness() {
  return {.evidence = static_cast<std::uint32_t>(RenderTapeActionWitnessEvidenceV1::QueueObserved),
      .constructionProfile = 1u, .armProfile = 1u, .freezeProfile = 1u,
      .captureToken = 7u, .captureEpoch = 8u, .queue = 9u};
}

CaptureIdentityV4Pair sealedCaptureIdentityV4Fixture(
    RenderTapeCaptureSession& session, RenderTapeBlobCatalogue& catalogue) {
  auto fixture = test::makeIdentityV4Fixture();
  const RenderTapeCaptureBlob resource{std::vector<std::byte>(64u, std::byte{0x31})};
  catalogue.blobs = {{.digest = RenderTapeCaptureSession::sha256(resource.bytes),
      .size = resource.bytes.size(), .verified = 1u}};
  constexpr D9CWireObjectIdentity output{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 51u};
  const RenderTapeSurfaceDescriptorV2 descriptor{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u, .width = 4u, .height = 4u, .depth = 1u}};
  const RenderTapeOracleAttachment attachment{.identity = output,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  const std::vector<std::byte> outputBytes(64u, std::byte{0x4a});
  const std::vector<std::byte> sourceBytes(64u, std::byte{0x3f});
  check(session.armWithBlobs(test::fullStateChunk(false), std::span(&resource, 1u)) == RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            session.objectDefine(output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                std::as_bytes(std::span(&descriptor, 1u)), 0u, {}) == RenderTapeCaptureStatus::Accepted &&
            session.commandChunk({.recordCount = 1u}, test::fullStateChunk(true)) == RenderTapeCaptureStatus::Accepted &&
            session.commandChunk({.recordCount = 1u}, test::fullStateChunk(true)) == RenderTapeCaptureStatus::Accepted &&
            session.commandChunk({.recordCount = 1u}, test::presentChunk()) == RenderTapeCaptureStatus::Accepted &&
            session.completePresent(5u, 1u, RenderTapeDigestValidity::Sha256,
                RenderTapeCaptureSession::sha256(outputBytes), std::as_bytes(std::span(&attachment, 1u)),
                outputBytes, sourceBytes) == RenderTapeCaptureStatus::Complete,
        "v4 attachment fixture seals the production capture owner with independent blob/output/source bytes");
  CaptureIdentityV4Pair pair;
  pair.identity = std::move(fixture.identity);
  pair.topology = std::move(fixture.topology);
  RenderTapeIdentityV4Status identityStatus{};
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  pair.identityBytes = buildRenderTapeIdentityV4(session.sealedArtifact(), catalogue, pair.identity, &identityStatus);
  pair.topologyBytes = buildRenderTapeExecutionTopologyV1(session.sealedArtifact(), catalogue,
      pair.identityBytes, pair.topology, &topologyStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid && topologyStatus == RenderTapeExecutionTopologyV1Status::Valid &&
            !pair.identityBytes.empty() && !pair.topologyBytes.empty(),
        "fixture pair authenticates provenance and accepted topology against the exact captured Tape/catalogue");
  return pair;
}

void checkV4AttachmentAborted(const RenderTapeCaptureSession& session) {
  checkV3AttachmentAborted(session);
  check(session.publicationBundle().executionTopology.empty() &&
            session.publicationBundle().actionWitness == RenderTapeActionWitnessV1{},
        "v4 attachment abort cannot retain an orphan topology or action witness");
}

void testCompleteCaptureIdentityV4Attachment() {
  for (std::uint32_t mode = 0u; mode < 4u; ++mode) {
    const bool snapshots = (mode & 1u) != 0u;
    const bool observed = (mode & 2u) != 0u;
    RenderTapeCaptureSession session(true);
    RenderTapeBlobCatalogue catalogue;
    auto pair = sealedCaptureIdentityV4Fixture(session, catalogue);
    const auto identityBytes = pair.identityBytes;
    const auto topologyBytes = pair.topologyBytes;
    const auto events = session.publicationBundle().events;
    const auto output = session.publicationBundle().outputOracle;
    const auto source = session.publicationBundle().sourceOracle;
    const auto* backing = session.publicationBundle().blobs[0].bytes.backingIdentity();
    auto witness = observed ? captureIdentityV4ActionWitness() : RenderTapeActionWitnessV1{};
    const auto expectedWitness = witness;
    const auto attach = [&](const auto& identity, const auto& topology) {
      return observed
          ? session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, identity, topology, witness)
          : session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, identity, topology);
    };
    const auto status = snapshots ? attach(pair.identity, pair.topology)
        : attach(pair.identityBytes, pair.topologyBytes);
    const auto& bundle = session.publicationBundle();
    check(status == RenderTapeCaptureStatus::Complete && session.state() == RenderTapeCaptureState::Sealed &&
              session.identityV4ValidationStatus() == RenderTapeIdentityV4Status::Valid &&
              session.topologyV1ValidationStatus() == RenderTapeExecutionTopologyV1Status::Valid &&
              bundle.identity == identityBytes && bundle.executionTopology == topologyBytes &&
              bundle.actionWitness == expectedWitness &&
              renderTapeActionWitnessV1StableExact(bundle.actionWitness) == observed &&
              bundle.events == events && bundle.outputOracle == output && bundle.sourceOracle == source &&
              bundle.blobs.size() == 1u && bundle.blobs[0].bytes.backingIdentity() == backing &&
              bundle.identitySettlements.empty() && !bundle.identitySettlement.count,
          "sealed pair and queue-observed or legacy-unknown witness attach atomically without changing captured payload owners");
    RenderTapeExecutionTopologyV1View topology;
    check(validateRenderTapeExecutionTopologyV1(bundle.events, catalogue, bundle.identity,
              bundle.executionTopology, &topology) == RenderTapeExecutionTopologyV1Status::Valid &&
              topology.header.identityBytes == bundle.identity.size() &&
              topology.header.identityDigest == RenderTapeCaptureSession::sha256(bundle.identity) &&
              topology.header.eventsDigest == RenderTapeCaptureSession::sha256(bundle.events),
          "one-way topology binding covers full identity bytes, exact Tape, capture key and queue");
    pair.identityBytes[0] ^= std::byte{1};
    pair.topologyBytes[0] ^= std::byte{1};
    pair.identity.sources.clear();
    pair.topology.placements.clear();
    witness = {};
    check(bundle.identity == identityBytes && bundle.executionTopology == topologyBytes &&
              bundle.actionWitness == expectedWitness,
          "attached pair owns bytes independently of both incoming Unix values and borrowed sealed inputs");
    check(session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, identityBytes, topologyBytes) ==
              RenderTapeCaptureStatus::InvalidState &&
              session.attachCaptureIdentityV3({7u, 8u}, 5u, identityBytes) == RenderTapeCaptureStatus::InvalidState &&
              session.attachCaptureIdentity(7u, 5u, {}, {}) == RenderTapeCaptureStatus::InvalidState &&
              bundle.identity == identityBytes && bundle.executionTopology == topologyBytes &&
              bundle.actionWitness == expectedWitness,
          "repeated attachment and older-version entrypoints cannot replace either half of a committed pair");
    session.abort();
    checkV4AttachmentAborted(session);
    check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
              session.publicationBundle().actionWitness == RenderTapeActionWitnessV1{} &&
              session.identityV4ValidationStatus() == RenderTapeIdentityV4Status::InvalidHeader &&
              session.topologyV1ValidationStatus() == RenderTapeExecutionTopologyV1Status::InvalidHeader,
          "rearming clears companion ownership and resets pair admission diagnostics");
  }
  for (std::uint32_t mode = 0u; mode < 4u; ++mode) {
    RenderTapeCaptureSession session(true);
    RenderTapeBlobCatalogue catalogue;
    const auto pair = sealedCaptureIdentityV4Fixture(session, catalogue);
    auto witness = captureIdentityV4ActionWitness();
    if (mode == 0u) witness.constructionProfile = witness.armProfile = witness.freezeProfile = 0u;
    if (mode == 1u) {
      witness.constructionProfile = 0u;
      witness.armRevision = witness.freezeRevision = 1u;
    }
    if (mode == 2u) witness.armRevision = witness.freezeRevision = 2u;
    if (mode == 3u) {
      witness.freezeProfile = 0u;
      witness.freezeRevision = 1u;
    }
    const auto status = (mode & 1u)
        ? session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, pair.identity, pair.topology, witness)
        : session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, pair.identityBytes, pair.topologyBytes, witness);
    check(status == RenderTapeCaptureStatus::Complete &&
              session.publicationBundle().actionWitness == witness &&
              !renderTapeActionWitnessV1StableExact(session.publicationBundle().actionWitness) &&
              session.publicationBundle().identity == pair.identityBytes &&
              session.publicationBundle().executionTopology == pair.topologyBytes,
        "valid Normal, pre-arm change, ABA and interval-change observations survive attachment without stable-exact promotion");
  }
  RenderTapeCaptureSession capturing(true);
  check(capturing.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted &&
            capturing.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            capturing.attachCaptureIdentityV4({7u, 8u}, 9u, 5u,
                std::span<const std::byte>{}, std::span<const std::byte>{}) == RenderTapeCaptureStatus::InvalidState &&
            capturing.state() == RenderTapeCaptureState::Capturing &&
            capturing.publicationBundle().identity.empty() && capturing.publicationBundle().executionTopology.empty(),
        "v4 attachment cannot seal, qualify or abort an interval before its final captured Tape exists");
}

void testCaptureIdentityV4PairRejections() {
  for (std::uint32_t mutation = 0u; mutation < 17u; ++mutation) {
    RenderTapeCaptureSession session(true);
    RenderTapeBlobCatalogue catalogue;
    auto pair = sealedCaptureIdentityV4Fixture(session, catalogue);
    auto capture = RenderTapeIdentityV4CaptureKey{7u, 8u};
    std::uint64_t queue = 9u, present = 5u;
    RenderTapeExecutionTopologyV1Header topology{};
    std::memcpy(&topology, pair.topologyBytes.data(), sizeof(topology));
    if (mutation == 0u) pair.identityBytes[40] ^= std::byte{1};
    if (mutation == 1u) topology.identityDigest[0] ^= std::byte{1};
    if (mutation == 2u) topology.eventsDigest[0] ^= std::byte{1};
    if (mutation == 3u) ++topology.identityBytes;
    if (mutation == 4u) ++topology.eventsBytes;
    if (mutation == 5u) ++topology.captureToken;
    if (mutation == 6u) ++topology.captureEpoch;
    if (mutation == 7u) ++topology.queue;
    if (mutation == 8u) ++capture.token;
    if (mutation == 9u) ++capture.epoch;
    if (mutation == 10u) ++queue;
    if (mutation == 11u) ++present;
    if (mutation == 12u) topology.reserved0 = 1u;
    if (mutation == 13u) topology.placements.offset = topology.plans.offset;
    std::memcpy(pair.topologyBytes.data(), &topology, sizeof(topology));
    if (mutation == 14u) pair.topologyBytes.clear();
    if (mutation == 15u) pair.identityBytes.clear();
    if (mutation == 16u) {
      const auto receiptOffset = pair.identityBytes.size() - sizeof(RenderTapeIdentityV4ReplayAttempt);
      pair.identityBytes[receiptOffset] ^= std::byte{1};
    }
    check(session.attachCaptureIdentityV4(capture, queue, present, pair.identityBytes, pair.topologyBytes) ==
              RenderTapeCaptureStatus::ValidationFailed,
          "tampered identity/topology, mismatched keys/queue/Present, missing companions and malformed receipts reject");
    checkV4AttachmentAborted(session);
  }
  for (std::uint32_t mutation = 0u; mutation < 10u; ++mutation) {
    RenderTapeCaptureSession session(true);
    RenderTapeBlobCatalogue catalogue;
    auto pair = sealedCaptureIdentityV4Fixture(session, catalogue);
    if (mutation == 0u) ++pair.identity.header.captureToken;
    if (mutation == 1u) ++pair.topology.header.captureEpoch;
    if (mutation == 2u) ++pair.identity.header.queue;
    if (mutation == 3u) pair.identity.header.eventsBytes = 1u;
    if (mutation == 4u) pair.identity.header.eventsDigest[0] = std::byte{1};
    if (mutation == 5u) pair.topology.header.identityBytes = 1u;
    if (mutation == 6u) pair.topology.header.identityDigest[0] = std::byte{1};
    if (mutation == 7u) pair.topology.header.eventsBytes = 1u;
    if (mutation == 8u) pair.topology.header.eventsDigest[0] = std::byte{1};
    if (mutation == 9u) pair.topology.placements.clear();
    check(session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, pair.identity, pair.topology) ==
              RenderTapeCaptureStatus::ValidationFailed,
          "neutral snapshot attachment does not overwrite prebound claims or accept an incomplete topology pair");
    checkV4AttachmentAborted(session);
  }
  for (const bool snapshots : {false, true}) {
    for (std::uint32_t mutation = 0u; mutation < 15u; ++mutation) {
      RenderTapeCaptureSession session(true);
      RenderTapeBlobCatalogue catalogue;
      const auto pair = sealedCaptureIdentityV4Fixture(session, catalogue);
      auto witness = captureIdentityV4ActionWitness();
      if (mutation == 0u) ++witness.version;
      if (mutation == 1u) witness.evidence = 2u;
      if (mutation == 2u) witness.constructionProfile = 2u;
      if (mutation == 3u) witness.armProfile = 2u;
      if (mutation == 4u) witness.freezeProfile = 2u;
      if (mutation == 5u) witness.reserved = 1u;
      if (mutation == 6u) ++witness.captureToken;
      if (mutation == 7u) ++witness.captureEpoch;
      if (mutation == 8u) ++witness.queue;
      if (mutation == 9u) witness.armRevision = 1u;
      if (mutation == 10u) witness.freezeProfile = 0u;
      if (mutation == 11u) witness.armRevision = witness.freezeRevision = 1u;
      if (mutation == 12u) witness.freezeRevision = 1u;
      if (mutation == 13u) witness.evidence = 0u;
      if (mutation == 14u) {
        witness = {};
        witness.captureToken = 7u;
      }
      const auto status = snapshots
          ? session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, pair.identity, pair.topology, witness)
          : session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, pair.identityBytes, pair.topologyBytes, witness);
      check(status == RenderTapeCaptureStatus::ValidationFailed,
          "malformed, noncanonical unknown, revision-inconsistent and wrong-key action evidence aborts the complete owner");
      checkV4AttachmentAborted(session);
    }
  }
}

void testCaptureIdentityV4AttachmentAllocationFailures() {
  for (const bool snapshots : {false, true}) {
    RenderTapeCaptureSession successful(true);
    RenderTapeBlobCatalogue catalogue;
    const auto pair = sealedCaptureIdentityV4Fixture(successful, catalogue);
    const auto witness = captureIdentityV4ActionWitness();
    capture_state_allocation_probe::count = 0u;
    capture_state_allocation_probe::enabled = true;
    const auto status = snapshots
        ? successful.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, pair.identity, pair.topology, witness)
        : successful.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, pair.identityBytes, pair.topologyBytes, witness);
    capture_state_allocation_probe::enabled = false;
    const auto allocations = capture_state_allocation_probe::count;
    check(status == RenderTapeCaptureStatus::Complete && allocations > 1u &&
              successful.publicationBundle().actionWitness == witness,
        "pair-and-witness admission allocation probe covers snapshot build, validators and independently owned byte vectors");
    for (std::size_t allocation = 0u; allocation < allocations; ++allocation) {
      RenderTapeCaptureSession session(true);
      RenderTapeBlobCatalogue fixtureCatalogue;
      const auto fixture = sealedCaptureIdentityV4Fixture(session, fixtureCatalogue);
      capture_state_allocation_probe::failAfter = allocation;
      capture_state_allocation_probe::failEnabled = true;
      const auto failure = snapshots
          ? session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, fixture.identity, fixture.topology, witness)
          : session.attachCaptureIdentityV4({7u, 8u}, 9u, 5u, fixture.identityBytes, fixture.topologyBytes, witness);
      capture_state_allocation_probe::failEnabled = false;
      check(failure == RenderTapeCaptureStatus::ValidationFailed || failure == RenderTapeCaptureStatus::CapacityExceeded,
          "every snapshot-build/validation/copy allocation failure fails closed, never retaining a half pair or witness");
      checkV4AttachmentAborted(session);
    }
  }
}

void testSettlementRowsBindExactSourceSubset() {
  const std::array sources{
      RenderTapeIdentitySource{.eventOrdinal = 9u, .sourceOrdinal = 11u,
                               .seqId = 101u},
      RenderTapeIdentitySource{.eventOrdinal = 9u, .sourceOrdinal = 12u,
                               .seqId = 102u},
      RenderTapeIdentitySource{.eventOrdinal = 10u, .sourceOrdinal = 13u,
                               .seqId = 103u},
  };
  const std::array valid{
      RenderTapeIdentitySettlement{.eventOrdinal = 9u,
                                   .rawOrdinal = 9u,
                                   .buildGeneration = 1u,
                                   .firstSourceOrdinal = 11u,
                                   .tailSeqId = 102u,
                                   .sourceCount = 2u},
      RenderTapeIdentitySettlement{.eventOrdinal = 10u,
                                   .rawOrdinal = 10u,
                                   .buildGeneration = 1u,
                                   .firstSourceOrdinal = 13u,
                                   .tailSeqId = 103u,
                                   .sourceCount = 1u},
  };
  check(validateRenderTapeIdentitySettlements(sources, valid),
        "valid settlements bind complete contiguous event source groups");

  auto swapped = valid;
  swapped[0].firstSourceOrdinal = 12u;
  check(!validateRenderTapeIdentitySettlements(sources, swapped),
        "swapped source settlement fails closed");
  auto foreign = valid;
  foreign[1].eventOrdinal = 77u;
  check(!validateRenderTapeIdentitySettlements(sources, foreign),
        "foreign event settlement fails closed");
  auto truncated = valid;
  truncated[0].sourceCount = 1u;
  check(!validateRenderTapeIdentitySettlements(sources, truncated),
        "truncated settlement coverage fails closed");
}

void testSequenceCaptureDefersSealUntilSecondPresent() {
  constexpr std::array<std::byte, 4u> firstBytes{
      std::byte{0x10u}, std::byte{0x11u}, std::byte{0x12u}, std::byte{0x13u}};
  constexpr std::array<std::byte, 4u> secondBytes{
      std::byte{0x20u}, std::byte{0x21u}, std::byte{0x22u}, std::byte{0x23u}};
  const std::array<RenderTapeCaptureBlob, 2u> blobs{
      RenderTapeCaptureBlob{std::vector<std::byte>(
          firstBytes.begin(), firstBytes.end())},
      RenderTapeCaptureBlob{std::vector<std::byte>(
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
            std::equal(selectedOutputSeed.content[0].begin(),
                       selectedOutputSeed.content[0].end(),
                       seededOutputContent[0].begin(),
                       seededOutputContent[0].end()),
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
  auto managedTexture = texture2d;
  RenderTapeTextureDescriptorV2 managedHeader{};
  std::memcpy(&managedHeader, managedTexture.data(), sizeof(managedHeader));
  managedHeader.initialContentDisposition = static_cast<std::uint32_t>(
      RenderTapeInitialContentDisposition::CompleteSeed);
  std::memcpy(managedTexture.data(), &managedHeader, sizeof(managedHeader));
  D9CSurfaceDesc managedSurface{};
  std::memcpy(&managedSurface,
              managedTexture.data() + sizeof(managedHeader),
              sizeof(managedSurface));
  managedSurface.format = 21u;
  managedSurface.pool = 1u;
  managedSurface.usage = 0u;
  std::memcpy(managedTexture.data() + sizeof(managedHeader), &managedSurface,
              sizeof(managedSurface));
  check(renderTapeArmColorSnapshotTextureSupported(managedTexture),
        "arm snapshots admit only the exact MANAGED A8R8G8B8 atlas shape");
  auto managedWrongFormat = managedTexture;
  managedSurface.format = 22u;
  std::memcpy(managedWrongFormat.data() + sizeof(managedHeader),
              &managedSurface, sizeof(managedSurface));
  check(!renderTapeArmColorSnapshotTextureSupported(managedWrongFormat),
        "MANAGED arm snapshots reject the X8R8G8B8 variant");
  auto managedWrongPool = managedTexture;
  managedSurface.format = 21u;
  managedSurface.pool = 0u;
  std::memcpy(managedWrongPool.data() + sizeof(managedHeader),
              &managedSurface, sizeof(managedSurface));
  check(!renderTapeArmColorSnapshotTextureSupported(managedWrongPool),
        "A8R8G8B8 arm snapshots reject a DEFAULT-pool shader texture");
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

void testA8R8G8B8ArmSnapshotContract() {
  const D9CWireObjectIdentity parent{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 1u, .objectId = 4294967306u};
  const D9CSurfaceDesc shape{
      .format = 21u, .resourceType = 3u, .usage = 1u, .pool = 0u,
      .width = 1280u, .height = 720u, .depth = 1u};
  const auto makeDescriptor = [](const D9CSurfaceDesc &surface,
                                 std::uint32_t levels = 1u) {
    const auto dimension = surface.resourceType == 5u
        ? RenderTapeTextureDimension::Cube
        : surface.resourceType == 4u ? RenderTapeTextureDimension::Volume
                                     : RenderTapeTextureDimension::Texture2D;
    auto descriptor = versionedTextureDescriptor(
        dimension, levels, surface.format, surface.width, surface.height);
    RenderTapeTextureDescriptorV2 header{};
    std::memcpy(&header, descriptor.data(), sizeof(header));
    for (std::uint32_t subresource = 0u;
         subresource < header.subresourceCount; ++subresource) {
      auto current = surface;
      const auto mip = subresource % levels;
      // Preserve deliberate invalid base extents in the negative fixture;
      // the production mip helper otherwise normalizes zero to one.
      current.width = surface.width == 0u ? 0u
          : renderTapeTextureMipExtent(surface.width, mip);
      current.height = surface.height == 0u ? 0u
          : renderTapeTextureMipExtent(surface.height, mip);
      std::memcpy(descriptor.data() + sizeof(header) +
                      subresource * sizeof(current),
                  &current, sizeof(current));
    }
    return descriptor;
  };
  const auto descriptor = makeDescriptor(shape);
  RenderTapeColorSnapshotLayout layout{};
  check(renderTapeArmColorSnapshotTextureSupported(descriptor) &&
            renderTapeDeriveColorSnapshotLayout(shape, layout) &&
            layout.encodingVersion == D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1 &&
            layout.pitch == 5120u && layout.byteCount == 3686400u,
        "STALKER DEFAULT A8R8G8B8 single-level RT derives exact alpha-preserving V1 4WH bytes");
  std::array<std::uint64_t, 1u> extents{};
  const auto content = renderTapeDeriveExpectedContentContract(
      parent.kind, descriptor, extents);
  check(content.status == RenderTapeExpectedContentStatus::Accepted &&
            content.count == 1u && content.bytes == layout.byteCount &&
            extents[0] == layout.byteCount,
        "A8R8G8B8 CompleteSeed parent authenticates the same exact snapshot extent");
  auto aliasShape = shape;
  aliasShape.resourceType = 1u;
  const RenderTapeSurfaceDescriptorV2 alias{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .parentTexture = parent, .surface = aliasShape};
  const auto aliasContent = renderTapeDeriveExpectedContentContract(
      D9C_CHUNK_HANDLE_KIND_SURFACE, std::as_bytes(std::span(&alias, 1u)));
  check(renderTapeSurfaceAliasMatchesTextureSubresource(descriptor, parent, alias) &&
            aliasContent.status == RenderTapeExpectedContentStatus::NotRequired &&
            aliasContent.bytes == 0u && aliasContent.count == 0u,
        "A8R8G8B8 exact surface alias inherits its parent seed without independent content");
  const D9CRenderTapeColorSnapshotRequest request{
      .identity = parent, .surface = shape,
      .encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1};
  const D9CRenderTapeColorSnapshotResult result{
      .status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE,
      .encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1,
      .width = shape.width, .height = shape.height, .pitch = layout.pitch,
      .format = shape.format, .byteCount = layout.byteCount};
  check(renderTapeColorSnapshotResultMatches(request, result, layout.byteCount),
        "DEFAULT A8R8G8B8 snapshot authenticates only its exact V1 receipt");
  auto wrongRequest = request;
  wrongRequest.encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_RGBA16FLOAT_LE_V2;
  auto wrongResult = result;
  wrongResult.pitch += 4u;
  check(!renderTapeColorSnapshotResultMatches(wrongRequest, result, layout.byteCount) &&
            !renderTapeColorSnapshotResultMatches(request, wrongResult, layout.byteCount) &&
            !renderTapeColorSnapshotResultMatches(request, result, layout.byteCount - 1u),
        "A8R8G8B8 V2 encoding, padded receipt and partial capacity cannot authorize capture");
  for (const auto field : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u}) {
    auto unsupported = shape;
    switch (field) {
    case 0u: unsupported.resourceType = 1u; break;
    case 1u: unsupported.resourceType = 5u; break;
    case 2u: unsupported.resourceType = 4u; break;
    case 3u: unsupported.pool = 1u; break;
    case 4u: unsupported.usage = 0u; break;
    case 5u: unsupported.usage = 2u; break;
    case 6u: unsupported.depth = 2u; break;
    case 7u: unsupported.multiSampleType = 2u; break;
    case 8u: unsupported.multiSampleQuality = 1u; break;
    case 9u: unsupported.width = 0u; break;
    case 10u: unsupported.height = 0u; break;
    }
    RenderTapeColorSnapshotLayout rejected{};
    check(!renderTapeDeriveColorSnapshotLayout(unsupported, rejected) &&
              rejected.encodingVersion == 0u && rejected.pitch == 0u &&
              rejected.byteCount == 0u &&
              !renderTapeArmColorSnapshotTextureSupported(makeDescriptor(unsupported)),
          "DEFAULT A8R8G8B8 RT snapshot does not widen other storage/pool/usage/sample shapes; field=" +
              std::to_string(field));
  }
  check(!renderTapeArmColorSnapshotTextureSupported(makeDescriptor(shape, 2u)),
        "A8R8G8B8 arm capture remains single-level and single-subresource");
  auto unavailable = descriptor;
  RenderTapeTextureDescriptorV2 unavailableHeader{};
  std::memcpy(&unavailableHeader, unavailable.data(), sizeof(unavailableHeader));
  unavailableHeader.initialContentDisposition = static_cast<std::uint32_t>(
      RenderTapeInitialContentDisposition::Unavailable);
  std::memcpy(unavailable.data(), &unavailableHeader, sizeof(unavailableHeader));
  check(!renderTapeArmColorSnapshotTextureSupported(unavailable),
        "A8R8G8B8 snapshot gate cannot manufacture CompleteSeed from unavailable content");
  const auto noSeed = renderTapeClassifyCommandAdmission({
      .originAccepted = true, .registryPresent = true, .liveObject = true,
      .producedDescriptorSupported = true, .producedProofAccepted = false});
  check(noSeed.status == RenderTapeCommandAdmissionStatus::ProducedProofRejected,
        "adding A8R8G8B8 snapshot bytes does not relax produced-pass proof admission");
  auto small = shape;
  small.width = 2u;
  small.height = 2u;
  const std::array<std::byte, 16u> bgra{
      std::byte{0x12u}, std::byte{0x34u}, std::byte{0x56u}, std::byte{0x00u},
      std::byte{0x78u}, std::byte{0x9au}, std::byte{0xbcu}, std::byte{0x7fu},
      std::byte{0xdeu}, std::byte{0xf0u}, std::byte{0x11u}, std::byte{0x80u},
      std::byte{0x22u}, std::byte{0x33u}, std::byte{0x44u}, std::byte{0xffu}};
  std::array<std::byte, 24u> padded;
  padded.fill(std::byte{0xacu});
  RenderTapeLinearLockLayout linear{};
  check(renderTapeLinearLockLayout(small, 12, nullptr, linear) ==
                RenderTapeLinearLayoutStatus::Accepted &&
            linear.bytesPerPixel == 4u &&
            writeRenderTapeLinearRows(bgra, padded.data(), linear) &&
            std::equal(bgra.begin(), bgra.begin() + 8u, padded.begin()) &&
            std::equal(bgra.begin() + 8u, bgra.end(), padded.begin() + 12u) &&
            padded[8u] == std::byte{0xacu} && padded[20u] == std::byte{0xacu},
        "A8R8G8B8 row upload preserves alpha 0/127/128/255 and destination padding exactly");
}

void testR32Float2DArmSnapshotContract() {
  const D9CWireObjectIdentity parent{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 3u, .objectId = 4294967311u};
  const D9CSurfaceDesc shape{
      .format = render_tape_d3d_format::R32F,
      .resourceType = 3u, .usage = 1u, .pool = 0u,
      .width = 7u, .height = 5u, .depth = 1u};
  const auto makeDescriptor = [](const D9CSurfaceDesc &surface,
                                 std::uint32_t levels = 1u) {
    const auto dimension = surface.resourceType == 5u
        ? RenderTapeTextureDimension::Cube
        : surface.resourceType == 4u ? RenderTapeTextureDimension::Volume
                                     : RenderTapeTextureDimension::Texture2D;
    auto descriptor = versionedTextureDescriptor(
        dimension, levels, surface.format, surface.width, surface.height);
    RenderTapeTextureDescriptorV2 header{};
    std::memcpy(&header, descriptor.data(), sizeof(header));
    for (std::uint32_t subresource = 0u;
         subresource < header.subresourceCount; ++subresource) {
      auto current = surface;
      const auto mip = subresource % levels;
      current.width = surface.width == 0u ? 0u
          : renderTapeTextureMipExtent(surface.width, mip);
      current.height = surface.height == 0u ? 0u
          : renderTapeTextureMipExtent(surface.height, mip);
      std::memcpy(descriptor.data() + sizeof(header) +
                      subresource * sizeof(current),
                  &current, sizeof(current));
    }
    return descriptor;
  };
  for (const auto extent : {std::pair{1u, 1u}, std::pair{7u, 5u}}) {
    auto current = shape;
    current.width = extent.first;
    current.height = extent.second;
    const auto descriptor = makeDescriptor(current);
    RenderTapeColorSnapshotLayout layout{};
    std::array<std::uint64_t, 1u> extents{};
    const auto content = renderTapeDeriveExpectedContentContract(
        parent.kind, descriptor, extents);
    check(renderTapeArmColorSnapshotR32FloatTextureSupported(current) &&
              renderTapeArmColorSnapshotTextureSupported(descriptor) &&
              renderTapeDeriveColorSnapshotLayout(current, layout) &&
              layout.encodingVersion == D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1 &&
              layout.pitch == 4u * current.width &&
              layout.byteCount == 4u * current.width * current.height &&
              content.status == RenderTapeExpectedContentStatus::Accepted &&
              content.count == 1u && content.bytes == layout.byteCount &&
              extents[0] == layout.byteCount,
          "R32F 1x1 and odd 7x5 DEFAULT RT checkpoints derive exact raw V1 4WH content");
    const D9CRenderTapeColorSnapshotRequest request{
        .identity = parent, .surface = current,
        .encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1};
    const D9CRenderTapeColorSnapshotResult result{
        .status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE,
        .encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1,
        .width = current.width, .height = current.height, .pitch = layout.pitch,
        .format = current.format, .byteCount = layout.byteCount};
    auto wrongResult = result;
    wrongResult.pitch += 4u;
    auto wrongRequest = request;
    wrongRequest.encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_RGBA16FLOAT_LE_V2;
    check(renderTapeColorSnapshotResultMatches(request, result, layout.byteCount) &&
              !renderTapeColorSnapshotResultMatches(request, wrongResult, layout.byteCount) &&
              !renderTapeColorSnapshotResultMatches(wrongRequest, result, layout.byteCount) &&
              !renderTapeColorSnapshotResultMatches(request, result, layout.byteCount - 1u),
          "R32F checkpoint authenticates only exact four-byte V1 pitch and capacity");

    constexpr std::array<std::uint32_t, 12u> words{
        0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u,
        0x7fc12345u, 0xffc54321u, 0x7f812345u, 0x00000001u,
        0x80000001u, 0x3f800001u, 0xbf000123u, 0x7f7fffffu};
    std::vector<std::byte> raw(static_cast<std::size_t>(layout.byteCount));
    for (std::size_t texel = 0u; texel < raw.size() / 4u; ++texel) {
      const auto word = words[(texel + current.width + 3u) % words.size()];
      for (std::size_t lane = 0u; lane < 4u; ++lane)
        raw[texel * 4u + lane] = std::byte((word >> (lane * 8u)) & 0xffu);
    }
    const auto paddedPitch = layout.pitch + 12u;
    std::vector<std::byte> padded(paddedPitch * current.height, std::byte{0xcdu});
    check(writeRenderTapeColorSnapshotRows(
              raw, padded.data(), static_cast<std::int32_t>(paddedPitch), current),
          "R32F snapshot row upload accepts exact raw float32 words");
    for (std::uint32_t row = 0u; row < current.height; ++row) {
      check(std::equal(raw.begin() + row * layout.pitch,
                       raw.begin() + (row + 1u) * layout.pitch,
                       padded.begin() + row * paddedPitch) &&
                std::all_of(padded.begin() + row * paddedPitch + layout.pitch,
                            padded.begin() + (row + 1u) * paddedPitch,
                            [](std::byte value) { return value == std::byte{0xcdu}; }),
            "R32F upload preserves signs, infinities, NaN payloads and denormals without padding writes");
    }
    const auto unchanged = padded;
    check(!writeRenderTapeColorSnapshotRows(
              raw, padded.data(), -static_cast<std::int32_t>(paddedPitch), current) &&
              !writeRenderTapeColorSnapshotRows(
                  raw, padded.data(), static_cast<std::int32_t>(layout.pitch - 1u), current) &&
              !writeRenderTapeColorSnapshotRows(
                  std::span(raw).first(raw.size() - 1u), padded.data(),
                  static_cast<std::int32_t>(paddedPitch), current) &&
              padded == unchanged,
          "malformed R32F row extent and pitch reject before any CPU write");
  }
  const auto descriptor = makeDescriptor(shape);
  auto aliasShape = shape;
  aliasShape.resourceType = 1u;
  const RenderTapeSurfaceDescriptorV2 alias{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .parentTexture = parent, .surface = aliasShape};
  const auto aliasContent = renderTapeDeriveExpectedContentContract(
      D9C_CHUNK_HANDLE_KIND_SURFACE, std::as_bytes(std::span(&alias, 1u)));
  check(renderTapeSurfaceAliasMatchesTextureSubresource(descriptor, parent, alias) &&
            aliasContent.status == RenderTapeExpectedContentStatus::NotRequired &&
            aliasContent.bytes == 0u && aliasContent.count == 0u,
        "R32F Surface aliases inherit the exact generation-qualified parent seed");
  for (const auto field : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u}) {
    auto unsupported = shape;
    switch (field) {
    case 0u: unsupported.resourceType = 1u; break;
    case 1u: unsupported.resourceType = 4u; break;
    case 2u: unsupported.pool = 1u; break;
    case 3u: unsupported.usage = 0u; break;
    case 4u: unsupported.usage = 3u; break;
    case 5u: unsupported.depth = 2u; break;
    case 6u: unsupported.multiSampleType = 2u; break;
    case 7u: unsupported.multiSampleQuality = 1u; break;
    case 8u: unsupported.width = 0u; break;
    case 9u: unsupported.height = 0u; break;
    }
    RenderTapeColorSnapshotLayout rejected{};
    check(!renderTapeArmColorSnapshotR32FloatTextureSupported(unsupported) &&
              !renderTapeDeriveColorSnapshotLayout(unsupported, rejected) &&
              rejected.encodingVersion == 0u && rejected.pitch == 0u &&
              rejected.byteCount == 0u &&
              !renderTapeArmColorSnapshotTextureSupported(makeDescriptor(unsupported)),
          "R32F 2D checkpoint rejects other storage/pool/usage/sample shapes; field=" +
              std::to_string(field));
  }
  auto cube = shape;
  cube.resourceType = 5u;
  check(!renderTapeArmColorSnapshotR32FloatTextureSupported(cube) &&
            renderTapeArmColorSnapshotTextureSupported(makeDescriptor(cube)),
        "new R32F 2D predicate is disjoint from the preexisting all-face cube checkpoint");
  check(!renderTapeArmColorSnapshotTextureSupported(makeDescriptor(shape, 2u)),
        "R32F 2D checkpoint remains single-mip and single-subresource");
  auto unavailable = descriptor;
  RenderTapeTextureDescriptorV2 header{};
  std::memcpy(&header, unavailable.data(), sizeof(header));
  header.initialContentDisposition = static_cast<std::uint32_t>(
      RenderTapeInitialContentDisposition::Unavailable);
  std::memcpy(unavailable.data(), &header, sizeof(header));
  check(!renderTapeArmColorSnapshotTextureSupported(unavailable),
        "R32F checkpoint cannot manufacture CompleteSeed from unavailable content");
  auto tooWide = shape;
  tooWide.width = static_cast<std::uint32_t>(
      std::numeric_limits<std::int32_t>::max()) / 4u + 1u;
  RenderTapeColorSnapshotLayout overflow{};
  check(!renderTapeDeriveColorSnapshotLayout(tooWide, overflow) &&
            renderTapeDeriveExpectedSurfaceContent(tooWide).status ==
                RenderTapeExpectedContentStatus::Overflow,
        "R32F tight row must fit the existing signed replay lock pitch");
}

void testRgba16FloatSnapshotContract() {
  static_assert(D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1 == 1u);
  static_assert(D9C_RENDER_TAPE_COLOR_ENCODING_RGBA16FLOAT_LE_V2 == 2u);
  const D9CWireObjectIdentity parent{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 1u, .objectId = 4294967304u};
  const D9CSurfaceDesc shape{
      .format = render_tape_d3d_format::A16B16G16R16F,
      .resourceType = 3u, .usage = 1u, .pool = 0u,
      .width = 1280u, .height = 720u, .depth = 1u};
  const auto makeDescriptor = [](const D9CSurfaceDesc &surface,
                                 std::uint32_t levels = 1u) {
    auto descriptor = versionedTextureDescriptor(
        surface.resourceType == 5u ? RenderTapeTextureDimension::Cube
            : surface.resourceType == 4u ? RenderTapeTextureDimension::Volume
                                        : RenderTapeTextureDimension::Texture2D,
        levels, surface.format, surface.width, surface.height);
    RenderTapeTextureDescriptorV2 header{};
    std::memcpy(&header, descriptor.data(), sizeof(header));
    for (std::uint32_t subresource = 0u;
         subresource < header.subresourceCount; ++subresource) {
      auto current = surface;
      const auto mip = subresource % levels;
      current.width = renderTapeTextureMipExtent(surface.width, mip);
      current.height = renderTapeTextureMipExtent(surface.height, mip);
      current.depth = surface.resourceType == 4u
          ? renderTapeTextureMipExtent(surface.depth, mip) : 1u;
      std::memcpy(descriptor.data() + sizeof(header) +
                      subresource * sizeof(current),
                  &current, sizeof(current));
    }
    return descriptor;
  };
  const auto descriptor = makeDescriptor(shape);
  RenderTapeColorSnapshotLayout layout{};
  check(renderTapeArmColorSnapshotTextureSupported(descriptor) &&
            renderTapeDeriveColorSnapshotLayout(shape, layout) &&
            layout.encodingVersion ==
                D9C_RENDER_TAPE_COLOR_ENCODING_RGBA16FLOAT_LE_V2 &&
            layout.pitch == 10240u && layout.byteCount == 7372800u,
        "STALKER RGBA16F single-level DEFAULT RT derives raw V2 8WH bytes");
  std::array<std::uint64_t, 1u> extents{};
  const auto contract = renderTapeDeriveExpectedContentContract(
      parent.kind, descriptor, extents);
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.count == 1u && contract.bytes == layout.byteCount &&
            extents[0] == layout.byteCount,
        "RGBA16F parent CompleteSeed uses the same exact snapshot extent");
  const auto missing = renderTapeDescribeMissingSeed(parent, descriptor, 0u, {});
  check(missing.expectedContentStatus == RenderTapeExpectedContentStatus::Accepted &&
            missing.expectedTightBytesValid &&
            missing.expectedTightBytes == layout.byteCount,
        "RGBA16F missing-seed diagnostic resolves the parent raw byte extent");

  const D9CRenderTapeColorSnapshotRequest request{
      .identity = parent, .surface = shape, .subresource = 0u,
      .encodingVersion = layout.encodingVersion};
  const D9CRenderTapeColorSnapshotResult result{
      .status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE,
      .encodingVersion = layout.encodingVersion, .subresource = 0u,
      .width = shape.width, .height = shape.height, .pitch = layout.pitch,
      .format = shape.format, .reserved0 = 0u, .byteCount = layout.byteCount};
  check(renderTapeColorSnapshotResultMatches(request, result, layout.byteCount),
        "exact eight-byte RGBA16F result is authenticated as V2");
  auto v1Request = request;
  v1Request.encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1;
  check(!renderTapeColorSnapshotResultMatches(v1Request, result, layout.byteCount),
        "V1 four-byte request cannot authenticate RGBA16F V2 pixels");
  auto wrongResult = result;
  wrongResult.encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1;
  check(!renderTapeColorSnapshotResultMatches(request, wrongResult, layout.byteCount),
        "V1 result cannot be substituted for RGBA16F V2");
  wrongResult = result;
  wrongResult.pitch = shape.width * 4u;
  check(!renderTapeColorSnapshotResultMatches(request, wrongResult, layout.byteCount),
        "four-byte row pitch cannot truncate RGBA16F");
  wrongResult = result;
  --wrongResult.byteCount;
  check(!renderTapeColorSnapshotResultMatches(request, wrongResult, layout.byteCount) &&
            !renderTapeColorSnapshotResultMatches(request, result, layout.byteCount - 1u),
        "partial RGBA16F extent or destination capacity remains fail-closed");
  wrongResult = result;
  wrongResult.reserved0 = 1u;
  check(!renderTapeColorSnapshotResultMatches(request, wrongResult, layout.byteCount),
        "RGBA16F result rejects unknown reserved provenance");
  auto wrongRequest = request;
  wrongRequest.identity.kind = D9C_CHUNK_HANDLE_KIND_SURFACE;
  check(!renderTapeColorSnapshotResultMatches(wrongRequest, result, layout.byteCount),
        "RGBA16F texture snapshot cannot authenticate an independent surface identity");
  wrongRequest = request;
  wrongRequest.subresource = 1u;
  check(!renderTapeColorSnapshotResultMatches(wrongRequest, result, layout.byteCount),
        "RGBA16F snapshot cannot name an extra subresource");

  auto aliasSurface = shape;
  aliasSurface.resourceType = 1u;
  const RenderTapeSurfaceDescriptorV2 alias{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u, .parentTexture = parent, .surface = aliasSurface};
  const auto aliasContract = renderTapeDeriveExpectedContentContract(
      D9C_CHUNK_HANDLE_KIND_SURFACE, std::as_bytes(std::span(&alias, 1u)));
  check(renderTapeSurfaceAliasMatchesTextureSubresource(descriptor, parent, alias) &&
            aliasContract.status == RenderTapeExpectedContentStatus::NotRequired &&
            aliasContract.bytes == 0u && aliasContract.count == 0u,
        "RGBA16F surface alias resolves its complete parent, not independent content");
  const auto noSeed = renderTapeClassifyCommandAdmission({
      .originAccepted = true, .registryPresent = true, .liveObject = true,
      .producedDescriptorSupported = true, .producedProofAccepted = false});
  const auto completeSeed = renderTapeClassifyCommandAdmission({
      .originAccepted = true, .registryPresent = true, .liveObject = true,
      .contentComplete = true,
      .producedDescriptorSupported = true, .producedProofAccepted = false});
  check(noSeed.status == RenderTapeCommandAdmissionStatus::ProducedProofRejected &&
            completeSeed.accepted() && !completeSeed.usesProducedProof,
        "binding-only still rejects Produced proof; complete arm bytes independently admit");

  auto standalone = shape;
  standalone.resourceType = 1u;
  auto cube = shape;
  cube.resourceType = 5u;
  auto managed = shape;
  managed.pool = 1u;
  auto shaderTexture = shape;
  shaderTexture.usage = 0u;
  auto volume = shape;
  volume.resourceType = 4u;
  volume.depth = 2u;
  auto msaa = shape;
  msaa.multiSampleType = 2u;
  auto msaaQuality = shape;
  msaaQuality.multiSampleQuality = 1u;
  for (const auto &unsupported :
       {standalone, cube, managed, shaderTexture, volume, msaa, msaaQuality}) {
    RenderTapeColorSnapshotLayout rejected{};
    check(!renderTapeDeriveColorSnapshotLayout(unsupported, rejected) &&
              rejected.encodingVersion == 0u && rejected.pitch == 0u &&
              rejected.byteCount == 0u &&
              !renderTapeArmColorSnapshotTextureSupported(makeDescriptor(unsupported)) &&
              renderTapeDeriveExpectedSurfaceContent(unsupported).status !=
                  RenderTapeExpectedContentStatus::Accepted,
          "RGBA16F unsupported storage/pool/usage/sample routes remain fail-closed");
  }
  const auto multiMip = makeDescriptor(shape, 2u);
  check(!renderTapeArmColorSnapshotTextureSupported(multiMip) &&
            renderTapeDeriveExpectedContentContract(parent.kind, multiMip).status ==
                RenderTapeExpectedContentStatus::UnsupportedFormat,
        "valid multi-mip RGBA16F descriptor is outside the snapshot/CompleteSeed shape");
  auto tooWide = shape;
  tooWide.width = static_cast<std::uint32_t>(
      std::numeric_limits<std::int32_t>::max()) / 8u + 1u;
  check(!renderTapeDeriveColorSnapshotLayout(tooWide, layout) &&
            renderTapeDeriveExpectedSurfaceContent(tooWide).status ==
                RenderTapeExpectedContentStatus::Overflow,
        "RGBA16F snapshot row cannot exceed downstream signed D3D9 lock pitch");
  auto v1Shape = shape;
  v1Shape.format = 22u;
  check(renderTapeDeriveColorSnapshotLayout(v1Shape, layout) &&
            layout.encodingVersion == D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1 &&
            layout.pitch == 5120u && layout.byteCount == 3686400u,
        "existing X8 snapshot retains exactly the four-byte V1 layout");
  v1Shape.width = static_cast<std::uint32_t>(
      std::numeric_limits<std::int32_t>::max()) / 4u + 1u;
  check(!renderTapeDeriveColorSnapshotLayout(v1Shape, layout),
        "V1 snapshot also rejects a tight row unrepresentable by signed replay lock pitch");
  RenderTapeLinearLockLayout linear{};
  RenderTapeVolumeLockLayout volumeLayout{};
  check(renderTapeLinearBytesPerPixel(shape.format) == 0u &&
            renderTapeLinearLockLayout(shape, 10240, nullptr, linear) ==
                RenderTapeLinearLayoutStatus::UnsupportedFormat &&
            renderTapeVolumeContentLayout(volume, volumeLayout) ==
                RenderTapeVolumeLayoutStatus::UnsupportedFormat,
        "RGBA16F snapshot support does not widen general PE LockRect/BPP/LockBox gates");

  auto smallShape = shape;
  smallShape.width = 2u;
  smallShape.height = 2u;
  constexpr std::array<std::byte, 32u> rawHalves{
      std::byte{0x00u}, std::byte{0x80u}, std::byte{0x00u}, std::byte{0x3cu},
      std::byte{0x00u}, std::byte{0x7cu}, std::byte{0x01u}, std::byte{0x7eu},
      std::byte{0x00u}, std::byte{0xbcu}, std::byte{0x01u}, std::byte{0x00u},
      std::byte{0x00u}, std::byte{0xfcu}, std::byte{0x23u}, std::byte{0xfeu},
      std::byte{0x55u}, std::byte{0x35u}, std::byte{0xaau}, std::byte{0xbau},
      std::byte{0xffu}, std::byte{0x7bu}, std::byte{0xffu}, std::byte{0xfbu},
      std::byte{0x00u}, std::byte{0x00u}, std::byte{0x00u}, std::byte{0x40u},
      std::byte{0x00u}, std::byte{0xc0u}, std::byte{0x00u}, std::byte{0x38u}};
  std::array<std::byte, 48u> padded{};
  padded.fill(std::byte{0xccu});
  check(writeRenderTapeColorSnapshotRows(rawHalves, padded.data(), 24, smallShape) &&
            std::equal(rawHalves.begin(), rawHalves.begin() + 16u, padded.begin()) &&
            std::equal(rawHalves.begin() + 16u, rawHalves.end(), padded.begin() + 24u) &&
            std::all_of(padded.begin() + 16u, padded.begin() + 24u,
                        [](std::byte value) { return value == std::byte{0xccu}; }) &&
            std::all_of(padded.begin() + 40u, padded.end(),
                        [](std::byte value) { return value == std::byte{0xccu}; }),
        "snapshot upload preserves raw half NaNs/signs/lanes and leaves row padding untouched");
  const auto unchanged = padded;
  check(!writeRenderTapeColorSnapshotRows(rawHalves, padded.data(), -24, smallShape) &&
            !writeRenderTapeColorSnapshotRows(rawHalves, padded.data(), 15, smallShape) &&
            !writeRenderTapeColorSnapshotRows(
                std::span(rawHalves).first(31u), padded.data(), 24, smallShape) &&
            !writeRenderTapeColorSnapshotRows(rawHalves, nullptr, 24, smallShape) &&
            !writeRenderTapeColorSnapshotRows(rawHalves, padded.data(), 24, standalone) &&
            padded == unchanged,
        "snapshot-only upload rejects bad rows/bytes/storage before any write");
}

void testCompletedEventQueryArmOverlay() {
  static_assert(std::is_standard_layout_v<D9CRenderTapeEventQueryCheckpointRequest>);
  static_assert(std::is_trivially_copyable_v<D9CRenderTapeEventQueryCheckpointRequest>);
  static_assert(sizeof(D9CRenderTapeEventQueryCheckpointRequest) == 40u);
  static_assert(sizeof(D9CRenderTapeEventQueryCheckpointResult) == 120u);
  static_assert(sizeof(RenderTapeQueryDescriptorV2) == 24u);
  static_assert(sizeof(RenderTapeQueryDescriptorV3) == 144u);
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_QUERY, .generation = 3u, .objectId = 17u};
  const RenderTapeQueryDescriptorV2 durable{
      .type = 8u, .dataBytes = 4u,
      .initialState = static_cast<std::uint32_t>(
          RenderTapeQueryInitialState::ClosedWithoutReplayResult),
      .initialIssueCycle = 2u,
  };
  const RenderTapeQueryCycleState durableCycle{
      .type = 8u, .issueCycle = 2u, .issueOrdinal = 4u};
  const D9CRenderTapeEventQueryCheckpointRequest request{
      .identity = identity, .issueCycle = 2u, .issueOrdinal = 4u,
      .checkpointVersion = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_VERSION_1,
      .queryType = 8u,
  };
  const D9CRenderTapeEventQueryCheckpointResult result{
      .checkpointVersion = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_VERSION_1,
      .status = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_COMPLETE,
      .queryType = 8u, .identity = identity, .issueCycle = 2u,
      .coreQueryIdentity = 88u, .deviceEpoch = 1u, .issueOrdinal = 4u,
      .rawOrdinal = 55u, .queueIdentity = 22u,
  };
  const RenderTapeQueryDescriptorV3 arm{
      .initialIssueCycle = 2u, .checkpoint = result};
  const auto durableBytes = std::as_bytes(std::span(&durable, 1u));
  const std::vector<std::byte> durableBefore(durableBytes.begin(), durableBytes.end());
  const std::vector<std::vector<std::byte>> noContent;
  const auto selected = renderTapeSelectArmObjectSnapshotOverlay(
      durableBytes, noContent, std::as_bytes(std::span(&arm, 1u)), noContent,
      4u, 4u);
  RenderTapeQueryDescriptorDecoded descriptor{};
  check(renderTapeEventQueryCheckpointResultMatches(request, result) &&
            selected.source == RenderTapeArmSnapshotOverlaySource::CurrentArm &&
            renderTapeLoadQueryDescriptor(selected.descriptor, descriptor) &&
            renderTapeEventQueryCheckpointMatchesCycle(descriptor, identity, durableCycle) &&
            renderTapeArmObjectSnapshotContentComplete(
                0u, false, RenderTapeArmObjectSnapshotOverlayPolicy::Ordinary,
                selected.source, selected.content) &&
            std::equal(durableBytes.begin(), durableBytes.end(), durableBefore.begin()) &&
            !durableCycle.capturedCycle && !durableCycle.completedEventCheckpoint,
        "completed EVENT arm overlay supplies typed authority without durable mutation or blob");
  PeCaptureState transferOwner({}, kRenderTapeProfileFrame, 0u);
  transferOwner.renderTapeArmSnapshotOrdinal = 4u;
  auto bufferReservation = transferOwner.renderTapeSnapshotBudget.reserveRetained(4u);
  check(bufferReservation.status == RenderTapeSnapshotBudgetStatus::Complete,
        "EVENT transfer fixture reserves buffer snapshot before allocation");
  RenderTapeArmObjectSnapshot bufferArmSnapshot;
  bufferArmSnapshot.armOrdinal = 4u;
  bufferArmSnapshot.identity = {
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 1u, .objectId = 901u};
  bufferArmSnapshot.descriptor = {std::byte{1u}};
  bufferArmSnapshot.retainedLeases.push_back(std::move(bufferReservation.lease));
  bufferArmSnapshot.content.emplace_back();
  bufferArmSnapshot.content[0] = {std::byte{1u}, std::byte{2u}, std::byte{3u}, std::byte{4u}};
  check(bufferArmSnapshot.retainedLeases[0].bindRetainedPayload(
            bufferArmSnapshot.content[0].data(), bufferArmSnapshot.content[0].size()),
        "EVENT transfer fixture binds buffer receipt to original vector");
  transferOwner.renderTapeArmSnapshots.push_back(std::move(bufferArmSnapshot));
  RenderTapeArmObjectSnapshot queryArmSnapshot;
  queryArmSnapshot.armOrdinal = 4u;
  queryArmSnapshot.identity = identity;
  queryArmSnapshot.descriptor = bytesOf(arm);
  transferOwner.renderTapeArmSnapshots.push_back(std::move(queryArmSnapshot));
  auto& bufferSnapshot = transferOwner.renderTapeArmSnapshots[0];
  auto& querySnapshot = transferOwner.renderTapeArmSnapshots[1];
  const auto* queryDescriptorPointer = querySnapshot.descriptor.data();
  const auto bufferOverlay = renderTapeSelectArmObjectSnapshotOverlay(
      {}, {}, bufferSnapshot.descriptor, bufferSnapshot.content, 4u, 4u);
  const auto bufferTransfer = selectRenderTapeArmContentTransfer(
      &bufferSnapshot, bufferSnapshot.identity, 4u, bufferOverlay, false);
  std::vector<RenderTapeCaptureBlob> transferredBlobs(1u);
  transferredBlobs[0].retainedLease = std::move(*bufferTransfer.retainedLease(0u));
  transferredBlobs[0].bytes.swap(*bufferTransfer.payload(0u));
  RenderTapeQueryDescriptorDecoded afterTransfer{};
  check(transferOwner.renderTapeCapture.armWithOwnedBlobs(
            bootstrapChunk(), std::move(transferredBlobs)) == RenderTapeCaptureStatus::Accepted &&
            transferOwner.renderTapeCapture.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            transferOwner.renderTapeCapture.objectDefine(identity,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
                querySnapshot.descriptor, 0u, {}) == RenderTapeCaptureStatus::Accepted &&
            bufferSnapshot.content[0].empty() && querySnapshot.content.empty() &&
            querySnapshot.descriptor.data() == queryDescriptorPointer && querySnapshot.armOrdinal == 4u &&
            renderTapeArmSnapshotEpochMatches(querySnapshot.armOrdinal, 4u) &&
            renderTapeLoadQueryDescriptor(querySnapshot.descriptor, afterTransfer) &&
            renderTapeEventQueryCheckpointMatchesCycle(afterTransfer, identity, durableCycle),
        "other payload transfer and query materialization preserve exact EVENT descriptor/ordinal authority");
  const auto stale = renderTapeSelectArmObjectSnapshotOverlay(
      durableBytes, noContent, std::as_bytes(std::span(&arm, 1u)), noContent,
      3u, 4u);
  check(stale.source == RenderTapeArmSnapshotOverlaySource::StaleArm &&
            stale.descriptor.empty(),
        "EVENT checkpoint from an earlier arm cannot be selected");
  auto reissued = durableCycle;
  check(renderTapeAdvanceQueryCycle(reissued, 2u, true) &&
            !renderTapeEventQueryCheckpointMatchesCycle(descriptor, identity, reissued),
        "reissue/open BEGIN invalidates old arm EVENT authority");
  auto staleIdentity = identity;
  ++staleIdentity.generation;
  check(!renderTapeEventQueryCheckpointMatchesCycle(
            descriptor, staleIdentity, durableCycle),
        "arm EVENT proof binds the exact live wire generation");
  const auto unavailable = renderTapeSelectArmObjectSnapshotOverlay(
      durableBytes, noContent, {}, noContent, 0u, 4u);
  check(unavailable.source == RenderTapeArmSnapshotOverlaySource::DurableBase &&
            renderTapeLoadQueryDescriptor(unavailable.descriptor, descriptor) &&
            !renderTapeEventQueryCheckpointMatchesCycle(descriptor, identity, durableCycle),
        "dormant unavailable EVENT stays a legacy uncheckpointed query until referenced");
}

void testStandaloneD24S8SnapshotContract() {
  static_assert(std::is_standard_layout_v<D9CRenderTapeD24S8SnapshotRequest>);
  static_assert(std::is_trivially_copyable_v<D9CRenderTapeD24S8SnapshotRequest>);
  static_assert(std::is_standard_layout_v<D9CRenderTapeD24S8SnapshotResult>);
  static_assert(std::is_trivially_copyable_v<D9CRenderTapeD24S8SnapshotResult>);
  static_assert(sizeof(D9CRenderTapeD24S8SnapshotResult) == 48u);
  static_assert(offsetof(D9CRenderTapeD24S8SnapshotResult, stencilOffset) == 32u);
  static_assert(offsetof(D9CRenderTapeD24S8SnapshotResult, byteCount) == 40u);
  const D9CWireObjectIdentity depth{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 7u, .objectId = 81u};
  const D9CWireObjectIdentity output{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 1u, .objectId = 82u};
  const D9CSurfaceDesc surface{
      .format = render_tape_d3d_format::D24S8, .resourceType = 1u,
      .usage = 2u, .pool = 0u, .width = 17u, .height = 19u, .depth = 1u,
  };
  const RenderTapeSurfaceDescriptorV2 descriptor{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::Standalone),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteDepthStencilFloat32UInt8V1),
      .surface = surface,
  };
  const auto descriptorBytes = std::as_bytes(std::span(&descriptor, 1u));
  RenderTapeSurfaceDescriptorV2 loaded{};
  RenderTapeD24S8SnapshotLayout layout{};
  check(renderTapeLoadSurfaceDescriptorV2(descriptorBytes, loaded) &&
            renderTapeDeriveD24S8SnapshotLayout(surface, layout) &&
            layout.depthPitch == 68u && layout.stencilPitch == 17u &&
            layout.stencilOffset == 1292u && layout.byteCount == 1615u,
        "D24S8 disposition 6 pins odd-extent tight depth/stencil planes");
  std::array<std::uint64_t, 1u> expected{};
  const auto contract = renderTapeDeriveExpectedContentContract(
      depth.kind, descriptorBytes, expected);
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.bytes == 1615u && contract.count == 1u &&
            expected[0] == 1615u &&
            renderTapeValidateExpectedContentExtents(
                depth.kind, descriptorBytes, expected) ==
                RenderTapeExpectedContentStatus::Accepted,
        "D24S8 canonical seed is one exact checked 5N subresource");
  for (const auto wrongBytes : {1292u, 1614u, 1616u}) {
    expected[0] = wrongBytes;
    check(renderTapeValidateExpectedContentExtents(
              depth.kind, descriptorBytes, expected) !=
              RenderTapeExpectedContentStatus::Accepted,
          "depth-only, truncated, and padded D24S8 seeds are rejected");
  }
  auto raw = descriptor;
  raw.initialContentDisposition = static_cast<std::uint32_t>(
      RenderTapeInitialContentDisposition::CompleteSeed);
  check(renderTapeDeriveExpectedContentContract(
            depth.kind, std::as_bytes(std::span(&raw, 1u))).status ==
            RenderTapeExpectedContentStatus::UnsupportedFormat &&
            renderTapeLinearBytesPerPixel(surface.format) == 0u &&
            !renderTapeSnapshotStandaloneD24X8Supported(surface) &&
            !renderTapeProducedStandaloneSurfaceSupported(surface),
        "D24S8 does not widen raw lock, D24X8, or produced-pass policy");

  for (const auto member : std::array{
           &D9CSurfaceDesc::format, &D9CSurfaceDesc::resourceType,
           &D9CSurfaceDesc::usage, &D9CSurfaceDesc::pool,
           &D9CSurfaceDesc::multiSampleType, &D9CSurfaceDesc::multiSampleQuality,
           &D9CSurfaceDesc::width, &D9CSurfaceDesc::height, &D9CSurfaceDesc::depth}) {
    auto invalid = descriptor;
    invalid.surface.*member = member == &D9CSurfaceDesc::width ||
                                     member == &D9CSurfaceDesc::height
                                 ? 0u : (invalid.surface.*member) + 1u;
    layout = {.depthPitch = 99u, .stencilPitch = 99u,
              .stencilOffset = 99u, .byteCount = 99u};
    check(!renderTapeSnapshotStandaloneD24S8Supported(invalid.surface) &&
              !renderTapeDeriveD24S8SnapshotLayout(invalid.surface, layout) &&
              layout.depthPitch == 0u && layout.stencilPitch == 0u &&
              layout.stencilOffset == 0u && layout.byteCount == 0u &&
              !renderTapeLoadSurfaceDescriptorV2(
                  std::as_bytes(std::span(&invalid, 1u)), loaded),
          "unsupported D24S8 shape clears layout and rejects disposition 6");
  }
  for (const auto storage : {RenderTapeSurfaceStorage::TextureSubresource,
                            RenderTapeSurfaceStorage::SwapchainBackbuffer}) {
    auto invalid = descriptor;
    invalid.storage = static_cast<std::uint32_t>(storage);
    check(!renderTapeLoadSurfaceDescriptorV2(
              std::as_bytes(std::span(&invalid, 1u)), loaded),
          "D24S8 disposition 6 is standalone-only");
  }
  for (unsigned mutation = 0u; mutation < 3u; ++mutation) {
    auto invalid = descriptor;
    if (mutation == 0u) invalid.schemaVersion = 3u;
    if (mutation == 1u) invalid.subresource = 1u;
    if (mutation == 2u) invalid.parentTexture = depth;
    check(!renderTapeLoadSurfaceDescriptorV2(
              std::as_bytes(std::span(&invalid, 1u)), loaded),
          "D24S8 descriptor rejects malformed version, subresource, or parent");
  }
  auto overflow = descriptor;
  overflow.surface.width = std::numeric_limits<std::uint32_t>::max();
  check(!renderTapeDeriveD24S8SnapshotLayout(overflow.surface, layout) &&
            renderTapeDeriveExpectedContentContract(
                depth.kind, std::as_bytes(std::span(&overflow, 1u))).status ==
                RenderTapeExpectedContentStatus::Overflow,
        "D24S8 four-byte depth row pitch cannot wrap uint32");
  overflow.surface.width = std::numeric_limits<std::uint32_t>::max() / 4u;
  overflow.surface.height = std::numeric_limits<std::uint32_t>::max();
  check(!renderTapeDeriveD24S8SnapshotLayout(overflow.surface, layout) &&
            layout.byteCount == 0u &&
            renderTapeDeriveExpectedContentContract(
                depth.kind, std::as_bytes(std::span(&overflow, 1u))).status ==
                RenderTapeExpectedContentStatus::Overflow,
        "D24S8 5N total extent cannot wrap uint64");

  const D9CRenderTapeD24S8SnapshotRequest request{
      .identity = depth, .surface = surface,
      .encodingVersion = D9C_RENDER_TAPE_D24S8_ENCODING_FLOAT32_LE_STENCIL_UINT8_V1,
  };
  const D9CRenderTapeD24S8SnapshotResult result{
      .status = D9C_RENDER_TAPE_D24S8_SNAPSHOT_COMPLETE,
      .encodingVersion = D9C_RENDER_TAPE_D24S8_ENCODING_FLOAT32_LE_STENCIL_UINT8_V1,
      .width = 17u, .height = 19u, .depthPitch = 68u, .stencilPitch = 17u,
      .physicalFormat = D9C_RENDER_TAPE_D24S8_PHYSICAL_FORMAT_D32FLOAT_STENCIL8,
      .stencilOffset = 1292u, .byteCount = 1615u,
  };
  check(renderTapeD24S8SnapshotResultMatches(request, result, 1615u) &&
            !renderTapeD24S8SnapshotResultMatches(request, {}, 1615u),
        "only an exact complete D32FloatS8 readback result can seed the arm overlay");
  for (const auto member : std::array{
           &D9CRenderTapeD24S8SnapshotResult::status,
           &D9CRenderTapeD24S8SnapshotResult::encodingVersion,
           &D9CRenderTapeD24S8SnapshotResult::width,
           &D9CRenderTapeD24S8SnapshotResult::height,
           &D9CRenderTapeD24S8SnapshotResult::depthPitch,
           &D9CRenderTapeD24S8SnapshotResult::stencilPitch,
           &D9CRenderTapeD24S8SnapshotResult::physicalFormat,
           &D9CRenderTapeD24S8SnapshotResult::reserved0}) {
    auto invalid = result;
    ++(invalid.*member);
    check(!renderTapeD24S8SnapshotResultMatches(request, invalid, 1615u),
          "every D24S8 result uint32 field is validated");
  }
  for (const auto member : std::array{
           &D9CRenderTapeD24S8SnapshotResult::stencilOffset,
           &D9CRenderTapeD24S8SnapshotResult::byteCount}) {
    auto invalid = result;
    ++(invalid.*member);
    check(!renderTapeD24S8SnapshotResultMatches(request, invalid, 1615u),
          "D24S8 plane offset and total byte count are validated");
  }
  auto wrongBacking = result;
  wrongBacking.physicalFormat = 255u; // Depth24Unorm_Stencil8 is not the contract.
  check(!renderTapeD24S8SnapshotResultMatches(request, wrongBacking, 1615u),
        "disposition 6 cannot lose its fixed D32FloatS8 backing contract");
  for (unsigned mutation = 0u; mutation < 6u; ++mutation) {
    auto invalid = request;
    if (mutation == 0u) invalid.identity.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
    if (mutation == 1u) invalid.identity.generation = 0u;
    if (mutation == 2u) invalid.identity.objectId = 0u;
    if (mutation == 3u) invalid.encodingVersion = 2u;
    if (mutation == 4u) invalid.reserved0 = 1u;
    if (mutation == 5u) invalid.surface.format = render_tape_d3d_format::D24X8;
    check(!renderTapeD24S8SnapshotResultMatches(invalid, result, 1615u),
          "D24S8 arm readback validates generation-qualified identity and request");
  }
  check(!renderTapeD24S8SnapshotResultMatches(request, result, 1614u) &&
            !renderTapeD24S8SnapshotResultMatches(request, result, 1616u),
        "D24S8 arm readback capacity must be exactly 5N");

  const std::vector<std::vector<std::byte>> durableContent(1u);
  std::vector<std::vector<std::byte>> armContent{
      std::vector<std::byte>(1615u, std::byte{0x15})};
  std::fill(armContent[0].begin() + 1292u, armContent[0].end(), std::byte{0x47});
  const auto rawBytes = std::as_bytes(std::span(&raw, 1u));
  const std::vector<std::byte> rawBefore(rawBytes.begin(), rawBytes.end());
  const auto selected = renderTapeSelectArmObjectSnapshotOverlay(
      std::as_bytes(std::span(&raw, 1u)), durableContent,
      descriptorBytes, armContent, 3u, 3u);
  check(selected.source == RenderTapeArmSnapshotOverlaySource::CurrentArm &&
            renderTapeArmObjectSnapshotContentComplete(
                1u, false, RenderTapeArmObjectSnapshotOverlayPolicy::Ordinary,
                selected.source, selected.content) &&
            std::equal(selected.content[0].begin(), selected.content[0].end(),
                       armContent[0].begin(), armContent[0].end()) &&
            durableContent[0].empty() &&
            std::equal(rawBytes.begin(), rawBytes.end(), rawBefore.begin()),
        "D24S8 arm overlay preserves both planes without mutating durable registry");
  const auto stale = renderTapeSelectArmObjectSnapshotOverlay(
      std::as_bytes(std::span(&raw, 1u)), durableContent,
      descriptorBytes, armContent, 2u, 3u);
  check(stale.source == RenderTapeArmSnapshotOverlaySource::StaleArm &&
            stale.content.empty(),
        "D24S8 seed from an earlier arm is not current content");
  std::array<RenderTapeBootstrapClosureObject, 2u> objects{
      RenderTapeBootstrapClosureObject{.identity = depth, .complete = true},
      RenderTapeBootstrapClosureObject{.identity = output, .complete = true},
  };
  std::vector<D9CWireObjectIdentity> closure;
  check(renderTapeBuildBootstrapClosure(std::span(&depth, 1u), output,
                                        objects, closure) ==
            RenderTapeBootstrapClosureStatus::Accepted && closure.size() == 2u &&
            renderTapeBootstrapClosureContains(closure, depth),
        "a seeded D24S8 remains in the exact bound bootstrap closure");
  objects[0].complete = false;
  const auto missing = renderTapeBuildBootstrapClosureAttributed(
      std::span(&depth, 1u), output, objects, closure);
  check(missing.status ==
            RenderTapeBootstrapClosureStatus::ReferencedObjectIncomplete &&
            missing.offendingIdentity.generation == depth.generation &&
            missing.offendingIdentity.objectId == depth.objectId,
        "missing D24S8 still rejects its exact bound identity without a produced shortcut");
  auto staleDepth = depth;
  ++staleDepth.generation;
  objects[0].complete = true;
  const auto staleIdentity = renderTapeBuildBootstrapClosureAttributed(
      std::span(&staleDepth, 1u), output, objects, closure);
  check(staleIdentity.status ==
            RenderTapeBootstrapClosureStatus::ReferencedObjectMissing &&
            staleIdentity.offendingIdentity.generation == staleDepth.generation &&
            staleIdentity.offendingIdentity.objectId == staleDepth.objectId,
        "seeded prior depth generation cannot satisfy the bound current generation");
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
        "the D24X8 snapshot encoding does not admit D24S8");
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
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.bytes == 256u && contract.count == 1u,
        "V2 volume derives an exact seed extent across all slices");
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
    auto volumeDescriptor = versionedTextureDescriptor(
        RenderTapeTextureDimension::Volume, 1u,
        render_tape_d3d_format::A8R8G8B8, 2u, 2u);
    auto destinationDescriptor = volumeDescriptor;
    setPool(volumeDescriptor, 2u);
    setPool(destinationDescriptor, 0u);
    const std::vector<std::vector<std::byte>> volumeSource{
        std::vector<std::byte>(32u, std::byte{0x31})};
    std::vector<std::vector<std::byte>> destination(1u);
    check(applyRenderTapeUpdateTextureClosure(
              volumeDescriptor, volumeSource, destinationDescriptor,
              destination) == RenderTapeUpdateTextureStatus::
                                 Accepted && destination == volumeSource,
          "UpdateTexture closes every slice of a complete volume mip");
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
      RenderTapeCaptureBlob{partial},
      RenderTapeCaptureBlob{complete}};
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
      RenderTapeCaptureBlob{complete},
      RenderTapeCaptureBlob{complete}};
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

void testArmFailureProvenanceAndAbortRelease() {
  const auto bootstrap = bootstrapChunk();
  const std::array<RenderTapeCaptureBlob, 2u> blobs{
      RenderTapeCaptureBlob{std::vector<std::byte>(3u, std::byte{1u})},
      RenderTapeCaptureBlob{std::vector<std::byte>(2u, std::byte{2u})}};
  RenderTapeCaptureSession bytesLimited(true, {.maxBlobBytes = 4u});
  check(bytesLimited.armWithBlobs(bootstrap, blobs) ==
            RenderTapeCaptureStatus::CapacityExceeded,
        "arm provenance fixture hits unchanged blob byte limit");
  const auto byteFailure = bytesLimited.armFailure();
  check(std::string_view(byteFailure.reason) == "blob_byte_limit" &&
            std::string_view(byteFailure.exceptionKind) == "none" &&
            std::string_view(byteFailure.layer) == "arm_with_blobs" &&
            byteFailure.blobIndex == 1u && byteFailure.blobBytes == 2u &&
            byteFailure.uniqueBlobCount == 1u &&
            byteFailure.cumulativeBlobBytes == 3u &&
            bytesLimited.limits().maxBlobBytes == 4u,
        "explicit blob limit is separate from exceptions with exact prior/incoming extents");
  RenderTapeCaptureSession entriesLimited(true, {.maxBlobEntries = 1u});
  check(entriesLimited.armWithBlobs(bootstrap, blobs) ==
            RenderTapeCaptureStatus::CapacityExceeded &&
            std::string_view(entriesLimited.armFailure().reason) == "blob_entry_limit" &&
            entriesLimited.armFailure().blobIndex == 1u,
        "explicit unique entry count limit has its own arm provenance");
  RenderTapeCaptureSession eventsLimited(true, {.maxEvents = 0u});
  check(eventsLimited.armWithBlobs(bootstrap, blobs) ==
            RenderTapeCaptureStatus::CapacityExceeded,
        "wrapper fixture delegates to unchanged bootstrap event reserve limit");
  const auto eventFailure = eventsLimited.armFailure();
  check(std::string_view(eventFailure.reason) == "event_count_limit" &&
            std::string_view(eventFailure.layer) == "arm" &&
            std::string_view(eventFailure.phase) == "bootstrap_event_reserve" &&
            std::string_view(eventFailure.exceptionKind) == "none" &&
            eventFailure.inputBlobCount == 2u && eventFailure.uniqueBlobCount == 2u &&
            eventFailure.cumulativeBlobBytes == 5u &&
            eventFailure.eventPayloadBytes ==
                sizeof(RenderTapeBootstrapHeader) + bootstrap.size(),
        "downstream event rejection retains both exact layer and wrapper byte totals");
  RenderTapeCaptureSession eventBytesLimited(true, {.maxEventBytes = 0u});
  check(eventBytesLimited.arm(bootstrap) == RenderTapeCaptureStatus::CapacityExceeded &&
            std::string_view(eventBytesLimited.armFailure().reason) == "event_byte_limit",
        "direct arm event-byte limit is not mislabeled as allocation failure");
  constexpr std::array<std::byte, 1u> malformed{std::byte{0u}};
  RenderTapeCaptureSession malformedArm(true);
  check(malformedArm.armWithBlobs(malformed, blobs) ==
            RenderTapeCaptureStatus::CapacityExceeded &&
            std::string_view(malformedArm.armFailure().layer) == "arm" &&
            std::string_view(malformedArm.armFailure().phase) == "bootstrap_append" &&
            std::string_view(malformedArm.armFailure().reason) == "exception" &&
            std::string_view(malformedArm.armFailure().exceptionKind) == "invalid_argument",
        "downstream canonical-overlay exception survives wrapper status translation");

  bool sawOwnedCopy = false;
  bool sawCatalogueCopy = false;
  bool sawBootstrapAppend = false;
  bool reachedSuccess = false;
  for (std::size_t allocation = 0u; allocation < 32u; ++allocation) {
    RenderTapeCaptureSession injected(true);
    capture_state_allocation_probe::failAfter = allocation;
    capture_state_allocation_probe::failEnabled = true;
    const auto status = injected.armWithBlobs(bootstrap, blobs);
    capture_state_allocation_probe::failEnabled = false;
    if (status == RenderTapeCaptureStatus::Accepted) {
      reachedSuccess = true;
      check(std::string_view(injected.armFailure().reason) == "none",
            "successful arm has no stale exception provenance");
      break;
    }
    const auto failure = injected.armFailure();
    check(status == RenderTapeCaptureStatus::CapacityExceeded &&
              std::string_view(failure.reason) == "exception" &&
              std::string_view(failure.exceptionKind) == "bad_alloc" &&
              injected.ownedBlobBytes() == 0u &&
              injected.publicationBundle().events.empty(),
          "every injected allocation failure is typed without publishing partial data");
    const std::string_view phase(failure.phase);
    check(phase != "blob_digest",
          "allocation fault injection cannot fail the allocation-free blob digest");
    sawOwnedCopy |= phase == "owned_blob_copy";
    sawCatalogueCopy |= phase == "catalogue_copy";
    sawBootstrapAppend |= phase == "bootstrap_append";
    if (std::string_view(failure.layer) == "arm")
      check(failure.uniqueBlobCount == 2u && failure.cumulativeBlobBytes == 5u,
            "inner allocation exception retains complete outer copied-byte accounting");
  }
  check(reachedSuccess && sawOwnedCopy && sawCatalogueCopy && sawBootstrapAppend,
        "allocation fault sweep binds both arm layers and each large copy/append phase");

  const RenderTapeCaptureBlob retained{
      std::vector<std::byte>(8197u, std::byte{0x35u})};
  RenderTapeCaptureSession session(true);
  capture_state_allocation_probe::watchSize = retained.bytes.size();
  check(session.armWithBlobs(bootstrap, std::span(&retained, 1u)) ==
            RenderTapeCaptureStatus::Accepted,
        "abort release fixture retains an owned payload distinct from caller bytes");
  capture_state_allocation_probe::watchSize = 0u;
  check(capture_state_allocation_probe::watchedPointer &&
            capture_state_allocation_probe::watchedPointer != retained.bytes.data() &&
            !capture_state_allocation_probe::watchedFreed,
        "allocation observer identifies the session-owned raw payload");
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 1u, .objectId = 99u};
  const D9CBufferDesc buffer{.size = 8197u};
  check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            session.objectDefine(identity,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
                std::as_bytes(std::span(&buffer, 1u)), 0u, {}) ==
                RenderTapeCaptureStatus::Accepted,
        "abort release fixture retains descriptor and builder event payloads");
  const auto previousEvents = session.eventCount();
  const auto previousBytes = session.bufferedBytes();
  capture_state_allocation_probe::count = 0u;
  capture_state_allocation_probe::enabled = true;
  session.abort();
  capture_state_allocation_probe::enabled = false;
  const auto aborted = session.lastAbortAccounting();
  check(capture_state_allocation_probe::count == 0u &&
            capture_state_allocation_probe::watchedFreed &&
            session.state() == RenderTapeCaptureState::Aborted &&
            session.ownedBlobBytes() == 0u && session.ownedBlobEntries() == 0u &&
            session.eventCount() == 0u && session.bufferedBytes() == 0u &&
            !session.hasLiveObject(identity) && session.sealedArtifact().empty() &&
            aborted.valid && aborted.eventCount == previousEvents &&
            aborted.eventBytes == previousBytes && aborted.blobCount == 1u &&
            aborted.blobBytes == retained.bytes.size() && aborted.objectCount == 1u,
        "abort frees payload owners allocation-free, zeros live accounting, preserves value evidence");
  session.abort();
  check(session.lastAbortAccounting().eventCount == previousEvents &&
            session.armWithBlobs(bootstrap, std::span(&retained, 1u)) ==
                RenderTapeCaptureStatus::Accepted &&
            session.ownedBlobBytes() == retained.bytes.size() &&
            !session.lastAbortAccounting().valid &&
            std::string_view(session.armFailure().reason) == "none",
        "repeated abort preserves attribution and retry restores unchanged admission");
  capture_state_allocation_probe::watchedPointer = nullptr;
}

void testMutationFailureProvenanceAndFaults() {
  static_assert(std::is_trivially_copyable_v<RenderTapeCaptureMutationFailure>);
  const auto bootstrap = bootstrapChunk();
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 7u, .objectId = 159u};
  const D9CBufferDesc descriptor{.size = 64u};
  constexpr std::array<std::byte, 7u> payload{
      std::byte{1u}, std::byte{2u}, std::byte{3u}, std::byte{4u},
      std::byte{5u}, std::byte{6u}, std::byte{7u}};
  const auto digest = RenderTapeCaptureSession::sha256(payload);
  const auto setup = [&](RenderTapeCaptureSession& session) {
    check(session.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
              session.objectDefine(identity,
                  static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
                  std::as_bytes(std::span(&descriptor, 1u)), 0u, {}) ==
                  RenderTapeCaptureStatus::Accepted,
          "mutation provenance fixture starts with exactly two complete events");
  };
  const auto mutate = [&](RenderTapeCaptureSession& session) {
    return session.resourceMutationBytes(
        identity, RenderTapeMutationKind::CpuUnlock, 2u, 3u, payload,
        RenderTapeBufferMutationDisposition::NoOverwrite);
  };
  const auto checkExtent = [&](const RenderTapeCaptureMutationFailure& failure) {
    check(failure.valid && failure.hasIdentity &&
              failure.identity.kind == identity.kind &&
              failure.identity.generation == identity.generation &&
              failure.identity.objectId == identity.objectId &&
              failure.kind == RenderTapeMutationKind::CpuUnlock &&
              failure.bufferDisposition == RenderTapeBufferMutationDisposition::NoOverwrite &&
              failure.subresource == 2u && failure.byteOffset == 3u &&
              failure.byteSize == payload.size() &&
              failure.eventPayloadBytes == sizeof(RenderTapeResourceMutationHeader),
          "nested failures preserve the exact identity, mutation kind and incoming extent");
  };

  RenderTapeCaptureSession disabled(false);
  capture_state_allocation_probe::count = 0u;
  capture_state_allocation_probe::enabled = true;
  const auto disabledStatus = mutate(disabled);
  capture_state_allocation_probe::enabled = false;
  check(disabledStatus == RenderTapeCaptureStatus::Disabled &&
            !disabled.mutationFailure().valid && capture_state_allocation_probe::count == 0u,
        "capture-disabled mutation remains allocation-free without diagnostic writes");
  RenderTapeCaptureSession notArmed(true);
  check(mutate(notArmed) == RenderTapeCaptureStatus::InvalidState &&
            std::string_view(notArmed.mutationFailure().phase) == "state_validation" &&
            std::string_view(notArmed.mutationFailure().layer) == "register_blob_bytes",
        "registration state failure remains distinct from event state failure");
  checkExtent(notArmed.mutationFailure());
  RenderTapeCaptureSession armed(true);
  check(armed.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
            mutate(armed) == RenderTapeCaptureStatus::InvalidState &&
            std::string_view(armed.mutationFailure().layer) == "resource_mutation" &&
            armed.mutationFailure().accounting.blobBytes == payload.size() &&
            armed.state() == RenderTapeCaptureState::Armed,
        "unchanged registration-before-validation order is visible for an Armed owner");

  for (const bool entryLimit : {false, true}) {
    RenderTapeCaptureSession limited(true,
        {.maxBlobEntries = entryLimit ? 0u : 4096u,
         .maxBlobBytes = entryLimit ? kRenderTapeDefaultMaxBlobBytes : 6u});
    setup(limited);
    capture_state_allocation_probe::count = 0u;
    capture_state_allocation_probe::enabled = true;
    const auto status = mutate(limited);
    capture_state_allocation_probe::enabled = false;
    const auto failure = limited.mutationFailure();
    checkExtent(failure);
    check(status == RenderTapeCaptureStatus::CapacityExceeded && failure.status == status &&
              std::string_view(failure.reason) ==
                  (entryLimit ? "blob_entry_limit" : "blob_byte_limit") &&
              std::string_view(failure.phase) ==
                  (entryLimit ? "blob_entry_admission" : "blob_byte_admission") &&
              std::string_view(failure.exceptionKind) == "none" &&
              failure.accounting.eventCount == 2u && failure.accounting.blobCount == 0u &&
              failure.accounting.blobBytes == 0u && failure.accounting.objectCount == 1u &&
              limited.state() == RenderTapeCaptureState::Capturing &&
              capture_state_allocation_probe::count == 0u,
          "explicit blob limits retain complete values allocation-free without aborting early");
    limited.abort();
    check(limited.mutationFailure().byteSize == payload.size() &&
              limited.lastAbortAccounting().eventCount == 2u,
          "caller-triggered abort cannot erase blob rejection provenance");
  }
  RenderTapeCaptureSession cumulativeLimit(true, {.maxBlobBytes = 10u});
  setup(cumulativeLimit);
  check(cumulativeLimit.registerBlobBytes(std::span(payload).first(4u)) ==
            RenderTapeCaptureStatus::Accepted &&
            mutate(cumulativeLimit) == RenderTapeCaptureStatus::CapacityExceeded &&
            std::string_view(cumulativeLimit.mutationFailure().reason) == "blob_byte_limit" &&
            cumulativeLimit.mutationFailure().byteSize == 7u &&
            cumulativeLimit.mutationFailure().accounting.blobBytes == 4u &&
            cumulativeLimit.mutationFailure().accounting.blobCount == 1u &&
            cumulativeLimit.ownedBlobBytes() == 4u,
        "cumulative admission failure distinguishes incoming bytes from already retained bytes");

  const std::uint64_t initialEventBytes = sizeof(RenderTapeBootstrapHeader) +
      bootstrap.size() + sizeof(RenderTapeObjectDefineHeader) + sizeof(descriptor);
  for (const bool countLimit : {false, true}) {
    RenderTapeCaptureSession limited(true,
        {.maxEvents = countLimit ? 2u : 4096u,
         .maxEventBytes = countLimit ? 64u * 1024u * 1024u : initialEventBytes});
    setup(limited);
    check(mutate(limited) == RenderTapeCaptureStatus::CapacityExceeded,
          "unchanged event reserve rejects the third event at the configured exact bound");
    const auto failure = limited.mutationFailure();
    checkExtent(failure);
    check(std::string_view(failure.reason) ==
                  (countLimit ? "event_count_limit" : "event_byte_limit") &&
              std::string_view(failure.phase) == "event_reserve" &&
              std::string_view(failure.exceptionKind) == "none" &&
              failure.captureState == RenderTapeCaptureState::Capturing &&
              failure.accounting.eventCount == 2u &&
              failure.accounting.eventBytes == initialEventBytes &&
              failure.accounting.blobCount == 1u &&
              failure.accounting.blobBytes == payload.size() &&
              failure.publishedBlobCount == 1u && failure.accounting.objectCount == 1u &&
              limited.state() == RenderTapeCaptureState::Aborted &&
              limited.eventCount() == 0u && limited.ownedBlobEntries() == 0u &&
              limited.lastAbortAccounting().eventCount == 2u &&
              limited.lastAbortAccounting().blobBytes == payload.size(),
          "automatic reserve abort classifies and preserves pre-release counts, not cleared counts");
    limited.abort();
    check(std::string_view(limited.mutationFailure().reason) ==
              (countLimit ? "event_count_limit" : "event_byte_limit"),
          "repeated abort retains the original event rejection reason");
  }

  RenderTapeCaptureSession validation(true);
  setup(validation);
  check(validation.registerBlobBytes(payload) == RenderTapeCaptureStatus::Accepted,
        "validation fixture independently verifies the referenced bytes");
  auto badIdentity = identity;
  badIdentity.generation = 0u;
  auto missingIdentity = identity;
  ++missingIdentity.objectId;
  struct InvalidMutation {
    D9CWireObjectIdentity identity;
    std::uint64_t offset;
    std::uint64_t size;
    RenderTapeDigest digest;
    const char* reason;
  };
  const std::array<InvalidMutation, 5u> invalid{
      InvalidMutation{badIdentity, 0u, payload.size(), digest, "invalid_identity"},
      InvalidMutation{missingIdentity, 0u, payload.size(), digest, "missing_live_object"},
      InvalidMutation{identity, 0u, 0u, digest, "empty_mutation"},
      InvalidMutation{identity, UINT64_MAX, payload.size(), digest, "mutation_extent_overflow"},
      InvalidMutation{identity, 0u, payload.size(), {}, "missing_verified_blob"}};
  for (const auto& input : invalid) {
    check(validation.resourceMutation(input.identity, RenderTapeMutationKind::Upload,
              0u, input.offset, input.size, input.digest) == RenderTapeCaptureStatus::InvalidInput &&
              std::string_view(validation.mutationFailure().reason) == input.reason &&
              std::string_view(validation.mutationFailure().phase) == "event_validation" &&
              std::string_view(validation.mutationFailure().exceptionKind) == "none" &&
              validation.eventCount() == 2u && validation.ownedBlobBytes() == payload.size(),
          "each existing event validation predicate has exact provenance without side effects");
  }
  RenderTapeCaptureSession missingObject(true);
  setup(missingObject);
  check(missingObject.resourceMutationBytes(missingIdentity, RenderTapeMutationKind::Upload,
            0u, 0u, payload) == RenderTapeCaptureStatus::InvalidInput &&
            std::string_view(missingObject.mutationFailure().reason) == "missing_live_object" &&
            missingObject.mutationFailure().identity.objectId == missingIdentity.objectId &&
            missingObject.mutationFailure().accounting.blobBytes == payload.size() &&
            missingObject.ownedBlobBytes() == payload.size() && missingObject.eventCount() == 2u &&
            missingObject.state() == RenderTapeCaptureState::Capturing,
        "wrapper validation rejection still retains the previously registered blob until caller abort");
  capture_state_allocation_probe::failAfter = 0u;
  capture_state_allocation_probe::failEnabled = true;
  const auto duplicateStatus = validation.registerBlobBytes(payload);
  const bool duplicateAllocated = !capture_state_allocation_probe::failEnabled;
  capture_state_allocation_probe::failEnabled = false;
  check(duplicateStatus == RenderTapeCaptureStatus::Accepted && !duplicateAllocated &&
            !validation.mutationFailure().valid && validation.ownedBlobEntries() == 1u,
        "byte equality deduplication allocates nothing and clears stale failure provenance");

  bool sawCatalogue = false;
  bool sawCopy = false;
  bool sawPublication = false;
  bool sawEvent = false;
  bool reachedSuccess = false;
  for (std::size_t allocation = 0u; allocation < 32u; ++allocation) {
    RenderTapeCaptureSession injected(true);
    setup(injected);
    capture_state_allocation_probe::failAfter = allocation;
    capture_state_allocation_probe::failEnabled = true;
    const auto status = mutate(injected);
    capture_state_allocation_probe::failEnabled = false;
    if (status == RenderTapeCaptureStatus::Accepted) {
      reachedSuccess = true;
      check(!injected.mutationFailure().valid && injected.eventCount() == 3u &&
                injected.ownedBlobBytes() == payload.size(),
            "successful mutation has unchanged admission and no stale provenance");
      break;
    }
    const auto failure = injected.mutationFailure();
    checkExtent(failure);
    const std::string_view phase(failure.phase);
    const bool eventFailure = phase == "event_append";
    check(status == RenderTapeCaptureStatus::CapacityExceeded && failure.status == status &&
              std::string_view(failure.reason) == "exception" &&
              std::string_view(failure.exceptionKind) == "bad_alloc" &&
              failure.accounting.eventCount == 2u &&
              failure.accounting.eventBytes == initialEventBytes &&
              failure.accounting.objectCount == 1u &&
              failure.accounting.blobCount == (eventFailure ? 1u : 0u) &&
              failure.accounting.blobBytes == (eventFailure ? payload.size() : 0u) &&
              failure.publishedBlobCount == (eventFailure ? 1u : 0u) &&
              injected.state() == (eventFailure ? RenderTapeCaptureState::Aborted
                                               : RenderTapeCaptureState::Capturing) &&
              injected.ownedBlobEntries() == 0u && injected.ownedBlobBytes() == 0u &&
              injected.publicationBundle().events.empty() && injected.sealedArtifact().empty(),
          "fault sweep separates exact allocation stages while preserving rollback and terminal semantics");
    check(phase != "blob_digest" && phase != "blob_deduplicate",
          "allocation-free digest and byte deduplication cannot be the allocation fault stage");
    sawCatalogue |= phase == "blob_catalogue_append";
    sawCopy |= phase == "blob_payload_copy";
    sawPublication |= phase == "blob_publication_append";
    sawEvent |= eventFailure;
    capture_state_allocation_probe::count = 0u;
    capture_state_allocation_probe::enabled = true;
    injected.abort();
    capture_state_allocation_probe::enabled = false;
    check(capture_state_allocation_probe::count == 0u &&
              injected.mutationFailure().phase == failure.phase &&
              injected.mutationFailure().accounting.eventCount == 2u &&
              injected.lastAbortAccounting().eventCount == 2u,
          "abort and provenance retention allocate nothing after every injected failure");
    check(injected.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
              !injected.mutationFailure().valid,
          "successful rearm clears terminal mutation provenance");
  }
  check(reachedSuccess && sawCatalogue && sawCopy && sawPublication && sawEvent,
        "fault sweep binds catalogue, payload copy, publication append, and event append independently");

  const std::array<std::exception_ptr, 5u> exceptions{
      std::make_exception_ptr(std::bad_alloc()),
      std::make_exception_ptr(std::length_error("injected length")),
      std::make_exception_ptr(std::invalid_argument("injected argument")),
      std::make_exception_ptr(std::exception()), std::make_exception_ptr(17u)};
  constexpr std::array<const char*, 5u> exceptionNames{
      "bad_alloc", "length_error", "invalid_argument", "std_exception", "unknown"};
  for (std::size_t i = 0u; i < exceptions.size(); ++i) {
    for (const bool eventFailure : {false, true}) {
      RenderTapeCaptureSession injected(true);
      setup(injected);
      if (eventFailure) {
        check(injected.registerBlobBytes(payload) == RenderTapeCaptureStatus::Accepted,
              "typed event fault fixture deduplicates registration before append");
      }
      capture_state_allocation_probe::injectedException = exceptions[i];
      capture_state_allocation_probe::failAfter = 0u;
      capture_state_allocation_probe::failEnabled = true;
      capture_state_allocation_probe::count = 0u;
      capture_state_allocation_probe::enabled = true;
      const auto status = mutate(injected);
      capture_state_allocation_probe::enabled = false;
      capture_state_allocation_probe::failEnabled = false;
      capture_state_allocation_probe::injectedException = {};
      check(status == RenderTapeCaptureStatus::CapacityExceeded &&
                std::string_view(injected.mutationFailure().exceptionKind) == exceptionNames[i] &&
                std::string_view(injected.mutationFailure().phase) ==
                    (eventFailure ? "event_append" : "blob_catalogue_append") &&
                capture_state_allocation_probe::count == 0u,
            "both failure layers classify typed and unknown exceptions without diagnostic allocation");
    }
  }

  RenderTapeCaptureSession preverified(true);
  setup(preverified);
  check(preverified.registerVerifiedBlob(digest, payload.size()) ==
            RenderTapeCaptureStatus::Accepted,
        "preverified catalogue fixture has no owned payload yet");
  capture_state_allocation_probe::failAfter = 0u;
  capture_state_allocation_probe::failEnabled = true;
  const auto preverifiedStatus = preverified.registerBlobBytes(payload);
  capture_state_allocation_probe::failEnabled = false;
  check(preverifiedStatus == RenderTapeCaptureStatus::CapacityExceeded &&
            std::string_view(preverified.mutationFailure().phase) == "blob_payload_copy" &&
            !preverified.mutationFailure().hasIdentity &&
            preverified.mutationFailure().accounting.blobCount == 1u &&
            preverified.ownedBlobEntries() == 1u && preverified.ownedBlobBytes() == 0u &&
            preverified.registerBlobBytes(payload) == RenderTapeCaptureStatus::Accepted &&
            !preverified.mutationFailure().valid && preverified.ownedBlobEntries() == 1u,
        "direct byte registration failure cannot roll back a pre-existing verified catalogue entry");
}

void testArmSnapshotTransferGuardExhaustive() {
  using Disposition = RenderTapeArmContentTransferDisposition;
  using Source = RenderTapeArmSnapshotOverlaySource;
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 3u, .objectId = 501u};
  RenderTapeSnapshotBudget snapshotBudget;
  check(snapshotBudget.initialize(kRenderTapeDefaultMaxBlobBytes) ==
            RenderTapeSnapshotBudgetStatus::Complete,
        "transfer guard fixture initializes retained snapshot budget");
  auto firstReservation = snapshotBudget.reserveRetained(1u);
  auto secondReservation = snapshotBudget.reserveRetained(1u);
  check(firstReservation && secondReservation,
        "transfer guard fixture reserves every original subresource before allocation");
  RenderTapeArmObjectSnapshot snapshot;
  snapshot.objectIndex = 999u;
  snapshot.armOrdinal = 7u;
  snapshot.identity = identity;
  snapshot.descriptor = {std::byte{1u}};
  snapshot.retainedLeases.push_back(std::move(firstReservation.lease));
  snapshot.retainedLeases.push_back(std::move(secondReservation.lease));
  snapshot.content.emplace_back(1u, std::byte{2u});
  snapshot.content.emplace_back(1u, std::byte{3u});
  for (std::size_t subresource = 0u; subresource < snapshot.content.size(); ++subresource)
    check(snapshot.retainedLeases[subresource].bindRetainedPayload(
              snapshot.content[subresource].data(), snapshot.content[subresource].size()),
          "transfer guard binds receipt to exact subresource allocation");
  const std::vector<std::byte> otherDescriptor{std::byte{1u}};
  const std::vector<std::vector<std::byte>> otherContent = snapshot.content;
  const auto* originalFirst = snapshot.content[0].data();
  constexpr std::array sources{
      Source::Missing, Source::DurableBase, Source::CurrentArm, Source::StaleArm};
  // Exhaust the actual production predicate: source, presence, exact kind /
  // generation / id, epoch, selected storage/extent, and admission state.
  for (const auto source : sources) {
    for (unsigned epoch = 0u; epoch < 4u; ++epoch) {
      for (unsigned bits = 0u; bits < 256u; ++bits) {
        const bool present = (bits & 1u) != 0u;
        const bool exactKind = (bits & 2u) != 0u;
        const bool exactGeneration = (bits & 4u) != 0u;
        const bool exactId = (bits & 8u) != 0u;
        const bool ownedContent = (bits & 16u) != 0u;
        const bool ownedDescriptor = (bits & 32u) != 0u;
        const bool exactCount = (bits & 64u) != 0u;
        const bool admitted = (bits & 128u) != 0u;
        snapshot.identity = identity;
        if (!exactKind) snapshot.identity.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
        if (!exactGeneration) ++snapshot.identity.generation;
        if (!exactId) ++snapshot.identity.objectId;
        snapshot.armOrdinal = epoch == 0u ? 7u : epoch == 1u ? 6u : 0u;
        const auto activeOrdinal = epoch == 3u ? 0u : 7u;
        const auto content = ownedContent
            ? std::span<const std::vector<std::byte>>(snapshot.content)
            : std::span<const std::vector<std::byte>>(otherContent);
        const RenderTapeArmObjectSnapshotOverlaySelection selected{
            .source = source,
            .descriptor = ownedDescriptor ? std::span<const std::byte>(snapshot.descriptor)
                                          : std::span<const std::byte>(otherDescriptor),
            .content = exactCount ? content : content.first(1u)};
        const auto transfer = selectRenderTapeArmContentTransfer(
            present ? &snapshot : nullptr, identity, activeOrdinal, selected, admitted);
        const auto expected = admitted ? Disposition::AlreadyAdmitted
            : source == Source::StaleArm ? Disposition::InvalidSnapshot
            : source != Source::CurrentArm ? Disposition::Borrowed
            : present && exactKind && exactGeneration && exactId && epoch == 0u &&
                      ownedContent && ownedDescriptor && exactCount
                ? Disposition::OwnedSnapshot : Disposition::InvalidSnapshot;
        check(transfer.disposition == expected &&
                  transfer.payload(0u) == (expected == Disposition::OwnedSnapshot
                      ? &snapshot.content[0] : nullptr) &&
                  transfer.payload(2u) == nullptr &&
                  snapshot.content[0].data() == originalFirst &&
                  snapshot.content == otherContent,
              "exhaustive transfer guards reject guessed/stale/admitted storage without consuming bytes");
      }
    }
  }
  snapshot.identity = identity;
  snapshot.identity.generation = 0u;
  snapshot.armOrdinal = 7u;
  const RenderTapeArmObjectSnapshotOverlaySelection invalidIdentityOverlay{
      .source = Source::CurrentArm, .descriptor = snapshot.descriptor, .content = snapshot.content};
  check(selectRenderTapeArmContentTransfer(&snapshot, snapshot.identity, 7u,
            invalidIdentityOverlay, false).disposition == Disposition::InvalidSnapshot,
        "exact equality cannot make a zero-generation identity a transferable owner");
}

void testSelectiveArmTransferAndOwnedMutation() {
  const auto bootstrap = bootstrapChunk();
  const D9CWireObjectIdentity first{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 3u, .objectId = 501u};
  auto second = first;
  ++second.objectId;
  const D9CBufferDesc descriptor{.size = 8197u};
  RenderTapeSnapshotBudget snapshotBudget;
  check(snapshotBudget.initialize(kRenderTapeDefaultMaxBlobBytes) ==
            RenderTapeSnapshotBudgetStatus::Complete,
        "selective transfer fixture initializes nonresettable snapshot budget");
  const auto makeSnapshot = [&](const D9CWireObjectIdentity& identity, std::uint64_t epoch) {
    auto reservation = snapshotBudget.reserveRetained(descriptor.size);
    check(reservation.status == RenderTapeSnapshotBudgetStatus::Complete,
          "selective transfer reserves before vector allocation");
    RenderTapeArmObjectSnapshot snapshot;
    snapshot.objectIndex = 999u;
    snapshot.armOrdinal = epoch;
    snapshot.identity = identity;
    snapshot.descriptor = bytesOf(descriptor);
    snapshot.retainedLeases.push_back(std::move(reservation.lease));
    snapshot.content.emplace_back(descriptor.size, std::byte{0x35u});
    check(snapshot.retainedLeases[0].bindRetainedPayload(
              snapshot.content[0].data(), snapshot.content[0].size()),
          "selective transfer binds original retained snapshot allocation");
    return snapshot;
  };
  const auto select = [](RenderTapeArmObjectSnapshot& snapshot, std::uint64_t epoch,
                         bool admitted = false) {
    const auto overlay = renderTapeSelectArmObjectSnapshotOverlay(
        {}, {}, snapshot.descriptor, snapshot.content, snapshot.armOrdinal, epoch);
    return selectRenderTapeArmContentTransfer(
        &snapshot, snapshot.identity, epoch, overlay, admitted);
  };
  const auto define = [&](RenderTapeCaptureSession& session, const auto& identity) {
    return session.objectDefine(identity,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
        std::as_bytes(std::span(&descriptor, 1u)), 0u, {}, descriptor.size, 1u);
  };

  PeCaptureState pe({}, kRenderTapeProfileFrame, 0u);
  pe.renderTapeArmSnapshotOrdinal = 7u;
  pe.renderTapeArmSnapshots.push_back(makeSnapshot(first, 7u));
  pe.renderTapeArmSnapshots.push_back(makeSnapshot(second, 7u));
  auto& selected = pe.renderTapeArmSnapshots[0];
  auto& unselected = pe.renderTapeArmSnapshots[1];
  const auto selectedDescriptor = selected.descriptor;
  const auto* selectedPointer = selected.content[0].data();
  const auto* unselectedPointer = unselected.content[0].data();
  // A metadata allocation failure before the producer's final swap cannot
  // surrender even the first selected payload or alter an unrelated owner.
  for (const std::size_t allocation : {0u, 1u}) {
    RenderTapeCaptureBootstrapSeed failedSeed;
    bool rejected = false;
    capture_state_allocation_probe::failAfter = allocation;
    capture_state_allocation_probe::failEnabled = true;
    try {
      failedSeed.blobs.emplace_back();
      failedSeed.mutations.push_back({.identity = first, .byteSize = descriptor.size});
    } catch (const std::bad_alloc&) {
      rejected = true;
    }
    capture_state_allocation_probe::failEnabled = false;
    check(rejected && selected.content[0].data() == selectedPointer &&
              unselected.content[0].data() == unselectedPointer &&
              selected.content[0].size() == descriptor.size &&
              pe.renderTapeCapture.publicationBundle().events.empty(),
          "failed bootstrap metadata staging precedes every destructive payload transfer");
  }
  RenderTapeCaptureBootstrapSeed seed;
  seed.blobs.emplace_back();
  const auto selectedExtent = selected.content[0].size();
  const auto selectedDigest = RenderTapeCaptureSession::sha256(selected.content[0]);
  seed.mutations.push_back({.identity = first, .kind = RenderTapeMutationKind::Upload,
      .byteSize = selectedExtent, .digest = selectedDigest});
  // The producer's final transfer follows complete validation/metadata staging.
  const auto transfer = select(selected, 7u);
  seed.blobs[0].retainedLease = std::move(*transfer.retainedLease(0u));
  seed.blobs[0].bytes.swap(*transfer.payload(0u));
  check(seed.blobs[0].bytes.data() == selectedPointer && selected.content[0].empty() &&
            selected.descriptor == selectedDescriptor && selected.identity.objectId == first.objectId &&
            selected.armOrdinal == 7u && unselected.content[0].data() == unselectedPointer &&
            unselected.content[0].size() == selectedExtent &&
            seed.mutations[0].byteSize == selectedExtent,
        "bootstrap transfers only the selected original payload and preserves all metadata/unselected storage");
  capture_state_allocation_probe::watchedPointer = const_cast<std::byte*>(selectedPointer);
  capture_state_allocation_probe::watchedFreed = false;
  capture_state_allocation_probe::watchSize = selectedExtent;
  check(pe.renderTapeCapture.armWithOwnedBlobs(bootstrap, std::move(seed.blobs)) ==
            RenderTapeCaptureStatus::Accepted &&
            pe.renderTapeCapture.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            define(pe.renderTapeCapture, first) == RenderTapeCaptureStatus::Accepted &&
            pe.renderTapeCapture.resourceMutation(first, RenderTapeMutationKind::Upload,
                0u, 0u, selectedExtent, selectedDigest) == RenderTapeCaptureStatus::Accepted &&
            capture_state_allocation_probe::watchedPointer == selectedPointer &&
            !capture_state_allocation_probe::watchedFreed,
        "bootstrap-to-session transfer conserves the original pointer without payload-sized allocation");
  pe.renderTapeAdmittedIdentities.push_back(first);
  const auto eventCountBeforeDuplicate = pe.renderTapeCapture.eventCount();
  for (unsigned reference = 0u; reference < 3u; ++reference) {
    const bool admitted = renderTapeBootstrapClosureContains(pe.renderTapeAdmittedIdentities, first);
    check(admitted && select(selected, 7u, admitted).disposition ==
              RenderTapeArmContentTransferDisposition::AlreadyAdmitted &&
              pe.renderTapeCapture.eventCount() == eventCountBeforeDuplicate,
          "duplicate exact parent references bypass moved-from content without a second Upload");
  }
  const auto unselectedTransfer = select(unselected, 7u);
  check(define(pe.renderTapeCapture, second) == RenderTapeCaptureStatus::Accepted &&
            pe.renderTapeCapture.resourceMutationOwnedBytes(second, RenderTapeMutationKind::Upload,
                0u, 0u, std::move(*unselectedTransfer.payload(0u)),
                RenderTapeBufferMutationDisposition::Plain, *unselectedTransfer.retainedLease(0u)) ==
                RenderTapeCaptureStatus::Accepted && unselected.content[0].empty() &&
            pe.renderTapeCapture.ownedBlobEntries() == 1u &&
            pe.renderTapeCapture.ownedBlobBytes() == selectedExtent &&
            pe.renderTapeCapture.eventCount() == eventCountBeforeDuplicate + 2u &&
            capture_state_allocation_probe::watchedPointer == selectedPointer &&
            !capture_state_allocation_probe::watchedFreed,
        "previously unselected JIT owner transfers, deduplicates equal bytes, but emits its own nonzero Upload");
  unselected.retainedLeases[0].reset();
  capture_state_allocation_probe::watchSize = 0u;
  pe.renderTapeCapture.abort();
  check(capture_state_allocation_probe::watchedFreed,
        "terminal owner release frees the exact snapshot pointer it consumed");
  capture_state_allocation_probe::watchedPointer = nullptr;

  for (const bool success : {false, true}) {
    RenderTapeCaptureSession session(true, {.maxEvents = success ? 4096u : 4u});
    auto a = makeSnapshot(first, 7u);
    auto b = makeSnapshot(second, 7u);
    const auto aTransfer = select(a, 7u);
    const auto bTransfer = select(b, 7u);
    check(session.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
              define(session, first) == RenderTapeCaptureStatus::Accepted &&
              session.resourceMutationOwnedBytes(first, RenderTapeMutationKind::Upload,
                  0u, 0u, std::move(*aTransfer.payload(0u)),
                  RenderTapeBufferMutationDisposition::Plain, *aTransfer.retainedLease(0u)) ==
                  RenderTapeCaptureStatus::Accepted &&
              define(session, second) == RenderTapeCaptureStatus::Accepted,
          "bounded serialized transition commits one owner before the second transfer");
    const auto status = session.resourceMutationOwnedBytes(second, RenderTapeMutationKind::Upload,
        0u, 0u, std::move(*bTransfer.payload(0u)),
        RenderTapeBufferMutationDisposition::Plain, *bTransfer.retainedLease(0u));
    a.retainedLeases[0].reset();
    b.retainedLeases[0].reset();
    check(a.content[0].empty() && b.content[0].empty() &&
              status == (success ? RenderTapeCaptureStatus::Accepted
                                 : RenderTapeCaptureStatus::CapacityExceeded) &&
              session.state() == (success ? RenderTapeCaptureState::Capturing
                                         : RenderTapeCaptureState::Aborted) &&
              (success ? session.ownedBlobBytes() == descriptor.size
                       : session.ownedBlobBytes() == 0u && session.eventCount() == 0u &&
                           session.publicationBundle().events.empty() &&
                           session.mutationFailure().byteSize == descriptor.size),
          "actual owned-transfer transition is complete admission or terminal abort, never missing-byte retry");
    if (!success) {
      std::vector<std::byte> forbiddenRetry(descriptor.size, std::byte{0x35u});
      check(session.resourceMutationOwnedBytes(second, RenderTapeMutationKind::Upload,
                0u, 0u, std::move(forbiddenRetry)) == RenderTapeCaptureStatus::InvalidState &&
                forbiddenRetry.empty() && session.state() == RenderTapeCaptureState::Aborted,
            "consumed partial transfer cannot retry in the same interval");
      auto fresh = makeSnapshot(first, 8u);
      const auto freshTransfer = select(fresh, 8u);
      check(session.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
                session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
                define(session, first) == RenderTapeCaptureStatus::Accepted &&
                session.resourceMutationOwnedBytes(first, RenderTapeMutationKind::Upload,
                    0u, 0u, std::move(*freshTransfer.payload(0u)),
                    RenderTapeBufferMutationDisposition::Plain, *freshTransfer.retainedLease(0u)) ==
                    RenderTapeCaptureStatus::Accepted &&
                !session.mutationFailure().valid,
            "next arm obtains fresh epoch-owned bytes and retries without stale failure authority");
      fresh.retainedLeases[0].reset();
    }
  }
  PeCaptureState partial({.maxEvents = 4u}, kRenderTapeProfileFrame, 0u);
  partial.renderTapeArmSnapshotOrdinal = 7u;
  partial.renderTapeArmSnapshots.push_back(makeSnapshot(first, 7u));
  partial.renderTapeArmSnapshots.push_back(makeSnapshot(second, 7u));
  auto third = second;
  ++third.objectId;
  partial.renderTapeArmSnapshots.push_back(makeSnapshot(third, 7u));
  auto& a = partial.renderTapeArmSnapshots[0];
  auto& b = partial.renderTapeArmSnapshots[1];
  auto& untouched = partial.renderTapeArmSnapshots[2];
  const auto aTransfer = select(a, 7u);
  const auto bTransfer = select(b, 7u);
  capture_state_allocation_probe::watchedPointer = untouched.content[0].data();
  capture_state_allocation_probe::watchedFreed = false;
  check(partial.renderTapeCapture.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
            partial.renderTapeCapture.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            define(partial.renderTapeCapture, first) == RenderTapeCaptureStatus::Accepted &&
            partial.renderTapeCapture.resourceMutationOwnedBytes(first, RenderTapeMutationKind::Upload,
                0u, 0u, std::move(*aTransfer.payload(0u)),
                RenderTapeBufferMutationDisposition::Plain, *aTransfer.retainedLease(0u)) ==
                RenderTapeCaptureStatus::Accepted &&
            define(partial.renderTapeCapture, second) == RenderTapeCaptureStatus::Accepted &&
            partial.renderTapeCapture.resourceMutationOwnedBytes(second, RenderTapeMutationKind::Upload,
                0u, 0u, std::move(*bTransfer.payload(0u)),
                RenderTapeBufferMutationDisposition::Plain, *bTransfer.retainedLease(0u)) ==
                RenderTapeCaptureStatus::CapacityExceeded &&
            untouched.content[0].size() == descriptor.size && !capture_state_allocation_probe::watchedFreed,
        "partial JIT transfer aborts the session without consuming unrelated arm payloads");
  a.retainedLeases[0].reset();
  b.retainedLeases[0].reset();
  // PE abortRenderTapeCapture clears all snapshot owners at the same terminal
  // boundary; its next-arm snapshot pass recreates them from authoritative data.
  partial.renderTapeCapture.abort();
  partial.renderTapeArmSnapshots.clear();
  check(capture_state_allocation_probe::watchedFreed &&
            partial.renderTapeCapture.ownedBlobBytes() == 0u &&
            partial.renderTapeCapture.publicationBundle().events.empty(),
        "terminal PE cleanup releases unselected as well as already transferred snapshot storage");
  capture_state_allocation_probe::watchedPointer = nullptr;
}

void testOwnedMutationFaultsAndBorrowedEquivalence() {
  const auto bootstrap = bootstrapChunk();
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 3u, .objectId = 501u};
  constexpr std::array<std::byte, 7u> original{
      std::byte{1u}, std::byte{2u}, std::byte{3u}, std::byte{4u},
      std::byte{5u}, std::byte{6u}, std::byte{7u}};
  const D9CBufferDesc descriptor{.size = original.size()};
  const auto setup = [&](RenderTapeCaptureSession& session) {
    check(session.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
              session.objectDefine(identity,
                  static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
                  std::as_bytes(std::span(&descriptor, 1u)), 0u, {}, original.size(), 1u) ==
                  RenderTapeCaptureStatus::Accepted,
          "owned mutation fixture defines validated complete buffer storage");
  };
  bool reachedSuccess = false;
  bool sawCatalogue = false;
  bool sawPublication = false;
  bool sawEvent = false;
  for (std::size_t allocation = 0u; allocation < 32u; ++allocation) {
    RenderTapeCaptureSession session(true);
    setup(session);
    std::vector<std::byte> bytes(original.begin(), original.end());
    capture_state_allocation_probe::failAfter = allocation;
    capture_state_allocation_probe::failEnabled = true;
    const auto status = session.resourceMutationOwnedBytes(
        identity, RenderTapeMutationKind::Upload, 0u, 0u, std::move(bytes));
    capture_state_allocation_probe::failEnabled = false;
    check(bytes.empty(), "owned mutation consumes at entry even when metadata admission throws");
    if (status == RenderTapeCaptureStatus::Accepted) {
      reachedSuccess = true;
      check(session.ownedBlobBytes() == original.size() && session.eventCount() == 3u,
            "successful owned wrapper preserves original nonzero mutation size");
      break;
    }
    const auto failure = session.mutationFailure();
    const std::string_view phase(failure.phase);
    check(status == RenderTapeCaptureStatus::CapacityExceeded &&
              failure.valid && failure.hasIdentity && failure.identity.objectId == identity.objectId &&
              failure.byteSize == original.size() &&
              std::string_view(failure.exceptionKind) == "bad_alloc" &&
              phase != "blob_payload_copy" && phase != "blob_payload_move" &&
              phase != "blob_digest" && session.state() == RenderTapeCaptureState::Aborted &&
              session.ownedBlobEntries() == 0u && session.eventCount() == 0u &&
              session.publicationBundle().events.empty() && session.sealedArtifact().empty(),
          "owned fault sweep attributes metadata/event failures and fail-stops without payload copy or publication");
    sawCatalogue |= phase == "blob_catalogue_append";
    sawPublication |= phase == "blob_publication_append";
    sawEvent |= phase == "event_append";
  }
  check(reachedSuccess && sawCatalogue && sawPublication && sawEvent,
        "owned fault sweep exercises both metadata layers and event allocation");

  for (const unsigned rejection : {0u, 1u, 2u}) {
    RenderTapeCaptureSession session(true,
        {.maxBlobEntries = rejection == 0u ? 0u : 4096u,
         .maxBlobBytes = rejection == 1u ? 6u : kRenderTapeDefaultMaxBlobBytes});
    setup(session);
    auto requestedIdentity = identity;
    if (rejection == 2u) ++requestedIdentity.objectId;
    std::vector<std::byte> bytes(original.begin(), original.end());
    const auto status = session.resourceMutationOwnedBytes(requestedIdentity,
        RenderTapeMutationKind::Upload, 0u, 0u, std::move(bytes));
    check(status == (rejection == 2u ? RenderTapeCaptureStatus::InvalidInput
                                   : RenderTapeCaptureStatus::CapacityExceeded) &&
              bytes.empty() && session.state() == RenderTapeCaptureState::Aborted &&
              session.mutationFailure().byteSize == original.size() &&
              std::string_view(session.mutationFailure().exceptionKind) == "none",
          "owned explicit limits and event validation rejection retain original extent and abort");
  }
  RenderTapeCaptureSession borrowed(true);
  RenderTapeCaptureSession owned(true);
  setup(borrowed);
  setup(owned);
  std::vector<std::byte> durable(original.begin(), original.end());
  const auto* durablePointer = durable.data();
  auto transferred = durable;
  check(borrowed.resourceMutationBytes(identity, RenderTapeMutationKind::Upload,
            0u, 0u, durable) == RenderTapeCaptureStatus::Accepted &&
            owned.resourceMutationOwnedBytes(identity, RenderTapeMutationKind::Upload,
                0u, 0u, std::move(transferred)) == RenderTapeCaptureStatus::Accepted &&
            transferred.empty() && durable.data() == durablePointer &&
            std::equal(durable.begin(), durable.end(), original.begin(), original.end()),
        "durable borrowed fallback remains byte-identical and is never consumed");
  const auto finish = [&](RenderTapeCaptureSession& session) {
    const auto output = outputSurfaceDescriptor();
    const auto present = presentChunk();
    const auto attachment = oracle();
    check(session.objectDefine(kSurface,
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
              std::as_bytes(std::span(&output, 1u)), 0u, {}) == RenderTapeCaptureStatus::Accepted &&
              session.commandChunk(CommandChunkEnvelope{.recordCount = 1u}, present) ==
                  RenderTapeCaptureStatus::Accepted &&
              session.completePresent(session.eventCount(), 1u,
                  RenderTapeDigestValidity::NotCaptured, {},
                  std::as_bytes(std::span(&attachment, 1u))) == RenderTapeCaptureStatus::Complete,
          "both mutation ownership paths seal the complete canonical reference frame");
  };
  finish(borrowed);
  finish(owned);
  check(borrowed.publicationBundle().events == owned.publicationBundle().events &&
            borrowed.publicationBundle().blobs.size() == 1u &&
            owned.publicationBundle().blobs.size() == 1u &&
            owned.publicationBundle().blobs[0].bytes == durable &&
            owned.publicationBundle().blobs[0].digest == RenderTapeCaptureSession::sha256(durable),
        "owned mutation independently rehashes original bytes and preserves borrowed events/publication");
}

namespace scratch_io_fault {

enum class Operation { None, Open, Seek, Read, ShortRead, Write, ShortWrite, Flush };
Operation operation = Operation::None;
std::size_t after = 0u;
std::size_t opens = 0u;
std::size_t closes = 0u;
bool fail(Operation at) {
  if (operation != at) return false;
  if (after != 0u) { --after; return false; }
  operation = Operation::None;
  return true;
}
std::FILE* open() {
  if (fail(Operation::Open)) return nullptr;
  auto* file = RenderTapeScratchIo::openDefault();
  if (file) ++opens;
  return file;
}
bool seek(std::FILE* file, std::uint64_t offset) {
  return !fail(Operation::Seek) && RenderTapeScratchIo::seekDefault(file, offset);
}
std::size_t read(std::FILE* file, void* bytes, std::size_t size) {
  if (fail(Operation::Read)) return 0u;
  return RenderTapeScratchIo::readDefault(file, bytes,
      fail(Operation::ShortRead) && size != 0u ? size - 1u : size);
}
std::size_t write(std::FILE* file, const void* bytes, std::size_t size) {
  if (fail(Operation::Write)) return 0u;
  return RenderTapeScratchIo::writeDefault(file, bytes,
      fail(Operation::ShortWrite) && size != 0u ? size - 1u : size);
}
bool flush(std::FILE* file) {
  return !fail(Operation::Flush) && RenderTapeScratchIo::flushDefault(file);
}
void close(std::FILE* file) { ++closes; RenderTapeScratchIo::closeDefault(file); }
RenderTapeScratchIo io() { return {open, seek, read, write, flush, close}; }

} // namespace scratch_io_fault

void testScratchPayloadIoAndRetirementRefinement() {
  using Operation = scratch_io_fault::Operation;
  constexpr std::size_t extent = 3u * kRenderTapePayloadWindowBytes + 17u;
  std::vector<std::byte> original(extent);
  for (std::size_t i = 0u; i < extent; ++i) original[i] = static_cast<std::byte>((i * 17u) & 0xffu);
  const std::array<std::byte, 3u> patch{std::byte{0x77u}, std::byte{0x88u}, std::byte{0x99u}};
  const RenderTapeShadowWritePlan full{.expectedBytes = extent, .rowBytes = extent,
                                      .fullOverwrite = true};
  const RenderTapeShadowWritePlan partial{.expectedBytes = extent,
      .destinationOffset = kRenderTapePayloadWindowBytes - 1u,
      .rowBytes = patch.size()};
  scratch_io_fault::opens = scratch_io_fault::closes = 0u;
  {
    auto arena = std::make_shared<RenderTapeScratchArena>(scratch_io_fault::io());
    RenderTapeCpuShadowContent shadow;
    shadow.setPayloadArena(arena);
    scratch_io_fault::operation = Operation::Open;
    check(shadow.write(full, original) == RenderTapeShadowWriteStatus::IoFailed &&
              shadow.empty() && arena->liveBytes() == 0u && arena->highWaterBytes() == 0u,
          "scratch open rejection cannot manufacture initial CPU authority or reserve an extent");
    check(shadow.write(full, original) == RenderTapeShadowWriteStatus::Accepted &&
              shadow.capacity() == 0u && shadow.readBytes().data() == nullptr &&
              arena->liveBytes() == extent && arena->highWaterBytes() == extent,
          "complete CPU A resides in one unmapped scratch extent, not a retained PE vector/cache");
    auto pinnedA = shadow.freeze();
    const auto* identityA = pinnedA.backingIdentity();
    const auto digestA = RenderTapeCaptureSession::sha256(pinnedA.readBytes());
    check(digestA == RenderTapeCaptureSession::sha256(original),
          "windowed hash agrees with original bytes across full-window and SHA padding boundaries");
    std::array<std::byte, 3u> across{};
    check(pinnedA.readAt(kRenderTapePayloadWindowBytes - 1u, across) &&
              across[0] == original[kRenderTapePayloadWindowBytes - 1u] &&
              !pinnedA.readAt(extent - 1u, across) &&
              !pinnedA.readAt(std::numeric_limits<std::uint64_t>::max(), across),
          "bounded scratch reads preserve exact offset bytes and reject wrapped/end-overrun windows");

    for (const auto operation : {Operation::Seek, Operation::Read, Operation::ShortRead,
                                Operation::Write, Operation::ShortWrite, Operation::Flush}) {
      scratch_io_fault::operation = operation;
      scratch_io_fault::after = operation == Operation::Write || operation == Operation::ShortWrite
          ? 4u : operation == Operation::Flush ? 1u : 0u;
      check(shadow.write(partial, patch) == RenderTapeShadowWriteStatus::IoFailed &&
                shadow.backingIdentity() == identityA && arena->liveBytes() == extent,
            "failed clone/patch/flush retires only staging and preserves exact committed owner and live accounting");
      scratch_io_fault::operation = Operation::None;
      check(pinnedA == original && shadow.readBytes().equals(pinnedA.readBytes()),
            "short writes and flush failures leave authenticated old CPU A unchanged");
      check(shadow.write(partial, patch) == RenderTapeShadowWriteStatus::Accepted,
            "an I/O-failed staging extent can be safely reused by the next complete transaction");
      auto expected = original;
      std::copy(patch.begin(), patch.end(), expected.begin() + partial.destinationOffset);
      check(shadow.freeze() == expected && pinnedA == original,
            "retry cannot flush stale pending bytes into either the reused extent or pinned old A");
      shadow = RenderTapeCpuShadowContent(pinnedA);
      shadow.setPayloadArena(arena);
    }

    for (const auto operation : {Operation::Read, Operation::ShortRead}) {
      scratch_io_fault::operation = operation;
      scratch_io_fault::after = 0u;
      bool rejected = false;
      try { (void)RenderTapeCaptureSession::sha256(pinnedA.readBytes()); }
      catch (const RenderTapePayloadReadError&) { rejected = true; }
      check(rejected, "failed byte reads cannot mint a trusted digest");
      RenderTapeCaptureSession session(true);
      check(session.arm(bootstrapChunk()) == RenderTapeCaptureStatus::Accepted,
            "scratch fault admission fixture arms the canonical stream");
      scratch_io_fault::operation = operation;
      check(session.registerSharedBlobBytes(pinnedA) == RenderTapeCaptureStatus::CapacityExceeded &&
                session.ownedBlobBytes() == 0u && session.ownedBlobEntries() == 0u &&
                std::string_view(session.mutationFailure().exceptionKind) == "scratch_io",
            "original-byte admission read failures preserve empty catalogue and typed I/O provenance");
    }
    scratch_io_fault::operation = Operation::None;

    auto iterator = pinnedA.begin();
    scratch_io_fault::operation = Operation::ShortRead;
    scratch_io_fault::after = 0u;
    bool iteratorRejected = false;
    try { (void)*iterator; } catch (const RenderTapePayloadReadError&) { iteratorRejected = true; }
    check(iteratorRejected && *iterator == original[0],
          "retrying the same iterator rereads after short I/O rather than exposing a partially valid cache");

    const auto boundedHighWater = arena->highWaterBytes();
    for (unsigned version = 0u; version < 32u; ++version) {
      check(shadow.write(partial, patch) == RenderTapeShadowWriteStatus::Accepted && pinnedA == original,
            "live immutable aliases prevent their extent from being reused by later CPU mutations");
    }
    check(arena->highWaterBytes() <= std::max<std::uint64_t>(boundedHighWater, 3u * extent) &&
              arena->liveBytes() == 2u * extent && scratch_io_fault::opens == 1u,
          "retired same-size intervals are reused, bounding high-water despite repeated version writes");
    shadow = {};
    check(arena->liveBytes() == extent && pinnedA == original,
          "retiring the mutable registry leaves its captured immutable lease authoritative");
    pinnedA = {};
    check(arena->liveBytes() == 0u,
          "the final lease retires its extent without treating reusable high-water as live payload");
  }
  check(scratch_io_fault::opens == 1u && scratch_io_fault::closes == 1u,
        "scope teardown closes the sole anonymous scratch file after the final owner retires");
}

void testScratchPayloadBoundedOwnershipTraces() {
  // Enumerate four-step histories over two CPU owners, captured leases,
  // overwrites, patches, abort-like lease release, and failed staging. Serial
  // vectors are the byte oracle; all ownership/reuse transitions execute the
  // production arena/shadow implementation rather than a duplicate allocator.
  constexpr unsigned actions = 7u;
  constexpr unsigned traces = actions * actions * actions * actions;
  const std::array<std::byte, 8u> a{std::byte{0x11u}, std::byte{0x12u}, std::byte{0x13u}, std::byte{0x14u},
      std::byte{0x15u}, std::byte{0x16u}, std::byte{0x17u}, std::byte{0x18u}};
  const std::array<std::byte, 8u> b{std::byte{0x31u}, std::byte{0x32u}, std::byte{0x33u}, std::byte{0x34u},
      std::byte{0x35u}, std::byte{0x36u}, std::byte{0x37u}, std::byte{0x38u}};
  const std::array<std::byte, 1u> patch{std::byte{0x99u}};
  for (unsigned trace = 0u; trace < traces; ++trace) {
    auto arena = std::make_shared<RenderTapeScratchArena>(scratch_io_fault::io());
    std::array<RenderTapeCpuShadowContent, 2u> current;
    std::array<RenderTapeImmutablePayload, 2u> retained;
    std::array<std::vector<std::byte>, 2u> serial, captured;
    for (auto& shadow : current) shadow.setPayloadArena(arena);
    auto remaining = trace;
    for (unsigned step = 0u; step < 4u; ++step) {
      const auto action = remaining % actions;
      remaining /= actions;
      if (action == 0u || action == 3u) {
        const unsigned resource = action == 0u ? 0u : 1u;
        const auto bytes = resource == 0u ? std::span<const std::byte>(a) : std::span<const std::byte>(b);
        check(applyRenderTapeBufferMutation(8u, 0u, bytes, current[resource]) ==
                  applyRenderTapeBufferMutation(8u, 0u, bytes, serial[resource]),
              "bounded full-write transition matches serial content authority");
      } else if (action == 1u) {
        check(applyRenderTapeBufferMutation(8u, 2u, patch, current[0]) ==
                  applyRenderTapeBufferMutation(8u, 2u, patch, serial[0]),
              "bounded partial-write transition uses CPU base including incomplete-seed rejection");
      } else if (action == 2u || action == 4u) {
        const unsigned resource = action == 2u ? 0u : 1u;
        if (!serial[resource].empty()) {
          retained[resource] = current[resource].freeze();
          captured[resource] = serial[resource];
        }
      } else if (action == 5u) {
        retained = {};
        captured = {};
      } else {
        scratch_io_fault::operation = scratch_io_fault::Operation::ShortWrite;
        scratch_io_fault::after = 0u;
        check(applyRenderTapeBufferMutation(8u, 0u, a, current[0]) == RenderTapeBlockMutationStatus::IoFailed,
              "bounded failed staging never commits a partially written version");
      }
      std::array<const void*, 4u> identities{};
      unsigned unique = 0u;
      const auto countOwner = [&](const void* identity) {
        if (!identity || std::find(identities.begin(), identities.begin() + unique, identity) !=
                            identities.begin() + unique) return;
        identities[unique++] = identity;
      };
      for (unsigned resource = 0u; resource < 2u; ++resource) {
        check(current[resource].readBytes().equals(RenderTapeByteView(std::span<const std::byte>(serial[resource]))) &&
                  retained[resource] == captured[resource] && current[resource].capacity() == 0u,
              "every bounded prefix preserves current bytes, immutable leases, and zero retained CPU heap capacity");
        countOwner(current[resource].backingIdentity());
        countOwner(retained[resource].backingIdentity());
      }
      check(arena->liveBytes() == unique * 8u && arena->highWaterBytes() <= (4u + 1u) * 8u,
            "production extent accounting matches the exact live ownership set at every bounded prefix");
    }
  }
}

void testCpuShadowVersionTransitions() {
  static_assert(std::is_same_v<decltype(std::declval<RenderTapeImmutablePayload&>().data()),
                               const std::byte*>);
  static_assert(std::is_same_v<decltype(
      *std::declval<RenderTapeImmutablePayload&>().begin()), std::byte>);
  const std::vector<std::byte> original(32u, std::byte{0x11u});
  const std::array<std::byte, 3u> patch{std::byte{0x21u}, std::byte{0x22u}, std::byte{0x23u}};
  for (unsigned state = 0u; state < 4u; ++state) {
    for (const bool full : {false, true}) {
      RenderTapeCpuShadowContent shadow(state == 0u ? std::vector<std::byte>{}
                                                    : std::vector<std::byte>(original));
      RenderTapeImmutablePayload external;
      if (state >= 2u) {
        external = shadow.freeze();
        if (state == 2u) external = {};
      }
      const auto* pointer = shadow.backingIdentity();
      const auto incoming = full ? std::span<const std::byte>(original)
                                 : std::span<const std::byte>(patch);
      auto expected = state == 0u ? std::vector<std::byte>{} : original;
      const auto reference = applyRenderTapeBufferMutation(32u, full ? 0u : 5u, incoming, expected);
      const auto status = applyRenderTapeBufferMutation(32u, full ? 0u : 5u, incoming, shadow);
      check(status == reference &&
                std::equal(shadow.readBytes().begin(), shadow.readBytes().end(), expected.begin(), expected.end()) &&
                (external.empty() || external == original) && shadow.capacity() == 0u,
            "unknown/private/frozen-unique/shared transitions match serial vector semantics and preserve leases");
      if (state == 3u && status == RenderTapeBlockMutationStatus::Accepted)
        check(shadow.backingIdentity() != pointer,
              "shared full and partial writes establish a distinct committed version");
    }
  }
  RenderTapeCpuShadowContent unknown;
  bool rejectedUnknown = false;
  try { (void)unknown.freeze(); } catch (const std::invalid_argument&) { rejectedUnknown = true; }
  check(rejectedUnknown && unknown.empty() &&
            applyRenderTapeBufferMutation(32u, 1u, patch, unknown) ==
                RenderTapeBlockMutationStatus::IncompleteSeed && unknown.empty(),
        "unknown freeze/partial write cannot manufacture complete content authority");

  RenderTapeCpuShadowContent freezeFault{std::vector<std::byte>(original)};
  const auto* privatePointer = freezeFault.backingIdentity();
  const auto privateCapacity = freezeFault.capacity();
  capture_state_allocation_probe::failAfter = 0u;
  capture_state_allocation_probe::failEnabled = true;
  bool failedFreeze = false;
  try { (void)freezeFault.freeze(); } catch (const std::bad_alloc&) { failedFreeze = true; }
  capture_state_allocation_probe::failEnabled = false;
  check(!failedFreeze && freezeFault.backingIdentity() == privatePointer &&
            freezeFault.capacity() == privateCapacity &&
            std::equal(freezeFault.readBytes().begin(), freezeFault.readBytes().end(), original.begin()),
        "scratch freeze shares the committed owner without allocation or resident payload capacity");
  auto oldLease = freezeFault.freeze();
  capture_state_allocation_probe::failAfter = 0u;
  capture_state_allocation_probe::failEnabled = true;
  const auto partialFault = applyRenderTapeBufferMutation(32u, 5u, patch, freezeFault);
  capture_state_allocation_probe::failEnabled = false;
  check(partialFault == RenderTapeBlockMutationStatus::AllocationFailed &&
            oldLease == original && freezeFault.backingIdentity() == privatePointer &&
            freezeFault.size() == original.size(),
        "shared partial-detach failure preserves both committed shadow and captured version");
  capture_state_allocation_probe::failAfter = 0u;
  capture_state_allocation_probe::failEnabled = true;
  const auto invalid = applyRenderTapeBufferMutation(32u, 31u, patch, freezeFault);
  const bool validationAllocated = !capture_state_allocation_probe::failEnabled;
  capture_state_allocation_probe::failEnabled = false;
  check(invalid == RenderTapeBlockMutationStatus::InvalidLayout && !validationAllocated && oldLease == original,
        "all bounds validation precedes any transactional detach allocation");
  oldLease = {};
  capture_state_allocation_probe::count = 0u;
  capture_state_allocation_probe::enabled = true;
  const auto reclaimed = applyRenderTapeBufferMutation(32u, 5u, patch, freezeFault);
  capture_state_allocation_probe::enabled = false;
  check(reclaimed == RenderTapeBlockMutationStatus::Accepted &&
            freezeFault.backingIdentity() != privatePointer && freezeFault.capacity() == 0u,
        "even unique scratch writes commit a separately staged extent so I/O failure can roll back");
  RenderTapeImmutablePayload retiredLease;
  {
    RenderTapeCpuShadowContent retired{std::vector<std::byte>(original)};
    retiredLease = retired.freeze();
  }
  check(retiredLease == original,
        "retiring the durable owner cannot release a still-retained immutable capture version");
  std::vector<std::byte> nonuniform(32u);
  for (std::size_t i = 0u; i < nonuniform.size(); ++i) nonuniform[i] = static_cast<std::byte>(i);
  RenderTapeCpuShadowContent active{std::move(nonuniform)};
  capture_state_allocation_probe::freezeDuringAllocation = &active;
  capture_state_allocation_probe::activeFreezeRejected = false;
  capture_state_allocation_probe::activeFreezeAccepted = false;
  std::array<std::byte, 3u> overlappingInput{};
  check(active.readBytes().readAt(0u, overlappingInput), "caller explicitly reads a bounded scratch window");
  check(applyRenderTapeBufferMutation(32u, 5u, overlappingInput, active) ==
            RenderTapeBlockMutationStatus::Accepted &&
            capture_state_allocation_probe::activeFreezeRejected &&
            !capture_state_allocation_probe::activeFreezeAccepted &&
            active.readBytes()[5] == std::byte{0u} && active.freeze().size() == 32u,
        "freeze rejects even allocation-hook reentrancy during an explicit writer and resumes only after commit");
}

void testCpuShadowWriterFamilyRefinement() {
  const auto exercise = [&](std::size_t extent, std::size_t patchBytes,
                            auto applyFull, auto applyPartial) {
    const std::vector<std::byte> old(extent, std::byte{0x11u});
    const std::vector<std::byte> incoming(extent, std::byte{0x66u});
    const std::vector<std::byte> patch(patchBytes, std::byte{0x22u});
    for (const bool full : {false, true}) {
      auto reference = old;
      RenderTapeCpuShadowContent shadow{std::vector<std::byte>(old)};
      const auto lease = shadow.freeze();
      const auto status = full ? applyFull(incoming, shadow) : applyPartial(patch, shadow);
      const auto referenceStatus = full ? applyFull(incoming, reference) : applyPartial(patch, reference);
      check(status == referenceStatus && status == RenderTapeBlockMutationStatus::Accepted &&
                lease == old &&
                std::equal(shadow.readBytes().begin(), shadow.readBytes().end(), reference.begin(), reference.end()),
            "all durable writer families refine existing full/partial vector semantics without mutating old leases");
      RenderTapeCpuShadowContent failed{std::vector<std::byte>(old)};
      const auto failedLease = failed.freeze();
      const auto* pointer = failed.backingIdentity();
      capture_state_allocation_probe::failAfter = 0u;
      capture_state_allocation_probe::failEnabled = true;
      const auto failedStatus = full ? applyFull(incoming, failed) : applyPartial(patch, failed);
      capture_state_allocation_probe::failEnabled = false;
      check(failedStatus == RenderTapeBlockMutationStatus::AllocationFailed &&
                failedLease == old && failed.backingIdentity() == pointer && failed.size() == extent,
            "every shared writer family's allocation fault preserves bytes and completeness atomically");
    }
  };
  exercise(32u, 3u,
      [](std::span<const std::byte> bytes, auto& content) { return applyRenderTapeBufferMutation(32u, 0u, bytes, content); },
      [](std::span<const std::byte> bytes, auto& content) { return applyRenderTapeBufferMutation(32u, 5u, bytes, content); });
  D9CSurfaceDesc block{.format = render_tape_d3d_format::DXT1, .width = 8u, .height = 8u};
  const RenderTapeLockRect blockRect{.left = 4, .top = 4, .right = 8, .bottom = 8};
  RenderTapeBlockLockLayout blockFull{}, blockPartial{};
  check(renderTapeBlockLockLayout(block, 16, nullptr, blockFull) == RenderTapeBlockLayoutStatus::Accepted &&
            renderTapeBlockLockLayout(block, 16, &blockRect, blockPartial) == RenderTapeBlockLayoutStatus::Accepted,
        "block COW fixture derives its production row layouts");
  exercise(32u, 8u,
      [&](std::span<const std::byte> bytes, auto& content) { return applyRenderTapeBlockMutation(blockFull, bytes, content); },
      [&](std::span<const std::byte> bytes, auto& content) { return applyRenderTapeBlockMutation(blockPartial, bytes, content); });
  D9CSurfaceDesc linear{.format = render_tape_d3d_format::A8R8G8B8, .width = 4u, .height = 3u};
  const RenderTapeLockRect linearRect{.left = 1, .top = 1, .right = 3, .bottom = 3};
  RenderTapeLinearLockLayout linearFull{}, linearPartial{};
  check(renderTapeLinearLockLayout(linear, 16, nullptr, linearFull) == RenderTapeLinearLayoutStatus::Accepted &&
            renderTapeLinearLockLayout(linear, 16, &linearRect, linearPartial) == RenderTapeLinearLayoutStatus::Accepted,
        "linear COW fixture derives production rectangle layouts");
  exercise(48u, 16u,
      [&](std::span<const std::byte> bytes, auto& content) { return applyRenderTapeLinearMutation(linearFull, bytes, content); },
      [&](std::span<const std::byte> bytes, auto& content) { return applyRenderTapeLinearMutation(linearPartial, bytes, content); });
  D9CSurfaceDesc volume{.format = render_tape_d3d_format::A8R8G8B8,
      .resourceType = 4u, .width = 2u, .height = 2u, .depth = 2u};
  const RenderTapeLockBox volumeBox{.left = 1, .top = 0,
      .right = 2, .bottom = 2, .front = 1, .back = 2};
  RenderTapeVolumeLockLayout volumeFull{}, volumePartial{};
  check(renderTapeVolumeLockLayout(volume, 8, 16, nullptr, volumeFull) == RenderTapeVolumeLayoutStatus::Accepted &&
            renderTapeVolumeLockLayout(volume, 8, 16, &volumeBox, volumePartial) == RenderTapeVolumeLayoutStatus::Accepted,
        "volume COW fixture derives production box layouts");
  exercise(32u, 8u,
      [&](std::span<const std::byte> bytes, auto& content) { return applyRenderTapeVolumeMutation(volumeFull, bytes, content); },
      [&](std::span<const std::byte> bytes, auto& content) { return applyRenderTapeVolumeMutation(volumePartial, bytes, content); });
}

void testSharedCaptureVersionPublicationAndLedger() {
  constexpr std::size_t size = 8197u;
  const auto bootstrap = bootstrapChunk();
  const D9CBufferDesc desc{.size = size};
  const D9CWireObjectIdentity a{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 3u, .objectId = 701u};
  auto b = a;
  ++b.objectId;
  PeCaptureState pe({}, kRenderTapeProfileFrame, 0u);
  pe.renderTapeRegistry.objects.resize(2u);
  pe.renderTapeRegistry.objects[0].identity = a;
  pe.renderTapeRegistry.objects[1].identity = b;
  for (auto& object : pe.renderTapeRegistry.objects) {
    object.contentCount = 1u;
    object.descriptor = bytesOf(desc);
  }
  auto& current = pe.renderTapeRegistry.objects[0].content;
  current.emplace_back(std::vector<std::byte>(size, std::byte{0x11u}));
  const auto* originalPointer = current[0].backingIdentity();
  capture_state_allocation_probe::watchSize = size;
  capture_state_allocation_probe::watchedAllocations = 0u;
  auto original = current[0].freeze();
  pe.renderTapeRegistry.objects[1].content.emplace_back(original);
  auto& session = pe.renderTapeCapture;
  check(session.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
            session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
        "shared capture fixture starts the bounded interval");
  for (auto& object : pe.renderTapeRegistry.objects) {
    check(session.objectDefine(object.identity,
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer), object.descriptor,
              0u, {}, size, 1u) == RenderTapeCaptureStatus::Accepted &&
              session.resourceMutationShadowBytes(object.identity, RenderTapeMutationKind::Upload,
                  0u, 0u, object.content[0]) == RenderTapeCaptureStatus::Accepted,
          "distinct identities admit independent shared-backed Upload events");
  }
  check(capture_state_allocation_probe::watchedAllocations == 0u &&
            session.ownedBlobEntries() == 1u && session.ownedBlobBytes() == size &&
            session.ownedPublishedBlobs()[0].bytes.backingIdentity() == originalPointer,
        "freeze and shared admission retain the original pointer with zero payload-sized allocations and exact dedup");
  const std::array<std::byte, 3u> patch{std::byte{0x21u}, std::byte{0x22u}, std::byte{0x23u}};
  check(applyRenderTapeBufferMutation(size, 5u, patch, current[0]) == RenderTapeBlockMutationStatus::Accepted &&
            capture_state_allocation_probe::watchedAllocations == 0u &&
            current[0].backingIdentity() != originalPointer && original.backingIdentity() == originalPointer &&
            original.readBytes()[5] == std::byte{0x11u} &&
            pe.renderTapeRegistry.objects[1].content[0].readBytes()[5] == std::byte{0x11u} &&
            current[0].readBytes()[5] == patch[0] &&
            session.resourceMutationShadowBytes(a, RenderTapeMutationKind::CpuUnlock,
                0u, 0u, current[0]) == RenderTapeCaptureStatus::Accepted,
        "A-to-B partial mutation detaches exactly once, keeps original leases immutable, and journals B separately");
  const auto* newPointer = current[0].backingIdentity();
  const auto& beforeSeal = measureRenderTapePayloadBoundary(pe);
  check(beforeSeal.uniqueComplete && beforeSeal.backingCount == 2u &&
            beforeSeal.uniqueBytes == 2u * size && beforeSeal.uniqueCapacity == 0u &&
            beforeSeal.uniqueSharedBytes == 2u * size && beforeSeal.logicalBytes == 4u * size,
        "boundary ledger joins registry/session leases by exact backing rather than summing refcounts");
  const auto output = outputSurfaceDescriptor();
  const auto present = presentChunk();
  const auto attachment = oracle();
  capture_state_allocation_probe::watchedAllocations = 0u;
  check(session.objectDefine(kSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&output, 1u)), 0u, {}) == RenderTapeCaptureStatus::Accepted &&
            session.commandChunk(CommandChunkEnvelope{.recordCount = 1u}, present) == RenderTapeCaptureStatus::Accepted &&
            session.completePresent(session.eventCount(), 1u, RenderTapeDigestValidity::NotCaptured, {},
                std::as_bytes(std::span(&attachment, 1u))) == RenderTapeCaptureStatus::Complete &&
            capture_state_allocation_probe::watchedAllocations == 0u &&
            session.publicationBundle().blobs[0].bytes.backingIdentity() == originalPointer &&
            session.publicationBundle().blobs[1].bytes.backingIdentity() == newPointer &&
            session.publicationBundle().blobs[0].digest == RenderTapeCaptureSession::sha256(original.readBytes()),
        "final publication shares immutable A/B versions without any payload copy and preserves original digests");
  capture_state_allocation_probe::watchSize = 0u;
  original = {};
  session.abort();
  check(current[0].backingIdentity() == newPointer &&
            pe.renderTapeRegistry.objects[1].content[0].backingIdentity() == originalPointer &&
            pe.renderTapeRegistry.objects[0].contentCount == 1u &&
            pe.renderTapeRegistry.objects[1].contentCount == 1u,
        "session abort releases capture leases without touching durable bytes or completeness metadata");
  capture_state_allocation_probe::count = 0u;
  capture_state_allocation_probe::enabled = true;
  const auto reused = applyRenderTapeBufferMutation(size, 7u, patch, current[0]);
  capture_state_allocation_probe::enabled = false;
  check(reused == RenderTapeBlockMutationStatus::Accepted && current[0].backingIdentity() != newPointer &&
            current[0].capacity() == 0u,
        "after abort scratch staging keeps durable bytes off the resident PE heap");

  RenderTapeCaptureSession borrowed(true);
  check(borrowed.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
            borrowed.registerBlobBytes(current[0].readBytes()) == RenderTapeCaptureStatus::Accepted &&
            borrowed.ownedPublishedBlobs()[0].bytes.backingIdentity() != current[0].backingIdentity(),
        "borrowed APIs still copy bytes before returning rather than borrowing durable storage");
  RenderTapeCaptureBlob ambiguous{std::vector<std::byte>(2u, std::byte{1u})};
  ambiguous.immutable = current[0].freeze();
  RenderTapeCaptureSession rejectAmbiguous(true);
  check(rejectAmbiguous.armWithBlobs(bootstrap, std::span(&ambiguous, 1u)) ==
            RenderTapeCaptureStatus::InvalidInput &&
            std::string_view(rejectAmbiguous.armFailure().reason) == "ambiguous_blob_storage",
        "bootstrap admission rejects simultaneously active raw and immutable payload representations");

  RenderTapePayloadLedger ledger;
  capture_state_allocation_probe::count = 0u;
  capture_state_allocation_probe::enabled = true;
  for (std::size_t i = 0u; i < ledger.kMaxBackings + 2u; ++i)
    ledger.observe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(i + 1u)), 3u, 5u, false);
  capture_state_allocation_probe::enabled = false;
  check(!ledger.uniqueComplete && ledger.backingCount == ledger.kMaxBackings &&
            ledger.logicalBytes == 3u * (ledger.kMaxBackings + 2u) &&
            ledger.uniqueBytes == 3u * ledger.kMaxBackings && capture_state_allocation_probe::count == 0u,
        "diagnostic overflow stays allocation-free and explicitly reports incomplete unique lower bounds");
}

void testSharedUpdateTextureAtomicCommit() {
  auto sourceDescriptor = versionedTextureDescriptor(RenderTapeTextureDimension::Texture2D,
      2u, render_tape_d3d_format::A8R8G8B8, 4u, 4u);
  // The generic descriptor fixture repeats the base extent. This atomicity
  // fixture needs a valid mip chain before allocation injection can reach the
  // source-freeze staging path.
  D9CSurfaceDesc mipOne{};
  const auto mipOneOffset = sizeof(RenderTapeTextureDescriptorV2) + sizeof(mipOne);
  std::memcpy(&mipOne, sourceDescriptor.data() + mipOneOffset, sizeof(mipOne));
  mipOne.width = 2u;
  mipOne.height = 2u;
  std::memcpy(sourceDescriptor.data() + mipOneOffset, &mipOne, sizeof(mipOne));
  auto destinationDescriptor = sourceDescriptor;
  const auto setPool = [](std::vector<std::byte>& descriptor, std::uint32_t pool) {
    RenderTapeTextureDescriptorV2 texture{};
    std::memcpy(&texture, descriptor.data(), sizeof(texture));
    for (std::uint32_t i = 0u; i < texture.subresourceCount; ++i) {
      D9CSurfaceDesc desc{};
      const auto offset = sizeof(texture) + i * sizeof(desc);
      std::memcpy(&desc, descriptor.data() + offset, sizeof(desc));
      desc.pool = pool;
      std::memcpy(descriptor.data() + offset, &desc, sizeof(desc));
    }
  };
  setPool(sourceDescriptor, 2u);
  setPool(destinationDescriptor, 0u);
  std::array<std::uint64_t, 2u> sourceExpected{}, destinationExpected{};
  check(renderTapeDeriveExpectedContentContract(D9C_CHUNK_HANDLE_KIND_TEXTURE,
            sourceDescriptor, sourceExpected).status == RenderTapeExpectedContentStatus::Accepted &&
            renderTapeDeriveExpectedContentContract(D9C_CHUNK_HANDLE_KIND_TEXTURE,
                destinationDescriptor, destinationExpected).status == RenderTapeExpectedContentStatus::Accepted &&
            sourceExpected == std::array<std::uint64_t, 2u>{64u, 16u} &&
            destinationExpected == sourceExpected,
        "shared UpdateTexture fault fixture has a valid matching two-mip 64/16-byte contract");
  bool reachedSuccess = false;
  bool sawStagingFault = false;
  for (std::size_t allocation = 0u; allocation < 16u; ++allocation) {
    std::vector<RenderTapeCpuShadowContent> source, destination;
    source.emplace_back(std::vector<std::byte>(64u, std::byte{0x11u}));
    source.emplace_back(std::vector<std::byte>(16u, std::byte{0x22u}));
    destination.emplace_back(std::vector<std::byte>(64u, std::byte{0x33u}));
    destination.emplace_back(std::vector<std::byte>(16u, std::byte{0x44u}));
    const auto old0 = destination[0].freeze();
    const auto old1 = destination[1].freeze();
    const auto* source0 = source[0].backingIdentity();
    const auto* source1 = source[1].backingIdentity();
    capture_state_allocation_probe::failAfter = allocation;
    capture_state_allocation_probe::failEnabled = true;
    const auto status = applyRenderTapeUpdateTextureClosure(
        sourceDescriptor, source, destinationDescriptor, destination);
    capture_state_allocation_probe::failEnabled = false;
    check(source[0].backingIdentity() == source0 && source[1].backingIdentity() == source1 &&
              source[0].readBytes()[0] == std::byte{0x11u} && source[1].readBytes()[0] == std::byte{0x22u} &&
              old0.readBytes()[0] == std::byte{0x33u} && old1.readBytes()[0] == std::byte{0x44u},
          "UpdateTexture staging/freezing never changes source semantics or captured old destination versions");
    if (status == RenderTapeUpdateTextureStatus::Accepted) {
      reachedSuccess = true;
      check(destination[0].backingIdentity() == source0 && destination[1].backingIdentity() == source1,
            "validated UpdateTexture commits immutable source leases with pointer conservation");
      const std::array<std::byte, 1u> patch{std::byte{0x77u}};
      check(applyRenderTapeBufferMutation(64u, 1u, patch, source[0]) == RenderTapeBlockMutationStatus::Accepted &&
                destination[0].readBytes()[1] == std::byte{0x11u} &&
                applyRenderTapeBufferMutation(16u, 1u, patch, destination[1]) == RenderTapeBlockMutationStatus::Accepted &&
                source[1].readBytes()[1] == std::byte{0x22u},
            "subsequent explicit source/destination writes detach without cross-mutating shared versions");
      break;
    }
    sawStagingFault = true;
    check(status == RenderTapeUpdateTextureStatus::AllocationFailed &&
              destination[0].backingIdentity() == old0.backingIdentity() &&
              destination[1].backingIdentity() == old1.backingIdentity() &&
              destination[0].size() == 64u && destination[1].size() == 16u &&
              std::equal(destination[0].readBytes().begin(), destination[0].readBytes().end(),
                         old0.begin(), old0.end()) &&
              std::equal(destination[1].readBytes().begin(), destination[1].readBytes().end(),
                         old1.begin(), old1.end()),
          "every precommit UpdateTexture fault preserves all destination bytes/extent/completeness atomically");
  }
  check(reachedSuccess && sawStagingFault,
        "UpdateTexture fault sweep reaches source-freeze staging and the complete atomic commit");
  std::vector<RenderTapeCpuShadowContent> overlap;
  overlap.emplace_back(std::vector<std::byte>(64u, std::byte{0x11u}));
  overlap.emplace_back(std::vector<std::byte>(16u, std::byte{0x22u}));
  overlap.emplace_back(std::vector<std::byte>(16u, std::byte{0x33u}));
  const auto* first = overlap[0].backingIdentity();
  const auto* second = overlap[1].backingIdentity();
  check(applyRenderTapeUpdateTextureClosure(sourceDescriptor,
            std::span(overlap).first(2u), destinationDescriptor, std::span(overlap).subspan(1u, 2u)) ==
            RenderTapeUpdateTextureStatus::Accepted &&
            overlap[1].backingIdentity() == first && overlap[2].backingIdentity() == second &&
            overlap[2].readBytes()[0] == std::byte{0x22u},
        "overlapping source/destination slot sets consume staged old-version leases, not earlier commits");
}

void testConsumingArmBlobOwnershipAndEquivalence() {
  const auto bootstrap = bootstrapChunk();
  const std::vector<RenderTapeCaptureBlob> borrowed{
      RenderTapeCaptureBlob{std::vector<std::byte>(4u, std::byte{0x31u})},
      RenderTapeCaptureBlob{std::vector<std::byte>(4u, std::byte{0x72u})},
      RenderTapeCaptureBlob{std::vector<std::byte>(4u, std::byte{0x31u})}};
  auto transferred = borrowed;
  RenderTapeCaptureSession copied(true, {.maxBlobEntries = 2u, .maxBlobBytes = 8u});
  RenderTapeCaptureSession consumed(true, {.maxBlobEntries = 2u, .maxBlobBytes = 8u});
  check(copied.armWithBlobs(bootstrap, borrowed) == RenderTapeCaptureStatus::Accepted &&
            consumed.armWithOwnedBlobs(bootstrap, std::move(transferred)) ==
                RenderTapeCaptureStatus::Accepted &&
            transferred.empty() && copied.ownedBlobEntries() == 2u &&
            consumed.ownedBlobEntries() == 2u && copied.ownedBlobBytes() == 8u &&
            consumed.ownedBlobBytes() == 8u && borrowed.size() == 3u &&
            borrowed[0].bytes.size() == 4u,
        "consuming and borrowed APIs preserve exact limits/dedup and borrowed caller storage");
  const auto finish = [&](RenderTapeCaptureSession& session) {
    const auto descriptor = outputSurfaceDescriptor();
    const auto present = presentChunk();
    const auto attachment = oracle();
    check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
          "blob ownership equivalence fixture begins the selected frame");
    check(session.objectDefine(kSurface,
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
              std::as_bytes(std::span(&descriptor, 1u)), 0u, {}) ==
              RenderTapeCaptureStatus::Accepted,
          "blob ownership equivalence fixture defines the output attachment");
    check(session.commandChunk(
              CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present) ==
              RenderTapeCaptureStatus::Accepted,
          "blob ownership equivalence fixture journals the exact Present");
    const auto presentOrdinal = session.eventCount();
    check(presentOrdinal == 3u &&
              session.completePresent(presentOrdinal, 1u,
                  RenderTapeDigestValidity::NotCaptured, {},
                  std::as_bytes(std::span(&attachment, 1u))) == RenderTapeCaptureStatus::Complete,
          "both blob ownership modes produce the same complete canonical frame");
  };
  finish(copied);
  finish(consumed);
  const auto& copiedBundle = copied.publicationBundle();
  const auto& consumedBundle = consumed.publicationBundle();
  check(copiedBundle.events == consumedBundle.events &&
            copiedBundle.blobs.size() == 2u && consumedBundle.blobs.size() == 2u &&
            std::equal(copiedBundle.blobs.begin(), copiedBundle.blobs.end(),
                consumedBundle.blobs.begin(), [](const auto& first, const auto& second) {
                  return first.digest == second.digest && first.bytes == second.bytes;
                }) &&
            consumedBundle.blobs[0].digest ==
                RenderTapeCaptureSession::sha256(borrowed[0].bytes) &&
            consumedBundle.blobs[1].digest ==
                RenderTapeCaptureSession::sha256(borrowed[1].bytes),
        "consuming arm independently rehashes payloads and preserves copying-path events/digests");

  std::vector<RenderTapeCaptureBlob> pointerSeed{
      RenderTapeCaptureBlob{std::vector<std::byte>(8197u, std::byte{0x35u})}};
  const auto* originalPointer = pointerSeed[0].bytes.data();
  capture_state_allocation_probe::watchedPointer = pointerSeed[0].bytes.data();
  capture_state_allocation_probe::watchedFreed = false;
  capture_state_allocation_probe::watchSize = pointerSeed[0].bytes.size();
  RenderTapeCaptureSession pointerSession(true);
  check(pointerSession.armWithOwnedBlobs(bootstrap, std::move(pointerSeed)) ==
            RenderTapeCaptureStatus::Accepted && pointerSeed.empty() &&
            pointerSession.ownedBlobBytes() == 8197u &&
            capture_state_allocation_probe::watchedPointer == originalPointer &&
            !capture_state_allocation_probe::watchedFreed,
        "consuming arm conserves the original payload pointer with no payload-sized allocation");
  capture_state_allocation_probe::watchSize = 0u;
  std::vector<RenderTapeCaptureBlob> invalidRetry{
      RenderTapeCaptureBlob{std::vector<std::byte>(5u, std::byte{0x11u})}};
  check(pointerSession.armWithOwnedBlobs(bootstrap, std::move(invalidRetry)) ==
            RenderTapeCaptureStatus::InvalidState && invalidRetry.empty() &&
            pointerSession.state() == RenderTapeCaptureState::Armed &&
            pointerSession.ownedBlobEntries() == 1u && pointerSession.ownedBlobBytes() == 8197u &&
            !capture_state_allocation_probe::watchedFreed,
        "failed consuming rearm releases transferred input without changing the existing session");
  pointerSession.abort();
  check(capture_state_allocation_probe::watchedFreed,
        "session abort releases the exact payload pointer consumed at arm");
  capture_state_allocation_probe::watchedPointer = nullptr;

  auto overflowing = borrowed;
  RenderTapeCaptureSession budget(true, {.maxBlobBytes = 7u});
  check(budget.armWithOwnedBlobs(bootstrap, std::move(overflowing)) ==
            RenderTapeCaptureStatus::CapacityExceeded && overflowing.empty() &&
            budget.ownedBlobEntries() == 0u && budget.ownedBlobBytes() == 0u &&
            budget.publicationBundle().events.empty() &&
            std::string_view(budget.armFailure().layer) == "arm_with_owned_blobs" &&
            std::string_view(budget.armFailure().reason) == "blob_byte_limit" &&
            budget.armFailure().blobIndex == 1u &&
            budget.armFailure().blobBytes == 4u && budget.armFailure().cumulativeBlobBytes == 4u,
        "consuming byte-budget failure preserves exact provenance without partial publication");
  std::vector<RenderTapeCaptureBlob> duplicates{borrowed[0], borrowed[0]};
  RenderTapeCaptureSession oneEntry(true, {.maxBlobEntries = 1u, .maxBlobBytes = 4u});
  check(oneEntry.armWithOwnedBlobs(bootstrap, std::move(duplicates)) ==
            RenderTapeCaptureStatus::Accepted && duplicates.empty() &&
            oneEntry.ownedBlobEntries() == 1u && oneEntry.ownedBlobBytes() == 4u,
        "duplicate transferred payloads consume exactly one budget entry and byte charge");
  auto entryOverflow = borrowed;
  RenderTapeCaptureSession oneEntryRejected(true, {.maxBlobEntries = 1u});
  check(oneEntryRejected.armWithOwnedBlobs(bootstrap, std::move(entryOverflow)) ==
            RenderTapeCaptureStatus::CapacityExceeded && entryOverflow.empty() &&
            std::string_view(oneEntryRejected.armFailure().reason) == "blob_entry_limit",
        "consuming path does not widen unique entry budgets");

  bool reachedSuccess = false;
  bool sawInnerFailure = false;
  for (std::size_t allocation = 0u; allocation < 32u; ++allocation) {
    auto input = borrowed;
    RenderTapeCaptureSession injected(true);
    capture_state_allocation_probe::failAfter = allocation;
    capture_state_allocation_probe::failEnabled = true;
    const auto status = injected.armWithOwnedBlobs(bootstrap, std::move(input));
    capture_state_allocation_probe::failEnabled = false;
    check(input.empty(), "consuming API takes ownership even when staged metadata allocation fails");
    if (status == RenderTapeCaptureStatus::Accepted) {
      reachedSuccess = true;
      break;
    }
    const auto& failure = injected.armFailure();
    check(status == RenderTapeCaptureStatus::CapacityExceeded &&
              std::string_view(failure.reason) == "exception" &&
              std::string_view(failure.exceptionKind) == "bad_alloc" &&
              std::string_view(failure.phase) != "owned_blob_copy" &&
              injected.ownedBlobEntries() == 0u && injected.ownedBlobBytes() == 0u &&
              injected.publicationBundle().events.empty() && injected.sealedArtifact().empty(),
          "consuming allocation failure remains transactional with exact typed attribution");
    if (std::string_view(failure.layer) == "arm") {
      sawInnerFailure = true;
      check(failure.inputBlobCount == 3u && failure.uniqueBlobCount == 2u &&
                failure.cumulativeBlobBytes == 8u,
            "consuming downstream failure retains duplicate-aware transferred payload totals");
    }
  }
  check(reachedSuccess && sawInnerFailure,
        "consuming fault sweep reaches downstream arm failures and unchanged success");
}

void testBoundedBlobBytesAndDeduplication() {
  const RenderTapeCaptureLimits limits{.maxBlobBytes = 4u};
  const std::array<RenderTapeCaptureBlob, 2u> overflowing{
      RenderTapeCaptureBlob{std::vector<std::byte>(3u, std::byte{1})},
      RenderTapeCaptureBlob{std::vector<std::byte>(2u, std::byte{2})}};
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
      RenderTapeCaptureBlob{std::vector<std::byte>(2u, std::byte{3})},
      RenderTapeCaptureBlob{std::vector<std::byte>(2u, std::byte{4})}};
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
      RenderTapeCaptureBlob{std::vector<std::byte>(
                                3u, std::byte{0x55})},
      RenderTapeCaptureBlob{std::vector<std::byte>(
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

void testRenderTapeCardinalityResolverTruthTable() {
  struct ResolverCase {
    std::string_view value;
    std::uint32_t expected;
  };
  constexpr std::array cases{
      ResolverCase{"", 4096u}, ResolverCase{"invalid", 4096u},
      ResolverCase{"0", 4096u}, ResolverCase{"000", 4096u},
      ResolverCase{"+1", 4096u}, ResolverCase{"-1", 4096u},
      ResolverCase{" 1", 4096u}, ResolverCase{"1 ", 4096u},
      ResolverCase{"1x", 4096u}, ResolverCase{"0x1000", 4096u},
      ResolverCase{"1", 1u}, ResolverCase{"0001", 1u},
      ResolverCase{"4096", 4096u}, ResolverCase{"4097", 4097u},
      ResolverCase{"65535", 65535u}, ResolverCase{"65536", 65536u},
      ResolverCase{"65537", 65536u}, ResolverCase{"4294967296", 65536u},
      ResolverCase{"18446744073709551615", 65536u},
      ResolverCase{"18446744073709551616", 4096u},
      ResolverCase{"999999999999999999999999999999999999", 4096u},
  };
  for (const auto& testCase : cases) {
    check(dxmt9PeRenderTapeMaxEventsFromText(testCase.value) == testCase.expected &&
              dxmt9PeRenderTapeMaxBlobEntriesFromText(testCase.value) == testCase.expected,
          "capture cardinalities parse decimal only, default invalid/zero/overflow, clamp valid excess");
    for (const bool enabled : {false, true}) {
      const auto resolved = dxmt9PeRenderTapeCaptureLimitsFromText(
          enabled, testCase.value, testCase.value, "1073741825");
      check(resolved.maxEvents == (enabled ? testCase.expected : 4096u) &&
                resolved.maxBlobEntries == (enabled ? testCase.expected : 4096u) &&
                resolved.maxEventBytes == 64u * 1024u * 1024u &&
                resolved.maxBlobBytes == (enabled ? kRenderTapeHardMaxBlobBytes
                                                 : kRenderTapeDefaultMaxBlobBytes),
            "capture-only resolver gates overrides independently of unchanged event/blob byte budgets");
    }
  }
  const auto independent = dxmt9PeRenderTapeCaptureLimitsFromText(true, "4097", "65536", "4");
  check(independent.maxEvents == 4097u && independent.maxBlobEntries == 65536u &&
            independent.maxBlobBytes == 4u,
        "capture event, blob entry and blob byte limits resolve independently");
}

void testProductionCardinalityDefaultsAndHardCaps() {
  const RenderTapeCaptureLimits defaults{};
  check(defaults.maxEvents == 4096u && defaults.maxBlobEntries == 4096u &&
            kRenderTapeDefaultMaxEvents == 4096u && kRenderTapeDefaultMaxBlobEntries == 4096u &&
            kRenderTapeHardMaxEvents == 65536u && kRenderTapeHardMaxBlobEntries == 65536u &&
            defaults.maxEventBytes == 64u * 1024u * 1024u &&
            defaults.maxBlobBytes == kRenderTapeDefaultMaxBlobBytes &&
            kRenderTapeHardMaxBlobBytes == 1u * 1024u * 1024u * 1024u,
        "capture defaults remain 4096 with separate 65536 count caps and unchanged byte budgets");
  for (const bool enabled : {false, true}) {
    RenderTapeCaptureSession clamped(enabled, {
        .maxEvents = std::numeric_limits<std::uint32_t>::max(),
        .maxBlobEntries = std::numeric_limits<std::uint32_t>::max(),
        .maxBlobBytes = kRenderTapeHardMaxBlobBytes + 1u});
    check(clamped.limits().maxEvents == kRenderTapeHardMaxEvents &&
              clamped.limits().maxBlobEntries == kRenderTapeHardMaxBlobEntries &&
              clamped.limits().maxEventBytes == defaults.maxEventBytes &&
              clamped.limits().maxBlobBytes == kRenderTapeHardMaxBlobBytes,
          "capture owner enforces count and blob byte hard caps even for non-environment callers");
  }
  RenderTapeCaptureSession zero(true, {.maxEvents = 0u, .maxBlobEntries = 0u});
  check(zero.limits().maxEvents == 0u && zero.limits().maxBlobEntries == 0u,
        "explicit zero test/session limits remain fail-closed rather than adopting environment defaults");
}

void testCaptureCardinalityAdmissionBoundaries() {
  const auto bootstrap = bootstrapChunk();
  const auto chunk = singleRecordChunk(D9C_COMMAND_RECORD_CLEAR,
      bytesOf(D9CCommandChunkWireClear{.rectOffset = sizeof(D9CCommandChunkWireClear)}));
  const CommandChunkEnvelope envelope{.recordCount = 1u, .handleCount = 0u};
  for (const auto text : {"4096", "4097", "65537"}) {
    const auto limits = dxmt9PeRenderTapeCaptureLimitsFromText(true, text);
    RenderTapeCaptureSession session(true, limits);
    check(session.arm(bootstrap) == RenderTapeCaptureStatus::Accepted &&
              session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
          "cardinality event fixture starts with original bootstrap counted exactly once");
    for (std::uint32_t count = 1u; count < limits.maxEvents; ++count)
      check(session.commandChunk(envelope, chunk) == RenderTapeCaptureStatus::Accepted,
          "capture retains every canonical event through the exact configured cardinality");
    check(session.eventCount() == limits.maxEvents &&
              session.bufferedBytes() < limits.maxEventBytes &&
              session.state() == RenderTapeCaptureState::Capturing,
          "default, enlarged and hard-clamped event count boundaries do not change the byte budget");
    check(session.commandChunk(envelope, chunk) == RenderTapeCaptureStatus::CapacityExceeded &&
              session.state() == RenderTapeCaptureState::Aborted &&
              session.lastAbortAccounting().valid &&
              session.lastAbortAccounting().eventCount == limits.maxEvents &&
              session.sealedArtifact().empty(),
          "one event beyond the resolved bound aborts without filtering or partial publication");
  }
  for (const auto text : {"4096", "4097"}) {
    const auto limits = dxmt9PeRenderTapeCaptureLimitsFromText(true, {}, text);
    RenderTapeCaptureSession session(true, limits);
    check(session.arm(bootstrap) == RenderTapeCaptureStatus::Accepted,
        "cardinality blob fixture starts without fabricated payload entries");
    // Distinct lengths keep this an entry-admission fixture: deduplication's
    // linear scan rejects prior sizes before initializing comparison windows.
    // Real owned payload ingress avoids millions of scratch reads of equal-
    // sized tiny blobs; scratch/equal-size behavior has separate focused tests.
    for (std::uint32_t index = 0u; index < limits.maxBlobEntries; ++index) {
      std::vector<std::byte> bytes(index + 1u, std::byte{0x31u});
      check(session.registerOwnedBlobBytes(std::move(bytes)) ==
                RenderTapeCaptureStatus::Accepted,
          "unique original blob bytes admit through the exact default/enlarged entry boundary");
    }
    const auto expectedBytes = static_cast<std::uint64_t>(limits.maxBlobEntries) *
        (limits.maxBlobEntries + 1u) / 2u;
    const std::vector<std::byte> duplicate(limits.maxBlobEntries, std::byte{0x31u});
    check(session.registerBlobBytes(duplicate) ==
              RenderTapeCaptureStatus::Accepted &&
              session.ownedBlobEntries() == limits.maxBlobEntries &&
              session.ownedBlobBytes() == expectedBytes && expectedBytes < limits.maxBlobBytes,
          "exact-boundary duplicate conserves entry and byte charges");
    const std::vector<std::byte> excess(limits.maxBlobEntries + 1u, std::byte{0x31u});
    check(session.registerBlobBytes(excess) ==
              RenderTapeCaptureStatus::CapacityExceeded &&
              session.ownedBlobEntries() == limits.maxBlobEntries &&
              session.ownedBlobBytes() == expectedBytes &&
              std::string_view(session.mutationFailure().reason) == "blob_entry_limit" &&
              session.sealedArtifact().empty(),
          "one new blob beyond the resolved bound rejects before growth without widening byte capacity");
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

void testSha256AllocationFreeBlocksAndPadding() {
  const auto matchesHex = [](const RenderTapeDigest& digest, std::string_view expected) {
    constexpr std::string_view digits = "0123456789abcdef";
    if (expected.size() != digest.size() * 2u) return false;
    for (std::size_t index = 0u; index < digest.size(); ++index) {
      const auto value = static_cast<unsigned>(digest[index]);
      if (expected[index * 2u] != digits[value >> 4u] ||
          expected[index * 2u + 1u] != digits[value & 0xfu])
        return false;
    }
    return true;
  };
  const auto checkHash = [&](std::span<const std::byte> bytes,
                            std::string_view expected) {
    capture_state_allocation_probe::count = 0u;
    capture_state_allocation_probe::enabled = true;
    const auto digest = RenderTapeCaptureSession::sha256(bytes);
    capture_state_allocation_probe::enabled = false;
    check(matchesHex(digest, expected) && capture_state_allocation_probe::count == 0u,
          "SHA256 matches the independent golden digest with zero heap allocations");
  };
  struct StandardVector {
    std::string_view bytes;
    std::string_view digest;
  };
  constexpr std::array standards{
      StandardVector{"",
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      StandardVector{"abc",
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      StandardVector{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}};
  for (const auto& vector : standards)
    checkHash(std::as_bytes(std::span(vector.bytes.data(), vector.bytes.size())),
              vector.digest);

  // Binary prefix vectors were derived independently with Digest::SHA.
  // The one-byte offset also exercises an unaligned complete input block.
  std::array<std::byte, 66u> binary{};
  for (std::size_t index = 0u; index < 65u; ++index)
    binary[index + 1u] = static_cast<std::byte>(index);
  struct BoundaryVector {
    std::size_t bytes;
    std::string_view digest;
  };
  constexpr std::array boundaries{
      BoundaryVector{55u,
          "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
      BoundaryVector{56u,
          "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
      BoundaryVector{63u,
          "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
      BoundaryVector{64u,
          "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
      BoundaryVector{65u,
          "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"}};
  for (const auto& vector : boundaries)
    checkHash(std::span(binary).subspan(1u, vector.bytes), vector.digest);

  const std::vector<std::byte> millionA(1000000u, std::byte{0x61u});
  checkHash(millionA,
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // Exact extent of capture12's failed first blob. Allocation precedes the
  // probe; only hashing is measured. Input is 14400 repetitions of bytes0..255.
  std::vector<std::byte> capturedExtent(3686400u);
  for (std::size_t index = 0u; index < capturedExtent.size(); ++index)
    capturedExtent[index] = static_cast<std::byte>(index & 0xffu);
  checkHash(capturedExtent,
            "38a46cf828a816c7b776f09bd5bc414b90f8f0e186c562d5ee3eb6131aa6f1c6");
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

// Model binding for RenderTapeIdentitySegments.tla's SettlementExact and
// RecordPartition predicates: the positive split deliberately uses source
// ordinals/seq IDs 51/53 and 61/66, while every swapped, truncated, foreign,
// or pass-metadata mutation fails before completion evidence is exposed.
void testProductionIdentityExactEventLookupDoesNotComplete() {
  RenderTapeProductionIdentityLedger ledger;
  const RenderTapeProductionPassRange range{
      .firstRecord = 0u, .recordCount = 1u, .dagPassIndex = 0u, .passKind = 4u};
  for (std::uint64_t event : {41u, 42u}) {
    check(ledger.append(9u, event, event + 10u, event + 20u, 1u,
                        std::span(&range, 1u)) &&
              ledger.registerExpectedSettlement(
                  9u, event, event, 3u, event + 10u, event + 20u, 1u),
          "exact-event fixture registers two independent accepted sources");
  }
  D9CRenderTapeIdentitySettlementEntry result{};
  check(ledger.expectedTail(9u, result) && result.eventOrdinal == 42u,
        "a later source exists when waiting for the earlier event");
  check(ledger.expectedSettlement(9u, 41u, result) &&
            result.eventOrdinal == 41u && result.rawOrdinal == 41u &&
            result.buildGeneration == 3u && result.firstSourceOrdinal == 51u &&
            result.tailSeqId == 61u && result.sourceCount == 1u,
        "exact lookup cannot substitute the latest accepted event tail");
  D9CRenderTapeIdentityCaptureResult capture{};
  check(!ledger.copy(9u, capture, {}),
        "looking up a settlement is not proof of completion");
  for (const auto token : {0u, 8u, 10u}) {
    result.tailSeqId = 99u;
    check(!ledger.expectedSettlement(token, 41u, result) && result.tailSeqId == 0u,
          "foreign capture lookup clears an obsolete output ticket");
  }
  for (const auto event : {0u, 40u, 43u})
    check(!ledger.expectedSettlement(9u, event, result) && result.eventOrdinal == 0u,
          "missing event never falls back to the capture tail");
  check(ledger.completeSettlement(9u, 41u) &&
            ledger.expectedSettlement(9u, 41u, result) &&
            !ledger.copy(9u, capture, {}),
        "completed early event remains identifiable but cannot settle a later one");
  check(!ledger.completeSettlement(9u, 41u) && ledger.markAllSettled(9u) &&
            ledger.copy(9u, capture, {}),
        "final capture settlement preserves an earlier exact acknowledgment");
  ledger.fail(9u);
  check(!ledger.expectedSettlement(9u, 41u, result) && result.tailSeqId == 0u,
        "failed ledger cannot provide a usable completion ticket");
}

void testExplicitMipReceiptSurvivesNestedCommitAndEmptyFlush() {
  PeRenderTapeCommitReceipt receipt{};
  PeRenderTapeCommitReceiptCollector collector{&receipt};
  PeRenderTapeCommitReceiptCollector* active = nullptr;
  const PeRenderTapeCommitReceipt accepted{
      .bridgeAccepted = true, .captureToken = 9u, .eventOrdinal = 41u};
  {
    PeRenderTapeCommitReceiptCollectorScope scope(active, collector);
    check(active == &collector, "collector is scoped to the device capture owner");
    active->observe({});
    check(!receipt.bridgeAccepted, "pre-effect failure creates no accepted ticket");
    // Production CapacityPost commits have no explicit out-parameter: they
    // use the same active collector as a later final flush.
    active->observe(accepted);
    active->observe({});
    check(receipt.hasAcceptedEvent() && receipt.eventOrdinal == 41u,
          "accepted CapacityPost ticket survives an empty final flush");
    PeRenderTapeCommitReceipt innerReceipt{};
    PeRenderTapeCommitReceiptCollector inner{&innerReceipt};
    try {
      PeRenderTapeCommitReceiptCollectorScope nested(active, inner);
      active->observe({.bridgeAccepted = true, .captureToken = 10u,
                       .eventOrdinal = 51u});
      throw 1;
    } catch (int) {
    }
    check(active == &collector && receipt.eventOrdinal == 41u &&
              innerReceipt.eventOrdinal == 51u,
          "nested scope unwinding restores ownership without mixing tickets");
  }
  check(active == nullptr, "scope cannot leave a borrowed stack collector installed");
  collector.observe(accepted);
  check(receipt.conflict && !receipt.hasAcceptedEvent() &&
            receipt.eventOrdinal == 41u,
        "even a duplicate accepted ticket exposes a second commit");
  receipt.reset();
  collector.observe(accepted);
  collector.observe({.bridgeAccepted = true, .captureToken = 9u,
                     .eventOrdinal = 42u});
  check(receipt.conflict && receipt.eventOrdinal == 41u,
        "a newer commit must not silently replace the isolated mip identity");
  receipt.reset();
  collector.observe({.bridgeAccepted = true});
  check(receipt.bridgeAccepted && !receipt.hasAcceptedEvent() && !receipt.conflict,
        "untagged accepted work requires completion, not direct re-execution");
}

void testProductionExactEventWaitAcknowledgesOnlySuccessfulWait() {
  using Status = RenderTapeEventWaitStatus;
  check(dxmt9c_device_wait_render_tape_event_completion(nullptr, 9u, 41u) < 0,
        "completion-only C bridge rejects missing device before any effect");
  RenderTapeProductionIdentityLedger ledger;
  const RenderTapeProductionPassRange range{
      .firstRecord = 0u, .recordCount = 1u, .dagPassIndex = 0u, .passKind = 4u};
  for (std::uint64_t event : {41u, 42u}) {
    check(ledger.append(9u, event, event + 10u, event + 20u, 1u,
                        std::span(&range, 1u)) &&
              ledger.registerExpectedSettlement(
                  9u, event, event, 3u, event + 10u, event + 20u, 1u),
          "exact wait fixture registers two separate events");
  }
  unsigned waits = 0u;
  auto wait = [&](const auto& expected) {
    ++waits;
    check(expected.eventOrdinal == 41u && expected.rawOrdinal == 41u &&
              expected.buildGeneration == 3u && expected.firstSourceOrdinal == 51u &&
              expected.tailSeqId == 61u && expected.sourceCount == 1u,
          "wait receives the entire exact earlier identity, not the latest tail");
    return false;
  };
  check(ledger.waitAndCompleteSettlement(9u, 41u, wait) == Status::WaitFailed &&
            waits == 1u,
        "failed queue wait cannot acknowledge an event");
  D9CRenderTapeIdentityCaptureResult capture{};
  check(!ledger.copy(9u, capture, {}), "failed wait leaves capture incomplete");
  for (auto token : {0u, 8u, 10u})
    check(ledger.waitAndCompleteSettlement(token, 41u, wait) ==
              Status::InvalidIdentity && waits == 1u,
          "invalid capture does not enter a queue wait");
  for (auto event : {0u, 40u, 43u})
    check(ledger.waitAndCompleteSettlement(9u, event, wait) ==
              Status::InvalidIdentity && waits == 1u,
          "missing exact event does not wait a newer tail");
  check(ledger.waitAndCompleteSettlement(9u, 41u, [&](const auto& expected) {
          (void)wait(expected);
          return true;
        }) == Status::Completed && waits == 2u && !ledger.copy(9u, capture, {}),
        "successful wait acknowledges only the earlier event");
  check(ledger.waitAndCompleteSettlement(9u, 41u, [](const auto&) { return true; }) ==
            Status::AcknowledgmentFailed,
        "duplicate completion is not reported as new success");
  bool threw = false;
  try {
    (void)ledger.waitAndCompleteSettlement(9u, 42u, [](const auto&) -> bool {
      throw 17;
    });
  } catch (int value) {
    threw = value == 17;
  }
  check(threw && !ledger.copy(9u, capture, {}),
        "throwing wait cannot settle the remaining event");
  check(ledger.waitAndCompleteSettlement(9u, 42u, [](const auto& expected) {
          return expected.eventOrdinal == 42u;
        }) == Status::Completed && ledger.copy(9u, capture, {}),
        "capture completes only after both exact waits");
  ledger.fail(9u);
  check(ledger.waitAndCompleteSettlement(9u, 41u, wait) ==
            Status::InvalidIdentity && waits == 2u,
        "failed ledger cannot invoke the queue wait");
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
  check(ledger.registerExpectedSettlement(7u, 11u, 11u, 1u, 21u, 31u, 1u),
        "identity ledger registers the exact event tail");
  D9CRenderTapeIdentityCaptureResult premature{};
  check(!ledger.copy(7u, premature, {}),
        "identity ledger withholds evidence before completion");
  check(ledger.completeSettlement(7u, 11u),
        "identity ledger exposes completion only after the fence");

  D9CRenderTapeIdentityCaptureResult query{};
  check(ledger.copy(7u, query, {}) &&
            query.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE &&
            query.sourceCount == 1u && query.rangeCount == 2u &&
            query.captureToken == 7u &&
            query.byteCount == sizeof(D9CRenderTapeIdentitySourceEntry) +
                                   2u * sizeof(D9CRenderTapeIdentityRangeEntry) +
                                   sizeof(D9CRenderTapeIdentitySettlementEntry),
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
            ledger.registerExpectedSettlement(8u, 12u, 12u, 2u, 22u, 32u, 1u) &&
            ledger.completeSettlement(8u, 12u) && ledger.copy(8u, query, {}),
        "a newer capture token resets stale failed state deterministically");

  RenderTapeProductionIdentityLedger splitLedger;
  const RenderTapeProductionPassRange firstFragment{
      .firstRecord = 0u,
      .recordCount = 2u,
      .dagPassIndex = 3u,
      .passKind = 1u,
      .logicalPassId = 7001u,
  };
  const RenderTapeProductionPassRange secondFragment{
      .firstRecord = 2u,
      .recordCount = 3u,
      .dagPassIndex = 3u,
      .passKind = 1u,
      .logicalPassId = 7001u,
  };
  RenderTapeProductionIdentityLedger truncatedSettlement;
  check(truncatedSettlement.append(9u, 41u, 51u, 61u, 0u, 2u,
                                   std::span(&firstFragment, 1u)) &&
            truncatedSettlement.append(9u, 41u, 53u, 66u, 2u, 3u,
                                       std::span(&secondFragment, 1u)),
        "settlement exact-group negative fixture appends both source rows");
  check(!truncatedSettlement.registerExpectedSettlement(
             9u, 41u, 41u, 3u, 51u, 51u, 1u),
        "settlement cannot claim only the first row of a same-event group");
  RenderTapeProductionIdentityLedger foreignFirst;
  check(foreignFirst.append(9u, 41u, 51u, 61u, 0u, 2u,
                            std::span(&firstFragment, 1u)) &&
            foreignFirst.append(9u, 41u, 53u, 66u, 2u, 3u,
                                std::span(&secondFragment, 1u)),
        "settlement foreign-first negative fixture appends both rows");
  check(!foreignFirst.registerExpectedSettlement(
             9u, 41u, 41u, 3u, 53u, 66u, 1u),
        "settlement cannot rebind a group to a later source ordinal");
  check(splitLedger.append(9u, 41u, 51u, 61u, 0u, 2u,
                           std::span(&firstFragment, 1u)) &&
            splitLedger.append(9u, 41u, 53u, 66u, 2u, 3u,
                               std::span(&secondFragment, 1u)),
        "v2 ledger accepts contiguous same-event source fragments");
  check(splitLedger.registerExpectedSettlement(9u, 41u, 41u, 3u, 51u, 66u, 2u) &&
            splitLedger.completeSettlement(9u, 41u),
        "split ledger settles only its complete tail without assuming +1 IDs");
  check(splitLedger.copy(9u, query, {}) && query.sourceCount == 2u &&
            query.rangeCount == 2u,
        "split ledger exposes both authoritative source rows");
  std::vector<std::byte> splitBytes(query.byteCount);
  check(splitLedger.copy(9u, copied, splitBytes),
        "split ledger copies its exact versioned sidecar extent");
  std::array<D9CRenderTapeIdentitySourceEntry, 2> splitSources{};
  std::array<D9CRenderTapeIdentityRangeEntry, 2> splitRanges{};
  std::memcpy(splitSources.data(), splitBytes.data(), sizeof(splitSources));
  std::memcpy(splitRanges.data(), splitBytes.data() + sizeof(splitSources),
              sizeof(splitRanges));
  check(splitSources[0].eventOrdinal == 41u &&
            splitSources[1].eventOrdinal == 41u &&
            splitSources[0].firstRecord == 0u &&
            splitSources[1].firstRecord == 2u &&
            splitRanges[0].firstRecord == 0u &&
            splitRanges[1].firstRecord == 2u &&
            splitRanges[0].logicalPassId == 7001u &&
            splitRanges[1].logicalPassId == 7001u,
        "split sidecar preserves event-local coverage and pass continuity");
  const RenderTapeProductionPassRange wrongPassMetadata{
      .firstRecord = 5u,
      .recordCount = 1u,
      .dagPassIndex = 9u,
      .passKind = 1u,
      .logicalPassId = 7001u,
  };
  check(!splitLedger.append(9u, 41u, 53u, 63u, 5u, 1u,
                            std::span(&wrongPassMetadata, 1u)),
        "same logical pass with changed metadata fails closed");

  RenderTapeProductionIdentityLedger unauthenticatedSplit;
  const RenderTapeProductionPassRange inferredFirst{
      .firstRecord = 0u, .recordCount = 2u, .dagPassIndex = 3u, .passKind = 1u};
  const RenderTapeProductionPassRange inferredSecond{
      .firstRecord = 2u, .recordCount = 3u, .dagPassIndex = 3u, .passKind = 1u};
  check(unauthenticatedSplit.append(
                10u, 42u, 61u, 71u, 0u, 2u,
                std::span(&inferredFirst, 1u)) &&
            unauthenticatedSplit.append(
                10u, 42u, 62u, 72u, 2u, 3u,
                std::span(&inferredSecond, 1u)),
        "split ledger accepts fragments without an asserted pass join");
  check(unauthenticatedSplit.registerExpectedSettlement(
            10u, 42u, 42u, 4u, 61u, 72u, 2u) &&
            unauthenticatedSplit.completeSettlement(10u, 42u) &&
            unauthenticatedSplit.copy(10u, query, {}),
        "unauthenticated split remains queryable");
  std::vector<std::byte> unauthenticatedBytes(query.byteCount);
  check(unauthenticatedSplit.copy(10u, copied, unauthenticatedBytes),
        "unauthenticated split copies its sidecar");
  std::array<D9CRenderTapeIdentityRangeEntry, 2> inferredRanges{};
  std::memcpy(inferredRanges.data(),
              unauthenticatedBytes.data() +
                  2u * sizeof(D9CRenderTapeIdentitySourceEntry),
              sizeof(inferredRanges));
  check(inferredRanges[0].logicalPassId != inferredRanges[1].logicalPassId,
        "zero logical pass IDs never infer continuity across source boundaries");
}

void testProductionIdentityEventLocalPassComposition() {
  RenderTapeProductionIdentityLedger ledger;
  const RenderTapeProductionPassRange render{
      .firstRecord = 0u, .recordCount = 1u,
      .dagPassIndex = 0u, .passKind = 1u, .logicalPassId = 1u};
  auto coordinator = render;
  coordinator.passKind = 4u;
  check(ledger.appendEventLocalPasses(1u, 11u, 21u, 31u, 0u, 1u,
                      std::span(&render, 1u)),
        "event-local counterexample admits first event pass one");
  check(ledger.appendEventLocalPasses(1u, 12u, 22u, 32u, 0u, 1u,
                      std::span(&coordinator, 1u)),
        "different events must not collide at event-local pass one");

  // The same metadata also must not silently alias two unrelated events.
  // Exhaust the small producer namespace, both DAG/kind values, and a split
  // event; query the actual serialized sidecar rather than a test-only mapper.
  for (const auto local : {1u, 3u}) {
    for (const auto nextLocal : {1u, 3u}) {
      for (const auto dag : {0u, 2u}) {
        for (const auto kind : {1u, 4u}) {
          RenderTapeProductionIdentityLedger composed;
          auto first = render;
          first.logicalPassId = local;
          auto continuation = first;
          continuation.firstRecord = 1u;
          auto next = render;
          next.logicalPassId = nextLocal;
          next.dagPassIndex = dag;
          next.passKind = kind;
          check(composed.appendEventLocalPasses(
                    1u, 11u, 21u, 31u, 0u, 1u, std::span(&first, 1u)) &&
                    composed.appendEventLocalPasses(
                    1u, 11u, 22u, 32u, 1u, 1u, std::span(&continuation, 1u)) &&
                    composed.registerExpectedSettlement(
                    1u, 11u, 101u, 201u, 21u, 32u, 2u) &&
                    composed.completeSettlement(1u, 11u) &&
                    composed.appendEventLocalPasses(
                    1u, 12u, 23u, 33u, 0u, 1u, std::span(&next, 1u)) &&
                    composed.registerExpectedSettlement(
                    1u, 12u, 102u, 202u, 23u, 33u, 1u) &&
                    composed.completeSettlement(1u, 12u),
                "event namespace preserves exact source settlement");
          D9CRenderTapeIdentityCaptureResult query{};
          check(composed.copy(1u, query, {}) && query.sourceCount == 3u &&
                    query.rangeCount == 3u,
                "composed identity has all source and range rows");
          std::vector<std::byte> bytes(query.byteCount);
          check(composed.copy(1u, query, bytes), "copy composed identity");
          std::array<D9CRenderTapeIdentityRangeEntry, 3> rows{};
          std::memcpy(rows.data(), bytes.data() +
              3u * sizeof(D9CRenderTapeIdentitySourceEntry), sizeof(rows));
          check(rows[0].logicalPassId == rows[1].logicalPassId &&
                    rows[2].logicalPassId != rows[0].logicalPassId &&
                    rows[2].dagPassIndex == dag && rows[2].passKind == kind,
                "same-event continuation joins, different event IDs never alias");
        }
      }
    }
  }

  for (const bool eventFirst : {false, true}) {
    RenderTapeProductionIdentityLedger mixed;
    auto second = render;
    second.firstRecord = 1u;
    const auto append = [&](bool local, std::uint64_t source,
                            const RenderTapeProductionPassRange& range) {
      return local ? mixed.appendEventLocalPasses(
          1u, 11u, source, source + 10u, range.firstRecord, 1u,
          std::span(&range, 1u)) : mixed.append(
          1u, 11u, source, source + 10u, range.firstRecord, 1u,
          std::span(&range, 1u));
    };
    check(append(eventFirst, 21u, render) &&
              !append(!eventFirst, 22u, second),
          "one event must not mix frame and event-local authority");
  }
  for (unsigned mutation = 0u; mutation != 6u; ++mutation) {
    RenderTapeProductionIdentityLedger invalid;
    check(invalid.appendEventLocalPasses(
              1u, 11u, 21u, 31u, 0u, 1u, std::span(&render, 1u)),
          "invalid event-local continuation fixture");
    auto second = render;
    second.firstRecord = 1u;
    if (mutation == 0u) second.passKind = 4u;
    if (mutation == 1u) second.dagPassIndex = 2u;
    if (mutation == 2u) second.logicalPassId = 0u;
    if (mutation == 3u)
      second.logicalPassId = std::numeric_limits<std::uint64_t>::max();
    if (mutation == 4u) second.firstRecord = 0u;
    if (mutation == 5u) second.recordCount = 2u;
    check(!invalid.appendEventLocalPasses(
              1u, 11u, 22u, 32u, 1u, 1u, std::span(&second, 1u)),
          "changed membership, missing identity, overflow or coverage fails closed");
    D9CRenderTapeIdentityCaptureResult query{};
    check(!invalid.copy(1u, query, {}), "no partial identity after rejection");
    check(invalid.appendEventLocalPasses(
              2u, 1u, 1u, 1u, 0u, 1u, std::span(&render, 1u)),
          "new token resets failed event namespace");
  }

  RenderTapeProductionIdentityLedger overflow;
  auto high = render;
  high.logicalPassId = std::numeric_limits<std::uint64_t>::max() - 2u;
  check(overflow.append(1u, 11u, 21u, 31u, 0u, 1u, std::span(&high, 1u)),
        "trusted frame ID establishes near-exhaustion allocator");
  high.logicalPassId = 3u;
  check(!overflow.appendEventLocalPasses(
            1u, 12u, 22u, 32u, 0u, 1u, std::span(&high, 1u)),
        "event base plus local offset must not wrap");
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

void testExplicitMipGenerationRejectionTruthTable() {
  for (const bool tracking : {false, true})
    for (const bool autogen : {false, true})
      for (const std::uint32_t width : {1u, 8u})
        for (const std::uint32_t height : {1u, 8u})
          for (const std::uint32_t depth : {1u, 8u})
            check(renderTapeExplicitMipGenerationRequiresRejection(
                      tracking, autogen, width, height, depth) ==
                      (tracking && autogen &&
                       (width > 1u || height > 1u || depth > 1u)),
                  "explicit mip capture guard includes 2D, cube and volume chains");
  check(std::string_view(renderTapeCaptureRejectionReasonName(
            RenderTapeCaptureRejectionReason::UnsupportedExplicitMipGeneration)) ==
            "unsupported_explicit_mip_generation",
        "unrepresented synchronous mip operation has a stable named rejection");
}

void testTextureLodMutationRejectionTruthTable() {
  constexpr std::array<std::uint32_t, 3u> lods{{0u, 1u, 7u}};
  for (const bool trackingEnabled : {false, true}) {
    for (const bool managedTexture : {false, true}) {
      for (const auto oldLod : lods) {
        for (const auto newLod : lods) {
          const bool expected = trackingEnabled && managedTexture &&
                                oldLod != newLod;
          check(renderTapeTextureLodMutationRequiresRejection(
                    trackingEnabled, managedTexture, oldLod, newLod) ==
                    expected,
                "texture LOD capture rejection predicate truth table drifted");
        }
      }
    }
  }
  check(std::string_view(renderTapeCaptureRejectionReasonName(
            RenderTapeCaptureRejectionReason::UnsupportedTextureLodMutation)) ==
            "unsupported_texture_lod_mutation",
        "texture LOD capture rejection has a stable diagnostic name");

  // Requests above the available mip range are compared after the PE helper's
  // clamp. A request that remains at the current effective LOD is harmless,
  // while a request whose clamp changes resource-owned state is unsupported.
  check(!renderTapeTextureLodMutationRequiresRejection(true, true, 0u, 0u) &&
            renderTapeTextureLodMutationRequiresRejection(true, true, 0u, 1u),
        "effective LOD transition distinguishes clamped no-op from mutation");
  check(!renderTapeTextureLodMutationRequiresRejection(false, true, 0u, 1u) &&
            !renderTapeTextureLodMutationRequiresRejection(true, false, 0u, 1u),
        "disabled and non-managed ineffective requests remain capture-safe");
}

void testLinearLockCaptureLayouts() {
  constexpr std::uint32_t argb = render_tape_d3d_format::A8R8G8B8;
  check(render_tape_d3d_format::X4R4G4B4 == 30u,
        "X4R4G4B4 pins the authoritative public D3DFORMAT value");
  check(render_tape_d3d_format::L16 == 81u,
        "L16 pins the authoritative public D3DFORMAT value");
  check(render_tape_d3d_format::R32F == 114u,
        "R32F pins the authoritative public D3DFORMAT value");
  check(render_tape_d3d_format::A16B16G16R16 == 36u,
        "A16B16G16R16 pins the authoritative public D3DFORMAT value");
  check(render_tape_d3d_format::A32B32G32R32F == 116u,
        "A32B32G32R32F pins the authoritative public D3DFORMAT value");
  check(render_tape_d3d_format::Q8W8V8U8 == 63u,
        "Q8W8V8U8 pins the authoritative public D3DFORMAT value");
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
      std::pair{render_tape_d3d_format::A16B16G16R16, 8u},
      std::pair{render_tape_d3d_format::A32B32G32R32F, 16u},
      std::pair{render_tape_d3d_format::A8P8, 2u},
      std::pair{render_tape_d3d_format::P8, 1u},
      std::pair{render_tape_d3d_format::L8, 1u},
      std::pair{render_tape_d3d_format::A8L8, 2u},
      std::pair{render_tape_d3d_format::V8U8, 2u},
      std::pair{render_tape_d3d_format::Q8W8V8U8, 4u},
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

void testFloat32RgbaLockCaptureIsByteExact() {
  constexpr auto format = render_tape_d3d_format::A32B32G32R32F;
  const D9CSurfaceDesc observed{.format = format, .resourceType = 3u,
      .width = 64u, .height = 64u, .depth = 1u};
  RenderTapeLinearLockLayout observedLayout{};
  check(renderTapeLinearLockLayout(observed, 1024, nullptr, observedLayout) ==
            RenderTapeLinearLayoutStatus::Accepted &&
            observedLayout.bytesPerPixel == 16u &&
            observedLayout.fullRowBytes == 1024u &&
            observedLayout.tightBytes == 65536u &&
            observedLayout.sourceBytes == 65536u && observedLayout.fullSubresource &&
            renderTapeDeriveExpectedTextureSubresourceContent(observed).bytes == 65536u,
        "STALKER 64x64 format116 lock has exactly 65536 canonical bytes");
  const D9CSurfaceDesc desc{.format = format, .resourceType = 3u,
      .width = 3u, .height = 2u, .depth = 1u};
  RenderTapeLinearLockLayout full{};
  check(renderTapeLinearLockLayout(desc, 64, nullptr, full) ==
            RenderTapeLinearLayoutStatus::Accepted && full.fullRowBytes == 48u &&
            full.tightBytes == 96u && full.sourceBytes == 112u,
        "RGBA32F full layout separates 16-byte texels from physical row padding");
  // Raw IEEE-754 words include signed zero, a NaN payload, and infinities.
  // Capture/replay must copy these bits without interpreting or normalizing them.
  const std::array<std::uint32_t, 4u> words{
      0x80000000u, 0x7fc12345u, 0x7f800000u, 0xff800000u};
  std::vector<std::byte> expected(96u);
  for (std::size_t texel = 0u; texel < 6u; ++texel)
    std::memcpy(expected.data() + texel * sizeof(words), words.data(), sizeof(words));
  std::vector<std::byte> physical(128u, std::byte{0xe3});
  for (std::size_t row = 0u; row < 2u; ++row)
    std::memcpy(physical.data() + row * 64u, expected.data() + row * 48u, 48u);
  std::vector<std::byte> copied;
  check(copyRenderTapeLinearRows(physical.data(), full, copied) && copied == expected,
        "format116 capture preserves all component bits and strips only row padding");
  std::vector<std::byte> replay(128u, std::byte{0xe3});
  check(writeRenderTapeLinearRows(copied, replay.data(), full) && replay == physical,
        "format116 replay restores every component while preserving padding");
  const auto before = replay;
  check(!writeRenderTapeLinearRows(std::span(copied).first(copied.size() - 1u),
                                    replay.data(), full) && replay == before,
        "short format116 bytes reject before writing any destination row");
  const RenderTapeLockRect rect{.left = 1, .top = 1, .right = 3, .bottom = 2};
  RenderTapeLinearLockLayout partial{};
  check(renderTapeLinearLockLayout(desc, 64, &rect, partial) ==
            RenderTapeLinearLayoutStatus::Accepted &&
            partial.destinationByteOffset == 16u && partial.subresourceSourceOffset == 80u &&
            partial.rowBytes == 32u && partial.tightBytes == 32u &&
            copyRenderTapeLinearRows(physical.data(), partial, copied,
                                      RenderTapeLockBitsOrigin::Subresource),
        "format116 partial capture uses checked 16-byte x offsets and physical y stride");
  std::vector<std::byte> content(96u, std::byte{0x25});
  auto changed = content;
  std::copy(copied.begin(), copied.end(), changed.begin() + 64u);
  check(applyRenderTapeLinearMutation(partial, copied, content) ==
            RenderTapeBlockMutationStatus::Accepted && content == changed,
        "format116 partial mutation preserves every byte outside the captured rectangle");
  check(renderTapeLinearLockLayout(observed, 1023, nullptr, full) ==
            RenderTapeLinearLayoutStatus::InvalidPitch,
        "format116 lock rejects a pitch shorter than its exact 16-byte row");
  auto overflow = observed;
  overflow.width = 134217728u;
  check(renderTapeLinearLockLayout(overflow, std::numeric_limits<std::int32_t>::max(),
                                    nullptr, full) == RenderTapeLinearLayoutStatus::InvalidPitch &&
            renderTapeDeriveExpectedTextureSubresourceContent(overflow).status ==
                RenderTapeExpectedContentStatus::Overflow,
        "format116 row fits wire width but cannot fit signed lock or canonical pitch");
  overflow.width = 268435456u;
  check(renderTapeLinearLockLayout(overflow, std::numeric_limits<std::int32_t>::max(),
                                    nullptr, full) == RenderTapeLinearLayoutStatus::Overflow,
        "format116 row-byte multiplication rejects beyond its uint32 layout field");
  auto unknown = observed;
  unknown.format = 117u;
  check(renderTapeLinearBytesPerPixel(unknown.format) == 0u &&
            renderTapeDeriveExpectedTextureSubresourceContent(unknown).status ==
                RenderTapeExpectedContentStatus::UnsupportedFormat,
        "adding exact format116 support leaves unknown formats fail-closed");

  auto texture = versionedTextureDescriptor(RenderTapeTextureDimension::Texture2D,
                                            3u, format, 7u, 5u);
  for (std::uint32_t mip = 0u; mip < 3u; ++mip) {
    D9CSurfaceDesc level{};
    const auto offset = sizeof(RenderTapeTextureDescriptorV2) + mip * sizeof(level);
    std::memcpy(&level, texture.data() + offset, sizeof(level));
    level.width = renderTapeTextureMipExtent(7u, mip);
    level.height = renderTapeTextureMipExtent(5u, mip);
    std::memcpy(texture.data() + offset, &level, sizeof(level));
  }
  std::array<std::uint64_t, 3u> mipBytes{};
  const auto contract = renderTapeDeriveExpectedContentContract(
      D9C_CHUNK_HANDLE_KIND_TEXTURE, texture, mipBytes);
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.bytes == 672u && contract.count == 3u &&
            mipBytes == std::array<std::uint64_t, 3u>{560u, 96u, 16u} &&
            renderTapeValidateExpectedContentExtents(
                D9C_CHUNK_HANDLE_KIND_TEXTURE, texture, mipBytes) ==
                RenderTapeExpectedContentStatus::Accepted,
        "format116 mip seed contract includes every exact shrinking 16-byte extent");
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
    testOptionalPeCaptureStateLifecycle();
    testDescriptorKindAxisTruthTable();
    testKindZeroIntervalDefineUsesNonZeroDescriptorTag();
    testProductionHookGateTruthTable();
    testProfileSelectionTruthTable();
    testProducedByCapturedPassCaptureEndToEnd();
    testProducedDefinitionTemporalOrderAndAliasJournal();
    testCompletePresentPublishesExactlyOneTape();
    testCompleteCaptureIdentityV3Attachment();
    testCaptureIdentityV3AttachmentRejections();
    testCaptureIdentityV3AttachmentAllocationFailures();
    testCaptureIdentityV3SnapshotAttachment();
    testCompleteCaptureIdentityV4Attachment();
    testCaptureIdentityV4PairRejections();
    testCaptureIdentityV4AttachmentAllocationFailures();
    testSettlementRowsBindExactSourceSubset();
    testSequenceCaptureDefersSealUntilSecondPresent();
    testValidationFailurePreservesEventAndChunkLocation();
    testObjectDefineValidationDetailTruthTable();
    testPresentCompleteOracleTargetTruthTable();
    testFailureBeforePublishAndBoundedBackpressure();
    testArmFailureProvenanceAndAbortRelease();
    testMutationFailureProvenanceAndFaults();
    testArmSnapshotTransferGuardExhaustive();
    testSelectiveArmTransferAndOwnedMutation();
    testOwnedMutationFaultsAndBorrowedEquivalence();
    testCpuShadowVersionTransitions();
    testScratchPayloadIoAndRetirementRefinement();
    testScratchPayloadBoundedOwnershipTraces();
    testCpuShadowWriterFamilyRefinement();
    testSharedCaptureVersionPublicationAndLedger();
    testSharedUpdateTextureAtomicCommit();
    testConsumingArmBlobOwnershipAndEquivalence();
    testBoundedBlobBytesAndDeduplication();
    testRenderTapeBlobCapacityResolverTruthTable();
    testRenderTapeCardinalityResolverTruthTable();
    testProductionCardinalityDefaultsAndHardCaps();
    testCaptureCardinalityAdmissionBoundaries();
    testProductionBlobDefaultIsCaptureBounded();
    testObjectLifetimeAndTerminalControls();
    testPendingChunkLifetimeTruthTable();
    testGammaRampOrderedControlIsCaptured();
    testPresentOutputRoleOwnershipTruthTable();
    testStandaloneSurfaceIdentityClosureTruthTable();
    testSurfaceAliasGenerationReplacementTransition();
    testPendingAliasFlushBeforeReplacementSequence();
    testSha256AllocationFreeBlocksAndPadding();
    testPresentCaptureResultAbiAndOneShotCancellation();
    testProductionIdentityExactEventLookupDoesNotComplete();
    testProductionExactEventWaitAcknowledgesOnlySuccessfulWait();
    testExplicitMipReceiptSurvivesNestedCommitAndEmptyFlush();
    testProductionIdentityLedgerIsTokenBoundAndNoClobber();
    testProductionIdentityEventLocalPassComposition();
    testBlockCompressedLockCaptureLayouts();
    testBlockCompressedSubrectMipAndOddExtentLayouts();
    testBlockCompressedCaptureRejectsInvalidLayouts();
    testExplicitMipGenerationRejectionTruthTable();
    testTextureLodMutationRejectionTruthTable();
    testLinearLockCaptureLayouts();
    testFloat32RgbaLockCaptureIsByteExact();
    testFullSnapshotClosureTruthTable();
    testBootstrapClosureTruthTable();
    testCommandAdmissionTruthTable();
    testObjectExpectedContentContractTruthTable();
    testMissingSeedDescriptorAndProvenanceTruthTable();
    testArmBoundaryTransitionTruthTable();
    testArmColorSnapshotDescriptorTruthTable();
    testA8R8G8B8ArmSnapshotContract();
    testR32Float2DArmSnapshotContract();
    testRgba16FloatSnapshotContract();
    testCompletedEventQueryArmOverlay();
    testStandaloneD24S8SnapshotContract();
    testExpectedContentContractDerivation();
    testUpdateTextureClosureTruthTable();
    testExpectedContentValidatorOrdering();
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "render tape capture spec failed: " << failure.what() << '\n';
    return 1;
  }
}
