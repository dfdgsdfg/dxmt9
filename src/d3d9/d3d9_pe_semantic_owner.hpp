#pragma once

// Chunk-scoped semantic ownership for the PE recorder.
//
// This is deliberately a value/typed layer.  A producer may pass its
// call-local typed views to admit(), but this owner never stores a wire object
// reference or a borrowed span.  Physical objects are pinned immediately and
// are represented by kind-qualified typed slots for the rest of the chunk.
// The owner is the sole all-family production final-wire transaction.
// Compatibility builders remain test/bootstrap oracles, never fallbacks.

#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_constant_settlement.hpp"
#include "d3d9_pe_retainer.hpp"
#include "d3d9_pe_sparse_iteration.hpp"
#include "d3d9_pe_semantic_tokens.hpp"
#include "d3d9_pe_semantic_owner_phase_observer.hpp"
#include "dxmt9/copy_materialization_ledger.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace dxmt9::d3d9::pe {

inline constexpr std::uint32_t kPeSemanticNoSlot =
    std::numeric_limits<std::uint32_t>::max();

template <std::uint32_t Kind>
struct PeSemanticQualifiedIdentity {
  D9CWireObjectIdentity value{};

  constexpr bool valid() const noexcept {
    return value.kind == Kind && value.generation != 0u &&
           value.objectId != 0u;
  }
  friend constexpr bool operator==(const PeSemanticQualifiedIdentity& left,
                                   const PeSemanticQualifiedIdentity& right) noexcept {
    return left.value.kind == right.value.kind &&
           left.value.generation == right.value.generation &&
           left.value.objectId == right.value.objectId;
  }
};

using PeSemanticSurfaceIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_SURFACE>;
using PeSemanticTextureIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_TEXTURE>;
using PeSemanticBufferIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_BUFFER>;
using PeSemanticShaderIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_SHADER>;
using PeSemanticDeclarationIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_VERTEX_DECL>;
using PeSemanticQueryIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_QUERY>;

template <typename Object, typename Identity>
struct PeSemanticPhysicalPin {
  Object* object = nullptr;
  Identity identity{};

  bool valid() const noexcept { return object != nullptr && identity.valid(); }
};

struct PeSemanticArenaRange {
  std::uint32_t offset = 0u;
  std::uint32_t count = 0u;

  bool empty() const noexcept { return count == 0u; }
};

// The input is borrowed only during admit().  It is intentionally closed over
// the current producer vocabulary: fixed values remain typed and only
// constants, UP data, and clear rectangles are span inputs.
struct PeSemanticRecordInput {
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  std::uint32_t recordType = 0u;
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;

  D9CCommandChunkWireDrawHeader draw{};
  D9CCommandChunkWireSetConst setConst{};
  D9CCommandChunkWireClear clear{};
  D9CCommandChunkWirePresent present{};
  D9CCommandChunkWireStretchRect stretchRect{};
  D9CCommandChunkWireColorFill colorFill{};
  D9CCommandChunkWireUpdateSurface updateSurface{};
  D9CCommandChunkWireQueryIssue queryIssue{};
  std::uint32_t updateFlags = 0u;
  std::uint32_t reszFlags = 0u;
  std::uint32_t mipmapFlags = 0u;

  // Direct fixed-record object inputs, already kind-qualified by the caller.
  SurfaceRef surface0{};
  SurfaceRef surface1{};
  TextureRef texture0{};
  TextureRef texture1{};
  BufferRef buffer0{};
  BufferRef buffer1{};
  ShaderRef shader0{};
  ShaderRef shader1{};
  DeclarationRef declaration{};
  QueryRef query{};

  SparseStateInput sparse{};
  std::span<const std::byte> constantBytes{};
  std::span<const D9CRect> clearRects{};
};

// Pure admission facts for one immutable record input.  This is deliberately
// independent of owner counters: the owner uses it as a local layout/count
// oracle before applying its owner-qualified retention delta.
// Identity counts are qualified by kind/generation/objectId and deduplicated
// within the record, matching the final-wire handle table semantics.
struct PeSemanticAdmissionPlan {
  bool valid = false;
  std::uint32_t recordType = 0u;
  std::uint32_t handleCount = 0u;
  std::uint32_t payloadBytes = 0u;
  std::uint32_t rectCount = 0u;
  std::uint32_t semanticBytes = 0u;
  // Public differential/oracle projection; compact production quota consumers
  // use PeSemanticPreparedSparseLayout::quotaDeltas as the sole authority.
  std::array<std::uint32_t, 17u> sparseCounts{};
  // Public differential/oracle projection. Compact production witnesses use
  // PeSemanticCompactAdmissionPlan instead of materializing this summary.
  std::array<std::uint32_t, 6u> uniquePinCounts{};
};

static_assert(std::is_trivially_copyable_v<PeSemanticAdmissionPlan>);

// Generic production preparation still needs the full sparse quota projection
// for foreign/non-compact producers, but its retention result is already
// carried by the owner-local delta witness.  Keep the six-entry unique-pin
// projection on the public oracle only.
struct PeSemanticProductionAdmissionPlan {
  bool valid = false;
  std::uint32_t recordType = 0u;
  std::uint32_t handleCount = 0u;
  std::uint32_t payloadBytes = 0u;
  std::uint32_t rectCount = 0u;
  std::uint32_t semanticBytes = 0u;
  std::array<std::uint32_t, 17u> sparseCounts{};
};

static_assert(std::is_trivially_copyable_v<PeSemanticProductionAdmissionPlan>);

// Compact production preparation needs only the facts consumed by capacity
// and emission. Sparse quota and unique-count projections remain in the public
// oracle plan above, not in this hot-path witness.
struct PeSemanticCompactAdmissionPlan {
  bool valid = false;
  std::uint32_t recordType = 0u;
  std::uint32_t handleCount = 0u;
  std::uint32_t payloadBytes = 0u;
  std::uint32_t rectCount = 0u;
  std::uint32_t semanticBytes = 0u;
};

static_assert(std::is_trivially_copyable_v<PeSemanticCompactAdmissionPlan>);

// A prepared draw section is the complete destination-independent part of a
// record's payload layout.  The source spans remain call-local; only their
// bounded schema facts cross into append.  In particular, payloadOffset is
// relative to the record payload (and includes the fixed draw header and
// section table), so writers can consume this layout without realigning or
// recounting the source a second time.
struct PeSemanticPreparedSection {
  std::uint16_t kind;
  std::uint16_t elementSize;
  std::uint32_t count;
  std::uint32_t payloadOffset;
  std::uint32_t byteSize;
};

struct PeSemanticPreparedSparseLayout {
  std::array<PeSemanticPreparedSection,
             D9C_COMMAND_CHUNK_SECTION_COUNT>
      sections;
  std::array<std::uint32_t, 17u> quotaDeltas;
  std::uint32_t sectionCount;
  std::uint32_t payloadBytes;
  std::uint32_t rectDelta;
  std::uint32_t semanticDelta;
  bool valid;

  PeSemanticPreparedSparseLayout() noexcept {}
  void reset() noexcept {
    sectionCount = 0u;
    payloadBytes = 0u;
    rectDelta = 0u;
    semanticDelta = 0u;
    valid = false;
    quotaDeltas.fill(0u);
  }
};

static_assert(std::is_trivially_copyable_v<PeSemanticPreparedSection>);
static_assert(std::is_trivially_copyable_v<PeSemanticPreparedSparseLayout>);

// The first seventeen canonical section kinds map one-to-one to the logical
// sparse quota categories. The prepared section inventory is the immutable
// active-set witness; constants and UP tails intentionally do not participate
// in sparse quota publication.
template <typename Visit>
  requires std::is_nothrow_invocable_r_v<bool, Visit&, std::size_t,
                                         std::uint32_t>
bool forEachPreparedSparseQuota(
    const PeSemanticPreparedSparseLayout& layout, Visit&& visit) noexcept {
  if (layout.sectionCount > layout.sections.size()) return false;
  for (std::size_t i = 0u; i < layout.sectionCount; ++i) {
    const auto kind = layout.sections[i].kind;
    if (kind < D9C_COMMAND_CHUNK_SECTION_RENDER_STATE ||
        kind > D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE)
      continue;
    const auto quota = static_cast<std::size_t>(
        kind - D9C_COMMAND_CHUNK_SECTION_RENDER_STATE);
    if (!visit(quota, layout.quotaDeltas[quota])) return false;
  }
  return true;
}

// One row per binding occurrence. Unlike the former owned binding arenas it
// carries no wire row copy: the source row is serialized directly while the
// admission-qualified pin (whose typed owner record carries kind, generation,
// and object identity) supplies the ownership witness and wire remap.
struct PeSemanticBindingLocator {
  // This is an active-prefix scratch record.  The planner writes every field
  // before advancing bindingOccurrences.count; inactive storage is never
  // initialized or consumed.
  std::uint32_t mappingOrdinal;
  std::uint32_t wireIndex;
  std::uint32_t occurrence;
  bool wireValid;
  bool hasPin;
};

static_assert(std::is_trivially_copyable_v<PeSemanticBindingLocator>);
static_assert(sizeof(PeSemanticBindingLocator) == 16u);

inline constexpr std::size_t kPeSemanticMaxBindingOccurrences =
    static_cast<std::size_t>(sectionRule(D9C_COMMAND_CHUNK_SECTION_TEXTURE)->maxCount) +
    static_cast<std::size_t>(sectionRule(D9C_COMMAND_CHUNK_SECTION_STREAM)->maxCount) +
    static_cast<std::size_t>(sectionRule(D9C_COMMAND_CHUNK_SECTION_SHADER)->maxCount) +
    static_cast<std::size_t>(sectionRule(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT)->maxCount) +
    static_cast<std::size_t>(sectionRule(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER)->maxCount) +
    static_cast<std::size_t>(sectionRule(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET)->maxCount) +
    static_cast<std::size_t>(sectionRule(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL)->maxCount);

struct PeSemanticBindingOccurrencePlan {
  std::array<PeSemanticBindingLocator,
             kPeSemanticMaxBindingOccurrences>
      values;
  std::array<PeSemanticArenaRange, 7u> ranges;
  std::size_t count = 0u;

  PeSemanticBindingOccurrencePlan() noexcept {}
  void reset() noexcept {
    ranges.fill({});
    count = 0u;
  }
};

static_assert(std::is_trivially_copyable_v<PeSemanticBindingOccurrencePlan>);

inline constexpr std::size_t kPeSemanticMaxRecordHandles = 64u;

// Standalone emission context retained for generic callers and differential
// tests. Prepared records use PeSemanticPreparedIdentityWitness below so the
// qualified identity is stored only once.
struct PeSemanticEmissionHandleContext {
  std::array<D9CWireObjectIdentity, kPeSemanticMaxRecordHandles> identities;
  std::size_t count = 0u;

  PeSemanticEmissionHandleContext() noexcept {}

  // Planning overwrites every identity below the active frontier through
  // appendKnownUnique(). Reset only that frontier: inactive identities are
  // never consumed, so clearing this roughly 1 KiB array would duplicate the
  // prepared-record value initialization on every admission.
  void resetFrontier() noexcept { count = 0u; }

  bool add(const D9CWireObjectIdentity& identity) noexcept {
    for (std::size_t i = 0u; i < count; ++i) {
      if (identities[i].kind == identity.kind &&
          identities[i].generation == identity.generation &&
          identities[i].objectId == identity.objectId) {
        return true;
      }
    }
    if (count == identities.size()) return false;
    identities[count++] = identity;
    return true;
  }

  // The caller may use this only after PeSemanticIdentitySet::add() has
  // reported `newlyWireVisible`: that proof establishes that this identity is
  // not already present in the wire witness.  Keep this path bounds-only so
  // admission does not linearly deduplicate a second time; a full witness
  // fails closed without writing past its fixed storage.
  bool appendKnownUnique(const D9CWireObjectIdentity& identity) noexcept {
    if (count >= identities.size()) return false;
    identities[count++] = identity;
    return true;
  }

};

static_assert(std::is_trivially_copyable_v<PeSemanticEmissionHandleContext>);
static_assert(sizeof(PeSemanticEmissionHandleContext) <=
              sizeof(D9CWireObjectIdentity) * kPeSemanticMaxRecordHandles +
                  std::max(sizeof(std::size_t),
                           alignof(D9CWireObjectIdentity)));

// The sole prepared-admission identity authority. Identity entries retain
// first-retain order; wireOrdinals records first-wire promotion order without
// copying identities. The active prefixes are bounded and all writes precede
// their frontier updates, so null/inactive tails remain untouched.
template <std::size_t MaxEntries>
struct PeSemanticPreparedIdentityWitnessT {
  static constexpr std::size_t entryCapacity = MaxEntries;
  struct Entry {
    D9CWireObjectIdentity identity;
    const void* object;
    std::uint32_t pin;
    std::uint32_t wireIndex;
  };

  std::array<Entry, MaxEntries> entries;
  std::array<std::uint32_t,
             std::min(MaxEntries, kPeSemanticMaxRecordHandles)> wireOrdinals;
  std::size_t count = 0u;
  std::size_t wireCount = 0u;

  PeSemanticPreparedIdentityWitnessT() noexcept {}

  static bool sameIdentity(const D9CWireObjectIdentity& left,
                           const D9CWireObjectIdentity& right) noexcept {
    return left.kind == right.kind && left.generation == right.generation &&
           left.objectId == right.objectId;
  }

  void reset() noexcept {
    count = 0u;
    wireCount = 0u;
  }

  bool add(const PeWireObjectRef& ref, std::uint32_t kind, bool wire,
           bool& newlyRetained, bool& newlyWireVisible,
           std::size_t* identityOrdinal = nullptr) noexcept {
    newlyRetained = false;
    newlyWireVisible = false;
    if (!ref.object) {
      return ref.identity.kind == 0u && ref.identity.generation == 0u &&
             ref.identity.objectId == 0u;
    }
    if (!ref.valid(kind)) return false;
    for (std::size_t i = 0u; i < count; ++i) {
      auto& entry = entries[i];
      if (entry.identity.kind == ref.identity.kind &&
          entry.identity.objectId == ref.identity.objectId) {
        if (entry.identity.generation != ref.identity.generation ||
            entry.object != ref.object) {
          return false;
        }
        if (wire && entry.wireIndex == kPeSemanticNoSlot) {
          if (wireCount >= wireOrdinals.size()) return false;
          wireOrdinals[wireCount] = static_cast<std::uint32_t>(i);
          entry.wireIndex = static_cast<std::uint32_t>(wireCount);
          ++wireCount;
          newlyWireVisible = true;
        }
        if (identityOrdinal) *identityOrdinal = i;
        return true;
      }
    }
    if (count >= entries.size() || (wire && wireCount >= wireOrdinals.size()))
      return false;
    entries[count] = {.identity = ref.identity,
                      .object = ref.object,
                      .pin = kPeSemanticNoSlot,
                      .wireIndex = kPeSemanticNoSlot};
    const auto ordinal = count;
    if (identityOrdinal) *identityOrdinal = ordinal;
    if (wire) {
      wireOrdinals[wireCount] = static_cast<std::uint32_t>(ordinal);
      entries[ordinal].wireIndex = static_cast<std::uint32_t>(wireCount);
      ++wireCount;
      newlyWireVisible = true;
    }
    ++count;
    newlyRetained = true;
    return true;
  }

  bool setPin(std::size_t identityOrdinal, std::uint32_t pin) noexcept {
    if (identityOrdinal >= count) return false;
    entries[identityOrdinal].pin = pin;
    return true;
  }

  Entry* findPin(const PeWireObjectRef& ref) noexcept {
    for (std::size_t i = 0u; i < count; ++i) {
      auto& entry = entries[i];
      if (sameIdentity(entry.identity, ref.identity) &&
          entry.object == ref.object)
        return &entry;
    }
    return nullptr;
  }

  const Entry* findPin(const PeWireObjectRef& ref) const noexcept {
    for (std::size_t i = 0u; i < count; ++i) {
      const auto& entry = entries[i];
      if (sameIdentity(entry.identity, ref.identity) &&
          entry.object == ref.object)
        return &entry;
    }
    return nullptr;
  }

  bool findWire(std::uint32_t kind, std::uint32_t pin,
                std::uint32_t& out) const noexcept {
    for (std::size_t i = 0u; i < count; ++i) {
      const auto& entry = entries[i];
      if (entry.identity.kind == kind && entry.pin == pin &&
          entry.wireIndex != kPeSemanticNoSlot) {
        out = entry.wireIndex;
        return true;
      }
    }
    return false;
  }

  const Entry* wireEntry(std::size_t wireOrdinal) const noexcept {
    if (wireOrdinal >= wireCount ||
        wireOrdinals[wireOrdinal] >= count)
      return nullptr;
    return &entries[wireOrdinals[wireOrdinal]];
  }

  const D9CWireObjectIdentity& wireIdentity(std::size_t wireOrdinal) const noexcept {
    return wireEntry(wireOrdinal)->identity;
  }
};

using PeSemanticPreparedIdentityWitness =
    PeSemanticPreparedIdentityWitnessT<80u>;
using PeSemanticCompactIdentityWitness =
    PeSemanticPreparedIdentityWitnessT<kPeSemanticMaxBindingOccurrences>;

static_assert(std::is_trivially_copyable_v<PeSemanticPreparedIdentityWitness>);
static_assert(std::is_trivially_copyable_v<PeSemanticCompactIdentityWitness>);
static_assert(sizeof(PeSemanticCompactIdentityWitness) <
              sizeof(PeSemanticPreparedIdentityWitness));

// Per-attempt facts that depend on the destination chunk.  The much larger
// PeSemanticRecordInput remains staged once and immutable; normal admission
// borrows it through PeSemanticRecordInputView and overlays only these values.
// Draws may point sparse at a destination-context projection owned by the
// recorder.  A CapacityPre rebase is the only path that rebuilds that bounded
// projection.
struct PeSemanticRecordDestinationContext {
  std::uint32_t recordType = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  const SparseStateInput* sparse = nullptr;
};

struct PeSemanticRecordInputView {
  const PeSemanticRecordInput* staged = nullptr;
  PeSemanticRecordDestinationContext destination{};

  bool valid() const noexcept {
    return staged != nullptr && destination.sparse != nullptr;
  }
};

inline PeSemanticRecordInputView borrowPeSemanticRecordInput(
    const PeSemanticRecordInput& input) noexcept {
  return {
      .staged = &input,
      .destination = {
          .recordType = input.recordType,
          .sourceOrdinal = input.sourceOrdinal,
          .recordOrdinal = input.recordOrdinal,
          .sparse = &input.sparse,
      },
  };
}

inline bool validPeSemanticInput(const PeSemanticRecordInput&) noexcept {
  return true;
}
inline bool validPeSemanticInput(const PeSemanticRecordInputView& input) noexcept {
  return input.valid();
}
inline const PeSemanticRecordInput& stagedPeSemanticInput(
    const PeSemanticRecordInput& input) noexcept {
  return input;
}
inline const PeSemanticRecordInput& stagedPeSemanticInput(
    const PeSemanticRecordInputView& input) noexcept {
  return *input.staged;
}
inline std::uint32_t peSemanticRecordType(
    const PeSemanticRecordInput& input) noexcept {
  return input.recordType;
}
inline std::uint32_t peSemanticRecordType(
    const PeSemanticRecordInputView& input) noexcept {
  return input.destination.recordType;
}
inline std::uint64_t peSemanticSourceOrdinal(
    const PeSemanticRecordInput& input) noexcept {
  return input.sourceOrdinal;
}
inline std::uint64_t peSemanticSourceOrdinal(
    const PeSemanticRecordInputView& input) noexcept {
  return input.destination.sourceOrdinal;
}
inline std::uint64_t peSemanticRecordOrdinal(
    const PeSemanticRecordInput& input) noexcept {
  return input.recordOrdinal;
}
inline std::uint64_t peSemanticRecordOrdinal(
    const PeSemanticRecordInputView& input) noexcept {
  return input.destination.recordOrdinal;
}
inline const SparseStateInput& peSemanticSparse(
    const PeSemanticRecordInput& input) noexcept {
  return input.sparse;
}
inline const SparseStateInput& peSemanticSparse(
    const PeSemanticRecordInputView& input) noexcept {
  return *input.destination.sparse;
}

static_assert(std::is_trivially_copyable_v<PeSemanticRecordDestinationContext>);
static_assert(std::is_trivially_copyable_v<PeSemanticRecordInputView>);
static_assert(sizeof(PeSemanticRecordDestinationContext) <= 32u);
static_assert(sizeof(PeSemanticRecordInputView) <= 40u);
static_assert(sizeof(PeSemanticRecordInputView) < sizeof(PeSemanticRecordInput));

template <typename Input>
concept PeSemanticRecordInputLike =
    std::same_as<std::remove_cvref_t<Input>, PeSemanticRecordInput> ||
    std::same_as<std::remove_cvref_t<Input>, PeSemanticRecordInputView>;

constexpr bool isPeSemanticConstantProducer(
    PeSemanticProducerKind producer) noexcept {
  return producer == PeSemanticProducerKind::VsFloatConstant ||
         producer == PeSemanticProducerKind::VsIntConstant ||
         producer == PeSemanticProducerKind::VsBoolConstant ||
         producer == PeSemanticProducerKind::PsFloatConstant ||
         producer == PeSemanticProducerKind::PsIntConstant ||
         producer == PeSemanticProducerKind::PsBoolConstant;
}

constexpr bool isPeSemanticSparseProducer(
    PeSemanticProducerKind producer) noexcept {
  return producer == PeSemanticProducerKind::DrawPrimitive ||
         producer == PeSemanticProducerKind::DrawIndexedPrimitive ||
         producer == PeSemanticProducerKind::DrawPrimitiveUp ||
         producer == PeSemanticProducerKind::DrawIndexedPrimitiveUp ||
         producer == PeSemanticProducerKind::ApplyState;
}

constexpr std::uint32_t peSemanticRecordTypeForProducer(
    PeSemanticProducerKind producer) noexcept {
  for (const auto& row : kPeSemanticProducerPolicyTable) {
    if (row.kind == producer) return row.recordType;
  }
  return 0u;
}

// Closed runtime-family view used by production constant staging.  Unlike the
// generic oracle envelope it has no authority for direct refs, sparse rows,
// UP data, or rectangle tails.
struct PeSemanticConstantRecordInput {
  PeSemanticProducerKind producer = PeSemanticProducerKind::VsFloatConstant;
  std::uint32_t recordType = 0u;
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  D9CCommandChunkWireSetConst setConst{};
  std::span<const std::byte> constantBytes{};
};

inline bool validPeSemanticInput(
    const PeSemanticConstantRecordInput&) noexcept {
  return true;
}
inline const PeSemanticConstantRecordInput& stagedPeSemanticInput(
    const PeSemanticConstantRecordInput& input) noexcept {
  return input;
}

// Call-local authority for the compact production sparse path. The recorder
// owns the noncopy plan; this view only couples it to the freshly revalidated
// lock capability and record ordinals for the duration of one owner call.
struct PeSemanticSparsePlanRecordInput {
  const RecorderLockCapability* access = nullptr;
  const SparseStatePlan* sparsePlan = nullptr;
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  std::uint32_t recordType = 0u;
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;

  const SparseStatePlan::SourceFacts* qualifiedSource() const noexcept {
    if (access == nullptr || sparsePlan == nullptr) return nullptr;
    const auto* source = sparsePlan->source(*access);
    return source != nullptr &&
                   sparsePlan->destination().chunkContextFinalized &&
                   source->draw.recordType == recordType
        ? source : nullptr;
  }

  bool valid() const noexcept { return qualifiedSource() != nullptr; }
};

inline bool validPeSemanticInput(
    const PeSemanticSparsePlanRecordInput& input) noexcept {
  return input.valid();
}
inline std::uint32_t peSemanticRecordType(
    const PeSemanticSparsePlanRecordInput& input) noexcept {
  return input.recordType;
}
inline std::uint64_t peSemanticSourceOrdinal(
    const PeSemanticSparsePlanRecordInput& input) noexcept {
  return input.sourceOrdinal;
}
inline std::uint64_t peSemanticRecordOrdinal(
    const PeSemanticSparsePlanRecordInput& input) noexcept {
  return input.recordOrdinal;
}

inline PeSemanticProducerKind peSemanticProducer(
    const PeSemanticRecordInput& input) noexcept {
  return input.producer;
}
inline PeSemanticProducerKind peSemanticProducer(
    const PeSemanticRecordInputView& input) noexcept {
  return input.staged->producer;
}
inline PeSemanticProducerKind peSemanticProducer(
    const PeSemanticConstantRecordInput& input) noexcept {
  return input.producer;
}
inline PeSemanticProducerKind peSemanticProducer(
    const PeSemanticSparsePlanRecordInput& input) noexcept {
  return input.producer;
}
inline std::uint32_t peSemanticRecordType(
    const PeSemanticConstantRecordInput& input) noexcept {
  return input.recordType;
}
inline std::uint64_t peSemanticSourceOrdinal(
    const PeSemanticConstantRecordInput& input) noexcept {
  return input.sourceOrdinal;
}
inline std::uint64_t peSemanticRecordOrdinal(
    const PeSemanticConstantRecordInput& input) noexcept {
  return input.recordOrdinal;
}

template <typename Input>
concept PeSemanticConstantInputLike =
    std::same_as<std::remove_cvref_t<Input>, PeSemanticConstantRecordInput>;

template <typename Input>
concept PeSemanticSparsePlanInputLike = std::same_as<
    std::remove_cvref_t<Input>, PeSemanticSparsePlanRecordInput>;

template <typename Input>
constexpr PeSemanticOwnerSettlementFamily peSemanticOwnerSettlementFamily(
    const Input& input) noexcept {
  if constexpr (PeSemanticSparsePlanInputLike<Input>) {
    switch (peSemanticProducer(input)) {
    case PeSemanticProducerKind::DrawPrimitive:
      return PeSemanticOwnerSettlementFamily::SparseDraw;
    case PeSemanticProducerKind::DrawIndexedPrimitive:
      return PeSemanticOwnerSettlementFamily::SparseIndexedDraw;
    case PeSemanticProducerKind::ApplyState:
      return PeSemanticOwnerSettlementFamily::SparseApply;
    default:
      break;
    }
  } else if constexpr (PeSemanticConstantInputLike<Input>) {
    return PeSemanticOwnerSettlementFamily::Constant;
  }
  return PeSemanticOwnerSettlementFamily::OtherGeneric;
}

template <typename Input>
constexpr PeSemanticOwnerAdmissionFamily peSemanticOwnerAdmissionFamily(
    const Input&) noexcept {
  if constexpr (PeSemanticSparsePlanInputLike<Input>)
    return PeSemanticOwnerAdmissionFamily::Compact;
  else if constexpr (PeSemanticConstantInputLike<Input>)
    return PeSemanticOwnerAdmissionFamily::Constant;
  else
    return PeSemanticOwnerAdmissionFamily::Generic;
}

template <typename Input>
concept PeSemanticOwnedRecordInputLike = PeSemanticRecordInputLike<Input> ||
    PeSemanticConstantInputLike<Input> || PeSemanticSparsePlanInputLike<Input>;

// Typed standalone-constant input. Its closed shape proves absence of direct
// resources, sparse rows, and UP/rectangle tails, allowing constant admission
// to skip those families without changing the generic oracle envelope.
template <PeSemanticProducerKind Producer>
  requires (isPeSemanticConstantProducer(Producer))
struct PeSemanticTypedConstantInput {
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  D9CCommandChunkWireSetConst setConst{};
  std::span<const std::byte> constantBytes{};

  PeSemanticConstantRecordInput asConstant() const noexcept {
    PeSemanticConstantRecordInput out{};
    out.producer = Producer;
    out.recordType = peSemanticRecordTypeForProducer(Producer);
    out.recordFlags = recordFlags;
    out.sourceOrdinal = sourceOrdinal;
    out.recordOrdinal = recordOrdinal;
    out.setConst = setConst;
    out.constantBytes = constantBytes;
    return out;
  }
};

static_assert(std::is_trivially_copyable_v<
              PeSemanticTypedConstantInput<PeSemanticProducerKind::VsFloatConstant>>);
static_assert(std::is_trivially_copyable_v<PeSemanticConstantRecordInput>);
static_assert(sizeof(PeSemanticConstantRecordInput) < sizeof(PeSemanticRecordInput));

namespace detail {

inline bool checkedSizeToU32(std::size_t value, std::uint32_t& out) noexcept {
  if (value > std::numeric_limits<std::uint32_t>::max()) return false;
  out = static_cast<std::uint32_t>(value);
  return true;
}

inline bool checkedSizeAdd(std::size_t lhs, std::size_t rhs,
                           std::size_t& out) noexcept {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) return false;
  out = lhs + rhs;
  return true;
}

inline bool checkedSizeMultiply(std::size_t lhs, std::size_t rhs,
                                std::size_t& out) noexcept {
  if (lhs != 0u && rhs > std::numeric_limits<std::size_t>::max() / lhs)
    return false;
  out = lhs * rhs;
  return true;
}

inline bool checkedAlign(std::size_t value, std::size_t alignment,
                         std::size_t& out) noexcept {
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
      value > std::numeric_limits<std::size_t>::max() - (alignment - 1u))
    return false;
  out = (value + alignment - 1u) & ~(alignment - 1u);
  return true;
}

struct PeSemanticIdentitySet {
  // A record has at most 64 wire-visible handles; the extra slots cover the
  // direct typed pins retained by the owner but intentionally omitted from
  // the record-local wire handle table.  Wire visibility is a per-entry flag
  // rather than a second set: one walk then answers the retention question and
  // the final-wire handle count together, so admission and emission cannot
  // disagree about which identities a record carries.
  // The arrays deliberately have no member initializers.  Admission only
  // inspects the active [0, count) frontier, and the append below writes every
  // field of a new entry before publishing count; value-initializing the
  // inactive arrays would otherwise clear 1,680 bytes per prepare on x86.
  std::array<D9CWireObjectIdentity, 80u> values;
  std::array<const void*, 80u> objects;
  std::array<bool, 80u> wireVisible;
  std::size_t count;
  std::size_t wireCount;

  PeSemanticIdentitySet() noexcept : count(0u), wireCount(0u) {}
  PeSemanticIdentitySet(const PeSemanticIdentitySet&) = delete;
  PeSemanticIdentitySet& operator=(const PeSemanticIdentitySet&) = delete;
  PeSemanticIdentitySet(PeSemanticIdentitySet&&) = delete;
  PeSemanticIdentitySet& operator=(PeSemanticIdentitySet&&) = delete;

  // `newlyRetained` reports a newly seen identity that owns an object, while
  // `newlyWireVisible` reports the first encounter that makes it part of the
  // record-local wire table.  The latter is separate because a direct pin is
  // retained before a later sparse binding can promote the same identity.
  bool add(const PeWireObjectRef& ref, std::uint32_t kind, bool wire,
           bool& newlyRetained, bool& newlyWireVisible,
           std::size_t* identityOrdinal = nullptr) noexcept {
    newlyRetained = false;
    newlyWireVisible = false;
    if (!ref.object) {
      return ref.identity.kind == 0u && ref.identity.generation == 0u &&
             ref.identity.objectId == 0u;
    }
    if (!ref.valid(kind)) return false;
    for (std::size_t i = 0u; i < count; ++i) {
      if (values[i].kind == ref.identity.kind &&
          values[i].objectId == ref.identity.objectId) {
        if (values[i].generation != ref.identity.generation ||
            objects[i] != ref.object) {
          return false;
        }
        if (wire && !wireVisible[i]) {
          wireVisible[i] = true;
          ++wireCount;
          newlyWireVisible = true;
        }
        if (identityOrdinal) *identityOrdinal = i;
        return true;
      }
    }
    if (count == values.size()) return false;
    // Publish count only after all active identity/object/visibility fields
    // are fully written; every scan above is confined to the active frontier.
    values[count] = ref.identity;
    objects[count] = ref.object;
    wireVisible[count] = wire;
    if (identityOrdinal) *identityOrdinal = count;
    if (wire) {
      ++wireCount;
      newlyWireVisible = true;
    }
    ++count;
    newlyRetained = true;
    return true;
  }
};

template <PeSemanticRecordInputLike Input>
inline bool preparePeSemanticSparseLayout(
    const Input& input, PeSemanticPreparedSparseLayout& out) noexcept {
  out.reset();
  if (!validPeSemanticInput(input)) return false;
  const auto& staged = stagedPeSemanticInput(input);
  if (!isPeSemanticSparseProducer(staged.producer)) {
    std::size_t payloadBytes = 0u;
    switch (staged.producer) {
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        if (!checkedSizeAdd(sizeof(D9CCommandChunkWireSetConst),
                            staged.constantBytes.size(), payloadBytes)) return false;
        break;
      case PeSemanticProducerKind::Clear: {
        std::size_t rectBytes = 0u;
        if (!checkedSizeMultiply(staged.clearRects.size(), sizeof(D9CRect),
                                 rectBytes) ||
            !checkedSizeAdd(sizeof(D9CCommandChunkWireClear), rectBytes,
                            payloadBytes)) return false;
        break;
      }
      case PeSemanticProducerKind::Present:
        payloadBytes = sizeof(staged.present);
        break;
      case PeSemanticProducerKind::StretchRect:
        payloadBytes = sizeof(staged.stretchRect);
        break;
      case PeSemanticProducerKind::ColorFill:
        payloadBytes = sizeof(staged.colorFill);
        break;
      case PeSemanticProducerKind::UpdateTexture:
        payloadBytes = sizeof(D9CCommandChunkWireUpdateTexture);
        break;
      case PeSemanticProducerKind::UpdateSurface:
        payloadBytes = sizeof(staged.updateSurface);
        break;
      case PeSemanticProducerKind::QueryIssue:
        payloadBytes = sizeof(staged.queryIssue);
        break;
      case PeSemanticProducerKind::Readback:
        payloadBytes = sizeof(D9CCommandChunkWireReadback);
        break;
      case PeSemanticProducerKind::ReszDepthResolve:
        payloadBytes = sizeof(D9CCommandChunkWireReszDepthResolve);
        break;
      case PeSemanticProducerKind::GenerateMipmaps:
        payloadBytes = sizeof(D9CCommandChunkWireGenerateMipmaps);
        break;
      default:
        return false;
    }
    if (!checkedSizeToU32(payloadBytes, out.payloadBytes)) return false;
    out.valid = true;
    return true;
  }
  const auto& s = peSemanticSparse(input);
  const std::size_t activeSections =
      !s.renderStates.empty() + !s.textures.empty() + !s.streams.empty() +
      !s.shaders.empty() + !s.vertexInputs.empty() + !s.indexBuffers.empty() +
      !s.renderTargets.empty() + !s.depthStencils.empty() + !s.viewports.empty() +
      !s.scissors.empty() + !s.materials.empty() + !s.clipPlanes.empty() +
      !s.textureStageStates.empty() + !s.samplerStates.empty() +
      !s.transforms.empty() + !s.lights.empty() + !s.lightEnables.empty() +
      s.vsFloatConstants.present() + s.vsIntConstants.present() +
      s.vsBoolConstants.present() + s.psFloatConstants.present() +
      s.psIntConstants.present() + s.psBoolConstants.present() +
      !s.upIndexData.empty() + !s.upVertexData.empty();
  if (activeSections > D9C_COMMAND_CHUNK_SECTION_COUNT) return false;
  std::size_t cursor = sizeof(D9CCommandChunkWireDrawHeader);
  std::size_t tableBytes = 0u;
  if (!checkedSizeMultiply(activeSections,
                           sizeof(D9CCommandChunkWireSectionDesc), tableBytes) ||
      !checkedSizeAdd(cursor, tableBytes, cursor)) {
    return false;
  }
  const auto append = [&](std::uint16_t kind, std::size_t count,
                          std::size_t bytes, std::size_t quota) noexcept {
    if (count == 0u) return true;
    if (out.sectionCount >= out.sections.size() ||
        (quota != std::numeric_limits<std::size_t>::max() &&
         quota >= out.quotaDeltas.size()))
      return false;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    std::uint32_t boundedCount = 0u;
    std::uint32_t boundedBytes = 0u;
    std::uint32_t boundedOffset = 0u;
    if (!rule || count > rule->maxCount ||
        !checkedSizeToU32(count, boundedCount) ||
        !checkedSizeToU32(bytes, boundedBytes) ||
        !checkedAlign(cursor, rule->payloadAlignment, aligned) ||
        !checkedSizeToU32(aligned, boundedOffset) ||
        bytes > std::numeric_limits<std::size_t>::max() - aligned) {
      return false;
    }
    out.sections[out.sectionCount++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = boundedCount,
        .payloadOffset = boundedOffset,
        .byteSize = boundedBytes,
    };
    if (quota != std::numeric_limits<std::size_t>::max())
      out.quotaDeltas[quota] = boundedCount;
    cursor = aligned + bytes;
    return true;
  };
  const auto typed = [&](std::uint16_t kind, std::size_t count,
                         std::size_t element, std::size_t quota) noexcept {
    std::size_t bytes = 0u;
    return checkedSizeMultiply(count, element, bytes) &&
           append(kind, count, bytes, quota);
  };
  const auto raw = [&](std::uint16_t kind, std::span<const std::byte> bytes,
                       std::size_t quota) noexcept {
    return append(kind, bytes.size(), bytes.size(), quota);
  };
  const auto constant = [&](std::uint16_t kind,
                            const SparseConstantRangeInput& range,
                            std::size_t quota) noexcept {
    if (!range.present()) return true;
    const auto* rule = sectionRule(kind);
    std::size_t registerBytes = 0u;
    std::size_t bytes = 0u;
    if (!rule || range.registerCount > rule->maxCount ||
        static_cast<std::uint64_t>(range.startRegister) +
                range.registerCount > rule->maxCount ||
        !checkedSizeMultiply(static_cast<std::size_t>(range.registerCount),
                             rule->elementSize, registerBytes) ||
        range.registerBytes.size() != registerBytes ||
        !checkedSizeAdd(sizeof(D9CCommandChunkWireConstantRange),
                        registerBytes, bytes)) {
      return false;
    }
    return append(kind, range.registerCount, bytes, quota);
  };
  if (!typed(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE, s.renderStates.size(),
             sizeof(D9CCommandChunkWireRenderState), 0u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_TEXTURE, s.textures.size(),
             sizeof(D9CCommandChunkWireTextureBinding), 1u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_STREAM, s.streams.size(),
             sizeof(D9CCommandChunkWireStreamBinding), 2u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_SHADER, s.shaders.size(),
             sizeof(D9CCommandChunkWireShaderBinding), 3u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT, s.vertexInputs.size(),
             sizeof(D9CCommandChunkWireVertexInput), 4u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER, s.indexBuffers.size(),
             sizeof(D9CCommandChunkWireIndexBinding), 5u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, s.renderTargets.size(),
             sizeof(D9CCommandChunkWireRenderTargetBinding), 6u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, s.depthStencils.size(),
             sizeof(D9CCommandChunkWireDepthStencilBinding), 7u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_VIEWPORT, s.viewports.size(),
             sizeof(D9CViewport), 8u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_SCISSOR, s.scissors.size(),
             sizeof(D9CRect), 9u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_MATERIAL, s.materials.size(),
             sizeof(D9CMaterial), 10u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE, s.clipPlanes.size(),
             sizeof(D9CCommandChunkWireClipPlane), 11u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
             s.textureStageStates.size(),
             sizeof(D9CDrawPacketTextureStageState), 12u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE, s.samplerStates.size(),
             sizeof(D9CDrawPacketSamplerState), 13u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_TRANSFORM, s.transforms.size(),
             sizeof(D9CDrawPacketTransform), 14u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_LIGHT, s.lights.size(),
             sizeof(D9CCommandChunkWireLight), 15u) ||
      !typed(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE, s.lightEnables.size(),
             sizeof(D9CCommandChunkWireLightEnable), 16u) ||
      !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F, s.vsFloatConstants,
                std::numeric_limits<std::size_t>::max()) ||
      !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I, s.vsIntConstants,
                std::numeric_limits<std::size_t>::max()) ||
      !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B, s.vsBoolConstants,
                std::numeric_limits<std::size_t>::max()) ||
      !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F, s.psFloatConstants,
                std::numeric_limits<std::size_t>::max()) ||
      !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I, s.psIntConstants,
                std::numeric_limits<std::size_t>::max()) ||
      !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B, s.psBoolConstants,
                std::numeric_limits<std::size_t>::max()) ||
      !raw(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA, s.upIndexData,
           std::numeric_limits<std::size_t>::max()) ||
      !raw(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA, s.upVertexData,
           std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  if (!checkedSizeToU32(cursor, out.payloadBytes)) return false;
  out.semanticDelta = 0u;
  const auto addSemantic = [&](std::size_t bytes) noexcept {
    if (bytes > std::numeric_limits<std::uint32_t>::max() -
                   out.semanticDelta)
      return false;
    out.semanticDelta += static_cast<std::uint32_t>(bytes);
    return true;
  };
  if (!addSemantic(s.vsFloatConstants.registerBytes.size()) ||
      !addSemantic(s.vsIntConstants.registerBytes.size()) ||
      !addSemantic(s.vsBoolConstants.registerBytes.size()) ||
      !addSemantic(s.psFloatConstants.registerBytes.size()) ||
      !addSemantic(s.psIntConstants.registerBytes.size()) ||
      !addSemantic(s.psBoolConstants.registerBytes.size()) ||
      !addSemantic(s.upIndexData.size()) || !addSemantic(s.upVertexData.size()))
    return false;
  out.valid = true;
  return true;
}

inline bool preparePeSemanticSparsePlanLayout(
    const SparseStatePlan::SourceFacts& source,
    const SparseStatePlan::DestinationFacts& destination,
    PeSemanticPreparedSparseLayout& out) noexcept {
  static_assert([] {
    for (const auto& rule : kSectionRules) {
      if (rule.payloadAlignment == 0u ||
          sizeof(D9CCommandChunkWireDrawHeader) % rule.payloadAlignment != 0u ||
          sizeof(D9CCommandChunkWireSectionDesc) % rule.payloadAlignment != 0u)
        return false;
    }
    return true;
  }(), "sparse layout bias must preserve every section alignment");
  out.reset();
  std::size_t cursor = 0u;
  const auto append = [&](std::uint16_t kind, std::size_t count,
                          std::size_t bytes, std::size_t quota) noexcept {
    if (count == 0u) return true;
    if (out.sectionCount >= D9C_COMMAND_CHUNK_SECTION_COUNT ||
        out.sectionCount >= out.sections.size() ||
        (quota != std::numeric_limits<std::size_t>::max() &&
         quota >= out.quotaDeltas.size())) return false;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    std::uint32_t boundedCount = 0u;
    std::uint32_t boundedBytes = 0u;
    std::uint32_t boundedOffset = 0u;
    if (!rule || count > rule->maxCount ||
        !checkedSizeToU32(count, boundedCount) ||
        !checkedSizeToU32(bytes, boundedBytes) ||
        !checkedAlign(cursor, rule->payloadAlignment, aligned) ||
        !checkedSizeToU32(aligned, boundedOffset) ||
        bytes > std::numeric_limits<std::size_t>::max() - aligned) return false;
    out.sections[out.sectionCount++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = boundedCount,
        .payloadOffset = boundedOffset,
        .byteSize = boundedBytes,
    };
    if (quota != std::numeric_limits<std::size_t>::max())
      out.quotaDeltas[quota] = boundedCount;
    cursor = aligned + bytes;
    return true;
  };
  const auto typed = [&](std::uint16_t kind, std::size_t count,
                         std::size_t element, std::size_t quota) noexcept {
    std::size_t bytes = 0u;
    return checkedSizeMultiply(count, element, bytes) &&
           append(kind, count, bytes, quota);
  };
  const auto constant = [&](std::uint16_t kind,
                            const SparseConstantRangeInput& range) noexcept {
    if (!range.present())
      return range.startRegister == 0u && range.registerBytes.empty();
    const auto* rule = sectionRule(kind);
    std::size_t registerBytes = 0u;
    std::size_t bytes = 0u;
    if (!rule || range.registerCount > rule->maxCount ||
        !checkedSizeMultiply(range.registerCount, rule->elementSize,
                             registerBytes) ||
        range.registerBytes.size() != registerBytes ||
        !checkedSizeAdd(sizeof(D9CCommandChunkWireConstantRange),
                        registerBytes, bytes)) return false;
    return append(kind, range.registerCount, bytes,
                  std::numeric_limits<std::size_t>::max());
  };
  if ((source.renderStateCount != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE, source.renderStateCount,
              sizeof(D9CCommandChunkWireRenderState), 0u)) ||
      (source.textureMask != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_TEXTURE,
              std::popcount(source.textureMask),
              sizeof(D9CCommandChunkWireTextureBinding), 1u)) ||
      (destination.streamMask != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_STREAM,
              std::popcount(destination.streamMask),
              sizeof(D9CCommandChunkWireStreamBinding), 2u)) ||
      (source.shaderMask != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_SHADER, std::popcount(source.shaderMask),
              sizeof(D9CCommandChunkWireShaderBinding), 3u)) ||
      (source.vertexInput &&
       !typed(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT, 1u,
              sizeof(D9CCommandChunkWireVertexInput), 4u)) ||
      (destination.indexBuffer &&
       !typed(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER, 1u,
              sizeof(D9CCommandChunkWireIndexBinding), 5u)) ||
      (source.renderTargetMask != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
              std::popcount(source.renderTargetMask),
              sizeof(D9CCommandChunkWireRenderTargetBinding), 6u)) ||
      (source.depthStencil &&
       !typed(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, 1u,
              sizeof(D9CCommandChunkWireDepthStencilBinding), 7u)) ||
      (source.viewport &&
       !typed(D9C_COMMAND_CHUNK_SECTION_VIEWPORT, 1u,
              sizeof(D9CViewport), 8u)) ||
      (source.scissor &&
       !typed(D9C_COMMAND_CHUNK_SECTION_SCISSOR, 1u, sizeof(D9CRect), 9u)) ||
      (source.material &&
       !typed(D9C_COMMAND_CHUNK_SECTION_MATERIAL, 1u,
              sizeof(D9CMaterial), 10u)) ||
      (source.clipPlaneMask != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE,
              std::popcount(source.clipPlaneMask),
              sizeof(D9CCommandChunkWireClipPlane), 11u)) ||
      (source.textureStageStateCount != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
              source.textureStageStateCount,
              sizeof(D9CDrawPacketTextureStageState), 12u)) ||
      (source.samplerStateCount != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
              source.samplerStateCount, sizeof(D9CDrawPacketSamplerState), 13u)) ||
      (source.transformCount != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_TRANSFORM, source.transformCount,
              sizeof(D9CDrawPacketTransform), 14u)) ||
      (source.lightMask != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_LIGHT, std::popcount(source.lightMask),
              sizeof(D9CCommandChunkWireLight), 15u)) ||
      (source.lightEnableMask != 0u &&
       !typed(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
              std::popcount(source.lightEnableMask),
              sizeof(D9CCommandChunkWireLightEnable), 16u)) ||
      (source.vsFloatConstants.present()
           ? !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F,
                       source.vsFloatConstants)
           : (source.vsFloatConstants.startRegister != 0u ||
              !source.vsFloatConstants.registerBytes.empty())) ||
      (source.vsIntConstants.present()
           ? !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I,
                       source.vsIntConstants)
           : (source.vsIntConstants.startRegister != 0u ||
              !source.vsIntConstants.registerBytes.empty())) ||
      (source.vsBoolConstants.present()
           ? !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B,
                       source.vsBoolConstants)
           : (source.vsBoolConstants.startRegister != 0u ||
              !source.vsBoolConstants.registerBytes.empty())) ||
      (source.psFloatConstants.present()
           ? !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F,
                       source.psFloatConstants)
           : (source.psFloatConstants.startRegister != 0u ||
              !source.psFloatConstants.registerBytes.empty())) ||
      (source.psIntConstants.present()
           ? !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I,
                       source.psIntConstants)
           : (source.psIntConstants.startRegister != 0u ||
              !source.psIntConstants.registerBytes.empty())) ||
      (source.psBoolConstants.present()
           ? !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B,
                       source.psBoolConstants)
           : (source.psBoolConstants.startRegister != 0u ||
              !source.psBoolConstants.registerBytes.empty())) ||
      (!source.payloads.upIndex.empty() &&
       !append(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA,
               source.payloads.upIndex.size(), source.payloads.upIndex.size(),
               std::numeric_limits<std::size_t>::max())) ||
      (!source.payloads.upVertex.empty() &&
       !append(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
               source.payloads.upVertex.size(), source.payloads.upVertex.size(),
               std::numeric_limits<std::size_t>::max()))) return false;
  std::size_t tableBytes = 0u;
  std::size_t bias = 0u;
  std::size_t payloadBytes = 0u;
  if (!checkedSizeMultiply(static_cast<std::size_t>(out.sectionCount),
                           sizeof(D9CCommandChunkWireSectionDesc), tableBytes) ||
      !checkedSizeAdd(sizeof(D9CCommandChunkWireDrawHeader), tableBytes, bias) ||
      !checkedSizeAdd(cursor, bias, payloadBytes) ||
      !checkedSizeToU32(payloadBytes, out.payloadBytes))
    return false;
  for (std::size_t i = 0u; i < out.sectionCount; ++i) {
    std::size_t shiftedOffset = 0u;
    if (!checkedSizeAdd(out.sections[i].payloadOffset, bias, shiftedOffset) ||
        !checkedSizeToU32(shiftedOffset, out.sections[i].payloadOffset))
      return false;
  }
  const auto addSemantic = [&](std::size_t bytes) noexcept {
    if (bytes > std::numeric_limits<std::uint32_t>::max() -
                    out.semanticDelta) return false;
    out.semanticDelta += static_cast<std::uint32_t>(bytes);
    return true;
  };
  if (!addSemantic(source.vsFloatConstants.registerBytes.size()) ||
      !addSemantic(source.vsIntConstants.registerBytes.size()) ||
      !addSemantic(source.vsBoolConstants.registerBytes.size()) ||
      !addSemantic(source.psFloatConstants.registerBytes.size()) ||
      !addSemantic(source.psIntConstants.registerBytes.size()) ||
      !addSemantic(source.psBoolConstants.registerBytes.size()) ||
      !addSemantic(source.payloads.upIndex.size()) ||
      !addSemantic(source.payloads.upVertex.size())) return false;
  out.valid = true;
  return true;
}

}  // namespace detail

// Producer role facts. They are pure functions of the producer kind, so the
// prepared witness resolves them once instead of re-deriving the same boolean
// chains inside header validation.
constexpr bool isPeSemanticUpProducer(
    PeSemanticProducerKind producer) noexcept {
  return producer == PeSemanticProducerKind::DrawPrimitiveUp ||
         producer == PeSemanticProducerKind::DrawIndexedPrimitiveUp;
}

// One identity walk over the immutable input.  `onUniqueRetained` is invoked
// exactly once per newly seen retained identity that owns an object, in
// admission order, with the owner-facing pin kind (0 surface, 1 texture,
// 2 buffer, 3 shader, 4 declaration, 5 query).  An observer that returns false
// rejects the whole plan, which is how the owner folds its retention-delta and
// generation-alias proof into this same pass instead of walking the record a
// second time.
// The public oracle keeps the optional unique-count projection by default;
// production preparation passes false so retention deltas remain the only
// owner-facing result for that question.
template <bool ProjectUniquePinCounts = true, PeSemanticRecordInputLike Input,
          typename AdmissionPlanT, typename OnUniqueRetained,
          typename IdentityWitness>
  requires (std::is_nothrow_invocable_r_v<bool, OnUniqueRetained&,
                                          const PeWireObjectRef&, std::size_t> ||
            std::is_nothrow_invocable_r_v<bool, OnUniqueRetained&,
                                          const PeWireObjectRef&, std::size_t,
                                          std::size_t>)
inline bool planPeSemanticAdmissionWithIdentity(
    const Input& input, AdmissionPlanT& out,
    OnUniqueRetained&& onUniqueRetained,
    IdentityWitness& identityWitness,
    PeSemanticEmissionHandleContext* wireWitness = nullptr,
    PeSemanticPreparedSparseLayout* sparseLayoutWitness = nullptr,
    PeSemanticBindingOccurrencePlan* bindingWitness = nullptr) noexcept {
  out = {};
  if (wireWitness != nullptr) wireWitness->resetFrontier();
  identityWitness.reset();
  if (sparseLayoutWitness != nullptr) sparseLayoutWitness->reset();
  if (bindingWitness != nullptr) bindingWitness->reset();
  if (!validPeSemanticInput(input)) return false;
  const auto& staged = stagedPeSemanticInput(input);
  PeSemanticPreparedSparseLayout localSparseLayout;
  auto* const sparseLayout = sparseLayoutWitness != nullptr
                                 ? sparseLayoutWitness
                                 : &localSparseLayout;
  if (!detail::preparePeSemanticSparseLayout(input, *sparseLayout)) return false;
  out.recordType = peSemanticRecordType(input);
  // The caller supplies the sole active-prefix identity authority. Generic
  // wrappers may mirror it into standalone differential witnesses, but this
  // helper never carries a second identity table.
  auto& identities = identityWitness;
  auto&& observe = onUniqueRetained;
  const auto retained = [&](const PeWireObjectRef& ref, std::uint32_t kind,
                            std::size_t pinKind, bool wire,
                            std::size_t* mappingOrdinal = nullptr) noexcept {
    bool newlyRetained = false;
    bool newlyWireVisible = false;
    std::size_t identityOrdinal = 0u;
    if (!identities.add(ref, kind, wire, newlyRetained, newlyWireVisible,
                         &identityOrdinal)) return false;
    if (newlyWireVisible && wireWitness != nullptr &&
        !wireWitness->appendKnownUnique(ref.identity)) return false;
    if (newlyRetained) {
      if constexpr (ProjectUniquePinCounts) {
        if (out.uniquePinCounts[pinKind] ==
            std::numeric_limits<std::uint32_t>::max()) return false;
        ++out.uniquePinCounts[pinKind];
      }
      if constexpr (std::is_nothrow_invocable_r_v<
                        bool, decltype(observe)&, const PeWireObjectRef&,
                        std::size_t, std::size_t>) {
        if (!observe(ref, pinKind, identityOrdinal)) return false;
      } else {
        if (!observe(ref, pinKind)) return false;
      }
    }
    if (mappingOrdinal != nullptr) *mappingOrdinal = identityOrdinal;
    // Identity insertion and callback publication share first-retain order;
    // the combined witness records wire promotion directly in wireOrdinals.
    return true;
  };
  // Every direct slot is already retained above, so this ordinarily only
  // raises the wire-visible flag. It still routes through `retained` so a
  // future producer that exposes a reference outside the ten direct slots
  // cannot become wire-visible without also being counted and retained.
  const auto direct = [&](const PeWireObjectRef& ref, std::uint32_t kind,
                          std::size_t pinKind) noexcept {
    return retained(ref, kind, pinKind, true);
  };
  if (!retained(staged.surface0, D9C_CHUNK_HANDLE_KIND_SURFACE, 0u, false) ||
      !retained(staged.surface1, D9C_CHUNK_HANDLE_KIND_SURFACE, 0u, false) ||
      !retained(staged.texture0, D9C_CHUNK_HANDLE_KIND_TEXTURE, 1u, false) ||
      !retained(staged.texture1, D9C_CHUNK_HANDLE_KIND_TEXTURE, 1u, false) ||
      !retained(staged.buffer0, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u, false) ||
      !retained(staged.buffer1, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u, false) ||
      !retained(staged.shader0, D9C_CHUNK_HANDLE_KIND_SHADER, 3u, false) ||
      !retained(staged.shader1, D9C_CHUNK_HANDLE_KIND_SHADER, 3u, false) ||
      !retained(staged.declaration, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, 4u,
                false) ||
      !retained(staged.query, D9C_CHUNK_HANDLE_KIND_QUERY, 5u, false)) {
    return false;
  }
  bool directValid = true;
  switch (staged.producer) {
    case PeSemanticProducerKind::Present:
    case PeSemanticProducerKind::ColorFill:
      directValid = direct(staged.surface0, D9C_CHUNK_HANDLE_KIND_SURFACE, 0u);
      break;
    case PeSemanticProducerKind::GenerateMipmaps:
      directValid = direct(staged.texture0, D9C_CHUNK_HANDLE_KIND_TEXTURE, 1u);
      break;
    case PeSemanticProducerKind::StretchRect:
    case PeSemanticProducerKind::UpdateSurface:
    case PeSemanticProducerKind::Readback:
      directValid = direct(staged.surface0, D9C_CHUNK_HANDLE_KIND_SURFACE, 0u) &&
                    direct(staged.surface1, D9C_CHUNK_HANDLE_KIND_SURFACE, 0u);
      break;
    case PeSemanticProducerKind::UpdateTexture:
      directValid = direct(staged.texture0, D9C_CHUNK_HANDLE_KIND_TEXTURE, 1u) &&
                    direct(staged.texture1, D9C_CHUNK_HANDLE_KIND_TEXTURE, 1u);
      break;
    case PeSemanticProducerKind::QueryIssue:
      directValid = direct(staged.query, D9C_CHUNK_HANDLE_KIND_QUERY, 5u);
      break;
    case PeSemanticProducerKind::ReszDepthResolve:
      directValid = direct(staged.surface0, D9C_CHUNK_HANDLE_KIND_SURFACE, 0u) &&
                    direct(staged.texture0, D9C_CHUNK_HANDLE_KIND_TEXTURE, 1u);
      break;
    default:
      break;
  }
  if (!directValid) return false;
  const auto sparse = [&](auto rows, std::uint32_t kind, std::size_t pinKind,
                          std::size_t sparseKind) noexcept {
    std::uint32_t count = 0u;
    if (!detail::checkedSizeToU32(rows.size(), count)) return false;
    out.sparseCounts[sparseKind] = count;
    std::size_t occurrence = 0u;
    if (bindingWitness != nullptr) {
      if (sparseKind == 0u || sparseKind > bindingWitness->ranges.size())
        return false;
      bindingWitness->ranges[sparseKind - 1u] = {
          .offset = static_cast<std::uint32_t>(bindingWitness->count),
          .count = count};
    }
    for (const auto& row : rows) {
      std::size_t mappingOrdinal = kPeSemanticNoSlot;
      if (row.wire.valid) {
        if (!retained(row.object, kind, pinKind, true,
                      row.object.object != nullptr
                          ? &mappingOrdinal
                          : nullptr)) return false;
        if (mappingOrdinal != kPeSemanticNoSlot &&
            mappingOrdinal >= identities.count) return false;
      }
      if (bindingWitness != nullptr) {
        if (bindingWitness->count >= bindingWitness->values.size() ||
            occurrence > std::numeric_limits<std::uint32_t>::max()) return false;
        const auto localWireIndex =
            mappingOrdinal == kPeSemanticNoSlot
                ? D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX
                : identities.entries[mappingOrdinal].wireIndex;
        bindingWitness->values[bindingWitness->count++] = {
            .mappingOrdinal = mappingOrdinal == kPeSemanticNoSlot
                                  ? kPeSemanticNoSlot
                                  : static_cast<std::uint32_t>(mappingOrdinal),
            .wireIndex = localWireIndex,
            .occurrence = static_cast<std::uint32_t>(occurrence),
            .wireValid = static_cast<bool>(row.wire.valid),
            .hasPin = static_cast<bool>(row.wire.valid && row.object.object != nullptr),
        };
      }
      ++occurrence;
    }
    return true;
  };
  const auto& s = peSemanticSparse(input);
  if (!sparse(s.textures, D9C_CHUNK_HANDLE_KIND_TEXTURE, 1u, 1u) ||
      !sparse(s.streams, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u, 2u) ||
      !sparse(s.shaders, D9C_CHUNK_HANDLE_KIND_SHADER, 3u, 3u) ||
      !sparse(s.vertexInputs, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, 4u, 4u) ||
      !sparse(s.indexBuffers, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u, 5u) ||
      !sparse(s.renderTargets, D9C_CHUNK_HANDLE_KIND_SURFACE, 0u, 6u) ||
      !sparse(s.depthStencils, D9C_CHUNK_HANDLE_KIND_SURFACE, 0u, 7u)) {
    return false;
  }
  const auto storeSparseCount = [&](std::size_t index,
                                    std::size_t count) noexcept {
    std::uint32_t bounded = 0u;
    if (!detail::checkedSizeToU32(count, bounded)) return false;
    out.sparseCounts[index] = bounded;
    return true;
  };
  if (!storeSparseCount(0u, s.renderStates.size()) ||
      !storeSparseCount(8u, s.viewports.size()) ||
      !storeSparseCount(9u, s.scissors.size()) ||
      !storeSparseCount(10u, s.materials.size()) ||
      !storeSparseCount(11u, s.clipPlanes.size()) ||
      !storeSparseCount(12u, s.textureStageStates.size()) ||
      !storeSparseCount(13u, s.samplerStates.size()) ||
      !storeSparseCount(14u, s.transforms.size()) ||
      !storeSparseCount(15u, s.lights.size()) ||
      !storeSparseCount(16u, s.lightEnables.size())) return false;
  std::uint32_t boundedRectCount = 0u;
  std::uint32_t boundedConstantBytes = 0u;
  if (!detail::checkedSizeToU32(staged.clearRects.size(), boundedRectCount) ||
      !detail::checkedSizeToU32(staged.constantBytes.size(), boundedConstantBytes)) {
    return false;
  }
  std::size_t payloadBytes = 0u;
  payloadBytes = sparseLayout->payloadBytes;
  if (!detail::checkedSizeToU32(payloadBytes, out.payloadBytes)) return false;
  out.rectCount = boundedRectCount;
  std::size_t semanticBytes = boundedConstantBytes;
  const auto addBytes = [&](std::size_t bytes) noexcept {
    if (semanticBytes > std::numeric_limits<std::uint32_t>::max() ||
        bytes > std::numeric_limits<std::uint32_t>::max() - semanticBytes) return false;
    semanticBytes += bytes;
    return true;
  };
  if (!addBytes(s.vsFloatConstants.registerBytes.size()) ||
      !addBytes(s.vsIntConstants.registerBytes.size()) ||
      !addBytes(s.vsBoolConstants.registerBytes.size()) ||
      !addBytes(s.psFloatConstants.registerBytes.size()) ||
      !addBytes(s.psIntConstants.registerBytes.size()) ||
      !addBytes(s.psBoolConstants.registerBytes.size()) ||
      !addBytes(s.upIndexData.size()) || !addBytes(s.upVertexData.size())) return false;
  if (!detail::checkedSizeToU32(semanticBytes, out.semanticBytes) ||
      !detail::checkedSizeToU32(
          identities.wireCount, out.handleCount)) return false;
  out.valid = true;
  return true;
}

template <bool ProjectUniquePinCounts = true, PeSemanticRecordInputLike Input,
          typename AdmissionPlanT, typename OnUniqueRetained>
  requires (std::is_nothrow_invocable_r_v<bool, OnUniqueRetained&,
                                          const PeWireObjectRef&, std::size_t> ||
            std::is_nothrow_invocable_r_v<bool, OnUniqueRetained&,
                                          const PeWireObjectRef&, std::size_t,
                                          std::size_t>)
inline bool planPeSemanticAdmissionWith(
    const Input& input, AdmissionPlanT& out,
    OnUniqueRetained&& onUniqueRetained,
    PeSemanticEmissionHandleContext* wireWitness = nullptr,
    PeSemanticPreparedSparseLayout* sparseLayoutWitness = nullptr,
    PeSemanticBindingOccurrencePlan* bindingWitness = nullptr,
    PeSemanticPreparedIdentityWitness* identityWitness = nullptr) noexcept {
  if (identityWitness != nullptr) {
    return planPeSemanticAdmissionWithIdentity<ProjectUniquePinCounts>(
        input, out, static_cast<OnUniqueRetained&&>(onUniqueRetained),
        *identityWitness,
        wireWitness, sparseLayoutWitness, bindingWitness);
  }
  PeSemanticPreparedIdentityWitness localIdentityWitness;
  return planPeSemanticAdmissionWithIdentity<ProjectUniquePinCounts>(
      input, out, static_cast<OnUniqueRetained&&>(onUniqueRetained),
      localIdentityWitness,
      wireWitness, sparseLayoutWitness, bindingWitness);
}

template <PeSemanticRecordInputLike Input>
inline bool planPeSemanticAdmission(const Input& input,
                                    PeSemanticAdmissionPlan& out) noexcept {
  return planPeSemanticAdmissionWith(
      input, out,
      [](const PeWireObjectRef&, std::size_t) noexcept { return true; });
}

// Why a prepared record exists at all: admission, retention, header
// validation, and emission accounting each used to re-derive the same facts
// from the same immutable input against the same unchanged owner state.  This
// is the single call-local witness that computes them once.  It is produced by
// PeSemanticBatchOwner::prepareAdmission(), consumed by the transactional
// append, and never stored: its pointer fields only qualify the current input
// and it outlives no call or ownership boundary.
//
// It is deliberately not accepted by any mutating entry point.  The owner
// re-prepares internally, so a forged witness cannot admit a record; the
// public form exists so capacity/retention truth tables are testable without
// exposing owner internals.
enum class PeSemanticAdmissionOutcome : std::uint8_t {
  Admissible,
  Unavailable,
  Malformed,
  Capacity,
};

template <std::size_t IdentityCapacity = 80u,
          typename AdmissionPlanT = PeSemanticAdmissionPlan>
struct PeSemanticPreparedRecordT {
  using AdmissionPlan = AdmissionPlanT;
  using IdentityEntry =
      typename PeSemanticPreparedIdentityWitnessT<IdentityCapacity>::Entry;
  AdmissionPlan plan;
  PeSemanticPreparedSparseLayout sparseLayout;
  PeSemanticBindingOccurrencePlan bindingOccurrences;
  // Sole private admission-local identity authority: entries retain first
  // identity order while wireOrdinals preserves first wire promotion order.
  PeSemanticPreparedIdentityWitnessT<IdentityCapacity> identityWitness;
  // Owner-qualified retention delta: how many novel typed pins per kind this
  // record adds beyond the pins the destination chunk already holds.
  std::array<std::uint32_t, 6u> retentionDeltas{};

  // Destination/role facts resolved once from the static schema tables.
  const RecordRule* rule = nullptr;
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  bool producerMatchesRecordType = false;
  bool constantProducer = false;
  bool upProducer = false;

  // The destination frontier the plan was proved against.  Append refuses a
  // witness whose destination has moved, so a stale witness fails closed
  // instead of committing an unproven layout.
  std::size_t recordCount = 0u;
  std::size_t emissionHandleCount = 0u;
  std::size_t emissionPayloadBytes = 0u;

  // Reusable exact-emission offsets produced by the capacity proof.
  std::size_t payloadOffset = 0u;
  std::size_t nextEmissionPayloadBytes = 0u;
  std::size_t nextEmissionHandleCount = 0u;
  std::size_t wireBytes = 0u;
  bool admissible = false;

  PeSemanticPreparedRecordT() noexcept {}

  // Reused preparation destinations need their scalar validity/frontier state
  // reset, but the planner owns the full plan reset and the wire context only
  // consumes identities below its active count. Keeping those bounded tails
  // intact avoids a second whole-witness clear without exposing stale state to
  // any admission or emission consumer.
  void resetForAdmission() noexcept {
    sparseLayout.reset();
    resetAdmissionFrontier();
  }

  void resetForCompactAdmission() noexcept {
    sparseLayout.valid = false;
    resetAdmissionFrontier();
  }

 private:
  void resetAdmissionFrontier() noexcept {
    plan.valid = false;
    bindingOccurrences.reset();
    identityWitness.reset();
    retentionDeltas.fill(0u);
    rule = nullptr;
    producer = PeSemanticProducerKind::DrawPrimitive;
    producerMatchesRecordType = false;
    constantProducer = false;
    upProducer = false;
    recordCount = 0u;
    emissionHandleCount = 0u;
    emissionPayloadBytes = 0u;
    payloadOffset = 0u;
    nextEmissionPayloadBytes = 0u;
    nextEmissionHandleCount = 0u;
    wireBytes = 0u;
    admissible = false;
  }

 public:
  bool valid() const noexcept {
    return admissible && plan.valid && rule != nullptr;
  }
  bool admissibleAt(std::size_t records, std::size_t handles,
                    std::size_t payloadBytes) const noexcept {
    return valid() && recordCount == records &&
           emissionHandleCount == handles &&
           emissionPayloadBytes == payloadBytes;
  }
};

using PeSemanticPreparedRecord = PeSemanticPreparedRecordT<80u>;
using PeSemanticProductionPreparedRecord =
    PeSemanticPreparedRecordT<80u, PeSemanticProductionAdmissionPlan>;
using PeSemanticCompactPreparedRecord =
    PeSemanticPreparedRecordT<kPeSemanticMaxBindingOccurrences>;
using PeSemanticProductionCompactPreparedRecord =
    PeSemanticPreparedRecordT<kPeSemanticMaxBindingOccurrences,
                              PeSemanticCompactAdmissionPlan>;

static_assert(std::is_trivially_copyable_v<PeSemanticPreparedRecord>);
static_assert(std::is_trivially_copyable_v<PeSemanticCompactPreparedRecord>);
static_assert(sizeof(PeSemanticCompactPreparedRecord) <
              sizeof(PeSemanticPreparedRecord));

// Constant preparation carries only the facts consumed by the common owner
// transaction.  In particular it has no identity, sparse, pin, or locator
// arenas, so the production constant path does not grow the generic witness's
// stack frame or clear its inactive product.
struct PeSemanticPreparedConstantRecord {
  const RecordRule* rule = nullptr;
  PeSemanticProducerKind producer = PeSemanticProducerKind::VsFloatConstant;
  std::uint32_t recordType = 0u;
  std::size_t payloadBytes = 0u;
  std::size_t semanticBytes = 0u;
  std::size_t recordCount = 0u;
  std::size_t emissionHandleCount = 0u;
  std::size_t emissionPayloadBytes = 0u;
  std::size_t payloadOffset = 0u;
  std::size_t nextEmissionPayloadBytes = 0u;
  std::size_t nextEmissionHandleCount = 0u;
  std::size_t wireBytes = 0u;
  bool producerMatchesRecordType = false;
  bool admissible = false;

  bool valid() const noexcept {
    return admissible && rule != nullptr && payloadBytes != 0u;
  }
  bool admissibleAt(std::size_t records, std::size_t handles,
                    std::size_t bytes) const noexcept {
    return valid() && recordCount == records &&
           emissionHandleCount == handles && emissionPayloadBytes == bytes;
  }
};

static_assert(std::is_trivially_copyable_v<PeSemanticPreparedConstantRecord>);
static_assert(sizeof(PeSemanticPreparedConstantRecord) <= 112u);

namespace detail {

struct PeSemanticOwnerAdmissionReconstruction {
  std::array<D9CWireObjectIdentity, 80u> identities;
  std::array<const void*, 80u> objects;
  std::size_t count = 0u;
  PeSemanticOwnerAdmissionDistribution distribution{};

  bool observe(const PeWireObjectRef& ref, std::uint32_t kind) noexcept {
    ++distribution.refOccurrences;
    if (!ref.object) return true;
    if (!ref.valid(kind)) return false;
    for (std::size_t i = 0u; i < count; ++i) {
      ++distribution.linearComparisons;
      if (identities[i].kind == ref.identity.kind &&
          identities[i].objectId == ref.identity.objectId) {
        return identities[i].generation == ref.identity.generation &&
               objects[i] == ref.object;
      }
    }
    if (count == identities.size()) return false;
    identities[count] = ref.identity;
    objects[count] = ref.object;
    ++count;
    ++distribution.uniqueIdentities;
    return true;
  }
};

template <typename Input, std::size_t IdentityCapacity, typename AdmissionPlanT>
bool reconstructPeSemanticOwnerAdmission(
    const Input& input,
    const PeSemanticPreparedRecordT<IdentityCapacity, AdmissionPlanT>& prepared,
    PeSemanticOwnerAdmissionDistribution& out) noexcept {
  out = {};
  if constexpr (PeSemanticSparsePlanInputLike<Input>) {
    PeSemanticOwnerAdmissionDistribution reconstructed{};
    std::array<bool, IdentityCapacity> seen{};
    const auto& occurrences = prepared.bindingOccurrences;
    static_assert(std::tuple_size_v<decltype(occurrences.ranges)> ==
                  std::tuple_size_v<decltype(reconstructed.bindings)>);
    if (occurrences.count > occurrences.values.size() ||
        prepared.identityWitness.count > IdentityCapacity ||
        prepared.identityWitness.wireCount > prepared.identityWitness.wireOrdinals.size())
      return false;
    std::size_t cursor = 0u;
    for (std::size_t family = 0u; family < occurrences.ranges.size();
         ++family) {
      const auto& range = occurrences.ranges[family];
      if (range.offset != cursor || range.offset > occurrences.count ||
          range.count > occurrences.count - range.offset)
        return false;
      auto& binding = reconstructed.bindings[family];
      binding.occurrences = range.count;
      for (std::size_t occurrence = 0u; occurrence < range.count;
           ++occurrence) {
        const auto& locator = occurrences.values[range.offset + occurrence];
        if (locator.occurrence != occurrence || !locator.wireValid) return false;
        if (!locator.hasPin) {
          if (locator.mappingOrdinal != kPeSemanticNoSlot ||
              locator.wireIndex != D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX)
            return false;
          continue;
        }
        if (locator.mappingOrdinal >= prepared.identityWitness.count ||
            locator.mappingOrdinal >= IdentityCapacity)
          return false;
        const auto ordinal = static_cast<std::size_t>(locator.mappingOrdinal);
        const auto& identity = prepared.identityWitness.entries[ordinal];
        if (identity.object == nullptr ||
            locator.wireIndex != identity.wireIndex ||
            locator.wireIndex >= prepared.identityWitness.wireCount)
          return false;
        ++binding.pinned;
        if (!seen[ordinal]) {
          if (ordinal != reconstructed.uniqueIdentities) return false;
          reconstructed.linearComparisons += ordinal;
          seen[ordinal] = true;
          ++reconstructed.uniqueIdentities;
          ++binding.first;
        } else {
          reconstructed.linearComparisons += ordinal + 1u;
          ++binding.duplicates;
        }
      }
      cursor += range.count;
    }
    if (cursor != occurrences.count ||
        reconstructed.uniqueIdentities != prepared.identityWitness.count)
      return false;
    reconstructed.bindingOccurrences = cursor;
    reconstructed.refOccurrences = cursor;
    reconstructed.bindingsMeasured = true;
    out = reconstructed;
    return true;
  } else {
    PeSemanticOwnerAdmissionReconstruction reconstruction;
    const auto& staged = stagedPeSemanticInput(input);
    const auto direct = [&](const PeWireObjectRef& ref, std::uint32_t kind) noexcept {
      return reconstruction.observe(ref, kind);
    };
    if (!direct(staged.surface0, D9C_CHUNK_HANDLE_KIND_SURFACE) ||
        !direct(staged.surface1, D9C_CHUNK_HANDLE_KIND_SURFACE) ||
        !direct(staged.texture0, D9C_CHUNK_HANDLE_KIND_TEXTURE) ||
        !direct(staged.texture1, D9C_CHUNK_HANDLE_KIND_TEXTURE) ||
        !direct(staged.buffer0, D9C_CHUNK_HANDLE_KIND_BUFFER) ||
        !direct(staged.buffer1, D9C_CHUNK_HANDLE_KIND_BUFFER) ||
        !direct(staged.shader0, D9C_CHUNK_HANDLE_KIND_SHADER) ||
        !direct(staged.shader1, D9C_CHUNK_HANDLE_KIND_SHADER) ||
        !direct(staged.declaration, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) ||
        !direct(staged.query, D9C_CHUNK_HANDLE_KIND_QUERY)) return false;
    switch (staged.producer) {
    case PeSemanticProducerKind::Present:
    case PeSemanticProducerKind::ColorFill:
      if (!direct(staged.surface0, D9C_CHUNK_HANDLE_KIND_SURFACE)) return false;
      break;
    case PeSemanticProducerKind::GenerateMipmaps:
      if (!direct(staged.texture0, D9C_CHUNK_HANDLE_KIND_TEXTURE)) return false;
      break;
    case PeSemanticProducerKind::StretchRect:
    case PeSemanticProducerKind::UpdateSurface:
    case PeSemanticProducerKind::Readback:
      if (!direct(staged.surface0, D9C_CHUNK_HANDLE_KIND_SURFACE) ||
          !direct(staged.surface1, D9C_CHUNK_HANDLE_KIND_SURFACE)) return false;
      break;
    case PeSemanticProducerKind::UpdateTexture:
      if (!direct(staged.texture0, D9C_CHUNK_HANDLE_KIND_TEXTURE) ||
          !direct(staged.texture1, D9C_CHUNK_HANDLE_KIND_TEXTURE)) return false;
      break;
    case PeSemanticProducerKind::QueryIssue:
      if (!direct(staged.query, D9C_CHUNK_HANDLE_KIND_QUERY)) return false;
      break;
    case PeSemanticProducerKind::ReszDepthResolve:
      if (!direct(staged.surface0, D9C_CHUNK_HANDLE_KIND_SURFACE) ||
          !direct(staged.texture0, D9C_CHUNK_HANDLE_KIND_TEXTURE)) return false;
      break;
    default: break;
    }
    const auto& sparse = peSemanticSparse(input);
    const auto rows = [&](auto values, std::uint32_t kind) noexcept {
      for (const auto& row : values) {
        if (row.wire.valid && !direct(row.object, kind)) return false;
      }
      return true;
    };
    if (!rows(sparse.textures, D9C_CHUNK_HANDLE_KIND_TEXTURE) ||
        !rows(sparse.streams, D9C_CHUNK_HANDLE_KIND_BUFFER) ||
        !rows(sparse.shaders, D9C_CHUNK_HANDLE_KIND_SHADER) ||
        !rows(sparse.vertexInputs, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) ||
        !rows(sparse.indexBuffers, D9C_CHUNK_HANDLE_KIND_BUFFER) ||
        !rows(sparse.renderTargets, D9C_CHUNK_HANDLE_KIND_SURFACE) ||
        !rows(sparse.depthStencils, D9C_CHUNK_HANDLE_KIND_SURFACE)) return false;
    out.bindingOccurrences = prepared.bindingOccurrences.count;
    out.refOccurrences = reconstruction.distribution.refOccurrences;
    out.uniqueIdentities = reconstruction.distribution.uniqueIdentities;
    out.linearComparisons = reconstruction.distribution.linearComparisons;
    if (reconstruction.count != prepared.identityWitness.count) return false;
    for (std::size_t i = 0u; i < reconstruction.count; ++i) {
      const auto& entry = prepared.identityWitness.entries[i];
      if (!prepared.identityWitness.sameIdentity(
              reconstruction.identities[i], entry.identity) ||
          reconstruction.objects[i] != entry.object) return false;
    }
    return true;
  }
}

inline bool reconstructPeSemanticOwnerAdmission(
    const PeSemanticConstantRecordInput&,
    const PeSemanticPreparedConstantRecord&,
    PeSemanticOwnerAdmissionDistribution& out) noexcept {
  out = {};
  return true;
}

}  // namespace detail

struct PeSemanticRecordSlot {
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  std::uint32_t recordType = 0u;
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;

  D9CCommandChunkWireDrawHeader draw{};
  D9CCommandChunkWireSetConst setConst{};
  D9CCommandChunkWireClear clear{};
  D9CCommandChunkWirePresent present{};
  D9CCommandChunkWireStretchRect stretchRect{};
  D9CCommandChunkWireColorFill colorFill{};
  D9CCommandChunkWireUpdateSurface updateSurface{};
  D9CCommandChunkWireQueryIssue queryIssue{};
  std::uint32_t updateFlags = 0u;
  std::uint32_t reszFlags = 0u;
  std::uint32_t mipmapFlags = 0u;

  std::uint32_t surface0 = kPeSemanticNoSlot;
  std::uint32_t surface1 = kPeSemanticNoSlot;
  std::uint32_t texture0 = kPeSemanticNoSlot;
  std::uint32_t texture1 = kPeSemanticNoSlot;
  std::uint32_t buffer0 = kPeSemanticNoSlot;
  std::uint32_t buffer1 = kPeSemanticNoSlot;
  std::uint32_t shader0 = kPeSemanticNoSlot;
  std::uint32_t shader1 = kPeSemanticNoSlot;
  std::uint32_t declaration = kPeSemanticNoSlot;
  std::uint32_t query = kPeSemanticNoSlot;

  PeSemanticArenaRange constantBytes{};
  PeSemanticArenaRange vsFloatConstants{};
  PeSemanticArenaRange vsIntConstants{};
  PeSemanticArenaRange vsBoolConstants{};
  PeSemanticArenaRange psFloatConstants{};
  PeSemanticArenaRange psIntConstants{};
  PeSemanticArenaRange psBoolConstants{};
  PeSemanticArenaRange clearRects{};
  PeSemanticArenaRange upIndexBytes{};
  PeSemanticArenaRange upVertexBytes{};
  D9CCommandChunkWireConstantRange vsFloatConstant{};
  D9CCommandChunkWireConstantRange vsIntConstant{};
  D9CCommandChunkWireConstantRange vsBoolConstant{};
  D9CCommandChunkWireConstantRange psFloatConstant{};
  D9CCommandChunkWireConstantRange psIntConstant{};
  D9CCommandChunkWireConstantRange psBoolConstant{};

  PeSemanticArenaRange renderStates{};
  PeSemanticArenaRange textures{};
  PeSemanticArenaRange streams{};
  PeSemanticArenaRange shaders{};
  PeSemanticArenaRange vertexInputs{};
  PeSemanticArenaRange indexBuffers{};
  PeSemanticArenaRange renderTargets{};
  PeSemanticArenaRange depthStencils{};
  PeSemanticArenaRange viewports{};
  PeSemanticArenaRange scissors{};
  PeSemanticArenaRange materials{};
  PeSemanticArenaRange clipPlanes{};
  PeSemanticArenaRange textureStageStates{};
  PeSemanticArenaRange samplerStates{};
  PeSemanticArenaRange transforms{};
  PeSemanticArenaRange lights{};
  PeSemanticArenaRange lightEnables{};
};

static_assert(std::is_trivially_copyable_v<PeSemanticRecordInput>);
static_assert(std::is_trivially_copyable_v<PeSemanticRecordSlot>);

// Input-qualified call-local staging. Sparse plans and constants are already
// canonicalized by their immutable source views, so retaining the broad
// all-family slot here would recreate the removed committed value product on
// every append stack frame.
struct PeSemanticSparsePlanRecordStaging {
};

struct PeSemanticConstantRecordStaging {
  D9CCommandChunkWireSetConst setConst{};
};

static_assert(std::is_trivially_copyable_v<PeSemanticSparsePlanRecordStaging>);
static_assert(std::is_trivially_copyable_v<PeSemanticConstantRecordStaging>);
static_assert(sizeof(PeSemanticSparsePlanRecordStaging) <
              sizeof(PeSemanticRecordSlot));
static_assert(sizeof(PeSemanticConstantRecordStaging) <
              sizeof(PeSemanticRecordSlot));

// A committed record is a receipt, not a second semantic value product.  The
// broad slot above is call-local staging used while a record is materialized;
// canonical wire roles are the only committed representation of values.
struct PeSemanticRecordReceipt {
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  std::uint32_t recordType = 0u;
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
};

static_assert(std::is_trivially_copyable_v<PeSemanticRecordReceipt>);
static_assert(sizeof(PeSemanticRecordReceipt) <= 40u);

// Emission never allocates. The caller supplies the three fixed-role regions
// for normal segmented transport or one aligned extent for the contiguous
// ExactFixed differential oracle; the owner only writes after a complete
// checked plan has been formed.
struct PeSemanticSegmentedEmission {
  D9CCommandChunkSegmentedTransportV1 transport{};
  std::uint32_t wireBytes = 0u;

  bool valid() const noexcept {
    return transport.header.recordCount != 0u && wireBytes != 0u &&
           transport.recordReserved == 0u && transport.handleReserved == 0u &&
           transport.payloadReserved == 0u &&
           transport.producerIdentity.firstEventOrdinal != 0u &&
           transport.producerIdentity.lastEventOrdinal >=
               transport.producerIdentity.firstEventOrdinal &&
           transport.producerIdentity.firstSourceOrdinal != 0u &&
           transport.producerIdentity.lastSourceOrdinal >=
               transport.producerIdentity.firstSourceOrdinal;
  }
};

struct PeSemanticExactFixedEmission {
  D9CCommandChunkSegmentedTransportV1 transport{};
  std::span<const std::byte> wire{};
  std::uint32_t wireBytes = 0u;

  bool valid() const noexcept {
    return wireBytes != 0u && wire.size() == wireBytes &&
           transport.header.recordCount != 0u &&
           transport.producerIdentity.firstEventOrdinal != 0u &&
           transport.producerIdentity.lastEventOrdinal >=
               transport.producerIdentity.firstEventOrdinal &&
           transport.producerIdentity.firstSourceOrdinal != 0u &&
           transport.producerIdentity.lastSourceOrdinal >=
               transport.producerIdentity.firstSourceOrdinal;
  }
};

// Live owner counters used by recorder cadence/checkpoint bookkeeping.  This
// is a call-local projection, not a receipt or cached wire-layout authority.
// Callers must read it again after mutation or a destination boundary; a
// previously copied count is not evidence about the new owner state.
struct PeSemanticEmissionFrontier {
  std::size_t recordCount = 0u;
  std::size_t handleCount = 0u;
  std::size_t payloadBytes = 0u;
};

static_assert(std::is_trivially_copyable_v<PeSemanticEmissionFrontier>);

struct PeSemanticPinIndexEntry {
  D9CWireObjectIdentity identity{};
  std::uint32_t pin = kPeSemanticNoSlot;
  // Zero is reserved for a logically empty bucket; a nonzero value is the
  // owner index generation in which this bucket is live.
  std::uint32_t generation = 0u;
};

static_assert(sizeof(PeSemanticPinIndexEntry) == 24u);

template <std::size_t N>
using PeSemanticPinIndex = std::array<PeSemanticPinIndexEntry, N>;

// Native owner specs use one bounded friend seam to corrupt an existing
// locator entry.  It is intentionally not a production API: malformed-hit
// coverage must exercise the private relation without exposing raw storage.
struct PeSemanticOwnerTestAccess;

template <std::size_t MaxRecords, std::size_t MaxPins,
          std::size_t MaxSemanticBytes, std::size_t MaxRects,
          std::size_t MaxSparseValues>
struct PeSemanticBatchStorage {
  static constexpr std::size_t pinIndexCapacity = MaxPins * 4u + 1u;
  std::array<PeSemanticRecordReceipt, MaxRecords> records{};
  std::array<PeSemanticPhysicalPin<D9CSurface, PeSemanticSurfaceIdentity>, MaxPins> surfaces{};
  std::array<PeSemanticPhysicalPin<D9CTexture, PeSemanticTextureIdentity>, MaxPins> textures{};
  std::array<PeSemanticPhysicalPin<D9CBuffer, PeSemanticBufferIdentity>, MaxPins> buffers{};
  std::array<PeSemanticPhysicalPin<D9CShader, PeSemanticShaderIdentity>, MaxPins> shaders{};
  std::array<PeSemanticPhysicalPin<D9CVertexDecl, PeSemanticDeclarationIdentity>, MaxPins> declarations{};
  std::array<PeSemanticPhysicalPin<D9CQuery, PeSemanticQueryIdentity>, MaxPins> queries{};
  // Each kind-local table is keyed by objectId.  The typed pin prefix is the
  // authority; a table entry is only a locator checked against that prefix.
  // There is intentionally no second exact-identity table whose consistency
  // could drift from this one.
  PeSemanticPinIndex<pinIndexCapacity> surfacePinIndex{};
  PeSemanticPinIndex<pinIndexCapacity> texturePinIndex{};
  PeSemanticPinIndex<pinIndexCapacity> bufferPinIndex{};
  PeSemanticPinIndex<pinIndexCapacity> shaderPinIndex{};
  PeSemanticPinIndex<pinIndexCapacity> declarationPinIndex{};
  PeSemanticPinIndex<pinIndexCapacity> queryPinIndex{};
  // Canonical D9C V2 role-layout backing.  These three regions are not a
  // seal-time rendering of the typed arenas above: admission writes each
  // accepted record's final wire record header, its final handle entries and
  // its fully serialized payload bytes here, at the exact in-role position the
  // sealed chunk gives them.  Sealing therefore finalizes a header and two
  // bounded table offsets instead of replaying every record.
  //
  // A sealed chunk places the payload at `WIRE_HEADER + records*RHS +
  // handles*HES`, which is unknown while records are admitted. The canonical
  // payload therefore lives in a physically separate fixed arena; this is
  // important because an ExactFixed destination may be the owner's `wire`
  // arena and may overlap the maximum-prefix staging address. Separating the
  // arenas makes retry/re-emission read-only with respect to canonical bytes.
  static constexpr std::size_t maxWireRecords = MaxRecords;
  // A record's wire handle table is bounded by the per-record emission dedup
  // frame, so this is the exact worst case rather than a guess.
  static constexpr std::size_t maxWireHandles = MaxRecords * 64u;
  static constexpr std::size_t wirePayloadBase =
      ((sizeof(D9CCommandChunkWireHeader) +
        maxWireRecords * sizeof(D9CCommandChunkWireRecordHeader) +
        maxWireHandles * sizeof(D9CCommandChunkWireHandleEntry) + 15u) /
       16u) * 16u;
  // Generous, deliberately loose payload ceiling: every constant/UP byte, every
  // clear rectangle, every sparse row of all seventeen kinds, plus each
  // record's own fixed head, section table and alignment padding.
  static constexpr std::size_t maxWirePayloadBytes =
      MaxSemanticBytes + MaxRects * sizeof(D9CRect) +
      MaxSparseValues * 1024u +
      MaxRecords * (sizeof(D9CCommandChunkWireDrawHeader) +
                    D9C_COMMAND_CHUNK_SECTION_COUNT *
                        sizeof(D9CCommandChunkWireSectionDesc) + 8u) +
      64u;
  static constexpr std::size_t maxWireBytes =
      wirePayloadBase + maxWirePayloadBytes;
  std::array<D9CCommandChunkWireRecordHeader, maxWireRecords> wireRecords{};
  std::array<D9CCommandChunkWireHandleEntry, maxWireHandles> wireHandles{};
  alignas(16) std::array<std::byte, maxWirePayloadBytes> canonicalPayload{};
  alignas(16) std::array<std::byte, maxWireBytes> wire{};
};

// The template capacities make overflow tests cheap while the defaults cover
// the ordinary bounded recorder chunk. Typed pins retain ownership and
// occurrence-qualified locators drive direct canonical binding emission;
// scalar rows and variable tails serialize directly into canonicalPayload.
template <std::size_t MaxRecords = 64u, std::size_t MaxPins = 256u,
          std::size_t MaxSemanticBytes = 1u << 20u,
          std::size_t MaxRects = 1024u, std::size_t MaxSparseValues = 2048u,
          typename LedgerAccess = dxmt9::core::ScopedCopyMaterializationLedgerAccess>
class PeSemanticBatchOwner final {
  friend struct PeSemanticOwnerTestAccess;

 public:
  static_assert(MaxPins <= static_cast<std::size_t>(kPeSemanticNoSlot));

  enum class AdmissionFailure : std::uint8_t {
    None,
    Unavailable,
    Capacity,
    Malformed,
    Header,
    Fixed,
    DirectPins,
    Sparse,
    SparseSchema,
    SparseArena,
    SparseTextures,
    SparseStreams,
    SparseShaders,
    SparseVertexInputs,
    SparseIndexBuffers,
    SparseRenderTargets,
    SparseDepthStencils,
    VariablePayload,
    EmissionMetrics,
    Settlement,
  };

  using Storage = PeSemanticBatchStorage<MaxRecords, MaxPins,
                                         MaxSemanticBytes, MaxRects,
                                         MaxSparseValues>;
  using EmissionHandleContext = PeSemanticEmissionHandleContext;
  static constexpr std::size_t maxRecords = MaxRecords;
  static constexpr std::size_t maxPins = MaxPins;
  static constexpr std::size_t maxSemanticBytes = MaxSemanticBytes;
  static constexpr std::size_t storageBytes = sizeof(Storage);
  static constexpr std::size_t warmRetainerCapacity =
      MaxPins * (D3D9PePendingCommandRetainer::kWarmEpochs + 2u);

  // Native tests use this tag to exercise the unavailable-owner contract
  // deterministically, without depending on host memory pressure.
  struct FailConstructionForTesting {};
  // Native tests use this tag to reach the otherwise impractical bounded
  // generation-wrap path without adding a production clock or counter.
  struct PinIndexGenerationForTesting {
    std::uint32_t value = 1u;
  };

  PeSemanticBatchOwner() noexcept
      : storage_(new (std::nothrow) Storage{}),
        retainer_(warmRetainerCapacity) {
    // Publish readiness only after both bounded heap storage and every
    // retainer reserve have completed successfully.  Neither admission nor
    // rollback needs to allocate after this point.
    ready_ = storage_ != nullptr && retainer_.constructionSucceeded();
  }
  explicit PeSemanticBatchOwner(FailConstructionForTesting) noexcept
      : storage_(new (std::nothrow) Storage{}),
        retainer_(MaxPins,
                  D3D9PePendingCommandRetainer::ConstructionMode::FailForTesting) {}
  explicit PeSemanticBatchOwner(PinIndexGenerationForTesting testing) noexcept
      : storage_(new (std::nothrow) Storage{}),
        retainer_(warmRetainerCapacity) {
    pinIndexGeneration_ = testing.value == 0u ? 1u : testing.value;
    ready_ = storage_ != nullptr && retainer_.constructionSucceeded();
  }
  PeSemanticBatchOwner(const PeSemanticBatchOwner&) = delete;
  PeSemanticBatchOwner& operator=(const PeSemanticBatchOwner&) = delete;
  ~PeSemanticBatchOwner() { releaseLedgerRetention(); }

  bool constructionSucceeded() const noexcept { return ready_; }
  AdmissionFailure lastAdmissionFailure() const noexcept {
    return lastAdmissionFailure_;
  }

  // Set once, after the cold diagnostics owner has been constructed.  The
  // pointer is nullable and is not consulted unless the explicit phase
  // observer is enabled, leaving the default owner transaction unchanged.
  void setPhaseObserver(PeSemanticOwnerPhaseObserver* observer) noexcept {
    phaseObserver_ = observer;
  }

 private:
  template <typename Prepared>
  bool proveEmissionCapacity(Prepared& witness, std::size_t payloadBytes,
                             std::size_t handleCount,
                             std::size_t semanticBytes) const noexcept {
    if (!ready_ || recordCount_ >= MaxRecords || !witness.rule ||
        payloadBytes == 0u ||
        semanticBytes > MaxSemanticBytes - semanticBytes_) {
      return false;
    }
    std::size_t aligned = 0u;
    if (!alignEmission(emissionPayloadBytes_, witness.rule->payloadAlignment,
                       aligned) ||
        payloadBytes > std::numeric_limits<std::size_t>::max() - aligned ||
        handleCount > std::numeric_limits<std::size_t>::max() -
                          emissionHandleCount_) {
      return false;
    }
    const auto nextPayload = aligned + payloadBytes;
    const auto nextHandles = emissionHandleCount_ + handleCount;
    if (nextPayload > std::numeric_limits<std::uint32_t>::max() ||
        nextHandles > std::numeric_limits<std::uint32_t>::max() ||
        !fitsStagedWire(nextHandles, nextPayload)) {
      return false;
    }
    const auto layout = planExactCommandChunkLayout(
        static_cast<std::uint32_t>(recordCount_ + 1u),
        static_cast<std::uint32_t>(nextHandles),
        static_cast<std::uint32_t>(nextPayload));
    if (!layout.valid() ||
        layout.totalBytes > D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES) {
      return false;
    }
    witness.payloadOffset = aligned;
    witness.nextEmissionPayloadBytes = nextPayload;
    witness.nextEmissionHandleCount = nextHandles;
    witness.wireBytes = layout.totalBytes;
    return true;
  }

  // Pure CapacityPre proof over an already-planned record.  It also publishes
  // the exact emission frontier it computed, so the transactional append and
  // its settlement never realign or re-plan the same layout.
  template <bool UseLayoutQuota, std::size_t IdentityCapacity,
            typename AdmissionPlanT>
  bool proveCapacity(
      PeSemanticPreparedRecordT<IdentityCapacity, AdmissionPlanT>& witness) const noexcept {
    const auto& plan = witness.plan;
    const auto& quota = [&]() noexcept -> const auto& {
      if constexpr (UseLayoutQuota)
        return witness.sparseLayout.quotaDeltas;
      else
        return plan.sparseCounts;
    }();
    if (!ready_ || !plan.valid ||
        (UseLayoutQuota && !witness.sparseLayout.valid) ||
        recordCount_ >= MaxRecords || !witness.rule ||
        plan.payloadBytes == 0u) return false;
    const auto quotaFits = [&]() noexcept {
      if constexpr (UseLayoutQuota) {
        return forEachPreparedSparseQuota(
            witness.sparseLayout,
            [&](std::size_t i, std::uint32_t count) noexcept {
              return count <= MaxSparseValues - sparseCounts_.values[i];
            });
      } else {
        for (std::size_t i = 0u; i < quota.size(); ++i) {
          if (quota[i] > MaxSparseValues - sparseCounts_.values[i])
            return false;
        }
        return true;
      }
    };
    if (!quotaFits()) return false;
    if (plan.rectCount > MaxRects - rectCount_ ||
        plan.semanticBytes > MaxSemanticBytes - semanticBytes_) return false;
    const auto fitsPins = [](std::size_t used, std::size_t add) noexcept {
      return add <= MaxPins - used;
    };
    if (!fitsPins(surfaceCount_, witness.retentionDeltas[0]) ||
        !fitsPins(textureCount_, witness.retentionDeltas[1]) ||
        !fitsPins(bufferCount_, witness.retentionDeltas[2]) ||
        !fitsPins(shaderCount_, witness.retentionDeltas[3]) ||
        !fitsPins(declarationCount_, witness.retentionDeltas[4]) ||
        !fitsPins(queryCount_, witness.retentionDeltas[5])) return false;
    return proveAggregateRetentionCapacity(witness) &&
           proveEmissionCapacity(witness, plan.payloadBytes, plan.handleCount,
                                 plan.semanticBytes);
  }

  template <std::size_t IdentityCapacity, typename AdmissionPlanT>
  bool proveAggregateRetentionCapacity(
      const PeSemanticPreparedRecordT<IdentityCapacity, AdmissionPlanT>& witness) const noexcept {
    // Deltas are a disjoint partition of the bounded identity witness. Their
    // sum is a safe upper bound: some new current-chunk pins are already warm.
    // Ordinary records need no second identity walk or retainer hash lookup.
    std::size_t upperBound = 0u;
    for (const auto delta : witness.retentionDeltas) upperBound += delta;
    if (retainer_.hasHeadroom(upperBound, warmRetainerCapacity)) return true;
    if (!retainer_.hasHeadroom(0u, warmRetainerCapacity)) return false;

    // Only a near-ceiling record needs exact warm membership. Consume the
    // existing unique identity table rather than materializing/deduplicating
    // another candidate list. This query neither refreshes an epoch nor
    // consumes the unique-acquisition fault point.
    const auto headroom = warmRetainerCapacity - retainer_.size();
    std::size_t novel = 0u;
    for (std::size_t i = 0u; i < witness.identityWitness.count; ++i) {
      const auto& entry = witness.identityWitness.entries[i];
      std::size_t currentCount = 0u;
      switch (entry.identity.kind) {
        case D9C_CHUNK_HANDLE_KIND_SURFACE: currentCount = surfaceCount_; break;
        case D9C_CHUNK_HANDLE_KIND_TEXTURE: currentCount = textureCount_; break;
        case D9C_CHUNK_HANDLE_KIND_BUFFER: currentCount = bufferCount_; break;
        case D9C_CHUNK_HANDLE_KIND_SHADER: currentCount = shaderCount_; break;
        case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL: currentCount = declarationCount_; break;
        case D9C_CHUNK_HANDLE_KIND_QUERY: currentCount = queryCount_; break;
        default: return false;
      }
      if (entry.pin < currentCount) continue;
      if (!retainer_.containsWireObject(entry.identity.kind, entry.object) &&
          ++novel > headroom) return false;
    }
    return true;
  }

  // Fold one record's identity dedup into the owner's retention question.  It
  // runs once per newly seen identity, and rejects a generation or pointer
  // alias of an already-pinned object exactly where the old separate retention
  // walk did.
  template <std::size_t IdentityCapacity, typename AdmissionPlanT>
  bool accumulateRetentionDelta(
      const PeWireObjectRef& ref, std::size_t pinKind,
      PeSemanticPreparedRecordT<IdentityCapacity, AdmissionPlanT>& witness,
      std::size_t identityOrdinal) const noexcept {
    auto& out = witness.retentionDeltas;
    bool existing = false;
    std::size_t currentCount = 0u;
    std::size_t existingPin = 0u;
    switch (pinKind) {
      case 0u:
        if (!findOrValidateExistingPin(
                storage_->surfacePinIndex,
                storage_->surfaces, surfaceCount_, ref.identity, ref.object,
                existing, existingPin)) return false;
        currentCount = surfaceCount_;
        break;
      case 1u:
        if (!findOrValidateExistingPin(
                storage_->texturePinIndex,
                storage_->textures, textureCount_, ref.identity, ref.object,
                existing, existingPin)) return false;
        currentCount = textureCount_;
        break;
      case 2u:
        if (!findOrValidateExistingPin(
                storage_->bufferPinIndex,
                storage_->buffers, bufferCount_, ref.identity, ref.object,
                existing, existingPin)) return false;
        currentCount = bufferCount_;
        break;
      case 3u:
        if (!findOrValidateExistingPin(
                storage_->shaderPinIndex,
                storage_->shaders, shaderCount_, ref.identity, ref.object,
                existing, existingPin)) return false;
        currentCount = shaderCount_;
        break;
      case 4u:
        if (!findOrValidateExistingPin(
                storage_->declarationPinIndex, storage_->declarations,
                declarationCount_, ref.identity, ref.object, existing,
                existingPin))
          return false;
        currentCount = declarationCount_;
        break;
      case 5u:
        if (!findOrValidateExistingPin(
                storage_->queryPinIndex,
                storage_->queries, queryCount_, ref.identity, ref.object,
                existing, existingPin)) return false;
        currentCount = queryCount_;
        break;
      default:
        return false;
    }
    const auto pin = existing ? existingPin : currentCount + out[pinKind];
    if (!existing) {
      if (out[pinKind] == std::numeric_limits<std::uint32_t>::max())
        return false;
      ++out[pinKind];
    }
    return witness.identityWitness.setPin(identityOrdinal,
                                          static_cast<std::uint32_t>(pin));
  }

 public:
  template <std::size_t IdentityCapacity, typename AdmissionPlanT>
  PeSemanticAdmissionOutcome prepareAdmission(
      const PeSemanticSparsePlanRecordInput& input,
      PeSemanticPreparedRecordT<IdentityCapacity, AdmissionPlanT>& witness) const noexcept {
    // The compact planner owns the complete layout reset.  Keep the outer
    // witness invalid for early unavailable/malformed exits without clearing
    // the planner's 17-entry quota/scalar product twice.
    witness.resetForCompactAdmission();
    witness.plan = {};
    if (!ready_) return PeSemanticAdmissionOutcome::Unavailable;
    if (input.access == nullptr || input.sparsePlan == nullptr)
      return PeSemanticAdmissionOutcome::Malformed;
    PeSemanticAdmissionOutcome outcome =
        PeSemanticAdmissionOutcome::Malformed;
    const bool qualified = input.sparsePlan->withQualifiedSources(
        *input.access,
        [&](const SparseStatePlan::QualifiedSources& sources) noexcept {
          if (sources.destination.chunkContextFinalized == false ||
              sources.source.fullSnapshot ||
              sources.source.draw.recordType != input.recordType) return;
          outcome = prepareAdmissionQualified(input, witness, sources);
        });
    return qualified ? outcome : PeSemanticAdmissionOutcome::Malformed;
  }

 private:
  // A call-local authority for the destination-relative facts proven by
  // prepareAdmission().  The type and constructor stay private, so producers
  // can provide inputs but cannot mint an unchecked append authority.  The
  // witness pointer binds its logical quota deltas and original frontiers;
  // the owner pointer binds the destination. Neither is copied into a second
  // authority table. The token never escapes this synchronous append.
  class PreparedAppendAuthority {
   public:
    PreparedAppendAuthority(const PreparedAppendAuthority&) = delete;
    PreparedAppendAuthority& operator=(const PreparedAppendAuthority&) = delete;

   private:
    friend class PeSemanticBatchOwner;
    template <PeSemanticOwnedRecordInputLike Input>
    PreparedAppendAuthority(const void* prepared,
                            const PeSemanticBatchOwner& owner,
                            const Input& input) noexcept
        : prepared_(prepared), owner_(&owner) {
      if constexpr (PeSemanticSparsePlanInputLike<Input>) sourceInput_ = &input;
    }

    bool binds(const auto& prepared,
               const PeSemanticBatchOwner& owner) const noexcept {
      return prepared_ == &prepared && owner_ == &owner &&
             prepared.admissibleAt(owner.recordCount_,
                                   owner.emissionHandleCount_,
                                   owner.emissionPayloadBytes_);
    }

    bool bindsSource(const PeSemanticSparsePlanRecordInput& input,
                     const auto& prepared,
                     const PeSemanticBatchOwner& owner) const noexcept {
      return sourceInput_ == &input && binds(prepared, owner);
    }

    const void* prepared_ = nullptr;
    const PeSemanticBatchOwner* owner_ = nullptr;
    const PeSemanticSparsePlanRecordInput* sourceInput_ = nullptr;
  };

  // A binding mapping validated by the compact writer. Only the private
  // qualified consumer below can consume this token; generic pinPrepared()
  // remains fully checked for all other callers.
  template <typename Object, typename Identity>
  class QualifiedBinding {
   private:
    friend class PeSemanticBatchOwner;

    QualifiedBinding() noexcept = default;

    QualifiedBinding(Object* object, const D9CWireObjectIdentity& identity,
                     std::uint32_t pin, std::uint32_t wire) noexcept
        : object_(object), identity_(identity), pin_(pin), wire_(wire) {}

    Object* object_ = nullptr;
    D9CWireObjectIdentity identity_{};
    std::uint32_t pin_ = kPeSemanticNoSlot;
    std::uint32_t wire_ = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
  };

  template <std::size_t IdentityCapacity, typename AdmissionPlanT>
  PeSemanticAdmissionOutcome prepareAdmissionQualified(
      const PeSemanticSparsePlanRecordInput& input,
      PeSemanticPreparedRecordT<IdentityCapacity, AdmissionPlanT>& witness,
      const SparseStatePlan::QualifiedSources& sources) const noexcept {
    const auto& sourceFacts = sources.source;
    const auto& destinationFacts = sources.destination;
    witness.producer = input.producer;
    witness.rule = recordRule(input.recordType);
    const auto* policy = peSemanticProducerPolicy(input.recordType);
    witness.producerMatchesRecordType =
        policy != nullptr && policy->kind == input.producer;
    witness.constantProducer = false;
    witness.upProducer = false;
    if (!isPeSemanticSparseProducer(input.producer) ||
        !detail::preparePeSemanticSparsePlanLayout(
            sourceFacts, destinationFacts, witness.sparseLayout)) {
      return PeSemanticAdmissionOutcome::Malformed;
    }
    witness.plan.recordType = input.recordType;
    witness.plan.payloadBytes = witness.sparseLayout.payloadBytes;
    witness.plan.semanticBytes = witness.sparseLayout.semanticDelta;
    if constexpr (std::is_same_v<AdmissionPlanT, PeSemanticAdmissionPlan>) {
      witness.plan.sparseCounts = witness.sparseLayout.quotaDeltas;
    }

    auto& identities = witness.identityWitness;
    const auto retain = [&](const PeWireObjectRef& ref, std::uint32_t kind,
                            std::size_t pinKind,
                            PeSemanticBindingLocator& locator,
                            std::size_t occurrence) noexcept {
      bool newlyRetained = false;
      bool newlyWireVisible = false;
      std::size_t mappingOrdinal = 0u;
      if (!identities.add(ref, kind, true, newlyRetained, newlyWireVisible,
                          &mappingOrdinal)) return false;
      if (newlyRetained) {
        if constexpr (std::is_same_v<AdmissionPlanT, PeSemanticAdmissionPlan>) {
          // Inspection only. Production is bounded by the identity witness
          // and needs neither this summary nor another six-counter product.
          if (witness.plan.uniquePinCounts[pinKind] ==
              std::numeric_limits<std::uint32_t>::max()) return false;
          ++witness.plan.uniquePinCounts[pinKind];
        }
        if (!accumulateRetentionDelta(ref, pinKind, witness, mappingOrdinal))
          return false;
      }
      locator = {
          .mappingOrdinal = ref.object == nullptr
              ? kPeSemanticNoSlot
              : static_cast<std::uint32_t>(mappingOrdinal),
          .wireIndex = ref.object == nullptr
              ? D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX
              : identities.entries[mappingOrdinal].wireIndex,
          .occurrence = static_cast<std::uint32_t>(occurrence),
          .wireValid = true,
          .hasPin = ref.object != nullptr,
      };
      return true;
    };
    const auto beginFamily = [&](std::size_t family,
                                 std::size_t count) noexcept {
      if (family >= witness.bindingOccurrences.ranges.size() ||
          count > std::numeric_limits<std::uint32_t>::max() ||
          count > witness.bindingOccurrences.values.size() -
                      witness.bindingOccurrences.count) return false;
      witness.bindingOccurrences.ranges[family] = {
          .offset = static_cast<std::uint32_t>(
              witness.bindingOccurrences.count),
          .count = static_cast<std::uint32_t>(count),
      };
      return true;
    };
    // The prepared layout already qualified these seven mask-derived counts
    // while proving the payload shape. Reuse that witness for occurrence
    // ranges so admission and emission share one bounded count authority.
    const auto& quota = witness.sparseLayout.quotaDeltas;
    bool sourcesValid = false;
    const auto& shadow = sources.shadow;
    const auto& bindings = sources.bindings;
    const bool borrowed = [&]() noexcept {
          const auto renderStates = sourceFacts.fullSnapshot
              ? shadow.renderStateShadowTyped()
              : sourceFacts.pendingDelta.pendingRenderStatesTyped();
          const auto tss = sourceFacts.fullSnapshot
              ? shadow.tssShadowTyped()
              : sourceFacts.pendingDelta.pendingTssTyped();
          const auto samplers = sourceFacts.fullSnapshot
              ? shadow.samplerStateShadowTyped()
              : sourceFacts.pendingDelta.pendingSamplerStatesTyped();
          const auto transforms = sourceFacts.fullSnapshot
              ? shadow.transformShadowTyped()
              : sourceFacts.pendingDelta.pendingTransformsTyped();
          if (renderStates.size() != sourceFacts.renderStateCount ||
              tss.size() != sourceFacts.textureStageStateCount ||
              samplers.size() != sourceFacts.samplerStateCount ||
              transforms.size() != sourceFacts.transformCount ||
              (sourceFacts.textureMask & ~((1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u)) != 0u ||
              (destinationFacts.streamMask & ~((1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u)) != 0u ||
              (sourceFacts.shaderMask & ~3u) != 0u ||
              (sourceFacts.renderTargetMask &
               ~((1u << D9C_DRAW_PACKET_MAX_RENDER_TARGETS) - 1u)) != 0u ||
              (sourceFacts.clipPlaneMask & ~0x3fu) != 0u ||
              (sourceFacts.lightMask &
               ~((1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u)) != 0u ||
              (sourceFacts.lightEnableMask &
               ~((1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u)) != 0u)
            return false;
          bool ok = true;
          const auto add = [&](const PeWireObjectRef& ref, std::uint32_t kind,
                               std::size_t pinKind, std::size_t family,
                               std::size_t occurrence) noexcept {
            if (!ok) return;
            auto& locator = witness.bindingOccurrences.values[
                witness.bindingOccurrences.ranges[family].offset + occurrence];
            ok = retain(ref, kind, pinKind, locator, occurrence);
          };
          if (!beginFamily(0u, quota[kTextures])) return false;
          std::size_t occurrence = 0u;
          if (!forEachSetBitAscending(
                  sourceFacts.textureMask &
                      ((1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u),
                  [&](std::uint32_t slot) noexcept {
                    add(bindings.textures[slot],
                        D9C_CHUNK_HANDLE_KIND_TEXTURE, 1u, 0u,
                        occurrence++);
                    return ok;
                  }))
            return false;
          if (!ok) return false;
          witness.bindingOccurrences.count += occurrence;
          if (!beginFamily(1u, quota[kStreams])) return false;
          occurrence = 0u;
          if (!forEachSetBitAscending(
                  destinationFacts.streamMask &
                      ((1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u),
                  [&](std::uint32_t slot) noexcept {
                    add(bindings.streams[slot].buffer,
                        D9C_CHUNK_HANDLE_KIND_BUFFER, 2u, 1u,
                        occurrence++);
                    return ok;
                  }))
            return false;
          if (!ok) return false;
          witness.bindingOccurrences.count += occurrence;
          if (!beginFamily(2u, quota[kShaders])) return false;
          occurrence = 0u;
          if ((sourceFacts.shaderMask & 1u) != 0u)
            add(bindings.vs, D9C_CHUNK_HANDLE_KIND_SHADER, 3u, 2u,
                occurrence++);
          if ((sourceFacts.shaderMask & 2u) != 0u)
            add(bindings.ps, D9C_CHUNK_HANDLE_KIND_SHADER, 3u, 2u,
                occurrence++);
          if (!ok) return false;
          witness.bindingOccurrences.count += occurrence;
          if (!beginFamily(3u, sourceFacts.vertexInput ? 1u : 0u)) return false;
          occurrence = 0u;
          if (sourceFacts.vertexInput) {
            const PeWireObjectRef ref =
                sourceFacts.vertexInputKind ==
                        D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION
                    ? static_cast<const PeWireObjectRef&>(bindings.vdecl)
                    : PeWireObjectRef{};
            add(ref, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, 4u, 3u,
                occurrence++);
          }
          if (!ok) return false;
          witness.bindingOccurrences.count += occurrence;
          if (!beginFamily(4u, destinationFacts.indexBuffer ? 1u : 0u)) return false;
          occurrence = 0u;
          if (destinationFacts.indexBuffer)
            add(bindings.indexBuffer, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u, 4u,
                occurrence++);
          if (!ok) return false;
          witness.bindingOccurrences.count += occurrence;
          if (!beginFamily(5u, quota[kRenderTargets])) return false;
          occurrence = 0u;
          if (!forEachSetBitAscending(
                  sourceFacts.renderTargetMask &
                      ((1u << D9C_DRAW_PACKET_MAX_RENDER_TARGETS) - 1u),
                  [&](std::uint32_t slot) noexcept {
                    add(bindings.renderTargets[slot],
                        D9C_CHUNK_HANDLE_KIND_SURFACE, 0u, 5u,
                        occurrence++);
                    return ok;
                  }))
            return false;
          if (!ok) return false;
          witness.bindingOccurrences.count += occurrence;
          if (!beginFamily(6u, sourceFacts.depthStencil ? 1u : 0u)) return false;
          occurrence = 0u;
          if (sourceFacts.depthStencil)
            add(bindings.depthStencil, D9C_CHUNK_HANDLE_KIND_SURFACE, 0u,
                6u, occurrence++);
          if (!ok) return false;
          witness.bindingOccurrences.count += occurrence;
      sourcesValid = true;
      return true;
    }();
    if (!borrowed || !sourcesValid ||
        !detail::checkedSizeToU32(identities.wireCount,
                                  witness.plan.handleCount)) {
      return PeSemanticAdmissionOutcome::Malformed;
    }
    witness.plan.valid = true;
    witness.recordCount = recordCount_;
    witness.emissionHandleCount = emissionHandleCount_;
    witness.emissionPayloadBytes = emissionPayloadBytes_;
    if (!proveCapacity<true>(witness)) return PeSemanticAdmissionOutcome::Capacity;
    witness.admissible = true;
    return PeSemanticAdmissionOutcome::Admissible;
  }

 public:
  // Observation-only preparation. One pass over the immutable input answers
  // layout, retention deltas, and exact emission frontier.
  template <std::size_t IdentityCapacity, typename AdmissionPlanT,
            PeSemanticRecordInputLike Input>
  PeSemanticAdmissionOutcome prepareAdmission(
      const Input& input,
      PeSemanticPreparedRecordT<IdentityCapacity, AdmissionPlanT>& witness) const noexcept {
    witness.resetForAdmission();
    if (!ready_) {
      witness.plan = {};
      return PeSemanticAdmissionOutcome::Unavailable;
    }
    if (!validPeSemanticInput(input)) {
      witness.plan = {};
      return PeSemanticAdmissionOutcome::Malformed;
    }
    const auto& staged = stagedPeSemanticInput(input);
    witness.producer = staged.producer;
    witness.rule = recordRule(peSemanticRecordType(input));
    const auto* policy = peSemanticProducerPolicy(peSemanticRecordType(input));
    witness.producerMatchesRecordType =
        policy != nullptr && policy->kind == staged.producer;
    witness.constantProducer = isPeSemanticConstantProducer(staged.producer);
    witness.upProducer = isPeSemanticUpProducer(staged.producer);
    if (!planPeSemanticAdmissionWith<
            std::is_same_v<AdmissionPlanT, PeSemanticAdmissionPlan>>(
        input, witness.plan,
        [&](const PeWireObjectRef& ref, std::size_t pinKind,
            std::size_t identityOrdinal) noexcept {
          return accumulateRetentionDelta(ref, pinKind, witness,
                                          identityOrdinal);
        }, nullptr, &witness.sparseLayout, &witness.bindingOccurrences,
        &witness.identityWitness)) {
      return PeSemanticAdmissionOutcome::Malformed;
    }
    witness.recordCount = recordCount_;
    witness.emissionHandleCount = emissionHandleCount_;
    witness.emissionPayloadBytes = emissionPayloadBytes_;
    if (!proveCapacity<false>(witness)) return PeSemanticAdmissionOutcome::Capacity;
    witness.admissible = true;
    return PeSemanticAdmissionOutcome::Admissible;
  }

  template <PeSemanticProducerKind Producer>
  PeSemanticAdmissionOutcome prepareAdmission(
      const PeSemanticTypedConstantInput<Producer>& input,
      PeSemanticPreparedConstantRecord& witness) const noexcept {
    return prepareAdmission(input.asConstant(), witness);
  }

  PeSemanticAdmissionOutcome prepareAdmission(
      const PeSemanticConstantRecordInput& input,
      PeSemanticPreparedConstantRecord& witness) const noexcept {
    witness = {};
    if (!ready_) return PeSemanticAdmissionOutcome::Unavailable;
    std::size_t payloadBytes = 0u;
    if (!detail::checkedSizeAdd(sizeof(D9CCommandChunkWireSetConst),
                                input.constantBytes.size(), payloadBytes)) {
      return PeSemanticAdmissionOutcome::Malformed;
    }
    witness.producer = input.producer;
    witness.recordType = input.recordType;
    witness.rule = recordRule(input.recordType);
    const auto* policy = peSemanticProducerPolicy(input.recordType);
    witness.producerMatchesRecordType =
        policy != nullptr && policy->kind == input.producer;
    witness.payloadBytes = payloadBytes;
    witness.semanticBytes = input.constantBytes.size();
    witness.recordCount = recordCount_;
    witness.emissionHandleCount = emissionHandleCount_;
    witness.emissionPayloadBytes = emissionPayloadBytes_;
    if (!proveEmissionCapacity(witness, payloadBytes, 0u,
                               witness.semanticBytes)) {
      return PeSemanticAdmissionOutcome::Capacity;
    }
    witness.admissible = true;
    return PeSemanticAdmissionOutcome::Admissible;
  }

  // A typed adapter used by producer-family call sites. It intentionally
  // aliases admission rather than exposing the owner internals to producers.
  template <PeSemanticRecordInputLike Input>
  bool appendOwnedRecord(const Input& input) noexcept {
    return admit(input);
  }

  template <PeSemanticProducerKind Producer>
  bool appendOwnedRecord(
      const PeSemanticTypedConstantInput<Producer>& input) noexcept {
    return admit(input);
  }

  bool appendOwnedRecord(
      const PeSemanticConstantRecordInput& input) noexcept {
    return admit(input);
  }

 private:
  template <std::size_t IdentityCapacity, typename AdmissionPlanT>
  static std::uint32_t preparedRecordType(
      const PeSemanticPreparedRecordT<IdentityCapacity, AdmissionPlanT>& prepared) noexcept {
    return prepared.plan.recordType;
  }
  static std::uint32_t preparedRecordType(
      const PeSemanticPreparedConstantRecord& prepared) noexcept {
    return prepared.recordType;
  }

  // Owner-local implementation. The pure facts and qualified deltas never
  // leave tryAppendOwnedRecord(), so there is no caller-visible TOCTOU seam.
  template <bool Observe, bool NarrowConstantRollback = false,
            PeSemanticOwnedRecordInputLike Input,
            typename Prepared, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool appendPreparedRecord(const Input& input,
                            const Prepared& prepared,
                            Commit&& commit) noexcept {
    static_assert(!NarrowConstantRollback ||
                  (!Observe && PeSemanticConstantInputLike<Input> &&
                   std::is_same_v<Prepared, PeSemanticPreparedConstantRecord>));
    const auto runPhase = [&](PeSemanticOwnerPhase phase,
                              auto&& operation) noexcept {
      if constexpr (Observe) {
        auto scope = phaseObserver_->child(phase);
        return operation();
      } else {
        return operation();
      }
    };
    const auto rollbackPhase = [&](const auto& checkpoint) noexcept {
      if constexpr (Observe) {
        auto scope = phaseObserver_->child(PeSemanticOwnerPhase::Rollback);
        rollback(checkpoint);
      } else {
        rollback(checkpoint);
      }
    };
    return recordAdmission([&]() noexcept {
      lastAdmissionFailure_ = AdmissionFailure::None;
      if (!ready_) {
        lastAdmissionFailure_ = AdmissionFailure::Unavailable;
        return false;
      }
      if (prepared.producer != peSemanticProducer(input) ||
          preparedRecordType(prepared) != peSemanticRecordType(input) ||
          !prepared.admissibleAt(recordCount_, emissionHandleCount_,
                                 emissionPayloadBytes_)) {
        lastAdmissionFailure_ = AdmissionFailure::Header;
        return false;
      }
      const auto checkpoint = [&]() noexcept {
        if constexpr (NarrowConstantRollback)
          return checkpointConstantState();
        else
          return checkpointState();
      }();
      if constexpr (!NarrowConstantRollback)
        retainedCheckpoint_ = checkpoint.retained;
      if (!validAdmissionHeader(input, prepared)) {
        lastAdmissionFailure_ = AdmissionFailure::Header;
        rollbackPhase(checkpoint);
        return false;
      }
      // Issue the destination-bound authority only after source/header
      // requalification.  All prepared consumers below use this one proof;
      // no public operation accepts the private token, and the token never
      // crosses the settlement callback.
      // validAdmissionHeader() has just performed the fresh source proof;
      // compact staging has no retain/callback frontier before its claims.
      const PreparedAppendAuthority authority(&prepared, *this, input);
      using Staging = std::conditional_t<
          PeSemanticConstantInputLike<Input>,
          PeSemanticConstantRecordStaging,
          std::conditional_t<PeSemanticSparsePlanInputLike<Input>,
                             PeSemanticSparsePlanRecordStaging,
                             PeSemanticRecordSlot>>;
      Staging slot;
      if (!runPhase(PeSemanticOwnerPhase::FixedDirectPinCopy,
                    [&]() noexcept {
                      return copyPreparedFixedAndDirect(input, slot, prepared,
                                                        authority);
                    })) {
        rollbackPhase(checkpoint);
        return false;
      }
      // Values are staged only for this synchronous transaction. Once the
      // canonical roles are written, retain only the compact receipt below.
      if (!runPhase(PeSemanticOwnerPhase::SparseVariableCopy,
                    [&]() noexcept {
                      return copyPreparedSparseAndVariable(input, slot,
                                                           prepared, authority);
                    })) {
        if (lastAdmissionFailure_ == AdmissionFailure::None)
          lastAdmissionFailure_ = AdmissionFailure::Sparse;
        rollbackPhase(checkpoint);
        return false;
      }
      if (!runPhase(PeSemanticOwnerPhase::CanonicalMaterializationMetrics,
                    [&]() noexcept {
                      return materializePreparedCanonicalRecord(
                                 input, slot, prepared, authority) &&
                             cacheEmissionMetrics(prepared, authority);
                    })) {
        if (lastAdmissionFailure_ == AdmissionFailure::None)
          lastAdmissionFailure_ = AdmissionFailure::EmissionMetrics;
        rollbackPhase(checkpoint);
        return false;
      }
      storage_->records[recordCount_] = {
          .producer = peSemanticProducer(input),
          .recordType = peSemanticRecordType(input),
          .recordFlags = [&]() noexcept {
            if constexpr (PeSemanticSparsePlanInputLike<Input>)
              return input.recordFlags;
            else
              return stagedPeSemanticInput(input).recordFlags;
          }(),
          .sourceOrdinal = peSemanticSourceOrdinal(input),
          .recordOrdinal = peSemanticRecordOrdinal(input),
      };
      ++recordCount_;
      auto&& callback = commit;
      const bool settled = [&]() noexcept {
        if constexpr (Observe) {
          auto settlement = phaseObserver_->beginSettlement(
              peSemanticOwnerSettlementFamily(input));
          return callback();
        } else {
          return callback();
        }
      }();
      if (!settled) {
        lastAdmissionFailure_ = AdmissionFailure::Settlement;
        rollbackPhase(checkpoint);
        return false;
      }
      lastSourceOrdinal_ = peSemanticSourceOrdinal(input);
      lastRecordOrdinal_ = peSemanticRecordOrdinal(input);
      return true;
    });
  }

 public:
  // The producer settlement callback runs while the admission checkpoint is
  // still live, so any settlement failure rolls back atomically without
  // adding checkpoint state to the small owner shell.
  template <PeSemanticOwnedRecordInputLike Input, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool tryAppendOwnedRecord(const Input& input,
                            Commit&& commit) noexcept {
    if (phaseObserver_) {
      return tryAppendOwnedRecordObserved(input, std::forward<Commit>(commit));
    }
    return tryAppendOwnedRecordUnobserved(input, std::forward<Commit>(commit));
  }

  // The concrete operation can only validate the pending ticket and clear one
  // constant shadow. It has no owner or callback access, so the unobserved
  // path can checkpoint only constant admission effects. Arbitrary callbacks
  // still enter tryAppendOwnedRecord() and retain the full checkpoint.
  bool tryAppendConstantRecord(
      const PeSemanticConstantRecordInput& input,
      PeSemanticConstantSettlement& settlement) noexcept {
    if (phaseObserver_)
      return tryAppendOwnedRecordObserved(input, [&]() noexcept {
        return settlement.settle(input.producer, input.setConst,
                                 input.constantBytes);
      });
    return tryAppendConstantRecordUnobserved(input, settlement);
  }

  template <typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool tryAppendSparseStatePlan(
      const RecorderLockCapability& access, const SparseStatePlan& plan,
      PeSemanticProducerKind producer, std::uint32_t recordType,
      std::uint32_t recordFlags, std::uint64_t sourceOrdinal,
      std::uint64_t recordOrdinal, Commit&& commit) noexcept {
    const PeSemanticSparsePlanRecordInput input{
        .access = &access,
        .sparsePlan = &plan,
        .producer = producer,
        .recordType = recordType,
        .recordFlags = recordFlags,
        .sourceOrdinal = sourceOrdinal,
        .recordOrdinal = recordOrdinal,
    };
    return tryAppendOwnedRecord(input, std::forward<Commit>(commit));
  }

 private:
  // Keep the input-specific witness frame below the family dispatch. PE32
  // otherwise merges the generic/plan witness into that dispatcher and probes
  // more than one stack page even for a narrow constant record.
  template <PeSemanticOwnedRecordInputLike Input, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  __attribute__((noinline)) bool tryAppendOwnedRecordUnobserved(const Input& input,
                                      Commit&& commit) noexcept {
    using Prepared = std::conditional_t<
        PeSemanticConstantInputLike<Input>, PeSemanticPreparedConstantRecord,
        std::conditional_t<PeSemanticSparsePlanInputLike<Input>,
                           PeSemanticProductionCompactPreparedRecord,
                           PeSemanticProductionPreparedRecord>>;
    Prepared prepared;
    switch (prepareAdmission(input, prepared)) {
      case PeSemanticAdmissionOutcome::Admissible:
        break;
      case PeSemanticAdmissionOutcome::Unavailable:
        lastAdmissionFailure_ = AdmissionFailure::Unavailable;
        return false;
      case PeSemanticAdmissionOutcome::Malformed:
        lastAdmissionFailure_ = AdmissionFailure::Malformed;
        return false;
      case PeSemanticAdmissionOutcome::Capacity:
        lastAdmissionFailure_ = AdmissionFailure::Capacity;
        return false;
    }
    return appendPreparedRecord<false>(input, prepared,
                                       std::forward<Commit>(commit));
  }

  __attribute__((noinline)) bool tryAppendConstantRecordUnobserved(
      const PeSemanticConstantRecordInput& input,
      PeSemanticConstantSettlement& settlement) noexcept {
    PeSemanticPreparedConstantRecord prepared;
    switch (prepareAdmission(input, prepared)) {
      case PeSemanticAdmissionOutcome::Admissible:
        break;
      case PeSemanticAdmissionOutcome::Unavailable:
        lastAdmissionFailure_ = AdmissionFailure::Unavailable;
        return false;
      case PeSemanticAdmissionOutcome::Malformed:
        lastAdmissionFailure_ = AdmissionFailure::Malformed;
        return false;
      case PeSemanticAdmissionOutcome::Capacity:
        lastAdmissionFailure_ = AdmissionFailure::Capacity;
        return false;
    }
    return appendPreparedRecord<false, true>(input, prepared, [&]() noexcept {
      return settlement.settle(input.producer, input.setConst,
                               input.constantBytes);
    });
  }

  static PeSemanticOwnerOutcome phaseOutcome(AdmissionFailure failure) noexcept {
    switch (failure) {
    case AdmissionFailure::Unavailable: return PeSemanticOwnerOutcome::Unavailable;
    case AdmissionFailure::Capacity: return PeSemanticOwnerOutcome::Capacity;
    case AdmissionFailure::Malformed: return PeSemanticOwnerOutcome::Malformed;
    case AdmissionFailure::Header: return PeSemanticOwnerOutcome::Header;
    case AdmissionFailure::Fixed: return PeSemanticOwnerOutcome::Fixed;
    case AdmissionFailure::DirectPins: return PeSemanticOwnerOutcome::DirectPins;
    case AdmissionFailure::Sparse:
    case AdmissionFailure::SparseSchema:
    case AdmissionFailure::SparseArena:
    case AdmissionFailure::SparseTextures:
    case AdmissionFailure::SparseStreams:
    case AdmissionFailure::SparseShaders:
    case AdmissionFailure::SparseVertexInputs:
    case AdmissionFailure::SparseIndexBuffers:
    case AdmissionFailure::SparseRenderTargets:
    case AdmissionFailure::SparseDepthStencils:
      return PeSemanticOwnerOutcome::Sparse;
    case AdmissionFailure::VariablePayload:
      return PeSemanticOwnerOutcome::VariablePayload;
    case AdmissionFailure::EmissionMetrics:
      return PeSemanticOwnerOutcome::EmissionMetrics;
    case AdmissionFailure::Settlement:
      return PeSemanticOwnerOutcome::Settlement;
    case AdmissionFailure::None: break;
    }
    return PeSemanticOwnerOutcome::Other;
  }

  template <PeSemanticOwnedRecordInputLike Input, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  __attribute__((noinline)) bool tryAppendOwnedRecordObserved(const Input& input,
                                    Commit&& commit) noexcept {
    auto parent = phaseObserver_->beginAppend();
    using Prepared = std::conditional_t<
        PeSemanticConstantInputLike<Input>, PeSemanticPreparedConstantRecord,
        std::conditional_t<PeSemanticSparsePlanInputLike<Input>,
                           PeSemanticProductionCompactPreparedRecord,
                           PeSemanticProductionPreparedRecord>>;
    Prepared prepared;
    PeSemanticAdmissionOutcome outcome;
    PeSemanticOwnerAdmissionDistribution distribution{};
    bool measured = false;
    {
      auto phase = phaseObserver_->child(PeSemanticOwnerPhase::PrepareAdmission);
      outcome = prepareAdmission(input, prepared);
    }
    if (parent.sampled() && outcome == PeSemanticAdmissionOutcome::Admissible) {
      measured = detail::reconstructPeSemanticOwnerAdmission(
          input, prepared, distribution);
      if constexpr (PeSemanticSparsePlanInputLike<Input>) {
        static_assert(kSectionRules.size() ==
                      PeSemanticOwnerCompactLayoutMetric::kSectionKindCount);
        static_assert([] {
          for (std::size_t i = 0u; i < kSectionRules.size(); ++i) {
            for (std::size_t j = i + 1u; j < kSectionRules.size(); ++j) {
              if (kSectionRules[i].kind == kSectionRules[j].kind) return false;
            }
          }
          return true;
        }());
        const auto& layout = prepared.sparseLayout;
        phaseObserver_->recordCompactLayoutDistribution(
            std::span<const PeSemanticPreparedSection>(layout.sections),
            layout.sectionCount, std::span<const SectionRule>(kSectionRules));
      }
    }
    if (outcome != PeSemanticAdmissionOutcome::Admissible) {
      switch (outcome) {
      case PeSemanticAdmissionOutcome::Unavailable:
        lastAdmissionFailure_ = AdmissionFailure::Unavailable;
        break;
      case PeSemanticAdmissionOutcome::Malformed:
        lastAdmissionFailure_ = AdmissionFailure::Malformed;
        break;
      case PeSemanticAdmissionOutcome::Capacity:
        lastAdmissionFailure_ = AdmissionFailure::Capacity;
        break;
      case PeSemanticAdmissionOutcome::Admissible: break;
      }
      phaseObserver_->recordOutcome(phaseOutcome(lastAdmissionFailure_));
      phaseObserver_->recordAdmissionAttempt(
          peSemanticOwnerAdmissionFamily(input), parent.sampled(), false, false,
          false);
      return false;
    }
    const bool accepted = appendPreparedRecord<true>(
        input, prepared, std::forward<Commit>(commit));
    phaseObserver_->recordOutcome(
        accepted ? PeSemanticOwnerOutcome::Accepted
                 : phaseOutcome(lastAdmissionFailure_));
    phaseObserver_->recordAdmissionAttempt(
        peSemanticOwnerAdmissionFamily(input), parent.sampled(), true, accepted,
        measured);
    if (parent.sampled() && measured) {
      phaseObserver_->recordAdmissionDistribution(
          peSemanticOwnerAdmissionFamily(input), distribution);
    }
    return accepted;
  }

 public:
  // Compatibility adapter for existing cold/oracle callers.
  template <PeSemanticRecordInputLike Input, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool appendOwnedRecord(const Input& input,
                         Commit&& commit) noexcept {
    return tryAppendOwnedRecord(input, std::forward<Commit>(commit));
  }

  template <PeSemanticProducerKind Producer, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool appendOwnedRecord(const PeSemanticTypedConstantInput<Producer>& input,
                         Commit&& commit) noexcept {
    return tryAppendOwnedRecord(input.asConstant(),
                                std::forward<Commit>(commit));
  }

  template <typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool appendOwnedRecord(const PeSemanticConstantRecordInput& input,
                         Commit&& commit) noexcept {
    return tryAppendOwnedRecord(input, std::forward<Commit>(commit));
  }

  template <typename Visit>
    requires std::is_nothrow_invocable_r_v<
        bool, Visit&, PeSemanticProducerKind,
        const PeSemanticRecordReceipt&>
  bool visitOwnedRecords(Visit&& visit) const noexcept {
    if (!ready_ || recordCount_ == 0u) return false;
    auto&& visitor = visit;
    for (std::size_t i = 0u; i < recordCount_; ++i) {
      if (!visitor(storage_->records[i].producer, storage_->records[i]))
        return false;
    }
    return true;
  }

  // Admission is atomic.  Every counter and every typed pin is restored when
  // any validation, arena, or retain operation fails.
  template <PeSemanticRecordInputLike Input>
  bool admit(const Input& input) noexcept {
    // Keep the test/oracle entry point on the same prepared production path.
    // There is no second legacy materialization route to drift from the
    // canonical role bytes emitted by ExactFixed.
    return tryAppendOwnedRecord(input, []() noexcept { return true; });
  }

  template <PeSemanticProducerKind Producer>
  bool admit(const PeSemanticTypedConstantInput<Producer>& input) noexcept {
    return tryAppendOwnedRecord(input.asConstant(),
                                []() noexcept { return true; });
  }

  bool admit(const PeSemanticConstantRecordInput& input) noexcept {
    return tryAppendOwnedRecord(input, []() noexcept { return true; });
  }

  // Destructive reset used by Reset/teardown/discard. It drops both current
  // and warm retainer pins; successful chunk settlement uses settle() so the
  // legacy warm-pin epoch optimization remains intact.
  void reset() noexcept {
    retainer_.clear();
    clearChunkState();
  }

  // Successful settlement closes one chunk epoch while preserving recently
  // used retainer entries. The typed slots and arenas are still cleared, so a
  // later chunk reacquires its own slot; the retainer index refreshes the warm
  // entry without another AddRef. This keeps settlement O(1) in owner state
  // plus the retainer's bounded epoch sweep, matching CommandChunkBuilder.
  bool settle() noexcept {
    if (phaseObserver_) return settleObserved();
    return settleUnobserved();
  }

  template <bool Observe>
  bool settleBody() noexcept {
    if constexpr (Observe) {
      auto settlePhase = phaseObserver_->beginOperation(
          PeSemanticOwnerPhase::SettleClear);
      if (!ready_ || recordCount_ == 0u) return false;
      {
        auto retainerPhase = phaseObserver_->child(
            PeSemanticOwnerPhase::SettleRetainerEpoch);
        retainer_.endEpoch();
      }
      {
        auto ownerResetPhase = phaseObserver_->child(
            PeSemanticOwnerPhase::SettleOwnerReset);
        clearChunkState();
      }
      ++settledChunks_;
      return true;
    }
    if (!ready_ || recordCount_ == 0u) return false;
    retainer_.endEpoch();
    clearChunkState();
    ++settledChunks_;
    return true;
  }

  bool settleObserved() noexcept { return settleBody<true>(); }
  bool settleUnobserved() noexcept { return settleBody<false>(); }

 private:
  void clearChunkState() noexcept {
    releaseLedgerRetention();
    lastClearedBytes_ = 0u;
    if (!ready_) return;
    // Counts and validity frontiers revoke the old values. Every successful
    // producer overwrites the complete published prefix before its count is
    // advanced, while canonical materialization writes all bytes (including
    // alignment gaps). Index entries are revoked by a generation bump; the
    // inactive tables remain untouched and are physically cleared only when
    // the bounded generation domain wraps.
    lastClearedBytes_ = invalidatePinIndexes();
    recordCount_ = 0u;
    semanticBytes_ = 0u;
    emissionHandleCount_ = 0u;
    emissionPayloadBytes_ = 0u;
    rectCount_ = 0u;
    sparseCounts_ = {};
    surfaceCount_ = textureCount_ = bufferCount_ = shaderCount_ =
        declarationCount_ = queryCount_ = 0u;
  }

 public:

  std::size_t size() const noexcept { return recordCount_; }
  std::size_t retainedCount() const noexcept { return retainer_.size(); }
  // The dependency query is part of the producer projection path.  Keep it
  // qualified by the complete wire identity: object-id-only or pointer-only
  // answers can incorrectly carry a stale generation across a wrapper reuse.
  bool referencesBuffer(const BufferRef& ref) const noexcept {
    if (!ref.valid()) return false;
    std::size_t pin = 0u;
    bool found = false;
    if (!checkedPinHit(storage_->bufferPinIndex, storage_->buffers,
                       bufferCount_, ref.identity, ref.object, found, pin) ||
        !found) {
      return false;
    }
    return true;
  }

  template <typename Visit>
    requires std::is_nothrow_invocable_r_v<
        bool, Visit&, const CommittedPendingChunkLease&>
  bool visitCommittedPendingChunkLease(const PeWireObjectRef& expected,
                                       Visit&& visit) const noexcept {
    if (!expected.object ||
        expected.identity.kind > D9C_CHUNK_HANDLE_KIND_QUERY ||
        expected.identity.generation == 0u ||
        expected.identity.objectId == 0u) {
      return false;
    }
    bool committed = false;
    for (std::size_t record = 0u; record < recordCount_; ++record) {
      if (!visitRecordHandles(
              storage_->records[record],
              [&](const D9CWireObjectIdentity& identity) noexcept {
                if (identity.kind == expected.identity.kind &&
                    identity.generation == expected.identity.generation &&
                    identity.objectId == expected.identity.objectId) {
                  committed = true;
                }
                return true;
              })) {
        return false;
      }
    }
    if (!committed) return false;
    const auto match = [&](const auto& pins, std::size_t count) noexcept {
      for (std::size_t i = 0u; i < count; ++i) {
        const auto& pin = pins[i];
        if (!pin.valid() || pin.object != expected.object ||
            !(pin.identity.value.kind == expected.identity.kind &&
              pin.identity.value.generation == expected.identity.generation &&
              pin.identity.value.objectId == expected.identity.objectId)) {
          continue;
        }
        const CommittedPendingChunkLease lease(expected);
        return visit(lease);
      }
      return false;
    };
    switch (expected.identity.kind) {
      case D9C_CHUNK_HANDLE_KIND_SURFACE:
        return match(storage_->surfaces, surfaceCount_);
      case D9C_CHUNK_HANDLE_KIND_TEXTURE:
        return match(storage_->textures, textureCount_);
      case D9C_CHUNK_HANDLE_KIND_BUFFER:
        return match(storage_->buffers, bufferCount_);
      case D9C_CHUNK_HANDLE_KIND_SHADER:
        return match(storage_->shaders, shaderCount_);
      case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
        return match(storage_->declarations, declarationCount_);
      case D9C_CHUNK_HANDLE_KIND_QUERY:
        return match(storage_->queries, queryCount_);
      default:
        return false;
    }
  }

  template <typename Visit>
    requires std::is_nothrow_invocable_v<
        Visit&, const CommittedPendingChunkLease&>
  void visitCommittedPendingChunkLeases(Visit&& visit) const noexcept {
    const auto each = [&](const auto& pins, std::size_t count) noexcept {
      for (std::size_t i = 0u; i < count; ++i) {
        const auto& pin = pins[i];
        if (!pin.valid()) continue;
        bool committed = false;
        for (std::size_t record = 0u; record < recordCount_; ++record) {
          if (!visitRecordHandles(
                  storage_->records[record],
                  [&](const D9CWireObjectIdentity& identity) noexcept {
                    if (identity.kind == pin.identity.value.kind &&
                        identity.generation == pin.identity.value.generation &&
                        identity.objectId == pin.identity.value.objectId) {
                      committed = true;
                    }
                    return true;
                  })) {
            return;
          }
        }
        if (!committed) continue;
        visit(CommittedPendingChunkLease(PeWireObjectRef{
            .identity = pin.identity.value,
            .object = pin.object}));
      }
    };
    each(storage_->surfaces, surfaceCount_);
    each(storage_->textures, textureCount_);
    each(storage_->buffers, bufferCount_);
    each(storage_->shaders, shaderCount_);
    each(storage_->declarations, declarationCount_);
    each(storage_->queries, queryCount_);
  }
  std::size_t semanticBytes() const noexcept { return semanticBytes_; }
  std::size_t clearedBytesLastBoundary() const noexcept {
    return lastClearedBytes_;
  }
  std::size_t rectCount() const noexcept { return rectCount_; }
  std::uint64_t settledChunks() const noexcept { return settledChunks_; }
  std::uint64_t nextSourceOrdinal() const noexcept {
    return lastSourceOrdinal_ == std::numeric_limits<std::uint64_t>::max()
        ? 0u : lastSourceOrdinal_ + 1u;
  }
  std::uint64_t nextRecordOrdinal() const noexcept {
    return lastRecordOrdinal_ == std::numeric_limits<std::uint64_t>::max()
        ? 0u : lastRecordOrdinal_ + 1u;
  }
  bool producerIdentity(
      D9CCommandChunkProducerIdentity& identity) const noexcept {
    identity = {};
    if (!ready_ || recordCount_ == 0u ||
        settledChunks_ == std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }
    const std::uint64_t first = storage_->records[0].sourceOrdinal;
    const std::uint64_t last = storage_->records[recordCount_ - 1u].sourceOrdinal;
    if (first == 0u || last < first) return false;
    identity = {
        .firstEventOrdinal = settledChunks_ + 1u,
        .lastEventOrdinal = settledChunks_ + 1u,
        .firstSourceOrdinal = first,
        .lastSourceOrdinal = last,
    };
    return true;
  }
  bool emissionMetrics(std::size_t& handles, std::size_t& payload,
                       std::size_t& wire) const noexcept {
    // Admission maintains these frontiers transactionally, so recorder
    // checkpoints and cadence probes are O(1). The full record walk remains
    // reserved for the final immutable emission validation/write.
    if (!ready_ || recordCount_ == 0u ||
        emissionHandleCount_ > std::numeric_limits<std::uint32_t>::max() ||
        emissionPayloadBytes_ > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    const auto layout = planExactCommandChunkLayout(
        static_cast<std::uint32_t>(recordCount_),
        static_cast<std::uint32_t>(emissionHandleCount_),
        static_cast<std::uint32_t>(emissionPayloadBytes_));
    if (!layout.valid() || layout.totalBytes > D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES)
      return false;
    handles = emissionHandleCount_;
    payload = emissionPayloadBytes_;
    wire = layout.totalBytes;
    return true;
  }

  // Recorder checkpoints and cadence probes only need the maintained logical
  // frontiers.  Keep exact wire planning in emissionMetrics() as the checked
  // cold reference and in the final emitter, where wire bytes are consumed.
  // Private admission proves the exact layout before publishing these counts;
  // rollback/reset/settle restore or clear that already-proved frontier.
  bool emissionFrontier(PeSemanticEmissionFrontier& out) const noexcept {
    out = {};
    if (!ready_ || recordCount_ == 0u ||
        recordCount_ > std::numeric_limits<std::uint32_t>::max() ||
        emissionHandleCount_ > std::numeric_limits<std::uint32_t>::max() ||
        emissionPayloadBytes_ > std::numeric_limits<std::uint32_t>::max() ||
        !fitsStagedWire(emissionHandleCount_, emissionPayloadBytes_)) {
      return false;
    }
    out = {.recordCount = recordCount_,
           .handleCount = emissionHandleCount_,
           .payloadBytes = emissionPayloadBytes_};
    return true;
  }

  // Normal P2 target: records, handles, and payload are independently owned
  // fixed roles. No header or role padding is copied into those spans.
  bool emitSegmented(std::span<std::byte> recordRegion,
                     std::span<std::byte> handleRegion,
                     std::span<std::byte> payloadRegion,
                     PeSemanticSegmentedEmission& out,
                     std::uint64_t renderTapeCaptureToken = 0u,
                     std::uint64_t renderTapeEventOrdinal = 0u) const noexcept {
    if (phaseObserver_)
      return emitSegmentedExternalObserved(recordRegion, handleRegion,
                                           payloadRegion, out,
                                           renderTapeCaptureToken,
                                           renderTapeEventOrdinal);
    return emitSegmentedExternalUnobserved(recordRegion, handleRegion,
                                            payloadRegion, out,
                                            renderTapeCaptureToken,
                                            renderTapeEventOrdinal);
  }

  template <bool Observe>
  bool emitSegmentedExternalCore(
      std::span<std::byte> recordRegion, std::span<std::byte> handleRegion,
      std::span<std::byte> payloadRegion, PeSemanticSegmentedEmission& out,
      std::uint64_t renderTapeCaptureToken,
      std::uint64_t renderTapeEventOrdinal) const noexcept {
    if constexpr (Observe) {
      auto parent = phaseObserver_->beginOperation(
          PeSemanticOwnerPhase::EmitSegmentedExternalCopy);
      return emitSegmentedExternalBody<true>(
          recordRegion, handleRegion, payloadRegion, out,
          renderTapeCaptureToken, renderTapeEventOrdinal);
    }
    return emitSegmentedExternalBody<false>(
        recordRegion, handleRegion, payloadRegion, out,
        renderTapeCaptureToken, renderTapeEventOrdinal);
  }

  template <bool Observe>
  bool emitSegmentedExternalBody(
      std::span<std::byte> recordRegion, std::span<std::byte> handleRegion,
      std::span<std::byte> payloadRegion, PeSemanticSegmentedEmission& out,
      std::uint64_t renderTapeCaptureToken,
      std::uint64_t renderTapeEventOrdinal) const noexcept {
    out = {};
    EmissionPlan plan{};
    if (!buildEmissionPlan(plan) ||
        recordRegion.size() < plan.recordBytes() ||
        handleRegion.size() < plan.handleBytes() ||
        payloadRegion.size() < plan.payloadBytes) {
      return false;
    }
    if constexpr (Observe) {
      auto roleCopy = phaseObserver_->child(
          PeSemanticOwnerPhase::EmitSegmentedExternalRoleCopy);
      if (!copyCanonicalRoles(plan, recordRegion, handleRegion, payloadRegion)) {
        return false;
      }
    } else if (!copyCanonicalRoles(plan, recordRegion, handleRegion,
                                   payloadRegion)) {
      return false;
    }
    out.transport = makeTransport(plan,
                                  recordRegion.first(plan.recordBytes()),
                                  handleRegion.first(plan.handleBytes()),
                                  payloadRegion.first(plan.payloadBytes),
                                  renderTapeCaptureToken,
                                  renderTapeEventOrdinal);
    if (!producerIdentity(out.transport.producerIdentity)) return false;
    out.wireBytes = plan.wireBytes;
    return out.valid();
  }

  bool emitSegmentedExternalObserved(
      std::span<std::byte> recordRegion, std::span<std::byte> handleRegion,
      std::span<std::byte> payloadRegion, PeSemanticSegmentedEmission& out,
      std::uint64_t captureToken, std::uint64_t eventOrdinal) const noexcept {
    return emitSegmentedExternalCore<true>(
        recordRegion, handleRegion, payloadRegion, out, captureToken,
        eventOrdinal);
  }

  bool emitSegmentedExternalUnobserved(
      std::span<std::byte> recordRegion, std::span<std::byte> handleRegion,
      std::span<std::byte> payloadRegion, PeSemanticSegmentedEmission& out,
      std::uint64_t captureToken, std::uint64_t eventOrdinal) const noexcept {
    return emitSegmentedExternalCore<false>(
        recordRegion, handleRegion, payloadRegion, out, captureToken,
        eventOrdinal);
  }

  // Canonical contiguous D9C V2 target used by the default production and
  // capture lanes. Admission has already materialized the immutable role
  // bytes. ExactFixed therefore only writes the final header and gathers the
  // three fixed roles; it never replays semantic records or recomputes them.
  bool emitExactFixed(std::span<std::byte> destination,
                      PeSemanticExactFixedEmission& out) const noexcept {
    out = {};
    if (phaseObserver_)
      return emitExactFixedObserved(destination, out);
    return emitExactFixedUnobserved(destination, out);
  }

  bool emitExactFixed(PeSemanticExactFixedEmission& out) const noexcept {
    if (!emitExactFixed(std::span<std::byte>(storage_->wire), out)) return false;
    auto* ledger = LedgerAccess::resolve(
        dxmt9::core::CopyMaterializationOwner::Pe);
    if (!ledger) return true;
    if (exactWireLedger_ && exactWireRetainedBytes_ != 0u) {
      exactWireLedger_->release(
          dxmt9::core::CopyMaterializationClass::PeWireFinal,
          exactWireRetainedBytes_);
    }
    ledger->retain(dxmt9::core::CopyMaterializationClass::PeWireFinal,
                   out.wireBytes);
    exactWireLedger_ = ledger;
    exactWireRetainedBytes_ = out.wireBytes;
    return true;
  }

  // Production segmented target aliases the fixed role regions materialized
  // at admission. Capture callers deliberately use emitExactFixed() instead.
  bool emitSegmented(PeSemanticSegmentedEmission& out,
                     std::uint64_t renderTapeCaptureToken = 0u,
                     std::uint64_t renderTapeEventOrdinal = 0u) const noexcept {
    if (phaseObserver_)
      return emitSegmentedAliasObserved(out, renderTapeCaptureToken,
                                        renderTapeEventOrdinal);
    return emitSegmentedAliasUnobserved(out, renderTapeCaptureToken,
                                        renderTapeEventOrdinal);
  }

  template <bool Observe>
  bool emitSegmentedAliasCore(
      PeSemanticSegmentedEmission& out, std::uint64_t captureToken,
      std::uint64_t eventOrdinal) const noexcept {
    if constexpr (Observe) {
      auto parent = phaseObserver_->beginOperation(
          PeSemanticOwnerPhase::EmitSegmentedAliasView);
      return emitSegmentedAliasBody<true>(out, captureToken, eventOrdinal);
    }
    return emitSegmentedAliasBody<false>(out, captureToken, eventOrdinal);
  }

  template <bool Observe>
  bool emitSegmentedAliasBody(
      PeSemanticSegmentedEmission& out, std::uint64_t captureToken,
      std::uint64_t eventOrdinal) const noexcept {
    out = {};
    EmissionPlan plan{};
    if (!buildEmissionPlan(plan)) return false;
    const auto records = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(storage_->wireRecords.data()),
        plan.recordBytes());
    const auto handles = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(storage_->wireHandles.data()),
        plan.handleBytes());
    const auto payload = std::span<const std::byte>(
        storage_->canonicalPayload.data(), plan.payloadBytes);
    out.transport = makeTransport(
        plan,
        records, handles, payload,
        captureToken, eventOrdinal);
    if (!producerIdentity(out.transport.producerIdentity)) return false;
    out.wireBytes = plan.wireBytes;
    return out.valid();
  }

  bool emitSegmentedAliasObserved(
      PeSemanticSegmentedEmission& out, std::uint64_t captureToken,
      std::uint64_t eventOrdinal) const noexcept {
    return emitSegmentedAliasCore<true>(out, captureToken, eventOrdinal);
  }

  bool emitSegmentedAliasUnobserved(
      PeSemanticSegmentedEmission& out, std::uint64_t captureToken,
      std::uint64_t eventOrdinal) const noexcept {
    return emitSegmentedAliasCore<false>(out, captureToken, eventOrdinal);
  }

  const PeSemanticRecordReceipt& record(std::size_t index) const noexcept {
    return storage_->records[index];
  }
  const PeSemanticPhysicalPin<D9CSurface, PeSemanticSurfaceIdentity>&
  surfacePin(std::size_t index) const noexcept { return storage_->surfaces[index]; }
  const PeSemanticPhysicalPin<D9CTexture, PeSemanticTextureIdentity>&
  texturePin(std::size_t index) const noexcept { return storage_->textures[index]; }
  const PeSemanticPhysicalPin<D9CBuffer, PeSemanticBufferIdentity>&
  bufferPin(std::size_t index) const noexcept { return storage_->buffers[index]; }
  const PeSemanticPhysicalPin<D9CShader, PeSemanticShaderIdentity>&
  shaderPin(std::size_t index) const noexcept { return storage_->shaders[index]; }
  const PeSemanticPhysicalPin<D9CVertexDecl, PeSemanticDeclarationIdentity>&
  declarationPin(std::size_t index) const noexcept { return storage_->declarations[index]; }
  const PeSemanticPhysicalPin<D9CQuery, PeSemanticQueryIdentity>&
  queryPin(std::size_t index) const noexcept { return storage_->queries[index]; }

  std::span<const D9CCommandChunkWireTextureBinding>
  textureBindings(const PeSemanticRecordReceipt& slot) const noexcept {
    return canonicalBindingRows<D9CCommandChunkWireTextureBinding>(slot,
        D9C_COMMAND_CHUNK_SECTION_TEXTURE);
  }
  std::span<const D9CCommandChunkWireStreamBinding>
  streamBindings(const PeSemanticRecordReceipt& slot) const noexcept {
    return canonicalBindingRows<D9CCommandChunkWireStreamBinding>(slot,
        D9C_COMMAND_CHUNK_SECTION_STREAM);
  }
  std::span<const D9CCommandChunkWireShaderBinding>
  shaderBindings(const PeSemanticRecordReceipt& slot) const noexcept {
    return canonicalBindingRows<D9CCommandChunkWireShaderBinding>(slot,
        D9C_COMMAND_CHUNK_SECTION_SHADER);
  }
  std::span<const D9CCommandChunkWireVertexInput>
  vertexInputs(const PeSemanticRecordReceipt& slot) const noexcept {
    return canonicalBindingRows<D9CCommandChunkWireVertexInput>(slot,
        D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT);
  }
  std::span<const D9CCommandChunkWireIndexBinding>
  indexBuffers(const PeSemanticRecordReceipt& slot) const noexcept {
    return canonicalBindingRows<D9CCommandChunkWireIndexBinding>(slot,
        D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER);
  }
  std::span<const D9CCommandChunkWireRenderTargetBinding>
  renderTargets(const PeSemanticRecordReceipt& slot) const noexcept {
    return canonicalBindingRows<D9CCommandChunkWireRenderTargetBinding>(slot,
        D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET);
  }
  std::span<const D9CCommandChunkWireDepthStencilBinding>
  depthStencils(const PeSemanticRecordReceipt& slot) const noexcept {
    return canonicalBindingRows<D9CCommandChunkWireDepthStencilBinding>(slot,
        D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL);
  }

 private:
  struct ConstantStateCheckpoint {
    std::size_t records = 0u;
    std::size_t semanticBytes = 0u;
    std::size_t emissionHandles = 0u;
    std::size_t emissionPayloadBytes = 0u;
  };

  struct StateCheckpoint {
    std::size_t records = 0u;
    std::size_t semanticBytes = 0u;
    std::size_t emissionHandles = 0u;
    std::size_t emissionPayloadBytes = 0u;
    std::size_t rects = 0u;
    std::array<std::size_t, 17u> sparse{};
    std::array<std::size_t, 6u> pins{};
    D3D9PePendingCommandRetainer::Acquired retained{};
  };

  struct SparseCounts {
    std::array<std::size_t, 17u> values{};
  };

  static constexpr std::size_t kMaxRecordHandles =
      kPeSemanticMaxRecordHandles;
  // Pin admission is on the producer hot path. Keep one kind-local object-ID
  // table per typed pin arena so a repeated warm pin does not scan every
  // prior pin. The table is deliberately over-provisioned; probing is bounded
  // by this compile-time constant and never allocates. A nonzero generation
  // qualifies a live bucket, allowing reset/rollback to revoke an inactive
  // table without clearing its tail.
  static constexpr std::size_t kPinIndexCapacity = MaxPins * 4u + 1u;

  static bool fitsStagedWire(std::size_t handleCount,
                             std::size_t payloadBytes) noexcept {
    return handleCount <= Storage::maxWireHandles &&
           payloadBytes <= Storage::maxWirePayloadBytes;
  }

  template <std::size_t N>
  using PinIndex = PeSemanticPinIndex<N>;

  struct EmissionPlan {
    D9CCommandChunkWireHeader header{};
    std::size_t recordCount = 0u;
    std::size_t handleCount = 0u;
    std::size_t payloadBytes = 0u;
    std::size_t wireBytes = 0u;

    std::size_t recordBytes() const noexcept {
      return recordCount * sizeof(D9CCommandChunkWireRecordHeader);
    }
    std::size_t handleBytes() const noexcept {
      return handleCount * sizeof(D9CCommandChunkWireHandleEntry);
    }
  };

  template <bool Observe>
  bool emitExactFixedCore(const EmissionPlan& plan,
                          std::span<std::byte> destination,
                          PeSemanticExactFixedEmission& out) const noexcept {
    if (destination.size() < plan.wireBytes ||
        reinterpret_cast<std::uintptr_t>(destination.data()) %
                alignof(D9CCommandChunkWireHandleEntry) != 0u) {
      return false;
    }
    const auto emit = [&]() noexcept {
      std::memcpy(destination.data(), &plan.header, sizeof(plan.header));
      auto records = destination.subspan(plan.header.recordTableOffset,
                                         plan.recordBytes());
      auto handles = destination.subspan(plan.header.handleTableOffset,
                                         plan.handleBytes());
      auto payload = destination.subspan(plan.header.payloadArenaOffset,
                                         plan.payloadBytes);
      const auto zeroGap = [&](std::size_t begin, std::size_t end) noexcept {
        if (begin > end || end > plan.wireBytes) return false;
        std::fill(destination.begin() + begin, destination.begin() + end,
                  std::byte{0});
        return true;
      };
      if (!zeroGap(sizeof(plan.header), plan.header.recordTableOffset) ||
          !zeroGap(plan.header.recordTableOffset + plan.recordBytes(),
                   plan.header.handleTableOffset) ||
          !zeroGap(plan.header.handleTableOffset + plan.handleBytes(),
                   plan.header.payloadArenaOffset)) {
        return false;
      }
      if constexpr (Observe) {
        auto roleCopy = phaseObserver_->child(
            PeSemanticOwnerPhase::EmitExactFixedRoleCopy);
        if (!copyCanonicalRoles(plan, records, handles, payload)) return false;
      } else if (!copyCanonicalRoles(plan, records, handles, payload)) {
        return false;
      }
      out.transport = makeTransport(plan, records, handles, payload, 0u, 0u);
      if (!producerIdentity(out.transport.producerIdentity)) return false;
      out.wire = std::span<const std::byte>(destination.data(), plan.wireBytes);
      out.wireBytes = plan.wireBytes;
      return out.valid();
    };
    auto* const ledger = LedgerAccess::resolve(
        dxmt9::core::CopyMaterializationOwner::Pe);
    if (!ledger) return emit();
    dxmt9::core::CopyMaterializationEvent event(
        ledger, dxmt9::core::CopyMaterializationClass::PeWireFinal,
        plan.wireBytes);
    if (!emit()) {
      event.cancel();
      return false;
    }
    event.commit();
    return true;
  }

  bool emitExactFixedObserved(
      std::span<std::byte> destination,
      PeSemanticExactFixedEmission& out) const noexcept {
    auto parent = phaseObserver_->beginOperation(
        PeSemanticOwnerPhase::EmitExactFixed);
    EmissionPlan plan{};
    {
      auto buildPlan = phaseObserver_->child(
          PeSemanticOwnerPhase::EmitExactFixedBuildPlan);
      if (!buildEmissionPlan(plan)) return false;
    }
    return emitExactFixedCore<true>(plan, destination, out);
  }

  bool emitExactFixedUnobserved(
      std::span<std::byte> destination,
      PeSemanticExactFixedEmission& out) const noexcept {
    EmissionPlan plan{};
    if (!buildEmissionPlan(plan)) return false;
    return emitExactFixedCore<false>(plan, destination, out);
  }

  static std::size_t pinHash(const D9CWireObjectIdentity& identity) noexcept {
    return static_cast<std::size_t>(
        identity.objectId * 0x9e3779b185ebca87ull);
  }

  template <std::size_t N>
  static bool findPinIndex(const PinIndex<N>& index,
                           const D9CWireObjectIdentity& identity,
                           std::uint32_t generation,
                           std::size_t& out,
                           const PeSemanticPinIndexEntry*& outEntry) noexcept {
    std::size_t cursor = pinHash(identity) % N;
    for (std::size_t probe = 0u; probe < N; ++probe) {
      const auto& entry = index[cursor];
      if (entry.generation != generation) return false;
      // A matching objectId is a hit even when its payload is malformed. The
      // caller validates kind, generation and pointer against the live
      // typed prefix before using the pin.
      if (entry.identity.objectId == identity.objectId) {
        out = entry.pin;
        outEntry = &entry;
        return true;
      }
      if (cursor == N - 1u)
        cursor = 0u;
      else
        ++cursor;
    }
    return false;
  }

  template <std::size_t N>
  static bool insertPinIndex(PinIndex<N>& index,
                             const D9CWireObjectIdentity& identity,
                             std::uint32_t pin,
                             std::uint32_t generation) noexcept {
    std::size_t cursor = pinHash(identity) % N;
    for (std::size_t probe = 0u; probe < N; ++probe) {
      auto& entry = index[cursor];
      if (entry.generation == 0u || entry.generation != generation ||
          entry.identity.objectId == identity.objectId) {
        entry.identity = identity;
        entry.pin = pin;
        entry.generation = generation;
        return true;
      }
      if (cursor == N - 1u)
        cursor = 0u;
      else
        ++cursor;
    }
    return false;
  }

  template <std::size_t N>
  static std::size_t clearPinIndex(PinIndex<N>& index) noexcept {
    for (auto& entry : index) entry = {};
    return N * sizeof(typename PinIndex<N>::value_type);
  }

  template <typename PinArray, std::size_t N>
  static void rebuildPinIndex(const PinArray& pins, std::size_t count,
                              PinIndex<N>& index,
                              std::uint32_t generation) noexcept {
    for (std::size_t i = 0u; i < count; ++i) {
      if (!pins[i].valid()) continue;
      (void)insertPinIndex(index, pins[i].identity.value,
                           static_cast<std::uint32_t>(i), generation);
    }
  }

  std::size_t clearPinIndexes() noexcept {
    return clearPinIndex(storage_->surfacePinIndex) +
           clearPinIndex(storage_->texturePinIndex) +
           clearPinIndex(storage_->bufferPinIndex) +
           clearPinIndex(storage_->shaderPinIndex) +
           clearPinIndex(storage_->declarationPinIndex) +
           clearPinIndex(storage_->queryPinIndex);
  }

  std::size_t invalidatePinIndexes() noexcept {
    if (pinIndexGeneration_ != std::numeric_limits<std::uint32_t>::max()) {
      ++pinIndexGeneration_;
      return 0u;
    }
    // Generation zero is reserved for never-published slots. A physical
    // clear is only needed on the bounded generation-wrap path; ordinary
    // reset/rollback invalidates all old entries with one generation bump.
    const auto clearedBytes = clearPinIndexes();
    pinIndexGeneration_ = 1u;
    return clearedBytes;
  }

  void rebuildPinIndexes() noexcept {
    (void)invalidatePinIndexes();
    rebuildPinIndex(storage_->surfaces, surfaceCount_,
                    storage_->surfacePinIndex,
                    pinIndexGeneration_);
    rebuildPinIndex(storage_->textures, textureCount_,
                    storage_->texturePinIndex,
                    pinIndexGeneration_);
    rebuildPinIndex(storage_->buffers, bufferCount_,
                    storage_->bufferPinIndex,
                    pinIndexGeneration_);
    rebuildPinIndex(storage_->shaders, shaderCount_,
                    storage_->shaderPinIndex,
                    pinIndexGeneration_);
    rebuildPinIndex(storage_->declarations, declarationCount_,
                    storage_->declarationPinIndex, pinIndexGeneration_);
    rebuildPinIndex(storage_->queries, queryCount_,
                    storage_->queryPinIndex,
                    pinIndexGeneration_);
  }

  template <typename PinArray, typename Visit>
  static bool visitPin(const PinArray& pins, std::size_t count,
                       std::uint32_t index, Visit&& visit) noexcept {
    if (index == kPeSemanticNoSlot) return true;
    if (index >= count || !pins[index].valid()) return false;
    return visit(pins[index].identity.value);
  }

  template <typename Wire>
  std::span<const Wire> canonicalBindingRows(
      const PeSemanticRecordReceipt& slot, std::uint16_t kind) const noexcept {
    if (!storage_ || recordCount_ == 0u) return {};
    const auto* begin = storage_->records.data();
    const auto* end = begin + recordCount_;
    if (&slot < begin || &slot >= end) return {};
    const auto ordinal = static_cast<std::size_t>(&slot - begin);
    const auto& record = storage_->wireRecords[ordinal];
    if (record.payloadOffset > emissionPayloadBytes_ ||
        record.payloadSize > emissionPayloadBytes_ - record.payloadOffset ||
        record.payloadSize < sizeof(D9CCommandChunkWireDrawHeader)) return {};
    const auto payload = std::span<const std::byte>(storage_->canonicalPayload)
                             .subspan(record.payloadOffset, record.payloadSize);
    D9CCommandChunkWireDrawHeader draw{};
    std::memcpy(&draw, payload.data(), sizeof(draw));
    if (draw.sectionTableOffset > payload.size() ||
        draw.sectionCount > D9C_COMMAND_CHUNK_SECTION_COUNT ||
        draw.sectionCount * sizeof(D9CCommandChunkWireSectionDesc) >
            payload.size() - draw.sectionTableOffset) return {};
    const auto* descs = reinterpret_cast<const D9CCommandChunkWireSectionDesc*>(
        payload.data() + draw.sectionTableOffset);
    for (std::size_t i = 0u; i < draw.sectionCount; ++i) {
      const auto& desc = descs[i];
      if (desc.kind != kind || desc.elementSize != sizeof(Wire) ||
          desc.byteSize != desc.count * sizeof(Wire) ||
          desc.payloadOffset > payload.size() ||
          desc.byteSize > payload.size() - desc.payloadOffset) continue;
      return {reinterpret_cast<const Wire*>(payload.data() + desc.payloadOffset),
              desc.count};
    }
    return {};
  }

  template <typename Visit>
  bool visitRecordHandles(const PeSemanticRecordReceipt& slot,
                          Visit&& visit) const noexcept {
    if (!storage_ || recordCount_ == 0u) return false;
    const auto* begin = storage_->records.data();
    const auto* end = begin + recordCount_;
    if (&slot < begin || &slot >= end) return false;
    const auto ordinal = static_cast<std::size_t>(&slot - begin);
    const auto& wire = storage_->wireRecords[ordinal];
    if (wire.firstHandle > emissionHandleCount_ ||
        wire.handleCount > emissionHandleCount_ - wire.firstHandle) return false;
    for (std::size_t i = 0u; i < wire.handleCount; ++i) {
      const auto& entry = storage_->wireHandles[wire.firstHandle + i];
      if (!visit({.kind = entry.kind,
                  .generation = entry.generation,
                  .objectId = entry.objectId})) return false;
    }
    return true;
  }

  static bool alignEmission(std::size_t value, std::size_t alignment,
                            std::size_t& out) noexcept {
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        value > std::numeric_limits<std::size_t>::max() - (alignment - 1u)) {
      return false;
    }
    out = (value + alignment - 1u) & ~(alignment - 1u);
    return true;
  }

  bool buildEmissionPlan(EmissionPlan& out) const noexcept {
    out = {};
    if (!ready_ || recordCount_ == 0u || recordCount_ > MaxRecords) return false;
    // The semantic walk and payload serializer run at admission. At seal we
    // only validate the already materialized role headers and derive the final
    // outer layout, whose offsets depend on the final aggregate counts.
    for (std::size_t i = 0u; i < recordCount_; ++i) {
      const auto& record = storage_->wireRecords[i];
      if (!recordRule(record.type) ||
          record.payloadOffset > emissionPayloadBytes_ ||
          record.payloadSize > emissionPayloadBytes_ - record.payloadOffset ||
          record.firstHandle > emissionHandleCount_ ||
          record.handleCount > emissionHandleCount_ - record.firstHandle) {
        return false;
      }
    }
    out.recordCount = recordCount_;
    out.handleCount = emissionHandleCount_;
    out.payloadBytes = emissionPayloadBytes_;
    if (!fitsStagedWire(out.handleCount, out.payloadBytes)) return false;
    if (out.recordCount > std::numeric_limits<std::uint32_t>::max() ||
        out.handleCount > std::numeric_limits<std::uint32_t>::max() ||
        out.payloadBytes > std::numeric_limits<std::uint32_t>::max()) return false;
    const auto layout = planExactCommandChunkLayout(
        static_cast<std::uint32_t>(out.recordCount),
        static_cast<std::uint32_t>(out.handleCount),
        static_cast<std::uint32_t>(out.payloadBytes));
    if (!layout.valid()) return false;
    out.header = {
        .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
        .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
        .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
        .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
        .recordTableOffset = layout.recordTableOffset,
        .recordCount = layout.recordCount,
        .handleTableOffset = layout.handleTableOffset,
        .handleCount = layout.handleCount,
        .payloadArenaOffset = layout.payloadArenaOffset,
        .payloadArenaSize = layout.payloadBytes,
        .reserved0 = 0u,
        .reserved1 = 0u,
    };
    if (layout.totalBytes > D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES) return false;
    out.wireBytes = layout.totalBytes;
    return true;
  }

  template <typename T>
  static bool writeValue(std::span<std::byte> destination, std::size_t& cursor,
                         const T& value) noexcept {
    if (cursor > destination.size() || sizeof(T) > destination.size() - cursor)
      return false;
    std::memcpy(destination.data() + cursor, &value, sizeof(T));
    cursor += sizeof(T);
    return true;
  }

  static bool zeroPaddingTo(std::span<std::byte> destination,
                            std::size_t& cursor,
                            std::size_t aligned) noexcept {
    if (cursor > aligned || aligned > destination.size()) return false;
    if (cursor != aligned) {
      std::fill(destination.begin() + cursor,
                destination.begin() + aligned, std::byte{0});
      cursor = aligned;
    }
    return true;
  }

  bool directHandle(std::uint32_t kind, std::uint32_t pin,
                    const auto& prepared,
                    std::size_t globalFirst, std::uint32_t& out) const noexcept {
    std::uint32_t local = kPeSemanticNoSlot;
    if (!prepared.identityWitness.findWire(kind, pin, local) ||
        globalFirst + local > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    out = static_cast<std::uint32_t>(globalFirst + local);
    return true;
  }

  template <PeSemanticRecordInputLike Input>
  bool writeSparsePayload(const Input& input, const PeSemanticRecordSlot& slot,
                          std::span<std::byte> payload,
                          std::size_t globalFirst,
                          const auto& prepared) const noexcept {
    const auto& sparse = peSemanticSparse(input);
    const auto& layout = prepared.sparseLayout;
    const auto count = static_cast<std::size_t>(layout.sectionCount);
    if (!layout.valid || count > D9C_COMMAND_CHUNK_SECTION_COUNT ||
        layout.payloadBytes != payload.size()) return false;
    D9CCommandChunkWireDrawHeader draw = slot.draw;
    draw.sectionCount = static_cast<std::uint32_t>(count);
    draw.sectionTableOffset = sizeof(draw);
    draw.sectionPayloadOffset = static_cast<std::uint32_t>(
        sizeof(draw) + count * sizeof(D9CCommandChunkWireSectionDesc));
    draw.reserved0 = 0u;
    std::size_t cursor = 0u;
    if (!writeValue(payload, cursor, draw)) return false;
    const auto tableBytes = count * sizeof(D9CCommandChunkWireSectionDesc);
    if (cursor > payload.size() || tableBytes > payload.size() - cursor) return false;
    cursor += tableBytes;
    std::size_t sectionIndex = 0u;
    // Consume the prepared descriptor verbatim.  The only source checks left
    // here are occurrence/byte-span checks and the qualified handle remap;
    // section order, alignment, offsets, and descriptor extent are facts of
    // admission and are never reconstructed by this writer.
    const auto consume = [&](std::uint16_t kind, std::size_t rows,
                             std::size_t bytes, std::uint16_t elementSize,
                             auto&& emit) noexcept {
      if (rows == 0u) return true;
      if (sectionIndex >= count || rows > std::numeric_limits<std::uint32_t>::max() ||
          bytes > std::numeric_limits<std::uint32_t>::max()) return false;
      const auto& expected = layout.sections[sectionIndex];
      if (expected.kind != kind || expected.count != rows ||
          (elementSize != 0u && expected.elementSize != elementSize) ||
          expected.byteSize != bytes || expected.payloadOffset < cursor ||
          expected.payloadOffset > payload.size() ||
          expected.byteSize > payload.size() - expected.payloadOffset ||
          !zeroPaddingTo(payload, cursor, expected.payloadOffset)) return false;
      auto destination = payload.subspan(expected.payloadOffset, expected.byteSize);
      if (!emit(destination)) return false;
      cursor = expected.payloadOffset + expected.byteSize;
      const auto descriptorOffset =
          static_cast<std::size_t>(draw.sectionTableOffset) +
          sectionIndex * sizeof(D9CCommandChunkWireSectionDesc);
      if (descriptorOffset > payload.size() ||
          sizeof(D9CCommandChunkWireSectionDesc) > payload.size() - descriptorOffset)
        return false;
      std::memcpy(payload.data() + descriptorOffset, &expected, sizeof(expected));
      ++sectionIndex;
      return true;
    };
    const auto typed = [&](auto kind, const auto& values) noexcept {
      return values.empty() ||
             consume(kind, values.size(), values.size_bytes(),
                     static_cast<std::uint16_t>(sizeof(values[0])),
                     [&](std::span<std::byte> destination) noexcept {
                       std::memcpy(destination.data(), values.data(),
                                   values.size_bytes());
                       return true;
                     });
    };
    const auto binding = [&](auto kind, const auto& values,
                             const auto& locators) noexcept {
      using Wire = std::remove_cvref_t<decltype(values[0].wire)>;
      if (values.empty()) return true;
      return consume(kind, values.size(), values.size() * sizeof(Wire),
                     static_cast<std::uint16_t>(sizeof(Wire)),
                     [&](std::span<std::byte> destination) noexcept {
                       if (locators.size() != values.size()) return false;
                       for (std::size_t occurrence = 0u;
                            occurrence < values.size(); ++occurrence) {
                         const auto& source = values[occurrence];
                         const auto& locator = locators[occurrence];
                         if (locator.occurrence != occurrence ||
                             locator.wireValid != source.wire.valid ||
                             (locator.wireValid &&
                              locator.hasPin != (source.object.object != nullptr)))
                           return false;
                         auto value = source.wire;
                         if constexpr (requires(Wire wire) { wire.reserved0; })
                           value.reserved0 = 0u;
                         value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
                         if (value.valid && locator.hasPin) {
                           if (locator.mappingOrdinal >= prepared.identityWitness.count ||
                               !std::remove_cvref_t<decltype(
                                   prepared.identityWitness)>::sameIdentity(
                                   prepared.identityWitness.entries[
                                       locator.mappingOrdinal]
                                       .identity,
                                   source.object.identity) ||
                               prepared.identityWitness.entries[locator.mappingOrdinal].object !=
                                   source.object.object)
                             return false;
                           const auto local = locator.wireIndex;
                           if (local == kPeSemanticNoSlot ||
                               local >= prepared.identityWitness.wireCount ||
                               prepared.identityWitness.entries[locator.mappingOrdinal].wireIndex !=
                                   local ||
                               globalFirst + local >
                                   std::numeric_limits<std::uint32_t>::max())
                             return false;
                           value.handleIndex =
                               static_cast<std::uint32_t>(globalFirst + local);
                         }
                         std::memcpy(destination.data() + occurrence * sizeof(Wire),
                                     &value, sizeof(Wire));
                       }
                       return true;
                     });
    };
    const auto constant = [&](auto kind, const auto& range,
                              const auto& source) noexcept {
      return range.registerCount == 0u ||
             consume(kind, range.registerCount, sizeof(range) + source.size(), 0u,
                     [&](std::span<std::byte> destination) noexcept {
                       if (destination.size() < sizeof(range) ||
                           source.size() != destination.size() - sizeof(range))
                         return false;
                       std::memcpy(destination.data(), &range, sizeof(range));
                       if (!source.empty())
                         std::memcpy(destination.data() + sizeof(range),
                                     source.data(), source.size());
                       return true;
                     });
    };
    const auto raw = [&](auto kind, const auto& bytes) noexcept {
      return bytes.empty() ||
             consume(kind, bytes.size(), bytes.size(),
                     static_cast<std::uint16_t>(sizeof(std::byte)),
                     [&](std::span<std::byte> destination) noexcept {
                       std::memcpy(destination.data(), bytes.data(), bytes.size());
                       return true;
                     });
    };
    if (!typed(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE, sparse.renderStates) ||
        !binding(D9C_COMMAND_CHUNK_SECTION_TEXTURE, sparse.textures,
                 std::span<const PeSemanticBindingLocator>(
                     prepared.bindingOccurrences.values.data() +
                         prepared.bindingOccurrences.ranges[0].offset,
                     prepared.bindingOccurrences.ranges[0].count)) ||
        !binding(D9C_COMMAND_CHUNK_SECTION_STREAM, sparse.streams,
                 std::span<const PeSemanticBindingLocator>(
                     prepared.bindingOccurrences.values.data() +
                         prepared.bindingOccurrences.ranges[1].offset,
                     prepared.bindingOccurrences.ranges[1].count)) ||
        !binding(D9C_COMMAND_CHUNK_SECTION_SHADER, sparse.shaders,
                 std::span<const PeSemanticBindingLocator>(
                     prepared.bindingOccurrences.values.data() +
                         prepared.bindingOccurrences.ranges[2].offset,
                     prepared.bindingOccurrences.ranges[2].count)) ||
        !binding(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT, sparse.vertexInputs,
                 std::span<const PeSemanticBindingLocator>(
                     prepared.bindingOccurrences.values.data() +
                         prepared.bindingOccurrences.ranges[3].offset,
                     prepared.bindingOccurrences.ranges[3].count)) ||
        !binding(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER, sparse.indexBuffers,
                 std::span<const PeSemanticBindingLocator>(
                     prepared.bindingOccurrences.values.data() +
                         prepared.bindingOccurrences.ranges[4].offset,
                     prepared.bindingOccurrences.ranges[4].count)) ||
        !binding(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, sparse.renderTargets,
                 std::span<const PeSemanticBindingLocator>(
                     prepared.bindingOccurrences.values.data() +
                         prepared.bindingOccurrences.ranges[5].offset,
                     prepared.bindingOccurrences.ranges[5].count)) ||
        !binding(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, sparse.depthStencils,
                 std::span<const PeSemanticBindingLocator>(
                     prepared.bindingOccurrences.values.data() +
                         prepared.bindingOccurrences.ranges[6].offset,
                     prepared.bindingOccurrences.ranges[6].count)) ||
        !typed(D9C_COMMAND_CHUNK_SECTION_VIEWPORT, sparse.viewports) ||
        !typed(D9C_COMMAND_CHUNK_SECTION_SCISSOR, sparse.scissors) ||
        !typed(D9C_COMMAND_CHUNK_SECTION_MATERIAL, sparse.materials) ||
        !typed(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE, sparse.clipPlanes) ||
        !typed(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
               sparse.textureStageStates) ||
        !typed(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE, sparse.samplerStates) ||
        !typed(D9C_COMMAND_CHUNK_SECTION_TRANSFORM, sparse.transforms) ||
        !typed(D9C_COMMAND_CHUNK_SECTION_LIGHT, sparse.lights) ||
        !typed(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE, sparse.lightEnables) ||
        !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F, slot.vsFloatConstant,
                  sparse.vsFloatConstants.registerBytes) ||
        !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I, slot.vsIntConstant,
                  sparse.vsIntConstants.registerBytes) ||
        !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B, slot.vsBoolConstant,
                  sparse.vsBoolConstants.registerBytes) ||
        !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F, slot.psFloatConstant,
                  sparse.psFloatConstants.registerBytes) ||
        !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I, slot.psIntConstant,
                  sparse.psIntConstants.registerBytes) ||
        !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B, slot.psBoolConstant,
                  sparse.psBoolConstants.registerBytes) ||
        !raw(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA, sparse.upIndexData) ||
        !raw(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA, sparse.upVertexData)) return false;
    return sectionIndex == count && cursor == payload.size();
  }

  bool writeSparsePlanPayloadQualified(
      const PeSemanticSparsePlanRecordStaging& slot,
      std::span<std::byte> payload,
      std::size_t globalFirst,
      const auto& prepared,
      const SparseStatePlan::QualifiedSources& sources) noexcept {
    (void)slot;
    const auto& sourceFacts = sources.source;
    const auto& destinationFacts = sources.destination;
    const auto& layout = prepared.sparseLayout;
    const auto count = static_cast<std::size_t>(layout.sectionCount);
    if (!layout.valid || count > D9C_COMMAND_CHUNK_SECTION_COUNT ||
        layout.payloadBytes != payload.size()) return false;
    auto draw = sparseStatePlanDrawHeader(sourceFacts);
    draw.sectionCount = static_cast<std::uint32_t>(count);
    draw.sectionTableOffset = sizeof(draw);
    draw.sectionPayloadOffset = static_cast<std::uint32_t>(
        sizeof(draw) + count * sizeof(D9CCommandChunkWireSectionDesc));
    draw.reserved0 = 0u;
    std::size_t cursor = 0u;
    if (!writeValue(payload, cursor, draw)) return false;
    const auto tableBytes = count * sizeof(D9CCommandChunkWireSectionDesc);
    if (cursor > payload.size() || tableBytes > payload.size() - cursor)
      return false;
    cursor += tableBytes;
    std::size_t sectionIndex = 0u;
    const auto consume = [&](std::uint16_t kind, std::size_t rows,
                             std::size_t bytes, std::uint16_t elementSize,
                             auto&& emit) noexcept {
      if (rows == 0u) return true;
      if (sectionIndex >= count ||
          rows > std::numeric_limits<std::uint32_t>::max() ||
          bytes > std::numeric_limits<std::uint32_t>::max()) return false;
      const auto& expected = layout.sections[sectionIndex];
      if (expected.kind != kind || expected.count != rows ||
          (elementSize != 0u && expected.elementSize != elementSize) ||
          expected.byteSize != bytes || expected.payloadOffset < cursor ||
          expected.payloadOffset > payload.size() ||
          expected.byteSize > payload.size() - expected.payloadOffset ||
          !zeroPaddingTo(payload, cursor, expected.payloadOffset)) return false;
      auto destination = payload.subspan(expected.payloadOffset,
                                         expected.byteSize);
      if (!emit(destination)) return false;
      cursor = expected.payloadOffset + expected.byteSize;
      const auto descriptorOffset =
          static_cast<std::size_t>(draw.sectionTableOffset) +
          sectionIndex * sizeof(D9CCommandChunkWireSectionDesc);
      if (descriptorOffset > payload.size() ||
          sizeof(D9CCommandChunkWireSectionDesc) >
              payload.size() - descriptorOffset) return false;
      std::memcpy(payload.data() + descriptorOffset, &expected,
                  sizeof(expected));
      ++sectionIndex;
      return true;
    };
    const auto visited = [&]<typename Wire>(std::uint16_t kind,
                                            std::size_t rows,
                                            auto&& visit) noexcept {
      return rows == 0u || consume(
          kind, rows, rows * sizeof(Wire),
          static_cast<std::uint16_t>(sizeof(Wire)),
          [&](std::span<std::byte> destination) noexcept {
            std::size_t occurrence = 0u;
            const auto emit = [&](const Wire& value) noexcept {
              if (occurrence >= rows) return false;
              std::memcpy(destination.data() + occurrence * sizeof(Wire),
                          &value, sizeof(Wire));
              ++occurrence;
              return true;
            };
            return visit(emit) && occurrence == rows;
          });
    };
    const auto binding = [&]<typename Wire>(std::uint16_t kind,
                                            std::size_t rows,
                                            std::size_t family,
                                            auto&& visit) noexcept {
      return rows == 0u || consume(
          kind, rows, rows * sizeof(Wire),
          static_cast<std::uint16_t>(sizeof(Wire)),
          [&](std::span<std::byte> destination) noexcept {
            if (family >= prepared.bindingOccurrences.ranges.size() ||
                prepared.bindingOccurrences.count >
                    prepared.bindingOccurrences.values.size())
              return false;
            const auto& range = prepared.bindingOccurrences.ranges[family];
            if (range.count != rows ||
                range.offset > prepared.bindingOccurrences.count ||
                range.count > prepared.bindingOccurrences.count - range.offset)
              return false;
            std::size_t occurrence = 0u;
            const auto acquire = [&]<typename Object, typename Identity,
                                     typename PinArray, typename Retain>(
                                     const PeWireObjectRef& source,
                                     const PeSemanticBindingLocator& locator,
                                     PinArray& pins, std::size_t& pinCount,
                                     Retain retain,
                                     std::uint32_t& wireIndex) noexcept {
              QualifiedBinding<Object, Identity> binding;
              if (!makeQualifiedBinding<Object, Identity>(
                      source, locator, prepared, globalFirst, binding))
                return false;
              std::uint32_t pin = kPeSemanticNoSlot;
              return pinPreparedQualifiedBinding<Object, Identity>(
                  pins, pinCount, pin, &wireIndex, binding, retain);
            };
            const auto emit = [&](Wire value,
                                  const PeWireObjectRef& ref) noexcept {
              if (occurrence >= rows) return false;
              const auto& locator = prepared.bindingOccurrences.values[
                  range.offset + occurrence];
              if (locator.occurrence != occurrence || !locator.wireValid ||
                  locator.hasPin != (ref.object != nullptr)) return false;
              if constexpr (requires(Wire wire) { wire.reserved0; })
                value.reserved0 = 0u;
              value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
              if (locator.hasPin) {
                bool acquired = false;
                std::uint32_t wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
                if constexpr (std::is_same_v<Wire,
                                             D9CCommandChunkWireTextureBinding>) {
                  acquired = acquire.template operator()<
                      D9CTexture, PeSemanticTextureIdentity>(
                      ref, locator, storage_->textures, textureCount_,
                      retainTextureFn(), wireIndex);
                } else if constexpr (std::is_same_v<
                                         Wire,
                                         D9CCommandChunkWireStreamBinding> ||
                                     std::is_same_v<
                                         Wire,
                                         D9CCommandChunkWireIndexBinding>) {
                  acquired = acquire.template operator()<
                      D9CBuffer, PeSemanticBufferIdentity>(
                      ref, locator, storage_->buffers, bufferCount_,
                      retainBufferFn(), wireIndex);
                } else if constexpr (std::is_same_v<
                                         Wire,
                                         D9CCommandChunkWireShaderBinding>) {
                  acquired = acquire.template operator()<
                      D9CShader, PeSemanticShaderIdentity>(
                      ref, locator, storage_->shaders, shaderCount_,
                      retainShaderFn(), wireIndex);
                } else if constexpr (std::is_same_v<
                                         Wire,
                                         D9CCommandChunkWireVertexInput>) {
                  acquired = acquire.template operator()<
                      D9CVertexDecl, PeSemanticDeclarationIdentity>(
                      ref, locator, storage_->declarations, declarationCount_,
                      retainDeclarationFn(), wireIndex);
                } else {
                  acquired = acquire.template operator()<
                      D9CSurface, PeSemanticSurfaceIdentity>(
                      ref, locator, storage_->surfaces, surfaceCount_,
                      retainSurfaceFn(), wireIndex);
                }
                if (!acquired) {
                  if constexpr (std::is_same_v<
                                    Wire,
                                    D9CCommandChunkWireTextureBinding>)
                    lastAdmissionFailure_ = AdmissionFailure::SparseTextures;
                  else if constexpr (std::is_same_v<
                                         Wire,
                                         D9CCommandChunkWireStreamBinding>)
                    lastAdmissionFailure_ = AdmissionFailure::SparseStreams;
                  else if constexpr (std::is_same_v<
                                         Wire,
                                         D9CCommandChunkWireShaderBinding>)
                    lastAdmissionFailure_ = AdmissionFailure::SparseShaders;
                  else if constexpr (std::is_same_v<
                                         Wire,
                                         D9CCommandChunkWireVertexInput>)
                    lastAdmissionFailure_ = AdmissionFailure::SparseVertexInputs;
                  else if constexpr (std::is_same_v<
                                         Wire,
                                         D9CCommandChunkWireIndexBinding>)
                    lastAdmissionFailure_ = AdmissionFailure::SparseIndexBuffers;
                  else if constexpr (std::is_same_v<
                                         Wire,
                                         D9CCommandChunkWireRenderTargetBinding>)
                    lastAdmissionFailure_ = AdmissionFailure::SparseRenderTargets;
                  else
                    lastAdmissionFailure_ = AdmissionFailure::SparseDepthStencils;
                  return false;
                }
                value.handleIndex = static_cast<std::uint32_t>(
                    globalFirst + wireIndex);
              }
              std::memcpy(destination.data() + occurrence * sizeof(Wire),
                          &value, sizeof(Wire));
              ++occurrence;
              return true;
            };
            return visit(emit) && occurrence == rows;
          });
    };
    const auto constant = [&](std::uint16_t kind,
                              const SparseConstantRangeInput& range) noexcept {
      if (!range.present()) return true;
      const D9CCommandChunkWireConstantRange header{
          .startRegister = range.startRegister,
          .registerCount = range.registerCount,
      };
      return consume(kind, range.registerCount,
                     sizeof(header) + range.registerBytes.size(), 0u,
                     [&](std::span<std::byte> destination) noexcept {
                       if (destination.size() !=
                           sizeof(header) + range.registerBytes.size())
                         return false;
                       std::memcpy(destination.data(), &header, sizeof(header));
                       if (!range.registerBytes.empty())
                         std::memcpy(destination.data() + sizeof(header),
                                     range.registerBytes.data(),
                                     range.registerBytes.size());
                       return true;
                     });
    };
    const auto raw = [&](std::uint16_t kind,
                         std::span<const std::byte> bytes) noexcept {
      return bytes.empty() || consume(
          kind, bytes.size(), bytes.size(), sizeof(std::byte),
          [&](std::span<std::byte> destination) noexcept {
            std::memcpy(destination.data(), bytes.data(), bytes.size());
            return true;
          });
    };
    bool emitted = true;
    const auto& shadow = sources.shadow;
    const auto& bindings = sources.bindings;
    {
          const auto renderStates = sourceFacts.pendingDelta.pendingRenderStatesTyped();
          const auto tss = sourceFacts.pendingDelta.pendingTssTyped();
          const auto samplers = sourceFacts.pendingDelta.pendingSamplerStatesTyped();
          const auto transforms = sourceFacts.pendingDelta.pendingTransformsTyped();
          if (renderStates.size() != sourceFacts.renderStateCount ||
              tss.size() != sourceFacts.textureStageStateCount ||
              samplers.size() != sourceFacts.samplerStateCount ||
              transforms.size() != sourceFacts.transformCount) return false;
          const auto visitRenderStates = [&](const auto& emit) noexcept {
            bool ok = true;
            renderStates.forEach(
                [&](RenderStateSlot state, std::uint32_t value) noexcept {
                  ok = ok && emit(D9CCommandChunkWireRenderState{
                      .state = rawSlot(state), .value = value});
                });
            return ok;
          };
          const auto visitTextures = [&](const auto& emit) noexcept {
            return forEachSetBitAscending(
                sourceFacts.textureMask &
                    ((1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u),
                [&](std::uint32_t source) noexcept {
                  D9CCommandChunkWireTextureBinding value{};
                  value.slot = source;
                  value.valid = 1u;
                  return emit(value, bindings.textures[source]);
                });
          };
          const auto visitStreams = [&](const auto& emit) noexcept {
            return forEachSetBitAscending(
                destinationFacts.streamMask &
                    ((1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u),
                [&](std::uint32_t source) noexcept {
                  D9CCommandChunkWireStreamBinding value{};
                  value.slot = source;
                  value.valid = 1u;
                  value.offset = bindings.streams[source].offset;
                  value.stride = bindings.streams[source].stride;
                  return emit(value, bindings.streams[source].buffer);
                });
          };
          const auto visitShaders = [&](const auto& emit) noexcept {
            for (std::uint32_t stage = 0u; stage < 2u; ++stage) {
              if ((sourceFacts.shaderMask & (1u << stage)) == 0u) continue;
              D9CCommandChunkWireShaderBinding value{};
              value.stage = stage;
              value.valid = 1u;
              if (!emit(value, stage == 0u
                                   ? static_cast<const PeWireObjectRef&>(bindings.vs)
                                   : static_cast<const PeWireObjectRef&>(bindings.ps)))
                return false;
            }
            return true;
          };
          const auto visitVertexInput = [&](const auto& emit) noexcept {
            if (!sourceFacts.vertexInput) return true;
            D9CCommandChunkWireVertexInput value{};
            value.valid = 1u;
            value.kind = sourceFacts.vertexInputKind;
            value.value = bindings.fvf;
            const PeWireObjectRef ref =
                value.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION
                    ? static_cast<const PeWireObjectRef&>(bindings.vdecl)
                    : PeWireObjectRef{};
            return emit(value, ref);
          };
          const auto visitIndex = [&](const auto& emit) noexcept {
            if (!destinationFacts.indexBuffer) return true;
            D9CCommandChunkWireIndexBinding value{};
            value.valid = 1u;
            return emit(value, bindings.indexBuffer);
          };
          const auto visitRenderTargets = [&](const auto& emit) noexcept {
            return forEachSetBitAscending(
                sourceFacts.renderTargetMask &
                    ((1u << D9C_DRAW_PACKET_MAX_RENDER_TARGETS) - 1u),
                [&](std::uint32_t source) noexcept {
                  D9CCommandChunkWireRenderTargetBinding value{};
                  value.slot = source;
                  value.valid = 1u;
                  return emit(value, bindings.renderTargets[source]);
                });
          };
          const auto visitDepthStencil = [&](const auto& emit) noexcept {
            if (!sourceFacts.depthStencil) return true;
            D9CCommandChunkWireDepthStencilBinding value{};
            value.valid = 1u;
            return emit(value, bindings.depthStencil);
          };
          const auto visitClipPlanes = [&](const auto& emit) noexcept {
            for (std::uint32_t source = 0u; source < 6u; ++source) {
              if ((sourceFacts.clipPlaneMask & (1u << source)) == 0u) continue;
              D9CCommandChunkWireClipPlane value{};
              value.slot = source;
              std::memcpy(value.values,
                          &shadow.clipPlaneShadow()[source * 4u],
                          sizeof(value.values));
              if (!emit(value)) return false;
            }
            return true;
          };
          const auto visitTss = [&](const auto& emit) noexcept {
            bool ok = true;
            tss.forEach([&](TextureStageIndex stage,
                            TextureStageStateType type,
                            std::uint32_t value) noexcept {
              ok = ok && emit(D9CDrawPacketTextureStageState{
                  rawSlot(stage), rawSlot(type), value});
            });
            return ok;
          };
          const auto visitSamplers = [&](const auto& emit) noexcept {
            bool ok = true;
            samplers.forEach([&](SamplerIndex sampler, SamplerStateType type,
                                 std::uint32_t value) noexcept {
              ok = ok && emit(D9CDrawPacketSamplerState{
                  rawSlot(sampler), rawSlot(type), value});
            });
            return ok;
          };
          const auto visitTransforms = [&](const auto& emit) noexcept {
            bool ok = true;
            transforms.forEach([&](TransformState state,
                                   const D9CMatrix& matrix) noexcept {
              ok = ok && emit(D9CDrawPacketTransform{
                  rawSlot(state), 0u, matrix});
            });
            return ok;
          };
          const auto visitLights = [&](const auto& emit) noexcept {
            for (std::uint32_t source = 0u;
                 source < D9C_DRAW_PACKET_MAX_LIGHTS; ++source) {
              if ((sourceFacts.lightMask & (1u << source)) == 0u) continue;
              if (!emit(D9CCommandChunkWireLight{
                      .slot = source,
                      .reserved0 = 0u,
                      .light = shadow.lightShadow()[source]})) return false;
            }
            return true;
          };
          const auto visitLightEnables = [&](const auto& emit) noexcept {
            const auto enabled = shadow.pendingLightEnableMask();
            for (std::uint32_t source = 0u;
                 source < D9C_DRAW_PACKET_MAX_LIGHTS; ++source) {
              if ((sourceFacts.lightEnableMask & (1u << source)) == 0u) continue;
              if (!emit(D9CCommandChunkWireLightEnable{
                      .slot = source,
                      .enabled = (enabled & (1u << source)) != 0u}))
                return false;
            }
            return true;
          };
          const auto once = [](const auto& value) noexcept {
            return [&value](const auto& emit) noexcept { return emit(value); };
          };
          // Layout preparation publishes the active sections in canonical
          // schema order. Consume exactly that inventory so absent categories
          // do not enter a typed/binding/constant/raw helper at all. The
          // prepared descriptor remains the authority for count/order/offset;
          // each visitor only supplies bytes for the active descriptor.
          for (std::size_t active = 0u; active < count && emitted; ++active) {
            const auto kind = layout.sections[active].kind;
            switch (kind) {
            case D9C_COMMAND_CHUNK_SECTION_RENDER_STATE:
              emitted = visited.template operator()<D9CCommandChunkWireRenderState>(
                  kind, sourceFacts.renderStateCount, visitRenderStates);
              break;
            case D9C_COMMAND_CHUNK_SECTION_TEXTURE:
              emitted = binding.template operator()<D9CCommandChunkWireTextureBinding>(
                  kind, layout.quotaDeltas[kTextures], 0u, visitTextures);
              break;
            case D9C_COMMAND_CHUNK_SECTION_STREAM:
              emitted = binding.template operator()<D9CCommandChunkWireStreamBinding>(
                  kind, layout.quotaDeltas[kStreams], 1u, visitStreams);
              break;
            case D9C_COMMAND_CHUNK_SECTION_SHADER:
              emitted = binding.template operator()<D9CCommandChunkWireShaderBinding>(
                  kind, layout.quotaDeltas[kShaders], 2u, visitShaders);
              break;
            case D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT:
              emitted = binding.template operator()<D9CCommandChunkWireVertexInput>(
                  kind, 1u, 3u, visitVertexInput);
              break;
            case D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER:
              emitted = binding.template operator()<D9CCommandChunkWireIndexBinding>(
                  kind, 1u, 4u, visitIndex);
              break;
            case D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET:
              emitted = binding.template operator()<D9CCommandChunkWireRenderTargetBinding>(
                  kind, layout.quotaDeltas[kRenderTargets], 5u,
                  visitRenderTargets);
              break;
            case D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL:
              emitted = binding.template operator()<D9CCommandChunkWireDepthStencilBinding>(
                  kind, 1u, 6u, visitDepthStencil);
              break;
            case D9C_COMMAND_CHUNK_SECTION_VIEWPORT:
              emitted = visited.template operator()<D9CViewport>(
                  kind, 1u, once(shadow.viewportShadow()));
              break;
            case D9C_COMMAND_CHUNK_SECTION_SCISSOR:
              emitted = visited.template operator()<D9CRect>(
                  kind, 1u, once(shadow.scissorShadow()));
              break;
            case D9C_COMMAND_CHUNK_SECTION_MATERIAL:
              emitted = visited.template operator()<D9CMaterial>(
                  kind, 1u, once(shadow.materialShadow()));
              break;
            case D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE:
              emitted = visited.template operator()<D9CCommandChunkWireClipPlane>(
                  kind, layout.quotaDeltas[kClipPlanes], visitClipPlanes);
              break;
            case D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE:
              emitted = visited.template operator()<D9CDrawPacketTextureStageState>(
                  kind, sourceFacts.textureStageStateCount, visitTss);
              break;
            case D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE:
              emitted = visited.template operator()<D9CDrawPacketSamplerState>(
                  kind, sourceFacts.samplerStateCount, visitSamplers);
              break;
            case D9C_COMMAND_CHUNK_SECTION_TRANSFORM:
              emitted = visited.template operator()<D9CDrawPacketTransform>(
                  kind, sourceFacts.transformCount, visitTransforms);
              break;
            case D9C_COMMAND_CHUNK_SECTION_LIGHT:
              emitted = visited.template operator()<D9CCommandChunkWireLight>(
                  kind, layout.quotaDeltas[kLights], visitLights);
              break;
            case D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE:
              emitted = visited.template operator()<D9CCommandChunkWireLightEnable>(
                  kind, layout.quotaDeltas[kLightEnables], visitLightEnables);
              break;
            case D9C_COMMAND_CHUNK_SECTION_VS_CONST_F:
              emitted = constant(kind, sourceFacts.vsFloatConstants);
              break;
            case D9C_COMMAND_CHUNK_SECTION_VS_CONST_I:
              emitted = constant(kind, sourceFacts.vsIntConstants);
              break;
            case D9C_COMMAND_CHUNK_SECTION_VS_CONST_B:
              emitted = constant(kind, sourceFacts.vsBoolConstants);
              break;
            case D9C_COMMAND_CHUNK_SECTION_PS_CONST_F:
              emitted = constant(kind, sourceFacts.psFloatConstants);
              break;
            case D9C_COMMAND_CHUNK_SECTION_PS_CONST_I:
              emitted = constant(kind, sourceFacts.psIntConstants);
              break;
            case D9C_COMMAND_CHUNK_SECTION_PS_CONST_B:
              emitted = constant(kind, sourceFacts.psBoolConstants);
              break;
            case D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA:
              emitted = raw(kind, sourceFacts.payloads.upIndex);
              break;
            case D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA:
              emitted = raw(kind, sourceFacts.payloads.upVertex);
              break;
            default:
              emitted = false;
              break;
            }
          }
    }
    return emitted && sectionIndex == count &&
           cursor == payload.size();
  }

  bool writeSparsePlanPayload(
      const PeSemanticSparsePlanRecordInput& input,
      const PeSemanticSparsePlanRecordStaging& slot,
      std::span<std::byte> payload,
      std::size_t globalFirst,
      const auto& prepared) noexcept {
    if (input.access == nullptr || input.sparsePlan == nullptr) return false;
    bool written = false;
    const bool qualified = input.sparsePlan->withQualifiedSources(
        *input.access,
        [&](const SparseStatePlan::QualifiedSources& sources) noexcept {
          if (!sources.destination.chunkContextFinalized ||
              sources.source.draw.recordType != input.recordType) return;
          written = writeSparsePlanPayloadQualified(
              slot, payload, globalFirst, prepared, sources);
        });
    return qualified && written;
  }

  bool writeConstantPayload(std::span<const std::byte> source,
                            const auto& slot,
                            std::span<std::byte> payload) const noexcept {
    std::size_t cursor = 0u;
    if (!writeValue(payload, cursor, slot.setConst) || cursor > payload.size() ||
        source.size() > payload.size() - cursor) {
      return false;
    }
    if (!source.empty()) {
      std::memcpy(payload.data() + cursor, source.data(),
                  source.size());
    }
    cursor += source.size();
    return cursor == payload.size();
  }

  template <PeSemanticRecordInputLike Input>
  bool writeRecordPayload(const Input& input, const PeSemanticRecordSlot& slot,
                          std::span<std::byte> payload,
                          std::size_t globalFirst,
                          const auto& prepared) const noexcept {
    const auto& staged = stagedPeSemanticInput(input);
    std::size_t cursor = 0u;
    switch (slot.producer) {
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        return writeSparsePayload(input, slot, payload, globalFirst, prepared);
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        return writeConstantPayload(staged.constantBytes, slot, payload);
      case PeSemanticProducerKind::Clear: {
        auto fixed = slot.clear;
        fixed.rectCount = slot.clearRects.count;
        fixed.rectOffset = sizeof(fixed);
        if (!writeValue(payload, cursor, fixed) ||
            slot.clearRects.offset > MaxRects ||
            slot.clearRects.count > MaxRects - slot.clearRects.offset ||
            staged.clearRects.size() != slot.clearRects.count ||
            slot.clearRects.count * sizeof(D9CRect) > payload.size() - cursor) return false;
        std::memcpy(payload.data() + cursor,
                    staged.clearRects.data(),
                    slot.clearRects.count * sizeof(D9CRect));
        cursor += slot.clearRects.count * sizeof(D9CRect);
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::Present: {
        auto fixed = slot.present;
        fixed.sourceHandleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, prepared,
                          globalFirst, fixed.sourceHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::StretchRect: {
        auto fixed = slot.stretchRect;
        fixed.reserved0 = 0u;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, prepared,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface1, prepared,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::ColorFill: {
        auto fixed = slot.colorFill;
        fixed.reserved0 = 0u;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, prepared,
                          globalFirst, fixed.surfaceHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::UpdateTexture: {
        D9CCommandChunkWireUpdateTexture fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture0, prepared,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture1, prepared,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::UpdateSurface: {
        auto fixed = slot.updateSurface;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, prepared,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface1, prepared,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::QueryIssue: {
        auto fixed = slot.queryIssue;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_QUERY, slot.query, prepared,
                          globalFirst, fixed.queryHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::Readback: {
        D9CCommandChunkWireReadback fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, prepared,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface1, prepared,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::ReszDepthResolve: {
        D9CCommandChunkWireReszDepthResolve fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, prepared,
                          globalFirst, fixed.msaaDepthHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture0, prepared,
                          globalFirst, fixed.intzDestHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::GenerateMipmaps: {
        D9CCommandChunkWireGenerateMipmaps fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture0, prepared,
                          globalFirst, fixed.textureHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::Count:
        return false;
    }
    return false;
  }

  bool writeRecordPayload(
      const PeSemanticSparsePlanRecordInput& input,
      const PeSemanticSparsePlanRecordStaging& slot,
      std::span<std::byte> payload,
      std::size_t globalFirst,
      const auto& prepared) noexcept {
    return writeSparsePlanPayload(input, slot, payload, globalFirst, prepared);
  }

  // Materialize one record's canonical D9C V2 role bytes while the source
  // slot is still being admitted. The payload's relative offsets and the
  // record-local/global handle ordinals are already known from the prepared
  // witness; only the outer chunk offsets remain frontier-dependent.
  template <typename Input>
    requires (PeSemanticRecordInputLike<Input> ||
              PeSemanticSparsePlanInputLike<Input>)
  bool materializeCanonicalRecord(
      const Input& input,
      const auto& slot,
      const auto& prepared,
      const PreparedAppendAuthority& authority) noexcept {
    // Generic/control staging can acquire resources before reaching this
    // point. Preserve its pre-write frontier guard, not merely the later
    // post-serializer check. Compact staging has no callbacks before here.
    if constexpr (PeSemanticRecordInputLike<Input>) {
      if (!authority.binds(prepared, *this)) return false;
    }
    if (!prepared.valid() ||
        prepared.plan.handleCount > kMaxRecordHandles ||
        prepared.payloadOffset > Storage::maxWirePayloadBytes ||
        prepared.plan.payloadBytes >
            Storage::maxWirePayloadBytes - prepared.payloadOffset ||
        prepared.emissionHandleCount > Storage::maxWireHandles ||
        recordCount_ >= Storage::maxWireRecords) {
      return false;
    }
    const auto& handles = prepared.identityWitness;
    if (handles.wireCount != prepared.plan.handleCount ||
        handles.wireCount > kMaxRecordHandles) {
      return false;
    }
    auto payload = std::span<std::byte>(storage_->canonicalPayload).subspan(
        prepared.payloadOffset, prepared.plan.payloadBytes);
    // Payload alignment belongs to the canonical payload role and is emitted
    // verbatim by both segmented and ExactFixed paths. Initialize only the
    // real inter-record gap explicitly; writeRecordPayload clears each
    // record-local alignment gap as it advances and overwrites every payload
    // value, so a wholesale per-record fill is unnecessary.
    std::fill(storage_->canonicalPayload.begin() +
                  prepared.emissionPayloadBytes,
              storage_->canonicalPayload.begin() + prepared.payloadOffset,
              std::byte{0});
    if (!writeRecordPayload(input, slot, payload, prepared.emissionHandleCount,
                            prepared)) {
      return false;
    }
    for (std::size_t i = 0u; i < handles.wireCount; ++i) {
      const auto* const entry = handles.wireEntry(i);
      if (entry == nullptr) return false;
      storage_->wireHandles[prepared.emissionHandleCount + i] = {
          .kind = entry->identity.kind,
          .generation = entry->identity.generation,
          .objectId = entry->identity.objectId,
      };
    }
    const auto recordType = peSemanticRecordType(input);
    const auto recordFlags = [&]() noexcept {
      if constexpr (PeSemanticSparsePlanInputLike<Input>)
        return input.recordFlags;
      else
        return stagedPeSemanticInput(input).recordFlags;
    }();
    storage_->wireRecords[recordCount_] = {
        .type = recordType,
        .flags = recordFlags,
        .payloadOffset = static_cast<std::uint32_t>(prepared.payloadOffset),
        .payloadSize = static_cast<std::uint32_t>(prepared.plan.payloadBytes),
        .firstHandle = static_cast<std::uint32_t>(prepared.emissionHandleCount),
        .handleCount = static_cast<std::uint32_t>(handles.wireCount),
        .reserved0 = 0u,
        .reserved1 = 0u,
    };
    return true;
  }

  template <typename Input>
    requires (PeSemanticRecordInputLike<Input> ||
              PeSemanticSparsePlanInputLike<Input>)
  bool materializePreparedCanonicalRecord(
      const Input& input, const auto& slot,
      const auto& prepared,
      const PreparedAppendAuthority& authority) noexcept {
    return materializeCanonicalRecord(input, slot, prepared, authority);
  }

  bool materializePreparedCanonicalRecord(
      const PeSemanticConstantRecordInput& input,
      const PeSemanticConstantRecordStaging& slot,
      const PeSemanticPreparedConstantRecord& prepared,
      const PreparedAppendAuthority& authority) noexcept {
    if (!authority.binds(prepared, *this) ||
        prepared.payloadOffset > Storage::maxWirePayloadBytes ||
        prepared.payloadBytes >
            Storage::maxWirePayloadBytes - prepared.payloadOffset ||
        prepared.emissionHandleCount > Storage::maxWireHandles ||
        prepared.nextEmissionHandleCount != prepared.emissionHandleCount ||
        recordCount_ >= Storage::maxWireRecords) {
      return false;
    }
    auto payload = std::span<std::byte>(storage_->canonicalPayload).subspan(
        prepared.payloadOffset, prepared.payloadBytes);
    std::fill(storage_->canonicalPayload.begin() +
                  prepared.emissionPayloadBytes,
              storage_->canonicalPayload.begin() + prepared.payloadOffset,
              std::byte{0});
    if (!writeConstantPayload(input.constantBytes, slot, payload)) return false;
    storage_->wireRecords[recordCount_] = {
        .type = input.recordType,
        .flags = input.recordFlags,
        .payloadOffset = static_cast<std::uint32_t>(prepared.payloadOffset),
        .payloadSize = static_cast<std::uint32_t>(prepared.payloadBytes),
        .firstHandle =
            static_cast<std::uint32_t>(prepared.emissionHandleCount),
        .handleCount = 0u,
        .reserved0 = 0u,
        .reserved1 = 0u,
    };
    return true;
  }

  bool copyCanonicalRoles(const EmissionPlan& plan,
                          std::span<std::byte> recordRegion,
                          std::span<std::byte> handleRegion,
                          std::span<std::byte> payloadRegion) const noexcept {
    if (recordRegion.size() < plan.recordBytes() ||
        handleRegion.size() < plan.handleBytes() ||
        payloadRegion.size() < plan.payloadBytes ||
        !fitsStagedWire(plan.handleCount, plan.payloadBytes)) {
      return false;
    }
    std::memcpy(recordRegion.data(), storage_->wireRecords.data(),
                plan.recordBytes());
    std::memcpy(handleRegion.data(), storage_->wireHandles.data(),
                plan.handleBytes());
    std::memmove(payloadRegion.data(),
                 storage_->canonicalPayload.data(),
                 plan.payloadBytes);
    return true;
  }

  static D9CCommandChunkSegmentedTransportV1 makeTransport(
      const EmissionPlan& plan, std::span<const std::byte> records,
      std::span<const std::byte> handles, std::span<const std::byte> payload,
      std::uint64_t captureToken, std::uint64_t eventOrdinal) noexcept {
    const auto roleToken = [](std::span<const std::byte> role) noexcept {
      return role.empty() ? D9CWireHandle{} : toWireHandle(role.data());
    };
    return {
        .header = plan.header,
        .records = roleToken(records),
        .recordBytes = static_cast<std::uint32_t>(records.size()),
        .recordReserved = 0u,
        .handles = roleToken(handles),
        .handleBytes = static_cast<std::uint32_t>(handles.size()),
        .handleReserved = 0u,
        .payload = roleToken(payload),
        .payloadBytes = static_cast<std::uint32_t>(payload.size()),
        .payloadReserved = 0u,
        .renderTapeCaptureToken = captureToken,
        .renderTapeEventOrdinal = eventOrdinal,
        .producerIdentity = {},
    };
  }

  static constexpr std::size_t kRenderStates = 0u;
  static constexpr std::size_t kTextures = 1u;
  static constexpr std::size_t kStreams = 2u;
  static constexpr std::size_t kShaders = 3u;
  static constexpr std::size_t kVertexInputs = 4u;
  static constexpr std::size_t kIndexBuffers = 5u;
  static constexpr std::size_t kRenderTargets = 6u;
  static constexpr std::size_t kDepthStencils = 7u;
  static constexpr std::size_t kViewports = 8u;
  static constexpr std::size_t kScissors = 9u;
  static constexpr std::size_t kMaterials = 10u;
  static constexpr std::size_t kClipPlanes = 11u;
  static constexpr std::size_t kTextureStageStates = 12u;
  static constexpr std::size_t kSamplerStates = 13u;
  static constexpr std::size_t kTransforms = 14u;
  static constexpr std::size_t kLights = 15u;
  static constexpr std::size_t kLightEnables = 16u;

  // The prepared section inventory is the sole active quota set. Keep its
  // canonical kind-to-category mapping compile-time coupled to the existing
  // owner counters so a schema reorder cannot silently publish the wrong
  // sparse dimension.
  static_assert(
      D9C_COMMAND_CHUNK_SECTION_RENDER_STATE -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kRenderStates &&
      D9C_COMMAND_CHUNK_SECTION_TEXTURE -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kTextures &&
      D9C_COMMAND_CHUNK_SECTION_STREAM -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kStreams &&
      D9C_COMMAND_CHUNK_SECTION_SHADER -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kShaders &&
      D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kVertexInputs &&
      D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kIndexBuffers &&
      D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kRenderTargets &&
      D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kDepthStencils &&
      D9C_COMMAND_CHUNK_SECTION_VIEWPORT -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kViewports &&
      D9C_COMMAND_CHUNK_SECTION_SCISSOR -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kScissors &&
      D9C_COMMAND_CHUNK_SECTION_MATERIAL -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kMaterials &&
      D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kClipPlanes &&
      D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE ==
              kTextureStageStates &&
      D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kSamplerStates &&
      D9C_COMMAND_CHUNK_SECTION_TRANSFORM -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kTransforms &&
      D9C_COMMAND_CHUNK_SECTION_LIGHT -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kLights &&
      D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE -
                  D9C_COMMAND_CHUNK_SECTION_RENDER_STATE == kLightEnables);

  std::size_t ownedMaterializedBytes() const noexcept {
    if (!ready_) return 0u;
    std::size_t bytes = recordCount_ * sizeof(storage_->records[0]);
    bytes += surfaceCount_ * sizeof(storage_->surfaces[0]);
    bytes += textureCount_ * sizeof(storage_->textures[0]);
    bytes += bufferCount_ * sizeof(storage_->buffers[0]);
    bytes += shaderCount_ * sizeof(storage_->shaders[0]);
    bytes += declarationCount_ * sizeof(storage_->declarations[0]);
    bytes += queryCount_ * sizeof(storage_->queries[0]);
    bytes += recordCount_ * sizeof(storage_->wireRecords[0]);
    bytes += emissionHandleCount_ * sizeof(storage_->wireHandles[0]);
    bytes += emissionPayloadBytes_;
    return bytes;
  }

  template <typename Admit>
    requires std::is_nothrow_invocable_r_v<bool, Admit&>
  bool recordAdmission(Admit&& admit) noexcept {
    auto&& operation = admit;
    auto* ledger = LedgerAccess::resolve(
        dxmt9::core::CopyMaterializationOwner::Pe);
    if (!ledger) return operation();

    const auto before = ownedMaterializedBytes();
    dxmt9::core::CopyMaterializationEvent event(
        ledger,
        dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission,
        0u);
    if (!operation()) {
      event.cancel();
      return false;
    }
    const auto after = ownedMaterializedBytes();
    const auto admittedBytes = after >= before ? after - before : 0u;
    event.setBytes(admittedBytes);
    event.commit();
    ledger->retain(
        dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission,
        admittedBytes);
    admissionLedger_ = ledger;
    return true;
  }

  void releaseLedgerRetention() noexcept {
    const auto admissionBytes = ownedMaterializedBytes();
    if (admissionLedger_ && admissionBytes != 0u) {
      admissionLedger_->release(
          dxmt9::core::CopyMaterializationClass::PeSemanticOwnerAdmission,
          admissionBytes);
    }
    admissionLedger_ = nullptr;
    if (exactWireLedger_ && exactWireRetainedBytes_ != 0u) {
      exactWireLedger_->release(
          dxmt9::core::CopyMaterializationClass::PeWireFinal,
          exactWireRetainedBytes_);
    }
    exactWireLedger_ = nullptr;
    exactWireRetainedBytes_ = 0u;
  }

  StateCheckpoint checkpointState() const noexcept {
    return {.records = recordCount_,
            .semanticBytes = semanticBytes_,
            .emissionHandles = emissionHandleCount_,
            .emissionPayloadBytes = emissionPayloadBytes_,
            .rects = rectCount_,
            .sparse = sparseCounts_.values,
            .pins = {surfaceCount_, textureCount_, bufferCount_, shaderCount_,
                     declarationCount_, queryCount_},
            .retained = retainer_.beginAcquire()};
  }

  ConstantStateCheckpoint checkpointConstantState() const noexcept {
    return {.records = recordCount_,
            .semanticBytes = semanticBytes_,
            .emissionHandles = emissionHandleCount_,
            .emissionPayloadBytes = emissionPayloadBytes_};
  }

  void rollback(const ConstantStateCheckpoint& checkpoint) noexcept {
    const auto recordsBeforeRollback = recordCount_;
    const auto handlesBeforeRollback = emissionHandleCount_;
    const auto payloadBeforeRollback = emissionPayloadBytes_;
    recordCount_ = checkpoint.records;
    semanticBytes_ = checkpoint.semanticBytes;
    emissionHandleCount_ = checkpoint.emissionHandles;
    emissionPayloadBytes_ = checkpoint.emissionPayloadBytes;
    if (recordCount_ < MaxRecords) storage_->records[recordCount_] = {};
    for (std::size_t i = checkpoint.records;
         i < std::min(recordsBeforeRollback, Storage::maxWireRecords); ++i) {
      storage_->wireRecords[i] = {};
    }
    for (std::size_t i = checkpoint.emissionHandles;
         i < std::min(handlesBeforeRollback, Storage::maxWireHandles); ++i) {
      storage_->wireHandles[i] = {};
    }
    if (checkpoint.emissionPayloadBytes < payloadBeforeRollback &&
        checkpoint.emissionPayloadBytes < Storage::maxWirePayloadBytes) {
      const auto count = std::min(
          payloadBeforeRollback - checkpoint.emissionPayloadBytes,
          Storage::maxWirePayloadBytes - checkpoint.emissionPayloadBytes);
      std::fill_n(storage_->canonicalPayload.begin() +
                      checkpoint.emissionPayloadBytes,
                  count, std::byte{0});
    }
  }

  void rollback(const StateCheckpoint& checkpoint) noexcept {
    const auto recordsBeforeRollback = recordCount_;
    const auto handlesBeforeRollback = emissionHandleCount_;
    const auto payloadBeforeRollback = emissionPayloadBytes_;
    retainer_.rollback(checkpoint.retained);
    recordCount_ = checkpoint.records;
    semanticBytes_ = checkpoint.semanticBytes;
    emissionHandleCount_ = checkpoint.emissionHandles;
    emissionPayloadBytes_ = checkpoint.emissionPayloadBytes;
    rectCount_ = checkpoint.rects;
    sparseCounts_.values = checkpoint.sparse;
    clearPins(storage_->surfaces, checkpoint.pins[0], surfaceCount_);
    clearPins(storage_->textures, checkpoint.pins[1], textureCount_);
    clearPins(storage_->buffers, checkpoint.pins[2], bufferCount_);
    clearPins(storage_->shaders, checkpoint.pins[3], shaderCount_);
    clearPins(storage_->declarations, checkpoint.pins[4], declarationCount_);
    clearPins(storage_->queries, checkpoint.pins[5], queryCount_);
    // A failed admission after the owner is already full has no speculative
    // record slot to clear; indexing records[MaxRecords] would be OOB.
    if (recordCount_ < MaxRecords) {
      storage_->records[recordCount_] = {};
    }
    for (std::size_t i = checkpoint.records;
         i < std::min(recordsBeforeRollback, Storage::maxWireRecords); ++i) {
      storage_->wireRecords[i] = {};
    }
    for (std::size_t i = checkpoint.emissionHandles;
         i < std::min(handlesBeforeRollback, Storage::maxWireHandles); ++i) {
      storage_->wireHandles[i] = {};
    }
    if (checkpoint.emissionPayloadBytes < payloadBeforeRollback &&
        checkpoint.emissionPayloadBytes < Storage::maxWirePayloadBytes) {
      const auto count = std::min(
          payloadBeforeRollback - checkpoint.emissionPayloadBytes,
          Storage::maxWirePayloadBytes - checkpoint.emissionPayloadBytes);
      std::fill_n(storage_->canonicalPayload.begin() +
                      checkpoint.emissionPayloadBytes,
                  count, std::byte{0});
    }
    rebuildPinIndexes();
  }

  // Settlement of the prepared witness. The capacity proof already aligned and
  // bounded this record's frontier, so publication is an assignment guarded by
  // the same destination-identity check the append entry made.
  bool cacheEmissionMetrics(
      const auto& prepared,
      const PreparedAppendAuthority& authority) noexcept {
    // Resource acquisition inside the serializer can invoke callbacks. Keep
    // the original post-source validity/frontier requalification; the removed
    // quota recheck was in compact copySparsePlan, not in admissibleAt.
    if (!authority.binds(prepared, *this)) return false;
    emissionPayloadBytes_ = prepared.nextEmissionPayloadBytes;
    emissionHandleCount_ = prepared.nextEmissionHandleCount;
    return true;
  }

  bool cacheEmissionMetrics(
      const PeSemanticPreparedConstantRecord& prepared,
      const PreparedAppendAuthority& authority) noexcept {
    if (!authority.binds(prepared, *this)) return false;
    emissionPayloadBytes_ = prepared.nextEmissionPayloadBytes;
    emissionHandleCount_ = prepared.nextEmissionHandleCount;
    return true;
  }

  template <typename PinArray>
  bool checkedPinHit(const PinIndex<kPinIndexCapacity>& index,
                     const PinArray& pins, std::size_t count,
                     const D9CWireObjectIdentity& identity,
                     const void* object, bool& found,
                     std::size_t& outPin) const noexcept {
    const PeSemanticPinIndexEntry* hit = nullptr;
    if (!findPinIndex(index, identity, pinIndexGeneration_, outPin, hit)) {
      found = false;
      return true;
    }
    // The table is only a locator.  The typed prefix is authoritative for
    // every field, including the physical pointer, and malformed hits fail
    // closed instead of becoming a new acquisition.
    if (hit == nullptr || hit->identity.kind != identity.kind ||
        hit->identity.generation != identity.generation ||
        hit->identity.objectId != identity.objectId || outPin >= count ||
        !pins[outPin].valid() || pins[outPin].object != object ||
        pins[outPin].identity.value.kind != identity.kind ||
        pins[outPin].identity.value.generation != identity.generation ||
        pins[outPin].identity.value.objectId != identity.objectId) {
      return false;
    }
    found = true;
    return true;
  }

  template <typename PinArray>
  bool findOrValidateExistingPin(
      const PinIndex<kPinIndexCapacity>& index,
      const PinArray& pins, std::size_t count,
      const D9CWireObjectIdentity& identity, const void* object,
      bool& existing, std::size_t& existingPin) const noexcept {
    return checkedPinHit(index, pins, count, identity, object, existing,
                         existingPin);
  }

  template <typename PinArray>
  static void clearPins(PinArray& pins, std::size_t from,
                        std::size_t& count) noexcept {
    for (std::size_t i = from; i < count; ++i) pins[i] = {};
    count = from;
  }

  // Owner-state-dependent header validation. The role/rule facts are resolved
  // once by the prepared witness; only the destination-relative checks below
  // depend on the owner, so a prepared append never re-reads the static tables.
  bool validAdmissionEnvelope(PeSemanticProducerKind producer,
                              std::uint32_t recordType,
                              std::uint32_t recordFlags,
                              std::uint64_t sourceOrdinal,
                              std::uint64_t recordOrdinal,
                              const RecordRule* rule,
                              bool producerMatches) const noexcept {
    if (recordCount_ >= MaxRecords || sourceOrdinal == 0u ||
        recordOrdinal == 0u || !producerMatches) return false;
    if (recordCount_ != 0u &&
        (recordOrdinal <= storage_->records[recordCount_ - 1u].recordOrdinal ||
         sourceOrdinal <= storage_->records[recordCount_ - 1u].sourceOrdinal)) {
      return false;
    }
    return rule != nullptr && recordType == rule->type &&
           (recordFlags & ~rule->allowedRecordFlags) == 0u &&
           producer != PeSemanticProducerKind::Count;
  }

  template <PeSemanticRecordInputLike Input>
  bool validAdmissionHeader(const Input& input,
                            const RecordRule* rule, bool producerMatches,
                            bool constantProducer,
                            bool upProducer) const noexcept {
    if (!validPeSemanticInput(input)) return false;
    const auto& staged = stagedPeSemanticInput(input);
    const auto& sparse = peSemanticSparse(input);
    const auto sourceOrdinal = peSemanticSourceOrdinal(input);
    const auto recordOrdinal = peSemanticRecordOrdinal(input);
    if (!validAdmissionEnvelope(staged.producer, peSemanticRecordType(input),
                                staged.recordFlags, sourceOrdinal,
                                recordOrdinal, rule, producerMatches)) {
      return false;
    }
    if ((!constantProducer && !staged.constantBytes.empty()) ||
        (!upProducer && (!sparse.upIndexData.empty() ||
                         !sparse.upVertexData.empty())) ||
        (staged.producer != PeSemanticProducerKind::Clear &&
         !staged.clearRects.empty())) {
      return false;
    }
    switch (staged.producer) {
      case PeSemanticProducerKind::Present:
        return staged.surface0.valid();
      case PeSemanticProducerKind::StretchRect:
      case PeSemanticProducerKind::UpdateSurface:
      case PeSemanticProducerKind::Readback:
        return staged.surface0.valid() && staged.surface1.valid();
      case PeSemanticProducerKind::ColorFill:
        return staged.surface0.valid();
      case PeSemanticProducerKind::UpdateTexture:
        return staged.texture0.valid() && staged.texture1.valid();
      case PeSemanticProducerKind::QueryIssue:
        return staged.query.valid();
      case PeSemanticProducerKind::ReszDepthResolve:
        return staged.surface0.valid() && staged.texture0.valid();
      case PeSemanticProducerKind::GenerateMipmaps:
        return staged.texture0.valid();
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        return true;
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        return validConstantInput(input);
      case PeSemanticProducerKind::Clear:
        return staged.clear.rectCount == staged.clearRects.size();
      case PeSemanticProducerKind::Count:
        return false;
    }
    return false;
  }

  template <PeSemanticRecordInputLike Input>
  bool validAdmissionHeader(const Input& input) const noexcept {
    const auto& staged = stagedPeSemanticInput(input);
    const auto* policy = peSemanticProducerPolicy(peSemanticRecordType(input));
    return validAdmissionHeader(
        input, recordRule(peSemanticRecordType(input)),
        policy != nullptr && policy->kind == staged.producer,
        isPeSemanticConstantProducer(staged.producer),
        isPeSemanticUpProducer(staged.producer));
  }

  template <PeSemanticRecordInputLike Input, std::size_t IdentityCapacity,
            typename AdmissionPlanT>
  bool validAdmissionHeader(
      const Input& input,
      const PeSemanticPreparedRecordT<IdentityCapacity, AdmissionPlanT>&
          prepared) const noexcept {
    return validAdmissionHeader(input, prepared.rule,
                                prepared.producerMatchesRecordType,
                                prepared.constantProducer,
                                prepared.upProducer);
  }

  bool validAdmissionHeader(
      const PeSemanticConstantRecordInput& input,
      const PeSemanticPreparedConstantRecord& prepared) const noexcept {
    return validPeSemanticInput(input) &&
           isPeSemanticConstantProducer(input.producer) &&
           validAdmissionEnvelope(
               input.producer, input.recordType, input.recordFlags,
               input.sourceOrdinal, input.recordOrdinal, prepared.rule,
               prepared.producerMatchesRecordType) &&
           validConstantInput(input.producer, input.setConst,
                              input.constantBytes);
  }

  bool validAdmissionHeader(
      const PeSemanticSparsePlanRecordInput& input,
      const auto& prepared) const noexcept {
    const auto* source = input.qualifiedSource();
    return source != nullptr && !source->fullSnapshot &&
           isPeSemanticSparseProducer(input.producer) &&
           validAdmissionEnvelope(
               input.producer, input.recordType, input.recordFlags,
               input.sourceOrdinal, input.recordOrdinal, prepared.rule,
               prepared.producerMatchesRecordType);
  }

  template <PeSemanticRecordInputLike Input>
  bool copyFixedValues(const Input& input, PeSemanticRecordSlot& slot) noexcept {
    const auto& staged = stagedPeSemanticInput(input);
    slot.producer = staged.producer;
    slot.recordType = peSemanticRecordType(input);
    slot.recordFlags = staged.recordFlags;
    slot.sourceOrdinal = peSemanticSourceOrdinal(input);
    slot.recordOrdinal = peSemanticRecordOrdinal(input);
    // Generic call-local staging selects only this producer's fixed family;
    // compact and constant inputs use their smaller staging types instead.
    switch (staged.producer) {
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        slot.draw = staged.draw;
        break;
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsBoolConstant:
      case PeSemanticProducerKind::PsIntConstant:
        slot.setConst = staged.setConst;
        break;
      case PeSemanticProducerKind::Clear:
        slot.clear = staged.clear;
        break;
      case PeSemanticProducerKind::Present:
        slot.present = staged.present;
        break;
      case PeSemanticProducerKind::StretchRect:
        slot.stretchRect = staged.stretchRect;
        break;
      case PeSemanticProducerKind::ColorFill:
        slot.colorFill = staged.colorFill;
        break;
      case PeSemanticProducerKind::UpdateSurface:
        slot.updateSurface = staged.updateSurface;
        break;
      case PeSemanticProducerKind::QueryIssue:
        slot.queryIssue = staged.queryIssue;
        break;
      case PeSemanticProducerKind::UpdateTexture:
      case PeSemanticProducerKind::Readback:
      case PeSemanticProducerKind::ReszDepthResolve:
      case PeSemanticProducerKind::GenerateMipmaps:
      case PeSemanticProducerKind::Count:
        break;
    }
    return true;
  }

  bool copyFixedValues(const PeSemanticConstantRecordInput& input,
                       PeSemanticConstantRecordStaging& slot) noexcept {
    slot.setConst = input.setConst;
    return true;
  }

  bool copyFixedValues(
      const PeSemanticSparsePlanRecordInput& input,
      PeSemanticSparsePlanRecordStaging& slot) noexcept {
    (void)input;
    (void)slot;
    // The draw header is derived by writeSparsePlanPayloadQualified(), where
    // the writer already owns the fresh qualified source callback.
    return true;
  }

  template <PeSemanticRecordInputLike Input>
  static bool validConstantInput(const Input& input) noexcept {
    const auto& staged = stagedPeSemanticInput(input);
    return validConstantInput(staged.producer, staged.setConst,
                              staged.constantBytes);
  }

  static bool validConstantInput(
      PeSemanticProducerKind producer,
      const D9CCommandChunkWireSetConst& setConst,
      std::span<const std::byte> constantBytes) noexcept {
    const bool boolean = producer == PeSemanticProducerKind::VsBoolConstant ||
                         producer == PeSemanticProducerKind::PsBoolConstant;
    const auto elementBytes = boolean ? 4u : 16u;
    const auto expected = static_cast<std::uint64_t>(
        setConst.registerCount) * elementBytes;
    const auto limit = boolean ? 16u : 256u;
    return setConst.registerCount <= limit && expected == constantBytes.size();
  }

  template <typename Object, typename Identity>
  PinIndex<kPinIndexCapacity>& pinIndex() noexcept {
    if constexpr (std::is_same_v<Object, D9CSurface>)
      return storage_->surfacePinIndex;
    if constexpr (std::is_same_v<Object, D9CTexture>)
      return storage_->texturePinIndex;
    if constexpr (std::is_same_v<Object, D9CBuffer>)
      return storage_->bufferPinIndex;
    if constexpr (std::is_same_v<Object, D9CShader>)
      return storage_->shaderPinIndex;
    if constexpr (std::is_same_v<Object, D9CVertexDecl>)
      return storage_->declarationPinIndex;
    return storage_->queryPinIndex;
  }

  template <typename Object, typename Identity, typename Ref, typename PinArray,
            typename Retain, typename Prepared,
            typename MappingEntry = typename Prepared::IdentityEntry>
  bool pinPrepared(Ref ref, PinArray& pins, std::size_t& count,
                   std::uint32_t& out,
                   const Prepared& prepared,
                   Retain retain,
                   std::uint32_t* wireIndex = nullptr,
                   const MappingEntry* qualifiedMapping = nullptr) noexcept {
    if (wireIndex) *wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
    if (!ref.object) {
      if (ref.identity.kind != 0u || ref.identity.generation != 0u ||
          ref.identity.objectId != 0u) {
        return false;
      }
      out = kPeSemanticNoSlot;
      return true;
    }
    if (!ref.valid()) return false;
    const Identity identity{.value = ref.identity};
    if (!identity.valid()) return false;
    const auto* mapping = qualifiedMapping;
    if (mapping != nullptr) {
      if (!std::remove_cvref_t<decltype(
              prepared.identityWitness)>::sameIdentity(mapping->identity,
                                                       ref.identity) ||
          mapping->object != ref.object) return false;
    } else {
      mapping = prepared.identityWitness.findPin(ref);
    }
    if (!mapping) return false;
    const auto preparedPin = mapping->pin;
    if (wireIndex) {
      if (mapping->wireIndex >= prepared.identityWitness.wireCount) return false;
      *wireIndex = mapping->wireIndex;
    }
    const std::size_t index = preparedPin;
    if (index < count) {
      if (!pins[index].valid() || pins[index].object != ref.object ||
          pins[index].identity.value.kind != identity.value.kind ||
          pins[index].identity.value.generation != identity.value.generation ||
          pins[index].identity.value.objectId != identity.value.objectId) {
        return false;
      }
      out = preparedPin;
      return true;
    }
    if (index != count) return false;
    if (index == MaxPins ||
        !retain(static_cast<Object*>(ref.object), retainedCheckpoint_))
      return false;
    pins[index] = {.object = static_cast<Object*>(ref.object),
                   .identity = identity};
    auto& indexTable = pinIndex<Object, Identity>();
    if (!insertPinIndex(indexTable, identity.value,
                        static_cast<std::uint32_t>(index),
                        pinIndexGeneration_)) {
      return false;
    }
    ++count;
    out = static_cast<std::uint32_t>(index);
    return true;
  }

  // identityWitness.add() at compact admission proved the expected kind and
  // nonzero generation/objectId; the mapping and fresh full source
  // identity/pointer equality below are the typed validity proof here.
  template <typename Object, typename Identity, typename Prepared>
  bool makeQualifiedBinding(const PeWireObjectRef& ref,
                            const PeSemanticBindingLocator& locator,
                            const Prepared& prepared, std::size_t globalFirst,
                            QualifiedBinding<Object, Identity>& out) const noexcept {
    if (locator.mappingOrdinal >= prepared.identityWitness.count) return false;
    const auto& mapping = prepared.identityWitness.entries[locator.mappingOrdinal];
    if (!std::remove_cvref_t<decltype(prepared.identityWitness)>::sameIdentity(
            mapping.identity, ref.identity) ||
        mapping.object != ref.object || locator.wireIndex != mapping.wireIndex ||
        mapping.wireIndex >= prepared.identityWitness.wireCount ||
        globalFirst > std::numeric_limits<std::uint32_t>::max() -
                          mapping.wireIndex) {
      return false;
    }
    out = QualifiedBinding<Object, Identity>(static_cast<Object*>(ref.object),
                                             ref.identity, mapping.pin,
                                             mapping.wireIndex);
    return true;
  }

  template <typename Object, typename Identity, typename PinArray, typename Retain>
  bool pinPreparedQualifiedBinding(
      PinArray& pins, std::size_t& count, std::uint32_t& out,
      std::uint32_t* wireIndex,
      const QualifiedBinding<Object, Identity>& binding, Retain retain) noexcept {
    if (wireIndex) *wireIndex = binding.wire_;
    const std::size_t index = binding.pin_;
    const Identity identity{.value = binding.identity_};
    if (index < count) {
      if (!pins[index].valid() || pins[index].object != binding.object_ ||
          pins[index].identity.value.kind != identity.value.kind ||
          pins[index].identity.value.generation != identity.value.generation ||
          pins[index].identity.value.objectId != identity.value.objectId) {
        return false;
      }
      out = static_cast<std::uint32_t>(index);
      return true;
    }
    if (index != count || index == MaxPins ||
        !retain(binding.object_, retainedCheckpoint_)) {
      return false;
    }
    pins[index] = {.object = binding.object_, .identity = identity};
    auto& indexTable = pinIndex<Object, Identity>();
    if (!insertPinIndex(indexTable, identity.value,
                        static_cast<std::uint32_t>(index),
                        pinIndexGeneration_)) {
      return false;
    }
    ++count;
    out = static_cast<std::uint32_t>(index);
    return true;
  }

  template <typename Ref>
  static bool typedOptionalRef(const PeWireObjectRef& source,
                               Ref& out) noexcept {
    out = qualifyLocalRef<Ref>(source);
    const bool hasObject = source.object != nullptr;
    const bool hasIdentity = source.identity.kind != 0u ||
                             source.identity.generation != 0u ||
                             source.identity.objectId != 0u;
    return hasObject ? out.valid() : !hasIdentity;
  }

  template <PeSemanticRecordInputLike Input>
  bool copyDirectPins(const Input& input,
                      PeSemanticRecordSlot& slot,
                      const auto& prepared) noexcept {
    const auto& staged = stagedPeSemanticInput(input);
    const auto retainSurface = [this](D9CSurface* p,
                                      D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainSurface(p, acquired, warmRetainerCapacity);
    };
    const auto retainTexture = [this](D9CTexture* p,
                                      D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainTexture(p, acquired, warmRetainerCapacity);
    };
    const auto retainBuffer = [this](D9CBuffer* p,
                                     D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainBuffer(p, acquired, warmRetainerCapacity);
    };
    const auto retainShader = [this](D9CShader* p,
                                     D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainShader(p, acquired, warmRetainerCapacity);
    };
    const auto retainDeclaration = [this](D9CVertexDecl* p,
                                          D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainVdecl(p, acquired, warmRetainerCapacity);
    };
    const auto retainQuery = [this](D9CQuery* p,
                                    D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainQuery(p, acquired, warmRetainerCapacity);
    };
    return pinPrepared<D9CSurface, PeSemanticSurfaceIdentity>(
               staged.surface0, storage_->surfaces, surfaceCount_, slot.surface0,
               prepared, retainSurface) &&
           pinPrepared<D9CSurface, PeSemanticSurfaceIdentity>(
               staged.surface1, storage_->surfaces, surfaceCount_, slot.surface1,
               prepared, retainSurface) &&
           pinPrepared<D9CTexture, PeSemanticTextureIdentity>(
               staged.texture0, storage_->textures, textureCount_, slot.texture0,
               prepared, retainTexture) &&
           pinPrepared<D9CTexture, PeSemanticTextureIdentity>(
               staged.texture1, storage_->textures, textureCount_, slot.texture1,
               prepared, retainTexture) &&
           pinPrepared<D9CBuffer, PeSemanticBufferIdentity>(
               staged.buffer0, storage_->buffers, bufferCount_, slot.buffer0,
               prepared, retainBuffer) &&
           pinPrepared<D9CBuffer, PeSemanticBufferIdentity>(
               staged.buffer1, storage_->buffers, bufferCount_, slot.buffer1,
               prepared, retainBuffer) &&
           pinPrepared<D9CShader, PeSemanticShaderIdentity>(
               staged.shader0, storage_->shaders, shaderCount_, slot.shader0,
               prepared, retainShader) &&
           pinPrepared<D9CShader, PeSemanticShaderIdentity>(
               staged.shader1, storage_->shaders, shaderCount_, slot.shader1,
               prepared, retainShader) &&
           pinPrepared<D9CVertexDecl, PeSemanticDeclarationIdentity>(
               staged.declaration, storage_->declarations, declarationCount_,
               slot.declaration, prepared, retainDeclaration) &&
           pinPrepared<D9CQuery, PeSemanticQueryIdentity>(
               staged.query, storage_->queries, queryCount_, slot.query, prepared,
               retainQuery);
  }

  template <PeSemanticRecordInputLike Input>
  bool copyPreparedFixedAndDirect(
      const Input& input, PeSemanticRecordSlot& slot,
      const auto& prepared,
      const PreparedAppendAuthority&) noexcept {
    if (!copyFixedValues(input, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::Fixed;
      return false;
    }
    if (!copyDirectPins(input, slot, prepared)) {
      lastAdmissionFailure_ = AdmissionFailure::DirectPins;
      return false;
    }
    return true;
  }

  bool copyPreparedFixedAndDirect(
      const PeSemanticConstantRecordInput& input,
      PeSemanticConstantRecordStaging& slot,
      const PeSemanticPreparedConstantRecord&,
      const PreparedAppendAuthority&) noexcept {
    if (copyFixedValues(input, slot)) return true;
    lastAdmissionFailure_ = AdmissionFailure::Fixed;
    return false;
  }

  bool copyPreparedFixedAndDirect(
      const PeSemanticSparsePlanRecordInput& input,
      PeSemanticSparsePlanRecordStaging& slot,
      const auto&, const PreparedAppendAuthority&) noexcept {
    if (copyFixedValues(input, slot)) return true;
    lastAdmissionFailure_ = AdmissionFailure::Fixed;
    return false;
  }

  bool appendLogicalBytes(std::span<const std::byte> source,
                          PeSemanticArenaRange& range) noexcept {
    if (source.empty()) { range = {}; return true; }
    if (semanticBytes_ > MaxSemanticBytes ||
        source.size() > MaxSemanticBytes - semanticBytes_ ||
        source.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    // semanticBytes_ remains the logical quota frontier, not a byte arena.
    range = {.offset = 0u, .count = static_cast<std::uint32_t>(source.size())};
    semanticBytes_ += source.size();
    return true;
  }

  bool appendLogicalBytes(std::span<const std::byte> source) noexcept {
    PeSemanticArenaRange ignored;
    return appendLogicalBytes(source, ignored);
  }

  template <PeSemanticRecordInputLike Input>
  bool copySparse(const Input& input,
                  PeSemanticRecordSlot& slot,
                  const auto& prepared) noexcept {
    const auto& s = peSemanticSparse(input);
    const auto& layout = prepared.sparseLayout;
    const bool sparseProducer = isPeSemanticSparseProducer(
        stagedPeSemanticInput(input).producer);
    if (!layout.valid ||
        layout.sectionCount > D9C_COMMAND_CHUNK_SECTION_COUNT) {
      lastAdmissionFailure_ = AdmissionFailure::SparseSchema;
      return false;
    }
    // Generic inputs retain the old oracle envelope even when their producer
    // does not publish a sparse payload. Their foreign sparse fields still
    // consume logical quotas and still go through the occurrence-qualified
    // pin path; only the closed typed producer adapters may prove absence.
    const auto withinSchema = [](std::uint16_t kind,
                                 std::size_t count) noexcept {
      const auto* rule = sectionRule(kind);
      return rule != nullptr && count <= rule->maxCount;
    };
    if (!withinSchema(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
                      s.renderStates.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_TEXTURE, s.textures.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_STREAM, s.streams.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_SHADER, s.shaders.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
                      s.vertexInputs.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
                      s.indexBuffers.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
                      s.renderTargets.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
                      s.depthStencils.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_VIEWPORT,
                      s.viewports.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_SCISSOR, s.scissors.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_MATERIAL, s.materials.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE,
                      s.clipPlanes.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
                      s.textureStageStates.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                      s.samplerStates.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_TRANSFORM, s.transforms.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_LIGHT, s.lights.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
                      s.lightEnables.size())) {
      lastAdmissionFailure_ = AdmissionFailure::SparseSchema;
      return false;
    }
    const auto& quota = sparseProducer ? layout.quotaDeltas
                                       : prepared.plan.sparseCounts;
    const auto consume = [&](std::size_t kind, std::size_t sourceCount,
                             PeSemanticArenaRange& range) noexcept {
      if (kind >= quota.size() || sourceCount != quota[kind]) return false;
      const auto count = quota[kind];
      if (count == 0u) {
        range = {};
        return true;
      }
      if (sparseCounts_.values[kind] > MaxSparseValues ||
          count > MaxSparseValues - sparseCounts_.values[kind]) return false;
      range = {.offset = 0u, .count = static_cast<std::uint32_t>(count)};
      sparseCounts_.values[kind] += count;
      return true;
    };
    if (!consume(kRenderStates, s.renderStates.size(), slot.renderStates) ||
        !consume(kViewports, s.viewports.size(), slot.viewports) ||
        !consume(kScissors, s.scissors.size(), slot.scissors) ||
        !consume(kMaterials, s.materials.size(), slot.materials) ||
        !consume(kClipPlanes, s.clipPlanes.size(), slot.clipPlanes) ||
        !consume(kTextureStageStates, s.textureStageStates.size(),
                 slot.textureStageStates) ||
        !consume(kSamplerStates, s.samplerStates.size(), slot.samplerStates) ||
        !consume(kTransforms, s.transforms.size(), slot.transforms) ||
        !consume(kLights, s.lights.size(), slot.lights) ||
        !consume(kLightEnables, s.lightEnables.size(), slot.lightEnables)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseArena;
      return false;
    }
    return copyBindingSparse(s, slot, prepared);
  }

  template <PeSemanticRecordInputLike Input>
  bool copyVariablePayloads(const Input& input,
                            PeSemanticRecordSlot& slot) noexcept {
    const auto& staged = stagedPeSemanticInput(input);
    const auto& sparse = peSemanticSparse(input);
    slot.vsFloatConstant = {.startRegister = sparse.vsFloatConstants.startRegister,
                            .registerCount = sparse.vsFloatConstants.registerCount};
    slot.vsIntConstant = {.startRegister = sparse.vsIntConstants.startRegister,
                          .registerCount = sparse.vsIntConstants.registerCount};
    slot.vsBoolConstant = {.startRegister = sparse.vsBoolConstants.startRegister,
                           .registerCount = sparse.vsBoolConstants.registerCount};
    slot.psFloatConstant = {.startRegister = sparse.psFloatConstants.startRegister,
                            .registerCount = sparse.psFloatConstants.registerCount};
    slot.psIntConstant = {.startRegister = sparse.psIntConstants.startRegister,
                          .registerCount = sparse.psIntConstants.registerCount};
    slot.psBoolConstant = {.startRegister = sparse.psBoolConstants.startRegister,
                           .registerCount = sparse.psBoolConstants.registerCount};
    if (!appendLogicalBytes(staged.constantBytes, slot.constantBytes) ||
        !appendLogicalBytes(sparse.vsFloatConstants.registerBytes,
                            slot.vsFloatConstants) ||
        !appendLogicalBytes(sparse.vsIntConstants.registerBytes,
                            slot.vsIntConstants) ||
        !appendLogicalBytes(sparse.vsBoolConstants.registerBytes,
                            slot.vsBoolConstants) ||
        !appendLogicalBytes(sparse.psFloatConstants.registerBytes,
                            slot.psFloatConstants) ||
        !appendLogicalBytes(sparse.psIntConstants.registerBytes,
                            slot.psIntConstants) ||
        !appendLogicalBytes(sparse.psBoolConstants.registerBytes,
                            slot.psBoolConstants) ||
        !appendLogicalBytes(sparse.upIndexData, slot.upIndexBytes) ||
        !appendLogicalBytes(sparse.upVertexData, slot.upVertexBytes)) return false;
    slot.clearRects = {};
    if (!staged.clearRects.empty()) {
      if (rectCount_ > MaxRects || staged.clearRects.size() > MaxRects - rectCount_ ||
          staged.clearRects.size() > std::numeric_limits<std::uint32_t>::max()) return false;
      slot.clearRects = {.offset = 0u,
                         .count = static_cast<std::uint32_t>(staged.clearRects.size())};
      rectCount_ += staged.clearRects.size();
    }
    return true;
  }

  bool copySparsePlan(const auto& prepared) noexcept {
    // The private authority has already proved the active-category quota and
    // unchanged destination frontier. This first adjacent claim publishes only
    // sparse logical counts; record/handle/payload emission frontiers remain
    // untouched until materialization and cacheEmissionMetrics.
    const auto& quota = prepared.sparseLayout.quotaDeltas;
    if (!forEachPreparedSparseQuota(
            prepared.sparseLayout,
            [&](std::size_t kind, std::uint32_t count) noexcept {
              if (kind >= quota.size()) return false;
              sparseCounts_.values[kind] +=
                  static_cast<std::size_t>(count);
              return true;
            })) {
      lastAdmissionFailure_ = AdmissionFailure::SparseArena;
      return false;
    }
    // Binding acquisition is fused into the canonical serializer below. The
    // pure preparation walk above remains the sole source qualification pass;
    // this method only claims logical quotas and leaves no second binding
    // traversal between preparation and emission.
    return true;
  }

  void claimCompactVariableQuota(const auto& prepared) noexcept {
    // Compact plans serialize these borrowed ranges directly from the plan;
    // only the aggregate logical quota is committed. Do not populate the
    // seven logical range fields or their six constant headers in staging.
    // The authority binds the semantic frontier and the prepared delta. The
    // physical payload bounds are still checked by materialization; this
    // assignment only publishes the already-proven logical quota.
    semanticBytes_ += prepared.sparseLayout.semanticDelta;
  }

  bool copyPreparedCompactSparseAndVariable(
      const PeSemanticSparsePlanRecordInput& input,
      const auto& prepared,
      const PreparedAppendAuthority& authority) noexcept {
    // These two logical claims are adjacent and callback-free. Bind the source
    // once, then publish sparse counts followed by semantic bytes; the later
    // serializer and emission-metric publication retain their own
    // requalification checks.
    if (!authority.bindsSource(input, prepared, *this) ||
        !prepared.sparseLayout.valid)
      return false;
    if (!copySparsePlan(prepared)) return false;
    claimCompactVariableQuota(prepared);
    return true;
  }

  template <PeSemanticRecordInputLike Input>
  bool copyPreparedSparseAndVariable(
      const Input& input, PeSemanticRecordSlot& slot,
      const auto& prepared,
      const PreparedAppendAuthority& authority) noexcept {
    (void)authority;
    if (!copySparse(input, slot, prepared)) return false;
    if (!copyVariablePayloads(input, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::VariablePayload;
      return false;
    }
    return true;
  }

  bool copyPreparedSparseAndVariable(
      const PeSemanticConstantRecordInput& input,
      PeSemanticConstantRecordStaging& slot,
      const PeSemanticPreparedConstantRecord&,
      const PreparedAppendAuthority&) noexcept {
    (void)slot;
    if (appendLogicalBytes(input.constantBytes)) return true;
    lastAdmissionFailure_ = AdmissionFailure::VariablePayload;
    return false;
  }

  bool copyPreparedSparseAndVariable(
      const PeSemanticSparsePlanRecordInput& input,
      PeSemanticSparsePlanRecordStaging&,
      const auto& prepared,
      const PreparedAppendAuthority& authority) noexcept {
    if (!copyPreparedCompactSparseAndVariable(input, prepared, authority)) {
      if (lastAdmissionFailure_ == AdmissionFailure::None)
        lastAdmissionFailure_ = AdmissionFailure::Sparse;
      return false;
    }
    return true;
  }

  bool copyBindingSparse(const SparseStateInput& s,
                         PeSemanticRecordSlot& slot,
                         const auto& prepared) noexcept {
    // The small per-family routines below only store occurrence-qualified
    // locators. They validate the source's kind before asking the retainer for
    // a pin, so an invalid/foreign binding cannot leak a retain; the wire row
    // itself is written directly from the borrowed source during materialize.
    const bool sparseProducer = isPeSemanticSparseProducer(
        prepared.producer);
    const auto& quota = sparseProducer ? prepared.sparseLayout.quotaDeltas
                                       : prepared.plan.sparseCounts;
    if (s.textures.size() != quota[kTextures] ||
        s.streams.size() != quota[kStreams] ||
        s.shaders.size() != quota[kShaders] ||
        s.vertexInputs.size() != quota[kVertexInputs] ||
        s.indexBuffers.size() != quota[kIndexBuffers] ||
        s.renderTargets.size() != quota[kRenderTargets] ||
        s.depthStencils.size() != quota[kDepthStencils]) {
      return false;
    }
    if (!copyTextures(s.textures, slot, prepared)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseTextures;
      return false;
    }
    if (!copyStreams(s.streams, slot, prepared)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseStreams;
      return false;
    }
    if (!copyShaders(s.shaders, slot, prepared)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseShaders;
      return false;
    }
    if (!copyVertexInputs(s.vertexInputs, slot, prepared)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseVertexInputs;
      return false;
    }
    if (!copyIndexBuffers(s.indexBuffers, slot, prepared)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseIndexBuffers;
      return false;
    }
    if (!copyRenderTargets(s.renderTargets, slot, prepared)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseRenderTargets;
      return false;
    }
    if (!copyDepthStencils(s.depthStencils, slot, prepared)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseDepthStencils;
      return false;
    }
    return true;
  }

  bool copyTextures(std::span<const SparseBindingInput<D9CCommandChunkWireTextureBinding>> source,
                    PeSemanticRecordSlot& slot,
                    const auto& prepared) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kTextures]) return false;
    const auto start = sparseCounts_.values[kTextures];
    for (std::size_t occurrence = 0u; occurrence < source.size(); ++occurrence) {
      const auto& item = source[occurrence];
      const auto& locator = prepared.bindingOccurrences.values[
          prepared.bindingOccurrences.ranges[0].offset + occurrence];
      if (locator.occurrence != occurrence ||
          locator.wireValid != static_cast<bool>(item.wire.valid) ||
          (locator.wireValid &&
           locator.hasPin != (item.object.object != nullptr))) return false;
      if (item.wire.valid) {
        auto ref = TextureRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          std::uint32_t pin = kPeSemanticNoSlot;
          std::uint32_t wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
          if (!ref.valid() ||
              locator.mappingOrdinal >= prepared.identityWitness.count ||
              !pinPrepared<D9CTexture, PeSemanticTextureIdentity>(
                  ref, storage_->textures, textureCount_, pin, prepared,
                  retainTextureFn(), &wireIndex,
                  &prepared.identityWitness.entries[locator.mappingOrdinal]) ||
              prepared.identityWitness.entries[locator.mappingOrdinal].pin != pin ||
              prepared.identityWitness.entries[locator.mappingOrdinal].wireIndex != wireIndex ||
              locator.wireIndex != wireIndex)
            return false;
        }
      }
      ++sparseCounts_.values[kTextures];
    }
    slot.textures = {.offset = static_cast<std::uint32_t>(start),
                     .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyStreams(std::span<const SparseBindingInput<D9CCommandChunkWireStreamBinding>> source,
                   PeSemanticRecordSlot& slot,
                   const auto& prepared) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kStreams]) return false;
    const auto start = sparseCounts_.values[kStreams];
    for (std::size_t occurrence = 0u; occurrence < source.size(); ++occurrence) {
      const auto& item = source[occurrence];
      const auto& locator = prepared.bindingOccurrences.values[
          prepared.bindingOccurrences.ranges[1].offset + occurrence];
      if (locator.occurrence != occurrence ||
          locator.wireValid != static_cast<bool>(item.wire.valid) ||
          (locator.wireValid &&
           locator.hasPin != (item.object.object != nullptr))) return false;
      if (item.wire.valid) {
        auto ref = BufferRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          std::uint32_t pin = kPeSemanticNoSlot;
          std::uint32_t wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
          if (!ref.valid() ||
              locator.mappingOrdinal >= prepared.identityWitness.count ||
              !pinPrepared<D9CBuffer, PeSemanticBufferIdentity>(
                  ref, storage_->buffers, bufferCount_, pin, prepared,
                  retainBufferFn(), &wireIndex,
                  &prepared.identityWitness.entries[locator.mappingOrdinal]) ||
              prepared.identityWitness.entries[locator.mappingOrdinal].pin != pin ||
              prepared.identityWitness.entries[locator.mappingOrdinal].wireIndex != wireIndex ||
              locator.wireIndex != wireIndex)
            return false;
        }
      }
      ++sparseCounts_.values[kStreams];
    }
    slot.streams = {.offset = static_cast<std::uint32_t>(start),
                    .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyShaders(std::span<const SparseBindingInput<D9CCommandChunkWireShaderBinding>> source,
                   PeSemanticRecordSlot& slot,
                   const auto& prepared) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kShaders]) return false;
    const auto start = sparseCounts_.values[kShaders];
    for (std::size_t occurrence = 0u; occurrence < source.size(); ++occurrence) {
      const auto& item = source[occurrence];
      const auto& locator = prepared.bindingOccurrences.values[
          prepared.bindingOccurrences.ranges[2].offset + occurrence];
      if (locator.occurrence != occurrence ||
          locator.wireValid != static_cast<bool>(item.wire.valid) ||
          (locator.wireValid &&
           locator.hasPin != (item.object.object != nullptr))) return false;
      if (item.wire.valid) {
        auto ref = ShaderRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          std::uint32_t pin = kPeSemanticNoSlot;
          std::uint32_t wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
          if (!ref.valid() ||
              locator.mappingOrdinal >= prepared.identityWitness.count ||
              !pinPrepared<D9CShader, PeSemanticShaderIdentity>(
                  ref, storage_->shaders, shaderCount_, pin, prepared,
                  retainShaderFn(), &wireIndex,
                  &prepared.identityWitness.entries[locator.mappingOrdinal]) ||
              prepared.identityWitness.entries[locator.mappingOrdinal].pin != pin ||
              prepared.identityWitness.entries[locator.mappingOrdinal].wireIndex != wireIndex ||
              locator.wireIndex != wireIndex)
            return false;
        }
      }
      ++sparseCounts_.values[kShaders];
    }
    slot.shaders = {.offset = static_cast<std::uint32_t>(start),
                    .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyVertexInputs(std::span<const SparseBindingInput<D9CCommandChunkWireVertexInput>> source,
                        PeSemanticRecordSlot& slot,
                        const auto& prepared) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kVertexInputs]) return false;
    const auto start = sparseCounts_.values[kVertexInputs];
    for (std::size_t occurrence = 0u; occurrence < source.size(); ++occurrence) {
      const auto& item = source[occurrence];
      const auto& locator = prepared.bindingOccurrences.values[
          prepared.bindingOccurrences.ranges[3].offset + occurrence];
      if (locator.occurrence != occurrence ||
          locator.wireValid != static_cast<bool>(item.wire.valid) ||
          (locator.wireValid &&
           locator.hasPin != (item.object.object != nullptr))) return false;
      if (item.wire.valid && item.wire.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION) {
        auto ref = DeclarationRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          std::uint32_t pin = kPeSemanticNoSlot;
          std::uint32_t wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
          if (!ref.valid() ||
              locator.mappingOrdinal >= prepared.identityWitness.count ||
              !pinPrepared<D9CVertexDecl, PeSemanticDeclarationIdentity>(
                  ref, storage_->declarations, declarationCount_, pin,
                  prepared, retainDeclarationFn(), &wireIndex,
                  &prepared.identityWitness.entries[locator.mappingOrdinal]) ||
              prepared.identityWitness.entries[locator.mappingOrdinal].pin != pin ||
              prepared.identityWitness.entries[locator.mappingOrdinal].wireIndex != wireIndex ||
              locator.wireIndex != wireIndex)
            return false;
        }
      } else if (item.wire.valid && item.wire.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF && item.object.object) {
        return false;
      }
      ++sparseCounts_.values[kVertexInputs];
    }
    slot.vertexInputs = {.offset = static_cast<std::uint32_t>(start),
                         .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyIndexBuffers(std::span<const SparseBindingInput<D9CCommandChunkWireIndexBinding>> source,
                        PeSemanticRecordSlot& slot,
                        const auto& prepared) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kIndexBuffers]) return false;
    const auto start = sparseCounts_.values[kIndexBuffers];
    for (std::size_t occurrence = 0u; occurrence < source.size(); ++occurrence) {
      const auto& item = source[occurrence];
      const auto& locator = prepared.bindingOccurrences.values[
          prepared.bindingOccurrences.ranges[4].offset + occurrence];
      if (locator.occurrence != occurrence ||
          locator.wireValid != static_cast<bool>(item.wire.valid) ||
          (locator.wireValid &&
           locator.hasPin != (item.object.object != nullptr))) return false;
      if (item.wire.valid) {
        auto ref = BufferRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          std::uint32_t pin = kPeSemanticNoSlot;
          std::uint32_t wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
          if (!ref.valid() ||
              locator.mappingOrdinal >= prepared.identityWitness.count ||
              !pinPrepared<D9CBuffer, PeSemanticBufferIdentity>(
                  ref, storage_->buffers, bufferCount_, pin, prepared,
                  retainBufferFn(), &wireIndex,
                  &prepared.identityWitness.entries[locator.mappingOrdinal]) ||
              prepared.identityWitness.entries[locator.mappingOrdinal].pin != pin ||
              prepared.identityWitness.entries[locator.mappingOrdinal].wireIndex != wireIndex ||
              locator.wireIndex != wireIndex)
            return false;
        }
      }
      ++sparseCounts_.values[kIndexBuffers];
    }
    slot.indexBuffers = {.offset = static_cast<std::uint32_t>(start),
                         .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyRenderTargets(std::span<const SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>> source,
                         PeSemanticRecordSlot& slot,
                         const auto& prepared) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kRenderTargets]) return false;
    const auto start = sparseCounts_.values[kRenderTargets];
    for (std::size_t occurrence = 0u; occurrence < source.size(); ++occurrence) {
      const auto& item = source[occurrence];
      const auto& locator = prepared.bindingOccurrences.values[
          prepared.bindingOccurrences.ranges[5].offset + occurrence];
      if (locator.occurrence != occurrence ||
          locator.wireValid != static_cast<bool>(item.wire.valid) ||
          (locator.wireValid &&
           locator.hasPin != (item.object.object != nullptr))) return false;
      if (item.wire.valid) {
        auto ref = SurfaceRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          std::uint32_t pin = kPeSemanticNoSlot;
          std::uint32_t wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
          if (!ref.valid() ||
              locator.mappingOrdinal >= prepared.identityWitness.count ||
              !pinPrepared<D9CSurface, PeSemanticSurfaceIdentity>(
                  ref, storage_->surfaces, surfaceCount_, pin, prepared,
                  retainSurfaceFn(), &wireIndex,
                  &prepared.identityWitness.entries[locator.mappingOrdinal]) ||
              prepared.identityWitness.entries[locator.mappingOrdinal].pin != pin ||
              prepared.identityWitness.entries[locator.mappingOrdinal].wireIndex != wireIndex ||
              locator.wireIndex != wireIndex)
            return false;
        }
      }
      ++sparseCounts_.values[kRenderTargets];
    }
    slot.renderTargets = {.offset = static_cast<std::uint32_t>(start),
                          .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyDepthStencils(std::span<const SparseBindingInput<D9CCommandChunkWireDepthStencilBinding>> source,
                         PeSemanticRecordSlot& slot,
                         const auto& prepared) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kDepthStencils]) return false;
    const auto start = sparseCounts_.values[kDepthStencils];
    for (std::size_t occurrence = 0u; occurrence < source.size(); ++occurrence) {
      const auto& item = source[occurrence];
      const auto& locator = prepared.bindingOccurrences.values[
          prepared.bindingOccurrences.ranges[6].offset + occurrence];
      if (locator.occurrence != occurrence ||
          locator.wireValid != static_cast<bool>(item.wire.valid) ||
          (locator.wireValid &&
           locator.hasPin != (item.object.object != nullptr))) return false;
      if (item.wire.valid) {
        auto ref = SurfaceRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          std::uint32_t pin = kPeSemanticNoSlot;
          std::uint32_t wireIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
          if (!ref.valid() ||
              locator.mappingOrdinal >= prepared.identityWitness.count ||
              !pinPrepared<D9CSurface, PeSemanticSurfaceIdentity>(
                  ref, storage_->surfaces, surfaceCount_, pin, prepared,
                  retainSurfaceFn(), &wireIndex,
                  &prepared.identityWitness.entries[locator.mappingOrdinal]) ||
              prepared.identityWitness.entries[locator.mappingOrdinal].pin != pin ||
              prepared.identityWitness.entries[locator.mappingOrdinal].wireIndex != wireIndex ||
              locator.wireIndex != wireIndex)
            return false;
        }
      }
      ++sparseCounts_.values[kDepthStencils];
    }
    slot.depthStencils = {.offset = static_cast<std::uint32_t>(start),
                          .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  // Typed retain functors keep the owner API free of type-erased object sinks.
  auto retainSurfaceFn() noexcept {
    return [this](D9CSurface* p, D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainSurface(p, acquired, warmRetainerCapacity);
    };
  }
  auto retainTextureFn() noexcept {
    return [this](D9CTexture* p, D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainTexture(p, acquired, warmRetainerCapacity);
    };
  }
  auto retainBufferFn() noexcept {
    return [this](D9CBuffer* p, D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainBuffer(p, acquired, warmRetainerCapacity);
    };
  }
  auto retainShaderFn() noexcept {
    return [this](D9CShader* p, D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainShader(p, acquired, warmRetainerCapacity);
    };
  }
  auto retainDeclarationFn() noexcept {
    return [this](D9CVertexDecl* p, D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainVdecl(p, acquired, warmRetainerCapacity);
    };
  }
  auto retainQueryFn() noexcept {
    return [this](D9CQuery* p, D3D9PePendingCommandRetainer::Acquired& acquired) {
      return retainer_.retainQuery(p, acquired, warmRetainerCapacity);
    };
  }

  std::size_t surfaceCount_ = 0u;
  std::size_t textureCount_ = 0u;
  std::size_t bufferCount_ = 0u;
  std::size_t shaderCount_ = 0u;
  std::size_t declarationCount_ = 0u;
  std::size_t queryCount_ = 0u;

  std::unique_ptr<Storage> storage_;
  bool ready_ = false;
  D3D9PePendingCommandRetainer retainer_{MaxPins};
  D3D9PePendingCommandRetainer::Acquired retainedCheckpoint_{};
  std::size_t recordCount_ = 0u;
  std::size_t semanticBytes_ = 0u;
  // Cached emission frontiers are updated with each admission and restored by
  // rollback. They are intentionally separate from semanticBytes_, which is
  // the preserved logical variable-byte quota, not physical wire payload size.
  std::size_t emissionHandleCount_ = 0u;
  std::size_t emissionPayloadBytes_ = 0u;
  std::size_t rectCount_ = 0u;
  std::size_t lastClearedBytes_ = 0u;
  std::uint32_t pinIndexGeneration_ = 1u;
  SparseCounts sparseCounts_{};
  std::uint64_t settledChunks_ = 0u;
  std::uint64_t lastSourceOrdinal_ = 0u;
  AdmissionFailure lastAdmissionFailure_ = AdmissionFailure::None;
  std::uint64_t lastRecordOrdinal_ = 0u;
  PeSemanticOwnerPhaseObserver* phaseObserver_ = nullptr;
  dxmt9::core::CopyMaterializationLedger* admissionLedger_ = nullptr;
  mutable dxmt9::core::CopyMaterializationLedger* exactWireLedger_ = nullptr;
  mutable std::size_t exactWireRetainedBytes_ = 0u;
};

// Bind the cold all-family refinement ledger to the exact immutable bytes
// emitted by the production owner. This is intentionally owner-generic so the
// native bounded owner uses the same relation as the 256-record production
// alias; compatibility builders do not participate.
template <typename Owner>
bool projectLastCommittedSemanticRecord(
    const Owner& owner, PeAllFamilySemanticTokenLedger& observer) noexcept {
  if (owner.size() == 0u) return false;
  PeSemanticExactFixedEmission emission{};
  if (!owner.emitExactFixed(emission) || !emission.valid()) return false;

  const auto& slot = owner.record(owner.size() - 1u);
  const auto& header = emission.transport.header;
  const auto recordIndex = header.recordCount - 1u;
  const auto recordOffset = static_cast<std::size_t>(header.recordTableOffset) +
      static_cast<std::size_t>(recordIndex) *
          sizeof(D9CCommandChunkWireRecordHeader);
  if (header.recordCount == 0u || recordOffset > emission.wire.size() ||
      sizeof(D9CCommandChunkWireRecordHeader) >
          emission.wire.size() - recordOffset) {
    return false;
  }
  D9CCommandChunkWireRecordHeader record{};
  std::memcpy(&record, emission.wire.data() + recordOffset, sizeof(record));
  if (record.type != slot.recordType || record.payloadSize == 0u ||
      record.payloadOffset > header.payloadArenaSize ||
      record.payloadSize > header.payloadArenaSize - record.payloadOffset ||
      record.firstHandle > header.handleCount ||
      record.handleCount > header.handleCount - record.firstHandle) {
    return false;
  }
  const auto payloadOffset =
      static_cast<std::size_t>(header.payloadArenaOffset) +
      record.payloadOffset;
  if (payloadOffset > emission.wire.size() ||
      record.payloadSize > emission.wire.size() - payloadOffset) {
    return false;
  }
  const PeSemanticByteRange range{
      .offset = record.payloadOffset,
      .length = record.payloadSize,
  };
  if (!observer.beginIdentityProjection(slot.sourceOrdinal,
                                        slot.recordOrdinal, range)) {
    return false;
  }
  for (std::uint32_t identityOrdinal = 0u;
       identityOrdinal < record.handleCount; ++identityOrdinal) {
    const auto handleIndex = record.firstHandle + identityOrdinal;
    const auto handleOffset =
        static_cast<std::size_t>(header.handleTableOffset) +
        static_cast<std::size_t>(handleIndex) *
            sizeof(D9CCommandChunkWireHandleEntry);
    if (handleOffset > emission.wire.size() ||
        sizeof(D9CCommandChunkWireHandleEntry) >
            emission.wire.size() - handleOffset) {
      return false;
    }
    D9CCommandChunkWireHandleEntry identity{};
    std::memcpy(&identity, emission.wire.data() + handleOffset,
                sizeof(identity));
    if (!observer.observeIdentity({
            .sourceOrdinal = slot.sourceOrdinal,
            .recordOrdinal = slot.recordOrdinal,
            .recordWireRange = range,
            .identityOrdinal = identityOrdinal,
            .kind = identity.kind,
            .generation = identity.generation,
            .objectId = identity.objectId,
        })) {
      return false;
    }
  }
  if (!observer.finishIdentityProjection(record.handleCount)) return false;
  return observer.accept({
      .recordType = slot.recordType,
      .sourceOrdinal = slot.sourceOrdinal,
      .recordOrdinal = slot.recordOrdinal,
      .wireRange = range,
      .exactValue = emission.wire.subspan(payloadOffset, record.payloadSize),
      .exactIdentityCount = record.handleCount,
      .exactIdentitiesValid = true,
  }) == PeSemanticProjectionAction::Accept;
}

// CapacityPre needs a current payload frontier even before the first record.
// An empty owner has no emit-ready chunk, so emissionMetrics() correctly
// returns false there; for cadence purposes that same state is the valid zero
// frontier. Keep this distinction shared by production and native tests.
template <typename Owner>
bool resolvePeSemanticCadenceFrontier(const Owner& owner,
                                      std::size_t& handles,
                                      std::size_t& payload) noexcept {
  handles = payload = 0u;
  if (owner.size() == 0u) return true;
  PeSemanticEmissionFrontier frontier{};
  if (!owner.emissionFrontier(frontier)) return false;
  handles = frontier.handleCount;
  payload = frontier.payloadBytes;
  return true;
}

template <typename Owner>
bool resolvePeSemanticCadenceMetrics(const Owner& owner, std::size_t& handles,
                                     std::size_t& payload,
                                     std::size_t& wire) noexcept {
  handles = payload = wire = 0u;
  return owner.size() == 0u || owner.emissionMetrics(handles, payload, wire);
}

template <std::size_t MaxRecords, std::size_t MaxPins,
          std::size_t MaxSemanticBytes, std::size_t MaxRects,
          std::size_t MaxSparseValues, typename LedgerAccess>
bool appendOwnedRecord(
    PeSemanticBatchOwner<MaxRecords, MaxPins, MaxSemanticBytes, MaxRects,
                         MaxSparseValues, LedgerAccess>& owner,
    const PeSemanticRecordInput& input) noexcept {
  return owner.appendOwnedRecord(input);
}

// The old Present/Readback candidate remains a call-local pilot.  It is not a
// chunk owner and must not be used as the all-family semantic owner.
static_assert(static_cast<std::size_t>(PeSemanticProducerKind::Count) == 21u);

}  // namespace dxmt9::d3d9::pe
