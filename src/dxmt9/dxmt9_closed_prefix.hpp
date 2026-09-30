#pragma once

#include "dxmt9_session_release.hpp"
#include <algorithm>

namespace dxmt9::core::metalqueue {

// Value algebra used by the queue-owned qualification authority below.
// The range candidate consumes receipts only under queue + clean-dirty guards.
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

// Queue-mutex-owned authority. Only the Encode coordinator reports a helper
// result; producer-side code cannot manufacture a quiescent context. This is
// also owns renewal for the guarded producer-side acknowledgement path.
class ClosedPrefixCoordinator {
 public:
  explicit ClosedPrefixCoordinator(std::uint64_t queue) noexcept
      : context_{queue, 1, 1, 0, false, false} {}
  ClosedPrefixCoordinator(const ClosedPrefixCoordinator&) = delete;
  ClosedPrefixCoordinator& operator=(const ClosedPrefixCoordinator&) = delete;

  void beginMutation() noexcept {
    witness_.reset();
    context_.encoderQuiescent = false;
    if (context_.encoderWorkGeneration == std::numeric_limits<std::uint64_t>::max())
      context_.stopped = true;
    else
      ++context_.encoderWorkGeneration;
  }

  void stop() noexcept {
    beginMutation();
    context_.stopped = true;
    if (context_.epoch != std::numeric_limits<std::uint64_t>::max())
      ++context_.epoch;
  }

  SessionReleaseAckResult acknowledgeClose(
      SessionReleaseState& transport, const SessionReleaseSnapshot& snapshot,
      ClosedPrefixOutcome outcome, std::uint64_t rawCoverage,
      std::uint64_t sequenceCoverage, std::uint64_t publishedSequence) noexcept {
    witness_.reset();
    context_.publishedSequence = publishedSequence;
    context_.encoderQuiescent = !context_.stopped;
    witness_ = ClosedPrefixWitness::acknowledgeAndCertify(
        transport, snapshot, context_, outcome, rawCoverage, sequenceCoverage);
    if (witness_)
      return SessionReleaseAckResult::Acknowledged;
    context_.encoderQuiescent = false;
    // Qualification rejection is not a failed ordinary release. Preserve the
    // existing FIFO semantics without replaying the already executed helper.
    return transport.acknowledge(snapshot, SessionReleaseCompletion::PassClosed,
                                 rawCoverage, sequenceCoverage);
  }

  // Inspection only, never an admission predicate: this does not inspect the
  // current release FIFO, terminal latch or a new request's writer policy.
  bool hasReceiptFor(std::uint64_t publishedSequence) const noexcept {
    return witness_.has_value() && context_.valid() &&
        context_.publishedSequence == publishedSequence;
  }

  // Consumer transaction for the default-OFF range candidate. The caller
  // must hold the queue lock and prove that omitted context creation has no
  // side effects (notably pending uniform-dirty consumption).
  bool tryAcknowledgeReuse(
      SessionReleaseState& transport, const SessionReleaseSnapshot& snapshot,
      SessionReleaseWriter writer, bool rangesEnabled,
      std::uint64_t publishedSequence, std::uint64_t sequenceCoverage,
      bool contextSideEffectsAbsent, std::uint64_t& rawCoverage) noexcept {
    if (!contextSideEffectsAbsent || !witness_ ||
        publishedSequence != context_.publishedSequence ||
        sequenceCoverage != context_.publishedSequence ||
        !witness_->permits(transport, snapshot, context_, writer, rangesEnabled))
      return false;
    const auto nextRawCoverage = std::max(rawCoverage, snapshot.event.fenceRawOrdinal);
    const auto renewed = ClosedPrefixWitness::acknowledgeAndCertify(
        transport, snapshot, context_, ClosedPrefixOutcome::NoActivePass,
        nextRawCoverage, sequenceCoverage);
    if (!renewed) return false;
    witness_ = renewed;
    rawCoverage = nextRawCoverage;
    return true;
  }

 private:
  ClosedPrefixContext context_;
  std::optional<ClosedPrefixWitness> witness_;
};

} // namespace dxmt9::core::metalqueue
