#pragma once

#include "dxmt9_draw_fetch_counts.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace dxmt9::encoders {

// These scalar helpers retain the encoder's existing arithmetic and fallback
// rules. The checked helpers below qualify only their defined source-address
// domain; neither layer identifies a successful Encode transformation route.
constexpr bool encoderEffectiveIndexed(bool requestedIndexed, bool hasBoundIndexBuffer,
    bool hasUserIndexData) noexcept {
  return requestedIndexed && (hasBoundIndexBuffer || hasUserIndexData);
}

constexpr std::uint32_t encoderStream0SourceStride(std::uint32_t boundStride,
    bool ffLayoutPresent, std::uint32_t ffLayoutStride,
    std::uint32_t computedDeclStride) noexcept {
  return boundStride ? boundStride : (ffLayoutPresent ? ffLayoutStride : computedDeclStride);
}

constexpr std::uint64_t encoderNonIndexedStartByteDisplacement(bool indexed,
    std::uint32_t startVertex, std::uint32_t sourceStride) noexcept {
  return !indexed && sourceStride != 0u
      ? static_cast<std::uint64_t>(startVertex) * sourceStride : 0u;
}

constexpr std::int32_t encoderDrawVertexBaseIndex(bool indexed, std::uint32_t startVertex,
    std::int32_t baseVertexIndex, std::uint32_t sourceStride) noexcept {
  return !indexed && sourceStride != 0u
      ? 0 : (indexed ? baseVertexIndex : static_cast<std::int32_t>(startVertex));
}

template <typename ByteOffset>
constexpr ByteOffset encoderIndexByteOffset(std::uint32_t startIndex,
    std::uint64_t ordinal, std::size_t elementBytes) noexcept {
  static_assert(std::is_unsigned_v<ByteOffset>);
  return static_cast<ByteOffset>(startIndex) * static_cast<ByteOffset>(elementBytes) +
      static_cast<ByteOffset>(ordinal) * static_cast<ByteOffset>(elementBytes);
}

// The caller supplies a complete element. Unknown types deliberately retain
// the production four-byte fallback; cold qualification rejects them first.
inline std::uint32_t decodeEncoderIndexValue(const std::uint8_t* element,
    core::IndexType type) noexcept {
  if (type == core::IndexType::UInt16) {
    std::uint16_t value = 0u;
    std::memcpy(&value, element, sizeof(value));
    return value;
  }
  std::uint32_t value = 0u;
  std::memcpy(&value, element, sizeof(value));
  return value;
}

// As in the existing expander, the index is converted to i32 before addition.
// The cold qualifier excludes unrepresentable indices and signed overflow;
// it must not reinterpret those cases using widened unsigned semantics.
constexpr std::int32_t encoderIndexedSourceVertex(std::int32_t baseVertexIndex,
    std::uint32_t indexValue) noexcept {
  return baseVertexIndex + static_cast<std::int32_t>(indexValue);
}

template <typename ByteOffset>
constexpr ByteOffset encoderSourceRowByteOffset(ByteOffset sourceBase,
    std::uint64_t vertexRow, ByteOffset sourceStride) noexcept {
  static_assert(std::is_unsigned_v<ByteOffset>);
  return sourceBase + static_cast<ByteOffset>(vertexRow) * sourceStride;
}

template <typename ByteSize>
constexpr ByteSize encoderSourceRowsByteSize(std::uint64_t rowCount,
    ByteSize sourceStride) noexcept {
  static_assert(std::is_unsigned_v<ByteSize>);
  return static_cast<ByteSize>(rowCount) * sourceStride;
}

enum class DrawFetchAddressStatus : std::uint8_t {
  Qualified,
  UnknownIndexType,
  IndexExtentOverflow,
  IndexBytesUnavailable,
  IndexValueOutsideSignedDomain,
  SourceVertexOverflow,
  NegativeSourceVertex,
  EmptyRange,
  ZeroStride,
  SourceOffsetOverflow,
  SourceSizeOverflow,
  SourceEndOverflow,
  SourceRowsOutOfBounds,
};

struct EncoderSourceRowsQualification {
  DrawFetchAddressStatus status = DrawFetchAddressStatus::EmptyRange;
  std::uint64_t firstVertexRow = 0u, rowCount = 0u;
  std::uint64_t byteOffset = 0u, byteSize = 0u, byteEnd = 0u;
  constexpr bool qualified() const noexcept { return status == DrawFetchAddressStatus::Qualified; }
};

// A contiguous full-stride source-row interval, also used for nonindexed draws.
// Full strides can include bytes not demanded by a shader: rejection is not a
// semantic-corruption verdict. This value contract proves no physical bytes,
// cross-buffer coherence, future preservation, or Encode/GPU consumption.
constexpr EncoderSourceRowsQualification qualifyEncoderSourceRows(
    std::uint64_t firstVertexRow, std::uint64_t rowCount,
    std::uint64_t sourceBase, std::uint64_t sourceStride,
    std::uint64_t sourceByteSize) noexcept {
  if (!rowCount) return {.status = DrawFetchAddressStatus::EmptyRange};
  if (!sourceStride) return {.status = DrawFetchAddressStatus::ZeroStride};
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (firstVertexRow > maximum / sourceStride ||
      sourceBase > maximum - firstVertexRow * sourceStride)
    return {.status = DrawFetchAddressStatus::SourceOffsetOverflow};
  if (rowCount > maximum / sourceStride)
    return {.status = DrawFetchAddressStatus::SourceSizeOverflow};
  const auto byteOffset = encoderSourceRowByteOffset(sourceBase, firstVertexRow, sourceStride);
  const auto byteSize = encoderSourceRowsByteSize(rowCount, sourceStride);
  if (byteSize > maximum - byteOffset)
    return {.status = DrawFetchAddressStatus::SourceEndOverflow};
  const auto byteEnd = byteOffset + byteSize;
  if (byteEnd > sourceByteSize)
    return {.status = DrawFetchAddressStatus::SourceRowsOutOfBounds};
  return {DrawFetchAddressStatus::Qualified, firstVertexRow, rowCount,
      byteOffset, byteSize, byteEnd};
}

// Decode only an immutable expected element whose exact extent is qualified.
// Its matching physical IB slice and resulting VB rows still need one coherent
// observation; independently locked per-buffer comparisons do not compose.
inline EncoderSourceRowsQualification qualifyEncoderIndexedSourceRow(
    std::span<const std::uint8_t> completeIndexBytes, core::IndexType indexType,
    std::uint32_t startIndex, std::uint64_t ordinal, std::int32_t baseVertexIndex,
    std::uint64_t sourceBase, std::uint64_t sourceStride,
    std::uint64_t sourceByteSize) noexcept {
  if (indexType != core::IndexType::UInt16 && indexType != core::IndexType::UInt32)
    return {.status = DrawFetchAddressStatus::UnknownIndexType};
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (ordinal > maximum - startIndex)
    return {.status = DrawFetchAddressStatus::IndexExtentOverflow};
  const auto extent = qualifyEncoderIndexExtent(indexType,
      static_cast<std::uint64_t>(startIndex) + ordinal, 1u);
  if (!extent.qualified())
    return {.status = DrawFetchAddressStatus::IndexExtentOverflow};
  if (extent.byteEnd > completeIndexBytes.size())
    return {.status = DrawFetchAddressStatus::IndexBytesUnavailable};
  const auto byteOffset = encoderIndexByteOffset<std::uint64_t>(startIndex, ordinal,
      extent.elementBytes);
  const auto indexValue = decodeEncoderIndexValue(
      completeIndexBytes.data() + static_cast<std::size_t>(byteOffset), indexType);
  constexpr auto maximumSigned = std::numeric_limits<std::int32_t>::max();
  if (indexValue > static_cast<std::uint32_t>(maximumSigned))
    return {.status = DrawFetchAddressStatus::IndexValueOutsideSignedDomain};
  const auto wideVertex = static_cast<std::int64_t>(baseVertexIndex) + indexValue;
  if (wideVertex > maximumSigned)
    return {.status = DrawFetchAddressStatus::SourceVertexOverflow};
  if (wideVertex < 0)
    return {.status = DrawFetchAddressStatus::NegativeSourceVertex};
  const auto vertexRow = encoderIndexedSourceVertex(baseVertexIndex, indexValue);
  return qualifyEncoderSourceRows(static_cast<std::uint64_t>(vertexRow), 1u,
      sourceBase, sourceStride, sourceByteSize);
}

} // namespace dxmt9::encoders
