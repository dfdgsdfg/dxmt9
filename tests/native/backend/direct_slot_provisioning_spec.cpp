// R-BACK-2.104 — empty-slot storage provisioning for direct final-slot leases.
//
// The regression this pins: the assembler reserved `size() + extra` exactly,
// so every populated slot satisfied `size() == capacity()` and the capacity arm
// of `directContinuationAdmission` was false by construction for every adjacent
// source. Wild GT1 measured 19,854 of 20,631 continuations (96.2%) rotating,
// GT2 21,432 of 21,537 (99.5%), taking command buffers per present from 4.000
// to 12.666 (GT1) and 3.999 to 20.815 (GT2).
//
// Everything here is pure and value-only: no queue, no Metal, no Wine.

#include "dxmt9/dxmt9_direct_continuation.hpp"
#include "dxmt9/dxmt9_cpu_ready_tape.hpp"

#include <array>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <vector>

using namespace dxmt9::core;

namespace dxmt9 {

struct DirectSpanAdmissionWitnessTestAccess {
  static core::DirectSpanAdmissionWitness mint(
      const CommandQueue* factory,
      core::DirectSpanAdmissionIdentity identity) noexcept {
    return core::DirectSpanAdmissionWitness(
        core::DirectSpanAdmissionWitness::FactoryIdentity(factory),
        identity);
  }

  static core::DirectSpanAdmissionConsume consume(
      core::DirectSpanAdmissionWitness&& witness,
      const CommandQueue* factory,
      const core::DirectSpanAdmissionIdentity& identity) noexcept {
    return std::move(witness).consume(
        core::DirectSpanAdmissionWitness::FactoryIdentity(factory), identity);
  }
};

}  // namespace dxmt9

namespace {

template <typename Staged>
concept LvalueCommit = requires(Staged& staged) {
  staged.commit();
};

template <typename Staged, typename Lease>
concept ExternalLeaseCommit = requires(Staged&& staged, Lease&& lease) {
  std::move(staged).commit(std::move(lease));
};

template <typename Staged>
concept StageWithoutLease = requires(ChunkSlot& destination,
                                     SourcePayloadCapacity& plan) {
  Staged::create(destination, plan);
};

template <typename Staged>
concept StageFromLeaseLvalue = requires(LeaseHeld& lease,
                                        SourcePayloadCapacity& plan) {
  Staged::create(lease, plan);
};

static_assert(!std::is_copy_constructible_v<LeaseHeld>);
static_assert(!std::is_copy_assignable_v<LeaseHeld>);
static_assert(!std::is_copy_constructible_v<StagedDirectSlot>);
static_assert(!std::is_copy_assignable_v<StagedDirectSlot>);
static_assert(std::is_nothrow_move_constructible_v<LeaseHeld>);
static_assert(std::is_nothrow_move_constructible_v<StagedDirectSlot>);
static_assert(std::is_nothrow_destructible_v<LeaseHeld>);
static_assert(std::is_nothrow_destructible_v<StagedDirectSlot>);
static_assert(!std::is_move_assignable_v<LeaseHeld>);
static_assert(!std::is_move_assignable_v<StagedDirectSlot>);
static_assert(requires(StagedDirectSlot&& staged) {
  { std::move(staged).commit() } -> std::same_as<DirectSlotCapacityLeaseReceipt>;
});
static_assert(noexcept(std::declval<StagedDirectSlot&&>().commit()));
static_assert(!LvalueCommit<StagedDirectSlot>);
static_assert(!ExternalLeaseCommit<StagedDirectSlot, LeaseHeld>);
static_assert(!StageWithoutLease<StagedDirectSlot>);
static_assert(!StageFromLeaseLvalue<StagedDirectSlot>);
static_assert(!std::is_copy_constructible_v<DirectSpanAdmissionWitness>);
static_assert(!std::is_copy_assignable_v<DirectSpanAdmissionWitness>);
static_assert(std::is_nothrow_move_constructible_v<
              DirectSpanAdmissionWitness>);
static_assert(!std::is_move_assignable_v<DirectSpanAdmissionWitness>);
static_assert(!std::is_copy_constructible_v<
              DetachedCompatibilityOwnerToken>);
static_assert(std::is_nothrow_move_constructible_v<
              DetachedCompatibilityOwnerToken>);

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

DirectSlotProvisionBudget candidateBudget() {
  return kDirectSlotCandidateBudget;
}

// The exact plan the producer emits for one lease span: one row per draw in
// every one-per-draw dimension, derived dimensions from the draw total, plus
// optional coordinator locators.
SourcePayloadCapacity exactSpanPlan(std::size_t draws,
                                    std::size_t clears = 0,
                                    std::size_t presents = 0,
                                    std::size_t payloadBytesPerDraw = 0) {
  SourcePayloadCapacity capacity{};
  capacity.drawPayloadBytes = draws * payloadBytesPerDraw;
  capacity.drawHotStates = draws;
  capacity.drawShaderLayouts = draws;
  capacity.drawDebugSnapshots = draws;
  capacity.drawPsoSubviews = draws;
  capacity.drawUniformFixedPayloads = draws;
  capacity.drawUniformVertexConstants = draws;
  capacity.drawUniformPixelConstants = draws;
  capacity.drawUniformPayloads = draws;
  capacity.drawParams = draws;
  capacity.drawRunRecords = draws;
  capacity.clearRecords = clears;
  capacity.presentRecords = presents;
  capacity.commandHeaders = draws + clears + presents;
  applyDerivedDrawCapacity(capacity, draws);
  return capacity;
}

// Invoke the real production primitive. There is deliberately no test-local
// copy of the reservation sequence.
void provision(ChunkSlot& slot, const SourcePayloadCapacity& plan) {
  check(provisionEmptyDirectSlotStorage(slot, plan),
        "the production staged provisioning primitive succeeds");
}

// Grow a slot by one source's exact plan without inspecting capacity, the way
// the assembler's appends do. Values are irrelevant here; only the SoA
// dimensions and the storage addresses are under test.
void appendExactPlan(ChunkSlot& slot, const SourcePayloadCapacity& extra) {
  slot.commandHeaders.resize(slot.commandHeaders.size() + extra.commandHeaders);
  slot.drawHotStates.resize(slot.drawHotStates.size() + extra.drawHotStates);
  slot.drawShaderLayouts.resize(slot.drawShaderLayouts.size() +
                                extra.drawShaderLayouts);
  slot.drawDebugSnapshots.resize(slot.drawDebugSnapshots.size() +
                                 extra.drawDebugSnapshots);
  slot.drawPsoSubviews.resize(slot.drawPsoSubviews.size() +
                              extra.drawPsoSubviews);
  slot.drawUniformFixedPayloads.resize(slot.drawUniformFixedPayloads.size() +
                                       extra.drawUniformFixedPayloads);
  slot.drawUniformVertexConstants.resize(
      slot.drawUniformVertexConstants.size() +
      extra.drawUniformVertexConstants);
  slot.drawUniformVertexConstantBytes.resize(
      slot.drawUniformVertexConstantBytes.size() +
      extra.drawUniformVertexConstantBytes);
  slot.drawUniformPixelConstants.resize(slot.drawUniformPixelConstants.size() +
                                        extra.drawUniformPixelConstants);
  slot.drawUniformPixelConstantBytes.resize(
      slot.drawUniformPixelConstantBytes.size() +
      extra.drawUniformPixelConstantBytes);
  slot.drawUniformPayloads.resize(slot.drawUniformPayloads.size() +
                                  extra.drawUniformPayloads);
  slot.drawUniformPayloadLookupNext.resize(
      slot.drawUniformPayloads.size());
  slot.drawUniformVertexConstantsLookupNext.resize(
      slot.drawUniformVertexConstants.size());
  slot.drawUniformPixelConstantsLookupNext.resize(
      slot.drawUniformPixelConstants.size());
  slot.drawParams.resize(slot.drawParams.size() + extra.drawParams);
  slot.drawPayloadArena.resize(slot.drawPayloadArena.size() +
                               extra.drawPayloadBytes);
  slot.drawRunRecords.resize(slot.drawRunRecords.size() + extra.drawRunRecords);
  slot.clearRecords.resize(slot.clearRecords.size() + extra.clearRecords);
  slot.presentRecords.resize(slot.presentRecords.size() + extra.presentRecords);
}

struct StorageAddresses {
  const void* commandHeaders = nullptr;
  const void* drawHotStates = nullptr;
  const void* drawShaderLayouts = nullptr;
  const void* drawUniformVertexConstantBytes = nullptr;
  const void* drawUniformPixelConstantBytes = nullptr;
  const void* drawUniformPayloads = nullptr;
  const void* drawParams = nullptr;
  const void* drawRunRecords = nullptr;
  const void* payloadLookupHeads = nullptr;

  friend bool operator==(const StorageAddresses&,
                         const StorageAddresses&) = default;
};

StorageAddresses addressesOf(const ChunkSlot& slot) {
  return StorageAddresses{
      .commandHeaders = slot.commandHeaders.data(),
      .drawHotStates = slot.drawHotStates.data(),
      .drawShaderLayouts = slot.drawShaderLayouts.data(),
      .drawUniformVertexConstantBytes =
          slot.drawUniformVertexConstantBytes.data(),
      .drawUniformPixelConstantBytes =
          slot.drawUniformPixelConstantBytes.data(),
      .drawUniformPayloads = slot.drawUniformPayloads.data(),
      .drawParams = slot.drawParams.data(),
      .drawRunRecords = slot.drawRunRecords.data(),
      .payloadLookupHeads = slot.drawUniformPayloadLookupHeads.data(),
  };
}

struct VectorTopology {
  const void* data = nullptr;
  std::size_t size = 0;
  std::size_t capacity = 0;

  friend bool operator==(const VectorTopology&,
                         const VectorTopology&) = default;
};

std::array<VectorTopology, kDirectChunkSlotPhysicalDimensionCount> topologyOf(
    const ChunkSlot& slot) {
  const auto entry = [](const auto& values) {
    return VectorTopology{values.data(), values.size(), values.capacity()};
  };
  return {{
#define DXMT9_TEST_TOPOLOGY_DIMENSION(                                     \
    region, plan, storage, element, physical, provision, allocation,       \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(entry(slot.storage),)
      DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_TEST_TOPOLOGY_DIMENSION)
#undef DXMT9_TEST_TOPOLOGY_DIMENSION
  }};
}

using CapacityShrinker = void (*)(ChunkSlot&);

constexpr std::array<CapacityShrinker,
                     kDirectChunkSlotPhysicalDimensionCount>
    kPhysicalCapacityShrinkers = {{
#define DXMT9_TEST_PHYSICAL_SHRINKER(                                      \
    region, plan, storage, element, physical, provision, allocation,       \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      +[](ChunkSlot& slot) { decltype(slot.storage){}.swap(slot.storage); },)
        DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(
            DXMT9_TEST_PHYSICAL_SHRINKER)
#undef DXMT9_TEST_PHYSICAL_SHRINKER
    }};

constexpr std::array<CapacityShrinker,
                     kDirectChunkSlotProvisionDimensionCount>
    kProvisionCapacityShrinkers = {{
#define DXMT9_TEST_PROVISION_SHRINKER(                                     \
    region, plan, storage, element, physical, provision, allocation,       \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PROVISION_##provision(                    \
      +[](ChunkSlot& slot) { decltype(slot.storage){}.swap(slot.storage); },)
        DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(
            DXMT9_TEST_PROVISION_SHRINKER)
#undef DXMT9_TEST_PROVISION_SHRINKER
    }};

void schemaInventoryIsClosedAndRoleTagged() {
  check(kSourcePayloadRegionCount == 32,
        "the schema preserves 32 semantic plan regions");
  check(kDirectChunkSlotDimensions.size() == 32,
        "every semantic region has one descriptor row");
  check(kDirectChunkSlotPhysicalDimensionCount == 31 &&
            kPhysicalCapacityShrinkers.size() == 31,
        "the schema preserves 31 physical ChunkSlot vectors");
  check(kDirectChunkSlotProvisionDimensionCount == 30 &&
            kProvisionCapacityShrinkers.size() == 30 &&
            static_cast<std::size_t>(
                DirectSlotProvisionAllocation::Count) == 30,
        "the schema preserves exactly 30 staged allocation and fault sites");
  check(kDirectChunkSlotDimensions[
            static_cast<std::size_t>(SourcePayloadRegion::ReadbackRecords)]
                .provision == DirectChunkSlotProvisionRole::CoverageOnly,
        "readback is a physical coverage-only structural rejection");
  check(kDirectChunkSlotDimensions[
            static_cast<std::size_t>(SourcePayloadRegion::ClearRects)]
                .physical == DirectChunkSlotPhysicalRole::SemanticOnly,
        "clear rects remain a semantic arena region without a ChunkSlot vector");
  check(kDirectChunkSlotDimensions[
            static_cast<std::size_t>(SourcePayloadRegion::DrawShaderLayouts)]
                .owner == DirectChunkSlotOwnerRole::Detached,
        "drawShaderLayouts is the sole detached-owner row");
  std::size_t detached = 0;
  for (const auto& dimension : kDirectChunkSlotDimensions) {
    detached += dimension.owner == DirectChunkSlotOwnerRole::Detached;
  }
  check(detached == 1, "the schema declares exactly one detached owner");
}

std::array<std::vector<std::uint32_t>, 9> lookupValuesOf(
    const ChunkSlot& slot) {
  return {slot.drawUniformPayloadLookupHeads,
          slot.drawUniformPayloadLookupTails,
          slot.drawUniformPayloadLookupNext,
          slot.drawUniformVertexConstantsLookupHeads,
          slot.drawUniformVertexConstantsLookupTails,
          slot.drawUniformVertexConstantsLookupNext,
          slot.drawUniformPixelConstantsLookupHeads,
          slot.drawUniformPixelConstantsLookupTails,
          slot.drawUniformPixelConstantsLookupNext};
}

// ---------------------------------------------------------------------------

// The exact-fit behaviour this requirement removes, stated as a fact so the
// regression cannot come back silently: reserving `size() + extra` leaves a
// populated slot with zero headroom, and the next source always rejects.
void exactFitReservationRejectsEveryAdjacentSource() {
  ChunkSlot slot;
  const auto plan = exactSpanPlan(112);
  provision(slot, plan);  // exact plan, no budget: the pre-2.104 shape
  appendExactPlan(slot, plan);
  const auto again = directContinuationAdmission(slot, plan);
  check(again.capacityRejected(),
        "exact-fit reservation must capacity-reject the adjacent source -- "
        "this is the regression R-BACK-2.104 exists to remove");
}

// The measured shape: GT2's mean lease span is 2,589,619/23,079 = 112 draws and
// GT1's is 2,147,215/23,526 = 91. Several consecutive sources of that size must
// share one slot with no rotation and no reallocation.
void consecutiveMeanSizedSourcesShareOneProvisionedSlot() {
  // {mean lease-span draws, measured draws per present} for GT1 and GT2.
  struct Workload { std::size_t meanDraws; std::size_t drawsPerPresent; };
  for (const Workload workload : {Workload{91, 741}, Workload{112, 1678}}) {
    const auto meanDraws = workload.meanDraws;
    ChunkSlot slot;
    const auto first = exactSpanPlan(meanDraws, /*clears=*/1);
    const auto budget = candidateBudget();
    const auto plan = directSlotProvisionPlan(first, budget);
    check(plan.drawParams >= 2 * meanDraws,
          "the provisioning budget must admit at least two mean-sized adjacent "
          "sources, or it is an inert constant");
    provision(slot, plan);
    const auto reserved = addressesOf(slot);

    std::size_t admitted = 0;
    std::size_t admittedDraws = 0;
    for (int i = 0; i < 16; ++i) {
      const auto source = exactSpanPlan(meanDraws, /*clears=*/1);
      const auto decision = directSlotStorageTransition(
          slot, source, budget, /*rotationAllowed=*/true);
      if (i == 0) {
        // Reuse is fail-closed without an identity-qualified ledger entry
        // (R-BACK-2.105), and this pure case owns no ledger, so the empty arm
        // stays `ProvisionEmpty`.
        check(!decision.populated &&
                  decision.action == DirectSlotStorageAction::ProvisionEmpty,
              "the first source provisions the empty slot");
      } else if (decision.action != DirectSlotStorageAction::AppendInPlace) {
        break;
      }
      appendExactPlan(slot, source);
      ++admitted;
      admittedDraws += meanDraws;
      check(addressesOf(slot) == reserved,
            "appending an admitted source must not move any provisioned "
            "storage");
    }
    check(admitted >= 2,
          "at least two consecutive mean-sized sources must construct into one "
          "slot without a capacity rotation");
    check(admitted >= 16,
          "one present's worth of measured adjacent lease spans (GT1 8.12, "
          "GT2 14.96 per present) must share one slot");
    check(admittedDraws >= workload.drawsPerPresent,
          "one slot must absorb a whole present's measured draw volume "
          "(GT1 741, GT2 1,678 draws per present) so storage adds no "
          "publication the reference lane does not take");
  }
}

// A populated slot is never reallocated, rehashed, or copied -- the property
// the whole design rests on.
void populatedSlotIsNeverReallocated() {
  ChunkSlot slot;
  const auto budget = candidateBudget();
  const auto first = exactSpanPlan(64, /*clears=*/1);
  provision(slot, directSlotProvisionPlan(first, budget));
  appendExactPlan(slot, first);
  const auto reserved = addressesOf(slot);
  const auto headBuckets = slot.drawUniformPayloadLookupHeads.size();

  for (int i = 0; i < 4; ++i) {
    const auto source = exactSpanPlan(64, /*clears=*/1);
    const auto decision = directSlotStorageTransition(
        slot, source, budget, /*rotationAllowed=*/true);
    check(decision.populated, "a slot holding commands is populated");
    check(decision.action == DirectSlotStorageAction::AppendInPlace,
          "an in-budget adjacent source appends in place");
    appendExactPlan(slot, source);
  }
  check(addressesOf(slot) == reserved,
        "no provisioned vector may reallocate while the slot is populated");
  check(slot.drawUniformPayloadLookupHeads.size() == headBuckets,
        "uniform lookup bucket tables must not be rehashed or resized while "
        "the slot is populated");
}

// The bucket tables are the arm that `reserveDrawUniformStageLookup` cannot
// provision: its rebuild clears heads/tails/next when the record vectors are
// empty. Pin the sizes explicitly.
void emptySlotLookupProvisioningSizesBucketTables() {
  ChunkSlot slot;
  const auto budget = candidateBudget();
  const auto plan = directSlotProvisionPlan(exactSpanPlan(112), budget);
  const auto expected =
      detail::chunkSlotUniformLookupBucketCount(plan.drawUniformPayloads);

  slot.reserveDrawUniformPayloadLookup(plan.drawUniformPayloads);
  check(slot.drawUniformPayloadLookupHeads.empty(),
        "reserveDrawUniformPayloadLookup provisions nothing on an empty slot -- "
        "this is why provisionEmptyDrawUniformLookup exists");

  slot.provisionEmptyDrawUniformLookup(plan.drawUniformPayloads,
                                       plan.drawUniformVertexConstants,
                                       plan.drawUniformPixelConstants);
  check(slot.drawUniformPayloadLookupHeads.size() == expected &&
            slot.drawUniformPayloadLookupTails.size() == expected,
        "payload lookup heads/tails must be sized to the provisioned bucket "
        "count");
  check(slot.drawUniformVertexConstantsLookupHeads.size() == expected &&
            slot.drawUniformPixelConstantsLookupHeads.size() == expected,
        "both stage lookup families must be sized to the provisioned bucket "
        "count");
  check(slot.drawUniformPayloadLookupNext.empty() &&
            slot.drawUniformPayloadLookupNext.capacity() >=
                plan.drawUniformPayloads,
        "`next` takes capacity but must keep size 0 so it stays equal to the "
        "record count");
  check(slot.drawUniformPayloadLookupReady(),
        "a provisioned empty slot must still report a consistent lookup");
}

// Provisioning must never under-reserve: a source larger than the budget is
// still reserved exactly, and is a *genuine* budget rotation afterwards.
void oversizedSourceIsReservedExactlyAndOwnsItsSlot() {
  const auto budget = candidateBudget();
  const auto huge = exactSpanPlan(budget.maxDraws * 4);
  const auto plan = directSlotProvisionPlan(huge, budget);
  check(plan.drawParams == huge.drawParams,
        "a source past the budget provisions exactly its own plan");
  check(plan.commandHeaders >= huge.commandHeaders &&
            plan.drawUniformVertexConstantBytes ==
                huge.drawUniformVertexConstantBytes,
        "an oversized source is never under-reserved");

  ChunkSlot slot;
  const auto provisioning = directSlotStorageTransition(
      slot, huge, budget, /*rotationAllowed=*/true);
  check(provisioning.action == DirectSlotStorageAction::ProvisionEmpty &&
            provisioning.sourceExceedsBudget,
        "an at-or-past-budget source is classified by the reducer, not "
        "re-derived at the call site");
  provision(slot, plan);
  appendExactPlan(slot, huge);
  const auto decision = directSlotStorageTransition(
      slot, exactSpanPlan(8), budget, /*rotationAllowed=*/true);
  check(decision.action == DirectSlotStorageAction::Rotate,
        "the source after an at-budget source is a genuine budget rotation");
}

// Rotation must stay reachable and typed; the fix removes the *cause*, not the
// path. With rotation not permitted the decision is a Legacy fallback, never a
// silent in-place append.
void rotationPathRemainsTypedAndReachable() {
  ChunkSlot slot;
  const auto budget = candidateBudget();
  const auto plan = exactSpanPlan(32);
  provision(slot, plan);
  appendExactPlan(slot, plan);
  const auto rotating = directSlotStorageTransition(
      slot, exactSpanPlan(32), budget, /*rotationAllowed=*/true);
  check(rotating.action == DirectSlotStorageAction::Rotate &&
            rotating.admission == DirectContinuationAdmission::CapacityRejected,
        "a capacity-rejected continuation with rotation permitted rotates");
  const auto blocked = directSlotStorageTransition(
      slot, exactSpanPlan(32), budget, /*rotationAllowed=*/false);
  check(blocked.action == DirectSlotStorageAction::LegacyFallback,
        "a capacity-rejected continuation without rotation falls back");
}

// Readback has no direct appender: it must stay a structural rejection and
// must never be provisioned.
void readbackIsNeverProvisionedAndStaysStructural() {
  const auto budget = candidateBudget();
  auto source = exactSpanPlan(16);
  source.readbackRecords = 1;
  const auto plan = directSlotProvisionPlan(source, budget);
  check(plan.readbackRecords == 0, "readback rows are never provisioned");

  ChunkSlot slot;
  provision(slot, directSlotProvisionPlan(exactSpanPlan(16), budget));
  appendExactPlan(slot, exactSpanPlan(16));
  const auto decision = directSlotStorageTransition(
      slot, source, budget, /*rotationAllowed=*/true);
  check(decision.action == DirectSlotStorageAction::LegacyFallback &&
            decision.admission ==
                DirectContinuationAdmission::StructuralRejected,
        "a readback-bearing source stays a structural rejection, not a "
        "capacity question");
}

// A Present- or Clear-bearing adjacent source is the shape that rotated
// unconditionally before this requirement: a populated slot reserved no
// coordinator row at all.
void coordinatorFloorsAdmitPresentAndClearTails() {
  ChunkSlot slot;
  const auto budget = candidateBudget();
  const auto first = exactSpanPlan(48);
  provision(slot, directSlotProvisionPlan(first, budget));
  appendExactPlan(slot, first);
  const auto clearing = directSlotStorageTransition(
      slot, exactSpanPlan(48, /*clears=*/1), budget, /*rotationAllowed=*/true);
  check(clearing.action == DirectSlotStorageAction::AppendInPlace,
        "a Clear-bearing adjacent source must not rotate on the coordinator "
        "dimension alone");
  const auto presenting = directSlotStorageTransition(
      slot, exactSpanPlan(48, /*clears=*/0, /*presents=*/1), budget,
      /*rotationAllowed=*/true);
  check(presenting.action == DirectSlotStorageAction::AppendInPlace,
        "a Present-tail adjacent source must not rotate on the coordinator "
        "dimension alone");
}

// Disabling the budget is a byte-identical rollback to exact-fit reservation.
void disabledBudgetRestoresExactFitBehaviour() {
  DirectSlotProvisionBudget disabled{};
  check(disabled.maxBytes == 0,
        "value-initialized provisioning policy is production-safe and off");
  check(!disabled.enabled(),
        "DXMT9_DIRECT_SLOT_HEADROOM_BYTES=0 disables provisioning");
  ChunkSlot slot;
  const auto decision = directSlotStorageTransition(
      slot, exactSpanPlan(64), disabled, /*rotationAllowed=*/true);
  check(decision.action == DirectSlotStorageAction::ProvisionDisabled,
        "a disabled budget provisions nothing and leaves the transaction's own "
        "exact reserve alone");
}

// The smallest positive override includes coordinator floors as well as two
// draw records. Otherwise the fixed rows consume part of the advertised draw
// budget and the supposedly enabled lane degenerates to one-source exact fit.
void minimumEnabledBudgetStillAdmitsTwoDraws() {
  DirectSlotProvisionBudget budget{};
  budget.maxBytes = kDirectSlotMinHeadroomBytes;
  const auto source = exactSpanPlan(1);
  const auto plan = directSlotProvisionPlan(source, budget);
  check(plan.drawParams >= 2,
        "the minimum positive headroom budget admits a second one-draw "
        "source after coordinator floors are priced");
  check(directSlotProvisionRetainedBytes(plan) <= budget.maxBytes,
        "the minimum positive headroom budget bounds the whole retained "
        "plan");
}

// Provisioning may only reallocate a slot that is empty in every direct
// dimension, not merely one whose command headers are empty.
void provisioningRefusesASlotThatIsNotFullyEmpty() {
  ChunkSlot slot;
  slot.drawParams.resize(1);
  check(slot.commandsEmpty() && !chunkSlotDirectStorageEmpty(slot),
        "a slot may carry rows without command headers");
  const auto decision = directSlotStorageTransition(
      slot, exactSpanPlan(16), candidateBudget(),
      /*rotationAllowed=*/true);
  check(decision.action == DirectSlotStorageAction::ProvisionSkippedNonEmpty,
        "provisioning must decline a slot that is not empty in every direct "
        "dimension");
}

// Heterogeneous sequences: the shape a first-span-proportional rule got wrong
// in both directions. A tiny leading span must not under-provision the slot for
// the mean-sized sources after it, and a large leading span must not
// reintroduce exact fit.
void heterogeneousSequencesStillAppendInPlace() {
  struct Case {
    const char* what;
    std::size_t first;
    std::size_t following;
    std::size_t count;
  };
  // 1-then-mean is the frame-boundary shape (a short Clear-bearing span);
  // 129-then-mean and 741-then-mean sit above the old 128-draw cliff, where a
  // proportional rule reserved the slot exactly and rotated every adjacent
  // source.
  for (const Case testCase : {Case{"tiny first span", 1, 112, 8},
                              Case{"just past the old cliff", 129, 112, 8},
                              Case{"large first span", 741, 112, 4}}) {
    ChunkSlot slot;
    const auto budget = candidateBudget();
    const auto first = exactSpanPlan(testCase.first, /*clears=*/1);
    const auto decision =
        directSlotStorageTransition(slot, first, budget,
                                    /*rotationAllowed=*/true);
    check(decision.action == DirectSlotStorageAction::ProvisionEmpty,
          "the first source provisions the empty slot");
    check(!decision.sourceExceedsBudget,
          "an in-budget first span is not an at-budget source, whatever its "
          "size relative to the following ones");
    provision(slot, decision.provision);
    const auto reserved = addressesOf(slot);
    appendExactPlan(slot, first);

    std::size_t appended = 0;
    for (std::size_t i = 0; i < testCase.count; ++i) {
      const auto source = exactSpanPlan(testCase.following, /*clears=*/1);
      const auto next = directSlotStorageTransition(slot, source, budget,
                                                    /*rotationAllowed=*/true);
      check(next.action == DirectSlotStorageAction::AppendInPlace,
            testCase.what);
      if (next.action != DirectSlotStorageAction::AppendInPlace) {
        break;
      }
      appendExactPlan(slot, source);
      ++appended;
    }
    check(appended == testCase.count,
          "every following mean-sized source in a heterogeneous sequence must "
          "append in place while the combined exact footprint is in budget");
    check(addressesOf(slot) == reserved,
          "a heterogeneous sequence must not move any provisioned storage");
  }
}

// Budget-fixed means budget-fixed: the provisioned draw total does not depend
// on the first span at all, as long as that span is inside the budget. This is
// the property that removes both the cliff and the first-span sensitivity.
void inBudgetFirstSpansAllProvisionTheSameCeiling() {
  const auto budget = candidateBudget();
  const auto reference =
      directSlotProvisionPlan(exactSpanPlan(1), budget).drawParams;
  check(reference > 1, "a one-draw first span must not provision one draw");
  for (const std::size_t draws : {std::size_t{1}, std::size_t{2},
                                  std::size_t{47}, std::size_t{112},
                                  std::size_t{128}, std::size_t{129},
                                  std::size_t{741}, std::size_t{1678}}) {
    const auto plan = directSlotProvisionPlan(exactSpanPlan(draws), budget);
    check(plan.drawParams == reference,
          "every in-budget first span provisions the same budget-fixed draw "
          "ceiling -- no cliff, no first-span proportionality");
    check(plan.drawParams >= 1678,
          "the ceiling must cover a whole measured GT2 present (1,678 draws) "
          "regardless of which span landed on the empty slot");
  }
}

// A coordinator-only span provisions no draw storage. It still gets the
// coordinator floors, so it does not rotate on a coordinator dimension, but
// committing the full draw ceiling on a span that does not draw would retain
// tens of megabytes on unknown demand.
void coordinatorOnlySpanProvisionsNoDrawStorage() {
  const auto budget = candidateBudget();
  const auto source = exactSpanPlan(0, /*clears=*/1);
  const auto plan = directSlotProvisionPlan(source, budget);
  check(plan.drawParams == 0,
        "a coordinator-only span provisions zero draws");
  check(plan.clearRecords >= budget.coordinatorFloor &&
            plan.presentRecords >= budget.presentFloor,
        "a coordinator-only span still gets the coordinator floors");
  check(!directSlotSourceExceedsProvisionBudget(source, budget),
        "a zero-draw span is never reported as an at-budget source");
  check(directSlotProvisionRetainedBytes(plan) <= budget.maxBytes,
        "a coordinator-only plan is trivially inside the ceiling");
}

// The retained-byte price is part of the contract, not a comment. The bound is
// on the WHOLE plan -- headers, coordinator rows, the uniform lookup triples and
// the draw-payload arena included -- so it is asserted with a non-zero per-draw
// payload, which is the case a draw-record-only price cannot fail.
void retainedBytesStayInsideTheDeclaredBound() {
  const auto budget = candidateBudget();
  check(kDirectSlotWorstCaseBytesPerDraw >= sizeof(FlatDrawStateRecord),
        "the per-draw price must include the dominant record");
  for (const std::size_t draws : {std::size_t{0}, std::size_t{1},
                                  std::size_t{91}, std::size_t{112},
                                  std::size_t{512}, std::size_t{2048}}) {
    for (const std::size_t payloadPerDraw :
         {std::size_t{0}, std::size_t{256}, std::size_t{4096},
          std::size_t{65536}}) {
      const auto source =
          exactSpanPlan(draws, /*clears=*/2, /*presents=*/1, payloadPerDraw);
      const auto plan = directSlotProvisionPlan(source, budget);
      check(plan.drawParams >= source.drawParams &&
                plan.drawPayloadBytes >= source.drawPayloadBytes &&
                plan.commandHeaders >= source.commandHeaders,
            "provisioning never under-reserves the source that priced it");
      if (directSlotSourceExceedsProvisionBudget(source, budget)) {
        // The one documented exception: a source larger than the budget is
        // reserved exactly and owns its slot.
        check(plan.drawParams == source.drawParams,
              "an at-or-past-budget source is reserved exactly");
        continue;
      }
      check(directSlotProvisionRetainedBytes(plan) <= budget.maxBytes,
            "an in-budget provisioning plan must stay inside the declared "
            "retained-byte ceiling, payload arena included");
      check(plan.drawParams <= budget.maxDraws,
            "an in-budget provisioning plan stays inside the draw ceiling");
    }
  }
}

// A non-zero per-draw payload must buy fewer draws, not silently multiply the
// retained bytes. This is the case the byte guard exists for.
void payloadBytesReduceTheDrawBudgetInsteadOfBreakingTheCap() {
  const auto budget = candidateBudget();
  const auto dry = directSlotProvisionPlan(exactSpanPlan(64), budget);
  const auto heavy = directSlotProvisionPlan(
      exactSpanPlan(64, /*clears=*/0, /*presents=*/0,
                    /*payloadBytesPerDraw=*/65536),
      budget);
  check(heavy.drawParams < dry.drawParams,
        "a payload-carrying source must be charged for its payload in the draw "
        "ceiling, not after it");
  check(heavy.drawParams >= 64,
        "the payload charge must not under-reserve the source itself");
  check(directSlotProvisionRetainedBytes(heavy) <= budget.maxBytes,
        "a payload-carrying plan stays inside the declared ceiling");
  check(directSlotProvisionRetainedBytes(dry) <= budget.maxBytes,
        "a payload-free plan stays inside the declared ceiling");
}

// The aggregate figure is a property of the design, not a footnote:
// queueCompatibility owns two persistent payloads per control shell.
void perSlotBoundIsStatedPerSlotAndScalesWithTheRing() {
  const auto budget = candidateBudget();
  const auto plan = directSlotProvisionPlan(exactSpanPlan(112), budget);
  const auto perSlot = directSlotProvisionRetainedBytes(plan);
  check(perSlot <= budget.maxBytes,
        "the per-slot ceiling binds the per-slot plan");
  check(perSlot > budget.maxBytes / 2,
        "the budget-fixed plan really does commit most of the declared "
        "ceiling -- the aggregate must be read as 64x this, not as "
        "free headroom");
  check(CpuReadyTapeConfig::queueCompatibility(32)
                .values().compatibilityPayloadCount == 64,
        "the aggregate multiplier is the physical payload count");
}

// The derived dimensions have exactly one owner; a per-source sum would
// under-reserve the non-linear bucket count.
void derivedDimensionsAreDerivedFromTheTotal() {
  SourcePayloadCapacity a{};
  applyDerivedDrawCapacity(a, 100);
  SourcePayloadCapacity b{};
  applyDerivedDrawCapacity(b, 200);
  check(b.drawUniformPayloadLookupHeads >= a.drawUniformPayloadLookupHeads,
        "bucket counts are monotone in the draw total");
  // The bucket count is a power-of-two step function of the draw total, which
  // is exactly why it must be derived once from the total: a per-island sum
  // reproduces it only by accident, and admission compares against the total.
  const auto isPowerOfTwo = [](std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
  };
  check(isPowerOfTwo(a.drawUniformPayloadLookupHeads) &&
            isPowerOfTwo(b.drawUniformPayloadLookupHeads),
        "bucket counts are powers of two");
  check(a.drawUniformPayloadLookupHeads >= 2 * 100 &&
            b.drawUniformPayloadLookupHeads >= 2 * 200,
        "bucket counts keep at least two buckets per record, the property the "
        "admission predicate compares against");
  check(b.drawUniformVertexConstantBytes ==
            200 * sizeof(VertexShaderConstants),
        "constant byte totals are exact");
}

struct FailingAllocation {
  DirectSlotProvisionAllocation target =
      DirectSlotProvisionAllocation::Count;
  bool observed = false;
};

bool failAllocation(void* opaque,
                    DirectSlotProvisionAllocation allocation) noexcept {
  auto& state = *static_cast<FailingAllocation*>(opaque);
  if (allocation != state.target) {
    return false;
  }
  state.observed = true;
  return true;
}

SourcePayloadCapacity allAllocationDimensionsPlan() {
  auto plan = exactSpanPlan(2, /*clears=*/1, /*presents=*/1,
                            /*payloadBytesPerDraw=*/16);
  plan.surfaceCopyRecords = 1;
  plan.stretchRectRecords = 1;
  plan.colorFillRecords = 1;
  plan.depthResolveRecords = 1;
  plan.generateMipmapsRecords = 1;
  plan.commandHeaders = plan.drawParams + plan.clearRecords +
      plan.surfaceCopyRecords + plan.stretchRectRecords +
      plan.colorFillRecords + plan.depthResolveRecords +
      plan.generateMipmapsRecords + plan.presentRecords;
  return plan;
}

void everyProductionAllocationFailureIsAtomic() {
  const auto plan = allAllocationDimensionsPlan();
  constexpr auto count = static_cast<std::size_t>(
      DirectSlotProvisionAllocation::Count);
  check(count == 30, "the production staging surface has exactly 30 allocations");
  for (std::size_t i = 0; i < count; ++i) {
    ChunkSlot slot;
    provision(slot, exactSpanPlan(1));
    const auto before = topologyOf(slot);
    const auto lookupBefore = lookupValuesOf(slot);
    const auto retainedBefore = directSlotPhysicalRetainedBytes(slot);
    FailingAllocation injection{
        .target = static_cast<DirectSlotProvisionAllocation>(i)};
    check(!provisionEmptyDirectSlotStorage(
              slot, plan,
              DirectSlotProvisionFault{&injection, &failAllocation}),
          "each deterministic allocation failure rejects the staged candidate");
    check(injection.observed,
          "each declared allocation dimension is reached by production");
    check(topologyOf(slot) == before && lookupValuesOf(slot) == lookupBefore &&
              directSlotPhysicalRetainedBytes(slot) == retainedBefore &&
              chunkSlotDirectStorageEmpty(slot),
          "allocation failure leaves every live pointer, size, capacity and "
          "lookup topology byte-identical");
    check(provisionEmptyDirectSlotStorage(slot, plan),
          "the same live payload accepts a later complete replacement");
    check(topologyOf(slot) != before && slot.drawUniformPayloadLookupReady(),
          "successful replacement atomically publishes the complete topology");
  }
}

void everyTypedAllocationFailureOwnsAndRollsBackLease() {
  const auto plan = allAllocationDimensionsPlan();
  const auto provision = directSlotProvisionPlan(plan, candidateBudget());
  const auto requested = directSlotProvisionRetainedBytes(provision);
  constexpr auto count = static_cast<std::size_t>(
      DirectSlotProvisionAllocation::Count);
  for (std::size_t i = 0; i < count; ++i) {
    QueueDirectSlotCapacityLeaseLedger ledger{};
    std::array<std::uint64_t, kDirectSlotCapacityPayloadCount> actual{};
    ChunkSlot destination;
    // Keep a real empty-but-reserved live dimension so the typed failure
    // matrix proves pointer/capacity preservation, not only empty vectors.
    destination.commandHeaders.reserve(1);
    actual[0] = directSlotPhysicalRetainedBytes(destination);
    const auto topologyBefore = topologyOf(destination);
    const auto lookupBefore = lookupValuesOf(destination);
    const auto readbackBefore = VectorTopology{
        destination.readbackRecords.data(), destination.readbackRecords.size(),
        destination.readbackRecords.capacity()};
    FailingAllocation injection{
        .target = static_cast<DirectSlotProvisionAllocation>(i)};

    auto lease = LeaseHeld::tryAcquire(
        ledger, destination, actual, 0, requested * 2, requested);
    check(lease.valid() && ledger.stagedBytes != 0,
          "each typed fault case acquires staged aggregate credit");
    const auto entriesAfterAcquire = ledger.entries;
    const auto retainedAfterAcquire = ledger.retainedBytes;
    const auto generationAfterAcquire = ledger.entries[0].generation;
    const auto stagedAfterAcquire = ledger.stagedBytes;
    {
      auto failed = StagedDirectSlot::create(
          std::move(lease), provision,
          DirectSlotProvisionFault{&injection, &failAllocation});
      check(!lease.valid() && !failed.valid() && injection.observed,
            "each typed allocation fault consumes LeaseHeld into failed staging");
      check(ledger.stagedBytes == stagedAfterAcquire &&
                topologyOf(destination) == topologyBefore &&
                lookupValuesOf(destination) == lookupBefore &&
                VectorTopology{destination.readbackRecords.data(),
                               destination.readbackRecords.size(),
                               destination.readbackRecords.capacity()} ==
                    readbackBefore,
            "failed typed staging keeps live storage and staged credit intact "
            "until owner destruction");
    }
    check(ledger.stagedBytes == 0 &&
              ledger.retainedBytes == retainedAfterAcquire &&
              ledger.entries == entriesAfterAcquire &&
              ledger.entries[0].generation == generationAfterAcquire &&
              topologyOf(destination) == topologyBefore &&
              lookupValuesOf(destination) == lookupBefore &&
              VectorTopology{destination.readbackRecords.data(),
                             destination.readbackRecords.size(),
                             destination.readbackRecords.capacity()} ==
                  readbackBefore,
          "typed fault owner destruction rolls back credit and preserves the "
          "live destination");
  }
}

void aggregateLeaseConservesGenerationAndCredit() {
  const auto compatibility = CpuReadyTapeConfig::queueCompatibility(32);
  check(compatibility.values().compatibilityPayloadCount == 64,
        "aggregate policy covers 64 physical payloads, not 32 controls");
  CpuReadyTape tape(compatibility);
  check(tape.compatibilityPayloadRetainedBytes(63).has_value() &&
            *tape.compatibilityPayloadRetainedBytes(63) == 0 &&
            !tape.compatibilityPayloadRetainedBytes(64).has_value(),
        "physical capacity inspection is scalar, bounded, and covers payload 64");

  DirectSlotCapacityLeaseState initial{
      .limitBytes = 500,
      .retainedBytes = 100,
      .entry = {.generation = 7, .retainedBytes = 100},
  };
  const auto observed = reduceDirectSlotCapacityLease(
      initial, DirectSlotCapacityLeaseEvent::Observe, 100);
  check(observed.accepted && observed.next == initial,
        "observing unchanged physical capacity preserves its generation");
  const auto staged = reduceDirectSlotCapacityLease(
      observed.next, DirectSlotCapacityLeaseEvent::Stage, 200);
  check(staged.accepted && staged.next.retainedBytes == 100 &&
            staged.next.stagedBytes == 200 &&
            staged.next.entry.generation == 7 && staged.ticket.valid() &&
            staged.ticket.generation == 7,
        "staging charges new credit without releasing old capacity");
  const auto rolledBack = reduceDirectSlotCapacityLease(
      staged.next, DirectSlotCapacityLeaseEvent::Rollback, 0, staged.ticket);
  check(rolledBack.accepted && rolledBack.next.limitBytes == initial.limitBytes &&
            rolledBack.next.retainedBytes == initial.retainedBytes &&
            rolledBack.next.stagedBytes == 0 &&
            rolledBack.next.entry == initial.entry &&
            !rolledBack.next.stagedTicket.valid(),
        "failure releases only staged credit with no generation leak");

  const auto adopted = reduceDirectSlotCapacityLease(
      staged.next, DirectSlotCapacityLeaseEvent::Adopt, 200, staged.ticket);
  check(adopted.accepted && adopted.next.retainedBytes == 200 &&
            adopted.next.stagedBytes == 0 &&
            adopted.next.entry.retainedBytes == 200 &&
            adopted.next.entry.generation == 8,
        "atomic adoption replaces credit and advances one capacity generation");

  auto deniedState = initial;
  deniedState.limitBytes = 250;
  const auto denied = reduceDirectSlotCapacityLease(
      deniedState, DirectSlotCapacityLeaseEvent::Stage, 200);
  check(!denied.accepted && denied.next == deniedState,
        "replacement is denied when old plus new exceeds the aggregate even "
        "though the final new capacity alone would fit");

  auto overLimitObservation = initial;
  overLimitObservation.limitBytes = 150;
  const auto overLimit = reduceDirectSlotCapacityLease(
      overLimitObservation, DirectSlotCapacityLeaseEvent::Observe, 200);
  check(!overLimit.accepted && overLimit.next == overLimitObservation,
        "observation denies retained physical capacity beyond the aggregate");

  auto reused = adopted.next;
  reused.retainedBytes += 50;  // another physical payload owns this credit
  const auto reuseObservation = reduceDirectSlotCapacityLease(
      reused, DirectSlotCapacityLeaseEvent::Observe, 300);
  check(reuseObservation.accepted &&
            reuseObservation.next.retainedBytes == 350 &&
            reuseObservation.next.entry.generation == 9,
        "physical-payload reuse updates only that payload and conserves peers");

  auto disabled = initial;
  disabled.limitBytes = 0;
  const auto disabledStage = reduceDirectSlotCapacityLease(
      disabled, DirectSlotCapacityLeaseEvent::Stage, 1);
  check(!disabledStage.accepted && disabledStage.next == disabled,
        "zero aggregate policy preserves the exact-fit lane");

  const auto staleAdopt = reduceDirectSlotCapacityLease(
      staged.next, DirectSlotCapacityLeaseEvent::Adopt, 200,
      {.generation = staged.ticket.generation,
       .serial = staged.ticket.serial - 1});
  check(!staleAdopt.accepted && staleAdopt.next == staged.next,
        "a stale generation-qualified adoption cannot settle a stage");
  const auto staleStage = reduceDirectSlotCapacityLease(
      staged.next, DirectSlotCapacityLeaseEvent::Stage, 1);
  check(!staleStage.accepted && staleStage.next == staged.next,
        "a second stage cannot overwrite an outstanding generation ticket");
  const auto staleRollback = reduceDirectSlotCapacityLease(
      staged.next, DirectSlotCapacityLeaseEvent::Rollback, 0,
      {.generation = staged.ticket.generation,
       .serial = staged.ticket.serial - 1});
  check(!staleRollback.accepted && staleRollback.next == staged.next,
        "a stale generation-qualified rollback cannot settle a stage");
  const auto doubleRollback = reduceDirectSlotCapacityLease(
      rolledBack.next, DirectSlotCapacityLeaseEvent::Rollback, 0,
      staged.ticket);
  check(!doubleRollback.accepted && doubleRollback.next == rolledBack.next,
        "a second settlement is rejected after rollback");
  const auto doubleAdopt = reduceDirectSlotCapacityLease(
      adopted.next, DirectSlotCapacityLeaseEvent::Adopt, 200, staged.ticket);
  check(!doubleAdopt.accepted && doubleAdopt.next == adopted.next,
        "a second settlement is rejected after adoption");

  std::array<DirectSlotCapacityLeaseEntry, 64> entries{};
  std::array<std::uint64_t, 64> actual{};
  entries[0] = {.generation = 3, .retainedBytes = 10};
  entries[63] = {.generation = 9, .retainedBytes = 20};
  actual[0] = 15;
  actual[63] = 30;
  const auto reconciled = reconcileDirectSlotCapacityLease(
      entries, actual, 0, 100, 17);
  check(reconciled.accepted && reconciled.state.retainedBytes == 45 &&
            reconciled.state.entry == reconciled.entries[0] &&
            reconciled.entries[0].generation == 4 &&
            reconciled.entries[63].generation == 10,
        "the full physical fold prices an unseen peer and restores the selected "
        "payload instead of leaking the last peer into its ticket");
  const auto currentStage = reduceDirectSlotCapacityLease(
      reconciled.state, DirectSlotCapacityLeaseEvent::Stage, 40);
  check(currentStage.accepted && currentStage.ticket.generation == 4,
        "the stage ticket is qualified by the selected payload generation");

  actual[62] = 80;
  const auto peerOverLimit = reconcileDirectSlotCapacityLease(
      entries, actual, 0, 100, 17);
  check(!peerOverLimit.accepted,
        "unseen peer exact-fit growth that exceeds the aggregate denies "
        "positive provisioning");
}

void typedLeaseTransactionCommitsOrRollsBack() {
  QueueDirectSlotCapacityLeaseLedger ledger{};
  std::array<std::uint64_t, kDirectSlotCapacityPayloadCount> actual{};
  ChunkSlot destination;
  const auto plan = exactSpanPlan(1, /*clears=*/1);
  const auto provision = directSlotProvisionPlan(plan, candidateBudget());
  const auto requested = directSlotProvisionRetainedBytes(provision);

  {
    bool generationAdvanced = false;
    auto lease = LeaseHeld::tryAcquire(
        ledger, destination, actual, 0, requested * 2, requested,
        &generationAdvanced);
    check(lease.valid(), "the typed transaction acquires a Plan-bound lease");
    auto movedLease = LeaseHeld(std::move(lease));
    check(!lease.valid() && movedLease.valid(),
          "moving LeaseHeld preserves its destination-bound capability");
    auto staged = StagedDirectSlot::create(std::move(movedLease), provision);
    check(staged.valid() && !movedLease.valid(),
          "staging consumes LeaseHeld and owns the moved capability");
    const auto receipt = std::move(staged).commit();
    check(receipt.committed && ledger.stagedBytes == 0 &&
              destination.commandHeaders.capacity() >= provision.commandHeaders,
          "rvalue commit atomically adopts storage and clears staged credit");
    const auto secondReceipt = std::move(staged).commit();
    check(!secondReceipt.committed && ledger.stagedBytes == 0,
          "a staged owner can commit only once");
    actual[0] = directSlotPhysicalRetainedBytes(destination);
  }

  const auto retained = ledger.retainedBytes;
  {
    auto lease = LeaseHeld::tryAcquire(
        ledger, destination, actual, 0, requested * 4, requested, nullptr);
    const auto stagedBefore = ledger.stagedBytes;
    FailingAllocation injection{
        .target = DirectSlotProvisionAllocation::CommandHeaders};
    auto failed = StagedDirectSlot::create(
        std::move(lease), provision,
        DirectSlotProvisionFault{&injection, &failAllocation});
    check(!failed.valid() && injection.observed &&
              ledger.stagedBytes == stagedBefore,
          "failed off-slot staging leaves the held credit until owner teardown");
    // failed owns the moved LeaseHeld; destruction rolls back below.
  }
  check(ledger.stagedBytes == 0 && ledger.retainedBytes == retained,
        "uncommitted destruction rolls back without a generation or credit leak");
}

void aggregateReconciliationIncludesReusedReadbackCapacity() {
  ChunkSlot reusedPayload;
  reusedPayload.readbackRecords.reserve(1);
  const auto readbackBytes = directSlotPhysicalRetainedBytes(reusedPayload);
  check(readbackBytes >= sizeof(ReadbackDesc),
        "physical retained-byte pricing includes readback records");

  QueueDirectSlotCapacityLeaseLedger ledger{};
  std::array<std::uint64_t, kDirectSlotCapacityPayloadCount> actual{};
  actual[0] = readbackBytes;
  bool reconciliationAccepted = false;
  const auto lease = LeaseHeld::tryAcquire(
      ledger, reusedPayload, actual, 0, readbackBytes, 1, nullptr, nullptr,
      &reconciliationAccepted);
  check(!lease.valid() && reconciliationAccepted &&
            ledger.retainedBytes == readbackBytes && ledger.stagedBytes == 0,
        "reused readback capacity is reconciled before aggregate stage denial");
}


// ---------------------------------------------------------------------------
// R-BACK-2.105 — retained-capacity reuse on a reclaimed compatibility payload.

DetachedCompatibilityOwnerIdentity detachedIdentity(
    ChunkSlot& slot, u64 seqId = 11,
    std::uint32_t storageGeneration = 13,
    u64 capacityGeneration = 17) {
  const auto receipt = DetachedCapacityReceipt::ledgerQualified(
      9, capacityGeneration, directSlotPhysicalRetainedBytes(slot));
  slot.admissionCapacityReceipt = receipt;
  return {
      .payload = &slot,
      .seqId = seqId,
      .sourceIndex = 3,
      .sourceGeneration = 5,
      .firstPage = 7,
      .pageCount = 1,
      .storageGeneration = storageGeneration,
      .payloadIndex = 9,
      .capacityReceipt = receipt,
  };
}

// The production reclaim pair, exactly as `QueueLifecycleController` runs it:
// move the owner rows out under the queue mutex, clear the payload, destroy
// the owners with the mutex released, then move the emptied buffer back after
// the relock. Modelled here as one function because only the ORDER matters to
// the storage question under test.
void reclaimPayload(ChunkSlot& slot) {
  // Swap out (O(1), leaves the payload's vector specified empty with capacity
  // zero), clear the rest of the payload, release the mutex, destroy the
  // owners, relock and revalidate, then swap the storage back.
  const auto retainedBytes = directSlotPhysicalRetainedBytes(slot);
  const auto identity = detachedIdentity(slot);
  auto deferredReleases = slot.detachResourceOwners(identity);
  check(deferredReleases.valid(),
        "detach returns one identity-qualified move-only owner token");
  check(directSlotPhysicalRetainedBytes(slot) == retainedBytes,
        "retained-byte accounting includes detached owner storage");
  slot.clearCommands();
  check(directSlotPhysicalRetainedBytes(slot) == retainedBytes,
        "clearing logical contents keeps detached capacity accounted");
  // ... queue mutex released here in production ...
  check(deferredReleases.destroyOwners(),
        "the token destroys retained rows without surrendering allocation");
  check(directSlotPhysicalRetainedBytes(slot) == retainedBytes,
        "the empty detached allocation remains accounted before restore");
  // ... relocked and identity-revalidated here ...
  check(std::move(deferredReleases).restore(slot, identity) ==
            DetachedCompatibilityOwnerDisposition::Restored,
        "rvalue restore consumes the exact revalidated token");
  check(directSlotPhysicalRetainedBytes(slot) == retainedBytes,
        "restoring owner storage preserves the exact physical total");
}

std::array<std::size_t, kDirectChunkSlotPhysicalDimensionCount> capacitiesOf(
    const ChunkSlot& slot) {
  const auto& t = topologyOf(slot);
  std::array<std::size_t, kDirectChunkSlotPhysicalDimensionCount> capacities{};
  for (std::size_t i = 0; i < capacities.size(); ++i) {
    capacities[i] = t[i].capacity;
  }
  return capacities;
}

// Fact (B)+(C1): `clearCommands()` retains every capacity, and the owner
// round trip is what keeps `drawShaderLayouts` -- the only provisioned
// dimension reclaim detaches -- from being the one exception. Without the
// restore half, a reclaimed payload could never satisfy complete physical
// coverage.
void reclaimRetainsEveryProvisionedCapacity() {
  const auto budget = candidateBudget();
  const auto source = exactSpanPlan(112, /*clears=*/1, /*presents=*/1);
  const auto plan = directSlotProvisionPlan(source, budget);

  ChunkSlot slot;
  provision(slot, plan);
  appendExactPlan(slot, source);
  const auto beforeCapacities = capacitiesOf(slot);
  const auto layoutsData = slot.drawShaderLayouts.data();

  reclaimPayload(slot);

  check(chunkSlotDirectStorageEmpty(slot),
        "a reclaimed payload is empty in every direct dimension");
  check(capacitiesOf(slot) == beforeCapacities,
        "reclaim retains every provisioned dimension's capacity, including "
        "drawShaderLayouts");
  check(slot.drawShaderLayouts.data() == layoutsData,
        "the owner buffer that comes back is the same allocation");

  // The half that proves the restore is load-bearing rather than decorative.
  ChunkSlot detachedOnly;
  provision(detachedOnly, plan);
  appendExactPlan(detachedOnly, source);
  {
    const auto identity = detachedIdentity(detachedOnly);
    auto owners = detachedOnly.detachResourceOwners(identity);
    detachedOnly.clearCommands();
    check(owners.destroyOwners(), "detached-only rows are destroyed");
    check(std::move(owners).abandon(detachedOnly) ==
              DetachedCompatibilityOwnerDisposition::Abandoned,
          "explicit abandon consumes a token whose allocation is forfeited");
  }
  check(detachedOnly.drawShaderLayouts.empty() &&
            detachedOnly.drawShaderLayouts.capacity() == 0,
        "the swap-based detach leaves the payload's vector specified empty "
        "with capacity zero -- the shape a bytes-only skip rule would miss");
  check(!directSlotEmptyStorageReusable(detachedOnly, plan),
        "a payload whose layout buffer was surrendered is not reusable");
}

// The redundant reprovision this requirement removes: a reclaimed payload
// whose retained storage already covers the plan must be reused in place, and
// the reuse must leave every allocation exactly where it was.
void reclaimedPayloadIsReusedWithoutReallocation() {
  const auto budget = candidateBudget();
  const auto source = exactSpanPlan(112, /*clears=*/1, /*presents=*/1);
  const auto plan = directSlotProvisionPlan(source, budget);

  ChunkSlot slot;
  provision(slot, plan);
  appendExactPlan(slot, source);
  reclaimPayload(slot);

  check(directSlotEmptyStorageReusable(slot, plan),
        "a reclaimed payload's retained storage covers the same plan again");
  check(!directSlotPhysicalCapacityCovers(slot, plan),
        "but its lookup tables lost their logical extent to clearCommands(), "
        "so the post-condition does not hold until they are restored");

  const auto decision = directSlotStorageTransition(
      slot, source, budget, /*rotationAllowed=*/true,
      /*reuseQualified=*/true);
  check(decision.action == DirectSlotStorageAction::ReuseProvisionedEmpty &&
            decision.constructsIntoCurrentSlot(),
        "a qualified reclaimed payload reuses its storage instead of "
        "re-staging an identical topology");
  check(decision.provision.drawParams == plan.drawParams,
        "reuse carries the same provisioning plan as the arm it replaces");

  const auto before = topologyOf(slot);
  check(restoreDirectSlotLookupTables(slot, decision.provision),
        "the lookup restore establishes complete physical coverage");
  const auto after = topologyOf(slot);
  for (std::size_t i = 0; i < before.size(); ++i) {
    check(before[i].data == after[i].data &&
              before[i].capacity == after[i].capacity,
          "restoring the lookup tables allocates nothing and moves nothing");
  }
  check(directSlotPhysicalCapacityCovers(slot, decision.provision),
        "reuse implies the assembler's reserve() is a no-op");

  // And the whole point: the source now appends in place instead of rotating.
  appendExactPlan(slot, source);
  check(directContinuationAdmission(slot, source).admitted(),
        "the reused slot still admits the adjacent source");
}

// Complete physical coverage has 31 rows, one more than provisioning:
// readback is never staged but must be checked non-vacuously so a future
// appender cannot turn the structural rejection into an unsafe omission.
void completePhysicalCoverageRequiresAll31Rows() {
  const auto stagedPlan = allAllocationDimensionsPlan();
  auto coveragePlan = stagedPlan;
  coveragePlan.readbackRecords = 1;
  check(coveragePlan.readbackRecords != 0,
        "the coverage-only readback row is non-vacuous");

  for (std::size_t i = 0; i < kPhysicalCapacityShrinkers.size(); ++i) {
    ChunkSlot slot;
    provision(slot, stagedPlan);
    slot.readbackRecords.reserve(coveragePlan.readbackRecords);
    check(directSlotPhysicalCapacityCovers(slot, coveragePlan),
          "all 31 physical rows cover the complete plan before shrinking");
    kPhysicalCapacityShrinkers[i](slot);
    check(!directSlotPhysicalCapacityCovers(slot, coveragePlan),
          "shrinking any physical row independently rejects coverage");
  }
  check(directContinuationAdmission(ChunkSlot{}, coveragePlan)
            .structuralRejected(),
        "non-zero readback remains structurally rejected before capacity");
}

void directSpanAdmissionWitnessIsLinearAndGenerationQualified() {
  const auto* factoryOwner = reinterpret_cast<const dxmt9::CommandQueue*>(
      static_cast<std::uintptr_t>(1));
  ChunkSlot payload;
  SourcePayloadCapacity plan{};
  plan.commandHeaders = 1;
  plan.drawHotStates = 1;
  payload.commandHeaders.reserve(23);
  payload.drawHotStates.reserve(29);
  DirectSpanAdmissionIdentity identity{
      .destination = &payload,
      .storageAction = DirectSpanStorageAction::Provisioned,
      .capacityReceipt = DirectSpanCapacityReceipt::ledgerQualified(
          9, 17, 31),
      .producerInterval = {
          .kind = dxmt9::core::DirectSpanProducerIntervalKind::Qualified,
          .firstEventOrdinal = 7,
          .lastEventOrdinal = 11,
          .firstSourceOrdinal = 13,
          .lastSourceOrdinal = 17,
      },
      .capacityEvidence = directSpanCapacityEvidence(payload, plan),
      .rawOrdinal = 2,
      .spanOrdinal = 1,
      .sourceOrdinal = 3,
      .seqId = 3,
      .buildGeneration = 5,
      .sourceGeneration = 7,
      .storageGeneration = 11,
      .sourceIndex = 13,
      .controlIndex = 17,
      .firstPage = 19,
      .pageCount = 1,
  };

  auto source = dxmt9::DirectSpanAdmissionWitnessTestAccess::mint(
      factoryOwner, identity);
  auto moved = std::move(source);
  check(!source.valid() && moved.valid(),
        "moving the witness transfers its only live admission");

  const auto* otherFactoryOwner = reinterpret_cast<const dxmt9::CommandQueue*>(
      static_cast<std::uintptr_t>(2));
  auto wrongIssuer = dxmt9::DirectSpanAdmissionWitnessTestAccess::mint(
      factoryOwner, identity);
  check(dxmt9::DirectSpanAdmissionWitnessTestAccess::consume(
            std::move(wrongIssuer), otherFactoryOwner, identity) ==
            DirectSpanAdmissionConsume::Stale &&
            !wrongIssuer.valid(),
        "a distinct factory cannot consume the witness, and the failed "
        "attempt consumes it");
  check(dxmt9::DirectSpanAdmissionWitnessTestAccess::consume(
            std::move(wrongIssuer), otherFactoryOwner, identity) ==
            DirectSpanAdmissionConsume::AlreadyConsumed,
        "a wrong-issuer witness remains single-use after the stale rejection");

  check(dxmt9::DirectSpanAdmissionWitnessTestAccess::consume(
            std::move(moved), factoryOwner, identity) ==
            DirectSpanAdmissionConsume::Consumed,
        "the exact admission identity consumes once");
  check(dxmt9::DirectSpanAdmissionWitnessTestAccess::consume(
            std::move(moved), factoryOwner, identity) ==
            DirectSpanAdmissionConsume::AlreadyConsumed,
        "double consume fails closed");

  auto stale = dxmt9::DirectSpanAdmissionWitnessTestAccess::mint(
      factoryOwner, identity);
  auto staleIdentity = identity;
  ++staleIdentity.capacityEvidence.physicalCapacity.drawHotStates;
  check(dxmt9::DirectSpanAdmissionWitnessTestAccess::consume(
            std::move(stale), factoryOwner, staleIdentity) ==
            DirectSpanAdmissionConsume::Stale,
        "changed non-header capacity rejects a stale witness");

  auto aba = dxmt9::DirectSpanAdmissionWitnessTestAccess::mint(
      factoryOwner, identity);
  auto abaIdentity = identity;
  ++abaIdentity.storageGeneration;
  check(dxmt9::DirectSpanAdmissionWitnessTestAccess::consume(
            std::move(aba), factoryOwner, abaIdentity) ==
            DirectSpanAdmissionConsume::Stale,
        "the same payload address under a new storage generation rejects ABA");
}

void detachedOwnerTokenRejectsMismatchUntilExplicitAbandon() {
  const auto source = exactSpanPlan(2);
  ChunkSlot slot;
  provision(slot, source);
  appendExactPlan(slot, source);
  const auto identity = detachedIdentity(slot);
  const auto retained = directSlotPhysicalRetainedBytes(slot);
  QueueDirectSlotCapacityLeaseLedger ledger{};
  ledger.retainedBytes = retained;
  ledger.entries[identity.payloadIndex] = {
      .generation = identity.capacityReceipt.generation(),
      .retainedBytes = retained,
  };
  const auto ledgerBefore = ledger;
  auto downgraded = identity;
  downgraded.capacityReceipt = DetachedCapacityReceipt::exactFitUnqualified();
  check(!slot.detachResourceOwners(downgraded).valid(),
        "a qualified physical receipt cannot be downgraded at detach");
  auto token = slot.detachResourceOwners(identity);
  auto moved = std::move(token);
  check(!token.valid() && moved.valid(),
        "detached owner identity and allocation move together");
  slot.clearCommands();
  check(moved.destroyOwners(), "detached owner rows are destroyed explicitly");
  auto stale = identity;
  ++stale.storageGeneration;
  check(std::move(moved).restore(slot, stale) ==
            DetachedCompatibilityOwnerDisposition::Rejected,
        "stale storage identity refuses restore without consuming the token");
  check(moved.valid() && slot.detachedOwnerMarker.active() &&
            directSlotPhysicalRetainedBytes(slot) == retained &&
            ledger.retainedBytes == ledgerBefore.retainedBytes &&
            ledger.entries == ledgerBefore.entries,
        "a restore mismatch neither completes reclaim nor loses ledger bytes");
  check(std::move(moved).abandon(slot) ==
            DetachedCompatibilityOwnerDisposition::Abandoned,
        "the poisoned path must explicitly abandon after revalidation fails");
  check(!slot.detachedOwnerMarker.active() &&
            directSlotPhysicalRetainedBytes(slot) < retained &&
            ledger.retainedBytes == ledgerBefore.retainedBytes &&
            ledger.entries == ledgerBefore.entries,
        "explicit abandon releases storage but preserves the poisoned ledger "
        "receipt for fail-closed reconciliation");
}

void detachedOwnerTokenKeepsOwnershipAfterAbandonReject() {
  ChunkSlot slot;
  const auto source = exactSpanPlan(2);
  provision(slot, source);
  appendExactPlan(slot, source);
  const auto identity = detachedIdentity(slot);
  auto token = slot.detachResourceOwners(identity);
  check(token.valid() && token.destroyOwners(),
        "fault fixture detaches and destroys compatibility owners");
  slot.clearCommands();
  ++slot.detachedOwnerMarker.serial;
  check(std::move(token).restore(slot, identity) ==
            DetachedCompatibilityOwnerDisposition::Rejected,
        "stale marker rejects restore");
  check(token.valid() &&
            std::move(token).abandon(slot) ==
                DetachedCompatibilityOwnerDisposition::Rejected,
        "stale marker rejects explicit abandon");
  check(token.valid() && slot.detachedOwnerMarker.active(),
        "a rejected abandon preserves the live token and marker");
  // Token teardown must remain safe in release builds when no explicit
  // abandon can be proven. The queue-level poisoned-retention path keeps this
  // same token alive until queue teardown in production.
}

// The per-dimension anti-drift gate. Remove the retained allocation from
// exactly one of the 30 enumerated provisioning dimensions after reclaim and
// reuse must refuse. Binding the sweep to DirectSlotProvisionAllocation::Count
// keeps the fixture synchronized with that allocation inventory; a newly added
// ChunkSlot vector must first join the inventory before this sweep can cover it.
void reuseRequiresEveryDimensionIndependently() {
  const auto budget = candidateBudget();
  // A non-zero per-draw payload matters: with `drawPayloadBytes == 0` the
  // arena dimension is vacuous and shrinking it could never be observed, so
  // the sweep would silently pass over one of the thirty.
  const auto source = exactSpanPlan(112, /*clears=*/1, /*presents=*/1,
                                    /*payloadBytesPerDraw=*/64);
  const auto plan = directSlotProvisionPlan(source, budget);
  check(plan.drawPayloadBytes != 0 && plan.surfaceCopyRecords != 0 &&
            plan.generateMipmapsRecords != 0 && plan.presentRecords != 0,
        "every swept dimension must be non-zero or the sweep is vacuous");

  static_assert(static_cast<std::size_t>(
                    DirectSlotProvisionAllocation::Count) == 30,
                "the shrink sweep must cover every provisioned dimension");
  for (std::size_t i = 0;
       i < static_cast<std::size_t>(DirectSlotProvisionAllocation::Count);
       ++i) {
    ChunkSlot slot;
    provision(slot, plan);
    appendExactPlan(slot, source);
    reclaimPayload(slot);
    check(directSlotEmptyStorageReusable(slot, plan),
          "the reclaimed payload is reusable before the dimension is shrunk");
    kProvisionCapacityShrinkers[i](slot);
    check(!directSlotEmptyStorageReusable(slot, plan),
          "shrinking any single provisioned dimension refuses reuse");
    const auto decision = directSlotStorageTransition(
        slot, source, budget, /*rotationAllowed=*/true,
        /*reuseQualified=*/true);
    check(decision.action == DirectSlotStorageAction::ProvisionEmpty,
          "an incompletely covered payload falls back to staged provisioning");
  }
}

// Counterexample for the stale-ledger premise. A payload provisioned once and
// then grown by ordinary exact-fit assembler reservation retains MORE bytes
// than the ledger recorded. After a reclaim a smaller plan finds complete
// physical coverage, so coverage alone would license a reuse that skips a
// ledger the aggregate can no longer trust.
void staleLedgerEntryMustNotQualifyReuse() {
  const auto budget = candidateBudget();
  const auto source = exactSpanPlan(112, /*clears=*/1, /*presents=*/1);
  const auto plan = directSlotProvisionPlan(source, budget);

  ChunkSlot slot;
  provision(slot, plan);
  const DirectSlotCapacityLeaseEntry settled{
      .generation = 1,
      .retainedBytes =
          static_cast<std::uint64_t>(directSlotPhysicalRetainedBytes(slot)),
  };
  check(directSlotCapacityLedgerQualifiesReuse(settled, 0, slot),
        "a settled entry qualifies its own payload");

  // Exact-fit growth on a dimension the provision did not cover: a readback
  // reservation is the shape `provisionEmptyDirectSlotStorage` never makes and
  // `TransactionalChunkSlotAssembler::reserve()` still performs.
  slot.readbackRecords.reserve(8);
  check(!directSlotCapacityLedgerQualifiesReuse(settled, 0, slot),
        "exact-fit growth after the last settled provision disqualifies the "
        "ledger entry");

  reclaimPayload(slot);
  check(directSlotEmptyStorageReusable(slot, plan),
        "the grown payload still covers the plan physically -- coverage alone "
        "cannot see the stale ledger");
  const auto qualified = directSlotCapacityLedgerQualifiesReuse(settled, 0,
                                                                slot);
  check(!qualified, "and the ledger entry is still stale after reclaim");
  const auto decision = directSlotStorageTransition(
      slot, source, budget, /*rotationAllowed=*/true, qualified);
  check(decision.action == DirectSlotStorageAction::ProvisionEmpty,
        "an unqualified payload must reprovision so the aggregate lease is "
        "reconciled, not silently reuse a stale ledger");

  // The other two disqualifiers, each independently.
  check(!directSlotCapacityLedgerQualifiesReuse(
            DirectSlotCapacityLeaseEntry{.generation = 0,
                                         .retainedBytes = settled.retainedBytes},
            0, slot),
        "generation 0 is the no-entry sentinel and never qualifies");
  const DirectSlotCapacityLeaseEntry current{
      .generation = 2,
      .retainedBytes =
          static_cast<std::uint64_t>(directSlotPhysicalRetainedBytes(slot)),
  };
  check(directSlotCapacityLedgerQualifiesReuse(current, 0, slot) &&
            !directSlotCapacityLedgerQualifiesReuse(current, 1, slot),
        "an outstanding staged lease disqualifies reuse");
}

// The default-off and exact-fit lanes must be untouched: no reuse arm exists
// without an enabled budget, and the reducer is fail-closed for every caller
// that does not supply a qualified ledger.
void reuseIsAbsentFromTheExactFitLane() {
  DirectSlotProvisionBudget disabled{};
  disabled.maxBytes = 0;
  const auto source = exactSpanPlan(112);
  ChunkSlot slot;
  provision(slot, source);
  appendExactPlan(slot, source);
  reclaimPayload(slot);
  const auto decision = directSlotStorageTransition(
      slot, source, disabled, /*rotationAllowed=*/true,
      /*reuseQualified=*/true);
  check(decision.action == DirectSlotStorageAction::ProvisionDisabled,
        "a disabled budget never reaches the reuse arm");

  const auto budget = candidateBudget();
  const auto plan = directSlotProvisionPlan(source, budget);
  ChunkSlot enabledSlot;
  provision(enabledSlot, plan);
  appendExactPlan(enabledSlot, source);
  reclaimPayload(enabledSlot);
  const auto unqualified = directSlotStorageTransition(
      enabledSlot, source, budget, /*rotationAllowed=*/true);
  check(unqualified.action == DirectSlotStorageAction::ProvisionEmpty,
        "the reuse qualifier defaults to false, so every caller without a "
        "ledger stays on the staged arm");
}

// The lookup arm is the half `clearCommands()` invalidates, so it gets its own
// rows rather than only being covered by the sweep above.
void lookupRestoreIsExactAndAllocationFree() {
  const auto budget = candidateBudget();
  const auto source = exactSpanPlan(112);
  const auto plan = directSlotProvisionPlan(source, budget);
  const auto buckets =
      detail::chunkSlotUniformLookupBucketCount(plan.drawUniformPayloads);

  ChunkSlot slot;
  provision(slot, plan);
  reclaimPayload(slot);
  check(slot.drawUniformPayloadLookupHeads.empty() &&
            slot.drawUniformPayloadLookupHeads.capacity() >= buckets,
        "reclaim clears the bucket tables' extent and keeps their storage");

  const auto heads = slot.drawUniformPayloadLookupHeads.data();
  const auto tails = slot.drawUniformPayloadLookupTails.data();
  check(restoreDirectSlotLookupTables(slot, plan), "restore succeeds");
  check(slot.drawUniformPayloadLookupHeads.size() == buckets &&
            slot.drawUniformPayloadLookupTails.size() == buckets &&
            slot.drawUniformPayloadLookupNext.empty(),
        "the restored tables carry the provisioned bucket count and an empty "
        "next chain");
  check(slot.drawUniformPayloadLookupHeads.data() == heads &&
            slot.drawUniformPayloadLookupTails.data() == tails,
        "the restore reuses the retained bucket storage");
  for (const auto head : slot.drawUniformPayloadLookupHeads) {
    check(head == detail::kChunkSlotInvalidUniformIndex,
          "every restored bucket is the empty sentinel, so no stale chain "
          "survives reclaim");
  }

  // Missing bucket storage is a refusal, not a silent rebuild: growing the
  // tables later would rehash a published prefix. Use an empty-vector swap so
  // the negative witness does not depend on allocator over-reservation.
  ChunkSlot shortTables;
  provision(shortTables, plan);
  reclaimPayload(shortTables);
  decltype(shortTables.drawUniformPayloadLookupHeads){}.swap(
      shortTables.drawUniformPayloadLookupHeads);
  check(!directSlotEmptyStorageReusable(shortTables, plan),
        "missing bucket capacity refuses reuse");

  // A zero-payload plan touches no lookup family at all, mirroring
  // `TransactionalChunkSlotAssembler::reserve()`'s own gate.
  const auto coordinatorOnly = exactSpanPlan(0, /*clears=*/1, /*presents=*/1);
  const auto coordinatorPlan = directSlotProvisionPlan(coordinatorOnly, budget);
  check(coordinatorPlan.drawUniformPayloads == 0, "the span draws nothing");
  ChunkSlot bare;
  provision(bare, coordinatorPlan);
  reclaimPayload(bare);
  check(directSlotEmptyStorageReusable(bare, coordinatorPlan) &&
            directSlotPhysicalCapacityCovers(bare, coordinatorPlan),
        "a coordinator-only plan needs no lookup storage in either predicate");
}

}  // namespace

int main() {
  schemaInventoryIsClosedAndRoleTagged();
  exactFitReservationRejectsEveryAdjacentSource();
  consecutiveMeanSizedSourcesShareOneProvisionedSlot();
  populatedSlotIsNeverReallocated();
  emptySlotLookupProvisioningSizesBucketTables();
  oversizedSourceIsReservedExactlyAndOwnsItsSlot();
  rotationPathRemainsTypedAndReachable();
  readbackIsNeverProvisionedAndStaysStructural();
  coordinatorFloorsAdmitPresentAndClearTails();
  disabledBudgetRestoresExactFitBehaviour();
  minimumEnabledBudgetStillAdmitsTwoDraws();
  provisioningRefusesASlotThatIsNotFullyEmpty();
  heterogeneousSequencesStillAppendInPlace();
  inBudgetFirstSpansAllProvisionTheSameCeiling();
  coordinatorOnlySpanProvisionsNoDrawStorage();
  retainedBytesStayInsideTheDeclaredBound();
  payloadBytesReduceTheDrawBudgetInsteadOfBreakingTheCap();
  perSlotBoundIsStatedPerSlotAndScalesWithTheRing();
  derivedDimensionsAreDerivedFromTheTotal();
  everyProductionAllocationFailureIsAtomic();
  everyTypedAllocationFailureOwnsAndRollsBackLease();
  aggregateLeaseConservesGenerationAndCredit();
  typedLeaseTransactionCommitsOrRollsBack();
  aggregateReconciliationIncludesReusedReadbackCapacity();
  reclaimRetainsEveryProvisionedCapacity();
  reclaimedPayloadIsReusedWithoutReallocation();
  completePhysicalCoverageRequiresAll31Rows();
  directSpanAdmissionWitnessIsLinearAndGenerationQualified();
  detachedOwnerTokenRejectsMismatchUntilExplicitAbandon();
  detachedOwnerTokenKeepsOwnershipAfterAbandonReject();
  reuseRequiresEveryDimensionIndependently();
  staleLedgerEntryMustNotQualifyReuse();
  reuseIsAbsentFromTheExactFitLane();
  lookupRestoreIsExactAndAllocationFree();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("direct slot provisioning spec ok\n");
  return EXIT_SUCCESS;
}
