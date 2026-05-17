// Pure-data spec for the present-drawable acquire policy resolver
// declared in dxmt9_presenter.hpp. Locks in the priority ordering
// (Async > SyncOnSubmit > PreAcquire > Sync) and the "0" / empty-string
// treatment of the three DXMT9_PRESENT_* env strings.

#include "../../../src/dxmt9/dxmt9_presenter.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::AcquirePolicy;

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

AcquirePolicy resolve(const char* asyncEnv,
                      const char* onSubmitEnv,
                      const char* preAcquireEnv) {
  return dxmt9::resolveAcquirePolicy(asyncEnv, onSubmitEnv, preAcquireEnv);
}

void testAllUnsetFallsBackToSync() {
  check(resolve(nullptr, nullptr, nullptr) == AcquirePolicy::Sync,
        "all-unset env resolves to Sync");
}

void testEmptyStringAndZeroCountAsUnset() {
  check(resolve("", "", "") == AcquirePolicy::Sync,
        "empty-string env values resolve to Sync");
  check(resolve("0", "0", "0") == AcquirePolicy::Sync,
        "literal \"0\" env values resolve to Sync");
}

void testAsyncEnvSelectsAsync() {
  check(resolve("1", nullptr, nullptr) == AcquirePolicy::Async,
        "DXMT9_PRESENT_ASYNC_ACQUIRE=1 selects Async");
}

void testOnSubmitEnvSelectsSyncOnSubmit() {
  check(resolve(nullptr, "1", nullptr) == AcquirePolicy::SyncOnSubmit,
        "DXMT9_PRESENT_ACQUIRE_ON_SUBMIT=1 selects SyncOnSubmit");
}

void testPreAcquireEnvSelectsPreAcquire() {
  check(resolve(nullptr, nullptr, "1") == AcquirePolicy::PreAcquire,
        "DXMT9_PRESENT_PREACQUIRE=1 selects PreAcquire");
}

void testAsyncWinsOverSyncOnSubmit() {
  check(resolve("1", "1", nullptr) == AcquirePolicy::Async,
        "Async outranks SyncOnSubmit when both set");
}

void testAsyncWinsOverPreAcquire() {
  check(resolve("1", nullptr, "1") == AcquirePolicy::Async,
        "Async outranks PreAcquire when both set");
}

void testSyncOnSubmitWinsOverPreAcquire() {
  check(resolve(nullptr, "1", "1") == AcquirePolicy::SyncOnSubmit,
        "SyncOnSubmit outranks PreAcquire when both set");
}

void testAllThreeSetPicksAsync() {
  check(resolve("1", "1", "1") == AcquirePolicy::Async,
        "all-three set still resolves to Async");
}

void testNonZeroStringsCountAsSet() {
  // "yes" / "true" / "on" should also activate the flag (anything
  // non-empty and not exactly "0").
  check(resolve("yes", nullptr, nullptr) == AcquirePolicy::Async,
        "non-zero string activates Async");
  check(resolve(nullptr, "true", nullptr) == AcquirePolicy::SyncOnSubmit,
        "non-zero string activates SyncOnSubmit");
  check(resolve(nullptr, nullptr, "on") == AcquirePolicy::PreAcquire,
        "non-zero string activates PreAcquire");
}

}  // namespace

int main() {
  try {
    testAllUnsetFallsBackToSync();
    testEmptyStringAndZeroCountAsUnset();
    testAsyncEnvSelectsAsync();
    testOnSubmitEnvSelectsSyncOnSubmit();
    testPreAcquireEnvSelectsPreAcquire();
    testAsyncWinsOverSyncOnSubmit();
    testAsyncWinsOverPreAcquire();
    testSyncOnSubmitWinsOverPreAcquire();
    testAllThreeSetPicksAsync();
    testNonZeroStringsCountAsSet();
  } catch (const TestFailure& failure) {
    std::cerr << "present_acquire_policy_spec failed: "
              << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "present_acquire_policy_spec unexpected exception: "
              << ex.what() << '\n';
    return 1;
  }
  std::cout << "present_acquire_policy_spec passed\n";
  return 0;
}
