// Pure spec for dxmt9::presentOrdinalBoundaryTarget — the app-side ordinal
// frame-latency target used by the commit-replay offload path — and for
// dxmt9::planPresentOrdinalWait, the pure policy/wait-target mapping that
// CommandQueue::waitPresentOrdinalBoundary delegates to. Also covers
// dxmt9::PresentOrdinalGate — the extracted cv/flag mechanics consulted by
// CommandQueue::waitPresentOrdinalBoundary / abortPresentOrdinalWaits (gap.md
// offload row: no direct test previously existed for the abort mechanics).
//
// R-BACK-2.51: also covers dxmt9::backBufferLatencyCap /
// dxmt9::cappedFrameLatency / dxmt9::resolvedPresentFrameLatency (the
// DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS math and Immediate-present default
// CommandQueue::waitPresentOrdinalBoundary applies before planning the wait,
// mirroring presentBoundaryLatency() on the inline boundary) and
// dxmt9::resolvePresentBoundaryAction (the per-present truth table
// CommandQueue::submitPresent uses to decide whether a given present's
// core::SwapDesc::pacedByPresentOrdinal flag means it should skip the inline
// boundary, replacing the old process-global offloadCommitReplayEnabled()
// check that incorrectly suppressed the boundary for every present in the
// process).
#include "../../../src/dxmt9/dxmt9_command_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };
void check(bool c, std::string_view m) { if (!c) throw TestFailure(std::string(m)); }

void testOrdinalTargetMath() {
  using dxmt9::kMaxQueuedChunks;
  using dxmt9::presentOrdinalBoundaryTarget;
  check(presentOrdinalBoundaryTarget(0, 3) == 0, "ordinal 0 never waits");
  check(presentOrdinalBoundaryTarget(3, 3) == 0, "inside latency window");
  check(presentOrdinalBoundaryTarget(4, 3) == 1, "first past-window waits on 1");
  check(presentOrdinalBoundaryTarget(2, 0) == 1, "latency clamps up to 1");
  check(presentOrdinalBoundaryTarget(1000, 64) ==
            1000 - static_cast<std::uint64_t>(kMaxQueuedChunks),
        "latency clamps down to kMaxQueuedChunks");
}

void testOrdinalTargetMatchesSeqIdShape() {
  using dxmt9::presentOrdinalBoundaryTarget;
  // The ordinal math must be the exact shape of the boundary seqId math so
  // the offload pacing is order-isomorphic to the inline boundary.
  for (std::uint64_t n : {1ull, 2ull, 5ull, 31ull, 32ull, 100ull}) {
    const auto t = presentOrdinalBoundaryTarget(n, 3);
    check(t == (n <= 3 ? 0 : n - 3), "target follows n - latency with floor");
  }
}

void testPlannerDisabledNeverWaitsAndPreservesStored() {
  using dxmt9::BoundaryPolicy;
  using dxmt9::planPresentOrdinalWait;
  const auto plan = planPresentOrdinalWait(BoundaryPolicy::Disabled, 100, 1, 42);
  check(plan.waitTargetOrdinal == 0, "Disabled never returns a wait target");
  check(plan.storedDeferredTarget == 42,
        "Disabled preserves the previously stored deferred target");
}

void testPlannerDefaultPolicyUsesOrdinalTargetAndPreservesStored() {
  using dxmt9::BoundaryPolicy;
  using dxmt9::planPresentOrdinalWait;
  using dxmt9::presentOrdinalBoundaryTarget;
  // Any non-Disabled, non-Deferred policy (PresentCompletion, Completion,
  // Default, AfterAcquire) collapses to the same direct-target branch.
  for (BoundaryPolicy policy : {BoundaryPolicy::Default, BoundaryPolicy::AfterAcquire,
                                BoundaryPolicy::Completion, BoundaryPolicy::PresentCompletion}) {
    const auto plan = planPresentOrdinalWait(policy, 10, 1, 7);
    check(plan.waitTargetOrdinal == presentOrdinalBoundaryTarget(10, 1),
          "direct policy wait target follows presentOrdinalBoundaryTarget(ordinal, latency)");
    check(plan.storedDeferredTarget == 7,
          "direct policy leaves the stored deferred target untouched");
  }
}

void testPlannerDeferredFirstCallNoWaitStoresNextTarget() {
  using dxmt9::BoundaryPolicy;
  using dxmt9::planPresentOrdinalWait;
  using dxmt9::presentOrdinalBoundaryTarget;
  // latency=1: first call with ordinal=1 and stored=0 -> no wait yet
  // (stored target was 0), and the new stored target becomes
  // presentOrdinalBoundaryTarget(ordinal+1=2, latency=1) == 1.
  const auto plan = planPresentOrdinalWait(BoundaryPolicy::DeferredPresentCompletion, 1, 1, 0);
  check(plan.waitTargetOrdinal == 0, "Deferred returns the previously stored target as the wait target");
  check(plan.storedDeferredTarget == presentOrdinalBoundaryTarget(2, 1),
        "Deferred stores target(ordinal+1, latency)");
  check(plan.storedDeferredTarget == 1, "sanity: target(2,1) == 1");
}

void testPlannerDeferredWaitsOnPreviouslyStoredTarget() {
  using dxmt9::BoundaryPolicy;
  using dxmt9::planPresentOrdinalWait;
  using dxmt9::presentOrdinalBoundaryTarget;
  // Second call: stored=1 (from the previous call) becomes the wait target
  // NOW, and the stored target advances again via monotonic max retention.
  const auto plan = planPresentOrdinalWait(BoundaryPolicy::DeferredPresentCompletion, 2, 1, 1);
  check(plan.waitTargetOrdinal == 1,
        "Deferred waits on the target stored by the previous call");
  check(plan.storedDeferredTarget == std::max<std::uint64_t>(1, presentOrdinalBoundaryTarget(3, 1)),
        "Deferred retains max(old stored, target(ordinal+1, latency))");
  check(plan.storedDeferredTarget == 2, "sanity: max(1, target(3,1)=2) == 2");
}

void testPlannerDeferredMonotonicMaxRetention() {
  using dxmt9::BoundaryPolicy;
  using dxmt9::planPresentOrdinalWait;
  // A stored target must never regress even if a later call's computed
  // next-target would be smaller (e.g. a large latency clamp shrinking the
  // delta) — the planner must retain the max of old vs. new.
  const auto plan = planPresentOrdinalWait(BoundaryPolicy::DeferredPresentCompletion,
                                            /*presentOrdinal=*/1, /*maxFrameLatency=*/1,
                                            /*storedDeferredTarget=*/1000);
  check(plan.storedDeferredTarget == 1000,
        "monotonic max retention: a small next-target does not regress a larger stored target");
  check(plan.waitTargetOrdinal == 1000,
        "the wait target is exactly the previously stored (larger) value");
}

void testPlannerDeferredUint64MaxSaturates() {
  using dxmt9::BoundaryPolicy;
  using dxmt9::planPresentOrdinalWait;
  using dxmt9::presentOrdinalBoundaryTarget;
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  // presentOrdinal+1 would overflow to 0; the planner must special-case
  // kMax so it computes target(kMax, latency) instead of wrapping.
  const auto plan = planPresentOrdinalWait(BoundaryPolicy::DeferredPresentCompletion, kMax, 1, 0);
  check(plan.storedDeferredTarget == presentOrdinalBoundaryTarget(kMax, 1),
        "u64-max ordinal does not overflow into presentOrdinalBoundaryTarget(0, latency)");
  check(plan.storedDeferredTarget != 0,
        "sanity: target(kMax, latency=1) is a large nonzero value, not the overflow-wrapped 0 case");
}

// --- Capped present-ordinal frame latency (R-BACK-2.51(h) cap-honoring) -----
//
// CommandQueue::waitPresentOrdinalBoundary now folds backBufferCount into
// its effective latency via these same two pure helpers before calling
// planPresentOrdinalWait, instead of using the raw maxFrameLatency
// unconditionally. This is the identical cap math presentBoundaryLatency()
// applies to the inline seqId-based boundary, so both boundary mechanisms
// stay latency-isomorphic for the same inputs.

void testBackBufferLatencyCapMath() {
  using dxmt9::backBufferLatencyCap;
  using dxmt9::core::kMaxFrameLatency;
  check(backBufferLatencyCap(0) == 2, "backBufferCount=0 normalizes to 1, cap = 1+1 = 2");
  check(backBufferLatencyCap(1) == 2, "backBufferCount=1 -> cap = 1+1 = 2");
  check(backBufferLatencyCap(2) == 3, "backBufferCount=2 -> cap = 2+1 = 3");
  check(backBufferLatencyCap(static_cast<std::uint32_t>(kMaxFrameLatency)) ==
            static_cast<std::uint32_t>(kMaxFrameLatency),
        "backBufferCount >= kMaxFrameLatency clamps to kMaxFrameLatency");
  check(backBufferLatencyCap(static_cast<std::uint32_t>(kMaxFrameLatency) + 10) ==
            static_cast<std::uint32_t>(kMaxFrameLatency),
        "backBufferCount far past kMaxFrameLatency still clamps to kMaxFrameLatency");
}

void testCappedFrameLatencyHonorsEnabledBit() {
  using dxmt9::cappedFrameLatency;
  check(cappedFrameLatency(30, 1, /*capEnabled=*/false) == 30,
        "cap disabled returns maxFrameLatency unchanged regardless of backBufferCount");
  check(cappedFrameLatency(30, 1, /*capEnabled=*/true) == 2,
        "cap enabled clamps a larger maxFrameLatency down to backBufferLatencyCap(1) == 2");
  check(cappedFrameLatency(1, 8, /*capEnabled=*/true) == 1,
        "cap enabled never raises a smaller maxFrameLatency up to the cap");
  check(cappedFrameLatency(0, 0, /*capEnabled=*/true) == 0,
        "cap enabled with maxFrameLatency=0 stays 0 (min() floor, not the cap)");
}

void testImmediateDefaultFrameLatencyResolution() {
  using dxmt9::effectivePresentFrameLatency;
  using dxmt9::core::kDefaultFrameLatency;
  check(effectivePresentFrameLatency(kDefaultFrameLatency,
                                     /*displaySyncEnabled=*/false) == 1,
        "Immediate present with engine-default maximum resolves to one frame");
  check(effectivePresentFrameLatency(kDefaultFrameLatency,
                                     /*displaySyncEnabled=*/true) ==
            kDefaultFrameLatency,
        "synchronized present retains the engine-default maximum");
  check(effectivePresentFrameLatency(2, /*displaySyncEnabled=*/false) == 2,
        "non-default Immediate maximum remains an explicit wider window");
  check(effectivePresentFrameLatency(1, /*displaySyncEnabled=*/false) == 1,
        "explicit one-frame Immediate maximum remains one");
}

void testResolvedPresentFrameLatencyComposition() {
  using dxmt9::resolvedPresentFrameLatency;
  using dxmt9::core::kDefaultFrameLatency;
  check(resolvedPresentFrameLatency(
            kDefaultFrameLatency, 1, /*displaySyncEnabled=*/false,
            /*capEnabled=*/false) == 1,
        "Immediate default resolves to one without the back-buffer cap");
  check(resolvedPresentFrameLatency(
            kDefaultFrameLatency, 1, /*displaySyncEnabled=*/false,
            /*capEnabled=*/true) == 1,
        "back-buffer cap never raises the Immediate one-frame default");
  check(resolvedPresentFrameLatency(
            kDefaultFrameLatency, 1, /*displaySyncEnabled=*/true,
            /*capEnabled=*/true) == 2,
        "synchronized default still composes with the one-backbuffer cap");
  check(resolvedPresentFrameLatency(
            3, 1, /*displaySyncEnabled=*/false,
            /*capEnabled=*/true) == 2,
        "non-default Immediate window still composes with the back-buffer cap");
}

// --- Per-present boundary action (R-BACK-2.51(g) truth table) --------
//
// CommandQueue::submitPresent's inline-vs-skip-vs-defer decision is now this
// pure function of (this present's pacedByPresentOrdinal flag, the resolved
// BoundaryPolicy) instead of a process-global offloadCommitReplayEnabled()
// check. A present paced by the commit-replay offload's present-ordinal
// boundary always skips the inline boundary; every other present -- direct
// COM callers, or presents replayed by the synchronous non-offload
// chunk-replay path -- keeps participating in the inline/deferred boundary
// exactly as it did before the offload path existed, regardless of whether
// DXMT9_OFFLOAD_COMMIT_REPLAY happens to be globally enabled for other,
// paced presents in the same process (this truth table takes no global-flag
// input at all, by construction).

void testResolvePresentBoundaryActionPacedAlwaysSkipsRegardlessOfPolicy() {
  using dxmt9::BoundaryPolicy;
  using dxmt9::PresentBoundaryAction;
  using dxmt9::resolvePresentBoundaryAction;
  for (BoundaryPolicy policy : {BoundaryPolicy::Disabled, BoundaryPolicy::DeferredPresentCompletion,
                                BoundaryPolicy::PresentCompletion, BoundaryPolicy::Completion,
                                BoundaryPolicy::Default, BoundaryPolicy::AfterAcquire}) {
    check(resolvePresentBoundaryAction(/*pacedByPresentOrdinal=*/true, policy) ==
              PresentBoundaryAction::SkipPacedByOffloadOrdinal,
          "a present paced by the offload ordinal boundary always skips the inline boundary");
  }
}

void testResolvePresentBoundaryActionUnpacedFollowsPolicy() {
  using dxmt9::BoundaryPolicy;
  using dxmt9::PresentBoundaryAction;
  using dxmt9::resolvePresentBoundaryAction;
  check(resolvePresentBoundaryAction(false, BoundaryPolicy::Disabled) ==
            PresentBoundaryAction::SkipDisabled,
        "unpaced + Disabled -> SkipDisabled");
  check(resolvePresentBoundaryAction(false, BoundaryPolicy::DeferredPresentCompletion) ==
            PresentBoundaryAction::Defer,
        "unpaced + DeferredPresentCompletion -> Defer");
  for (BoundaryPolicy policy : {BoundaryPolicy::PresentCompletion, BoundaryPolicy::Completion,
                                BoundaryPolicy::Default, BoundaryPolicy::AfterAcquire}) {
    check(resolvePresentBoundaryAction(false, policy) == PresentBoundaryAction::ApplyInline,
          "unpaced + any non-Disabled non-Deferred policy -> ApplyInline");
  }
}

void testResolvePresentBoundaryActionUnpacedIndependentOfGlobalOffloadState() {
  // R-BACK-2.51(g) regression guard: this is the exact bug the per-present
  // fix closes. Before the fix, submitPresent branched on a process-global
  // offloadCommitReplayEnabled() check, so ANY present -- including a direct
  // COM caller that never went through the chunk-replay ordinal wait --
  // would silently lose the inline boundary whenever the flag was globally
  // on. resolvePresentBoundaryAction takes no global-flag parameter at all,
  // so an unpaced present's action cannot depend on it.
  using dxmt9::BoundaryPolicy;
  using dxmt9::PresentBoundaryAction;
  using dxmt9::resolvePresentBoundaryAction;
  check(resolvePresentBoundaryAction(/*pacedByPresentOrdinal=*/false,
                                     BoundaryPolicy::PresentCompletion) ==
            PresentBoundaryAction::ApplyInline,
        "an unpaced present applies the inline boundary regardless of any global offload state");
}

// --- PresentOrdinalGate: needsWait/waitDone truth tables ------------------

void testGateNeedsWaitTruthTable() {
  using dxmt9::PresentOrdinalGate;
  PresentOrdinalGate gate;

  // Zero target: never waits regardless of other state.
  gate.completedOrdinal = 0;
  gate.aborted = false;
  check(!gate.needsWait(0), "needsWait(0) is always false");

  // Satisfied: completedOrdinal already reached the target.
  gate.completedOrdinal = 5;
  gate.aborted = false;
  check(!gate.needsWait(5), "needsWait false when completedOrdinal == target");
  check(!gate.needsWait(4), "needsWait false when completedOrdinal > target");

  // Aborted: sticky release valve short-circuits even a pending target.
  gate.completedOrdinal = 0;
  gate.aborted = true;
  check(!gate.needsWait(10), "needsWait false once aborted, even if pending");

  // Pending: nonzero target, not aborted, not yet satisfied -> must wait.
  gate.completedOrdinal = 3;
  gate.aborted = false;
  check(gate.needsWait(10), "needsWait true for a pending, non-aborted target");
}

void testGateWaitDoneTruthTable() {
  using dxmt9::PresentOrdinalGate;
  PresentOrdinalGate gate;

  // Stopped: releases regardless of ordinal/abort state.
  gate.completedOrdinal = 0;
  gate.aborted = false;
  check(gate.waitDone(10, /*stopped=*/true), "waitDone true when stopped");

  // Aborted: releases even though the ordinal never reached target.
  gate.completedOrdinal = 0;
  gate.aborted = true;
  check(gate.waitDone(10, /*stopped=*/false), "waitDone true when aborted");

  // Satisfied: ordinal reached (or passed) target.
  gate.completedOrdinal = 10;
  gate.aborted = false;
  check(gate.waitDone(10, /*stopped=*/false), "waitDone true when completedOrdinal >= target");
  gate.completedOrdinal = 11;
  check(gate.waitDone(10, /*stopped=*/false), "waitDone true when completedOrdinal > target");

  // None of the above: still pending.
  gate.completedOrdinal = 3;
  gate.aborted = false;
  check(!gate.waitDone(10, /*stopped=*/false), "waitDone false while pending and not stopped/aborted");
}

// --- PresentOrdinalGate: real cv release mechanics ------------------------
//
// Mirrors the release-build hang fix: a waiter parked on waitDone() as its
// condition-variable predicate must be released either by advancing
// completedOrdinal past the target, or by the sticky abort path (which is
// what abortPresentOrdinalWaits() drives in production so a dead offload
// worker can never leave an app thread parked forever).

void testGateCvReleasedByAbort() {
  using dxmt9::PresentOrdinalGate;
  std::mutex mutex;
  std::condition_variable cv;
  PresentOrdinalGate gate;
  gate.completedOrdinal = 0;
  constexpr std::uint64_t kTarget = 100;

  std::atomic<bool> waiting{false};
  std::atomic<bool> released{false};
  std::thread waiter([&] {
    std::unique_lock lock(mutex);
    waiting.store(true);
    cv.wait(lock, [&] { return gate.waitDone(kTarget, /*stopped=*/false); });
    released.store(true);
  });

  while (!waiting.load()) {
    std::this_thread::yield();
  }
  // Give the waiter a chance to actually enter cv.wait() before releasing it.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!released.load(), "waiter must still be parked before abort is set");

  {
    std::lock_guard lock(mutex);
    gate.aborted = true;
  }
  cv.notify_all();
  waiter.join();
  check(released.load(), "setting aborted + notify_all releases the cv waiter");
}

void testGateCvReleasedByAdvancingCompletedOrdinal() {
  using dxmt9::PresentOrdinalGate;
  std::mutex mutex;
  std::condition_variable cv;
  PresentOrdinalGate gate;
  gate.completedOrdinal = 0;
  constexpr std::uint64_t kTarget = 100;

  std::atomic<bool> waiting{false};
  std::atomic<bool> released{false};
  std::thread waiter([&] {
    std::unique_lock lock(mutex);
    waiting.store(true);
    cv.wait(lock, [&] { return gate.waitDone(kTarget, /*stopped=*/false); });
    released.store(true);
  });

  while (!waiting.load()) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!released.load(), "waiter must still be parked before the ordinal reaches target");

  {
    std::lock_guard lock(mutex);
    gate.completedOrdinal = kTarget;
  }
  cv.notify_all();
  waiter.join();
  check(released.load(), "advancing completedOrdinal to target + notify_all releases the cv waiter");
}
}  // namespace

int main() {
  try {
    testOrdinalTargetMath();
    testOrdinalTargetMatchesSeqIdShape();
    testPlannerDisabledNeverWaitsAndPreservesStored();
    testPlannerDefaultPolicyUsesOrdinalTargetAndPreservesStored();
    testPlannerDeferredFirstCallNoWaitStoresNextTarget();
    testPlannerDeferredWaitsOnPreviouslyStoredTarget();
    testPlannerDeferredMonotonicMaxRetention();
    testPlannerDeferredUint64MaxSaturates();
    testBackBufferLatencyCapMath();
    testCappedFrameLatencyHonorsEnabledBit();
    testImmediateDefaultFrameLatencyResolution();
    testResolvedPresentFrameLatencyComposition();
    testResolvePresentBoundaryActionPacedAlwaysSkipsRegardlessOfPolicy();
    testResolvePresentBoundaryActionUnpacedFollowsPolicy();
    testResolvePresentBoundaryActionUnpacedIndependentOfGlobalOffloadState();
    testGateNeedsWaitTruthTable();
    testGateWaitDoneTruthTable();
    testGateCvReleasedByAbort();
    testGateCvReleasedByAdvancingCompletedOrdinal();
  } catch (const TestFailure& e) {
    std::cerr << "present_ordinal_boundary_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "present_ordinal_boundary_spec passed\n";
  return 0;
}
