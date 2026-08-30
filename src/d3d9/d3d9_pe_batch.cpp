#include "d3d9_pe_batch.hpp"

namespace dxmt9::d3d9::pe {

PePresentBatchPlan planPePresentBatch(const PePresentBatch& batch) noexcept {
  if (!batch.valid()) {
    return {};
  }
  return PePresentBatchPlan{
      .layout = planExactCommandChunkLayout(
          1u, 1u, sizeof(D9CCommandChunkWirePresent)),
  };
}

PePresentBatchTransaction::PePresentBatchTransaction(
    PePresentBatch batch)
    : batch_(batch), plan_(planPePresentBatch(batch)), builder_(plan_.layout) {}

bool PePresentBatchTransaction::emit() noexcept {
  if (!plan_.valid() || emitted_ || builder_.sealed()) {
    return false;
  }
  if (!appendPresent(builder_, batch_.command, batch_.source)) {
    return false;
  }
  emitted_ = true;
  return true;
}

SealedCommandChunk PePresentBatchTransaction::seal() noexcept {
  if (!emitted_) {
    return {};
  }
  return builder_.seal();
}

void PePresentBatchTransaction::reset() noexcept {
  builder_.reset();
  emitted_ = false;
}

void PePresentBatchTransaction::resetAndReleaseRetained() noexcept {
  builder_.resetAndReleaseRetained();
  emitted_ = false;
}

}  // namespace dxmt9::d3d9::pe
