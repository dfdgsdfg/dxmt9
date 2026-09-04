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
#include "d3d9_pe_retainer.hpp"
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

template <typename Wire, typename Identity>
struct PeSemanticOwnedBinding {
  Wire wire{};
  std::uint32_t pin = kPeSemanticNoSlot;
  bool hasPin = false;
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
  std::array<std::uint32_t, 17u> sparseCounts{};
  std::array<std::uint32_t, 6u> uniquePinCounts{};
};

static_assert(std::is_trivially_copyable_v<PeSemanticAdmissionPlan>);

inline constexpr std::size_t kPeSemanticMaxRecordHandles = 64u;

// The bounded record-local handle order is both an emission input and a
// prepared admission witness.  Keeping this as the existing emission context
// avoids a second identity representation while making the witness
// pointer-free and trivially copyable.
struct PeSemanticEmissionHandleContext {
  std::array<D9CWireObjectIdentity, kPeSemanticMaxRecordHandles> identities{};
  std::size_t count = 0u;

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

  bool indexOf(const D9CWireObjectIdentity& identity,
               std::size_t& out) const noexcept {
    for (std::size_t i = 0u; i < count; ++i) {
      if (identities[i].kind == identity.kind &&
          identities[i].generation == identity.generation &&
          identities[i].objectId == identity.objectId) {
        out = i;
        return true;
      }
    }
    return false;
  }
};

static_assert(std::is_trivially_copyable_v<PeSemanticEmissionHandleContext>);
static_assert(sizeof(PeSemanticEmissionHandleContext) <=
              sizeof(D9CWireObjectIdentity) * kPeSemanticMaxRecordHandles +
                  std::max(sizeof(std::size_t),
                           alignof(D9CWireObjectIdentity)));

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

struct PeSemanticIdentitySet {
  // A record has at most 64 wire-visible handles; the extra slots cover the
  // direct typed pins retained by the owner but intentionally omitted from
  // the record-local wire handle table.  Wire visibility is a per-entry flag
  // rather than a second set: one walk then answers the retention question and
  // the final-wire handle count together, so admission and emission cannot
  // disagree about which identities a record carries.
  std::array<D9CWireObjectIdentity, 80u> values{};
  std::array<const void*, 80u> objects{};
  std::array<bool, 80u> wireVisible{};
  std::size_t count = 0u;
  std::size_t wireCount = 0u;

  // `newlyRetained` reports a newly seen identity that owns an object, while
  // `newlyWireVisible` reports the first encounter that makes it part of the
  // record-local wire table.  The latter is separate because a direct pin is
  // retained before a later sparse binding can promote the same identity.
  bool add(const PeWireObjectRef& ref, std::uint32_t kind, bool wire,
           bool& newlyRetained, bool& newlyWireVisible) noexcept {
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
        return true;
      }
    }
    if (count == values.size()) return false;
    values[count] = ref.identity;
    objects[count] = ref.object;
    wireVisible[count] = wire;
    if (wire) {
      ++wireCount;
      newlyWireVisible = true;
    }
    ++count;
    newlyRetained = true;
    return true;
  }
};

inline bool addAdmissionSection(std::uint16_t kind, std::size_t count,
                               std::size_t bytes,
                               std::size_t& cursor) noexcept {
  if (count == 0u) return true;
  const auto* rule = sectionRule(kind);
  if (!rule || count > rule->maxCount ||
      bytes > std::numeric_limits<std::size_t>::max() - cursor) {
    return false;
  }
  const auto alignment = static_cast<std::size_t>(rule->payloadAlignment);
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
    return false;
  }
  std::size_t aligned = 0u;
  if (!checkedSizeAdd(cursor, alignment - 1u, aligned)) return false;
  aligned &= ~(alignment - 1u);
  return checkedSizeAdd(aligned, bytes, cursor);
}

template <PeSemanticRecordInputLike Input>
inline bool admissionPayloadBytes(const Input& input,
                                  std::size_t& out) noexcept {
  out = 0u;
  if (!validPeSemanticInput(input)) return false;
  const auto& staged = stagedPeSemanticInput(input);
  switch (staged.producer) {
    case PeSemanticProducerKind::DrawPrimitive:
    case PeSemanticProducerKind::DrawIndexedPrimitive:
    case PeSemanticProducerKind::DrawPrimitiveUp:
    case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
    case PeSemanticProducerKind::ApplyState: {
      const auto& s = peSemanticSparse(input);
      const std::size_t sectionCount =
          !s.renderStates.empty() + !s.textures.empty() + !s.streams.empty() +
          !s.shaders.empty() + !s.vertexInputs.empty() + !s.indexBuffers.empty() +
          !s.renderTargets.empty() + !s.depthStencils.empty() + !s.viewports.empty() +
          !s.scissors.empty() + !s.materials.empty() + !s.clipPlanes.empty() +
          !s.textureStageStates.empty() + !s.samplerStates.empty() + !s.transforms.empty() +
          !s.lights.empty() + !s.lightEnables.empty() +
          (s.vsFloatConstants.present()) + (s.vsIntConstants.present()) +
          (s.vsBoolConstants.present()) + (s.psFloatConstants.present()) +
          (s.psIntConstants.present()) + (s.psBoolConstants.present()) +
          !s.upIndexData.empty() + !s.upVertexData.empty();
      if (sectionCount > D9C_COMMAND_CHUNK_SECTION_COUNT) return false;
      std::size_t sectionBytes = 0u;
      std::size_t cursor = 0u;
      if (!checkedSizeMultiply(sectionCount,
                               sizeof(D9CCommandChunkWireSectionDesc),
                               sectionBytes) ||
          !checkedSizeAdd(sizeof(D9CCommandChunkWireDrawHeader), sectionBytes,
                          cursor)) return false;
      const auto typed = [&](std::uint16_t kind, std::size_t count,
                             std::size_t element) noexcept {
        std::size_t bytes = 0u;
        return checkedSizeMultiply(count, element, bytes) &&
               addAdmissionSection(kind, count, bytes, cursor);
      };
      const auto raw = [&](std::uint16_t kind, std::span<const std::byte> bytes) noexcept {
        return addAdmissionSection(kind, bytes.size(), bytes.size(), cursor);
      };
      const auto constant = [&](std::uint16_t kind,
                                const SparseConstantRangeInput& range) noexcept {
        if (!range.present()) return true;
        const auto* rule = sectionRule(kind);
        std::size_t registerBytes = 0u;
        std::size_t sectionBytes = 0u;
        if (!rule || range.registerCount > rule->maxCount ||
            static_cast<std::uint64_t>(range.startRegister) + range.registerCount >
                rule->maxCount ||
            !checkedSizeMultiply(static_cast<std::size_t>(range.registerCount),
                                 rule->elementSize, registerBytes) ||
            range.registerBytes.size() != registerBytes ||
            !checkedSizeAdd(sizeof(D9CCommandChunkWireConstantRange),
                            registerBytes, sectionBytes)) {
          return false;
        }
        return addAdmissionSection(
            kind, range.registerCount, sectionBytes, cursor);
      };
      if (!typed(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE, s.renderStates.size(),
                 sizeof(D9CCommandChunkWireRenderState)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_TEXTURE, s.textures.size(),
                 sizeof(D9CCommandChunkWireTextureBinding)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_STREAM, s.streams.size(),
                 sizeof(D9CCommandChunkWireStreamBinding)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_SHADER, s.shaders.size(),
                 sizeof(D9CCommandChunkWireShaderBinding)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT, s.vertexInputs.size(),
                 sizeof(D9CCommandChunkWireVertexInput)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER, s.indexBuffers.size(),
                 sizeof(D9CCommandChunkWireIndexBinding)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, s.renderTargets.size(),
                 sizeof(D9CCommandChunkWireRenderTargetBinding)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, s.depthStencils.size(),
                 sizeof(D9CCommandChunkWireDepthStencilBinding)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_VIEWPORT, s.viewports.size(), sizeof(D9CViewport)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_SCISSOR, s.scissors.size(), sizeof(D9CRect)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_MATERIAL, s.materials.size(), sizeof(D9CMaterial)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE, s.clipPlanes.size(),
                 sizeof(D9CCommandChunkWireClipPlane)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE, s.textureStageStates.size(),
                 sizeof(D9CDrawPacketTextureStageState)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE, s.samplerStates.size(),
                 sizeof(D9CDrawPacketSamplerState)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_TRANSFORM, s.transforms.size(),
                 sizeof(D9CDrawPacketTransform)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_LIGHT, s.lights.size(),
                 sizeof(D9CCommandChunkWireLight)) ||
          !typed(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE, s.lightEnables.size(),
                 sizeof(D9CCommandChunkWireLightEnable)) ||
          !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F, s.vsFloatConstants) ||
          !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I, s.vsIntConstants) ||
          !constant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B, s.vsBoolConstants) ||
          !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F, s.psFloatConstants) ||
          !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I, s.psIntConstants) ||
          !constant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B, s.psBoolConstants) ||
          !raw(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA, s.upIndexData) ||
          !raw(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA, s.upVertexData)) {
        return false;
      }
      out = cursor;
      return true;
    }
    case PeSemanticProducerKind::VsFloatConstant:
    case PeSemanticProducerKind::VsIntConstant:
    case PeSemanticProducerKind::VsBoolConstant:
    case PeSemanticProducerKind::PsFloatConstant:
    case PeSemanticProducerKind::PsIntConstant:
    case PeSemanticProducerKind::PsBoolConstant:
      return checkedSizeAdd(sizeof(D9CCommandChunkWireSetConst),
                            staged.constantBytes.size(), out);
    case PeSemanticProducerKind::Clear: {
      std::size_t rectBytes = 0u;
      return checkedSizeMultiply(staged.clearRects.size(), sizeof(D9CRect),
                                 rectBytes) &&
             checkedSizeAdd(sizeof(D9CCommandChunkWireClear), rectBytes, out);
    }
    case PeSemanticProducerKind::Present: out = sizeof(staged.present); return true;
    case PeSemanticProducerKind::StretchRect: out = sizeof(staged.stretchRect); return true;
    case PeSemanticProducerKind::ColorFill: out = sizeof(staged.colorFill); return true;
    case PeSemanticProducerKind::UpdateTexture:
      out = sizeof(D9CCommandChunkWireUpdateTexture); return true;
    case PeSemanticProducerKind::UpdateSurface: out = sizeof(staged.updateSurface); return true;
    case PeSemanticProducerKind::QueryIssue: out = sizeof(staged.queryIssue); return true;
    case PeSemanticProducerKind::Readback: out = sizeof(D9CCommandChunkWireReadback); return true;
    case PeSemanticProducerKind::ReszDepthResolve:
      out = sizeof(D9CCommandChunkWireReszDepthResolve); return true;
    case PeSemanticProducerKind::GenerateMipmaps:
      out = sizeof(D9CCommandChunkWireGenerateMipmaps); return true;
    case PeSemanticProducerKind::Count: return false;
  }
  return false;
}

}  // namespace detail

// Producer role facts. They are pure functions of the producer kind, so the
// prepared witness resolves them once instead of re-deriving the same boolean
// chains inside header validation.
constexpr bool isPeSemanticConstantProducer(
    PeSemanticProducerKind producer) noexcept {
  return producer == PeSemanticProducerKind::VsFloatConstant ||
         producer == PeSemanticProducerKind::VsIntConstant ||
         producer == PeSemanticProducerKind::VsBoolConstant ||
         producer == PeSemanticProducerKind::PsFloatConstant ||
         producer == PeSemanticProducerKind::PsIntConstant ||
         producer == PeSemanticProducerKind::PsBoolConstant;
}

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
template <PeSemanticRecordInputLike Input, typename OnUniqueRetained>
  requires std::is_nothrow_invocable_r_v<bool, OnUniqueRetained&,
                                         const PeWireObjectRef&, std::size_t>
inline bool planPeSemanticAdmissionWith(
    const Input& input, PeSemanticAdmissionPlan& out,
    OnUniqueRetained&& onUniqueRetained,
    PeSemanticEmissionHandleContext* wireWitness = nullptr) noexcept {
  out = {};
  if (wireWitness != nullptr) wireWitness->resetFrontier();
  if (!validPeSemanticInput(input)) return false;
  const auto& staged = stagedPeSemanticInput(input);
  out.recordType = peSemanticRecordType(input);
  // One wire/retention set keeps the planner's fixed stack bounded
  // independently of the six handle kinds; kind remains part of identity.
  detail::PeSemanticIdentitySet pins{};
  auto&& observe = onUniqueRetained;
  const auto retained = [&](const PeWireObjectRef& ref, std::uint32_t kind,
                            std::size_t pinKind, bool wire) noexcept {
    bool newlyRetained = false;
    bool newlyWireVisible = false;
    if (!pins.add(ref, kind, wire, newlyRetained, newlyWireVisible)) return false;
    if (newlyWireVisible && wireWitness != nullptr &&
        !wireWitness->appendKnownUnique(ref.identity)) return false;
    if (!newlyRetained) return true;
    if (out.uniquePinCounts[pinKind] ==
        std::numeric_limits<std::uint32_t>::max()) return false;
    ++out.uniquePinCounts[pinKind];
    return observe(ref, pinKind);
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
    for (const auto& row : rows) {
      if (!row.wire.valid) continue;
      if (!retained(row.object, kind, pinKind, true)) return false;
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
  if (!detail::admissionPayloadBytes(input, payloadBytes) ||
      !detail::checkedSizeToU32(payloadBytes, out.payloadBytes)) return false;
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
      !detail::checkedSizeToU32(pins.wireCount, out.handleCount)) return false;
  out.valid = true;
  return true;
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
// append, and never stored: it borrows nothing and outlives no call.
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

struct PeSemanticPreparedRecord {
  PeSemanticAdmissionPlan plan{};
  // Exact first-seen wire identity/order produced by the admission walk. It
  // borrows nothing and is consumed only by the private append transaction.
  PeSemanticEmissionHandleContext wireHandles{};
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

static_assert(std::is_trivially_copyable_v<PeSemanticPreparedRecord>);

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

struct PeSemanticPinIndexEntry {
  D9CWireObjectIdentity identity{};
  std::uint32_t pin = kPeSemanticNoSlot;
  bool occupied = false;
};

template <std::size_t N>
using PeSemanticPinIndex = std::array<PeSemanticPinIndexEntry, N>;

template <std::size_t MaxRecords, std::size_t MaxPins,
          std::size_t MaxSemanticBytes, std::size_t MaxRects,
          std::size_t MaxSparseValues>
struct PeSemanticBatchStorage {
  static constexpr std::size_t pinIndexCapacity = MaxPins * 4u + 1u;
  std::array<PeSemanticRecordSlot, MaxRecords> records{};
  std::array<PeSemanticPhysicalPin<D9CSurface, PeSemanticSurfaceIdentity>, MaxPins> surfaces{};
  std::array<PeSemanticPhysicalPin<D9CTexture, PeSemanticTextureIdentity>, MaxPins> textures{};
  std::array<PeSemanticPhysicalPin<D9CBuffer, PeSemanticBufferIdentity>, MaxPins> buffers{};
  std::array<PeSemanticPhysicalPin<D9CShader, PeSemanticShaderIdentity>, MaxPins> shaders{};
  std::array<PeSemanticPhysicalPin<D9CVertexDecl, PeSemanticDeclarationIdentity>, MaxPins> declarations{};
  std::array<PeSemanticPhysicalPin<D9CQuery, PeSemanticQueryIdentity>, MaxPins> queries{};
  PeSemanticPinIndex<pinIndexCapacity> surfaceIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> surfaceObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> textureIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> textureObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> bufferIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> bufferObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> shaderIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> shaderObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> declarationIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> declarationObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> queryIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> queryObjectIndex{};
  std::array<std::byte, MaxSemanticBytes> constantBytes{};
  std::array<D9CRect, MaxRects> rects{};
  std::array<D9CCommandChunkWireRenderState, MaxSparseValues> renderStates{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireTextureBinding, PeSemanticTextureIdentity>, MaxSparseValues> texturesArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireStreamBinding, PeSemanticBufferIdentity>, MaxSparseValues> streamsArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireShaderBinding, PeSemanticShaderIdentity>, MaxSparseValues> shadersArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireVertexInput, PeSemanticDeclarationIdentity>, MaxSparseValues> vertexInputsArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireIndexBinding, PeSemanticBufferIdentity>, MaxSparseValues> indexBuffersArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireRenderTargetBinding, PeSemanticSurfaceIdentity>, MaxSparseValues> renderTargetsArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireDepthStencilBinding, PeSemanticSurfaceIdentity>, MaxSparseValues> depthStencilsArena{};
  std::array<D9CViewport, MaxSparseValues> viewports{};
  std::array<D9CRect, MaxSparseValues> scissors{};
  std::array<D9CMaterial, MaxSparseValues> materials{};
  std::array<D9CCommandChunkWireClipPlane, MaxSparseValues> clipPlanes{};
  std::array<D9CDrawPacketTextureStageState, MaxSparseValues> textureStageStates{};
  std::array<D9CDrawPacketSamplerState, MaxSparseValues> samplerStates{};
  std::array<D9CDrawPacketTransform, MaxSparseValues> transforms{};
  std::array<D9CCommandChunkWireLight, MaxSparseValues> lights{};
  std::array<D9CCommandChunkWireLightEnable, MaxSparseValues> lightEnables{};
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
// the ordinary bounded recorder chunk.  Sparse arrays are typed arenas; they
// are not a byte serialization of SparseStateInput.
template <std::size_t MaxRecords = 64u, std::size_t MaxPins = 256u,
          std::size_t MaxSemanticBytes = 1u << 20u,
          std::size_t MaxRects = 1024u, std::size_t MaxSparseValues = 2048u>
class PeSemanticBatchOwner final {
 public:
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
  // Pure CapacityPre proof over an already-planned record.  It also publishes
  // the exact emission frontier it computed, so the transactional append and
  // its settlement never realign or re-plan the same layout.
  bool proveCapacity(PeSemanticPreparedRecord& witness) const noexcept {
    const auto& plan = witness.plan;
    if (!ready_ || !plan.valid || recordCount_ >= MaxRecords) return false;
    const auto* rule = witness.rule;
    if (!rule || plan.payloadBytes == 0u) return false;
    for (std::size_t i = 0u; i < plan.sparseCounts.size(); ++i) {
      if (plan.sparseCounts[i] > MaxSparseValues - sparseCounts_.values[i])
        return false;
    }
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
    std::size_t aligned = 0u;
    if (!alignEmission(emissionPayloadBytes_, rule->payloadAlignment, aligned) ||
        plan.payloadBytes > std::numeric_limits<std::size_t>::max() - aligned) {
      return false;
    }
    const auto nextPayload = aligned + plan.payloadBytes;
    const auto nextHandles = emissionHandleCount_ + plan.handleCount;
    if (nextPayload > std::numeric_limits<std::uint32_t>::max() ||
        nextHandles > std::numeric_limits<std::uint32_t>::max()) return false;
    // The canonical role-layout backing is proved here, before any byte of
    // this record is materialized. A chunk that cannot be staged is a
    // CapacityPre rejection, never a late seal failure.
    if (!fitsStagedWire(nextHandles, nextPayload)) return false;
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

  // Fold one record's identity dedup into the owner's retention question.  It
  // runs once per newly seen identity, and rejects a generation or pointer
  // alias of an already-pinned object exactly where the old separate retention
  // walk did.
  bool accumulateRetentionDelta(const PeWireObjectRef& ref, std::size_t pinKind,
                                std::array<std::uint32_t, 6u>& out) const noexcept {
    bool existing = false;
    switch (pinKind) {
      case 0u:
        if (!findOrValidateExistingPin(
                storage_->surfaceIdentityIndex, storage_->surfaceObjectIndex,
                storage_->surfaces, surfaceCount_, ref.identity, ref.object,
                existing)) return false;
        break;
      case 1u:
        if (!findOrValidateExistingPin(
                storage_->textureIdentityIndex, storage_->textureObjectIndex,
                storage_->textures, textureCount_, ref.identity, ref.object,
                existing)) return false;
        break;
      case 2u:
        if (!findOrValidateExistingPin(
                storage_->bufferIdentityIndex, storage_->bufferObjectIndex,
                storage_->buffers, bufferCount_, ref.identity, ref.object,
                existing)) return false;
        break;
      case 3u:
        if (!findOrValidateExistingPin(
                storage_->shaderIdentityIndex, storage_->shaderObjectIndex,
                storage_->shaders, shaderCount_, ref.identity, ref.object,
                existing)) return false;
        break;
      case 4u:
        if (!findOrValidateExistingPin(
                storage_->declarationIdentityIndex,
                storage_->declarationObjectIndex, storage_->declarations,
                declarationCount_, ref.identity, ref.object, existing))
          return false;
        break;
      case 5u:
        if (!findOrValidateExistingPin(
                storage_->queryIdentityIndex, storage_->queryObjectIndex,
                storage_->queries, queryCount_, ref.identity, ref.object,
                existing)) return false;
        break;
      default:
        return false;
    }
    if (!existing) {
      if (out[pinKind] == std::numeric_limits<std::uint32_t>::max())
        return false;
      ++out[pinKind];
    }
    return true;
  }

 public:
  // Observation-only preparation.  One pass over the immutable input answers
  // layout, per-kind retention delta, destination role, and exact emission
  // frontier; the transactional append then consumes those facts instead of
  // re-deriving them.  This is also the live CapacityPre predicate: an
  // `Admissible` outcome is exactly the old private canAdmitStorage() answer.
  template <PeSemanticRecordInputLike Input>
  PeSemanticAdmissionOutcome prepareAdmission(
      const Input& input,
      PeSemanticPreparedRecord& witness) const noexcept {
    witness = {};
    if (!ready_) return PeSemanticAdmissionOutcome::Unavailable;
    if (!validPeSemanticInput(input)) return PeSemanticAdmissionOutcome::Malformed;
    const auto& staged = stagedPeSemanticInput(input);
    witness.producer = staged.producer;
    witness.rule = recordRule(peSemanticRecordType(input));
    const auto* policy = peSemanticProducerPolicy(peSemanticRecordType(input));
    witness.producerMatchesRecordType =
        policy != nullptr && policy->kind == staged.producer;
    witness.constantProducer = isPeSemanticConstantProducer(staged.producer);
    witness.upProducer = isPeSemanticUpProducer(staged.producer);
    if (!planPeSemanticAdmissionWith(
        input, witness.plan,
        [&](const PeWireObjectRef& ref, std::size_t pinKind) noexcept {
              return accumulateRetentionDelta(ref, pinKind,
                                              witness.retentionDeltas);
            },
        &witness.wireHandles)) {
      return PeSemanticAdmissionOutcome::Malformed;
    }
    witness.recordCount = recordCount_;
    witness.emissionHandleCount = emissionHandleCount_;
    witness.emissionPayloadBytes = emissionPayloadBytes_;
    if (!proveCapacity(witness)) return PeSemanticAdmissionOutcome::Capacity;
    witness.admissible = true;
    return PeSemanticAdmissionOutcome::Admissible;
  }

  // A typed adapter used by producer-family call sites. It intentionally
  // aliases admission rather than exposing the owner internals to producers.
  template <PeSemanticRecordInputLike Input>
  bool appendOwnedRecord(const Input& input) noexcept {
    return admit(input);
  }

 private:
  // Owner-local implementation. The pure facts and qualified deltas never
  // leave tryAppendOwnedRecord(), so there is no caller-visible TOCTOU seam.
  template <bool Observe, PeSemanticRecordInputLike Input, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool appendPreparedRecord(const Input& input,
                            const PeSemanticPreparedRecord& prepared,
                            Commit&& commit) noexcept {
    const auto runPhase = [&](PeSemanticOwnerPhase phase,
                              auto&& operation) noexcept {
      if constexpr (Observe) {
        auto scope = phaseObserver_->child(phase);
        return operation();
      } else {
        return operation();
      }
    };
    const auto rollbackPhase = [&](const StateCheckpoint& checkpoint) noexcept {
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
      // Nothing between prepareAdmission() and here mutates the destination,
      // so this is an invariant guard rather than a second capacity pass: a
      // witness that describes another record, or was proved against another
      // frontier, fails closed.
      const auto& staged = stagedPeSemanticInput(input);
      if (prepared.producer != staged.producer ||
          prepared.plan.recordType != peSemanticRecordType(input) ||
          !prepared.admissibleAt(recordCount_, emissionHandleCount_,
                                 emissionPayloadBytes_)) {
        lastAdmissionFailure_ = AdmissionFailure::Header;
        return false;
      }
      const auto checkpoint = checkpointState();
      retainedCheckpoint_ = checkpoint.retained;
      if (!validAdmissionHeader(input, prepared)) {
        lastAdmissionFailure_ = AdmissionFailure::Header;
        rollbackPhase(checkpoint);
        return false;
      }
      if (!runPhase(PeSemanticOwnerPhase::FixedDirectPinCopy,
                    [&]() noexcept {
                      if (!copyFixedValues(input)) {
                        lastAdmissionFailure_ = AdmissionFailure::Fixed;
                        return false;
                      }
                      if (!copyDirectPins(input, storage_->records[recordCount_])) {
                        lastAdmissionFailure_ = AdmissionFailure::DirectPins;
                        return false;
                      }
                      return true;
                    })) {
        rollbackPhase(checkpoint);
        return false;
      }
      auto& slot = storage_->records[recordCount_];
      if (!runPhase(PeSemanticOwnerPhase::SparseVariableCopy,
                    [&]() noexcept {
                      if (!copySparse(input, slot)) return false;
                      if (!copyVariablePayloads(input, slot)) {
                        lastAdmissionFailure_ = AdmissionFailure::VariablePayload;
                        return false;
                      }
                      return true;
                    })) {
        if (lastAdmissionFailure_ == AdmissionFailure::None)
          lastAdmissionFailure_ = AdmissionFailure::Sparse;
        rollbackPhase(checkpoint);
        return false;
      }
      if (!runPhase(PeSemanticOwnerPhase::CanonicalMaterializationMetrics,
                    [&]() noexcept {
                      return materializeCanonicalRecord(slot, prepared) &&
                             cacheEmissionMetrics(prepared);
                    })) {
        lastAdmissionFailure_ = AdmissionFailure::EmissionMetrics;
        rollbackPhase(checkpoint);
        return false;
      }
      ++recordCount_;
      auto&& callback = commit;
      if (!runPhase(PeSemanticOwnerPhase::PendingDeltaSettlement,
                    [&]() noexcept { return callback(); })) {
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
  template <PeSemanticRecordInputLike Input, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool tryAppendOwnedRecord(const Input& input,
                            Commit&& commit) noexcept {
    if (phaseObserver_) {
      return tryAppendOwnedRecordObserved(input, std::forward<Commit>(commit));
    }
    return tryAppendOwnedRecordUnobserved(input, std::forward<Commit>(commit));
  }

 private:
  template <PeSemanticRecordInputLike Input, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool tryAppendOwnedRecordUnobserved(const Input& input,
                                      Commit&& commit) noexcept {
    PeSemanticPreparedRecord prepared{};
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

  template <PeSemanticRecordInputLike Input, typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool tryAppendOwnedRecordObserved(const Input& input,
                                    Commit&& commit) noexcept {
    auto parent = phaseObserver_->beginAppend();
    PeSemanticPreparedRecord prepared{};
    PeSemanticAdmissionOutcome outcome;
    {
      auto phase = phaseObserver_->child(PeSemanticOwnerPhase::PrepareAdmission);
      outcome = prepareAdmission(input, prepared);
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
      return false;
    }
    const bool accepted = appendPreparedRecord<true>(
        input, prepared, std::forward<Commit>(commit));
    phaseObserver_->recordOutcome(
        accepted ? PeSemanticOwnerOutcome::Accepted
                 : phaseOutcome(lastAdmissionFailure_));
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

  template <typename Visit>
    requires std::is_nothrow_invocable_r_v<
        bool, Visit&, PeSemanticProducerKind,
        const PeSemanticRecordSlot&>
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
      auto phase = phaseObserver_->beginOperation(
          PeSemanticOwnerPhase::SettleClear);
      return settleBody<false>();
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
    lastClearedBytes_ += clearUsed(storage_->records, recordCount_);
    lastClearedBytes_ += clearUsed(storage_->surfaces, surfaceCount_);
    lastClearedBytes_ += clearUsed(storage_->textures, textureCount_);
    lastClearedBytes_ += clearUsed(storage_->buffers, bufferCount_);
    lastClearedBytes_ += clearUsed(storage_->shaders, shaderCount_);
    lastClearedBytes_ += clearUsed(storage_->declarations, declarationCount_);
    lastClearedBytes_ += clearUsed(storage_->queries, queryCount_);
    lastClearedBytes_ += clearUsed(storage_->constantBytes, semanticBytes_);
    lastClearedBytes_ += clearUsed(storage_->rects, rectCount_);
    lastClearedBytes_ += clearUsed(storage_->renderStates,
                                   sparseCounts_.values[kRenderStates]);
    lastClearedBytes_ += clearUsed(storage_->texturesArena,
                                   sparseCounts_.values[kTextures]);
    lastClearedBytes_ += clearUsed(storage_->streamsArena,
                                   sparseCounts_.values[kStreams]);
    lastClearedBytes_ += clearUsed(storage_->shadersArena,
                                   sparseCounts_.values[kShaders]);
    lastClearedBytes_ += clearUsed(storage_->vertexInputsArena,
                                   sparseCounts_.values[kVertexInputs]);
    lastClearedBytes_ += clearUsed(storage_->indexBuffersArena,
                                   sparseCounts_.values[kIndexBuffers]);
    lastClearedBytes_ += clearUsed(storage_->renderTargetsArena,
                                   sparseCounts_.values[kRenderTargets]);
    lastClearedBytes_ += clearUsed(storage_->depthStencilsArena,
                                   sparseCounts_.values[kDepthStencils]);
    lastClearedBytes_ += clearUsed(storage_->viewports,
                                   sparseCounts_.values[kViewports]);
    lastClearedBytes_ += clearUsed(storage_->scissors,
                                   sparseCounts_.values[kScissors]);
    lastClearedBytes_ += clearUsed(storage_->materials,
                                   sparseCounts_.values[kMaterials]);
    lastClearedBytes_ += clearUsed(storage_->clipPlanes,
                                   sparseCounts_.values[kClipPlanes]);
    lastClearedBytes_ += clearUsed(storage_->textureStageStates,
                                   sparseCounts_.values[kTextureStageStates]);
    lastClearedBytes_ += clearUsed(storage_->samplerStates,
                                   sparseCounts_.values[kSamplerStates]);
    lastClearedBytes_ += clearUsed(storage_->transforms,
                                   sparseCounts_.values[kTransforms]);
    lastClearedBytes_ += clearUsed(storage_->lights,
                                   sparseCounts_.values[kLights]);
    lastClearedBytes_ += clearUsed(storage_->lightEnables,
                                   sparseCounts_.values[kLightEnables]);
    lastClearedBytes_ += clearUsed(storage_->wireRecords, recordCount_);
    lastClearedBytes_ += clearUsed(storage_->wireHandles, emissionHandleCount_);
    if (emissionPayloadBytes_ != 0u) {
      std::fill_n(storage_->canonicalPayload.begin(),
                  emissionPayloadBytes_, std::byte{0});
      lastClearedBytes_ += emissionPayloadBytes_;
    }
    recordCount_ = 0u;
    semanticBytes_ = 0u;
    emissionHandleCount_ = 0u;
    emissionPayloadBytes_ = 0u;
    rectCount_ = 0u;
    sparseCounts_ = {};
    surfaceCount_ = textureCount_ = bufferCount_ = shaderCount_ =
        declarationCount_ = queryCount_ = 0u;
    clearPinIndexes();
  }

  template <typename Array>
  static std::size_t clearUsed(Array& values, std::size_t count) noexcept {
    count = std::min(count, values.size());
    std::fill_n(values.begin(), count, typename Array::value_type{});
    return count * sizeof(typename Array::value_type);
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
    return findPinIndex(storage_->bufferIdentityIndex, ref.identity, true,
                        pin) &&
        pin < bufferCount_ && storage_->buffers[pin].object == ref.object;
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
    auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
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

  const PeSemanticRecordSlot& record(std::size_t index) const noexcept {
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

  std::span<const std::byte> constantBytes(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->constantBytes.data() + slot.constantBytes.offset,
            slot.constantBytes.count};
  }
  std::span<const D9CRect> clearRects(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->rects.data() + slot.clearRects.offset, slot.clearRects.count};
  }
  std::span<const std::byte> upIndexBytes(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->constantBytes.data() + slot.upIndexBytes.offset,
            slot.upIndexBytes.count};
  }
  std::span<const std::byte> upVertexBytes(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->constantBytes.data() + slot.upVertexBytes.offset,
            slot.upVertexBytes.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireTextureBinding, PeSemanticTextureIdentity>>
  textureBindings(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->texturesArena.data() + slot.textures.offset, slot.textures.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireStreamBinding, PeSemanticBufferIdentity>>
  streamBindings(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->streamsArena.data() + slot.streams.offset, slot.streams.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireShaderBinding, PeSemanticShaderIdentity>>
  shaderBindings(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->shadersArena.data() + slot.shaders.offset, slot.shaders.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireVertexInput, PeSemanticDeclarationIdentity>>
  vertexInputs(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->vertexInputsArena.data() + slot.vertexInputs.offset,
            slot.vertexInputs.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireIndexBinding, PeSemanticBufferIdentity>>
  indexBuffers(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->indexBuffersArena.data() + slot.indexBuffers.offset,
            slot.indexBuffers.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireRenderTargetBinding, PeSemanticSurfaceIdentity>>
  renderTargets(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->renderTargetsArena.data() + slot.renderTargets.offset,
            slot.renderTargets.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireDepthStencilBinding, PeSemanticSurfaceIdentity>>
  depthStencils(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->depthStencilsArena.data() + slot.depthStencils.offset,
            slot.depthStencils.count};
  }

 private:
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
  // Pin admission is on the producer hot path. Keep exact identity and
  // object-id membership in fixed, typed open-addressed tables so a repeated
  // warm pin does not scan every prior pin. The table is deliberately
  // over-provisioned; probing is bounded by this compile-time constant and
  // never allocates.
  static constexpr std::size_t kPinIndexCapacity = MaxPins * 4u + 1u;

  static bool fitsStagedWire(std::size_t handleCount,
                             std::size_t payloadBytes) noexcept {
    return handleCount <= Storage::maxWireHandles &&
           payloadBytes <= Storage::maxWirePayloadBytes;
  }

  using PinIndexEntry = PeSemanticPinIndexEntry;

  template <std::size_t N>
  using PinIndex = PeSemanticPinIndex<N>;

  struct EmissionRecordPlan {
    std::size_t payloadOffset = 0u;
    std::size_t payloadBytes = 0u;
    std::size_t firstHandle = 0u;
    std::size_t handleCount = 0u;
  };

  struct EmissionPlan {
    std::array<EmissionRecordPlan, MaxRecords> records{};
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
    auto* const ledger = dxmt9::core::activeCopyMaterializationLedger(
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

  static std::size_t pinHash(const D9CWireObjectIdentity& identity,
                             bool includeGeneration) noexcept {
    std::uint64_t value = identity.objectId * 0x9e3779b185ebca87ull;
    value ^= static_cast<std::uint64_t>(identity.kind) *
             0xc2b2ae3d27d4eb4full;
    if (includeGeneration) {
      value ^= identity.generation + 0x165667b19e3779f9ull +
               (value << 6u) + (value >> 2u);
    }
    return static_cast<std::size_t>(value);
  }

  template <std::size_t N>
  static bool findPinIndex(const PinIndex<N>& index,
                           const D9CWireObjectIdentity& identity,
                           bool includeGeneration,
                           std::size_t& out) noexcept {
    const std::size_t start = pinHash(identity, includeGeneration) % N;
    for (std::size_t probe = 0u; probe < N; ++probe) {
      const auto& entry = index[(start + probe) % N];
      if (!entry.occupied) return false;
      const bool matches = entry.identity.kind == identity.kind &&
          entry.identity.objectId == identity.objectId &&
          (!includeGeneration ||
           entry.identity.generation == identity.generation);
      if (matches) {
        out = entry.pin;
        return true;
      }
    }
    return false;
  }

  template <std::size_t N>
  static bool insertPinIndex(PinIndex<N>& index,
                             const D9CWireObjectIdentity& identity,
                             std::uint32_t pin,
                             bool includeGeneration = true) noexcept {
    const std::size_t start = pinHash(identity, includeGeneration) % N;
    for (std::size_t probe = 0u; probe < N; ++probe) {
      auto& entry = index[(start + probe) % N];
      if (!entry.occupied ||
          (entry.identity.kind == identity.kind &&
           entry.identity.objectId == identity.objectId &&
           (!includeGeneration ||
            entry.identity.generation == identity.generation))) {
        entry.identity = identity;
        entry.pin = pin;
        entry.occupied = true;
        return true;
      }
    }
    return false;
  }

  template <std::size_t N>
  static void clearPinIndex(PinIndex<N>& index) noexcept {
    for (auto& entry : index) entry = {};
  }

  template <typename PinArray, std::size_t N>
  static void rebuildPinIndex(const PinArray& pins, std::size_t count,
                              PinIndex<N>& exact, PinIndex<N>& object) noexcept {
    clearPinIndex(exact);
    clearPinIndex(object);
    for (std::size_t i = 0u; i < count; ++i) {
      if (!pins[i].valid()) continue;
      (void)insertPinIndex(exact, pins[i].identity.value,
                           static_cast<std::uint32_t>(i));
      (void)insertPinIndex(object, pins[i].identity.value,
                           static_cast<std::uint32_t>(i), false);
    }
  }

  void clearPinIndexes() noexcept {
    clearPinIndex(storage_->surfaceIdentityIndex);
    clearPinIndex(storage_->surfaceObjectIndex);
    clearPinIndex(storage_->textureIdentityIndex);
    clearPinIndex(storage_->textureObjectIndex);
    clearPinIndex(storage_->bufferIdentityIndex);
    clearPinIndex(storage_->bufferObjectIndex);
    clearPinIndex(storage_->shaderIdentityIndex);
    clearPinIndex(storage_->shaderObjectIndex);
    clearPinIndex(storage_->declarationIdentityIndex);
    clearPinIndex(storage_->declarationObjectIndex);
    clearPinIndex(storage_->queryIdentityIndex);
    clearPinIndex(storage_->queryObjectIndex);
  }

  void rebuildPinIndexes() noexcept {
    rebuildPinIndex(storage_->surfaces, surfaceCount_,
                    storage_->surfaceIdentityIndex, storage_->surfaceObjectIndex);
    rebuildPinIndex(storage_->textures, textureCount_,
                    storage_->textureIdentityIndex, storage_->textureObjectIndex);
    rebuildPinIndex(storage_->buffers, bufferCount_,
                    storage_->bufferIdentityIndex, storage_->bufferObjectIndex);
    rebuildPinIndex(storage_->shaders, shaderCount_,
                    storage_->shaderIdentityIndex, storage_->shaderObjectIndex);
    rebuildPinIndex(storage_->declarations, declarationCount_,
                    storage_->declarationIdentityIndex,
                    storage_->declarationObjectIndex);
    rebuildPinIndex(storage_->queries, queryCount_,
                    storage_->queryIdentityIndex, storage_->queryObjectIndex);
  }

  template <typename PinArray, typename Visit>
  static bool visitPin(const PinArray& pins, std::size_t count,
                       std::uint32_t index, Visit&& visit) noexcept {
    if (index == kPeSemanticNoSlot) return true;
    if (index >= count || !pins[index].valid()) return false;
    return visit(pins[index].identity.value);
  }

  template <typename Visit>
  bool visitRecordHandles(const PeSemanticRecordSlot& slot,
                          Visit&& visit) const noexcept {
    const auto direct = [&](auto const& pins, std::size_t count,
                            std::uint32_t index) noexcept {
      return visitPin(pins, count, index, visit);
    };
    switch (slot.producer) {
      case PeSemanticProducerKind::Present:
        if (!direct(storage_->surfaces, surfaceCount_, slot.surface0)) return false;
        break;
      case PeSemanticProducerKind::StretchRect:
      case PeSemanticProducerKind::UpdateSurface:
      case PeSemanticProducerKind::Readback:
        if (!direct(storage_->surfaces, surfaceCount_, slot.surface0) ||
            !direct(storage_->surfaces, surfaceCount_, slot.surface1)) return false;
        break;
      case PeSemanticProducerKind::ColorFill:
        if (!direct(storage_->surfaces, surfaceCount_, slot.surface0)) return false;
        break;
      case PeSemanticProducerKind::UpdateTexture:
        if (!direct(storage_->textures, textureCount_, slot.texture0) ||
            !direct(storage_->textures, textureCount_, slot.texture1)) return false;
        break;
      case PeSemanticProducerKind::QueryIssue:
        if (!direct(storage_->queries, queryCount_, slot.query)) return false;
        break;
      case PeSemanticProducerKind::ReszDepthResolve:
        if (!direct(storage_->surfaces, surfaceCount_, slot.surface0) ||
            !direct(storage_->textures, textureCount_, slot.texture0)) return false;
        break;
      case PeSemanticProducerKind::GenerateMipmaps:
        if (!direct(storage_->textures, textureCount_, slot.texture0)) return false;
        break;
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        break;
      default:
        break;
    }
    if (slot.producer != PeSemanticProducerKind::DrawPrimitive &&
        slot.producer != PeSemanticProducerKind::DrawIndexedPrimitive &&
        slot.producer != PeSemanticProducerKind::DrawPrimitiveUp &&
        slot.producer != PeSemanticProducerKind::DrawIndexedPrimitiveUp &&
        slot.producer != PeSemanticProducerKind::ApplyState) {
      return true;
    }
    const auto textures = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->textures, textureCount_, item.pin, visit);
    };
    const auto streams = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->buffers, bufferCount_, item.pin, visit);
    };
    const auto shaders = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->shaders, shaderCount_, item.pin, visit);
    };
    const auto declarations = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->declarations, declarationCount_, item.pin, visit);
    };
    const auto surfaces = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->surfaces, surfaceCount_, item.pin, visit);
    };
    const auto buffers = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->buffers, bufferCount_, item.pin, visit);
    };
    const auto each = [&](const auto& arena, const PeSemanticArenaRange& range,
                          const auto& one) noexcept {
      if (range.offset > arena.size() || range.count > arena.size() - range.offset) return false;
      for (std::size_t i = 0u; i < range.count; ++i)
        if (!one(arena[range.offset + i])) return false;
      return true;
    };
    return each(storage_->texturesArena, slot.textures, textures) &&
           each(storage_->streamsArena, slot.streams, streams) &&
           each(storage_->shadersArena, slot.shaders, shaders) &&
           each(storage_->vertexInputsArena, slot.vertexInputs, declarations) &&
           each(storage_->indexBuffersArena, slot.indexBuffers, buffers) &&
           each(storage_->renderTargetsArena, slot.renderTargets, surfaces) &&
           each(storage_->depthStencilsArena, slot.depthStencils, surfaces);
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

  static bool addEmissionSection(std::uint16_t kind, std::size_t count,
                                 std::size_t bytes,
                                 std::size_t& cursor) noexcept {
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    if (!rule || count == 0u || count > rule->maxCount ||
        count > std::numeric_limits<std::uint32_t>::max() ||
        bytes > std::numeric_limits<std::uint32_t>::max() ||
        !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        bytes > std::numeric_limits<std::size_t>::max() - aligned) {
      return false;
    }
    cursor = aligned + bytes;
    return true;
  }

  static bool addEmissionConstant(std::uint16_t kind,
                                  const PeSemanticArenaRange& bytes,
                                  const D9CCommandChunkWireConstantRange& range,
                                  std::size_t& cursor) noexcept {
    if (range.registerCount == 0u) return bytes.count == 0u;
    const auto* rule = sectionRule(kind);
    if (!rule || range.registerCount > rule->maxCount ||
        static_cast<std::uint64_t>(range.startRegister) +
                range.registerCount > rule->maxCount ||
        bytes.count != static_cast<std::size_t>(range.registerCount) *
                           rule->elementSize) {
      return false;
    }
    return addEmissionSection(kind, range.registerCount,
                              sizeof(D9CCommandChunkWireConstantRange) +
                                  bytes.count,
                              cursor);
  }

  std::size_t sparseSectionCount(const PeSemanticRecordSlot& slot) const noexcept {
    const auto present = [](const PeSemanticArenaRange& range) noexcept {
      return range.count != 0u;
    };
    return present(slot.renderStates) + present(slot.textures) +
           present(slot.streams) + present(slot.shaders) +
           present(slot.vertexInputs) + present(slot.indexBuffers) +
           present(slot.renderTargets) + present(slot.depthStencils) +
           present(slot.viewports) + present(slot.scissors) +
           present(slot.materials) + present(slot.clipPlanes) +
           present(slot.textureStageStates) + present(slot.samplerStates) +
           present(slot.transforms) + present(slot.lights) +
           present(slot.lightEnables) + present(slot.vsFloatConstants) +
           present(slot.vsIntConstants) + present(slot.vsBoolConstants) +
           present(slot.psFloatConstants) + present(slot.psIntConstants) +
           present(slot.psBoolConstants) + present(slot.upIndexBytes) +
           present(slot.upVertexBytes);
  }

  bool payloadSize(const PeSemanticRecordSlot& slot,
                   std::size_t& out) const noexcept {
    out = 0u;
    switch (slot.producer) {
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState: {
        const auto sections = sparseSectionCount(slot);
        if (sections > D9C_COMMAND_CHUNK_SECTION_COUNT ||
            sizeof(D9CCommandChunkWireDrawHeader) >
                std::numeric_limits<std::size_t>::max() -
                    sections * sizeof(D9CCommandChunkWireSectionDesc)) return false;
        std::size_t cursor = sizeof(D9CCommandChunkWireDrawHeader) +
                             sections * sizeof(D9CCommandChunkWireSectionDesc);
        const auto addTyped = [&](std::uint16_t kind,
                                  const PeSemanticArenaRange& range,
                                  std::size_t element) noexcept {
          return range.count == 0u ||
                 (range.offset <= MaxSparseValues &&
                  range.count <= MaxSparseValues - range.offset &&
                  addEmissionSection(kind, range.count, range.count * element,
                                     cursor));
        };
        const auto addRawBytes = [&](std::uint16_t kind,
                                     const PeSemanticArenaRange& range) noexcept {
          return range.count == 0u ||
                 (range.offset <= MaxSemanticBytes &&
                  range.count <= MaxSemanticBytes - range.offset &&
                  addEmissionSection(kind, range.count, range.count,
                                     cursor));
        };
        if (!addTyped(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE, slot.renderStates,
                      sizeof(D9CCommandChunkWireRenderState)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_TEXTURE, slot.textures,
                      sizeof(D9CCommandChunkWireTextureBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_STREAM, slot.streams,
                      sizeof(D9CCommandChunkWireStreamBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_SHADER, slot.shaders,
                      sizeof(D9CCommandChunkWireShaderBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT, slot.vertexInputs,
                      sizeof(D9CCommandChunkWireVertexInput)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER, slot.indexBuffers,
                      sizeof(D9CCommandChunkWireIndexBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, slot.renderTargets,
                      sizeof(D9CCommandChunkWireRenderTargetBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, slot.depthStencils,
                      sizeof(D9CCommandChunkWireDepthStencilBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_VIEWPORT, slot.viewports,
                      sizeof(D9CViewport)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_SCISSOR, slot.scissors,
                      sizeof(D9CRect)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_MATERIAL, slot.materials,
                      sizeof(D9CMaterial)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE, slot.clipPlanes,
                      sizeof(D9CCommandChunkWireClipPlane)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
                      slot.textureStageStates,
                      sizeof(D9CDrawPacketTextureStageState)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                      slot.samplerStates, sizeof(D9CDrawPacketSamplerState)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_TRANSFORM, slot.transforms,
                      sizeof(D9CDrawPacketTransform)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_LIGHT, slot.lights,
                      sizeof(D9CCommandChunkWireLight)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
                      slot.lightEnables, sizeof(D9CCommandChunkWireLightEnable)) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F,
                                  slot.vsFloatConstants, slot.vsFloatConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I,
                                  slot.vsIntConstants, slot.vsIntConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B,
                                  slot.vsBoolConstants, slot.vsBoolConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F,
                                  slot.psFloatConstants, slot.psFloatConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I,
                                  slot.psIntConstants, slot.psIntConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B,
                                  slot.psBoolConstants, slot.psBoolConstant, cursor) ||
            !addRawBytes(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA,
                         slot.upIndexBytes) ||
            !addRawBytes(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
                         slot.upVertexBytes)) return false;
        out = cursor;
        return true;
      }
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        return slot.constantBytes.offset <= MaxSemanticBytes &&
               slot.constantBytes.count <= MaxSemanticBytes - slot.constantBytes.offset &&
               slot.constantBytes.count <= std::numeric_limits<std::size_t>::max() -
                   sizeof(D9CCommandChunkWireSetConst) &&
               (out = sizeof(D9CCommandChunkWireSetConst) + slot.constantBytes.count,
                true);
      case PeSemanticProducerKind::Clear:
        return slot.clearRects.count <= MaxRects - slot.clearRects.offset &&
               slot.clearRects.count <=
                   (std::numeric_limits<std::size_t>::max() -
                    sizeof(D9CCommandChunkWireClear)) / sizeof(D9CRect) &&
               (out = sizeof(D9CCommandChunkWireClear) +
                      slot.clearRects.count * sizeof(D9CRect), true);
      case PeSemanticProducerKind::Present: out = sizeof(slot.present); return true;
      case PeSemanticProducerKind::StretchRect: out = sizeof(slot.stretchRect); return true;
      case PeSemanticProducerKind::ColorFill: out = sizeof(slot.colorFill); return true;
      case PeSemanticProducerKind::UpdateTexture: out = sizeof(D9CCommandChunkWireUpdateTexture); return true;
      case PeSemanticProducerKind::UpdateSurface: out = sizeof(slot.updateSurface); return true;
      case PeSemanticProducerKind::QueryIssue: out = sizeof(slot.queryIssue); return true;
      case PeSemanticProducerKind::Readback: out = sizeof(D9CCommandChunkWireReadback); return true;
      case PeSemanticProducerKind::ReszDepthResolve: out = sizeof(D9CCommandChunkWireReszDepthResolve); return true;
      case PeSemanticProducerKind::GenerateMipmaps: out = sizeof(D9CCommandChunkWireGenerateMipmaps); return true;
      case PeSemanticProducerKind::Count: return false;
    }
    return false;
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
      out.records[i] = {.payloadOffset = record.payloadOffset,
                        .payloadBytes = record.payloadSize,
                        .firstHandle = record.firstHandle,
                        .handleCount = record.handleCount};
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

  template <typename T>
  static bool writeTypedSection(
      std::uint16_t kind, std::span<const T> values,
      std::span<std::byte> payload, std::size_t& cursor,
      std::array<D9CCommandChunkWireSectionDesc,
                 D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
      std::size_t& sectionIndex) noexcept {
    if (values.empty()) return true;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    if (!rule || values.size() > rule->maxCount ||
        sizeof(T) != rule->elementSize ||
        !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        aligned > payload.size() ||
        values.size_bytes() > payload.size() - aligned ||
        sectionIndex == descs.size() ||
        !zeroPaddingTo(payload, cursor, aligned)) return false;
    std::memcpy(payload.data() + cursor, values.data(), values.size_bytes());
    descs[sectionIndex++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = static_cast<std::uint32_t>(values.size()),
        .payloadOffset = static_cast<std::uint32_t>(cursor),
        .byteSize = static_cast<std::uint32_t>(values.size_bytes()),
    };
    cursor += values.size_bytes();
    return true;
  }

  template <typename Wire, typename Identity>
  bool writeBindingSection(
      std::uint16_t kind, std::span<const PeSemanticOwnedBinding<Wire, Identity>> values,
      std::span<std::byte> payload, std::size_t& cursor,
      std::array<D9CCommandChunkWireSectionDesc,
                 D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
      std::size_t& sectionIndex, const EmissionHandleContext& handles,
      std::size_t globalFirst) const noexcept {
    if (values.empty()) return true;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    const auto wireBytes = values.size() * sizeof(Wire);
    if (!rule || values.size() > rule->maxCount ||
        sizeof(Wire) != rule->elementSize ||
        !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        aligned > payload.size() || wireBytes > payload.size() - aligned ||
        sectionIndex == descs.size() ||
        !zeroPaddingTo(payload, cursor, aligned)) {
      return false;
    }
    for (const auto& source : values) {
      Wire value = source.wire;
      if constexpr (requires(Wire wire) { wire.reserved0; }) {
        value.reserved0 = 0u;
      }
      value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
      if (value.valid && source.hasPin) {
        D9CWireObjectIdentity identity{};
        if (!bindingIdentity(source, identity)) {
          return false;
        }
        std::size_t local = 0u;
        if (!handles.indexOf(identity, local) ||
            globalFirst + local > std::numeric_limits<std::uint32_t>::max()) {
          return false;
        }
        value.handleIndex = static_cast<std::uint32_t>(globalFirst + local);
      }
      if (!writeValue(payload, cursor, value)) return false;
    }
    descs[sectionIndex++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = static_cast<std::uint32_t>(values.size()),
        .payloadOffset = static_cast<std::uint32_t>(aligned),
        .byteSize = static_cast<std::uint32_t>(wireBytes),
    };
    return true;
  }

  template <typename Wire, typename Identity>
  bool bindingIdentity(const PeSemanticOwnedBinding<Wire, Identity>& source,
                       D9CWireObjectIdentity& out) const noexcept {
    if constexpr (std::is_same_v<Identity, PeSemanticTextureIdentity>) {
      return source.pin < textureCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->textures[source.pin].valid() &&
             (out = storage_->textures[source.pin].identity.value, true);
    } else if constexpr (std::is_same_v<Identity, PeSemanticBufferIdentity>) {
      return source.pin < bufferCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->buffers[source.pin].valid() &&
             (out = storage_->buffers[source.pin].identity.value, true);
    } else if constexpr (std::is_same_v<Identity, PeSemanticShaderIdentity>) {
      return source.pin < shaderCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->shaders[source.pin].valid() &&
             (out = storage_->shaders[source.pin].identity.value, true);
    } else if constexpr (std::is_same_v<Identity, PeSemanticDeclarationIdentity>) {
      return source.pin < declarationCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->declarations[source.pin].valid() &&
             (out = storage_->declarations[source.pin].identity.value, true);
    } else if constexpr (std::is_same_v<Identity, PeSemanticSurfaceIdentity>) {
      return source.pin < surfaceCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->surfaces[source.pin].valid() &&
             (out = storage_->surfaces[source.pin].identity.value, true);
    }
    return false;
  }

  static bool writeBytesSection(
      std::uint16_t kind, std::span<const std::byte> bytes,
      std::span<std::byte> payload, std::size_t& cursor,
      std::array<D9CCommandChunkWireSectionDesc,
                 D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
      std::size_t& sectionIndex) noexcept {
    if (bytes.empty()) return true;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    if (!rule || bytes.size() > rule->maxCount ||
        !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        aligned > payload.size() || bytes.size() > payload.size() - aligned ||
        sectionIndex == descs.size() ||
        !zeroPaddingTo(payload, cursor, aligned)) return false;
    std::memcpy(payload.data() + cursor, bytes.data(), bytes.size());
    descs[sectionIndex++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = static_cast<std::uint32_t>(bytes.size()),
        .payloadOffset = static_cast<std::uint32_t>(cursor),
        .byteSize = static_cast<std::uint32_t>(bytes.size()),
    };
    cursor += bytes.size();
    return true;
  }

  bool directIdentity(std::uint32_t kind, std::uint32_t pin,
                      D9CWireObjectIdentity& out) const noexcept {
    switch (kind) {
      case D9C_CHUNK_HANDLE_KIND_SURFACE:
        return visitPin(storage_->surfaces, surfaceCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_TEXTURE:
        return visitPin(storage_->textures, textureCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_BUFFER:
        return visitPin(storage_->buffers, bufferCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_SHADER:
        return visitPin(storage_->shaders, shaderCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
        return visitPin(storage_->declarations, declarationCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_QUERY:
        return visitPin(storage_->queries, queryCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      default:
        return false;
    }
  }

  bool directHandle(std::uint32_t kind, std::uint32_t pin,
                    const EmissionHandleContext& handles,
                    std::size_t globalFirst, std::uint32_t& out) const noexcept {
    D9CWireObjectIdentity identity{};
    std::size_t local = 0u;
    if (!directIdentity(kind, pin, identity) ||
        !handles.indexOf(identity, local) ||
        globalFirst + local > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    out = static_cast<std::uint32_t>(globalFirst + local);
    return true;
  }

  bool writeConstantSection(
      std::uint16_t kind, const PeSemanticArenaRange& bytes,
      const D9CCommandChunkWireConstantRange& range,
      std::span<std::byte> payload, std::size_t& cursor,
      std::array<D9CCommandChunkWireSectionDesc,
                 D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
      std::size_t& sectionIndex) const noexcept {
    if (range.registerCount == 0u) return bytes.count == 0u;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    if (!rule || !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        aligned > payload.size() ||
        sizeof(range) + bytes.count > payload.size() - aligned ||
        sectionIndex == descs.size() || bytes.offset > MaxSemanticBytes ||
        bytes.count > MaxSemanticBytes - bytes.offset ||
        !zeroPaddingTo(payload, cursor, aligned)) return false;
    if (!writeValue(payload, cursor, range)) return false;
    const auto source = std::span<const std::byte>(
        storage_->constantBytes.data() + bytes.offset, bytes.count);
    if (cursor > payload.size() || source.size() > payload.size() - cursor) return false;
    std::memcpy(payload.data() + cursor, source.data(), source.size());
    cursor += source.size();
    descs[sectionIndex++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = range.registerCount,
        .payloadOffset = static_cast<std::uint32_t>(aligned),
        .byteSize = static_cast<std::uint32_t>(sizeof(range) + bytes.count),
    };
    return true;
  }

  bool writeSparsePayload(const PeSemanticRecordSlot& slot,
                          std::span<std::byte> payload,
                          std::size_t globalFirst,
                          const EmissionHandleContext& handles) const noexcept {
    const auto count = sparseSectionCount(slot);
    if (count > D9C_COMMAND_CHUNK_SECTION_COUNT) return false;
    D9CCommandChunkWireDrawHeader draw = slot.draw;
    draw.sectionCount = static_cast<std::uint32_t>(count);
    draw.sectionTableOffset = sizeof(draw);
    draw.sectionPayloadOffset = static_cast<std::uint32_t>(
        sizeof(draw) + count * sizeof(D9CCommandChunkWireSectionDesc));
    draw.reserved0 = 0u;
    std::size_t cursor = 0u;
    if (!writeValue(payload, cursor, draw)) return false;
    std::array<D9CCommandChunkWireSectionDesc,
               D9C_COMMAND_CHUNK_SECTION_COUNT>
        descs{};
    const auto tableBytes = count * sizeof(D9CCommandChunkWireSectionDesc);
    if (cursor > payload.size() || tableBytes > payload.size() - cursor) return false;
    cursor += tableBytes;
    std::size_t sectionIndex = 0u;
    const auto textures = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireTextureBinding, PeSemanticTextureIdentity>>(
        storage_->texturesArena.data() + slot.textures.offset, slot.textures.count);
    const auto streams = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireStreamBinding, PeSemanticBufferIdentity>>(
        storage_->streamsArena.data() + slot.streams.offset, slot.streams.count);
    const auto shaders = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireShaderBinding, PeSemanticShaderIdentity>>(
        storage_->shadersArena.data() + slot.shaders.offset, slot.shaders.count);
    const auto vertexInputs = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireVertexInput, PeSemanticDeclarationIdentity>>(
        storage_->vertexInputsArena.data() + slot.vertexInputs.offset, slot.vertexInputs.count);
    const auto indexBuffers = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireIndexBinding, PeSemanticBufferIdentity>>(
        storage_->indexBuffersArena.data() + slot.indexBuffers.offset, slot.indexBuffers.count);
    const auto renderTargets = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireRenderTargetBinding, PeSemanticSurfaceIdentity>>(
        storage_->renderTargetsArena.data() + slot.renderTargets.offset, slot.renderTargets.count);
    const auto depthStencils = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireDepthStencilBinding, PeSemanticSurfaceIdentity>>(
        storage_->depthStencilsArena.data() + slot.depthStencils.offset, slot.depthStencils.count);
    if (!writeTypedSection(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
                           std::span<const D9CCommandChunkWireRenderState>(
                               storage_->renderStates.data() + slot.renderStates.offset,
                               slot.renderStates.count), payload, cursor, descs, sectionIndex) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_TEXTURE, textures, payload, cursor,
                             descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_STREAM, streams, payload, cursor,
                             descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_SHADER, shaders, payload, cursor,
                             descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT, vertexInputs, payload,
                             cursor, descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER, indexBuffers, payload,
                             cursor, descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, renderTargets, payload,
                             cursor, descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, depthStencils, payload,
                             cursor, descs, sectionIndex, handles, globalFirst) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_VIEWPORT,
                           std::span<const D9CViewport>(
                               storage_->viewports.data() + slot.viewports.offset,
                               slot.viewports.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_SCISSOR,
                           std::span<const D9CRect>(storage_->scissors.data() + slot.scissors.offset,
                                                    slot.scissors.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_MATERIAL,
                           std::span<const D9CMaterial>(storage_->materials.data() + slot.materials.offset,
                                                        slot.materials.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE,
                           std::span<const D9CCommandChunkWireClipPlane>(
                               storage_->clipPlanes.data() + slot.clipPlanes.offset,
                               slot.clipPlanes.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
                           std::span<const D9CDrawPacketTextureStageState>(
                               storage_->textureStageStates.data() + slot.textureStageStates.offset,
                               slot.textureStageStates.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                           std::span<const D9CDrawPacketSamplerState>(
                               storage_->samplerStates.data() + slot.samplerStates.offset,
                               slot.samplerStates.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_TRANSFORM,
                           std::span<const D9CDrawPacketTransform>(
                               storage_->transforms.data() + slot.transforms.offset,
                               slot.transforms.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_LIGHT,
                           std::span<const D9CCommandChunkWireLight>(
                               storage_->lights.data() + slot.lights.offset,
                               slot.lights.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
                           std::span<const D9CCommandChunkWireLightEnable>(
                               storage_->lightEnables.data() + slot.lightEnables.offset,
                               slot.lightEnables.count), payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F, slot.vsFloatConstants,
                              slot.vsFloatConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I, slot.vsIntConstants,
                              slot.vsIntConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B, slot.vsBoolConstants,
                              slot.vsBoolConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F, slot.psFloatConstants,
                              slot.psFloatConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I, slot.psIntConstants,
                              slot.psIntConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B, slot.psBoolConstants,
                              slot.psBoolConstant, payload, cursor, descs, sectionIndex) ||
        !writeBytesSection(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA,
                           std::span<const std::byte>(storage_->constantBytes.data() + slot.upIndexBytes.offset,
                                                       slot.upIndexBytes.count), payload, cursor, descs, sectionIndex) ||
        !writeBytesSection(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
                           std::span<const std::byte>(storage_->constantBytes.data() + slot.upVertexBytes.offset,
                                                       slot.upVertexBytes.count), payload, cursor, descs, sectionIndex)) return false;
    if (sectionIndex != count || cursor != payload.size()) return false;
    std::memcpy(payload.data() + draw.sectionTableOffset, descs.data(), tableBytes);
    return true;
  }

  bool writeRecordPayload(const PeSemanticRecordSlot& slot,
                          std::span<std::byte> payload,
                          std::size_t globalFirst,
                          const EmissionHandleContext& handles) const noexcept {
    std::size_t cursor = 0u;
    switch (slot.producer) {
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        return writeSparsePayload(slot, payload, globalFirst, handles);
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        return writeValue(payload, cursor, slot.setConst) &&
               slot.constantBytes.offset <= MaxSemanticBytes &&
               slot.constantBytes.count <= MaxSemanticBytes - slot.constantBytes.offset &&
               cursor + slot.constantBytes.count <= payload.size() &&
               (std::memcpy(payload.data() + cursor,
                            storage_->constantBytes.data() + slot.constantBytes.offset,
                            slot.constantBytes.count),
                cursor += slot.constantBytes.count, cursor == payload.size());
      case PeSemanticProducerKind::Clear: {
        auto fixed = slot.clear;
        fixed.rectCount = slot.clearRects.count;
        fixed.rectOffset = sizeof(fixed);
        if (!writeValue(payload, cursor, fixed) ||
            slot.clearRects.offset > MaxRects ||
            slot.clearRects.count > MaxRects - slot.clearRects.offset ||
            slot.clearRects.count * sizeof(D9CRect) > payload.size() - cursor) return false;
        std::memcpy(payload.data() + cursor,
                    storage_->rects.data() + slot.clearRects.offset,
                    slot.clearRects.count * sizeof(D9CRect));
        cursor += slot.clearRects.count * sizeof(D9CRect);
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::Present: {
        auto fixed = slot.present;
        fixed.sourceHandleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.sourceHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::StretchRect: {
        auto fixed = slot.stretchRect;
        fixed.reserved0 = 0u;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface1, handles,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::ColorFill: {
        auto fixed = slot.colorFill;
        fixed.reserved0 = 0u;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.surfaceHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::UpdateTexture: {
        D9CCommandChunkWireUpdateTexture fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture0, handles,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture1, handles,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::UpdateSurface: {
        auto fixed = slot.updateSurface;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface1, handles,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::QueryIssue: {
        auto fixed = slot.queryIssue;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_QUERY, slot.query, handles,
                          globalFirst, fixed.queryHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::Readback: {
        D9CCommandChunkWireReadback fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface1, handles,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::ReszDepthResolve: {
        D9CCommandChunkWireReszDepthResolve fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.msaaDepthHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture0, handles,
                          globalFirst, fixed.intzDestHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::GenerateMipmaps: {
        D9CCommandChunkWireGenerateMipmaps fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture0, handles,
                          globalFirst, fixed.textureHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::Count:
        return false;
    }
    return false;
  }

  // Materialize one record's canonical D9C V2 role bytes while the source
  // slot is still being admitted. The payload's relative offsets and the
  // record-local/global handle ordinals are already known from the prepared
  // witness; only the outer chunk offsets remain frontier-dependent.
  bool materializeCanonicalRecord(
      const PeSemanticRecordSlot& slot,
      const PeSemanticPreparedRecord& prepared) noexcept {
    if (!prepared.valid() || prepared.recordCount != recordCount_ ||
        prepared.emissionHandleCount != emissionHandleCount_ ||
        prepared.emissionPayloadBytes != emissionPayloadBytes_ ||
        prepared.plan.handleCount > kMaxRecordHandles ||
        prepared.payloadOffset > Storage::maxWirePayloadBytes ||
        prepared.plan.payloadBytes >
            Storage::maxWirePayloadBytes - prepared.payloadOffset ||
        prepared.emissionHandleCount > Storage::maxWireHandles ||
        recordCount_ >= Storage::maxWireRecords) {
      return false;
    }
    const auto& handles = prepared.wireHandles;
    if (handles.count != prepared.plan.handleCount ||
        handles.count > kMaxRecordHandles) {
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
    if (!writeRecordPayload(slot, payload, prepared.emissionHandleCount,
                            handles)) {
      return false;
    }
    for (std::size_t i = 0u; i < handles.count; ++i) {
      storage_->wireHandles[prepared.emissionHandleCount + i] = {
          .kind = handles.identities[i].kind,
          .generation = handles.identities[i].generation,
          .objectId = handles.identities[i].objectId,
      };
    }
    storage_->wireRecords[recordCount_] = {
        .type = slot.recordType,
        .flags = slot.recordFlags,
        .payloadOffset = static_cast<std::uint32_t>(prepared.payloadOffset),
        .payloadSize = static_cast<std::uint32_t>(prepared.plan.payloadBytes),
        .firstHandle = static_cast<std::uint32_t>(prepared.emissionHandleCount),
        .handleCount = static_cast<std::uint32_t>(handles.count),
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
    return {
        .header = plan.header,
        .records = toWireHandle(records.data()),
        .recordBytes = static_cast<std::uint32_t>(records.size()),
        .recordReserved = 0u,
        .handles = toWireHandle(handles.data()),
        .handleBytes = static_cast<std::uint32_t>(handles.size()),
        .handleReserved = 0u,
        .payload = toWireHandle(payload.data()),
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

  std::size_t ownedMaterializedBytes() const noexcept {
    if (!ready_) return 0u;
    std::size_t bytes = recordCount_ * sizeof(storage_->records[0]);
    bytes += surfaceCount_ * sizeof(storage_->surfaces[0]);
    bytes += textureCount_ * sizeof(storage_->textures[0]);
    bytes += bufferCount_ * sizeof(storage_->buffers[0]);
    bytes += shaderCount_ * sizeof(storage_->shaders[0]);
    bytes += declarationCount_ * sizeof(storage_->declarations[0]);
    bytes += queryCount_ * sizeof(storage_->queries[0]);
    bytes += semanticBytes_;
    bytes += rectCount_ * sizeof(storage_->rects[0]);
    bytes += sparseCounts_.values[kRenderStates] *
        sizeof(storage_->renderStates[0]);
    bytes += sparseCounts_.values[kTextures] *
        sizeof(storage_->texturesArena[0]);
    bytes += sparseCounts_.values[kStreams] *
        sizeof(storage_->streamsArena[0]);
    bytes += sparseCounts_.values[kShaders] *
        sizeof(storage_->shadersArena[0]);
    bytes += sparseCounts_.values[kVertexInputs] *
        sizeof(storage_->vertexInputsArena[0]);
    bytes += sparseCounts_.values[kIndexBuffers] *
        sizeof(storage_->indexBuffersArena[0]);
    bytes += sparseCounts_.values[kRenderTargets] *
        sizeof(storage_->renderTargetsArena[0]);
    bytes += sparseCounts_.values[kDepthStencils] *
        sizeof(storage_->depthStencilsArena[0]);
    bytes += sparseCounts_.values[kViewports] * sizeof(storage_->viewports[0]);
    bytes += sparseCounts_.values[kScissors] * sizeof(storage_->scissors[0]);
    bytes += sparseCounts_.values[kMaterials] * sizeof(storage_->materials[0]);
    bytes += sparseCounts_.values[kClipPlanes] *
        sizeof(storage_->clipPlanes[0]);
    bytes += sparseCounts_.values[kTextureStageStates] *
        sizeof(storage_->textureStageStates[0]);
    bytes += sparseCounts_.values[kSamplerStates] *
        sizeof(storage_->samplerStates[0]);
    bytes += sparseCounts_.values[kTransforms] *
        sizeof(storage_->transforms[0]);
    bytes += sparseCounts_.values[kLights] * sizeof(storage_->lights[0]);
    bytes += sparseCounts_.values[kLightEnables] *
        sizeof(storage_->lightEnables[0]);
    bytes += recordCount_ * sizeof(storage_->wireRecords[0]);
    bytes += emissionHandleCount_ * sizeof(storage_->wireHandles[0]);
    bytes += emissionPayloadBytes_;
    return bytes;
  }

  template <typename Admit>
    requires std::is_nothrow_invocable_r_v<bool, Admit&>
  bool recordAdmission(Admit&& admit) noexcept {
    auto&& operation = admit;
    auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
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
  bool cacheEmissionMetrics(const PeSemanticPreparedRecord& prepared) noexcept {
    if (!prepared.admissibleAt(recordCount_, emissionHandleCount_,
                               emissionPayloadBytes_)) {
      return false;
    }
    emissionPayloadBytes_ = prepared.nextEmissionPayloadBytes;
    emissionHandleCount_ = prepared.nextEmissionHandleCount;
    return true;
  }

  // Compatibility/oracle path: retain the old slot walk for tests that call
  // owner admission directly without a prepared pure result.
  bool cacheEmissionMetrics(const PeSemanticRecordSlot& slot) noexcept {
    const auto* rule = recordRule(slot.recordType);
    if (!rule) return false;
    EmissionHandleContext handles{};
    if (!visitRecordHandles(slot, [&](const D9CWireObjectIdentity& identity) noexcept {
          return handles.add(identity);
        })) {
      return false;
    }
    std::size_t bytes = 0u;
    std::size_t aligned = 0u;
    if (!payloadSize(slot, bytes) ||
        !alignEmission(emissionPayloadBytes_, rule->payloadAlignment, aligned) ||
        bytes > std::numeric_limits<std::size_t>::max() - aligned ||
        emissionHandleCount_ > std::numeric_limits<std::size_t>::max() -
            handles.count) {
      return false;
    }
    const auto nextPayloadBytes = aligned + bytes;
    const auto nextHandleCount = emissionHandleCount_ + handles.count;
    if (recordCount_ + 1u > std::numeric_limits<std::uint32_t>::max() ||
        nextPayloadBytes > std::numeric_limits<std::uint32_t>::max() ||
        nextHandleCount > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    const auto layout = planExactCommandChunkLayout(
        static_cast<std::uint32_t>(recordCount_ + 1u),
        static_cast<std::uint32_t>(nextHandleCount),
        static_cast<std::uint32_t>(nextPayloadBytes));
    if (!layout.valid() || layout.totalBytes > D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES)
      return false;
    emissionPayloadBytes_ = nextPayloadBytes;
    emissionHandleCount_ = nextHandleCount;
    return true;
  }

  template <typename PinArray>
  bool findOrValidateExistingPin(
      const PinIndex<kPinIndexCapacity>& exactIndex,
      const PinIndex<kPinIndexCapacity>& objectIndex,
      const PinArray& pins, std::size_t count,
      const D9CWireObjectIdentity& identity, const void* object,
      bool& existing) const noexcept {
    std::size_t objectPin = 0u;
    const bool objectFound =
        findPinIndex(objectIndex, identity, false, objectPin);
    std::size_t exactPin = 0u;
    const bool exactFound =
        findPinIndex(exactIndex, identity, true, exactPin);
    if (!objectFound) {
      if (exactFound) return false;
      existing = false;
      return true;
    }
    if (!exactFound || exactPin != objectPin || objectPin >= count ||
        pins[objectPin].object != object) {
      // The object-id index deliberately rejects a generation or pointer
      // alias, even when the full identity differs.
      return false;
    }
    existing = true;
    return true;
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
    if (recordCount_ >= MaxRecords || sourceOrdinal == 0u ||
        recordOrdinal == 0u || !producerMatches) return false;
    if (recordCount_ != 0u &&
        (recordOrdinal <= storage_->records[recordCount_ - 1u].recordOrdinal ||
         sourceOrdinal <= storage_->records[recordCount_ - 1u].sourceOrdinal)) {
      return false;
    }
    if (!rule || (staged.recordFlags & ~rule->allowedRecordFlags) != 0u) {
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

  template <PeSemanticRecordInputLike Input>
  bool validAdmissionHeader(
      const Input& input,
      const PeSemanticPreparedRecord& prepared) const noexcept {
    return validAdmissionHeader(input, prepared.rule,
                                prepared.producerMatchesRecordType,
                                prepared.constantProducer,
                                prepared.upProducer);
  }

  template <PeSemanticRecordInputLike Input>
  bool copyFixedValues(const Input& input) noexcept {
    const auto& staged = stagedPeSemanticInput(input);
    auto& slot = storage_->records[recordCount_];
    slot.producer = staged.producer;
    slot.recordType = peSemanticRecordType(input);
    slot.recordFlags = staged.recordFlags;
    slot.sourceOrdinal = peSemanticSourceOrdinal(input);
    slot.recordOrdinal = peSemanticRecordOrdinal(input);
    slot.draw = staged.draw;
    slot.setConst = staged.setConst;
    slot.clear = staged.clear;
    slot.present = staged.present;
    slot.stretchRect = staged.stretchRect;
    slot.colorFill = staged.colorFill;
    slot.updateSurface = staged.updateSurface;
    slot.queryIssue = staged.queryIssue;
    slot.updateFlags = staged.updateFlags;
    slot.reszFlags = staged.reszFlags;
    slot.mipmapFlags = staged.mipmapFlags;
    return true;
  }

  template <PeSemanticRecordInputLike Input>
  static bool validConstantInput(const Input& input) noexcept {
    const auto& staged = stagedPeSemanticInput(input);
    const bool boolean = staged.producer == PeSemanticProducerKind::VsBoolConstant ||
                         staged.producer == PeSemanticProducerKind::PsBoolConstant;
    const auto elementBytes = boolean ? 4u : 16u;
    const auto expected = static_cast<std::uint64_t>(
        staged.setConst.registerCount) * elementBytes;
    const auto limit = boolean ? 16u : 256u;
    return staged.setConst.registerCount <= limit &&
           expected == staged.constantBytes.size();
  }

  template <typename Object, typename Identity>
  PinIndex<kPinIndexCapacity>& exactPinIndex() noexcept {
    if constexpr (std::is_same_v<Object, D9CSurface>)
      return storage_->surfaceIdentityIndex;
    if constexpr (std::is_same_v<Object, D9CTexture>)
      return storage_->textureIdentityIndex;
    if constexpr (std::is_same_v<Object, D9CBuffer>)
      return storage_->bufferIdentityIndex;
    if constexpr (std::is_same_v<Object, D9CShader>)
      return storage_->shaderIdentityIndex;
    if constexpr (std::is_same_v<Object, D9CVertexDecl>)
      return storage_->declarationIdentityIndex;
    return storage_->queryIdentityIndex;
  }

  template <typename Object, typename Identity>
  PinIndex<kPinIndexCapacity>& objectPinIndex() noexcept {
    if constexpr (std::is_same_v<Object, D9CSurface>)
      return storage_->surfaceObjectIndex;
    if constexpr (std::is_same_v<Object, D9CTexture>)
      return storage_->textureObjectIndex;
    if constexpr (std::is_same_v<Object, D9CBuffer>)
      return storage_->bufferObjectIndex;
    if constexpr (std::is_same_v<Object, D9CShader>)
      return storage_->shaderObjectIndex;
    if constexpr (std::is_same_v<Object, D9CVertexDecl>)
      return storage_->declarationObjectIndex;
    return storage_->queryObjectIndex;
  }

  template <typename Object, typename Identity, typename Ref, typename PinArray,
            typename Retain>
  bool pin(Ref ref, PinArray& pins, std::size_t& count, std::uint32_t& out,
           Retain retain) noexcept {
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
    auto& exactIndex = exactPinIndex<Object, Identity>();
    auto& objectIndex = objectPinIndex<Object, Identity>();
    std::size_t existing = 0u;
    if (findPinIndex(objectIndex, identity.value, false, existing)) {
      // The object-id index also rejects a stale generation for the same
      // object identity. An exact hit is the only duplicate admitted.
      std::size_t exact = 0u;
      if (!findPinIndex(exactIndex, identity.value, true, exact) ||
          exact != existing || pins[existing].object != ref.object) {
        return false;
      }
      out = static_cast<std::uint32_t>(existing);
      return true;
    }
    if (findPinIndex(exactIndex, identity.value, true, existing)) {
      return false;
    }
    const std::size_t index = count;
    const bool warmObject = [&]() noexcept {
      if constexpr (std::is_same_v<Object, D9CSurface>)
        return retainer_.containsSurface(static_cast<D9CSurface*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CTexture>)
        return retainer_.containsTexture(static_cast<D9CTexture*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CBuffer>)
        return retainer_.containsBuffer(static_cast<D9CBuffer*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CShader>)
        return retainer_.containsShader(static_cast<D9CShader*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CVertexDecl>)
        return retainer_.containsVdecl(static_cast<D9CVertexDecl*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CQuery>)
        return retainer_.containsQuery(static_cast<D9CQuery*>(ref.object));
      return false;
    }();
    if (index == MaxPins ||
        (!warmObject && retainer_.size() >= warmRetainerCapacity) ||
        !retain(static_cast<Object*>(ref.object),
                                    retainedCheckpoint_)) return false;
    pins[index] = {.object = static_cast<Object*>(ref.object),
                   .identity = identity};
    if (!insertPinIndex(exactIndex, identity.value,
                        static_cast<std::uint32_t>(index)) ||
        !insertPinIndex(objectIndex, identity.value,
                        static_cast<std::uint32_t>(index), false)) {
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
                      PeSemanticRecordSlot& slot) noexcept {
    const auto& staged = stagedPeSemanticInput(input);
    const auto retainSurface = [this](D9CSurface* p,
                                      D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainSurface(p, retainedCheckpoint_);
    };
    const auto retainTexture = [this](D9CTexture* p,
                                      D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainTexture(p, retainedCheckpoint_);
    };
    const auto retainBuffer = [this](D9CBuffer* p,
                                     D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainBuffer(p, retainedCheckpoint_);
    };
    const auto retainShader = [this](D9CShader* p,
                                     D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainShader(p, retainedCheckpoint_);
    };
    const auto retainDeclaration = [this](D9CVertexDecl* p,
                                          D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainVdecl(p, retainedCheckpoint_);
    };
    const auto retainQuery = [this](D9CQuery* p,
                                    D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainQuery(p, retainedCheckpoint_);
    };
    return pin<D9CSurface, PeSemanticSurfaceIdentity>(staged.surface0,
                                                       storage_->surfaces, surfaceCount_, slot.surface0,
                                                       retainSurface) &&
           pin<D9CSurface, PeSemanticSurfaceIdentity>(staged.surface1,
                                                       storage_->surfaces, surfaceCount_, slot.surface1,
                                                       retainSurface) &&
           pin<D9CTexture, PeSemanticTextureIdentity>(staged.texture0,
                                                       storage_->textures, textureCount_, slot.texture0,
                                                       retainTexture) &&
           pin<D9CTexture, PeSemanticTextureIdentity>(staged.texture1,
                                                       storage_->textures, textureCount_, slot.texture1,
                                                       retainTexture) &&
           pin<D9CBuffer, PeSemanticBufferIdentity>(staged.buffer0, storage_->buffers, bufferCount_,
                                                     slot.buffer0, retainBuffer) &&
           pin<D9CBuffer, PeSemanticBufferIdentity>(staged.buffer1, storage_->buffers, bufferCount_,
                                                     slot.buffer1, retainBuffer) &&
           pin<D9CShader, PeSemanticShaderIdentity>(staged.shader0, storage_->shaders, shaderCount_,
                                                     slot.shader0, retainShader) &&
           pin<D9CShader, PeSemanticShaderIdentity>(staged.shader1, storage_->shaders, shaderCount_,
                                                     slot.shader1, retainShader) &&
           pin<D9CVertexDecl, PeSemanticDeclarationIdentity>(
               staged.declaration, storage_->declarations, declarationCount_, slot.declaration,
               retainDeclaration) &&
           pin<D9CQuery, PeSemanticQueryIdentity>(staged.query, storage_->queries, queryCount_,
                                                  slot.query, retainQuery);
  }

  template <typename T, std::size_t N>
  bool appendSparse(std::span<const T> source, std::array<T, N>& arena,
                    std::size_t& used, PeSemanticArenaRange& range) noexcept {
    if (source.empty()) { range = {}; return true; }
    if (source.size() > MaxSparseValues - used) return false;
    range = {.offset = static_cast<std::uint32_t>(used),
             .count = static_cast<std::uint32_t>(source.size())};
    std::copy(source.begin(), source.end(), arena.begin() + used);
    used += source.size();
    return true;
  }

  bool appendBytes(std::span<const std::byte> source,
                   std::array<std::byte, MaxSemanticBytes>& arena,
                   std::size_t& used, PeSemanticArenaRange& range) noexcept {
    if (source.empty()) { range = {}; return true; }
    if (source.size() > MaxSemanticBytes - used ||
        source.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    range = {.offset = static_cast<std::uint32_t>(used),
             .count = static_cast<std::uint32_t>(source.size())};
    std::copy(source.begin(), source.end(), arena.begin() + used);
    used += source.size();
    return true;
  }

  template <PeSemanticRecordInputLike Input>
  bool copySparse(const Input& input,
                  PeSemanticRecordSlot& slot) noexcept {
    const auto& s = peSemanticSparse(input);
    const auto withinSchema = [](std::uint16_t kind,
                                 std::size_t count) noexcept {
      const auto* rule = sectionRule(kind);
      return rule && count <= rule->maxCount;
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
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_TRANSFORM,
                      s.transforms.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_LIGHT, s.lights.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
                      s.lightEnables.size())) {
      lastAdmissionFailure_ = AdmissionFailure::SparseSchema;
      return false;
    }
    if (!appendSparse(s.renderStates, storage_->renderStates, sparseCounts_.values[kRenderStates], slot.renderStates) ||
        !appendSparse(s.viewports, storage_->viewports, sparseCounts_.values[kViewports], slot.viewports) ||
        !appendSparse(s.scissors, storage_->scissors, sparseCounts_.values[kScissors], slot.scissors) ||
        !appendSparse(s.materials, storage_->materials, sparseCounts_.values[kMaterials], slot.materials) ||
        !appendSparse(s.clipPlanes, storage_->clipPlanes, sparseCounts_.values[kClipPlanes], slot.clipPlanes) ||
        !appendSparse(s.textureStageStates, storage_->textureStageStates, sparseCounts_.values[kTextureStageStates], slot.textureStageStates) ||
        !appendSparse(s.samplerStates, storage_->samplerStates, sparseCounts_.values[kSamplerStates], slot.samplerStates) ||
        !appendSparse(s.transforms, storage_->transforms, sparseCounts_.values[kTransforms], slot.transforms) ||
        !appendSparse(s.lights, storage_->lights, sparseCounts_.values[kLights], slot.lights) ||
        !appendSparse(s.lightEnables, storage_->lightEnables, sparseCounts_.values[kLightEnables], slot.lightEnables)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseArena;
      return false;
    }
    return copyBindingSparse(s, slot);
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
    if (!appendBytes(staged.constantBytes, storage_->constantBytes, semanticBytes_,
                     slot.constantBytes) ||
        !appendBytes(sparse.vsFloatConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.vsFloatConstants) ||
        !appendBytes(sparse.vsIntConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.vsIntConstants) ||
        !appendBytes(sparse.vsBoolConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.vsBoolConstants) ||
        !appendBytes(sparse.psFloatConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.psFloatConstants) ||
        !appendBytes(sparse.psIntConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.psIntConstants) ||
        !appendBytes(sparse.psBoolConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.psBoolConstants) ||
        !appendBytes(sparse.upIndexData, storage_->constantBytes, semanticBytes_,
                     slot.upIndexBytes) ||
        !appendBytes(sparse.upVertexData, storage_->constantBytes, semanticBytes_,
                     slot.upVertexBytes)) return false;
    if (!staged.clearRects.empty()) {
      if (staged.clearRects.size() > MaxRects - rectCount_) return false;
      slot.clearRects = {.offset = static_cast<std::uint32_t>(rectCount_),
                         .count = static_cast<std::uint32_t>(staged.clearRects.size())};
      std::copy(staged.clearRects.begin(), staged.clearRects.end(),
                storage_->rects.begin() + rectCount_);
      rectCount_ += staged.clearRects.size();
    }
    return true;
  }

  bool copyBindingSparse(const SparseStateInput& s,
                         PeSemanticRecordSlot& slot) noexcept {
    // The typed sparse arenas are copied by the small per-family routines
    // below.  They validate the source's kind before asking the retainer for a
    // pin, so an invalid/foreign binding cannot leak a retain.
    if (!copyTextures(s.textures, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseTextures;
      return false;
    }
    if (!copyStreams(s.streams, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseStreams;
      return false;
    }
    if (!copyShaders(s.shaders, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseShaders;
      return false;
    }
    if (!copyVertexInputs(s.vertexInputs, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseVertexInputs;
      return false;
    }
    if (!copyIndexBuffers(s.indexBuffers, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseIndexBuffers;
      return false;
    }
    if (!copyRenderTargets(s.renderTargets, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseRenderTargets;
      return false;
    }
    if (!copyDepthStencils(s.depthStencils, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseDepthStencils;
      return false;
    }
    return true;
  }

  bool copyTextures(std::span<const SparseBindingInput<D9CCommandChunkWireTextureBinding>> source,
                    PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kTextures]) return false;
    const auto start = sparseCounts_.values[kTextures];
    for (const auto& item : source) {
      auto& out = storage_->texturesArena[sparseCounts_.values[kTextures]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = TextureRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CTexture, PeSemanticTextureIdentity>(ref, storage_->textures, textureCount_, out.pin,
                                                         retainTextureFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.textures = {.offset = static_cast<std::uint32_t>(start),
                     .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyStreams(std::span<const SparseBindingInput<D9CCommandChunkWireStreamBinding>> source,
                   PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kStreams]) return false;
    const auto start = sparseCounts_.values[kStreams];
    for (const auto& item : source) {
      auto& out = storage_->streamsArena[sparseCounts_.values[kStreams]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = BufferRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CBuffer, PeSemanticBufferIdentity>(ref, storage_->buffers, bufferCount_, out.pin,
                                                         retainBufferFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.streams = {.offset = static_cast<std::uint32_t>(start),
                    .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyShaders(std::span<const SparseBindingInput<D9CCommandChunkWireShaderBinding>> source,
                   PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kShaders]) return false;
    const auto start = sparseCounts_.values[kShaders];
    for (const auto& item : source) {
      auto& out = storage_->shadersArena[sparseCounts_.values[kShaders]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = ShaderRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CShader, PeSemanticShaderIdentity>(ref, storage_->shaders, shaderCount_, out.pin,
                                                         retainShaderFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.shaders = {.offset = static_cast<std::uint32_t>(start),
                    .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyVertexInputs(std::span<const SparseBindingInput<D9CCommandChunkWireVertexInput>> source,
                        PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kVertexInputs]) return false;
    const auto start = sparseCounts_.values[kVertexInputs];
    for (const auto& item : source) {
      auto& out = storage_->vertexInputsArena[sparseCounts_.values[kVertexInputs]++];
      out.wire = item.wire;
      if (item.wire.valid && item.wire.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION) {
        auto ref = DeclarationRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CVertexDecl, PeSemanticDeclarationIdentity>(ref, storage_->declarations, declarationCount_, out.pin,
                                                                   retainDeclarationFn())) return false;
          out.hasPin = true;
        }
      } else if (item.wire.valid && item.wire.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF && item.object.object) {
        return false;
      }
    }
    slot.vertexInputs = {.offset = static_cast<std::uint32_t>(start),
                         .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyIndexBuffers(std::span<const SparseBindingInput<D9CCommandChunkWireIndexBinding>> source,
                        PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kIndexBuffers]) return false;
    const auto start = sparseCounts_.values[kIndexBuffers];
    for (const auto& item : source) {
      auto& out = storage_->indexBuffersArena[sparseCounts_.values[kIndexBuffers]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = BufferRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CBuffer, PeSemanticBufferIdentity>(ref, storage_->buffers, bufferCount_, out.pin,
                                                         retainBufferFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.indexBuffers = {.offset = static_cast<std::uint32_t>(start),
                         .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyRenderTargets(std::span<const SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>> source,
                         PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kRenderTargets]) return false;
    const auto start = sparseCounts_.values[kRenderTargets];
    for (const auto& item : source) {
      auto& out = storage_->renderTargetsArena[sparseCounts_.values[kRenderTargets]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = SurfaceRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CSurface, PeSemanticSurfaceIdentity>(ref, storage_->surfaces, surfaceCount_, out.pin,
                                                          retainSurfaceFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.renderTargets = {.offset = static_cast<std::uint32_t>(start),
                          .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyDepthStencils(std::span<const SparseBindingInput<D9CCommandChunkWireDepthStencilBinding>> source,
                         PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kDepthStencils]) return false;
    const auto start = sparseCounts_.values[kDepthStencils];
    for (const auto& item : source) {
      auto& out = storage_->depthStencilsArena[sparseCounts_.values[kDepthStencils]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = SurfaceRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CSurface, PeSemanticSurfaceIdentity>(ref, storage_->surfaces, surfaceCount_, out.pin,
                                                          retainSurfaceFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.depthStencils = {.offset = static_cast<std::uint32_t>(start),
                          .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  // Typed retain functors keep the owner API free of type-erased object sinks.
  auto retainSurfaceFn() noexcept {
    return [this](D9CSurface* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainSurface(p, retainedCheckpoint_);
    };
  }
  auto retainTextureFn() noexcept {
    return [this](D9CTexture* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainTexture(p, retainedCheckpoint_);
    };
  }
  auto retainBufferFn() noexcept {
    return [this](D9CBuffer* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainBuffer(p, retainedCheckpoint_);
    };
  }
  auto retainShaderFn() noexcept {
    return [this](D9CShader* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainShader(p, retainedCheckpoint_);
    };
  }
  auto retainDeclarationFn() noexcept {
    return [this](D9CVertexDecl* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainVdecl(p, retainedCheckpoint_);
    };
  }
  auto retainQueryFn() noexcept {
    return [this](D9CQuery* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainQuery(p, retainedCheckpoint_);
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
  // the borrowed-variable arena cursor rather than wire payload size.
  std::size_t emissionHandleCount_ = 0u;
  std::size_t emissionPayloadBytes_ = 0u;
  std::size_t rectCount_ = 0u;
  std::size_t lastClearedBytes_ = 0u;
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
bool resolvePeSemanticCadenceMetrics(const Owner& owner, std::size_t& handles,
                                     std::size_t& payload,
                                     std::size_t& wire) noexcept {
  handles = payload = wire = 0u;
  return owner.size() == 0u || owner.emissionMetrics(handles, payload, wire);
}

template <std::size_t MaxRecords, std::size_t MaxPins,
          std::size_t MaxSemanticBytes, std::size_t MaxRects,
          std::size_t MaxSparseValues>
bool appendOwnedRecord(
    PeSemanticBatchOwner<MaxRecords, MaxPins, MaxSemanticBytes, MaxRects,
                         MaxSparseValues>& owner,
    const PeSemanticRecordInput& input) noexcept {
  return owner.appendOwnedRecord(input);
}

// The old Present/Readback candidate remains a call-local pilot.  It is not a
// chunk owner and must not be used as the all-family semantic owner.
static_assert(static_cast<std::size_t>(PeSemanticProducerKind::Count) == 21u);

}  // namespace dxmt9::d3d9::pe
