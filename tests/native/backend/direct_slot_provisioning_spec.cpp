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

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace dxmt9::core;

namespace {

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

// Reserve a provisioning plan into an empty slot exactly the way
// `provisionEmptyDirectSlotUnlocked` does in the queue. Kept in step with that
// function by `provisioningMatchesQueueReserveShape` below.
void provision(ChunkSlot& slot, const SourcePayloadCapacity& plan) {
  const auto reserve = [](auto& values, std::size_t required) {
    if (values.capacity() < required) {
      values.reserve(required);
    }
  };
  reserve(slot.commandHeaders, plan.commandHeaders);
  reserve(slot.drawHotStates, plan.drawHotStates);
  reserve(slot.drawShaderLayouts, plan.drawShaderLayouts);
  reserve(slot.drawDebugSnapshots, plan.drawDebugSnapshots);
  reserve(slot.drawPsoSubviews, plan.drawPsoSubviews);
  reserve(slot.drawUniformFixedPayloads, plan.drawUniformFixedPayloads);
  reserve(slot.drawUniformVertexConstants, plan.drawUniformVertexConstants);
  reserve(slot.drawUniformVertexConstantBytes,
          plan.drawUniformVertexConstantBytes);
  reserve(slot.drawUniformPixelConstants, plan.drawUniformPixelConstants);
  reserve(slot.drawUniformPixelConstantBytes,
          plan.drawUniformPixelConstantBytes);
  reserve(slot.drawUniformPayloads, plan.drawUniformPayloads);
  reserve(slot.drawParams, plan.drawParams);
  reserve(slot.drawPayloadArena, plan.drawPayloadBytes);
  reserve(slot.drawRunRecords, plan.drawRunRecords);
  reserve(slot.clearRecords, plan.clearRecords);
  reserve(slot.surfaceCopyRecords, plan.surfaceCopyRecords);
  reserve(slot.stretchRectRecords, plan.stretchRectRecords);
  reserve(slot.colorFillRecords, plan.colorFillRecords);
  reserve(slot.depthResolveRecords, plan.depthResolveRecords);
  reserve(slot.generateMipmapsRecords, plan.generateMipmapsRecords);
  reserve(slot.presentRecords, plan.presentRecords);
  slot.provisionEmptyDrawUniformLookup(plan.drawUniformPayloads,
                                       plan.drawUniformVertexConstants,
                                       plan.drawUniformPixelConstants);
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

// The whole-ring figure is a property of the design, not a footnote: the
// compatibility ring is `kCommandChunkCount` slots wide and each may hold a
// full provisioning plan.
void perSlotBoundIsStatedPerSlotAndScalesWithTheRing() {
  const auto budget = candidateBudget();
  const auto plan = directSlotProvisionPlan(exactSpanPlan(112), budget);
  const auto perSlot = directSlotProvisionRetainedBytes(plan);
  check(perSlot <= budget.maxBytes,
        "the per-slot ceiling binds the per-slot plan");
  check(perSlot > budget.maxBytes / 2,
        "the budget-fixed plan really does commit most of the declared "
        "ceiling -- the whole-ring figure must be read as 32x this, not as "
        "free headroom");
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

}  // namespace

int main() {
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
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("direct slot provisioning spec ok\n");
  return EXIT_SUCCESS;
}
