#pragma once

#include "dxmt9_session_release.hpp"

namespace dxmt9::core::metalqueue {

// Candidate admission algebra only. Production has not yet supplied the
// encoder-work epoch/quiescence authority required to skip a release wake.
struct ClosedPrefixContext {
  std::uint64_t queue = 0;
  std::uint64_t epoch = 0;
  std::uint64_t encoderWorkGeneration = 0;
  std::uint64_t publishedSequence = 0;
  bool encoderQuiescent = false;
  bool stopped = false;

  constexpr bool valid() const noexcept {
    return queue && epoch && encoderWorkGeneration && encoderQuiescent && !stopped;
  }
};

enum class ClosedPrefixOutcome : std::uint8_t {
  NoActivePass,
  Closed,
  Invalid,
};

// Conditional admission value, not a production authority: issuance consumes
// the exact current FIFO head through the transport's real acknowledgement.
// The helper outcome and coordinator context still require trusted execution.
// Copying does not grant reuse in another context/epoch.
class ClosedPrefixWitness {
 public:
  // Queue-lock-owned transaction. Failure leaves the FIFO unchanged; callers
  // must then execute the ordinary release path, not treat rejection as an ack.
  static std::optional<ClosedPrefixWitness> acknowledgeAndCertify(
      SessionReleaseState& transport,
      const SessionReleaseSnapshot& acknowledged,
      ClosedPrefixContext context, ClosedPrefixOutcome outcome,
      std::uint64_t coveredRawOrdinal, std::uint64_t coveredSequence) noexcept {
    if (!context.valid() || !eligible(acknowledged) ||
        (outcome != ClosedPrefixOutcome::Closed &&
         outcome != ClosedPrefixOutcome::NoActivePass) ||
        transport.terminalRequested() || transport.orderedSize() != 1 ||
        transport.peekNext() != acknowledged ||
        context.publishedSequence != coveredSequence ||
        acknowledged.event.fenceSeqId != coveredSequence)
      return std::nullopt;
    if (transport.acknowledge(acknowledged, SessionReleaseCompletion::PassClosed,
            coveredRawOrdinal, coveredSequence) != SessionReleaseAckResult::Acknowledged)
      return std::nullopt;
    return ClosedPrefixWitness(context, acknowledged.event);
  }

  bool permits(const SessionReleaseState& transport,
               const SessionReleaseSnapshot& next,
               ClosedPrefixContext context, SessionReleaseWriter writer,
               bool rangesEnabled) const noexcept {
    return context.valid() && context.queue == context_.queue &&
        context.epoch == context_.epoch &&
        context.encoderWorkGeneration == context_.encoderWorkGeneration &&
        context.publishedSequence == context_.publishedSequence &&
        retainWriterForSessionRelease(writer, rangesEnabled,
            next.event.reason, next.event.action) && eligible(next) &&
        !transport.terminalRequested() && transport.orderedSize() == 1 &&
        transport.peekNext() == next &&
        transport.acknowledgedOrdinal() == acknowledged_.ordinal &&
        next.event.ordinal > acknowledged_.ordinal &&
        next.event.ordinal - acknowledged_.ordinal == 1 &&
        next.event.fenceRawOrdinal >= acknowledged_.fenceRawOrdinal &&
        next.event.fenceSeqId == context_.publishedSequence;
  }

 private:
  static bool eligible(const SessionReleaseSnapshot& snapshot) noexcept {
    return snapshot.valid() && snapshot.origin == SessionReleaseOrigin::Ordered &&
        snapshot.generation == 1 &&
        snapshot.event.reason == SessionReleaseReason::DirectObservation &&
        snapshot.event.action == SessionReleaseAction::ClosePass;
  }
  ClosedPrefixWitness(ClosedPrefixContext context,
                      SessionReleaseEvent acknowledged) noexcept
      : context_(context), acknowledged_(acknowledged) {}
  ClosedPrefixContext context_;
  SessionReleaseEvent acknowledged_;
};

} // namespace dxmt9::core::metalqueue
