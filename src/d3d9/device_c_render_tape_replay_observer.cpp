#include "device_c_render_tape_replay_observer.hpp"

#include "device_c_replay_projection.hpp"

namespace dxmt9::d3d9 {
namespace detail {
thread_local ReplayTransactionObserverState* replayTransactionObserver = nullptr;
} // namespace detail
namespace {

using State = detail::ReplayTransactionObserverState;
using Kind = ReplayTransactionObservationKind;
using Failure = ReplayTransactionObserverFailure;
using Witness = ReplayTransactionEffectWitness;

void poison(State& scope, Failure failure) noexcept {
  if (scope.failed) return;
  scope.failed = true;
  if (scope.sink.failed) scope.sink.failed(scope.sink.context, failure);
}

bool sameSink(ReplayTransactionObserverSink left,
    ReplayTransactionObserverSink right) noexcept {
  return left.context == right.context && left.observe == right.observe && left.failed == right.failed;
}

bool rawAdmitted(ReplayTransactionRawIdentity raw) noexcept {
  return raw.captureToken && raw.eventOrdinal && raw.rawOrdinal;
}

State* scopeFor(const ReplayTransaction& transaction) noexcept {
  auto* scope = detail::replayTransactionObserver;
  if (!scope || scope->failed) return nullptr;
  const auto& state = transaction.state();
  if (!scope->active || scope->transaction != &transaction || !state.identity.valid() ||
      state.identity.sequence != scope->raw.rawOrdinal ||
      state.nextRecordOrdinal > scope->raw.totalRawRecords) {
    poison(*scope, Failure::InvalidObservation);
    return nullptr;
  }
  return scope;
}

void emit(State& scope, const ReplayTransaction& transaction, Kind kind,
    Witness witness = Witness::Transaction) noexcept {
  const ReplayTransactionObservation observation{
      scope.raw, kind, witness, &transaction, &transaction.state()};
  if (!scope.sink.observe(scope.sink.context, observation)) poison(scope, Failure::SinkRejected);
}

} // namespace

ScopedReplayTransactionObserver::ScopedReplayTransactionObserver(
    ReplayTransactionRawIdentity raw, ReplayTransactionObserverSink sink) noexcept {
  previous_ = detail::replayTransactionObserver;
  state_.raw = raw;
  state_.sink = sink;
  if (!sink && !previous_) return;
  if (!sink || !rawAdmitted(raw)) poison(state_, Failure::InvalidScope);
  if (previous_) {
    if (!state_.failed && previous_->raw == raw && sameSink(previous_->sink, sink)) {
      installed_ = previous_;
      return;
    }
    poison(*previous_, Failure::NestedScopeMismatch);
    poison(state_, Failure::NestedScopeMismatch);
  }
  owner_ = true;
  installed_ = &state_;
  detail::replayTransactionObserver = installed_;
}

ScopedReplayTransactionObserver::~ScopedReplayTransactionObserver() {
  if (!owner_) {
    if (installed_ && detail::replayTransactionObserver != installed_)
      poison(*installed_, Failure::ScopeOrderMismatch);
    return;
  }
  if (detail::replayTransactionObserver != installed_) {
    poison(state_, Failure::ScopeOrderMismatch);
    return;
  }
  if (state_.active) poison(state_, Failure::NonterminalScopeExit);
  if (state_.destinationRollbackPending) poison(state_, Failure::UnresolvedDestinationRollback);
  detail::replayTransactionObserver = previous_;
}

void observeReplayTransactionBegin(const ReplayTransaction& transaction) noexcept {
  auto* scope = detail::replayTransactionObserver;
  if (!scope || scope->failed) return;
  if (scope->active) {
    poison(*scope, Failure::BeginOverwritesAttempt);
    return;
  }
  if (scope->destinationRollbackPending) {
    poison(*scope, Failure::UnresolvedDestinationRollback);
    return;
  }
  const auto& state = transaction.state();
  if (state.phase != ReplayTransactionPhase::Created || !state.identity.valid() ||
      state.identity.sequence != scope->raw.rawOrdinal ||
      state.nextRecordOrdinal > scope->raw.totalRawRecords) {
    poison(*scope, Failure::InvalidObservation);
    return;
  }
  scope->transaction = &transaction;
  scope->active = true;
  scope->effectSeen = false;
  scope->destinationSeen = false;
  emit(*scope, transaction, Kind::Begin);
}

void observeReplayTransactionEffect(const ReplayTransaction& transaction, Witness witness) noexcept {
  auto* scope = scopeFor(transaction);
  if (!scope) return;
  const auto& state = transaction.state();
  if (state.terminal() || (witness == Witness::Transaction && state.phase != ReplayTransactionPhase::EffectStarted) ||
      (witness != Witness::Transaction && witness != Witness::DirectLease && witness != Witness::ArenaEmitted)) {
    poison(*scope, Failure::InvalidObservation);
    return;
  }
  if (scope->effectSeen) return;
  scope->effectSeen = true;
  emit(*scope, transaction, Kind::FirstEffect, witness);
}

void observeReplayTransactionDestination(const ReplayTransaction& transaction) noexcept {
  auto* scope = scopeFor(transaction);
  if (!scope) return;
  const auto& state = transaction.state();
  if (scope->destinationSeen || state.phase != ReplayTransactionPhase::DestinationReceived ||
      !state.destination.valid() || state.destination.identity != state.identity ||
      state.destination.commandCount != state.stagedCommandCount) {
    poison(*scope, Failure::InvalidObservation);
    return;
  }
  scope->destinationSeen = true;
  emit(*scope, transaction, Kind::DestinationAccepted);
}

void observeReplayTransactionTerminal(const ReplayTransaction& transaction, Kind kind) noexcept {
  auto* scope = scopeFor(transaction);
  if (!scope) return;
  const auto& state = transaction.state();
  if (!transaction.journal().empty() ||
      (kind == Kind::Committed && (state.phase != ReplayTransactionPhase::Committed || !scope->destinationSeen)) ||
      (kind == Kind::RolledBack && state.phase != ReplayTransactionPhase::RolledBack) ||
      (kind == Kind::FailedStop && state.phase != ReplayTransactionPhase::FailedStop) ||
      (kind != Kind::Committed && kind != Kind::RolledBack && kind != Kind::FailedStop)) {
    poison(*scope, Failure::InvalidObservation);
    return;
  }
  if (kind == Kind::RolledBack && (scope->effectSeen || scope->destinationSeen)) {
    poison(*scope, Failure::RollbackAfterEffect);
    return;
  }
  scope->active = false;
  scope->destinationRollbackPending = kind == Kind::RolledBack;
  emit(*scope, transaction, kind);
  if (!scope->destinationRollbackPending) scope->transaction = nullptr;
}

void noteDestinationRollback(const ReplayTransaction& transaction, bool succeeded) noexcept {
  auto* scope = detail::replayTransactionObserver;
  if (!scope || scope->failed) return;
  if (scope->active || !scope->destinationRollbackPending || scope->transaction != &transaction ||
      transaction.state().phase != ReplayTransactionPhase::RolledBack) {
    poison(*scope, Failure::InvalidObservation);
    return;
  }
  if (!succeeded) {
    poison(*scope, Failure::DestinationRollbackFailed);
    return;
  }
  emit(*scope, transaction, Kind::DestinationRollbackConfirmed);
  scope->destinationRollbackPending = false;
  scope->transaction = nullptr;
}

} // namespace dxmt9::d3d9
