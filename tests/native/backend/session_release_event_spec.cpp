#include "../../../src/dxmt9/dxmt9_session_release.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using namespace dxmt9::core::metalqueue;

static_assert(std::is_trivially_copyable_v<SessionReleaseEvent>);
static_assert(std::is_standard_layout_v<SessionReleaseEvent>);
static_assert(std::is_trivially_copyable_v<SessionReleaseSnapshot>);
static_assert(std::is_standard_layout_v<SessionReleaseSnapshot>);

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void testOrdinaryEventsAreStrictFifo() {
  SessionReleaseState state;
  const auto first = state.tryPostOrdered(
      SessionReleaseReason::ExplicitFlush,
      SessionReleaseAction::SubmitAndWait, 4, 7);
  const auto second = state.tryPostOrdered(
      SessionReleaseReason::IndependentSubmission,
      SessionReleaseAction::SubmitSession, 5, 8);
  check(first.accepted() && second.accepted() &&
            first.snapshot.event.ordinal < second.snapshot.event.ordinal,
        "ordinary events receive increasing identities");
  check(state.peekNext() == first.snapshot,
        "ordinary FIFO exposes its oldest event first");
  check(state.acknowledge(second.snapshot,
                          SessionReleaseCompletion::SessionSubmitted,
                          5, 8) == SessionReleaseAckResult::NotNext,
        "a younger ordinary event cannot acknowledge out of order");
  check(state.acknowledge(first.snapshot,
                          SessionReleaseCompletion::SessionSubmitted,
                          4, 7) == SessionReleaseAckResult::Acknowledged,
        "oldest ordinary event acknowledges after its fence action");
  check(state.peekNext() == second.snapshot,
        "acknowledging the head reveals the next ordinary event");
}

void testOrdinaryFifoFullDoesNotOverwrite() {
  SessionReleaseState state;
  SessionReleaseSnapshot first{};
  for (std::size_t i = 0; i < SessionReleaseState::orderedCapacity(); ++i) {
    const auto posted = state.tryPostOrdered(
        SessionReleaseReason::ExplicitFlush,
        SessionReleaseAction::SubmitSession,
        static_cast<std::uint64_t>(i + 1u),
        static_cast<std::uint64_t>(i + 1u));
    check(posted.status == SessionReleasePostStatus::Posted,
          "every fixed FIFO cell accepts exactly one ordinary event");
    if (i == 0) {
      first = posted.snapshot;
    }
  }
  check(state.orderedFull(), "ordinary FIFO reports its hard capacity");
  const auto rejected = state.tryPostOrdered(
      SessionReleaseReason::ExplicitFlush,
      SessionReleaseAction::SubmitSession,
      SessionReleaseState::orderedCapacity() + 1u,
      SessionReleaseState::orderedCapacity() + 1u);
  check(rejected.status == SessionReleasePostStatus::Full,
        "a full ordinary FIFO rejects instead of dropping an event");
  check(state.peekNext() == first,
        "full rejection cannot overwrite the oldest event");
}

void testOrdinaryFenceAndActionMustBeCovered() {
  SessionReleaseState state;
  const auto posted = state.tryPostOrdered(
      SessionReleaseReason::ExplicitFlush,
      SessionReleaseAction::SubmitAndWait, 9, 12);
  check(posted.accepted(), "fenced event posts");
  check(state.acknowledge(posted.snapshot,
                          SessionReleaseCompletion::SessionSubmitted,
                          8, 12) ==
            SessionReleaseAckResult::FenceNotCovered,
        "raw fence cannot be acknowledged early");
  check(state.acknowledge(posted.snapshot,
                          SessionReleaseCompletion::SessionSubmitted,
                          9, 11) ==
            SessionReleaseAckResult::FenceNotCovered,
        "sequence fence cannot be acknowledged early");
  check(state.acknowledge(posted.snapshot,
                          SessionReleaseCompletion::PassClosed,
                          9, 12) ==
            SessionReleaseAckResult::ActionNotCompleted,
        "pass close cannot acknowledge a submit action");
  check(state.acknowledge(posted.snapshot,
                          SessionReleaseCompletion::SessionSubmitted,
                          9, 12) == SessionReleaseAckResult::Acknowledged,
        "SubmitAndWait acknowledges at submit rather than GPU completion");
}

void testOrderedControlMayOwnForwardRawFenceAfterSequenceCoverage() {
  const SessionReleaseEvent event{
      .ordinal = 1,
      .reason = SessionReleaseReason::DirectObservation,
      .action = SessionReleaseAction::SubmitAndWait,
      .fenceRawOrdinal = 9,
      .fenceSeqId = 4,
  };
  const SessionReleaseSnapshot ordered{
      .origin = SessionReleaseOrigin::Ordered,
      .event = event,
      .generation = 1,
  };
  check(!sessionReleaseActionReady(ordered, 8, 3),
        "ordered control cannot act before its older sequence prefix");
  check(sessionReleaseActionReady(ordered, 8, 4),
        "sequence coverage makes the ordered-control action runnable");
  check(!sessionReleaseFenceCovered(event, 8, 4),
        "action readiness does not fabricate final raw acknowledgement");
  check(sessionReleaseFenceCovered(event, 9, 4),
        "the full acknowledgement still requires the control raw identity");
  const SessionReleaseSnapshot terminal{
      .origin = SessionReleaseOrigin::Terminal,
      .event = event,
      .generation = 1,
  };
  check(!sessionReleaseActionReady(terminal, 8, 4) &&
            sessionReleaseActionReady(terminal, 9, 4),
        "terminal action readiness retains the complete raw and sequence fence");
}

void testRegressingFenceIsRejected() {
  SessionReleaseState state;
  check(state.tryPostOrdered(
            SessionReleaseReason::ExplicitFlush,
            SessionReleaseAction::SubmitSession, 10, 20).accepted(),
        "initial monotone fence posts");
  check(state.tryPostOrdered(
            SessionReleaseReason::ExplicitFlush,
            SessionReleaseAction::SubmitSession, 9, 21).status ==
            SessionReleasePostStatus::Invalid,
        "raw fence regression is rejected");
  check(state.tryPostOrdered(
            SessionReleaseReason::SessionCap,
            SessionReleaseAction::SubmitSession, 11, 19).status ==
            SessionReleasePostStatus::Invalid,
        "sequence fence regression is rejected for fixed-cap events");
}

void testTerminalLatchIsStickyAndOrdered() {
  SessionReleaseState state;
  const auto ordinary = state.tryPostOrdered(
      SessionReleaseReason::ExplicitFlush,
      SessionReleaseAction::SubmitSession, 1, 1);
  const auto shutdown = state.requestTerminal(
      SessionReleaseReason::Shutdown, 4, 4);
  check(ordinary.accepted() && shutdown.accepted() &&
            state.terminalRequested() && state.terminalPending(),
        "terminal request installs a sticky fixed latch");
  check(state.peekNext() == ordinary.snapshot,
        "terminal request cannot overtake an older ordinary event");
  const auto deviceLoss = state.requestTerminal(
      SessionReleaseReason::DeviceLoss, 6, 6);
  check(deviceLoss.status == SessionReleasePostStatus::Advanced &&
            deviceLoss.snapshot.event.ordinal ==
                shutdown.snapshot.event.ordinal &&
            deviceLoss.snapshot.event.reason ==
                SessionReleaseReason::DeviceLoss,
        "device loss advances and upgrades the active terminal latch");
  check(state.tryPostOrdered(
            SessionReleaseReason::ExplicitFlush,
            SessionReleaseAction::SubmitSession, 7, 7).status ==
            SessionReleasePostStatus::Stopped,
        "terminal request permanently closes ordinary event admission");
  check(state.acknowledge(ordinary.snapshot,
                          SessionReleaseCompletion::SessionSubmitted,
                          1, 1) == SessionReleaseAckResult::Acknowledged,
        "older ordinary event remains acknowledgeable during shutdown");
  check(state.peekNext() == deviceLoss.snapshot,
        "advanced terminal generation becomes the next event");
  check(state.acknowledge(shutdown.snapshot,
                          SessionReleaseCompletion::SessionSubmitted,
                          6, 6) == SessionReleaseAckResult::Stale,
        "stale shutdown generation cannot clear upgraded device loss");
  check(state.acknowledge(deviceLoss.snapshot,
                          SessionReleaseCompletion::SessionSubmitted,
                          6, 6) == SessionReleaseAckResult::Acknowledged,
        "current terminal generation acknowledges after its fence submit");
  check(!state.hasPending() && state.terminalRequested() &&
            !state.terminalPending(),
        "terminal acknowledgement drains the latch but keeps admission closed");
}

void testTerminalRequestsClampUnknownAndRegressedFences() {
  SessionReleaseState state;
  const auto ordinary = state.tryPostOrdered(
      SessionReleaseReason::ExplicitFlush,
      SessionReleaseAction::SubmitSession, 10, 20);
  const auto shutdown = state.requestTerminal(
      SessionReleaseReason::Shutdown, 0, 0);
  check(ordinary.accepted() && shutdown.accepted() &&
            shutdown.snapshot.event.fenceRawOrdinal == 10 &&
            shutdown.snapshot.event.fenceSeqId == 20,
        "unknown terminal fences inherit the global ordered waterline");
  const auto deviceLoss = state.requestTerminal(
      SessionReleaseReason::DeviceLoss, 3, 4);
  check(deviceLoss.status == SessionReleasePostStatus::Advanced &&
            deviceLoss.snapshot.event.reason ==
                SessionReleaseReason::DeviceLoss &&
            deviceLoss.snapshot.event.fenceRawOrdinal == 10 &&
            deviceLoss.snapshot.event.fenceSeqId == 20,
        "regressed DeviceLoss fences are clamped and upgrade Shutdown");
  const auto laterShutdown = state.requestTerminal(
      SessionReleaseReason::Shutdown, 0, 0);
  check(laterShutdown.status == SessionReleasePostStatus::Advanced &&
            laterShutdown.snapshot.event.reason ==
                SessionReleaseReason::DeviceLoss,
        "a later Shutdown cannot downgrade sticky DeviceLoss");
  check(state.acknowledge(ordinary.snapshot,
                          SessionReleaseCompletion::SessionSubmitted,
                          10, 20) == SessionReleaseAckResult::Acknowledged &&
            state.acknowledge(laterShutdown.snapshot,
                              SessionReleaseCompletion::SessionSubmitted,
                              10, 20) ==
                SessionReleaseAckResult::Acknowledged,
        "clamped terminal generation acknowledges at the inherited fence");
}

}  // namespace

int main() {
  try {
    testOrdinaryEventsAreStrictFifo();
    testOrdinaryFifoFullDoesNotOverwrite();
    testOrdinaryFenceAndActionMustBeCovered();
    testOrderedControlMayOwnForwardRawFenceAfterSequenceCoverage();
    testRegressingFenceIsRejected();
    testTerminalLatchIsStickyAndOrdered();
    testTerminalRequestsClampUnknownAndRegressedFences();
  } catch (const std::exception& error) {
    std::cerr << "session_release_event_spec: FAIL: " << error.what()
              << '\n';
    return 1;
  }
  std::cout << "session_release_event_spec: PASS\n";
  return 0;
}
