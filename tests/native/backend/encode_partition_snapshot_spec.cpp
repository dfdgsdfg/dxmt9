#include "../../../src/dxmt9/dxmt9_encode_session.hpp"
#include "../../../src/dxmt9/dxmt9_encode_partition.hpp"
#include "../../../src/dxmt9/render/backend_interface.hpp"

#include <array>
#include <cstring>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using dxmt9::encoders::EncodePartitionEntrySnapshot;
using dxmt9::encoders::EncodePartitionEntryValidation;
using dxmt9::encoders::EncodePartitionRangeKind;
using dxmt9::encoders::EncodePartitionRangeSnapshot;
using dxmt9::encoders::RetainedEncodeSourceLocator;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    fail(std::string(message));
  }
}

static_assert(std::is_trivially_copyable_v<RetainedEncodeSourceLocator>);
static_assert(std::is_standard_layout_v<RetainedEncodeSourceLocator>);
static_assert(sizeof(RetainedEncodeSourceLocator) == 16);
static_assert(std::is_trivially_copyable_v<EncodePartitionEntrySnapshot>);
static_assert(std::is_standard_layout_v<EncodePartitionEntrySnapshot>);
static_assert(sizeof(EncodePartitionEntrySnapshot) == 64);
static_assert(std::is_trivially_copyable_v<EncodePartitionRangeSnapshot>);
static_assert(std::is_standard_layout_v<EncodePartitionRangeSnapshot>);
static_assert(sizeof(EncodePartitionRangeSnapshot) == 80);
static_assert(!std::is_pointer_v<decltype(EncodePartitionEntrySnapshot::source)>);
static_assert(!std::is_pointer_v<decltype(EncodePartitionEntrySnapshot::uniformHandle)>);
static_assert(!std::is_pointer_v<decltype(
    EncodePartitionEntrySnapshot::bindingOverrideBytes)>);
static_assert(!std::is_pointer_v<decltype(
    EncodePartitionEntrySnapshot::bindingSnapshotBytes)>);

EncodePartitionEntrySnapshot makeSnapshot() {
  return EncodePartitionEntrySnapshot{
      .source = RetainedEncodeSourceLocator{
          .sourceOrdinal = 3,
          .slotIndex = 7,
          .seqId = 0x123456789abcdef0ull,
      },
      .commandIndex = 11,
      .drawRunRecordIndex = 5,
      .stateIndex = 9,
      .drawParamIndex = 13,
      .uniformHandle = dxmt9::core::DrawUniformHandle{
          .index = 17,
          .generation = 19,
          .hash = 0xfeedfacecafebeefull,
      },
      .bindingOverrideBytes = dxmt9::core::DrawPayloadRange{
          .offset = 23,
          .size = 29,
      },
      .bindingSnapshotBytes = dxmt9::core::DrawPayloadRange{
          .offset = 31,
          .size = 37,
      },
  };
}

void sourceLocatorKeepsRetainedOrderAndReuseIdentity() {
  auto first = makeSnapshot();
  first.source = {.sourceOrdinal = 0, .slotIndex = 7, .seqId = 41};
  auto second = makeSnapshot();
  second.source = {.sourceOrdinal = 1, .slotIndex = 8, .seqId = 42};
  auto reused = makeSnapshot();
  reused.source = {.sourceOrdinal = 2, .slotIndex = 7, .seqId = 43};
  const std::vector<EncodePartitionEntrySnapshot> order{first, second, reused};

  checkEq(order[0].source.sourceOrdinal, std::uint32_t{0},
          "retained order starts at ordinal zero");
  checkEq(order[1].source.sourceOrdinal, std::uint32_t{1},
          "retained order preserves the successor ordinal");
  checkEq(order[2].source.seqId, std::uint64_t{43},
          "retained order preserves sequence identity");
  checkEq(order[0].source.slotIndex, order[2].source.slotIndex,
          "a queue slot may be reused later");
  check(!(order[0].source == order[2].source),
        "ordinal and seqId reject stale identity after slot reuse");
}

void snapshotCopiesAsAnIndependentValue() {
  const EncodePartitionEntrySnapshot original = makeSnapshot();
  EncodePartitionEntrySnapshot copy = original;

  checkEq(copy, original, "snapshot copy preserves every locator");
  copy.commandIndex += 1;
  copy.bindingOverrideBytes.offset += 1;
  check(!(copy == original), "snapshot copy has independent value storage");
  checkEq(original.commandIndex, std::uint32_t{11},
          "copy mutation does not alter original command locator");
  checkEq(original.bindingOverrideBytes.offset, std::uint32_t{23},
          "copy mutation does not alter original payload locator");
}

void encodeOptionsDefaultToSerialWithoutPartitionMetadata() {
  dxmt9::encoders::EncodeChunkOptions options{};
  check(options.partitionRanges.empty(),
        "existing backend options default partition ranges empty");
  check(options.sessionLookaheadSources.empty(),
        "existing backend options default retained source table empty");
}

dxmt9::encoders::EncodeChunkOptions forwardBackendOptions(
    dxmt9::encoders::EncodeChunkOptions options) {
  return options;
}

void partitionRangeSpanForwardsWithExistingOptions() {
  dxmt9::encoders::EncodeChunkOptions options{};
  const std::array<EncodePartitionRangeSnapshot, 1> ranges{
      EncodePartitionRangeSnapshot{
          .kind = EncodePartitionRangeKind::DrawRunEntries,
          .replayOrdinalBegin = 2,
          .replayOrdinalCount = 1,
          .drawEntryCount = 3,
          .entry = makeSnapshot(),
      },
  };
  options.partitionRanges = ranges;
  std::array<dxmt9::core::metalqueue::ReadySlotSnapshot, 1> sources{};
  options.sessionLookaheadSources = sources;
  options.disableMidChunkCommits = true;
  options.allowInjectedCommandBufferMidChunkCommits = true;
  options.deferSessionFinalization = true;

  auto forwarded = forwardBackendOptions(std::move(options));
  checkEq(forwarded.partitionRanges.data(), ranges.data(),
          "backend option forwarding preserves the call-local range span");
  checkEq(forwarded.partitionRanges.front(), ranges.front(),
          "backend option forwarding preserves the complete range snapshot");
  checkEq(forwarded.sessionLookaheadSources.data(), sources.data(),
          "backend option forwarding preserves the call-local source table");
  check(forwarded.disableMidChunkCommits,
        "backend option forwarding preserves disable-mid-chunk option");
  check(forwarded.allowInjectedCommandBufferMidChunkCommits,
        "backend option forwarding preserves injected-buffer split option");
  check(forwarded.deferSessionFinalization,
        "partition metadata cannot alter serial session finalization option");
}

struct ResolverFixture {
  dxmt9::core::ChunkSlot slot{};
  EncodePartitionEntrySnapshot snapshot{};
  std::array<dxmt9::core::metalqueue::ReadySlotSnapshot, 1> sources{};

  ResolverFixture() {
    slot.seqId = 41;

    std::array<dxmt9::core::DrawParam, 1> firstDraw{};
    std::array<dxmt9::core::u8, 3> firstPayloadBytes{1, 2, 3};
    std::array<dxmt9::core::DrawParamPayloadView, 1> firstPayload{
        dxmt9::core::DrawParamPayloadView{
            .userVertexData = firstPayloadBytes,
        },
    };
    slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                       dxmt9::core::DrawUniformPayload{}, firstDraw,
                       firstPayload);

    std::array<dxmt9::core::DrawParam, 1> secondDraw{};
    dxmt9::core::DrawBindingOverride bindingOverride{};
    bindingOverride.alphaTestStateValid = true;
    bindingOverride.alphaTestRef = 0x7fu;
    dxmt9::core::DrawBindingSnapshot bindingSnapshot{};
    bindingSnapshot.streamMask = 1u;
    std::array<dxmt9::core::u8,
               sizeof(dxmt9::core::DrawBindingOverride)> overrideBytes{};
    std::array<dxmt9::core::u8,
               sizeof(dxmt9::core::DrawBindingSnapshot)> snapshotBytes{};
    std::memcpy(overrideBytes.data(), &bindingOverride, overrideBytes.size());
    std::memcpy(snapshotBytes.data(), &bindingSnapshot, snapshotBytes.size());
    std::array<dxmt9::core::DrawParamPayloadView, 1> secondPayload{
        dxmt9::core::DrawParamPayloadView{
            .bindingOverrideData = overrideBytes,
            .bindingSnapshotData = snapshotBytes,
        },
    };
    slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                       dxmt9::core::DrawUniformPayload{}, secondDraw,
                       secondPayload);

    checkEq(slot.commandCount(), std::size_t{2},
            "resolver fixture contains two draw-run commands");
    const auto command = slot.drawRunCommandAt(1);
    check(command.drawRunRecord && command.drawParams.size() == 1,
          "resolver fixture target draw-run is complete");
    const auto& record = *command.drawRunRecord;
    const auto& param = command.drawParams.front();
    const auto effectiveUniform = param.uniformHandle.valid()
        ? param.uniformHandle
        : record.uniformHandle;
    snapshot = EncodePartitionEntrySnapshot{
        .source = RetainedEncodeSourceLocator{
            .sourceOrdinal = 0,
            .slotIndex = 7,
            .seqId = slot.seqId,
        },
        .commandIndex = 1,
        .drawRunRecordIndex = 1,
        .stateIndex = record.stateIndex,
        .drawParamIndex = record.firstParam,
        .uniformHandle = effectiveUniform,
        .bindingOverrideBytes = dxmt9::core::DrawPayloadRange{
            .offset = record.payloadOffset + param.bindingOverrideRange.offset,
            .size = param.bindingOverrideRange.size,
        },
        .bindingSnapshotBytes = dxmt9::core::DrawPayloadRange{
            .offset = record.payloadOffset + param.bindingSnapshotRange.offset,
            .size = param.bindingSnapshotRange.size,
        },
    };
    sources[0] = dxmt9::core::metalqueue::ReadySlotSnapshot{
        .slotIndex = 7,
        .seqId = slot.seqId,
        .hasPresent = false,
        .commandBegin = 1,
        .commandCount = 1,
        .slot = &slot,
    };
  }
};

void retainedSourceResolverReturnsCallLocalViewsForValidMetadata() {
  ResolverFixture fixture;
  const auto resolved = dxmt9::encoders::resolveEncodePartitionEntry(
      fixture.snapshot, fixture.sources);

  check(static_cast<bool>(resolved), "complete retained metadata resolves");
  check(resolved.validation == EncodePartitionEntryValidation::Valid,
        "successful resolution reports Valid");
  checkEq(resolved.entry.slot, &fixture.slot,
          "resolved entry borrows the selected retained slot");
  checkEq(resolved.entry.drawRunRecord,
          &fixture.slot.drawRunRecords[1],
          "resolved entry points at the selected draw-run record");
  checkEq(resolved.entry.drawParam,
          &fixture.slot.drawParams[fixture.snapshot.drawParamIndex],
          "resolved entry points at the selected draw parameter");
  check(resolved.entry.uniform != nullptr,
        "resolved entry validates the effective uniform handle");
  checkEq(resolved.entry.bindingOverrideBytes.size(),
          sizeof(dxmt9::core::DrawBindingOverride),
          "resolved entry exposes the binding override bytes");
  checkEq(resolved.entry.bindingSnapshotBytes.size(),
          sizeof(dxmt9::core::DrawBindingSnapshot),
          "resolved entry exposes the binding snapshot bytes");
  check(fixture.snapshot.bindingOverrideBytes.offset > 0,
        "fixture proves snapshot payload locators are arena-absolute");
}

void retainedSourceResolverRejectsEveryLocatorLayer() {
  ResolverFixture fixture;
  auto expect = [&](EncodePartitionEntrySnapshot snapshot,
                    EncodePartitionEntryValidation validation,
                    std::string_view message) {
    const auto resolved = dxmt9::encoders::resolveEncodePartitionEntry(
        snapshot, fixture.sources);
    check(!static_cast<bool>(resolved), message);
    check(resolved.validation == validation, message);
  };

  auto snapshot = fixture.snapshot;
  snapshot.source.sourceOrdinal = 1;
  expect(snapshot, EncodePartitionEntryValidation::SourceOrdinalOutOfRange,
         "out-of-range source ordinal is rejected");

  snapshot = fixture.snapshot;
  snapshot.source.seqId += 1;
  expect(snapshot, EncodePartitionEntryValidation::SourceIdentityMismatch,
         "stale source sequence is rejected");

  snapshot = fixture.snapshot;
  snapshot.commandIndex = 0;
  expect(snapshot, EncodePartitionEntryValidation::CommandIndexOutOfRange,
         "command outside the retained source range is rejected");

  snapshot = fixture.snapshot;
  snapshot.drawRunRecordIndex = 0;
  expect(snapshot, EncodePartitionEntryValidation::DrawRunRecordMismatch,
         "mismatched draw-run record index is rejected");

  snapshot = fixture.snapshot;
  snapshot.stateIndex += 1;
  expect(snapshot, EncodePartitionEntryValidation::StateIndexMismatch,
         "mismatched state index is rejected");

  snapshot = fixture.snapshot;
  snapshot.drawParamIndex += 1;
  expect(snapshot, EncodePartitionEntryValidation::DrawParamIndexMismatch,
         "draw parameter outside the selected run is rejected");

  snapshot = fixture.snapshot;
  snapshot.uniformHandle.generation += 1;
  expect(snapshot, EncodePartitionEntryValidation::UniformHandleMismatch,
         "mismatched uniform generation is rejected");

  snapshot = fixture.snapshot;
  snapshot.bindingOverrideBytes.offset += 1;
  expect(snapshot, EncodePartitionEntryValidation::BindingOverrideRangeMismatch,
         "mismatched absolute binding override range is rejected");

  snapshot = fixture.snapshot;
  snapshot.bindingSnapshotBytes.size -= 1;
  expect(snapshot, EncodePartitionEntryValidation::BindingSnapshotRangeMismatch,
         "mismatched binding snapshot schema size is rejected");

  auto invalidSources = fixture.sources;
  invalidSources[0].commandCount = fixture.slot.commandCount();
  const auto invalidRange = dxmt9::encoders::resolveEncodePartitionEntry(
      fixture.snapshot, invalidSources);
  check(invalidRange.validation ==
            EncodePartitionEntryValidation::SourceCommandRangeInvalid,
        "retained source range outside the slot is rejected");
}

}  // namespace

int main() {
  try {
    snapshotCopiesAsAnIndependentValue();
    sourceLocatorKeepsRetainedOrderAndReuseIdentity();
    encodeOptionsDefaultToSerialWithoutPartitionMetadata();
    partitionRangeSpanForwardsWithExistingOptions();
    retainedSourceResolverReturnsCallLocalViewsForValidMetadata();
    retainedSourceResolverRejectsEveryLocatorLayer();
  } catch (const std::exception& error) {
    std::cerr << "encode_partition_snapshot_spec: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
