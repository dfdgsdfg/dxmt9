#include "../../../src/dxmt9/dxmt9_draw_encoder_internal.hpp"
#include "../../../src/dxmt9/dxmt9_encode_partition.hpp"
#include "../../../src/dxmt9/dxmt9_render_pass_close_ledger.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dxmt9::encoders::EncodePartitionRangeKind;
using dxmt9::encoders::EncodePartitionRangeSnapshot;
using dxmt9::encoders::EncodePartitionRangeValidation;
using dxmt9::encoders::EncodePartitionReplayStream;
using dxmt9::core::DrawParam;

dxmt9::core::CpuReadyTape::SourceRef testTapeSource(
    std::size_t slotIndex,
    std::uint64_t seqId) {
  return {
      .id = {
          .index = static_cast<std::uint32_t>(slotIndex),
          .generation = seqId,
      },
      .storage = {
          .firstPage = static_cast<std::uint32_t>(slotIndex),
          .pageCount = 1,
          .generation = seqId,
      },
  };
}

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

template <typename T>
void check(const T& condition, std::string_view message) {
  if (!static_cast<bool>(condition)) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    fail(std::string(message));
  }
}

struct MixedFixture {
  dxmt9::core::ChunkSlot slot{};

  MixedFixture() {
    slot.seqId = 71;
    slot.appendClear(dxmt9::core::ClearDesc{});
    appendDrawRun(3);
    slot.appendClear(dxmt9::core::ClearDesc{});
    appendDrawRun(2);
    slot.appendClear(dxmt9::core::ClearDesc{});
    checkEq(slot.commandCount(), std::size_t{5},
            "mixed fixture command count");
  }

  void appendDrawRun(std::size_t count) {
    std::vector<DrawParam> draws(count);
    std::vector<dxmt9::core::DrawParamPayloadView> payloads(count);
    slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                       dxmt9::core::DrawUniformPayload{}, draws, payloads);
  }

  EncodePartitionReplayStream sourceOrder(
      std::size_t begin = 0,
      std::size_t count = std::numeric_limits<std::size_t>::max()) const {
    if (count == std::numeric_limits<std::size_t>::max()) {
      count = slot.commandCount() - begin;
    }
    return dxmt9::encoders::makeEncodePartitionReplayStream(
        4, slot, begin, count, false, {}, {}, testTapeSource(4, slot.seqId));
  }

  EncodePartitionReplayStream ordered(
      std::span<const std::uint32_t> order) const {
    std::vector<std::size_t> replayOrdinalByCommandIndex(
        slot.commandCount());
    return dxmt9::encoders::makeEncodePartitionReplayStream(
        4, slot, 0, slot.commandCount(), true, order,
        replayOrdinalByCommandIndex, testTapeSource(4, slot.seqId));
  }
};

EncodePartitionRangeSnapshot drawRange(
    const EncodePartitionReplayStream& stream,
    std::uint32_t ordinal,
    std::uint32_t firstParam,
    std::uint32_t count) {
  EncodePartitionRangeSnapshot range{
      .kind = EncodePartitionRangeKind::DrawRunEntries,
      .replayOrdinalBegin = ordinal,
      .replayOrdinalCount = 1,
      .drawEntryCount = count,
  };
  check(dxmt9::encoders::buildEncodePartitionEntrySnapshot(
            stream, ordinal, firstParam, range.entry),
        "draw range entry synthesis");
  return range;
}

EncodePartitionRangeSnapshot commandRange(std::uint32_t ordinal,
                                          std::uint32_t count = 1) {
  return EncodePartitionRangeSnapshot{
      .kind = EncodePartitionRangeKind::CommandSegment,
      .replayOrdinalBegin = ordinal,
      .replayOrdinalCount = count,
      .drawEntryCount = 0,
  };
}

std::vector<EncodePartitionRangeSnapshot> identityRanges(
    const EncodePartitionReplayStream& stream) {
  dxmt9::encoders::EncodePartitionIdentityCursor cursor(stream);
  std::vector<EncodePartitionRangeSnapshot> ranges;
  EncodePartitionRangeSnapshot range{};
  while (cursor.next(range)) {
    ranges.push_back(range);
  }
  return ranges;
}

dxmt9::core::ChunkSlot makePlannerDrawSlot(std::uint64_t seqId,
                                           std::size_t drawCount,
                                           bool mergeable) {
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = seqId;
  std::vector<DrawParam> draws(drawCount);
  std::vector<dxmt9::core::DrawParamPayloadView> payloads(drawCount);
  if (mergeable) {
    for (std::size_t i = 0; i < draws.size(); ++i) {
      auto& draw = draws[i];
      draw.indexed = true;
      draw.primitiveType = dxmt9::core::PrimitiveType::TriangleList;
      draw.indexType = dxmt9::core::IndexType::UInt16;
      draw.instanceCount = 1u;
      draw.primitiveCount = 1u;
      draw.startIndex = static_cast<std::uint32_t>(i * 3u);
    }
  }
  slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                     dxmt9::core::DrawUniformPayload{}, draws, payloads);
  return slot;
}

void productionPlannerThresholdsAreDeterministic() {
  using dxmt9::encoders::ProductionEncodePartitionPlanStorage;
  using dxmt9::encoders::ProductionPartitionFallbackReason;
  auto planDraws = [](std::size_t drawCount,
                      ProductionEncodePartitionPlanStorage& storage) {
    auto slot = makePlannerDrawSlot(101u + drawCount, drawCount, false);
    const auto stream = dxmt9::encoders::makeEncodePartitionReplayStream(
        6u, slot, 0u, slot.commandCount(), false, {}, {},
        testTapeSource(6u, slot.seqId));
    const auto result = dxmt9::encoders::planProductionEncodePartitions(
        stream, storage);
    return std::pair{result,
                     std::vector<EncodePartitionRangeSnapshot>(
                         storage.view().begin(), storage.view().end())};
  };

  ProductionEncodePartitionPlanStorage storage{};
  const auto [below, belowRanges] = planDraws(
      dxmt9::encoders::kProductionPartitionDrawThreshold - 1u, storage);
  check(!below.explicitPlan && belowRanges.empty() &&
            below.fallback ==
                ProductionPartitionFallbackReason::NoEligibleDrawRun,
        "threshold minus one retains identity");

  const auto [edge, edgeRanges] = planDraws(
      dxmt9::encoders::kProductionPartitionDrawThreshold, storage);
  check(edge.explicitPlan && edgeRanges.size() == 2u,
        "threshold draw run receives an explicit subdivision");
  checkEq(edgeRanges[0].drawEntryCount,
          dxmt9::encoders::kProductionPartitionTargetDraws,
          "threshold first range uses the deterministic target");
  checkEq(edgeRanges[1].drawEntryCount,
          dxmt9::encoders::kProductionPartitionDrawThreshold -
              dxmt9::encoders::kProductionPartitionTargetDraws,
          "threshold tail covers every remaining draw");

  const auto [above, aboveRanges] = planDraws(
      dxmt9::encoders::kProductionPartitionDrawThreshold + 1u, storage);
  check(above.explicitPlan && aboveRanges.size() == 2u &&
            aboveRanges[1].drawEntryCount == 33u,
        "threshold plus one has a deterministic complete tail");
  const auto [repeat, repeatRanges] = planDraws(
      dxmt9::encoders::kProductionPartitionDrawThreshold + 1u, storage);
  checkEq(repeat, above, "identical planner inputs produce equal results");
  checkEq(repeatRanges, aboveRanges,
          "identical planner inputs produce byte-equivalent locators");
}

void productionPlannerUsesFinalMixedReplaySelection() {
  using dxmt9::encoders::ProductionEncodePartitionPlanStorage;
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = 211u;
  slot.appendClear({});
  std::vector<DrawParam> draws(
      dxmt9::encoders::kProductionPartitionDrawThreshold);
  std::vector<dxmt9::core::DrawParamPayloadView> payloads(draws.size());
  slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                     dxmt9::core::DrawUniformPayload{}, draws, payloads);
  slot.appendClear({});
  checkEq(slot.commandCount(), std::size_t{3},
          "mixed planner fixture has command/draw/command shape");

  auto makeStream = [&](bool active,
                        std::span<const std::uint32_t> order,
                        std::span<std::size_t> scratch) {
    return dxmt9::encoders::makeEncodePartitionReplayStream(
        7u, slot, 0u, slot.commandCount(), active, order, scratch,
        testTapeSource(7u, slot.seqId));
  };
  ProductionEncodePartitionPlanStorage storage{};
  const auto sourceOrder = makeStream(false, {}, {});
  const auto sourcePlan = dxmt9::encoders::planProductionEncodePartitions(
      sourceOrder, storage);
  check(sourcePlan.explicitPlan && storage.count == 4u,
        "mixed stream keeps serial commands around two draw ranges");
  check(storage.ranges[0].kind == EncodePartitionRangeKind::CommandSegment &&
            storage.ranges[1].entry.commandIndex == 1u &&
            storage.ranges[2].entry.commandIndex == 1u &&
            storage.ranges[3].kind == EncodePartitionRangeKind::CommandSegment,
        "mixed source-order plan covers command and DrawRun identities");

  const std::array<std::uint32_t, 3> order{2u, 1u, 0u};
  std::array<std::size_t, 3> scratch{};
  const auto reordered = makeStream(true, order, scratch);
  const auto reorderedPlan =
      dxmt9::encoders::planProductionEncodePartitions(reordered, storage);
  check(reorderedPlan.explicitPlan && storage.count == 4u &&
            storage.ranges[1].entry.commandIndex == 1u,
        "planner subdivides the final FrameGraph order, not source order");
  check(dxmt9::encoders::validateEncodePartitionRanges(storage.view(),
                                                       reordered),
        "reordered production plan validates exact final coverage");

  const auto empty = makeStream(true, {}, {});
  const auto emptyPlan =
      dxmt9::encoders::planProductionEncodePartitions(empty, storage);
  check(!emptyPlan.explicitPlan && storage.view().empty(),
        "DCE-empty effective replay retains the empty identity cursor");
}

void productionPlannerPreservesMergeChainsAndFailsOpenBoundedly() {
  using dxmt9::encoders::ProductionEncodePartitionPlanStorage;
  using dxmt9::encoders::ProductionPartitionFallbackReason;
  auto mergeable = makePlannerDrawSlot(
      301u, dxmt9::encoders::kProductionPartitionDrawThreshold, true);
  const auto mergeStream = dxmt9::encoders::makeEncodePartitionReplayStream(
      8u, mergeable, 0u, mergeable.commandCount(), false, {}, {},
      testTapeSource(8u, mergeable.seqId));
  ProductionEncodePartitionPlanStorage storage{};
  const auto mergePlan =
      dxmt9::encoders::planProductionEncodePartitions(mergeStream, storage);
  check(!mergePlan.explicitPlan && storage.view().empty() &&
            mergePlan.fallback ==
                ProductionPartitionFallbackReason::MergePreservation &&
            mergePlan.mergePreservedIdentityCount == 1u,
        "a compatible indexed merge chain remains one identity DrawRun");

  auto makeCapacityFixture = [](std::uint64_t seqId,
                                std::size_t largeRunCount) {
    dxmt9::core::ChunkSlot slot{};
    slot.seqId = seqId;
    std::vector<DrawParam> draws(
        dxmt9::encoders::kProductionPartitionDrawThreshold);
    std::vector<dxmt9::core::DrawParamPayloadView> payloads(draws.size());
    for (std::size_t i = 0u; i < largeRunCount; ++i) {
      slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                         dxmt9::core::DrawUniformPayload{}, draws, payloads);
    }
    return slot;
  };

  auto exact = makeCapacityFixture(
      302u, dxmt9::encoders::kProductionPartitionRangeCapacity / 2u);
  const auto exactStream =
      dxmt9::encoders::makeEncodePartitionReplayStream(
          9u, exact, 0u, exact.commandCount(), false, {}, {},
          testTapeSource(9u, exact.seqId));
  const auto exactPlan =
      dxmt9::encoders::planProductionEncodePartitions(exactStream,
                                                       storage);
  check(exactPlan.explicitPlan &&
            storage.count ==
                dxmt9::encoders::kProductionPartitionRangeCapacity &&
            dxmt9::encoders::validateEncodePartitionRanges(storage.view(),
                                                             exactStream),
        "the exact fixed range capacity validates and remains explicit");

  auto oversized = makeCapacityFixture(
      303u, dxmt9::encoders::kProductionPartitionRangeCapacity / 2u + 1u);
  const auto oversizedStream =
      dxmt9::encoders::makeEncodePartitionReplayStream(
          10u, oversized, 0u, oversized.commandCount(), false, {}, {},
          testTapeSource(10u, oversized.seqId));
  const auto oversizedPlan =
      dxmt9::encoders::planProductionEncodePartitions(oversizedStream,
                                                      storage);
  check(!oversizedPlan.explicitPlan && storage.view().empty() &&
            oversizedPlan.fallback ==
                ProductionPartitionFallbackReason::RangeCapacity,
        "one subdivided DrawRun beyond fixed capacity atomically selects "
        "identity");

  dxmt9::core::ChunkSlot compressed{};
  compressed.seqId = 304u;
  std::array<DrawParam, 1> singleton{};
  std::array<dxmt9::core::DrawParamPayloadView, 1> singletonPayload{};
  for (std::size_t i = 0u;
       i < dxmt9::encoders::kProductionPartitionRangeCapacity * 2u;
       ++i) {
    compressed.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                             dxmt9::core::DrawUniformPayload{}, singleton,
                             singletonPayload);
  }
  std::vector<DrawParam> largeDraws(
      dxmt9::encoders::kProductionPartitionDrawThreshold);
  std::vector<dxmt9::core::DrawParamPayloadView> largePayloads(
      largeDraws.size());
  compressed.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                           dxmt9::core::DrawUniformPayload{}, largeDraws,
                           largePayloads);
  const auto compressedStream =
      dxmt9::encoders::makeEncodePartitionReplayStream(
          11u, compressed, 0u, compressed.commandCount(), false, {}, {},
          testTapeSource(11u, compressed.seqId));
  const auto compressedPlan =
      dxmt9::encoders::planProductionEncodePartitions(compressedStream,
                                                      storage);
  check(compressedPlan.explicitPlan && storage.count == 3u &&
            storage.ranges[0].kind ==
                EncodePartitionRangeKind::CommandSegment &&
            storage.ranges[0].replayOrdinalCount ==
                dxmt9::encoders::kProductionPartitionRangeCapacity * 2u &&
            dxmt9::encoders::validateEncodePartitionRanges(storage.view(),
                                                             compressedStream),
        "ordinary DrawRuns compress into one serial command span around an "
        "explicit subdivision");

  auto malformed = makePlannerDrawSlot(
      305u, dxmt9::encoders::kProductionPartitionDrawThreshold, false);
  // The replay factory bounds command arithmetic, and SourcePayloadView
  // exposes only DrawRun spans backed by the owned parameter SoA. A planner
  // draw-end overflow would therefore require materializing more than
  // UINT32_MAX draws; corrupt the narrow public state locator to exercise the
  // real SnapshotInvalid path without constructing an impossible source span.
  malformed.drawRunRecords.front().stateIndex =
      std::numeric_limits<std::uint32_t>::max();
  const auto malformedStream =
      dxmt9::encoders::makeEncodePartitionReplayStream(
          12u, malformed, 0u, malformed.commandCount(), false, {}, {},
          testTapeSource(12u, malformed.seqId));
  check(malformedStream.valid,
        "malformed DrawRun remains a representable replay stream");
  const auto malformedPlan =
      dxmt9::encoders::planProductionEncodePartitions(malformedStream,
                                                      storage);
  check(!malformedPlan.explicitPlan && storage.view().empty() &&
            malformedPlan.fallback ==
                ProductionPartitionFallbackReason::SnapshotInvalid,
        "malformed DrawRun snapshot failure atomically selects identity");

  auto invalidStream = oversizedStream;
  invalidStream.valid = false;
  const auto invalidPlan =
      dxmt9::encoders::planProductionEncodePartitions(invalidStream, storage);
  check(!invalidPlan.explicitPlan && storage.view().empty() &&
            invalidPlan.fallback ==
                ProductionPartitionFallbackReason::ReplayStreamInvalid,
        "malformed replay metadata fails open without a partial plan");
}

void productionPlannerKeepsTransitiveMergeChainWhole() {
  constexpr std::size_t kDrawCount = 96u;
  constexpr std::size_t kChainBegin = 28u;
  constexpr std::size_t kChainEnd = 37u;
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = 305u;
  std::array<DrawParam, kDrawCount> draws{};
  std::array<dxmt9::core::DrawParamPayloadView, kDrawCount> payloads{};
  for (std::size_t i = kChainBegin; i < kChainEnd; ++i) {
    auto& draw = draws[i];
    draw.indexed = true;
    draw.primitiveType = dxmt9::core::PrimitiveType::TriangleList;
    draw.indexType = dxmt9::core::IndexType::UInt16;
    draw.instanceCount = 1u;
    draw.primitiveCount = 1u;
    draw.startIndex = static_cast<std::uint32_t>((i - kChainBegin) * 3u);
  }
  slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                     dxmt9::core::DrawUniformPayload{}, draws, payloads);
  const auto stream = dxmt9::encoders::makeEncodePartitionReplayStream(
      12u, slot, 0u, slot.commandCount(), false, {}, {},
      testTapeSource(12u, slot.seqId));

  dxmt9::encoders::ProductionEncodePartitionPlanStorage storage{};
  const auto plan =
      dxmt9::encoders::planProductionEncodePartitions(stream, storage);
  const std::vector<EncodePartitionRangeSnapshot> ranges(
      storage.view().begin(), storage.view().end());
  check(plan.explicitPlan && ranges.size() == 3u &&
            dxmt9::encoders::validateEncodePartitionRanges(ranges, stream),
        "safe surrounding portions form a complete explicit plan");
  check(ranges[0].drawEntryCount == kChainBegin &&
            ranges[1].entry.drawParamIndex == kChainBegin &&
            ranges[1].drawEntryCount == 32u &&
            ranges[2].entry.drawParamIndex == 60u,
        "the desired edge moves before the full compatible chain");
  const auto chainRange =
      dxmt9::encoders::resolveEncodePartition(ranges[1], stream);
  check(chainRange &&
            dxmt9::encoders::makeCompatibleIndexedDrawMerge(
                chainRange.partition.drawParams.first(kChainEnd - kChainBegin),
                {})
                    .drawCount == kChainEnd - kChainBegin,
        "one planned range retains the full transitive merge opportunity");

  const auto repeat =
      dxmt9::encoders::planProductionEncodePartitions(stream, storage);
  checkEq(repeat, plan,
          "transitive merge fixture produces a deterministic result");
  checkEq(std::vector<EncodePartitionRangeSnapshot>(storage.view().begin(),
                                                    storage.view().end()),
          ranges, "transitive merge fixture produces deterministic ranges");
}

void canonicalPartitionSelectorResolvesQueueImmutableModes() {
  using dxmt9::render::PartitionExecutionMode;
  using dxmt9::render::PartitionModeFallback;
  const auto defaults = dxmt9::render::resolveRenderPartitionConfig(nullptr);
  const auto empty = dxmt9::render::resolveRenderPartitionConfig("");
  const auto identity =
      dxmt9::render::resolveRenderPartitionConfig("identity");
  const auto serial = dxmt9::render::resolveRenderPartitionConfig("serial");
  const auto parallel =
      dxmt9::render::resolveRenderPartitionConfig("parallel");
  const auto invalid =
      dxmt9::render::resolveRenderPartitionConfig("surprise");
  check(defaults.resolved == PartitionExecutionMode::IdentitySerial &&
            identity.resolved == PartitionExecutionMode::IdentitySerial &&
            serial.resolved == PartitionExecutionMode::ExplicitSerial,
        "canonical identity/serial spelling resolves once into typed modes");
  check(parallel.resolved == PartitionExecutionMode::IdentitySerial &&
            parallel.fallback ==
                PartitionModeFallback::ParallelUnsupported &&
            empty.resolved == PartitionExecutionMode::IdentitySerial &&
            empty.fallback == PartitionModeFallback::InvalidValue &&
            invalid.resolved == PartitionExecutionMode::IdentitySerial &&
            invalid.fallback == PartitionModeFallback::InvalidValue,
        "unsupported or invalid requests fail closed to identity");
}

void identityTraversesMixedSourceOrder() {
  MixedFixture fixture;
  const auto stream = fixture.sourceOrder();
  const auto ranges = identityRanges(stream);
  checkEq(ranges.size(), std::size_t{5},
          "mixed identity alternates command and draw ranges");
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    checkEq(ranges[i].replayOrdinalBegin, static_cast<std::uint32_t>(i),
            "identity preserves source replay ordinal");
  }
  checkEq(ranges[0].kind, EncodePartitionRangeKind::CommandSegment,
          "clear uses command coverage");
  checkEq(ranges[1].kind, EncodePartitionRangeKind::DrawRunEntries,
          "draw run uses draw-entry coverage");
  checkEq(ranges[1].drawEntryCount, std::uint32_t{3},
          "identity retains the complete first draw run");
  checkEq(ranges[3].drawEntryCount, std::uint32_t{2},
          "identity retains the complete second draw run");
  check(dxmt9::encoders::validateEncodePartitionRanges(ranges, stream),
        "mixed identity exactly covers source order");
}

void replayStreamValidProvesActiveOrderContract() {
  MixedFixture fixture;
  const auto missingLocator =
      dxmt9::encoders::makeEncodePartitionReplayStream(
          4, fixture.slot, 0, fixture.slot.commandCount(), false, {});
  check(!missingLocator.valid,
        "replay stream rejects a missing stable Tape locator");
  auto make = [&](std::size_t commandBegin,
                  std::size_t commandCount,
                  bool commandOrderActive,
                  std::span<const std::uint32_t> commandOrder,
                  std::span<std::size_t> scratch = {}) {
    return dxmt9::encoders::makeEncodePartitionReplayStream(
        4, fixture.slot, commandBegin, commandCount,
        commandOrderActive, commandOrder, scratch,
        testTapeSource(4, fixture.slot.seqId));
  };

  const std::array<std::uint32_t, 2> validReorder{3u, 1u};
  std::array<std::size_t, 3> scratch{};
  const auto valid = make(1u, 3u, true, validReorder, scratch);
  check(valid.valid,
        "active replay stream accepts a unique in-range reorder");
  checkEq(scratch[0], std::size_t{1},
          "active replay scratch maps the second replay ordinal");
  checkEq(scratch[1], std::numeric_limits<std::size_t>::max(),
          "active replay scratch leaves an unselected command missing");
  checkEq(scratch[2], std::size_t{0},
          "active replay scratch maps the first replay ordinal");

  const std::array<std::uint32_t, 1> validSubset{2u};
  check(make(1u, 3u, true, validSubset, scratch).valid,
        "active replay stream accepts a unique in-range subset");
  check(make(1u, 3u, true, {}, {}).valid,
        "active replay stream accepts empty DCE without scratch");

  const std::array<std::uint32_t, 1> belowRange{0u};
  check(!make(1u, 3u, true, belowRange, scratch).valid,
        "active replay stream rejects an index below source range");
  const std::array<std::uint32_t, 1> aboveRange{4u};
  check(!make(1u, 3u, true, aboveRange, scratch).valid,
        "active replay stream rejects an index above source range");
  const std::array<std::uint32_t, 2> duplicate{1u, 1u};
  check(!make(1u, 3u, true, duplicate, scratch).valid,
        "active replay stream rejects duplicate command selection");
  const std::array<std::uint32_t, 4> oversized{1u, 2u, 3u, 1u};
  check(!make(1u, 3u, true, oversized, scratch).valid,
        "active replay stream rejects order longer than source range");
  std::array<std::size_t, 2> undersizedScratch{};
  check(!make(1u, 3u, true, validSubset, undersizedScratch).valid,
        "active replay stream rejects insufficient validation scratch");
  check(!make(1u, 3u, true, validSubset).valid,
        "active nonempty replay order cannot bypass validation scratch");
  check(!make(1u, 3u, false, validSubset).valid,
        "source-order replay rejects an inactive order payload");
  check(!make(4u, 2u, false, {}).valid,
        "source-order replay rejects an invalid source range");
}

void identityUsesFramegraphOrderAndDceSelection() {
  MixedFixture fixture;
  const std::array<std::uint32_t, 5> reordered{3, 4, 0, 1, 2};
  const auto reorderedStream = fixture.ordered(reordered);
  const auto reorderedRanges = identityRanges(reorderedStream);
  checkEq(reorderedRanges.front().kind,
          EncodePartitionRangeKind::DrawRunEntries,
          "FrameGraph first selected draw remains first");
  checkEq(reorderedRanges.front().entry.commandIndex, std::uint32_t{3},
          "FrameGraph command index resolves from effective order");
  check(dxmt9::encoders::validateEncodePartitionRanges(
            reorderedRanges, reorderedStream),
        "FrameGraph reorder identity validates");

  const std::array<std::uint32_t, 2> subset{1, 3};
  const auto subsetStream = fixture.ordered(subset);
  const auto subsetRanges = identityRanges(subsetStream);
  checkEq(subsetRanges.size(), std::size_t{2},
          "DCE subset emits only selected commands");
  checkEq(subsetRanges[0].entry.commandIndex, std::uint32_t{1},
          "DCE subset first command identity");
  checkEq(subsetRanges[1].entry.commandIndex, std::uint32_t{3},
          "DCE subset second command identity");

  const std::span<const std::uint32_t> emptyOrder{};
  const auto emptyStream = fixture.ordered(emptyOrder);
  check(identityRanges(emptyStream).empty(),
        "DCE empty selection has an empty identity traversal");
  check(dxmt9::encoders::validateEncodePartitionRanges({}, emptyStream),
        "DCE empty selection has exact empty coverage");
}

void identityUsesPartialSessionSourceRange() {
  MixedFixture fixture;
  const auto stream = fixture.sourceOrder(1, 3);
  const auto ranges = identityRanges(stream);
  checkEq(ranges.size(), std::size_t{3},
          "partial sessionSource traverses only its selected commands");
  checkEq(ranges[0].entry.commandIndex, std::uint32_t{1},
          "partial identity begins at absolute command one");
  checkEq(ranges[1].kind, EncodePartitionRangeKind::CommandSegment,
          "partial middle clear remains a command segment");
  checkEq(ranges[2].entry.commandIndex, std::uint32_t{3},
          "partial identity ends at absolute command three");
  check(dxmt9::encoders::validateEncodePartitionRanges(ranges, stream),
        "partial sessionSource identity validates exactly");
}

void identityRetainsEmptyAndMalformedDrawRunsAsCommands() {
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = 81;
  std::array<DrawParam, 1> oneDraw{};
  std::array<dxmt9::core::DrawParamPayloadView, 1> onePayload{};
  slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                     dxmt9::core::DrawUniformPayload{}, oneDraw,
                     onePayload);
  slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                     dxmt9::core::DrawUniformPayload{}, oneDraw,
                     onePayload);
  checkEq(slot.commandCount(), std::size_t{2},
          "empty and malformed fixture contains two commands");
  slot.drawRunRecords[0].paramCount = 0;
  slot.drawRunRecords[1].paramCount = 2;

  const auto stream = dxmt9::encoders::makeEncodePartitionReplayStream(
      3, slot, 0, slot.commandCount(), false, {}, {},
      testTapeSource(3, slot.seqId));
  const auto ranges = identityRanges(stream);
  checkEq(ranges.size(), std::size_t{1},
          "adjacent non-encodable draw runs coalesce as commands");
  checkEq(ranges.front().kind, EncodePartitionRangeKind::CommandSegment,
          "empty and malformed DrawRuns retain complete-command coverage");
  checkEq(ranges.front().replayOrdinalCount, std::uint32_t{2},
          "identity preserves both old skip-behavior commands");
}

void validTwoAndThreeSubrangePlansGroupCommandOnce() {
  MixedFixture fixture;
  const auto stream = fixture.sourceOrder();
  const auto firstRun = fixture.slot.commandAt(1);
  const std::uint32_t first = firstRun.drawRunRecord->firstParam;
  const std::array<EncodePartitionRangeSnapshot, 6> twoRanges{
      commandRange(0),
      drawRange(stream, 1, first, 1),
      drawRange(stream, 1, first + 1, 2),
      commandRange(2),
      drawRange(stream, 3,
                fixture.slot.commandAt(3).drawRunRecord->firstParam, 2),
      commandRange(4),
  };
  check(dxmt9::encoders::validateEncodePartitionRanges(twoRanges, stream),
        "two-subrange plan validates");

  auto threeRanges = std::vector<EncodePartitionRangeSnapshot>{
      commandRange(0),
      drawRange(stream, 1, first, 1),
      drawRange(stream, 1, first + 1, 1),
      drawRange(stream, 1, first + 2, 1),
      commandRange(2),
      drawRange(stream, 3,
                fixture.slot.commandAt(3).drawRunRecord->firstParam, 2),
      commandRange(4),
  };
  check(dxmt9::encoders::validateEncodePartitionRanges(threeRanges, stream),
        "three-subrange plan validates");

  dxmt9::encoders::EncodePartitionSerialCursor cursor(
      stream, threeRanges, true);
  std::array<std::uint32_t, 5> commandVisits{};
  dxmt9::encoders::EncodePartitionSerialBatch batch{};
  while (cursor.next(batch)) {
    if (batch.kind == EncodePartitionRangeKind::CommandSegment) {
      for (std::uint32_t ordinal = batch.replayOrdinalBegin;
           ordinal < batch.replayOrdinalBegin + batch.replayOrdinalCount;
           ++ordinal) {
        ++commandVisits[ordinal];
      }
    } else {
      ++commandVisits[batch.replayOrdinalBegin];
      check(!batch.identityResolved,
            "explicit DrawRun batch requires execution re-resolution");
      if (batch.replayOrdinalBegin == 1) {
        checkEq(batch.ranges.size(), std::size_t{3},
                "adjacent subranges group under one draw command");
      }
    }
  }
  for (const auto visits : commandVisits) {
    checkEq(visits, std::uint32_t{1},
            "serial outer cursor visits every effective command once");
  }
}

void identitySerialBatchCarriesResolvedFullDrawRun() {
  MixedFixture fixture;
  const auto stream = fixture.sourceOrder();
  dxmt9::encoders::EncodePartitionSerialCursor cursor(stream, {}, false);
  dxmt9::encoders::EncodePartitionSerialBatch batch{};

  check(cursor.next(batch), "identity serial cursor visits first command");
  check(!batch.identityResolved,
        "identity command segment carries no borrowed draw view");
  check(cursor.next(batch), "identity serial cursor visits first draw run");
  check(batch.identityResolved,
        "identity DrawRun carries its call-local resolved view");
  checkEq(batch.ranges.size(), std::size_t{1},
          "identity DrawRun retains one full partition snapshot");
  checkEq(batch.identityPartition.entry.slot, &fixture.slot,
          "identity resolved view points at the current source only");
  checkEq(batch.identityPartition.entry.command.drawRunRecord,
          fixture.slot.commandAt(1).drawRunRecord,
          "identity resolved view retains the selected DrawRun");
  checkEq(batch.identityPartition.drawParams.size(), std::size_t{3},
          "identity resolved view retains the complete DrawRun span");
  checkEq(batch.identityPartition.drawParams.data(),
          fixture.slot.commandAt(1).drawParams.data(),
          "identity executor can consume the existing full-range view");
}

void invalidPlansFailAllOrNothingToIdentity() {
  MixedFixture fixture;
  const auto stream = fixture.sourceOrder();
  const auto command = fixture.slot.commandAt(1);
  const std::uint32_t first = command.drawRunRecord->firstParam;

  auto expectFallback = [&](std::span<const EncodePartitionRangeSnapshot> plan,
                            EncodePartitionRangeValidation expected,
                            std::string_view message) {
    const auto validation =
        dxmt9::encoders::validateEncodePartitionRanges(plan, stream);
    checkEq(validation.validation, expected, message);
    dxmt9::encoders::EncodePartitionSerialCursor cursor(
        stream, plan, static_cast<bool>(validation));
    std::size_t covered = 0;
    dxmt9::encoders::EncodePartitionSerialBatch batch{};
    while (cursor.next(batch)) {
      covered += batch.replayOrdinalCount;
    }
    checkEq(covered, stream.replayOrdinalCount(),
            "invalid plan fallback restarts the complete effective stream");
  };

  const std::array gap{
      commandRange(0),
      drawRange(stream, 1, first + 1, 2),
  };
  expectFallback(gap, EncodePartitionRangeValidation::DrawCoverageGap,
                 "draw gap rejected");

  const std::array overlap{
      commandRange(0),
      drawRange(stream, 1, first, 2),
      drawRange(stream, 1, first + 1, 2),
  };
  expectFallback(overlap, EncodePartitionRangeValidation::DrawCoverageOverlap,
                 "draw overlap rejected");

  const std::array duplicate{
      commandRange(0),
      drawRange(stream, 1, first, 3),
      drawRange(stream, 1, first, 3),
  };
  expectFallback(duplicate,
                 EncodePartitionRangeValidation::ReplayCoverageOverlap,
                 "duplicate completed draw range rejected");

  const std::array partialTail{
      commandRange(0),
      drawRange(stream, 1, first, 1),
  };
  expectFallback(partialTail,
                 EncodePartitionRangeValidation::DrawCoveragePartialTail,
                 "partial draw tail rejected");

  auto stale = drawRange(stream, 1, first, 3);
  stale.entry.source.seqId += 1;
  const std::array stalePlan{commandRange(0), stale};
  expectFallback(stalePlan,
                 EncodePartitionRangeValidation::EntryResolutionFailed,
                 "stale source identity rejects the complete plan");

  auto mismatched = drawRange(stream, 1, first, 3);
  mismatched.entry.stateIndex += 1;
  const std::array mismatchPlan{commandRange(0), mismatched};
  expectFallback(mismatchPlan,
                 EncodePartitionRangeValidation::EntryResolutionFailed,
                 "entry payload mismatch rejects the complete plan");

  const std::array overflow{
      commandRange(std::numeric_limits<std::uint32_t>::max(), 2),
  };
  expectFallback(overflow,
                 EncodePartitionRangeValidation::ReplayOrdinalOverflow,
                 "overflow rejects the complete plan before traversal");
}

void partitionBoundariesLimitCompatibleIndexedMergeCandidates() {
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = 91;
  std::array<DrawParam, 3> draws{};
  for (auto& draw : draws) {
    draw.indexed = true;
    draw.primitiveType = dxmt9::core::PrimitiveType::TriangleList;
    draw.indexType = dxmt9::core::IndexType::UInt16;
    draw.instanceCount = 1;
    draw.primitiveCount = 1;
  }
  draws[0].startIndex = 0;
  draws[1].startIndex = 3;
  draws[2].startIndex = 6;
  std::array<dxmt9::core::DrawParamPayloadView, 3> payloads{};
  slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                     dxmt9::core::DrawUniformPayload{}, draws,
                     payloads);
  const auto stream = dxmt9::encoders::makeEncodePartitionReplayStream(
      2, slot, 0, 1, false, {}, {}, testTapeSource(2, slot.seqId));
  const auto identity = identityRanges(stream);
  checkEq(identity.size(), std::size_t{1},
          "merge fixture identity is one full draw range");
  const auto full = dxmt9::encoders::resolveEncodePartition(
      identity.front(), stream);
  check(full, "identity full range resolves");
  dxmt9::encoders::EncodePartitionSerialCursor identityCursor(
      stream, {}, false);
  dxmt9::encoders::EncodePartitionSerialBatch identityBatch{};
  check(identityCursor.next(identityBatch),
        "identity serial cursor yields the full DrawRun");
  check(identityBatch.identityResolved,
        "identity serial execution receives the resolved fast path");
  checkEq(dxmt9::encoders::makeCompatibleIndexedDrawMerge(
              identityBatch.identityPartition.drawParams, {}).drawCount,
          std::size_t{3},
          "identity fast path retains the existing merge candidate shape");

  const std::uint32_t first =
      slot.commandAt(0).drawRunRecord->firstParam;
  const std::array split{
      drawRange(stream, 0, first, 1),
      drawRange(stream, 0, first + 1, 2),
  };
  check(dxmt9::encoders::validateEncodePartitionRanges(split, stream),
        "merge split plan validates");
  const auto left = dxmt9::encoders::resolveEncodePartition(split[0], stream);
  const auto right = dxmt9::encoders::resolveEncodePartition(split[1], stream);
  checkEq(dxmt9::encoders::makeCompatibleIndexedDrawMerge(
              left.partition.drawParams, {}).drawCount,
          std::size_t{1},
          "left explicit range cannot merge into its successor");
  checkEq(dxmt9::encoders::makeCompatibleIndexedDrawMerge(
              right.partition.drawParams, {}).drawCount,
          std::size_t{2},
          "right explicit range retains only its internal merge candidates");
}

void naturalFallbackReentryRequiresTheCompleteSameWindowInterval() {
  using dxmt9::encoders::NaturalFallbackReentryRelation;
  using dxmt9::encoders::ReplayWindowDisposition;
  using dxmt9::encoders::ReplayWindowProvenance;
  const auto natural = [](std::uint64_t windowId,
                          std::uint32_t sourceIndex) {
    return ReplayWindowProvenance{
        .disposition =
            ReplayWindowDisposition::NaturalAfterMergeFallback,
        .windowId = windowId,
        .sourceIndex = sourceIndex,
        .sourceCount = 3u,
    };
  };

  const auto a0 = natural(41u, 0u);
  const auto b = natural(41u, 1u);
  const auto a1 = natural(41u, 2u);
  const std::array sameIntervening{b};
  checkEq(dxmt9::encoders::classifyNaturalFallbackReentry(
              a0, sameIntervening, a1),
          NaturalFallbackReentryRelation::SameWindow,
          "A-B-A inside one natural fallback window is attributed exactly");

  const std::array ordinaryIntervening{ReplayWindowProvenance{}};
  checkEq(dxmt9::encoders::classifyNaturalFallbackReentry(
              a0, ordinaryIntervening, a1),
          NaturalFallbackReentryRelation::CrossWindow,
          "an ordinary intervening pass prevents same-window attribution");

  const auto laterA = natural(42u, 0u);
  checkEq(dxmt9::encoders::classifyNaturalFallbackReentry(
              a0, sameIntervening, laterA),
          NaturalFallbackReentryRelation::CrossWindow,
          "matching natural dispositions from different windows remain "
          "cross-window");

  constexpr std::uint32_t currentPassIndex = 5u;
  for (std::uint32_t distance = 2u; distance <= 4u; ++distance) {
    std::array<ReplayWindowProvenance, 4> recent{};
    for (std::uint32_t pass = currentPassIndex - distance;
         pass < currentPassIndex; ++pass) {
      recent[pass % recent.size()] = natural(41u, pass % 3u);
    }
    checkEq(dxmt9::encoders::
                classifyNaturalFallbackReentryFromRecentHistory(
                    a0, recent, currentPassIndex, distance, a1),
            NaturalFallbackReentryRelation::SameWindow,
            "d2-d4 recent-history ring wrap preserves the complete window");
  }

  std::array<ReplayWindowProvenance, 4> naturalIntervening{};
  naturalIntervening[3u] = natural(41u, 1u);
  checkEq(dxmt9::encoders::
              classifyNaturalFallbackReentryFromRecentHistory(
                  ReplayWindowProvenance{}, naturalIntervening, 4u, 1u,
                  ReplayWindowProvenance{}),
          NaturalFallbackReentryRelation::CrossWindow,
          "a natural-only intervening pass is a cross-window interval");

  checkEq(dxmt9::encoders::
              classifyNaturalFallbackReentryFromRecentHistory(
                  a0, naturalIntervening, 5u, 5u, a1),
          NaturalFallbackReentryRelation::Excluded,
          "history beyond the bounded d1-d4 attribution range is excluded");

  const ReplayWindowProvenance ordinary{};
  const std::array ordinaryOnly{ordinary, ordinary};
  checkEq(dxmt9::encoders::classifyNaturalFallbackReentry(
              ordinary, ordinaryOnly, ordinary),
          NaturalFallbackReentryRelation::Excluded,
          "ordinary A-B-B-A does not enter natural fallback attribution");
}

void activeSeedMergeJoinRequiresExactPhysicalTokenAndTarget() {
  using dxmt9::encoders::ActiveSeedMergeContinuationRelation;
  using dxmt9::encoders::ActiveSeedMergeJoinRelation;
  using dxmt9::encoders::ActiveSeedMergeTargetWitness;
  using dxmt9::encoders::ActiveSeedMergeTicketContext;
  using dxmt9::encoders::RenderPassInstanceToken;
  using dxmt9::encoders::ReplayWindowDisposition;
  using dxmt9::encoders::ReplayWindowProvenance;
  const RenderPassInstanceToken seed{.seqId = 71u, .encoderIndex = 9u};
  const ActiveSeedMergeTicketContext context{
      .seed = seed, .windowId = 81u, .sourceCount = 2u};
  const ActiveSeedMergeTargetWitness target{
      .retainedSourceIndex = 1u,
      .commandIndex = 3u,
      .mergeOrdinal = 0u,
      .mergeDistance = 1u,
  };
  const ReplayWindowProvenance current{
      .disposition = ReplayWindowDisposition::NaturalAfterMergeFallback,
      .windowId = 81u,
      .sourceIndex = 1u,
      .sourceCount = 2u,
  };
  const std::array intervening{ReplayWindowProvenance{
      .disposition = ReplayWindowDisposition::NaturalAfterMergeFallback,
      .windowId = 81u,
      .sourceIndex = 0u,
      .sourceCount = 2u,
  }};

  checkEq(dxmt9::encoders::classifyActiveSeedMergePassStart(
              context, target, current, 1u, 3u, seed, intervening),
          ActiveSeedMergeJoinRelation::Matched,
          "exact source/command/window/token joins the physical seed");
  checkEq(dxmt9::encoders::classifyActiveSeedMergePassStart(
              context, target, current, 1u, 2u, seed, intervening),
          ActiveSeedMergeJoinRelation::NotTarget,
          "a different command is not allowed to consume the ticket");
  checkEq(dxmt9::encoders::classifyActiveSeedMergePassStart(
              context, target, current, 1u, 3u,
              RenderPassInstanceToken{.seqId = 71u, .encoderIndex = 10u},
              intervening),
          ActiveSeedMergeJoinRelation::Mismatch,
          "a different physical encoder token rejects aggregate-key inference");
  auto wrongWindow = current;
  wrongWindow.windowId = 82u;
  checkEq(dxmt9::encoders::classifyActiveSeedMergePassStart(
              context, target, wrongWindow, 1u, 3u, seed, intervening),
          ActiveSeedMergeJoinRelation::Mismatch,
          "a target from another fallback window fails closed");
  checkEq(dxmt9::encoders::classifyActiveSeedMergePassStart(
              context, target, current, 1u, 3u, seed,
              std::span<const ReplayWindowProvenance>{}),
          ActiveSeedMergeJoinRelation::Mismatch,
          "unbounded re-entry distance cannot claim a seed bridge");
  const std::array wrongIntervening{ReplayWindowProvenance{
      .disposition = ReplayWindowDisposition::NaturalAfterMergeFallback,
      .windowId = 82u,
      .sourceIndex = 0u,
      .sourceCount = 2u,
  }};
  checkEq(dxmt9::encoders::classifyActiveSeedMergePassStart(
              context, target, current, 1u, 3u, seed,
              wrongIntervening),
          ActiveSeedMergeJoinRelation::Mismatch,
          "every intervening physical pass must belong to the current window");

  checkEq(dxmt9::encoders::classifyActiveSeedMergeContinuation(
              context, target, current, 1u, 3u, seed),
          ActiveSeedMergeContinuationRelation::Continued,
          "an exact target already inside the physical seed is continued");
  checkEq(dxmt9::encoders::classifyActiveSeedMergeContinuation(
              context, target, current, 1u, 2u, seed),
          ActiveSeedMergeContinuationRelation::NotTarget,
          "continuation cannot consume a neighboring command target");
  checkEq(dxmt9::encoders::classifyActiveSeedMergeContinuation(
              context, target, current, 1u, 3u,
              RenderPassInstanceToken{.seqId = 72u, .encoderIndex = 9u}),
          ActiveSeedMergeContinuationRelation::Mismatch,
          "continuation requires the exact active physical seed token");
}

void activeSeedTargetResolutionDoesNotAssumeCommandOrder() {
  using dxmt9::encoders::ActiveSeedMergeTargetResolver;
  using dxmt9::encoders::ActiveSeedMergeTargetWitness;
  const std::array targets{
      ActiveSeedMergeTargetWitness{
          .retainedSourceIndex = 0u,
          .commandIndex = 1u,
          .mergeOrdinal = 0u,
          .mergeDistance = 1u,
      },
      ActiveSeedMergeTargetWitness{
          .retainedSourceIndex = 0u,
          .commandIndex = 7u,
          .mergeOrdinal = 1u,
          .mergeDistance = 2u,
      },
  };
  ActiveSeedMergeTargetResolver resolver{targets};
  resolver.beginCommand(0u, 7u);
  check(resolver.currentTarget() == &targets[1] &&
            resolver.consumeCurrent(),
        "reordered high command resolves its exact source-local target");
  resolver.endCommand();
  resolver.beginCommand(0u, 1u);
  check(resolver.currentTarget() == &targets[0] &&
            resolver.consumeCurrent(),
        "later low command remains independently resolvable");
  resolver.endCommand();
  checkEq(resolver.unconsumed(), std::size_t{0},
          "non-monotonic replay consumes both exact tickets");
}

void activeSeedDiagnosticTokenIsSeparateAndEmptyLookupIsCold() {
  using dxmt9::encoders::ActiveSeedInstanceRevalidation;
  using dxmt9::encoders::RenderPassInstanceToken;
  const RenderPassInstanceToken planned{.seqId = 4u, .encoderIndex = 8u};
  checkEq(dxmt9::encoders::classifyActiveSeedInstanceRevalidation(
              planned, planned),
          ActiveSeedInstanceRevalidation::Available,
          "equal diagnostic tokens may publish attribution tickets");
  checkEq(dxmt9::encoders::classifyActiveSeedInstanceRevalidation(
              planned,
              RenderPassInstanceToken{.seqId = 4u, .encoderIndex = 9u}),
          ActiveSeedInstanceRevalidation::Stale,
          "token-only mismatch is diagnostic stale, not semantic stale");
  checkEq(dxmt9::encoders::classifyActiveSeedInstanceRevalidation(
              planned, std::nullopt),
          ActiveSeedInstanceRevalidation::Unavailable,
          "missing observation token drops attribution conservatively");

  dxmt9::encoders::ActiveSeedMergeTargetResolver empty{};
  empty.beginCommand(0u, 7u);
  check(empty.currentTarget() == nullptr && empty.unconsumed() == 0u,
        "empty/perf-off target storage has no lookup result or terminal work");
  const dxmt9::encoders::ActiveSeedMergeTicketContext context{
      .seed = planned, .windowId = 2u, .sourceCount = 2u};
  check(!dxmt9::encoders::activeSeedMergeTicketAttributionEnabled(
            false, context, 1u) &&
            !dxmt9::encoders::activeSeedMergeTicketAttributionEnabled(
                true, context, 0u),
        "perf-off or empty-target calls do not activate the audit path");
}

void shortReentryDispositionAndCloseCountersConserve() {
  using dxmt9::encoders::ReplayWindowDisposition;
  using dxmt9::encoders::ReplayWindowProvenance;
  using dxmt9::encoders::ShortReentryClearOpenTarget;
  using dxmt9::encoders::ShortReentryDisposition;
  using dxmt9::encoders::ShortReentrySourceShape;
  using dxmt9::perf::EncoderSplitReason;

  const auto window = [](ReplayWindowDisposition disposition,
                         std::uint64_t id,
                         std::uint32_t sourceIndex = 0u) {
    return ReplayWindowProvenance{
        .disposition = disposition,
        .windowId = id,
        .sourceIndex = sourceIndex,
        .sourceCount = 3u,
    };
  };
  const auto classifyD1 = [&](ReplayWindowProvenance prior,
                              ReplayWindowProvenance middle,
                              ReplayWindowProvenance current) {
    const std::array intervening{middle};
    return dxmt9::encoders::classifyShortReentryDisposition(
        prior, intervening, current);
  };

  const ReplayWindowProvenance ordinary{};
  const auto natural0 = window(
      ReplayWindowDisposition::NaturalAfterMergeFallback, 11u, 0u);
  const auto natural1 = window(
      ReplayWindowDisposition::NaturalAfterMergeFallback, 11u, 1u);
  const auto natural2 = window(
      ReplayWindowDisposition::NaturalAfterMergeFallback, 11u, 2u);
  checkEq(classifyD1(ordinary, ordinary, ordinary),
          ShortReentryDisposition::Ordinary,
          "canonical ordinary interval has its own bucket");
  checkEq(classifyD1(natural0, natural1, natural2),
          ShortReentryDisposition::NaturalSameWindow,
          "complete natural interval preserves one window identity");
  checkEq(classifyD1(
              natural0, natural1,
              window(ReplayWindowDisposition::NaturalAfterMergeFallback,
                     12u, 2u)),
          ShortReentryDisposition::NaturalCrossWindow,
          "natural window identity change is classified as cross-window");
  checkEq(classifyD1(natural0, ordinary, ordinary),
          ShortReentryDisposition::NaturalCrossWindow,
          "any incomplete natural interval agrees with legacy cross-window "
          "attribution");

  const std::array homogeneousCases{
      std::pair{ReplayWindowDisposition::PlannedComposite,
                ShortReentryDisposition::PlannedComposite},
      std::pair{ReplayWindowDisposition::EligibilityPresent,
                ShortReentryDisposition::EligibilityPresent},
      std::pair{ReplayWindowDisposition::EligibilityOther,
                ShortReentryDisposition::EligibilityOther},
      std::pair{ReplayWindowDisposition::PermutationRejectedFallback,
                ShortReentryDisposition::PermutationRejected},
  };
  for (const auto& [disposition, expected] : homogeneousCases) {
    checkEq(classifyD1(window(disposition, 20u, 0u),
                       window(disposition, 20u, 1u),
                       window(disposition, 20u, 2u)),
            expected,
            "homogeneous complete interval maps to one disposition bucket");
  }
  checkEq(classifyD1(
              window(ReplayWindowDisposition::EligibilityPresent, 25u),
              ordinary, ordinary),
          ShortReentryDisposition::EligibilityPresent,
          "ordinary windows are transparent around one eligibility cause");
  checkEq(classifyD1(
              window(ReplayWindowDisposition::PlannedComposite, 30u),
              ordinary,
              window(ReplayWindowDisposition::EligibilityOther, 31u, 2u)),
          ShortReentryDisposition::MixedOrInvalid,
          "different special dispositions remain explicitly mixed");
  auto malformed = ordinary;
  malformed.windowId = 1u;
  checkEq(classifyD1(malformed, ordinary, ordinary),
          ShortReentryDisposition::MixedOrInvalid,
          "noncanonical ordinary provenance is invalid");

  const std::array d2Intervening{natural1, natural2};
  checkEq(dxmt9::encoders::classifyShortReentryDisposition(
              natural0, d2Intervening, natural0),
          ShortReentryDisposition::NaturalSameWindow,
          "d2 classification consumes both intervening replay windows");
  const std::array tooLong{ordinary, ordinary, ordinary};
  checkEq(dxmt9::encoders::classifyShortReentryDisposition(
              ordinary, tooLong, ordinary),
          ShortReentryDisposition::MixedOrInvalid,
          "unretained intervals fail into the explicit invalid bucket");

  const auto classifySourceShape = [](std::uint64_t prior,
                                      std::initializer_list<std::uint64_t> middle,
                                      std::uint64_t current) {
    return dxmt9::encoders::classifyShortReentrySourceShape(
        prior, std::span<const std::uint64_t>(middle.begin(), middle.size()),
        current);
  };
  checkEq(classifySourceShape(10u, {10u}, 10u),
          ShortReentrySourceShape::AllSameSource,
          "d1 entirely inside one source is classified exactly");
  checkEq(classifySourceShape(10u, {10u}, 11u),
          ShortReentrySourceShape::PriorAndInterveningSameCurrentNewer,
          "d1 suffix ends before a newer returning source");
  checkEq(classifySourceShape(10u, {11u}, 11u),
          ShortReentrySourceShape::PriorOlderInterveningAndCurrentSame,
          "d1 newer source owns the intervening and returning passes");
  checkEq(classifySourceShape(10u, {11u}, 12u),
          ShortReentrySourceShape::MixedOrInvalid,
          "three distinct d1 sources remain mixed");
  checkEq(classifySourceShape(20u, {20u, 20u}, 20u),
          ShortReentrySourceShape::AllSameSource,
          "d2 entirely inside one source is classified exactly");
  checkEq(classifySourceShape(20u, {20u, 20u}, 21u),
          ShortReentrySourceShape::PriorAndInterveningSameCurrentNewer,
          "both d2 intervening passes must match the prior source");
  checkEq(classifySourceShape(20u, {21u, 21u}, 21u),
          ShortReentrySourceShape::PriorOlderInterveningAndCurrentSame,
          "both d2 intervening passes must match the returning source");
  checkEq(classifySourceShape(20u, {20u, 21u}, 21u),
          ShortReentrySourceShape::MixedOrInvalid,
          "split d2 intervening ownership is not collapsed directionally");
  checkEq(classifySourceShape(20u, {20u, 20u}, 19u),
          ShortReentrySourceShape::MixedOrInvalid,
          "non-monotonic source identities remain invalid");
  checkEq(classifySourceShape(0u, {20u}, 20u),
          ShortReentrySourceShape::MixedOrInvalid,
          "missing source identity remains explicit");

  const auto clearOpenTarget = [&](std::uint32_t distance,
                                   ShortReentryDisposition disposition,
                                   ShortReentrySourceShape sourceShape,
                                   bool openedWithClear,
                                   std::optional<EncoderSplitReason> reason) {
    return dxmt9::encoders::classifyShortReentryClearOpenTarget(
        distance, disposition, sourceShape, openedWithClear, reason);
  };
  const auto directionalShape =
      ShortReentrySourceShape::PriorAndInterveningSameCurrentNewer;
  checkEq(clearOpenTarget(1u, ShortReentryDisposition::Ordinary,
                          directionalShape, true,
                          EncoderSplitReason::ClearBarrier),
          ShortReentryClearOpenTarget::Exact,
          "d1 directional clear-open return is the exact target");
  checkEq(clearOpenTarget(1u, ShortReentryDisposition::NaturalCrossWindow,
                          directionalShape, true,
                          EncoderSplitReason::ClearBarrier),
          ShortReentryClearOpenTarget::NaturalCross,
          "natural cross-window target remains an exact subset");
  checkEq(clearOpenTarget(2u, ShortReentryDisposition::NaturalCrossWindow,
                          directionalShape, true,
                          EncoderSplitReason::ClearBarrier),
          ShortReentryClearOpenTarget::Excluded,
          "d2 does not enter the narrow clear-open target");
  checkEq(clearOpenTarget(1u, ShortReentryDisposition::NaturalCrossWindow,
                          ShortReentrySourceShape::AllSameSource, true,
                          EncoderSplitReason::ClearBarrier),
          ShortReentryClearOpenTarget::Excluded,
          "same-source returns do not enter the directional target");
  checkEq(clearOpenTarget(1u, ShortReentryDisposition::NaturalCrossWindow,
                          directionalShape, false,
                          EncoderSplitReason::ClearBarrier),
          ShortReentryClearOpenTarget::Excluded,
          "a Clear close without a clear-open B pass is excluded");
  checkEq(clearOpenTarget(1u, ShortReentryDisposition::NaturalCrossWindow,
                          directionalShape, true, std::nullopt),
          ShortReentryClearOpenTarget::Excluded,
          "missing exact close identity fails closed");
  checkEq(clearOpenTarget(1u, ShortReentryDisposition::NaturalCrossWindow,
                          directionalShape, true,
                          EncoderSplitReason::RenderTargetChange),
          ShortReentryClearOpenTarget::Excluded,
          "a non-Clear prior close is excluded");

  const auto before =
      dxmt9::perf::test::snapshotRenderPassShortReentryAttribution();
  dxmt9::perf::countRenderPassShortReentryClearOpenTarget(false, 12u, 20u);
  dxmt9::perf::countRenderPassShortReentryClearOpenTarget(true, 30u, 40u);
  for (std::uint8_t disposition = 0u; disposition < 8u; ++disposition) {
    dxmt9::perf::countRenderPassShortReentryDisposition(1u, disposition);
    dxmt9::perf::countRenderPassShortReentryDisposition(2u, disposition);
    const std::uint8_t sourceShape = disposition % 4u;
    dxmt9::perf::countRenderPassShortReentrySourceShape(1u, sourceShape);
    dxmt9::perf::countRenderPassShortReentrySourceShape(2u, sourceShape);
  }
  constexpr std::array closeReasons{
      EncoderSplitReason::Final,
      EncoderSplitReason::RenderTargetChange,
      EncoderSplitReason::Hazard,
      EncoderSplitReason::ClearBarrier,
      EncoderSplitReason::SurfaceCopy,
      EncoderSplitReason::StretchRect,
      EncoderSplitReason::Readback,
      EncoderSplitReason::ColorFill,
      EncoderSplitReason::Present,
      EncoderSplitReason::PresentAcquire,
      EncoderSplitReason::TileMidPassIneligible,
      EncoderSplitReason::OrderedControl,
  };
  for (const auto reason : closeReasons) {
    dxmt9::perf::countRenderPassShortReentryPriorClose(reason);
  }
  for (std::uint32_t i = 0u; i < 4u; ++i) {
    dxmt9::perf::countRenderPassShortReentryPriorCloseMissing();
  }
  const auto after =
      dxmt9::perf::test::snapshotRenderPassShortReentryAttribution();
  std::uint64_t dispositionDelta = 0u;
  std::uint64_t sourceShapeDelta = 0u;
  for (std::size_t i = 0; i < 8u; ++i) {
    checkEq(after.distance1Disposition[i] - before.distance1Disposition[i],
            std::uint64_t{1}, "each d1 disposition increments exactly once");
    checkEq(after.distance2Disposition[i] - before.distance2Disposition[i],
            std::uint64_t{1}, "each d2 disposition increments exactly once");
    dispositionDelta +=
        after.distance1Disposition[i] - before.distance1Disposition[i] +
        after.distance2Disposition[i] - before.distance2Disposition[i];
  }
  for (std::size_t i = 0; i < 4u; ++i) {
    checkEq(after.distance1SourceShape[i] - before.distance1SourceShape[i],
            std::uint64_t{2},
            "each d1 source shape receives two synthetic reentries");
    checkEq(after.distance2SourceShape[i] - before.distance2SourceShape[i],
            std::uint64_t{2},
            "each d2 source shape receives two synthetic reentries");
    sourceShapeDelta +=
        after.distance1SourceShape[i] - before.distance1SourceShape[i] +
        after.distance2SourceShape[i] - before.distance2SourceShape[i];
  }
  std::uint64_t closeDelta =
      after.priorCloseMissing - before.priorCloseMissing;
  for (std::size_t i = 0; i < closeReasons.size(); ++i) {
    checkEq(after.priorCloseReason[i] - before.priorCloseReason[i],
            std::uint64_t{1}, "each exact close reason increments once");
    closeDelta += after.priorCloseReason[i] - before.priorCloseReason[i];
  }
  checkEq(dispositionDelta, std::uint64_t{16},
          "d1+d2 disposition buckets conserve all short reentries");
  checkEq(sourceShapeDelta, dispositionDelta,
          "d1+d2 source-shape buckets conserve all short reentries");
  checkEq(closeDelta, dispositionDelta,
          "exact prior-close reasons plus missing conserve the same total");
  checkEq(after.clearOpenTargetCount - before.clearOpenTargetCount,
          std::uint64_t{2}, "all exact clear-open targets are counted");
  checkEq(after.clearOpenTargetPriorStoreBytes -
              before.clearOpenTargetPriorStoreBytes,
          std::uint64_t{42}, "exact target prior Store bytes are summed");
  checkEq(after.clearOpenTargetCurrentLoadBytes -
              before.clearOpenTargetCurrentLoadBytes,
          std::uint64_t{60}, "exact target current Load bytes are summed");
  checkEq(after.clearOpenNaturalCrossCount -
              before.clearOpenNaturalCrossCount,
          std::uint64_t{1}, "natural cross remains a target subset");
  checkEq(after.clearOpenNaturalCrossPriorStoreBytes -
              before.clearOpenNaturalCrossPriorStoreBytes,
          std::uint64_t{30},
          "natural cross prior Store bytes are attributed exactly");
  checkEq(after.clearOpenNaturalCrossCurrentLoadBytes -
              before.clearOpenNaturalCrossCurrentLoadBytes,
          std::uint64_t{40},
          "natural cross current Load bytes are attributed exactly");
}

void physicalPassCloseLedgerRequiresExactTokensAndConservesFrame() {
  using dxmt9::encoders::RenderPassCloseKey;
  using dxmt9::encoders::RenderPassCloseLedger;
  using dxmt9::encoders::RenderPassCloseRecord;
  using dxmt9::encoders::RenderPassCloseTerminalRelation;
  using dxmt9::encoders::RenderPassInstanceToken;
  using dxmt9::encoders::SessionFinalizeCause;
  using dxmt9::perf::EncoderSplitReason;

  const RenderPassCloseKey a{.color0 = 11u, .depth = 21u, .sampleCount = 1u};
  const RenderPassCloseKey b{.color0 = 12u, .depth = 21u, .sampleCount = 1u};
  const RenderPassInstanceToken a0{.seqId = 30u, .encoderIndex = 7u};
  const RenderPassInstanceToken b0{.seqId = 31u, .encoderIndex = 8u};
  RenderPassCloseLedger<2> ledger;
  check(ledger.noteClose(RenderPassCloseRecord{
            .token = a0,
            .key = a,
            .splitReason = EncoderSplitReason::Final,
            .finalizeCause = SessionFinalizeCause::SessionCap,
            .storeBytes = 12u,
        }),
        "a valid exact close enters bounded storage");

  const auto wrong = ledger.noteStart(
      RenderPassInstanceToken{.seqId = 30u, .encoderIndex = 9u}, a, a0);
  check(wrong.terminal == RenderPassCloseTerminalRelation::None &&
            wrong.shortCrossPriorSplitReason == EncoderSplitReason::Final &&
            wrong.shortCrossPriorStoreBytes == 12u,
        "terminalization requires the exact token while an explicit exact "
        "cross lookup remains available");
  const auto nonAdjacent = ledger.noteStart(a0, b);
  check(nonAdjacent.terminal ==
            RenderPassCloseTerminalRelation::AdjacentDifferentKey &&
            nonAdjacent.terminalCause == SessionFinalizeCause::SessionCap,
        "the next exact pass terminalizes the prior close once");

  check(ledger.noteClose(RenderPassCloseRecord{
            .token = b0,
            .key = b,
            .splitReason = EncoderSplitReason::Hazard,
            .finalizeCause = SessionFinalizeCause::FailOrOther,
        }),
        "the second close fits the fixed ledger");
  const auto adjacent = ledger.noteStart(b0, b);
  check(adjacent.terminal ==
            RenderPassCloseTerminalRelation::AdjacentSameKey,
        "same-key adjacency is attributed only through the exact prior token");
  const auto cross = ledger.noteStart({}, a, a0);
  check(cross.shortCrossPriorSplitReason == EncoderSplitReason::Final &&
            cross.shortCrossPriorStoreBytes == 12u,
        "a later short re-entry recovers the exact prior A close reason");
  const auto missingCross = ledger.noteStart(
      {}, a,
      RenderPassInstanceToken{.seqId = a0.seqId,
                              .encoderIndex = a0.encoderIndex + 1u});
  check(missingCross.shortCrossLookupAttempted &&
            !missingCross.shortCrossPriorSplitReason &&
            missingCross.shortCrossPriorStoreBytes == 0u,
        "a wrong-token short cross remains an explicit attribution miss");
  const auto terminal = ledger.finishFrame();
  checkEq(terminal.notReopenedBeforePresent, std::size_t{0},
          "both recorded closes were terminalized before Present");
  checkEq(ledger.size(), std::size_t{0},
          "Present deterministically clears bounded close history");

  check(!ledger.noteClose(RenderPassCloseRecord{}),
        "invalid tokens fail closed without consuming capacity");
  check(ledger.noteClose(RenderPassCloseRecord{
            .token = a0, .key = a,
            .splitReason = EncoderSplitReason::Present,
        }),
        "a final frame close can remain unmatched until Present");
  checkEq(ledger.finishFrame().notReopenedBeforePresent, std::size_t{1},
          "Present supplies the deterministic not-reopened terminal");
}

}  // namespace

int main() {
  setenv("DXMT_PERF_COUNTERS", "1", 1);
  try {
    identityTraversesMixedSourceOrder();
    productionPlannerThresholdsAreDeterministic();
    productionPlannerUsesFinalMixedReplaySelection();
    productionPlannerPreservesMergeChainsAndFailsOpenBoundedly();
    productionPlannerKeepsTransitiveMergeChainWhole();
    canonicalPartitionSelectorResolvesQueueImmutableModes();
    replayStreamValidProvesActiveOrderContract();
    identityUsesFramegraphOrderAndDceSelection();
    identityUsesPartialSessionSourceRange();
    identityRetainsEmptyAndMalformedDrawRunsAsCommands();
    validTwoAndThreeSubrangePlansGroupCommandOnce();
    identitySerialBatchCarriesResolvedFullDrawRun();
    invalidPlansFailAllOrNothingToIdentity();
    partitionBoundariesLimitCompatibleIndexedMergeCandidates();
    naturalFallbackReentryRequiresTheCompleteSameWindowInterval();
    activeSeedMergeJoinRequiresExactPhysicalTokenAndTarget();
    activeSeedTargetResolutionDoesNotAssumeCommandOrder();
    activeSeedDiagnosticTokenIsSeparateAndEmptyLookupIsCold();
    shortReentryDispositionAndCloseCountersConserve();
    physicalPassCloseLedgerRequiresExactTokensAndConservesFrame();
  } catch (const std::exception& error) {
    std::cerr << "encode_partition_serial_spec: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
