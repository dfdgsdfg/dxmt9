#pragma once

#include <cstddef>
#include <cstdint>

namespace dxmt9::core::metalqueue {

// Default-off producer policy for publishing one immutable compatibility
// prefix before the frame's trailing Present source.  The decision is pure so
// native tests and the production queue use the same ordering of fail-closed
// checks.
enum class CpuReadyEarlyPrefixDecision : std::uint8_t {
  Disabled = 0,
  Publish,
  AlreadyPublished,
  NoTailCredit,
  OrderedControl,
  Ineligible,
  Capacity,
};

struct CpuReadyEarlyPrefixSnapshot {
  bool tapeEnabled = false;
  bool experimentEnabled = false;
  bool alreadyPublished = false;
  bool containsOrderedControl = false;
  bool compatibilityWritingSource = false;
  bool hasCommands = false;
  bool hasPresent = false;
  bool activeStrictSource = false;
  std::size_t inflightCount = 0;
  std::size_t inflightLimit = 0;
  bool successorControlSlotFree = false;
  bool successorTapeCapacity = false;
};

constexpr CpuReadyEarlyPrefixDecision decideCpuReadyEarlyPrefix(
    const CpuReadyEarlyPrefixSnapshot& snapshot) noexcept {
  if (!snapshot.tapeEnabled || !snapshot.experimentEnabled) {
    return CpuReadyEarlyPrefixDecision::Disabled;
  }
  if (snapshot.alreadyPublished) {
    return CpuReadyEarlyPrefixDecision::AlreadyPublished;
  }
  if (snapshot.containsOrderedControl) {
    return CpuReadyEarlyPrefixDecision::OrderedControl;
  }
  if (!snapshot.compatibilityWritingSource || !snapshot.hasCommands ||
      snapshot.hasPresent || snapshot.activeStrictSource) {
    return CpuReadyEarlyPrefixDecision::Ineligible;
  }
  // The prefix itself consumes one inflight credit.  Keeping one additional
  // credit unused proves that the later Present-bearing tail can publish even
  // if no older source completes in the meantime.
  if (snapshot.inflightLimit < 2u ||
      snapshot.inflightCount >= snapshot.inflightLimit - 1u) {
    return CpuReadyEarlyPrefixDecision::NoTailCredit;
  }
  if (!snapshot.successorControlSlotFree ||
      !snapshot.successorTapeCapacity) {
    return CpuReadyEarlyPrefixDecision::Capacity;
  }
  return CpuReadyEarlyPrefixDecision::Publish;
}

enum class CpuReadyEarlySessionAction : std::uint8_t {
  ExistingPolicy = 0,
  Park,
  ContinueJoin,
  JoinPresent,
  FailPreEffect,
};

// Once the early source has been encoded, its command buffer remains
// unsubmitted.  Only an appendable source may extend it, and only a Present
// tail may submit it.  Stop or an ordered/non-appendable boundary abandons the
// unsubmitted session and fail-stops rather than creating an extra GPU-visible
// submission.
constexpr CpuReadyEarlySessionAction decideCpuReadyEarlySessionAction(
    bool pendingEarlyPrefix, bool stopped, bool orderedRelease,
    bool hasNextSource, bool nextAppendable,
    bool nextHasFinalPresentTail) noexcept {
  if (!pendingEarlyPrefix) {
    return CpuReadyEarlySessionAction::ExistingPolicy;
  }
  if (stopped || orderedRelease ||
      (hasNextSource && !nextAppendable)) {
    return CpuReadyEarlySessionAction::FailPreEffect;
  }
  if (!hasNextSource) {
    return CpuReadyEarlySessionAction::Park;
  }
  return nextHasFinalPresentTail
      ? CpuReadyEarlySessionAction::JoinPresent
      : CpuReadyEarlySessionAction::ContinueJoin;
}

constexpr bool cpuReadyEarlyPrefixEnvEnabled(
    const char* tapeValue, const char* experimentValue) noexcept {
  const auto enabled = [](const char* value) constexpr noexcept {
    return value && value[0] != '\0' && value[0] != '0';
  };
  return enabled(tapeValue) && enabled(experimentValue);
}

}  // namespace dxmt9::core::metalqueue
