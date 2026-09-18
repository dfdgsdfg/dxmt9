#include "../../../src/d3d9/device_c_query_drain_observer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

using EndRecord = dxmt9::d3d9::query_drain_diag::EndRecord;
using Queue = dxmt9::d3d9::query_drain_diag::QueueSnapshot;
using Result = dxmt9::d3d9::query_drain_diag::ResultClass;

EndRecord end(std::uint64_t identity, std::uint64_t issue,
              std::uint64_t epoch, std::uint64_t raw) {
  return {.valid = true,
          .identity = identity,
          .issue = issue,
          .epoch = epoch,
          .backendQueue = 4u,
          .backendSequence = issue,
          .rawOrdinal = raw,
          .ordinal = issue};
}

void testMapEvictionAndInvalidation() {
  dxmt9::core::querydiag::Ledger ledger(false);
  for (std::uint64_t i = 1; i <= ledger.kEndCapacity; ++i)
    ledger.recordEnd(i, 1u, 1u, 2u, i, i);
  ledger.recordEnd(ledger.kEndCapacity + 1u, 1u, 1u, 2u, 999u, 999u);
  check(ledger.endStats().evictions == 1u,
        "END map eviction is counted in cold diagnostics");
  check(!ledger.endSnapshot(1u), "bounded END map evicts oldest entry");
  check(ledger.endSnapshot(ledger.kEndCapacity + 1u).has_value(),
        "bounded END map retains newest entry");
  ledger.invalidateEnd(ledger.kEndCapacity + 1u);
  check(ledger.endStats().invalidations == 1u,
        "BEGIN invalidation is counted in cold diagnostics");
  check(!ledger.endSnapshot(ledger.kEndCapacity + 1u),
        "BEGIN/failure invalidates prior END");
  ledger.recordEnd(9u, 7u, 3u, 8u, 70u, 700u);
  ledger.recordEnd(9u, 8u, 4u, 8u, 80u, 800u);
  const auto current = ledger.endSnapshot(9u);
  check(current && current->issue == 8u && current->epoch == 4u,
        "latest issue and reset epoch replace stale END");
}

void testClassification() {
  const auto current = end(11u, 3u, 2u, 30u);
  const Queue empty{};
  check(dxmt9::d3d9::query_drain_diag::classifyRecords(
            11u, nullptr, &current, empty, empty, true) ==
            Result::MissingBeforeAfterEnd,
        "post-drain END does not become current-issue authority");
  check(dxmt9::d3d9::query_drain_diag::classifyRecords(
            11u, &current, nullptr, empty, empty, true) == Result::NoKnownEnd,
        "missing END remains explicit");
  const auto changed = end(11u, 4u, 2u, 40u);
  const auto wrongIdentity = end(12u, 99u, 9u, 90u);
  check(dxmt9::d3d9::query_drain_diag::classifyRecords(
            11u, &wrongIdentity, &current, empty, empty, true) ==
            Result::MissingBeforeAfterEnd,
        "END from another immutable identity is not evidence");
  check(dxmt9::d3d9::query_drain_diag::classifyRecords(
            11u, &current, &changed, empty, empty, true) == Result::EndChanged,
        "reissued END is not conflated with the old issue");
  Queue tail{.queuedCount = 2u,
             .backOrdinal = 32u,
             .backOrdinalKnown = true};
  check(dxmt9::d3d9::query_drain_diag::classifyRecords(
            11u, &current, &current, tail, empty, true) ==
            Result::QueuedTailBeforeDrain,
        "FIFO tail already queued before drain is classified");
  check(dxmt9::d3d9::query_drain_diag::classifyRecords(
            11u, &current, &current, empty, tail, true) ==
            Result::QueuedTailAfterDrainOnly,
        "newly admitted post-drain tail is kept separate");
  check(dxmt9::d3d9::query_drain_diag::classifyRecords(
            11u, &current, &current, empty, empty, true) ==
            Result::KnownEndNoQueuedTail,
        "stable END with no sampled tail remains qualified");
  check(dxmt9::d3d9::query_drain_diag::classifyRecords(
            11u, &current, &current, empty, empty, false) == Result::FailedDrain,
        "failed drain can never report a success class");
  const auto noRaw = end(11u, 3u, 2u, 0u);
  check(dxmt9::d3d9::query_drain_diag::classifyRecords(
            11u, &noRaw, &noRaw, empty, empty, true) ==
            Result::EndKnownNoRawOrdinal,
        "END without raw ordinal remains explicitly unknown");
}

void testFifoUnknownAndTlsNesting() {
  Queue queue{.queuedCount = 1u,
              .frontPlaceholder = true,
              .backPlaceholder = true,
              .unknownOrdinal = true};
  check(!queue.frontOrdinalKnown && !queue.backOrdinalKnown &&
            queue.unknownOrdinal,
        "placeholder FIFO endpoints do not invent ordinals");
  using dxmt9::core::querydiag::ScopedReplayRawOrdinal;
  check(dxmt9::core::querydiag::replayRawOrdinal() == 0u,
        "TLS starts clear");
  {
    ScopedReplayRawOrdinal outer(12u);
    check(dxmt9::core::querydiag::replayRawOrdinal() == 12u,
          "outer raw ordinal is visible");
    {
      ScopedReplayRawOrdinal inner(34u);
      check(dxmt9::core::querydiag::replayRawOrdinal() == 34u,
            "nested raw ordinal overrides outer");
    }
    check(dxmt9::core::querydiag::replayRawOrdinal() == 12u,
          "nested TLS scope restores outer value");
  }
  check(dxmt9::core::querydiag::replayRawOrdinal() == 0u,
        "TLS scope restores clear value");
}

void testBoundedDetailLedger() {
  dxmt9::d3d9::query_drain_diag::Ledger ledger(false);
  for (std::size_t i = 0; i < 20u; ++i) {
    dxmt9::d3d9::query_drain_diag::Detail detail;
    detail.queryIdentity = i + 1u;
    detail.result = Result::FailedDrain;
    detail.drainNanoseconds = 9u;
    detail.drained = false;
    ledger.record(detail);
  }
  for (std::size_t i = 20u; i < 40u; ++i) {
    dxmt9::d3d9::query_drain_diag::Detail detail;
    detail.queryIdentity = i + 1u;
    detail.result = Result::FailedDrain;
    detail.beforeQueue.queuedCount = 1u;
    detail.drainNanoseconds = 9u;
    detail.drained = false;
    ledger.record(detail);
  }
  const auto totals = ledger.totals();
  check(ledger.detailCount() == ledger.kDetailCapacity &&
            totals.omittedDetails == 8u && totals.failedDrains == 40u &&
            totals.classes[static_cast<std::size_t>(Result::FailedDrain)] == 40u,
        "detail rows are bounded while aggregate totals remain complete");
  check(totals.drainNanoseconds == 360u &&
            totals.beforeQueueBusy == 20u && totals.beforeQueueEmpty == 20u &&
            totals.beforeQueueBusyNanoseconds == 180u &&
            totals.beforeQueueEmptyNanoseconds == 180u &&
            totals.classDrainNanoseconds[
                static_cast<std::size_t>(Result::FailedDrain)] == 360u,
        "durations conserve the same complete populations as call counts");
  const auto details = ledger.details();
  check(details[0].queryIdentity == 21u && details[16].queryIdentity == 1u,
        "initial empty polls cannot consume the later busy detail quota");
}

}  // namespace

int main() {
  try {
    testMapEvictionAndInvalidation();
    testClassification();
    testFifoUnknownAndTlsNesting();
    testBoundedDetailLedger();
  } catch (const TestFailure& failure) {
    std::cerr << "query_replay_drain_observer_spec: " << failure.what() << '\n';
    return 1;
  }
  std::cout << "query_replay_drain_observer_spec passed\n";
  return 0;
}
