#pragma once

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_semantic_tokens.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dxmt9::d3d9::pe {

enum class PeExactProductionDisposition : std::uint8_t {
  LegacyFallback,
  ExactSingleton,
};

enum class PeExactFallbackReason : std::uint8_t {
  None,
  DeferredStateSettlement,
  DeferredResourceSideEffect,
  DeferredCaptureSettlement,
  BorrowedVariableInput,
  CrossCallOrderingBoundary,
};

struct PeExactProductionPolicyRow {
  PeSemanticProducerKind producer;
  PeExactProductionDisposition disposition;
  PeExactFallbackReason fallbackReason;
};

constexpr PeExactProductionPolicyRow peExactProductionPolicy(
    PeSemanticProducerKind producer) noexcept {
  switch (producer) {
    case PeSemanticProducerKind::Present:
    case PeSemanticProducerKind::Readback:
      return {producer, PeExactProductionDisposition::ExactSingleton,
              PeExactFallbackReason::None};
    case PeSemanticProducerKind::DrawPrimitive:
    case PeSemanticProducerKind::DrawIndexedPrimitive:
    case PeSemanticProducerKind::ApplyState:
      return {producer, PeExactProductionDisposition::LegacyFallback,
              PeExactFallbackReason::DeferredStateSettlement};
    case PeSemanticProducerKind::DrawPrimitiveUp:
    case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
    case PeSemanticProducerKind::Clear:
      return {producer, PeExactProductionDisposition::LegacyFallback,
              PeExactFallbackReason::BorrowedVariableInput};
    case PeSemanticProducerKind::VsFloatConstant:
    case PeSemanticProducerKind::VsIntConstant:
    case PeSemanticProducerKind::VsBoolConstant:
    case PeSemanticProducerKind::PsFloatConstant:
    case PeSemanticProducerKind::PsIntConstant:
    case PeSemanticProducerKind::PsBoolConstant:
      return {producer, PeExactProductionDisposition::LegacyFallback,
              PeExactFallbackReason::DeferredStateSettlement};
    case PeSemanticProducerKind::UpdateTexture:
      return {producer, PeExactProductionDisposition::LegacyFallback,
              PeExactFallbackReason::DeferredCaptureSettlement};
    case PeSemanticProducerKind::GenerateMipmaps:
    case PeSemanticProducerKind::StretchRect:
    case PeSemanticProducerKind::ColorFill:
    case PeSemanticProducerKind::UpdateSurface:
      return {producer, PeExactProductionDisposition::LegacyFallback,
              PeExactFallbackReason::DeferredResourceSideEffect};
    case PeSemanticProducerKind::QueryIssue:
    case PeSemanticProducerKind::ReszDepthResolve:
      return {producer, PeExactProductionDisposition::LegacyFallback,
              PeExactFallbackReason::CrossCallOrderingBoundary};
    case PeSemanticProducerKind::Count:
      break;
  }
  return {producer, PeExactProductionDisposition::LegacyFallback,
          PeExactFallbackReason::CrossCallOrderingBoundary};
}

consteval auto makePeExactProductionPolicyTable() noexcept {
  std::array<PeExactProductionPolicyRow,
             static_cast<std::size_t>(PeSemanticProducerKind::Count)>
      rows{};
  for (std::size_t index = 0u; index < rows.size(); ++index) {
    rows[index] = peExactProductionPolicy(
        static_cast<PeSemanticProducerKind>(index));
  }
  return rows;
}

inline constexpr auto kPeExactProductionPolicyTable =
    makePeExactProductionPolicyTable();

consteval bool peExactProductionPolicyComplete() noexcept {
  for (std::size_t index = 0u;
       index < kPeExactProductionPolicyTable.size(); ++index) {
    const auto& row = kPeExactProductionPolicyTable[index];
    if (row.producer != static_cast<PeSemanticProducerKind>(index) ||
        (row.disposition == PeExactProductionDisposition::ExactSingleton) !=
            (row.fallbackReason == PeExactFallbackReason::None)) {
      return false;
    }
  }
  return true;
}

static_assert(peExactProductionPolicyComplete());

bool preparePeExactSingleton(CommandChunkBuilder& builder,
                             PeSemanticProducerKind producer,
                             std::uint32_t handleCount,
                             std::uint32_t payloadBytes) noexcept;

// The first bounded batch value owner is the Present record. It owns values,
// not a callback/span borrowed from a COM entry, so the same object is
// traversed by planning and emission. It does not own PendingDelta, capture
// state, or recorder publication.
struct PePresentBatch {
  D9CCommandChunkWirePresent command{};
  SurfaceRef source{};

  bool valid() const noexcept {
    return source.valid();
  }
};

static_assert(std::is_trivially_copyable_v<PePresentBatch>);

struct PePresentBatchPlan {
  ExactCommandChunkLayoutPlan layout{};

  bool valid() const noexcept { return layout.valid(); }
  std::size_t recordCount() const noexcept { return layout.recordCount; }
  std::size_t uniqueHandleCount() const noexcept {
    return layout.handleCount;
  }
  std::size_t payloadBytes() const noexcept { return layout.payloadBytes; }
};

// Pass 1. This is pure with respect to recorder, wrapper, pending, and
// capture state: it only validates the owned batch and computes exact final
// region sizes. In particular, it never retains `source`.
PePresentBatchPlan planPePresentBatch(const PePresentBatch& batch) noexcept;

struct PeReadbackBatch {
  SurfaceRef source{};
  SurfaceRef destination{};

  bool valid() const noexcept {
    return source.valid() && destination.valid();
  }
};

static_assert(std::is_trivially_copyable_v<PeReadbackBatch>);

PePresentBatchPlan planPeReadbackBatch(const PeReadbackBatch& batch) noexcept;

bool appendPeExactPresentSingleton(CommandChunkBuilder& builder,
                                   const PePresentBatch& batch) noexcept;
bool appendPeExactReadbackSingleton(CommandChunkBuilder& builder,
                                    const PeReadbackBatch& batch) noexcept;

// Owns one complete immutable batch across the plan/emission passes. The
// transaction is intentionally not copyable or movable: its sealed span is
// backed by its builder and cannot outlive this owner. Reset permits a
// retry/rebuild with the same owned values and fixed final layout.
class PePresentBatchTransaction final {
 public:
  explicit PePresentBatchTransaction(PePresentBatch batch);

  PePresentBatchTransaction(const PePresentBatchTransaction&) = delete;
  PePresentBatchTransaction& operator=(const PePresentBatchTransaction&) =
      delete;
  PePresentBatchTransaction(PePresentBatchTransaction&&) = delete;
  PePresentBatchTransaction& operator=(PePresentBatchTransaction&&) = delete;

  const PePresentBatch& batch() const noexcept { return batch_; }
  const PePresentBatchPlan& plan() const noexcept { return plan_; }

  // Pass 2. The callback-derived source is not retained: emission visits the
  // transaction-owned `batch_` synchronously and the builder owns any
  // physical pin acquired by appendPresent. A failed emission rolls back the
  // builder record and remains retryable.
  bool emit() noexcept;
  SealedCommandChunk seal() noexcept;
  void reset() noexcept;
  void resetAndReleaseRetained() noexcept;

  bool emitted() const noexcept { return emitted_; }

 private:
  PePresentBatch batch_{};
  PePresentBatchPlan plan_{};
  CommandChunkBuilder builder_;
  bool emitted_ = false;
};

}  // namespace dxmt9::d3d9::pe
