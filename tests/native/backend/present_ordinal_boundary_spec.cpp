// Pure spec for dxmt9::presentOrdinalBoundaryTarget — the app-side ordinal
// frame-latency target used by the commit-replay offload path — and for
// dxmt9::planPresentOrdinalWait, the pure policy/wait-target mapping that
// CommandQueue::waitPresentOrdinalBoundary delegates to.
#include "../../../src/dxmt9/dxmt9_command_queue.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

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
  } catch (const TestFailure& e) {
    std::cerr << "present_ordinal_boundary_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "present_ordinal_boundary_spec passed\n";
  return 0;
}
