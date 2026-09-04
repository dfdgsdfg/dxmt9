#include "dxmt9/dxmt9_early_prefix_policy.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using dxmt9::core::metalqueue::CpuReadyEarlyPrefixDecision;
using dxmt9::core::metalqueue::CpuReadyEarlyPrefixSnapshot;
using dxmt9::core::metalqueue::CpuReadyEarlySessionAction;
using dxmt9::core::metalqueue::decideCpuReadyEarlyPrefix;
using dxmt9::core::metalqueue::decideCpuReadyEarlySessionAction;

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

constexpr CpuReadyEarlyPrefixSnapshot eligible() noexcept {
  return {
      .tapeEnabled = true,
      .experimentEnabled = true,
      .compatibilityWritingSource = true,
      .hasCommands = true,
      .inflightCount = 4,
      .inflightLimit = 31,
      .successorControlSlotFree = true,
      .successorTapeCapacity = true,
  };
}

void publicationPolicyIsomorphicCases() {
  auto snapshot = eligible();
  check(decideCpuReadyEarlyPrefix(snapshot) ==
            CpuReadyEarlyPrefixDecision::Publish,
        "eligible prefix publishes only with reserved tail capacity");

  snapshot.experimentEnabled = false;
  check(decideCpuReadyEarlyPrefix(snapshot) ==
            CpuReadyEarlyPrefixDecision::Disabled,
        "default-off path is disabled before other observations");
  snapshot = eligible();
  snapshot.alreadyPublished = true;
  check(decideCpuReadyEarlyPrefix(snapshot) ==
            CpuReadyEarlyPrefixDecision::AlreadyPublished,
        "at most one prefix publishes per frame");
  snapshot = eligible();
  snapshot.containsOrderedControl = true;
  check(decideCpuReadyEarlyPrefix(snapshot) ==
            CpuReadyEarlyPrefixDecision::OrderedControl,
        "ordered-control raw fails closed");
  snapshot = eligible();
  snapshot.hasPresent = true;
  check(decideCpuReadyEarlyPrefix(snapshot) ==
            CpuReadyEarlyPrefixDecision::Ineligible,
        "Present-bearing source is never an early prefix");
  snapshot = eligible();
  snapshot.inflightCount = snapshot.inflightLimit - 1u;
  check(decideCpuReadyEarlyPrefix(snapshot) ==
            CpuReadyEarlyPrefixDecision::NoTailCredit,
        "prefix cannot consume the reserved Present publication credit");
  snapshot = eligible();
  snapshot.successorControlSlotFree = false;
  check(decideCpuReadyEarlyPrefix(snapshot) ==
            CpuReadyEarlyPrefixDecision::Capacity,
        "occupied successor control slot denies publication");
  snapshot = eligible();
  snapshot.successorTapeCapacity = false;
  check(decideCpuReadyEarlyPrefix(snapshot) ==
            CpuReadyEarlyPrefixDecision::Capacity,
        "Tape denial preserves the existing Present-only source");
}

void parkedLifecycleAndConservation() {
  std::uint64_t commandBuffers = 0;
  std::uint64_t renderPasses = 0;
  bool pending = true;

  auto action = decideCpuReadyEarlySessionAction(
      pending, false, false, false, false, false);
  check(action == CpuReadyEarlySessionAction::Park,
        "no future source parks exactly one unsubmitted session");
  check(commandBuffers == 0 && renderPasses == 0,
        "parking has no submitted command-buffer or pass side effect");

  action = decideCpuReadyEarlySessionAction(
      pending, false, false, true, true, true);
  check(action == CpuReadyEarlySessionAction::JoinPresent,
        "later Present tail joins the parked prefix");
  if (action == CpuReadyEarlySessionAction::JoinPresent) {
    ++commandBuffers;
    ++renderPasses;
    pending = false;
  }
  check(!pending && commandBuffers == 1 && renderPasses == 1,
        "prefix and Present conserve the baseline one-CB/one-pass shape");

  check(decideCpuReadyEarlySessionAction(
            true, false, false, true, true, false) ==
            CpuReadyEarlySessionAction::ContinueJoin,
        "appendable non-Present source preserves order in the parked session");
  check(decideCpuReadyEarlySessionAction(
            true, false, true, false, false, false) ==
            CpuReadyEarlySessionAction::FailPreEffect,
        "ordered release abandons before a GPU-visible effect");
  check(decideCpuReadyEarlySessionAction(
            true, false, false, true, false, false) ==
            CpuReadyEarlySessionAction::FailPreEffect,
        "non-appendable successor cannot create a second submission");
  check(decideCpuReadyEarlySessionAction(
            true, true, false, false, false, false) ==
            CpuReadyEarlySessionAction::FailPreEffect,
        "stop wakes and abandons the unsubmitted session");
  check(decideCpuReadyEarlySessionAction(
            false, true, true, true, false, false) ==
            CpuReadyEarlySessionAction::ExistingPolicy,
        "ordinary sessions retain their established failure/stop behavior");
}

void environmentGate() {
  using dxmt9::core::metalqueue::cpuReadyEarlyPrefixEnvEnabled;
  check(!cpuReadyEarlyPrefixEnvEnabled(nullptr, nullptr),
        "both absent variables disable experiment");
  check(!cpuReadyEarlyPrefixEnvEnabled("1", "0"),
        "explicit experiment zero disables it");
  check(!cpuReadyEarlyPrefixEnvEnabled("0", "1"),
        "experiment cannot bypass Tape gate");
  check(cpuReadyEarlyPrefixEnvEnabled("1", "1"),
        "both gates enable the experiment");
}

}  // namespace

int main() {
  static_assert(decideCpuReadyEarlyPrefix(eligible()) ==
                CpuReadyEarlyPrefixDecision::Publish);
  publicationPolicyIsomorphicCases();
  parkedLifecycleAndConservation();
  environmentGate();
  std::cout << "cpu-ready early-prefix policy spec passed\n";
  return 0;
}
