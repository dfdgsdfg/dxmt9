#include "../../../src/dxmt9/dxmt9_draw_encoder_internal.hpp"
#include "../../../src/dxmt9/dxmt9_encode_partition.hpp"

#include <array>
#include <cstdint>
#include <exception>
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

}  // namespace

int main() {
  try {
    identityTraversesMixedSourceOrder();
    replayStreamValidProvesActiveOrderContract();
    identityUsesFramegraphOrderAndDceSelection();
    identityUsesPartialSessionSourceRange();
    identityRetainsEmptyAndMalformedDrawRunsAsCommands();
    validTwoAndThreeSubrangePlansGroupCommandOnce();
    identitySerialBatchCarriesResolvedFullDrawRun();
    invalidPlansFailAllOrNothingToIdentity();
    partitionBoundariesLimitCompatibleIndexedMergeCandidates();
  } catch (const std::exception& error) {
    std::cerr << "encode_partition_serial_spec: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
