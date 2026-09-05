#include "d3d9_pe_semantic_batch.hpp"
#include "d3d9_pe_batch.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_semantic_recorder_state.hpp"
#include "device_c_chunk_validate.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

struct RefCounter { std::uint32_t refs = 1u; };
struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};

template <typename T> void addRef(T* value) { ++value->refs; }
template <typename T> std::uint32_t release(T* value) { return --value->refs; }
extern "C" void dxmt9c_surface_addref(D9CSurface* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) { return release(value); }
extern "C" void dxmt9c_texture_addref(D9CTexture* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* value) { return release(value); }
extern "C" void dxmt9c_buffer_addref(D9CBuffer* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* value) { return release(value); }
extern "C" void dxmt9c_shader_addref(D9CShader* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* value) { return release(value); }
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* value) { return release(value); }
extern "C" void dxmt9c_query_addref(D9CQuery* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* value) { return release(value); }

namespace {
using namespace dxmt9::d3d9;
using namespace dxmt9::d3d9::pe;
using namespace dxmt9::d3d9::pe::semantic_batch;
using BatchOwner = ImmutableSemanticBatchOwner<32u, 16384u, 128u>;
using BatchPlan = SemanticBatchCountPlan<32u, 16384u, 128u>;

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
void check(bool condition, std::string_view message) {
  if (!condition) throw Failure(std::string(message));
}
PeWireObjectRef wireRef(void* object, std::uint32_t kind, std::uint64_t id) {
  return {.identity = {.kind = kind, .generation = 11u, .objectId = id},
          .object = object};
}

struct FamilyFixture {
  D9CTexture sourceTexture, destinationTexture;
  D9CBuffer buffer;
  D9CShader shader;
  D9CVertexDecl declaration;
  D9CSurface sourceSurface, destinationSurface;
  D9CQuery query;
  TextureRef sourceTextureRef{qualifyLocalRef<TextureRef>(
      wireRef(&sourceTexture, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0xc100u))};
  TextureRef destinationTextureRef{qualifyLocalRef<TextureRef>(
      wireRef(&destinationTexture, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0xc200u))};
  SurfaceRef sourceSurfaceRef{qualifyLocalRef<SurfaceRef>(
      wireRef(&sourceSurface, D9C_CHUNK_HANDLE_KIND_SURFACE, 0xc300u))};
  SurfaceRef destinationSurfaceRef{qualifyLocalRef<SurfaceRef>(
      wireRef(&destinationSurface, D9C_CHUNK_HANDLE_KIND_SURFACE, 0xc400u))};
  QueryRef queryRef{qualifyLocalRef<QueryRef>(
      wireRef(&query, D9C_CHUNK_HANDLE_KIND_QUERY, 0xc500u))};
  BufferRef bufferRef{qualifyLocalRef<BufferRef>(
      wireRef(&buffer, D9C_CHUNK_HANDLE_KIND_BUFFER, 0xc600u))};
  ShaderRef shaderRef{qualifyLocalRef<ShaderRef>(
      wireRef(&shader, D9C_CHUNK_HANDLE_KIND_SHADER, 0xc700u))};
  DeclarationRef declarationRef{qualifyLocalRef<DeclarationRef>(
      wireRef(&declaration, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, 0xc800u))};
  std::array<std::byte, 16u> wideConstant{};
  std::array<std::byte, 4u> boolConstant{};
  std::array<std::byte, 12u> vertices{};
  std::array<std::byte, 6u> indices{};
  std::array<D9CRect, 1u> clearRects{D9CRect{0, 0, 8, 8}};
  std::array<D9CCommandChunkWireRenderState, 1u> renderStates{
      D9CCommandChunkWireRenderState{.state = 7u, .value = 1u}};

  bool emit(PeSemanticProducerKind kind, CommandChunkBuilder& builder) noexcept {
    using Kind = PeSemanticProducerKind;
    switch (kind) {
      case Kind::DrawPrimitive: {
        // Keep one independent canonical row for every handle-bearing sparse
        // family in the batch oracle. The owner under test only receives the
        // validated bytes/identities from this record, so this catches a
        // family omission without sharing its prepared layout implementation.
        const std::array textures = {SparseBindingInput<
            D9CCommandChunkWireTextureBinding>{
            .wire = {.slot = 3u, .valid = 1u}, .object = sourceTextureRef}};
        const std::array streams = {SparseBindingInput<
            D9CCommandChunkWireStreamBinding>{
            .wire = {.slot = 4u, .valid = 1u, .offset = 8u, .stride = 16u,
                     .frequency = 1u}, .object = bufferRef}};
        const std::array shaders = {SparseBindingInput<
            D9CCommandChunkWireShaderBinding>{
            .wire = {.stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
                     .valid = 1u}, .object = shaderRef}};
        const std::array vertexInputs = {SparseBindingInput<
            D9CCommandChunkWireVertexInput>{
            .wire = {.valid = 1u,
                     .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION},
            .object = declarationRef}};
        const std::array indexBuffers = {SparseBindingInput<
            D9CCommandChunkWireIndexBinding>{
            .wire = {.valid = 1u}, .object = bufferRef}};
        const std::array renderTargets = {SparseBindingInput<
            D9CCommandChunkWireRenderTargetBinding>{
            .wire = {.slot = 2u, .valid = 1u}, .object = sourceSurfaceRef}};
        const std::array depthStencils = {SparseBindingInput<
            D9CCommandChunkWireDepthStencilBinding>{
            .wire = {.valid = 1u}, .object = sourceSurfaceRef}};
        const SparseStateInput sparse{
            .textures = textures,
            .streams = streams,
            .shaders = shaders,
            .vertexInputs = vertexInputs,
            .indexBuffers = indexBuffers,
            .renderTargets = renderTargets,
            .depthStencils = depthStencils,
        };
        return appendSparseRecord(
            builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
            D9CCommandChunkWireDrawHeader{.primitiveType = 4u,
                                          .primitiveCount = 1u},
            sparse);
      }
      case Kind::DrawIndexedPrimitive:
        return appendSparseRecord(builder, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
            D9CCommandChunkWireDrawHeader{.primitiveType = 4u, .numVertices = 3u,
                                          .primitiveCount = 1u}, {});
      case Kind::DrawPrimitiveUp:
        return appendSparseRecord(builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
            D9CCommandChunkWireDrawHeader{.primitiveType = 4u, .primitiveCount = 1u,
                                          .stride = 4u},
            SparseStateInput{.upVertexData = vertices});
      case Kind::DrawIndexedPrimitiveUp:
        return appendSparseRecord(builder, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
            D9CCommandChunkWireDrawHeader{.primitiveType = 4u, .numVertices = 3u,
                .primitiveCount = 1u, .stride = 4u, .indexFormat = 101u},
            SparseStateInput{.upIndexData = indices, .upVertexData = vertices});
      case Kind::ApplyState:
        return appendApplyState(builder, 0u,
                                SparseStateInput{.renderStates = renderStates});
      case Kind::VsFloatConstant:
        return appendSetConstants(builder, D9C_COMMAND_RECORD_SET_VS_CONST_F, 0u, 1u, wideConstant);
      case Kind::VsIntConstant:
        return appendSetConstants(builder, D9C_COMMAND_RECORD_SET_VS_CONST_I, 0u, 1u, wideConstant);
      case Kind::VsBoolConstant:
        return appendSetConstants(builder, D9C_COMMAND_RECORD_SET_VS_CONST_B, 0u, 1u, boolConstant);
      case Kind::PsFloatConstant:
        return appendSetConstants(builder, D9C_COMMAND_RECORD_SET_PS_CONST_F, 0u, 1u, wideConstant);
      case Kind::PsIntConstant:
        return appendSetConstants(builder, D9C_COMMAND_RECORD_SET_PS_CONST_I, 0u, 1u, wideConstant);
      case Kind::PsBoolConstant:
        return appendSetConstants(builder, D9C_COMMAND_RECORD_SET_PS_CONST_B, 0u, 1u, boolConstant);
      case Kind::Clear:
        return appendClear(builder, D9CCommandChunkWireClear{
            .flags = 1u, .colorARGB = 0xff010203u, .z = 1.0f}, clearRects);
      case Kind::StretchRect:
        return appendStretchRect(builder, {}, sourceSurfaceRef, destinationSurfaceRef);
      case Kind::ColorFill:
        return appendColorFill(builder, {.colorARGB = 0xff010203u}, destinationSurfaceRef);
      case Kind::UpdateTexture:
        return appendUpdateTexture(builder, sourceTextureRef, destinationTextureRef);
      case Kind::UpdateSurface:
        return appendUpdateSurface(builder, {}, sourceSurfaceRef, destinationSurfaceRef);
      case Kind::QueryIssue:
        return appendQueryIssue(builder, 1u, queryRef);
      case Kind::Readback:
        return appendReadback(builder, sourceSurfaceRef, destinationSurfaceRef);
      case Kind::ReszDepthResolve:
        return appendReszDepthResolve(builder, sourceSurfaceRef, destinationTextureRef);
      case Kind::GenerateMipmaps:
        return appendGenerateMipmaps(builder, sourceTextureRef);
      case Kind::Present:
        return appendPresent(builder, {}, sourceSurfaceRef);
      case Kind::Count:
        return false;
    }
    return false;
  }
  bool refsBalanced() const noexcept {
    return sourceTexture.refs == 1u && destinationTexture.refs == 1u &&
           buffer.refs == 1u && shader.refs == 1u && declaration.refs == 1u &&
           sourceSurface.refs == 1u && destinationSurface.refs == 1u &&
           query.refs == 1u;
  }
};

void exactFixedMatchesCanonicalAllFamilyWire() {
  FamilyFixture fixture;
  CommandChunkBuilder builder;
  for (const auto& policy : kPeSemanticProducerPolicyTable)
    check(fixture.emit(policy.kind, builder), "all-family source emits");
  const auto sealed = builder.seal();
  check(sealed.valid(), "all-family source seals");
  const std::vector<std::byte> canonical(sealed.blob.begin(), sealed.blob.end());
  const CommandChunkEnvelope envelope{
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = sealed.recordCount,
      .handleCount = sealed.handleCount};
  ImportedChunkView imported;
  check(validateCommandChunk(canonical, envelope, &imported).valid(),
        "all-family source validates");
  const auto allBindingRows = imported.record(0u);
  constexpr std::array<std::uint16_t, 7u> bindingKinds{
      D9C_COMMAND_CHUNK_SECTION_TEXTURE,
      D9C_COMMAND_CHUNK_SECTION_STREAM,
      D9C_COMMAND_CHUNK_SECTION_SHADER,
      D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
      D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
      D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
      D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL};
  check(allBindingRows.sections.size() == bindingKinds.size(),
        "all seven canonical binding families are present in one row");
  for (std::size_t index = 0u; index < bindingKinds.size(); ++index) {
    const auto section = allBindingRows.section(index);
    check(section.descriptor.kind == bindingKinds[index] &&
              section.descriptor.count == 1u &&
              std::any_of(section.payload.begin(), section.payload.end(),
                          [](std::byte value) { return value != std::byte{}; }),
          "canonical binding family row retains ordered nonzero values");
  }

  D9CCommandChunkSegmentedTransportV1 role{};
  role.header = imported.header;
  role.records = toWireHandle(imported.records.data());
  role.recordBytes = static_cast<std::uint32_t>(imported.records.size_bytes());
  role.handles = toWireHandle(imported.handles.data());
  role.handleBytes = static_cast<std::uint32_t>(imported.handles.size_bytes());
  role.payload = toWireHandle(imported.payloadArena.data());
  role.payloadBytes = static_cast<std::uint32_t>(imported.payloadArena.size());
  role.renderTapeCaptureToken = 0x1234u;
  role.renderTapeEventOrdinal = 7u;

  BatchOwner owner;
  check(owner.bindSegmentedOwnership(role), "owner binds exact role geometry");
  std::array<PeCommittedSemanticIdentityValue, 16u> identities{};
  for (std::size_t index = 0u; index < imported.records.size(); ++index) {
    const auto& record = imported.records[index];
    check(record.handleCount <= identities.size(), "identity slice is bounded");
    for (std::size_t offset = 0u; offset < record.handleCount; ++offset) {
      const auto& source = imported.handles[record.firstHandle + offset];
      identities[offset] = {.kind = source.kind, .generation = source.generation,
                            .objectId = source.objectId};
    }
    const auto* policy = peSemanticProducerPolicy(record.type);
    check(policy && owner.append({
        .producer = policy->kind, .recordType = record.type,
        .recordFlags = record.flags,
        .exactPayload = imported.payloadArena.subspan(record.payloadOffset,
                                                       record.payloadSize),
        .identities = {identities.data(), record.handleCount},
        .sourceOrdinal = 100u + index, .recordOrdinal = 1u + index,
        .pendingWitness = index % 2u == 0u, .pendingKey = 0x10u + index,
        .pendingValue = 0x20u + index, .captureWitness = index == 0u,
        .retainerWitness = record.handleCount != 0u}),
        "owner copies an exact canonical family row");
  }
  check(owner.freeze(), "role and semantic batch extents conserve at freeze");
  BatchPlan plan;
  check(planSemanticBatch(owner, plan) &&
            plan.recordCount == imported.records.size() &&
            plan.handleCount == imported.handles.size(),
        "pure plan preserves canonical unique handles");
  std::size_t priorPayloadEnd = 0u;
  for (std::size_t index = 0u; index < plan.recordCount; ++index) {
    const auto& planned = plan.records[index];
    const auto* rule = recordRule(imported.records[index].type);
    check(rule != nullptr &&
              planned.payloadOffset % rule->payloadAlignment == 0u &&
              planned.payloadOffset >= priorPayloadEnd,
          "pure batch plan preserves each record's canonical alignment");
    priorPayloadEnd = planned.payloadOffset + planned.payloadBytes;
  }
  check(plan.payloadBytes == priorPayloadEnd && plan.payloadBytes != 0u,
        "pure batch plan exposes a nonzero contiguous payload frontier");
  alignas(8) std::array<std::byte, 16384u> destination{};
  ExactFixedEmission emission;
  auto forgedPlan = plan;
  ++forgedPlan.records[0].payloadOffset;
  check(!emitExactFixed(owner, forgedPlan, destination, emission),
        "ExactFixed rejects a plan that does not match its immutable owner");
  check(emitExactFixed(owner, plan, destination, emission),
        "ExactFixed emits once");
  check(emission.wireBytes == canonical.size() &&
            std::equal(emission.wire.begin(), emission.wire.end(), canonical.begin()),
        "all 21 families are byte-identical to contiguous D9C V2");
  ImportedChunkView roundTrip;
  check(validateCommandChunk(emission.wire, envelope, &roundTrip).valid(),
        "ExactFixed output passes the production validator");
  builder.resetAndReleaseRetained();
  check(fixture.refsBalanced(), "all-family source releases every retain");
}

void ownershipAndFailureCutsFailClosed() {
  D9CCommandChunkSegmentedTransportV1 malformed{};
  malformed.header.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
  malformed.header.headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  malformed.header.recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  malformed.header.handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  malformed.header.recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  malformed.header.handleTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  malformed.header.payloadArenaOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  malformed.renderTapeCaptureToken = 1u;
  BatchOwner owner;
  check(!owner.bindSegmentedOwnership(malformed),
        "half-bound capture identity rejects before adoption");

  SemanticBatchSettlement pre;
  check(!pre.reserveAll(false) && pre.rollbackPreEffect() &&
            pre.fallbackOnce() && !pre.fallbackOnce(),
        "pre-effect rollback permits one fallback");
  SemanticBatchSettlement partial;
  check(partial.reserveAll(true) && !partial.adoptAll(false) &&
            partial.phase() == BatchPhase::Poisoned &&
            !partial.rollbackPreEffect(),
        "partial adoption is fail-stop");
  SemanticBatchSettlement post;
  check(post.reserveAll(true) && post.adoptAll(true) && post.exactFixed(true) &&
            !post.settle(true, true, false) &&
            post.phase() == BatchPhase::Poisoned,
        "post-emission settlement failure is fail-stop");
}

void preWireCandidateOwnershipAndRetry() {
  D9CSurface source;
  D9CSurface destination;
  const auto sourceRef = qualifyLocalRef<SurfaceRef>(wireRef(
      &source, D9C_CHUNK_HANDLE_KIND_SURFACE, 0xd100u));
  D9CCommandChunkWirePresent command{
      .hwnd = 0x1020304050607080ull,
      .flags = 3u,
      .hasSrc = 1u,
      .hasDst = 1u,
      .sourceHandleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
      .src = D9CRect{1, 2, 31, 42},
      .dst = D9CRect{5, 6, 35, 46},
  };
  const auto candidate = PeOwnedRecordCandidate::present(command, sourceRef);
  command.hwnd = 0u;
  command.src.left = 99;
  check(candidate.valid() && candidate.uniqueHandleCount() == 1u,
        "call-local candidate owns bounded values before source mutation");

  PeSemanticChunkOwner legacyOwner;
  check(legacyOwner.bind(candidate), "legacy owner binds candidate once");
  CommandChunkBuilder legacy;
  check(legacyOwner.emitLegacy(legacy),
        "legacy emitter consumes the owned candidate");
  const auto legacyWire = legacy.seal();
  check(legacyWire.valid(), "legacy candidate wire seals");
  ImportedChunkView legacyImported;
  check(validateCommandChunk(
            std::span<const std::byte>(legacyWire.blob.data(),
                                        legacyWire.blob.size()),
            {D9C_COMMAND_CHUNK_VERSION, legacyWire.recordCount,
             legacyWire.handleCount},
            &legacyImported)
            .valid(),
        "legacy candidate wire validates");
  check(legacyImported.payloadArena.size() >= sizeof(command),
        "candidate preserves fixed payload bytes");
  D9CCommandChunkWirePresent emitted{};
  std::memcpy(&emitted, legacyImported.payloadArena.data(), sizeof(emitted));
  check(emitted.hwnd == 0x1020304050607080ull && emitted.src.left == 1,
        "mutating the source after bind cannot change candidate values");
  legacy.resetAndReleaseRetained();

  PeSemanticChunkOwner exactOwner;
  check(exactOwner.bind(candidate), "exact owner binds the same candidate");
  CommandChunkBuilder exact;
  check(exact.prepareExactFinalLayout(exactOwner.exactLayout()) &&
            exactOwner.emitExact(exact),
        "exact emitter consumes the same candidate without post-wire copy");
  const auto exactWire = exact.seal();
  check(exactWire.valid() &&
            std::equal(exactWire.blob.begin(), exactWire.blob.end(),
                       legacyWire.blob.begin()),
        "legacy and exact candidate sinks are byte-identical");
  check(!exactOwner.emitLegacy(exact),
        "legacy emission cannot mix into an exact candidate chunk");
  exact.resetAndReleaseRetained();

  const auto duplicate = PeOwnedRecordCandidate::readback(
      sourceRef, sourceRef);
  check(duplicate.valid() && duplicate.handleCount() == 2u &&
            duplicate.uniqueHandleCount() == 1u,
        "qualified identity dedup is preserved in the candidate");
  PeSemanticChunkOwner duplicateOwner;
  check(duplicateOwner.bind(duplicate), "duplicate candidate binds once");
  CommandChunkBuilder duplicateExact;
  check(duplicateExact.prepareExactFinalLayout(duplicateOwner.exactLayout()) &&
            duplicateOwner.emitExact(duplicateExact),
        "duplicate identity emits through the exact sink");
  const auto duplicateWire = duplicateExact.seal();
  check(duplicateWire.valid() && duplicateWire.handleCount == 1u,
        "exact sink emits one handle for one qualified identity");
  duplicateExact.resetAndReleaseRetained();

  const auto conflicting = PeOwnedRecordCandidate::readback(
      sourceRef, qualifyLocalRef<SurfaceRef>(wireRef(
          &destination, D9C_CHUNK_HANDLE_KIND_SURFACE, 0xd100u)));
  check(!conflicting.valid() && conflicting.uniqueHandleCount() == 0u,
        "identity collision with a different wrapper fails closed");

  CommandChunkBuilder rollback(
      CommandChunkBuilderCapacities{.records = 1u, .handles = 0u,
                                    .payloadBytes = 128u, .sealedBytes = 128u});
  PeSemanticChunkOwner rollbackOwner;
  check(rollbackOwner.bind(candidate), "rollback owner binds candidate");
  // Deliberately under-plan the final handle table to force a failure after
  // the typed append starts; this exercises the builder checkpoint rather
  // than relying on spare-vector capacities in exact mode.
  check(rollback.prepareExactFinalLayout(planExactCommandChunkLayout(
              1u, 0u, sizeof(D9CCommandChunkWirePresent))) &&
            !rollbackOwner.emitExact(rollback) &&
            rollback.recordCount() == 0u && rollback.handleCount() == 0u &&
            rollbackOwner.bound() && source.refs == 1u,
        "failed exact emission rolls back candidate retention");
  check(rollback.returnToMutableLayout(),
        "failed exact emission restores the legacy sink");

  PeAllFamilySemanticTokenLedger ordinals;
  const auto first = ordinals.beginSource(D9C_COMMAND_RECORD_PRESENT);
  ordinals.preserveForRetry(first);
  check(first != 0u && ordinals.beginSource(D9C_COMMAND_RECORD_PRESENT) == first,
        "pre-effect retry reuses the issued ordinal");
  check(ordinals.beginSource(D9C_COMMAND_RECORD_PRESENT) != first,
        "a fresh append receives a fresh ordinal");

  static_assert(std::is_nothrow_default_constructible_v<PeSemanticChunkOwner>);
  static_assert(sizeof(PeSemanticChunkOwner) <= 160u);
  static_assert(!std::is_constructible_v<PeSemanticChunkOwner, std::size_t>);
}

void callLocalPilotTransactionPlanAndEmission() {
  D9CSurface source;
  D9CSurface destination;
  const auto sourceRef = qualifyLocalRef<SurfaceRef>(wireRef(
      &source, D9C_CHUNK_HANDLE_KIND_SURFACE, 0xe100u));
  const auto destinationRef = qualifyLocalRef<SurfaceRef>(wireRef(
      &destination, D9C_CHUNK_HANDLE_KIND_SURFACE, 0xe200u));
  const PePresentBatch present{
      .command = D9CCommandChunkWirePresent{.hwnd = 0x1234u},
      .source = sourceRef,
  };
  const PeReadbackBatch readback{
      .source = sourceRef,
      .destination = destinationRef,
  };
  PePrewireChunkTransaction owner;
  check(owner.append(PeOwnedRecordCandidate::present(
                        present.command, present.source),
                    {.sourceOrdinal = 11u, .recordOrdinal = 101u,
                     .pendingGeneration = 7u, .pendingKey = 1u,
                     .pendingValue = 2u, .retainerCheckpoint = 3u,
                     .pendingWitness = true, .retainerCheckpointValid = true}),
        "call-local pilot admits first typed candidate");
  check(owner.append(PeOwnedRecordCandidate::readback(
                        readback.source, readback.destination),
                    {.sourceOrdinal = 12u, .recordOrdinal = 102u,
                     .captureToken = 9u, .captureOrdinal = 10u,
                     .captureReserved = true}),
        "call-local pilot admits second typed candidate");
  const auto plan = owner.plan();
  check(plan.valid() && plan.recordCount == 2u &&
            plan.uniqueHandleCount == 3u && owner.size() == 2u,
        "call-local pilot computes bounded multi-record count and dedup plan");
  check(owner.checkpoint(0u).recordOrdinal == 101u &&
            owner.checkpoint(1u).captureOrdinal == 10u,
        "call-local pilot retains source/record checkpoints");

  CommandChunkBuilder occupied;
  check(appendPresent(occupied, {}, sourceRef),
        "occupied builder supplies a pre-effect rollback cut");
  check(!owner.emitExact(occupied) && !owner.emitted() && owner.plan().valid(),
        "non-empty exact destination rejects before adoption and remains retryable");
  occupied.resetAndReleaseRetained();

  CommandChunkBuilder underplanned;
  check(underplanned.prepareExactFinalLayout(
            planExactCommandChunkLayout(2u, 0u, 1u)),
        "under-planned exact layout reserves");
  check(!owner.emitExact(underplanned),
        "under-planned exact emission rejects");
  check(!owner.emitted(), "under-planned owner remains retryable");
  check(!underplanned.exactFinalLayout(),
        "under-planned builder returns to legacy storage");
  check(owner.plan().valid(), "under-planned owner retains its immutable plan");

  CommandChunkBuilder legacy;
  check(owner.emitLegacy(legacy), "call-local pilot legacy fallback emits typed rows");
  const auto legacyWire = legacy.seal();
  check(legacyWire.valid(), "legacy multi-record owner seals");
  CommandChunkBuilder exact;
  check(owner.emitExact(exact),
        "call-local pilot emits the same candidates into final layout once");
  const auto exactWire = owner.seal();
  check(exactWire.valid() && exactWire.blob.size() == legacyWire.blob.size() &&
            std::equal(exactWire.blob.begin(), exactWire.blob.end(),
                       legacyWire.blob.begin()),
        "multi-record exact and legacy bytes are identical");
  check(!owner.emitExact(exact), "exact emission cannot run twice");
  owner.resetAndReleaseRetained();
  legacy.resetAndReleaseRetained();
  exact.resetAndReleaseRetained();
  check(source.refs == 1u && destination.refs == 1u,
        "multi-record owner rollback/reset releases retained identities");

  PePrewireChunkTransaction residual;
  const auto unsupported = PeExactProductionPolicyRow{
      .producer = PeSemanticProducerKind::DrawPrimitive,
      .disposition = PeExactProductionDisposition::LegacyFallback,
      .fallbackReason = PeExactFallbackReason::DeferredStateSettlement};
  check(unsupported.producer == PeSemanticProducerKind::DrawPrimitive &&
            peExactProductionPolicy(unsupported.producer).fallbackReason !=
                PeExactFallbackReason::None,
        "unsupported family remains explicitly classified as residual");
  check(!residual.append(PeOwnedRecordCandidate{},
                         {.sourceOrdinal = 1u, .recordOrdinal = 1u}),
        "untyped residual cannot enter the exact owner");
  for (const auto& row : kPeExactProductionPolicyTable) {
    check(row.producer != PeSemanticProducerKind::Count,
          "all semantic producer rows are classified");
  }
}

void hotStagingFamiliesResetAuthorityAndPreserveAcceptedBytes() {
  using namespace dxmt9::d3d9::pe;
  PeSemanticRecordInput staged{};
  PeSemanticStagingFamily family = PeSemanticStagingFamily::None;
  bool valid = false;
  std::array<D9CCommandChunkWireRenderState, 1u> renderStates{
      D9CCommandChunkWireRenderState{.state = 9u, .value = 0x1234u}};
  const SparseStateInput drawSparse{.renderStates = renderStates};
  const D9CCommandChunkWireDrawHeader firstDraw{
      .primitiveType = 4u, .primitiveCount = 1u};
  D9CSurface staleSurface;
  std::array<std::byte, 4u> staleConstant{};
  staged.surface0 = qualifyLocalRef<SurfaceRef>(wireRef(
      &staleSurface, D9C_CHUNK_HANDLE_KIND_SURFACE, 0xca11u));
  staged.setConst.registerCount = 1u;
  staged.constantBytes = staleConstant;
  stagePeSemanticDrawInput(
      staged, family, valid, PeSemanticProducerKind::DrawPrimitive,
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, firstDraw, drawSparse);
  check(valid && family == PeSemanticStagingFamily::Draw &&
            staged.draw.primitiveType == 4u &&
            staged.sparse.renderStates.size() == 1u &&
            staged.constantBytes.empty() && staged.clearRects.empty() &&
            staged.surface0.object == nullptr,
        "draw adapter publishes only its initialized sparse frontier");

  const D9CCommandChunkWireDrawHeader secondDraw{
      .primitiveType = 5u, .primitiveCount = 2u};
  stagePeSemanticDrawInput(
      staged, family, valid, PeSemanticProducerKind::DrawIndexedPrimitive,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, secondDraw, {});
  check(valid && staged.draw.primitiveType == 5u &&
            staged.draw.primitiveCount == 2u && staged.sparse.renderStates.empty() &&
            staged.constantBytes.empty() && staged.surface0.object == nullptr,
        "same-family draw reuse replaces sparse values without stale authority");

  std::array<std::byte, 16u> acceptedBytes{};
  acceptedBytes[0] = std::byte{0xa1};
  acceptedBytes[15] = std::byte{0xb2};
  PeSemanticConstantRecordInput stagedConstant;
  stagePeSemanticConstantInput(
      stagedConstant, family, valid, PeSemanticProducerKind::VsFloatConstant,
      D9C_COMMAND_RECORD_SET_VS_CONST_F,
      D9CCommandChunkWireSetConst{.startRegister = 3u, .registerCount = 1u},
      acceptedBytes);
  check(valid && family == PeSemanticStagingFamily::Constant &&
            stagedConstant.setConst.startRegister == 3u &&
            stagedConstant.constantBytes.data() == acceptedBytes.data(),
        "constant adapter publishes a narrow input without sparse or ref authority");

  auto accepted = stagedConstant;
  accepted.sourceOrdinal = 1u;
  accepted.recordOrdinal = 1u;
  using Owner = PeSemanticBatchOwner<4u, 8u, 1024u, 4u, 8u>;
  Owner owner;
  check(owner.admit(accepted), "constant adapter input is accepted");
  const auto originalBytes = acceptedBytes;
  acceptedBytes[0] = std::byte{0x5a};
  PeSemanticExactFixedEmission emission;
  check(owner.emitExactFixed(emission), "accepted constant emits exact bytes");
  check(std::search(emission.wire.begin(), emission.wire.end(),
                    originalBytes.begin(), originalBytes.end()) !=
            emission.wire.end(),
        "accepted bytes remain immutable after source mutation");

  stagePeSemanticDrawInput(
      staged, family, valid, PeSemanticProducerKind::DrawPrimitive,
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, firstDraw, {});
  check(valid && family == PeSemanticStagingFamily::Draw &&
            staged.constantBytes.empty() && staged.setConst.registerCount == 0u &&
            staged.sparse.renderStates.empty(),
        "A/B/A family transition clears prior constant bytes");
}

struct StagingAuthorityFixture {
  D9CSurface surface0, surface1;
  D9CTexture texture0, texture1;
  D9CBuffer buffer0, buffer1;
  D9CShader shader0, shader1;
  D9CVertexDecl declaration;
  D9CQuery query;
  std::array<std::byte, 16u> oldConstant{};
  std::array<D9CRect, 1u> oldRects{{D9CRect{10, 11, 12, 13}}};
  std::array<D9CCommandChunkWireRenderState, 1u> oldRenderStates{{{21u, 22u}}};
  std::array<std::byte, 4u> oldUpVertices{};
  std::array<std::byte, 4u> oldUpIndices{};
  PeSemanticRecordInput staged{};

  void seedColdAuthorities() {
    using namespace dxmt9::d3d9::pe;
    staged.producer = PeSemanticProducerKind::Present;
    staged.recordType = D9C_COMMAND_RECORD_PRESENT;
    staged.recordFlags = 0x55u;
    staged.sourceOrdinal = 77u;
    staged.recordOrdinal = 88u;
    staged.draw.primitiveType = 6u;
    staged.setConst = {.startRegister = 7u, .registerCount = 2u};
    staged.clear = {.flags = 0x11u, .colorARGB = 0x22334455u, .z = 0.5f};
    staged.present.hwnd = 0x1234u;
    staged.stretchRect.srcRect = D9CRect{1, 2, 3, 4};
    staged.colorFill.colorARGB = 0xaabbccddu;
    staged.updateSurface.srcRect = D9CRect{5, 6, 7, 8};
    staged.queryIssue.flags = 9u;
    staged.updateFlags = 10u;
    staged.reszFlags = 11u;
    staged.mipmapFlags = 12u;
    staged.surface0 = qualifyLocalRef<SurfaceRef>(wireRef(
        &surface0, D9C_CHUNK_HANDLE_KIND_SURFACE, 0xf001u));
    staged.surface1 = qualifyLocalRef<SurfaceRef>(wireRef(
        &surface1, D9C_CHUNK_HANDLE_KIND_SURFACE, 0xf002u));
    staged.texture0 = qualifyLocalRef<TextureRef>(wireRef(
        &texture0, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0xf003u));
    staged.texture1 = qualifyLocalRef<TextureRef>(wireRef(
        &texture1, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0xf004u));
    staged.buffer0 = qualifyLocalRef<BufferRef>(wireRef(
        &buffer0, D9C_CHUNK_HANDLE_KIND_BUFFER, 0xf005u));
    staged.buffer1 = qualifyLocalRef<BufferRef>(wireRef(
        &buffer1, D9C_CHUNK_HANDLE_KIND_BUFFER, 0xf006u));
    staged.shader0 = qualifyLocalRef<ShaderRef>(wireRef(
        &shader0, D9C_CHUNK_HANDLE_KIND_SHADER, 0xf007u));
    staged.shader1 = qualifyLocalRef<ShaderRef>(wireRef(
        &shader1, D9C_CHUNK_HANDLE_KIND_SHADER, 0xf008u));
    staged.declaration = qualifyLocalRef<DeclarationRef>(wireRef(
        &declaration, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, 0xf009u));
    staged.query = qualifyLocalRef<QueryRef>(wireRef(
        &query, D9C_CHUNK_HANDLE_KIND_QUERY, 0xf00au));
    staged.sparse.renderStates = oldRenderStates;
    staged.sparse.upVertexData = oldUpVertices;
    staged.sparse.upIndexData = oldUpIndices;
    staged.constantBytes = oldConstant;
    staged.clearRects = oldRects;
  }
};

void checkNoStaleSemanticAuthority(const PeSemanticRecordInput& staged,
                                  bool constantFamily = false) {
  if (!constantFamily) {
    check(staged.setConst.startRegister == 0u &&
              staged.setConst.registerCount == 0u && staged.constantBytes.empty(),
          "staging clears stale constant authority outside constant family");
  }
  check(staged.clear.flags == 0u && staged.clear.colorARGB == 0u &&
            staged.clear.z == 0.0f,
        "staging clears stale clear authority");
  check(staged.present.hwnd == 0u && staged.stretchRect.srcRect.left == 0 &&
            staged.colorFill.colorARGB == 0u &&
            staged.updateSurface.srcRect.left == 0 &&
            staged.queryIssue.flags == 0u && staged.updateFlags == 0u &&
            staged.reszFlags == 0u && staged.mipmapFlags == 0u,
        "staging clears stale cold fixed authorities");
  check(staged.surface0.object == nullptr && staged.surface1.object == nullptr &&
            staged.texture0.object == nullptr && staged.texture1.object == nullptr &&
            staged.buffer0.object == nullptr && staged.buffer1.object == nullptr &&
            staged.shader0.object == nullptr && staged.shader1.object == nullptr &&
            staged.declaration.object == nullptr && staged.query.object == nullptr,
        "staging clears all ten stale typed refs");
  check(staged.clearRects.empty(), "staging clears stale rectangle spans");
}

using StagingCompareOwner =
    PeSemanticBatchOwner<8u, 32u, 4096u, 8u, 32u>;

void compareSemanticOwnerBytes(const StagingCompareOwner& actual,
                               const StagingCompareOwner& expected,
                               std::string_view message) {
  PeSemanticExactFixedEmission actualEmission;
  PeSemanticExactFixedEmission expectedEmission;
  check(actual.emitExactFixed(actualEmission) &&
            expected.emitExactFixed(expectedEmission),
        "staged and generic owners emit exact bytes");
  check(actualEmission.wireBytes == expectedEmission.wireBytes &&
            std::equal(actualEmission.wire.begin(), actualEmission.wire.end(),
                       expectedEmission.wire.begin()),
        message);
}

void admitStagedAndFreshGeneric(const PeSemanticRecordInput& staged,
                                PeSemanticRecordInput generic,
                                std::uint64_t ordinal,
                                StagingCompareOwner& actual,
                                StagingCompareOwner& expected,
                                std::string_view message) {
  auto view = borrowPeSemanticRecordInput(staged);
  view.destination.sourceOrdinal = ordinal;
  view.destination.recordOrdinal = ordinal;
  generic.sourceOrdinal = ordinal;
  generic.recordOrdinal = ordinal;
  check(actual.admit(view), "staged input admits");
  check(expected.admit(generic), "fresh generic input admits");
  compareSemanticOwnerBytes(actual, expected, message);
}

void admitStagedAndFreshGeneric(PeSemanticConstantRecordInput staged,
                                PeSemanticRecordInput generic,
                                std::uint64_t ordinal,
                                StagingCompareOwner& actual,
                                StagingCompareOwner& expected,
                                std::string_view message) {
  staged.sourceOrdinal = staged.recordOrdinal = ordinal;
  generic.sourceOrdinal = generic.recordOrdinal = ordinal;
  check(actual.admit(staged), "narrow staged constant input admits");
  check(expected.admit(generic), "fresh generic constant input admits");
  compareSemanticOwnerBytes(actual, expected, message);
}

void stagedFamiliesClearColdAuthoritiesAndMatchGenericAdmission() {
  using namespace dxmt9::d3d9::pe;
  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u, .numVertices = 3u, .primitiveCount = 1u};
  const D9CCommandChunkWireDrawHeader changedDraw{
      .primitiveType = 5u, .numVertices = 6u, .primitiveCount = 2u};
  std::array<D9CCommandChunkWireRenderState, 1u> drawStates{{{31u, 32u}}};
  std::array<D9CCommandChunkWireRenderState, 1u> applyStates{{{41u, 42u}}};
  const SparseStateInput drawSparse{.renderStates = drawStates};
  const SparseStateInput applySparse{.renderStates = applyStates};

  StagingAuthorityFixture fixture;
  fixture.seedColdAuthorities();
  PeSemanticStagingFamily family = PeSemanticStagingFamily::Cold;
  bool valid = false;
  stagePeSemanticDrawInput(
      fixture.staged, family, valid, PeSemanticProducerKind::DrawPrimitive,
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, draw, drawSparse);
  check(valid && family == PeSemanticStagingFamily::Draw &&
            fixture.staged.sparse.renderStates.data() == drawStates.data() &&
            fixture.staged.sparse.upVertexData.empty() &&
            fixture.staged.sparse.upIndexData.empty() &&
            fixture.staged.constantBytes.empty() &&
            fixture.staged.clearRects.empty(),
        "Cold to Draw replaces old sparse and variable authorities");
  checkNoStaleSemanticAuthority(fixture.staged);

  StagingCompareOwner actual;
  StagingCompareOwner expected;
  PeSemanticRecordInput genericDraw{};
  genericDraw.producer = PeSemanticProducerKind::DrawPrimitive;
  genericDraw.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  genericDraw.draw = draw;
  genericDraw.sparse = drawSparse;
  admitStagedAndFreshGeneric(fixture.staged, genericDraw, 1u, actual, expected,
                             "Draw bytes match fresh generic admission");

  stagePeSemanticDrawInput(
      fixture.staged, family, valid,
      PeSemanticProducerKind::DrawIndexedPrimitive,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, changedDraw, {});
  check(valid && family == PeSemanticStagingFamily::Draw &&
            fixture.staged.draw.primitiveType == changedDraw.primitiveType &&
            fixture.staged.sparse.renderStates.empty(),
        "same-family Draw change replaces every active field");
  PeSemanticRecordInput genericChangedDraw{};
  genericChangedDraw.producer = PeSemanticProducerKind::DrawIndexedPrimitive;
  genericChangedDraw.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  genericChangedDraw.draw = changedDraw;
  admitStagedAndFreshGeneric(fixture.staged, genericChangedDraw, 2u, actual,
                             expected,
                             "same-family Draw bytes match generic admission");

  stagePeSemanticDrawInput(
      fixture.staged, family, valid, PeSemanticProducerKind::ApplyState,
      D9C_COMMAND_RECORD_APPLY_STATE, {}, applySparse);
  check(valid && family == PeSemanticStagingFamily::Draw &&
            fixture.staged.sparse.renderStates.data() == applyStates.data(),
        "ApplyState remains in the draw-family staging frontier");
  PeSemanticRecordInput genericApply{};
  genericApply.producer = PeSemanticProducerKind::ApplyState;
  genericApply.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  genericApply.sparse = applySparse;
  admitStagedAndFreshGeneric(fixture.staged, genericApply, 3u, actual, expected,
                             "ApplyState bytes match fresh generic admission");

  StagingAuthorityFixture constantFixture;
  constantFixture.seedColdAuthorities();
  PeSemanticConstantRecordInput stagedConstant;
  family = PeSemanticStagingFamily::Cold;
  std::array<std::byte, 16u> constantA{};
  std::array<std::byte, 16u> constantB{};
  constantA[0] = std::byte{0xa1};
  constantA[15] = std::byte{0xb2};
  constantB[0] = std::byte{0xc3};
  constantB[15] = std::byte{0xd4};
  const D9CCommandChunkWireSetConst setA{
      .startRegister = 3u, .registerCount = 1u};
  const D9CCommandChunkWireSetConst setB{
      .startRegister = 8u, .registerCount = 1u};
  stagePeSemanticConstantInput(
      stagedConstant, family, valid,
      PeSemanticProducerKind::VsFloatConstant,
      D9C_COMMAND_RECORD_SET_VS_CONST_F, setA, constantA);
  check(valid && family == PeSemanticStagingFamily::Constant &&
            stagedConstant.constantBytes.data() == constantA.data() &&
            constantFixture.staged.surface0.object != nullptr &&
            !constantFixture.staged.sparse.upVertexData.empty(),
        "narrow constant staging cannot read dormant generic cold authorities");

  StagingCompareOwner constantActual;
  StagingCompareOwner constantExpected;
  PeSemanticRecordInput genericA{};
  genericA.producer = PeSemanticProducerKind::VsFloatConstant;
  genericA.recordType = D9C_COMMAND_RECORD_SET_VS_CONST_F;
  genericA.setConst = setA;
  genericA.constantBytes = constantA;
  admitStagedAndFreshGeneric(stagedConstant, genericA, 1u,
                             constantActual, constantExpected,
                             "constant A bytes match fresh generic admission");

  stagePeSemanticConstantInput(
      stagedConstant, family, valid,
      PeSemanticProducerKind::PsFloatConstant,
      D9C_COMMAND_RECORD_SET_PS_CONST_F, setB, constantB);
  PeSemanticRecordInput genericB{};
  genericB.producer = PeSemanticProducerKind::PsFloatConstant;
  genericB.recordType = D9C_COMMAND_RECORD_SET_PS_CONST_F;
  genericB.setConst = setB;
  genericB.constantBytes = constantB;
  admitStagedAndFreshGeneric(stagedConstant, genericB, 2u,
                             constantActual, constantExpected,
                             "constant B bytes match fresh generic admission");

  stagePeSemanticConstantInput(
      stagedConstant, family, valid,
      PeSemanticProducerKind::VsFloatConstant,
      D9C_COMMAND_RECORD_SET_VS_CONST_F, setA, constantA);
  PeSemanticRecordInput genericAgainA{};
  genericAgainA.producer = PeSemanticProducerKind::VsFloatConstant;
  genericAgainA.recordType = D9C_COMMAND_RECORD_SET_VS_CONST_F;
  genericAgainA.setConst = setA;
  genericAgainA.constantBytes = constantA;
  admitStagedAndFreshGeneric(stagedConstant, genericAgainA, 3u,
                             constantActual, constantExpected,
                             "constant A-B-A bytes match generic admission");
  check(constantActual.retainedCount() == 0u &&
            constantFixture.surface0.refs == 1u &&
            constantFixture.texture0.refs == 1u,
        "dormant generic refs do not leak into narrow constant admission");
}
}  // namespace

int main() {
  try {
    static_assert(!std::is_copy_constructible_v<BatchOwner>);
    static_assert(!std::is_move_constructible_v<BatchOwner>);
    exactFixedMatchesCanonicalAllFamilyWire();
    ownershipAndFailureCutsFailClosed();
    preWireCandidateOwnershipAndRetry();
    callLocalPilotTransactionPlanAndEmission();
    hotStagingFamiliesResetAuthorityAndPreserveAcceptedBytes();
    stagedFamiliesClearColdAuthoritiesAndMatchGenericAdmission();
  } catch (const Failure& failure) {
    std::cerr << "pe_semantic_batch_spec failed: " << failure.what() << '\n';
    return 1;
  }
  return 0;
}
