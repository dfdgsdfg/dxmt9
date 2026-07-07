// Pure spec for dxmt9::presentOrdinalBoundaryTarget — the app-side ordinal
// frame-latency target used by the commit-replay offload path — and for
// dxmt9::planPresentOrdinalWait, the pure policy/wait-target mapping that
// CommandQueue::waitPresentOrdinalBoundary delegates to. Also covers
// dxmt9::PresentOrdinalGate — the extracted cv/flag mechanics consulted by
// CommandQueue::waitPresentOrdinalBoundary / abortPresentOrdinalWaits (gap.md
// offload row: no direct test previously existed for the abort mechanics).
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
