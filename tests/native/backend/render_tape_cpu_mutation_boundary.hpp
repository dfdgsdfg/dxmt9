#pragma once

#include <cstdint>

namespace dxmt9::test {

// Cold, bounded admission input for a CPU render-tape mutation.  The
// reducer deliberately knows nothing about a lock, a queue, or a Metal
// object: its result is only the ordering obligation at the mutation
// boundary.  A caller which gets ReleaseBeforeMutation must acknowledge the
// pending source before entering its real (possibly blocking) lock/map path.
struct CpuTapeMutationWitness {
  bool exactlyOnePending = false;
  bool alreadyDeferredMutation = false;
  bool bufferResolved = false;
  bool defaultPool = false;
  bool referencedByPending = false;
  bool versioned = false;
  bool discard = false;
  bool noWait = false;
  bool capturedBackingValid = false;
};

enum class CpuTapeMutationResult : std::uint8_t {
  ReleaseBeforeMutation,
  DeferUnrelatedPlain,
  DeferCapturedDiscard,
};

// This is the single shared admission predicate.  Keep it separate from the
// two disposition predicates so a truth-table test can pin every conjunct and
// every disposition without depending on enum spelling.
inline constexpr bool cpuTapeMutationDeferralGate(
    const CpuTapeMutationWitness& witness) noexcept {
  return witness.exactlyOnePending && !witness.alreadyDeferredMutation &&
         witness.bufferResolved && witness.defaultPool && witness.noWait;
}

// Pure CPU policy reducer.  It is intentionally conservative: a hold is
// admitted only for the two fully-qualified shapes.  In particular, a
// captured DISCARD without a valid captured backing falls through to release;
// this prevents a later mutation from replacing bytes still named by the
// pending tape source.
inline constexpr CpuTapeMutationResult reduceCpuTapeMutationAdmission(
    const CpuTapeMutationWitness& witness) noexcept {
  if (!cpuTapeMutationDeferralGate(witness))
    return CpuTapeMutationResult::ReleaseBeforeMutation;

  if (!witness.referencedByPending && !witness.versioned && !witness.discard)
    return CpuTapeMutationResult::DeferUnrelatedPlain;

  if (witness.referencedByPending && witness.versioned && witness.discard &&
      witness.capturedBackingValid)
    return CpuTapeMutationResult::DeferCapturedDiscard;

  return CpuTapeMutationResult::ReleaseBeforeMutation;
}

}  // namespace dxmt9::test
