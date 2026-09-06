#include "d3d9_pe_semantic_owner.hpp"
#include "d3d9_pe_recorder_transaction.hpp"
#include "d3d9_pe_recorder.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_semantic_owner_phase_observer.hpp"
#include "device_c_chunk_validate.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <sys/wait.h>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>
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

template <typename Owner>
bool ownerUpVertexPrefix(const Owner& owner,
                         std::span<const std::byte> expected) {
  PeSemanticExactFixedEmission emission{};
  if (!owner.emitExactFixed(emission) || emission.transport.header.recordCount != 1u)
    return false;
  const auto& header = emission.transport.header;
  D9CCommandChunkWireRecordHeader record{};
  std::memcpy(&record, emission.wire.data() + header.recordTableOffset,
              sizeof(record));
  if (record.payloadOffset > header.payloadArenaSize ||
      record.payloadSize > header.payloadArenaSize - record.payloadOffset)
    return false;
  const auto payload = emission.wire.subspan(
      header.payloadArenaOffset + record.payloadOffset, record.payloadSize);
  if (payload.size() < sizeof(D9CCommandChunkWireDrawHeader)) return false;
  D9CCommandChunkWireDrawHeader draw{};
  std::memcpy(&draw, payload.data(), sizeof(draw));
  if (draw.sectionTableOffset > payload.size() ||
      draw.sectionCount > D9C_COMMAND_CHUNK_SECTION_COUNT ||
      draw.sectionCount * sizeof(D9CCommandChunkWireSectionDesc) >
          payload.size() - draw.sectionTableOffset)
    return false;
  for (std::size_t i = 0u; i < draw.sectionCount; ++i) {
    D9CCommandChunkWireSectionDesc desc{};
    std::memcpy(&desc, payload.data() + draw.sectionTableOffset +
                    i * sizeof(desc), sizeof(desc));
    if (desc.kind != D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA) continue;
    if (desc.payloadOffset > payload.size() ||
        desc.byteSize > payload.size() - desc.payloadOffset ||
        expected.size() > desc.byteSize)
      return false;
    return std::memcmp(payload.data() + desc.payloadOffset, expected.data(),
                       expected.size()) == 0;
  }
  return expected.empty();
}

template <PeSemanticRecordInputLike Input>
bool appendCanonicalSemanticRecord(CommandChunkBuilder& builder,
                                   const Input& input) {
  const auto& staged = stagedPeSemanticInput(input);
  switch (staged.producer) {
    case PeSemanticProducerKind::Present:
      return appendPresent(builder, staged.present, staged.surface0);
    case PeSemanticProducerKind::StretchRect:
      return appendStretchRect(builder, staged.stretchRect, staged.surface0,
                               staged.surface1);
    case PeSemanticProducerKind::ColorFill:
      return appendColorFill(builder, staged.colorFill, staged.surface0);
    case PeSemanticProducerKind::UpdateTexture:
      return appendUpdateTexture(builder, staged.texture0, staged.texture1);
    case PeSemanticProducerKind::UpdateSurface:
      return appendUpdateSurface(builder, staged.updateSurface, staged.surface0,
                                 staged.surface1);
    case PeSemanticProducerKind::QueryIssue:
      return appendQueryIssue(builder, staged.queryIssue.flags, staged.query);
    case PeSemanticProducerKind::Readback:
      return appendReadback(builder, staged.surface0, staged.surface1);
    case PeSemanticProducerKind::ReszDepthResolve:
      return appendReszDepthResolve(builder, staged.surface0, staged.texture0);
    case PeSemanticProducerKind::GenerateMipmaps:
      return appendGenerateMipmaps(builder, staged.texture0);
    case PeSemanticProducerKind::VsFloatConstant:
    case PeSemanticProducerKind::VsIntConstant:
    case PeSemanticProducerKind::VsBoolConstant:
    case PeSemanticProducerKind::PsFloatConstant:
    case PeSemanticProducerKind::PsIntConstant:
    case PeSemanticProducerKind::PsBoolConstant:
      return appendSetConstants(builder, staged.recordType,
                                staged.setConst.startRegister,
                                staged.setConst.registerCount,
                                staged.constantBytes);
    case PeSemanticProducerKind::Clear:
      return appendClear(builder, staged.clear, staged.clearRects);
    case PeSemanticProducerKind::DrawPrimitive:
    case PeSemanticProducerKind::DrawIndexedPrimitive:
    case PeSemanticProducerKind::DrawPrimitiveUp:
    case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
    case PeSemanticProducerKind::ApplyState:
      return appendSparseRecord(builder, peSemanticRecordType(input),
                                staged.draw, peSemanticSparse(input));
    case PeSemanticProducerKind::Count:
      return false;
  }
  return false;
}

template <typename TestedOwner>
bool equalCanonicalOwnerBytes(const TestedOwner& owner,
                              CommandChunkBuilder& canonical,
                              PeSemanticExactFixedEmission& exact) {
  const auto sealed = canonical.seal();
  return sealed.valid() && owner.emitExactFixed(exact) && exact.valid() &&
         sealed.blob.size() == exact.wireBytes &&
         std::equal(sealed.blob.begin(), sealed.blob.end(), exact.wire.begin());
}

bool ownerSegmentedRolesEqualExact(const PeSemanticExactFixedEmission& exact,
                                   const PeSemanticSegmentedEmission& segmented) {
  const auto& header = exact.transport.header;
  const auto role = [](const D9CWireHandle& handle, std::uint32_t bytes) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(
            d9cWireHandleValue(handle))),
        bytes);
  };
  const auto exactRecords = exact.wire.subspan(header.recordTableOffset,
                                                segmented.transport.recordBytes);
  const auto exactHandles = exact.wire.subspan(header.handleTableOffset,
                                                segmented.transport.handleBytes);
  const auto exactPayload = exact.wire.subspan(header.payloadArenaOffset,
                                                segmented.transport.payloadBytes);
  return segmented.transport.header.recordCount == header.recordCount &&
         segmented.transport.header.handleCount == header.handleCount &&
         segmented.transport.header.payloadArenaSize ==
             header.payloadArenaSize &&
         segmented.wireBytes == exact.wireBytes &&
         std::equal(exactRecords.begin(), exactRecords.end(),
                    role(segmented.transport.records,
                         segmented.transport.recordBytes).begin()) &&
         std::equal(exactHandles.begin(), exactHandles.end(),
                    role(segmented.transport.handles,
                         segmented.transport.handleBytes).begin()) &&
         std::equal(exactPayload.begin(), exactPayload.end(),
                    role(segmented.transport.payload,
                         segmented.transport.payloadBytes).begin());
}

void genericConstantRetainsForeignDirectAuthority() {
  // Family-specific production staging proves these fields absent. The public
  // generic envelope did not: specialization must retain its old semantics.
  constexpr std::array producers{
      PeSemanticProducerKind::VsFloatConstant,
      PeSemanticProducerKind::VsIntConstant,
      PeSemanticProducerKind::VsBoolConstant,
      PeSemanticProducerKind::PsFloatConstant,
      PeSemanticProducerKind::PsIntConstant,
      PeSemanticProducerKind::PsBoolConstant};
  const std::array<std::byte, 16u> values{};
  for (const auto producer : producers) {
    D9CTexture texture;
    Owner owner;
    CommandChunkBuilder canonical;
    auto input = base(producer, 1u, 1u);
    input.setConst.registerCount = 1u;
    const bool boolean = producer == PeSemanticProducerKind::VsBoolConstant ||
                         producer == PeSemanticProducerKind::PsBoolConstant;
    input.constantBytes = std::span<const std::byte>(values).first(
        boolean ? 4u : 16u);
    input.texture0 = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&texture, 0xf14u);
    check(owner.admit(input) && owner.retainedCount() == 1u && texture.refs == 2u,
          "generic constant preserves a retained-only foreign direct ref");
    check(appendCanonicalSemanticRecord(canonical, input),
          "generic constant canonical fixture emits");
    PeSemanticExactFixedEmission exact{};
    check(equalCanonicalOwnerBytes(owner, canonical, exact) &&
              exact.transport.header.handleCount == 0u,
          "retained-only constant ref does not become a wire-visible ref");
    owner.reset();
    check(texture.refs == 1u, "generic constant reset releases its direct ref");
    check(!owner.tryAppendOwnedRecord(input, []() noexcept { return false; }) &&
              owner.size() == 0u && owner.retainedCount() == 0u &&
              texture.refs == 1u,
          "generic constant failed settlement rolls its foreign retain back");
    input.texture0.identity.kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
    check(!owner.admit(input) && owner.retainedCount() == 0u,
          "generic constant does not skip malformed foreign direct refs");
  }
}

template <PeSemanticProducerKind Producer>
void typedConstantAdmissionDifferential() {
  constexpr bool boolean = Producer == PeSemanticProducerKind::VsBoolConstant ||
                           Producer == PeSemanticProducerKind::PsBoolConstant;
  constexpr std::size_t registerBytes = boolean ? 4u : 16u;
  using ConstantOwner = PeSemanticBatchOwner<3u, 1u, 3u * registerBytes, 1u, 1u>;
  ConstantOwner owner;
  CommandChunkBuilder canonical;
  std::array<std::byte, registerBytes> values{};
  PeSemanticTypedConstantInput<Producer> input{};
  input.setConst.startRegister = 1u;
  input.setConst.registerCount = 1u;
  input.constantBytes = values;
  for (std::uint64_t ordinal = 1u; ordinal <= 3u; ++ordinal) {
    values.fill(ordinal == 2u ? std::byte{0x52} : std::byte{0x31});
    input.sourceOrdinal = input.recordOrdinal = ordinal;
    check(owner.appendOwnedRecord(input, []() noexcept { return true; }) &&
              owner.retainedCount() == 0u,
          "typed constant A/B/A commits without acquiring resource pins");
    check(appendSetConstants(canonical, peSemanticRecordTypeForProducer(Producer),
                             input.setConst.startRegister,
                             input.setConst.registerCount, values),
          "independent constant oracle commits the same A/B/A value");
  }
  values.fill(std::byte{0xee});
  PeSemanticExactFixedEmission exact{};
  check(equalCanonicalOwnerBytes(owner, canonical, exact),
        "typed constant A/B/A owns exact bytes before source mutation");
  PeSemanticSegmentedEmission segmented{};
  check(owner.emitSegmented(segmented) &&
            ownerSegmentedRolesEqualExact(exact, segmented),
        "typed constant segmented roles equal contiguous oracle bytes");
  input.sourceOrdinal = input.recordOrdinal = 4u;
  bool callbackEntered = false;
  check(!owner.appendOwnedRecord(input, [&]() noexcept {
          callbackEntered = true;
          return true;
        }) && !callbackEntered && owner.size() == 3u,
        "typed constant capacity failure occurs before settlement");
  owner.reset();
  input.sourceOrdinal = input.recordOrdinal = 1u;
  check(!owner.appendOwnedRecord(input, []() noexcept { return false; }) &&
            owner.size() == 0u && owner.retainedCount() == 0u,
        "typed constant callback failure rolls back the same owner transaction");
  check(owner.appendOwnedRecord(input, []() noexcept { return true; }),
        "typed constant failed settlement can retry the unchanged ordinal");
  owner.reset();
  input.constantBytes = std::span<const std::byte>(values).first(registerBytes - 1u);
  check(!owner.admit(input) && owner.size() == 0u,
        "typed constant byte extent mismatch fails closed");
  input.constantBytes = values;
  input.setConst.registerCount = std::numeric_limits<std::uint32_t>::max();
  check(!owner.admit(input) && owner.size() == 0u,
        "typed constant register-count overflow fails closed");
  input.setConst.registerCount = 1u;
  input.sourceOrdinal = 0u;
  check(!owner.admit(input) && owner.size() == 0u,
        "typed constant cannot bypass source identity validation");

  // Specialization is representation-only: retain the generic owner's
  // count/byte policy rather than introducing a new start+count restriction.
  ConstantOwner genericOwner;
  input.sourceOrdinal = input.recordOrdinal = 1u;
  input.setConst.startRegister = std::numeric_limits<std::uint32_t>::max();
  auto generic = base(Producer, 1u, 1u);
  generic.setConst = input.setConst;
  generic.constantBytes = input.constantBytes;
  check(owner.admit(input) == genericOwner.admit(generic) && owner.size() == 1u,
        "typed constant preserves generic start-register acceptance policy");
  PeSemanticExactFixedEmission typedExtent{}, genericExtent{};
  check(owner.emitExactFixed(typedExtent) &&
            genericOwner.emitExactFixed(genericExtent) &&
            typedExtent.wireBytes == genericExtent.wireBytes &&
            std::memcmp(typedExtent.wire.data(), genericExtent.wire.data(),
                        typedExtent.wireBytes) == 0,
        "typed constant preserves generic bytes for the full accepted domain");

  for (unsigned scenario = 0u; scenario < 5u; ++scenario) {
    ConstantOwner typedObserved, genericObserved;
    PeSemanticOwnerPhaseObserver typedObserver(1u), genericObserver(1u);
    typedObserved.setPhaseObserver(&typedObserver);
    genericObserved.setPhaseObserver(&genericObserver);
    input.sourceOrdinal = input.recordOrdinal = 1u;
    input.setConst.registerCount = 1u;
    if (scenario == 2u || scenario == 3u) input.sourceOrdinal = 0u;
    if (scenario == 3u)
      input.setConst.registerCount = std::numeric_limits<std::uint32_t>::max();
    generic = base(Producer, input.sourceOrdinal, input.recordOrdinal);
    generic.setConst = input.setConst;
    generic.constantBytes = input.constantBytes;
    const auto settle = [scenario]() noexcept { return scenario != 1u; };
    if (scenario == 4u) {
      for (std::uint64_t ordinal = 1u; ordinal <= 3u; ++ordinal) {
        input.sourceOrdinal = input.recordOrdinal = ordinal;
        generic.sourceOrdinal = generic.recordOrdinal = ordinal;
        check(typedObserved.appendOwnedRecord(input, settle) &&
                  genericObserved.appendOwnedRecord(generic, settle),
              "observed constant capacity fixture fills both representations");
      }
      input.sourceOrdinal = generic.sourceOrdinal = 0u;
    }
    check(typedObserved.appendOwnedRecord(input, settle) ==
              genericObserved.appendOwnedRecord(generic, settle) &&
              typedObserved.lastAdmissionFailure() ==
                  genericObserved.lastAdmissionFailure() &&
              typedObserver.stats().outcomes == genericObserver.stats().outcomes,
          "typed constant preserves outcome and failure-precedence accounting");
    for (std::size_t phase = 0u;
         phase < PeSemanticOwnerPhaseStats::kPhaseCount; ++phase) {
      check(typedObserver.stats().phases[phase].events ==
                genericObserver.stats().phases[phase].events &&
                typedObserver.stats().phases[phase].samples ==
                genericObserver.stats().phases[phase].samples,
            "typed constant preserves observer phase populations");
    }
  }
}

void inactiveBindingMetadataRemainsNonAuthoritative() {
  D9CTexture texture;
  Owner owner;
  CommandChunkBuilder canonical;
  const std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
      rows{{{.wire = {.slot = 3u, .valid = 0u},
               .object = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
                   &texture, 0xf16u)}}};
  auto input = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
  input.draw.primitiveCount = 1u;
  input.sparse.textures = rows;
  check(owner.admit(input) && owner.retainedCount() == 0u && texture.refs == 1u,
        "inactive binding row cannot acquire authority from object metadata");
  check(appendCanonicalSemanticRecord(canonical, input),
        "inactive binding canonical fixture emits");
  PeSemanticExactFixedEmission exact{};
  check(equalCanonicalOwnerBytes(owner, canonical, exact),
        "inactive occurrence retains canonical ignored-metadata behavior");
}

void bindingOccurrenceStorageUsesActiveFrontier() {
  constexpr std::size_t locatorBytes =
      sizeof(PeSemanticBindingLocator) * kPeSemanticMaxBindingOccurrences;
  alignas(PeSemanticBindingOccurrencePlan)
      std::array<std::byte, sizeof(PeSemanticBindingOccurrencePlan)> storage;
  std::fill(storage.begin(), storage.end(), std::byte{0xa7});

  auto* plan = ::new (storage.data()) PeSemanticBindingOccurrencePlan;
  const auto locatorBytesView = [&]() noexcept {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(plan->values.data()), locatorBytes);
  };
  check(std::all_of(locatorBytesView().begin(), locatorBytesView().end(),
                    [](std::byte value) { return value == std::byte{0xa7}; }),
        "inactive binding locator storage is not default-initialized");

  plan->values[0] = {.mappingOrdinal = 3u,
                     .wireIndex = 5u,
                     .occurrence = 0u,
                     .wireValid = true,
                     .hasPin = true};
  plan->count = 1u;
  plan->reset();
  check(plan->count == 0u && plan->values[0].mappingOrdinal == 3u &&
            plan->values[0].wireIndex == 5u &&
            plan->values[0].occurrence == 0u &&
            plan->values[0].wireValid && plan->values[0].hasPin,
        "active binding locator survives frontier reset for reuse");
  const auto inactiveTail = locatorBytesView().subspan(sizeof(PeSemanticBindingLocator));
  check(std::all_of(inactiveTail.begin(), inactiveTail.end(),
                    [](std::byte value) { return value == std::byte{0xa7}; }),
        "frontier reset leaves inactive binding locator tail untouched");

  plan->values[0] = {.mappingOrdinal = kPeSemanticNoSlot,
                     .wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
                     .occurrence = 0u,
                     .wireValid = true,
                     .hasPin = false};
  plan->count = 1u;
  check(plan->values[0].mappingOrdinal == kPeSemanticNoSlot &&
            plan->values[0].wireIndex == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX &&
            plan->values[0].wireValid && !plan->values[0].hasPin,
        "reused active binding locator writes every nullable field");
}

void compactIdentityWitnessBoundMatchesSevenFamilies() {
  static_assert(kPeSemanticMaxBindingOccurrences ==
                D9C_DRAW_PACKET_MAX_TEXTURES + D9C_DRAW_PACKET_MAX_STREAMS +
                2u + 1u + 1u + D9C_DRAW_PACKET_MAX_RENDER_TARGETS + 1u);
  D9CSurface object;
  PeSemanticCompactPreparedRecord prepared;
  auto& witness = prepared.identityWitness;
  bool newlyRetained = false;
  bool newlyWireVisible = false;
  for (std::size_t i = 0u; i < witness.entries.size(); ++i) {
    const PeWireObjectRef ref{
        .identity = {.kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
                     .generation = 1u,
                     .objectId = static_cast<std::uint32_t>(0xc800u + i)},
        .object = &object};
    check(witness.add(ref, D9C_CHUNK_HANDLE_KIND_SURFACE, true,
                      newlyRetained, newlyWireVisible) && newlyRetained &&
              newlyWireVisible,
          "compact witness admits every seven-family binding occurrence");
  }
  const auto count = witness.count;
  const PeWireObjectRef overflow{
      .identity = {.kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
                   .generation = 1u,
                   .objectId = 0xc900u},
      .object = &object};
  check(count == 45u && witness.wireCount == 45u &&
            !witness.add(overflow, D9C_CHUNK_HANDLE_KIND_SURFACE, true,
                         newlyRetained, newlyWireVisible) &&
            witness.count == count && witness.wireCount == count,
        "compact witness fails closed at the exact seven-family bound");
}

void compactSparsePreparationSelectsSchemaWitness() {
  Owner owner;
  PeSemanticSparsePlanRecordInput input{};
  PeSemanticCompactPreparedRecord compact;
  check(owner.prepareAdmission(input, compact) ==
            PeSemanticAdmissionOutcome::Malformed,
        "sparse-plan preparation instantiates the 45-entry witness path");
  check(compact.identityWitness.entryCapacity ==
            kPeSemanticMaxBindingOccurrences &&
            compact.identityWitness.count == 0u,
        "compact sparse preparation leaves its bounded witness inactive on reject");
}

template <typename Wire, std::size_t RowCount, typename Object,
          std::uint32_t Kind,
          typename MakeValid, typename MakeNull, typename MakeInactive,
          typename Configure, typename SetDirect>
void preparedBindingFamilyMatrix(const char* family, MakeValid makeValid,
                                 MakeNull makeNull, MakeInactive makeInactive,
                                 Configure configure, SetDirect setDirect) {
  using Row = SparseBindingInput<Wire>;
  Object object;
  Object pointerAlias;
  const auto ref = localRef<Kind>(&object, 0xb00u + RowCount, 7u);

  std::array<Row, RowCount> validRows{};
  for (std::size_t i = 0u; i < validRows.size(); ++i) {
    validRows[i] = {.wire = makeValid(i), .object = ref};
  }

  const auto makeInput = [&](std::uint64_t ordinal) {
    auto input = base(PeSemanticProducerKind::DrawPrimitive, ordinal,
                      ordinal);
    input.draw.primitiveCount = 1u;
    configure(input.sparse, std::span<const Row>(validRows));
    setDirect(input, ref);
    return input;
  };

  // A valid sparse row is authoritative even when the same qualified object
  // is also present in a retained-only direct slot. This is the duplicate
  // identity case for singleton sparse schemas as well as for multi-row ones.
  {
    Owner owner;
    auto input = makeInput(1u);
    PeSemanticPreparedRecord prepared{};
    const auto preparedOutcome = owner.prepareAdmission(input, prepared);
    bool occurrenceMappingConsumedShape = std::all_of(
        prepared.bindingOccurrences.values.begin(),
        prepared.bindingOccurrences.values.begin() +
            prepared.bindingOccurrences.count,
        [&](const PeSemanticBindingLocator& locator) noexcept {
          return locator.wireValid && locator.hasPin &&
                 locator.mappingOrdinal < prepared.identityWitness.count &&
                 prepared.identityWitness.entries[locator.mappingOrdinal].pin == 0u &&
                 locator.wireIndex == 0u;
        });
    for (std::size_t i = 0u; i < prepared.bindingOccurrences.count; ++i) {
      if (prepared.bindingOccurrences.values[i].occurrence != i)
        occurrenceMappingConsumedShape = false;
    }
    check(preparedOutcome == PeSemanticAdmissionOutcome::Admissible &&
              prepared.bindingOccurrences.count == RowCount &&
              prepared.identityWitness.count == 1u && prepared.identityWitness.wireCount == 1u &&
              occurrenceMappingConsumedShape,
          "seven-family prepared occurrence mapping has one deduped wire pin");
    CommandChunkBuilder canonical;
    check(owner.admit(input) && owner.retainedCount() == 1u &&
              appendCanonicalSemanticRecord(canonical, input),
          family);
    PeSemanticExactFixedEmission exact{};
    check(equalCanonicalOwnerBytes(owner, canonical, exact),
          "seven-family valid row matches canonical ExactFixed bytes");
    PeSemanticSegmentedEmission segmented{};
    check(owner.emitSegmented(segmented) &&
              ownerSegmentedRolesEqualExact(exact, segmented),
          "seven-family valid row matches canonical segmented bytes");
    std::size_t visited = 0u;
    owner.visitCommittedPendingChunkLeases(
        [&](const CommittedPendingChunkLease&) noexcept { ++visited; });
    check(visited == 1u && owner.visitCommittedPendingChunkLease(
                              ref,
                              [](const CommittedPendingChunkLease&) noexcept {
                                return true;
                              }),
          "seven-family committed sparse visitor exposes one qualified lease");
    canonical.resetAndReleaseRetained();
    const auto warmRefs = object.refs;
    check(owner.settle() && owner.retainedCount() == 1u,
          "seven-family successful settlement retains a warm pin");
    auto retry = makeInput(2u);
    CommandChunkBuilder retryCanonical;
    check(owner.admit(retry) && owner.retainedCount() == 1u &&
              appendCanonicalSemanticRecord(retryCanonical, retry),
          "seven-family warm retry admits the same qualified object");
    PeSemanticExactFixedEmission retryExact{};
    check(equalCanonicalOwnerBytes(owner, retryCanonical, retryExact),
          "seven-family warm retry preserves canonical bytes");
    retryCanonical.resetAndReleaseRetained();
    check(object.refs == warmRefs,
          "seven-family warm retry avoids a duplicate retain");
    owner.reset();
    check(object.refs == 1u,
          "seven-family warm retry reset releases the owner pin exactly once");
  }

  // Null rows carry a canonical unbind and must not acquire a physical pin.
  {
    Owner owner;
    std::array<Row, 1u> rows{{{.wire = makeNull(), .object = {}}}};
    auto input = base(PeSemanticProducerKind::DrawPrimitive, 3u, 3u);
    configure(input.sparse, std::span<const Row>(rows));
    CommandChunkBuilder canonical;
    check(owner.admit(input) && owner.retainedCount() == 0u &&
              appendCanonicalSemanticRecord(canonical, input),
          "seven-family valid-null row admits without a pin");
    PeSemanticExactFixedEmission exact{};
    PeSemanticSegmentedEmission segmented{};
    check(equalCanonicalOwnerBytes(owner, canonical, exact) &&
              exact.transport.header.handleCount == 0u &&
              owner.emitSegmented(segmented) &&
              ownerSegmentedRolesEqualExact(exact, segmented),
          "seven-family valid-null row preserves exact and segmented bytes");
    check(!owner.visitCommittedPendingChunkLease(
              ref, [](const CommittedPendingChunkLease&) noexcept {
                return true;
              }),
          "seven-family valid-null row has no committed lease authority");
    canonical.resetAndReleaseRetained();
    owner.reset();
  }

  // Inactive rows deliberately retain non-null metadata to prove that the
  // wire-valid bit, rather than a borrowed wrapper, controls authority.
  {
    Owner owner;
    std::array<Row, 1u> rows{{{.wire = makeInactive(), .object = ref}}};
    auto input = base(PeSemanticProducerKind::DrawPrimitive, 4u, 4u);
    configure(input.sparse, std::span<const Row>(rows));
    CommandChunkBuilder canonical;
    check(owner.admit(input) && owner.retainedCount() == 0u &&
              object.refs == 1u &&
              appendCanonicalSemanticRecord(canonical, input),
          "seven-family inactive row ignores nonnull metadata");
    PeSemanticExactFixedEmission exact{};
    check(equalCanonicalOwnerBytes(owner, canonical, exact) &&
              exact.transport.header.handleCount == 0u,
          "seven-family inactive row preserves canonical no-handle bytes");
    canonical.resetAndReleaseRetained();
    owner.reset();
  }

  // Keep all rejection assertions on one accepted prefix so rollback is
  // observable without introducing any new malformed-generic policy.
  {
    Owner owner;
    auto accepted = makeInput(5u);
    check(owner.admit(accepted) && object.refs == 2u,
          "seven-family identity fixture admits its accepted prefix");

    auto wrongKind = makeInput(6u);
    configure(wrongKind.sparse, std::span<const Row>(validRows));
    auto wrongKindRows = validRows;
    wrongKindRows[0].object.identity.kind =
        Kind == D9C_CHUNK_HANDLE_KIND_TEXTURE
            ? D9C_CHUNK_HANDLE_KIND_BUFFER
            : D9C_CHUNK_HANDLE_KIND_TEXTURE;
    configure(wrongKind.sparse, std::span<const Row>(wrongKindRows));
    setDirect(wrongKind, ref);
    check(!owner.admit(wrongKind) && object.refs == 2u,
          "seven-family wrong-kind identity rejects without mutation");

    auto stale = makeInput(7u);
    auto staleRows = validRows;
    ++staleRows[0].object.identity.generation;
    configure(stale.sparse, std::span<const Row>(staleRows));
    setDirect(stale, ref);
    check(!owner.admit(stale) && object.refs == 2u,
          "seven-family stale-generation identity rejects without mutation");

    auto alias = makeInput(8u);
    auto aliasRows = validRows;
    aliasRows[0].object.object = &pointerAlias;
    configure(alias.sparse, std::span<const Row>(aliasRows));
    setDirect(alias, ref);
    check(!owner.admit(alias) && object.refs == 2u && pointerAlias.refs == 1u,
          "seven-family same-id different-pointer identity rejects atomically");
    owner.reset();
    check(object.refs == 1u && pointerAlias.refs == 1u,
          "seven-family identity rejection rollback balances retains");
  }
}

void preparedBindingSevenFamilyMatrix() {
  preparedBindingFamilyMatrix<D9CCommandChunkWireTextureBinding, 2u,
                              D9CTexture, D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      "texture family admits valid duplicate rows",
      [](std::size_t i) {
        return D9CCommandChunkWireTextureBinding{.slot = static_cast<std::uint32_t>(i),
                                                 .valid = 1u};
      },
      [] { return D9CCommandChunkWireTextureBinding{
          .slot = 0u, .valid = 1u,
          .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}; },
      [] { return D9CCommandChunkWireTextureBinding{.slot = 0u, .valid = 0u}; },
      [](SparseStateInput& sparse, auto rows) { sparse.textures = rows; },
      [](PeSemanticRecordInput& input, const auto& ref) { input.texture0 = ref; });

  preparedBindingFamilyMatrix<D9CCommandChunkWireStreamBinding, 2u,
                              D9CBuffer, D9C_CHUNK_HANDLE_KIND_BUFFER>(
      "stream family admits valid duplicate rows",
      [](std::size_t i) {
        return D9CCommandChunkWireStreamBinding{
            .slot = static_cast<std::uint32_t>(i), .valid = 1u,
            .offset = 8u, .stride = 16u, .frequency = 1u};
      },
      [] { return D9CCommandChunkWireStreamBinding{
          .slot = 0u, .valid = 1u,
          .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}; },
      [] { return D9CCommandChunkWireStreamBinding{.slot = 0u, .valid = 0u}; },
      [](SparseStateInput& sparse, auto rows) { sparse.streams = rows; },
      [](PeSemanticRecordInput& input, const auto& ref) { input.buffer0 = ref; });

  preparedBindingFamilyMatrix<D9CCommandChunkWireShaderBinding, 2u,
                              D9CShader, D9C_CHUNK_HANDLE_KIND_SHADER>(
      "shader family admits valid duplicate rows",
      [](std::size_t i) {
        return D9CCommandChunkWireShaderBinding{
            .stage = static_cast<std::uint32_t>(i), .valid = 1u};
      },
      [] { return D9CCommandChunkWireShaderBinding{
          .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX, .valid = 1u,
          .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}; },
      [] { return D9CCommandChunkWireShaderBinding{
          .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX, .valid = 0u}; },
      [](SparseStateInput& sparse, auto rows) { sparse.shaders = rows; },
      [](PeSemanticRecordInput& input, const auto& ref) { input.shader0 = ref; });

  preparedBindingFamilyMatrix<D9CCommandChunkWireVertexInput, 1u,
                              D9CVertexDecl, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL>(
      "vertex-input family admits valid duplicate identity",
      [](std::size_t) {
        return D9CCommandChunkWireVertexInput{
            .valid = 1u, .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION};
      },
      [] { return D9CCommandChunkWireVertexInput{
          .valid = 1u, .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF,
          .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}; },
      [] { return D9CCommandChunkWireVertexInput{
          .valid = 0u, .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF}; },
      [](SparseStateInput& sparse, auto rows) { sparse.vertexInputs = rows; },
      [](PeSemanticRecordInput& input, const auto& ref) {
        input.declaration = ref;
      });

  preparedBindingFamilyMatrix<D9CCommandChunkWireIndexBinding, 1u,
                              D9CBuffer, D9C_CHUNK_HANDLE_KIND_BUFFER>(
      "index-buffer family admits valid duplicate identity",
      [](std::size_t) { return D9CCommandChunkWireIndexBinding{.valid = 1u}; },
      [] { return D9CCommandChunkWireIndexBinding{
          .valid = 1u, .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}; },
      [] { return D9CCommandChunkWireIndexBinding{.valid = 0u}; },
      [](SparseStateInput& sparse, auto rows) { sparse.indexBuffers = rows; },
      [](PeSemanticRecordInput& input, const auto& ref) { input.buffer0 = ref; });

  preparedBindingFamilyMatrix<D9CCommandChunkWireRenderTargetBinding, 2u,
                              D9CSurface, D9C_CHUNK_HANDLE_KIND_SURFACE>(
      "render-target family admits valid duplicate rows",
      [](std::size_t i) {
        return D9CCommandChunkWireRenderTargetBinding{
            .slot = static_cast<std::uint32_t>(i), .valid = 1u};
      },
      [] { return D9CCommandChunkWireRenderTargetBinding{
          .slot = 0u, .valid = 1u,
          .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}; },
      [] { return D9CCommandChunkWireRenderTargetBinding{.slot = 0u, .valid = 0u}; },
      [](SparseStateInput& sparse, auto rows) { sparse.renderTargets = rows; },
      [](PeSemanticRecordInput& input, const auto& ref) { input.surface0 = ref; });

  preparedBindingFamilyMatrix<D9CCommandChunkWireDepthStencilBinding, 1u,
                              D9CSurface, D9C_CHUNK_HANDLE_KIND_SURFACE>(
      "depth-stencil family admits valid duplicate identity",
      [](std::size_t) {
        return D9CCommandChunkWireDepthStencilBinding{.valid = 1u};
      },
      [] { return D9CCommandChunkWireDepthStencilBinding{
          .valid = 1u, .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}; },
      [] { return D9CCommandChunkWireDepthStencilBinding{.valid = 0u}; },
      [](SparseStateInput& sparse, auto rows) { sparse.depthStencils = rows; },
      [](PeSemanticRecordInput& input, const auto& ref) { input.surface0 = ref; });
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
    check(appendCanonicalSemanticRecord(canonicalBuilder, in),
          "canonical builder emits every semantic producer row");
  }
  check(owner.size() == 21u && owner.retainedCount() == 3u,
        "all-family owner retains each typed identity exactly once");
  const auto canonical = canonicalBuilder.seal();
  PeSemanticExactFixedEmission ownerEmission{};
  check(canonical.valid() && owner.emitExactFixed(ownerEmission) &&
            ownerEmission.valid() && canonical.blob.size() == ownerEmission.wire.size() &&
            std::equal(canonical.blob.begin(), canonical.blob.end(),
                       ownerEmission.wire.begin()),
        "all-family owner bytes match canonical builder final wire");
  check(ownerEmission.transport.header.recordCount == 21u &&
            ownerEmission.transport.header.handleCount != 0u,
        "all-family owner final wire has qualified contiguous roles");
  PeSemanticOwnerPhaseObserver allFamilyObserver(1u);
  owner.setPhaseObserver(&allFamilyObserver);
  alignas(8) std::array<std::byte, 8192u> observedExact;
  std::fill(observedExact.begin(), observedExact.end(), std::byte{0x5a});
  PeSemanticExactFixedEmission observedEmission{};
  check(owner.emitExactFixed(observedExact, observedEmission) &&
            observedEmission.wireBytes == ownerEmission.wireBytes &&
            std::equal(ownerEmission.wire.begin(), ownerEmission.wire.end(),
                       observedEmission.wire.begin()) &&
            std::all_of(observedExact.begin() + observedEmission.wireBytes,
                        observedExact.end(), [](std::byte value) {
                          return value == std::byte{0x5a};
                        }),
        "observed all-family ExactFixed preserves bytes and canary");
  owner.setPhaseObserver(nullptr);
  std::size_t visited = 0u;
  check(owner.visitOwnedRecords([&](PeSemanticProducerKind kind,
                                    const PeSemanticRecordReceipt& slot) noexcept {
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

void preparedLayoutCoversEverySparseAndVariableSection() {
  using LayoutOwner = PeSemanticBatchOwner<4u, 16u, 4096u, 8u, 8u>;
  LayoutOwner owner;
  CommandChunkBuilder canonical;
  D9CTexture texture;
  D9CBuffer buffer;
  D9CShader shader;
  D9CVertexDecl declaration;
  D9CSurface surface;
  const auto textureRef = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &texture, 0x1101u, 7u);
  const auto bufferRef = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(
      &buffer, 0x1102u, 8u);
  const auto shaderRef = localRef<D9C_CHUNK_HANDLE_KIND_SHADER>(
      &shader, 0x1103u, 9u);
  const auto declarationRef = localRef<D9C_CHUNK_HANDLE_KIND_VERTEX_DECL>(
      &declaration, 0x1104u, 10u);
  const auto surfaceRef = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &surface, 0x1105u, 11u);

  std::array<D9CCommandChunkWireRenderState, 1u> renderStates{{
      D9CCommandChunkWireRenderState{7u, 0x101u}}};
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u> textures{{
      SparseBindingInput<D9CCommandChunkWireTextureBinding>{
      {.slot = 3u, .valid = 1u}, textureRef}}};
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u> streams{{
      SparseBindingInput<D9CCommandChunkWireStreamBinding>{
      {.slot = 4u, .valid = 1u, .offset = 8u, .stride = 16u,
       .frequency = 1u}, bufferRef}}};
  std::array<SparseBindingInput<D9CCommandChunkWireShaderBinding>, 1u> shaders{{
      SparseBindingInput<D9CCommandChunkWireShaderBinding>{
      {.stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX, .valid = 1u},
      shaderRef}}};
  std::array<SparseBindingInput<D9CCommandChunkWireVertexInput>, 1u> vertexInputs{{
      SparseBindingInput<D9CCommandChunkWireVertexInput>{
      {.valid = 1u, .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION},
      declarationRef}}};
  std::array<SparseBindingInput<D9CCommandChunkWireIndexBinding>, 1u> indexBuffers{{
      SparseBindingInput<D9CCommandChunkWireIndexBinding>{
      {.valid = 1u}, bufferRef}}};
  std::array<SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>, 1u>
      renderTargets{{SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>{
          {.slot = 2u, .valid = 1u}, surfaceRef}}};
  std::array<SparseBindingInput<D9CCommandChunkWireDepthStencilBinding>, 1u>
      depthStencils{{SparseBindingInput<D9CCommandChunkWireDepthStencilBinding>{
          {.valid = 1u}, surfaceRef}}};
  std::array<D9CViewport, 1u> viewports{{
      D9CViewport{3u, 5u, 127u, 91u, 0.25f, 0.75f}}};
  std::array<D9CRect, 1u> scissors{{D9CRect{11, 13, 47, 61}}};
  std::array<D9CMaterial, 1u> materials{{D9CMaterial{
      .diffuse = {0.125f, 0.25f, 0.5f, 1.0f}, .power = 17.0f}}};
  std::array<D9CCommandChunkWireClipPlane, 1u> clipPlanes{{D9CCommandChunkWireClipPlane{
      .slot = 2u, .values = {0.5f, 0.25f, -1.0f, 2.0f}}}};
  std::array<D9CDrawPacketTextureStageState, 1u> textureStageStates{{
      D9CDrawPacketTextureStageState{1u, 2u, 5u}}};
  std::array<D9CDrawPacketSamplerState, 1u> samplerStates{{
      D9CDrawPacketSamplerState{3u, 4u, 2u}}};
  std::array<D9CDrawPacketTransform, 1u> transforms{{D9CDrawPacketTransform{
      .state = 2u,
                                                  .matrix = {.m = {1.0f}}}}};
  transforms[0].matrix.m[7] = 0.5f;
  transforms[0].matrix.m[15] = 2.0f;
  std::array<D9CCommandChunkWireLight, 1u> lights{{D9CCommandChunkWireLight{.slot = 3u,
                                               .light = {.type = 1u,
                                                         .range = 23.0f}}}};
  std::array<D9CCommandChunkWireLightEnable, 1u> lightEnables{{
      D9CCommandChunkWireLightEnable{3u, 1u}}};
  const std::array<std::byte, 16u> vsFloat{
      std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
      std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18},
      std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b}, std::byte{0x1c},
      std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f}, std::byte{0x20}};
  const std::array<std::byte, 16u> vsInt = {
      std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24},
      std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28},
      std::byte{0x29}, std::byte{0x2a}, std::byte{0x2b}, std::byte{0x2c},
      std::byte{0x2d}, std::byte{0x2e}, std::byte{0x2f}, std::byte{0x30}};
  const std::array<std::byte, 4u> vsBool{
      std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}};
  const std::array<std::byte, 16u> psFloat = vsFloat;
  const std::array<std::byte, 16u> psInt = vsInt;
  const std::array<std::byte, 4u> psBool = vsBool;
  const std::array<std::byte, 6u> upIndex{
      std::byte{0xa1}, std::byte{0xa2}, std::byte{0xa3}, std::byte{0xa4},
      std::byte{0xa5}, std::byte{0xa6}};
  const std::array<std::byte, 12u> upVertex{
      std::byte{0xb1}, std::byte{0xb2}, std::byte{0xb3}, std::byte{0xb4},
      std::byte{0xb5}, std::byte{0xb6}, std::byte{0xb7}, std::byte{0xb8},
      std::byte{0xb9}, std::byte{0xba}, std::byte{0xbb}, std::byte{0xbc}};

  const SparseStateInput sparse{
      .renderStates = renderStates,
      .textures = textures,
      .streams = streams,
      .shaders = shaders,
      .vertexInputs = vertexInputs,
      .indexBuffers = indexBuffers,
      .renderTargets = renderTargets,
      .depthStencils = depthStencils,
      .viewports = viewports,
      .scissors = scissors,
      .materials = materials,
      .clipPlanes = clipPlanes,
      .textureStageStates = textureStageStates,
      .samplerStates = samplerStates,
      .transforms = transforms,
      .lights = lights,
      .lightEnables = lightEnables,
      .vsFloatConstants = {2u, 1u, vsFloat},
      .vsIntConstants = {3u, 1u, vsInt},
      .vsBoolConstants = {4u, 1u, vsBool},
      .psFloatConstants = {5u, 1u, psFloat},
      .psIntConstants = {6u, 1u, psInt},
      .psBoolConstants = {7u, 1u, psBool},
      .upIndexData = upIndex,
      .upVertexData = upVertex,
  };
  auto input = base(PeSemanticProducerKind::DrawIndexedPrimitiveUp, 1u, 1u);
  input.draw.primitiveType = 4u;
  input.draw.numVertices = 3u;
  input.draw.primitiveCount = 1u;
  input.draw.stride = 4u;
  input.draw.indexFormat = 101u;
  input.sparse = sparse;

  PeSemanticPreparedRecord witness{};
  check(owner.prepareAdmission(input, witness) ==
            PeSemanticAdmissionOutcome::Admissible && witness.plan.valid,
        "one prepared layout admits every sparse and variable section");
  check(witness.plan.handleCount == 5u && witness.plan.payloadBytes != 0u &&
            witness.plan.semanticBytes ==
                vsFloat.size() + vsInt.size() + vsBool.size() +
                    psFloat.size() + psInt.size() + psBool.size() +
                    upIndex.size() + upVertex.size(),
        "prepared layout accounts for qualified handles and semantic bytes");
  for (const auto count : witness.plan.sparseCounts)
    check(count == 1u, "prepared layout publishes every sparse category count");

  check(owner.admit(input) && appendSparseRecord(
            canonical, input.recordType, input.draw, input.sparse),
        "prepared all-section input enters the independent canonical oracle");
  PeSemanticExactFixedEmission exact{};
  check(equalCanonicalOwnerBytes(owner, canonical, exact),
        "prepared all-section bytes equal CommandChunkBuilder output");
  PeSemanticSegmentedEmission segmented{};
  check(owner.emitSegmented(segmented) &&
            ownerSegmentedRolesEqualExact(exact, segmented),
        "prepared all-section contiguous and segmented output agree");

  dxmt9::d3d9::ImportedChunkView imported;
  const auto envelope = dxmt9::d3d9::CommandChunkEnvelope{
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = exact.transport.header.recordCount,
      .handleCount = exact.transport.header.handleCount,
      .producerIdentity = exact.transport.producerIdentity};
  check(dxmt9::d3d9::validateCommandChunk(exact.wire, envelope, &imported).valid() &&
            imported.record(0u).sections.size() == D9C_COMMAND_CHUNK_SECTION_COUNT,
        "prepared layout validates with all 25 sections in schema order");
  const auto record = imported.record(0u);
  for (std::size_t index = 0u; index < record.sections.size(); ++index) {
    const auto section = record.section(index);
    const auto* rule = dxmt9::d3d9::sectionRule(section.descriptor.kind);
    check(section.descriptor.kind == index + 1u && rule != nullptr &&
              section.descriptor.payloadOffset % rule->payloadAlignment == 0u &&
              section.descriptor.byteSize != 0u &&
              std::any_of(section.payload.begin(), section.payload.end(),
                          [](std::byte value) { return value != std::byte{}; }),
          "prepared section order, alignment, and nonzero values are retained");
  }
  check(record.section(0u).descriptor.count == 1u &&
            record.section(1u).descriptor.count == 1u &&
            record.section(2u).descriptor.count == 1u &&
            record.section(3u).descriptor.count == 1u &&
            record.section(4u).descriptor.count == 1u &&
            record.section(5u).descriptor.count == 1u &&
            record.section(6u).descriptor.count == 1u &&
            record.section(7u).descriptor.count == 1u &&
            record.section(17u).descriptor.count == 1u &&
            record.section(23u).descriptor.count == upIndex.size() &&
            record.section(24u).descriptor.count == upVertex.size(),
        "all seven binding families and constant/UP counts remain logical");

  // Every binding family accepts a canonical nullable row without creating a
  // physical pin. This is deliberately separate from the populated matrix so
  // a null unbind cannot be hidden by a neighboring retained identity.
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
      nullTextures{{{.wire = {.slot = 0u, .valid = 1u,
                               .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}}}};
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u>
      nullStreams{{{.wire = {.slot = 0u, .valid = 1u,
                             .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}}}};
  std::array<SparseBindingInput<D9CCommandChunkWireShaderBinding>, 1u>
      nullShaders{{{.wire = {.stage = 0u, .valid = 1u,
                             .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}}}};
  std::array<SparseBindingInput<D9CCommandChunkWireVertexInput>, 1u>
      nullVertexInputs{{{.wire = {.valid = 1u, .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF,
                                  .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}}}};
  std::array<SparseBindingInput<D9CCommandChunkWireIndexBinding>, 1u>
      nullIndexBuffers{{{.wire = {.valid = 1u,
                                  .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}}}};
  std::array<SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>, 1u>
      nullRenderTargets{{{.wire = {.slot = 0u, .valid = 1u,
                                   .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}}}};
  std::array<SparseBindingInput<D9CCommandChunkWireDepthStencilBinding>, 1u>
      nullDepthStencils{{{.wire = {.valid = 1u,
                                   .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}}}};
  const auto nullable = [&](auto configure, std::string_view message) {
    LayoutOwner nullableOwner;
    CommandChunkBuilder nullableCanonical;
    auto nullInput = base(PeSemanticProducerKind::DrawPrimitive, 2u, 2u);
    SparseStateInput nullSparse{};
    configure(nullSparse);
    nullInput.sparse = nullSparse;
    check(nullableOwner.admit(nullInput) && nullableOwner.retainedCount() == 0u &&
              appendSparseRecord(nullableCanonical, nullInput.recordType,
                                 nullInput.draw, nullInput.sparse),
          message);
    PeSemanticExactFixedEmission nullableEmission{};
    check(equalCanonicalOwnerBytes(nullableOwner, nullableCanonical,
                                   nullableEmission) &&
              nullableEmission.transport.header.handleCount == 0u,
          "nullable binding bytes equal the canonical no-handle row");
  };
  nullable([&](SparseStateInput& value) { value.textures = nullTextures; },
           "nullable texture row admits");
  nullable([&](SparseStateInput& value) { value.streams = nullStreams; },
           "nullable stream row admits");
  nullable([&](SparseStateInput& value) { value.shaders = nullShaders; },
           "nullable shader row admits");
  nullable([&](SparseStateInput& value) { value.vertexInputs = nullVertexInputs; },
           "nullable vertex-input row admits");
  nullable([&](SparseStateInput& value) { value.indexBuffers = nullIndexBuffers; },
           "nullable index-buffer row admits");
  nullable([&](SparseStateInput& value) { value.renderTargets = nullRenderTargets; },
           "nullable render-target row admits");
  nullable([&](SparseStateInput& value) { value.depthStencils = nullDepthStencils; },
           "nullable depth-stencil row admits");

  // MaxSparseValues is a logical per-category quota, not a global count.
  using CategoryOwner = PeSemanticBatchOwner<4u, 8u, 4096u, 8u, 1u>;
  CategoryOwner quota;
  check(quota.admit(input), "one row in each sparse category fits its quota");
  auto extra = input;
  extra.sourceOrdinal = extra.recordOrdinal = 2u;
  check(!quota.admit(extra) &&
            quota.lastAdmissionFailure() == CategoryOwner::AdmissionFailure::Capacity &&
            quota.size() == 1u,
        "logical per-category sparse quota rejects cumulative rows");
  owner.reset();
  canonical.resetAndReleaseRetained();
  quota.reset();
  check(texture.refs == 1u && buffer.refs == 1u && shader.refs == 1u &&
            declaration.refs == 1u && surface.refs == 1u,
        "all-family prepared layout reset releases every typed pin");
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

void emptyClearSpanMatchesCanonicalAndResetReuse() {
  Owner owner;
  CommandChunkBuilder canonical;
  auto empty = base(PeSemanticProducerKind::Clear, 1u, 1u);
  check(owner.admit(empty) && appendCanonicalSemanticRecord(canonical, empty),
        "default empty clear rect span admits and emits");
  PeSemanticExactFixedEmission first{};
  check(equalCanonicalOwnerBytes(owner, canonical, first),
        "default empty clear rect span matches canonical bytes");
  std::array<std::byte, 4096u> firstBytes{};
  std::copy(first.wire.begin(), first.wire.begin() + first.wireBytes,
            firstBytes.begin());
  owner.reset();
  canonical.resetAndReleaseRetained();

  const std::array<D9CRect, 1u> rects{{{3, 5, 17, 23}}};
  auto populated = base(PeSemanticProducerKind::Clear, 2u, 2u);
  populated.clear.rectCount = 1u;
  populated.clearRects = rects;
  check(owner.admit(populated) &&
            appendCanonicalSemanticRecord(canonical, populated),
        "clear rect span populates reusable owner state");
  PeSemanticExactFixedEmission populatedEmission{};
  check(equalCanonicalOwnerBytes(owner, canonical, populatedEmission),
        "populated clear rect span matches canonical bytes");
  owner.reset();
  canonical.resetAndReleaseRetained();

  auto reused = base(PeSemanticProducerKind::Clear, 3u, 3u);
  check(owner.admit(reused) && appendCanonicalSemanticRecord(canonical, reused),
        "default empty clear rect span re-admits after reset");
  PeSemanticExactFixedEmission reusedEmission{};
  check(equalCanonicalOwnerBytes(owner, canonical, reusedEmission) &&
            reusedEmission.wireBytes == first.wireBytes &&
            std::equal(firstBytes.begin(), firstBytes.begin() + first.wireBytes,
                       reusedEmission.wire.begin()),
        "post-reset empty clear reuse matches the original canonical bytes");
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

void exactPaddingWriterAdversarial() {
  using AdversarialOwner = PeSemanticBatchOwner<4u, 8u, 512u, 8u, 8u>;
  AdversarialOwner owner;
  const std::array<D9CCommandChunkWireRenderState, 1u> renderStates{{
      {.state = 7u, .value = 11u},
  }};
  std::array<std::byte, 6u> largeIndex{};
  for (std::size_t i = 0u; i < largeIndex.size(); ++i)
    largeIndex[i] = static_cast<std::byte>((i * 13u + 3u) & 0xffu);
  std::array<std::byte, 127u> largeVertex{};
  for (std::size_t i = 0u; i < largeVertex.size(); ++i)
    largeVertex[i] = static_cast<std::byte>((i * 17u + 5u) & 0xffu);
  const std::array<std::byte, 16u> constants{{
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
      std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88},
      std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc},
      std::byte{0xdd}, std::byte{0xee}, std::byte{0xf0}, std::byte{0x01},
  }};
  SparseStateInput sparse{};
  sparse.renderStates = renderStates;
  sparse.vsFloatConstants = {
      .startRegister = 3u, .registerCount = 1u, .registerBytes = constants};
  sparse.upIndexData = largeIndex;
  sparse.upVertexData = largeVertex;

  auto large = base(PeSemanticProducerKind::DrawIndexedPrimitiveUp, 1u, 1u);
  large.draw.primitiveType = 4u;
  large.draw.numVertices = static_cast<std::uint32_t>(largeVertex.size());
  large.draw.primitiveCount = 1u;
  large.draw.stride = 1u;
  large.draw.indexFormat = 101u;
  large.sparse = sparse;
  D9CSurface surface;
  auto small = base(PeSemanticProducerKind::Present, 2u, 2u);
  small.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x710u);
  check(owner.admit(large) && owner.admit(small),
        "mixed-alignment large-then-small records admit");

  CommandChunkBuilder canonical;
  check(appendSparseRecord(canonical, large.recordType, large.draw, sparse) &&
            appendPresent(canonical, small.present, small.surface0),
        "mixed-alignment canonical records emit");
  const auto sealed = canonical.seal();
  std::vector<std::byte> exact(sealed.blob.size() + 32u, std::byte{0xa5});
  PeSemanticExactFixedEmission exactEmission{};
  check(sealed.valid() && owner.emitExactFixed(exact, exactEmission) &&
            exactEmission.wireBytes == sealed.blob.size() &&
            std::equal(sealed.blob.begin(), sealed.blob.end(),
                       exactEmission.wire.begin()) &&
            std::all_of(exact.begin() + exactEmission.wireBytes, exact.end(),
                        [](std::byte value) { return value == std::byte{0xa5}; }),
        "prefilled ExactFixed destination preserves canonical bytes and tail");

  D9CCommandChunkWireRecordHeader firstRecord{};
  D9CCommandChunkWireRecordHeader secondRecord{};
  const auto& header = exactEmission.transport.header;
  const auto envelope = dxmt9::d3d9::CommandChunkEnvelope{
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
      .producerIdentity = exactEmission.transport.producerIdentity};
  dxmt9::d3d9::ImportedChunkView imported;
  check(dxmt9::d3d9::validateCommandChunk(exactEmission.wire, envelope,
                                          &imported)
            .valid(),
        "mixed-alignment ExactFixed output passes the production validator");
  std::memcpy(&firstRecord,
              exact.data() + header.recordTableOffset,
              sizeof(firstRecord));
  std::memcpy(&secondRecord,
              exact.data() + header.recordTableOffset + sizeof(firstRecord),
              sizeof(secondRecord));
  const auto firstEnd = static_cast<std::size_t>(header.payloadArenaOffset) +
      firstRecord.payloadOffset + firstRecord.payloadSize;
  const auto secondStart = static_cast<std::size_t>(header.payloadArenaOffset) +
      secondRecord.payloadOffset;
  check(firstRecord.payloadSize > secondRecord.payloadSize &&
            secondStart > firstEnd &&
        std::all_of(exact.begin() + firstEnd, exact.begin() + secondStart,
                        [](std::byte value) { return value == std::byte{0}; }),
        "mixed section alignment emits a zeroed inter-record gap");

  const auto firstImportedRecord = imported.record(0u);
  const D9CCommandChunkWireSectionDesc* upIndexDesc = nullptr;
  const D9CCommandChunkWireSectionDesc* upVertexDesc = nullptr;
  for (const auto& desc : firstImportedRecord.sections) {
    if (desc.kind == D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA)
      upIndexDesc = &desc;
    if (desc.kind == D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA)
      upVertexDesc = &desc;
  }
  check(upIndexDesc != nullptr && upVertexDesc != nullptr &&
            upIndexDesc->byteSize == largeIndex.size() &&
            upIndexDesc->byteSize % 4u != 0u &&
            upVertexDesc->byteSize == largeVertex.size(),
        "validated indexed UP record exposes both raw data sections");
  const auto firstPayloadStart = static_cast<std::size_t>(header.payloadArenaOffset) +
      firstRecord.payloadOffset;
  const auto indexEnd = firstPayloadStart + upIndexDesc->payloadOffset +
      upIndexDesc->byteSize;
  const auto vertexStart = firstPayloadStart + upVertexDesc->payloadOffset;
  check(vertexStart > indexEnd &&
            std::all_of(exact.begin() + indexEnd, exact.begin() + vertexStart,
                        [](std::byte value) { return value == std::byte{0}; }),
        "validated indexed UP record zeroes the internal index-to-vertex gap");

  std::array<std::byte, 1024u> segmentedRecords{};
  std::array<std::byte, 1024u> segmentedHandles{};
  std::array<std::byte, 4096u> segmentedPayload{};
  std::fill(segmentedRecords.begin(), segmentedRecords.end(), std::byte{0x5a});
  std::fill(segmentedHandles.begin(), segmentedHandles.end(), std::byte{0x5a});
  std::fill(segmentedPayload.begin(), segmentedPayload.end(), std::byte{0x5a});
  PeSemanticSegmentedEmission segmented{};
  check(owner.emitSegmented(segmentedRecords, segmentedHandles, segmentedPayload,
                            segmented) &&
            std::equal(exact.begin() + header.recordTableOffset,
                       exact.begin() + header.recordTableOffset +
                           segmented.transport.recordBytes,
                       segmentedRecords.begin()) &&
            std::equal(exact.begin() + header.handleTableOffset,
                       exact.begin() + header.handleTableOffset +
                           segmented.transport.handleBytes,
                       segmentedHandles.begin()) &&
            std::equal(exact.begin() + header.payloadArenaOffset,
                       exact.begin() + header.payloadArenaOffset +
                           segmented.transport.payloadBytes,
                       segmentedPayload.begin()),
        "ExactFixed and segmented roles preserve identical bytes and order");
  check(dxmt9::d3d9::validateSegmentedCommandChunk(
            segmented.transport,
            std::span<const std::byte>(segmentedRecords.data(),
                                       segmented.transport.recordBytes),
            std::span<const std::byte>(segmentedHandles.data(),
                                       segmented.transport.handleBytes),
            std::span<const std::byte>(segmentedPayload.data(),
                                       segmented.transport.payloadBytes),
            envelope)
            .valid(),
        "mixed-alignment segmented output passes the production validator");

  AdversarialOwner rollbackOwner;
  check(!rollbackOwner.tryAppendOwnedRecord(
              large, []() noexcept { return false; }) &&
            rollbackOwner.size() == 0u,
        "failed settlement rolls back the large canonical record");
  auto retrySmall = small;
  retrySmall.sourceOrdinal = 3u;
  retrySmall.recordOrdinal = 3u;
  check(rollbackOwner.admit(retrySmall),
        "small record retries after a rolled-back large record");
  CommandChunkBuilder retryCanonical;
  check(appendPresent(retryCanonical, retrySmall.present, retrySmall.surface0),
        "retry canonical small record emits");
  const auto retrySealed = retryCanonical.seal();
  PeSemanticExactFixedEmission retryEmission{};
  check(retrySealed.valid() && rollbackOwner.emitExactFixed(retryEmission) &&
            retrySealed.blob.size() == retryEmission.wireBytes &&
            std::equal(retrySealed.blob.begin(), retrySealed.blob.end(),
                       retryEmission.wire.begin()),
        "retry after failure does not expose stale large-record payload bytes");
  rollbackOwner.reset();
  retryCanonical.resetAndReleaseRetained();
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

void preparedWireWitnessPreservesOrderAndQualifiedDedup() {
  Owner owner;
  D9CTexture texture;
  D9CBuffer buffer;
  const auto textureRef = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &texture, 0x631u, 7u);
  const auto bufferRef = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(
      &buffer, 0x631u, 8u);
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
      textures{{{.wire = {.slot = 0u, .valid = 1u}, .object = textureRef}}};
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u>
      streams{{{.wire = {.slot = 1u, .valid = 1u}, .object = bufferRef}}};
  auto input = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
  // The direct texture pin is retained but is not independently wire-visible
  // for a draw; sparse texture[0] makes the same identity wire-visible.
  input.texture0 = textureRef;
  input.sparse.textures = textures;
  input.sparse.streams = streams;

  PeSemanticPreparedRecord witness{};
  check(owner.prepareAdmission(input, witness) ==
            PeSemanticAdmissionOutcome::Admissible &&
            witness.identityWitness.wireCount == 2u &&
            witness.identityWitness.wireIdentity(0).kind == textureRef.identity.kind &&
            witness.identityWitness.wireIdentity(0).generation ==
                textureRef.identity.generation &&
            witness.identityWitness.wireIdentity(0).objectId ==
                textureRef.identity.objectId &&
            witness.identityWitness.wireIdentity(1).kind == bufferRef.identity.kind &&
            witness.identityWitness.wireIdentity(1).generation ==
                bufferRef.identity.generation &&
            witness.identityWitness.wireIdentity(1).objectId ==
                bufferRef.identity.objectId,
        "prepared witness captures first-seen sparse wire order with direct dedup");
  check(owner.tryAppendOwnedRecord(input, []() noexcept { return true; }),
        "qualified wire witness fixture admits");
  alignas(8) std::array<std::byte, 4096u> bytes{};
  PeSemanticExactFixedEmission emission{};
  check(owner.emitExactFixed(bytes, emission) && emission.valid() &&
            emission.transport.header.handleCount == 2u,
        "qualified wire witness emits both distinct kind identities");
  D9CCommandChunkWireHandleEntry handles[2u]{};
  std::memcpy(handles, bytes.data() + emission.transport.header.handleTableOffset,
              sizeof(handles));
  check(handles[0].kind == textureRef.identity.kind &&
            handles[0].generation == textureRef.identity.generation &&
            handles[0].objectId == textureRef.identity.objectId &&
            handles[1].kind == bufferRef.identity.kind &&
            handles[1].generation == bufferRef.identity.generation &&
            handles[1].objectId == bufferRef.identity.objectId,
        "materialized handles equal the prepared qualified order byte-for-byte");
  owner.reset();
  check(texture.refs == 1u && buffer.refs == 1u,
        "qualified wire witness rollback/reset balances both typed retains");
}

void knownUniqueWireWitnessAppendIsBounded() {
  PeSemanticEmissionHandleContext context{};
  const D9CWireObjectIdentity first{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 1u, .objectId = 0x651u};
  check(context.add(first), "compatibility wire witness add accepts first identity");
  check(context.add(first) && context.count == 1u,
        "compatibility wire witness add still deduplicates identities");
  check(context.identities[0].kind == first.kind &&
            context.identities[0].generation == first.generation &&
            context.identities[0].objectId == first.objectId,
        "wire witness keeps the first identity at its known ordinal");
  for (std::size_t i = context.count; i < context.identities.size(); ++i) {
    check(context.appendKnownUnique({
              .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
              .generation = 1u,
              .objectId = static_cast<std::uint32_t>(0x651u + i)}),
          "known-unique wire witness append accepts bounded identity");
  }
  const auto countAtCapacity = context.count;
  check(!context.appendKnownUnique(first) && context.count == countAtCapacity,
        "known-unique wire witness append fails closed at capacity");
}

void planningReusesStaleWireWitnessStorage() {
  D9CTexture texture;
  D9CBuffer buffer;
  const auto textureRef = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &texture, 0x661u, 7u);
  const auto bufferRef = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(
      &buffer, 0x662u, 8u);
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
      textures{{{.wire = {.slot = 0u, .valid = 1u}, .object = textureRef}}};
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u>
      streams{{{.wire = {.slot = 1u, .valid = 1u}, .object = bufferRef}}};
  auto input = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
  input.sparse.textures = textures;
  input.sparse.streams = streams;

  const D9CWireObjectIdentity stale{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 0xfeedu,
      .objectId = 0xbeefu};
  PeSemanticEmissionHandleContext context{};
  context.identities.fill(stale);
  context.count = context.identities.size();
  PeSemanticAdmissionPlan plan{};
  check(planPeSemanticAdmissionWith(
            input, plan,
            [](const PeWireObjectRef&, std::size_t) noexcept { return true; },
            &context),
        "planning resets a stale wire witness frontier");
  check(context.count == 2u &&
            context.identities[0].kind == textureRef.identity.kind &&
            context.identities[0].generation == textureRef.identity.generation &&
            context.identities[0].objectId == textureRef.identity.objectId &&
            context.identities[1].kind == bufferRef.identity.kind &&
            context.identities[1].generation == bufferRef.identity.generation &&
            context.identities[1].objectId == bufferRef.identity.objectId,
        "planning restores first-seen qualified wire order from stale storage");
  const std::array<D9CCommandChunkWireHandleEntry, 2u> expected{{
      {.kind = textureRef.identity.kind,
       .generation = textureRef.identity.generation,
       .objectId = textureRef.identity.objectId},
      {.kind = bufferRef.identity.kind,
       .generation = bufferRef.identity.generation,
       .objectId = bufferRef.identity.objectId},
  }};
  check(std::memcmp(context.identities.data(), expected.data(),
                    sizeof(expected)) == 0,
        "planning restores canonical active handle bytes");
  check(std::all_of(context.identities.begin() + context.count,
                    context.identities.end(),
                    [&](const auto& identity) { return identity.kind == stale.kind &&
                        identity.generation == stale.generation &&
                        identity.objectId == stale.objectId; }),
        "planning does not require inactive identity storage to be zeroed");
}

void identitySetUsesAnActiveFrontier() {
  using IdentitySet = detail::PeSemanticIdentitySet;
  static_assert(!std::is_copy_constructible_v<IdentitySet>);
  static_assert(!std::is_copy_assignable_v<IdentitySet>);
  static_assert(!std::is_move_constructible_v<IdentitySet>);
  static_assert(!std::is_move_assignable_v<IdentitySet>);
  D9CSurface surface;
  D9CSurface alias;
  IdentitySet prior{};
  const D9CWireObjectIdentity staleIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 0xa5u,
      .objectId = 0x811u};
  for (std::size_t i = 0u; i < prior.values.size(); ++i) {
    prior.values[i] = staleIdentity;
    prior.objects[i] = &surface;
    prior.wireVisible[i] = true;
  }
  prior.count = 0u;
  prior.wireCount = 0u;
  std::array<std::byte, sizeof(IdentitySet)> priorBytes{};
  std::memcpy(priorBytes.data(), &prior, sizeof(prior));
  alignas(IdentitySet) std::array<std::byte, sizeof(IdentitySet)> storage{};
  std::memcpy(storage.data(), priorBytes.data(), priorBytes.size());
  auto* set = ::new (storage.data()) IdentitySet{};
  const auto valuesBegin = offsetof(IdentitySet, values);
  const auto arraysEnd = offsetof(IdentitySet, wireVisible) +
                         sizeof(set->wireVisible);
  check(set->count == 0u && set->wireCount == 0u,
        "identity admission starts with an empty active frontier");
  check(std::memcmp(storage.data() + valuesBegin,
                    priorBytes.data() + valuesBegin,
                    arraysEnd - valuesBegin) == 0,
        "identity admission does not clear adversarial inactive storage");

  bool newlyRetained = true;
  bool newlyWireVisible = true;
  check(set->add(PeWireObjectRef{}, D9C_CHUNK_HANDLE_KIND_SURFACE, false,
                 newlyRetained, newlyWireVisible) &&
            !newlyRetained && !newlyWireVisible && set->count == 0u,
        "empty null refs remain valid without entering the frontier");
  const auto surfaceRef = PeWireObjectRef{
      .identity = {.kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
                   .generation = 3u,
                   .objectId = 0x811u},
      .object = &surface};
  const auto nullObject = PeWireObjectRef{
      .identity = surfaceRef.identity,
      .object = nullptr};
  check(!set->add(nullObject, D9C_CHUNK_HANDLE_KIND_SURFACE, false,
                  newlyRetained, newlyWireVisible) && set->count == 0u,
        "nonempty null refs fail closed without entering the frontier");
  check(set->add(surfaceRef, D9C_CHUNK_HANDLE_KIND_SURFACE, false,
                 newlyRetained, newlyWireVisible) && newlyRetained &&
            !newlyWireVisible && set->count == 1u && set->wireCount == 0u,
        "new direct pins publish a completely written retained entry");
  check(set->add(surfaceRef, D9C_CHUNK_HANDLE_KIND_SURFACE, true,
                 newlyRetained, newlyWireVisible) && !newlyRetained &&
            newlyWireVisible && set->count == 1u && set->wireCount == 1u,
        "duplicate sparse encounter promotes a retained identity to wire");
  check(set->add(surfaceRef, D9C_CHUNK_HANDLE_KIND_SURFACE, true,
                 newlyRetained, newlyWireVisible) && !newlyRetained &&
            !newlyWireVisible && set->count == 1u && set->wireCount == 1u,
        "duplicate wire encounter does not promote twice");

  const auto generationCollision = PeWireObjectRef{
      .identity = {.kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
                   .generation = 4u,
                   .objectId = surfaceRef.identity.objectId},
      .object = &surface};
  check(!set->add(generationCollision, D9C_CHUNK_HANDLE_KIND_SURFACE, false,
                  newlyRetained, newlyWireVisible) && set->count == 1u,
        "generation collision fails closed without advancing the frontier");
  const auto pointerCollision = PeWireObjectRef{
      .identity = surfaceRef.identity,
      .object = &alias};
  check(!set->add(pointerCollision, D9C_CHUNK_HANDLE_KIND_SURFACE, false,
                  newlyRetained, newlyWireVisible) && set->count == 1u,
        "object pointer collision fails closed without advancing the frontier");
  const auto crossKind = PeWireObjectRef{
      .identity = {.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
                   .generation = surfaceRef.identity.generation,
                   .objectId = surfaceRef.identity.objectId},
      .object = &surface};
  check(set->add(crossKind, D9C_CHUNK_HANDLE_KIND_TEXTURE, true,
                 newlyRetained, newlyWireVisible) && newlyRetained &&
            newlyWireVisible && set->count == 2u && set->wireCount == 2u,
        "same object id across kinds remains a distinct wire identity");

  const auto retry = PeWireObjectRef{
      .identity = {.kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
                   .generation = 1u,
                   .objectId = 0x812u},
      .object = &surface};
  check(set->add(retry, D9C_CHUNK_HANDLE_KIND_BUFFER, false,
                 newlyRetained, newlyWireVisible) && newlyRetained &&
            !newlyWireVisible && set->count == 3u,
        "a valid admission retries after a rejected identity collision");
  while (set->count < set->values.size()) {
    const auto ref = PeWireObjectRef{
        .identity = {.kind = D9C_CHUNK_HANDLE_KIND_QUERY,
                     .generation = 1u,
                     .objectId = static_cast<std::uint32_t>(0x900u + set->count)},
        .object = &surface};
    check(set->add(ref, D9C_CHUNK_HANDLE_KIND_QUERY, true, newlyRetained,
                   newlyWireVisible) && newlyRetained && newlyWireVisible,
          "unique identities fill the bounded admission frontier");
  }
  const auto countAtCapacity = set->count;
  const auto wireCountAtCapacity = set->wireCount;
  check(set->add(surfaceRef, D9C_CHUNK_HANDLE_KIND_SURFACE, true,
                 newlyRetained, newlyWireVisible) && !newlyRetained &&
            !newlyWireVisible && set->count == countAtCapacity &&
            set->wireCount == wireCountAtCapacity,
        "existing identities remain admissible at full capacity");
  const auto overflow = PeWireObjectRef{
      .identity = {.kind = D9C_CHUNK_HANDLE_KIND_QUERY,
                   .generation = 1u,
                   .objectId = 0xfffu},
      .object = &surface};
  check(!set->add(overflow, D9C_CHUNK_HANDLE_KIND_QUERY, true,
                  newlyRetained, newlyWireVisible) &&
            set->count == countAtCapacity &&
            set->wireCount == countAtCapacity - 1u,
        "identity capacity failure leaves the active frontier unchanged");
  set->~IdentitySet();
}

void preparedWireWitnessTracksFirstWirePromotion() {
  Owner owner;
  CommandChunkBuilder canonical;
  D9CSurface surface;
  D9CTexture texture;
  const auto surfaceRef = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &surface, 0x641u, 3u);
  const auto textureRef = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &texture, 0x642u, 4u);
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 2u>
      textures{{
          {.wire = {.slot = 0u, .valid = 1u}, .object = textureRef},
          {.wire = {.slot = 1u, .valid = 1u}, .object = textureRef},
      }};
  std::array<SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>, 1u>
      renderTargets{{
          {.wire = {.slot = 0u, .valid = 1u}, .object = surfaceRef},
      }};
  auto input = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
  // The direct surface pin is retained but is not wire-visible for a draw.
  // Sparse texture[0] is the first wire encounter; renderTargets[0] then
  // promotes the preinserted surface and must append it second.
  input.surface0 = surfaceRef;
  input.sparse.textures = textures;
  input.sparse.renderTargets = renderTargets;

  PeSemanticPreparedRecord witness{};
  check(owner.prepareAdmission(input, witness) ==
                PeSemanticAdmissionOutcome::Admissible &&
            witness.plan.uniquePinCounts[0] == 1u &&
            witness.plan.uniquePinCounts[1] == 1u &&
            witness.plan.handleCount == 2u && witness.identityWitness.wireCount == 2u &&
            witness.identityWitness.wireIdentity(0).kind == textureRef.identity.kind &&
            witness.identityWitness.wireIdentity(0).generation == textureRef.identity.generation &&
            witness.identityWitness.wireIdentity(0).objectId == textureRef.identity.objectId &&
            witness.identityWitness.wireIdentity(1).kind == surfaceRef.identity.kind &&
            witness.identityWitness.wireIdentity(1).generation == surfaceRef.identity.generation &&
            witness.identityWitness.wireIdentity(1).objectId == surfaceRef.identity.objectId &&
            surface.refs == 1u && texture.refs == 1u,
        "prepared witness records first wire promotion after direct retention");
  check(appendSparseRecord(canonical, input.recordType, input.draw, input.sparse),
        "promotion fixture canonical sparse emission");
  check(owner.tryAppendOwnedRecord(input, []() noexcept { return true; }),
        "promotion fixture owner admission");
  alignas(8) std::array<std::byte, 4096u> bytes{};
  PeSemanticExactFixedEmission emission{};
  const auto sealed = canonical.seal();
  check(sealed.valid() && owner.emitExactFixed(bytes, emission) &&
            emission.valid() && emission.wireBytes == sealed.blob.size() &&
            std::equal(sealed.blob.begin(), sealed.blob.end(),
                       emission.wire.begin()),
        "promotion fixture final bytes equal canonical oracle");
  D9CCommandChunkWireHandleEntry handles[2u]{};
  std::memcpy(handles, bytes.data() + emission.transport.header.handleTableOffset,
              sizeof(handles));
  check(handles[0].kind == textureRef.identity.kind &&
            handles[0].generation == textureRef.identity.generation &&
            handles[0].objectId == textureRef.identity.objectId &&
            handles[1].kind == surfaceRef.identity.kind &&
            handles[1].generation == surfaceRef.identity.generation &&
            handles[1].objectId == surfaceRef.identity.objectId,
        "promotion fixture final handle order is texture then surface");
  check(surface.refs == 3u && texture.refs == 3u,
        "promotion fixture retains each duplicate sparse identity once");
  owner.reset();
  check(surface.refs == 2u && texture.refs == 2u,
        "promotion fixture owner reset releases its typed retains");
  canonical.resetAndReleaseRetained();
  check(surface.refs == 1u && texture.refs == 1u,
        "promotion fixture canonical reset releases its typed retains");
}

void preparedIdentityWitnessUsesBoundedActivePrefixes() {
  PeSemanticPreparedIdentityWitness witness{};
  std::array<D9CSurface, 80u> surfaces{};
  bool newlyRetained = false;
  bool newlyWireVisible = false;
  for (std::size_t i = 0u; i < surfaces.size(); ++i) {
    const auto ref = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
        &surfaces[i], static_cast<std::uint32_t>(0x900u + i), 1u);
    check(witness.add(ref, D9C_CHUNK_HANDLE_KIND_SURFACE, false,
                     newlyRetained, newlyWireVisible) && newlyRetained &&
              !newlyWireVisible,
          "prepared identity witness fills the 80-entry retain frontier");
  }
  check(witness.count == 80u && witness.wireCount == 0u,
        "retained-only identities do not consume wire ordinals");
  const auto first = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &surfaces[0], 0x900u, 1u);
  check(witness.add(first, D9C_CHUNK_HANDLE_KIND_SURFACE, true,
                   newlyRetained, newlyWireVisible) && !newlyRetained &&
            newlyWireVisible && witness.wireCount == 1u &&
            witness.wireOrdinals[0] == 0u,
        "first wire promotion records retained identity ordinal");
  for (std::size_t i = 1u; i < 64u; ++i) {
    const auto ref = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
        &surfaces[i], static_cast<std::uint32_t>(0x900u + i), 1u);
    check(witness.add(ref, D9C_CHUNK_HANDLE_KIND_SURFACE, true,
                     newlyRetained, newlyWireVisible) && !newlyRetained &&
              newlyWireVisible && witness.wireOrdinals[i] == i,
          "wire promotion preserves first-wire order independently");
  }
  const auto wireCountBefore = witness.wireCount;
  const auto retainedCountBefore = witness.count;
  const auto overflowWire = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &surfaces[64], 0x940u, 1u);
  check(!witness.add(overflowWire, D9C_CHUNK_HANDLE_KIND_SURFACE, true,
                     newlyRetained, newlyWireVisible) &&
            witness.count == retainedCountBefore &&
            witness.wireCount == wireCountBefore,
        "wire frontier overflow fails closed without publishing a pin");
  const auto staleGeneration = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &surfaces[0], 0x900u, 2u);
  check(!witness.add(staleGeneration, D9C_CHUNK_HANDLE_KIND_SURFACE, false,
                    newlyRetained, newlyWireVisible) &&
            witness.count == retainedCountBefore,
        "stale generation collision fails closed");
  D9CSurface alias;
  const auto wrongObject = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &alias, 0x900u, 1u);
  check(!witness.add(wrongObject, D9C_CHUNK_HANDLE_KIND_SURFACE, false,
                    newlyRetained, newlyWireVisible) &&
            witness.count == retainedCountBefore,
        "wrong object collision fails closed");
  const PeWireObjectRef nullRef{};
  check(witness.add(nullRef, D9C_CHUNK_HANDLE_KIND_SURFACE, true,
                   newlyRetained, newlyWireVisible) && !newlyRetained &&
            !newlyWireVisible && witness.count == retainedCountBefore,
        "null identity remains outside both active prefixes");
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
  PeSemanticPreparedSparseLayout layout;
  check(detail::preparePeSemanticSparseLayout(draw, layout) && layout.valid,
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
  CommandChunkBuilder canonical;
  std::array<std::byte, 4u> source{
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
  auto input = base(PeSemanticProducerKind::VsBoolConstant, 1u, 1u);
  input.setConst.registerCount = 1u;
  input.constantBytes = source;
  check(appendCanonicalSemanticRecord(canonical, input) &&
            owner.tryAppendOwnedRecord(input, []() noexcept { return true; }),
        "immediate operation copies span content before returning");
  source[0] = std::byte{0x7f};
  PeSemanticExactFixedEmission exact{};
  check(equalCanonicalOwnerBytes(owner, canonical, exact),
        "post-operation span mutation cannot alter owned canonical payload");
  owner.reset();
  canonical.resetAndReleaseRetained();
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

void preparedAdmissionReinitializesReusedWitness() {
  using Outcome = PeSemanticAdmissionOutcome;
  Owner owner;
  D9CSurface first;
  D9CSurface second;
  D9CSurface replacement;
  PeSemanticPreparedRecord witness{};

  const auto samePrepared = [](const PeSemanticPreparedRecord& lhs,
                               const PeSemanticPreparedRecord& rhs) {
    if (lhs.plan.valid != rhs.plan.valid ||
        lhs.plan.recordType != rhs.plan.recordType ||
        lhs.plan.handleCount != rhs.plan.handleCount ||
        lhs.plan.payloadBytes != rhs.plan.payloadBytes ||
        lhs.plan.rectCount != rhs.plan.rectCount ||
        lhs.plan.semanticBytes != rhs.plan.semanticBytes ||
        lhs.plan.sparseCounts != rhs.plan.sparseCounts ||
        lhs.plan.uniquePinCounts != rhs.plan.uniquePinCounts ||
        lhs.retentionDeltas != rhs.retentionDeltas || lhs.rule != rhs.rule ||
        lhs.producer != rhs.producer ||
        lhs.producerMatchesRecordType != rhs.producerMatchesRecordType ||
        lhs.constantProducer != rhs.constantProducer ||
        lhs.upProducer != rhs.upProducer ||
        lhs.recordCount != rhs.recordCount ||
        lhs.emissionHandleCount != rhs.emissionHandleCount ||
        lhs.emissionPayloadBytes != rhs.emissionPayloadBytes ||
        lhs.payloadOffset != rhs.payloadOffset ||
        lhs.nextEmissionPayloadBytes != rhs.nextEmissionPayloadBytes ||
        lhs.nextEmissionHandleCount != rhs.nextEmissionHandleCount ||
        lhs.wireBytes != rhs.wireBytes || lhs.admissible != rhs.admissible ||
        lhs.identityWitness.count != rhs.identityWitness.count ||
        lhs.identityWitness.wireCount != rhs.identityWitness.wireCount) {
      return false;
    }
    for (std::size_t i = 0u; i < lhs.identityWitness.wireCount; ++i) {
      const auto& left = lhs.identityWitness.wireIdentity(i);
      const auto& right = rhs.identityWitness.wireIdentity(i);
      if (left.kind != right.kind || left.generation != right.generation ||
          left.objectId != right.objectId) {
        return false;
      }
    }
    return true;
  };

  auto larger = base(PeSemanticProducerKind::StretchRect, 1u, 1u);
  larger.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&first, 0xa01u, 1u);
  larger.surface1 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&second, 0xa02u, 1u);
  check(owner.prepareAdmission(larger, witness) == Outcome::Admissible &&
            witness.valid() && witness.identityWitness.wireCount == 2u &&
            witness.plan.handleCount == 2u,
        "reused-witness fixture starts with two first-seen wire identities");

  auto smaller = base(PeSemanticProducerKind::Present, 2u, 2u);
  smaller.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&replacement, 0xa03u, 2u);
  check(owner.prepareAdmission(smaller, witness) == Outcome::Admissible &&
            witness.valid() && witness.identityWitness.wireCount == 1u &&
            witness.plan.handleCount == 1u &&
            witness.identityWitness.wireIdentity(0).objectId == 0xa03u &&
            witness.identityWitness.wireIdentity(0).generation == 2u,
        "valid-to-valid reuse resets the active wire frontier and identity");
  PeSemanticPreparedRecord freshSmaller{};
  check(owner.prepareAdmission(smaller, freshSmaller) == Outcome::Admissible &&
            samePrepared(witness, freshSmaller),
        "valid-to-valid reuse matches a fresh witness across all active facts");

  // The second identity from `larger` remains physically in the bounded
  // witness array, but its stale tail is outside the count and cannot affect
  // the smaller record's plan or later append.
  check(witness.identityWitness.entries[1].identity.objectId == 0xa02u &&
            witness.identityWitness.entries[1].identity.generation == 1u,
        "valid-to-valid reuse leaves only an inactive stale tail");

  auto malformed = smaller;
  malformed.surface0.identity.generation = 0u;
  check(owner.prepareAdmission(malformed, witness) == Outcome::Malformed &&
            !witness.valid() && witness.identityWitness.wireCount == 0u &&
            !witness.plan.valid && witness.plan.handleCount == 0u &&
            witness.retentionDeltas[0] == 0u,
        "valid-to-malformed reuse invalidates the prepared witness");
  PeSemanticPreparedRecord freshMalformed{};
  check(owner.prepareAdmission(malformed, freshMalformed) == Outcome::Malformed &&
            samePrepared(witness, freshMalformed),
        "early malformed reuse matches a fresh witness across active facts");
  check(owner.prepareAdmission(smaller, witness) == Outcome::Admissible &&
            witness.valid() && witness.identityWitness.wireCount == 1u &&
            witness.plan.handleCount == 1u,
        "a failed malformed preparation can be followed by a valid one");
  PeSemanticPreparedRecord freshAfterMalformed{};
  check(owner.prepareAdmission(smaller, freshAfterMalformed) ==
                Outcome::Admissible &&
            samePrepared(witness, freshAfterMalformed),
        "early malformed reuse recovers the complete active witness");

  PeSemanticRecordInputView nullView{};
  PeSemanticPreparedRecord cleared{};
  check(owner.prepareAdmission(nullView, witness) == Outcome::Malformed &&
            samePrepared(witness, cleared),
        "a null input view clears the reused witness on early Malformed");
  check(owner.prepareAdmission(smaller, witness) == Outcome::Admissible,
        "an early null-view Malformed can be followed by a valid preparation");
  PeSemanticPreparedRecord freshAfterNullView{};
  check(owner.prepareAdmission(smaller, freshAfterNullView) ==
                Outcome::Admissible &&
            samePrepared(witness, freshAfterNullView),
        "null-view recovery matches a fresh witness across all active facts");

  check(owner.tryAppendOwnedRecord(smaller, []() noexcept { return true; }),
        "partial-malformed fixture publishes an earlier valid wire identity");
  D9CTexture partialTexture;
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
      partialTextures{{
          {.wire = {.slot = 0u, .valid = 1u},
           .object = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
               &partialTexture, 0xa04u, 1u)},
      }};
  std::array<SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>, 1u>
      partialTargets{{
          {.wire = {.slot = 0u, .valid = 1u}, .object = smaller.surface0},
      }};
  partialTargets[0].object.identity.generation = 3u;
  auto partialMalformed = base(PeSemanticProducerKind::DrawPrimitive, 4u, 4u);
  partialMalformed.surface0 = smaller.surface0;
  partialMalformed.sparse.textures = partialTextures;
  partialMalformed.sparse.renderTargets = partialTargets;
  check(owner.prepareAdmission(partialMalformed, witness) == Outcome::Malformed &&
            !witness.valid() && witness.plan.recordType ==
                                     partialMalformed.recordType &&
            witness.plan.sparseCounts[1] == 1u &&
            witness.plan.sparseCounts[6] == 1u &&
            witness.retentionDeltas[1] == 1u &&
            witness.identityWitness.wireCount == 1u &&
            witness.identityWitness.wireIdentity(0).kind ==
                partialTextures[0].object.identity.kind &&
            witness.identityWitness.wireIdentity(0).objectId ==
                partialTextures[0].object.identity.objectId,
        "planner Malformed preserves the pre-failure active wire prefix");
  PeSemanticPreparedRecord freshPartialMalformed{};
  check(owner.prepareAdmission(partialMalformed, freshPartialMalformed) ==
                Outcome::Malformed &&
            samePrepared(witness, freshPartialMalformed),
        "partial planner Malformed matches a fresh witness across active facts");
  check(owner.prepareAdmission(smaller, witness) == Outcome::Admissible,
        "partial planner Malformed can be followed by a valid preparation");
  PeSemanticPreparedRecord freshAfterPartial{};
  check(owner.prepareAdmission(smaller, freshAfterPartial) ==
                Outcome::Admissible &&
            samePrepared(witness, freshAfterPartial),
        "partial-malformed recovery matches a fresh witness across all active facts");

  std::array<D9CCommandChunkWireRenderState, 9u> renderStates{};
  auto capacity = base(PeSemanticProducerKind::DrawPrimitive, 3u, 3u);
  capacity.sparse.renderStates = renderStates;
  check(owner.prepareAdmission(capacity, witness) == Outcome::Capacity &&
            !witness.valid() && witness.identityWitness.wireCount == 0u &&
            witness.plan.valid,
        "valid-to-capacity reuse remains inadmissible with a well-formed plan");
  PeSemanticPreparedRecord freshCapacity{};
  check(owner.prepareAdmission(capacity, freshCapacity) == Outcome::Capacity &&
            samePrepared(witness, freshCapacity),
        "capacity reuse matches a fresh witness across all active facts");
  check(owner.prepareAdmission(smaller, witness) == Outcome::Admissible &&
            witness.valid() && witness.identityWitness.wireCount == 1u,
        "a failed capacity preparation can be followed by a valid one");
  PeSemanticPreparedRecord freshAfterCapacity{};
  check(owner.prepareAdmission(smaller, freshAfterCapacity) ==
                Outcome::Admissible &&
            samePrepared(witness, freshAfterCapacity),
        "capacity recovery matches a fresh witness across all active facts");

  using PinBound = PeSemanticBatchOwner<4u, 1u, 4096u, 4u, 4u>;
  PinBound pinBound;
  D9CSurface pinned;
  D9CSurface novel;
  auto pinFirst = base(PeSemanticProducerKind::Present, 5u, 5u);
  pinFirst.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&pinned, 0xa05u, 1u);
  check(pinBound.tryAppendOwnedRecord(pinFirst,
                                      []() noexcept { return true; }),
        "handle-bearing capacity fixture fills the pin budget");
  auto handleCapacity = base(PeSemanticProducerKind::Present, 6u, 6u);
  handleCapacity.surface0 =
      localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&novel, 0xa06u, 1u);
  check(pinBound.prepareAdmission(handleCapacity, witness) == Outcome::Capacity &&
            witness.plan.valid && !witness.admissible &&
            witness.identityWitness.wireCount == 1u &&
            witness.retentionDeltas[0] == 1u,
        "handle-bearing Capacity retains the candidate wire prefix");
  PeSemanticPreparedRecord freshHandleCapacity{};
  check(pinBound.prepareAdmission(handleCapacity, freshHandleCapacity) ==
                Outcome::Capacity &&
            samePrepared(witness, freshHandleCapacity),
        "handle-bearing capacity reuse matches a fresh witness");
  pinBound.reset();
  check(pinBound.prepareAdmission(handleCapacity, witness) == Outcome::Admissible,
        "a capacity-failed witness can be reused after its boundary");
  PeSemanticPreparedRecord freshAfterHandleCapacity{};
  check(pinBound.prepareAdmission(handleCapacity, freshAfterHandleCapacity) ==
                Outcome::Admissible &&
            samePrepared(witness, freshAfterHandleCapacity),
        "post-capacity recovery matches a fresh witness");

  Owner unavailable(Owner::FailConstructionForTesting{});
  check(unavailable.prepareAdmission(smaller, witness) == Outcome::Unavailable &&
            !witness.valid() && witness.rule == nullptr &&
            witness.identityWitness.wireCount == 0u && !witness.plan.valid &&
            witness.plan.handleCount == 0u,
        "valid-to-unavailable reuse clears the witness before early return");
  check(samePrepared(witness, cleared),
        "Unavailable resets every active fact to a fresh empty witness");
  check(owner.prepareAdmission(smaller, witness) == Outcome::Admissible &&
            witness.valid() && witness.identityWitness.wireCount == 1u,
        "an unavailable preparation does not poison later valid reuse");
  PeSemanticPreparedRecord freshAfterUnavailable{};
  check(owner.prepareAdmission(smaller, freshAfterUnavailable) ==
                Outcome::Admissible &&
            samePrepared(witness, freshAfterUnavailable),
        "unavailable recovery matches a fresh witness across all active facts");
  owner.reset();
  check(replacement.refs == 1u,
        "the reused-witness fixture releases pins before local objects expire");
}

void directCanonicalScalarAndConstantMasks() {
  Owner owner;
  CommandChunkBuilder canonical;
  std::array<D9CCommandChunkWireRenderState, 1u> renderStates{{{7u, 11u}}};
  std::array<D9CViewport, 1u> viewports{};
  std::array<D9CRect, 1u> scissors{};
  std::array<D9CMaterial, 1u> materials{};
  std::array<D9CCommandChunkWireClipPlane, 1u> clipPlanes{};
  std::array<D9CDrawPacketTextureStageState, 1u> textureStageStates{};
  std::array<D9CDrawPacketSamplerState, 1u> samplerStates{};
  std::array<D9CDrawPacketTransform, 1u> transforms{};
  std::array<D9CCommandChunkWireLight, 1u> lights{};
  std::array<D9CCommandChunkWireLightEnable, 1u> lightEnables{};

  // Nonzero, category-distinct values reject an accidental zero-fill or a
  // same-sized wrong-section copy, not only missing section descriptors.
  viewports[0] = {3u, 5u, 127u, 91u, 0.25f, 0.75f};
  scissors[0] = {11, 13, 47, 61};
  materials[0].diffuse = {0.125f, 0.25f, 0.5f, 1.0f};
  materials[0].power = 17.0f;
  clipPlanes[0].slot = 2u;
  clipPlanes[0].values[0] = 0.5f;
  clipPlanes[0].values[3] = -3.0f;
  textureStageStates[0] = {1u, 2u, 5u};
  samplerStates[0] = {3u, 4u, 2u};
  transforms[0].state = 2u;
  transforms[0].matrix.m[0] = 1.0f;
  transforms[0].matrix.m[7] = 0.5f;
  transforms[0].matrix.m[15] = 2.0f;
  lights[0].slot = 3u;
  lights[0].light.type = 1u;
  lights[0].light.diffuse = {0.25f, 0.5f, 0.75f, 1.0f};
  lights[0].light.range = 23.0f;
  lightEnables[0] = {3u, 1u};

  std::size_t scalarCases = 0u;
  for (std::size_t mask = 0u; mask < (1u << 10u); ++mask) {
    auto input = base(PeSemanticProducerKind::DrawPrimitive,
                      0x1000u + mask, 0x2000u + mask);
    input.draw.primitiveCount = static_cast<std::uint32_t>(mask + 1u);
    if (mask & (1u << 0u)) input.sparse.renderStates = renderStates;
    if (mask & (1u << 1u)) input.sparse.viewports = viewports;
    if (mask & (1u << 2u)) input.sparse.scissors = scissors;
    if (mask & (1u << 3u)) input.sparse.materials = materials;
    if (mask & (1u << 4u)) input.sparse.clipPlanes = clipPlanes;
    if (mask & (1u << 5u))
      input.sparse.textureStageStates = textureStageStates;
    if (mask & (1u << 6u)) input.sparse.samplerStates = samplerStates;
    if (mask & (1u << 7u)) input.sparse.transforms = transforms;
    if (mask & (1u << 8u)) input.sparse.lights = lights;
    if (mask & (1u << 9u)) input.sparse.lightEnables = lightEnables;

    check(owner.admit(input), "every scalar sparse mask admits");
    check(appendCanonicalSemanticRecord(canonical, input),
          "every scalar sparse mask admits in the canonical oracle");
    PeSemanticExactFixedEmission exact{};
    check(equalCanonicalOwnerBytes(owner, canonical, exact),
          "every scalar sparse mask has exact canonical bytes");
    if (mask != 0u && (mask & (mask - 1u)) == 0u) {
      using CategoryQuotaOwner = PeSemanticBatchOwner<2u, 8u, 128u, 4u, 1u>;
      CategoryQuotaOwner quotaOwner;
      check(quotaOwner.admit(input), "each scalar category fits its logical quota");
      auto extra = input;
      ++extra.sourceOrdinal;
      ++extra.recordOrdinal;
      check(!quotaOwner.admit(extra) &&
                quotaOwner.lastAdmissionFailure() ==
                    CategoryQuotaOwner::AdmissionFailure::Capacity &&
                equalCanonicalOwnerBytes(quotaOwner, canonical, exact),
            "each scalar category rejects cumulative plus-one and preserves bytes");
    }
    ++scalarCases;
    owner.reset();
    canonical.resetAndReleaseRetained();
  }
  check(scalarCases == (1u << 10u),
        "scalar refinement covers all 2^10 section-presence masks");

  std::array<std::byte, 16u> vsFloatBytes{};
  std::array<std::byte, 16u> vsIntBytes{};
  std::array<std::byte, 4u> vsBoolBytes{};
  std::array<std::byte, 16u> psFloatBytes{};
  std::array<std::byte, 16u> psIntBytes{};
  std::array<std::byte, 4u> psBoolBytes{};
  for (std::size_t i = 0u; i < vsFloatBytes.size(); ++i) {
    vsFloatBytes[i] = static_cast<std::byte>(0x10u + i);
    vsIntBytes[i] = static_cast<std::byte>(0x30u + i);
    psFloatBytes[i] = static_cast<std::byte>(0x50u + i);
    psIntBytes[i] = static_cast<std::byte>(0x70u + i);
  }
  for (std::size_t i = 0u; i < vsBoolBytes.size(); ++i) {
    vsBoolBytes[i] = static_cast<std::byte>(0x90u + i);
    psBoolBytes[i] = static_cast<std::byte>(0xb0u + i);
  }
  std::size_t constantCases = 0u;
  for (std::size_t mask = 0u; mask < (1u << 6u); ++mask) {
    auto input = base(PeSemanticProducerKind::DrawPrimitive,
                      0x3000u + mask, 0x4000u + mask);
    if (mask & (1u << 0u)) {
      input.sparse.vsFloatConstants = {
          .startRegister = 1u, .registerCount = 1u, .registerBytes = vsFloatBytes};
    }
    if (mask & (1u << 1u)) {
      input.sparse.vsIntConstants = {
          .startRegister = 2u, .registerCount = 1u, .registerBytes = vsIntBytes};
    }
    if (mask & (1u << 2u)) {
      input.sparse.vsBoolConstants = {
          .startRegister = 3u, .registerCount = 1u, .registerBytes = vsBoolBytes};
    }
    if (mask & (1u << 3u)) {
      input.sparse.psFloatConstants = {
          .startRegister = 4u, .registerCount = 1u, .registerBytes = psFloatBytes};
    }
    if (mask & (1u << 4u)) {
      input.sparse.psIntConstants = {
          .startRegister = 5u, .registerCount = 1u, .registerBytes = psIntBytes};
    }
    if (mask & (1u << 5u)) {
      input.sparse.psBoolConstants = {
          .startRegister = 6u, .registerCount = 1u, .registerBytes = psBoolBytes};
    }

    check(owner.admit(input), "every embedded constant mask admits");
    check(appendCanonicalSemanticRecord(canonical, input),
          "every embedded constant mask admits in the canonical oracle");
    PeSemanticExactFixedEmission exact{};
    check(equalCanonicalOwnerBytes(owner, canonical, exact),
          "every embedded constant mask has exact canonical bytes");
    ++constantCases;
    owner.reset();
    canonical.resetAndReleaseRetained();
  }
  check(constantCases == (1u << 6u),
        "constant refinement covers all 2^6 embedded-range masks");

  // Exhaust all length-three words over two payload values, including A/B/A.
  // One mutable source buffer is reused and overwritten after every admission.
  for (std::uint32_t word = 0u; word < 8u; ++word) {
    std::array<std::byte, 4u> values{};
    for (std::uint32_t step = 0u; step < 3u; ++step) {
      values.fill(((word >> step) & 1u) ? std::byte{0x22} : std::byte{0x11});
      auto input = base(PeSemanticProducerKind::VsBoolConstant, step + 1u, step + 1u);
      input.setConst.registerCount = 1u;
      input.constantBytes = values;
      check(appendCanonicalSemanticRecord(canonical, input) && owner.admit(input),
            "each bounded A/B value word admits in source order");
      values.fill(std::byte{0xee});
    }
    PeSemanticExactFixedEmission exact{};
    PeSemanticSegmentedEmission segmented{};
    check(equalCanonicalOwnerBytes(owner, canonical, exact) &&
              owner.emitSegmented(segmented) &&
              ownerSegmentedRolesEqualExact(exact, segmented),
          "each bounded value sequence owns exact bytes after source overwrite");
    owner.reset();
    canonical.resetAndReleaseRetained();
  }
}

void directCanonicalMutationAndPendingRefinement() {
  Owner owner;
  CommandChunkBuilder canonical;
  PeAllFamilySemanticTokenLedger pending;
  D9CTexture bindingTexture;
  D9CTexture replacementTexture;

  std::array<std::byte, 4u> standaloneBytes{
      std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
  auto first = base(PeSemanticProducerKind::VsBoolConstant, 0u, 0u);
  first.setConst.registerCount = 1u;
  first.constantBytes = standaloneBytes;
  first.sourceOrdinal = pending.beginSource(first.recordType);
  first.recordOrdinal = 101u;
  check(first.sourceOrdinal != 0u &&
            appendCanonicalSemanticRecord(canonical, first) && owner.admit(first) &&
            projectLastCommittedSemanticRecord(owner, pending),
        "A admission accepts and projects its standalone constant witness");
  standaloneBytes[0] = std::byte{0xa1};
  standaloneBytes[1] = std::byte{0xa2};

  std::array<D9CCommandChunkWireRenderState, 1u> stagedRows{{{7u, 19u}}};
  std::array<D9CCommandChunkWireRenderState, 1u> destinationRows{{{7u, 29u}}};
  std::array<std::byte, 4u> embeddedBytes{
      std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};
  std::array<std::byte, 2u> upIndexBytes{};
  std::array<std::byte, 3u> upVertexBytes{
      std::byte{0x41}, std::byte{0x42}, std::byte{0x43}};
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
      textureBindings{{{
          .wire = {.slot = 0u, .valid = 1u},
          .object = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
              &bindingTexture, 0xc101u, 7u),
      }}};
  auto stagedSecond = base(PeSemanticProducerKind::DrawIndexedPrimitiveUp, 0u, 0u);
  stagedSecond.draw.primitiveType = 1u;
  stagedSecond.draw.primitiveCount = 1u;
  stagedSecond.draw.numVertices = 1u;
  stagedSecond.draw.stride = 3u;
  stagedSecond.draw.indexFormat = 101u;
  stagedSecond.sparse.renderStates = stagedRows;
  stagedSecond.sparse.vsBoolConstants = {
      .startRegister = 3u, .registerCount = 1u, .registerBytes = embeddedBytes};
  stagedSecond.sparse.upIndexData = upIndexBytes;
  stagedSecond.sparse.upVertexData = upVertexBytes;
  auto destinationSparse = stagedSecond.sparse;
  destinationSparse.renderStates = destinationRows;
  destinationSparse.textures = textureBindings;
  auto second = borrowPeSemanticRecordInput(stagedSecond);
  second.destination.sourceOrdinal = pending.beginSource(stagedSecond.recordType);
  second.destination.recordOrdinal = 202u;
  second.destination.sparse = &destinationSparse;
  check(second.destination.sourceOrdinal != 0u,
        "B issues a source ordinal");
  check(appendCanonicalSemanticRecord(canonical, second),
        "B canonical oracle accepts mixed binding plus scalar state");
  check(owner.admit(second), "B owner accepts mixed binding plus scalar state");
  check(projectLastCommittedSemanticRecord(owner, pending),
        "B projects mixed binding plus scalar state");
  destinationRows[0].value = 0x2au;
  textureBindings[0].wire.slot = 4u;
  textureBindings[0].object = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &replacementTexture, 0xc102u, 8u);
  embeddedBytes[0] = std::byte{0xb1};
  upIndexBytes[0] = std::byte{0xb2};
  upVertexBytes[0] = std::byte{0xb3};

  std::array<D9CRect, 1u> clearRects{{{1, 2, 3, 4}}};
  auto third = base(PeSemanticProducerKind::Clear, 0u, 0u);
  third.clear.rectCount = 1u;
  third.clearRects = clearRects;
  third.sourceOrdinal = pending.beginSource(third.recordType);
  third.recordOrdinal = 303u;
  check(third.sourceOrdinal != 0u &&
            appendCanonicalSemanticRecord(canonical, third) && owner.admit(third) &&
            projectLastCommittedSemanticRecord(owner, pending),
        "A admission accepts and projects its clear-rectangle witness");
  clearRects[0] = {};

  // These mutations happen after all three admissions and immediately before
  // the final emission. The expected builder bytes were already copied at each
  // admission, so only an owner that materializes canonical payload eagerly
  // can preserve the A/B/A sequence.
  standaloneBytes[2] = std::byte{0xc1};
  destinationRows[0].value = 0x3au;
  embeddedBytes[1] = std::byte{0xc2};
  upIndexBytes[1] = std::byte{0xc3};
  upVertexBytes[1] = std::byte{0xc4};
  clearRects[0] = {};

  PeSemanticExactFixedEmission exact{};
  const auto sealed = canonical.seal();
  check(sealed.valid() && owner.emitExactFixed(exact) && exact.valid() &&
            sealed.blob.size() == exact.wireBytes &&
            std::equal(sealed.blob.begin(), sealed.blob.end(), exact.wire.begin()),
        "accepted A/B/A mutations preserve exact canonical serialized bytes");
  PeSemanticSegmentedEmission segmented{};
  check(owner.emitSegmented(segmented) &&
            ownerSegmentedRolesEqualExact(exact, segmented),
        "ExactFixed and segmented role views are byte-equivalent");

  const auto& header = exact.transport.header;
  const auto& producerIdentity = exact.transport.producerIdentity;
  D9CCommandChunkWireHandleEntry handle{};
  std::memcpy(&handle, exact.wire.data() + header.handleTableOffset,
              sizeof(handle));
  check(header.recordCount == 3u && header.handleCount == 1u &&
            handle.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
            handle.generation == 7u && handle.objectId == 0xc101u,
        "serialized handle identity is distinct from source and record ordinals");
  check(producerIdentity.firstEventOrdinal == 1u &&
            producerIdentity.lastEventOrdinal == 1u &&
            producerIdentity.firstSourceOrdinal == first.sourceOrdinal &&
            producerIdentity.lastSourceOrdinal == third.sourceOrdinal,
        "transport producer identity preserves the accepted source interval");
  check(owner.record(0u).sourceOrdinal == first.sourceOrdinal &&
            owner.record(0u).recordOrdinal == first.recordOrdinal &&
            owner.record(1u).sourceOrdinal == second.destination.sourceOrdinal &&
            owner.record(1u).recordOrdinal == second.destination.recordOrdinal &&
            owner.record(2u).sourceOrdinal == third.sourceOrdinal &&
            owner.record(2u).recordOrdinal == third.recordOrdinal,
        "owner records preserve source and record ordinals through direct admission");
  check(pending.acceptedCount() == 3u && pending.pendingCount() == 3u &&
            pending.pending(0u).sourceOrdinal == first.sourceOrdinal &&
            pending.pending(0u).recordOrdinal == first.recordOrdinal &&
            pending.pending(1u).sourceOrdinal == second.destination.sourceOrdinal &&
            pending.pending(1u).recordOrdinal == second.destination.recordOrdinal &&
            pending.pending(2u).sourceOrdinal == third.sourceOrdinal &&
            pending.pending(2u).recordOrdinal == third.recordOrdinal,
        "PendingDelta-style semantic projection accepts each A/B/A record once");
  check(owner.retainedCount() == 1u && bindingTexture.refs == 3u &&
            replacementTexture.refs == 1u,
        "accepted binding retains once per owner while the oracle holds its own retain");
  owner.reset();
  check(pending.settleCapture(PeSemanticCaptureDisposition::Skipped) &&
            pending.pendingCount() == 0u && pending.settledCount() == 3u,
        "accepted A/B/A PendingDelta witnesses settle exactly once");
  canonical.resetAndReleaseRetained();
  check(bindingTexture.refs == 1u && replacementTexture.refs == 1u,
        "direct mutation refinement releases the retained multiplicity at reset");
}

void directCanonicalQuotaAndSettlementRefinement() {
  {
    using Tiny = PeSemanticBatchOwner<1u, 8u, 128u, 4u, 4u>;
    Tiny owner;
    D9CSurface surface;
    auto exact = base(PeSemanticProducerKind::Present, 1u, 1u);
    exact.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface,
                                                              0xd001u, 1u);
    PeSemanticPreparedRecord witness{};
    check(owner.prepareAdmission(exact, witness) ==
              PeSemanticAdmissionOutcome::Admissible,
          "record quota exact fit prepares");
    check(owner.admit(exact), "record quota exact fit admits");
    auto plusOne = exact;
    plusOne.sourceOrdinal = 2u;
    plusOne.recordOrdinal = 2u;
    check(!owner.admit(plusOne) &&
              owner.lastAdmissionFailure() == Tiny::AdmissionFailure::Capacity &&
              owner.size() == 1u,
          "record quota plus-one rejects without disturbing the exact fit");
    owner.reset();
  }
  {
    using Tiny = PeSemanticBatchOwner<2u, 8u, 1u, 4u, 4u>;
    Tiny owner;
    std::array<std::byte, 1u> one{{std::byte{0x51}}};
    auto exact = base(PeSemanticProducerKind::DrawPrimitiveUp, 1u, 1u);
    exact.sparse.upVertexData = one;
    check(owner.admit(exact), "semantic-byte quota exact fit admits");
    auto plusOne = exact;
    std::array<std::byte, 1u> another{{std::byte{0x52}}};
    plusOne.sourceOrdinal = 2u;
    plusOne.recordOrdinal = 2u;
    plusOne.sparse.upVertexData = another;
    check(!owner.admit(plusOne) &&
              owner.lastAdmissionFailure() == Tiny::AdmissionFailure::Capacity &&
              owner.size() == 1u,
          "semantic-byte quota plus-one rejects before owner mutation");
    owner.reset();
  }
  {
    using Tiny = PeSemanticBatchOwner<2u, 8u, 128u, 1u, 4u>;
    Tiny owner;
    std::array<D9CRect, 1u> one{};
    auto exact = base(PeSemanticProducerKind::Clear, 1u, 1u);
    exact.clear.rectCount = 1u;
    exact.clearRects = one;
    check(owner.admit(exact), "rectangle quota exact fit admits");
    auto plusOne = exact;
    std::array<D9CRect, 1u> another{{{9, 10, 19, 20}}};
    plusOne.sourceOrdinal = 2u;
    plusOne.recordOrdinal = 2u;
    plusOne.clear.rectCount = 1u;
    plusOne.clearRects = another;
    check(!owner.admit(plusOne) &&
              owner.lastAdmissionFailure() == Tiny::AdmissionFailure::Capacity &&
              owner.size() == 1u,
          "rectangle quota plus-one rejects before owner mutation");
    owner.reset();
  }
  {
    using Tiny = PeSemanticBatchOwner<2u, 8u, 128u, 4u, 1u>;
    Tiny owner;
    std::array<D9CCommandChunkWireRenderState, 1u> one{{{1u, 2u}}};
    auto exact = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
    exact.sparse.renderStates = one;
    check(owner.admit(exact), "scalar sparse quota exact fit admits");
    auto plusOne = exact;
    std::array<D9CCommandChunkWireRenderState, 1u> another{{{3u, 4u}}};
    plusOne.sourceOrdinal = 2u;
    plusOne.recordOrdinal = 2u;
    plusOne.sparse.renderStates = another;
    check(!owner.admit(plusOne) &&
              owner.lastAdmissionFailure() == Tiny::AdmissionFailure::Capacity &&
              owner.size() == 1u,
          "scalar sparse quota plus-one rejects before owner mutation");
    owner.reset();
  }
  {
    using Tiny = PeSemanticBatchOwner<2u, 1u, 128u, 4u, 4u>;
    Tiny owner;
    D9CSurface firstSurface;
    D9CSurface secondSurface;
    auto exact = base(PeSemanticProducerKind::Present, 1u, 1u);
    exact.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
        &firstSurface, 0xd101u, 1u);
    check(owner.admit(exact), "pin quota exact fit admits");
    auto plusOne = exact;
    plusOne.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
        &secondSurface, 0xd102u, 1u);
    plusOne.sourceOrdinal = 2u;
    plusOne.recordOrdinal = 2u;
    check(!owner.admit(plusOne) &&
              owner.lastAdmissionFailure() == Tiny::AdmissionFailure::Capacity &&
              owner.size() == 1u && secondSurface.refs == 1u,
          "pin quota plus-one rejects without retaining a novel object");
    owner.reset();
    check(firstSurface.refs == 1u && secondSurface.refs == 1u,
          "pin quota exact/plus-one refinement balances retention");
  }

  using PrefixOwner = PeSemanticBatchOwner<4u, 4u, 4096u, 4u, 4u>;
  PrefixOwner owner;
  CommandChunkBuilder canonical;
  D9CSurface prefixSurface;
  D9CTexture retryTexture;
  auto prefix = base(PeSemanticProducerKind::Present, 11u, 111u);
  prefix.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &prefixSurface, 0xd201u, 1u);
  std::array<D9CCommandChunkWireRenderState, 1u> retryRows{{{12u, 112u}}};
  std::array<std::byte, 4u> retryConstants{
      std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}};
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
      retryTextures{{{
          .wire = {.slot = 0u, .valid = 1u},
          .object = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&retryTexture,
                                                             0xd202u, 1u),
      }}};
  auto retry = base(PeSemanticProducerKind::DrawPrimitive, 12u, 112u);
  retry.sparse.renderStates = retryRows;
  retry.sparse.vsBoolConstants = {
      .startRegister = 5u, .registerCount = 1u, .registerBytes = retryConstants};
  retry.sparse.textures = retryTextures;
  check(appendCanonicalSemanticRecord(canonical, prefix) && owner.admit(prefix),
        "settlement prefix admits in owner and canonical oracle");
  PeSemanticExactFixedEmission beforeFailure{};
  check(owner.emitExactFixed(beforeFailure), "settlement prefix emits before retry");
  std::array<std::byte, 4096u> prefixBytes{};
  std::copy(beforeFailure.wire.begin(), beforeFailure.wire.end(),
            prefixBytes.begin());
  std::uint32_t settlementCalls = 0u;
  check(!owner.tryAppendOwnedRecord(retry, [&]() noexcept {
          ++settlementCalls;
          return false;
        }) &&
            settlementCalls == 1u && owner.size() == 1u &&
            owner.record(0u).sourceOrdinal == prefix.sourceOrdinal &&
            owner.record(0u).recordOrdinal == prefix.recordOrdinal &&
            owner.retainedCount() == 1u && retryTexture.refs == 1u,
        "failed settlement rolls back while preserving the accepted prefix");
  PeSemanticExactFixedEmission afterFailure{};
  check(owner.emitExactFixed(afterFailure) &&
            afterFailure.wireBytes == beforeFailure.wireBytes &&
            afterFailure.wireBytes <= prefixBytes.size() &&
            std::equal(prefixBytes.begin(), prefixBytes.begin() + afterFailure.wireBytes,
                       afterFailure.wire.begin()),
        "failed settlement preserves serialized prefix identity");
  retryRows[0] = {.state = 13u, .value = 113u};
  retryConstants[0] = std::byte{0x7f};
  retryTextures[0].wire.slot = 3u;
  check(retryTexture.refs == 1u,
        "rejected mixed retry leaves its source pin available for mutation");
  check(appendCanonicalSemanticRecord(canonical, retry) &&
            owner.tryAppendOwnedRecord(retry, []() noexcept { return true; }),
        "settlement retry accepts the mutated mixed Draw/APPLY_STATE record");
  PeSemanticExactFixedEmission final{};
  check(equalCanonicalOwnerBytes(owner, canonical, final),
        "settlement retry preserves exact canonical prefix and suffix bytes");
  check(owner.record(0u).sourceOrdinal == prefix.sourceOrdinal &&
            owner.record(1u).sourceOrdinal == retry.sourceOrdinal &&
            owner.retainedCount() == 2u && retryTexture.refs == 3u,
        "settlement retry retains once in the owner and once in the oracle");
  owner.reset();
  canonical.resetAndReleaseRetained();
  check(prefixSurface.refs == 1u && retryTexture.refs == 1u,
        "failed settlement and retry leave the typed pin ledger balanced");
}

void directCanonicalLedgerExcludesDuplicateStaging() {
  dxmt9::core::CopyMaterializationLedger ledger;
  dxmt9::core::ScopedCopyMaterializationLedger observe(
      dxmt9::core::CopyMaterializationOwner::Pe, ledger);
  Owner owner;
  D9CTexture texture;
  std::array<D9CCommandChunkWireRenderState, 1u> renderStates{{{9u, 10u}}};
  std::array<std::byte, 4u> embeddedBytes{
      std::byte{0x61}, std::byte{0x62}, std::byte{0x63}, std::byte{0x64}};
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
      textures{{{
          .wire = {.slot = 0u, .valid = 1u},
          .object = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(&texture,
                                                             0xe101u, 2u),
      }}};
  auto input = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
  input.sparse.renderStates = renderStates;
  input.sparse.textures = textures;
  input.sparse.vsBoolConstants = {
      .startRegister = 2u, .registerCount = 1u, .registerBytes = embeddedBytes};
  check(owner.admit(input), "ledger refinement admits one mixed direct record");
  PeSemanticSegmentedEmission segmented{};
  check(owner.emitSegmented(segmented) &&
            segmented.transport.header.recordCount == 1u &&
            segmented.transport.header.handleCount == 1u,
        "ledger refinement derives canonical roles from segmented emission");
  const auto admitted = ledger.snapshot(
      dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission);
  const auto expectedAdmissionBytes =
      sizeof(PeSemanticRecordReceipt) +
      sizeof(PeSemanticPhysicalPin<D9CTexture, PeSemanticTextureIdentity>) +
      static_cast<std::size_t>(segmented.transport.header.recordCount) *
          sizeof(D9CCommandChunkWireRecordHeader) +
      static_cast<std::size_t>(segmented.transport.header.handleCount) *
          sizeof(D9CCommandChunkWireHandleEntry) +
      segmented.transport.header.payloadArenaSize;
  check(admitted.calls == 1u && admitted.bytes == expectedAdmissionBytes &&
            admitted.retainedBytes == admitted.bytes &&
            admitted.retainedBytesPeak == admitted.bytes,
        "direct admission ledger excludes scalar, constant, and binding duplicate staging");

  constexpr auto kNoDuplicateStaging = std::array{
      dxmt9::core::CopyMaterializationClass::PeBuilderTemporary,
      dxmt9::core::CopyMaterializationClass::PeSealRecords,
      dxmt9::core::CopyMaterializationClass::PeSealHandles,
      dxmt9::core::CopyMaterializationClass::PeSealPayload,
      dxmt9::core::CopyMaterializationClass::PeSectionAppend,
      dxmt9::core::CopyMaterializationClass::UpScratch,
      dxmt9::core::CopyMaterializationClass::ArenaByteCopy,
  };
  for (const auto materializationClass : kNoDuplicateStaging) {
    const auto snapshot = ledger.snapshot(materializationClass);
    check(snapshot.calls == 0u && snapshot.bytes == 0u &&
              snapshot.semanticCalls == 0u && snapshot.retainedBytes == 0u,
          "eliminated duplicate staging remains absent from physical ledger");
  }

  PeSemanticExactFixedEmission exact{};
  check(owner.emitExactFixed(exact),
        "direct ledger refinement emits its canonical final wire");
  const auto finalWire = ledger.snapshot(
      dxmt9::core::CopyMaterializationClass::PeWireFinal);
  check(finalWire.calls == 1u && finalWire.bytes == exact.wireBytes &&
            finalWire.retainedBytes == exact.wireBytes,
        "final wire is the sole additional physical emission ledger row");
  const auto repeated = input;
  auto second = repeated;
  second.sourceOrdinal = 2u;
  second.recordOrdinal = 2u;
  check(owner.admit(second) && owner.retainedCount() == 1u &&
            texture.refs == 2u,
        "repeated direct identity retains once while records remain distinct");
  const auto admittedTwice = ledger.snapshot(
      dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission);
  check(admittedTwice.calls == 2u && admittedTwice.retainedBytes != 0u,
        "repeated admission records physical owner growth without pin duplication");
  owner.reset();
  check(texture.refs == 1u &&
            ledger.snapshot(
                dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission)
                    .retainedBytes == 0u &&
            ledger.snapshot(dxmt9::core::CopyMaterializationClass::PeWireFinal)
                    .retainedBytes == 0u,
        "ledger refinement reset releases the single retained object");
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
  CommandChunkBuilder constantCanonical;
  check(appendCanonicalSemanticRecord(constantCanonical, constant) &&
            owner.admit(constant), "constant admission copies borrowed bytes");
  copiedBytes[0] = std::byte{0x7f};
  PeSemanticExactFixedEmission constantExact{};
  check(equalCanonicalOwnerBytes(owner, constantCanonical, constantExact),
        "constant arena isolates borrowed input mutation in canonical payload");
  owner.reset();
  constantCanonical.resetAndReleaseRetained();

  std::array<std::byte, 2u> up{std::byte{0x31}, std::byte{0x32}};
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u> sparseTexture{};
  sparseTexture[0].wire.valid = 1u;
  sparseTexture[0].wire.slot = 0u;
  sparseTexture[0].object = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &partialSource, 20u);
  auto upRecord = base(PeSemanticProducerKind::DrawPrimitiveUp, 9u, 9u);
  upRecord.sparse.upVertexData = up;
  upRecord.sparse.textures = sparseTexture;
  CommandChunkBuilder upCanonical;
  check(appendCanonicalSemanticRecord(upCanonical, upRecord) &&
            owner.admit(upRecord), "UP and sparse typed arenas admit");
  up[0] = std::byte{0x7e};
  sparseTexture[0].wire.slot = 4u;
  PeSemanticExactFixedEmission upExact{};
  check(equalCanonicalOwnerBytes(owner, upCanonical, upExact) &&
            upExact.transport.header.handleCount == 1u &&
            upExact.transport.header.payloadArenaSize >= up.size(),
        "UP and sparse inputs preserve owned canonical payload and binding section");
  owner.reset();
  upCanonical.resetAndReleaseRetained();
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
  check(ownerUpVertexPrefix(owner, up) &&
            partialSource.refs == 2u,
        "retry reacquires a released sparse pin with canonical UP bytes");
  const std::array<std::byte, 3u> overflowingUp{};
  auto failedUp = upRecord;
  failedUp.sourceOrdinal = 10u;
  failedUp.recordOrdinal = 10u;
  failedUp.sparse.upVertexData = overflowingUp;
  check(!owner.admit(failedUp) && owner.size() == 1u &&
            ownerUpVertexPrefix(owner, up),
        "UP arena overflow rolls back the attempted admission");
  owner.reset();
  check(partialSource.refs == 1u, "failed UP admission leaves no leaked pin");
  auto finalRetry = upRecord;
  finalRetry.sourceOrdinal = 1u;
  finalRetry.recordOrdinal = 1u;
  check(owner.admit(finalRetry) && ownerUpVertexPrefix(owner, up) &&
            partialSource.refs == 2u,
        "reset/retry restores canonical UP prefix and reacquires the pin");
  owner.reset();
}

// R-CORE-REC-7.22: index generations revoke a whole inactive pin table in
// constant work. Keep this trace deliberately bounded and compare each
// transition with the reference rule (a reset means no active pin; a retry
// of a warm object owns exactly one retain). Collision and failed settlement
// must not make stale buckets authoritative.
void pinIndexGenerationReferenceTrace() {
  using Tiny = PeSemanticBatchOwner<8u, 2u, 4096u, 4u, 4u>;
  Tiny owner;
  D9CBuffer first;
  D9CBuffer second;
  const auto firstRef = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(
      &first, 0xd01u, 3u);
  const auto secondRef = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(
      &second, 0xd02u, 4u);

  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u>
      firstRows{{{.wire = {.slot = 0u, .valid = 1u}, .object = firstRef}}};
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u>
      secondRows{{{.wire = {.slot = 0u, .valid = 1u}, .object = secondRef}}};
  auto firstInput = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
  firstInput.sparse.streams = firstRows;
  check(owner.admit(firstInput) && owner.referencesBuffer(firstRef) &&
            first.refs == 2u,
        "generation trace admits the reference model's first live pin");
  check(owner.settle() && owner.clearedBytesLastBoundary() == 0u &&
            !owner.referencesBuffer(firstRef),
        "normal boundary revokes stale pin indexes without a full clear");

  // The retainer keeps the physical pin warm, but the owner index must still
  // reject the old chunk's slot before a new record publishes it.
  firstInput.sourceOrdinal = firstInput.recordOrdinal = 1u;
  check(owner.admit(firstInput) && owner.referencesBuffer(firstRef) &&
            first.refs == 2u && owner.retainedCount() == 1u,
        "reset/reuse requalifies a warm object without another AddRef");

  auto failed = base(PeSemanticProducerKind::DrawPrimitive, 2u, 2u);
  failed.sparse.streams = secondRows;
  check(!owner.tryAppendOwnedRecord(failed, []() noexcept { return false; }) &&
            owner.referencesBuffer(firstRef) &&
            !owner.referencesBuffer(secondRef) && second.refs == 1u,
        "rollback revokes a partially acquired collision-free suffix");
  failed.sourceOrdinal = failed.recordOrdinal = 2u;
  check(owner.tryAppendOwnedRecord(failed, []() noexcept { return true; }) &&
            owner.referencesBuffer(secondRef) && second.refs == 2u,
        "post-rollback retry publishes only the fresh qualified pin");

  auto generationCollision =
      base(PeSemanticProducerKind::DrawPrimitive, 3u, 3u);
  firstRows[0].object.identity.generation = 9u;
  generationCollision.sparse.streams = firstRows;
  check(!owner.admit(generationCollision) && owner.size() == 2u &&
            first.refs == 2u,
        "generation collision remains rejected after index reuse");
  owner.reset();
  check(owner.clearedBytesLastBoundary() == 0u && first.refs == 1u &&
            second.refs == 1u && owner.retainedCount() == 0u,
        "reset releases physical pins while leaving no stale index authority");

  // Churn the same object through many reset/reuse cycles. The old bucket is
  // a logical empty after each reset, while the exact and object-id probes
  // still reject generation and pointer aliases once the new pin is live.
  D9CSurface alias;
  for (std::uint32_t cycle = 0u; cycle < 32u; ++cycle) {
    firstRows[0].object = firstRef;
    auto churn = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
    churn.sparse.streams = firstRows;
    check(owner.admit(churn) && first.refs == 2u,
          "reset churn admits the reused identity");
    owner.reset();
    check(owner.clearedBytesLastBoundary() == 0u && first.refs == 1u,
          "reset churn invalidates old buckets without a table clear");
    check(owner.admit(churn), "reset churn re-admits after stale invalidation");
    auto staleGeneration = churn;
    staleGeneration.sourceOrdinal = staleGeneration.recordOrdinal = 2u;
    auto staleStream = churn.sparse.streams[0];
    staleStream.object.identity.generation = 8u;
    staleGeneration.sparse.streams = std::span(&staleStream, 1u);
    auto pointerAlias = churn;
    pointerAlias.sourceOrdinal = pointerAlias.recordOrdinal = 3u;
    auto aliasStream = churn.sparse.streams[0];
    aliasStream.object.object = &alias;
    pointerAlias.sparse.streams = std::span(&aliasStream, 1u);
    check(!owner.admit(staleGeneration) && !owner.admit(pointerAlias) &&
              first.refs == 2u && alias.refs == 1u,
          "reset churn preserves exact and object collision rejection");
    owner.reset();
    if (cycle == 31u)
      check(!owner.visitCommittedPendingChunkLease(
                 PeWireObjectRef{.identity = firstRef.identity,
                                 .object = &first},
                 [](const CommittedPendingChunkLease&) noexcept { return true; }),
            "final churn reset exposes no stale committed pin");
  }

  using WrapOwner = PeSemanticBatchOwner<4u, 2u, 4096u, 4u, 4u>;
  WrapOwner::PinIndexGenerationForTesting nearWrap{
      std::numeric_limits<std::uint32_t>::max()};
  WrapOwner wrapOwner(nearWrap);
  D9CSurface surface;
  auto present = base(PeSemanticProducerKind::Present, 1u, 1u);
  present.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0xd03u);
  check(wrapOwner.admit(present) && wrapOwner.settle() &&
            wrapOwner.clearedBytesLastBoundary() != 0u &&
            !wrapOwner.visitCommittedPendingChunkLease(
                PeWireObjectRef{.identity = present.surface0.identity,
                                .object = &surface},
                [](const CommittedPendingChunkLease&) noexcept { return true; }),
        "generation wrap takes the bounded physical-clear slow path");
  check(wrapOwner.admit(present) && surface.refs == 2u &&
            wrapOwner.retainedCount() == 1u,
        "generation wrap resets the index domain before warm retry");
  wrapOwner.reset();
  check(surface.refs == 1u,
        "generation-wrap retry keeps release multiplicity exact");
}

// R-CORE-REC-7.12/7.13 bounded refinement: keep these sequences deliberately
// small, but make every accepted/rejected transition observable through the
// public owner surface. In particular, a failed sparse pin after an earlier
// pin and a failed settlement after an accepted prefix must not leave either
// an ownership or an index bucket authoritative.
void r712R713BoundedIdentityAndLifecycleRefinement() {
  {
    Owner owner;
    D9CTexture texture;
    D9CSurface surface;
    const auto textureRef = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
        &texture, 0xc01u, 4u);
    const auto surfaceRef = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
        &surface, 0xc02u, 5u);
    std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 2u>
        textures{{
            {.wire = {.slot = 0u, .valid = 1u}, .object = textureRef},
            {.wire = {.slot = 1u, .valid = 1u}, .object = textureRef},
        }};
    std::array<SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>, 1u>
        targets{{{.wire = {.slot = 0u, .valid = 1u}, .object = surfaceRef}}};
    auto input = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
    input.texture0 = textureRef;  // retained direct, then promoted by sparse.
    input.surface0 = surfaceRef;  // retained direct, then promoted by sparse.
    input.sparse.textures = textures;
    input.sparse.renderTargets = targets;
    CommandChunkBuilder canonical;
    check(owner.admit(input) && owner.retainedCount() == 2u &&
              texture.refs == 2u && surface.refs == 2u,
          "direct and duplicate sparse handles retain each identity once");
    check(appendCanonicalSemanticRecord(canonical, input),
          "duplicate direct and sparse handles enter the canonical oracle");
    PeSemanticExactFixedEmission emission{};
    check(equalCanonicalOwnerBytes(owner, canonical, emission) &&
              emission.transport.header.handleCount == 2u,
          "duplicate direct and sparse handles emit two first-seen identities");
    D9CCommandChunkWireHandleEntry handles[2u]{};
    std::memcpy(handles,
                emission.wire.data() + emission.transport.header.handleTableOffset,
                sizeof(handles));
    check(handles[0].kind == textureRef.identity.kind &&
              handles[0].generation == textureRef.identity.generation &&
              handles[0].objectId == textureRef.identity.objectId &&
              handles[1].kind == surfaceRef.identity.kind &&
              handles[1].generation == surfaceRef.identity.generation &&
              handles[1].objectId == surfaceRef.identity.objectId,
          "retained-only direct identities become wire-visible in sparse order");
    owner.reset();
    canonical.resetAndReleaseRetained();
    check(texture.refs == 1u && surface.refs == 1u,
          "duplicate direct and sparse reset balances both typed retains");
  }

  {
    Owner owner;
    std::array<SparseBindingInput<D9CCommandChunkWireShaderBinding>, 2u>
        nullShaders{{
            {.wire = {.stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
                      .valid = 1u,
                      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}},
            {.wire = {.stage = D9C_COMMAND_CHUNK_SHADER_STAGE_PIXEL,
                      .valid = 1u,
                      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX}},
        }};
    auto input = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
    input.sparse.shaders = nullShaders;
    CommandChunkBuilder canonical;
    check(owner.admit(input) && owner.retainedCount() == 0u &&
              appendCanonicalSemanticRecord(canonical, input),
          "null sparse bindings represent unbinding without a physical pin");
    PeSemanticExactFixedEmission emission{};
    check(equalCanonicalOwnerBytes(owner, canonical, emission) &&
              emission.transport.header.handleCount == 0u,
          "null unbinding preserves canonical bytes without handles");
    owner.reset();
    canonical.resetAndReleaseRetained();
  }

  {
    using Tiny = PeSemanticBatchOwner<4u, 2u, 4096u, 4u, 4u>;
    Tiny owner;
    D9CSurface first;
    D9CSurface alias;
    auto accepted = base(PeSemanticProducerKind::Present, 1u, 1u);
    accepted.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
        &first, 0xc11u, 7u);
    CommandChunkBuilder canonical;
    check(owner.admit(accepted) && appendCanonicalSemanticRecord(canonical, accepted),
          "identity collision fixture admits its canonical prefix");
    PeSemanticExactFixedEmission before{};
    check(equalCanonicalOwnerBytes(owner, canonical, before),
          "identity collision fixture captures the accepted prefix bytes");
    const std::vector<std::byte> beforeBytes(before.wire.begin(), before.wire.end());
    auto wrongKind = accepted;
    wrongKind.sourceOrdinal = wrongKind.recordOrdinal = 2u;
    wrongKind.surface0.identity.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
    auto staleGeneration = accepted;
    staleGeneration.sourceOrdinal = staleGeneration.recordOrdinal = 2u;
    staleGeneration.surface0.identity.generation = 8u;
    auto pointerAlias = accepted;
    pointerAlias.sourceOrdinal = pointerAlias.recordOrdinal = 2u;
    pointerAlias.surface0.object = &alias;
    check(!owner.admit(wrongKind) && !owner.admit(staleGeneration) &&
              !owner.admit(pointerAlias) && owner.size() == 1u &&
              first.refs == 3u && alias.refs == 1u,
          "wrong kind generation and same-id pointer aliases reject atomically");
    PeSemanticExactFixedEmission after{};
    check(equalCanonicalOwnerBytes(owner, canonical, after) &&
              after.wireBytes == beforeBytes.size() &&
              std::equal(beforeBytes.begin(), beforeBytes.end(), after.wire.begin()),
          "rejected identity aliases preserve accepted bytes and ownership");
    owner.reset();
    canonical.resetAndReleaseRetained();
    check(first.refs == 1u && alias.refs == 1u,
          "identity alias rejection releases no speculative retain");
  }

  {
    using PinBound = PeSemanticBatchOwner<4u, 1u, 4096u, 4u, 4u>;
    PinBound owner;
    D9CSurface pinned;
    D9CSurface novel;
    auto first = base(PeSemanticProducerKind::Present, 1u, 1u);
    first.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&pinned, 0xc21u);
    auto duplicate = first;
    duplicate.sourceOrdinal = duplicate.recordOrdinal = 2u;
    auto newIdentity = first;
    newIdentity.sourceOrdinal = newIdentity.recordOrdinal = 3u;
    newIdentity.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&novel, 0xc22u);
    check(owner.admit(first) && owner.admit(duplicate) && owner.size() == 2u &&
              owner.retainedCount() == 1u && pinned.refs == 2u,
          "full pin capacity still accepts an existing duplicate identity");
    check(!owner.admit(newIdentity) && owner.size() == 2u && novel.refs == 1u &&
              owner.lastAdmissionFailure() == PinBound::AdmissionFailure::Capacity,
          "full pin capacity rejects a new identity before retain");
    owner.reset();
    check(pinned.refs == 1u && novel.refs == 1u,
          "full-capacity duplicate/new distinction leaves a balanced ledger");
  }

  {
    Owner owner;
    D9CTexture first;
    D9CVertexDecl invalid;
    const auto firstRef = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
        &first, 0xc31u, 1u);
    const auto invalidRef = localRef<D9C_CHUNK_HANDLE_KIND_VERTEX_DECL>(
        &invalid, 0xc32u, 1u);
    std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
        textures{{{.wire = {.slot = 0u, .valid = 1u}, .object = firstRef}}};
    std::array<SparseBindingInput<D9CCommandChunkWireVertexInput>, 1u> inputs{{
        {.wire = {.valid = 1u, .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF},
         .object = invalidRef},
    }};
    auto input = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
    input.sparse.textures = textures;
    input.sparse.vertexInputs = inputs;
    check(!owner.admit(input) && owner.size() == 0u &&
              owner.lastAdmissionFailure() == Owner::AdmissionFailure::SparseVertexInputs &&
              owner.retainedCount() == 0u && first.refs == 1u && invalid.refs == 1u,
          "partial sparse pin acquisition rolls back every speculative retain");
    inputs[0].object = {};
    CommandChunkBuilder canonical;
    check(owner.admit(input) && appendCanonicalSemanticRecord(canonical, input),
          "partial sparse pin fixture retries after removing the invalid object");
    PeSemanticExactFixedEmission emission{};
    check(equalCanonicalOwnerBytes(owner, canonical, emission) &&
              emission.transport.header.handleCount == 1u && first.refs == 3u,
          "partial sparse pin retry matches canonical bytes and one texture pin");
    owner.reset();
    canonical.resetAndReleaseRetained();
    check(first.refs == 1u && invalid.refs == 1u,
          "partial sparse pin rollback and retry release exactly once");
  }

  {
    using IndexOwner = PeSemanticBatchOwner<4u, 8u, 4096u, 4u, 4u>;
    IndexOwner owner;
    D9CBuffer acceptedBuffer;
    D9CBuffer retryBuffer;
    D9CBuffer probeBuffer;
    const auto acceptedRef = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(
        &acceptedBuffer, 0xc41u, 3u);
    // This bounded owner has a 33-entry index. These IDs deliberately hash the
    // retry identity into the accepted bucket and the probe into the next one.
    const auto retryRef = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(
        &retryBuffer, 0xc2au, 4u);
    const auto probeRef = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(
        &probeBuffer, 0xc15u, 5u);
    std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u>
        acceptedStreams{{{.wire = {.slot = 0u, .valid = 1u},
                          .object = acceptedRef}}};
    std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u>
        retryStreams{{{.wire = {.slot = 1u, .valid = 1u},
                       .object = retryRef}}};
    std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>, 1u>
        probeStreams{{{.wire = {.slot = 2u, .valid = 1u},
                       .object = probeRef}}};
    auto accepted = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
    accepted.sparse.streams = acceptedStreams;
    auto retry = base(PeSemanticProducerKind::DrawPrimitive, 2u, 2u);
    retry.sparse.streams = retryStreams;
    auto probe = base(PeSemanticProducerKind::DrawPrimitive, 2u, 2u);
    probe.sparse.streams = probeStreams;
    check(owner.admit(accepted), "settlement retry fixture admits its prefix");
    check(!owner.tryAppendOwnedRecord(retry, []() noexcept { return false; }) &&
              owner.size() == 1u && owner.retainedCount() == 1u &&
              acceptedBuffer.refs == 2u && retryBuffer.refs == 1u &&
              owner.referencesBuffer(acceptedRef) &&
              !owner.referencesBuffer(retryRef),
          "failed settlement removes the speculative pin and stale index authority");
    bool uncommittedLease = false;
    check(!owner.visitCommittedPendingChunkLease(
              retryRef, [&](const CommittedPendingChunkLease&) noexcept {
                uncommittedLease = true;
                return true;
              }) && !uncommittedLease,
          "failed settlement does not expose an uncommitted wire handle");
    check(owner.tryAppendOwnedRecord(probe, []() noexcept { return true; }) &&
              owner.referencesBuffer(acceptedRef) && owner.referencesBuffer(probeRef) &&
              owner.retainedCount() == 2u && probeBuffer.refs == 2u,
          "rollback clears stale buckets before accepting a new probe identity");
    retry.sourceOrdinal = retry.recordOrdinal = 3u;
    check(owner.tryAppendOwnedRecord(retry, []() noexcept { return true; }) &&
              owner.referencesBuffer(acceptedRef) && owner.referencesBuffer(retryRef) &&
              owner.retainedCount() == 3u && retryBuffer.refs == 2u,
          "settlement retry reacquires the removed identity exactly once");
    CommandChunkBuilder canonical;
    check(appendCanonicalSemanticRecord(canonical, accepted) &&
              appendCanonicalSemanticRecord(canonical, probe) &&
              appendCanonicalSemanticRecord(canonical, retry),
          "settlement retry fixture enters all accepted records in the oracle");
    PeSemanticExactFixedEmission emission{};
    check(equalCanonicalOwnerBytes(owner, canonical, emission),
          "failed settlement retry preserves exact accepted-prefix and suffix bytes");
    owner.reset();
    canonical.resetAndReleaseRetained();
    check(acceptedBuffer.refs == 1u && probeBuffer.refs == 1u &&
              retryBuffer.refs == 1u,
          "failed settlement retry leaves all buffer retains balanced");
  }

  {
    Owner owner;
    D9CTexture texture;
    D9CViewport viewport{3u, 5u, 127u, 91u, 0.25f, 0.75f};
    D9CCommandChunkWireRenderState renderState{7u, 0x1111u};
    std::array<D9CCommandChunkWireRenderState, 1u> renderStates{{renderState}};
    std::array<D9CViewport, 1u> viewports{{viewport}};
    std::array<std::byte, 4u> constantBytes{{
        std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}}};
    const auto textureRef = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
        &texture, 0xc51u, 1u);
    std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>, 1u>
        familyTextures{{{.wire = {.slot = 0u, .valid = 1u},
                         .object = textureRef}}};
    auto familyA = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
    familyA.sparse.renderStates = renderStates;
    familyA.sparse.textures = familyTextures;
    auto familyB = base(PeSemanticProducerKind::VsBoolConstant, 2u, 2u);
    familyB.setConst.registerCount = 1u;
    familyB.constantBytes = constantBytes;
    auto familyA2 = base(PeSemanticProducerKind::DrawPrimitive, 3u, 3u);
    familyA2.sparse.viewports = viewports;
    CommandChunkBuilder canonical;
    check(appendCanonicalSemanticRecord(canonical, familyA) && owner.admit(familyA),
          "family A first record admits and copies canonical values");
    renderStates[0].value = 0xeeeeu;
    check(appendCanonicalSemanticRecord(canonical, familyB) && owner.admit(familyB),
          "family B record admits between family A records");
    constantBytes.fill(std::byte{0xee});
    check(appendCanonicalSemanticRecord(canonical, familyA2) && owner.admit(familyA2),
          "family A second record admits with a different active category");
    viewports[0].width = 999u;
    PeSemanticExactFixedEmission emission{};
    PeSemanticSegmentedEmission segmented{};
    check(equalCanonicalOwnerBytes(owner, canonical, emission) &&
              owner.emitSegmented(segmented) && ownerSegmentedRolesEqualExact(emission, segmented) &&
              owner.retainedCount() == 1u && texture.refs == 3u,
          "bounded family A/B/A sequence preserves bytes and one warmable pin");
    check(owner.settle() && owner.retainedCount() == 1u && texture.refs == 3u,
          "settlement clears the active family prefix while preserving warm retention");
    canonical.resetAndReleaseRetained();
    auto reused = base(PeSemanticProducerKind::Present, 4u, 4u);
    D9CSurface surface;
    reused.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0xc52u, 2u);
    check(owner.admit(reused), "settled owner accepts a new family after reset of active ranges");
    CommandChunkBuilder reusedCanonical;
    check(appendCanonicalSemanticRecord(reusedCanonical, reused),
          "post-settle reuse enters the fresh family in the oracle");
    PeSemanticExactFixedEmission reusedEmission{};
    check(equalCanonicalOwnerBytes(owner, reusedCanonical, reusedEmission),
          "post-settle reuse has no authority from prior A/B/A family ranges");
    owner.reset();
    reusedCanonical.resetAndReleaseRetained();
    check(texture.refs == 1u && surface.refs == 1u,
          "settle then reset releases warm and new family pins exactly once");
  }
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
  check(owner.settle() && owner.clearedBytesLastBoundary() == 0u,
        "settlement invalidates the source frontier without capacity clears");

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

void calibratedOwnerPhaseObserver() {
  using ObservedOwner = PeSemanticBatchOwner<4u, 2u, 4096u, 4u, 4u>;
  dxmt9::core::CopyMaterializationLedger copyLedger;
  dxmt9::core::ScopedCopyMaterializationLedger copyScope(
      dxmt9::core::CopyMaterializationOwner::Pe, copyLedger);
  PeSemanticOwnerPhaseObserver observer(2u);
  ObservedOwner owner;
  owner.setPhaseObserver(&observer);
  D9CSurface surface;
  auto input = base(PeSemanticProducerKind::Present, 0x701u, 0x702u);
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x703u);
  check(owner.tryAppendOwnedRecord(input, []() noexcept { return true; }),
        "observer fixture records accepted admission");
  auto rollbackInput = input;
  rollbackInput.sourceOrdinal = 0x704u;
  rollbackInput.recordOrdinal = 0x705u;
  check(owner.tryAppendOwnedRecord(rollbackInput, []() noexcept { return false; }) == false,
        "observer fixture records settlement rollback");
  PeSemanticExactFixedEmission exact{};
  PeSemanticSegmentedEmission segmented{};
  std::array<std::byte, 1024u> externalRecords{};
  std::array<std::byte, 1024u> externalHandles{};
  std::array<std::byte, 4096u> externalPayload{};
  PeSemanticSegmentedEmission external{};
  check(owner.emitExactFixed(exact) && owner.emitSegmented(segmented) &&
            owner.emitSegmented(externalRecords, externalHandles, externalPayload,
                                external),
        "observer fixture emits both transport forms");
  check(segmented.transport.recordBytes == external.transport.recordBytes &&
            segmented.transport.handleBytes == external.transport.handleBytes &&
            segmented.transport.payloadBytes == external.transport.payloadBytes &&
            std::equal(externalRecords.begin(),
                       externalRecords.begin() + segmented.transport.recordBytes,
                       reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(
                           d9cWireHandleValue(segmented.transport.records)))) &&
            std::equal(externalHandles.begin(),
                       externalHandles.begin() + segmented.transport.handleBytes,
                       reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(
                           d9cWireHandleValue(segmented.transport.handles)))) &&
            std::equal(externalPayload.begin(),
                       externalPayload.begin() + segmented.transport.payloadBytes,
                       reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(
                           d9cWireHandleValue(segmented.transport.payload)))),
        "external segmented copy preserves the true alias role bytes");
  check(owner.settle(), "observer fixture settles and clears");
  const auto admitted = copyLedger.snapshot(
      dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission);
  const auto exactWire = copyLedger.snapshot(
      dxmt9::core::CopyMaterializationClass::PeWireFinal);
  check(admitted.calls == 1u && admitted.retainedBytes == 0u &&
            exactWire.calls == 1u && exactWire.bytes == exact.wireBytes &&
            exactWire.retainedBytes == 0u,
        "phase observation coexists with unchanged admission/final-wire ledger rows");
  const auto& stats = observer.stats();
  const auto phase = [&](PeSemanticOwnerPhase value) ->
      const PeSemanticOwnerPhaseMetric& {
    return stats.phases[static_cast<std::size_t>(value)];
  };
  check(stats.appendEvents == 2u && stats.appendSampled == 1u &&
            stats.operationEvents == 4u && stats.operationSampled == 0u &&
            stats.nullCalibrationSamples == 1u &&
            stats.outcomes[static_cast<std::size_t>(PeSemanticOwnerOutcome::Accepted)] == 1u &&
            stats.outcomes[static_cast<std::size_t>(PeSemanticOwnerOutcome::Settlement)] == 1u,
        "observer uses one deterministic parent decision and fixed outcomes");
  check(phase(PeSemanticOwnerPhase::TryAppendOwnedRecord).events == 2u &&
            phase(PeSemanticOwnerPhase::PrepareAdmission).events == 2u &&
            phase(PeSemanticOwnerPhase::Rollback).events == 1u &&
            phase(PeSemanticOwnerPhase::EmitExactFixed).events == 1u &&
            phase(PeSemanticOwnerPhase::EmitExactFixedBuildPlan).events == 1u &&
            phase(PeSemanticOwnerPhase::EmitSegmentedAliasView).events == 1u &&
            phase(PeSemanticOwnerPhase::EmitSegmentedExternalCopy).events == 1u &&
            phase(PeSemanticOwnerPhase::EmitSegmentedExternalRoleCopy).events == 1u &&
            phase(PeSemanticOwnerPhase::SettleClear).events == 1u &&
            phase(PeSemanticOwnerPhase::SettleRetainerEpoch).events == 1u &&
            phase(PeSemanticOwnerPhase::SettleOwnerReset).events == 1u,
        "observer covers owner and transport lifecycle phases");
  check(phase(PeSemanticOwnerPhase::PrepareAdmission).samples == 1u &&
            phase(PeSemanticOwnerPhase::FixedDirectPinCopy).samples == 1u &&
            phase(PeSemanticOwnerPhase::PendingDeltaSettlement).samples == 1u &&
            phase(PeSemanticOwnerPhase::Rollback).samples == 1u,
        "child timers are parent-gated and sampled inclusively");
  const auto& family = stats.settlements[static_cast<std::size_t>(
      PeSemanticOwnerSettlementFamily::OtherGeneric)];
  const auto& aggregate = phase(PeSemanticOwnerPhase::PendingDeltaSettlement);
  check(family.events == aggregate.events &&
            family.samples == aggregate.samples &&
            family.totalNs == aggregate.totalNs &&
            family.maxNs == aggregate.maxNs,
        "family attribution reuses the exact aggregate interval, including rollback");
  for (std::size_t i = 0u; i < stats.settlements.size() - 1u; ++i) {
    check(stats.settlements[i].events == 0u &&
              stats.settlements[i].samples == 0u,
          "unvisited settlement families do not fabricate samples");
  }
}

void observerSettlementFamilyTruthTable() {
  PeSemanticOwnerPhaseObserver observer(1u);
  PeSemanticSparsePlanRecordInput sparse{};
  const std::array producers{
      PeSemanticProducerKind::DrawPrimitive,
      PeSemanticProducerKind::DrawIndexedPrimitive,
      PeSemanticProducerKind::ApplyState};
  const std::array families{
      PeSemanticOwnerSettlementFamily::SparseDraw,
      PeSemanticOwnerSettlementFamily::SparseIndexedDraw,
      PeSemanticOwnerSettlementFamily::SparseApply};
  {
    auto parent = observer.beginAppend();
    for (std::size_t i = 0u; i < producers.size(); ++i) {
      sparse.producer = producers[i];
      check(peSemanticOwnerSettlementFamily(sparse) == families[i],
            "typed sparse input selects its exact settlement family");
      auto scope = observer.beginSettlement(peSemanticOwnerSettlementFamily(sparse));
    }
    PeSemanticConstantRecordInput constant{};
    check(peSemanticOwnerSettlementFamily(constant) ==
              PeSemanticOwnerSettlementFamily::Constant,
          "typed constants select their own settlement family");
    {
      auto scope = observer.beginSettlement(peSemanticOwnerSettlementFamily(constant));
    }
    const auto generic = base(PeSemanticProducerKind::DrawPrimitive, 1u, 1u);
    check(peSemanticOwnerSettlementFamily(generic) ==
              PeSemanticOwnerSettlementFamily::OtherGeneric,
          "generic input is not mislabeled as a compact draw");
    auto scope = observer.beginSettlement(peSemanticOwnerSettlementFamily(generic));
  }
  std::uint64_t totalNs = 0u;
  for (const auto& family : observer.stats().settlements) {
    check(family.events == 1u && family.samples == 1u,
          "each selected family reuses one aggregate interval");
    totalNs += family.totalNs;
  }
  const auto& aggregate = observer.stats().phases[static_cast<std::size_t>(
      PeSemanticOwnerPhase::PendingDeltaSettlement)];
  check(aggregate.events == 5u && aggregate.samples == 5u &&
            aggregate.totalNs == totalNs,
        "all family intervals exactly partition aggregate settlement");
}

void observedSettlementChildrenSampleAtN1() {
  using ObservedOwner = PeSemanticBatchOwner<2u, 2u, 4096u, 2u, 2u>;
  PeSemanticOwnerPhaseObserver observer(1u);
  ObservedOwner owner;
  owner.setPhaseObserver(&observer);
  D9CSurface surface;
  auto input = base(PeSemanticProducerKind::Present, 0x761u, 0x762u);
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x763u);
  check(owner.tryAppendOwnedRecord(input, []() noexcept { return true; }) &&
            owner.settle(),
        "N=1 observed owner settles one accepted record");
  const auto& stats = observer.stats();
  const auto& retainer = stats.phases[static_cast<std::size_t>(
      PeSemanticOwnerPhase::SettleRetainerEpoch)];
  const auto& reset = stats.phases[static_cast<std::size_t>(
      PeSemanticOwnerPhase::SettleOwnerReset)];
  check(retainer.events == 1u && retainer.samples == 1u &&
            reset.events == 1u && reset.samples == 1u,
        "N=1 observed settlement samples both gated child phases");
}

void observerScopeRestoresNestedState() {
  PeSemanticOwnerPhaseObserver sampledObserver(1u);
  {
    auto outer = sampledObserver.beginAppend();
    check(outer.sampled(), "N=1 parent scope samples");
    {
      auto nested = sampledObserver.beginOperation(
          PeSemanticOwnerPhase::SettleClear);
      check(nested.sampled(), "nested operation samples independently");
    }
    auto child = sampledObserver.child(PeSemanticOwnerPhase::Rollback);
    check(child.sampled(), "outer sampled state is restored after nested stop");
  }
  for (int i = 0; i < 2; ++i) {
    auto repeated = sampledObserver.beginOperation(
        PeSemanticOwnerPhase::EmitSegmentedAliasView);
    check(repeated.sampled(), "N=1 repeated operation samples");
  }
  check(sampledObserver.stats().operationEvents == 3u &&
            sampledObserver.stats().operationSampled == 3u,
        "N=1 operation denominator and samples remain complete");

  PeSemanticOwnerPhaseObserver unsampledObserver(2u);
  {
    auto outer = unsampledObserver.beginAppend();
    check(!outer.sampled(), "first decimated parent is unsampled");
    auto moved = std::move(outer);
    {
      auto nested = unsampledObserver.beginOperation(
          PeSemanticOwnerPhase::SettleClear);
      check(!nested.sampled(), "nested unsampled operation remains disabled");
    }
    auto child = unsampledObserver.child(PeSemanticOwnerPhase::Rollback);
    check(!child.sampled(), "unsampled parent state is restored after nesting");
  }
  {
    auto repeated = unsampledObserver.beginAppend();
    check(repeated.sampled(), "moved unsampled scope restores for next parent");
  }
  check(unsampledObserver.stats().appendEvents == 2u &&
            unsampledObserver.stats().appendSampled == 1u,
        "moved and nested scopes do not leave stale parent state");
}

void observerSeparatesPrepareMalformedFromHeader() {
  using ObservedOwner = PeSemanticBatchOwner<4u, 2u, 4096u, 4u, 4u>;
  PeSemanticOwnerPhaseObserver observer(1u);
  ObservedOwner owner;
  owner.setPhaseObserver(&observer);
  PeSemanticRecordInputView malformed{};
  check(!owner.tryAppendOwnedRecord(malformed, []() noexcept { return true; }) &&
            owner.lastAdmissionFailure() == ObservedOwner::AdmissionFailure::Malformed,
        "prepare malformed input is classified distinctly");
  D9CSurface surface;
  auto header = base(PeSemanticProducerKind::Present, 0u, 1u);
  header.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x704u);
  check(!owner.tryAppendOwnedRecord(header, []() noexcept { return true; }) &&
            owner.lastAdmissionFailure() == ObservedOwner::AdmissionFailure::Header,
        "post-plan header rejection remains classified as Header");
  check(observer.stats().outcomes[
             static_cast<std::size_t>(PeSemanticOwnerOutcome::Malformed)] == 1u &&
            observer.stats().outcomes[
             static_cast<std::size_t>(PeSemanticOwnerOutcome::Header)] == 1u,
        "observer outcome ledger preserves malformed and header denominators");
}

// Exercise the actual compact producer at the schema's legal maximum binding
// occurrence count. This uses an ordinary indexed draw rather than a snapshot
// or a hand-built SparseStatePlan, so plan selection, destination finalization,
// compact admission, and the generic builder oracle consume live shadows.
void realCompactSparseMaxFamilyDifferentialAndRollback(bool allDistinct,
                                                        bool faultChild = false) {
  using RealOwner = PeSemanticBatchOwner<4u, 64u, 1u << 20u, 16u, 128u>;

  PeHotStateShadow shadow{};
  PeConstShadowBlock constants{};
  PeBindingView bindings{};
  PeChunkContext chunk{};
  PeDrawPayloads payloads{};
  PeDrawParams params{
      .recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
      .primitiveType = 4u,
      .baseVertex = -7,
      .minVertex = 3u,
      .numVertices = 37u,
      .startIndex = 11u,
      .primitiveCount = 9u,
  };

  std::array<D9CTexture, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<D9CBuffer, D9C_DRAW_PACKET_MAX_STREAMS + 1u> buffers{};
  std::array<D9CShader, 2u> shaders{};
  D9CVertexDecl declaration;
  D9CBuffer indexBuffer;
  std::array<D9CSurface, D9C_DRAW_PACKET_MAX_RENDER_TARGETS + 1u>
      surfaces{};
  D9CSurface depthStencil;
  D9CTexture prefixTexture;

  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_TEXTURES; ++slot) {
    if (!allDistinct && slot == 0u) continue;  // Null occurrence variant.
    bindings.textures[slot] = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
        &textures[slot], 0x1000u + slot);
  }
  if (!allDistinct) bindings.textures[2u] = bindings.textures[1u];
  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
    if (!allDistinct && slot == 0u) continue;
    bindings.streams[slot].buffer = localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(
        &buffers[slot], 0x2000u + slot);
  }
  if (!allDistinct) bindings.streams[2u].buffer = bindings.streams[1u].buffer;
  bindings.vs = localRef<D9C_CHUNK_HANDLE_KIND_SHADER>(&shaders[0], 0x3000u);
  bindings.ps = localRef<D9C_CHUNK_HANDLE_KIND_SHADER>(&shaders[1], 0x3001u);
  bindings.vdecl = localRef<D9C_CHUNK_HANDLE_KIND_VERTEX_DECL>(
      &declaration, 0x4000u);
  bindings.indexBuffer =
      localRef<D9C_CHUNK_HANDLE_KIND_BUFFER>(&indexBuffer, 0x5000u);
  for (std::uint32_t slot = 0u;
       slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if (!allDistinct && slot == 0u) continue;
    bindings.renderTargets[slot] = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
        &surfaces[slot], 0x6000u + slot);
  }
  if (!allDistinct) bindings.renderTargets[2u] = bindings.renderTargets[1u];
  bindings.depthStencil = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(
      &depthStencil, 0x7000u);

  shadow.writer().pendingTextureMask() =
      (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  shadow.writer().pendingStreamMask() =
      (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  shadow.writer().pendingVs() = true;
  shadow.writer().pendingPs() = true;
  shadow.writer().pendingVdecl() = true;
  shadow.writer().pendingIb() = true;
  shadow.writer().pendingRtMask() =
      (1u << D9C_DRAW_PACKET_MAX_RENDER_TARGETS) - 1u;
  shadow.writer().pendingDs() = true;

  // Keep a real compact-plan record committed before the max-family attempt;
  // a failed late acquisition must preserve this exact prefix byte-for-byte.
  PeHotStateShadow prefixShadow{};
  PeConstShadowBlock prefixConstants{};
  PeBindingView prefixBindings{};
  prefixBindings.textures[0] = localRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>(
      &prefixTexture, 0x1800u);
  prefixShadow.writer().pendingTextureMask() = 1u;
  PeDrawParams prefixParams{.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                            .primitiveType = 4u,
                            .startVertex = 1u,
                            .primitiveCount = 1u};
  PeDrawPayloads prefixPayloads{};
  PeChunkContext prefixChunk{};
  const PeHotStateShadow oraclePrefixShadow = prefixShadow;
  const PeConstShadowBlock oraclePrefixConstants = prefixConstants;
  const PeHotStateShadow oracleMaxShadow = shadow;
  const PeConstShadowBlock oracleMaxConstants = constants;

  // Build the independent generic producer oracle before owner settlement
  // consumes the pending delta. Drain builder retention before owner checks.
  CommandChunkBuilder oracle;
  std::vector<std::byte> oracleBytes;
  if (!faultChild) {
    PeSparseScratch prefixOracleScratch{};
    PeSparseScratch oracleScratch{};
    PeConstShadowBlock prefixOracleConstants = prefixConstants;
    PeConstShadowBlock oracleConstants = constants;
    SparseStateInput prefixOracleState{};
    SparseStateInput oracleState{};
    D9CCommandChunkWireDrawHeader prefixOracleHeader{};
    D9CCommandChunkWireDrawHeader oracleHeader{};
    check(buildSparseState(prefixShadow, prefixOracleConstants, prefixBindings,
                           prefixPayloads, prefixParams, false, false,
                           prefixOracleScratch, prefixOracleHeader,
                           prefixOracleState) &&
              appendSparseRecord(oracle, prefixParams.recordType,
                                 prefixOracleHeader, prefixOracleState) &&
              buildSparseState(shadow, oracleConstants, bindings, payloads,
                               params, false, false, oracleScratch, oracleHeader,
                               oracleState) &&
              appendSparseRecord(oracle, params.recordType, oracleHeader,
                                 oracleState),
          "real compact max-family generic producer oracle emits");
    const auto oracleSealed = oracle.seal();
    check(oracleSealed.valid() && oracleSealed.blob.size() != 0u,
          "real compact max-family oracle seals canonical bytes");
    oracleBytes.assign(oracleSealed.blob.begin(), oracleSealed.blob.end());
    oracle.resetAndReleaseRetained();
  }

  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken ownerThread;
  std::uint64_t epoch = 1u;
  RecorderLockGuard guard(mutex, false);
  RealOwner owner;
  check(owner.constructionSucceeded(),
        "real compact max-family owner has sufficient bounded capacity");

  SparseStatePlan prefixPlan;
  SparseStatePlan maxPlan;
  PeSemanticCompactPreparedRecord prepared{};
  std::size_t preparedIdentityCount = 0u;
  std::size_t preparedWireCount = 0u;
  std::size_t preparedOccurrenceCount = 0u;
  bool preparedMaxWitness = false;
  bool maxPlanBuilt = false;
  bool maxPlanFinalized = false;
  const bool built = guard.withCapability(
      ownerThread, epoch, [&](const auto& access) noexcept {
        if (!buildSparseStatePlan(access, prefixShadow, prefixConstants,
                                  prefixBindings, prefixPayloads, prefixParams,
                                  false, false, prefixPlan) ||
            !finalizeSparseStatePlanChunkContext(access, prefixChunk,
                                                 prefixPlan)) {
          return false;
        }
        const auto accepted = settleRecorderAppend(
            {.phase = AppendSettlement::Prepared, .appendSucceeded = true});
        if (!owner.tryAppendSparseStatePlan(
                access, prefixPlan, PeSemanticProducerKind::DrawPrimitive,
                prefixParams.recordType,
                D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u, [&]() noexcept {
                  return acceptSparseStatePlan(
                      access, prefixShadow, prefixConstants, prefixPlan,
                      accepted, nullptr, 1u);
                })) {
          return false;
        }
        maxPlanBuilt = buildSparseStatePlan(
            access, shadow, constants, bindings, payloads, params, false,
            false, maxPlan);
        maxPlanFinalized = maxPlanBuilt &&
            finalizeSparseStatePlanChunkContext(access, chunk, maxPlan);
        if (maxPlanFinalized) {
          const PeSemanticSparsePlanRecordInput witnessInput{
              .access = &access,
              .sparsePlan = &maxPlan,
              .producer = PeSemanticProducerKind::DrawIndexedPrimitive,
              .recordType = params.recordType,
              .recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
              .sourceOrdinal = 2u,
              .recordOrdinal = 2u,
          };
          preparedMaxWitness =
              owner.prepareAdmission(witnessInput, prepared) ==
              PeSemanticAdmissionOutcome::Admissible;
          preparedIdentityCount = prepared.identityWitness.count;
          preparedWireCount = prepared.identityWitness.wireCount;
          preparedOccurrenceCount = prepared.bindingOccurrences.count;
        }
        return maxPlanFinalized;
      });
  check(built && maxPlanBuilt && maxPlanFinalized && owner.size() == 1u,
        "real compact max-family plan builds under live recorder access");
  check(preparedMaxWitness && preparedOccurrenceCount == 45u &&
            preparedIdentityCount == (allDistinct ? 45u : 39u) &&
            preparedWireCount == (allDistinct ? 45u : 39u),
        "compact prepared witness records 45 live family occurrences and identities");

  PeSemanticExactFixedEmission prefixEmission{};
  check(owner.emitExactFixed(prefixEmission),
        "real compact prefix emits before late settlement fault");
  const std::vector<std::byte> prefixBytes(prefixEmission.wire.begin(),
                                           prefixEmission.wire.end());
  const auto prefixTextureRefs = prefixTexture.refs;

  bool faultHandled = false;
  bool settlementReached = false;
  if (faultChild) {
    const bool faulted = guard.withCapability(
        ownerThread, epoch, [&](const auto& access) noexcept {
          return !owner.tryAppendSparseStatePlan(
              access, maxPlan, PeSemanticProducerKind::DrawIndexedPrimitive,
              params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 2u, 2u,
              [&]() noexcept {
                settlementReached = true;
                return true;
              });
        });
    check(faulted &&
              owner.lastAdmissionFailure() ==
                  RealOwner::AdmissionFailure::SparseDepthStencils &&
              !settlementReached,
          "fresh-process retain fault reaches the final depth-stencil family before settlement");
    faultHandled = true;
  }

  // Reject after compact admission has acquired the max-family pins. This is
  // the late/partial-AddRef path: preserve the committed prefix and release
  // every newly acquired unique family pin before retrying the same plan.
  const auto rejected = settleRecorderAppend(
      {.phase = AppendSettlement::Prepared, .appendSucceeded = false});
  const bool rejectedAppend = !faultHandled && guard.withCapability(
      ownerThread, epoch, [&](const auto& access) noexcept {
        return !owner.tryAppendSparseStatePlan(
            access, maxPlan, PeSemanticProducerKind::DrawIndexedPrimitive,
            params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 2u, 2u,
            [&]() noexcept {
              return acceptSparseStatePlan(access, shadow, constants, maxPlan,
                                           rejected, nullptr, 2u);
            });
      });
  check((faultHandled || rejectedAppend) && owner.size() == 1u &&
            owner.retainedCount() == 1u && prefixTexture.refs == prefixTextureRefs,
        "late max-family settlement fault preserves committed prefix");
  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_TEXTURES; ++slot)
    check(textures[slot].refs == 1u,
          "late max-family texture AddRef rolls back completely");
  for (auto& object : buffers)
    check(object.refs == 1u,
          "late max-family buffer AddRef rolls back completely");
  for (auto& object : shaders)
    check(object.refs == 1u,
          "late max-family shader AddRef rolls back completely");
  check(declaration.refs == 1u && indexBuffer.refs == 1u,
        "late max-family declaration/index AddRef rolls back completely");
  for (auto& object : surfaces)
    check(object.refs == 1u,
          "late max-family render-target AddRef rolls back completely");
  check(depthStencil.refs == 1u,
        "late max-family depth-stencil AddRef rolls back completely");
  PeSemanticExactFixedEmission afterFault{};
  check(owner.emitExactFixed(afterFault) &&
            afterFault.wireBytes == prefixBytes.size() &&
            std::equal(prefixBytes.begin(), prefixBytes.end(),
                       afterFault.wire.begin()),
        "late max-family fault leaves owner canonical prefix unchanged");

  if (faultChild) {
    check(faultHandled &&
              owner.lastAdmissionFailure() ==
                  RealOwner::AdmissionFailure::SparseDepthStencils &&
              !settlementReached,
          "fresh-process retain fault reaches the final depth-stencil family before settlement");
    check(owner.size() == 1u && owner.retainedCount() == 1u &&
              prefixTexture.refs == prefixTextureRefs,
          "final depth-stencil fault preserves the committed prefix");
    for (std::uint32_t slot = 0u;
         slot < D9C_DRAW_PACKET_MAX_TEXTURES; ++slot)
      check(textures[slot].refs == 1u,
            "final depth-stencil fault rolls back every texture AddRef");
    for (auto& object : buffers)
      check(object.refs == 1u,
            "final depth-stencil fault rolls back every buffer AddRef");
    for (auto& object : shaders)
      check(object.refs == 1u,
            "final depth-stencil fault rolls back every shader AddRef");
    check(declaration.refs == 1u && indexBuffer.refs == 1u,
          "final depth-stencil fault rolls back declaration/index AddRef");
    for (auto& object : surfaces)
      check(object.refs == 1u,
            "final depth-stencil fault rolls back every render-target AddRef");
    check(depthStencil.refs == 1u,
          "final depth-stencil fault rolls back depth-stencil AddRef");

    PeSparseScratch childPrefixScratch{};
    PeSparseScratch childMaxScratch{};
    PeConstShadowBlock childPrefixConstants = oraclePrefixConstants;
    PeConstShadowBlock childMaxConstants = oracleMaxConstants;
    SparseStateInput childPrefixState{};
    SparseStateInput childMaxState{};
    D9CCommandChunkWireDrawHeader childPrefixHeader{};
    D9CCommandChunkWireDrawHeader childMaxHeader{};
    CommandChunkBuilder childOracle;
    check(buildSparseState(oraclePrefixShadow, childPrefixConstants,
                           prefixBindings, prefixPayloads, prefixParams, false,
                           false, childPrefixScratch, childPrefixHeader,
                           childPrefixState) &&
              appendSparseRecord(childOracle, prefixParams.recordType,
                                 childPrefixHeader, childPrefixState) &&
              buildSparseState(oracleMaxShadow, childMaxConstants, bindings,
                               payloads, params, false, false, childMaxScratch,
                               childMaxHeader, childMaxState) &&
              appendSparseRecord(childOracle, params.recordType, childMaxHeader,
                                 childMaxState),
          "post-fault generic producer oracle emits after fault budget consumption");
    const auto childSealed = childOracle.seal();
    check(childSealed.valid(), "post-fault generic oracle seals");
    oracleBytes.assign(childSealed.blob.begin(), childSealed.blob.end());
    childOracle.resetAndReleaseRetained();

    const auto accepted = settleRecorderAppend(
        {.phase = AppendSettlement::Prepared, .appendSucceeded = true});
    const bool retried = guard.withCapability(
        ownerThread, epoch, [&](const auto& access) noexcept {
          return owner.tryAppendSparseStatePlan(
              access, maxPlan, PeSemanticProducerKind::DrawIndexedPrimitive,
              params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 2u, 2u,
              [&]() noexcept {
                return acceptSparseStatePlan(access, shadow, constants, maxPlan,
                                             accepted, nullptr, 2u);
              });
        });
    PeSemanticExactFixedEmission childFinal{};
    check(retried && owner.size() == 2u &&
              owner.emitExactFixed(childFinal) &&
              childFinal.wireBytes == oracleBytes.size() &&
              std::equal(oracleBytes.begin(), oracleBytes.end(),
                         childFinal.wire.begin()),
          "post-fault clean retry equals the delayed generic oracle");
    owner.reset();
    check(prefixTexture.refs == 1u,
          "post-fault retry reset releases the committed prefix pin");
    return;
  }

  const auto accepted = settleRecorderAppend(
      {.phase = AppendSettlement::Prepared, .appendSucceeded = true});
  const bool retried = guard.withCapability(
      ownerThread, epoch, [&](const auto& access) noexcept {
        return owner.tryAppendSparseStatePlan(
            access, maxPlan, PeSemanticProducerKind::DrawIndexedPrimitive,
            params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 2u, 2u,
            [&]() noexcept {
              return acceptSparseStatePlan(access, shadow, constants, maxPlan,
                                           accepted, nullptr, 2u);
            });
      });
  PeSemanticExactFixedEmission finalEmission{};
  check(retried && owner.size() == 2u && owner.emitExactFixed(finalEmission) &&
            finalEmission.wireBytes == oracleBytes.size() &&
            std::equal(oracleBytes.begin(), oracleBytes.end(),
                       finalEmission.wire.begin()),
        "real compact max-family retry equals generic producer oracle bytes");
  owner.reset();
  check(prefixTexture.refs == 1u,
        "real compact max-family reset releases the committed prefix pin");
}

void runRealCompactRetainFaultChild(const char* executable) {
  const auto pid = fork();
  check(pid >= 0, "real compact retain fault fixture forks a child");
  if (pid == 0) {
    setenv("DXMT9_PE_RECORDER_FAULT", "retain_acquire=45", 1);
    execlp(executable, executable, "--real-compact-retain-fault-child",
           static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0,
        "real compact final depth-stencil retain fault child succeeds");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 &&
      std::strcmp(argv[1], "--real-compact-retain-fault-child") == 0) {
    try {
      realCompactSparseMaxFamilyDifferentialAndRollback(true, true);
    } catch (const Failure& failure) {
      std::cerr << "pe_semantic_owner_spec fault child failed: "
                << failure.what() << '\n';
      return 1;
    }
    return 0;
  }
  try {
    static_assert(kPeSemanticMaxBindingOccurrences == 45u);
    static_assert(sizeof(PeSemanticCompactIdentityWitness) <
                  sizeof(PeSemanticPreparedIdentityWitness));
    static_assert(sizeof(PeSemanticCompactPreparedRecord) <
                  sizeof(PeSemanticPreparedRecord));
    static_assert(PeSemanticCompactIdentityWitness::entryCapacity == 45u);
    static_assert(sizeof(PeSemanticSparsePlanRecordStaging) <
                  sizeof(PeSemanticRecordSlot));
    static_assert(sizeof(PeSemanticConstantRecordStaging) <
                  sizeof(PeSemanticRecordSlot));
    static_assert(sizeof(PeSemanticSparsePlanRecordStaging) ==
                  sizeof(D9CCommandChunkWireDrawHeader));
    static_assert(sizeof(PeSemanticConstantRecordStaging) ==
                  sizeof(D9CCommandChunkWireSetConst));
    static_assert(sizeof(PeSemanticRecordReceipt) <
                  sizeof(PeSemanticRecordSlot));
    static_assert(sizeof(PeSemanticAdmissionPlan) <= 256u);
    static_assert(std::is_trivially_copyable_v<PeSemanticEmissionHandleContext>);
    static_assert(sizeof(PeSemanticEmissionHandleContext) <= 1088u);
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
    compactIdentityWitnessBoundMatchesSevenFamilies();
    compactSparsePreparationSelectsSchemaWitness();
    duplicateSurfacePinSmoke();
    qualifiedBufferReferenceLookup();
    pureAdmissionPlanDeduplicatesQualifiedBindings();
    preparedWireWitnessPreservesOrderAndQualifiedDedup();
    knownUniqueWireWitnessAppendIsBounded();
    planningReusesStaleWireWitnessStorage();
    identitySetUsesAnActiveFrontier();
    preparedWireWitnessTracksFirstWirePromotion();
    preparedIdentityWitnessUsesBoundedActivePrefixes();
    admissionArithmeticRejectsOverflow();
    preparedLayoutCoversEverySparseAndVariableSection();
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
    preparedAdmissionReinitializesReusedWitness();
    directCanonicalScalarAndConstantMasks();
    directCanonicalMutationAndPendingRefinement();
    directCanonicalQuotaAndSettlementRefinement();
    directCanonicalLedgerExcludesDuplicateStaging();
    preparedWitnessSettlementAndRollback();
    genericConstantRetainsForeignDirectAuthority();
    typedConstantAdmissionDifferential<PeSemanticProducerKind::VsFloatConstant>();
    typedConstantAdmissionDifferential<PeSemanticProducerKind::VsIntConstant>();
    typedConstantAdmissionDifferential<PeSemanticProducerKind::VsBoolConstant>();
    typedConstantAdmissionDifferential<PeSemanticProducerKind::PsFloatConstant>();
    typedConstantAdmissionDifferential<PeSemanticProducerKind::PsIntConstant>();
    typedConstantAdmissionDifferential<PeSemanticProducerKind::PsBoolConstant>();
    inactiveBindingMetadataRemainsNonAuthoritative();
    bindingOccurrenceStorageUsesActiveFrontier();
    preparedBindingSevenFamilyMatrix();
    classificationAndLifetime();
    partialRectClearMatchesCanonicalBuilder();
    emptyClearSpanMatchesCanonicalAndResetReuse();
    largeCanonicalPayloadSurvivesRepeatedExactEmission();
    exactPaddingWriterAdversarial();
    collisionCopyOverflowAndRetry();
    pinIndexGenerationReferenceTrace();
    r712R713BoundedIdentityAndLifecycleRefinement();
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
    calibratedOwnerPhaseObserver();
    observerSettlementFamilyTruthTable();
    observedSettlementChildrenSampleAtN1();
    observerScopeRestoresNestedState();
    observerSeparatesPrepareMalformedFromHeader();
    realCompactSparseMaxFamilyDifferentialAndRollback(true);
    realCompactSparseMaxFamilyDifferentialAndRollback(false);
    runRealCompactRetainFaultChild(argv[0]);
  } catch (const Failure& failure) {
    std::cerr << "pe_semantic_owner_spec failed: " << failure.what() << '\n';
    return 1;
  }
  return 0;
}
