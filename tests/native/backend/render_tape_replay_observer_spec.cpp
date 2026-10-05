#include "device_c_render_tape_replay_observer.hpp"
#include "device_c_replay_projection.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using namespace dxmt9;
using namespace dxmt9::d3d9;
using Kind = ReplayTransactionObservationKind;
using Witness = ReplayTransactionEffectWitness;
using Failure = ReplayTransactionObserverFailure;
constexpr ReplayTransactionRawIdentity raw{9u, 12u, 42u, 8u};
constexpr ReplaySourceIdentity source{0u, 7u, 7u, 42u};
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Observed {
  Kind kind{};
  Witness witness{};
  ReplayTransactionRawIdentity raw{};
  ReplayTransactionState state{};
  bool journalEmpty = false;
  bool restored = false;
};

struct Collector {
  std::array<Observed, 32> rows{};
  std::size_t count = 0u;
  std::size_t failures = 0u;
  Failure lastFailure{};
  core::Device* device = nullptr;
  bool rejectEffect = false;

  static bool observe(void* context, const ReplayTransactionObservation& value) noexcept {
    auto& self = *static_cast<Collector*>(context);
    if (!value.transaction || !value.state || self.count == self.rows.size()) return false;
    auto& row = self.rows[self.count++];
    row.kind = value.kind;
    row.witness = value.effect;
    row.raw = value.raw;
    row.state = *value.state;
    row.journalEmpty = value.transaction->journal().empty();
    row.restored = !self.device || (self.device->state().viewport.width == 17u &&
        self.device->state().renderStates.at(3u) == 9u);
    return !(self.rejectEffect && value.kind == Kind::FirstEffect);
  }
  static void failed(void* context, Failure failure) noexcept {
    auto& self = *static_cast<Collector*>(context);
    ++self.failures;
    self.lastFailure = failure;
  }
  ReplayTransactionObserverSink sink() noexcept { return {this, observe, failed}; }
};

void project(ReplayTransaction& transaction, std::uint32_t index) {
  check(transaction.project({11u, source.source, index}), "real original record projection accepted");
  check(transaction.stage({1u, 4u}), "real staged emission accepted");
}

void commit(ReplayTransaction& transaction) {
  check(transaction.receiveDestination({.identity = transaction.state().identity,
      .queueSequence = transaction.state().identity.sequence,
      .commandCount = transaction.state().stagedCommandCount}), "actual destination accepted");
  check(transaction.commit(), "actual transaction commits");
}

void disabledAndThreadLocal() {
  Collector collector;
  ReplayTransaction transaction;
  check(!replayTransactionObserverEnabled(), "observer disabled outside capture scope");
  {
    ScopedReplayTransactionObserver disabled(raw, {});
    transaction.begin(source, nullptr, 2u);
    project(transaction, 2u);
    noteExternalEffect(transaction, Witness::DirectLease);
    check(transaction.startIrreversibleEffect(), "disabled effect preserves production result");
    commit(transaction);
  }
  check(collector.count == 0u && collector.failures == 0u, "disabled transitions dispatch no observation");
  {
    ScopedReplayTransactionObserver enabled(raw, collector.sink());
    bool threadDisabled = false;
    std::thread other([&] {
      threadDisabled = !replayTransactionObserverEnabled();
      ReplayTransaction local;
      local.begin(source);
      commit(local);
    });
    other.join();
    check(threadDisabled && collector.count == 0u, "capture scope does not leak into another thread");
    transaction.begin(source);
    commit(transaction);
  }
  check(collector.count == 3u && collector.failures == 0u, "enabled original methods dispatch only on owning thread");
}

void successfulTransitionsAndCleanup() {
  Collector collector;
  ReplayTransaction transaction;
  core::DeviceState state;
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source, nullptr, 3u);
    check(collector.rows[0].kind == Kind::Begin && collector.rows[0].state.nextRecordOrdinal == 3u &&
        collector.rows[0].state.phase == ReplayTransactionPhase::Created && collector.rows[0].raw == raw,
        "begin observes reset transaction and actual nonzero first cursor");
    check(transaction.journal().captureViewport(state), "working journal captures semantic value");
    project(transaction, 3u);
    check(collector.count == 1u, "project and stage have no per-record observation");
    check(transaction.startIrreversibleEffect(), "actual first effect succeeds");
    check(transaction.startIrreversibleEffect(), "duplicate effect remains accepted by production protocol");
    check(collector.count == 2u && collector.rows[1].kind == Kind::FirstEffect &&
        collector.rows[1].witness == Witness::Transaction, "first effect observed exactly once");
    commit(transaction);
    check(collector.rows[2].kind == Kind::DestinationAccepted &&
        collector.rows[2].state.phase == ReplayTransactionPhase::DestinationReceived &&
        collector.rows[2].state.destination.commandCount == 1u, "accepted destination carries actual production receipt");
    check(collector.rows[3].kind == Kind::Committed && collector.rows[3].journalEmpty &&
        collector.rows[3].state.nextRecordOrdinal == 4u, "commit observation follows journal cleanup");
    const auto count = collector.count;
    check(!transaction.commit() && !transaction.failStop() && !transaction.startIrreversibleEffect(),
        "failed terminal transitions retain original false results");
    check(collector.count == count, "failed transitions cannot fabricate observations");
  }
  check(collector.failures == 0u, "successful scope closes cleanly");
}

void rejectedTransitionsAndSink() {
  Collector collector;
  ReplayTransaction transaction;
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source);
    check(!transaction.stage({1u, 4u}) && !transaction.commit(), "invalid production transitions rejected");
    auto foreign = source;
    foreign.sequence = 43u;
    check(!transaction.receiveDestination({.identity = foreign, .queueSequence = 43u}),
        "foreign production destination rejected");
    check(collector.count == 1u && collector.failures == 0u, "unsuccessful transitions do not dispatch or poison capture");
    commit(transaction);
  }
  collector = {};
  collector.rejectEffect = true;
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source);
    check(transaction.startIrreversibleEffect(), "observer rejection cannot change successful renderer effect");
    commit(transaction);
    check(transaction.state().phase == ReplayTransactionPhase::Committed, "renderer commits despite failed diagnostic sink");
  }
  check(collector.failures == 1u && collector.lastFailure == Failure::SinkRejected && collector.count == 2u,
      "sink rejection poisons capture once and suppresses later observations");
}

void semanticAndDestinationRollback() {
  auto device = std::make_unique<core::Device>(core::AdapterInfo{}, core::BackendLimits{},
      core::PresentParameters{}, 0u);
  device->mutableState().viewport.width = 17u;
  device->mutableState().renderStates.set(3u, 9u);
  Collector collector;
  collector.device = device.get();
  ReplayTransaction transaction;
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source, device.get(), 2u);
    check(transaction.journal().captureViewport(device->state()) &&
        transaction.journal().captureRenderState(device->state(), 3u), "rollback journal captures real device semantics");
    device->mutableState().viewport.width = 99u;
    device->mutableState().renderStates.set(3u, 55u);
    project(transaction, 2u);
    check(transaction.rollback(*device), "real semantic rollback restores device");
    check(collector.rows[1].kind == Kind::RolledBack && collector.rows[1].journalEmpty &&
        collector.rows[1].restored && collector.rows[1].state.nextRecordOrdinal == 3u,
        "rollback observation follows actual restoration and cleanup");
    noteDestinationRollback(transaction, true);
    check(collector.rows[2].kind == Kind::DestinationRollbackConfirmed, "destination rollback has independent actual confirmation");
    transaction.begin(source, device.get(), 2u);
    project(transaction, 2u);
    commit(transaction);
  }
  check(collector.failures == 0u && collector.count == 6u, "confirmed pre-effect rollback permits new real retry attempt");
  collector = {};
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source, device.get());
    check(transaction.rollback(*device), "zero-projection rollback is a real terminal outcome");
    noteDestinationRollback(transaction, false);
  }
  check(collector.failures == 1u && collector.lastFailure == Failure::DestinationRollbackFailed,
      "failed destination rollback cannot qualify semantic rollback");
}

void externalEffectsAndFailStop() {
  Collector collector;
  ReplayTransaction transaction;
  core::DeviceState state;
  auto device = std::make_unique<core::Device>(core::AdapterInfo{}, core::BackendLimits{},
      core::PresentParameters{}, 0u);
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source);
    project(transaction, 0u);
    noteExternalEffect(transaction, Witness::DirectLease);
    noteExternalEffect(transaction, Witness::ArenaEmitted);
    check(collector.count == 2u && collector.rows[1].witness == Witness::DirectLease &&
        transaction.state().phase == ReplayTransactionPhase::Staged, "external first effect is observed without altering reducer state");
    check(transaction.startIrreversibleEffect(), "later production effect transition remains independent");
    check(collector.count == 2u, "explicit external and reducer witnesses share one first-effect observation");
    check(!transaction.rollback(*device) && collector.count == 2u,
        "actual failed rollback after reducer effect dispatches no terminal observation");
    check(transaction.journal().captureViewport(state), "fail-stop has a live working journal");
    check(transaction.failStop(), "real fail-stop succeeds");
    check(collector.rows[2].kind == Kind::FailedStop && collector.rows[2].journalEmpty,
        "fail-stop is observed only after cleanup, never as rollback");
  }
  check(collector.failures == 0u, "actual fail-stop is forwarded for identity root to invalidate capture");
  collector = {};
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source, device.get());
    noteExternalEffect(transaction, Witness::ArenaEmitted);
    check(collector.rows[1].witness == Witness::ArenaEmitted,
        "Arena emission supplies an explicit first-effect witness, never inferred at terminal");
    check(transaction.rollback(*device), "diagnostic cannot change reducer's actual rollback result");
  }
  check(collector.failures == 1u && collector.lastFailure == Failure::RollbackAfterEffect,
      "external irreversible effect prevents capture from treating journal restore as valid rollback");
}

void overwritesAndIncompleteScope() {
  Collector collector;
  ReplayTransaction transaction;
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source);
    transaction.begin(source, nullptr, 2u);
    check(transaction.state().nextRecordOrdinal == 2u, "begin overwrite never changes renderer reset behavior");
    commit(transaction);
  }
  check(collector.count == 1u && collector.failures == 1u &&
      collector.lastFailure == Failure::BeginOverwritesAttempt, "new begin cannot silently erase a nonterminal captured attempt");
  collector = {};
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source);
  }
  check(collector.lastFailure == Failure::NonterminalScopeExit && collector.failures == 1u,
      "scope exit cannot drop an open actual attempt");
  commit(transaction);
  auto device = std::make_unique<core::Device>(core::AdapterInfo{}, core::BackendLimits{},
      core::PresentParameters{}, 0u);
  collector = {};
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source, device.get());
    check(transaction.rollback(*device), "semantic rollback alone succeeds");
    transaction.begin(source, device.get());
    commit(transaction);
  }
  check(collector.lastFailure == Failure::UnresolvedDestinationRollback && collector.failures == 1u,
      "next actual begin cannot erase unconfirmed destination rollback");
  collector = {};
  {
    ScopedReplayTransactionObserver scope(raw, collector.sink());
    transaction.begin(source, device.get());
    check(transaction.rollback(*device), "unconfirmed terminal rollback remains provisional");
  }
  check(collector.lastFailure == Failure::UnresolvedDestinationRollback,
      "scope exit cannot qualify an unconfirmed destination rollback");
}

void nestedScopeIdentity() {
  Collector collector;
  ReplayTransaction transaction;
  {
    ScopedReplayTransactionObserver outer(raw, collector.sink());
    transaction.begin(source);
    {
      ScopedReplayTransactionObserver same(raw, collector.sink());
      commit(transaction);
      transaction.begin(source, nullptr, 1u);
      commit(transaction);
    }
  }
  check(collector.count == 6u && collector.failures == 0u,
      "same raw and sink nest by alias, each real begin remains distinct");
  collector = {};
  Collector foreign;
  {
    ScopedReplayTransactionObserver outer(raw, collector.sink());
    auto foreignRaw = raw;
    ++foreignRaw.eventOrdinal;
    {
      ScopedReplayTransactionObserver nested(foreignRaw, foreign.sink());
      transaction.begin(source);
      commit(transaction);
    }
    transaction.begin(source);
    commit(transaction);
  }
  check(collector.failures == 1u && foreign.failures == 1u && collector.count == 0u && foreign.count == 0u,
      "foreign raw nesting poisons both captures and cannot misattribute renderer transitions");
  collector = {};
  foreign = {};
  {
    ScopedReplayTransactionObserver outer(raw, collector.sink());
    ScopedReplayTransactionObserver nested(raw, foreign.sink());
    transaction.begin(source);
    commit(transaction);
  }
  check(collector.lastFailure == Failure::NestedScopeMismatch && foreign.failures == 1u,
      "same raw with foreign sink is not a permitted alias");
  check(!replayTransactionObserverEnabled(), "all scope paths restore disabled TLS state");
}
} // namespace

int main() {
  try {
    disabledAndThreadLocal();
    successfulTransitionsAndCleanup();
    rejectedTransitionsAndSink();
    semanticAndDestinationRollback();
    externalEffectsAndFailStop();
    overwritesAndIncompleteScope();
    nestedScopeIdentity();
    std::cout << checks << " replay transaction observer checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "replay transaction observer failure: " << error.what() << '\n';
    return 1;
  }
}
