// Pure-data spec for the present-boundary policy resolver declared in
// dxmt9_presenter.hpp. Locks in the priority ordering
// (Disabled > DeferredPresentCompletion > PresentCompletion >
// Completion > AfterAcquire > Default)
// and the "0" / empty-string / null treatment of the four
// DXMT9_PRESENT_BOUNDARY_* / DXMT9_DISABLE_PRESENT_BOUNDARY env strings.
//
// The PresentCompletion env-var is special: a null / empty string
// counts as set (matches the historical default-true behavior of the
// legacy presentBoundaryWaitsForPresentCompletion lambda); only an
// explicit "0" demotes us to the lower-priority branches.

#include "../../../src/dxmt9/dxmt9_presenter.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::BoundaryPolicy;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

BoundaryPolicy resolve(const char* disableEnv,
                       const char* deferredEnv,
                       const char* presentCompletionEnv,
                       const char* completionEnv,
                       const char* afterAcquireEnv) {
  return dxmt9::resolveBoundaryPolicy(disableEnv, deferredEnv,
                                      presentCompletionEnv, completionEnv,
                                      afterAcquireEnv);
}

// Helper — explicitly opt out of the PresentCompletion default so the
// lower-priority branches can be exercised.
constexpr const char* kPresentCompletionOff = "0";

void testAllUnsetFallsBackToPresentCompletion() {
  // Historical default-true semantics: when nothing is set the
  // boundary still waits on presentCompletedSeqId_.
  check(resolve(nullptr, nullptr, nullptr, nullptr, nullptr) == BoundaryPolicy::PresentCompletion,
        "all-unset env resolves to PresentCompletion (default-true)");
}

void testEmptyStringPresentCompletionStaysOn() {
  // Empty string for PresentCompletion env is "not set" -> default true.
  check(resolve(nullptr, nullptr, "", nullptr, nullptr) == BoundaryPolicy::PresentCompletion,
        "empty-string PresentCompletion env stays default-on");
}

void testExplicitZeroPresentCompletionDemotes() {
  // Explicit "0" opts out of PresentCompletion; falls through to Default.
  check(resolve(nullptr, nullptr, kPresentCompletionOff, nullptr, nullptr) == BoundaryPolicy::Default,
        "PRESENT_BOUNDARY_PRESENT_COMPLETION=0 with no other env falls through to Default");
}

void testDisableEnvWinsOverEverything() {
  check(resolve("1", nullptr, nullptr, nullptr, nullptr) == BoundaryPolicy::Disabled,
        "DXMT9_DISABLE_PRESENT_BOUNDARY=1 alone resolves to Disabled");
  check(resolve("1", "1", "1", "1", "1") == BoundaryPolicy::Disabled,
        "DXMT9_DISABLE_PRESENT_BOUNDARY=1 outranks all other boundary env-vars");
  check(resolve("1", "1", kPresentCompletionOff, "1", "1") == BoundaryPolicy::Disabled,
        "Disabled wins even when PresentCompletion is explicitly off and others are on");
}

void testDeferredWinsOverDefaultPresentCompletion() {
  check(resolve(nullptr, "1", nullptr, nullptr, nullptr) ==
            BoundaryPolicy::DeferredPresentCompletion,
        "Deferred present boundary outranks default PresentCompletion");
  check(resolve(nullptr, "1", "1", "1", "1") ==
            BoundaryPolicy::DeferredPresentCompletion,
        "Deferred present boundary outranks all non-disabled boundary modes");
}

void testPresentCompletionWinsOverCompletion() {
  // PresentCompletion default-on swallows Completion / AfterAcquire
  // alike when nothing higher-priority is set.
  check(resolve(nullptr, nullptr, "1", "1", nullptr) == BoundaryPolicy::PresentCompletion,
        "PresentCompletion=1 outranks Completion=1");
}

void testPresentCompletionWinsOverAfterAcquire() {
  check(resolve(nullptr, nullptr, "1", nullptr, "1") == BoundaryPolicy::PresentCompletion,
        "PresentCompletion=1 outranks AfterAcquire=1");
}

void testCompletionSelected() {
  // Explicit PresentCompletion=0 + Completion=1 => Completion.
  check(resolve(nullptr, nullptr, kPresentCompletionOff, "1", nullptr) == BoundaryPolicy::Completion,
        "PRESENT_COMPLETION=0 + COMPLETION=1 selects Completion");
}

void testCompletionWinsOverAfterAcquire() {
  check(resolve(nullptr, nullptr, kPresentCompletionOff, "1", "1") == BoundaryPolicy::Completion,
        "Completion outranks AfterAcquire when PresentCompletion is opted out");
}

void testAfterAcquireSelected() {
  // PresentCompletion=0 + Completion empty + AfterAcquire=1.
  check(resolve(nullptr, nullptr, kPresentCompletionOff, nullptr, "1") == BoundaryPolicy::AfterAcquire,
        "PRESENT_COMPLETION=0 + AFTER_ACQUIRE=1 selects AfterAcquire");
  check(resolve(nullptr, nullptr, kPresentCompletionOff, "0", "1") == BoundaryPolicy::AfterAcquire,
        "COMPLETION=0 (explicit) does not block AfterAcquire");
}

void testDefaultFallsThroughWhenAllExplicitlyOff() {
  check(resolve("0", "0", kPresentCompletionOff, "0", "0") == BoundaryPolicy::Default,
        "all env-vars explicit \"0\" resolve to Default");
  check(resolve("0", "", kPresentCompletionOff, "", "") == BoundaryPolicy::Default,
        "empty-string Completion / AfterAcquire stay off");
}

void testDisableLiteralZeroIsUnset() {
  // Literal "0" for DXMT9_DISABLE_PRESENT_BOUNDARY counts as "not set"
  // — boundary remains active and the default PresentCompletion wins.
  check(resolve("0", nullptr, nullptr, nullptr, nullptr) == BoundaryPolicy::PresentCompletion,
        "DXMT9_DISABLE_PRESENT_BOUNDARY=0 leaves boundary on (default PresentCompletion)");
}

void testNonZeroStringsCountAsSet() {
  // "yes" / "true" / "on" should also activate each boundary flag
  // (anything non-empty and not exactly "0"). The PresentCompletion
  // env follows the same rule for the explicit-on case.
  check(resolve("yes", kPresentCompletionOff, kPresentCompletionOff, nullptr, nullptr) == BoundaryPolicy::Disabled,
        "non-zero string activates Disabled");
  check(resolve(nullptr, "yes", kPresentCompletionOff, nullptr, nullptr) ==
            BoundaryPolicy::DeferredPresentCompletion,
        "non-zero string activates Deferred");
  check(resolve(nullptr, nullptr, "true", nullptr, nullptr) == BoundaryPolicy::PresentCompletion,
        "non-zero string keeps PresentCompletion on");
  check(resolve(nullptr, nullptr, kPresentCompletionOff, "yes", nullptr) == BoundaryPolicy::Completion,
        "non-zero string activates Completion");
  check(resolve(nullptr, nullptr, kPresentCompletionOff, nullptr, "on") == BoundaryPolicy::AfterAcquire,
        "non-zero string activates AfterAcquire");
}

}  // namespace

int main() {
  try {
    testAllUnsetFallsBackToPresentCompletion();
    testEmptyStringPresentCompletionStaysOn();
    testExplicitZeroPresentCompletionDemotes();
    testDisableEnvWinsOverEverything();
    testDeferredWinsOverDefaultPresentCompletion();
    testPresentCompletionWinsOverCompletion();
    testPresentCompletionWinsOverAfterAcquire();
    testCompletionSelected();
    testCompletionWinsOverAfterAcquire();
    testAfterAcquireSelected();
    testDefaultFallsThroughWhenAllExplicitlyOff();
    testDisableLiteralZeroIsUnset();
    testNonZeroStringsCountAsSet();
  } catch (const TestFailure& failure) {
    std::cerr << "present_boundary_policy_spec failed: "
              << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "present_boundary_policy_spec unexpected exception: "
              << ex.what() << '\n';
    return 1;
  }
  std::cout << "present_boundary_policy_spec passed\n";
  return 0;
}
