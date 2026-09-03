#pragma once

// Compatibility-lane draw-run island batching (R-BACK-2.102 companion).
//
// The Direct final-slot lane constructs a whole lease span into one queue-owned
// `ChunkSlot`. Everything the emission plan does NOT hand to a Direct lease --
// a whole raw whose plan is not `rotationFreeProductionEligible`, an exact
// `CompatibilityRange`, an ordered-control or compatibility separator span, and
// every raw at all when `DXMT9_DIRECT_CHUNK_SLOT_REPLAY=0` or
// `DXMT_TRACE_RENDER` is set -- replays through the ordinary sink. That sink
// used to call the queue once per draw: one `CommandQueue::mutex_` acquisition,
// one `ensureWritingSlot`, one command header, one `DrawRunRecord` and one
// `FlatDrawStateRecord` copy for every single D3D9 draw record.
//
// This header owns the one decision that lane needs: may the incoming draw
// extend the run the sink is already holding, or must that run be published
// first? It is pure, allocation-free and value-only so an exhaustive native
// truth table can pin it without a queue, a device, Metal or Wine.
//
// The rule is deliberately conservative and *content* based rather than
// distance based. Two draws share a run only when every input the run record
// folds into one -- the canonical draw state, the uniform payload and the
// per-draw binding override -- is bit-identical. So `A -> B -> A` retains three
// runs in one source-local batch, never one or two: the second `A` compares
// against the pending `B`, not against a history.
//
// It is NOT a carrier: an admitted extension appends a `DrawParam` and a
// `DrawParamPayloadView` (spans into already-immutable storage) and nothing
// else. No `FlatDrawStateRecord`, `DrawShaderLayoutContext` or
// `DrawUniformPayload` is copied per draw, which is what R-BACK-2.100's
// materialization floor forbids.

#include "dxmt9/core_constants.hpp"

#include <array>
#include <cstdint>

namespace dxmt9::core {

// Number of independent draw-state cache generations the binding-agnostic
// submission-batch snapshot is keyed on. Keeping them in one array rather than
// nine named fields makes the identity trivially comparable and keeps this
// header free of any dependency on the core Device layout.
inline constexpr std::size_t kCompatibilityDrawBatchGenerationCount = 9;

// Ordinal positions inside CompatibilityDrawBatchIdentity::generations. The
// producer fills every slot; the names exist so a caller (and a spec) cannot
// silently transpose two of them.
enum class CompatibilityDrawBatchGeneration : std::uint8_t {
  StableState = 0,
  ShaderLayout,
  Uniform,
  VertexShaderConstant,
  PixelShaderConstant,
  UniformNonConstant,
  RenderStateFlat,
  TextureStageStateFlat,
  SamplerStateFlat,
};

static_assert(static_cast<std::size_t>(
                  CompatibilityDrawBatchGeneration::SamplerStateFlat) + 1u ==
                  kCompatibilityDrawBatchGenerationCount,
              "generation ordinal alphabet drifted from the array bound");

// Everything a batched `submitDrawRun` collapses into ONE record. Two draws may
// share a run only when these compare equal.
//
// The generations answer "would the binding-agnostic draw-state cache be
// rebuilt?" -- if none moved, the cached `hot` / `shaderLayout` / `uniforms`
// the pending run already captured are still byte-identical, so the run's
// single canonical state and single uniform payload remain exact for the
// incoming draw too.
//
// The generations alone are NOT sufficient. The binding-agnostic cache
// deliberately ignores stream/index bindings (`clearDrawStateBindingFields`),
// so a `SetStreamSource` between two draws can leave every generation still
// and yet change the per-draw binding override that the run serializes. The
// binding fields below are therefore compared in full, and the raw stream
// strides are carried separately because `refreshShaderLayoutExtraStreamStrides`
// reads `streamStrides[1..kMaxStreams)` for streams that carry no buffer at all.
struct CompatibilityDrawBatchIdentity {
  std::array<std::uint64_t, kCompatibilityDrawBatchGenerationCount>
      generations{};
  std::array<std::uint64_t, kMaxStreams> streamBuffers{};
  std::array<std::uint32_t, kMaxStreams> streamOffsets{};
  std::array<std::uint32_t, kMaxStreams> streamStrides{};
  std::uint32_t streamMask = 0;
  std::uint64_t indexBuffer = 0;
  std::uint32_t indexType = 0;
  std::uint32_t alphaTestEnable = 0;
  std::uint32_t alphaTestFunc = 0;
  std::uint32_t alphaTestRef = 0;
  bool indexed = false;
  bool indexBufferValid = false;
  bool alphaTestStateValid = false;

  friend constexpr bool operator==(const CompatibilityDrawBatchIdentity&,
                                   const CompatibilityDrawBatchIdentity&) =
      default;
};

// What the sink must do with the incoming draw, before it appends anything.
enum class CompatibilityDrawBatchAdmission : std::uint8_t {
  // No run is open; open one with this draw.
  Start = 0,
  // The open run folds this draw exactly; append and publish nothing.
  Extend,
  // The open run cannot express this draw. Close it, then open a new run in
  // the same source-local batch (publication happens at the batch boundary).
  FlushAndStart,
  // This draw is not expressible as a batched run at all (UP data, a
  // TriangleFan, a state-block recording, render tracing). Publish any open
  // run, then submit this draw on its own.
  Unbatchable,
  Count,
};

// Why an open run had to be published. Mutually exclusive, and reported in
// this precedence order so an observed cut is never "unclassified".
enum class CompatibilityDrawBatchCut : std::uint8_t {
  // Nothing was published.
  None = 0,
  // The draw itself cannot ride a batched run.
  Unbatchable,
  // Canonical state, uniforms or bindings differ from the open run.
  Identity,
  // The open run already holds the maximum number of draws.
  Capacity,
  Count,
};

struct CompatibilityDrawBatchDecision {
  CompatibilityDrawBatchAdmission admission =
      CompatibilityDrawBatchAdmission::Start;
  CompatibilityDrawBatchCut cut = CompatibilityDrawBatchCut::None;

  friend constexpr bool operator==(const CompatibilityDrawBatchDecision&,
                                   const CompatibilityDrawBatchDecision&) =
      default;
};

// --------------------------------------------------------------------------
// Record cut classification.
//
// The island is cut by the *record walk*, not by the draw path, for everything
// that is not a draw. The first version of this lane cut before every record
// that was not a batchable draw, which is safe but vacuous in production: the
// PE producer emits standalone constant records between draws by default
// (`DXMT9_PE_INLINE_CONST_DELTA` is off), so an island could never hold more
// than one draw on any real workload.
//
// The rule below is the narrowest classification that is still provable:
//
// * A record may run inside an open island only when it is PURE PROJECTION --
//   it mutates device state and touches nothing else. `ImportedRecordReplayInfo`
//   already carries exactly that shape, and only `ConstantUpload` has it
//   (`mutatesDeviceState`, `referencesResources == false`, `barrier == false`).
//   A constant write names no resource, so it cannot drop the last strong
//   reference to anything the pending run's canonical state points at, and it
//   never writes the binding-agnostic draw-state cache -- it only bumps
//   `drawUniformGeneration_` and the per-stage constant generation. The pending
//   run therefore keeps borrowing byte-identical cache content, and the NEXT
//   draw's generation/identity comparison decides whether the island survives.
//   When the constant bytes are unchanged the core setter early-outs, no
//   generation moves, and the island legitimately continues.
//
// * `StateApply` is deliberately NOT admitted even though it is "state only"
//   for effect-cut purposes. It carries `referencesResources`, so replaying it
//   can release the last reference to a texture / stream / index buffer / render
//   target that the *pending* run still names, and queue-side resource marking
//   (`pool_.markDrawResources`) does not happen until the run is published.
//   Its setters also do not early-out on unchanged values, so admitting it
//   would buy nothing: any real APPLY_STATE bumps a generation and the next
//   draw cuts the island anyway.
//
// * Everything else -- coordinators (Clear, Present), surface ops, query issue,
//   readback, UP and TriangleFan draws, records replayed while a device state
//   block is recording, and any unknown record type -- is an observable effect
//   and always cuts. The classification is fail-closed: a record with no known
//   shape lands in `ObservableEffect`.
enum class CompatibilityDrawBatchRecordClass : std::uint8_t {
  // A draw that may open, extend or cut the island; the draw path decides.
  BatchableDraw = 0,
  // Pure device-state projection: may run inside an open island.
  PureProjection,
  // Must be preceded by publication of any open island.
  ObservableEffect,
  Count,
};

// The record facts the classification reads. Every field is a projection of
// `dxmt9::d3d9::devicec::ImportedRecordReplayInfo` plus the two device-level
// facts the record view alone cannot supply.
struct CompatibilityDrawBatchRecordFacts {
  // `recordCanBatchDraw`: a DRAW/DRAW_INDEXED record that is not a TriangleFan
  // and is not being recorded into a device state block.
  bool batchableDraw = false;
  bool mutatesDeviceState = false;
  bool referencesResources = false;
  bool barrier = false;
  // A device state block is recording, so nothing this record does reaches the
  // device state the island borrows. Cut anyway: a state block is an observable
  // device transaction and the island must never straddle one.
  bool stateBlockRecording = false;

  friend constexpr bool operator==(const CompatibilityDrawBatchRecordFacts&,
                                   const CompatibilityDrawBatchRecordFacts&) =
      default;
};

constexpr CompatibilityDrawBatchRecordClass
classifyCompatibilityDrawBatchRecord(
    const CompatibilityDrawBatchRecordFacts& facts) noexcept {
  if (facts.stateBlockRecording) {
    return CompatibilityDrawBatchRecordClass::ObservableEffect;
  }
  if (facts.batchableDraw) {
    return CompatibilityDrawBatchRecordClass::BatchableDraw;
  }
  if (facts.mutatesDeviceState && !facts.referencesResources &&
      !facts.barrier) {
    return CompatibilityDrawBatchRecordClass::PureProjection;
  }
  return CompatibilityDrawBatchRecordClass::ObservableEffect;
}

// The one question the record walk asks.
constexpr bool compatibilityDrawBatchRecordCutsIsland(
    CompatibilityDrawBatchRecordClass recordClass) noexcept {
  return recordClass == CompatibilityDrawBatchRecordClass::ObservableEffect;
}

// What a publication attempt did. `Failed` is never reported as success: the
// caller that owns the replay boundary turns it into a replay failure.
enum class CompatibilityDrawBatchFlushStatus : std::uint8_t {
  // Nothing was open (or the call re-entered an in-progress publish).
  Empty = 0,
  // The open island reached the queue exactly once.
  Published,
  // The publish threw. The island is retired and the device is poisoned.
  Failed,
  Count,
};

// Bound on one source-local batch. This is not a queue reservation:
// `ChunkSlot::appendDrawRun` reserves each run's span in one call, so the cap
// bounds retained DOD scratch and the number of state snapshots waiting for one
// queue transaction. It is a constant rather than a policy knob.
inline constexpr std::uint32_t kCompatibilityDrawBatchMaxDraws = 256;

// Pure, total, allocation-free.
//
// `openDraws == 0` means no run is open, in which case `open` must be ignored.
// A caller that passes `openDraws != 0` guarantees it is holding exactly that
// many draws whose folded identity is `open`.
constexpr CompatibilityDrawBatchDecision compatibilityDrawBatchAdmission(
    bool batchable, std::uint32_t openDraws,
    const CompatibilityDrawBatchIdentity& open,
    const CompatibilityDrawBatchIdentity& incoming,
    std::uint32_t maxDraws = kCompatibilityDrawBatchMaxDraws) noexcept {
  if (!batchable) {
    return {CompatibilityDrawBatchAdmission::Unbatchable,
            openDraws != 0u ? CompatibilityDrawBatchCut::Unbatchable
                            : CompatibilityDrawBatchCut::None};
  }
  if (openDraws == 0u) {
    return {CompatibilityDrawBatchAdmission::Start,
            CompatibilityDrawBatchCut::None};
  }
  if (!(open == incoming)) {
    return {CompatibilityDrawBatchAdmission::FlushAndStart,
            CompatibilityDrawBatchCut::Identity};
  }
  // A zero or one draw ceiling degenerates to the pre-batching lane rather
  // than to an unbounded run, so the cap can never be read as "no cap".
  if (maxDraws == 0u || openDraws >= maxDraws) {
    return {CompatibilityDrawBatchAdmission::FlushAndStart,
            CompatibilityDrawBatchCut::Capacity};
  }
  return {CompatibilityDrawBatchAdmission::Extend,
          CompatibilityDrawBatchCut::None};
}

}  // namespace dxmt9::core
