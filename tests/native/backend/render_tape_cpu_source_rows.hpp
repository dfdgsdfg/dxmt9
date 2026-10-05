#pragma once

#include "dxmt9/dxmt9_buffer_read_proof.hpp"
#include "dxmt9/dxmt9_draw_fetch_address.hpp"
#include "dxmt9/dxmt9_draw_fetch_counts.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::test {

inline constexpr std::size_t kCpuTapeSourceRowsMaxRows = 65536u;
inline constexpr std::size_t kCpuTapeSourceRowsMaxRanges = 65536u;

struct CpuTapeSourceRowsLimits {
  // Work counts original elements, including repeated indexed rows. Range and
  // byte limits count the exact IB slice plus unique full-stride VB rows.
  std::size_t maxRows = 32768u;
  std::size_t maxRanges = 32769u;
  std::uint64_t maxReadBytes = 256u * 1024u * 1024u;
};

struct CpuTapeSourceRowsInput {
  core::DrawParam parameters{};
  bool actualEffectiveIndexed = false;
  bool hasBoundIndexBuffer = false;
  std::uint32_t streamMask = 1u, streamFrequency = 1u;
  // The caller supplies the effective source stride/base/complete extent,
  // not a stride guessed from a declaration or the draw's logical limits.
  std::uint64_t sourceBase = 0u, sourceStride = 0u, sourceByteSize = 0u;
  std::span<const std::uint8_t> expectedIndexBytes{};
};

// Closed, owned numeric projection only. Full source rows overapproximate the
// admitted source-row contract, NOT arbitrary shader attribute demand, actual
// Encode/staging routes or physical reads. No backing pin or span survives.
struct CpuTapeSourceRowsPlan {
  bool effectiveIndexed = false;
  std::uint32_t qualifiedPrimitiveCount = 0u, sourceElementCount = 0u;
  std::size_t uniqueVertexRows = 0u;
  std::uint64_t plannedReadBytes = 0u;
  std::optional<resources::BufferReadProofRange> indexRange{};
  std::vector<resources::BufferReadProofRange> vertexRanges{};
};

enum class CpuTapeSourceRowsStatus : std::uint8_t {
  Qualified, InvalidLimits, UnsupportedDomain, EffectiveIndexedMismatch,
  CountQualificationRejected, RowWorkLimitExceeded, IndexExtentRejected,
  IndexBoundsExceeded, SourceAddressRejected, RangeLimitExceeded,
  ReadByteLimitExceeded, AllocationFailed,
};

// Cold bounded value transform. On EVERY rejection, including allocation
// failure, out remains unchanged. The expected IB is immutable logical data;
// neither decoding it nor returning ranges proves concrete backing bytes.
inline CpuTapeSourceRowsStatus buildCpuTapeSourceRowsPlan(
    const CpuTapeSourceRowsInput& input, CpuTapeSourceRowsPlan& out,
    CpuTapeSourceRowsLimits limits = {}) noexcept {
  using Status = CpuTapeSourceRowsStatus;
  if (!limits.maxRows || limits.maxRows > kCpuTapeSourceRowsMaxRows ||
      !limits.maxRanges || limits.maxRanges > kCpuTapeSourceRowsMaxRanges ||
      !limits.maxReadBytes) return Status::InvalidLimits;
  const auto& draw = input.parameters;
  // DrawPayloadRange size==0 means absent, even with a nonzero offset. Any
  // nonempty UP range is outside this domain, including malformed ranges.
  if (input.streamMask != 1u || input.streamFrequency != 1u || draw.instanceCount != 1u ||
      draw.userVertexRange.size || draw.userIndexRange.size || !input.sourceStride ||
      !input.sourceByteSize) return Status::UnsupportedDomain;
  const bool indexed = encoders::encoderEffectiveIndexed(
      draw.indexed, input.hasBoundIndexBuffer, draw.userIndexRange.size != 0u);
  if (indexed != input.actualEffectiveIndexed) return Status::EffectiveIndexedMismatch;
  const auto counts = encoders::qualifyEncoderDrawCounts(draw.primitiveType, draw.primitiveCount);
  if (!counts.qualified()) return Status::CountQualificationRejected;
  if (counts.vertexCount > limits.maxRows) return Status::RowWorkLimitExceeded;

  try {
    CpuTapeSourceRowsPlan candidate;
    candidate.effectiveIndexed = indexed;
    candidate.qualifiedPrimitiveCount = counts.primitiveCount;
    candidate.sourceElementCount = counts.vertexCount;
    if (!indexed) {
      const auto rows = encoders::qualifyEncoderSourceRows(draw.startVertex, counts.vertexCount,
          input.sourceBase, input.sourceStride, input.sourceByteSize);
      if (!rows.qualified()) return Status::SourceAddressRejected;
      if (rows.byteSize > limits.maxReadBytes) return Status::ReadByteLimitExceeded;
      candidate.uniqueVertexRows = counts.vertexCount;
      candidate.plannedReadBytes = rows.byteSize;
      candidate.vertexRanges.push_back({rows.byteOffset, rows.byteSize});
    } else {
      const auto indices = encoders::qualifyEncoderIndexExtent(
          draw.indexType, draw.startIndex, counts.vertexCount);
      if (!indices.qualified()) return Status::IndexExtentRejected;
      if (indices.byteEnd > input.expectedIndexBytes.size()) return Status::IndexBoundsExceeded;
      if (indices.byteSize > limits.maxReadBytes) return Status::ReadByteLimitExceeded;
      if (limits.maxRanges < 2u) return Status::RangeLimitExceeded;
      candidate.indexRange = resources::BufferReadProofRange{indices.byteOffset, indices.byteSize};
      candidate.plannedReadBytes = indices.byteSize;
      std::vector<std::uint64_t> rowOffsets;
      rowOffsets.reserve(counts.vertexCount);
      for (std::uint64_t ordinal = 0u; ordinal < counts.vertexCount; ++ordinal) {
        const auto row = encoders::qualifyEncoderIndexedSourceRow(input.expectedIndexBytes,
            draw.indexType, draw.startIndex, ordinal, draw.baseVertexIndex,
            input.sourceBase, input.sourceStride, input.sourceByteSize);
        if (!row.qualified()) return Status::SourceAddressRejected;
        rowOffsets.push_back(row.byteOffset);
      }
      std::sort(rowOffsets.begin(), rowOffsets.end());
      rowOffsets.erase(std::unique(rowOffsets.begin(), rowOffsets.end()), rowOffsets.end());
      candidate.uniqueVertexRows = rowOffsets.size();
      candidate.vertexRanges.reserve(std::min(rowOffsets.size(), limits.maxRanges - 1u));
      for (const auto offset : rowOffsets) {
        if (input.sourceStride > limits.maxReadBytes - candidate.plannedReadBytes)
          return Status::ReadByteLimitExceeded;
        // Each qualified row has checked its own byte end. Sorted unique
        // offsets only merge exact adjacency; holes never become fetched rows.
        if (!candidate.vertexRanges.empty() &&
            offset - candidate.vertexRanges.back().byteOffset == candidate.vertexRanges.back().byteSize) {
          candidate.vertexRanges.back().byteSize += input.sourceStride;
        } else {
          if (candidate.vertexRanges.size() >= limits.maxRanges - 1u) return Status::RangeLimitExceeded;
          candidate.vertexRanges.push_back({offset, input.sourceStride});
        }
        candidate.plannedReadBytes += input.sourceStride;
      }
    }
    out = std::move(candidate);
    return Status::Qualified;
  } catch (...) { return Status::AllocationFailed; }
}

} // namespace dxmt9::test
