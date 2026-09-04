#include "d3d9_pe_semantic_owner.hpp"
#include "d3d9_pe_recorder_transaction.hpp"
#include "d3d9_pe_recorder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
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
    const auto inputView = borrowPeSemanticRecordInput(in);
    PeSemanticAdmissionPlan admission{};
    if (!planPeSemanticAdmission(inputView, admission) ||
        !owner.tryAppendOwnedRecord(inputView, []() noexcept { return true; })) {
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
  alignas(8) std::array<std::byte, 8192u> exact;
  std::fill(exact.begin(), exact.end(), std::byte{0xa5});
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
                       exactEmission.wire.begin()) &&
            std::all_of(exact.begin() + exactEmission.wireBytes, exact.end(),
                        [](std::byte value) {
                          return value == std::byte{0xa5};
                        }),
        "owner ExactFixed bytes equal canonical builder bytes and preserve the tail canary");
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

void largeCanonicalPayloadSurvivesRepeatedExactEmission() {
  DefaultOwner owner;
  std::array<std::byte, 4096u> constants{};
  for (std::size_t i = 0u; i < constants.size(); ++i)
    constants[i] = static_cast<std::byte>((i * 37u) & 0xffu);
  for (std::uint64_t ordinal = 1u; ordinal <= Owner::maxRecords; ++ordinal) {
    auto input = base(PeSemanticProducerKind::VsFloatConstant, ordinal, ordinal);
    input.setConst.registerCount = 256u;
    input.constantBytes = constants;
    check(owner.admit(input), "large canonical payload admission");
  }
  std::size_t handles = 0u;
  std::size_t payload = 0u;
  std::size_t wire = 0u;
  check(owner.emissionMetrics(handles, payload, wire) && payload > 65536u,
        "large canonical payload crosses the former staging overlap");
  std::vector<std::byte> first(wire);
  std::vector<std::byte> second(wire);
  PeSemanticExactFixedEmission firstEmission;
  PeSemanticExactFixedEmission secondEmission;
  check(owner.emitExactFixed(first, firstEmission) && firstEmission.valid() &&
            owner.emitExactFixed(second, secondEmission) && secondEmission.valid() &&
            firstEmission.wireBytes == secondEmission.wireBytes &&
            std::equal(first.begin(), first.end(), second.begin()) &&
            std::equal(firstEmission.wire.begin(), firstEmission.wire.end(),
                       secondEmission.wire.begin()),
        "repeated large ExactFixed emission preserves canonical bytes");
  owner.reset();
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

void qualifiedBufferReferenceLookup() {
  Owner owner;
  D9CBuffer buffer;
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u>
      streams{};
  streams[0].wire = {
      .slot = 0u, .valid = 1u, .offset = 16u, .stride = 32u,
      .frequency = 0u};
  const auto ref = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(&buffer, 0x52u, 7u);
  streams[0].object = ref;
  auto input = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
  input.sparse.streams = streams;
  check(owner.admit(input), "qualified buffer reference admission");
  check(owner.referencesBuffer(ref),
        "qualified buffer reference finds the exact object and generation");
  auto stale = ref;
  stale.identity.generation = 8u;
  check(!owner.referencesBuffer(stale),
        "qualified buffer reference rejects a stale generation");
  auto differentObject = ref;
  D9CBuffer other;
  differentObject.object = &other;
  check(!owner.referencesBuffer(differentObject),
        "qualified buffer reference rejects an object mismatch");
  owner.reset();
}

void pureAdmissionPlanDeduplicatesQualifiedBindings() {
  Owner owner;
  D9CBuffer buffer;
  const auto ref = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(&buffer, 0x62u, 4u);
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u> streams{{
      {.wire = {.slot = 0u, .valid = 1u, .offset = 0u, .stride = 16u,
                .frequency = 0u},
       .object = ref}}};
  std::array<SparseBindingInput<D9CCommandChunkWireIndexBinding>, 1u> indices{{
      {.wire = {.valid = 1u}, .object = ref}}};
  auto input = base(PeSemanticProducerKind::DrawIndexedPrimitive, 1u, 1u);
  input.sparse.streams = streams;
  input.sparse.indexBuffers = indices;
  PeSemanticAdmissionPlan plan{};
  check(planPeSemanticAdmission(input, plan) && plan.valid,
        "pure admission planning accepts qualified sparse bindings");
  check(plan.uniquePinCounts[2] == 1u && plan.handleCount == 1u &&
            plan.sparseCounts[2] == 1u && plan.sparseCounts[5] == 1u &&
            plan.payloadBytes != 0u,
        "pure admission planning deduplicates one buffer across two roles");
  check(owner.tryAppendOwnedRecord(input, []() noexcept { return true; }),
        "owner admission consumes the private prepared result");
  alignas(8) std::array<std::byte, 4096u> bytes{};
  PeSemanticExactFixedEmission emission;
  check(owner.emitExactFixed(bytes, emission) && emission.transport.header.handleCount == 1u,
        "prepared admission emits one qualified final-wire handle");
  owner.reset();
}

void admissionArithmeticRejectsOverflow() {
  std::size_t result = 0u;
  check(!detail::checkedSizeAdd(std::numeric_limits<std::size_t>::max(), 1u,
                                result),
        "checked admission addition rejects size overflow");
  check(!detail::checkedSizeMultiply(std::numeric_limits<std::size_t>::max(), 2u,
                                     result),
        "checked admission multiplication rejects size overflow");

  // Keep every production span physically valid; multiplication boundaries
  // are exercised through the pure checked-size helper below.
  std::array<D9CViewport, 1u> viewport{};
  auto draw = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
  draw.sparse.viewports = viewport;
  check(detail::admissionPayloadBytes(draw, result),
        "payload planner accepts a physically valid typed section");

  if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
    std::uint32_t bounded = 0u;
    check(!detail::checkedSizeToU32(
              static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1u,
              bounded),
          "checked admission conversion rejects a synthetic count above wire width");
    PeSemanticAdmissionPlan plan{};
    check(planPeSemanticAdmission(draw, plan) && plan.sparseCounts[8] == 1u,
          "planner accepts the valid sparse span after synthetic boundary checks");
  }
}

void pendingDeltaViewRejectsStaleTicket() {
  PeHotStateShadow shadow{};
  const auto view = shadow.pendingDeltaView();
  check(view.valid() &&
            view.ticket().generation == shadow.pendingTicket().generation,
        "pending delta view captures the current ticket");
  shadow.transition().setRenderState(RenderStateSlot::fromRaw(1u), 0x42u);
  check(!view.valid(),
        "pending delta view invalidates after a newer pending mutation");
  const auto fresh = shadow.pendingDeltaView();
  check(fresh.valid() && fresh.pendingRenderStatesTyped().size() == 1u,
        "fresh pending delta view observes the new immutable frontier");
}

void transactionalAdmissionRollsBackWithoutLeakingPins() {
  Owner owner;
  D9CSurface surface;
  auto input = base(PeSemanticProducerKind::Present, 1u, 1u);
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x72u, 3u);
  PeSemanticAdmissionPlan plan{};
  check(planPeSemanticAdmission(input, plan),
        "transactional failure fixture plans without exposing capability");
  check(!owner.tryAppendOwnedRecord(input, []() noexcept { return false; }) &&
            owner.size() == 0u && owner.retainedCount() == 0u && surface.refs == 1u,
        "transactional settlement failure rolls back owner and typed retain");
  check(owner.tryAppendOwnedRecord(input, []() noexcept { return true; }) &&
            owner.size() == 1u && surface.refs == 2u,
        "same input remains retryable after pre-effect rollback");
  owner.reset();
  check(surface.refs == 1u, "prepared retry releases its typed retain");
}

void borrowedViewCapacityRebaseAndSettlementFault() {
  using Tiny = PeSemanticBatchOwner<1u, 2u, 4096u, 4u, 4u>;
  Tiny owner;
  D9CSurface first;
  D9CSurface second;
  auto filler = base(PeSemanticProducerKind::Present, 1u, 1u);
  filler.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&first, 0x721u, 3u);
  check(owner.admit(filler), "borrowed-view capacity fixture fills the owner");

  struct StagedCanary {
    std::array<std::byte, 16u> before{};
    PeSemanticRecordInput input{};
    std::array<std::byte, 16u> after{};
  } stagedCanary;
  std::fill(stagedCanary.before.begin(), stagedCanary.before.end(),
            std::byte{0x5a});
  std::fill(stagedCanary.after.begin(), stagedCanary.after.end(),
            std::byte{0xa5});
  stagedCanary.input = base(PeSemanticProducerKind::Present, 0u, 0u);
  const auto effectiveRecordType = stagedCanary.input.recordType;
  stagedCanary.input.recordType = 0u;
  stagedCanary.input.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&second, 0x722u, 4u);
  const auto stagedSparse = &stagedCanary.input.sparse;
  auto inputView = borrowPeSemanticRecordInput(stagedCanary.input);
  inputView.destination.recordType = effectiveRecordType;
  inputView.destination.sourceOrdinal = 2u;
  inputView.destination.recordOrdinal = 2u;

  std::uint32_t settlementCalls = 0u;
  check(!owner.tryAppendOwnedRecord(inputView, [&]() noexcept {
          ++settlementCalls;
          return true;
        }) &&
            owner.lastAdmissionFailure() == Tiny::AdmissionFailure::Capacity &&
            settlementCalls == 0u && second.refs == 1u,
        "CapacityPre rejects the borrowed view before settlement or retention");
  check(owner.settle(), "CapacityPre settles the prior destination");

  check(!owner.tryAppendOwnedRecord(inputView, [&]() noexcept {
          ++settlementCalls;
          return false;
        }) &&
            settlementCalls == 1u && owner.size() == 0u && second.refs == 1u,
        "settlement fault rolls the rebased borrowed view back atomically");
  check(owner.tryAppendOwnedRecord(inputView, [&]() noexcept {
          ++settlementCalls;
          return true;
        }) &&
            settlementCalls == 2u && owner.size() == 1u &&
            owner.record(0u).sourceOrdinal == 2u &&
            owner.record(0u).recordOrdinal == 2u && second.refs == 2u,
        "the same staged input and overlay retry durably after rollback");
  check(stagedCanary.input.sourceOrdinal == 0u &&
            stagedCanary.input.recordOrdinal == 0u &&
            stagedCanary.input.recordType == 0u &&
            &stagedCanary.input.sparse == stagedSparse &&
            std::all_of(stagedCanary.before.begin(), stagedCanary.before.end(),
                        [](std::byte value) {
                          return value == std::byte{0x5a};
                        }) &&
            std::all_of(stagedCanary.after.begin(), stagedCanary.after.end(),
                        [](std::byte value) {
                          return value == std::byte{0xa5};
                        }),
        "borrowed admission never mutates the staged descriptor or its canaries");
  owner.reset();
  check(first.refs == 1u && second.refs == 1u,
        "borrowed-view capacity/fault fixture balances typed retention");
}

void borrowedViewSparseOverlayMatchesCanonicalBytes() {
  Owner owner;
  std::array<D9CCommandChunkWireRenderState, 1u> stagedRows{{{7u, 11u}}};
  std::array<D9CCommandChunkWireRenderState, 1u> destinationRows{{{7u, 29u}}};
  auto staged = base(PeSemanticProducerKind::DrawPrimitive, 0u, 0u);
  staged.draw.primitiveCount = 3u;
  staged.sparse.renderStates = stagedRows;
  auto destinationSparse = staged.sparse;
  destinationSparse.renderStates = destinationRows;
  auto inputView = borrowPeSemanticRecordInput(staged);
  inputView.destination.sourceOrdinal = 1u;
  inputView.destination.recordOrdinal = 1u;
  inputView.destination.sparse = &destinationSparse;
  check(owner.tryAppendOwnedRecord(inputView,
                                   []() noexcept { return true; }),
        "borrowed view admits the destination sparse overlay");

  CommandChunkBuilder canonical;
  check(appendSparseRecord(canonical, inputView.destination.recordType,
                           staged.draw, destinationSparse),
        "canonical builder admits the destination sparse overlay");
  const auto sealed = canonical.seal();
  alignas(8) std::array<std::byte, 4096u> exact;
  std::fill(exact.begin(), exact.end(), std::byte{0xcc});
  PeSemanticExactFixedEmission emission{};
  check(sealed.valid() && owner.emitExactFixed(exact, emission) &&
            sealed.blob.size() == emission.wireBytes &&
            std::equal(sealed.blob.begin(), sealed.blob.end(),
                       emission.wire.begin()) &&
            stagedRows[0].value == 11u && destinationRows[0].value == 29u,
        "sparse overlay emits canonical bytes without mutating staged rows");
  owner.reset();
  canonical.resetAndReleaseRetained();
}

void admissionFactsAreNotAcceptedByProductionApi() {
  Owner owner;
  D9CSurface surface;
  auto input = base(PeSemanticProducerKind::Present, 1u, 1u);
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x731u, 2u);
  PeSemanticAdmissionPlan plan{};
  check(planPeSemanticAdmission(input, plan),
        "transactional fixture keeps pure facts out of production API");
  plan.handleCount = 0u;
  // The strongest representable forgery cannot be supplied to the
  // transactional operation; it plans from the immutable input internally.
  check(owner.tryAppendOwnedRecord(input, []() noexcept { return true; }) &&
            owner.size() == 1u && surface.refs == 2u,
        "same-address forged facts are unrepresentable by production API");
  owner.reset();
  check(surface.refs == 1u, "unforgeable admission releases its typed retain");
}

void immediateAppendClosesSpanMutationGap() {
  Owner owner;
  std::array<std::byte, 4u> source{
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
  auto input = base(PeSemanticProducerKind::VsBoolConstant, 1u, 1u);
  input.setConst.registerCount = 1u;
  input.constantBytes = source;
  check(owner.tryAppendOwnedRecord(input, []() noexcept { return true; }),
        "immediate operation copies span content before returning");
  source[0] = std::byte{0x7f};
  check(owner.constantBytes(owner.record(0u))[0] == std::byte{0x11},
        "post-operation span mutation cannot alter owned semantic bytes");
  owner.reset();
}

void repeatedIdentityUsesBatchGlobalRetentionDelta() {
  using Tiny = PeSemanticBatchOwner<4u, 1u, 4096u, 4u, 4u>;
  Tiny owner;
  D9CSurface surface;
  for (std::uint64_t ordinal = 1u; ordinal <= Tiny::maxRecords;
       ++ordinal) {
    auto input = base(PeSemanticProducerKind::Present, ordinal, ordinal);
    input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
        &surface, 0x741u, 5u);
    check(owner.tryAppendOwnedRecord(input,
                                    []() noexcept { return true; }),
          "repeated identity consumes one global retained pin");
  }
  PeSemanticExactFixedEmission exact{};
  check(owner.size() == Tiny::maxRecords && owner.emitExactFixed(exact) &&
            exact.transport.header.recordCount == Tiny::maxRecords,
        "repeated identities preserve per-record ExactFixed handle layout");
  PeSemanticSegmentedEmission segmented{};
  check(owner.emitSegmented(segmented) &&
            segmented.transport.header.handleCount == Tiny::maxRecords,
        "repeated identities preserve segmented per-record handles");
  owner.reset();
  check(surface.refs == 1u,
        "repeated identities release the retained stack object before scope exit");
}

void retentionIdentityAndPointerAliasesReject() {
  using Tiny = PeSemanticBatchOwner<4u, 2u, 4096u, 4u, 4u>;
  Tiny owner;
  D9CSurface first;
  D9CSurface alias;
  D9CTexture crossKind;
  auto accepted = base(PeSemanticProducerKind::Present, 1u, 1u);
  accepted.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&first, 0x752u, 7u);
  check(owner.tryAppendOwnedRecord(accepted,
                                   []() noexcept { return true; }),
        "identity alias fixture admits its first generation");

  auto crossKindIdentity = base(PeSemanticProducerKind::UpdateTexture, 2u, 2u);
  crossKindIdentity.texture0 = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &crossKind, 0x752u, 7u);
  crossKindIdentity.texture1 = crossKindIdentity.texture0;
  check(owner.tryAppendOwnedRecord(crossKindIdentity,
                                   []() noexcept { return true; }) &&
            crossKind.refs == 2u,
        "same object id in a different kind remains a distinct identity");

  auto generationAlias = base(PeSemanticProducerKind::Present, 3u, 3u);
  generationAlias.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &first, 0x752u, 8u);
  check(!owner.tryAppendOwnedRecord(generationAlias,
                                    []() noexcept { return true; }) &&
            owner.size() == 2u,
        "different generation with one object id is rejected");

  auto pointerAlias = base(PeSemanticProducerKind::Present, 4u, 4u);
  pointerAlias.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &alias, 0x752u, 7u);
  check(!owner.tryAppendOwnedRecord(pointerAlias,
                                    []() noexcept { return true; }) &&
            owner.size() == 2u && first.refs == 2u && alias.refs == 1u,
        "same generation and object id with another pointer is rejected");
  owner.reset();
  check(first.refs == 1u && alias.refs == 1u && crossKind.refs == 1u,
        "identity alias rejection leaves typed ledger balanced");
}

void capacityFailureIsPreEffectAndRetryableAfterBoundary() {
  using Tiny = PeSemanticBatchOwner<4u, 1u, 4096u, 4u, 4u>;
  Tiny owner;
  D9CSurface first;
  D9CSurface second;
  auto firstInput = base(PeSemanticProducerKind::Present, 1u, 1u);
  firstInput.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &first, 0x761u, 1u);
  check(owner.tryAppendOwnedRecord(firstInput,
                                   []() noexcept { return true; }),
        "capacity retry fixture admits the first pin");
  auto secondInput = base(PeSemanticProducerKind::Present, 2u, 2u);
  secondInput.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &second, 0x762u, 1u);
  check(!owner.tryAppendOwnedRecord(secondInput,
                                    []() noexcept { return true; }) &&
            owner.lastAdmissionFailure() == Tiny::AdmissionFailure::Capacity &&
            owner.size() == 1u && first.refs == 2u && second.refs == 1u,
        "capacity failure is pre-effect and retains no novel pin");
  check(owner.settle() &&
            owner.tryAppendOwnedRecord(secondInput,
                                       []() noexcept { return true; }) &&
            owner.size() == 1u && second.refs == 2u,
        "capacity retry succeeds after the owner boundary");
  owner.reset();
  check(first.refs == 1u && second.refs == 1u,
        "capacity retry fixture releases both typed pins");
}

// Truth table for the call-local prepared witness. Every row states the
// destination shape, the record, and the exact admission outcome plus the
// retention delta the same pass must publish. The table is the binding between
// the CapacityPre predicate and the transactional append: an `Admissible` row
// is precisely the answer the removed private canAdmitStorage() gave.
void preparedAdmissionWitnessTruthTable() {
  using Outcome = PeSemanticAdmissionOutcome;
  Owner owner;
  D9CSurface surface;
  D9CSurface aliasObject;
  PeSemanticPreparedRecord witness{};

  auto fresh = base(PeSemanticProducerKind::Present, 1u, 1u);
  fresh.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x9a1u, 3u);
  check(owner.prepareAdmission(fresh, witness) == Outcome::Admissible &&
            witness.valid() && witness.rule != nullptr &&
            witness.producerMatchesRecordType && !witness.constantProducer &&
            !witness.upProducer && witness.plan.handleCount == 1u &&
            witness.plan.uniquePinCounts[0] == 1u &&
            witness.retentionDeltas[0] == 1u && witness.recordCount == 0u &&
            witness.emissionHandleCount == 0u &&
            witness.emissionPayloadBytes == 0u && witness.payloadOffset == 0u &&
            witness.nextEmissionHandleCount == 1u && witness.wireBytes != 0u,
        "novel identity prepares one retained pin and one wire handle");
  check(owner.size() == 0u && owner.retainedCount() == 0u,
        "preparation is observation-only and mutates no owner state");

  check(owner.tryAppendOwnedRecord(fresh, []() noexcept { return true; }),
        "prepared witness fixture admits its first record");
  auto warm = base(PeSemanticProducerKind::Present, 2u, 2u);
  warm.surface0 = fresh.surface0;
  check(owner.prepareAdmission(warm, witness) == Outcome::Admissible &&
            witness.retentionDeltas[0] == 0u && witness.plan.handleCount == 1u &&
            witness.recordCount == 1u && witness.emissionHandleCount == 1u &&
            witness.nextEmissionHandleCount == 2u &&
            witness.payloadOffset >= witness.emissionPayloadBytes &&
            witness.nextEmissionPayloadBytes ==
                witness.payloadOffset + witness.plan.payloadBytes,
        "an already-pinned identity costs zero retention and keeps its handle");

  auto generationStale = base(PeSemanticProducerKind::Present, 3u, 3u);
  generationStale.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x9a1u, 4u);
  check(owner.prepareAdmission(generationStale, witness) == Outcome::Malformed &&
            !witness.valid(),
        "a stale generation of a pinned object id is refused before any effect");

  auto pointerAlias = base(PeSemanticProducerKind::Present, 3u, 3u);
  pointerAlias.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&aliasObject, 0x9a1u, 3u);
  check(owner.prepareAdmission(pointerAlias, witness) == Outcome::Malformed,
        "a pointer alias of a pinned identity is refused before any effect");

  auto forgedKind = base(PeSemanticProducerKind::Present, 3u, 3u);
  forgedKind.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&aliasObject, 0x9a2u, 1u);
  forgedKind.surface0.identity.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
  check(owner.prepareAdmission(forgedKind, witness) == Outcome::Malformed,
        "a kind-forged direct reference never reaches the capacity proof");

  auto zeroGeneration = base(PeSemanticProducerKind::Present, 3u, 3u);
  zeroGeneration.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&aliasObject, 0x9a3u, 0u);
  check(owner.prepareAdmission(zeroGeneration, witness) == Outcome::Malformed,
        "a zero-generation identity is malformed, not a capacity question");

  // The record rule is a destination fact, so an unknown record type is
  // refused by the capacity proof rather than by the identity plan.
  auto unknownType = base(PeSemanticProducerKind::Present, 3u, 3u);
  unknownType.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&aliasObject, 0x9a4u, 1u);
  unknownType.recordType = 0xffffffffu;
  check(owner.prepareAdmission(unknownType, witness) == Outcome::Capacity &&
            witness.rule == nullptr && !witness.producerMatchesRecordType,
        "an unresolvable record rule fails the capacity proof");

  // One row past Owner's sparse arena (MaxSparseValues == 8), still well
  // inside the render-state section rule, so this is a destination question.
  std::array<D9CCommandChunkWireRenderState, 9u> renderStates{};
  auto sparseOverflow = base(PeSemanticProducerKind::DrawPrimitive, 3u, 3u);
  sparseOverflow.sparse.renderStates = renderStates;
  check(owner.prepareAdmission(sparseOverflow, witness) == Outcome::Capacity,
        "a sparse arena that cannot fit the destination is a capacity answer");

  std::array<std::byte, 256u> constantBytes{};
  auto byteOverflow = base(PeSemanticProducerKind::VsFloatConstant, 3u, 3u);
  byteOverflow.setConst.registerCount = 16u;
  byteOverflow.constantBytes = constantBytes;
  check(owner.prepareAdmission(byteOverflow, witness) == Outcome::Capacity,
        "variable bytes beyond the destination arena are a capacity answer");

  std::array<D9CRect, 9u> rects{};
  auto rectOverflow = base(PeSemanticProducerKind::Clear, 3u, 3u);
  rectOverflow.clear.rectCount = static_cast<std::uint32_t>(rects.size());
  rectOverflow.clearRects = rects;
  check(owner.prepareAdmission(rectOverflow, witness) == Outcome::Capacity,
        "clear rectangles beyond the destination arena are a capacity answer");

  // Record capacity, then the CapacityPre flush that makes the same record
  // admissible again.
  for (std::uint64_t ordinal = owner.size() + 1u; ordinal <= Owner::maxRecords;
       ++ordinal) {
    auto filler = base(PeSemanticProducerKind::Present, ordinal, ordinal);
    filler.surface0 = fresh.surface0;
    check(owner.tryAppendOwnedRecord(filler, []() noexcept { return true; }),
          "record-capacity fixture fills the destination chunk");
  }
  auto overflow = base(PeSemanticProducerKind::Present,
                       Owner::maxRecords + 1u, Owner::maxRecords + 1u);
  overflow.surface0 = fresh.surface0;
  check(owner.prepareAdmission(overflow, witness) == Outcome::Capacity &&
            !owner.tryAppendOwnedRecord(overflow,
                                        []() noexcept { return true; }) &&
            owner.lastAdmissionFailure() ==
                Owner::AdmissionFailure::Capacity &&
            owner.size() == Owner::maxRecords,
        "a full destination rejects pre-effect with the capacity code");
  check(owner.settle() &&
            owner.prepareAdmission(overflow, witness) == Outcome::Admissible &&
            owner.tryAppendOwnedRecord(overflow, []() noexcept { return true; }),
        "the CapacityPre boundary makes the identical record admissible again");
  owner.reset();
  check(surface.refs == 1u && aliasObject.refs == 1u,
        "prepared-witness truth table leaves the typed ledger balanced");

  // Pin capacity is a destination fact of its own: the plan is well formed and
  // the retention delta is real, but the chunk has no room for a novel pin.
  using PinBound = PeSemanticBatchOwner<4u, 1u, 4096u, 4u, 4u>;
  PinBound pinBound;
  D9CSurface pinned;
  D9CSurface novel;
  auto first = base(PeSemanticProducerKind::Present, 1u, 1u);
  first.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&pinned, 0x9b1u, 1u);
  check(pinBound.tryAppendOwnedRecord(first, []() noexcept { return true; }),
        "pin-capacity fixture admits the only available pin");
  auto second = base(PeSemanticProducerKind::Present, 2u, 2u);
  second.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&novel, 0x9b2u, 1u);
  PeSemanticPreparedRecord pinWitness{};
  check(pinBound.prepareAdmission(second, pinWitness) == Outcome::Capacity &&
            pinWitness.plan.valid && pinWitness.retentionDeltas[0] == 1u &&
            !pinWitness.admissible && novel.refs == 1u,
        "an exhausted pin budget is a capacity answer over a well-formed plan");
  pinBound.reset();
  check(pinned.refs == 1u && novel.refs == 1u,
        "pin-capacity fixture releases every typed pin");

  // Allocation fault: the owner never published readiness, so preparation
  // reports it without reading the bounded storage at all.
  Owner unavailable(Owner::FailConstructionForTesting{});
  PeSemanticPreparedRecord unavailableWitness{};
  check(unavailable.prepareAdmission(fresh, unavailableWitness) ==
                Outcome::Unavailable &&
            !unavailableWitness.valid() &&
            unavailableWitness.rule == nullptr &&
            !unavailable.tryAppendOwnedRecord(fresh,
                                              []() noexcept { return true; }) &&
            unavailable.lastAdmissionFailure() ==
                Owner::AdmissionFailure::Unavailable,
        "an allocation-faulted owner reports Unavailable before any plan work");
}

// The witness is the only frontier arithmetic in the append path, so both
// settlement outcomes must agree with it exactly: a rejected settlement
// restores the captured frontier and a durable one publishes the proved one.
void preparedWitnessSettlementAndRollback() {
  using Outcome = PeSemanticAdmissionOutcome;
  Owner owner;
  D9CSurface surface;
  auto input = base(PeSemanticProducerKind::Present, 1u, 1u);
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x9c1u, 2u);
  PeSemanticPreparedRecord witness{};
  check(owner.prepareAdmission(input, witness) == Outcome::Admissible &&
            witness.admissibleAt(0u, 0u, 0u),
        "settlement fixture prepares against the empty destination");

  check(!owner.tryAppendOwnedRecord(input, []() noexcept { return false; }) &&
            owner.size() == witness.recordCount && owner.retainedCount() == 0u &&
            surface.refs == 1u,
        "a rejected settlement rolls the record and its typed retain back");
  std::size_t handles = 0u;
  std::size_t payload = 0u;
  std::size_t wire = 0u;
  check(resolvePeSemanticCadenceMetrics(owner, handles, payload, wire) &&
            handles == witness.emissionHandleCount &&
            payload == witness.emissionPayloadBytes,
        "rollback restores exactly the frontier the witness captured");

  check(owner.tryAppendOwnedRecord(input, []() noexcept { return true; }) &&
            owner.emissionMetrics(handles, payload, wire) &&
            handles == witness.nextEmissionHandleCount &&
            payload == witness.nextEmissionPayloadBytes &&
            wire == witness.wireBytes && surface.refs == 2u,
        "a durable settlement publishes the proved frontier without replanning");

  PeSemanticExactFixedEmission emission{};
  check(owner.emitExactFixed(emission) && emission.valid() &&
            emission.wireBytes == witness.wireBytes &&
            emission.transport.header.handleCount ==
                witness.nextEmissionHandleCount,
        "the emitted chunk matches the witness the capacity proof produced");
  owner.reset();
  check(surface.refs == 1u, "settlement fixture releases its typed retain");
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

void producerIdentityFollowsSettlementAndSourceRange() {
  Owner owner;
  D9CSurface surface;
  auto first = base(PeSemanticProducerKind::Present, 1u, 1u);
  first.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x771u);
  auto second = base(PeSemanticProducerKind::Present, 2u, 2u);
  second.surface0 = first.surface0;
  check(owner.admit(first) && owner.admit(second),
        "producer identity fixture admits a source interval");
  PeSemanticExactFixedEmission firstEmission;
  check(owner.emitExactFixed(firstEmission) &&
            firstEmission.transport.producerIdentity.firstEventOrdinal == 1u &&
            firstEmission.transport.producerIdentity.lastEventOrdinal == 1u &&
            firstEmission.transport.producerIdentity.firstSourceOrdinal == 1u &&
            firstEmission.transport.producerIdentity.lastSourceOrdinal == 2u,
        "first immutable emission binds its exact PE event/source interval");
  check(owner.settle(), "producer identity fixture settles its first event");

  auto third = base(PeSemanticProducerKind::Present, 3u, 3u);
  third.surface0 = first.surface0;
  check(owner.admit(third), "producer identity fixture admits next event");
  PeSemanticSegmentedEmission secondEmission;
  check(owner.emitSegmented(secondEmission) &&
            secondEmission.transport.producerIdentity.firstEventOrdinal == 2u &&
            secondEmission.transport.producerIdentity.lastEventOrdinal == 2u &&
            secondEmission.transport.producerIdentity.firstSourceOrdinal == 3u &&
            secondEmission.transport.producerIdentity.lastSourceOrdinal == 3u,
        "settlement advances the PE event while preserving source order");
  owner.reset();
  check(surface.refs == 1u, "producer identity fixture releases its pin");
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

void ownerQualifiedMaterializationLedger() {
  dxmt9::core::CopyMaterializationLedger ledger;
  dxmt9::core::ScopedCopyMaterializationLedger observe(
      dxmt9::core::CopyMaterializationOwner::Pe, ledger);
  Owner owner;
  D9CSurface surface;
  auto present = base(PeSemanticProducerKind::Present, 1u, 1u);
  present.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0xb101u, 3u);
  check(owner.tryAppendOwnedRecord(present,
                                   []() noexcept { return true; }),
        "ledger fixture admits one prepared semantic record");
  const auto admitted = ledger.snapshot(
      dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission);
  check(admitted.calls == 1u && admitted.bytes != 0u &&
            admitted.retainedBytes == admitted.bytes &&
            admitted.retainedBytesPeak == admitted.bytes,
        "semantic admission records one PE-owned materialization and retention");

  auto invalid = base(PeSemanticProducerKind::Present, 0u, 0u);
  invalid.surface0 = present.surface0;
  check(!owner.admit(invalid), "invalid ledger admission fails closed");
  const auto afterFailure = ledger.snapshot(
      dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission);
  check(afterFailure.calls == admitted.calls &&
            afterFailure.bytes == admitted.bytes &&
            afterFailure.retainedBytes == admitted.retainedBytes,
        "rolled-back admission emits no copy-ledger event");

  PeSemanticExactFixedEmission first;
  check(owner.emitExactFixed(first), "ledger fixture emits ExactFixed");
  const auto exactFirst = ledger.snapshot(
      dxmt9::core::CopyMaterializationClass::PeWireFinal);
  check(exactFirst.calls == 1u && exactFirst.bytes == first.wireBytes &&
            exactFirst.retainedBytes == first.wireBytes &&
            exactFirst.retainedBytesPeak == first.wireBytes,
        "ExactFixed emission records one PE-owned physical final wire");

  PeSemanticExactFixedEmission repeated;
  check(owner.emitExactFixed(repeated) && repeated.wireBytes == first.wireBytes,
        "repeated ExactFixed emission preserves bytes");
  const auto exactRepeated = ledger.snapshot(
      dxmt9::core::CopyMaterializationClass::PeWireFinal);
  check(exactRepeated.calls == 2u &&
            exactRepeated.bytes == first.wireBytes + repeated.wireBytes &&
            exactRepeated.retainedBytes == repeated.wireBytes &&
            exactRepeated.retainedBytesPeak == repeated.wireBytes,
        "re-emission counts physical work without double-retaining the buffer");
  check(ledger.snapshot(
            dxmt9::core::CopyMaterializationClass::PeBuilderTemporary).calls ==
            0u &&
            ledger.snapshot(
                dxmt9::core::CopyMaterializationClass::PeSealRecords).calls ==
                0u &&
            ledger.snapshot(
                dxmt9::core::CopyMaterializationClass::PeSealHandles).calls ==
                0u &&
            ledger.snapshot(
                dxmt9::core::CopyMaterializationClass::PeSealPayload).calls ==
                0u,
        "semantic owner ledger never fabricates retired builder or seal work");

  check(owner.settle(), "ledger fixture settles the owner");
  check(ledger.snapshot(
            dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission)
                .retainedBytes == 0u &&
            ledger.snapshot(dxmt9::core::CopyMaterializationClass::PeWireFinal)
                .retainedBytes == 0u,
        "settlement releases admission and final-wire ledger retention");
  owner.reset();
  check(surface.refs == 1u, "ledger fixture releases its warm pin");
}
}  // namespace

int main() {
  try {
    static_assert(sizeof(PeSemanticAdmissionPlan) <= 256u);
    static_assert(sizeof(PeSemanticRecordInput) >= 800u);
    static_assert(sizeof(PeSemanticRecordDestinationContext) <= 32u);
    static_assert(sizeof(PeSemanticRecordInputView) <= 40u);
    static_assert(sizeof(PeSemanticRecordInputView) * 20u <
                  sizeof(PeSemanticRecordInput));
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
    qualifiedBufferReferenceLookup();
    pureAdmissionPlanDeduplicatesQualifiedBindings();
    admissionArithmeticRejectsOverflow();
    pendingDeltaViewRejectsStaleTicket();
    transactionalAdmissionRollsBackWithoutLeakingPins();
    borrowedViewCapacityRebaseAndSettlementFault();
    borrowedViewSparseOverlayMatchesCanonicalBytes();
    admissionFactsAreNotAcceptedByProductionApi();
    immediateAppendClosesSpanMutationGap();
    repeatedIdentityUsesBatchGlobalRetentionDelta();
    retentionIdentityAndPointerAliasesReject();
    capacityFailureIsPreEffectAndRetryableAfterBoundary();
    preparedAdmissionWitnessTruthTable();
    preparedWitnessSettlementAndRollback();
    classificationAndLifetime();
    partialRectClearMatchesCanonicalBuilder();
    largeCanonicalPayloadSurvivesRepeatedExactEmission();
    collisionCopyOverflowAndRetry();
    emissionSmoke();
    producerIdentityFollowsSettlementAndSourceRange();
    semanticBridgePreRetryPreservesOwnerBytes();
    semanticRetryPredicateTruthTable();
    productionCapacityMatchesLegacyCadence();
    warmRetainerCapacityDistinguishesNovelObjects();
    settledOwnerCannotExposePriorRanges();
    emptyOwnerStartsAtZeroCadence();
    committedLeaseQualificationUsesOwnerPins();
    productionOwnerProjectionBindsColdLedger();
    ownerQualifiedMaterializationLedger();
  } catch (const Failure& failure) {
    std::cerr << "pe_semantic_owner_spec failed: " << failure.what() << '\n';
    return 1;
  }
  return 0;
}
