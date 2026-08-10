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

static_assert(noexcept(planDeferredTerminalSuffixReplay(
    DeferredTerminalSuffixPlanningSourceView{},
    DeferredTerminalSuffixPlanningSourceView{},
    ActiveRenderPlanningSeed{})));
static_assert(noexcept(validateDeferredTerminalSuffixReplay(
    DeferredTerminalSuffixPlanningSourceView{},
    DeferredTerminalSuffixPlanningSourceView{}, ActiveRenderPlanningSeed{},
    DeferredTerminalSuffixProof{},
    std::span<const RetainedSourceCommandLocator>{})));
static_assert(sizeof(DeferredTerminalSuffixPlan) <= 1024u);

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

dxmt9::core::CpuReadyTape::SourceRef sourceRef(std::uint32_t index,
                                               std::uint64_t generation,
                                               std::uint32_t firstPage) {
  return {
      .id = {.index = index, .generation = generation},
      .storage = {
          .firstPage = firstPage,
          .pageCount = 1u,
          .generation = generation,
      },
  };
}

DeferredTerminalSuffixPlanningSourceView planningSource(
    const ChunkSlot& slot, std::uint32_t sourceIndex,
    std::uint64_t sourceOrdinal, std::uint64_t seqId) {
  return {
      .payload = dxmt9::core::SourcePayloadView(slot),
      .source = sourceRef(sourceIndex, sourceOrdinal + 10u, sourceIndex + 1u),
      .sourceOrdinal = sourceOrdinal,
      .seqId = seqId,
  };
}

struct AliasPair {
  Handle first{};
  Handle second{};
  Handle canonical{};
};

ResourceHandle resolveAliasPair(const void* context,
                                ResourceHandle handle) noexcept {
  const auto& aliases = *static_cast<const AliasPair*>(context);
  return handle.value == aliases.first.value ||
          handle.value == aliases.second.value
      ? ResourceHandle{aliases.canonical.value}
      : handle;
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

void testDeferredTerminalSuffixProofQualifiesExactShape() {
  const Handle targetA{0xA500u};
  const Handle targetB{0xB500u};
  ChunkSlot current;
  ChunkSlot successor;
  appendDrawRun(current, targetA);
  appendClearColor(current, targetB);
  appendDrawRun(current, targetB);
  appendDrawRun(successor, targetA);
  const auto currentSource = planningSource(current, 2u, 41u, 101u);
  const auto successorSource = planningSource(successor, 3u, 42u, 102u);
  const ActiveRenderPlanningSeed seed = activeSeed(targetA);

  const DeferredTerminalSuffixPlan plan = planDeferredTerminalSuffixReplay(
      currentSource, successorSource, seed);
  const std::array natural{
      RetainedSourceCommandLocator{0u, 0u},
      RetainedSourceCommandLocator{0u, 1u},
      RetainedSourceCommandLocator{0u, 2u},
      RetainedSourceCommandLocator{1u, 0u},
  };
  const std::array joined{
      RetainedSourceCommandLocator{0u, 0u},
      RetainedSourceCommandLocator{1u, 0u},
      RetainedSourceCommandLocator{0u, 1u},
      RetainedSourceCommandLocator{0u, 2u},
  };
  check(plan.qualified() && plan.proof.naturalReplay == natural &&
            plan.proof.joinedReplay == joined &&
            plan.proof.currentPrefix.commandBegin == 0u &&
            plan.proof.currentPrefix.commandCount == 1u &&
            plan.proof.currentSuffix.commandBegin == 1u &&
            plan.proof.currentSuffix.commandCount == 2u &&
            plan.proof.successorHead.commandBegin == 0u &&
            plan.proof.successorHead.commandCount == 1u &&
            plan.proof.currentPrefix.source == currentSource.source &&
            plan.proof.successorHead.source == successorSource.source,
        "exact A,Clear(B),B | A produces bounded source-qualified ranges");
  check(validateDeferredTerminalSuffixReplay(
            currentSource, successorSource, seed, plan.proof, joined) ==
            DeferredTerminalSuffixReplayValidation::Valid,
        "narrow validator accepts only the proved joined replay");

  const std::array ordinarySources{
      MultiSourcePlanningSource{.payload = currentSource.payload},
      MultiSourcePlanningSource{.payload = successorSource.payload},
  };
  check(validateMultiSourceReplayPermutation(ordinarySources, joined) ==
            MultiSourceReplayValidation::NonDrawCrossSourceMovement,
        "universal validator remains strict for the same Clear crossing");
}

void testDeferredTerminalSuffixRejectsBoundariesAndMalformedRanges() {
  const Handle targetA{0xA600u};
  const Handle targetB{0xB600u};
  const ActiveRenderPlanningSeed seed = activeSeed(targetA);

  {
    ChunkSlot current;
    ChunkSlot successor;
    appendDrawRun(current, targetA);
    appendClearColor(current, targetB);
    appendDrawRun(current, targetB);
    appendDrawRun(current, targetB);
    appendDrawRun(successor, targetA);
    check(planDeferredTerminalSuffixReplay(
              planningSource(current, 0u, 1u, 1u),
              planningSource(successor, 1u, 2u, 2u), seed)
              .disposition ==
            DeferredTerminalSuffixDisposition::UnsupportedShape,
          "extra terminal DrawRun is outside the exact bounded shape");
  }
  {
    ChunkSlot current;
    ChunkSlot successor;
    appendDrawRun(current, targetA);
    current.appendPresent(dxmt9::core::SwapDesc{}, targetB);
    appendDrawRun(current, targetB);
    appendDrawRun(successor, targetA);
    check(planDeferredTerminalSuffixReplay(
              planningSource(current, 0u, 1u, 1u),
              planningSource(successor, 1u, 2u, 2u), seed)
              .disposition ==
            DeferredTerminalSuffixDisposition::UnsupportedBoundary,
          "Present cannot occupy the terminal Clear boundary");
  }
  {
    ChunkSlot current;
    ChunkSlot successor;
    appendDrawRun(current, targetA);
    dxmt9::core::ClearDesc clear{};
    clear.colorAttachments[0].handle = targetB;
    clear.clearColor = true;
    clear.rects.push_back({.left = 0, .top = 0, .right = 2, .bottom = 2});
    current.appendClear(clear);
    appendDrawRun(current, targetB);
    appendDrawRun(successor, targetA);
    check(planDeferredTerminalSuffixReplay(
              planningSource(current, 0u, 1u, 1u),
              planningSource(successor, 1u, 2u, 2u), seed)
              .disposition ==
            DeferredTerminalSuffixDisposition::MalformedRange,
          "rectangular partial Clear cannot become a foldable full Clear");
  }
  {
    ChunkSlot current;
    ChunkSlot successor;
    appendDrawRun(current, targetA);
    appendClearColor(current, targetB);
    appendDrawRun(current, targetB);
    appendDrawRun(successor, targetA);
    current.drawRunRecords.back().paramCount = 0u;
    check(planDeferredTerminalSuffixReplay(
              planningSource(current, 0u, 1u, 1u),
              planningSource(successor, 1u, 2u, 2u), seed)
              .disposition ==
            DeferredTerminalSuffixDisposition::MalformedRange,
          "malformed DrawRun parameter coverage is rejected");
  }
}

void testDeferredTerminalSuffixRejectsIdentityAttachmentAndHazards() {
  const Handle targetA{0xA700u};
  const Handle targetB{0xB700u};
  const Handle targetC{0xC700u};
  const ActiveRenderPlanningSeed seed = activeSeed(targetA);

  {
    ChunkSlot current;
    ChunkSlot successor;
    appendDrawRun(current, targetA);
    appendClearColor(current, targetB);
    appendDrawRun(current, targetB);
    appendDrawRun(successor, targetA);
    const auto currentSource = planningSource(current, 0u, 7u, 20u);
    auto successorSource = planningSource(successor, 1u, 9u, 21u);
    check(planDeferredTerminalSuffixReplay(
              currentSource, successorSource, seed).disposition ==
            DeferredTerminalSuffixDisposition::StaleIdentity,
          "non-adjacent source ordinal cannot qualify as exact successor");
    successorSource = planningSource(successor, 1u, 8u, 22u);
    check(planDeferredTerminalSuffixReplay(
              currentSource, successorSource, seed).disposition ==
            DeferredTerminalSuffixDisposition::StaleIdentity,
          "non-adjacent seqId cannot qualify as exact successor");
    successorSource = planningSource(successor, 0u, 8u, 21u);
    check(planDeferredTerminalSuffixReplay(
              currentSource, successorSource, seed).disposition ==
            DeferredTerminalSuffixDisposition::StaleIdentity,
          "reused live source slot cannot qualify as exact successor");
  }
  {
    ChunkSlot current;
    ChunkSlot successor;
    appendDrawRun(current, targetA);
    appendClearColor(current, targetB);
    appendDrawRun(current, targetB);
    appendDrawRun(successor, targetC);
    check(planDeferredTerminalSuffixReplay(
              planningSource(current, 0u, 1u, 1u),
              planningSource(successor, 1u, 2u, 2u), seed)
              .disposition ==
            DeferredTerminalSuffixDisposition::AttachmentMismatch,
          "successor must return to exact active A attachments");
  }
  {
    ChunkSlot current;
    ChunkSlot successor;
    appendDrawRun(current, targetA);
    appendClearColor(current, targetB);
    appendDrawRun(current, targetB, targetA);
    appendDrawRun(successor, targetA);
    check(planDeferredTerminalSuffixReplay(
              planningSource(current, 0u, 1u, 1u),
              planningSource(successor, 1u, 2u, 2u), seed)
              .disposition ==
            DeferredTerminalSuffixDisposition::DependencyWedged,
          "B reading A creates a dependency wedge against returning A");
  }
  {
    ChunkSlot current;
    ChunkSlot successor;
    appendDrawRun(current, targetA);
    appendClearColor(current, targetB);
    appendDrawRun(current, targetB);
    appendDrawRun(successor, targetA, targetB);
    check(planDeferredTerminalSuffixReplay(
              planningSource(current, 0u, 1u, 1u),
              planningSource(successor, 1u, 2u, 2u), seed)
              .disposition ==
            DeferredTerminalSuffixDisposition::DependencyWedged,
          "returning A reading B cannot move before the B suffix");
  }
  {
    ChunkSlot current;
    ChunkSlot successor;
    appendDrawRun(current, targetA);
    appendClearColor(current, targetB);
    appendDrawRun(current, targetB);
    appendDrawRun(successor, targetA);
    const AliasPair aliases{
        .first = targetA,
        .second = targetB,
        .canonical = Handle{0xAB00u},
    };
    const ResourceAliasResolver resolver{
        .context = &aliases,
        .resolve = resolveAliasPair,
    };
    check(planDeferredTerminalSuffixReplay(
              planningSource(current, 0u, 1u, 1u),
              planningSource(successor, 1u, 2u, 2u), seed, resolver)
              .disposition ==
            DeferredTerminalSuffixDisposition::DependencyWedged,
          "aliased A/B resources cannot be relocated across the Clear");
  }
}

void testDeferredTerminalSuffixNarrowValidatorRejectsStaleAndMalformed() {
  const Handle targetA{0xA800u};
  const Handle targetB{0xB800u};
  ChunkSlot current;
  ChunkSlot successor;
  appendDrawRun(current, targetA);
  appendClearColor(current, targetB);
  appendDrawRun(current, targetB);
  appendDrawRun(successor, targetA);
  const auto currentSource = planningSource(current, 4u, 12u, 30u);
  const auto successorSource = planningSource(successor, 5u, 13u, 31u);
  const ActiveRenderPlanningSeed seed = activeSeed(targetA);
  const DeferredTerminalSuffixPlan plan = planDeferredTerminalSuffixReplay(
      currentSource, successorSource, seed);
  check(plan.qualified(), "validator fixture must first qualify");

  const std::array natural = plan.proof.naturalReplay;
  check(validateDeferredTerminalSuffixReplay(
            currentSource, successorSource, seed, plan.proof, natural) ==
            DeferredTerminalSuffixReplayValidation::UnsupportedMovement,
        "narrow validator rejects any complete but unproved permutation");
  check(validateDeferredTerminalSuffixReplay(
            currentSource, successorSource, seed, plan.proof,
            std::span<const RetainedSourceCommandLocator>(
                plan.proof.joinedReplay.data(), 3u)) ==
            DeferredTerminalSuffixReplayValidation::Missing,
        "narrow validator rejects incomplete replay coverage");
  auto duplicate = plan.proof.joinedReplay;
  duplicate[3] = duplicate[2];
  check(validateDeferredTerminalSuffixReplay(
            currentSource, successorSource, seed, plan.proof, duplicate) ==
            DeferredTerminalSuffixReplayValidation::Duplicate,
        "narrow validator rejects duplicate command coverage");
  auto staleSuccessor = successorSource;
  ++staleSuccessor.source.id.generation;
  check(validateDeferredTerminalSuffixReplay(
            currentSource, staleSuccessor, seed, plan.proof,
            plan.proof.joinedReplay) ==
            DeferredTerminalSuffixReplayValidation::StaleIdentity,
        "generation-stale successor is rejected before replay");
  auto corruptedProof = plan.proof;
  corruptedProof.currentSuffix.commandCount = 1u;
  check(validateDeferredTerminalSuffixReplay(
            currentSource, successorSource, seed, corruptedProof,
            plan.proof.joinedReplay) ==
            DeferredTerminalSuffixReplayValidation::InvalidProof,
        "mutated proof cannot authorize the narrow movement");
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
    testDeferredTerminalSuffixProofQualifiesExactShape();
    testDeferredTerminalSuffixRejectsBoundariesAndMalformedRanges();
    testDeferredTerminalSuffixRejectsIdentityAttachmentAndHazards();
    testDeferredTerminalSuffixNarrowValidatorRejectsStaleAndMalformed();
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
