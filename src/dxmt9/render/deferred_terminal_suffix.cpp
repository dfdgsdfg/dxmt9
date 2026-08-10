#include "deferred_terminal_suffix.hpp"

#include <limits>

namespace dxmt9::render {

namespace {

constexpr std::uint32_t kSemanticDrainFlags =
    DeferredTerminalSuffixWakeOrderedRelease |
    DeferredTerminalSuffixWakeProducerWait |
    DeferredTerminalSuffixWakeInitializer |
    DeferredTerminalSuffixWakeQuery |
    DeferredTerminalSuffixWakeReadback |
    DeferredTerminalSuffixWakeUpdateTexture |
    DeferredTerminalSuffixWakePresent |
    DeferredTerminalSuffixWakeStop |
    DeferredTerminalSuffixWakeDeviceLoss |
    DeferredTerminalSuffixWakeWriterLost |
    DeferredTerminalSuffixWakeLeaseInvalidated |
    DeferredTerminalSuffixWakeHeadroomInvalidated;

constexpr std::uint32_t kPressureFlags =
    DeferredTerminalSuffixWakeAdmissionPressure |
    DeferredTerminalSuffixWakeWriterPressure;

using LeaseCapacityClaim = core::CpuReadyTape::LeaseCapacityClaim;

bool consecutive(const DeferredTerminalSuffixSourceIdentity& current,
                 const DeferredTerminalSuffixSourceIdentity& successor)
    noexcept {
  if (!current.valid() || !successor.valid() ||
      current.sourceOrdinal == std::numeric_limits<std::uint64_t>::max() ||
      current.seqId == std::numeric_limits<std::uint64_t>::max() ||
      successor.sourceOrdinal != current.sourceOrdinal + 1u ||
      successor.seqId != current.seqId + 1u) {
    return false;
  }
  return current.rawOrdinal == 0u || successor.rawOrdinal == 0u ||
      successor.rawOrdinal > current.rawOrdinal;
}

bool identityMatchesExpected(
    const DeferredTerminalSuffixSourceIdentity& expected,
    const DeferredTerminalSuffixSourceIdentity& observed) noexcept {
  return expected.source == observed.source &&
      expected.sourceOrdinal == observed.sourceOrdinal &&
      expected.seqId == observed.seqId &&
      (expected.rawOrdinal == 0u || observed.rawOrdinal == 0u ||
       expected.rawOrdinal == observed.rawOrdinal);
}

bool admissionMatchesIdentity(
    const SessionAdmissionCandidate& admission,
    const DeferredTerminalSuffixSourceIdentity& identity) noexcept {
  return sessionAdmissionCandidateIdentityValid(admission) &&
      admission.sourceOrdinal == identity.sourceOrdinal &&
      admission.seqId == identity.seqId &&
      (admission.rawOrdinal == 0u || identity.rawOrdinal == 0u ||
       admission.rawOrdinal == identity.rawOrdinal);
}

bool rangeMatchesIdentity(
    const framegraph::SourceCommandRange& range,
    const DeferredTerminalSuffixSourceIdentity& identity) noexcept {
  return range.valid() && range.source == identity.source &&
      range.sourceOrdinal == identity.sourceOrdinal &&
      range.seqId == identity.seqId;
}

bool currentRangesCoverSnapshot(
    const framegraph::SourceCommandRange& prefix,
    const framegraph::SourceCommandRange& suffix,
    const core::metalqueue::ReadySlotSnapshot& snapshot) noexcept {
  const std::uint64_t suffixBegin =
      static_cast<std::uint64_t>(prefix.commandBegin) +
      prefix.commandCount;
  const std::uint64_t commandEnd = suffixBegin + suffix.commandCount;
  return suffixBegin == suffix.commandBegin &&
      prefix.commandBegin == snapshot.commandBegin &&
      commandEnd == static_cast<std::uint64_t>(snapshot.commandBegin) +
          snapshot.commandCount;
}

bool claimAtMost(const LeaseCapacityClaim& value,
                 const LeaseCapacityClaim& limit) noexcept {
  return value.sources <= limit.sources && value.pages <= limit.pages &&
      value.bytes <= limit.bytes &&
      value.payloadBlocks <= limit.payloadBlocks &&
      value.readyEntries <= limit.readyEntries &&
      value.retentionEntries <= limit.retentionEntries &&
      value.allocatorTickets <= limit.allocatorTickets;
}

bool claimValid(const LeaseCapacityClaim& claim) noexcept {
  return claim.sources == 1u && claim.pages != 0u && claim.bytes != 0u &&
      claim.payloadBlocks != 0u && claim.readyEntries == 1u &&
      claim.retentionEntries != 0u && claim.allocatorTickets != 0u;
}

LeaseCapacityClaim physicalClaim(
    const SessionCapacityVector& capacity) noexcept {
  return {
      .sources = capacity.sources,
      .pages = capacity.pages,
      .bytes = capacity.bytes,
      .payloadBlocks = capacity.payloadBlocks,
      .readyEntries = capacity.readyEntries,
      .retentionEntries = capacity.retentionEntries,
      .allocatorTickets = capacity.allocatorTickets,
  };
}

SessionCapacityVector capacityForPhysicalClaim(
    const LeaseCapacityClaim& claim) noexcept {
  return {
      .sources = claim.sources,
      .pages = claim.pages,
      .bytes = claim.bytes,
      .payloadBlocks = claim.payloadBlocks,
      .readyEntries = claim.readyEntries,
      .retentionEntries = claim.retentionEntries,
      .allocatorTickets = claim.allocatorTickets,
  };
}

bool baseStructurallyValid(
    const DeferredTerminalSuffixState& state) noexcept {
  const auto usableBound = sessionCapacityLeaseUsableBound({
      .generation = state.leaseGeneration,
      .reserved = state.leaseReserved,
      .used = state.leaseUsedBeforeSuccessor,
      .successorRemaining = state.leaseSuccessorRemaining,
  });
  const auto physicallyCharged = addSessionCapacity(
      state.leaseUsedBeforeSuccessor,
      capacityForPhysicalClaim(state.successorWritingClaim));
  return consecutive(state.currentIdentity,
                  state.expectedWritingSuccessor) &&
      admissionMatchesIdentity(state.admission, state.currentIdentity) &&
      state.current.sourceId == state.currentIdentity.source.id &&
      state.current.storage == state.currentIdentity.source.storage &&
      state.current.seqId == state.currentIdentity.seqId &&
      state.current.metadata.rawOrdinal == state.currentIdentity.rawOrdinal &&
      state.current.metadata.sourceOrdinal ==
          state.currentIdentity.sourceOrdinal &&
      state.current.metadata.seqId == state.currentIdentity.seqId &&
      state.current.semantic == state.admission.semantic &&
      state.currentCompletion.locatorBacked() &&
      state.currentCompletion.source == state.currentIdentity.source &&
      state.currentCompletion.seqId == state.currentIdentity.seqId &&
      state.currentCompletion.slotIndex == state.current.slotIndex &&
      state.currentCompletion.commandBegin == state.current.commandBegin &&
      state.currentCompletion.commandCount == state.current.commandCount &&
      rangeMatchesIdentity(state.currentPrefix, state.currentIdentity) &&
      rangeMatchesIdentity(state.currentSuffix, state.currentIdentity) &&
      currentRangesCoverSnapshot(state.currentPrefix, state.currentSuffix,
                                 state.current) &&
      state.leaseGeneration != 0u &&
      state.currentCharge == sessionCapacityFor(state.admission) &&
      sessionCapacityFitsWithin(state.currentCharge,
                                state.leaseUsedBeforeSuccessor) &&
      usableBound.has_value() &&
      sessionCapacityFitsWithin(state.leaseUsedBeforeSuccessor,
                                *usableBound) &&
      physicallyCharged.has_value() &&
      sessionCapacityFitsWithin(*physicallyCharged, *usableBound) &&
      claimValid(state.successorWritingClaim) &&
      claimAtMost(state.successorWritingClaim,
                  state.successorPhysicalHeadroom);
}

bool provisionalStructurallyValid(
    const DeferredTerminalSuffixState& state) noexcept {
  return (state.phase == DeferredTerminalSuffixPhase::PrefixEncoded ||
          state.phase == DeferredTerminalSuffixPhase::Held) &&
      baseStructurallyValid(state) &&
      state.successorClaim == SessionCapacityVector{} &&
      !state.proofAttached;
}

bool proofMatchesState(
    const DeferredTerminalSuffixState& state,
    const framegraph::DeferredTerminalSuffixProof& proof,
    const DeferredTerminalSuffixSourceIdentity& successor) noexcept {
  return proof.currentPrefix == state.currentPrefix &&
      proof.currentSuffix == state.currentSuffix &&
      rangeMatchesIdentity(proof.successorHead, successor);
}

bool tentativeStructurallyValid(
    const DeferredTerminalSuffixState& state) noexcept {
  const auto usableBound = sessionCapacityLeaseUsableBound({
      .generation = state.leaseGeneration,
      .reserved = state.leaseReserved,
      .used = state.leaseUsedBeforeSuccessor,
      .successorRemaining = state.leaseSuccessorRemaining,
  });
  const auto charged = addSessionCapacity(
      state.leaseUsedBeforeSuccessor, state.successorClaim);
  return state.phase == DeferredTerminalSuffixPhase::SuccessorTentative &&
      baseStructurallyValid(state) &&
      admissionMatchesIdentity(state.successorAdmission,
                               state.expectedWritingSuccessor) &&
      state.successorAdmission.key == state.admission.key &&
      state.successorClaim == sessionCapacityFor(state.successorAdmission) &&
      physicalClaim(state.successorClaim) == state.successorWritingClaim &&
      sessionCapacityFitsWithin(state.successorClaim,
                                state.leaseSuccessorRemaining) &&
      usableBound.has_value() && charged.has_value() &&
      sessionCapacityFitsWithin(*charged, *usableBound) &&
      state.proofAttached &&
      proofMatchesState(state, state.proof,
                        state.expectedWritingSuccessor);
}

bool liveLeaseMatches(
    const DeferredTerminalSuffixState& state,
    const SessionCapacityLease& lease,
    const SessionCapacityVector& expectedUsed) noexcept {
  return lease.valid() && lease.generation == state.leaseGeneration &&
      lease.reserved == state.leaseReserved &&
      lease.successorRemaining == state.leaseSuccessorRemaining &&
      lease.used == expectedUsed;
}

bool liveCurrentMatches(
    const DeferredTerminalSuffixState& state,
    const DeferredTerminalSuffixObservation& observation) noexcept {
  return observation.currentIdentity == state.currentIdentity &&
      observation.currentAdmission == state.admission &&
      observation.release == state.release;
}

bool exactSuccessorReady(
    const DeferredTerminalSuffixState& state,
    const DeferredTerminalSuffixObservation& observation) noexcept {
  if ((observation.wakeFlags &
       DeferredTerminalSuffixWakeExactSuccessorReady) == 0u ||
      !identityMatchesExpected(state.expectedWritingSuccessor,
                               observation.successor) ||
      !admissionMatchesIdentity(observation.successorAdmission,
                               observation.successor) ||
      observation.successorAdmission.key != state.admission.key) {
    return false;
  }
  const auto claim = sessionCapacityFor(observation.successorAdmission);
  return physicalClaim(claim) == state.successorWritingClaim &&
      sessionCapacityFitsWithin(claim,
                                state.leaseSuccessorRemaining);
}

DeferredTerminalSuffixDecisionReason firstDrainReason(
    std::uint32_t wakeFlags) noexcept {
  struct Entry {
    std::uint32_t flag;
    DeferredTerminalSuffixDecisionReason reason;
  };
  constexpr Entry entries[] = {
      {DeferredTerminalSuffixWakeOrderedRelease,
       DeferredTerminalSuffixDecisionReason::OrderedRelease},
      {DeferredTerminalSuffixWakeProducerWait,
       DeferredTerminalSuffixDecisionReason::ProducerWait},
      {DeferredTerminalSuffixWakeInitializer,
       DeferredTerminalSuffixDecisionReason::Initializer},
      {DeferredTerminalSuffixWakeQuery,
       DeferredTerminalSuffixDecisionReason::Query},
      {DeferredTerminalSuffixWakeReadback,
       DeferredTerminalSuffixDecisionReason::Readback},
      {DeferredTerminalSuffixWakeUpdateTexture,
       DeferredTerminalSuffixDecisionReason::UpdateTexture},
      {DeferredTerminalSuffixWakePresent,
       DeferredTerminalSuffixDecisionReason::Present},
      {DeferredTerminalSuffixWakeStop,
       DeferredTerminalSuffixDecisionReason::Stop},
      {DeferredTerminalSuffixWakeDeviceLoss,
       DeferredTerminalSuffixDecisionReason::DeviceLoss},
      {DeferredTerminalSuffixWakeWriterLost,
       DeferredTerminalSuffixDecisionReason::WriterLost},
      {DeferredTerminalSuffixWakeLeaseInvalidated,
       DeferredTerminalSuffixDecisionReason::LeaseInvalidated},
      {DeferredTerminalSuffixWakeHeadroomInvalidated,
       DeferredTerminalSuffixDecisionReason::HeadroomInvalidated},
      {DeferredTerminalSuffixWakeAdmissionPressure,
       DeferredTerminalSuffixDecisionReason::AdmissionPressure},
      {DeferredTerminalSuffixWakeWriterPressure,
       DeferredTerminalSuffixDecisionReason::WriterPressure},
  };
  for (const Entry& entry : entries) {
    if ((wakeFlags & entry.flag) != 0u) {
      return entry.reason;
    }
  }
  return DeferredTerminalSuffixDecisionReason::None;
}

}  // namespace

std::optional<DeferredTerminalSuffixState>
makeDeferredTerminalSuffixPrefixEncoded(
    const DeferredTerminalSuffixSourceIdentity& current,
    const core::metalqueue::ReadySlotSnapshot& currentSnapshot,
    const core::metalqueue::QueueCompletionSource& currentCompletion,
    const SessionAdmissionCandidate& admission,
    const SessionCapacityVector& currentCharge,
    const framegraph::SourceCommandRange& currentPrefix,
    const framegraph::SourceCommandRange& currentSuffix,
    const core::CpuReadyTape::SourceRef& writingSuccessor,
    const core::CpuReadyTape::LeaseCapacityClaim& writingClaim,
    const SessionCapacityLease& lease,
    const SessionCapacityVector& successorHeadroom,
    const core::metalqueue::SessionReleaseSnapshot& release) noexcept {
  if (current.sourceOrdinal == std::numeric_limits<std::uint64_t>::max() ||
      current.seqId == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  DeferredTerminalSuffixState state{
      .phase = DeferredTerminalSuffixPhase::PrefixEncoded,
      .current = currentSnapshot,
      .currentCompletion = currentCompletion,
      .currentIdentity = current,
      .expectedWritingSuccessor = {
          .source = writingSuccessor,
          .sourceOrdinal = current.sourceOrdinal + 1u,
          .seqId = current.seqId + 1u,
      },
      .admission = admission,
      .currentCharge = currentCharge,
      .currentPrefix = currentPrefix,
      .currentSuffix = currentSuffix,
      .successorWritingClaim = writingClaim,
      .successorPhysicalHeadroom = physicalClaim(successorHeadroom),
      .leaseReserved = lease.reserved,
      .leaseUsedBeforeSuccessor = lease.used,
      .leaseSuccessorRemaining = lease.successorRemaining,
      .release = release,
      .leaseGeneration = lease.generation,
  };
  if (!lease.valid() || lease.successorRemaining != successorHeadroom ||
      !provisionalStructurallyValid(state)) {
    return std::nullopt;
  }
  return state;
}

std::optional<DeferredTerminalSuffixState> holdDeferredTerminalSuffix(
    const DeferredTerminalSuffixState& prefixEncoded) noexcept {
  if (prefixEncoded.phase !=
          DeferredTerminalSuffixPhase::PrefixEncoded ||
      !provisionalStructurallyValid(prefixEncoded)) {
    return std::nullopt;
  }
  DeferredTerminalSuffixState held = prefixEncoded;
  held.phase = DeferredTerminalSuffixPhase::Held;
  return held;
}

std::optional<DeferredTerminalSuffixState>
markDeferredTerminalSuffixSuccessorTentative(
    const DeferredTerminalSuffixState& held,
    const DeferredTerminalSuffixObservation& observation) noexcept {
  if (held.phase != DeferredTerminalSuffixPhase::Held ||
      !provisionalStructurallyValid(held) ||
      !liveCurrentMatches(held, observation) ||
      !exactSuccessorReady(held, observation) ||
      !observation.proofValidated ||
      !proofMatchesState(held, observation.proof,
                         observation.successor) ||
      (observation.wakeFlags & kSemanticDrainFlags) != 0u) {
    return std::nullopt;
  }
  const SessionCapacityVector successorClaim =
      sessionCapacityFor(observation.successorAdmission);
  const auto charged = addSessionCapacity(
      held.leaseUsedBeforeSuccessor, successorClaim);
  const auto usableBound = sessionCapacityLeaseUsableBound(
      observation.lease);
  if (!charged || !usableBound ||
      !sessionCapacityFitsWithin(*charged, *usableBound) ||
      !liveLeaseMatches(held, observation.lease, *charged)) {
    return std::nullopt;
  }
  DeferredTerminalSuffixState tentative = held;
  tentative.phase = DeferredTerminalSuffixPhase::SuccessorTentative;
  tentative.expectedWritingSuccessor = observation.successor;
  tentative.successorAdmission = observation.successorAdmission;
  tentative.successorClaim = successorClaim;
  tentative.proof = observation.proof;
  tentative.proofAttached = true;
  return tentativeStructurallyValid(tentative)
      ? std::optional<DeferredTerminalSuffixState>{tentative}
      : std::nullopt;
}

DeferredTerminalSuffixDecisionResult classifyDeferredTerminalSuffix(
    const DeferredTerminalSuffixState& state,
    const DeferredTerminalSuffixObservation& observation) noexcept {
  const bool held = state.phase == DeferredTerminalSuffixPhase::Held;
  if ((held && !provisionalStructurallyValid(state)) ||
      (!held && !tentativeStructurallyValid(state))) {
    return {};
  }
  if (observation.schedulingMutexOwned) {
    return {
        .decision = DeferredTerminalSuffixDecision::Invalid,
        .reason = DeferredTerminalSuffixDecisionReason::SchedulingMutexOwned,
    };
  }
  const auto expectedUsed = held
      ? std::optional<SessionCapacityVector>{
            state.leaseUsedBeforeSuccessor}
      : addSessionCapacity(state.leaseUsedBeforeSuccessor,
                           state.successorClaim);
  if (!expectedUsed || !liveCurrentMatches(state, observation) ||
      !liveLeaseMatches(state, observation.lease, *expectedUsed)) {
    return {
        .decision = DeferredTerminalSuffixDecision::NaturalDrain,
        .reason = DeferredTerminalSuffixDecisionReason::StaleIdentity,
    };
  }

  const std::uint32_t semanticDrains =
      observation.wakeFlags & kSemanticDrainFlags;
  if (semanticDrains != 0u) {
    return {
        .decision = DeferredTerminalSuffixDecision::NaturalDrain,
        .reason = firstDrainReason(semanticDrains),
    };
  }

  if ((observation.wakeFlags &
       DeferredTerminalSuffixWakeExactSuccessorReady) != 0u) {
    if (!exactSuccessorReady(state, observation)) {
      return {
          .decision = DeferredTerminalSuffixDecision::NaturalDrain,
          .reason = DeferredTerminalSuffixDecisionReason::StaleIdentity,
      };
    }
    if (!held && (!state.proofAttached ||
                  observation.successorAdmission !=
                      state.successorAdmission)) {
      return {
          .decision = DeferredTerminalSuffixDecision::NaturalDrain,
          .reason = DeferredTerminalSuffixDecisionReason::StaleIdentity,
      };
    }
    return {
        .decision = held
            ? DeferredTerminalSuffixDecision::ReserveExactSuccessor
            : DeferredTerminalSuffixDecision::JoinExactSuccessor,
        .reason =
            DeferredTerminalSuffixDecisionReason::ExactSuccessorReady,
    };
  }

  const std::uint32_t pressure = observation.wakeFlags & kPressureFlags;
  if (pressure != 0u) {
    return {
        .decision = DeferredTerminalSuffixDecision::NaturalDrain,
        .reason = firstDrainReason(pressure),
    };
  }
  if (!held) {
    return {
        .decision = DeferredTerminalSuffixDecision::NaturalDrain,
        .reason = DeferredTerminalSuffixDecisionReason::StaleIdentity,
    };
  }
  return {
      .decision = DeferredTerminalSuffixDecision::WaitUnlocked,
      .reason = DeferredTerminalSuffixDecisionReason::None,
  };
}

}  // namespace dxmt9::render
