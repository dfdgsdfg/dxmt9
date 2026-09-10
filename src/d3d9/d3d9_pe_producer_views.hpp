#pragma once

// POD inputs for the PE sparse-state producer.
//
// The producer is pure over (a) bindings, (b) the constant shadow, and
// (c) draw payloads. Destination-chunk state (d) is NOT a pass-1 input:
// production applies it inside draw emitters after CapacityPre, and never on
// the barrier path. Compare the callers of
// d3d9_pe_device.cpp's populatePendingChunkDrawStreamDependencies against
// chunkBarrierFlush, which builds an APPLY_STATE packet with no chunk-context
// step at all. Compact plans use finalizeSparseStatePlanChunkContext; the
// compatibility projection uses addChunkContextSections. See
// docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md §3.
//
// References here name symbols rather than line numbers on purpose: this
// migration moves large blocks of d3d9_pe_device.cpp, so line citations go
// stale within a task or two.
//
// PeChunkContext is passed as data rather than reached for through
// CommandChunkBuilder so a native differential test can drive it.

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "device_c_chunk_schema.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/pe_recorder_lock.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace dxmt9::d3d9::pe {

// Per-render-target "was this slot explicitly set" flags. A bool array rather
// than a bitmask, matching the device's currentRtExplicitMask(). This lived in
// d3d9_pe_draw_packet.hpp until Task 10 deleted that header: it was the only
// declaration there that outlived the fat packet, and it belongs next to the
// PeBindingView field that holds it.
using PeRtExplicitMask =
    std::array<bool, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>;

// Qualified buffer identity plus the device's authoritative offset/stride.
struct PeStreamBinding {
  BufferRef buffer{};
  std::uint32_t offset = 0u;
  std::uint32_t stride = 0u;
};

// (a) COM-derived bindings, already translated by the device. The producer
// forwards `object` to CommandChunkBuilder::appendHandle, which owns
// retention; it never dereferences it.
struct PeBindingView {
  std::array<TextureRef, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<PeStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  // Stream frequency is independent from the source tuple.  In particular,
  // rebinding a stream's buffer/offset/stride must not reset its divider or
  // D3DSTREAMSOURCE flags.
  std::array<std::uint32_t, D9C_DRAW_PACKET_MAX_STREAMS> streamFrequencies = [] {
    std::array<std::uint32_t, D9C_DRAW_PACKET_MAX_STREAMS> values{};
    values.fill(1u);
    return values;
  }();
  ShaderRef vs{};
  ShaderRef ps{};
  DeclarationRef vdecl{};
  BufferRef indexBuffer{};
  SurfaceRef depthStencil{};
  std::array<SurfaceRef, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>
      renderTargets{};
  // Per-slot bool array, NOT a bitmask: this mirrors the device's
  // currentRtExplicitMask(), whose type is PeRtExplicitMask
  // (std::array<bool, 4>), and populateDrawPacketAttachmentSnapshot binds it
  // by const reference.
  PeRtExplicitMask rtExplicitMask{};
  std::uint32_t fvf = 0u;
};

// (d) Destination-chunk history, consumed by addChunkContextSections only.
// Mirrors what d3d9_pe_device.cpp's populatePendingChunkDrawStreamDependencies
// and populatePendingChunkDrawIndexDependency compute today.
//
// submittedIndexBufferWire holds the POINTER-valued wire, matching what the
// device stores in submittedIndexBufferWireValue_ via
// d9cWireHandleValue(toWireHandle(rawIBuf(indexBuf_))) -- not an objectId.
// Comparing the wrong one makes the predicate always differ, so every indexed
// draw re-emits its index binding.
struct PeChunkContext {
  std::uint32_t retainedStreamMask = 0u;
  bool indexBufferKnown = false;
  std::uint64_t submittedIndexBufferWire = 0u;
  // Whether the DESTINATION chunk already references the currently bound index
  // buffer. This is a second, independent reason to emit the index section:
  // populateDrawPacketIndexDependency sets ibValid when the handle is non-null
  // and the chunk has not retained it, regardless of pendingIb / known /
  // changed. Omitting it lets a fresh chunk's first indexed draw with an
  // unchanged IB emit no section, so that chunk never retains the buffer.
  bool indexBufferRetained = false;
};

// Pure compact destination resolvers. They take a typed lazy retention
// callback so production can bind the current semantic owner while native
// tests can use a counting spy. The callback is reached only after the
// source-selected mask, binding presence, and all non-retention index reasons
// have been resolved.
template <typename Retention>
  requires std::is_nothrow_invocable_r_v<
      bool, Retention&, std::uint32_t, const BufferRef&>
std::uint32_t resolveSparseStreamMask(
    std::uint32_t selectedStreamMask, const PeBindingView& bindings,
    Retention&& retained) noexcept {
  std::uint32_t result = selectedStreamMask;
  std::uint32_t candidates =
      (D9C_DRAW_PACKET_MAX_STREAMS >= 32u
           ? 0xffffffffu
           : (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u) &
      ~selectedStreamMask;
  while (candidates != 0u) {
    const auto slot = static_cast<std::uint32_t>(
        std::countr_zero(candidates));
    candidates &= candidates - 1u;
    const auto& stream = bindings.streams[slot];
    if (stream.buffer.object != nullptr && !retained(slot, stream.buffer)) {
      result |= 1u << slot;
    }
  }
  return result;
}

template <typename Retention>
  requires std::is_nothrow_invocable_r_v<bool, Retention&, const BufferRef&>
bool resolveSparseIndexBuffer(bool indexedDraw, bool selectedIndexBuffer,
                              bool indexBufferKnown,
                              std::uint64_t submittedIndexBufferWire,
                              std::uint64_t currentIndexBufferWire,
                              const BufferRef& indexBuffer,
                              Retention&& retained) noexcept {
  if (!indexedDraw) return false;
  if (selectedIndexBuffer || !indexBufferKnown ||
      submittedIndexBufferWire != currentIndexBufferWire) {
    return true;
  }
  return indexBuffer.object != nullptr && !retained(indexBuffer);
}

// (c) Draw payloads. Borrowed for the producer call and the append that
// consumes its output. Never stored.
struct PeDrawPayloads {
  std::span<const std::byte> upIndex{};
  std::span<const std::byte> upVertex{};
};

// Per-draw scalars. APPLY_STATE passes this default-constructed and the
// producer leaves the draw header alone. baseVertex is signed to match
// D9CCommandChunkWireDrawHeader::baseVertex (int32_t); a negative
// BaseVertexIndex is legal in D3D9.
struct PeDrawParams {
  std::uint32_t recordType = 0u;
  std::uint32_t primitiveType = 0u;
  std::int32_t baseVertex = 0;
  std::uint32_t minVertex = 0u;
  std::uint32_t numVertices = 0u;
  std::uint32_t startVertex = 0u;
  std::uint32_t startIndex = 0u;
  std::uint32_t primitiveCount = 0u;
  std::uint32_t stride = 0u;
  std::uint32_t indexFormat = 0u;
};

// Compact pass-1 result for the normal recorder path.  It deliberately owns
// no section-sized wire storage: masks/counts select rows from the borrowed
// source views, constant and UP ranges borrow their call-local bytes, and pass
// 2 writes those rows into the semantic owner's canonical payload. The source
// facts are producer-issued; borrowed payload still requires the recorder
// borrow/ticket and live dependency checks. CommandChunkBuilder remains the
// independent compatibility oracle, not the production destination.
//
// PeSparseScratch remains below as an isolated compatibility/oversized-batch
// owner.  It must not be embedded here or moved to a draw-call stack frame.
struct SparseStatePlan {
  SparseStatePlan() noexcept = default;
  SparseStatePlan(const SparseStatePlan&) = delete;
  SparseStatePlan& operator=(const SparseStatePlan&) = delete;
  SparseStatePlan(SparseStatePlan&&) = delete;
  SparseStatePlan& operator=(SparseStatePlan&&) = delete;

  void invalidate() noexcept {
    // Revoke-only invalidation is deliberately cheap.  It runs on failed
    // partial builds and after settlement; stale value fields are unreachable
    // while prepared is false and are overwritten by the next build entry.
    source_.prepared = false;
    destination_.chunkContextFinalized = false;
    source_.pendingDelta = {};
    source_.pendingTicket = {};
    sourceDomain_.invalidate();
    sourceShadow_ = nullptr;
    sourceConstants_ = nullptr;
    sourceBindings_ = nullptr;
  }

  bool validFor(const RecorderLockCapability& access) const noexcept {
    // Qualify every borrowed source against the fresh authority before
    // consulting PendingDeltaView, which dereferences the borrowed shadow.
    return source_.prepared && source_.pendingTicket.valid() &&
           sourceDomain_.validFor(access) && sourceShadow_ != nullptr &&
           sourceConstants_ != nullptr && sourceBindings_ != nullptr &&
           source_.pendingDelta.valid();
  }

  struct SourceFacts {
    PeDrawParams draw{};
    bool prepared = false;
    bool fullSnapshot = false;
    // Producer-selected record contents, not a cache of mutable Live Dirty.
    bool hasConstantRanges = false;
    PeHotStateShadow::PendingDeltaView pendingDelta{};
    PendingDeltaTicket pendingTicket{};
    std::uint32_t drawFlags = 0u;
    std::uint32_t renderStateCount = 0u;
    std::uint32_t textureStageStateCount = 0u;
    std::uint32_t samplerStateCount = 0u;
    std::uint32_t transformCount = 0u;
    std::uint32_t textureMask = 0u;
    std::uint32_t selectedStreamMask = 0u;
    std::uint32_t renderTargetMask = 0u;
    std::uint32_t clipPlaneMask = 0u;
    std::uint32_t lightMask = 0u;
    std::uint32_t lightEnableMask = 0u;
    std::uint8_t shaderMask = 0u;
    bool vertexInput = false;
    std::uint8_t vertexInputKind = 0u;
    bool selectedIndexBuffer = false;
    bool depthStencil = false;
    bool viewport = false;
    bool scissor = false;
    bool material = false;
    SparseConstantRangeInput vsFloatConstants{};
    SparseConstantRangeInput vsIntConstants{};
    SparseConstantRangeInput vsBoolConstants{};
    SparseConstantRangeInput psFloatConstants{};
    SparseConstantRangeInput psIntConstants{};
    SparseConstantRangeInput psBoolConstants{};
    PeDrawPayloads payloads{};
  };

  // Presence uses existing metadata padding in native and both PE layouts.
  static_assert(offsetof(SourceFacts, hasConstantRanges) == 42u);
  static_assert(offsetof(SourceFacts, pendingDelta) == 48u);
  static_assert(sizeof(SourceFacts) == (sizeof(void*) == 8u ? 304u : 240u));

  struct DestinationFacts {
    std::uint32_t streamMask = 0u;
    bool indexBuffer = false;
    bool chunkContextFinalized = false;
  };

  // A synchronous, capability-qualified view of every source needed by an
  // ordinary compact operation.  It is created only inside
  // withQualifiedSources after the owner/epoch/lane/ticket checks have all
  // passed, and is consumed by the callback before that operation returns.
  // This lets a writer, finalizer, or settlement visitor reuse one lexical
  // qualification instead of rechecking the same borrows for every source.
  class QualifiedSources final {
   public:
    QualifiedSources(const QualifiedSources&) = delete;
    QualifiedSources& operator=(const QualifiedSources&) = delete;
    QualifiedSources(QualifiedSources&&) = delete;
    QualifiedSources& operator=(QualifiedSources&&) = delete;

    const SourceFacts& source;
    const DestinationFacts& destination;
    const PeHotStateShadow& shadow;
    PeConstShadowBlock& constants;
    const PeBindingView& bindings;

   private:
    QualifiedSources(const SourceFacts& source,
                     const DestinationFacts& destination,
                     const PeHotStateShadow& shadow,
                     PeConstShadowBlock& constants,
                     const PeBindingView& bindings) noexcept
        : source(source),
          destination(destination),
          shadow(shadow),
          constants(constants),
          bindings(bindings) {}

    friend struct SparseStatePlan;
  };

  template <typename Fn>
    requires (std::is_nothrow_invocable_v<Fn&&, const QualifiedSources&> &&
              std::is_same_v<std::invoke_result_t<Fn&&, const QualifiedSources&>,
                             void>)
  bool withQualifiedSources(const RecorderLockCapability& access,
                            Fn&& fn) const noexcept {
    // This is the one public-entry qualification for the whole synchronous
    // operation. The private domain and typed pointers below are used only
    // after it has succeeded, and cannot be obtained as unchecked getters.
    if (!validFor(access)) return false;
    const QualifiedSources sources{
        source_, destination_, *sourceShadow_, *sourceConstants_,
        *sourceBindings_};
    std::forward<Fn>(fn)(sources);
    return true;
  }

  template <typename Fn>
    requires (std::is_nothrow_invocable_v<Fn&&, const PeBindingView&> &&
              std::is_same_v<std::invoke_result_t<Fn&&, const PeBindingView&>,
                             void>)
  bool withBindingSource(const RecorderLockCapability& access,
                         Fn&& fn) const noexcept {
    return withQualifiedSources(
        access, [&](const QualifiedSources& sources) noexcept {
          std::forward<Fn>(fn)(sources.bindings);
        });
  }

  template <typename Fn>
    requires (std::is_nothrow_invocable_v<
                  Fn&&, const PeHotStateShadow&, const PeBindingView&> &&
              std::is_same_v<std::invoke_result_t<
                                 Fn&&, const PeHotStateShadow&,
                                 const PeBindingView&>, void>)
  bool withEmitSources(const RecorderLockCapability& access,
                       Fn&& fn) const noexcept {
    return withQualifiedSources(
        access, [&](const QualifiedSources& sources) noexcept {
          std::forward<Fn>(fn)(sources.shadow, sources.bindings);
        });
  }

  template <typename Fn>
    requires (std::is_nothrow_invocable_v<
                  Fn&&, const PeHotStateShadow&, PeConstShadowBlock&,
                  const PeBindingView&> &&
              std::is_same_v<std::invoke_result_t<
                                 Fn&&, const PeHotStateShadow&,
                                 PeConstShadowBlock&, const PeBindingView&>,
                             void>)
  bool withSettlementSources(const RecorderLockCapability& access,
                             Fn&& fn) const noexcept {
    return withQualifiedSources(
        access, [&](const QualifiedSources& sources) noexcept {
          std::forward<Fn>(fn)(sources.shadow, sources.constants,
                               sources.bindings);
        });
  }

  const SourceFacts* source(const RecorderLockCapability& access) const noexcept {
    return validFor(access) ? &source_ : nullptr;
  }

  // These values are copied metadata used by preflight/revoke paths. They do
  // not expose any borrowed spans or PendingDeltaView.
  bool prepared() const noexcept { return source_.prepared; }
  std::uint32_t recordType() const noexcept { return source_.draw.recordType; }
  std::uint32_t drawFlags() const noexcept { return source_.drawFlags; }
  PendingDeltaTicket pendingTicket() const noexcept {
    return source_.pendingTicket;
  }

  const DestinationFacts& destination() const noexcept { return destination_; }

 private:
  void reset() noexcept {
    // Build entry is the only point that needs bounded initialization.  Draw
    // parameters, payloads, counts, and masks are assigned unconditionally
    // by the producer before any read.  Clear only OR-built/optional fields
    // whose absent form must not inherit the previous plan.
    // No caller can consume an unpublished plan. Successful preparation
    // overwrites the domain, three pointers and pending ticket/view; the build
    // guard fully revokes them on every failure. Do not clear and immediately
    // overwrite those capability fields on the successful hot path.
    source_.prepared = false;
    destination_.chunkContextFinalized = false;
    source_.drawFlags = 0u;
    source_.hasConstantRanges = false;
    source_.renderTargetMask = 0u;
    source_.shaderMask = 0u;
    source_.vertexInputKind = 0u;
    source_.vsFloatConstants = {};
    source_.vsIntConstants = {};
    source_.vsBoolConstants = {};
    source_.psFloatConstants = {};
    source_.psIntConstants = {};
    source_.psBoolConstants = {};
  }

  bool bindSources(const RecorderLockCapability& access,
                   const PeHotStateShadow& shadow,
                   PeConstShadowBlock& constants,
                   const PeBindingView& bindings) noexcept {
    if (!access.valid()) {
      sourceDomain_.invalidate();
      sourceShadow_ = nullptr;
      sourceConstants_ = nullptr;
      sourceBindings_ = nullptr;
      return false;
    }
    sourceDomain_.bind(access);
    sourceShadow_ = &shadow;
    sourceConstants_ = &constants;
    sourceBindings_ = &bindings;
    return true;
  }

  SourceFacts source_{};
  DestinationFacts destination_{};

  // One private domain qualifies all three typed source pointers.  Keeping
  // the pointers typed prevents a raw/type-erased sink while ensuring that a
  // partial bind can never leave one source qualified independently.
  RecorderBorrowDomain sourceDomain_{};
  const PeHotStateShadow* sourceShadow_ = nullptr;
  PeConstShadowBlock* sourceConstants_ = nullptr;
  const PeBindingView* sourceBindings_ = nullptr;

  friend bool buildSparseStatePlan(const RecorderLockCapability&,
                                   const PeHotStateShadow&,
                                   PeConstShadowBlock&, const PeBindingView&,
                                   const PeDrawPayloads&, const PeDrawParams&,
                                   bool, bool, SparseStatePlan&) noexcept;
  friend bool finalizeSparseStatePlanChunkContext(
      const RecorderLockCapability&, const PeChunkContext&,
      SparseStatePlan&) noexcept;
  template <typename Retention>
    requires std::is_nothrow_invocable_r_v<
        bool, Retention&, std::uint32_t, const BufferRef&>
  friend bool finalizeSparseStatePlanChunkContext(
      const RecorderLockCapability&, const PeChunkContext&, SparseStatePlan&,
      Retention&&) noexcept;
  template <typename Retention>
    requires std::is_nothrow_invocable_r_v<
        bool, Retention&, std::uint32_t, const BufferRef&>
  friend bool finalizeSparseStatePlanChunkContext(
      const RecorderLockCapability&, const PeChunkContext&, SparseStatePlan&,
      Retention&&, PendingDeltaTicket, PendingDeltaTicket) noexcept;
};

inline std::uint32_t sparseStatePlanSectionCount(
    const SparseStatePlan::SourceFacts& source,
    const SparseStatePlan::DestinationFacts& destination) noexcept {
  std::uint32_t count = 0u;
  const auto add = [&count](bool present) noexcept {
    count += present ? 1u : 0u;
  };
  add(source.renderStateCount != 0u);
  add(source.textureMask != 0u);
  add(destination.streamMask != 0u);
  add(source.shaderMask != 0u);
  add(source.vertexInput);
  add(destination.indexBuffer);
  add(source.renderTargetMask != 0u);
  add(source.depthStencil);
  add(source.viewport);
  add(source.scissor);
  add(source.material);
  add(source.clipPlaneMask != 0u);
  add(source.textureStageStateCount != 0u);
  add(source.samplerStateCount != 0u);
  add(source.transformCount != 0u);
  add(source.lightMask != 0u);
  add(source.lightEnableMask != 0u);
  add(source.vsFloatConstants.present());
  add(source.vsIntConstants.present());
  add(source.vsBoolConstants.present());
  add(source.psFloatConstants.present());
  add(source.psIntConstants.present());
  add(source.psBoolConstants.present());
  add(!source.payloads.upIndex.empty());
  add(!source.payloads.upVertex.empty());
  return count;
}

inline std::size_t sparseStatePlanConstantPayloadBytes(
    const SparseStatePlan::SourceFacts& source) noexcept {
  return source.vsFloatConstants.registerBytes.size() +
         source.vsIntConstants.registerBytes.size() +
         source.vsBoolConstants.registerBytes.size() +
         source.psFloatConstants.registerBytes.size() +
         source.psIntConstants.registerBytes.size() +
         source.psBoolConstants.registerBytes.size();
}

inline D9CCommandChunkWireDrawHeader sparseStatePlanDrawHeader(
    const SparseStatePlan::SourceFacts& source) noexcept {
  D9CCommandChunkWireDrawHeader header{};
  header.flags = source.drawFlags;
  switch (source.draw.recordType) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
      header.primitiveType = source.draw.primitiveType;
      header.startVertex = source.draw.startVertex;
      header.primitiveCount = source.draw.primitiveCount;
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
      header.primitiveType = source.draw.primitiveType;
      header.baseVertex = source.draw.baseVertex;
      header.minVertex = source.draw.minVertex;
      header.numVertices = source.draw.numVertices;
      header.startIndex = source.draw.startIndex;
      header.primitiveCount = source.draw.primitiveCount;
      break;
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
      header.primitiveType = source.draw.primitiveType;
      header.primitiveCount = source.draw.primitiveCount;
      header.stride = source.draw.stride;
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
      header.primitiveType = source.draw.primitiveType;
      header.minVertex = source.draw.minVertex;
      header.numVertices = source.draw.numVertices;
      header.primitiveCount = source.draw.primitiveCount;
      header.stride = source.draw.stride;
      header.indexFormat = source.draw.indexFormat;
      break;
    default:
      break;
  }
  return header;
}

static_assert(!std::is_copy_constructible_v<SparseStatePlan> &&
              !std::is_move_constructible_v<SparseStatePlan> &&
              !std::is_copy_assignable_v<SparseStatePlan> &&
              !std::is_move_assignable_v<SparseStatePlan>,
              "SparseStatePlan borrows must remain call-scope only");

// Device-owned, reused compatibility/oversized storage. SparseStateInput spans
// point into this owner for Render Tape full snapshots, SWVP override packets,
// and oversized APPLY_STATE batches. The normal plan path never writes it.
// The SparseStateInput spans the
// producer fills point into these arrays, so the scratch must outlive the
// append that consumes them. The producer returns false rather than
// truncating.
//
// Every capacity is asserted against the canonical schema's own maxCount below, not
// against a hand-copied D9C_DRAW_PACKET_MAX_* value. Oversizing a category is
// not a harmless slack: appendSparseRecord rejects a span longer than
// maxCount through validSectionCount(), so the draw would be dropped at
// runtime. Undersizing truncates. Both are caught at compile time now.
//
// Single-element arrays are used for the scalar sections (viewport, scissor,
// material, index buffer, depth stencil, vertex input) so every category is
// addressed the same way and SparseStateInput's spans can point at them
// uniformly.
struct PeSparseScratch {
  std::array<D9CCommandChunkWireRenderState,
             D9C_DRAW_PACKET_MAX_RENDER_STATES> renderStates{};
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>,
             D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>,
             D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  // Two shader stages (vertex, pixel).
  std::array<SparseBindingInput<D9CCommandChunkWireShaderBinding>, 2>
      shaders{};
  // ONE vertex input, not two. The section is SectionRuleSingle with
  // maxCount 1: FVF and vertex declaration are the two values of the entry's
  // `kind` field, not two entries. Declaration wins when both are dirty, and
  // `value` carries the FVF either way -- see buildSparseState's vertex-input
  // block in d3d9_pe_producer.cpp.
  std::array<SparseBindingInput<D9CCommandChunkWireVertexInput>, 1>
      vertexInputs{};
  std::array<SparseBindingInput<D9CCommandChunkWireIndexBinding>, 1>
      indexBuffers{};
  std::array<SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>,
             D9C_DRAW_PACKET_MAX_RENDER_TARGETS> renderTargets{};
  std::array<SparseBindingInput<D9CCommandChunkWireDepthStencilBinding>, 1>
      depthStencils{};
  std::array<D9CViewport, 1> viewports{};
  std::array<D9CRect, 1> scissors{};
  std::array<D9CMaterial, 1> materials{};
  // D3D9 exposes six clip planes; the packet carries clipPlanes[6 * 4] floats.
  std::array<D9CCommandChunkWireClipPlane, 6> clipPlanes{};
  std::array<D9CDrawPacketTextureStageState, D9C_DRAW_PACKET_MAX_TSS>
      textureStageStates{};
  std::array<D9CDrawPacketSamplerState, D9C_DRAW_PACKET_MAX_SAMPLER>
      samplerStates{};
  std::array<D9CDrawPacketTransform, D9C_DRAW_PACKET_MAX_TRANSFORMS>
      transforms{};
  std::array<D9CCommandChunkWireLight, D9C_DRAW_PACKET_MAX_LIGHTS> lights{};
  std::array<D9CCommandChunkWireLightEnable, D9C_DRAW_PACKET_MAX_LIGHTS>
      lightEnables{};
};

// Tie every scratch capacity to the canonical schema rather than to a copied macro,
// so a schema change breaks the build here instead of dropping draws at
// runtime. sectionRule is constexpr, so this costs nothing.
namespace detail {

constexpr std::uint32_t sectionMaxCount(std::uint16_t kind) {
  const auto* rule = sectionRule(kind);
  return rule != nullptr ? rule->maxCount : 0u;
}

#define DXMT9_ASSERT_SCRATCH_CAP(member, sectionKind)                        \
  static_assert(std::tuple_size_v<decltype(PeSparseScratch::member)> ==      \
                    sectionMaxCount(sectionKind),                            \
                "PeSparseScratch::" #member                                  \
                " must equal the canonical schema maxCount for " #sectionKind)

DXMT9_ASSERT_SCRATCH_CAP(renderStates, D9C_COMMAND_CHUNK_SECTION_RENDER_STATE);
DXMT9_ASSERT_SCRATCH_CAP(textures, D9C_COMMAND_CHUNK_SECTION_TEXTURE);
DXMT9_ASSERT_SCRATCH_CAP(streams, D9C_COMMAND_CHUNK_SECTION_STREAM);
DXMT9_ASSERT_SCRATCH_CAP(shaders, D9C_COMMAND_CHUNK_SECTION_SHADER);
DXMT9_ASSERT_SCRATCH_CAP(vertexInputs, D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT);
DXMT9_ASSERT_SCRATCH_CAP(indexBuffers, D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER);
DXMT9_ASSERT_SCRATCH_CAP(renderTargets, D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET);
DXMT9_ASSERT_SCRATCH_CAP(depthStencils, D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL);
DXMT9_ASSERT_SCRATCH_CAP(viewports, D9C_COMMAND_CHUNK_SECTION_VIEWPORT);
DXMT9_ASSERT_SCRATCH_CAP(scissors, D9C_COMMAND_CHUNK_SECTION_SCISSOR);
DXMT9_ASSERT_SCRATCH_CAP(materials, D9C_COMMAND_CHUNK_SECTION_MATERIAL);
DXMT9_ASSERT_SCRATCH_CAP(clipPlanes, D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE);
DXMT9_ASSERT_SCRATCH_CAP(textureStageStates,
                         D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE);
DXMT9_ASSERT_SCRATCH_CAP(samplerStates, D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE);
DXMT9_ASSERT_SCRATCH_CAP(transforms, D9C_COMMAND_CHUNK_SECTION_TRANSFORM);
DXMT9_ASSERT_SCRATCH_CAP(lights, D9C_COMMAND_CHUNK_SECTION_LIGHT);
DXMT9_ASSERT_SCRATCH_CAP(lightEnables, D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE);

#undef DXMT9_ASSERT_SCRATCH_CAP

// PeBindingView::textures indexes the PE shadow's texture slots, so the two
// must agree; today both are 20 but nothing else ties them together.
static_assert(std::tuple_size_v<decltype(PeBindingView::textures)> ==
                  kPeTextureSlots,
              "PeBindingView::textures must cover every PE texture slot");

}  // namespace detail

}  // namespace dxmt9::d3d9::pe
