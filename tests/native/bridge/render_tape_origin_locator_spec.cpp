#include "device_c_render_tape_origin_locator.hpp"
#include "dxmt9/device_c.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition)
    throw Failure(std::string(message));
}

struct ChunkFixture {
  D9CCommandChunkWireRecordHeader record{};
  std::vector<D9CCommandChunkWireHandleEntry> handles{};
  std::vector<std::byte> payload{};

  ImportedChunkView view() const noexcept {
    return ImportedChunkView{
        .records = std::span<const D9CCommandChunkWireRecordHeader>(&record, 1u),
        .handles = handles,
        .payloadArena = payload,
    };
  }
};

D9CWireObjectIdentity identity(std::uint32_t kind, std::uint32_t generation,
                               std::uint64_t objectId) {
  return D9CWireObjectIdentity{
      .kind = kind,
      .generation = generation,
      .objectId = objectId,
  };
}

struct SparseSectionSpec {
  std::uint16_t kind = 0u;
  std::uint32_t handleIndex = 0u;
  std::uint32_t slot = 0u;
};

ChunkFixture sparseSectionsFixture(
    std::uint32_t recordType, std::span<const SparseSectionSpec> specs) {
  ChunkFixture fixture;
  std::uint32_t handleCount = 0u;
  for (const auto& spec : specs)
    handleCount = std::max(handleCount, spec.handleIndex + 1u);
  fixture.handles.resize(handleCount);
  for (std::uint32_t handleIndex = 0u; handleIndex < handleCount;
       ++handleIndex) {
    fixture.handles[handleIndex] = {
        .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
        .generation = 7u + handleIndex,
        .objectId = 99u + handleIndex,
    };
  }
  D9CCommandChunkWireDrawHeader draw{};
  draw.primitiveType = recordType == D9C_COMMAND_RECORD_APPLY_STATE ? 0u : 4u;
  draw.primitiveCount = recordType == D9C_COMMAND_RECORD_APPLY_STATE ? 0u : 1u;
  draw.sectionCount = static_cast<std::uint32_t>(specs.size());
  draw.sectionTableOffset = sizeof(draw);
  draw.sectionPayloadOffset =
      sizeof(draw) + specs.size() * sizeof(D9CCommandChunkWireSectionDesc);
  std::vector<D9CCommandChunkWireSectionDesc> sections;
  sections.reserve(specs.size());
  std::size_t bindingOffset = draw.sectionPayloadOffset;
  for (const auto& spec : specs) {
    const auto elementSize = static_cast<std::uint16_t>(
        spec.kind == D9C_COMMAND_CHUNK_SECTION_TEXTURE
            ? sizeof(D9CCommandChunkWireTextureBinding)
            : spec.kind == D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET
                  ? sizeof(D9CCommandChunkWireRenderTargetBinding)
                  : sizeof(D9CCommandChunkWireDepthStencilBinding));
    sections.push_back({
        .kind = spec.kind,
        .elementSize = elementSize,
        .count = 1u,
        .payloadOffset = static_cast<std::uint32_t>(bindingOffset),
        .byteSize = elementSize,
    });
    bindingOffset += elementSize;
  }
  fixture.payload.resize(bindingOffset);
  std::memcpy(fixture.payload.data(), &draw, sizeof(draw));
  std::memcpy(fixture.payload.data() + draw.sectionTableOffset, sections.data(),
              sections.size() * sizeof(sections[0]));
  bindingOffset = draw.sectionPayloadOffset;
  for (const auto& spec : specs) {
    if (spec.kind == D9C_COMMAND_CHUNK_SECTION_TEXTURE) {
      const D9CCommandChunkWireTextureBinding binding{
          .slot = spec.slot, .valid = 1u, .handleIndex = spec.handleIndex};
      std::memcpy(fixture.payload.data() + bindingOffset, &binding,
                  sizeof(binding));
      bindingOffset += sizeof(binding);
    } else if (spec.kind == D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET) {
      const D9CCommandChunkWireRenderTargetBinding binding{
          .slot = spec.slot, .valid = 1u, .handleIndex = spec.handleIndex};
      std::memcpy(fixture.payload.data() + bindingOffset, &binding,
                  sizeof(binding));
      bindingOffset += sizeof(binding);
    } else {
      const D9CCommandChunkWireDepthStencilBinding binding{
          .valid = 1u, .handleIndex = spec.handleIndex};
      std::memcpy(fixture.payload.data() + bindingOffset, &binding,
                  sizeof(binding));
      bindingOffset += sizeof(binding);
    }
  }
  fixture.record = {
      .type = recordType,
      .payloadSize = static_cast<std::uint32_t>(fixture.payload.size()),
      .firstHandle = 0u,
      .handleCount = handleCount,
  };
  return fixture;
}

ChunkFixture sparseFixture(std::uint32_t recordType, std::uint16_t sectionKind,
                           std::uint32_t slot) {
  const std::array<SparseSectionSpec, 1> specs{{
      {.kind = sectionKind, .handleIndex = 0u, .slot = slot},
  }};
  return sparseSectionsFixture(recordType, specs);
}

template <typename T>
ChunkFixture nonDrawFixture(std::uint32_t recordType, const T& fixed,
                            std::uint32_t handleCount) {
  ChunkFixture fixture;
  fixture.handles = {
      {.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 7u, .objectId = 99u},
      {.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 8u, .objectId = 100u},
  };
  fixture.handles.resize(handleCount);
  fixture.payload.resize(sizeof(T));
  std::memcpy(fixture.payload.data(), &fixed, sizeof(T));
  fixture.record = {
      .type = recordType,
      .payloadSize = sizeof(T),
      .firstHandle = 0u,
      .handleCount = handleCount,
  };
  return fixture;
}

void testSparseRoles() {
  const auto origin = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 7u, 99u);
  for (const auto [recordType, expected] : {
           std::pair{D9C_COMMAND_RECORD_APPLY_STATE,
                     RenderTapeCommandRole::BindingOnly},
           std::pair{D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                     RenderTapeCommandRole::ShaderReadCandidate},
       }) {
    auto fixture = sparseFixture(recordType, D9C_COMMAND_CHUNK_SECTION_TEXTURE,
                                 4u);
    const auto located = renderTapeLocateOrigin(fixture.view(), 0u, origin);
    check(located.status == RenderTapeOriginLocatorStatus::Accepted &&
              located.originIdentity.objectId == 99u &&
              located.recordIndex == 0u &&
              located.recordType == recordType &&
              located.sectionKind == D9C_COMMAND_CHUNK_SECTION_TEXTURE &&
              located.bindingSlot == 4u && located.role == expected,
          "sparse texture role and binding slot");
  }

  auto renderTarget = sparseFixture(
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 2u);
  check(renderTapeLocateOrigin(renderTarget.view(), 0u, origin).role ==
            RenderTapeCommandRole::RenderTargetBinding,
            "render-target binding is only a write candidate");
  auto depth = sparseFixture(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                             D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, 0u);
  const auto depthLocated = renderTapeLocateOrigin(depth.view(), 0u, origin);
  check(depthLocated.role == RenderTapeCommandRole::DepthStencilBinding &&
            depthLocated.bindingSlot == kRenderTapeOriginSentinel,
            "depth binding is only a write candidate");

  const std::array<SparseSectionSpec, 3> orderedByFixture{{
      {.kind = D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
       .handleIndex = 0u},
      {.kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
       .handleIndex = 1u,
       .slot = 5u},
      {.kind = D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
       .handleIndex = 2u,
       .slot = 1u},
  }};
  auto mixed = sparseSectionsFixture(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                                     orderedByFixture);
  const auto mixedTexture = renderTapeLocateOrigin(
      mixed.view(), 1u,
      identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 8u, 100u));
  const auto mixedTarget = renderTapeLocateOrigin(
      mixed.view(), 2u,
      identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 9u, 101u));
  check(mixedTexture.sectionKind == D9C_COMMAND_CHUNK_SECTION_TEXTURE &&
            mixedTexture.bindingSlot == 5u &&
            mixedTexture.role == RenderTapeCommandRole::ShaderReadCandidate &&
            mixedTarget.sectionKind == D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET &&
            mixedTarget.bindingSlot == 1u &&
            mixedTarget.role == RenderTapeCommandRole::RenderTargetBinding,
        "unrelated earlier depth does not steal later texture or RT match");
}

void testCopyAndReadbackRoles() {
  const auto source = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 7u, 99u);
  const auto destination = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 8u, 100u);
  const D9CCommandChunkWireUpdateTexture update{
      .srcHandleIndex = 0u,
      .dstHandleIndex = 1u,
  };
  auto fixture = nonDrawFixture(D9C_COMMAND_RECORD_UPDATE_TEXTURE, update, 2u);
  check(renderTapeLocateOrigin(fixture.view(), 0u, source).role ==
            RenderTapeCommandRole::CopySource &&
            renderTapeLocateOrigin(fixture.view(), 1u, destination).role ==
                RenderTapeCommandRole::CopyDestination,
        "UpdateTexture source and destination roles");

  const D9CCommandChunkWireReadback readback{
      .srcHandleIndex = 0u,
      .dstHandleIndex = 1u,
  };
  fixture = nonDrawFixture(D9C_COMMAND_RECORD_READBACK, readback, 2u);
  check(renderTapeLocateOrigin(fixture.view(), 0u, source).role ==
            RenderTapeCommandRole::ReadbackSource &&
            renderTapeLocateOrigin(fixture.view(), 1u, destination).role ==
                RenderTapeCommandRole::CopyDestination,
        "Readback source and destination roles");
}

void testMalformedAndAliasPreservation() {
  const auto origin = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 14u, 7525u);
  const auto resolved = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 14u, 7524u);
  auto fixture = sparseFixture(D9C_COMMAND_RECORD_APPLY_STATE,
                               D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u);
  fixture.handles[0] = {
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 14u,
      .objectId = 7525u,
  };
  const auto alias = renderTapeLocateOrigin(fixture.view(), 0u, resolved);
  check(alias.originIdentity.kind == origin.kind &&
            alias.originIdentity.generation == origin.generation &&
            alias.originIdentity.objectId == origin.objectId &&
            alias.resolvedIdentity.kind == resolved.kind &&
            alias.resolvedIdentity.generation == resolved.generation &&
            alias.resolvedIdentity.objectId == resolved.objectId &&
            alias.aliasOrigin &&
            alias.role == RenderTapeCommandRole::RenderTargetBinding &&
            alias.storageRole == RenderTapeStorageRole::RenderTargetCandidate,
        "alias resolution preserves original handle identity");

  check(renderTapeLocateOrigin(fixture.view(), 4u, origin).status ==
            RenderTapeOriginLocatorStatus::InvalidHandle,
        "out-of-range handle is typed and fail-closed");

  fixture.payload.resize(1u);
  fixture.record.payloadSize = 1u;
  check(renderTapeLocateOrigin(fixture.view(), 0u, origin).status ==
            RenderTapeOriginLocatorStatus::MalformedRecord,
        "short referenced record is typed as malformed");
}

} // namespace

int main() {
  try {
    testSparseRoles();
    testCopyAndReadbackRoles();
    testMalformedAndAliasPreservation();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "render tape origin locator spec passed\n";
  return 0;
}
