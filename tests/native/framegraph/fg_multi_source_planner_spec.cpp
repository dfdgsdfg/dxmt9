#include "../../../src/dxmt9/framegraph/fg_multi_source_planner.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"
#include "arena_payload_fixture.hpp"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace {

using namespace dxmt9::framegraph;
using dxmt9::core::ChunkSlot;
using dxmt9::core::CommandPayloadIndex;
using dxmt9::core::DrawDebugSnapshot;
using dxmt9::core::DrawRunCommandRecord;
using dxmt9::core::DrawShaderLayoutContext;
using dxmt9::core::FlatDrawStateRecord;
using dxmt9::core::Handle;
using dxmt9::core::MetalCommandHeader;
using dxmt9::core::MetalCommandKind;
using dxmt9::tests::framegraph::ArenaPayloadFixture;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void appendDrawRun(ChunkSlot& slot, Handle target, Handle sampled = {}) {
  FlatDrawStateRecord hot{};
  hot.colorAttachments[0].handle = target;
  hot.renderTargetMask = target.value != 0 ? 1u : 0u;
  if (sampled.value != 0) {
    hot.textures[0] = sampled;
    hot.textureMask = 1u;
  }
  const auto stateIndex =
      static_cast<std::uint32_t>(slot.drawHotStates.size());
  slot.drawHotStates.push_back(hot);
  slot.drawShaderLayouts.push_back(DrawShaderLayoutContext{});
  slot.drawDebugSnapshots.push_back(DrawDebugSnapshot{});
  slot.drawParams.push_back(dxmt9::core::DrawParam{});

  DrawRunCommandRecord record{};
  record.stateIndex = stateIndex;
  record.firstParam =
      static_cast<std::uint32_t>(slot.drawParams.size() - 1u);
  record.paramCount = 1u;
  const auto recordIndex =
      static_cast<std::uint32_t>(slot.drawRunRecords.size());
  slot.drawRunRecords.push_back(record);
  slot.commandHeaders.push_back(MetalCommandHeader{
      .kind = MetalCommandKind::DrawRun,
      .payloadIndex = CommandPayloadIndex::fromU32(recordIndex),
  });
}

void appendClearColor(ChunkSlot& slot, Handle target) {
  dxmt9::core::ClearDesc clear{};
  clear.colorAttachments[0].handle = target;
  clear.clearColor = true;
  slot.appendClear(clear);
}

ActiveRenderPlanningSeed activeSeed(Handle target) {
  ActiveRenderPlanningSeed seed{};
  seed.targets.color[0] = TextureHandle{target.value};
  seed.targets.color_count = 1u;
  seed.write_dependencies[0] = ResourceHandle{target.value};
  seed.dependency_count = 1u;
  seed.complete = true;
  return seed;
}

void testLegalActiveAThenSourceBThenSourceA() {
  const Handle target_a{0xA000u};
  const Handle target_b{0xB000u};
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, target_b);
  appendDrawRun(second, target_a);
  ArenaPayloadFixture secondArena(second);
  check(secondArena.valid(), "mixed Legacy+Arena fixture publishes");

  const std::array sources{
      MultiSourcePlanningSource{.payload =
                                    dxmt9::core::SourcePayloadView(first)},
      MultiSourcePlanningSource{.payload = secondArena.view()},
  };
  const ActiveRenderPlanningSeed seed = activeSeed(target_a);
  const MultiSourceReplayPlan plan =
      planMultiSourcePassCoalesceReplay(sources, &seed);

  const std::vector<RetainedSourceCommandLocator> expected{
      {.retainedSourceIndex = 1u, .commandIndex = 0u},
      {.retainedSourceIndex = 0u, .commandIndex = 0u},
  };
  check(plan.valid() && plan.reordered() && plan.commands == expected,
        "active A plus represented B|A plans A1,B0");
  check(plan.diagnostics.outcome == MultiSourcePlannerOutcome::Planned &&
            plan.diagnostics.seedApply ==
                MultiSourceSeedApplyDiagnostic::Applied &&
            plan.diagnostics.activeTargetMatch ==
                MultiSourceActiveTargetMatchDiagnostic::Present &&
            plan.diagnostics.firstMatchingCommand ==
                MultiSourceFirstMatchingCommandDiagnostic::DrawRun &&
            plan.diagnostics.firstMatchingPassDistance == 2u &&
            plan.diagnostics.merge ==
                MultiSourceMergeDiagnostic::SeedMerged &&
            plan.diagnostics.optimizerMergeCount == 1u &&
            plan.diagnostics.activeSeedMergeCount == 1u &&
            plan.diagnostics.activeSeedMergeDistanceTotal == 2u &&
            plan.diagnostics.activeSeedMergeDistanceMax == 2u &&
            plan.diagnostics.activeSeedCommandBefore == 0u &&
            plan.diagnostics.activeSeedCommandAfter == 1u &&
            plan.diagnostics.activeSeedEmptyIntervening == 0u &&
            !plan.diagnostics.activeSeedMergeAttributionMissing,
        "planned diagnostics retain seed match and merge mechanism");
}

void testWarCycleFallsBackToNaturalFifo() {
  const Handle target_a{0xA000u};
  const Handle target_b{0xB000u};
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, target_b, target_a);
  appendDrawRun(second, target_a);
  ArenaPayloadFixture firstArena(first);
  ArenaPayloadFixture secondArena(second);
  check(firstArena.valid() && secondArena.valid(),
        "WAR fixtures publish");

  const std::array sources{
      MultiSourcePlanningSource{.payload = firstArena.view()},
      MultiSourcePlanningSource{.payload = secondArena.view()},
  };
  const ActiveRenderPlanningSeed seed = activeSeed(target_a);
  const MultiSourceReplayPlan plan =
      planMultiSourcePassCoalesceReplay(sources, &seed);

  const std::vector<RetainedSourceCommandLocator> expected{
      {.retainedSourceIndex = 0u, .commandIndex = 0u},
      {.retainedSourceIndex = 1u, .commandIndex = 0u},
  };
  check(plan.valid() && !plan.reordered() && plan.commands == expected,
        "seed-to-reader WAR cycle preserves B0,A1 FIFO");
  check(plan.diagnostics.outcome == MultiSourcePlannerOutcome::NoMerge &&
            plan.diagnostics.seedBlockedCycle &&
            !plan.diagnostics.seedSecondNonDraw,
        "WAR fallback reports the seed blocked-cycle mechanism");
}

void testSeedRejectedDiagnostic() {
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, Handle{0xA000u});
  appendDrawRun(second, Handle{0xB000u});
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(first)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(second)},
  };
  ActiveRenderPlanningSeed seed = activeSeed(Handle{0xA000u});
  seed.complete = false;
  const auto plan = planMultiSourcePassCoalesceReplay(sources, &seed);
  check(plan.valid() && !plan.reordered() &&
            plan.diagnostics.outcome ==
                MultiSourcePlannerOutcome::SeedRejected &&
            plan.diagnostics.seedApply ==
                MultiSourceSeedApplyDiagnostic::Incomplete,
        "incomplete seed reports rejection without changing FIFO replay");
}

void testNoActiveTargetMatchDiagnostic() {
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, Handle{0xB000u});
  appendDrawRun(second, Handle{0xC000u});
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(first)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(second)},
  };
  const ActiveRenderPlanningSeed seed = activeSeed(Handle{0xA000u});
  const auto plan = planMultiSourcePassCoalesceReplay(sources, &seed);
  check(plan.valid() && !plan.reordered() &&
            plan.diagnostics.outcome ==
                MultiSourcePlannerOutcome::NoActiveTargetMatch &&
            plan.diagnostics.activeTargetMatch ==
                MultiSourceActiveTargetMatchDiagnostic::Absent &&
            plan.diagnostics.firstMatchingCommand ==
                MultiSourceFirstMatchingCommandDiagnostic::None &&
            plan.diagnostics.firstMatchingPassDistance == 0u,
        "absent active attachment match is distinct from a hazard blocker");
}

void testSeedSecondNonDrawDiagnostic() {
  ChunkSlot first;
  ChunkSlot second;
  appendClearColor(first, Handle{0xA000u});
  appendDrawRun(second, Handle{0xB000u});
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(first)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(second)},
  };
  const ActiveRenderPlanningSeed seed = activeSeed(Handle{0xA000u});
  const auto plan = planMultiSourcePassCoalesceReplay(sources, &seed);
  check(plan.valid() && !plan.reordered() &&
            plan.diagnostics.outcome == MultiSourcePlannerOutcome::NoMerge &&
            plan.diagnostics.seedSecondNonDraw &&
            plan.diagnostics.firstMatchingCommand ==
                MultiSourceFirstMatchingCommandDiagnostic::NonDraw &&
            plan.diagnostics.firstMatchingPassDistance == 1u,
        "matching Clear reports the second-nondraw seed blocker");
}

void testSeedMergedNaturalOrderDiagnostic() {
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, Handle{0xA000u});
  appendDrawRun(second, Handle{0xB000u});
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(first)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(second)},
  };
  const ActiveRenderPlanningSeed seed = activeSeed(Handle{0xA000u});
  const auto plan = planMultiSourcePassCoalesceReplay(
      sources, &seed, ResourceAliasResolver{}, true);
  check(plan.valid() && !plan.reordered() &&
            plan.diagnostics.outcome ==
                MultiSourcePlannerOutcome::NaturalAfterMerge &&
            plan.diagnostics.merge ==
                MultiSourceMergeDiagnostic::SeedMerged &&
            plan.diagnostics.firstMatchingPassDistance == 1u &&
            plan.diagnostics.optimizerMergeCount == 1u &&
            plan.diagnostics.activeSeedMergeCount == 1u &&
            plan.diagnostics.activeSeedMergeDistanceTotal == 1u &&
            plan.diagnostics.activeSeedMergeDistanceMax == 1u &&
            plan.diagnostics.activeSeedCommandBefore == 0u &&
            plan.diagnostics.activeSeedCommandAfter == 0u &&
            plan.diagnostics.activeSeedEmptyIntervening == 0u &&
            !plan.diagnostics.activeSeedMergeAttributionMissing &&
            !plan.diagnostics.activeSeedMergeWitnessOverflow &&
            !plan.diagnostics.activeSeedMergeWitnessMismatch &&
            plan.diagnostics.activeSeedMergeWitnesses ==
                std::vector<dxmt9::encoders::ActiveSeedMergeTargetWitness>{
                    {.retainedSourceIndex = 0u,
                     .commandIndex = 0u,
                     .mergeOrdinal = 0u,
                     .mergeDistance = 1u}},
        "seed merge that preserves FIFO is distinct from no merge");

}

void testSeedMergedNaturalOrderWithInterveningProducerDiagnostic() {
  const Handle targetA{0xA000u};
  const Handle targetB{0xB000u};
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, targetB);
  appendDrawRun(second, targetA, targetB);
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(first)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(second)},
  };
  const ActiveRenderPlanningSeed seed = activeSeed(targetA);
  const auto plan = planMultiSourcePassCoalesceReplay(
      sources, &seed, ResourceAliasResolver{}, true);
  check(plan.valid() && !plan.reordered() &&
            plan.diagnostics.outcome ==
                MultiSourcePlannerOutcome::NaturalAfterMerge &&
            plan.diagnostics.merge ==
                MultiSourceMergeDiagnostic::SeedMerged &&
            plan.diagnostics.firstMatchingPassDistance == 2u &&
            plan.diagnostics.optimizerMergeCount == 1u &&
            plan.diagnostics.activeSeedMergeCount == 1u &&
            plan.diagnostics.activeSeedMergeDistanceTotal == 2u &&
            plan.diagnostics.activeSeedMergeDistanceMax == 2u &&
            plan.diagnostics.activeSeedCommandBefore == 1u &&
            plan.diagnostics.activeSeedCommandAfter == 0u &&
            plan.diagnostics.activeSeedEmptyIntervening == 0u &&
            !plan.diagnostics.activeSeedMergeAttributionMissing &&
            !plan.diagnostics.seedBlockedCycle,
        "producer before returning target is a distinct natural seed merge");
}

void testMultipleActiveSeedMergesAreCounted() {
  const Handle targetA{0xA000u};
  const Handle targetB{0xB000u};
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, targetA);
  appendDrawRun(first, targetB);
  appendDrawRun(second, targetA, targetB);
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(first)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(second)},
  };
  const ActiveRenderPlanningSeed seed = activeSeed(targetA);
  const auto plan = planMultiSourcePassCoalesceReplay(
      sources, &seed, ResourceAliasResolver{}, true);

  check(plan.valid() && !plan.reordered() &&
            plan.diagnostics.outcome ==
                MultiSourcePlannerOutcome::MovedHeadUnproved &&
            plan.diagnostics.merge ==
                MultiSourceMergeDiagnostic::SeedMerged &&
            plan.diagnostics.optimizerMergeCount == 2u &&
            plan.diagnostics.activeSeedMergeCount == 2u &&
            plan.diagnostics.activeSeedMergeDistanceTotal == 3u &&
            plan.diagnostics.activeSeedMergeDistanceMax == 2u &&
            plan.diagnostics.activeSeedCommandBefore == 1u &&
            plan.diagnostics.activeSeedCommandAfter == 0u &&
            plan.diagnostics.activeSeedEmptyIntervening == 0u &&
            !plan.diagnostics.activeSeedMergeAttributionMissing &&
            !plan.diagnostics.activeSeedMergeWitnessOverflow &&
            !plan.diagnostics.activeSeedMergeWitnessMismatch &&
            plan.diagnostics.activeSeedMergeWitnesses ==
                std::vector<dxmt9::encoders::ActiveSeedMergeTargetWitness>{
                    {.retainedSourceIndex = 0u,
                     .commandIndex = 0u,
                     .mergeOrdinal = 0u,
                     .mergeDistance = 1u},
                    {.retainedSourceIndex = 1u,
                     .commandIndex = 0u,
                     .mergeOrdinal = 1u,
                     .mergeDistance = 2u}},
        "fixpoint planner reports both actual active-seed merges");
  check(std::is_sorted(
            plan.diagnostics.activeSeedMergeWitnesses.begin(),
            plan.diagnostics.activeSeedMergeWitnesses.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.retainedSourceIndex, left.commandIndex,
                              left.mergeOrdinal) <
                  std::tie(right.retainedSourceIndex, right.commandIndex,
                           right.mergeOrdinal);
            }),
        "planner publishes source-local subspan order independently of "
        "passcoalesce merge ordinal");
}

void testSeedNaturalDistanceCountersAreJointAndExhaustive() {
  using dxmt9::perf::CpuReadyMultiSourcePlannerMerge;
  using dxmt9::perf::CpuReadyMultiSourcePlannerOutcome;
  using dxmt9::perf::CpuReadyMultiSourceSeedMergeAttribution;
  const auto before =
      dxmt9::perf::test::snapshotCpuReadyMultiSourceSeedNaturalDistance();
  const CpuReadyMultiSourceSeedMergeAttribution adjacent{
      .optimizerMergeCount = 1u,
      .seedMergeCount = 1u,
      .seedMergeDistanceTotal = 1u,
      .seedMergeDistanceMax = 1u,
  };

  dxmt9::perf::recordCpuReadyMultiSourcePlannerOutcome(
      CpuReadyMultiSourcePlannerOutcome::NaturalAfterMerge,
      CpuReadyMultiSourcePlannerMerge::Seed, 0u, adjacent, false, false);
  dxmt9::perf::recordCpuReadyMultiSourcePlannerOutcome(
      CpuReadyMultiSourcePlannerOutcome::NaturalAfterMerge,
      CpuReadyMultiSourcePlannerMerge::Seed, 1u, adjacent, false, false);
  dxmt9::perf::recordCpuReadyMultiSourcePlannerOutcome(
      CpuReadyMultiSourcePlannerOutcome::NaturalAfterMerge,
      CpuReadyMultiSourcePlannerMerge::Seed, 2u, adjacent, false, false);
  dxmt9::perf::recordCpuReadyMultiSourcePlannerOutcome(
      CpuReadyMultiSourcePlannerOutcome::NaturalAfterMerge,
      CpuReadyMultiSourcePlannerMerge::NonSeedOnly, 2u, {}, false, false);
  dxmt9::perf::recordCpuReadyMultiSourcePlannerOutcome(
      CpuReadyMultiSourcePlannerOutcome::Planned,
      CpuReadyMultiSourcePlannerMerge::Seed, 2u, {}, false, false);

  const auto after =
      dxmt9::perf::test::snapshotCpuReadyMultiSourceSeedNaturalDistance();
  check(after.missing - before.missing == 1u &&
            after.adjacent - before.adjacent == 1u &&
            after.intervening - before.intervening == 1u,
        "only natural seed merges classify one exact distance bucket");
}

void testSeedNaturalAttributionCountersClassifyShapes() {
  using dxmt9::perf::CpuReadyMultiSourcePlannerMerge;
  using dxmt9::perf::CpuReadyMultiSourcePlannerOutcome;
  using dxmt9::perf::CpuReadyMultiSourceSeedMergeAttribution;
  const auto before =
      dxmt9::perf::test::snapshotCpuReadyMultiSourceSeedNaturalAttribution();
  const auto record = [](CpuReadyMultiSourceSeedMergeAttribution attribution) {
    dxmt9::perf::recordCpuReadyMultiSourcePlannerOutcome(
        CpuReadyMultiSourcePlannerOutcome::NaturalAfterMerge,
        CpuReadyMultiSourcePlannerMerge::Seed, 1u, attribution, false, false);
  };

  record({.optimizerMergeCount = 1u,
          .seedMergeCount = 1u,
          .seedMergeDistanceTotal = 1u,
          .seedMergeDistanceMax = 1u});
  record({.optimizerMergeCount = 1u,
          .seedMergeCount = 1u,
          .seedMergeDistanceTotal = 2u,
          .seedMergeDistanceMax = 2u,
          .commandBefore = 1u});
  record({.optimizerMergeCount = 1u,
          .seedMergeCount = 1u,
          .seedMergeDistanceTotal = 2u,
          .seedMergeDistanceMax = 2u,
          .emptyIntervening = 1u});
  record({.optimizerMergeCount = 2u,
          .seedMergeCount = 2u,
          .seedMergeDistanceTotal = 3u,
          .seedMergeDistanceMax = 2u,
          .commandBefore = 1u});
  record({.optimizerMergeCount = 1u,
          .seedMergeCount = 1u,
          .seedMergeDistanceTotal = 2u,
          .seedMergeDistanceMax = 2u,
          .commandAfter = 1u});

  const auto after =
      dxmt9::perf::test::snapshotCpuReadyMultiSourceSeedNaturalAttribution();
  check(after.mergeOperations - before.mergeOperations == 6u &&
            after.mergeDistanceTotal - before.mergeDistanceTotal == 10u &&
            after.mergeDistanceMax >= 2u &&
            after.commandBefore - before.commandBefore == 2u &&
            after.commandAfter - before.commandAfter == 1u &&
            after.emptyIntervening - before.emptyIntervening == 1u &&
            after.adjacent - before.adjacent == 1u &&
            after.dependencyKept - before.dependencyKept == 1u &&
            after.commandless - before.commandless == 1u &&
            after.multiMerge - before.multiMerge == 1u &&
            after.missing - before.missing == 1u,
        "natural seed attribution counters classify one causal shape each");
}

void testPermutationValidationRejectsMalformedPlans() {
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, Handle{0xA000u});
  appendDrawRun(second, Handle{0xB000u});
  ArenaPayloadFixture firstArena(first);
  ArenaPayloadFixture secondArena(second);
  check(firstArena.valid() && secondArena.valid(),
        "validation fixtures publish");
  const std::array sources{
      MultiSourcePlanningSource{.payload = firstArena.view()},
      MultiSourcePlanningSource{.payload = secondArena.view()},
  };

  const std::array duplicate{
      RetainedSourceCommandLocator{0u, 0u},
      RetainedSourceCommandLocator{0u, 0u},
  };
  check(validateMultiSourceReplayPermutation(sources, duplicate) ==
            MultiSourceReplayValidation::Duplicate,
        "duplicate locator is rejected");
  const std::array missing{
      RetainedSourceCommandLocator{0u, 0u},
  };
  check(validateMultiSourceReplayPermutation(sources, missing) ==
            MultiSourceReplayValidation::Missing,
        "missing locator is rejected");
  const std::array outOfRangeSource{
      RetainedSourceCommandLocator{0u, 0u},
      RetainedSourceCommandLocator{2u, 0u},
  };
  check(validateMultiSourceReplayPermutation(sources, outOfRangeSource) ==
            MultiSourceReplayValidation::InvalidSource,
        "out-of-range retained-source index is rejected");
  const std::array invalidCommand{
      RetainedSourceCommandLocator{0u, 0u},
      RetainedSourceCommandLocator{1u, 1u},
  };
  check(validateMultiSourceReplayPermutation(sources, invalidCommand) ==
            MultiSourceReplayValidation::InvalidCommand,
        "out-of-range source command is rejected");

  const std::array invalidSources{
      MultiSourcePlanningSource{},
  };
  const MultiSourceReplayPlan invalid =
      planMultiSourcePassCoalesceReplay(invalidSources);
  check(!invalid.valid() && invalid.commands.empty() &&
            invalid.disposition == MultiSourceReplayDisposition::InvalidInput,
        "invalid represented source cannot produce an executable plan");
}

void testCrossSourceNonDrawMovementRejected() {
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, Handle{0xA000u});
  second.appendPresent(dxmt9::core::SwapDesc{}, Handle{0xA000u});
  ArenaPayloadFixture firstArena(first);
  ArenaPayloadFixture secondArena(second);
  check(firstArena.valid() && secondArena.valid(),
        "non-draw fixtures publish");
  const std::array sources{
      MultiSourcePlanningSource{.payload = firstArena.view()},
      MultiSourcePlanningSource{.payload = secondArena.view()},
  };
  const std::array movedPresent{
      RetainedSourceCommandLocator{1u, 0u},
      RetainedSourceCommandLocator{0u, 0u},
  };
  check(validateMultiSourceReplayPermutation(sources, movedPresent) ==
            MultiSourceReplayValidation::NonDrawCrossSourceMovement,
        "Present cannot cross a retained source boundary");
}

void testRejectedReturnCandidateRetainsClearCrossingProof() {
  const Handle targetA{0xA000u};
  const Handle targetB{0xB000u};
  ChunkSlot older;
  ChunkSlot younger;
  appendDrawRun(older, targetA);
  appendClearColor(older, targetB);
  appendDrawRun(older, targetB);
  appendDrawRun(younger, targetA);
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(older)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(younger)},
  };

  const auto plan = planMultiSourcePassCoalesceReplay(sources);
  const std::vector<RetainedSourceCommandLocator> natural{
      {.retainedSourceIndex = 0u, .commandIndex = 0u},
      {.retainedSourceIndex = 0u, .commandIndex = 1u},
      {.retainedSourceIndex = 0u, .commandIndex = 2u},
      {.retainedSourceIndex = 1u, .commandIndex = 0u},
  };
  check(plan.valid() && !plan.reordered() && plan.commands == natural,
        "rejected Clear crossing remains an executable natural FIFO plan");
  check(plan.diagnostics.outcome ==
                MultiSourcePlannerOutcome::PermutationRejected &&
            plan.validation == MultiSourceReplayValidation::Valid,
        "unsafe A,Clear(B),B|A optimization falls back to valid FIFO replay");
}

void testRejectedReturnCandidateRetainsNaturalYoungerSuffix() {
  const Handle targetA{0xA100u};
  const Handle targetB{0xB100u};
  const Handle targetC{0xC100u};
  ChunkSlot older;
  ChunkSlot younger;
  appendDrawRun(older, targetA);
  appendClearColor(older, targetB);
  appendDrawRun(older, targetB);
  appendDrawRun(younger, targetA);
  appendDrawRun(younger, targetC);
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(older)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(younger)},
  };

  const auto plan = planMultiSourcePassCoalesceReplay(sources);
  check(plan.valid() && !plan.reordered() &&
            plan.diagnostics.outcome ==
                MultiSourcePlannerOutcome::PermutationRejected,
        "unsafe A,Clear(B),B | A,C optimization retains natural FIFO replay");
}

void testValidReturnCandidateJointTerminals() {
  const Handle targetA{0xA200u};
  const Handle targetB{0xB200u};
  const Handle targetC{0xC200u};
  ChunkSlot older;
  appendDrawRun(older, targetA);
  appendDrawRun(older, targetB);

  {
    ChunkSlot younger;
    appendDrawRun(younger, targetA);
    const std::array sources{
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(older)},
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(younger)},
    };
    const auto plan = planMultiSourcePassCoalesceReplay(sources);
    check(plan.valid() && plan.reordered() &&
              plan.diagnostics.outcome ==
                  MultiSourcePlannerOutcome::Planned,
          "valid A,B | A return remains an executable reordered plan");
  }

  {
    ChunkSlot younger;
    appendDrawRun(younger, targetA);
    appendDrawRun(younger, targetC);
    const std::array sources{
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(older)},
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(younger)},
    };
    const auto plan = planMultiSourcePassCoalesceReplay(sources);
    check(plan.valid() && plan.reordered() &&
              plan.diagnostics.outcome ==
                  MultiSourcePlannerOutcome::Planned,
          "valid A,B | A,C return remains an executable reordered plan");
  }
}

void testOrderedControlsAndDependenciesDoNotClaimClearReturn() {
  const Handle targetA{0xA000u};
  const Handle targetB{0xB000u};
  {
    ChunkSlot older;
    ChunkSlot younger;
    appendDrawRun(older, targetA);
    older.appendPresent(dxmt9::core::SwapDesc{}, targetB);
    appendDrawRun(older, targetB);
    appendDrawRun(younger, targetA);
    const std::array sources{
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(older)},
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(younger)},
    };
    const auto plan = planMultiSourcePassCoalesceReplay(sources);
    check(plan.valid() && !plan.reordered(),
          "Present boundary retains natural FIFO replay");
  }
  {
    ChunkSlot older;
    ChunkSlot younger;
    appendDrawRun(older, targetA);
    older.appendSurfaceCopy(dxmt9::core::SurfaceCopyDesc{
        .source = targetB,
        .destination = Handle{0xC000u},
    });
    appendDrawRun(older, targetB);
    appendDrawRun(younger, targetA);
    const std::array sources{
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(older)},
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(younger)},
    };
    const auto plan = planMultiSourcePassCoalesceReplay(sources);
    check(plan.valid() && !plan.reordered() &&
              plan.diagnostics.outcome ==
                  MultiSourcePlannerOutcome::NoMerge,
          "SurfaceCopy crossing fails closed to natural replay");
  }
  {
    ChunkSlot older;
    ChunkSlot younger;
    appendDrawRun(older, targetA);
    appendDrawRun(older, targetB, targetA);
    appendDrawRun(younger, targetA);
    const std::array sources{
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(older)},
        MultiSourcePlanningSource{
            .payload = dxmt9::core::SourcePayloadView(younger)},
    };
    const auto plan = planMultiSourcePassCoalesceReplay(sources);
    check(plan.valid() && !plan.reordered(),
          "dependency cycle retains natural replay");
  }
}

void testNonSingleCutCandidateStaysOutsideReturnBucket() {
  const Handle targetA{0xA000u};
  const Handle targetB{0xB000u};
  const Handle targetC{0xC000u};
  ChunkSlot older;
  ChunkSlot middle;
  ChunkSlot younger;
  appendDrawRun(older, targetA);
  appendDrawRun(older, targetB);
  appendDrawRun(middle, targetC);
  appendDrawRun(younger, targetA);
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(older)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(middle)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(younger)},
  };
  const auto plan = planMultiSourcePassCoalesceReplay(sources);
  check(plan.valid(),
        "three-source planning remains a complete validated permutation");
}

void testReplayRunBuilderPreservesSourceSwap() {
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, Handle{0xA000u});
  appendDrawRun(second, Handle{0xB000u});
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(first)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(second)},
  };
  MultiSourceReplayPlan plan{};
  plan.commands = {
      {.retainedSourceIndex = 1u, .commandIndex = 0u},
      {.retainedSourceIndex = 0u, .commandIndex = 0u},
  };
  plan.disposition = MultiSourceReplayDisposition::Planned;
  plan.validation = MultiSourceReplayValidation::Valid;

  const auto runs = buildMultiSourceReplayRuns(sources, plan);
  const std::vector<MultiSourceReplayRun> expected{
      {.retainedSourceIndex = 1u, .commandBegin = 0u, .commandCount = 1u,
       .sourceFragmentOrdinal = 0u, .sourceFragmentCount = 1u,
       .transactionFragmentOrdinal = 0u, .transactionFragmentCount = 2u},
      {.retainedSourceIndex = 0u, .commandBegin = 0u, .commandCount = 1u,
       .sourceFragmentOrdinal = 0u, .sourceFragmentCount = 1u,
       .transactionFragmentOrdinal = 1u, .transactionFragmentCount = 2u},
  };
  check(runs.valid() && runs.runs == expected,
        "B|A replay stays two qualified source runs");
}

void testReplayRunBuilderPreservesRepeatedSource() {
  ChunkSlot first;
  ChunkSlot second;
  appendDrawRun(first, Handle{0xA000u});
  appendDrawRun(first, Handle{0xA000u});
  appendDrawRun(second, Handle{0xB000u});
  const std::array sources{
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(first)},
      MultiSourcePlanningSource{
          .payload = dxmt9::core::SourcePayloadView(second)},
  };
  MultiSourceReplayPlan plan{};
  plan.commands = {
      {.retainedSourceIndex = 0u, .commandIndex = 0u},
      {.retainedSourceIndex = 1u, .commandIndex = 0u},
      {.retainedSourceIndex = 0u, .commandIndex = 1u},
  };
  plan.disposition = MultiSourceReplayDisposition::Planned;
  plan.validation = MultiSourceReplayValidation::Valid;

  const auto runs = buildMultiSourceReplayRuns(sources, plan);
  const std::vector<MultiSourceReplayRun> expected{
      {.retainedSourceIndex = 0u, .commandBegin = 0u, .commandCount = 1u,
       .sourceFragmentOrdinal = 0u, .sourceFragmentCount = 2u,
       .transactionFragmentOrdinal = 0u, .transactionFragmentCount = 3u},
      {.retainedSourceIndex = 1u, .commandBegin = 0u, .commandCount = 1u,
       .sourceFragmentOrdinal = 0u, .sourceFragmentCount = 1u,
       .transactionFragmentOrdinal = 1u, .transactionFragmentCount = 3u},
      {.retainedSourceIndex = 0u, .commandBegin = 1u, .commandCount = 1u,
       .sourceFragmentOrdinal = 1u, .sourceFragmentCount = 2u,
       .transactionFragmentOrdinal = 2u, .transactionFragmentCount = 3u},
  };
  check(runs.valid() && runs.runs == expected &&
            runs.runs.front().firstSourceFragment() &&
            !runs.runs.front().lastSourceFragment() &&
            runs.runs.back().lastSourceFragment(),
        "A|B|A exposes an exact fragment-aware source transaction");

  plan.commands[2] = plan.commands[0];
  const auto malformed = buildMultiSourceReplayRuns(sources, plan);
  check(!malformed.valid() && malformed.runs.empty() &&
            malformed.validation ==
                MultiSourceReplayRunValidation::InvalidPlan,
        "malformed replay never produces a partial run list");
}

}  // namespace

int main() {
  setenv("DXMT_PERF_COUNTERS", "1", 1);
  try {
    testLegalActiveAThenSourceBThenSourceA();
    testWarCycleFallsBackToNaturalFifo();
    testSeedRejectedDiagnostic();
    testNoActiveTargetMatchDiagnostic();
    testSeedSecondNonDrawDiagnostic();
    testSeedMergedNaturalOrderDiagnostic();
    testSeedMergedNaturalOrderWithInterveningProducerDiagnostic();
    testMultipleActiveSeedMergesAreCounted();
    testSeedNaturalDistanceCountersAreJointAndExhaustive();
    testSeedNaturalAttributionCountersClassifyShapes();
    testPermutationValidationRejectsMalformedPlans();
    testCrossSourceNonDrawMovementRejected();
    testRejectedReturnCandidateRetainsClearCrossingProof();
    testRejectedReturnCandidateRetainsNaturalYoungerSuffix();
    testValidReturnCandidateJointTerminals();
    testOrderedControlsAndDependenciesDoNotClaimClearReturn();
    testNonSingleCutCandidateStaysOutsideReturnBucket();
    testReplayRunBuilderPreservesSourceSwap();
    testReplayRunBuilderPreservesRepeatedSource();
  } catch (const std::exception& error) {
    std::cerr << "fg_multi_source_planner_spec: FAIL: " << error.what()
              << '\n';
    return 1;
  }
  std::cout << "fg_multi_source_planner_spec: PASS\n";
  return 0;
}
