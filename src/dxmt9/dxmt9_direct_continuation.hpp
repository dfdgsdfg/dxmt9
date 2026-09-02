#pragma once

#include "dxmt9_chunk_slot_capacity.hpp"
#include "dxmt9_source_payload.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace dxmt9::core {

// A populated compatibility ChunkSlot may be extended only by a plan whose
// final representation is already completely reserved.  Keep the result
// typed and value-only so the production queue and native truth-table tests
// cannot grow separate admission policies.
enum class DirectContinuationAdmission : std::uint8_t {
  Admitted,
  CapacityRejected,
  StructuralRejected,
};

struct DirectContinuationAdmissionResult {
  DirectContinuationAdmission disposition =
      DirectContinuationAdmission::StructuralRejected;

  constexpr bool admitted() const noexcept {
    return disposition == DirectContinuationAdmission::Admitted;
  }
  constexpr bool capacityRejected() const noexcept {
    return disposition == DirectContinuationAdmission::CapacityRejected;
  }
  constexpr bool structuralRejected() const noexcept {
    return disposition == DirectContinuationAdmission::StructuralRejected;
  }
};

// ---------------------------------------------------------------------------
// Empty-slot storage provisioning (R-BACK-2.104).
//
// The per-transaction reservation below (`SourcePayloadCapacity extra`) is a
// *semantic* contract: it is the exact plan the assembler is allowed to append
// and prepare-time conservation checks against. The final slot's *physical*
// vector capacity is a separate, purely allocational property. Before this
// requirement existed the two were the same number -- the assembler reserved
// `size() + extra` exactly -- so every populated slot satisfied
// `size() == capacity()` and the capacity arm below was false by construction
// for every following source. Each adjacent source then had to publish the
// slot and take a fresh one, and an allocator decision became a Metal
// command-buffer and render-pass boundary.
//
// Provisioning fixes that at the only address-safe moment: while the slot is
// empty in every direct dimension, so no published prefix is reallocated,
// rehashed or copied. A populated slot is still never grown.
//
// The draw dimension is provisioned to a **budget-fixed ceiling**, not to a
// multiple of whichever span happened to arrive first. A first-span-proportional
// rule leaves the regression live for two whole populations: a short leading
// span (a Clear-bearing or few-draw span at a frame boundary) under-provisions
// the slot for every source after it, and a large leading span reintroduces
// exact fit. The ceiling is the same priced, bounded number in both cases, so
// the slot's storage does not depend on where the producer happened to cut.
// The non-draw coordinator dimensions stay proportional to the source's own
// coordinator use, so a dimension the workload does not touch stays sparse.
//
// A source whose own exact plan exceeds the ceiling is still reserved exactly
// and is a *genuine* budget rotation afterwards, not a provisioning failure.
struct DirectSlotProvisionBudget {
  static constexpr std::size_t kCandidateMaxBytes = 48u * 1024u * 1024u;
  // Multiplier for the NON-draw coordinator dimensions only. It is deliberately
  // not the draw rule: coordinator rows are cheap (ClearDesc and
  // PresentCommandRecord are hundreds of bytes, not tens of kilobytes) and are
  // genuinely proportional to what the source uses. Small floors also reserve
  // bounded room in untouched families so a Clear- or Present-bearing adjacent
  // source does not rotate on a coordinator dimension alone. Draws are
  // budget-fixed instead; see
  // `directSlotProvisionDrawBudget`.
  std::size_t coordinatorScale = 16;
  // Draw-equivalent ceiling. Keeps every derived count far inside u32 and
  // covers a whole measured GT2 present (1,678 draws) with margin.
  std::size_t maxDraws = 2048;
  // Retained-byte ceiling for ONE slot. Bounds the whole provisioning plan --
  // draws, per-draw payload bytes, coordinator rows and command headers -- and
  // not merely the draw-record dimensions; see
  // `directSlotProvisionRetainedBytes`, which is the function the ceiling is
  // derived against.
  //
  // This is a per-payload number and queueCompatibility owns
  // `2 * kCommandChunkCount` persistent payloads, so the worst-case aggregate
  // retention is that many times larger. That
  // figure is stated, not waved away, in
  // `specs/backend/encode-scheduling/{spec,gap}.md`; it is the reason this
  // mechanism is bounded and explicitly opt-in rather than free.
  //
  // Provisioning is fail-closed by default. A positive
  // `DXMT9_DIRECT_SLOT_HEADROOM_BYTES` plus an explicit positive aggregate
  // ceiling select the experimental candidate; `0` retains the byte-identical
  // exact-fit lane. The candidate
  // ceiling remains named here so pure algebra/model tests do not smuggle a
  // production default through value initialization.
  std::size_t maxBytes = 0;
  // A populated slot reserves no coordinator row at all, so a Clear- or
  // Present-bearing adjacent source rotates today even when every draw
  // dimension fits. These floors are cheap: ClearDesc and PresentCommandRecord
  // together are a few kilobytes per slot.
  std::size_t coordinatorFloor = 4;
  std::size_t presentFloor = 1;

  constexpr bool enabled() const noexcept {
    return coordinatorScale != 0 && maxDraws != 0 && maxBytes != 0;
  }
};

inline constexpr DirectSlotProvisionBudget kDirectSlotCandidateBudget = [] {
  DirectSlotProvisionBudget budget{};
  budget.maxBytes = DirectSlotProvisionBudget::kCandidateMaxBytes;
  return budget;
}();

// Worst-case retained final-slot bytes for one direct draw, priced from the
// real record sizes rather than guessed. Every slot vector the provisioning
// plan reserves per draw appears here.
//
// The uniform lookup tables are the one non-linear term.
// `chunkSlotUniformLookupBucketCount` rounds `2 * records` UP to a power of
// two, so the worst case is just under FOUR buckets per record, not two, across
// three families and two tables each (3 x 2 x 4 x u32), plus one `next` entry
// per record per family (3 x u32). Pricing those at two buckets would make the
// byte ceiling below unsound, because `directSlotProvisionRetainedBytes` counts
// what is actually reserved.
//
// `FlatDrawStateRecord`, the two full constant snapshots,
// `DrawUniformFixedPayloadRecord` and `DrawShaderLayoutContext` are ~97% of the
// total; shrinking those is what would let the budget rise without cost. See
// `specs/backend/encode-scheduling/gap.md`.
inline constexpr std::size_t kDirectSlotWorstCaseBytesPerDraw =
    sizeof(MetalCommandHeader) + sizeof(FlatDrawStateRecord) +
    sizeof(DrawShaderLayoutContext) + sizeof(DrawDebugSnapshot) +
    sizeof(DrawPsoSubview) + sizeof(DrawUniformFixedPayloadRecord) +
    sizeof(DrawUniformVertexConstantsRecord) + sizeof(VertexShaderConstants) +
    sizeof(DrawUniformPixelConstantsRecord) + sizeof(PixelShaderConstants) +
    sizeof(DrawUniformPayloadRecord) + sizeof(DrawParam) +
    sizeof(DrawRunCommandRecord) +
    3u * sizeof(std::uint32_t) +
    3u * 2u * 4u * sizeof(std::uint32_t);

// Derived (non-additive) per-draw dimensions. The uniform lookup bucket count
// is a non-linear function of the draw count, so it must be derived once from
// a total and never summed. Shared so the producer plan, the provisioning
// budget and the admission predicate cannot drift apart.
inline constexpr void applyDerivedDrawCapacity(
    SourcePayloadCapacity& capacity, std::size_t drawCount) noexcept {
  capacity.drawUniformVertexConstantBytes =
      drawCount * sizeof(VertexShaderConstants);
  capacity.drawUniformPixelConstantBytes =
      drawCount * sizeof(PixelShaderConstants);
  const auto buckets = detail::chunkSlotUniformLookupBucketCount(drawCount);
  capacity.drawUniformPayloadLookupHeads = buckets;
  capacity.drawUniformPayloadLookupTails = buckets;
  capacity.drawUniformPayloadLookupNext = drawCount;
  capacity.drawUniformVertexConstantsLookupHeads = buckets;
  capacity.drawUniformVertexConstantsLookupTails = buckets;
  capacity.drawUniformVertexConstantsLookupNext = drawCount;
  capacity.drawUniformPixelConstantsLookupHeads = buckets;
  capacity.drawUniformPixelConstantsLookupTails = buckets;
  capacity.drawUniformPixelConstantsLookupNext = drawCount;
}

inline constexpr std::size_t directSlotCeilDiv(std::size_t numerator,
                                               std::size_t denominator)
    noexcept {
  return denominator == 0
      ? 0u
      : numerator / denominator + (numerator % denominator != 0 ? 1u : 0u);
}

inline constexpr std::size_t directSlotSaturatingMul(
    std::size_t value, std::size_t factor) noexcept {
  if (value == 0 || factor == 0) {
    return 0;
  }
  return value > std::numeric_limits<std::size_t>::max() / factor
      ? std::numeric_limits<std::size_t>::max()
      : value * factor;
}

// One coordinator dimension: proportional to the source's own use, then lifted
// to its floor. Independent of the draw budget by construction, so a tiny
// first span cannot inflate a coordinator dimension by the draw ratio.
inline constexpr std::size_t directSlotProvisionCoordinator(
    std::size_t value, std::size_t scale, std::size_t floorValue) noexcept {
  const auto scaled = directSlotSaturatingMul(value, scale);
  return scaled < floorValue ? floorValue : scaled;
}

// The coordinator half of a provisioning plan. Computed first and separately
// because it does not depend on the draw budget, which lets the byte ceiling
// charge for it before deciding how many draws are affordable.
struct DirectSlotProvisionCoordinatorPlan {
  std::size_t clearRecords = 0;
  std::size_t clearRects = 0;
  std::size_t surfaceCopyRecords = 0;
  std::size_t stretchRectRecords = 0;
  std::size_t colorFillRecords = 0;
  std::size_t depthResolveRecords = 0;
  std::size_t generateMipmapsRecords = 0;
  std::size_t presentRecords = 0;

  constexpr std::size_t records() const noexcept {
    return clearRecords + surfaceCopyRecords + stretchRectRecords +
        colorFillRecords + depthResolveRecords + generateMipmapsRecords +
        presentRecords;
  }
};

inline constexpr DirectSlotProvisionCoordinatorPlan
directSlotProvisionCoordinatorPlan(
    const SourcePayloadCapacity& extra,
    const DirectSlotProvisionBudget& budget) noexcept {
  const auto scale = budget.enabled() ? budget.coordinatorScale : std::size_t{1};
  const auto recordFloor =
      budget.enabled() ? budget.coordinatorFloor : std::size_t{0};
  const auto presentFloor =
      budget.enabled() ? budget.presentFloor : std::size_t{0};
  return DirectSlotProvisionCoordinatorPlan{
      .clearRecords =
          directSlotProvisionCoordinator(extra.clearRecords, scale, recordFloor),
      .clearRects = directSlotProvisionCoordinator(extra.clearRects, scale, 0),
      .surfaceCopyRecords = directSlotProvisionCoordinator(
          extra.surfaceCopyRecords, scale, recordFloor),
      .stretchRectRecords = directSlotProvisionCoordinator(
          extra.stretchRectRecords, scale, recordFloor),
      .colorFillRecords = directSlotProvisionCoordinator(
          extra.colorFillRecords, scale, recordFloor),
      .depthResolveRecords = directSlotProvisionCoordinator(
          extra.depthResolveRecords, scale, recordFloor),
      .generateMipmapsRecords = directSlotProvisionCoordinator(
          extra.generateMipmapsRecords, scale, recordFloor),
      .presentRecords = directSlotProvisionCoordinator(
          extra.presentRecords, scale, presentFloor),
  };
}

// Retained bytes the coordinator half commits the slot to, including the
// command headers those rows will occupy. `clearRects` is deliberately absent:
// it is an arena-layout dimension with no reserved ChunkSlot vector, and this
// function prices exactly what `provisionEmptyDirectSlotUnlocked` reserves.
inline constexpr std::size_t directSlotProvisionCoordinatorBytes(
    const DirectSlotProvisionCoordinatorPlan& plan) noexcept {
  return plan.clearRecords * sizeof(ClearDesc) +
      plan.surfaceCopyRecords * sizeof(SurfaceCopyDesc) +
      plan.stretchRectRecords * sizeof(StretchRectDesc) +
      plan.colorFillRecords * sizeof(ColorFillDesc) +
      plan.depthResolveRecords * sizeof(DepthResolveDesc) +
      plan.generateMipmapsRecords * sizeof(GenerateMipmapsDesc) +
      plan.presentRecords * sizeof(PresentCommandRecord) +
      plan.records() * sizeof(MetalCommandHeader);
}

// Smallest override `DXMT9_DIRECT_SLOT_HEADROOM_BYTES` may express while still
// meaning "provisioning is on". The coordinator floors are part of the slot's
// retained shape, so pricing only two draws would leave room for at most one
// after those fixed bytes were subtracted. Include the exact default floor
// price and two draws: this is the smallest enabled budget that can admit a
// second adjacent one-draw source, the property that distinguishes
// provisioning from exact fit.
inline constexpr std::size_t kDirectSlotMinHeadroomBytes =
    directSlotProvisionCoordinatorBytes(directSlotProvisionCoordinatorPlan(
        SourcePayloadCapacity{}, kDirectSlotCandidateBudget)) +
    2u * kDirectSlotWorstCaseBytesPerDraw;

// Worst-case retained final-slot bytes a provisioning plan commits ONE slot to.
// Complete over every vector `provisionEmptyDirectSlotUnlocked` reserves:
// headers, all one-per-draw record dimensions, both constant-byte regions, the
// draw-payload arena, the three uniform lookup triples, and every coordinator
// row. The ceiling below is derived against this function, and the native
// budget regression asserts the bound with a NON-ZERO per-draw payload, so the
// bound is checked rather than merely stated.
//
// This is one payload. Queue compatibility owns two payloads per control.
inline constexpr std::size_t directSlotProvisionRetainedBytes(
    const SourcePayloadCapacity& plan) noexcept {
  return plan.commandHeaders * sizeof(MetalCommandHeader) +
      plan.drawHotStates * sizeof(FlatDrawStateRecord) +
      plan.drawShaderLayouts * sizeof(DrawShaderLayoutContext) +
      plan.drawDebugSnapshots * sizeof(DrawDebugSnapshot) +
      plan.drawPsoSubviews * sizeof(DrawPsoSubview) +
      plan.drawUniformFixedPayloads * sizeof(DrawUniformFixedPayloadRecord) +
      plan.drawUniformVertexConstants *
          sizeof(DrawUniformVertexConstantsRecord) +
      plan.drawUniformVertexConstantBytes +
      plan.drawUniformPixelConstants * sizeof(DrawUniformPixelConstantsRecord) +
      plan.drawUniformPixelConstantBytes +
      plan.drawUniformPayloads * sizeof(DrawUniformPayloadRecord) +
      plan.drawParams * sizeof(DrawParam) + plan.drawPayloadBytes +
      plan.drawRunRecords * sizeof(DrawRunCommandRecord) +
      (plan.drawUniformPayloadLookupHeads +
       plan.drawUniformPayloadLookupTails +
       plan.drawUniformPayloadLookupNext +
       plan.drawUniformVertexConstantsLookupHeads +
       plan.drawUniformVertexConstantsLookupTails +
       plan.drawUniformVertexConstantsLookupNext +
       plan.drawUniformPixelConstantsLookupHeads +
       plan.drawUniformPixelConstantsLookupTails +
       plan.drawUniformPixelConstantsLookupNext) *
          sizeof(std::uint32_t) +
      plan.clearRecords * sizeof(ClearDesc) +
      plan.surfaceCopyRecords * sizeof(SurfaceCopyDesc) +
      plan.stretchRectRecords * sizeof(StretchRectDesc) +
      plan.colorFillRecords * sizeof(ColorFillDesc) +
      plan.depthResolveRecords * sizeof(DepthResolveDesc) +
      plan.generateMipmapsRecords * sizeof(GenerateMipmapsDesc) +
      plan.presentRecords * sizeof(PresentCommandRecord);
}

// Retained bytes of the storage a physical compatibility payload actually
// owns. Unlike the logical plan price above, this reads vector capacities so
// aggregate lease credit follows the persistent CpuReadyTape payload across
// source/control reuse and exact-fit growth.
inline std::size_t directSlotPhysicalRetainedBytes(
    const ChunkSlot& slot) noexcept {
  return chunkSlotPhysicalRetainedBytes(slot);
}

// Per-draw payload arena bytes this source implies. Rounded UP so provisioning
// can never under-reserve the source that priced it.
inline constexpr std::size_t directSlotProvisionPayloadBytesPerDraw(
    const SourcePayloadCapacity& extra) noexcept {
  return directSlotCeilDiv(extra.drawPayloadBytes, extra.drawParams);
}

// The budget-fixed draw ceiling for a slot whose first exact span is `extra`.
//
//   perDraw  = kDirectSlotWorstCaseBytesPerDraw
//            + ceil(extra.drawPayloadBytes / extra.drawParams)
//   fixed    = coordinator record bytes + their command headers
//   ceiling  = min(maxDraws, (maxBytes - fixed) / perDraw)
//
// so `directSlotProvisionRetainedBytes(plan) <= maxBytes` holds by
// construction whenever the ceiling is what binds. Folding the payload rate
// into `perDraw` is what makes a payload-carrying source unable to push the
// plan past the declared ceiling: it is charged for its own payload before the
// draw count is chosen, rather than scaled up afterwards.
//
// Returns 0 when the coordinator half alone already exhausts the budget, in
// which case the caller reserves exactly (never under-reserve).
inline constexpr std::size_t directSlotProvisionDrawCeiling(
    const SourcePayloadCapacity& extra,
    const DirectSlotProvisionBudget& budget) noexcept {
  if (!budget.enabled()) {
    return 0;
  }
  const auto fixedBytes = directSlotProvisionCoordinatorBytes(
      directSlotProvisionCoordinatorPlan(extra, budget));
  if (fixedBytes >= budget.maxBytes) {
    return 0;
  }
  const auto perDraw = kDirectSlotWorstCaseBytesPerDraw +
      directSlotProvisionPayloadBytesPerDraw(extra);
  const auto byBytes = (budget.maxBytes - fixedBytes) / perDraw;
  return byBytes < budget.maxDraws ? byBytes : budget.maxDraws;
}

// Provisioned draw total. Budget-fixed: every in-budget first span that draws
// at all provisions the same ceiling, so slot storage does not depend on where
// the producer happened to cut the first span. A source at or beyond the
// ceiling is its own budget and is reserved exactly -- provisioning never
// under-reserves.
//
// A coordinator-only span (`drawParams == 0`) provisions ZERO draws. It is
// reachable in production -- a span may carry locators and no draw -- and
// committing the full draw ceiling on the strength of a span that does not draw
// would retain tens of megabytes for a slot whose demand is unknown. Such a
// span still gets the coordinator floors, so the arm this requirement fixes
// (a Clear- or Present-bearing adjacent source rotating on a coordinator
// dimension alone) is still covered; a following draw-bearing source rotates
// once and then provisions properly against its own plan. That single rotation
// is the conservative choice, and it is recorded as such in
// `specs/backend/encode-scheduling/gap.md`.
inline constexpr std::size_t directSlotProvisionDrawBudget(
    const SourcePayloadCapacity& extra,
    const DirectSlotProvisionBudget& budget) noexcept {
  const auto exact = extra.drawParams;
  if (!budget.enabled() || exact == 0) {
    return exact;
  }
  const auto ceiling = directSlotProvisionDrawCeiling(extra, budget);
  return exact >= ceiling ? exact : ceiling;
}

// True when the source's own exact plan is at or beyond the provisioning
// ceiling, so the slot is reserved exactly and the next source is a *genuine*
// budget rotation under R-BACK-2.103 clause (3). A coordinator-only span
// (`drawParams == 0`) is never this: it provisions zero draw storage, and
// reporting it as at-budget would inflate the residual with spans that do not
// draw at all.
inline constexpr bool directSlotSourceExceedsProvisionBudget(
    const SourcePayloadCapacity& extra,
    const DirectSlotProvisionBudget& budget) noexcept {
  return budget.enabled() && extra.drawParams != 0 &&
      extra.drawParams >= directSlotProvisionDrawCeiling(extra, budget);
}

// The provisioning target for an empty slot, expressed in the same value type
// as an exact plan: budget-fixed draws, proportional coordinator rows with
// floors, derived dimensions from the combined draw total.
inline constexpr SourcePayloadCapacity directSlotProvisionPlan(
    const SourcePayloadCapacity& extra,
    const DirectSlotProvisionBudget& budget) noexcept {
  const auto draws = directSlotProvisionDrawBudget(extra, budget);
  const auto coordinator = directSlotProvisionCoordinatorPlan(extra, budget);
  SourcePayloadCapacity plan{};
  plan.drawHotStates = draws;
  plan.drawShaderLayouts = draws;
  plan.drawDebugSnapshots = draws;
  plan.drawPsoSubviews = draws;
  plan.drawUniformFixedPayloads = draws;
  plan.drawUniformVertexConstants = draws;
  plan.drawUniformPixelConstants = draws;
  plan.drawUniformPayloads = draws;
  plan.drawParams = draws;
  plan.drawRunRecords = draws;
  // Exactly the rate the ceiling was priced against, so the reserved arena can
  // never exceed what the byte budget already charged for. When the source owns
  // its slot the reservation is its own byte count, unrounded.
  plan.drawPayloadBytes = (draws == extra.drawParams)
      ? extra.drawPayloadBytes
      : directSlotSaturatingMul(directSlotProvisionPayloadBytesPerDraw(extra),
                                draws);
  if (plan.drawPayloadBytes < extra.drawPayloadBytes) {
    plan.drawPayloadBytes = extra.drawPayloadBytes;
  }
  plan.clearRecords = coordinator.clearRecords;
  plan.clearRects = coordinator.clearRects;
  plan.surfaceCopyRecords = coordinator.surfaceCopyRecords;
  plan.stretchRectRecords = coordinator.stretchRectRecords;
  plan.colorFillRecords = coordinator.colorFillRecords;
  plan.depthResolveRecords = coordinator.depthResolveRecords;
  plan.generateMipmapsRecords = coordinator.generateMipmapsRecords;
  plan.presentRecords = coordinator.presentRecords;
  // Readback has no direct appender and must never be provisioned: a plan
  // reserving one is a structural rejection, not a capacity question.
  plan.readbackRecords = 0;
  plan.commandHeaders = draws + coordinator.records();
  if (plan.commandHeaders < extra.commandHeaders) {
    plan.commandHeaders = extra.commandHeaders;
  }
  applyDerivedDrawCapacity(plan, draws);
  return plan;
}

// True only when nothing has been appended to any dimension this policy
// provisions. `commandsEmpty()` alone is not enough to license a reallocating
// reserve: it inspects one vector, and provisioning must not move storage any
// other published row still occupies.
inline bool chunkSlotDirectStorageEmpty(const ChunkSlot& slot) noexcept {
  return slot.commandHeaders.empty() && slot.drawHotStates.empty() &&
      slot.drawShaderLayouts.empty() && slot.drawDebugSnapshots.empty() &&
      slot.drawPsoSubviews.empty() && slot.drawUniformFixedPayloads.empty() &&
      slot.drawUniformVertexConstants.empty() &&
      slot.drawUniformVertexConstantBytes.empty() &&
      slot.drawUniformPixelConstants.empty() &&
      slot.drawUniformPixelConstantBytes.empty() &&
      slot.drawUniformPayloads.empty() &&
      slot.drawUniformPayloadLookupNext.empty() &&
      slot.drawUniformVertexConstantsLookupNext.empty() &&
      slot.drawUniformPixelConstantsLookupNext.empty() &&
      slot.drawParams.empty() && slot.drawPayloadArena.empty() &&
      slot.drawRunRecords.empty() && slot.clearRecords.empty() &&
      slot.surfaceCopyRecords.empty() && slot.stretchRectRecords.empty() &&
      slot.readbackRecords.empty() && slot.colorFillRecords.empty() &&
      slot.depthResolveRecords.empty() &&
      slot.generateMipmapsRecords.empty() && slot.presentRecords.empty();
}

enum class DirectSlotProvisionAllocation : std::uint8_t {
  CommandHeaders,
  DrawHotStates,
  DrawShaderLayouts,
  DrawDebugSnapshots,
  DrawPsoSubviews,
  DrawUniformFixedPayloads,
  DrawUniformVertexConstants,
  DrawUniformVertexConstantBytes,
  DrawUniformPixelConstants,
  DrawUniformPixelConstantBytes,
  DrawUniformPayloads,
  DrawParams,
  DrawPayloadArena,
  DrawRunRecords,
  ClearRecords,
  SurfaceCopyRecords,
  StretchRectRecords,
  ColorFillRecords,
  DepthResolveRecords,
  GenerateMipmapsRecords,
  PresentRecords,
  PayloadLookupHeads,
  PayloadLookupTails,
  PayloadLookupNext,
  VertexLookupHeads,
  VertexLookupTails,
  VertexLookupNext,
  PixelLookupHeads,
  PixelLookupTails,
  PixelLookupNext,
  Count,
};

struct DirectSlotProvisionFault {
  void* context = nullptr;
  bool (*fail)(void*, DirectSlotProvisionAllocation) noexcept = nullptr;

  bool shouldFail(DirectSlotProvisionAllocation allocation) const noexcept {
    return fail && fail(context, allocation);
  }
};

class LeaseHeld;
struct DirectSlotCapacityLeaseReceipt;

// Low-level isolated reservation helper used by StagedDirectSlot (and kept as
// a value-test seam). Tests inject at the operation seam; production passes
// the default no-fault value. All allocations land in the isolated holder and
// the only live-slot mutation is the final noexcept swap.
inline bool provisionEmptyDirectSlotStorage(
    ChunkSlot& slot, const SourcePayloadCapacity& plan,
    DirectSlotProvisionFault fault = {}) noexcept {
  DXMT_ASSERT(chunkSlotDirectStorageEmpty(slot) &&
              "direct slot provisioning requires an empty physical payload");
  if (!chunkSlotDirectStorageEmpty(slot)) {
    return false;
  }
  ChunkSlot staged;
  const auto reserve = [&](auto& values, std::size_t required,
                           DirectSlotProvisionAllocation allocation) {
    if (required == 0) {
      return;
    }
    if (fault.shouldFail(allocation)) {
      throw std::bad_alloc{};
    }
    values.reserve(required);
  };
  const auto lookup = [&](std::vector<std::uint32_t>& heads,
                          std::vector<std::uint32_t>& tails,
                          std::vector<std::uint32_t>& next,
                          std::size_t count,
                          DirectSlotProvisionAllocation headAllocation,
                          DirectSlotProvisionAllocation tailAllocation,
                          DirectSlotProvisionAllocation nextAllocation) {
    if (count == 0) {
      return;
    }
    const auto buckets = detail::chunkSlotUniformLookupBucketCount(count);
    if (fault.shouldFail(headAllocation)) {
      throw std::bad_alloc{};
    }
    heads.assign(buckets, detail::kChunkSlotInvalidUniformIndex);
    if (fault.shouldFail(tailAllocation)) {
      throw std::bad_alloc{};
    }
    tails.assign(buckets, detail::kChunkSlotInvalidUniformIndex);
    if (fault.shouldFail(nextAllocation)) {
      throw std::bad_alloc{};
    }
    next.reserve(count);
  };
  try {
    reserve(staged.commandHeaders, plan.commandHeaders,
            DirectSlotProvisionAllocation::CommandHeaders);
    reserve(staged.drawHotStates, plan.drawHotStates,
            DirectSlotProvisionAllocation::DrawHotStates);
    reserve(staged.drawShaderLayouts, plan.drawShaderLayouts,
            DirectSlotProvisionAllocation::DrawShaderLayouts);
    reserve(staged.drawDebugSnapshots, plan.drawDebugSnapshots,
            DirectSlotProvisionAllocation::DrawDebugSnapshots);
    reserve(staged.drawPsoSubviews, plan.drawPsoSubviews,
            DirectSlotProvisionAllocation::DrawPsoSubviews);
    reserve(staged.drawUniformFixedPayloads, plan.drawUniformFixedPayloads,
            DirectSlotProvisionAllocation::DrawUniformFixedPayloads);
    reserve(staged.drawUniformVertexConstants,
            plan.drawUniformVertexConstants,
            DirectSlotProvisionAllocation::DrawUniformVertexConstants);
    reserve(staged.drawUniformVertexConstantBytes,
            plan.drawUniformVertexConstantBytes,
            DirectSlotProvisionAllocation::DrawUniformVertexConstantBytes);
    reserve(staged.drawUniformPixelConstants, plan.drawUniformPixelConstants,
            DirectSlotProvisionAllocation::DrawUniformPixelConstants);
    reserve(staged.drawUniformPixelConstantBytes,
            plan.drawUniformPixelConstantBytes,
            DirectSlotProvisionAllocation::DrawUniformPixelConstantBytes);
    reserve(staged.drawUniformPayloads, plan.drawUniformPayloads,
            DirectSlotProvisionAllocation::DrawUniformPayloads);
    reserve(staged.drawParams, plan.drawParams,
            DirectSlotProvisionAllocation::DrawParams);
    reserve(staged.drawPayloadArena, plan.drawPayloadBytes,
            DirectSlotProvisionAllocation::DrawPayloadArena);
    reserve(staged.drawRunRecords, plan.drawRunRecords,
            DirectSlotProvisionAllocation::DrawRunRecords);
    reserve(staged.clearRecords, plan.clearRecords,
            DirectSlotProvisionAllocation::ClearRecords);
    reserve(staged.surfaceCopyRecords, plan.surfaceCopyRecords,
            DirectSlotProvisionAllocation::SurfaceCopyRecords);
    reserve(staged.stretchRectRecords, plan.stretchRectRecords,
            DirectSlotProvisionAllocation::StretchRectRecords);
    reserve(staged.colorFillRecords, plan.colorFillRecords,
            DirectSlotProvisionAllocation::ColorFillRecords);
    reserve(staged.depthResolveRecords, plan.depthResolveRecords,
            DirectSlotProvisionAllocation::DepthResolveRecords);
    reserve(staged.generateMipmapsRecords, plan.generateMipmapsRecords,
            DirectSlotProvisionAllocation::GenerateMipmapsRecords);
    reserve(staged.presentRecords, plan.presentRecords,
            DirectSlotProvisionAllocation::PresentRecords);
    lookup(staged.drawUniformPayloadLookupHeads,
           staged.drawUniformPayloadLookupTails,
           staged.drawUniformPayloadLookupNext, plan.drawUniformPayloads,
           DirectSlotProvisionAllocation::PayloadLookupHeads,
           DirectSlotProvisionAllocation::PayloadLookupTails,
           DirectSlotProvisionAllocation::PayloadLookupNext);
    lookup(staged.drawUniformVertexConstantsLookupHeads,
           staged.drawUniformVertexConstantsLookupTails,
           staged.drawUniformVertexConstantsLookupNext,
           plan.drawUniformVertexConstants,
           DirectSlotProvisionAllocation::VertexLookupHeads,
           DirectSlotProvisionAllocation::VertexLookupTails,
           DirectSlotProvisionAllocation::VertexLookupNext);
    lookup(staged.drawUniformPixelConstantsLookupHeads,
           staged.drawUniformPixelConstantsLookupTails,
           staged.drawUniformPixelConstantsLookupNext,
           plan.drawUniformPixelConstants,
           DirectSlotProvisionAllocation::PixelLookupHeads,
           DirectSlotProvisionAllocation::PixelLookupTails,
           DirectSlotProvisionAllocation::PixelLookupNext);
  } catch (...) {
    return false;
  }

  using std::swap;
  swap(slot.commandHeaders, staged.commandHeaders);
  swap(slot.drawHotStates, staged.drawHotStates);
  swap(slot.drawShaderLayouts, staged.drawShaderLayouts);
  swap(slot.drawDebugSnapshots, staged.drawDebugSnapshots);
  swap(slot.drawPsoSubviews, staged.drawPsoSubviews);
  swap(slot.drawUniformFixedPayloads, staged.drawUniformFixedPayloads);
  swap(slot.drawUniformVertexConstants, staged.drawUniformVertexConstants);
  swap(slot.drawUniformVertexConstantBytes,
       staged.drawUniformVertexConstantBytes);
  swap(slot.drawUniformPixelConstants, staged.drawUniformPixelConstants);
  swap(slot.drawUniformPixelConstantBytes,
       staged.drawUniformPixelConstantBytes);
  swap(slot.drawUniformPayloads, staged.drawUniformPayloads);
  swap(slot.drawUniformPayloadLookupHeads,
       staged.drawUniformPayloadLookupHeads);
  swap(slot.drawUniformPayloadLookupTails,
       staged.drawUniformPayloadLookupTails);
  swap(slot.drawUniformPayloadLookupNext,
       staged.drawUniformPayloadLookupNext);
  swap(slot.drawUniformVertexConstantsLookupHeads,
       staged.drawUniformVertexConstantsLookupHeads);
  swap(slot.drawUniformVertexConstantsLookupTails,
       staged.drawUniformVertexConstantsLookupTails);
  swap(slot.drawUniformVertexConstantsLookupNext,
       staged.drawUniformVertexConstantsLookupNext);
  swap(slot.drawUniformPixelConstantsLookupHeads,
       staged.drawUniformPixelConstantsLookupHeads);
  swap(slot.drawUniformPixelConstantsLookupTails,
       staged.drawUniformPixelConstantsLookupTails);
  swap(slot.drawUniformPixelConstantsLookupNext,
       staged.drawUniformPixelConstantsLookupNext);
  swap(slot.drawParams, staged.drawParams);
  swap(slot.drawPayloadArena, staged.drawPayloadArena);
  swap(slot.drawRunRecords, staged.drawRunRecords);
  swap(slot.clearRecords, staged.clearRecords);
  swap(slot.surfaceCopyRecords, staged.surfaceCopyRecords);
  swap(slot.stretchRectRecords, staged.stretchRectRecords);
  swap(slot.colorFillRecords, staged.colorFillRecords);
  swap(slot.depthResolveRecords, staged.depthResolveRecords);
  swap(slot.generateMipmapsRecords, staged.generateMipmapsRecords);
  swap(slot.presentRecords, staged.presentRecords);
  return true;
}

struct DirectSlotCapacityLeaseEntry {
  std::uint64_t generation = 0;
  std::uint64_t retainedBytes = 0;

  friend constexpr bool operator==(const DirectSlotCapacityLeaseEntry&,
                                   const DirectSlotCapacityLeaseEntry&) = default;
};

// A settlement ticket is bound to both the observed capacity generation and
// a monotone operation serial.  The serial prevents a ticket from a prior
// rollback from settling a later stage that happens to use the same bytes.
struct DirectSlotCapacityLeaseTicket {
  std::uint64_t generation = 0;
  std::uint64_t serial = 0;

  constexpr bool valid() const noexcept {
    return generation != 0 && serial != 0;
  }
  friend constexpr bool operator==(const DirectSlotCapacityLeaseTicket&,
                                   const DirectSlotCapacityLeaseTicket&) = default;
};

struct DirectSlotCapacityLeaseState {
  std::uint64_t limitBytes = 0;
  std::uint64_t retainedBytes = 0;
  std::uint64_t stagedBytes = 0;
  std::uint64_t nextTicketSerial = 1;
  DirectSlotCapacityLeaseEntry entry{};
  DirectSlotCapacityLeaseTicket stagedTicket{};

  friend constexpr bool operator==(const DirectSlotCapacityLeaseState&,
                                   const DirectSlotCapacityLeaseState&) = default;
};

enum class DirectSlotCapacityLeaseEvent : std::uint8_t {
  Observe,
  Stage,
  Adopt,
  Rollback,
};

struct DirectSlotCapacityLeaseTransition {
  DirectSlotCapacityLeaseState next{};
  bool accepted = false;
  DirectSlotCapacityLeaseTicket ticket{};
};

inline constexpr std::uint64_t nextDirectSlotCapacityGeneration(
    std::uint64_t generation) noexcept {
  return generation == std::numeric_limits<std::uint64_t>::max()
      ? 0
      : generation + 1;
}

// Shared pure aggregate-lease reducer used by production and native tests.
// `Observe` binds retained credit to the physical payload's actual vector
// capacities; `Stage` charges new capacity without releasing old capacity;
// `Adopt` replaces old with staged and advances the capacity generation;
// `Rollback` releases only staged credit.
inline constexpr DirectSlotCapacityLeaseTransition
reduceDirectSlotCapacityLease(
    DirectSlotCapacityLeaseState state,
    DirectSlotCapacityLeaseEvent event,
    std::uint64_t bytes = 0,
    DirectSlotCapacityLeaseTicket ticket = {}) noexcept {
  const auto add = [](std::uint64_t a, std::uint64_t b,
                      std::uint64_t& result) constexpr noexcept {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
      return false;
    }
    result = a + b;
    return true;
  };
  switch (event) {
  case DirectSlotCapacityLeaseEvent::Observe: {
    if (state.stagedBytes != 0 || state.retainedBytes < state.entry.retainedBytes) {
      return {state, false};
    }
    std::uint64_t retained = state.retainedBytes - state.entry.retainedBytes;
    if (!add(retained, bytes, retained)) {
      return {state, false};
    }
    if (state.limitBytes != 0 && retained > state.limitBytes) {
      return {state, false};
    }
    if (state.entry.generation == 0 ||
        state.entry.retainedBytes != bytes) {
      const auto generation = nextDirectSlotCapacityGeneration(
          state.entry.generation);
      if (generation == 0) {
        return {state, false};
      }
      state.entry.generation = generation;
    }
    state.entry.retainedBytes = bytes;
    state.retainedBytes = retained;
    return {state, true};
  }
  case DirectSlotCapacityLeaseEvent::Stage: {
    std::uint64_t transient = 0;
    if (bytes == 0 || state.limitBytes == 0 || state.stagedBytes != 0 ||
        ticket.valid() ||
        !state.entry.generation ||
        state.nextTicketSerial == 0 ||
        !add(state.retainedBytes, bytes, transient) ||
        transient > state.limitBytes) {
      return {state, false};
    }
    state.stagedBytes = bytes;
    state.stagedTicket = {.generation = state.entry.generation,
                          .serial = state.nextTicketSerial};
    if (state.nextTicketSerial == std::numeric_limits<std::uint64_t>::max()) {
      state.nextTicketSerial = 0;
    } else {
      ++state.nextTicketSerial;
    }
    return {.next = state, .accepted = true, .ticket = state.stagedTicket};
  }
  case DirectSlotCapacityLeaseEvent::Adopt: {
    if (state.stagedBytes == 0 || bytes == 0 || bytes > state.stagedBytes ||
        state.retainedBytes < state.entry.retainedBytes ||
        !ticket.valid() || ticket != state.stagedTicket ||
        ticket.generation != state.entry.generation) {
      return {state, false};
    }
    std::uint64_t retained = state.retainedBytes - state.entry.retainedBytes;
    if (!add(retained, bytes, retained)) {
      return {state, false};
    }
    const auto generation = nextDirectSlotCapacityGeneration(
        state.entry.generation);
    if (generation == 0) {
      return {state, false};
    }
    state.entry = {.generation = generation,
                   .retainedBytes = bytes};
    state.retainedBytes = retained;
    state.stagedBytes = 0;
    state.stagedTicket = {};
    return {state, true};
  }
  case DirectSlotCapacityLeaseEvent::Rollback:
    if (bytes != 0 || state.stagedBytes == 0 || !ticket.valid() ||
        ticket != state.stagedTicket ||
        ticket.generation != state.entry.generation) {
      return {state, false};
    }
    state.stagedBytes = 0;
    state.stagedTicket = {};
    return {state, true};
  }
  return {state, false};
}

template <std::size_t PayloadCount>
struct DirectSlotCapacityLeaseReconciliation;

template <std::size_t PayloadCount>
inline constexpr DirectSlotCapacityLeaseReconciliation<PayloadCount>
reconcileDirectSlotCapacityLease(
    const std::array<DirectSlotCapacityLeaseEntry, PayloadCount>& entries,
    const std::array<std::uint64_t, PayloadCount>& actualRetainedBytes,
    std::size_t currentIndex, std::uint64_t limitBytes,
    std::uint64_t nextTicketSerial) noexcept;

// Queue-owned aggregate ledger. All fields are scalar/fixed-capacity and are
// guarded by the queue mutex. A LeaseHeld never outlives that guard.
template <std::size_t PayloadCount>
struct DirectSlotCapacityLeaseLedger {
  std::array<DirectSlotCapacityLeaseEntry, PayloadCount> entries{};
  std::uint64_t limitBytes = 0;
  std::uint64_t retainedBytes = 0;
  std::uint64_t stagedBytes = 0;
  std::uint64_t nextTicketSerial = 1;
};

inline constexpr std::size_t kDirectSlotCapacityPayloadCount = 64;
using QueueDirectSlotCapacityLeaseLedger =
    DirectSlotCapacityLeaseLedger<kDirectSlotCapacityPayloadCount>;

struct DirectSlotCapacityLeaseReceipt {
  std::uint64_t retainedBytes = 0;
  std::uint64_t generation = 0;
  bool committed = false;

  constexpr explicit operator bool() const noexcept { return committed; }
};

// Move-only linear capability for one outstanding aggregate lease. Its
// destructor is the rollback edge, and that edge is disarmed before commit;
// therefore the typed surface has no committed-to-rollback transition.
class LeaseHeld {
 public:
  LeaseHeld() = default;
  LeaseHeld(const LeaseHeld&) = delete;
  LeaseHeld& operator=(const LeaseHeld&) = delete;

  LeaseHeld(LeaseHeld&& other) noexcept
      : ledger_(other.ledger_), destination_(other.destination_),
        state_(other.state_), index_(other.index_), ticket_(other.ticket_),
        active_(other.active_) {
    other.disarm();
  }

  LeaseHeld& operator=(LeaseHeld&&) = delete;

  ~LeaseHeld() { rollback(); }

  bool valid() const noexcept {
    return active_ && ledger_ != nullptr && destination_ != nullptr &&
        ticket_.valid();
  }
  DirectSlotCapacityLeaseTicket ticket() const noexcept { return ticket_; }

  static LeaseHeld tryAcquire(
      QueueDirectSlotCapacityLeaseLedger& ledger,
      ChunkSlot& destination,
      const std::array<std::uint64_t, kDirectSlotCapacityPayloadCount>&
          actualRetainedBytes,
      std::size_t currentIndex, std::uint64_t limitBytes,
      std::uint64_t requestedBytes, bool* generationAdvanced = nullptr,
      std::uint64_t* reconciledRetainedBytes = nullptr,
      bool* reconciliationAccepted = nullptr) noexcept;

 private:
  LeaseHeld(QueueDirectSlotCapacityLeaseLedger& ledger,
            ChunkSlot& destination,
            DirectSlotCapacityLeaseState state, std::size_t index,
            DirectSlotCapacityLeaseTicket ticket) noexcept
      : ledger_(&ledger), destination_(&destination), state_(state),
        index_(index), ticket_(ticket), active_(true) {}

  void rollbackLedger() noexcept {
    auto& ledger = *ledger_;
    const auto rolledBack = reduceDirectSlotCapacityLease(
        state_, DirectSlotCapacityLeaseEvent::Rollback, 0, ticket_);
    if (!rolledBack.accepted || index_ >= ledger.entries.size()) {
      return;
    }
    ledger.entries[index_] = rolledBack.next.entry;
    ledger.limitBytes = rolledBack.next.limitBytes;
    ledger.retainedBytes = rolledBack.next.retainedBytes;
    ledger.stagedBytes = rolledBack.next.stagedBytes;
    ledger.nextTicketSerial = rolledBack.next.nextTicketSerial;
  }

  void rollback() noexcept {
    if (!active_) {
      return;
    }
    rollbackLedger();
    disarm();
  }

  void disarm() noexcept {
    ledger_ = nullptr;
    destination_ = nullptr;
    state_ = {};
    index_ = 0;
    ticket_ = {};
    active_ = false;
  }

  QueueDirectSlotCapacityLeaseLedger* ledger_ = nullptr;
  ChunkSlot* destination_ = nullptr;
  DirectSlotCapacityLeaseState state_{};
  std::size_t index_ = 0;
  DirectSlotCapacityLeaseTicket ticket_{};
  bool active_ = false;

  friend class StagedDirectSlot;
};

// Owns the reserved candidate and the held aggregate capability as one
// linear object. Construction consumes LeaseHeld&&; callers cannot create a
// staged owner without first acquiring and moving the matching lease.
class StagedDirectSlot {
 public:
  StagedDirectSlot() = default;
  StagedDirectSlot(const StagedDirectSlot&) = delete;
  StagedDirectSlot& operator=(const StagedDirectSlot&) = delete;

  StagedDirectSlot(StagedDirectSlot&& other) noexcept
      : lease_(std::move(other.lease_)),
        destination_(other.destination_), ready_(other.ready_) {
    swapStorage(staged_, other.staged_);
    other.destination_ = nullptr;
    other.ready_ = false;
  }

  StagedDirectSlot& operator=(StagedDirectSlot&&) = delete;

  static StagedDirectSlot create(
      LeaseHeld&& lease, const SourcePayloadCapacity& plan,
      DirectSlotProvisionFault fault = {}) noexcept {
    if (!lease.valid()) {
      return {};
    }
    return StagedDirectSlot(std::move(lease), plan, fault);
  }

  bool valid() const noexcept {
    return ready_ && destination_ != nullptr && lease_.valid();
  }
  std::uint64_t retainedBytes() const noexcept {
    return directSlotPhysicalRetainedBytes(staged_);
  }

  // Consuming commit is intentionally rvalue-only. A committed owner is
  // disarmed before the first live-slot swap and cannot roll back later.
  DirectSlotCapacityLeaseReceipt commit() && noexcept {
    if (!valid()) {
      return {};
    }
    const auto adopted = reduceDirectSlotCapacityLease(
        lease_.state_, DirectSlotCapacityLeaseEvent::Adopt, retainedBytes(),
        lease_.ticket_);
    if (!adopted.accepted) {
      return {};
    }
    auto* ledger = lease_.ledger_;
    const auto index = lease_.index_;
    lease_.disarm();
    swapStorage(*destination_, staged_);
    ledger->entries[index] = adopted.next.entry;
    ledger->limitBytes = adopted.next.limitBytes;
    ledger->retainedBytes = adopted.next.retainedBytes;
    ledger->stagedBytes = adopted.next.stagedBytes;
    ledger->nextTicketSerial = adopted.next.nextTicketSerial;
    ready_ = false;
    destination_ = nullptr;
    return {.retainedBytes = adopted.next.retainedBytes,
            .generation = adopted.next.entry.generation,
            .committed = true};
  }

 private:
  StagedDirectSlot(LeaseHeld&& lease, const SourcePayloadCapacity& plan,
                   DirectSlotProvisionFault fault) noexcept
      : lease_(std::move(lease)), destination_(lease_.destination_) {
    if (destination_ == nullptr ||
        !chunkSlotDirectStorageEmpty(*destination_)) {
      destination_ = nullptr;
      return;
    }
    ready_ = provisionEmptyDirectSlotStorage(staged_, plan, fault);
    if (!ready_) {
      destination_ = nullptr;
    }
  }

  static void swapStorage(ChunkSlot& lhs, ChunkSlot& rhs) noexcept {
    using std::swap;
    swap(lhs.commandHeaders, rhs.commandHeaders);
    swap(lhs.drawHotStates, rhs.drawHotStates);
    swap(lhs.drawShaderLayouts, rhs.drawShaderLayouts);
    swap(lhs.drawDebugSnapshots, rhs.drawDebugSnapshots);
    swap(lhs.drawPsoSubviews, rhs.drawPsoSubviews);
    swap(lhs.drawUniformFixedPayloads, rhs.drawUniformFixedPayloads);
    swap(lhs.drawUniformVertexConstants, rhs.drawUniformVertexConstants);
    swap(lhs.drawUniformVertexConstantBytes,
         rhs.drawUniformVertexConstantBytes);
    swap(lhs.drawUniformPixelConstants, rhs.drawUniformPixelConstants);
    swap(lhs.drawUniformPixelConstantBytes,
         rhs.drawUniformPixelConstantBytes);
    swap(lhs.drawUniformPayloads, rhs.drawUniformPayloads);
    swap(lhs.drawUniformPayloadLookupHeads,
         rhs.drawUniformPayloadLookupHeads);
    swap(lhs.drawUniformPayloadLookupTails,
         rhs.drawUniformPayloadLookupTails);
    swap(lhs.drawUniformPayloadLookupNext, rhs.drawUniformPayloadLookupNext);
    swap(lhs.drawUniformVertexConstantsLookupHeads,
         rhs.drawUniformVertexConstantsLookupHeads);
    swap(lhs.drawUniformVertexConstantsLookupTails,
         rhs.drawUniformVertexConstantsLookupTails);
    swap(lhs.drawUniformVertexConstantsLookupNext,
         rhs.drawUniformVertexConstantsLookupNext);
    swap(lhs.drawUniformPixelConstantsLookupHeads,
         rhs.drawUniformPixelConstantsLookupHeads);
    swap(lhs.drawUniformPixelConstantsLookupTails,
         rhs.drawUniformPixelConstantsLookupTails);
    swap(lhs.drawUniformPixelConstantsLookupNext,
         rhs.drawUniformPixelConstantsLookupNext);
    swap(lhs.drawParams, rhs.drawParams);
    swap(lhs.drawPayloadArena, rhs.drawPayloadArena);
    swap(lhs.drawRunRecords, rhs.drawRunRecords);
    swap(lhs.clearRecords, rhs.clearRecords);
    swap(lhs.surfaceCopyRecords, rhs.surfaceCopyRecords);
    swap(lhs.stretchRectRecords, rhs.stretchRectRecords);
    swap(lhs.readbackRecords, rhs.readbackRecords);
    swap(lhs.colorFillRecords, rhs.colorFillRecords);
    swap(lhs.depthResolveRecords, rhs.depthResolveRecords);
    swap(lhs.generateMipmapsRecords, rhs.generateMipmapsRecords);
    swap(lhs.presentRecords, rhs.presentRecords);
  }

  LeaseHeld lease_{};
  ChunkSlot staged_{};
  ChunkSlot* destination_ = nullptr;
  bool ready_ = false;
};

template <std::size_t PayloadCount>
struct DirectSlotCapacityLeaseReconciliation {
  DirectSlotCapacityLeaseState state{};
  std::array<DirectSlotCapacityLeaseEntry, PayloadCount> entries{};
  bool accepted = false;
  bool generationAdvanced = false;
};

// Reconcile the complete physical owner set before one payload may stage new
// headroom.  The result deliberately restores `state.entry` to currentIndex:
// leaving the last folded peer selected would bind the settlement ticket to
// the wrong physical payload whenever currentIndex != PayloadCount - 1.
template <std::size_t PayloadCount>
inline constexpr DirectSlotCapacityLeaseReconciliation<PayloadCount>
reconcileDirectSlotCapacityLease(
    const std::array<DirectSlotCapacityLeaseEntry, PayloadCount>& entries,
    const std::array<std::uint64_t, PayloadCount>& actualRetainedBytes,
    std::size_t currentIndex,
    std::uint64_t limitBytes,
    std::uint64_t nextTicketSerial) noexcept {
  DirectSlotCapacityLeaseReconciliation<PayloadCount> result{};
  result.entries = entries;
  if (currentIndex >= PayloadCount || limitBytes == 0 ||
      nextTicketSerial == 0) {
    return result;
  }

  result.state.limitBytes = limitBytes;
  result.state.nextTicketSerial = nextTicketSerial;
  for (const auto& entry : entries) {
    if (entry.retainedBytes >
        std::numeric_limits<std::uint64_t>::max() -
            result.state.retainedBytes) {
      return result;
    }
    result.state.retainedBytes += entry.retainedBytes;
  }

  for (std::size_t i = 0; i < PayloadCount; ++i) {
    result.state.entry = entries[i];
    const auto observed = reduceDirectSlotCapacityLease(
        result.state, DirectSlotCapacityLeaseEvent::Observe,
        actualRetainedBytes[i]);
    if (!observed.accepted) {
      return result;
    }
    result.generationAdvanced = result.generationAdvanced ||
        observed.next.entry.generation != result.state.entry.generation;
    result.state = observed.next;
    result.entries[i] = result.state.entry;
  }

  result.state.entry = result.entries[currentIndex];
  result.accepted = true;
  return result;
}

inline LeaseHeld LeaseHeld::tryAcquire(
    QueueDirectSlotCapacityLeaseLedger& ledger,
    ChunkSlot& destination,
    const std::array<std::uint64_t, kDirectSlotCapacityPayloadCount>&
        actualRetainedBytes,
    std::size_t currentIndex, std::uint64_t limitBytes,
    std::uint64_t requestedBytes, bool* generationAdvanced,
    std::uint64_t* reconciledRetainedBytes,
    bool* reconciliationAccepted) noexcept {
  if (generationAdvanced) {
    *generationAdvanced = false;
  }
  if (reconciledRetainedBytes) {
    *reconciledRetainedBytes = 0;
  }
  if (reconciliationAccepted) {
    *reconciliationAccepted = false;
  }
  const auto reconciliation = reconcileDirectSlotCapacityLease(
      ledger.entries, actualRetainedBytes, currentIndex, limitBytes,
      ledger.nextTicketSerial);
  if (!reconciliation.accepted) {
    return {};
  }
  // Preserve the pre-refactor observation boundary even if Stage is denied:
  // physical reconciliation is an observable ledger update, not part of the
  // candidate rollback.
  ledger.entries = reconciliation.entries;
  ledger.entries[currentIndex] = reconciliation.state.entry;
  ledger.limitBytes = reconciliation.state.limitBytes;
  ledger.retainedBytes = reconciliation.state.retainedBytes;
  ledger.stagedBytes = reconciliation.state.stagedBytes;
  ledger.nextTicketSerial = reconciliation.state.nextTicketSerial;
  if (reconciliationAccepted) {
    *reconciliationAccepted = true;
  }
  if (reconciledRetainedBytes) {
    *reconciledRetainedBytes = reconciliation.state.retainedBytes;
  }
  if (generationAdvanced) {
    *generationAdvanced = reconciliation.generationAdvanced;
  }
  const auto stage = reduceDirectSlotCapacityLease(
      reconciliation.state, DirectSlotCapacityLeaseEvent::Stage,
      requestedBytes);
  if (!stage.accepted) {
    return {};
  }
  ledger.entries[currentIndex] = stage.next.entry;
  ledger.limitBytes = stage.next.limitBytes;
  ledger.retainedBytes = stage.next.retainedBytes;
  ledger.stagedBytes = stage.next.stagedBytes;
  ledger.nextTicketSerial = stage.next.nextTicketSerial;
  return LeaseHeld(ledger, destination, stage.next, currentIndex, stage.ticket);
}

inline constexpr bool directContinuationAppendFits(
    std::size_t current, std::size_t extra, std::size_t capacity) noexcept {
  return extra <= std::numeric_limits<std::size_t>::max() - current &&
      capacity >= current + extra;
}

// Pure production admission predicate.  `extra` is the immutable final
// capacity plan for the source being considered; it is not a count of raw
// records. APPLY_STATE and constant setters are state-only and therefore do
// not contribute commandHeaders.  The plan producer must nevertheless emit
// the complete one-draw-per-header SoA shape checked below, plus at most one
// header per coordinator locator.
inline DirectContinuationAdmissionResult directContinuationAdmission(
    const ChunkSlot& slot, const SourcePayloadCapacity& extra) noexcept {
  // Readback has a declared final-slot vector but no direct-branch appender
  // (`TransactionalChunkSlotAssembler::tryAppendReadback` hard-fails when the
  // destination is a direct ChunkSlot), so a plan reserving one can never be
  // built and must stay a structural rejection rather than a capacity one.
  if (extra.readbackRecords != 0) {
    return {DirectContinuationAdmission::StructuralRejected};
  }

  // A source-wide emission plan may carry coordinator locators alongside its
  // draw islands. Each contributes exactly one command header plus one row in
  // its own typed vector, so the header total is no longer the draw count.
  // Derive the draw count from `drawParams` -- the one dimension that is
  // one-per-draw and never written by a coordinator -- and require the header
  // total to equal draws plus locators exactly. With no coordinator dimension
  // present this reduces to the historical draw-only predicate with
  // byte-identical behavior.
  const auto drawCount = extra.drawParams;
  const auto coordinatorHeaders =
      extra.clearRecords + extra.surfaceCopyRecords +
      extra.stretchRectRecords + extra.colorFillRecords +
      extra.depthResolveRecords + extra.generateMipmapsRecords +
      extra.presentRecords;
  if (coordinatorHeaders >
          std::numeric_limits<std::size_t>::max() - drawCount ||
      extra.commandHeaders != drawCount + coordinatorHeaders ||
      // A Clear locator's rects live inside its own ClearDesc, so the rect
      // dimension is a transaction-wide bookkeeping total, never a slot
      // vector. It may only be non-zero when a Clear locator is present.
      (extra.clearRects != 0 && extra.clearRecords == 0)) {
    return {DirectContinuationAdmission::StructuralRejected};
  }

  const auto drawShapeMatches =
      drawCount != 0 && extra.drawHotStates == drawCount &&
      extra.drawShaderLayouts == drawCount &&
      extra.drawDebugSnapshots == drawCount &&
      extra.drawPsoSubviews == drawCount &&
      extra.drawUniformFixedPayloads == drawCount &&
      extra.drawUniformVertexConstants == drawCount &&
      extra.drawUniformPixelConstants == drawCount &&
      extra.drawUniformPayloads == drawCount && extra.drawParams == drawCount &&
      extra.drawRunRecords == drawCount;
  if (!drawShapeMatches ||
      drawCount > std::numeric_limits<std::size_t>::max() /
          sizeof(VertexShaderConstants) ||
      drawCount > std::numeric_limits<std::size_t>::max() /
          sizeof(PixelShaderConstants) ||
      extra.drawUniformVertexConstantBytes !=
          drawCount * sizeof(VertexShaderConstants) ||
      extra.drawUniformPixelConstantBytes !=
          drawCount * sizeof(PixelShaderConstants) ||
      extra.drawUniformPayloadLookupHeads !=
          detail::chunkSlotUniformLookupBucketCount(drawCount) ||
      extra.drawUniformPayloadLookupTails !=
          extra.drawUniformPayloadLookupHeads ||
      extra.drawUniformPayloadLookupNext != drawCount ||
      extra.drawUniformVertexConstantsLookupHeads !=
          extra.drawUniformPayloadLookupHeads ||
      extra.drawUniformVertexConstantsLookupTails !=
          extra.drawUniformPayloadLookupTails ||
      extra.drawUniformVertexConstantsLookupNext != drawCount ||
      extra.drawUniformPixelConstantsLookupHeads !=
          extra.drawUniformPayloadLookupHeads ||
      extra.drawUniformPixelConstantsLookupTails !=
          extra.drawUniformPayloadLookupTails ||
      extra.drawUniformPixelConstantsLookupNext != drawCount) {
    return {DirectContinuationAdmission::StructuralRejected};
  }
  if (slot.pipelinePrefetchSealed || !slot.drawStateStorageConsistent() ||
      !slot.commandPayloadsInRange()) {
    return {DirectContinuationAdmission::StructuralRejected};
  }

  const auto fits = [&](std::size_t current, std::size_t add,
                        std::size_t capacity) noexcept {
    return directContinuationAppendFits(current, add, capacity);
  };
  if (!fits(slot.commandHeaders.size(), extra.commandHeaders,
            slot.commandHeaders.capacity()) ||
      !fits(slot.drawHotStates.size(), extra.drawHotStates,
            slot.drawHotStates.capacity()) ||
      !fits(slot.drawShaderLayouts.size(), extra.drawShaderLayouts,
            slot.drawShaderLayouts.capacity()) ||
      !fits(slot.drawDebugSnapshots.size(), extra.drawDebugSnapshots,
            slot.drawDebugSnapshots.capacity()) ||
      !fits(slot.drawPsoSubviews.size(), extra.drawPsoSubviews,
            slot.drawPsoSubviews.capacity()) ||
      !fits(slot.drawUniformFixedPayloads.size(),
            extra.drawUniformFixedPayloads,
            slot.drawUniformFixedPayloads.capacity()) ||
      !fits(slot.drawUniformVertexConstants.size(),
            extra.drawUniformVertexConstants,
            slot.drawUniformVertexConstants.capacity()) ||
      !fits(slot.drawUniformVertexConstantBytes.size(),
            extra.drawUniformVertexConstantBytes,
            slot.drawUniformVertexConstantBytes.capacity()) ||
      !fits(slot.drawUniformPixelConstants.size(),
            extra.drawUniformPixelConstants,
            slot.drawUniformPixelConstants.capacity()) ||
      !fits(slot.drawUniformPixelConstantBytes.size(),
            extra.drawUniformPixelConstantBytes,
            slot.drawUniformPixelConstantBytes.capacity()) ||
      !fits(slot.drawUniformPayloads.size(), extra.drawUniformPayloads,
            slot.drawUniformPayloads.capacity()) ||
      !fits(slot.drawParams.size(), extra.drawParams, slot.drawParams.capacity()) ||
      !fits(slot.drawPayloadArena.size(), extra.drawPayloadBytes,
            slot.drawPayloadArena.capacity()) ||
      !fits(slot.drawRunRecords.size(), extra.drawRunRecords,
            slot.drawRunRecords.capacity()) ||
      !fits(slot.clearRecords.size(), extra.clearRecords,
            slot.clearRecords.capacity()) ||
      !fits(slot.surfaceCopyRecords.size(), extra.surfaceCopyRecords,
            slot.surfaceCopyRecords.capacity()) ||
      !fits(slot.stretchRectRecords.size(), extra.stretchRectRecords,
            slot.stretchRectRecords.capacity()) ||
      !fits(slot.colorFillRecords.size(), extra.colorFillRecords,
            slot.colorFillRecords.capacity()) ||
      !fits(slot.depthResolveRecords.size(), extra.depthResolveRecords,
            slot.depthResolveRecords.capacity()) ||
      !fits(slot.generateMipmapsRecords.size(), extra.generateMipmapsRecords,
            slot.generateMipmapsRecords.capacity()) ||
      !fits(slot.presentRecords.size(), extra.presentRecords,
            slot.presentRecords.capacity())) {
    return {DirectContinuationAdmission::CapacityRejected};
  }

  // The assembler's lookup reserve may resize `next` or rebuild bucket
  // heads. Both operations are forbidden after this pre-effect admission.
  const auto lookupReady = [&](const auto& records, const auto& heads,
                               const auto& tails, const auto& next,
                               std::size_t additional) noexcept {
    if (additional == 0) {
      return heads.size() == tails.size() &&
          next.size() == records.size() &&
          (records.empty() ||
           heads.size() >= detail::chunkSlotUniformLookupBucketCount(
               records.size()));
    }
    if (additional > std::numeric_limits<std::size_t>::max() - records.size()) {
      return false;
    }
    const auto bucketCount = detail::chunkSlotUniformLookupBucketCount(
        records.size() + additional);
    return heads.size() == tails.size() && heads.size() >= bucketCount &&
        next.size() == records.size() &&
        next.capacity() >= records.size() + additional;
  };
  if (!lookupReady(slot.drawUniformPayloads,
                   slot.drawUniformPayloadLookupHeads,
                   slot.drawUniformPayloadLookupTails,
                   slot.drawUniformPayloadLookupNext,
                   extra.drawUniformPayloads) ||
      !lookupReady(slot.drawUniformVertexConstants,
                   slot.drawUniformVertexConstantsLookupHeads,
                   slot.drawUniformVertexConstantsLookupTails,
                   slot.drawUniformVertexConstantsLookupNext,
                   extra.drawUniformVertexConstants) ||
      !lookupReady(slot.drawUniformPixelConstants,
                   slot.drawUniformPixelConstantsLookupHeads,
                   slot.drawUniformPixelConstantsLookupTails,
                   slot.drawUniformPixelConstantsLookupNext,
                   extra.drawUniformPixelConstants)) {
    return {DirectContinuationAdmission::CapacityRejected};
  }
  return {DirectContinuationAdmission::Admitted};
}


// ---------------------------------------------------------------------------
// Shared storage transition (R-BACK-2.104).
//
// One pure reducer for the whole "may this source construct into the current
// final slot, and what does that cost the slot's storage" question. Production
// (`CommandQueue::beginDirectChunkSlotReplay`), the native truth table and the
// TLA model binding all consume THIS function, and production switches on
// `action` rather than re-deriving rotate-vs-fallback from `admission`. The one
// lifecycle input the reducer cannot see -- whether this admission has already
// spent its single bounded rotation -- is passed in as `rotationAllowed`, so
// every arm the tests assert on is an arm production can take.
//
// It decides storage only. Producer identity, span-witness succession,
// Present ordering, ordered-control cuts and every semantic effect stay where
// they are and are unchanged by this reducer.
enum class DirectSlotStorageAction : std::uint8_t {
  // Slot is empty in every direct dimension: reserve the provisioning plan
  // once, here, where no published row can be moved by the allocation.
  ProvisionEmpty,
  // Slot carries no commands but is not empty in every dimension. Reserve
  // nothing extra and let the transaction's own exact reservation stand.
  ProvisionSkippedNonEmpty,
  // Provisioning is switched off (`DXMT9_DIRECT_SLOT_HEADROOM_BYTES=0`).
  // Byte-identical to the pre-R-BACK-2.104 exact-fit behaviour.
  ProvisionDisabled,
  // Populated slot with room: append in place, no allocation at all.
  AppendInPlace,
  // Populated slot without room for this exact plan. Publish the extent and
  // retry against a fresh slot. After provisioning this is a *genuine budget*
  // rotation -- the source is larger than the slot budget -- not an artefact
  // of reserving exactly what the previous source needed.
  Rotate,
  // Structural, or capacity with rotation not permitted. Legacy owns it.
  LegacyFallback,
};

struct DirectSlotStorageDecision {
  DirectSlotStorageAction action = DirectSlotStorageAction::LegacyFallback;
  DirectContinuationAdmission admission =
      DirectContinuationAdmission::StructuralRejected;
  // Meaningful only for ProvisionEmpty.
  SourcePayloadCapacity provision{};
  bool populated = false;
  // ProvisionEmpty only: the source's own exact plan was at or beyond the
  // provisioning ceiling, so the slot was reserved exactly and the next source
  // is a genuine budget rotation. Classified here rather than re-derived at the
  // call site, so a coordinator-only span cannot be counted as at-budget.
  bool sourceExceedsBudget = false;

  constexpr bool constructsIntoCurrentSlot() const noexcept {
    return action == DirectSlotStorageAction::ProvisionEmpty ||
        action == DirectSlotStorageAction::ProvisionSkippedNonEmpty ||
        action == DirectSlotStorageAction::ProvisionDisabled ||
        action == DirectSlotStorageAction::AppendInPlace;
  }
};

inline DirectSlotStorageDecision directSlotStorageTransition(
    const ChunkSlot& slot, const SourcePayloadCapacity& extra,
    const DirectSlotProvisionBudget& budget,
    bool rotationAllowed) noexcept {
  DirectSlotStorageDecision decision{};
  decision.populated = !slot.commandsEmpty();
  if (!decision.populated) {
    // `commandsEmpty()` is the historical fresh-slot test and stays the
    // authority on whether this is a continuation. Provisioning needs the
    // strictly stronger property, because it is the only step that may move
    // storage.
    if (!budget.enabled()) {
      decision.action = DirectSlotStorageAction::ProvisionDisabled;
      return decision;
    }
    if (!chunkSlotDirectStorageEmpty(slot)) {
      decision.action = DirectSlotStorageAction::ProvisionSkippedNonEmpty;
      return decision;
    }
    decision.action = DirectSlotStorageAction::ProvisionEmpty;
    decision.provision = directSlotProvisionPlan(extra, budget);
    decision.sourceExceedsBudget =
        directSlotSourceExceedsProvisionBudget(extra, budget);
    return decision;
  }
  decision.admission = directContinuationAdmission(slot, extra).disposition;
  switch (decision.admission) {
  case DirectContinuationAdmission::Admitted:
    decision.action = DirectSlotStorageAction::AppendInPlace;
    break;
  case DirectContinuationAdmission::CapacityRejected:
    decision.action = rotationAllowed ? DirectSlotStorageAction::Rotate
                                      : DirectSlotStorageAction::LegacyFallback;
    break;
  case DirectContinuationAdmission::StructuralRejected:
    decision.action = DirectSlotStorageAction::LegacyFallback;
    break;
  }
  return decision;
}

}  // namespace dxmt9::core
