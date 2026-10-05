#include "dxmt9/dxmt9_draw_fetch_counts.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using namespace dxmt9::encoders;
using Primitive = dxmt9::core::PrimitiveType;
using Index = dxmt9::core::IndexType;
using Status = DrawFetchCountStatus;
constexpr auto maximum32 = std::numeric_limits<std::uint32_t>::max();
constexpr auto maximum64 = std::numeric_limits<std::uint64_t>::max();
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

constexpr std::array<std::uint64_t, 6> expectedPrimitiveCounts(std::uint64_t count) {
  return {count, count * 2u, count + 1u, count * 3u, count + 2u, count + 2u};
}

static_assert(encoderPrimitiveVertexCount(Primitive::LineList, maximum32 / 2u + 1u) == 0u);
static_assert(encoderPrimitiveVertexCount(Primitive::LineStrip, maximum32) == 0u);
static_assert(encoderIndexElementSize(static_cast<Index>(maximum32)) == 4u);
static_assert(qualifyEncoderDrawCounts(Primitive::TriangleList, 0u).vertexCount == 3u);
static_assert(qualifyEncoderIndexExtent(Index::UInt16, 3u, 2u).byteEnd == 10u);

void exhaustiveSmallPrimitiveDomain() {
  for (std::uint32_t raw = 0u; raw < 16u; ++raw) {
    const auto type = static_cast<Primitive>(raw);
    for (std::uint32_t count = 0u; count < 65u; ++count) {
      const auto rawExpected = expectedPrimitiveCounts(count);
      check(encoderPrimitiveVertexCount(type, count) == (raw < rawExpected.size()
              ? static_cast<std::uint32_t>(rawExpected[raw]) : 0u),
          "shared production formula preserves every small primitive enum/count result");
      const auto qualified = qualifyEncoderDrawCounts(type, count);
      if (raw < rawExpected.size()) {
        const auto effectiveCount = std::max(1u, count);
        const auto effectiveExpected = expectedPrimitiveCounts(effectiveCount);
        check(qualified.qualified() && qualified.primitiveCount == effectiveCount &&
            qualified.vertexCount == std::max(1u, static_cast<std::uint32_t>(effectiveExpected[raw])),
            "checked draw counts apply actual encoder primitive and vertex minimum-one clamps");
      } else {
        check(!qualified.qualified() && qualified.status == Status::UnknownPrimitiveType &&
            !qualified.primitiveCount && !qualified.vertexCount,
            "cold qualification rejects unknown primitive without promoting production fallback");
      }
    }
  }
}

void primitiveWrapAndQualificationBoundaries() {
  const std::array counts{0u, 1u, 2u, maximum32 / 3u, maximum32 / 3u + 1u,
      maximum32 / 2u, maximum32 / 2u + 1u, maximum32 - 2u, maximum32 - 1u, maximum32};
  for (std::uint32_t raw = 0u; raw < 6u; ++raw) {
    const auto type = static_cast<Primitive>(raw);
    for (const auto count : counts) {
      const auto rawExpected = expectedPrimitiveCounts(count)[raw];
      check(encoderPrimitiveVertexCount(type, count) == static_cast<std::uint32_t>(rawExpected),
          "production multiplication/addition still wraps in the existing u32 domain");
      const auto effectiveCount = std::max(1u, count);
      const auto wideExpected = expectedPrimitiveCounts(effectiveCount)[raw];
      const auto qualified = qualifyEncoderDrawCounts(type, count);
      if (wideExpected <= maximum32) {
        check(qualified.qualified() && qualified.primitiveCount == effectiveCount &&
            qualified.vertexCount == std::max(1u, encoderPrimitiveVertexCount(type, effectiveCount)),
            "representable boundary qualification equals the exact production call-site count");
      } else {
        check(!qualified.qualified() && qualified.status == Status::VertexCountOverflow &&
            !qualified.primitiveCount && !qualified.vertexCount,
            "wide boundary overflow rejects qualification without changing production wrap/clamp");
      }
    }
  }
  check(qualifyEncoderDrawCounts(Primitive::PointList, maximum32).vertexCount == maximum32 &&
      qualifyEncoderDrawCounts(Primitive::LineList, maximum32 / 2u).qualified() &&
      qualifyEncoderDrawCounts(Primitive::LineStrip, maximum32 - 1u).vertexCount == maximum32 &&
      qualifyEncoderDrawCounts(Primitive::TriangleList, maximum32 / 3u).vertexCount == maximum32 &&
      qualifyEncoderDrawCounts(Primitive::TriangleStrip, maximum32 - 2u).vertexCount == maximum32 &&
      qualifyEncoderDrawCounts(Primitive::TriangleFan, maximum32 - 2u).vertexCount == maximum32,
      "each primitive formula accepts its exact last representable boundary");
  check(std::max(1u, encoderPrimitiveVertexCount(Primitive::LineList, maximum32 / 2u + 1u)) == 1u &&
      std::max(1u, encoderPrimitiveVertexCount(Primitive::LineStrip, maximum32)) == 1u &&
      std::max(1u, encoderPrimitiveVertexCount(Primitive::TriangleStrip, maximum32 - 1u)) == 1u,
      "existing encoder vertex minimum-one behavior after wrapped zero is retained");
  for (const auto raw : {6u, 7u, maximum32}) {
    for (const auto count : counts) {
      const auto type = static_cast<Primitive>(raw);
      check(encoderPrimitiveVertexCount(type, count) == 0u &&
          qualifyEncoderDrawCounts(type, count).status == Status::UnknownPrimitiveType,
          "unknown primitive stays zero in production but is never cold-qualified");
    }
  }
}

void exhaustiveSmallIndexDomain() {
  for (std::uint32_t raw = 0u; raw < 8u; ++raw) {
    const auto type = static_cast<Index>(raw);
    const std::size_t expectedSize = raw == 0u ? 2u : 4u;
    check(encoderIndexElementSize(type) == expectedSize,
        "production UInt16 size and every other-enum four-byte fallback are unchanged");
    for (std::uint64_t first = 0u; first < 9u; ++first) {
      for (std::uint64_t count = 0u; count < 17u; ++count) {
        const auto result = qualifyEncoderIndexExtent(type, first, count);
        if (raw < 2u) {
          check(result.qualified() && result.elementBytes == expectedSize &&
              result.byteOffset == first * expectedSize && result.byteSize == count * expectedSize &&
              result.byteEnd == (first + count) * expectedSize,
              "checked UInt16/UInt32 extent uses actual element size and permits empty intervals");
        } else {
          check(!result.qualified() && result.status == Status::UnknownIndexType &&
              !result.elementBytes && !result.byteOffset && !result.byteSize && !result.byteEnd,
              "unknown index type rejects even zero-count qualification, without changing fallback");
        }
      }
    }
  }
}

void indexExtentOverflowBoundaries() {
  for (const auto type : {Index::UInt16, Index::UInt32}) {
    const auto size = encoderIndexElementSize(type);
    const auto limit = maximum64 / size;
    const auto greatestAlignedByte = limit * size;
    const auto empty = qualifyEncoderIndexExtent(type, 0u, 0u);
    const auto lastEmpty = qualifyEncoderIndexExtent(type, limit, 0u);
    const auto lastSize = qualifyEncoderIndexExtent(type, 0u, limit);
    const auto lastEnd = qualifyEncoderIndexExtent(type, limit - 1u, 1u);
    check(empty.qualified() && !empty.byteOffset && !empty.byteSize && !empty.byteEnd &&
        lastEmpty.qualified() && lastEmpty.byteOffset == greatestAlignedByte &&
        !lastEmpty.byteSize && lastEmpty.byteEnd == greatestAlignedByte &&
        lastSize.qualified() && lastSize.byteSize == greatestAlignedByte &&
        lastSize.byteEnd == greatestAlignedByte && lastEnd.qualified() &&
        lastEnd.byteEnd == greatestAlignedByte,
        "offset, size and summed extent accept equality at their last aligned u64 boundary");
    check(qualifyEncoderIndexExtent(type, limit + 1u, 0u).status == Status::IndexOffsetOverflow &&
        qualifyEncoderIndexExtent(type, 0u, limit + 1u).status == Status::IndexSizeOverflow &&
        qualifyEncoderIndexExtent(type, limit, 1u).status == Status::IndexEndOverflow,
        "one-over offset, size and individually-valid offset-plus-size overflow fail distinctly");
    check(qualifyEncoderIndexExtent(type, maximum64, maximum64).status == Status::IndexOffsetOverflow,
        "overflow precedence is deterministic and never performs wrapping multiplication");
    const auto draw = qualifyEncoderDrawCounts(Primitive::TriangleList, 0u);
    const auto extent = qualifyEncoderIndexExtent(type, 7u, draw.vertexCount);
    check(draw.qualified() && extent.qualified() && extent.byteOffset == 7u * size &&
        extent.byteSize == 3u * size && extent.byteEnd == 10u * size,
        "zero input primitive count still observes the actual minimum-one triangle index count");
    const auto maximumDraw = qualifyEncoderDrawCounts(Primitive::PointList, maximum32);
    const auto actualU32Extent = qualifyEncoderIndexExtent(type, maximum32, maximumDraw.vertexCount);
    check(maximumDraw.qualified() && actualU32Extent.qualified() &&
        actualU32Extent.byteEnd == static_cast<std::uint64_t>(maximum32) * size * 2u,
        "the actual u32 draw/start-index domain remains exactly representable in u64 index extents");
  }
  check(encoderIndexElementSize(static_cast<Index>(maximum32)) == 4u &&
      qualifyEncoderIndexExtent(static_cast<Index>(maximum32), 0u, 0u).status == Status::UnknownIndexType,
      "maximum unknown index enum preserves production fallback but fails cold qualification");
}

} // namespace

int main() {
  try {
    exhaustiveSmallPrimitiveDomain();
    primitiveWrapAndQualificationBoundaries();
    exhaustiveSmallIndexDomain();
    indexExtentOverflowBoundaries();
    std::cout << "draw fetch counts: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "draw fetch counts: " << error.what() << '\n';
    return 1;
  }
}
