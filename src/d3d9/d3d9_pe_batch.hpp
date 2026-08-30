#pragma once

#include "d3d9_pe_chunk_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dxmt9::d3d9::pe {

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
