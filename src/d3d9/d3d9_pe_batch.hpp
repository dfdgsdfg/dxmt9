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

// The metadata is deliberately value-only.  It is the transaction's witness
// for the recorder settlement owners; the transaction never mutates any of
// these owners while planning or emitting.  A retry reuses the same witness,
// including the source/record ordinals, instead of inventing a new ordinal.
struct PeSemanticTransactionCheckpoint {
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;
  std::uint64_t pendingGeneration = 0u;
  std::uint64_t pendingKey = 0u;
  std::uint64_t pendingValue = 0u;
  std::uint64_t captureToken = 0u;
  std::uint64_t captureOrdinal = 0u;
  std::size_t recordCheckpoint = 0u;
  std::size_t handleCheckpoint = 0u;
  std::size_t payloadCheckpoint = 0u;
  std::size_t retainerCheckpoint = 0u;
  bool pendingWitness = false;
  bool captureReserved = false;
  bool retainerCheckpointValid = false;

  constexpr bool valid() const noexcept {
    return sourceOrdinal != 0u && recordOrdinal != 0u &&
           (!pendingWitness || pendingGeneration != 0u) &&
           (!captureReserved ||
            (captureToken != 0u && captureOrdinal != 0u));
  }
};

static_assert(std::is_trivially_copyable_v<PeSemanticTransactionCheckpoint>);

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

// A bounded value owner for the small set of PE records that can currently
// make the direct-final-wire pilot.  The candidate stores semantic fields and
// validated local references, not a wire blob.  Its object pointers are
// call-local witnesses consumed synchronously by emitLegacy()/emitExact();
// the CommandChunkBuilder remains the only owner that retains them.
//
// Keeping this owner separate from the Tape semantic batch is intentional:
// Present and synchronous Readback have no PendingDelta or Tape settlement,
// and their existing appendRecord transaction must remain the sole place
// that owns CapacityPre/Post, capture, bridge, and retry state.
class PeOwnedRecordCandidate final {
 public:
  static constexpr std::size_t maxHandles = 2u;

  PeOwnedRecordCandidate() noexcept = default;

  static PeOwnedRecordCandidate present(
      const D9CCommandChunkWirePresent& command,
      const SurfaceRef& source) noexcept;
  static PeOwnedRecordCandidate readback(
      const SurfaceRef& source, const SurfaceRef& destination) noexcept;

  bool valid() const noexcept;
  PeSemanticProducerKind producer() const noexcept { return producer_; }
  std::uint32_t recordType() const noexcept;
  std::uint32_t fixedPayloadBytes() const noexcept;
  std::size_t handleCount() const noexcept { return handleCount_; }
  std::size_t uniqueHandleCount() const noexcept;

  // Both sinks consume this same candidate.  The sink choice only selects the
  // builder storage mode; neither operation materializes or copies a wire
  // representation after emission.
  bool emitLegacy(CommandChunkBuilder& builder) const noexcept;
  bool emitExact(CommandChunkBuilder& builder) const noexcept;

 private:
  bool identityValid() const noexcept;
  D9CCommandChunkWirePresent presentCommand() const noexcept;
  SurfaceRef surface(std::size_t index) const noexcept;

  PeSemanticProducerKind producer_ = PeSemanticProducerKind::Present;
  std::uint64_t hwnd_ = 0u;
  std::uint32_t flags_ = 0u;
  std::uint32_t hasSrc_ = 0u;
  std::uint32_t hasDst_ = 0u;
  D9CRect src_{};
  D9CRect dst_{};
  std::array<PeWireObjectRef, maxHandles> handles_{};
  std::uint32_t handleCount_ = 0u;
};

static_assert(std::is_trivially_copyable_v<PeOwnedRecordCandidate>);
static_assert(sizeof(PeOwnedRecordCandidate) <= 128u);

// The pilot remains a bounded holder for one candidate, not a second
// settlement state machine and not a place to retain borrows across calls.
// Production singleton adapters still prepare only an empty exact builder;
// multi-record call-local pilot composition belongs to
// PePrewireChunkTransaction below.  It is not the all-family owner; that is
// PeSemanticBatchOwner in d3d9_pe_semantic_owner.hpp.
class PeSemanticChunkOwner final {
 public:
  static constexpr std::size_t maxCandidates = 1u;

  bool bind(const PeOwnedRecordCandidate& candidate) noexcept {
    if (bound_ || !candidate.valid()) return false;
    candidate_ = candidate;
    bound_ = true;
    return true;
  }
  void clear() noexcept { bound_ = false; candidate_ = {}; }

  bool bound() const noexcept { return bound_; }
  const PeOwnedRecordCandidate& candidate() const noexcept { return candidate_; }
  ExactCommandChunkLayoutPlan exactLayout() const noexcept;

  bool emitLegacy(CommandChunkBuilder& builder) const noexcept;
  bool emitExact(CommandChunkBuilder& builder) const noexcept;

 private:
  PeOwnedRecordCandidate candidate_{};
  bool bound_ = false;
};

static_assert(std::is_trivially_copyable_v<PeSemanticChunkOwner>);

struct PeSemanticChunkPlan {
  ExactCommandChunkLayoutPlan layout{};
  std::array<std::uint32_t, 64u> payloadOffsets{};
  std::array<std::uint32_t, 64u> handleCounts{};
  std::size_t recordCount = 0u;
  std::size_t uniqueHandleCount = 0u;

  bool valid() const noexcept {
    return layout.valid() && recordCount == layout.recordCount &&
           uniqueHandleCount == layout.handleCount;
  }
};

// Compatibility transaction for the old Present/Readback call-local pilot.
// It is retained only for its focused tests and singleton adapters.  It must
// not be used as a long-lived all-family owner: its candidate references are
// consumed synchronously by the typed builder sinks, while the new
// PeSemanticBatchOwner acquires typed physical pins at admission.
class PePrewireChunkTransaction final {
 public:
  static constexpr std::size_t maxRecords = 64u;

  PePrewireChunkTransaction() noexcept = default;
  PePrewireChunkTransaction(const PePrewireChunkTransaction&) = delete;
  PePrewireChunkTransaction& operator=(const PePrewireChunkTransaction&) = delete;

  bool append(const PeOwnedRecordCandidate& candidate,
              const PeSemanticTransactionCheckpoint& checkpoint) noexcept;
  PeSemanticChunkPlan plan() const noexcept;

  // Exact emission visits the same candidates used by plan().  The caller
  // supplies an empty builder; this method prepares and seals it exactly once.
  bool emitExact(CommandChunkBuilder& builder) noexcept;
  bool emitLegacy(CommandChunkBuilder& builder) const noexcept;
  SealedCommandChunk seal() noexcept;
  void reset() noexcept;
  void resetAndReleaseRetained() noexcept;

  std::size_t size() const noexcept { return count_; }
  bool emitted() const noexcept { return emitted_; }
  bool poisoned() const noexcept { return poisoned_; }
  const PeSemanticTransactionCheckpoint& checkpoint(
      std::size_t index) const noexcept { return checkpoints_[index]; }

 private:
  std::array<PeOwnedRecordCandidate, maxRecords> candidates_{};
  std::array<PeSemanticTransactionCheckpoint, maxRecords> checkpoints_{};
  std::size_t count_ = 0u;
  bool emitted_ = false;
  bool poisoned_ = false;
  CommandChunkBuilder* exactBuilder_ = nullptr;
};

static_assert(std::is_nothrow_default_constructible_v<PePrewireChunkTransaction>);
static_assert(!std::is_copy_constructible_v<PePrewireChunkTransaction>);

PePresentBatchPlan planPeReadbackBatch(const PeReadbackBatch& batch) noexcept;

bool appendPeExactPresentSingleton(CommandChunkBuilder& builder,
                                   const PePresentBatch& batch) noexcept;
bool appendPeExactReadbackSingleton(CommandChunkBuilder& builder,
                                    const PeReadbackBatch& batch) noexcept;
bool appendPeLegacyPresentSingleton(CommandChunkBuilder& builder,
                                    const PePresentBatch& batch) noexcept;
bool appendPeLegacyReadbackSingleton(CommandChunkBuilder& builder,
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
