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
        4, slot, begin, count, false, {});
  }

  EncodePartitionReplayStream ordered(
      std::span<const std::uint32_t> order) const {
    return dxmt9::encoders::makeEncodePartitionReplayStream(
        4, slot, 0, slot.commandCount(), true, order);
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
      3, slot, 0, slot.commandCount(), false, {});
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
      2, slot, 0, 1, false, {});
  const auto identity = identityRanges(stream);
  checkEq(identity.size(), std::size_t{1},
          "merge fixture identity is one full draw range");
  const auto full = dxmt9::encoders::resolveEncodePartition(
      identity.front(), stream);
  check(full, "identity full range resolves");
  checkEq(dxmt9::encoders::makeCompatibleIndexedDrawMerge(
              full.partition.drawParams, {}).drawCount,
          std::size_t{3},
          "identity full range retains the existing merge candidate shape");

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
    identityUsesFramegraphOrderAndDceSelection();
    identityUsesPartialSessionSourceRange();
    identityRetainsEmptyAndMalformedDrawRunsAsCommands();
    validTwoAndThreeSubrangePlansGroupCommandOnce();
    invalidPlansFailAllOrNothingToIdentity();
    partitionBoundariesLimitCompatibleIndexedMergeCandidates();
  } catch (const std::exception& error) {
    std::cerr << "encode_partition_serial_spec: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
