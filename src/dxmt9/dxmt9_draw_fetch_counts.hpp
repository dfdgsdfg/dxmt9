#pragma once

#include "dxmt9/core_constants.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt9::encoders {
namespace detail {

template <typename Count>
constexpr Count encoderPrimitiveVertexCountFormula(core::PrimitiveType type, Count primitiveCount) noexcept {
  switch (type) {
    case core::PrimitiveType::PointList: return primitiveCount;
    case core::PrimitiveType::LineList: return primitiveCount * 2u;
    case core::PrimitiveType::LineStrip: return primitiveCount + 1u;
    case core::PrimitiveType::TriangleList: return primitiveCount * 3u;
    case core::PrimitiveType::TriangleStrip:
    case core::PrimitiveType::TriangleFan:
      return primitiveCount + 2u;
  }
  return 0u;
}

} // namespace detail

// Production arithmetic deliberately retains u32 wrap and the unknown-enum
// zero/default-four behavior. Existing encoder call-site clamps stay in place.
constexpr std::uint32_t encoderPrimitiveVertexCount(core::PrimitiveType type,
    std::uint32_t primitiveCount) noexcept {
  return detail::encoderPrimitiveVertexCountFormula(type, primitiveCount);
}

constexpr std::size_t encoderIndexElementSize(core::IndexType type) noexcept {
  return type == core::IndexType::UInt16 ? 2u : 4u;
}

enum class DrawFetchCountStatus : std::uint8_t {
  Qualified, UnknownPrimitiveType, VertexCountOverflow, UnknownIndexType,
  IndexOffsetOverflow, IndexSizeOverflow, IndexEndOverflow,
};

struct EncoderDrawCountsQualification {
  DrawFetchCountStatus status = DrawFetchCountStatus::UnknownPrimitiveType;
  std::uint32_t primitiveCount = 0u, vertexCount = 0u;
  constexpr bool qualified() const noexcept { return status == DrawFetchCountStatus::Qualified; }
};

struct EncoderIndexExtentQualification {
  DrawFetchCountStatus status = DrawFetchCountStatus::UnknownIndexType;
  std::size_t elementBytes = 0u;
  std::uint64_t byteOffset = 0u, byteSize = 0u, byteEnd = 0u;
  constexpr bool qualified() const noexcept { return status == DrawFetchCountStatus::Qualified; }
};

// Cold qualification only: reject shapes whose production arithmetic wraps,
// while using the same formula and the encoder's actual two minimum-one clamps.
// Rejection does not alter rendering or qualify any complete fetch footprint.
constexpr EncoderDrawCountsQualification qualifyEncoderDrawCounts(core::PrimitiveType type,
    std::uint32_t primitiveCount) noexcept {
  const auto effectivePrimitiveCount = primitiveCount ? primitiveCount : 1u;
  const auto wideVertexCount = detail::encoderPrimitiveVertexCountFormula(
      type, static_cast<std::uint64_t>(effectivePrimitiveCount));
  // The clamped count is positive, so only the shared unknown-enum arm can
  // return zero in the wide, non-overflowing arithmetic domain.
  if (!wideVertexCount) return {};
  if (wideVertexCount > std::numeric_limits<std::uint32_t>::max())
    return {.status = DrawFetchCountStatus::VertexCountOverflow};
  const auto vertexCount = encoderPrimitiveVertexCount(type, effectivePrimitiveCount);
  return {DrawFetchCountStatus::Qualified, effectivePrimitiveCount, vertexCount ? vertexCount : 1u};
}

// Numeric index-element interval only. Resource size, start/base-vertex,
// streams, declaration, and actual indexed vertex fetches are separate gates.
constexpr EncoderIndexExtentQualification qualifyEncoderIndexExtent(core::IndexType type,
    std::uint64_t firstIndex, std::uint64_t elementCount) noexcept {
  if (type != core::IndexType::UInt16 && type != core::IndexType::UInt32) return {};
  const auto elementBytes = encoderIndexElementSize(type);
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (firstIndex > maximum / elementBytes)
    return {.status = DrawFetchCountStatus::IndexOffsetOverflow};
  if (elementCount > maximum / elementBytes)
    return {.status = DrawFetchCountStatus::IndexSizeOverflow};
  const auto byteOffset = firstIndex * elementBytes;
  const auto byteSize = elementCount * elementBytes;
  if (byteSize > maximum - byteOffset)
    return {.status = DrawFetchCountStatus::IndexEndOverflow};
  return {DrawFetchCountStatus::Qualified, elementBytes, byteOffset, byteSize, byteOffset + byteSize};
}

} // namespace dxmt9::encoders
