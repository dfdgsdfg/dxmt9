#include "d3d9_pe_commit_transition.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <type_traits>
#include <vector>

namespace pe = dxmt9::d3d9::pe;

namespace {

static_assert(!std::is_aggregate_v<pe::RecorderCommitPlan>);

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

struct CommitWitness {
  pe::RecorderCommitPhase phase = pe::RecorderCommitPhase::Unsealed;
  std::vector<std::uint8_t> bytes{1u, 2u, 3u, 4u};
  std::uint32_t records = 2u;
  std::uint32_t handles = 3u;
  std::uint32_t pendingRefs = 2u;
  std::uint32_t aliasDestroys = 0u;
  std::uint32_t parentDestroys = 0u;
  std::uint32_t drainBegins = 0u;
  std::uint32_t drainCompletes = 0u;
  std::uint32_t builderResets = 0u;
  std::uint32_t warmAdvances = 0u;
  bool commandAccepted = false;

  bool apply(pe::RecorderCommitFacts facts) {
    const auto plan = pe::settleRecorderCommit(facts);
    if (!plan.valid()) return false;
    phase = plan.next();
    commandAccepted = commandAccepted || plan.commandAccepted();
    if (plan.objectDestroy()) {
      if (plan.action() == pe::RecorderCommitAction::DestroyAlias) {
        ++aliasDestroys;
        if (pendingRefs != 0u) --pendingRefs;
      } else if (plan.action() == pe::RecorderCommitAction::DestroyParent) {
        ++parentDestroys;
        if (pendingRefs != 0u) --pendingRefs;
      }
    }
    if (plan.action() == pe::RecorderCommitAction::BeginDrain) {
      ++drainBegins;
    } else if (plan.action() == pe::RecorderCommitAction::FinishDrain) {
      ++drainCompletes;
    }
    if (plan.resetBuilder()) {
      ++builderResets;
      bytes.clear();
      records = handles = pendingRefs = 0u;
    }
    if (plan.advanceWarmEpoch()) ++warmAdvances;
    return true;
  }
};

void testRetryAndAcceptedCapture() {
  CommitWitness witness{};
  const auto retryBytes = witness.bytes;
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Unsealed,
            .event = pe::RecorderCommitEvent::SealFailed}) &&
            witness.phase == pe::RecorderCommitPhase::Unsealed &&
            witness.bytes == retryBytes,
        "seal failure preserves retry bytes");
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Unsealed,
            .event = pe::RecorderCommitEvent::SealAccepted}) &&
            witness.phase == pe::RecorderCommitPhase::Sealed,
        "successful seal enters sealed phase");
  const auto sealedBytes = witness.bytes;
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Sealed,
            .event = pe::RecorderCommitEvent::BridgeFailed}) &&
            witness.phase == pe::RecorderCommitPhase::Sealed &&
            witness.bytes == sealedBytes,
        "bridge failure preserves sealed retry bytes");
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Sealed,
            .event = pe::RecorderCommitEvent::BridgeAccepted}) &&
            witness.commandAccepted &&
            witness.phase == pe::RecorderCommitPhase::Accepted,
        "bridge acceptance publishes command only");
  check(witness.apply({
            .phase = pe::RecorderCommitPhase::Accepted,
            .event = pe::RecorderCommitEvent::CaptureRejected}) &&
            witness.commandAccepted &&
            witness.phase == pe::RecorderCommitPhase::CaptureSettled,
        "capture rejection does not retract accepted command");

  CommitWitness skipped{};
  skipped.commandAccepted = true;
  check(skipped.apply({
            .phase = pe::RecorderCommitPhase::Accepted,
            .event = pe::RecorderCommitEvent::CaptureSkipped}) &&
            skipped.commandAccepted &&
            skipped.phase == pe::RecorderCommitPhase::CaptureSettled,
        "capture skipped settles without capture side effects");
}

void testDrainOrderingAndReset() {
  CommitWitness witness{};
  witness.apply({.phase = pe::RecorderCommitPhase::Unsealed,
                 .event = pe::RecorderCommitEvent::SealAccepted});
  witness.apply({.phase = pe::RecorderCommitPhase::Sealed,
                 .event = pe::RecorderCommitEvent::BridgeAccepted});
  check(witness.apply({.phase = pe::RecorderCommitPhase::Accepted,
                       .event = pe::RecorderCommitEvent::CaptureRejected}) &&
            witness.phase == pe::RecorderCommitPhase::CaptureSettled,
        "capture rejection settles after accepted bridge");
  check(witness.apply({.phase = pe::RecorderCommitPhase::CaptureSettled,
                       .event = pe::RecorderCommitEvent::DrainPending}) &&
            witness.phase == pe::RecorderCommitPhase::Draining,
        "accepted capture begins pending-reference drain");
  check(!pe::settleRecorderCommit({
              .phase = pe::RecorderCommitPhase::Draining,
              .event = pe::RecorderCommitEvent::DrainParent,
              .aliasesRemain = true,
              .parentPending = true})
             .valid(),
        "parent cannot destroy before aliases");
  check(witness.apply({.phase = pe::RecorderCommitPhase::Draining,
                       .event = pe::RecorderCommitEvent::DrainAlias,
                       .aliasesRemain = true}) &&
            witness.aliasDestroys == 1u,
        "alias destroy is admitted first");
  check(witness.apply({.phase = pe::RecorderCommitPhase::Draining,
                       .event = pe::RecorderCommitEvent::DrainParent,
                       .parentPending = true}) &&
            witness.parentDestroys == 1u && witness.phase ==
                pe::RecorderCommitPhase::Drained,
        "parent destroy is exactly once after aliases");
  check(witness.drainBegins == 1u,
        "capture failure begins pending-reference drain exactly once");
  check(!witness.apply({.phase = pe::RecorderCommitPhase::Drained,
                        .event = pe::RecorderCommitEvent::DrainParent,
                        .parentPending = true}),
        "repeated parent destroy is rejected");
  check(witness.apply({.phase = pe::RecorderCommitPhase::Drained,
                       .event = pe::RecorderCommitEvent::BuilderReset}) &&
            witness.bytes.empty() && witness.builderResets == 1u,
        "builder reset follows reference drain");
  check(witness.apply({.phase = pe::RecorderCommitPhase::Reset,
                       .event = pe::RecorderCommitEvent::WarmEpochAdvance}) &&
            witness.warmAdvances == 1u &&
            witness.phase == pe::RecorderCommitPhase::WarmAdvanced,
        "warm epoch advances after reset");
  check(witness.apply({.phase = pe::RecorderCommitPhase::WarmAdvanced,
                       .event = pe::RecorderCommitEvent::DrainComplete}) &&
            witness.phase == pe::RecorderCommitPhase::Unsealed,
        "completed settlement returns to unsealed");

  // The no-pending-reference variant takes the explicit DrainComplete edge;
  // it exercises the same pure helper used by production cleanup.
  CommitWitness noPending{};
  noPending.apply({.phase = pe::RecorderCommitPhase::Unsealed,
                   .event = pe::RecorderCommitEvent::SealAccepted});
  noPending.apply({.phase = pe::RecorderCommitPhase::Sealed,
                   .event = pe::RecorderCommitEvent::BridgeAccepted});
  noPending.apply({.phase = pe::RecorderCommitPhase::Accepted,
                   .event = pe::RecorderCommitEvent::CaptureRejected});
  noPending.apply({.phase = pe::RecorderCommitPhase::CaptureSettled,
                   .event = pe::RecorderCommitEvent::DrainPending});
  check(noPending.apply({.phase = pe::RecorderCommitPhase::Draining,
                         .event = pe::RecorderCommitEvent::DrainComplete,
                         .parentPending = false}) &&
            noPending.drainCompletes == 1u &&
            noPending.phase == pe::RecorderCommitPhase::Drained,
        "capture failure completes drain exactly once");
}

void testDiscardAndFailureMatrix() {
  constexpr std::array<pe::RecorderCommitPhase, 8> phases = {
      pe::RecorderCommitPhase::Unsealed, pe::RecorderCommitPhase::Sealed,
      pe::RecorderCommitPhase::Accepted,
      pe::RecorderCommitPhase::CaptureSettled,
      pe::RecorderCommitPhase::Draining, pe::RecorderCommitPhase::Drained,
      pe::RecorderCommitPhase::Reset, pe::RecorderCommitPhase::WarmAdvanced};
  for (const auto phase : phases) {
    CommitWitness witness{};
    witness.phase = phase;
    const auto plan = pe::settleRecorderCommit({
        .phase = phase,
        .event = pe::RecorderCommitEvent::ExplicitDiscard});
    check(plan.valid() && plan.action() == pe::RecorderCommitAction::DiscardAll &&
              plan.resetBuilder() && !plan.advanceWarmEpoch(),
          "explicit discard releases pins without warm epoch advance");
    witness.apply({.phase = phase,
                   .event = pe::RecorderCommitEvent::ExplicitDiscard});
    check(witness.bytes.empty() && witness.records == 0u &&
              witness.handles == 0u && witness.pendingRefs == 0u &&
              witness.aliasDestroys == 0u && witness.parentDestroys == 0u,
          "discard clears bytes/counts/pins without ObjectDestroy");
    check(!pe::settleRecorderCommit({
                .phase = pe::RecorderCommitPhase::Discarded,
                .event = pe::RecorderCommitEvent::ExplicitDiscard})
             .valid(),
          "repeated discard is rejected");
  }
}

}  // namespace

int main() {
  testRetryAndAcceptedCapture();
  testDrainOrderingAndReset();
  testDiscardAndFailureMatrix();
  if (failures != 0) return 1;
  std::puts("pe commit transition spec: PASS");
  return 0;
}
