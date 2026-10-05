#include "render_tape_cpu_source_rows.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

// Standalone cold-allocation faults only. There is no production/harness hook.
namespace {
std::atomic<std::ptrdiff_t> allocationFailureAfter{-1};
}

void* operator new(std::size_t bytes) {
  auto remaining = allocationFailureAfter.load(std::memory_order_relaxed);
  while (remaining >= 0) {
    if (allocationFailureAfter.compare_exchange_weak(
            remaining, remaining - 1, std::memory_order_relaxed)) {
      if (!remaining) throw std::bad_alloc();
      break;
    }
  }
  if (auto* memory = std::malloc(bytes ? bytes : 1u)) return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using namespace dxmt9;
using namespace dxmt9::test;
using Status = CpuTapeSourceRowsStatus;
using Primitive = core::PrimitiveType;
using Index = core::IndexType;
using Range = resources::BufferReadProofRange;
constexpr auto maximum32 = std::numeric_limits<std::uint32_t>::max();
constexpr auto maximum64 = std::numeric_limits<std::uint64_t>::max();
std::size_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

struct AllocationFailure {
  explicit AllocationFailure(std::ptrdiff_t count) { allocationFailureAfter.store(count); }
  ~AllocationFailure() { allocationFailureAfter.store(-1); }
};

template <typename Element>
std::vector<std::uint8_t> indexBytes(std::span<const Element> values) {
  std::vector<std::uint8_t> bytes(values.size() * sizeof(Element));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

CpuTapeSourceRowsInput indexedInput(Index type, std::span<const std::uint8_t> bytes) {
  CpuTapeSourceRowsInput input;
  input.parameters.primitiveType = Primitive::TriangleList;
  input.parameters.primitiveCount = 1u;
  input.parameters.indexed = true;
  input.parameters.indexType = type;
  input.actualEffectiveIndexed = input.hasBoundIndexBuffer = true;
  input.sourceBase = 3u;
  input.sourceStride = 8u;
  input.sourceByteSize = 131u;
  input.expectedIndexBytes = bytes;
  return input;
}

CpuTapeSourceRowsPlan sentinel() {
  CpuTapeSourceRowsPlan out;
  out.effectiveIndexed = true;
  out.qualifiedPrimitiveCount = 7u;
  out.sourceElementCount = 8u;
  out.uniqueVertexRows = 9u;
  out.plannedReadBytes = 10u;
  out.indexRange = Range{111u, 222u};
  out.vertexRanges.push_back({333u, 444u});
  return out;
}

void unchanged(const CpuTapeSourceRowsPlan& out, const Range* storage) {
  check(out.effectiveIndexed && out.qualifiedPrimitiveCount == 7u && out.sourceElementCount == 8u &&
      out.uniqueVertexRows == 9u && out.plannedReadBytes == 10u && out.indexRange &&
      out.indexRange->byteOffset == 111u && out.indexRange->byteSize == 222u &&
      out.vertexRanges.size() == 1u && out.vertexRanges[0].byteOffset == 333u &&
      out.vertexRanges[0].byteSize == 444u && out.vertexRanges.data() == storage,
      "failure preserves every output field and its existing owned storage");
}

void rejects(const CpuTapeSourceRowsInput& input, Status expected, CpuTapeSourceRowsLimits limits = {}) {
  auto out = sentinel();
  const auto* storage = out.vertexRanges.data();
  check(buildCpuTapeSourceRowsPlan(input, out, limits) == expected, "intended planner rejection status");
  unchanged(out, storage);
}

void adjacencyDuplicatesAndExactIndexRange() {
  for (const auto type : {Index::UInt16, Index::UInt32}) {
    const std::array<std::uint32_t, 5u> values32{99u, 8u, 2u, 3u, 99u};
    const std::array<std::uint16_t, 5u> values16{99u, 8u, 2u, 3u, 99u};
    const auto bytes = type == Index::UInt16 ? indexBytes<std::uint16_t>(values16)
                                            : indexBytes<std::uint32_t>(values32);
    auto input = indexedInput(type, bytes);
    input.parameters.startIndex = 1u;
    CpuTapeSourceRowsPlan out;
    check(buildCpuTapeSourceRowsPlan(input, out) == Status::Qualified && out.effectiveIndexed &&
        out.sourceElementCount == 3u && out.qualifiedPrimitiveCount == 1u && out.uniqueVertexRows == 3u &&
        out.indexRange && out.indexRange->byteOffset == encoders::encoderIndexElementSize(type) &&
        out.indexRange->byteSize == 3u * encoders::encoderIndexElementSize(type) &&
        out.vertexRanges.size() == 2u && out.vertexRanges[0].byteOffset == 19u &&
        out.vertexRanges[0].byteSize == 16u && out.vertexRanges[1].byteOffset == 67u &&
        out.vertexRanges[1].byteSize == 8u &&
        out.plannedReadBytes == 24u + 3u * encoders::encoderIndexElementSize(type),
        "exact IB slice excludes prefix/tail, sorted adjacent rows merge without filling row4..7 gap");
  }
  const std::array<std::uint16_t, 3u> duplicates{4u, 2u, 4u};
  const auto bytes = indexBytes<std::uint16_t>(duplicates);
  CpuTapeSourceRowsPlan out;
  check(buildCpuTapeSourceRowsPlan(indexedInput(Index::UInt16, bytes), out) == Status::Qualified &&
      out.sourceElementCount == 3u && out.uniqueVertexRows == 2u && out.vertexRanges.size() == 2u &&
      out.vertexRanges[0].byteOffset == 19u && out.vertexRanges[0].byteSize == 8u &&
      out.vertexRanges[1].byteOffset == 35u && out.vertexRanges[1].byteSize == 8u &&
      out.plannedReadBytes == 22u,
      "duplicates do not consume extra VB comparison bytes or cause an envelope merge");
}

void exhaustiveSmallIndexedRows() {
  for (const auto type : {Index::UInt16, Index::UInt32}) {
    for (std::int32_t base = -1; base <= 1; ++base) {
      for (std::uint32_t code = 0u; code < 512u; ++code) {
        const std::array<std::uint32_t, 3u> values32{code % 8u, (code / 8u) % 8u, code / 64u};
        const std::array<std::uint16_t, 3u> values16{
            static_cast<std::uint16_t>(values32[0]), static_cast<std::uint16_t>(values32[1]),
            static_cast<std::uint16_t>(values32[2])};
        const auto bytes = type == Index::UInt16 ? indexBytes<std::uint16_t>(values16)
                                                : indexBytes<std::uint32_t>(values32);
        auto input = indexedInput(type, bytes);
        input.parameters.baseVertexIndex = base;
        input.sourceStride = 5u;
        input.sourceByteSize = 48u; // base3 + nine complete rows
        const bool negative = base < 0 && std::any_of(values32.begin(), values32.end(),
            [](auto value) { return value == 0u; });
        if (negative) { rejects(input, Status::SourceAddressRejected); continue; }
        std::array<bool, 9u> selected{};
        for (const auto value : values32) selected[static_cast<std::size_t>(std::int64_t{value} + base)] = true;
        CpuTapeSourceRowsPlan out;
        check(buildCpuTapeSourceRowsPlan(input, out) == Status::Qualified, "defined signed indexed rows qualify");
        std::size_t uniqueRows = 0u;
        for (std::size_t row = 0u; row < selected.size(); ++row) {
          uniqueRows += selected[row];
          const auto offset = 3u + row * 5u;
          const bool covered = std::any_of(out.vertexRanges.begin(), out.vertexRanges.end(),
              [&](const auto& range) {
                return offset >= range.byteOffset && offset - range.byteOffset < range.byteSize;
              });
          check(covered == selected[row], "every possible row is covered iff actually selected, with no gap fill");
        }
        check(out.uniqueVertexRows == uniqueRows &&
            out.plannedReadBytes == uniqueRows * 5u + 3u * encoders::encoderIndexElementSize(type),
            "deduplicated full-stride byte accounting conserves all selected rows");
      }
    }
  }
}

void nonindexedOriginalIntervalAndEffectiveFallthrough() {
  CpuTapeSourceRowsInput input;
  input.parameters.primitiveType = Primitive::TriangleList;
  input.parameters.primitiveCount = 130u;
  input.parameters.startVertex = 5738u;
  input.sourceStride = 16u;
  input.sourceByteSize = 98048u;
  CpuTapeSourceRowsPlan out;
  check(buildCpuTapeSourceRowsPlan(input, out) == Status::Qualified && !out.effectiveIndexed && !out.indexRange &&
      out.sourceElementCount == 390u && out.uniqueVertexRows == 390u && out.vertexRanges.size() == 1u &&
      out.vertexRanges[0].byteOffset == 91808u && out.vertexRanges[0].byteSize == 6240u &&
      out.vertexRanges[0].byteOffset + out.vertexRanges[0].byteSize == 98048u && out.plannedReadBytes == 6240u,
      "original sole nonindexed draw derives [91808,98048), including exact end equality");
  --input.sourceByteSize;
  rejects(input, Status::SourceAddressRejected);
  ++input.sourceByteSize;
  input.parameters.indexed = true; // No bound IB: production's effective predicate is false.
  input.parameters.indexType = static_cast<Index>(maximum32);
  input.parameters.baseVertexIndex = std::numeric_limits<std::int32_t>::min();
  check(buildCpuTapeSourceRowsPlan(input, out) == Status::Qualified && !out.effectiveIndexed && !out.indexRange,
      "provided false effective predicate preserves existing indexed-request/no-IB fallthrough, not GPU success");
  input.actualEffectiveIndexed = true;
  rejects(input, Status::EffectiveIndexedMismatch);
}

void allKnownPrimitiveCounts() {
  for (std::uint32_t raw = 0u; raw < 7u; ++raw) {
    for (std::uint32_t count = 0u; count < 5u; ++count) {
      CpuTapeSourceRowsInput input;
      input.parameters.primitiveType = static_cast<Primitive>(raw);
      input.parameters.primitiveCount = count;
      input.parameters.startVertex = 1u;
      input.sourceBase = 2u;
      input.sourceStride = 7u;
      input.sourceByteSize = 4096u;
      if (raw == 6u) { rejects(input, Status::CountQualificationRejected); continue; }
      const auto effective = count ? count : 1u;
      const std::array expected{effective, effective * 2u, effective + 1u,
          effective * 3u, effective + 2u, effective + 2u};
      CpuTapeSourceRowsPlan out;
      check(buildCpuTapeSourceRowsPlan(input, out) == Status::Qualified &&
          out.qualifiedPrimitiveCount == effective && out.sourceElementCount == expected[raw] &&
          out.uniqueVertexRows == expected[raw] && out.vertexRanges.size() == 1u &&
          out.vertexRanges[0].byteOffset == 9u && out.vertexRanges[0].byteSize == expected[raw] * 7u,
          "known source-row primitive formulas preserve both shared minimum-one clamps without claiming expansion");
    }
  }
}

void domainCountsAndBounds() {
  const std::array<std::uint16_t, 3u> values{0u, 1u, 2u};
  const auto bytes = indexBytes<std::uint16_t>(values);
  const auto valid = indexedInput(Index::UInt16, bytes);
  for (std::uint32_t mutation = 0u; mutation < 7u; ++mutation) {
    auto input = valid;
    if (mutation == 0u) input.streamMask = 0u;
    if (mutation == 1u) input.streamMask = 3u;
    if (mutation == 2u) input.streamFrequency = 0x80000002u;
    if (mutation == 3u) input.parameters.instanceCount = 2u;
    if (mutation == 4u) input.parameters.userVertexRange = {maximum32, 1u};
    if (mutation == 5u) input.parameters.userIndexRange = {maximum32, 1u};
    if (mutation == 6u) input.sourceStride = 0u;
    rejects(input, Status::UnsupportedDomain);
  }
  auto input = valid;
  input.parameters.userVertexRange = input.parameters.userIndexRange = {maximum32, 0u};
  CpuTapeSourceRowsPlan out;
  check(buildCpuTapeSourceRowsPlan(input, out) == Status::Qualified,
      "zero-size UP ranges are explicitly absent regardless of offset, matching DrawPayloadRange semantics");
  input = valid;
  input.actualEffectiveIndexed = false;
  rejects(input, Status::EffectiveIndexedMismatch);
  input = valid;
  input.parameters.primitiveType = static_cast<Primitive>(maximum32);
  rejects(input, Status::CountQualificationRejected);
  input = valid;
  input.parameters.primitiveCount = maximum32;
  rejects(input, Status::CountQualificationRejected);
  input = valid;
  input.parameters.primitiveCount = 0u;
  check(buildCpuTapeSourceRowsPlan(input, out) == Status::Qualified && out.qualifiedPrimitiveCount == 1u &&
      out.sourceElementCount == 3u, "shared encoder minimum-one primitive/count behavior is preserved");
  input = valid;
  input.parameters.indexType = static_cast<Index>(maximum32);
  rejects(input, Status::IndexExtentRejected);
  input = valid;
  input.expectedIndexBytes = input.expectedIndexBytes.first(5u);
  rejects(input, Status::IndexBoundsExceeded);
  input = valid;
  input.parameters.startIndex = maximum32;
  rejects(input, Status::IndexBoundsExceeded);
  input = valid;
  input.sourceByteSize = 26u; // base3 + three rows requires27
  rejects(input, Status::SourceAddressRejected);
  input.sourceByteSize = 27u;
  check(buildCpuTapeSourceRowsPlan(input, out) == Status::Qualified && out.vertexRanges[0].byteSize == 24u,
      "last full-stride row may end exactly at complete logical VB extent");
}

void signedAndArithmeticBoundaries() {
  for (const auto value : {0x80000000u, maximum32}) {
    const std::array values{value, value, value};
    const auto bytes = indexBytes<std::uint32_t>(values);
    auto input = indexedInput(Index::UInt32, bytes);
    input.sourceByteSize = maximum64;
    rejects(input, Status::SourceAddressRejected);
  }
  const std::array<std::uint32_t, 3u> lastSigned{0x7fffffffu, 0x7fffffffu, 0x7fffffffu};
  const auto bytes = indexBytes<std::uint32_t>(lastSigned);
  auto input = indexedInput(Index::UInt32, bytes);
  input.sourceBase = 0u;
  input.sourceStride = 1u;
  input.sourceByteSize = 0x80000000ull;
  CpuTapeSourceRowsPlan out;
  check(buildCpuTapeSourceRowsPlan(input, out) == Status::Qualified && out.uniqueVertexRows == 1u &&
      out.vertexRanges[0].byteOffset == 0x7fffffffu && out.vertexRanges[0].byteSize == 1u,
      "last representable signed UInt32 index is qualified without widening the production signed domain");
  input.parameters.baseVertexIndex = 1;
  rejects(input, Status::SourceAddressRejected);
  input.parameters.baseVertexIndex = std::numeric_limits<std::int32_t>::min();
  rejects(input, Status::SourceAddressRejected);
  CpuTapeSourceRowsInput direct;
  direct.parameters.primitiveType = Primitive::PointList;
  direct.parameters.primitiveCount = 1u;
  direct.sourceStride = 1u;
  direct.sourceBase = direct.sourceByteSize = maximum64;
  rejects(direct, Status::SourceAddressRejected); // byte-end addition overflow
  direct.sourceBase = 0u;
  direct.sourceStride = maximum64;
  direct.parameters.startVertex = 2u;
  rejects(direct, Status::SourceAddressRejected); // row-offset multiplication overflow
  direct.parameters.startVertex = 0u;
  direct.parameters.primitiveCount = 2u;
  rejects(direct, Status::SourceAddressRejected); // contiguous row-size multiplication overflow
  const std::array<std::uint16_t, 3u> zeroRows{};
  const auto zeroBytes = indexBytes<std::uint16_t>(zeroRows);
  auto aggregate = indexedInput(Index::UInt16, zeroBytes);
  aggregate.sourceBase = 0u;
  aggregate.sourceStride = maximum64 - 4u;
  aggregate.sourceByteSize = maximum64;
  rejects(aggregate, Status::ReadByteLimitExceeded, {3u, 2u, maximum64});
  aggregate.sourceStride = maximum64 - 6u;
  check(buildCpuTapeSourceRowsPlan(aggregate, out, {3u, 2u, maximum64}) == Status::Qualified &&
      out.plannedReadBytes == maximum64 && out.vertexRanges[0].byteSize == maximum64 - 6u,
      "aggregate IB+unique-VB bytes accept exact u64 maximum and reject one-over before addition wraps");
}

void limitsOriginalMaximumAndAllocationUnwind() {
  const std::array<std::uint16_t, 3u> holes{0u, 2u, 4u};
  const auto bytes = indexBytes<std::uint16_t>(holes);
  const auto input = indexedInput(Index::UInt16, bytes);
  for (const auto limits : {CpuTapeSourceRowsLimits{0u, 4u, 30u},
      CpuTapeSourceRowsLimits{3u, 0u, 30u}, CpuTapeSourceRowsLimits{3u, 4u, 0u},
      CpuTapeSourceRowsLimits{kCpuTapeSourceRowsMaxRows + 1u, 4u, 30u},
      CpuTapeSourceRowsLimits{3u, kCpuTapeSourceRowsMaxRanges + 1u, 30u}})
    rejects(input, Status::InvalidLimits, limits);
  rejects(input, Status::RowWorkLimitExceeded, {2u, 4u, 30u});
  rejects(input, Status::RangeLimitExceeded, {3u, 1u, 30u});
  rejects(input, Status::RangeLimitExceeded, {3u, 3u, 30u});
  rejects(input, Status::ReadByteLimitExceeded, {3u, 4u, 5u});
  rejects(input, Status::ReadByteLimitExceeded, {3u, 4u, 29u});
  CpuTapeSourceRowsPlan out;
  check(buildCpuTapeSourceRowsPlan(input, out, {3u, 4u, 30u}) == Status::Qualified &&
      out.vertexRanges.size() == 3u && out.plannedReadBytes == 30u,
      "work/range/aggregate byte caps accept equality, including one exact IB range");
  std::vector<std::uint16_t> originalRows(20898u);
  for (std::size_t index = 0u; index < originalRows.size(); ++index)
    originalRows[index] = static_cast<std::uint16_t>(index);
  const auto originalBytes = indexBytes<std::uint16_t>(originalRows);
  auto maximumOriginal = indexedInput(Index::UInt16, originalBytes);
  maximumOriginal.parameters.primitiveCount = 6966u;
  maximumOriginal.sourceBase = 0u;
  maximumOriginal.sourceStride = 12u;
  maximumOriginal.sourceByteSize = originalRows.size() * 12u;
  check(buildCpuTapeSourceRowsPlan(maximumOriginal, out) == Status::Qualified &&
      out.sourceElementCount == 20898u && out.uniqueVertexRows == 20898u && out.vertexRanges.size() == 1u &&
      out.vertexRanges[0].byteSize == 250776u && out.plannedReadBytes == 292572u,
      "default32768 work bound admits original max6966 triangle draw/20898 rows without widening hard bounds");
  rejects(maximumOriginal, Status::RowWorkLimitExceeded, {20897u, 32769u, 292572u});
  CpuTapeSourceRowsInput hardBoundary;
  hardBoundary.parameters.primitiveType = Primitive::PointList;
  hardBoundary.parameters.primitiveCount = static_cast<std::uint32_t>(kCpuTapeSourceRowsMaxRows);
  hardBoundary.sourceStride = 1u;
  hardBoundary.sourceByteSize = kCpuTapeSourceRowsMaxRows;
  check(buildCpuTapeSourceRowsPlan(hardBoundary, out,
          {kCpuTapeSourceRowsMaxRows, 1u, kCpuTapeSourceRowsMaxRows}) == Status::Qualified &&
      out.sourceElementCount == kCpuTapeSourceRowsMaxRows && out.vertexRanges.size() == 1u,
      "nonindexed contiguous rows accept exact hard work bound with one bounded owned range");
  ++hardBoundary.parameters.primitiveCount;
  rejects(hardBoundary, Status::RowWorkLimitExceeded,
      {kCpuTapeSourceRowsMaxRows, 1u, kCpuTapeSourceRowsMaxRows});
  for (const auto successes : {0, 1}) {
    auto preserved = sentinel();
    const auto* storage = preserved.vertexRanges.data();
    Status status;
    {
      AllocationFailure fail(successes);
      status = buildCpuTapeSourceRowsPlan(input, preserved);
    }
    check(status == Status::AllocationFailed, "both work-buffer and owned-output allocation failures are explicit");
    unchanged(preserved, storage);
  }
}

} // namespace

int main() {
  try {
    adjacencyDuplicatesAndExactIndexRange();
    exhaustiveSmallIndexedRows();
    nonindexedOriginalIntervalAndEffectiveFallthrough();
    allKnownPrimitiveCounts();
    domainCountsAndBounds();
    signedAndArithmeticBoundaries();
    limitsOriginalMaximumAndAllocationUnwind();
    std::cout << "CPU source rows: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CPU source rows: " << error.what() << '\n';
    return 1;
  }
}
