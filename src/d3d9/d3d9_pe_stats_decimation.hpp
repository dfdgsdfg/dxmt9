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

// Bucketed variant: splits a scope's decimated samples by the per-call element
// count, so a scope whose cost is dominated by fixed per-call overhead can be
// told apart from one dominated by per-element work. A flat ns/sample across
// buckets means the entry cost dominates; a slope means the per-element loop
// does. Diagnostic only, and driven by the same decimation gate — the buckets
// cost one comparison chain on the already-sampled path.
// Shared instrument-cost calibration for every decimated scope. Each sampling
// site times one empty region with the identical clock pair and records it
// here, so `raw_mean - null_mean` is the scope's real cost. This is not
// optional bookkeeping: on 2026-07-29 the const-setter scope measured 202 ns
// per call raw against a 181 ns instrument cost -- 91% of the reading was the
// clock. Decimation keeps perturbation ~1%; it does nothing for bias, and
// N-variation cannot expose it because `sampled_ms * N / presents` reduces to
// `events * (true + bias) / presents`, independent of N.
inline PeDecimatedScopeStats &peDecimatedNullScopeStats() {
  static PeDecimatedScopeStats stats{};
  return stats;
}

struct PeDecimatedBucketStats {
  static constexpr int kBuckets = 6;  // 1, 2, 3-4, 5-8, 9-16, >16
  PeDecimatedScopeStats bucket[kBuckets]{};

  static int bucketFor(std::uint32_t count) {
    if (count <= 1u) return 0;
    if (count == 2u) return 1;
    if (count <= 4u) return 2;
    if (count <= 8u) return 3;
    if (count <= 16u) return 4;
    return 5;
  }
  void record(std::uint32_t count, std::uint64_t ns) {
    auto &b = bucket[bucketFor(count)];
    ++b.sampled;
    b.sampledNs += ns;
  }
  void countEvent(std::uint32_t count) { ++bucket[bucketFor(count)].events; }
};
