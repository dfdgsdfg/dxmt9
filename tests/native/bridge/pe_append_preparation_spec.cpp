// Bounded binding of production family selection and fault prediction.
// Real owner inputs are compared with direct inputs/canonical builder bytes.
// This is not an end-to-end COM-device HRESULT/cadence harness.

#include "d3d9_pe_append_preparation.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_semantic_owner.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

struct RefCounter {
  std::uint32_t refs = 1u;
};
struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};

template <typename T>
void appendPreparationAddRef(T* value) {
  ++value->refs;
}
template <typename T>
std::uint32_t appendPreparationRelease(T* value) {
  return --value->refs;
}

extern "C" void dxmt9c_surface_addref(D9CSurface* value) {
  appendPreparationAddRef(value);
}
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) {
  return appendPreparationRelease(value);
}
extern "C" void dxmt9c_texture_addref(D9CTexture* value) {
  appendPreparationAddRef(value);
}
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* value) {
  return appendPreparationRelease(value);
}
extern "C" void dxmt9c_buffer_addref(D9CBuffer* value) {
  appendPreparationAddRef(value);
}
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* value) {
  return appendPreparationRelease(value);
}
extern "C" void dxmt9c_shader_addref(D9CShader* value) {
  appendPreparationAddRef(value);
}
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* value) {
  return appendPreparationRelease(value);
}
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* value) {
  appendPreparationAddRef(value);
}
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* value) {
  return appendPreparationRelease(value);
}
extern "C" void dxmt9c_query_addref(D9CQuery* value) {
  appendPreparationAddRef(value);
}
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* value) {
  return appendPreparationRelease(value);
}

namespace {

namespace pe = dxmt9::d3d9::pe;
using Owner = pe::PeSemanticBatchOwner<8u, 8u, 4096u, 8u, 8u>;

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw Failure(std::string(message));
}

template <std::uint32_t Kind, typename Object>
pe::PeLocalObjectRef<Kind> localRef(Object* object, std::uint32_t objectId) {
  pe::PeLocalObjectRef<Kind> result{};
  result.identity = {.kind = Kind, .generation = 1u, .objectId = objectId};
  result.object = object;
  return result;
}

void selectedPreparationCarriesOnlyItsFamily() {
  pe::PeSemanticRecordInput generic{};
  pe::PeSemanticConstantRecordInput constant{};
  pe::SparseStatePlan sparse{};
  for (const auto family : {PeSemanticStagingFamily::Constant,
                            PeSemanticStagingFamily::SparsePlan,
                            PeSemanticStagingFamily::Cold,
                            PeSemanticStagingFamily::Draw,
                            PeSemanticStagingFamily::None}) {
    unsigned calls = 0u;
    const bool selected = pe::withPeSemanticAppendPreparation(
        family, generic, constant, sparse,
        pe::PeSemanticProducerKind::DrawIndexedPrimitive,
        [&](auto& preparation) {
          ++calls;
          using P = std::remove_cvref_t<decltype(preparation)>;
          preparation.setDestination(17u, 31u, 41u);
          if constexpr (pe::isPeSemanticConstantAppendPreparation<P>) {
            static_assert(sizeof(P) == sizeof(void*));
            return family == PeSemanticStagingFamily::Constant &&
                   preparation.input == &constant &&
                   constant.sourceOrdinal == 31u && constant.recordOrdinal == 41u;
          } else if constexpr (pe::isPeSemanticSparseAppendPreparation<P>) {
            return family == PeSemanticStagingFamily::SparsePlan &&
                   preparation.plan == &sparse &&
                   preparation.producer == pe::PeSemanticProducerKind::DrawIndexedPrimitive &&
                   preparation.sourceOrdinal == 31u && preparation.recordOrdinal == 41u;
          } else {
            return family != PeSemanticStagingFamily::Constant &&
                   family != PeSemanticStagingFamily::SparsePlan &&
                   preparation.inputView.staged == &generic &&
                   preparation.inputView.destination.sourceOrdinal == 31u &&
                   preparation.inputView.destination.recordOrdinal == 41u;
          }
        });
    check(selected && calls == 1u, "exactly one typed preparation is selected");
  }
  bool propagated = false;
  try {
    pe::withPeSemanticAppendPreparation(
        PeSemanticStagingFamily::Constant, generic, constant, sparse,
        constant.producer, [](auto&) -> bool { throw Failure("sentinel"); });
  } catch (const Failure&) {
    propagated = true;
  }
  check(propagated, "family dispatch preserves the envelope exception boundary");
}

void faultThresholdTruthTable() {
  const auto maximum = std::numeric_limits<std::size_t>::max();
  for (const bool enabled : {false, true})
    for (const std::size_t records : {0u, 1u, 3u, 4u})
      for (const auto payload : {std::size_t{0u}, std::size_t{15u},
                                std::size_t{16u}, std::size_t{17u}, maximum})
        for (const auto bytes : {std::size_t{0u}, std::size_t{1u},
                                std::size_t{16u}, std::size_t{17u}, maximum}) {
          unsigned queries = 0u;
          const bool actual = pe::peSemanticFaultWouldFlush(
              enabled, records, payload, bytes, [&]() noexcept {
                ++queries;
                return pe::PeSemanticAppendLimits{3u, 16u};
              });
          // Independent overflow-free spelling of the old payload threshold.
          const bool overflow = payload > 16u ||
                                (payload <= 16u && bytes > 16u - payload);
          const bool expected = enabled && records != 0u &&
                                (records >= 3u || overflow);
          check(actual == expected && (enabled ? queries <= 1u : queries == 0u),
                "fault-OFF skips limits; enabled payload boundary preserves truth table");
        }
}

template <typename O>
std::vector<std::byte> wire(O& owner) {
  pe::PeSemanticExactFixedEmission emission{};
  check(owner.emitExactFixed(emission), "emit accepted owner prefix");
  return {emission.wire.begin(), emission.wire.end()};
}

template <typename O>
bool appendGeneric(O& owner, pe::PeSemanticRecordInput& input, bool settle = true) {
  pe::PeSemanticConstantRecordInput constant{};
  pe::SparseStatePlan sparse{};
  return pe::withPeSemanticAppendPreparation(
      PeSemanticStagingFamily::Cold, input, constant, sparse, input.producer,
      [&](auto& preparation) noexcept {
        using P = std::remove_cvref_t<decltype(preparation)>;
        if constexpr (pe::isPeSemanticGenericAppendPreparation<P>) {
          preparation.setDestination(input.recordType, input.sourceOrdinal,
                                     input.recordOrdinal);
          return owner.tryAppendOwnedRecord(
              preparation.inputView, [settle]() noexcept { return settle; });
        }
        return false;
      });
}

void genericTraceMatchesCapacityAndFailureEdges() {
  using Tiny = pe::PeSemanticBatchOwner<3u, 8u, 4096u, 8u, 8u>;
  Tiny actual, reference;
  D9CSurface surface;
  pe::PeSemanticRecordInput input{};
  input.producer = pe::PeSemanticProducerKind::Present;
  input.recordType = D9C_COMMAND_RECORD_PRESENT;
  input.surface0 = localRef<D9C_CHUNK_HANDLE_KIND_SURFACE>(&surface, 0x101u);
  for (std::uint64_t ordinal = 1u; ordinal <= 4u; ++ordinal) {
    input.sourceOrdinal = input.recordOrdinal = ordinal;
    const bool a = appendGeneric(actual, input);
    const bool b = reference.tryAppendOwnedRecord(input, []() noexcept { return true; });
    check(a == b && a == (ordinal <= 3u) &&
              actual.lastAdmissionFailure() == reference.lastAdmissionFailure() &&
              wire(actual) == wire(reference),
          "generic preparation preserves accepted bytes and capacity-denied prefix");
  }
  actual.reset();
  reference.reset();
  input.sourceOrdinal = input.recordOrdinal = 1u;
  check(!appendGeneric(actual, input, false) &&
            !reference.tryAppendOwnedRecord(input, []() noexcept { return false; }) &&
            actual.size() == 0u && surface.refs == 1u,
        "generic settlement rollback releases pins before same-ordinal retry");
  check(appendGeneric(actual, input) && reference.admit(input) &&
            wire(actual) == wire(reference),
        "generic retry preserves exact source identity and wire");
  actual.reset();
  reference.reset();
  input.surface0.identity.kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
  check(!appendGeneric(actual, input) && !reference.admit(input) &&
            actual.lastAdmissionFailure() == reference.lastAdmissionFailure() &&
            actual.size() == 0u && reference.size() == 0u,
        "malformed generic reference fails on non-full owners, independently of capacity");
}

void constantDifferentialAndSettlement() {
  struct Family { pe::PeSemanticProducerKind producer;
                  ConstShadow PeConstShadowBlock::* shadow;
                  std::size_t bytes; };
  const std::array families{
      Family{pe::PeSemanticProducerKind::VsFloatConstant, &PeConstShadowBlock::vsConstF, 16u},
      Family{pe::PeSemanticProducerKind::VsIntConstant, &PeConstShadowBlock::vsConstI, 16u},
      Family{pe::PeSemanticProducerKind::VsBoolConstant, &PeConstShadowBlock::vsConstB, 4u},
      Family{pe::PeSemanticProducerKind::PsFloatConstant, &PeConstShadowBlock::psConstF, 16u},
      Family{pe::PeSemanticProducerKind::PsIntConstant, &PeConstShadowBlock::psConstI, 16u},
      Family{pe::PeSemanticProducerKind::PsBoolConstant, &PeConstShadowBlock::psConstB, 4u}};
  for (const auto& family : families) {
    Owner actual, reference;
    pe::CommandChunkBuilder canonical;
    PeConstShadowBlock constants, referenceConstants;
    PeHotStateShadow pending;
    pe::PeSemanticRecordInput generic{};
    pe::SparseStatePlan sparse{};
    pe::PeSemanticConstantRecordInput input{};
    input.producer = family.producer;
    input.recordType = pe::peSemanticRecordTypeForProducer(family.producer);
    input.setConst = {0u, 1u};
    std::array<std::byte, 16u> values{};
    input.constantBytes = std::span<const std::byte>(values).first(family.bytes);
    // A/B/A, then valid payload + stale ticket, then same-ordinal retry.
    for (unsigned step = 0u; step < 5u; ++step) {
      const bool fail = step == 3u;
      input.sourceOrdinal = input.recordOrdinal = step < 3u ? step + 1u : 4u;
      values.fill(step == 1u ? std::byte{0x52} : std::byte{0x31});
      auto& shadow = constants.*family.shadow;
      shadow.values.assign(family.bytes, std::uint8_t{0x31});
      shadow.dirtyElems = {1u};
      shadow.dirtyStart = 0u;
      shadow.dirtyEnd = 1u;
      referenceConstants.*family.shadow = shadow;
      const auto ticket = fail ? PendingDeltaTicket{2u} : pending.pendingTicket();
      pe::PeSemanticConstantSettlement settlement(constants, pending, ticket);
      pe::PeSemanticConstantSettlement referenceSettlement(referenceConstants, pending, ticket);
      const bool accepted = pe::withPeSemanticAppendPreparation(
          PeSemanticStagingFamily::Constant, generic, input, sparse, input.producer,
          [&](auto& preparation) noexcept {
            using P = std::remove_cvref_t<decltype(preparation)>;
            if constexpr (pe::isPeSemanticConstantAppendPreparation<P>) {
              preparation.setDestination(input.recordType, input.sourceOrdinal,
                                         input.recordOrdinal);
              return actual.tryAppendConstantRecord(*preparation.input, settlement);
            }
            return false;
          });
      const bool expected = reference.tryAppendOwnedRecord(input, [&]() noexcept {
        return referenceSettlement.settle(input.producer, input.setConst, input.constantBytes);
      });
      check(accepted == !fail && accepted == expected &&
                actual.lastAdmissionFailure() == reference.lastAdmissionFailure() &&
                shadow.dirty() == fail &&
                shadow.dirtyElems == (referenceConstants.*family.shadow).dirtyElems &&
                wire(actual) == wire(reference),
            "six constant families preserve A/B/A, real settlement failure, retry and bytes");
      if (accepted)
        check(pe::appendSetConstants(canonical, input.recordType, 0u, 1u,
                                     input.constantBytes), "canonical constant oracle appends");
    }
    const auto sealed = canonical.seal();
    check(sealed.valid() &&
              std::vector<std::byte>(sealed.blob.begin(), sealed.blob.end()) == wire(actual),
          "family-specialized constant append matches independent canonical builder");
  }
}

void compactTraceUsesRealPlanAdapter() {
  Owner owner;
  PeHotStateShadow shadow;
  shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(7u), 1u);
  PeConstShadowBlock constants;
  pe::PeBindingView bindings;
  pe::PeDrawPayloads payloads;
  pe::PeDrawParams params{.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                          .primitiveType = 4u, .primitiveCount = 1u};
  pe::PeChunkContext chunk{};
  pe::SparseStatePlan plan;
  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken thread;
  std::uint64_t epoch = 1u;
  pe::RecorderLockGuard guard(mutex, false);
  const bool accepted = guard.withCapability(
      thread, epoch, [&](const auto& access) noexcept {
        if (!pe::buildSparseStatePlan(access, shadow, constants, bindings,
                                      payloads, params, false, false, plan) ||
            !pe::finalizeSparseStatePlanChunkContext(access, chunk, plan))
          return false;
        pe::PeSemanticRecordInput generic;
        pe::PeSemanticConstantRecordInput constant;
        return pe::withPeSemanticAppendPreparation(
            PeSemanticStagingFamily::SparsePlan, generic, constant, plan,
            pe::PeSemanticProducerKind::DrawPrimitive,
            [&](auto& preparation) noexcept {
              using P = std::remove_cvref_t<decltype(preparation)>;
              if constexpr (pe::isPeSemanticSparseAppendPreparation<P>) {
                preparation.setDestination(params.recordType, 1u, 1u);
                return owner.tryAppendSparseStatePlan(
                    access, *preparation.plan, preparation.producer, params.recordType,
                    D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, preparation.sourceOrdinal,
                    preparation.recordOrdinal, []() noexcept { return true; });
              }
              return false;
            });
      });
  check(accepted && owner.size() == 1u, "compact adapter drives real owner admission");
}

}  // namespace

int main() {
  try {
    selectedPreparationCarriesOnlyItsFamily();
    faultThresholdTruthTable();
    genericTraceMatchesCapacityAndFailureEdges();
    constantDifferentialAndSettlement();
    compactTraceUsesRealPlanAdapter();
  } catch (const Failure& failure) {
    std::cerr << "pe_append_preparation_spec failed: " << failure.what() << '\n';
    return 1;
  }
  return 0;
}
