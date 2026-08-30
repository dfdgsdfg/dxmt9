#include "d3d9_pe_batch.hpp"

namespace dxmt9::d3d9::pe {

bool preparePeExactSingleton(CommandChunkBuilder& builder,
                             PeSemanticProducerKind producer,
                             std::uint32_t handleCount,
                             std::uint32_t payloadBytes) noexcept {
  const auto policy = peExactProductionPolicy(producer);
  if (policy.disposition != PeExactProductionDisposition::ExactSingleton ||
      policy.fallbackReason != PeExactFallbackReason::None) {
    return false;
  }
  return builder.prepareExactFinalLayout(
      planExactCommandChunkLayout(1u, handleCount, payloadBytes));
}

PePresentBatchPlan planPePresentBatch(const PePresentBatch& batch) noexcept {
  if (!batch.valid()) {
    return {};
  }
  return PePresentBatchPlan{
      .layout = planExactCommandChunkLayout(
          1u, 1u, sizeof(D9CCommandChunkWirePresent)),
  };
}

PePresentBatchPlan planPeReadbackBatch(const PeReadbackBatch& batch) noexcept {
  if (!batch.valid()) {
    return {};
  }
  return PePresentBatchPlan{
      .layout = planExactCommandChunkLayout(
          1u, 2u, sizeof(D9CCommandChunkWireReadback)),
  };
}

bool appendPeExactPresentSingleton(CommandChunkBuilder& builder,
                                   const PePresentBatch& batch) noexcept {
  const bool exactPrepared = preparePeExactSingleton(
      builder, PeSemanticProducerKind::Present, 1u,
      sizeof(D9CCommandChunkWirePresent));
  const bool accepted = appendPresent(builder, batch.command, batch.source);
  if (!accepted && exactPrepared) {
    (void)builder.returnToLegacyFinalLayout();
  }
  return accepted;
}

bool appendPeExactReadbackSingleton(CommandChunkBuilder& builder,
                                    const PeReadbackBatch& batch) noexcept {
  const bool exactPrepared = preparePeExactSingleton(
      builder, PeSemanticProducerKind::Readback, 2u,
      sizeof(D9CCommandChunkWireReadback));
  const bool accepted = appendReadback(
      builder, batch.source, batch.destination);
  if (!accepted && exactPrepared) {
    (void)builder.returnToLegacyFinalLayout();
  }
  return accepted;
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
