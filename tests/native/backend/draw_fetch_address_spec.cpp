#include "dxmt9/dxmt9_draw_fetch_address.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using namespace dxmt9::encoders;
using Index = dxmt9::core::IndexType;
using Primitive = dxmt9::core::PrimitiveType;
using Status = DrawFetchAddressStatus;
constexpr auto maximum32 = std::numeric_limits<std::uint32_t>::max();
constexpr auto maximum64 = std::numeric_limits<std::uint64_t>::max();
constexpr auto maximumSigned = std::numeric_limits<std::int32_t>::max();
constexpr auto minimumSigned = std::numeric_limits<std::int32_t>::min();
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

void checkRejected(const EncoderSourceRowsQualification& result, Status expected) {
  check(!result.qualified() && result.status == expected && !result.firstVertexRow &&
      !result.rowCount && !result.byteOffset && !result.byteSize && !result.byteEnd,
      "rejected row qualification exposes no partial interval");
}

std::vector<std::uint8_t> indexBytes(Index type, std::span<const std::uint32_t> values) {
  const auto size = type == Index::UInt16 ? 2u : 4u;
  std::vector<std::uint8_t> bytes(values.size() * size);
  for (std::size_t ordinal = 0u; ordinal < values.size(); ++ordinal) {
    if (type == Index::UInt16) {
      const auto value = static_cast<std::uint16_t>(values[ordinal]);
      std::memcpy(bytes.data() + ordinal * size, &value, sizeof(value));
    } else {
      const auto value = values[ordinal];
      std::memcpy(bytes.data() + ordinal * size, &value, sizeof(value));
    }
  }
  return bytes;
}

static_assert(encoderEffectiveIndexed(true, true, false));
static_assert(!encoderEffectiveIndexed(true, false, false));
static_assert(encoderStream0SourceStride(0u, true, 0u, 12u) == 0u);
static_assert(encoderStream0SourceStride(0u, false, 0u, 12u) == 12u);
static_assert(encoderNonIndexedStartByteDisplacement(false, maximum32, maximum32) ==
    static_cast<std::uint64_t>(maximum32) * maximum32);
static_assert(qualifyEncoderSourceRows(5738u, 390u, 0u, 16u, 98048u).byteOffset == 91808u);
static_assert(qualifyEncoderSourceRows(5738u, 390u, 0u, 16u, 98048u).byteEnd == 98048u);

void strideAndNormalizationDomain() {
  for (const bool requested : {false, true}) {
    for (const bool bound : {false, true}) {
      for (const bool up : {false, true}) {
        check(encoderEffectiveIndexed(requested, bound, up) == (requested && (bound || up)),
            "shared source-indexed predicate is not the requested flag alone");
      }
    }
  }
  for (std::uint32_t bound = 0u; bound < 5u; ++bound) {
    for (const bool present : {false, true}) {
      for (std::uint32_t ffp = 0u; ffp < 5u; ++ffp) {
        for (std::uint32_t computed = 0u; computed < 5u; ++computed) {
          const auto expected = bound ? bound : (present ? ffp : computed);
          check(encoderStream0SourceStride(bound, present, ffp, computed) == expected,
              "bound stride and present-zero FFP fallback preserve the production selector");
          const auto programmableExpected = present ? (bound ? bound : ffp) : computed;
          const auto programmableActual = encoderStream0SourceStride(present ? bound : 0u,
              present, present ? ffp : 0u, present ? 0u : computed);
          check(programmableActual == programmableExpected,
              "programmable call-site selection retains the no-layout computed-stride branch");
        }
      }
    }
  }
  constexpr bool noLayoutPresent = false;
  constexpr std::uint32_t nonzeroBoundStride = 32u;
  check(encoderStream0SourceStride(noLayoutPresent ? nonzeroBoundStride : 0u,
      noLayoutPresent, 0u, 12u) == 12u,
      "nonzero bound stride does not replace the programmable no-layout computed result");
  const std::array starts{0u, 1u, 5738u, 65510u, static_cast<std::uint32_t>(maximumSigned),
      static_cast<std::uint32_t>(maximumSigned) + 1u, maximum32};
  const std::array bases{minimumSigned, -1, 0, 1, 65510, maximumSigned};
  for (const bool indexed : {false, true}) {
    for (const auto start : starts) {
      for (const auto stride : {0u, 1u, 16u, 144u, maximum32}) {
        for (const auto base : bases) {
          const auto expectedOffset = !indexed && stride != 0u
              ? static_cast<std::uint64_t>(start) * static_cast<std::uint64_t>(stride) : 0u;
          const auto expectedBase = !indexed && stride != 0u
              ? 0 : (indexed ? base : static_cast<std::int32_t>(start));
          check(encoderNonIndexedStartByteDisplacement(indexed, start, stride) == expectedOffset &&
              encoderDrawVertexBaseIndex(indexed, start, base, stride) == expectedBase,
              "start displacement and shader base retain exact indexed/nonindexed normalization");
        }
      }
    }
  }
  check(encoderSourceRowByteOffset<std::uint32_t>(maximum32 - 3u, 1u, 8u) == 4u &&
      encoderSourceRowsByteSize<std::uint32_t>(maximum32, 2u) == maximum32 - 1u &&
      encoderIndexByteOffset<std::uint32_t>(maximum32, 1u, 4u) == 0u,
      "unqualified production unsigned arithmetic remains in its original width");
}

void exhaustiveSmallRowDomain() {
  for (std::uint64_t first = 0u; first < 9u; ++first) {
    for (std::uint64_t count = 0u; count < 9u; ++count) {
      for (std::uint64_t base = 0u; base < 4u; ++base) {
        for (std::uint64_t stride = 0u; stride < 9u; ++stride) {
          const auto expectedOffset = base + first * stride;
          const auto expectedSize = count * stride;
          const auto expectedEnd = expectedOffset + expectedSize;
          for (const auto extent : {0u, 1u, 8u, 32u, 64u, 128u, 256u}) {
            const auto result = qualifyEncoderSourceRows(first, count, base, stride, extent);
            if (!count) {
              checkRejected(result, Status::EmptyRange);
            } else if (!stride) {
              checkRejected(result, Status::ZeroStride);
            } else if (expectedEnd > extent) {
              checkRejected(result, Status::SourceRowsOutOfBounds);
            } else {
              check(result.qualified() && result.firstVertexRow == first && result.rowCount == count &&
                  result.byteOffset == expectedOffset && result.byteSize == expectedSize &&
                  result.byteEnd == expectedEnd,
                  "qualified contiguous rows equal independently computed small-domain addresses");
              check(encoderSourceRowByteOffset(base, first, stride) == result.byteOffset &&
                  encoderSourceRowsByteSize(count, stride) == result.byteSize,
                  "checked row arithmetic binds to the production scalar formulas");
            }
          }
        }
      }
    }
  }
}

void indexedDecodeAndSignedDomain() {
  const std::array bases{-4, -1, 0, 1, 65510, maximumSigned - 1, maximumSigned};
  std::array<std::uint8_t, 3> unaligned{};
  for (std::uint32_t value = 0u; value <= 65535u; ++value) {
    const auto narrow = static_cast<std::uint16_t>(value);
    std::memcpy(unaligned.data() + 1u, &narrow, sizeof(narrow));
    const auto bytes = std::span<const std::uint8_t>(unaligned).subspan(1u);
    check(decodeEncoderIndexValue(bytes.data(), Index::UInt16) == value,
        "every UInt16 value decodes safely from an unaligned complete element");
    for (const auto base : bases) {
      const auto expected = static_cast<std::int64_t>(base) + value;
      const auto row = qualifyEncoderIndexedSourceRow(bytes, Index::UInt16, 0u, 0u, base,
          3u, 144u, maximum64);
      if (expected < 0) {
        checkRejected(row, Status::NegativeSourceVertex);
      } else if (expected > maximumSigned) {
        checkRejected(row, Status::SourceVertexOverflow);
      } else {
        check(row.qualified() && row.firstVertexRow == static_cast<std::uint64_t>(expected) &&
            row.rowCount == 1u && row.byteOffset == 3u + static_cast<std::uint64_t>(expected) * 144u &&
            row.byteSize == 144u && row.byteEnd == row.byteOffset + 144u &&
            encoderIndexedSourceVertex(base, value) == expected,
            "indexed row qualifies exact signed-base arithmetic throughout the real UInt16 domain");
      }
    }
  }
  const std::array values{0u, 1u, 65535u, static_cast<std::uint32_t>(maximumSigned),
      static_cast<std::uint32_t>(maximumSigned) + 1u, maximum32};
  const auto bytes = indexBytes(Index::UInt32, values);
  for (std::size_t ordinal = 0u; ordinal < values.size(); ++ordinal) {
    const auto value = values[ordinal];
    for (const auto base : {minimumSigned, -1, 0, 1, maximumSigned}) {
      const auto expected = static_cast<std::int64_t>(base) + value;
      const auto row = qualifyEncoderIndexedSourceRow(bytes, Index::UInt32, 0u, ordinal, base,
          0u, 1u, maximum64);
      if (value > static_cast<std::uint32_t>(maximumSigned)) {
        checkRejected(row, Status::IndexValueOutsideSignedDomain);
      } else if (expected < 0) {
        checkRejected(row, Status::NegativeSourceVertex);
      } else if (expected > maximumSigned) {
        checkRejected(row, Status::SourceVertexOverflow);
      } else {
        check(row.qualified() && row.firstVertexRow == static_cast<std::uint64_t>(expected),
            "UInt32 qualification is restricted to existing defined signed-cast/addition semantics");
      }
    }
  }
  const auto unknown = static_cast<Index>(maximum32);
  check(decodeEncoderIndexValue(bytes.data() + 4u, unknown) == 1u,
      "unknown index type preserves the unqualified production four-byte decode fallback");
  checkRejected(qualifyEncoderIndexedSourceRow(bytes, unknown, 0u, 0u, 0, 0u, 1u, maximum64),
      Status::UnknownIndexType);
}

// Independent serial reference to the old expansion loop in its defined
// domain. These are pure value/copy tests, not physical Encode-byte evidence.
bool referenceExpansion(std::span<const std::uint8_t> source,
    std::span<const std::uint8_t> indices, Index type, std::uint32_t start,
    std::int32_t base, std::uint64_t count, std::size_t sourceBase,
    std::size_t stride, std::vector<std::uint8_t>& out) {
  if (source.empty() || indices.empty() || !stride) return false;
  const std::size_t size = type == Index::UInt16 ? 2u : 4u;
  out.assign(static_cast<std::size_t>(count) * stride, 0u);
  for (std::uint64_t ordinal = 0u; ordinal < count; ++ordinal) {
    const auto offset = static_cast<std::size_t>(start) * size +
        static_cast<std::size_t>(ordinal) * size;
    if (offset + size > indices.size()) return false;
    std::uint32_t value = 0u;
    if (type == Index::UInt16) {
      std::uint16_t narrow = 0u;
      std::memcpy(&narrow, indices.data() + offset, sizeof(narrow));
      value = narrow;
    } else {
      std::memcpy(&value, indices.data() + offset, sizeof(value));
    }
    const auto vertex = base + static_cast<std::int32_t>(value);
    if (vertex < 0) return false;
    const auto sourceOffset = sourceBase + static_cast<std::size_t>(vertex) * stride;
    if (sourceOffset + stride > source.size()) return false;
    std::memcpy(out.data() + static_cast<std::size_t>(ordinal) * stride,
        source.data() + sourceOffset, stride);
  }
  return true;
}

bool sharedExpansion(std::span<const std::uint8_t> source,
    std::span<const std::uint8_t> indices, Index type, std::uint32_t start,
    std::int32_t base, std::uint64_t count, std::size_t sourceBase,
    std::size_t stride, std::vector<std::uint8_t>& out) {
  if (source.empty() || indices.empty() || !stride) return false;
  const auto size = encoderIndexElementSize(type);
  out.assign(encoderSourceRowsByteSize(count, stride), 0u);
  for (std::uint64_t ordinal = 0u; ordinal < count; ++ordinal) {
    const auto offset = encoderIndexByteOffset<std::size_t>(start, ordinal, size);
    if (offset + size > indices.size()) return false;
    const auto vertex = encoderIndexedSourceVertex(base,
        decodeEncoderIndexValue(indices.data() + offset, type));
    if (vertex < 0) return false;
    const auto sourceOffset = encoderSourceRowByteOffset(sourceBase,
        static_cast<std::uint64_t>(vertex), stride);
    if (sourceOffset + stride > source.size()) return false;
    std::memcpy(out.data() + encoderSourceRowsByteSize(ordinal, stride),
        source.data() + sourceOffset, stride);
  }
  return true;
}

void exhaustiveDefinedExpansionDomain() {
  std::array<std::uint8_t, 128> source{};
  for (std::size_t ordinal = 0u; ordinal < source.size(); ++ordinal)
    source[ordinal] = static_cast<std::uint8_t>(ordinal * 17u + 3u);
  const std::array values{5u, 0u, 2u, 2u, 7u, 1u, 6u, 3u};
  std::vector<std::uint8_t> reference, shared;
  for (const auto type : {Index::UInt16, Index::UInt32, static_cast<Index>(7u)}) {
    const auto indices = indexBytes(type, values);
    for (std::size_t sourceBase = 0u; sourceBase < 3u; ++sourceBase) {
      for (std::size_t stride = 0u; stride < 7u; ++stride) {
        for (std::int32_t base = -4; base < 5; ++base) {
          for (std::uint32_t start = 0u; start < 4u; ++start) {
            for (std::uint64_t count = 0u; count < 10u; ++count) {
              const auto expected = referenceExpansion(source, indices, type, start, base,
                  count, sourceBase, stride, reference);
              const auto actual = sharedExpansion(source, indices, type, start, base,
                  count, sourceBase, stride, shared);
              check(actual == expected && shared == reference,
                  "shared scalar expansion preserves success/failure and partial/full copied bytes");
            }
          }
        }
      }
    }
  }
}

void overflowBoundsAndCountComposition() {
  checkRejected(qualifyEncoderSourceRows(0u, 0u, 0u, 0u, 0u), Status::EmptyRange);
  checkRejected(qualifyEncoderSourceRows(0u, 1u, 0u, 0u, maximum64), Status::ZeroStride);
  checkRejected(qualifyEncoderSourceRows(maximum64 / 2u + 1u, 1u, 0u, 2u, maximum64),
      Status::SourceOffsetOverflow);
  checkRejected(qualifyEncoderSourceRows(1u, 1u, maximum64, 1u, maximum64),
      Status::SourceOffsetOverflow);
  checkRejected(qualifyEncoderSourceRows(0u, maximum64 / 2u + 1u, 0u, 2u, maximum64),
      Status::SourceSizeOverflow);
  checkRejected(qualifyEncoderSourceRows(0u, 1u, maximum64, 1u, maximum64),
      Status::SourceEndOverflow);
  const auto last = qualifyEncoderSourceRows(0u, 1u, maximum64 - 1u, 1u, maximum64);
  check(last.qualified() && last.byteEnd == maximum64,
      "row end equality at the exact u64 boundary is qualified");
  checkRejected(qualifyEncoderSourceRows(5738u, 390u, 0u, 16u, 98047u),
      Status::SourceRowsOutOfBounds);
  const std::array values{3u, 5u, 7u};
  for (const auto type : {Index::UInt16, Index::UInt32}) {
    const auto bytes = indexBytes(type, values);
    const auto lastRow = qualifyEncoderIndexedSourceRow(bytes, type, 1u, 1u, 2,
        3u, 4u, 43u);
    check(lastRow.qualified() && lastRow.firstVertexRow == 9u &&
        lastRow.byteOffset == 39u && lastRow.byteEnd == 43u,
        "nonzero start and ordinal decode the exact last complete index element");
    checkRejected(qualifyEncoderIndexedSourceRow(bytes, type, 1u, 2u, 0,
        0u, 1u, maximum64), Status::IndexBytesUnavailable);
    checkRejected(qualifyEncoderIndexedSourceRow(std::span<const std::uint8_t>(bytes).first(1u),
        type, 0u, 0u, 0, 0u, 1u, maximum64), Status::IndexBytesUnavailable);
    checkRejected(qualifyEncoderIndexedSourceRow(bytes, type, 1u, maximum64, 0,
        0u, 1u, maximum64), Status::IndexExtentOverflow);
    checkRejected(qualifyEncoderIndexedSourceRow(bytes, type, 0u, maximum64 / 2u, 0,
        0u, 1u, maximum64), Status::IndexExtentOverflow);
    checkRejected(qualifyEncoderIndexedSourceRow(bytes, type, 0u, 0u, 0,
        0u, 0u, maximum64), Status::ZeroStride);
  }
  for (std::uint32_t raw = 0u; raw < 6u; ++raw) {
    for (std::uint32_t primitiveCount = 0u; primitiveCount < 17u; ++primitiveCount) {
      const auto counts = qualifyEncoderDrawCounts(static_cast<Primitive>(raw), primitiveCount);
      const auto rows = qualifyEncoderSourceRows(3u, counts.vertexCount, 5u, 16u, maximum64);
      const auto extent = qualifyEncoderIndexExtent(Index::UInt16, 7u, counts.vertexCount);
      check(counts.qualified() && rows.qualified() && rows.rowCount == counts.vertexCount &&
          rows.byteSize == static_cast<std::uint64_t>(counts.vertexCount) * 16u &&
          extent.qualified() && extent.byteSize == static_cast<std::uint64_t>(counts.vertexCount) * 2u,
          "row and IB plans consume the same production-qualified clamped primitive counts");
    }
  }
  const auto realMaximum = qualifyEncoderDrawCounts(Primitive::TriangleList, 6966u);
  const auto realRows = qualifyEncoderSourceRows(0u, realMaximum.vertexCount, 0u, 144u, maximum64);
  check(realMaximum.vertexCount == 20898u && realRows.byteSize == 3009312u,
      "full real maximum row/count metadata is representable without an artificial 512-row gate");
  check(!qualifyEncoderDrawCounts(Primitive::TriangleList, maximum32).qualified(),
      "overflowing production count remains unqualified before source-row planning");
}

} // namespace

int main() {
  try {
    strideAndNormalizationDomain();
    exhaustiveSmallRowDomain();
    indexedDecodeAndSignedDomain();
    exhaustiveDefinedExpansionDomain();
    overflowBoundsAndCountComposition();
    std::cout << "draw fetch address: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "draw fetch address: " << error.what() << '\n';
    return 1;
  }
}
