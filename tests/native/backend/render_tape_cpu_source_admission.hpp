#pragma once

#include <cstddef>
#include <cstdint>

namespace dxmt9::test {

// Cold no-worker harness gate, not a reservation or a production capability.
// The caller samples under the real queue mutex and may consume the snapshot
// only while it is the sole queue owner. `tapeCanReserve` is the production
// predicate for ONE source; it never proves aggregate headroom for N sources.
struct CpuTapeSourceAdmissionInputs {
  bool stopped = false;
  bool writerOwned = false;
  bool writerValid = false;
  bool writerEmpty = false;
  bool nextControlFree = false;
  bool tapeCanReserve = false;
  std::size_t inflightCount = 0u;
  std::size_t inflightLimit = 0u;
};

enum class CpuTapeSourceAdmission : std::uint8_t {
  Admitted,
  Stopped,
  InvalidLimit,
  InvalidWriter,
  WritingNotEmpty,
  ControlBusy,
  TapePressure,
  InflightPressure,
};

inline constexpr CpuTapeSourceAdmission admitCpuTapeSource(
    const CpuTapeSourceAdmissionInputs& input) noexcept {
  if (input.stopped) return CpuTapeSourceAdmission::Stopped;
  if (input.inflightLimit == 0u) return CpuTapeSourceAdmission::InvalidLimit;
  if (input.inflightCount >= input.inflightLimit)
    return CpuTapeSourceAdmission::InflightPressure;
  if (input.writerOwned) {
    if (!input.writerValid) return CpuTapeSourceAdmission::InvalidWriter;
    if (!input.writerEmpty) return CpuTapeSourceAdmission::WritingNotEmpty;
  } else {
    if (!input.nextControlFree) return CpuTapeSourceAdmission::ControlBusy;
    if (!input.tapeCanReserve) return CpuTapeSourceAdmission::TapePressure;
  }
  return CpuTapeSourceAdmission::Admitted;
}

// Explicit release policy, not proof of queue/resource admission. Eligibility
// is established from the immutable emission plan and isolated runtime before
// this reducer can preserve a pending source across the next command chunk.
enum class CpuTapeWindowAction : std::uint8_t {
  Reject, KeepPending, ReleaseOldest, DrainAll,
};

inline constexpr CpuTapeWindowAction cpuTapeWindowAction(
    std::size_t pending, std::size_t limit, bool workBoundary,
    bool eligibleCommand, bool pendingEligible) noexcept {
  if (limit < 1u || limit > 2u || pending > limit)
    return CpuTapeWindowAction::Reject;
  if (limit == 2u && eligibleCommand && (pending == 0u || pendingEligible))
    return pending == limit ? CpuTapeWindowAction::ReleaseOldest
                            : CpuTapeWindowAction::KeepPending;
  return workBoundary ? CpuTapeWindowAction::DrainAll
                      : CpuTapeWindowAction::KeepPending;
}

}  // namespace dxmt9::test
