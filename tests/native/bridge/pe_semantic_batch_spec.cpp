#include "d3d9_pe_semantic_batch.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "device_c_chunk_validate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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
      case Kind::DrawPrimitive:
        return appendSparseRecord(builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
            D9CCommandChunkWireDrawHeader{.primitiveType = 4u, .primitiveCount = 1u}, {});
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
}  // namespace

int main() {
  try {
    static_assert(!std::is_copy_constructible_v<BatchOwner>);
    static_assert(!std::is_move_constructible_v<BatchOwner>);
    exactFixedMatchesCanonicalAllFamilyWire();
    ownershipAndFailureCutsFailClosed();
  } catch (const Failure& failure) {
    std::cerr << "pe_semantic_batch_spec failed: " << failure.what() << '\n';
    return 1;
  }
  return 0;
}
