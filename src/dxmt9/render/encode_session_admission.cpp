#include "encode_session_admission.hpp"

#include <limits>

namespace dxmt9::render {

namespace {

template <typename T>
bool addWithin(T lhs, T rhs, T limit) noexcept {
  return rhs <= limit && lhs <= limit - rhs;
}

bool candidateFits(const EncodeSessionAdmissionState& session,
                   const SessionAdmissionCandidate& candidate,
                   const EncodeSessionLimits& limits) noexcept {
  return addWithin(session.sources, std::uint32_t{1}, limits.maxSources) &&
         addWithin(session.pages, candidate.semantic.pageCount,
                   limits.maxPages) &&
         addWithin(session.bytes, candidate.semantic.byteCount,
                   limits.maxBytes) &&
         addWithin(session.draws, candidate.semantic.drawCount,
                   limits.maxDraws) &&
         addWithin(session.commandBuffers, candidate.predictedCommandBuffers,
                   limits.maxCommandBuffers);
}

bool candidateIdentityValid(
    const SessionAdmissionCandidate& candidate) noexcept {
  return candidate.key.valid() && candidate.semantic.valid() &&
         candidate.sourceOrdinal != 0 && candidate.seqId != 0 &&
         candidate.predictedCommandBuffers != 0;
}

bool candidateMustBeIsolated(
    const SessionAdmissionCandidate& candidate) noexcept {
  return candidate.semantic.requiresIsolation() ||
         candidate.key.captureMode == EncodeCaptureMode::Isolated ||
         candidate.semantic.entryKind == core::SourceEntryEncoderKind::Empty;
}

}  // namespace

SessionAdmissionDecision classifySessionAdmission(
    const EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept {
  if (!limits.valid() || !candidateIdentityValid(candidate)) {
    return SessionAdmissionDecision::RejectInvalid;
  }

  if (!session.valid()) {
    if (candidateMustBeIsolated(candidate) ||
        candidate.semantic.hasPresent() ||
        !candidateFits({}, candidate, limits)) {
      return SessionAdmissionDecision::ProcessCandidateIsolated;
    }
    return SessionAdmissionDecision::Admit;
  }

  if (!session.key.valid() || session.sources == 0 ||
      session.lastSourceOrdinal == 0 || session.lastSeqId == 0) {
    return SessionAdmissionDecision::RejectInvalid;
  }
  if (candidate.sourceOrdinal <= session.lastSourceOrdinal ||
      candidate.seqId <= session.lastSeqId ||
      (candidate.rawOrdinal != 0 && session.lastRawOrdinal != 0 &&
       candidate.rawOrdinal <= session.lastRawOrdinal)) {
    return SessionAdmissionDecision::RejectInvalid;
  }
  if (candidateMustBeIsolated(candidate) || session.hasPresent() ||
      (candidate.semantic.hasPresent() &&
       !candidate.semantic.hasFinalPresentTail()) ||
      candidate.key != session.key ||
      candidate.sourceOrdinal != session.lastSourceOrdinal + 1u ||
      candidate.seqId != session.lastSeqId + 1u ||
      !candidateFits(session, candidate, limits)) {
    return SessionAdmissionDecision::SubmitPrefixBeforeCandidate;
  }
  return SessionAdmissionDecision::Admit;
}

bool appendSessionAdmission(
    EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept {
  if (classifySessionAdmission(session, candidate, limits) !=
      SessionAdmissionDecision::Admit) {
    return false;
  }
  if (!session.valid()) {
    session = EncodeSessionAdmissionState{
        .key = candidate.key,
        .lastRawOrdinal = candidate.rawOrdinal,
        .lastSourceOrdinal = candidate.sourceOrdinal,
        .lastSeqId = candidate.seqId,
        .bytes = candidate.semantic.byteCount,
        .sources = 1,
        .pages = candidate.semantic.pageCount,
        .draws = candidate.semantic.drawCount,
        .commandBuffers = candidate.predictedCommandBuffers,
        .flags = EncodeSessionAdmissionValid |
                 (candidate.semantic.hasPresent()
                      ? EncodeSessionAdmissionHasPresent
                      : 0u),
    };
    return true;
  }
  session.lastRawOrdinal = candidate.rawOrdinal;
  session.lastSourceOrdinal = candidate.sourceOrdinal;
  session.lastSeqId = candidate.seqId;
  session.bytes += candidate.semantic.byteCount;
  ++session.sources;
  session.pages += candidate.semantic.pageCount;
  session.draws += candidate.semantic.drawCount;
  session.commandBuffers += candidate.predictedCommandBuffers;
  if (candidate.semantic.hasPresent()) {
    session.flags |= EncodeSessionAdmissionHasPresent;
  }
  return true;
}

RenderContinuationDecision classifyRenderContinuation(
    const ActiveRenderContinuationState& active,
    const core::SourceSemanticSummary& incoming) noexcept {
  if (!active.active()) {
    return RenderContinuationDecision::NoActivePass;
  }
  if (!incoming.valid()) {
    return RenderContinuationDecision::ExactReplayRequired;
  }
  if (incoming.entryKind != core::SourceEntryEncoderKind::Render ||
      incoming.firstBoundaryOrdinal == 0 ||
      (incoming.entryRender.flags &
       core::RenderContinuationPreRenderBarrier) != 0) {
    return RenderContinuationDecision::ClosePass;
  }
  if ((active.flags &
       (ActiveRenderContinuationPendingNonFoldableClear |
        ActiveRenderContinuationInitializerWait |
        ActiveRenderContinuationSidecarObservation)) != 0) {
    return RenderContinuationDecision::ClosePass;
  }
  if (!active.key.valid() || !active.key.entryStateComplete() ||
      !incoming.entryRender.valid() ||
      !incoming.entryRender.entryStateComplete() ||
      !incoming.entryStable() || incoming.requiresIsolation()) {
    return RenderContinuationDecision::ExactReplayRequired;
  }
  if (active.key.attachments != incoming.entryRender.attachments ||
      active.key.passActionEpoch != incoming.entryRender.passActionEpoch) {
    return RenderContinuationDecision::ClosePass;
  }
  if (active.activeWrites.overlaps(incoming.entryRender.entryReads)) {
    return RenderContinuationDecision::ClosePass;
  }

  const core::RenderRoute activeRoute = active.key.route;
  const core::RenderRoute incomingRoute = incoming.entryRender.route;
  if (activeRoute == core::RenderRoute::Unknown ||
      incomingRoute == core::RenderRoute::Unknown) {
    return RenderContinuationDecision::ExactReplayRequired;
  }
  // Portable can execute either form. Tile is sticky and must split before a
  // draw that requires the portable route.
  if (activeRoute == core::RenderRoute::Tile &&
      incomingRoute == core::RenderRoute::Portable) {
    return RenderContinuationDecision::ClosePass;
  }
  if (!active.activeWrites.complete() ||
      !incoming.entryRender.entryReads.complete() ||
      !active.activeWrites.canonicalized() ||
      !incoming.entryRender.entryReads.canonicalized()) {
    return RenderContinuationDecision::ExactReplayRequired;
  }
  return RenderContinuationDecision::ContinueProven;
}

}  // namespace dxmt9::render
