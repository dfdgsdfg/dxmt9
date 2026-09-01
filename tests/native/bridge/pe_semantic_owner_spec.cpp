#include "d3d9_pe_semantic_owner.hpp"
#include "d3d9_pe_recorder_transaction.hpp"
#include "d3d9_pe_recorder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

struct RefCounter { std::uint32_t refs = 1u; };
struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};

template <typename T> void addRef(T* value) { ++value->refs; }
template <typename T> std::uint32_t release(T* value) { return --value->refs; }
extern "C" void dxmt9c_surface_addref(D9CSurface* p) { addRef(p); }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* p) { return release(p); }
extern "C" void dxmt9c_texture_addref(D9CTexture* p) { addRef(p); }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* p) { return release(p); }
extern "C" void dxmt9c_buffer_addref(D9CBuffer* p) { addRef(p); }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* p) { return release(p); }
extern "C" void dxmt9c_shader_addref(D9CShader* p) { addRef(p); }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* p) { return release(p); }
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* p) { addRef(p); }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* p) { return release(p); }
extern "C" void dxmt9c_query_addref(D9CQuery* p) { addRef(p); }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* p) { return release(p); }

namespace {
using namespace dxmt9::d3d9::pe;
using Owner = PeSemanticBatchOwner<22u, 8u, 128u, 8u, 8u>;
using DefaultOwner = PeSemanticBatchOwner<>;
using ProductionOwner = PeSemanticBatchOwner<256u, 256u, 1310720u, 1024u, 2048u>;

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
void check(bool ok, std::string_view text) { if (!ok) throw Failure(std::string(text)); }

// Ref wrappers predate this owner and intentionally have no element_type.
template <std::uint32_t Kind, typename Object>
PeLocalObjectRef<Kind> localRef(Object* object, std::uint32_t id,
                                std::uint32_t generation = 1u) {
  PeLocalObjectRef<Kind> out{};
  out.identity = {.kind = Kind, .generation = generation, .objectId = id};
  out.object = object;
  return out;
}

PeSemanticRecordInput base(PeSemanticProducerKind producer,
                           std::uint64_t source, std::uint64_t ordinal) {
  const auto& row = kPeSemanticProducerPolicyTable[
      static_cast<std::size_t>(producer)];
  return {.producer = producer, .recordType = row.recordType,
          .sourceOrdinal = source, .recordOrdinal = ordinal};
}

void classificationAndLifetime() {
  Owner owner;
  CommandChunkBuilder canonicalBuilder;
  D9CSurface surface;
  D9CTexture texture;
  D9CBuffer buffer;
  D9CShader shader;
  D9CVertexDecl decl;
  D9CQuery query;
  const auto s = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 1u);
  const auto t = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&texture, 2u);
  const auto q = localRef<D9C_CHUNK_HANDLE_KIND_QUERY>(&query, 6u);
  const std::array<std::byte, 16u> constant{};
  for (const auto& row : kPeSemanticProducerPolicyTable) {
    auto in = base(row.kind, 100u + static_cast<unsigned>(row.kind),
                   200u + static_cast<unsigned>(row.kind));
    switch (row.kind) {
      case PeSemanticProducerKind::Present: in.surface0 = s; break;
      case PeSemanticProducerKind::StretchRect:
      case PeSemanticProducerKind::UpdateSurface:
      case PeSemanticProducerKind::Readback: in.surface0 = s; in.surface1 = s; break;
      case PeSemanticProducerKind::ColorFill: in.surface0 = s; break;
      case PeSemanticProducerKind::UpdateTexture: in.texture0 = t; in.texture1 = t; break;
      case PeSemanticProducerKind::QueryIssue: in.query = q; break;
      case PeSemanticProducerKind::ReszDepthResolve: in.surface0 = s; in.texture0 = t; break;
      case PeSemanticProducerKind::GenerateMipmaps: in.texture0 = t; break;
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        in.setConst.registerCount = 1u;
        in.constantBytes = std::span<const std::byte>(constant).first(
            (row.kind == PeSemanticProducerKind::VsBoolConstant ||
             row.kind == PeSemanticProducerKind::PsBoolConstant) ? 4u : 16u);
        break;
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        in.texture0 = row.kind == PeSemanticProducerKind::ApplyState ? t : TextureRef{};
        break;
      case PeSemanticProducerKind::Clear: in.clear.rectCount = 0u; break;
      case PeSemanticProducerKind::Count: break;
    }
    if (!owner.admit(in)) {
      std::cerr << "semantic admission failed for producer "
                << static_cast<unsigned>(row.kind) << "\n";
      throw Failure("every semantic producer row admits a typed slot");
    }
    bool canonical = false;
    switch (row.kind) {
      case PeSemanticProducerKind::Present:
        canonical = appendPresent(canonicalBuilder, in.present, in.surface0);
        break;
      case PeSemanticProducerKind::StretchRect:
        canonical = appendStretchRect(canonicalBuilder, in.stretchRect,
                                      in.surface0, in.surface1);
        break;
      case PeSemanticProducerKind::ColorFill:
        canonical = appendColorFill(canonicalBuilder, in.colorFill, in.surface0);
        break;
      case PeSemanticProducerKind::UpdateTexture:
        canonical = appendUpdateTexture(canonicalBuilder, in.texture0, in.texture1);
        break;
      case PeSemanticProducerKind::UpdateSurface:
        canonical = appendUpdateSurface(canonicalBuilder, in.updateSurface,
                                        in.surface0, in.surface1);
        break;
      case PeSemanticProducerKind::QueryIssue:
        canonical = appendQueryIssue(canonicalBuilder, in.queryIssue.flags, in.query);
        break;
      case PeSemanticProducerKind::Readback:
        canonical = appendReadback(canonicalBuilder, in.surface0, in.surface1);
        break;
      case PeSemanticProducerKind::ReszDepthResolve:
        canonical = appendReszDepthResolve(canonicalBuilder, in.surface0, in.texture0);
        break;
      case PeSemanticProducerKind::GenerateMipmaps:
        canonical = appendGenerateMipmaps(canonicalBuilder, in.texture0);
        break;
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        canonical = appendSetConstants(canonicalBuilder, in.recordType,
                                       in.setConst.startRegister,
                                       in.setConst.registerCount,
                                       in.constantBytes);
        break;
      case PeSemanticProducerKind::Clear:
        canonical = appendClear(canonicalBuilder, in.clear, in.clearRects);
        break;
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        canonical = appendSparseRecord(canonicalBuilder, in.recordType, in.draw,
                                       in.sparse);
        break;
      case PeSemanticProducerKind::Count:
        break;
    }
    check(canonical, "canonical builder emits every semantic producer row");
  }
  check(owner.size() == 21u && owner.retainedCount() == 3u,
        "all-family owner retains each typed identity exactly once");
  std::size_t visited = 0u;
  check(owner.visitOwnedRecords([&](PeSemanticProducerKind kind,
                                    const PeSemanticRecordSlot& slot) noexcept {
          ++visited;
          return kind == slot.producer;
        }) && visited == 21u,
        "all-family typed records have a pure visitor");
  check(surface.refs == 3u && texture.refs == 3u && query.refs == 3u,
        "pins are acquired at admission");
  alignas(8) std::array<std::byte, 8192u> exact{};
  PeSemanticExactFixedEmission exactEmission;
  check(owner.emitExactFixed(exact, exactEmission),
        "all 21 typed records emit through ExactFixed");
  std::array<std::byte, 1024u> segmentedRecords{};
  std::array<std::byte, 1024u> segmentedHandles{};
  std::array<std::byte, 4096u> segmentedPayload{};
  PeSemanticSegmentedEmission segmented;
  check(owner.emitSegmented(segmentedRecords, segmentedHandles, segmentedPayload,
                            segmented),
        "all 21 typed records emit through segmented roles");
  check(exactEmission.transport.header.recordCount == 21u &&
            segmented.transport.header.recordCount == 21u &&
            exactEmission.transport.header.handleCount ==
                segmented.transport.header.handleCount &&
            std::equal(exact.begin() + exactEmission.transport.header.recordTableOffset,
                       exact.begin() + exactEmission.transport.header.recordTableOffset +
                           segmented.transport.recordBytes,
                       segmentedRecords.begin()) &&
            std::equal(exact.begin() + exactEmission.transport.header.handleTableOffset,
                       exact.begin() + exactEmission.transport.header.handleTableOffset +
                           segmented.transport.handleBytes,
                       segmentedHandles.begin()) &&
            std::equal(exact.begin() + exactEmission.transport.header.payloadArenaOffset,
                       exact.begin() + exactEmission.transport.header.payloadArenaOffset +
                           segmented.transport.payloadBytes,
                       segmentedPayload.begin()),
        "segmented and ExactFixed regions conserve identical bytes");
  const auto sealed = canonicalBuilder.seal();
  check(sealed.valid() && sealed.blob.size() == exactEmission.wireBytes &&
            std::equal(sealed.blob.begin(), sealed.blob.end(),
                       exactEmission.wire.begin()),
        "owner ExactFixed bytes equal canonical builder bytes");
  owner.reset();
  check(surface.refs == 2u && texture.refs == 2u && buffer.refs == 1u &&
            shader.refs == 1u && decl.refs == 1u && query.refs == 2u,
        "reset releases every typed pin");
  canonicalBuilder.resetAndReleaseRetained();
  check(surface.refs == 1u && texture.refs == 1u && query.refs == 1u,
        "canonical comparison builder releases every retain");
}

void partialRectClearMatchesCanonicalBuilder() {
  Owner owner;
  const std::array<D9CRect, 2u> rects{{
      {1, 2, 31, 42},
      {7, 11, 53, 67},
  }};
  auto input = base(PeSemanticProducerKind::Clear, 1u, 1u);
  input.clear.rectCount = static_cast<std::uint32_t>(rects.size());
  input.clear.rectOffset = sizeof(D9CCommandChunkWireClear);
  input.clearRects = rects;
  check(owner.admit(input), "partial-rect clear admission");

  CommandChunkBuilder canonical;
  check(appendClear(canonical, input.clear, input.clearRects),
        "partial-rect canonical clear emission");
  const auto sealed = canonical.seal();
  alignas(8) std::array<std::byte, 4096u> bytes{};
  PeSemanticExactFixedEmission emission;
  check(sealed.valid() && owner.emitExactFixed(bytes, emission) &&
            emission.wireBytes == sealed.blob.size() &&
            std::equal(sealed.blob.begin(), sealed.blob.end(),
                       emission.wire.begin()),
        "partial-rect semantic clear equals canonical bytes");
  owner.reset();
  canonical.resetAndReleaseRetained();
}

void duplicateSurfacePinSmoke() {
  Owner owner;
  D9CSurface surface;
  auto input = base(PeSemanticProducerKind::Present, 1u, 1u);
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x41u);
  check(owner.admit(input), "duplicate surface pin admission");
  input = base(PeSemanticProducerKind::StretchRect, 2u, 2u);
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x41u);
  input.surface1 = input.surface0;
  check(owner.admit(input), "duplicate surface pin admission");
  owner.reset();
}

void collisionCopyOverflowAndRetry() {
  using Tiny = PeSemanticBatchOwner<2u, 1u, 4u, 1u, 1u>;
  Tiny owner;
  D9CSurface first, second;
  auto in = base(PeSemanticProducerKind::Present, 1u, 1u);
  in.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&first, 9u);
  check(owner.admit(in), "first admission");
  auto duplicate = in;
  duplicate.sourceOrdinal = 2u;
  duplicate.recordOrdinal = 2u;
  duplicate.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&second, 9u);
  duplicate.surface0.identity.generation = 2u;
  check(!owner.admit(duplicate) && owner.size() == 1u && second.refs == 1u,
        "generation collision rejects atomically");
  auto nullIdentity = in;
  nullIdentity.sourceOrdinal = 2u;
  nullIdentity.recordOrdinal = 2u;
  nullIdentity.surface0.object = nullptr;
  check(!owner.admit(nullIdentity) && owner.size() == 1u,
        "null optional object with a nonzero identity fails closed");
  auto missingIdentity = in;
  missingIdentity.sourceOrdinal = 2u;
  missingIdentity.recordOrdinal = 2u;
  missingIdentity.surface0.object = &second;
  missingIdentity.surface0.identity = {};
  check(!owner.admit(missingIdentity) && owner.size() == 1u &&
            second.refs == 1u,
        "nonnull optional object with a default identity fails closed");
  D9CTexture partialSource;
  D9CTexture partialCollision;
  auto partial = base(PeSemanticProducerKind::UpdateTexture, 2u, 2u);
  partial.texture0 = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&partialSource, 10u);
  partial.texture1 = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &partialCollision, 10u, 2u);
  check(!owner.admit(partial) && owner.size() == 1u &&
            partialSource.refs == 1u && partialCollision.refs == 1u,
        "partial typed pin admission rolls back without a leaked retain");
  auto same = in;
  same.sourceOrdinal = 3u;
  same.recordOrdinal = 3u;
  check(owner.admit(same) && owner.retainedCount() == 1u && first.refs == 2u,
        "duplicate identity does not duplicate retain");
  auto overflow = in;
  overflow.sourceOrdinal = 4u;
  overflow.recordOrdinal = 4u;
  const std::array<std::byte, 8u> overflowBytes{};
  overflow.constantBytes = overflowBytes;
  overflow.setConst.registerCount = 1u;
  overflow.producer = PeSemanticProducerKind::VsBoolConstant;
  overflow.recordType = D9C_COMMAND_RECORD_SET_VS_CONST_B;
  check(owner.size() == Tiny::maxRecords,
        "owner is full before deterministic failed-admission regression");
  check(!owner.admit(overflow) && owner.size() == 2u,
        "full-owner failed admission rolls back without disturbing prior records");
  check(owner.settle() && owner.retainedCount() == 1u && first.refs == 2u,
        "settlement preserves the warm pin and permits retry");
  check(owner.admit(in) && owner.size() == 1u, "reset/retry admits cleanly");
  owner.reset();
  check(first.refs == 1u && second.refs == 1u, "no leaked retains after retry");

  std::array<std::byte, 4u> copiedBytes{std::byte{0x11}, std::byte{0x22},
                                        std::byte{0x33}, std::byte{0x44}};
  auto constant = base(PeSemanticProducerKind::VsBoolConstant, 8u, 8u);
  constant.setConst.registerCount = 1u;
  constant.constantBytes = copiedBytes;
  check(owner.admit(constant), "constant admission copies borrowed bytes");
  copiedBytes[0] = std::byte{0x7f};
  check(owner.constantBytes(owner.record(0u))[0] == std::byte{0x11},
        "constant arena isolates borrowed input mutation");
  owner.reset();

  std::array<std::byte, 2u> up{std::byte{0x31}, std::byte{0x32}};
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u> sparseTexture{};
  sparseTexture[0].wire.valid = 1u;
  sparseTexture[0].wire.slot = 0u;
  sparseTexture[0].object = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &partialSource, 20u);
  auto upRecord = base(PeSemanticProducerKind::DrawPrimitiveUp, 9u, 9u);
  upRecord.sparse.upVertexData = up;
  upRecord.sparse.textures = sparseTexture;
  check(owner.admit(upRecord), "UP and sparse typed arenas admit");
  up[0] = std::byte{0x7e};
  sparseTexture[0].wire.slot = 4u;
  check(owner.upVertexBytes(owner.record(0u))[0] == std::byte{0x31} &&
            owner.textureBindings(owner.record(0u)).size() == 1u &&
            owner.textureBindings(owner.record(0u))[0].wire.slot == 0u,
        "UP and sparse inputs are copied into owned arenas");
  owner.reset();
  check(partialSource.refs == 1u, "sparse pin is released at reset");

  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
      missingSparseObject{};
  missingSparseObject[0].wire.slot = 0u;
  missingSparseObject[0].wire.valid = 1u;
  auto malformedSparse = base(PeSemanticProducerKind::DrawPrimitive, 9u, 9u);
  malformedSparse.sparse.textures = missingSparseObject;
  check(owner.admit(malformedSparse) && owner.retainedCount() == 0u,
        "valid sparse binding without a physical object represents unbind");
  owner.reset();

  owner.admit(upRecord);
  check(owner.record(0u).upVertexBytes.offset == 0u &&
            partialSource.refs == 2u,
        "retry reacquires a released sparse pin at offset zero");
  const std::array<std::byte, 3u> overflowingUp{};
  auto failedUp = upRecord;
  failedUp.sourceOrdinal = 10u;
  failedUp.recordOrdinal = 10u;
  failedUp.sparse.upVertexData = overflowingUp;
  check(!owner.admit(failedUp) && owner.size() == 1u &&
            owner.record(0u).upVertexBytes.offset == 0u,
        "UP arena overflow rolls back the attempted admission");
  owner.reset();
  check(partialSource.refs == 1u, "failed UP admission leaves no leaked pin");
  auto finalRetry = upRecord;
  finalRetry.sourceOrdinal = 1u;
  finalRetry.recordOrdinal = 1u;
  check(owner.admit(finalRetry) && owner.record(0u).upVertexBytes.offset == 0u &&
            partialSource.refs == 2u,
        "reset/retry restarts UP offsets and reacquires the pin");
  owner.reset();
  }

void emissionSmoke() {
  Owner owner;
  D9CSurface surface;
  auto input = base(PeSemanticProducerKind::Present, 1u, 1u);
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x55u);
  check(owner.admit(input), "emission owner admission");
  alignas(8) std::array<std::byte, 4096u> exact{};
  PeSemanticExactFixedEmission exactEmission;
  check(owner.emitExactFixed(exact, exactEmission) && exactEmission.valid(),
        "owner ExactFixed emission");
  std::array<std::byte, 128u> records{};
  std::array<std::byte, 128u> handles{};
  std::array<std::byte, 512u> payload{};
  PeSemanticSegmentedEmission segmented;
  check(owner.emitSegmented(records, handles, payload, segmented) &&
            segmented.valid(), "owner segmented emission");
  PeSemanticSegmentedEmission selfSegmented;
  check(owner.emitSegmented(selfSegmented) && selfSegmented.valid() &&
            selfSegmented.transport.header.recordCount ==
                segmented.transport.header.recordCount &&
            selfSegmented.transport.header.handleCount ==
                segmented.transport.header.handleCount &&
            selfSegmented.transport.recordBytes == segmented.transport.recordBytes &&
            selfSegmented.transport.handleBytes == segmented.transport.handleBytes &&
            selfSegmented.transport.payloadBytes == segmented.transport.payloadBytes,
        "owner production segmented emission uses fixed storage");
  owner.reset();

  Owner sparseOwner;
  D9CTexture texture;
  D9CBuffer buffer;
  D9CShader shader;
  D9CVertexDecl declaration;
  std::array<D9CCommandChunkWireRenderState, 1u> renderStates{{{7u, 9u}}};
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u> textures{{
      {.wire = {.slot = 0u, .valid = 1u, .handleIndex = 0u, .reserved0 = 0u},
       .object = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&texture, 0x66u)}}};
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u> streams{{
      {.wire = {.slot = 0u, .valid = 1u, .handleIndex = 0u, .offset = 0u,
                .stride = 4u, .frequency = 0u, .reserved0 = 0u},
       .object = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(&buffer, 0x67u)}}};
  std::array<SparseBindingInput<D9CCommandChunkWireShaderBinding>, 1u> shaders{{
      {.wire = {.stage = 0u, .valid = 1u, .handleIndex = 0u, .reserved0 = 0u},
       .object = localRef<D9C_CHUNK_HANDLE_KIND_SHADER>(&shader, 0x68u)}}};
  std::array<SparseBindingInput<D9CCommandChunkWireVertexInput>, 1u> vertexInputs{{
      {.wire = {.valid = 1u,
                .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION,
                .value = 0u, .handleIndex = 0u},
       .object = localRef<D9C_CHUNK_HANDLE_KIND_VERTEX_DECL>(&declaration, 0x69u)}}};
  std::array<SparseBindingInput<D9CCommandChunkWireIndexBinding>, 1u> indexBuffers{{
      {.wire = {.valid = 1u, .handleIndex = 0u},
       .object = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(&buffer, 0x67u)}}};
  std::array<SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>, 1u> targets{{
      {.wire = {.slot = 0u, .valid = 1u, .handleIndex = 0u, .reserved0 = 0u},
       .object = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x55u)}}};
  std::array<SparseBindingInput<D9CCommandChunkWireDepthStencilBinding>, 1u> depth{{
      {.wire = {.valid = 1u, .handleIndex = 0u},
       .object = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x55u)}}};
  std::array<std::byte, 16u> sparseConstant{};
  auto draw = base(PeSemanticProducerKind::DrawPrimitive, 2u, 2u);
  draw.draw.primitiveCount = 1u;
  draw.sparse.vsFloatConstants.registerCount = 1u;
  draw.sparse.vsFloatConstants.registerBytes =
      sparseConstant;
  draw.sparse.renderStates = renderStates;
  draw.sparse.textures = textures;
  draw.sparse.streams = streams;
  draw.sparse.shaders = shaders;
  draw.sparse.vertexInputs = vertexInputs;
  draw.sparse.indexBuffers = indexBuffers;
  draw.sparse.renderTargets = targets;
  draw.sparse.depthStencils = depth;
  check(sparseOwner.admit(draw), "all typed sparse emission admission");
  CommandChunkBuilder sparseCanonical;
  check(appendSparseRecord(sparseCanonical, draw.recordType, draw.draw, draw.sparse),
        "canonical builder emits typed sparse record");
  alignas(8) std::array<std::byte, 8192u> sparseExact{};
  PeSemanticExactFixedEmission sparseEmission;
  check(sparseOwner.emitExactFixed(sparseExact, sparseEmission),
        "all typed sparse ExactFixed emission");
  const auto sparseSealed = sparseCanonical.seal();
  check(sparseSealed.valid() && sparseSealed.blob.size() == sparseEmission.wireBytes &&
            std::equal(sparseSealed.blob.begin(), sparseSealed.blob.end(),
                       sparseEmission.wire.begin()),
        "typed sparse ExactFixed bytes equal canonical builder bytes");
  sparseOwner.reset();
  sparseCanonical.resetAndReleaseRetained();

  Owner nullableOwner;
  std::array<SparseBindingInput<D9CCommandChunkWireShaderBinding>, 2u>
      nullableShaders{{
          {.wire = {.stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
                    .valid = 1u,
                    .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
                    .reserved0 = 0u}},
          {.wire = {.stage = D9C_COMMAND_CHUNK_SHADER_STAGE_PIXEL,
                    .valid = 1u,
                    .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
                    .reserved0 = 0u}},
      }};
  auto nullableDraw = base(PeSemanticProducerKind::DrawPrimitive, 3u, 3u);
  nullableDraw.draw.primitiveCount = 1u;
  nullableDraw.sparse.shaders = nullableShaders;
  check(nullableOwner.admit(nullableDraw),
        "valid sparse bindings admit null handles for unbind");
  CommandChunkBuilder nullableCanonical;
  check(appendSparseRecord(nullableCanonical, nullableDraw.recordType,
                           nullableDraw.draw, nullableDraw.sparse),
        "canonical builder admits the same null shader bindings");
  PeSemanticExactFixedEmission nullableEmission;
  check(nullableOwner.emitExactFixed(nullableEmission),
        "nullable sparse owner emits ExactFixed bytes");
  const auto nullableSealed = nullableCanonical.seal();
  check(nullableSealed.valid() &&
            nullableSealed.blob.size() == nullableEmission.wireBytes &&
            std::equal(nullableSealed.blob.begin(), nullableSealed.blob.end(),
                       nullableEmission.wire.begin()),
        "nullable sparse ExactFixed bytes equal canonical builder bytes");
  nullableOwner.reset();
  nullableCanonical.resetAndReleaseRetained();
}

void semanticBridgePreRetryPreservesOwnerBytes() {
  Owner owner;
  D9CSurface surface;
  auto input = base(PeSemanticProducerKind::Present, 7u, 7u);
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x77u);
  check(owner.admit(input), "bridge-pre retry owner admission");

  PeRecorderChunkTransaction transaction;
  check(transaction.beginChunk() &&
            transaction.noteRecord(input.recordType, 64u, {}, 0u, 0u, 0u, 0u) &&
            transaction.recordEmitResult(true) && transaction.recordSealResult(true),
        "semantic lane reaches sealed transaction state");
  std::size_t handleCount = 0u;
  std::size_t payloadBytes = 0u;
  std::size_t wireBytes = 0u;
  check(owner.emissionMetrics(handleCount, payloadBytes, wireBytes) &&
            transaction.recordSealedEvidence(owner.size(), handleCount,
                                             payloadBytes, owner.retainedCount()),
        "semantic lane records sealed evidence");
  alignas(8) std::array<std::byte, 4096u> firstBytes{};
  PeSemanticExactFixedEmission first;
  check(owner.emitExactFixed(firstBytes, first), "semantic lane first emission");
  check(transaction.recordBridgePreEffectFailure() && transaction.retryable(),
        "bridge-pre fault preserves semantic retry state");
  check(!transaction.noteRecord(input.recordType, 64u, {}, 0u, 0u, 0u, 0u),
        "bridge-pre retry rejects a fresh record before exact drain");
  check(owner.retainedCount() == 1u && owner.record(0u).sourceOrdinal == 7u,
        "bridge-pre fault preserves owner identity and pin");
  check(transaction.reopenBridgePreEffectRetry() &&
            transaction.recordSealResult(true),
        "semantic lane reopens the same bridge-pre retry");
  std::size_t retryHandleCount = 0u;
  std::size_t retryPayloadBytes = 0u;
  std::size_t retryWireBytes = 0u;
  check(owner.emissionMetrics(retryHandleCount, retryPayloadBytes,
                              retryWireBytes) &&
            retryHandleCount == handleCount && retryPayloadBytes == payloadBytes &&
            retryWireBytes == wireBytes &&
            transaction.recordSealedEvidence(owner.size(), retryHandleCount,
                                             retryPayloadBytes,
                                             owner.retainedCount()),
        "retry reuses exact semantic evidence");
  alignas(8) std::array<std::byte, 4096u> retryBytes{};
  PeSemanticExactFixedEmission retry;
  check(owner.emitExactFixed(retryBytes, retry) &&
            retry.wireBytes == first.wireBytes &&
            std::equal(first.wire.begin(), first.wire.end(), retry.wire.begin()),
        "bridge-pre retry preserves exact semantic bytes");
  check(transaction.recordBridgeResult(true) &&
            transaction.phase() == RecorderChunkTransactionPhase::BridgeAccepted &&
            transaction.recordCaptureResult(RecorderChunkCaptureDisposition::Skipped) &&
            transaction.recordCapacityPostResult(true) && transaction.complete(),
        "semantic bridge-pre retry settles exactly once");
  owner.reset();
  check(surface.refs == 1u, "semantic bridge-pre retry releases one pin");
}

void semanticRetryPredicateTruthTable() {
  check(peRecorderRetryBytesReady(false, true, true),
        "semantic-only emission reopens retry");
  check(!peRecorderRetryBytesReady(false, false, true),
        "empty semantic owner is not retry bytes");
  check(peRecorderRetryBytesReady(true, false, true),
        "legacy sealed bytes reopen retry");
  check(!peRecorderRetryBytesReady(true, true, false),
        "non-retryable transaction never reopens");
}

void productionCapacityMatchesLegacyCadence() {
  static_assert(sizeof(ProductionOwner) <= 512u);
  ProductionOwner owner;
  D9CSurface surface;
  const auto surfaceRef = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x256u);
  for (std::uint64_t ordinal = 1u; ordinal <= 256u; ++ordinal) {
    auto input = base(PeSemanticProducerKind::Present, ordinal, ordinal);
    input.surface0 = surfaceRef;
    check(owner.admit(input), "production owner admits every 256-record slot");
  }
  check(owner.size() == 256u && owner.retainedCount() == 1u && surface.refs == 2u,
        "production owner preserves 256-record cadence and dedup pin");
  std::size_t handles = 0u;
  std::size_t payload = 0u;
  std::size_t wire = 0u;
  PeSemanticExactFixedEmission emission;
  check(owner.emissionMetrics(handles, payload, wire) &&
            owner.emitExactFixed(emission) && emission.valid() &&
            emission.transport.header.recordCount == 256u,
        "production owner emits the full 256-record chunk");
  std::size_t repeatHandles = 0u;
  std::size_t repeatPayload = 0u;
  std::size_t repeatWire = 0u;
  for (unsigned i = 0u; i < 8u; ++i) {
    check(owner.emissionMetrics(repeatHandles, repeatPayload, repeatWire) &&
              repeatHandles == handles && repeatPayload == payload &&
              repeatWire == wire,
          "cached metrics remain stable across repeated reads");
  }
  const auto storageBytes = ProductionOwner::storageBytes;
  check(owner.settle() && owner.retainedCount() == 1u && surface.refs == 2u &&
            owner.clearedBytesLastBoundary() < storageBytes,
        "successful settlement clears only used production ranges");
  owner.reset();
  check(surface.refs == 1u && owner.size() == 0u && owner.retainedCount() == 0u,
        "production owner reset releases the cadence test pin");
}

void warmRetainerCapacityDistinguishesNovelObjects() {
  using Tiny = PeSemanticBatchOwner<2u, 1u, 128u, 1u, 1u>;
  Tiny owner;
  D9CSurface warm, novel;
  auto first = base(PeSemanticProducerKind::Present, 1u, 1u);
  first.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&warm, 0x901u);
  check(owner.admit(first) && owner.settle() && owner.retainedCount() == 1u &&
            warm.refs == 2u,
        "full retainer reaches a warm epoch");
  auto duplicate = first;
  duplicate.sourceOrdinal = 2u;
  duplicate.recordOrdinal = 2u;
  check(owner.admit(duplicate) && owner.size() == 1u && warm.refs == 2u,
        "full-capacity warm duplicate refreshes without AddRef");
  auto rejected = duplicate;
  rejected.sourceOrdinal = 3u;
  rejected.recordOrdinal = 3u;
  rejected.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&novel, 0x902u);
  check(!owner.admit(rejected) && owner.size() == 1u && novel.refs == 1u &&
            warm.refs == 2u,
        "current-chunk pin capacity rejects a novel object atomically");
  check(owner.settle() && owner.admit(rejected) && novel.refs == 2u &&
            warm.refs == 2u,
        "a new chunk admits a novel pin while the prior warm epoch survives");
  owner.reset();
  check(owner.retainedCount() == 0u && warm.refs == 1u && novel.refs == 1u,
        "destructive reset releases warm and rejected pins");
}

void settledOwnerCannotExposePriorRanges() {
  Owner owner;
  D9CSurface surface;
  std::array<std::byte, 4u> priorBytes{
      std::byte{0xa5}, std::byte{0xa5}, std::byte{0xa5}, std::byte{0xa5}};
  auto prior = base(PeSemanticProducerKind::DrawPrimitiveUp, 1u, 1u);
  prior.sparse.upVertexData = priorBytes;
  check(owner.admit(prior), "stale-range source admission");
  PeSemanticExactFixedEmission priorEmission;
  check(owner.emitExactFixed(priorEmission), "stale-range source emission");
  check(owner.settle() && owner.clearedBytesLastBoundary() != 0u,
        "settlement clears used source ranges only");

  auto next = base(PeSemanticProducerKind::Present, 2u, 2u);
  next.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x933u);
  check(owner.admit(next), "stale-range next admission");
  PeSemanticExactFixedEmission nextEmission;
  check(owner.emitExactFixed(nextEmission), "stale-range next emission");

  Owner fresh;
  D9CSurface freshSurface;
  auto freshNext = base(PeSemanticProducerKind::Present, 2u, 2u);
  freshNext.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&freshSurface, 0x933u);
  check(fresh.admit(freshNext), "fresh-range comparison admission");
  PeSemanticExactFixedEmission freshEmission;
  check(fresh.emitExactFixed(freshEmission) &&
            nextEmission.wireBytes == freshEmission.wireBytes &&
            std::equal(nextEmission.wire.begin(), nextEmission.wire.end(),
                       freshEmission.wire.begin()),
        "settled owner output matches fresh owner without capacity memset");
  owner.reset();
  fresh.reset();
  check(surface.refs == 1u && freshSurface.refs == 1u,
        "stale-range owners release all pins");
}

void emptyOwnerStartsAtZeroCadence() {
  Owner owner;
  std::size_t handles = 9u;
  std::size_t payload = 9u;
  std::size_t wire = 9u;
  check(resolvePeSemanticCadenceMetrics(owner, handles, payload, wire) &&
            handles == 0u && payload == 0u && wire == 0u,
        "empty production owner exposes a valid zero CapacityPre cadence");

  auto present = base(PeSemanticProducerKind::Present, 1u, 1u);
  D9CSurface surface;
  present.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x944u);
  check(owner.admit(present) &&
            resolvePeSemanticCadenceMetrics(owner, handles, payload, wire) &&
            handles == 1u && payload != 0u && wire != 0u,
        "non-empty production owner exposes its exact cadence metrics");
  owner.reset();
  check(surface.refs == 1u, "cadence fixture releases its pin");
}

void committedLeaseQualificationUsesOwnerPins() {
  Owner owner;
  D9CSurface surface;
  D9CTexture source;
  D9CTexture destination;
  auto present = base(PeSemanticProducerKind::Present, 1u, 1u);
  present.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x951u, 3u);
  auto update = base(PeSemanticProducerKind::UpdateTexture, 2u, 2u);
  update.texture0 =
      localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&source, 0x952u, 4u);
  update.texture1 =
      localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&destination, 0x953u, 5u);
  check(owner.admit(present) && owner.admit(update),
        "lease fixture admits committed owner records");

  std::size_t visited = 0u;
  bool localPointersValid = true;
  owner.visitCommittedPendingChunkLeases(
      [&](const CommittedPendingChunkLease& lease) noexcept {
        localPointersValid = localPointersValid &&
            lease.object().object != nullptr;
        ++visited;
      });
  check(localPointersValid && visited == 3u,
        "owner enumerates each committed exact wire identity once");

  bool matched = false;
  check(owner.visitCommittedPendingChunkLease(
            present.surface0,
            [&](const CommittedPendingChunkLease& lease) noexcept {
              matched = lease.object().object == &surface &&
                  lease.object().identity.generation == 3u;
              return matched;
            }) && matched,
        "owner issues a lease only for matching identity and wrapper");
  auto wrongGeneration = present.surface0;
  ++wrongGeneration.identity.generation;
  check(!owner.visitCommittedPendingChunkLease(
            wrongGeneration,
            [](const CommittedPendingChunkLease&) noexcept { return true; }),
        "owner rejects a mismatched generation");
  auto wrongWrapper = present.surface0;
  wrongWrapper.object = &destination;
  check(!owner.visitCommittedPendingChunkLease(
            wrongWrapper,
            [](const CommittedPendingChunkLease&) noexcept { return true; }),
        "owner rejects a mismatched local wrapper");
  owner.reset();
}

void productionOwnerProjectionBindsColdLedger() {
  Owner owner;
  PeAllFamilySemanticTokenLedger ledger;
  D9CTexture source;
  D9CTexture destination;
  const auto sourceOrdinal =
      ledger.beginSource(D9C_COMMAND_RECORD_UPDATE_TEXTURE);
  auto input = base(PeSemanticProducerKind::UpdateTexture,
                    sourceOrdinal, 1u);
  input.texture0 =
      localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&source, 0xa101u, 7u);
  input.texture1 =
      localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&destination, 0xa102u, 9u);
  check(sourceOrdinal != 0u && owner.admit(input),
        "projection fixture admits one owner record");
  check(projectLastCommittedSemanticRecord(owner, ledger),
        "cold ledger projects the owner's immutable final bytes");
  check(ledger.acceptedCount() == 1u && ledger.pendingCount() == 1u,
        "owner projection accepts exactly one semantic token");
  const auto& token = ledger.pending(0u);
  const auto identities = ledger.pendingExactIdentities(0u);
  check(token.recordType == D9C_COMMAND_RECORD_UPDATE_TEXTURE &&
            token.sourceOrdinal == sourceOrdinal &&
            token.recordOrdinal == 1u && token.wireRange.valid() &&
            ledger.pendingExactValue(0u).size() == token.wireRange.length &&
            identities.size() == 2u &&
            identities[0].kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
            identities[0].generation == 7u &&
            identities[0].objectId == 0xa101u &&
            identities[1].generation == 9u &&
            identities[1].objectId == 0xa102u,
        "owner projection preserves exact range and qualified identities");
  check(ledger.settleCapture(PeSemanticCaptureDisposition::Skipped),
        "projected token settles through the existing capture disposition");
  owner.reset();
  check(source.refs == 1u && destination.refs == 1u,
        "owner projection adds no lifetime outside the owner transaction");
}
}  // namespace

int main() {
  try {
    static_assert(sizeof(DefaultOwner) <= 512u);
    check(sizeof(DefaultOwner) <= 512u && DefaultOwner{}.constructionSucceeded(),
          "default owner shell stays within the size budget");
    {
      DefaultOwner unavailable(DefaultOwner::FailConstructionForTesting{});
      check(!unavailable.constructionSucceeded() &&
                !unavailable.admit(base(PeSemanticProducerKind::Present, 1u, 1u)) &&
                !unavailable.settle(),
            "retainer construction failure publishes an unavailable owner");
      unavailable.reset();
      check(!unavailable.constructionSucceeded() && unavailable.retainedCount() == 0u,
            "unavailable owner reset is safe and retain-free");
    }
    static_assert(!std::is_copy_constructible_v<Owner>);
    static_assert(!std::is_move_constructible_v<Owner>);
    duplicateSurfacePinSmoke();
    classificationAndLifetime();
    partialRectClearMatchesCanonicalBuilder();
    collisionCopyOverflowAndRetry();
    emissionSmoke();
    semanticBridgePreRetryPreservesOwnerBytes();
    semanticRetryPredicateTruthTable();
    productionCapacityMatchesLegacyCadence();
    warmRetainerCapacityDistinguishesNovelObjects();
    settledOwnerCannotExposePriorRanges();
    emptyOwnerStartsAtZeroCadence();
    committedLeaseQualificationUsesOwnerPins();
    productionOwnerProjectionBindsColdLedger();
  } catch (const Failure& failure) {
    std::cerr << "pe_semantic_owner_spec failed: " << failure.what() << '\n';
    return 1;
  }
  return 0;
}
