#pragma once

#include "render_tape_cpu_resource_context.hpp"
#include "render_tape_cpu_source_rows.hpp"

#include "dxmt9/dxmt9_ffp_shaders.hpp"
#include "dxmt9/dxmt9_resource_pool.hpp"
#include "dxmt9/query_fence.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dxmt9::test {

enum class CpuTapeSourceObservationStatus : std::uint8_t {
  NotAttempted, Qualified, LayoutUnavailable, UnsupportedDomain, BindingMismatch,
  MissingBufferVersion, MissingBufferFacts, HistoricalFactsMismatch,
  SnapshotAssociationMismatch, SourceWindowRejected, PlannerRejected, BatchRejected,
};

constexpr const char* cpuTapeSourceObservationStatusName(
    CpuTapeSourceObservationStatus status) noexcept {
  using Status = CpuTapeSourceObservationStatus;
  switch (status) {
  case Status::NotAttempted: return "not-attempted";
  case Status::Qualified: return "qualified";
  case Status::LayoutUnavailable: return "layout-unavailable";
  case Status::UnsupportedDomain: return "unsupported-domain";
  case Status::BindingMismatch: return "binding-mismatch";
  case Status::MissingBufferVersion: return "missing-buffer-version";
  case Status::MissingBufferFacts: return "missing-buffer-facts";
  case Status::HistoricalFactsMismatch: return "historical-facts-mismatch";
  case Status::SnapshotAssociationMismatch: return "snapshot-association-mismatch";
  case Status::SourceWindowRejected: return "source-window-rejected";
  case Status::PlannerRejected: return "planner-rejected";
  case Status::BatchRejected: return "batch-rejected";
  }
  return "unknown";
}

// Owned, bounded values only. Qualification means one coherent instantaneous
// comparison of the scoped IB interval and full-stride VB source rows. It is
// not full shader-fetch coverage, preservation after unlock, or Encode/GPU
// authority. A conservative mismatch is not a semantic-corruption verdict.
struct CpuTapeSourceObservation {
  CpuTapeSourceObservationStatus status = CpuTapeSourceObservationStatus::NotAttempted;
  std::uint64_t sourceSequence = 0u, completedSequence = 0u;
  bool effectiveIndexed = false;
  std::uint64_t sourceBase = 0u, sourceStride = 0u, sourceByteSize = 0u;
  std::optional<CpuTapeSourceRowsStatus> plannerStatus{};
  std::uint32_t qualifiedPrimitiveCount = 0u, sourceElementCount = 0u;
  std::size_t uniqueVertexRows = 0u, vertexRangeCount = 0u;
  std::optional<resources::BufferReadProofRange> indexRange{};
  std::uint64_t plannedReadBytes = 0u;
  std::optional<resources::BufferReadProofBatchResult> readProof{};

  bool qualified() const noexcept { return status == CpuTapeSourceObservationStatus::Qualified; }
};

namespace cpu_source_observation_detail {

inline bool historicalFactsMatch(const CpuTapeResourceVersion& version,
    const resources::BufferVersionObservation& facts) noexcept {
  return facts.observed() && facts.buffer == version.nativeHandle() &&
      facts.byteSize == version.expectedBufferBytes().size() && facts.contentRevision &&
      facts.currentBacking.metalHandle && facts.currentBacking.byteSize == facts.byteSize;
}

inline bool snapshotAssociationMatches(const resources::BufferVersionObservation& facts,
    const std::optional<core::DrawBufferBindingSnapshot>& snapshot) noexcept {
  if (facts.hasVersionedBacking != snapshot.has_value()) return false;
  if (!snapshot) return true;
  // These are original copied metadata associations, not an old-ring content
  // epoch or equality between the CPU content epoch and the Pool revision.
  return snapshot->valid() && snapshot->byteSize == facts.byteSize &&
      snapshot->metalHandle == facts.currentBacking.metalHandle &&
      snapshot->contentRevision == facts.contentRevision;
}

}  // namespace cpu_source_observation_detail

// The caller supplies actual effective retained draw values, its original
// immutable record context, and one genuine queue progress pull while holding
// the synchronous source borrow. This helper authenticates neither that borrow
// nor the original locator; the controlled collector owns those obligations.
// No wrapper bytes, Pool record pointer, or snapshot address is dereferenced.
inline CpuTapeSourceObservation observeCpuTapeSourceRows(const resources::Pool& pool,
    const core::FlatDrawStateRecord& hot, const core::DrawShaderLayoutContext* layout,
    const core::DrawParam& parameters, const core::DrawBindingSnapshot* bindingSnapshot,
    const CpuTapeResourceContext& context, std::uint64_t sourceSequence,
    const query::Progress& progress, std::uint64_t expectedQueue,
    CpuTapeSourceRowsLimits limits = {}) noexcept {
  using Status = CpuTapeSourceObservationStatus;
  CpuTapeSourceObservation out;
  out.sourceSequence = sourceSequence;
  out.completedSequence = progress.completed;
  out.sourceBase = hot.streamOffsets[0];
  out.effectiveIndexed = encoders::encoderEffectiveIndexed(parameters.indexed,
      bool(hot.indexBuffer), parameters.userIndexRange.size != 0u);
  const auto reject = [&](Status status) {
    out.status = status;
    return out;
  };
  if (!expectedQueue || progress.queue != expectedQueue || progress.failed ||
      !sourceSequence || progress.completed >= sourceSequence)
    return reject(Status::SourceWindowRejected);
  if (!layout) return reject(Status::LayoutUnavailable);
  if (hot.streamMask != 1u || hot.streamFrequencies[0] != 1u ||
      parameters.instanceCount != 1u || parameters.userVertexRange.size ||
      parameters.userIndexRange.size)
    return reject(Status::UnsupportedDomain);
  const auto& decl = layout->vertexDecl;
  if (decl.elements.size() > decl.elements.max_size()) return reject(Status::UnsupportedDomain);
  // A declaration can require another stream independently of the hot mask.
  for (const auto& element : decl.elements)
    if (element.stream != 0u) return reject(Status::UnsupportedDomain);
  if (decl.streams[0].offset != hot.streamOffsets[0] ||
      decl.streams[0].stride != hot.streamStrides[0])
    return reject(Status::BindingMismatch);

  const auto ffLayout = ffp::decodeFixedFunctionVertexLayout(decl);
  const bool fixedFunction = layout->vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex;
  if (fixedFunction && !ffLayout) return reject(Status::UnsupportedDomain);
  out.sourceStride = fixedFunction
      ? encoders::encoderStream0SourceStride(decl.streams[0].stride, true, ffLayout->stride, 0u)
      : encoders::encoderStream0SourceStride(ffLayout ? decl.streams[0].stride : 0u,
          ffLayout.has_value(), ffLayout ? ffLayout->stride : 0u,
          ffLayout ? 0u : ffp::computeVertexDeclStride(decl));
  if (!out.sourceStride) return reject(Status::UnsupportedDomain);

  const auto vb = context.find(D9C_CHUNK_HANDLE_KIND_BUFFER, hot.streamBuffers[0]);
  const auto ib = out.effectiveIndexed
      ? context.find(D9C_CHUNK_HANDLE_KIND_BUFFER, hot.indexBuffer) : nullptr;
  if (!vb || (out.effectiveIndexed && !ib)) return reject(Status::MissingBufferVersion);
  out.sourceByteSize = vb->expectedBufferBytes().size();
  if (!vb->localBufferFacts() || (ib && !ib->localBufferFacts()))
    return reject(Status::MissingBufferFacts);
  if (!cpu_source_observation_detail::historicalFactsMatch(*vb, *vb->localBufferFacts()) ||
      (ib && !cpu_source_observation_detail::historicalFactsMatch(*ib, *ib->localBufferFacts())))
    return reject(Status::HistoricalFactsMismatch);

  std::optional<core::DrawBufferBindingSnapshot> vertexSnapshot, indexSnapshot;
  if (bindingSnapshot && (bindingSnapshot->streamMask & 1u) &&
      bindingSnapshot->streams[0].snapshot.valid()) {
    const auto& stream = bindingSnapshot->streams[0];
    if (stream.buffer != hot.streamBuffers[0] || stream.offset != hot.streamOffsets[0] ||
        stream.stride != hot.streamStrides[0])
      return reject(Status::SnapshotAssociationMismatch);
    vertexSnapshot = stream.snapshot;
  }
  if (out.effectiveIndexed && bindingSnapshot && bindingSnapshot->indexSnapshotValid &&
      bindingSnapshot->indexSnapshot.valid()) {
    if (bindingSnapshot->indexBuffer != hot.indexBuffer ||
        bindingSnapshot->indexType != parameters.indexType)
      return reject(Status::SnapshotAssociationMismatch);
    indexSnapshot = bindingSnapshot->indexSnapshot;
  }
  if (!cpu_source_observation_detail::snapshotAssociationMatches(
          *vb->localBufferFacts(), vertexSnapshot) ||
      (ib && !cpu_source_observation_detail::snapshotAssociationMatches(
          *ib->localBufferFacts(), indexSnapshot)))
    return reject(Status::SnapshotAssociationMismatch);

  CpuTapeSourceRowsPlan plan;
  out.plannerStatus = buildCpuTapeSourceRowsPlan({.parameters = parameters,
      .actualEffectiveIndexed = out.effectiveIndexed, .hasBoundIndexBuffer = bool(hot.indexBuffer),
      .streamMask = hot.streamMask, .streamFrequency = hot.streamFrequencies[0],
      .sourceBase = out.sourceBase, .sourceStride = out.sourceStride,
      .sourceByteSize = out.sourceByteSize,
      .expectedIndexBytes = ib ? ib->expectedBufferBytes() : std::span<const std::uint8_t>{}}, plan, limits);
  if (*out.plannerStatus != CpuTapeSourceRowsStatus::Qualified) return reject(Status::PlannerRejected);
  out.qualifiedPrimitiveCount = plan.qualifiedPrimitiveCount;
  out.sourceElementCount = plan.sourceElementCount;
  out.uniqueVertexRows = plan.uniqueVertexRows;
  out.vertexRangeCount = plan.vertexRanges.size();
  out.indexRange = plan.indexRange;
  out.plannedReadBytes = plan.plannedReadBytes;

  std::array<resources::BufferReadProofRequest, 2u> requests{};
  std::size_t requestCount = 0u;
  const resources::BufferReadProofLimits memberLimits{limits.maxRanges, limits.maxReadBytes};
  if (ib) {
    requests[requestCount++] = {.buffer = ib->nativeHandle(), .sourceSequence = sourceSequence,
        .completedSequence = progress.completed, .snapshot = indexSnapshot,
        .expectedBytes = ib->expectedBufferBytes(),
        .ranges = std::span<const resources::BufferReadProofRange>(&*plan.indexRange, 1u),
        .limits = memberLimits};
  }
  requests[requestCount++] = {.buffer = vb->nativeHandle(), .sourceSequence = sourceSequence,
      .completedSequence = progress.completed, .snapshot = vertexSnapshot,
      .expectedBytes = vb->expectedBufferBytes(), .ranges = plan.vertexRanges, .limits = memberLimits};
  out.readProof = pool.proveBufferReadFootprints(
      std::span<const resources::BufferReadProofRequest>(requests.data(), requestCount),
      {.maxRequests = 2u, .maxRanges = limits.maxRanges, .maxReadBytes = limits.maxReadBytes});
  return reject(out.readProof->proved() ? Status::Qualified : Status::BatchRejected);
}

}  // namespace dxmt9::test
