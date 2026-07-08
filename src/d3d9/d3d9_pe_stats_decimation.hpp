#pragma once

// Decimated (every-Nth-event) CPU timing for PE-recorder hot scopes.
//
// Full DXMT9_PE_RECORDER_STATS instrumentation times every scope, which
// under Rosetta QPC costs ~1.5us per timed scope and ~20k scopes/present —
// enough to cause ~34% throughput loss. This header provides a clock-free,
// header-only, independently unit-testable building block for sampling only
// every Nth event instead, so PE d3d9.dll recording cost can be measured
// with ~1% perturbation. It is fully independent of
// DXMT9_PE_RECORDER_STATS / PeRecorderStats — call sites read the wall
// clock themselves (this header never calls std::chrono) and pass the
// already-computed elapsed nanoseconds into recordSample().

#include <cstdint>

struct PeDecimatedScopeStats {
  std::uint64_t events = 0;
  std::uint64_t sampled = 0;
  std::uint64_t sampledNs = 0;
};

// RAII: increments events always; times only every Nth event (events % n == 0
// after increment). n==0 disables entirely. Caller supplies a now() function
// pointer or the call sites read the clock — keep the header clock-free:
class PeDecimatedScopeTimer {
 public:
  // returns true when this event should be timed
  static bool shouldSample(PeDecimatedScopeStats &s, std::uint32_t n) {
    ++s.events;
    return n != 0 && (s.events % n) == 0;
  }
  static void recordSample(PeDecimatedScopeStats &s, std::uint64_t ns) {
    ++s.sampled;
    s.sampledNs += ns;
  }
};
