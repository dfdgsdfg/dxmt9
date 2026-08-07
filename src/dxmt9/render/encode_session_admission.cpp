#include "encode_session_admission.hpp"

#include <limits>

namespace dxmt9::render {

namespace {

template <typename T>
bool addWithin(T lhs, T rhs, T limit) noexcept {
  return rhs <= limit && lhs <= limit - rhs;
}

std::uint32_t candidatePageFootprint(
    const SessionAdmissionCandidate& candidate) noexcept {
  return candidate.reservationPages != 0 ? candidate.reservationPages
                                         : candidate.semantic.pageCount;
}

bool candidateFits(const EncodeSessionAdmissionState& session,
                   const SessionAdmissionCandidate& candidate,
                   const EncodeSessionLimits& limits) noexcept {
  return addWithin(session.sources, std::uint32_t{1}, limits.maxSources) &&
         addWithin(session.pages, candidatePageFootprint(candidate),
                   limits.maxPages) &&
         addWithin(session.residencyBytes.value, candidate.residencyBytes.value,
                   limits.maxBytes) &&
         addWithin(session.draws, candidate.semantic.drawCount,
                   limits.maxDraws) &&
         addWithin(session.commandBuffers, candidate.predictedCommandBuffers,
                   limits.maxCommandBuffers);
}

SessionCapacityDimension limitingDimension(
    const EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept {
  if (!addWithin(session.sources, std::uint32_t{1}, limits.maxSources)) {
    return SessionCapacityDimension::Sources;
  }
  if (!addWithin(session.pages, candidatePageFootprint(candidate),
                 limits.maxPages)) {
    return SessionCapacityDimension::Pages;
  }
  if (!addWithin(session.residencyBytes.value, candidate.residencyBytes.value,
                 limits.maxBytes)) {
    return SessionCapacityDimension::Bytes;
  }
  if (!addWithin(session.draws, candidate.semantic.drawCount,
                 limits.maxDraws)) {
    return SessionCapacityDimension::Draws;
  }
  if (!addWithin(session.commandBuffers, candidate.predictedCommandBuffers,
                 limits.maxCommandBuffers)) {
    return SessionCapacityDimension::CommandBuffers;
  }
  return SessionCapacityDimension::None;
}

SessionCapacityRejectionObservation capacityRejectionObservation(
    const EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept {
  const std::uint64_t payloadPages = candidate.semantic.pageCount;
  const std::uint64_t requiredPages = candidatePageFootprint(candidate);
  const std::uint64_t requiredTotalSources =
      static_cast<std::uint64_t>(session.sources) + 1u;
  const std::uint64_t requiredTotalPages =
      static_cast<std::uint64_t>(session.pages) + requiredPages;
  std::uint8_t exceededAxes = SessionCapacityExceededNone;
  if (requiredTotalSources > limits.maxSources) {
    exceededAxes |= SessionCapacityExceededSources;
  }
  if (requiredTotalPages > limits.maxPages) {
    exceededAxes |= SessionCapacityExceededPages;
  }
  if (exceededAxes == SessionCapacityExceededNone) {
    return {};
  }
  return {
      .predecessorSources = session.sources,
      .predecessorPages = session.pages,
      .candidatePayloadPages = payloadPages,
      .candidateWrapPaddingPages =
          requiredPages > payloadPages ? requiredPages - payloadPages : 0u,
      .candidateRequiredPages = requiredPages,
      .requiredTotalSources = requiredTotalSources,
      .requiredTotalPages = requiredTotalPages,
      .exceededAxes = exceededAxes,
  };
}

constexpr bool vectorAtMost(const SessionCapacityVector& left,
                            const SessionCapacityVector& right) noexcept {
  return left.sources <= right.sources && left.pages <= right.pages &&
         left.bytes <= right.bytes && left.draws <= right.draws &&
         left.payloadBlocks <= right.payloadBlocks &&
         left.readyEntries <= right.readyEntries &&
         left.retentionEntries <= right.retentionEntries &&
         left.allocatorTickets <= right.allocatorTickets &&
         left.commandBuffers <= right.commandBuffers;
}

constexpr bool addVector(const SessionCapacityVector& left,
                         const SessionCapacityVector& right,
                         SessionCapacityVector& result) noexcept {
  const auto add = [](std::uint64_t a, std::uint64_t b,
                      std::uint64_t& out) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
      return false;
    }
    out = a + b;
    return true;
  };
  return add(left.sources, right.sources, result.sources) &&
         add(left.pages, right.pages, result.pages) &&
         add(left.bytes, right.bytes, result.bytes) &&
         add(left.draws, right.draws, result.draws) &&
         add(left.payloadBlocks, right.payloadBlocks,
             result.payloadBlocks) &&
         add(left.readyEntries, right.readyEntries,
             result.readyEntries) &&
         add(left.retentionEntries, right.retentionEntries,
             result.retentionEntries) &&
         add(left.allocatorTickets, right.allocatorTickets,
             result.allocatorTickets) &&
         add(left.commandBuffers, right.commandBuffers,
             result.commandBuffers);
}

constexpr SessionCapacityVector subtractVector(
    const SessionCapacityVector& left,
    const SessionCapacityVector& right) noexcept {
  return {
      .sources = left.sources - right.sources,
      .pages = left.pages - right.pages,
      .bytes = left.bytes - right.bytes,
      .draws = left.draws - right.draws,
      .payloadBlocks = left.payloadBlocks - right.payloadBlocks,
      .readyEntries = left.readyEntries - right.readyEntries,
      .retentionEntries = left.retentionEntries - right.retentionEntries,
      .allocatorTickets = left.allocatorTickets - right.allocatorTickets,
      .commandBuffers = left.commandBuffers - right.commandBuffers,
  };
}

bool candidateIdentityValid(
    const SessionAdmissionCandidate& candidate) noexcept {
  return candidate.key.valid() && candidate.semantic.valid() &&
         candidate.residencyBytes.valid() &&
         candidate.sourceOrdinal != 0 && candidate.seqId != 0 &&
         candidate.predictedCommandBuffers != 0 &&
         candidate.payloadBlocks != 0 && candidate.retentionEntries != 0 &&
         candidate.allocatorTickets != 0;
}

bool candidateMustBeIsolated(
    const SessionAdmissionCandidate& candidate) noexcept {
  return candidate.semantic.requiresIsolation() ||
         candidate.key.captureMode == EncodeCaptureMode::Isolated ||
         candidate.semantic.entryKind == core::SourceEntryEncoderKind::Empty;
}

}  // namespace

bool SessionCapacityPolicy::valid() const noexcept {
  SessionCapacityVector reserved{};
  return highWater.sources != 0 && highWater.pages != 0 &&
         highWater.bytes != 0 && highWater.draws != 0 &&
         highWater.payloadBlocks != 0 && highWater.readyEntries != 0 &&
         highWater.retentionEntries != 0 &&
         highWater.allocatorTickets != 0 &&
         highWater.commandBuffers != 0 && maxSession.sources != 0 &&
         maxSession.pages != 0 && maxSession.bytes != 0 &&
         maxSession.draws != 0 && maxSession.payloadBlocks != 0 &&
         maxSession.readyEntries != 0 && maxSession.retentionEntries != 0 &&
         maxSession.allocatorTickets != 0 &&
         maxSession.commandBuffers != 0 &&
         successorHeadroom.sources != 0 &&
         successorHeadroom.pages != 0 &&
         successorHeadroom.bytes != 0 &&
         successorHeadroom.draws != 0 &&
         successorHeadroom.payloadBlocks != 0 &&
         successorHeadroom.readyEntries != 0 &&
         successorHeadroom.retentionEntries != 0 &&
         successorHeadroom.allocatorTickets != 0 &&
         successorHeadroom.commandBuffers != 0 &&
         ordinaryDirect.sources == 1 && ordinaryDirect.pages != 0 &&
         ordinaryDirect.bytes != 0 && ordinaryDirect.draws != 0 &&
         ordinaryDirect.payloadBlocks != 0 &&
         ordinaryDirect.readyEntries == 1 &&
         ordinaryDirect.retentionEntries != 0 &&
         ordinaryDirect.allocatorTickets != 0 &&
         ordinaryDirect.commandBuffers != 0 &&
         successorHeadroom.pages >=
             worstCaseNonWrappingReservationPages(ordinaryDirect.pages) &&
         vectorAtMost(ordinaryDirect, successorHeadroom) &&
         addVector(maxSession, successorHeadroom, reserved) &&
         vectorAtMost(reserved, highWater);
}

std::optional<SessionCapacityVector>
sessionCapacityUnavailableForFirstLease(
    const SessionCapacityLeaseAcquisitionSnapshot& snapshot,
    const SessionCapacityVector& successorHeadroom) noexcept {
  if (!snapshot.valid) {
    return std::nullopt;
  }
  if (!snapshot.orderedTailWritingSuccessor.has_value()) {
    return snapshot.olderUnavailable;
  }
  if (vectorAtMost(*snapshot.orderedTailWritingSuccessor,
                   successorHeadroom)) {
    return snapshot.olderUnavailable;
  }
  SessionCapacityVector unavailable{};
  if (!addVector(snapshot.olderUnavailable,
                 *snapshot.orderedTailWritingSuccessor, unavailable)) {
    return std::nullopt;
  }
  return unavailable;
}

bool SessionCapacityLeaseState::acquire(
    const SessionCapacityPolicy& policy,
    const SessionCapacityVector& unavailable,
    const SessionCapacityVector& initialCharge) noexcept {
  if (lease_.valid() || !policy.valid() || nextGeneration_ == 0) {
    ++stats_.denials;
    return false;
  }
  SessionCapacityVector reserved{};
  SessionCapacityVector physicalClaim{};
  if (!addVector(policy.maxSession, policy.successorHeadroom, reserved) ||
      !addVector(unavailable, reserved, physicalClaim) ||
      !vectorAtMost(physicalClaim, policy.highWater) ||
      !vectorAtMost(initialCharge, policy.maxSession)) {
    ++stats_.denials;
    return false;
  }
  lease_ = {
      .generation = nextGeneration_++,
      .reserved = reserved,
      .used = initialCharge,
      .successorRemaining = policy.successorHeadroom,
  };
  ++stats_.acquisitions;
  stats_.peakCurrent = std::max<std::uint64_t>(stats_.peakCurrent, 1);
  stats_.reserved = reserved;
  stats_.used = initialCharge;
  stats_.slack = subtractVector(policy.maxSession, initialCharge);
  stats_.successorMinimum = policy.successorHeadroom;
  return true;
}

bool SessionCapacityLeaseState::charge(
    std::uint64_t generation,
    const SessionCapacityVector& candidate) noexcept {
  if (!lease_.valid() || generation != lease_.generation) {
    return false;
  }
  SessionCapacityVector next{};
  if (!addVector(lease_.used, candidate, next) ||
      !vectorAtMost(next, subtractVector(lease_.reserved,
                                         lease_.successorRemaining))) {
    return false;
  }
  lease_.used = next;
  stats_.used = next;
  stats_.slack = subtractVector(
      subtractVector(lease_.reserved, lease_.successorRemaining), next);
  return true;
}

bool SessionCapacityLeaseState::uncharge(
    std::uint64_t generation,
    const SessionCapacityVector& candidate) noexcept {
  if (!lease_.valid() || generation != lease_.generation ||
      !vectorAtMost(candidate, lease_.used)) {
    return false;
  }
  lease_.used = subtractVector(lease_.used, candidate);
  stats_.used = lease_.used;
  stats_.slack = subtractVector(
      subtractVector(lease_.reserved, lease_.successorRemaining),
      lease_.used);
  return true;
}

bool SessionCapacityLeaseState::release(std::uint64_t generation) noexcept {
  if (!lease_.valid() || generation != lease_.generation) {
    return false;
  }
  lease_ = {};
  ++stats_.releases;
  stats_.reserved = {};
  stats_.used = {};
  stats_.slack = {};
  return true;
}

SessionCapacityVector sessionCapacityFor(
    const SessionAdmissionCandidate& candidate) noexcept {
  return {
      .sources = 1,
      .pages = candidatePageFootprint(candidate),
      .bytes = candidate.residencyBytes.value,
      .draws = candidate.semantic.drawCount,
      .payloadBlocks = candidate.payloadBlocks,
      .readyEntries = 1,
      .retentionEntries = candidate.retentionEntries,
      .allocatorTickets = candidate.allocatorTickets,
      .commandBuffers = candidate.predictedCommandBuffers,
  };
}

SessionCapacityVector sessionPhysicalResidencyCapacityFor(
    const SessionAdmissionCandidate& candidate) noexcept {
  return {
      .sources = 1,
      .pages = candidatePageFootprint(candidate),
      .bytes = candidate.residencyBytes.value,
      .payloadBlocks = candidate.payloadBlocks,
      .readyEntries = 1,
      .retentionEntries = candidate.retentionEntries,
      .allocatorTickets = candidate.allocatorTickets,
  };
}

bool retireSessionAdmissionResidency(
    EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate) noexcept {
  const std::uint32_t pages = candidatePageFootprint(candidate);
  if (!session.valid() || session.residentSources == 0u ||
      pages > session.pages ||
      candidate.residencyBytes.value > session.residencyBytes.value) {
    return false;
  }
  --session.residentSources;
  session.pages -= pages;
  session.residencyBytes.value -= candidate.residencyBytes.value;
  return true;
}

SessionAdmissionResult classifySessionAdmissionDetailed(
    const EncodeSessionAdmissionState& session,
    const SessionAdmissionCandidate& candidate,
    const EncodeSessionLimits& limits) noexcept {
  const auto decision = classifySessionAdmission(session, candidate, limits);
  const bool rejectedBeforeCandidate =
      decision == SessionAdmissionDecision::SubmitPrefixBeforeCandidate ||
      decision == SessionAdmissionDecision::ProcessCandidateIsolated;
  const SessionCapacityDimension dimension = rejectedBeforeCandidate
      ? limitingDimension(session, candidate, limits)
      : SessionCapacityDimension::None;
  return {
      .decision = decision,
      .limitingDimension = dimension,
      .capacityRejection =
          rejectedBeforeCandidate &&
                  (dimension == SessionCapacityDimension::Sources ||
                   dimension == SessionCapacityDimension::Pages)
              ? capacityRejectionObservation(session, candidate, limits)
              : SessionCapacityRejectionObservation{},
  };
}

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
        .residencyBytes = candidate.residencyBytes,
        .sources = 1,
        .residentSources = 1,
        .pages = candidatePageFootprint(candidate),
        .draws = candidate.semantic.drawCount,
        .commandBuffers = candidate.predictedCommandBuffers,
        .flags = EncodeSessionAdmissionValid |
                 (candidate.semantic.hasPresent()
                      ? EncodeSessionAdmissionHasPresent
                      : 0u),
    };
    return true;
  }
  if (candidate.rawOrdinal != 0u) {
    session.lastRawOrdinal = candidate.rawOrdinal;
  }
  session.lastSourceOrdinal = candidate.sourceOrdinal;
  session.lastSeqId = candidate.seqId;
  session.residencyBytes.value += candidate.residencyBytes.value;
  ++session.sources;
  ++session.residentSources;
  session.pages += candidatePageFootprint(candidate);
  session.draws += candidate.semantic.drawCount;
  session.commandBuffers += candidate.predictedCommandBuffers;
  if (candidate.semantic.hasPresent()) {
    session.flags |= EncodeSessionAdmissionHasPresent;
  }
  return true;
}

MultiSourceSessionWindowPreflight preflightMultiSourceSessionWindow(
    const EncodeSessionAdmissionState& pending,
    std::span<const SessionAdmissionCandidate> candidates,
    const EncodeSessionLimits& limits,
    MultiSourceSessionWindowFrontier frontier,
    bool captureBoundary,
    bool initializerBoundary,
    bool orderedReleaseBoundary) noexcept {
  MultiSourceSessionWindowPreflight result{
      .stagedAdmission = pending,
  };
  const bool fresh =
      frontier == MultiSourceSessionWindowFrontier::FreshClean;
  const bool carried =
      frontier == MultiSourceSessionWindowFrontier::CleanClosed ||
      frontier == MultiSourceSessionWindowFrontier::ActiveRenderComplete;
  if ((!fresh && !carried) || (fresh && pending.valid()) ||
      (carried && !pending.valid())) {
    return result;
  }
  if (candidates.size() < 2u ||
      candidates.size() > kMaxMultiSourceSessionWindowSources) {
    result.reason = MultiSourceSessionWindowPreflightReason::SourceCount;
    return result;
  }
  if (captureBoundary) {
    result.reason = MultiSourceSessionWindowPreflightReason::CaptureBoundary;
    return result;
  }
  if (initializerBoundary) {
    result.reason =
        MultiSourceSessionWindowPreflightReason::InitializerBoundary;
    return result;
  }
  if (orderedReleaseBoundary) {
    result.reason =
        MultiSourceSessionWindowPreflightReason::OrderedReleaseBoundary;
    return result;
  }

  std::uint64_t priorRawOrdinal = pending.lastRawOrdinal;
  std::uint64_t priorSourceOrdinal = pending.lastSourceOrdinal;
  std::uint64_t priorSeqId = pending.lastSeqId;
  bool hasPrior = pending.valid();
  for (const SessionAdmissionCandidate& candidate : candidates) {
    if (!candidateIdentityValid(candidate)) {
      result.reason =
          MultiSourceSessionWindowPreflightReason::InvalidCandidate;
      return result;
    }
    if (candidate.semantic.hasPresent()) {
      result.reason =
          MultiSourceSessionWindowPreflightReason::PresentBoundary;
      return result;
    }
    if (candidate.semantic.requiresIsolation()) {
      result.reason =
          MultiSourceSessionWindowPreflightReason::IsolationBoundary;
      return result;
    }
    if ((candidate.semantic.flags &
         core::SourceSemanticInitializerRequirement) != 0u) {
      result.reason = MultiSourceSessionWindowPreflightReason::
          InitializerSemanticBoundary;
      return result;
    }
    if (hasPrior &&
        (priorSourceOrdinal == std::numeric_limits<std::uint64_t>::max() ||
         priorSeqId == std::numeric_limits<std::uint64_t>::max() ||
         candidate.sourceOrdinal != priorSourceOrdinal + 1u ||
         candidate.seqId != priorSeqId + 1u ||
         (priorRawOrdinal != 0u && candidate.rawOrdinal != 0u &&
          candidate.rawOrdinal <= priorRawOrdinal))) {
      result.reason = MultiSourceSessionWindowPreflightReason::
          NonConsecutiveIdentity;
      return result;
    }
    if (!appendSessionAdmission(result.stagedAdmission, candidate, limits)) {
      result.reason =
          MultiSourceSessionWindowPreflightReason::AdmissionRejected;
      return result;
    }
    if (candidate.rawOrdinal != 0u) {
      priorRawOrdinal = candidate.rawOrdinal;
    }
    priorSourceOrdinal = candidate.sourceOrdinal;
    priorSeqId = candidate.seqId;
    hasPrior = true;
  }
  result.reason = MultiSourceSessionWindowPreflightReason::Eligible;
  return result;
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
