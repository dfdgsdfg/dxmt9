#include "d3d9_pe_batch.hpp"

#include <limits>

namespace dxmt9::d3d9::pe {

namespace {

bool sameIdentity(const PeWireObjectRef& left,
                 const PeWireObjectRef& right) noexcept {
  return left.identity.kind == right.identity.kind &&
         left.identity.generation == right.identity.generation &&
         left.identity.objectId == right.identity.objectId;
}

bool alignPayload(std::size_t value, std::uint32_t alignment,
                  std::size_t& out) noexcept {
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
      value > std::numeric_limits<std::size_t>::max() - (alignment - 1u)) {
    return false;
  }
  out = (value + alignment - 1u) & ~(alignment - 1u);
  return true;
}

}  // namespace

PeOwnedRecordCandidate PeOwnedRecordCandidate::present(
    const D9CCommandChunkWirePresent& command,
    const SurfaceRef& source) noexcept {
  PeOwnedRecordCandidate candidate;
  candidate.producer_ = PeSemanticProducerKind::Present;
  candidate.hwnd_ = command.hwnd;
  candidate.flags_ = command.flags;
  candidate.hasSrc_ = command.hasSrc;
  candidate.hasDst_ = command.hasDst;
  candidate.src_ = command.src;
  candidate.dst_ = command.dst;
  candidate.handles_[0] = source;
  candidate.handleCount_ = 1u;
  return candidate;
}

PeOwnedRecordCandidate PeOwnedRecordCandidate::readback(
    const SurfaceRef& source, const SurfaceRef& destination) noexcept {
  PeOwnedRecordCandidate candidate;
  candidate.producer_ = PeSemanticProducerKind::Readback;
  candidate.handles_[0] = source;
  candidate.handles_[1] = destination;
  candidate.handleCount_ = 2u;
  return candidate;
}

bool PeOwnedRecordCandidate::identityValid() const noexcept {
  const auto expectedKind = D9C_CHUNK_HANDLE_KIND_SURFACE;
  for (std::size_t index = 0u; index < handleCount_; ++index) {
    if (!handles_[index].valid(expectedKind)) return false;
    for (std::size_t prior = 0u; prior < index; ++prior) {
      if (sameIdentity(handles_[prior], handles_[index]) &&
          handles_[prior].object != handles_[index].object) {
        return false;
      }
    }
  }
  return true;
}

bool PeOwnedRecordCandidate::valid() const noexcept {
  const bool shape =
      (producer_ == PeSemanticProducerKind::Present && handleCount_ == 1u) ||
      (producer_ == PeSemanticProducerKind::Readback && handleCount_ == 2u);
  return shape && identityValid();
}

std::uint32_t PeOwnedRecordCandidate::recordType() const noexcept {
  return producer_ == PeSemanticProducerKind::Present
      ? static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT)
      : producer_ == PeSemanticProducerKind::Readback
          ? static_cast<std::uint32_t>(D9C_COMMAND_RECORD_READBACK)
          : 0u;
}

std::uint32_t PeOwnedRecordCandidate::fixedPayloadBytes() const noexcept {
  return producer_ == PeSemanticProducerKind::Present
      ? sizeof(D9CCommandChunkWirePresent)
      : producer_ == PeSemanticProducerKind::Readback
          ? sizeof(D9CCommandChunkWireReadback)
          : 0u;
}

std::size_t PeOwnedRecordCandidate::uniqueHandleCount() const noexcept {
  if (!valid()) return 0u;
  std::size_t unique = 0u;
  for (std::size_t index = 0u; index < handleCount_; ++index) {
    bool duplicate = false;
    for (std::size_t prior = 0u; prior < index; ++prior) {
      if (sameIdentity(handles_[prior], handles_[index])) {
        duplicate = true;
        break;
      }
    }
    unique += !duplicate;
  }
  return unique;
}

D9CCommandChunkWirePresent
PeOwnedRecordCandidate::presentCommand() const noexcept {
  return D9CCommandChunkWirePresent{
      .hwnd = hwnd_,
      .flags = flags_,
      .hasSrc = hasSrc_,
      .hasDst = hasDst_,
      .sourceHandleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
      .src = src_,
      .dst = dst_,
  };
}

SurfaceRef PeOwnedRecordCandidate::surface(std::size_t index) const noexcept {
  return qualifyLocalRef<SurfaceRef>(handles_[index]);
}

bool PeOwnedRecordCandidate::emitLegacy(
    CommandChunkBuilder& builder) const noexcept {
  if (!valid()) return false;
  if (producer_ == PeSemanticProducerKind::Present) {
    return appendPresent(builder, presentCommand(), surface(0u));
  }
  return appendReadback(builder, surface(0u), surface(1u));
}

bool PeOwnedRecordCandidate::emitExact(
    CommandChunkBuilder& builder) const noexcept {
  if (!valid()) return false;
  if (producer_ == PeSemanticProducerKind::Present) {
    return appendPresent(builder, presentCommand(), surface(0u));
  }
  return appendReadback(builder, surface(0u), surface(1u));
}

ExactCommandChunkLayoutPlan PeSemanticChunkOwner::exactLayout() const noexcept {
  if (!bound_) return {};
  return planExactCommandChunkLayout(
      1u, static_cast<std::uint32_t>(candidate_.uniqueHandleCount()),
      candidate_.fixedPayloadBytes());
}

bool PeSemanticChunkOwner::emitLegacy(
    CommandChunkBuilder& builder) const noexcept {
  return bound_ && !builder.exactFinalLayout() && candidate_.emitLegacy(builder);
}

bool PeSemanticChunkOwner::emitExact(
    CommandChunkBuilder& builder) const noexcept {
  return bound_ && builder.exactFinalLayout() &&
         candidate_.emitExact(builder);
}

bool PePrewireChunkTransaction::append(
    const PeOwnedRecordCandidate& candidate,
    const PeSemanticTransactionCheckpoint& checkpoint) noexcept {
  if (count_ == maxRecords || emitted_ || poisoned_ || !candidate.valid() ||
      !checkpoint.valid()) {
    return false;
  }
  const auto policy = peExactProductionPolicy(candidate.producer());
  if (policy.disposition != PeExactProductionDisposition::ExactSingleton ||
      policy.fallbackReason != PeExactFallbackReason::None) {
    // This is a typed fail-closed residual, not an implicit legacy conversion:
    // callers must keep the normal appendRecord transaction for this family.
    return false;
  }
  if (count_ != 0u &&
      checkpoint.recordOrdinal <= checkpoints_[count_ - 1u].recordOrdinal) {
    return false;
  }
  candidates_[count_] = candidate;
  checkpoints_[count_] = checkpoint;
  ++count_;
  return true;
}

PeSemanticChunkPlan PePrewireChunkTransaction::plan() const noexcept {
  PeSemanticChunkPlan out{};
  if (count_ == 0u || emitted_ || poisoned_) return out;
  std::size_t payloadBytes = 0u;
  std::size_t handles = 0u;
  for (std::size_t index = 0u; index < count_; ++index) {
    const auto& candidate = candidates_[index];
    const auto* rule = recordRule(candidate.recordType());
    std::size_t payloadOffset = 0u;
    if (!rule || !alignPayload(payloadBytes, rule->payloadAlignment,
                               payloadOffset) ||
        candidate.fixedPayloadBytes() >
            std::numeric_limits<std::size_t>::max() - payloadOffset ||
        payloadOffset + candidate.fixedPayloadBytes() >
            D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES ||
        handles > std::numeric_limits<std::size_t>::max() -
                      candidate.uniqueHandleCount()) {
      return {};
    }
    out.payloadOffsets[index] = static_cast<std::uint32_t>(payloadOffset);
    out.handleCounts[index] = static_cast<std::uint32_t>(
        candidate.uniqueHandleCount());
    payloadBytes = payloadOffset + candidate.fixedPayloadBytes();
    handles += candidate.uniqueHandleCount();
  }
  if (payloadBytes > std::numeric_limits<std::uint32_t>::max() ||
      handles > std::numeric_limits<std::uint32_t>::max() ||
      count_ > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  out.layout = planExactCommandChunkLayout(
      static_cast<std::uint32_t>(count_), static_cast<std::uint32_t>(handles),
      static_cast<std::uint32_t>(payloadBytes));
  out.recordCount = count_;
  out.uniqueHandleCount = handles;
  return out.valid() ? out : PeSemanticChunkPlan{};
}

bool PePrewireChunkTransaction::emitExact(
    CommandChunkBuilder& builder) noexcept {
  if (emitted_ || poisoned_ || exactBuilder_ || builder.sealed() ||
      builder.recordCount() != 0u || builder.handleCount() != 0u ||
      builder.payloadBytes() != 0u) {
    return false;
  }
  const auto exactPlan = plan();
  if (!exactPlan.valid() ||
      (!builder.exactFinalLayout() &&
       !builder.prepareExactFinalLayout(exactPlan.layout))) {
    return false;
  }
  exactBuilder_ = &builder;
  for (std::size_t index = 0u; index < count_; ++index) {
    if (!candidates_[index].emitExact(builder)) {
      builder.resetAndReleaseRetained();
      (void)builder.returnToMutableLayout();
      exactBuilder_ = nullptr;
      return false;
    }
  }
  if (builder.recordCount() != exactPlan.layout.recordCount ||
      builder.handleCount() != exactPlan.layout.handleCount ||
      builder.payloadBytes() != exactPlan.layout.payloadBytes) {
    builder.resetAndReleaseRetained();
    (void)builder.returnToMutableLayout();
    exactBuilder_ = nullptr;
    return false;
  }
  emitted_ = true;
  return true;
}

bool PePrewireChunkTransaction::emitLegacy(
    CommandChunkBuilder& builder) const noexcept {
  if (emitted_ || poisoned_ || count_ == 0u || builder.sealed() ||
      builder.exactFinalLayout() || builder.recordCount() != 0u ||
      builder.handleCount() != 0u || builder.payloadBytes() != 0u) {
    return false;
  }
  for (std::size_t index = 0u; index < count_; ++index) {
    if (!candidates_[index].emitLegacy(builder)) {
      // No accepted effect has crossed this owner boundary.  Drop all
      // partial builder work so the same immutable candidates and checkpoints
      // remain retryable as one transaction.
      builder.resetAndReleaseRetained();
      return false;
    }
  }
  return true;
}

SealedCommandChunk PePrewireChunkTransaction::seal() noexcept {
  if (!emitted_ || !exactBuilder_) return {};
  return exactBuilder_->seal();
}

void PePrewireChunkTransaction::reset() noexcept {
  if (exactBuilder_) exactBuilder_->reset();
  count_ = 0u;
  emitted_ = false;
  poisoned_ = false;
  exactBuilder_ = nullptr;
  candidates_.fill({});
  checkpoints_.fill({});
}

void PePrewireChunkTransaction::resetAndReleaseRetained() noexcept {
  if (exactBuilder_) {
    exactBuilder_->resetAndReleaseRetained();
    exactBuilder_ = nullptr;
  }
  reset();
}

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
  const auto candidate = PeOwnedRecordCandidate::present(
      batch.command, batch.source);
  if (!candidate.valid()) {
    return {};
  }
  return PePresentBatchPlan{
      .layout = planExactCommandChunkLayout(
          1u, static_cast<std::uint32_t>(candidate.uniqueHandleCount()),
          candidate.fixedPayloadBytes()),
  };
}

PePresentBatchPlan planPeReadbackBatch(const PeReadbackBatch& batch) noexcept {
  const auto candidate = PeOwnedRecordCandidate::readback(
      batch.source, batch.destination);
  if (!candidate.valid()) {
    return {};
  }
  return PePresentBatchPlan{
      .layout = planExactCommandChunkLayout(
          1u, static_cast<std::uint32_t>(candidate.uniqueHandleCount()),
          candidate.fixedPayloadBytes()),
  };
}

namespace {

bool bindCandidate(PeSemanticChunkOwner& owner,
                   const PeOwnedRecordCandidate& candidate) noexcept {
  owner.clear();
  return owner.bind(candidate);
}

}  // namespace

bool appendPeLegacyPresentSingleton(CommandChunkBuilder& builder,
                                    const PePresentBatch& batch) noexcept {
  PeSemanticChunkOwner owner;
  const auto candidate = PeOwnedRecordCandidate::present(
      batch.command, batch.source);
  return bindCandidate(owner, candidate) && owner.emitLegacy(builder);
}

bool appendPeLegacyReadbackSingleton(CommandChunkBuilder& builder,
                                     const PeReadbackBatch& batch) noexcept {
  PeSemanticChunkOwner owner;
  const auto candidate = PeOwnedRecordCandidate::readback(
      batch.source, batch.destination);
  return bindCandidate(owner, candidate) && owner.emitLegacy(builder);
}

bool appendPeExactPresentSingleton(CommandChunkBuilder& builder,
                                   const PePresentBatch& batch) noexcept {
  PeSemanticChunkOwner owner;
  const auto candidate = PeOwnedRecordCandidate::present(
      batch.command, batch.source);
  if (!bindCandidate(owner, candidate)) return false;
  const auto plan = owner.exactLayout();
  const bool exactPrepared = plan.valid() &&
      builder.prepareExactFinalLayout(plan);
  // Preserve the old helper's fallback contract: an already-populated legacy
  // chunk, or an exact-allocation failure that left the builder empty, stays
  // on the legacy sink.  An exact builder can never accept the legacy sink.
  const bool accepted = exactPrepared
      ? owner.emitExact(builder)
      : owner.emitLegacy(builder);
  if (!accepted && exactPrepared) {
    (void)builder.returnToMutableLayout();
  }
  return accepted;
}

bool appendPeExactReadbackSingleton(CommandChunkBuilder& builder,
                                    const PeReadbackBatch& batch) noexcept {
  PeSemanticChunkOwner owner;
  const auto candidate = PeOwnedRecordCandidate::readback(
      batch.source, batch.destination);
  if (!bindCandidate(owner, candidate)) return false;
  const auto plan = owner.exactLayout();
  const bool exactPrepared = plan.valid() &&
      builder.prepareExactFinalLayout(plan);
  const bool accepted = exactPrepared
      ? owner.emitExact(builder)
      : owner.emitLegacy(builder);
  if (!accepted && exactPrepared) {
    (void)builder.returnToMutableLayout();
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
  PeSemanticChunkOwner owner;
  const auto candidate = PeOwnedRecordCandidate::present(
      batch_.command, batch_.source);
  if (!owner.bind(candidate) || !owner.emitExact(builder_)) {
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
