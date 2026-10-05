#include "device_c_render_tape_origin_locator.hpp"
#include "dxmt9/device_c.h"

// This locator regression is deliberately fed the same typed sparse builder
// that emits production draw sections.  The origin-locator target otherwise
// does not link the PE compatibility-builder library, so keep the test-only
// translation-unit inclusion local to this fixture rather than adding a
// production or build-system dependency.
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_producer_views.hpp"
#include "../../../src/d3d9/d3d9_pe_chunk_builder.cpp"
#include "../../../src/d3d9/d3d9_pe_chunk_draw.cpp"

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

struct D9CBuffer {};
struct D9CTexture {};
struct D9CSurface {};

extern "C" void dxmt9c_buffer_addref(D9CBuffer*) {}
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer*) { return 0u; }
extern "C" void dxmt9c_texture_addref(D9CTexture*) {}
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture*) { return 0u; }
extern "C" void dxmt9c_surface_addref(D9CSurface*) {}
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface*) { return 0u; }

namespace {

using namespace dxmt9::d3d9;
namespace pe = dxmt9::d3d9::pe;

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

struct CanonicalSparseChunk {
  std::vector<std::byte> blob;
  CommandChunkEnvelope envelope{
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = 1u,
      .handleCount = 3u,
      .producerIdentity = {
          .firstEventOrdinal = 1u,
          .lastEventOrdinal = 1u,
          .firstSourceOrdinal = 1u,
          .lastSourceOrdinal = 1u,
      },
  };
};

CanonicalSparseChunk canonicalInlineDraw(std::uint32_t constantMask) {
  D9CBuffer vertexBuffer;
  D9CTexture texture;
  D9CSurface renderTarget;
  const auto bufferRef = pe::PeWireObjectRef{
      .identity = identity(D9C_CHUNK_HANDLE_KIND_BUFFER, 21u, 210u),
      .object = &vertexBuffer,
  };
  const auto textureRef = pe::PeWireObjectRef{
      .identity = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 22u, 220u),
      .object = &texture,
  };
  const auto renderTargetRef = pe::PeWireObjectRef{
      .identity = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 23u, 230u),
      .object = &renderTarget,
  };

  std::array<pe::SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1>
      textures{};
  textures[0].wire = {
      .slot = 0u,
      .valid = 1u,
      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
      .reserved0 = 0u,
  };
  textures[0].object = textureRef;
  std::array<pe::SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1>
      streams{};
  streams[0].wire = {
      .slot = 0u,
      .valid = 1u,
      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
      .offset = 0u,
      .stride = 16u,
      .frequency = 0u,
      .reserved0 = 0u,
  };
  streams[0].object = bufferRef;
  std::array<pe::SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>, 1>
      renderTargets{};
  renderTargets[0].wire = {
      .slot = 0u,
      .valid = 1u,
      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
      .reserved0 = 0u,
  };
  renderTargets[0].object = renderTargetRef;

  std::array<std::byte, 16u> floatBytes{};
  std::array<std::byte, 4u> boolBytes{};
  for (std::size_t i = 0u; i < floatBytes.size(); ++i)
    floatBytes[i] = std::byte{static_cast<unsigned char>(0x30u + i)};
  for (std::size_t i = 0u; i < boolBytes.size(); ++i)
    boolBytes[i] = std::byte{static_cast<unsigned char>(0x50u + i)};

  pe::SparseStateInput state{};
  state.textures = textures;
  state.streams = streams;
  state.renderTargets = renderTargets;
  if ((constantMask & (1u << 0u)) != 0u)
    state.vsFloatConstants = {0u, 1u, floatBytes};
  if ((constantMask & (1u << 1u)) != 0u)
    state.vsIntConstants = {0u, 1u, floatBytes};
  if ((constantMask & (1u << 2u)) != 0u)
    state.vsBoolConstants = {0u, 1u, boolBytes};
  if ((constantMask & (1u << 3u)) != 0u)
    state.psFloatConstants = {0u, 1u, floatBytes};
  if ((constantMask & (1u << 4u)) != 0u)
    state.psIntConstants = {0u, 1u, floatBytes};
  if ((constantMask & (1u << 5u)) != 0u)
    state.psBoolConstants = {0u, 1u, boolBytes};

  pe::CommandChunkBuilder builder;
  D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u,
      .primitiveCount = 1u,
  };
  check(pe::appendSparseRecord(builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                               draw, state),
        "typed canonical sparse draw appends");
  const auto sealed = builder.seal();
  check(sealed.valid(), "typed canonical sparse draw seals");
  CanonicalSparseChunk result;
  result.blob.assign(sealed.blob.begin(), sealed.blob.end());
  return result;
}

ImportedChunkView validateCanonical(CanonicalSparseChunk& fixture) {
  ImportedChunkView imported;
  const auto validation = validateCommandChunk(fixture.blob,
                                                fixture.envelope, &imported);
  check(validation.valid(), "typed canonical sparse draw validates");
  return imported;
}

std::uint32_t handleIndexFor(const ImportedChunkView& imported,
                             const D9CWireObjectIdentity& wanted) {
  for (std::uint32_t index = 0u;
       index < imported.handles.size(); ++index) {
    const auto& entry = imported.handles[index];
    if (entry.kind == wanted.kind && entry.generation == wanted.generation &&
        entry.objectId == wanted.objectId)
      return index;
  }
  return D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
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

  const D9CCommandChunkWireGenerateMipmaps generate{
      .textureHandleIndex = 0u,
  };
  fixture = nonDrawFixture(D9C_COMMAND_RECORD_GENERATE_MIPMAPS, generate, 1u);
  check(renderTapeLocateOrigin(fixture.view(), 0u, source).role ==
            RenderTapeCommandRole::CopySource,
        "GenerateMipmaps retains the base-level source identity");
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

void testCanonicalInlineConstantOrigins() {
  const auto texture = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 22u, 220u);
  const auto buffer = identity(D9C_CHUNK_HANDLE_KIND_BUFFER, 21u, 210u);
  const auto renderTarget =
      identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 23u, 230u);
  const auto textureRole = RenderTapeCommandRole::ShaderReadCandidate;
  const auto renderTargetRole = RenderTapeCommandRole::RenderTargetBinding;

  // Each row is a real typed CommandChunkBuilder draw, validated before the
  // locator sees it.  The stream handle is intentionally unknown to the
  // locator, so it traverses every following constant section; zero
  // elementSize is canonical for those variable-width sections.
  for (const auto mask : {1u, 2u, 4u, 8u, 16u, 32u, 63u}) {
    auto fixture = canonicalInlineDraw(mask);
    const auto imported = validateCanonical(fixture);
    const auto textureIndex = handleIndexFor(imported, texture);
    const auto bufferIndex = handleIndexFor(imported, buffer);
    const auto renderTargetIndex = handleIndexFor(imported, renderTarget);
    check(textureIndex != D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX &&
              bufferIndex != D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX &&
              renderTargetIndex != D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
          "typed canonical draw retains all resource handles");

    const auto textureLocated =
        renderTapeLocateOrigin(imported, textureIndex, texture);
    check(textureLocated.status == RenderTapeOriginLocatorStatus::Accepted &&
              textureLocated.sectionKind ==
                  D9C_COMMAND_CHUNK_SECTION_TEXTURE &&
              textureLocated.bindingSlot == 0u &&
              textureLocated.role == textureRole,
          "inline constant sections preserve matched texture origin");

    const auto renderTargetLocated =
        renderTapeLocateOrigin(imported, renderTargetIndex, renderTarget);
    check(renderTargetLocated.status ==
                  RenderTapeOriginLocatorStatus::Accepted &&
              renderTargetLocated.sectionKind ==
                  D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET &&
              renderTargetLocated.bindingSlot == 0u &&
              renderTargetLocated.role == renderTargetRole,
          "inline constant sections preserve matched render-target origin");

    const auto bufferLocated =
        renderTapeLocateOrigin(imported, bufferIndex, buffer);
    check(bufferLocated.status == RenderTapeOriginLocatorStatus::Accepted &&
              bufferLocated.recordIndex == 0u &&
              bufferLocated.sectionKind == kRenderTapeOriginSentinel &&
              bufferLocated.bindingSlot == kRenderTapeOriginSentinel &&
              bufferLocated.role == RenderTapeCommandRole::Unknown &&
              bufferLocated.storageRole == RenderTapeStorageRole::Unknown,
          "inline constant sections do not reject unknown stream binding");
  }
}

void testCanonicalMalformedSectionGeometryStillRejects() {
  auto malformedConstant = canonicalInlineDraw(1u);
  auto imported = validateCanonical(malformedConstant);
  const auto record = imported.record(0u);
  std::size_t constantIndex = record.sections.size();
  for (std::size_t index = 0u; index < record.sections.size(); ++index) {
    if (record.sections[index].kind ==
        D9C_COMMAND_CHUNK_SECTION_VS_CONST_F) {
      constantIndex = index;
      break;
    }
  }
  check(constantIndex < record.sections.size(),
        "typed canonical draw contains a constant section");
  const auto constant = record.section(constantIndex);
  D9CCommandChunkWireConstantRange range{};
  std::memcpy(&range, constant.payload.data(), sizeof(range));
  range.registerCount = 2u;
  std::memcpy(const_cast<std::byte*>(constant.payload.data()), &range,
              sizeof(range));
  check(!validateCommandChunk(malformedConstant.blob,
                              malformedConstant.envelope)
             .valid(),
        "malformed constant payload remains rejected by canonical validator");

  auto malformedBinding = canonicalInlineDraw(1u);
  imported = validateCanonical(malformedBinding);
  const auto bindingRecord = imported.record(0u);
  std::size_t streamIndex = bindingRecord.sections.size();
  for (std::size_t index = 0u; index < bindingRecord.sections.size(); ++index) {
    if (bindingRecord.sections[index].kind ==
        D9C_COMMAND_CHUNK_SECTION_STREAM) {
      streamIndex = index;
      break;
    }
  }
  check(streamIndex < bindingRecord.sections.size(),
        "typed canonical draw contains a stream binding");
  auto draw = bindingRecord.drawHeader;
  D9CCommandChunkWireSectionDesc descriptor =
      bindingRecord.sections[streamIndex];
  descriptor.elementSize = 0u;
  const auto descriptorOffset =
      static_cast<std::size_t>(draw.sectionTableOffset) +
      streamIndex * sizeof(D9CCommandChunkWireSectionDesc);
  check(descriptorOffset + sizeof(descriptor) <= bindingRecord.payload.size(),
        "typed section table mutation remains in the validated record");
  std::memcpy(const_cast<std::byte*>(bindingRecord.payload.data()) +
                  descriptorOffset,
              &descriptor, sizeof(descriptor));
  check(!validateCommandChunk(malformedBinding.blob,
                              malformedBinding.envelope)
             .valid(),
        "malformed handle-bearing element geometry remains rejected");
}

void testNullRtAllUsesNotFirstOrigin() {
  const auto alias = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 7u, 99u);
  const auto parent = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 8u, 100u);
  auto rt = sparseFixture(D9C_COMMAND_RECORD_APPLY_STATE,
      D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u);
  rt.handles[0].kind = alias.kind;
  check(renderTapeNullRtUsesAdmitted(rt.view(), alias, parent),
      "NULL alias binding-only RT use needs no fabricated terminal clear");
  auto draw = rt;
  draw.record.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  check(renderTapeNullRtUsesAdmitted(draw.view(), alias, parent),
      "NULL alias explicit RT draw binding remains in the write-only domain");
  const std::array specs{
      SparseSectionSpec{D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u, 0u},
      SparseSectionSpec{D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, 0u, 0u}};
  auto mixed = sparseSectionsFixture(D9C_COMMAND_RECORD_APPLY_STATE, specs);
  mixed.handles[0].kind = alias.kind;
  check(renderTapeLocateOrigin(mixed.view(), 0u, parent).role ==
            RenderTapeCommandRole::RenderTargetBinding &&
      !renderTapeNullRtUsesAdmitted(mixed.view(), alias, parent),
      "later forbidden section rejects even though the first diagnostic origin is an RT binding");
  auto sampler = sparseSectionsFixture(D9C_COMMAND_RECORD_APPLY_STATE,
      std::array{SparseSectionSpec{D9C_COMMAND_CHUNK_SECTION_TEXTURE, 1u, 0u}});
  sampler.handles[0] = rt.handles[0];
  check(!renderTapeNullRtUsesAdmitted(sampler.view(), alias, parent) &&
      !renderTapeNullRtUsesAdmitted(sampler.view(), parent),
      "parent sampler APPLY_STATE binding rejects before any inherited later draw");
  const auto secondOffset = (rt.payload.size() + 3u) & ~std::size_t{3u};
  auto payload = rt.payload;
  payload.resize(secondOffset);
  payload.insert(payload.end(), sampler.payload.begin(), sampler.payload.end());
  std::array records{rt.record, sampler.record};
  records[1].payloadOffset = static_cast<std::uint32_t>(secondOffset);
  records[1].firstHandle = 1u;
  records[1].handleCount = 1u;
  const ImportedChunkView repeated{
      .records = records, .handles = sampler.handles, .payloadArena = payload};
  check(renderTapeLocateOrigin(repeated, 0u, parent).role ==
            RenderTapeCommandRole::RenderTargetBinding &&
      !renderTapeNullRtUsesAdmitted(repeated, alias, parent),
      "later record in the same chunk cannot hide a parent sampler behind its first RT origin");
  for (const auto type : {D9C_COMMAND_RECORD_STRETCH_RECT, D9C_COMMAND_RECORD_COLOR_FILL,
           D9C_COMMAND_RECORD_UPDATE_SURFACE, D9C_COMMAND_RECORD_READBACK,
           D9C_COMMAND_RECORD_PRESENT}) {
    auto forbidden = rt;
    forbidden.record.type = type;
    check(!renderTapeNullRtUsesAdmitted(forbidden.view(), alias, parent),
        "NULL marker cannot become copy/fill/readback/Present contents");
  }
}

} // namespace

int main() {
  try {
    testSparseRoles();
    testCopyAndReadbackRoles();
    testMalformedAndAliasPreservation();
    testCanonicalInlineConstantOrigins();
    testCanonicalMalformedSectionGeometryStillRejects();
    testNullRtAllUsesNotFirstOrigin();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "render tape origin locator spec passed\n";
  return 0;
}
