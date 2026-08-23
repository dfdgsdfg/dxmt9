#include "d3d9_pe_transition_algebra.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace pe = dxmt9::d3d9::pe;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

bool samePlan(const pe::StateWritePlan& a,
              const pe::StateWritePlan& b) {
  return a.kind == b.kind && a.writeLive == b.writeLive &&
      a.writePending == b.writePending &&
      a.writeRecorded == b.writeRecorded &&
      a.directOrderedCall == b.directOrderedCall &&
      a.semanticTransition == b.semanticTransition;
}

struct QualifiedKey {
  std::uint8_t category;
  std::uint8_t slot;

  friend bool operator==(QualifiedKey, QualifiedKey) = default;

  friend bool operator<(QualifiedKey a, QualifiedKey b) {
    return std::pair(a.category, a.slot) < std::pair(b.category, b.slot);
  }
};

struct Domains {
  std::map<QualifiedKey, int> live;
  std::map<QualifiedKey, int> pending;
  std::map<QualifiedKey, int> recorded;
};

pe::StateWritePlan write(Domains& domains, QualifiedKey key, int value,
                         pe::RecorderPhase phase,
                         pe::WriteOrigin origin) {
  const auto liveIt = domains.live.find(key);
  const bool liveContains = liveIt != domains.live.end();
  const auto plan = pe::planRecorderStateWrite({
      .phase = phase,
      .origin = origin,
      .liveContains = liveContains,
      .liveEquals = liveContains && liveIt->second == value,
      .pendingContains = domains.pending.contains(key),
      .recordedContains = domains.recorded.contains(key),
  });
  if (plan.writeLive) domains.live[key] = value;
  if (plan.writePending) domains.pending[key] = value;
  if (plan.writeRecorded) domains.recorded[key] = value;
  return plan;
}

void exhaustiveStateTruthTable() {
  constexpr pe::RecorderPhase phases[] = {
      pe::RecorderPhase::Live, pe::RecorderPhase::Recording};
  constexpr pe::WriteOrigin origins[] = {
      pe::WriteOrigin::ExplicitSet, pe::WriteOrigin::PriorValueOperation};

  for (auto phase : phases) {
    for (auto origin : origins) {
        for (unsigned bits = 0; bits < 16u; ++bits) {
          const bool liveContains = (bits & 1u) != 0u;
          const bool liveEquals = (bits & 2u) != 0u;
          const bool pendingContains = (bits & 4u) != 0u;
          const bool recordedContains = (bits & 8u) != 0u;
          const auto plan = pe::planRecorderStateWrite({
              .phase = phase,
              .origin = origin,
              .liveContains = liveContains,
              .liveEquals = liveEquals,
              .pendingContains = pendingContains,
              .recordedContains = recordedContains,
          });
          const bool normalizedEquals = liveContains && liveEquals;
          if (phase == pe::RecorderPhase::Live) {
            if (normalizedEquals) {
              check(!plan.writeLive && !plan.writePending,
                    "equal live writes never create duplicate effects");
              check(plan.kind == (pendingContains
                                      ? pe::StateWriteKind::RetainPending
                                      : pe::StateWriteKind::NoOp),
                    "equal live write distinguishes pending obligation");
            } else {
              check(plan.kind == pe::StateWriteKind::QueueDelta &&
                        plan.writeLive && plan.writePending,
                    "changed live write queues one delta");
            }
            check(!plan.writeRecorded && !plan.directOrderedCall,
                  "live writes do not touch recording-only domains");
          } else if (origin == pe::WriteOrigin::ExplicitSet) {
            check(plan.kind == pe::StateWriteKind::RecordExplicit &&
                      plan.writeRecorded && !plan.writeLive &&
                      !plan.writePending && !plan.directOrderedCall &&
                      plan.semanticTransition,
                  "recording explicit write is record-only");
          } else {
            check(plan.kind == pe::StateWriteKind::ApplyPriorValueOnly &&
                      plan.writeLive && !plan.writePending &&
                      !plan.writeRecorded && plan.directOrderedCall,
                  "prior-value write changes live but never recorded set");
          }

          if (!liveContains && liveEquals) {
            auto normalized = pe::planRecorderStateWrite({
                .phase = phase,
                .origin = origin,
                .liveContains = false,
                .liveEquals = false,
                .pendingContains = pendingContains,
                .recordedContains = recordedContains,
            });
            check(samePlan(plan, normalized),
                  "impossible equality tuple normalizes to different");
          }
        }
    }
  }
}

void stateSequenceEvidence() {
  const QualifiedKey render7{0u, 7u};
  Domains aba{};
  aba.live[render7] = 1;
  write(aba, render7, 2, pe::RecorderPhase::Live,
        pe::WriteOrigin::ExplicitSet);
  write(aba, render7, 1, pe::RecorderPhase::Live,
        pe::WriteOrigin::ExplicitSet);
  check(aba.live[render7] == 1 && aba.pending.size() == 1u &&
            aba.pending[render7] == 1,
        "A->B->A leaves one newest pending row");
  write(aba, render7, 3, pe::RecorderPhase::Live,
        pe::WriteOrigin::ExplicitSet);
  check(aba.pending.size() == 1u && aba.pending[render7] == 3,
        "same-key A->B->C is last-write-wins");

  Domains livePrior{};
  livePrior.live[render7] = 1;
  livePrior.pending[render7] = 1;
  const auto livePriorPlan = write(
      livePrior, render7, 4, pe::RecorderPhase::Live,
      pe::WriteOrigin::PriorValueOperation);
  check(livePriorPlan.kind == pe::StateWriteKind::QueueDelta &&
            !livePriorPlan.directOrderedCall &&
            livePrior.pending[render7] == 4,
        "prior-value operation outside recording replaces pending with result");

  Domains sameValue{};
  sameValue.live[render7] = 9;
  write(sameValue, render7, 9, pe::RecorderPhase::Recording,
        pe::WriteOrigin::ExplicitSet);
  check(sameValue.recorded.contains(render7) &&
            sameValue.recorded[render7] == 9,
        "first same-value recording fixes tracked membership");
  write(sameValue, render7, 10, pe::RecorderPhase::Recording,
        pe::WriteOrigin::ExplicitSet);
  check(sameValue.recorded.size() == 1u &&
            sameValue.recorded[render7] == 10,
        "repeated recording keeps cardinality and newest value");

  Domains multiply{};
  multiply.live[render7] = 4;
  write(multiply, render7, 5, pe::RecorderPhase::Recording,
        pe::WriteOrigin::PriorValueOperation);
  check(multiply.live[render7] == 5 && multiply.recorded.empty() &&
            multiply.pending.empty(),
        "prior-value-only recording persists without enlarging tracked set");
  write(multiply, render7, 6, pe::RecorderPhase::Recording,
        pe::WriteOrigin::ExplicitSet);
  check(multiply.live[render7] == 5 && multiply.recorded[render7] == 6,
        "explicit recording does not modify primary live state");
  write(multiply, render7, 7, pe::RecorderPhase::Recording,
        pe::WriteOrigin::PriorValueOperation);
  check(multiply.recorded[render7] == 6 && multiply.live[render7] == 7 &&
            multiply.pending.empty(),
        "prior-value write after explicit Set preserves recorded value and live result");

  const QualifiedKey tss7{1u, 7u};
  const QualifiedKey sampler7{2u, 7u};
  Domains forward{};
  Domains reverse{};
  write(forward, render7, 1, pe::RecorderPhase::Live,
        pe::WriteOrigin::ExplicitSet);
  write(forward, tss7, 2, pe::RecorderPhase::Live,
        pe::WriteOrigin::ExplicitSet);
  write(reverse, tss7, 2, pe::RecorderPhase::Live,
        pe::WriteOrigin::ExplicitSet);
  write(reverse, render7, 1, pe::RecorderPhase::Live,
        pe::WriteOrigin::ExplicitSet);
  check(forward.live == reverse.live && forward.pending == reverse.pending,
        "distinct qualified writes commute");
  write(forward, sampler7, 3, pe::RecorderPhase::Live,
        pe::WriteOrigin::ExplicitSet);
  check(forward.live.size() == 3u && forward.pending.size() == 3u,
        "render/TSS/sampler ordinal 7 remain kind-qualified");

  const Domains beforeInvalid = forward;
  const bool validationSucceeded = false;
  if (validationSucceeded) {
    write(forward, {3u, 255u}, 99, pe::RecorderPhase::Live,
          pe::WriteOrigin::ExplicitSet);
  }
  check(forward.live == beforeInvalid.live &&
            forward.pending == beforeInvalid.pending &&
            forward.recorded == beforeInvalid.recorded,
        "invalid input applies no transition");
}

void exhaustiveAppendTruthTable() {
  constexpr pe::AppendSettlement phases[] = {
      pe::AppendSettlement::Prepared, pe::AppendSettlement::Accepted,
      pe::AppendSettlement::Failed, pe::AppendSettlement::Discarded};
  for (auto phase : phases) {
    for (unsigned bits = 0u; bits < 4u; ++bits) {
      const bool succeeded = (bits & 1u) != 0u;
      const bool discard = (bits & 2u) != 0u;
      const auto plan = pe::settleRecorderAppend({
          .phase = phase,
          .appendSucceeded = succeeded,
          .explicitDiscard = discard,
      });
      const bool valid = phase == pe::AppendSettlement::Prepared &&
          !(succeeded && discard);
      check(plan.valid == valid, "append truth-table validity");
      check(plan.consumeRepresentedPending ==
                (valid && succeeded),
            "only accepted append consumes represented pending");
      check(plan.recordDurable == (valid && succeeded),
            "only accepted append becomes durable");
      if (!valid) {
        check(plan.next == phase && !plan.consumeRepresentedPending,
              "repeated/contradictory settlement fails closed");
      } else if (succeeded) {
        check(plan.next == pe::AppendSettlement::Accepted &&
                  !plan.retainPreparedProjection,
              "accepted transition is terminal");
      } else if (discard) {
        check(plan.next == pe::AppendSettlement::Discarded &&
                  !plan.retainPreparedProjection,
              "discard abandons projection without consumption");
      } else {
        check(plan.next == pe::AppendSettlement::Failed &&
                  plan.retainPreparedProjection,
              "failed append retains exact retry projection");
      }
    }
  }

  std::map<int, int> pending{{0, 10}, {1, 11}, {2, 12}};
  const std::map<int, int> prepared{{0, 10}, {1, 11}};
  const auto failed = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = false,
  });
  if (failed.consumeRepresentedPending) {
    for (const auto& [key, value] : prepared) {
      (void)value;
      pending.erase(key);
    }
  }
  check(pending.size() == 3u && failed.retainPreparedProjection,
        "injected append failure retains all pending and retry witness");
  const auto retryProjection = prepared;
  check(retryProjection == prepared, "retry projection is byte/value equal");

  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  if (accepted.consumeRepresentedPending) {
    for (const auto& [key, value] : retryProjection) {
      (void)value;
      pending.erase(key);
    }
  }
  check(pending.size() == 1u && pending.contains(2) &&
            accepted.recordDurable,
        "accepted retry consumes exact subset and preserves oversized tail");
  const auto second = pe::settleRecorderAppend({
      .phase = accepted.next,
      .appendSucceeded = true,
  });
  check(!second.valid && !second.consumeRepresentedPending &&
            pending.size() == 1u,
        "accepted projection cannot be consumed twice");

  const auto discarded = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .explicitDiscard = true,
  });
  check(discarded.valid && !discarded.consumeRepresentedPending &&
            pending.size() == 1u,
        "discard never consumes pending");
}

}  // namespace

int main() {
  exhaustiveStateTruthTable();
  stateSequenceEvidence();
  exhaustiveAppendTruthTable();
  if (failures != 0) {
    std::fprintf(stderr, "%d PE transition algebra checks failed\n", failures);
    return 1;
  }
  std::puts("PE recorder transition algebra truth tables passed");
  return 0;
}
