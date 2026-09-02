#pragma once

#include "dxmt9_source_payload.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt9::core {

// A populated compatibility ChunkSlot may be extended only by a plan whose
// final representation is already completely reserved.  Keep the result
// typed and value-only so the production queue and native truth-table tests
// cannot grow separate admission policies.
enum class DirectContinuationAdmission : std::uint8_t {
  Admitted,
  CapacityRejected,
  StructuralRejected,
};

struct DirectContinuationAdmissionResult {
  DirectContinuationAdmission disposition =
      DirectContinuationAdmission::StructuralRejected;

  constexpr bool admitted() const noexcept {
    return disposition == DirectContinuationAdmission::Admitted;
  }
  constexpr bool capacityRejected() const noexcept {
    return disposition == DirectContinuationAdmission::CapacityRejected;
  }
  constexpr bool structuralRejected() const noexcept {
    return disposition == DirectContinuationAdmission::StructuralRejected;
  }
};

inline constexpr bool directContinuationAppendFits(
    std::size_t current, std::size_t extra, std::size_t capacity) noexcept {
  return extra <= std::numeric_limits<std::size_t>::max() - current &&
      capacity >= current + extra;
}

// Pure production admission predicate.  `extra` is the immutable final
// capacity plan for the source being considered; it is not a count of raw
// records. APPLY_STATE and constant setters are state-only and therefore do
// not contribute commandHeaders.  The plan producer must nevertheless emit
// the complete one-draw-per-header SoA shape checked below, plus at most one
// header per coordinator locator.
inline DirectContinuationAdmissionResult directContinuationAdmission(
    const ChunkSlot& slot, const SourcePayloadCapacity& extra) noexcept {
  // Readback has a declared final-slot vector but no direct-branch appender
  // (`TransactionalChunkSlotAssembler::tryAppendReadback` hard-fails when the
  // destination is a direct ChunkSlot), so a plan reserving one can never be
  // built and must stay a structural rejection rather than a capacity one.
  if (extra.readbackRecords != 0) {
    return {DirectContinuationAdmission::StructuralRejected};
  }

  // A source-wide emission plan may carry coordinator locators alongside its
  // draw islands. Each contributes exactly one command header plus one row in
  // its own typed vector, so the header total is no longer the draw count.
  // Derive the draw count from `drawParams` -- the one dimension that is
  // one-per-draw and never written by a coordinator -- and require the header
  // total to equal draws plus locators exactly. With no coordinator dimension
  // present this reduces to the historical draw-only predicate with
  // byte-identical behavior.
  const auto drawCount = extra.drawParams;
  const auto coordinatorHeaders =
      extra.clearRecords + extra.surfaceCopyRecords +
      extra.stretchRectRecords + extra.colorFillRecords +
      extra.depthResolveRecords + extra.generateMipmapsRecords +
      extra.presentRecords;
  if (coordinatorHeaders >
          std::numeric_limits<std::size_t>::max() - drawCount ||
      extra.commandHeaders != drawCount + coordinatorHeaders ||
      // A Clear locator's rects live inside its own ClearDesc, so the rect
      // dimension is a transaction-wide bookkeeping total, never a slot
      // vector. It may only be non-zero when a Clear locator is present.
      (extra.clearRects != 0 && extra.clearRecords == 0)) {
    return {DirectContinuationAdmission::StructuralRejected};
  }

  const auto drawShapeMatches =
      drawCount != 0 && extra.drawHotStates == drawCount &&
      extra.drawShaderLayouts == drawCount &&
      extra.drawDebugSnapshots == drawCount &&
      extra.drawPsoSubviews == drawCount &&
      extra.drawUniformFixedPayloads == drawCount &&
      extra.drawUniformVertexConstants == drawCount &&
      extra.drawUniformPixelConstants == drawCount &&
      extra.drawUniformPayloads == drawCount && extra.drawParams == drawCount &&
      extra.drawRunRecords == drawCount;
  if (!drawShapeMatches ||
      drawCount > std::numeric_limits<std::size_t>::max() /
          sizeof(VertexShaderConstants) ||
      drawCount > std::numeric_limits<std::size_t>::max() /
          sizeof(PixelShaderConstants) ||
      extra.drawUniformVertexConstantBytes !=
          drawCount * sizeof(VertexShaderConstants) ||
      extra.drawUniformPixelConstantBytes !=
          drawCount * sizeof(PixelShaderConstants) ||
      extra.drawUniformPayloadLookupHeads !=
          detail::chunkSlotUniformLookupBucketCount(drawCount) ||
      extra.drawUniformPayloadLookupTails !=
          extra.drawUniformPayloadLookupHeads ||
      extra.drawUniformPayloadLookupNext != drawCount ||
      extra.drawUniformVertexConstantsLookupHeads !=
          extra.drawUniformPayloadLookupHeads ||
      extra.drawUniformVertexConstantsLookupTails !=
          extra.drawUniformPayloadLookupTails ||
      extra.drawUniformVertexConstantsLookupNext != drawCount ||
      extra.drawUniformPixelConstantsLookupHeads !=
          extra.drawUniformPayloadLookupHeads ||
      extra.drawUniformPixelConstantsLookupTails !=
          extra.drawUniformPayloadLookupTails ||
      extra.drawUniformPixelConstantsLookupNext != drawCount) {
    return {DirectContinuationAdmission::StructuralRejected};
  }
  if (slot.pipelinePrefetchSealed || !slot.drawStateStorageConsistent() ||
      !slot.commandPayloadsInRange()) {
    return {DirectContinuationAdmission::StructuralRejected};
  }

  const auto fits = [&](std::size_t current, std::size_t add,
                        std::size_t capacity) noexcept {
    return directContinuationAppendFits(current, add, capacity);
  };
  if (!fits(slot.commandHeaders.size(), extra.commandHeaders,
            slot.commandHeaders.capacity()) ||
      !fits(slot.drawHotStates.size(), extra.drawHotStates,
            slot.drawHotStates.capacity()) ||
      !fits(slot.drawShaderLayouts.size(), extra.drawShaderLayouts,
            slot.drawShaderLayouts.capacity()) ||
      !fits(slot.drawDebugSnapshots.size(), extra.drawDebugSnapshots,
            slot.drawDebugSnapshots.capacity()) ||
      !fits(slot.drawPsoSubviews.size(), extra.drawPsoSubviews,
            slot.drawPsoSubviews.capacity()) ||
      !fits(slot.drawUniformFixedPayloads.size(),
            extra.drawUniformFixedPayloads,
            slot.drawUniformFixedPayloads.capacity()) ||
      !fits(slot.drawUniformVertexConstants.size(),
            extra.drawUniformVertexConstants,
            slot.drawUniformVertexConstants.capacity()) ||
      !fits(slot.drawUniformVertexConstantBytes.size(),
            extra.drawUniformVertexConstantBytes,
            slot.drawUniformVertexConstantBytes.capacity()) ||
      !fits(slot.drawUniformPixelConstants.size(),
            extra.drawUniformPixelConstants,
            slot.drawUniformPixelConstants.capacity()) ||
      !fits(slot.drawUniformPixelConstantBytes.size(),
            extra.drawUniformPixelConstantBytes,
            slot.drawUniformPixelConstantBytes.capacity()) ||
      !fits(slot.drawUniformPayloads.size(), extra.drawUniformPayloads,
            slot.drawUniformPayloads.capacity()) ||
      !fits(slot.drawParams.size(), extra.drawParams, slot.drawParams.capacity()) ||
      !fits(slot.drawPayloadArena.size(), extra.drawPayloadBytes,
            slot.drawPayloadArena.capacity()) ||
      !fits(slot.drawRunRecords.size(), extra.drawRunRecords,
            slot.drawRunRecords.capacity()) ||
      !fits(slot.clearRecords.size(), extra.clearRecords,
            slot.clearRecords.capacity()) ||
      !fits(slot.surfaceCopyRecords.size(), extra.surfaceCopyRecords,
            slot.surfaceCopyRecords.capacity()) ||
      !fits(slot.stretchRectRecords.size(), extra.stretchRectRecords,
            slot.stretchRectRecords.capacity()) ||
      !fits(slot.colorFillRecords.size(), extra.colorFillRecords,
            slot.colorFillRecords.capacity()) ||
      !fits(slot.depthResolveRecords.size(), extra.depthResolveRecords,
            slot.depthResolveRecords.capacity()) ||
      !fits(slot.generateMipmapsRecords.size(), extra.generateMipmapsRecords,
            slot.generateMipmapsRecords.capacity()) ||
      !fits(slot.presentRecords.size(), extra.presentRecords,
            slot.presentRecords.capacity())) {
    return {DirectContinuationAdmission::CapacityRejected};
  }

  // The assembler's lookup reserve may resize `next` or rebuild bucket
  // heads. Both operations are forbidden after this pre-effect admission.
  const auto lookupReady = [&](const auto& records, const auto& heads,
                               const auto& tails, const auto& next,
                               std::size_t additional) noexcept {
    if (additional == 0) {
      return heads.size() == tails.size() &&
          next.size() == records.size() &&
          (records.empty() ||
           heads.size() >= detail::chunkSlotUniformLookupBucketCount(
               records.size()));
    }
    if (additional > std::numeric_limits<std::size_t>::max() - records.size()) {
      return false;
    }
    const auto bucketCount = detail::chunkSlotUniformLookupBucketCount(
        records.size() + additional);
    return heads.size() == tails.size() && heads.size() >= bucketCount &&
        next.size() == records.size() &&
        next.capacity() >= records.size() + additional;
  };
  if (!lookupReady(slot.drawUniformPayloads,
                   slot.drawUniformPayloadLookupHeads,
                   slot.drawUniformPayloadLookupTails,
                   slot.drawUniformPayloadLookupNext,
                   extra.drawUniformPayloads) ||
      !lookupReady(slot.drawUniformVertexConstants,
                   slot.drawUniformVertexConstantsLookupHeads,
                   slot.drawUniformVertexConstantsLookupTails,
                   slot.drawUniformVertexConstantsLookupNext,
                   extra.drawUniformVertexConstants) ||
      !lookupReady(slot.drawUniformPixelConstants,
                   slot.drawUniformPixelConstantsLookupHeads,
                   slot.drawUniformPixelConstantsLookupTails,
                   slot.drawUniformPixelConstantsLookupNext,
                   extra.drawUniformPixelConstants)) {
    return {DirectContinuationAdmission::CapacityRejected};
  }
  return {DirectContinuationAdmission::Admitted};
}

}  // namespace dxmt9::core
