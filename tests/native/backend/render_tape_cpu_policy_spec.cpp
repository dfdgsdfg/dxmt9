#include "render_tape_cpu_policy.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace dxmt9::test;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void policyTruthTable() {
  const CpuTapePolicy empty{};
  check(cpuTapePolicyValid(empty), "zero policy is admitted");
  check(admitCpuTapePolicy(empty) == CpuTapePolicyAdmission::Accepted,
        "zero policy reason");

  for (const bool compatibility : {false, true}) {
    for (std::size_t drawLimit = 0u; drawLimit <= 65u; ++drawLimit) {
      const CpuTapePolicy policy{.compatibility = compatibility,
                                 .drawCommandLimit = drawLimit};
      const bool expected = drawLimit == 0u ||
          (compatibility && drawLimit >= 3u && drawLimit <= 64u);
      check(cpuTapePolicyValid(policy) == expected,
            "draw-limit route/range admission");
      if (!expected && drawLimit != 0u && !compatibility) {
        check(admitCpuTapePolicy(policy) ==
                  CpuTapePolicyAdmission::DrawLimitRequiresCompatibility,
              "production draw limit must be rejected by route");
      }
    }
  }

  for (const std::size_t slot : {std::size_t{1u * 1024u * 1024u},
                                 std::size_t{64u * 1024u * 1024u}}) {
    const CpuTapePolicy policy{
        .slotHeadroomBytes = slot, .aggregateHeadroomBytes = slot};
    check(cpuTapePolicyValid(policy), "production headroom endpoints");
    auto wrongRoute = policy;
    wrongRoute.compatibility = true;
    check(admitCpuTapePolicy(wrongRoute) ==
              CpuTapePolicyAdmission::HeadroomRequiresProductionSlot,
          "compatibility headroom must be rejected");
  }

  for (const std::size_t value : {std::size_t{1u}, std::size_t{1024u * 1024u - 1u},
                                  std::size_t{64u * 1024u * 1024u + 1u}}) {
    check(!cpuTapePolicyValid(
              CpuTapePolicy{.slotHeadroomBytes = value,
                            .aggregateHeadroomBytes = value}),
          "slot headroom out of range");
  }
  check(!cpuTapePolicyValid(CpuTapePolicy{.slotHeadroomBytes = 1u * 1024u * 1024u,
                                          .aggregateHeadroomBytes = 512u * 1024u * 1024u + 1u}),
        "aggregate headroom maximum");
  check(admitCpuTapePolicy(CpuTapePolicy{.slotHeadroomBytes = 8u * 1024u * 1024u,
                                         .aggregateHeadroomBytes = 4u * 1024u * 1024u}) ==
            CpuTapePolicyAdmission::AggregateHeadroomBelowSlot,
        "aggregate cannot be below slot");
  check(admitCpuTapePolicy(CpuTapePolicy{.slotHeadroomBytes = 1u * 1024u * 1024u}) ==
            CpuTapePolicyAdmission::HeadroomMustBePaired,
        "one-sided slot headroom");
  check(admitCpuTapePolicy(CpuTapePolicy{.aggregateHeadroomBytes = 1u * 1024u * 1024u}) ==
            CpuTapePolicyAdmission::HeadroomMustBePaired,
        "one-sided aggregate headroom");
  check(admitCpuTapePolicy(CpuTapePolicy{.compatibility = true,
                                         .drawCommandLimit = 3u,
                                         .slotHeadroomBytes = 1u * 1024u * 1024u,
                                         .aggregateHeadroomBytes = 1u * 1024u * 1024u}) ==
            CpuTapePolicyAdmission::DrawLimitConflictsWithHeadroom,
        "draw limit cannot combine with headroom");

  const auto max = std::numeric_limits<std::size_t>::max();
  check(!cpuTapePolicyValid(CpuTapePolicy{.slotHeadroomBytes = max,
                                          .aggregateHeadroomBytes = max}),
        "maximum size headroom cannot wrap into admission");
  check(!cpuTapePolicyValid(CpuTapePolicy{.slotHeadroomBytes = 1u * 1024u * 1024u,
                                          .aggregateHeadroomBytes = max}),
        "maximum aggregate cannot bypass cap");
}

// This is an abstract append/split reducer, not a duplicate arithmetic check:
// append one record at a time, publish whenever the current piece reaches L,
// then retain the final source.  Its peak is the contract's pending bound.
std::size_t reduceAppendSplitSequence(std::size_t records,
                                      std::size_t limit) {
  std::size_t piece = 0u;
  std::size_t published = 0u;
  for (std::size_t record = 0u; record < records; ++record) {
    ++piece;
    if (piece == limit) {
      ++published;
      piece = 0u;
    }
  }
  if (piece != 0u) ++published;
  return published + 1u;
}

void exhaustivePendingProof() {
  check(kCpuTapeGeneralDrawPayloadLimitBytes == 0u,
        "general CPU policy does not change the payload cap");
  std::size_t valuations = 0u;
  for (std::size_t records = 0u; records <= 64u; ++records) {
    for (std::size_t limit = 0u; limit <= 65u; ++limit) {
      const auto bound = cpuTapeConservativeMaxPendingPerRaw(records, limit);
      const bool valid = limit >= 3u && limit <= 64u;
      check((bound != 0u) == valid, "one/two and out-of-range limits reject");
      if (valid) {
        check(bound == reduceAppendSplitSequence(records, limit),
              "append/split reducer agrees with pending bound");
        check(bound <= 31u, "admitted pending bound is at most 31");
        for (std::size_t queue = 0u; queue <= 31u; ++queue) {
          check(cpuTapeGeneralQueueBounded(records, limit, queue) ==
                    (queue >= bound),
                "fresh isolated queue admits exactly sufficient capacity");
          ++valuations;
        }
      } else {
        for (std::size_t queue = 0u; queue <= 31u; ++queue)
          check(!cpuTapeGeneralQueueBounded(records, limit, queue),
                "invalid limit cannot claim queue capacity");
      }
    }
  }
  check(cpuTapeConservativeMaxPendingPerRaw(64u, 1u) == 0u,
        "limit one is exhausted");
  check(cpuTapeConservativeMaxPendingPerRaw(64u, 2u) == 0u,
        "limit two is exhausted");
  check(reduceAppendSplitSequence(64u, 1u) > kCpuTapeMaxPendingPerRaw &&
            reduceAppendSplitSequence(64u, 2u) > kCpuTapeMaxPendingPerRaw,
        "rejected tiny limits have concrete queue-exhaustion counterexamples");
  check(valuations == 62u * 65u * 32u,
        "complete records/limits/queue proof domain");
}
}  // namespace

int main() {
  try {
    policyTruthTable();
    exhaustivePendingProof();
    // Measurement omits proof observations, not production lifetime writes;
    // the queue/TLA models remain authoritative for ownership and completion.
    std::cout << "CPU tape policy: bounded admission and append/split proof passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CPU tape policy failed: " << error.what() << '\n';
    return 1;
  }
}
