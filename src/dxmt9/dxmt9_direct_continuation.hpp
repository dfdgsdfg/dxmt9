#pragma once

#include "dxmt9_chunk_slot_capacity.hpp"
#include "dxmt9_source_payload.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace dxmt9 {
class CommandQueue;
struct DirectSpanAdmissionWitnessTestAccess;
}

namespace dxmt9::core {

enum class DirectSpanStorageAction : std::uint8_t {
  ExactFit,
  Provisioned,
  ReusedProvisioned,
  AppendInPlace,
  Rotated,
};

enum class DirectSpanCapacityReceiptKind : std::uint8_t {
  ExactFitUnqualified,
  LedgerQualified,
};

class DirectSpanCapacityReceipt {
 public:
  static constexpr DirectSpanCapacityReceipt exactFitUnqualified() noexcept {
    return {};
  }

  static constexpr DirectSpanCapacityReceipt ledgerQualified(
      std::uint32_t payloadIndex, std::uint64_t generation,
      std::uint64_t retainedBytes) noexcept {
    DirectSpanCapacityReceipt result;
    result.kind_ = DirectSpanCapacityReceiptKind::LedgerQualified;
    result.payloadIndex_ = payloadIndex;
    result.generation_ = generation;
    result.retainedBytes_ = retainedBytes;
    return result;
  }

  constexpr DirectSpanCapacityReceiptKind kind() const noexcept {
    return kind_;
  }
  constexpr bool qualified() const noexcept {
    return kind_ == DirectSpanCapacityReceiptKind::LedgerQualified;
  }
  constexpr std::uint32_t payloadIndex() const noexcept {
    return payloadIndex_;
  }
  constexpr std::uint64_t generation() const noexcept { return generation_; }
  constexpr std::uint64_t retainedBytes() const noexcept {
    return retainedBytes_;
  }
  constexpr bool valid() const noexcept {
    return kind_ == DirectSpanCapacityReceiptKind::ExactFitUnqualified ||
        (payloadIndex_ != std::numeric_limits<std::uint32_t>::max() &&
         generation_ != 0 && retainedBytes_ != 0);
  }

  friend constexpr bool operator==(const DirectSpanCapacityReceipt&,
                                   const DirectSpanCapacityReceipt&) = default;

 private:
  DirectSpanCapacityReceiptKind kind_ =
      DirectSpanCapacityReceiptKind::ExactFitUnqualified;
  std::uint32_t payloadIndex_ = std::numeric_limits<std::uint32_t>::max();
  std::uint64_t generation_ = 0;
  std::uint64_t retainedBytes_ = 0;
};

enum class DirectSpanProducerIntervalKind : std::uint8_t {
  Unqualified = 0,
  Qualified,
};

struct DirectSpanProducerInterval {
  DirectSpanProducerIntervalKind kind =
      DirectSpanProducerIntervalKind::Unqualified;
  std::uint64_t firstEventOrdinal = 0;
  std::uint64_t lastEventOrdinal = 0;
  std::uint64_t firstSourceOrdinal = 0;
  std::uint64_t lastSourceOrdinal = 0;

  constexpr bool valid() const noexcept {
    return kind == DirectSpanProducerIntervalKind::Unqualified
        ? firstEventOrdinal == 0 && lastEventOrdinal == 0 &&
            firstSourceOrdinal == 0 && lastSourceOrdinal == 0
        : firstEventOrdinal != 0 && lastEventOrdinal >= firstEventOrdinal &&
            firstSourceOrdinal != 0 && lastSourceOrdinal >= firstSourceOrdinal;
  }

  friend constexpr bool operator==(const DirectSpanProducerInterval&,
                                   const DirectSpanProducerInterval&) = default;
};

struct DirectSpanCapacityEvidence {
  SourcePayloadCapacity plan{};
  SourcePayloadCapacity physicalCapacity{};

  friend constexpr bool operator==(const DirectSpanCapacityEvidence&,
                                   const DirectSpanCapacityEvidence&) = default;
};

inline DirectSpanCapacityEvidence directSpanCapacityEvidence(
    const ChunkSlot& slot, const SourcePayloadCapacity& plan) noexcept {
  DirectSpanCapacityEvidence evidence{.plan = plan};
#define DXMT9_SNAPSHOT_DIRECT_SPAN_CAPACITY(                               \
    region, planMember, storage, element, physical, provision, allocation, \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      evidence.physicalCapacity.planMember = slot.storage.capacity();)
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_SNAPSHOT_DIRECT_SPAN_CAPACITY)
#undef DXMT9_SNAPSHOT_DIRECT_SPAN_CAPACITY
  return evidence;
}

inline constexpr bool directSpanCapacityEvidenceCoversPlan(
    const DirectSpanCapacityEvidence& evidence) noexcept {
  bool covers = evidence.plan.commandHeaders != 0;
#define DXMT9_VALIDATE_DIRECT_SPAN_CAPACITY(                               \
    region, planMember, storage, element, physical, provision, allocation, \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      DXMT9_DIRECT_CHUNK_SLOT_EXPAND_ORDINARY_##lookup(                    \
          covers = covers && evidence.physicalCapacity.planMember >=       \
              evidence.plan.planMember;))
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_VALIDATE_DIRECT_SPAN_CAPACITY)
#undef DXMT9_VALIDATE_DIRECT_SPAN_CAPACITY
  // Lookup-table plan members describe logical topology rather than direct
  // `reserve(extra)` arguments. Their exact capacities remain in the witness
  // (and are therefore stale-sensitive), but exact-fit admission is allowed
  // to build them during its one admitted storage action.
  return covers;
}

struct DirectSpanAdmissionIdentity {
  const ChunkSlot* destination = nullptr;
  std::uint32_t schemaRevision = kDirectChunkSlotSchemaRevision;
  DirectSpanStorageAction storageAction = DirectSpanStorageAction::ExactFit;
  bool rotatedBeforeAdmission = false;
  DirectSpanCapacityReceipt capacityReceipt{};
  DirectSpanProducerInterval producerInterval{};
  DirectSpanCapacityEvidence capacityEvidence{};
  std::uint64_t rawOrdinal = 0;
  std::uint32_t spanOrdinal = 0;
  std::uint64_t sourceOrdinal = 0;
  std::uint64_t seqId = 0;
  std::uint64_t buildGeneration = 0;
  std::uint64_t sourceGeneration = 0;
  std::uint64_t storageGeneration = 0;
  std::uint32_t sourceIndex = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t controlIndex = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t firstPage = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t pageCount = 0;

  constexpr bool valid() const noexcept {
    return destination != nullptr &&
        schemaRevision == kDirectChunkSlotSchemaRevision &&
        capacityReceipt.valid() && producerInterval.valid() &&
        directSpanCapacityEvidenceCoversPlan(capacityEvidence) &&
        rawOrdinal != 0 &&
        sourceOrdinal != 0 && seqId != 0 && buildGeneration != 0 &&
        sourceGeneration != 0 && storageGeneration != 0 &&
        sourceIndex != std::numeric_limits<std::uint32_t>::max() &&
        controlIndex != std::numeric_limits<std::uint32_t>::max() &&
        firstPage != std::numeric_limits<std::uint32_t>::max() &&
        pageCount != 0;
  }

  friend constexpr bool operator==(const DirectSpanAdmissionIdentity&,
                                   const DirectSpanAdmissionIdentity&) =
      default;
};

enum class DirectSpanAdmissionConsume : std::uint8_t {
  Consumed,
  Invalid,
  Stale,
  AlreadyConsumed,
};

// Linear proof that one Direct span was admitted against one exact physical
// payload identity. Only CommandQueue's private factory may mint it. Moving
// transfers the single consume edge; stale/ABA and a second consume are typed
// failures, never permission to retry semantic replay.
class DirectSpanAdmissionWitness final {
  friend class dxmt9::CommandQueue;
  friend struct dxmt9::DirectSpanAdmissionWitnessTestAccess;

  struct FactoryIdentity {
    explicit FactoryIdentity(const dxmt9::CommandQueue* owner) noexcept
        : owner(owner) {}
    const dxmt9::CommandQueue* owner = nullptr;
  };

  DirectSpanAdmissionWitness(FactoryIdentity factory,
                             DirectSpanAdmissionIdentity identity) noexcept
      : identity_(identity) {
    issuer_ = factory.owner;
  }

 public:
  DirectSpanAdmissionWitness() = default;
  DirectSpanAdmissionWitness(const DirectSpanAdmissionWitness&) = delete;
  DirectSpanAdmissionWitness& operator=(
      const DirectSpanAdmissionWitness&) = delete;
  DirectSpanAdmissionWitness(DirectSpanAdmissionWitness&& other) noexcept
      : identity_(other.identity_), issuer_(other.issuer_),
        consumed_(other.consumed_) {
    other.identity_ = {};
    other.issuer_ = nullptr;
    other.consumed_ = true;
  }
  DirectSpanAdmissionWitness& operator=(DirectSpanAdmissionWitness&&) = delete;

  bool valid() const noexcept { return !consumed_ && identity_.valid(); }

  DirectSpanAdmissionConsume consume(
      FactoryIdentity issuer,
      const DirectSpanAdmissionIdentity& current) && noexcept {
    if (consumed_) {
      return DirectSpanAdmissionConsume::AlreadyConsumed;
    }
    consumed_ = true;
    if (!identity_.valid()) {
      identity_ = {};
      return DirectSpanAdmissionConsume::Invalid;
    }
    const bool matches = issuer_ != nullptr && issuer.owner == issuer_ &&
        identity_ == current;
    identity_ = {};
    issuer_ = nullptr;
    return matches ? DirectSpanAdmissionConsume::Consumed
                   : DirectSpanAdmissionConsume::Stale;
  }

 private:
  DirectSpanAdmissionIdentity identity_{};
  const dxmt9::CommandQueue* issuer_ = nullptr;
  bool consumed_ = false;
};

static_assert(!std::is_copy_constructible_v<DirectSpanAdmissionWitness>);
static_assert(!std::is_copy_assignable_v<DirectSpanAdmissionWitness>);
static_assert(std::is_nothrow_move_constructible_v<
              DirectSpanAdmissionWitness>);

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
  std::size_t total = 0;
#define DXMT9_PRICE_STAGED_CHUNK_SLOT_DIMENSION(                           \
    region, planMember, storage, element, physical, provision, allocation, \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PROVISION_##provision(                    \
      total += plan.planMember * sizeof(element);)
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(
      DXMT9_PRICE_STAGED_CHUNK_SLOT_DIMENSION)
#undef DXMT9_PRICE_STAGED_CHUNK_SLOT_DIMENSION
  return total;
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
  if (slot.detachedOwnerMarker.active()) {
    return false;
  }
  bool empty = true;
#define DXMT9_EMPTY_CHUNK_SLOT_Ordinary(storage) empty = empty && slot.storage.empty();
#define DXMT9_EMPTY_CHUNK_SLOT_Head(storage)
#define DXMT9_EMPTY_CHUNK_SLOT_Tail(storage)
#define DXMT9_EMPTY_CHUNK_SLOT_Next(storage) empty = empty && slot.storage.empty();
#define DXMT9_CHECK_EMPTY_CHUNK_SLOT_DIMENSION(                            \
    region, plan, storage, element, physical, provision, allocation,       \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      DXMT9_EMPTY_CHUNK_SLOT_##lookup(storage))
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(
      DXMT9_CHECK_EMPTY_CHUNK_SLOT_DIMENSION)
#undef DXMT9_CHECK_EMPTY_CHUNK_SLOT_DIMENSION
#undef DXMT9_EMPTY_CHUNK_SLOT_Next
#undef DXMT9_EMPTY_CHUNK_SLOT_Tail
#undef DXMT9_EMPTY_CHUNK_SLOT_Head
#undef DXMT9_EMPTY_CHUNK_SLOT_Ordinary
  return empty;
}

enum class DirectSlotProvisionAllocation : std::uint8_t {
#define DXMT9_DECLARE_PROVISION_ALLOCATION(                               \
    region, plan, storage, element, physical, provision, allocation,       \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PROVISION_##provision(allocation,)
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_DECLARE_PROVISION_ALLOCATION)
#undef DXMT9_DECLARE_PROVISION_ALLOCATION
  Count,
};
static_assert(static_cast<std::size_t>(DirectSlotProvisionAllocation::Count) ==
              kDirectChunkSlotProvisionDimensionCount);

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
  const auto lookupTable = [&](std::vector<std::uint32_t>& values,
                               std::size_t required,
                               DirectSlotProvisionAllocation allocation) {
    if (required == 0) {
      return;
    }
    if (fault.shouldFail(allocation)) {
      throw std::bad_alloc{};
    }
    values.assign(required, detail::kChunkSlotInvalidUniformIndex);
  };
  try {
#define DXMT9_PROVISION_Ordinary(storage, required, allocation)            \
  reserve(staged.storage, required, allocation);
#define DXMT9_PROVISION_Head(storage, required, allocation)                \
  lookupTable(staged.storage, required, allocation);
#define DXMT9_PROVISION_Tail(storage, required, allocation)                \
  lookupTable(staged.storage, required, allocation);
#define DXMT9_PROVISION_Next(storage, required, allocation)                \
  reserve(staged.storage, required, allocation);
#define DXMT9_PROVISION_CHUNK_SLOT_DIMENSION(                              \
    region, planMember, storage, element, physical, provision, allocation, \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PROVISION_##provision(                    \
      DXMT9_PROVISION_##lookup(                                           \
          storage, plan.planMember, DirectSlotProvisionAllocation::allocation))
    DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(
        DXMT9_PROVISION_CHUNK_SLOT_DIMENSION)
#undef DXMT9_PROVISION_CHUNK_SLOT_DIMENSION
#undef DXMT9_PROVISION_Next
#undef DXMT9_PROVISION_Tail
#undef DXMT9_PROVISION_Head
#undef DXMT9_PROVISION_Ordinary
  } catch (...) {
    return false;
  }

  using std::swap;
#define DXMT9_SWAP_CHUNK_SLOT_DIMENSION(                                   \
    region, plan, storage, element, physical, provision, allocation,       \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      swap(slot.storage, staged.storage);)
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_SWAP_CHUNK_SLOT_DIMENSION)
#undef DXMT9_SWAP_CHUNK_SLOT_DIMENSION
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
#define DXMT9_SWAP_STAGED_CHUNK_SLOT_DIMENSION(                            \
    region, plan, storage, element, physical, provision, allocation,       \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      swap(lhs.storage, rhs.storage);)
    DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(
        DXMT9_SWAP_STAGED_CHUNK_SLOT_DIMENSION)
#undef DXMT9_SWAP_STAGED_CHUNK_SLOT_DIMENSION
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

// ---------------------------------------------------------------------------
// Complete physical-capacity coverage (R-BACK-2.105).
//
// `directContinuationAdmission` answers "may this source extend the populated
// slot"; the two predicates below answer the strictly physical question that
// question presupposes: is every `std::vector` operation
// `TransactionalChunkSlotAssembler::reserve()` is about to perform provably a
// no-op on this payload?
//
// That has to be its own predicate because production disables the assembler's
// per-append capacity guard for range builds
// (`TransactionalChunkSlotAssembler::tryAppendDirectDraw`, gated on
// `directRangeBuild_`, which `beginDirectChunkSlotReplay` always sets). The
// only later guard is the prepare-time conservation check, which runs AFTER
// every append -- i.e. after storage would already have moved. A false positive
// here is therefore unrecoverable, so these arms mirror `reserve()`'s own call
// list and its callees' branch conditions one-for-one rather than paraphrasing
// them.
//
// Unlike `directContinuationAdmission` this covers `readbackRecords` too:
// admission gets away with omitting it only because a non-zero readback plan is
// already a structural rejection, and a predicate that must be callable on the
// empty arm cannot inherit that early return.

// One uniform lookup family. Mirrors `ChunkSlot::reserveDrawUniformStageLookup`
// and `ChunkSlot::reserveDrawUniformPayloadLookup` branch for branch: `next`
// must not grow (`detail::chunkSlotReserveAtLeast` grows GEOMETRICALLY, which
// would move a published prefix's chain storage), and the rebuild must not
// fire, because a rebuild `assign`s the bucket tables and rehashes every
// already-interned record.
template <typename Record>
inline bool directSlotLookupCovers(
    const std::vector<Record>& records,
    const std::vector<std::uint32_t>& heads,
    const std::vector<std::uint32_t>& tails,
    const std::vector<std::uint32_t>& next,
    std::size_t additional) noexcept {
  if (additional == 0) {
    // `reserveDrawUniform*Lookup(0)` returns immediately, so demanding room
    // would be a false negative rather than a safety property.
    return true;
  }
  if (additional > std::numeric_limits<std::size_t>::max() - records.size()) {
    return false;
  }
  const auto total = records.size() + additional;
  if (next.capacity() < total) {
    return false;
  }
  auto bucketCount = detail::chunkSlotUniformLookupBucketCount(total);
  if (bucketCount < heads.size()) {
    bucketCount = heads.size();
  }
  return heads.size() >= bucketCount && heads.size() == tails.size() &&
      next.size() == records.size();
}

// `reserve()`'s own clear-rect accumulation precondition. It is the one
// arm that can fail on a slot whose vectors are all large enough.
inline bool directSlotClearRectAccumulationFits(
    const ChunkSlot& slot) noexcept {
  std::size_t clearRectCount = 0;
  for (const auto& clear : slot.clearRecords) {
    if (clear.rects.size() >
        std::numeric_limits<std::size_t>::max() - clearRectCount) {
      return false;
    }
    clearRectCount += clear.rects.size();
  }
  return true;
}

// True only when every physical vector `reserve()` touches already holds
// `plan`, so the whole reservation is a no-op and no published row moves.
// This is the post-condition of a successful provision or reuse, and the
// pre-condition the direct append path relies on.
inline bool directSlotPhysicalCapacityCovers(
    const ChunkSlot& slot, const SourcePayloadCapacity& plan) noexcept {
  if (slot.prefetchedPipelinesSealed() || !slot.drawStateStorageConsistent() ||
      !slot.commandPayloadsInRange() ||
      !directSlotClearRectAccumulationFits(slot) ||
      slot.detachedOwnerMarker.active()) {
    return false;
  }
  const auto fits = [](const auto& values, std::size_t extra) noexcept {
    return directContinuationAppendFits(values.size(), extra,
                                        values.capacity());
  };
#define DXMT9_COVER_ORDINARY_CHUNK_SLOT_DIMENSION(storage, planMember)      \
  if (!fits(slot.storage, plan.planMember)) {                              \
    return false;                                                          \
  }
#define DXMT9_COVER_CHUNK_SLOT_DIMENSION(                                  \
    region, planMember, storage, element, physical, provision, allocation, \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      DXMT9_DIRECT_CHUNK_SLOT_EXPAND_ORDINARY_##lookup(                    \
          DXMT9_COVER_ORDINARY_CHUNK_SLOT_DIMENSION(storage, planMember)))
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_COVER_CHUNK_SLOT_DIMENSION)
#undef DXMT9_COVER_CHUNK_SLOT_DIMENSION
#undef DXMT9_COVER_ORDINARY_CHUNK_SLOT_DIMENSION
  // `reserve()` gates ALL THREE lookup families on the payload dimension
  // alone, so a zero-payload plan touches none of them.
  if (plan.drawUniformPayloads == 0) {
    return true;
  }
  return directSlotLookupCovers(slot.drawUniformPayloads,
                                slot.drawUniformPayloadLookupHeads,
                                slot.drawUniformPayloadLookupTails,
                                slot.drawUniformPayloadLookupNext,
                                plan.drawUniformPayloads) &&
      directSlotLookupCovers(slot.drawUniformVertexConstants,
                             slot.drawUniformVertexConstantsLookupHeads,
                             slot.drawUniformVertexConstantsLookupTails,
                             slot.drawUniformVertexConstantsLookupNext,
                             plan.drawUniformVertexConstants) &&
      directSlotLookupCovers(slot.drawUniformPixelConstants,
                             slot.drawUniformPixelConstantsLookupHeads,
                             slot.drawUniformPixelConstantsLookupTails,
                             slot.drawUniformPixelConstantsLookupNext,
                             plan.drawUniformPixelConstants);
}

// The reuse arm's pre-condition.
//
// A reclaimed compatibility payload is empty in every direct dimension but is
// NOT byte-identical to a fresh one: `ChunkSlot::clearCommands` calls `clear()`
// on ~30 vectors, which drops every size to zero and retains every capacity.
// The three lookup families' bucket tables therefore keep their storage but
// lose their logical `assign`ed extent, so `directSlotPhysicalCapacityCovers`
// is false on such a payload even when nothing needs to be allocated.
//
// This predicate is the same complete coverage question asked one step
// earlier: it reads bucket-table CAPACITY where the post-condition reads size,
// because `restoreDirectSlotLookupTables` below re-establishes the logical
// extent with no allocation. Every other dimension is checked identically.
inline bool directSlotEmptyStorageReusable(
    const ChunkSlot& slot, const SourcePayloadCapacity& plan) noexcept {
  if (!chunkSlotDirectStorageEmpty(slot) || slot.prefetchedPipelinesSealed() ||
      !slot.drawStateStorageConsistent() || !slot.commandPayloadsInRange()) {
    return false;
  }
  // Reuse must cover the full provisioning plan, not merely the arriving
  // source's exact plan. Accepting partial coverage would silently restore
  // exact-fit behaviour for every source after this one -- the regression
  // R-BACK-2.104 exists to remove -- while still reporting a reuse.
  //
  // Every dimension is empty, so `directContinuationAppendFits(0, extra, cap)`
  // reduces to `cap >= extra`; the plain arms are therefore identical to the
  // post-condition's and only the bucket tables are read differently.
  const auto fits = [](const auto& values, std::size_t extra) noexcept {
    return values.capacity() >= extra;
  };
#define DXMT9_REUSE_ORDINARY_CHUNK_SLOT_DIMENSION(storage, planMember)      \
  if (!fits(slot.storage, plan.planMember)) {                              \
    return false;                                                          \
  }
#define DXMT9_REUSE_CHUNK_SLOT_DIMENSION(                                  \
    region, planMember, storage, element, physical, provision, allocation, \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      DXMT9_DIRECT_CHUNK_SLOT_EXPAND_ORDINARY_##lookup(                    \
          DXMT9_REUSE_ORDINARY_CHUNK_SLOT_DIMENSION(storage, planMember)))
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_REUSE_CHUNK_SLOT_DIMENSION)
#undef DXMT9_REUSE_CHUNK_SLOT_DIMENSION
#undef DXMT9_REUSE_ORDINARY_CHUNK_SLOT_DIMENSION
  if (plan.drawUniformPayloads == 0) {
    return true;
  }
  // Bucket tables are read by CAPACITY: `restoreDirectSlotLookupTables` below
  // re-`assign`s their logical extent, and `assign(n, v)` with
  // `n <= capacity()` allocates nothing.
  const auto lookupCapacity = [](const std::vector<std::uint32_t>& heads,
                                 const std::vector<std::uint32_t>& tails,
                                 const std::vector<std::uint32_t>& next,
                                 std::size_t count) noexcept {
    if (count == 0) {
      return true;
    }
    const auto buckets = detail::chunkSlotUniformLookupBucketCount(count);
    return heads.capacity() >= buckets && tails.capacity() >= buckets &&
        next.capacity() >= count;
  };
  return lookupCapacity(slot.drawUniformPayloadLookupHeads,
                        slot.drawUniformPayloadLookupTails,
                        slot.drawUniformPayloadLookupNext,
                        plan.drawUniformPayloads) &&
      lookupCapacity(slot.drawUniformVertexConstantsLookupHeads,
                     slot.drawUniformVertexConstantsLookupTails,
                     slot.drawUniformVertexConstantsLookupNext,
                     plan.drawUniformVertexConstants) &&
      lookupCapacity(slot.drawUniformPixelConstantsLookupHeads,
                     slot.drawUniformPixelConstantsLookupTails,
                     slot.drawUniformPixelConstantsLookupNext,
                     plan.drawUniformPixelConstants);
}

// Is the aggregate ledger entry for one physical payload still an exact,
// identity-qualified description of that payload's storage? Shared by
// production and the native counterexample so the two cannot drift.
//
// `generation == 0` is the ledger's "no entry" sentinel, so it never
// qualifies. A non-zero `stagedBytes` means some payload holds an outstanding
// lease and the aggregate is mid-transaction. And `retainedBytes` must equal
// the payload's ACTUAL physical retained bytes: any exact-fit growth the
// assembler performed after the last settled provision shows up exactly here.
inline bool directSlotCapacityLedgerQualifiesReuse(
    const DirectSlotCapacityLeaseEntry& entry, std::uint64_t stagedBytes,
    const ChunkSlot& payload) noexcept {
  return stagedBytes == 0 && entry.generation != 0 &&
      entry.retainedBytes ==
          static_cast<std::uint64_t>(directSlotPhysicalRetainedBytes(payload));
}

// Re-establish the three lookup families' logical extent on an empty payload
// whose bucket-table CAPACITY already covers `plan`. Routed through the
// production `ChunkSlot` primitive rather than reimplemented, so the reuse arm
// and the staged-provisioning arm cannot drift apart.
//
// `assign(n, v)` with `n <= capacity()` and `chunkSlotReserveAtLeast` with
// `capacity() >= required` are both allocation-free, which is why this may run
// under the queue mutex on a payload the aggregate lease has already priced.
inline bool restoreDirectSlotLookupTables(
    ChunkSlot& slot, const SourcePayloadCapacity& plan) noexcept {
  try {
    slot.provisionEmptyDrawUniformLookup(plan.drawUniformPayloads,
                                         plan.drawUniformVertexConstants,
                                         plan.drawUniformPixelConstants);
  } catch (...) {
    return false;
  }
  return directSlotPhysicalCapacityCovers(slot, plan);
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
#define DXMT9_ADMIT_ORDINARY_CHUNK_SLOT_DIMENSION(storage, planMember)      \
  if (!fits(slot.storage.size(), extra.planMember,                         \
            slot.storage.capacity())) {                                   \
    return {DirectContinuationAdmission::CapacityRejected};                \
  }
#define DXMT9_ADMIT_CHUNK_SLOT_DIMENSION(                                  \
    region, planMember, storage, element, physical, provision, allocation, \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      DXMT9_DIRECT_CHUNK_SLOT_EXPAND_ORDINARY_##lookup(                    \
          DXMT9_ADMIT_ORDINARY_CHUNK_SLOT_DIMENSION(storage, planMember)))
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_ADMIT_CHUNK_SLOT_DIMENSION)
#undef DXMT9_ADMIT_CHUNK_SLOT_DIMENSION
#undef DXMT9_ADMIT_ORDINARY_CHUNK_SLOT_DIMENSION

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
  // Slot is empty in every direct dimension AND the physical storage a prior
  // provision left behind already covers this plan completely. A reclaimed
  // compatibility payload is exactly this shape: `clearCommands()` drops every
  // size to zero and retains every capacity, so re-staging a fresh topology
  // would free and re-malloc storage that is already correct. Restore the
  // lookup families' logical extent -- allocation-free -- and construct.
  //
  // Distinct from `ProvisionEmpty` because it makes NO aggregate-lease
  // transaction: no reconciliation snapshot, no staged credit, no capacity
  // generation advance. The ledger entry already describes these bytes,
  // because nothing about the payload's physical capacity changed.
  ReuseProvisionedEmpty,
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
  // ProvisionEmpty / ReuseProvisionedEmpty only: the source's own exact plan
  // was at or beyond the provisioning ceiling, so the slot was reserved exactly and the next source
  // is a genuine budget rotation. Classified here rather than re-derived at the
  // call site, so a coordinator-only span cannot be counted as at-budget.
  bool sourceExceedsBudget = false;

  constexpr bool constructsIntoCurrentSlot() const noexcept {
    return action == DirectSlotStorageAction::ProvisionEmpty ||
        action == DirectSlotStorageAction::ReuseProvisionedEmpty ||
        action == DirectSlotStorageAction::ProvisionSkippedNonEmpty ||
        action == DirectSlotStorageAction::ProvisionDisabled ||
        action == DirectSlotStorageAction::AppendInPlace;
  }
};

// `reuseQualified` is the one input the reducer cannot derive: whether the
// aggregate ledger entry for THIS physical payload is still identity-qualified
// for its current storage -- a valid generation whose recorded retained bytes
// equal the payload's actual physical retained bytes, with no staged credit
// outstanding. Physical coverage alone is not enough to license skipping the
// lease: a payload can be provisioned once, then grow by ordinary exact-fit
// assembler reservation on a later source whose lease was denied, and a
// smaller plan arriving after the next reclaim would find complete coverage
// while the ledger still described the pre-growth bytes. Defaulting it to
// `false` keeps every caller that does not own a ledger fail-closed on
// `ProvisionEmpty`.
inline DirectSlotStorageDecision directSlotStorageTransition(
    const ChunkSlot& slot, const SourcePayloadCapacity& extra,
    const DirectSlotProvisionBudget& budget,
    bool rotationAllowed, bool reuseQualified = false) noexcept {
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
    decision.provision = directSlotProvisionPlan(extra, budget);
    decision.sourceExceedsBudget =
        directSlotSourceExceedsProvisionBudget(extra, budget);
    // R-BACK-2.105: a reclaimed payload keeps the capacity its previous
    // provision bought. Reprovisioning it is a redundant free+malloc of the
    // whole ~30-vector topology -- up to the declared per-payload ceiling --
    // inside the queue's critical section, and it produces a payload that is
    // physically indistinguishable from the one being discarded.
    decision.action =
        reuseQualified &&
                directSlotEmptyStorageReusable(slot, decision.provision)
            ? DirectSlotStorageAction::ReuseProvisionedEmpty
            : DirectSlotStorageAction::ProvisionEmpty;
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
