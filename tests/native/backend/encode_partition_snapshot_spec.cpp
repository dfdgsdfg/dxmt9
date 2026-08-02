#include "../../../src/dxmt9/dxmt9_encode_session.hpp"
#include "../../../src/dxmt9/render/backend_interface.hpp"

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
  check(!options.partitionEntry.has_value(),
        "existing backend options default partition entry empty");
}

dxmt9::encoders::EncodeChunkOptions forwardBackendOptions(
    dxmt9::encoders::EncodeChunkOptions options) {
  return options;
}

void optionalPartitionEntryForwardsByValueWithExistingOptions() {
  dxmt9::encoders::EncodeChunkOptions options{};
  options.partitionEntry = makeSnapshot();
  options.disableMidChunkCommits = true;
  options.allowInjectedCommandBufferMidChunkCommits = true;
  options.deferSessionFinalization = true;

  auto forwarded = forwardBackendOptions(std::move(options));
  check(forwarded.partitionEntry.has_value(),
        "backend option forwarding preserves optional partition entry");
  checkEq(*forwarded.partitionEntry, makeSnapshot(),
          "backend option forwarding preserves the complete snapshot value");
  check(forwarded.disableMidChunkCommits,
        "backend option forwarding preserves disable-mid-chunk option");
  check(forwarded.allowInjectedCommandBufferMidChunkCommits,
        "backend option forwarding preserves injected-buffer split option");
  check(forwarded.deferSessionFinalization,
        "partition metadata cannot alter serial session finalization option");
}

}  // namespace

int main() {
  try {
    snapshotCopiesAsAnIndependentValue();
    sourceLocatorKeepsRetainedOrderAndReuseIdentity();
    encodeOptionsDefaultToSerialWithoutPartitionMetadata();
    optionalPartitionEntryForwardsByValueWithExistingOptions();
  } catch (const std::exception& error) {
    std::cerr << "encode_partition_snapshot_spec: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
