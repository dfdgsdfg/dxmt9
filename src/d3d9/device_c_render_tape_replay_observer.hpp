#pragma once

#include <cstdint>

namespace dxmt9::d3d9 {

class ReplayTransaction;
struct ReplayTransactionState;

struct ReplayTransactionRawIdentity {
  std::uint64_t captureToken = 0u;
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint32_t totalRawRecords = 0u;
  friend constexpr bool operator==(ReplayTransactionRawIdentity,
                                  ReplayTransactionRawIdentity) = default;
};

enum class ReplayTransactionObservationKind : std::uint8_t {
  Begin, FirstEffect, DestinationAccepted, Committed, RolledBack, FailedStop,
  DestinationRollbackConfirmed,
};

enum class ReplayTransactionEffectWitness : std::uint8_t {
  Transaction, DirectLease, ArenaEmitted,
};

enum class ReplayTransactionObserverFailure : std::uint8_t {
  InvalidScope, NestedScopeMismatch, ScopeOrderMismatch, NonterminalScopeExit,
  BeginOverwritesAttempt, UnresolvedDestinationRollback, InvalidObservation,
  RollbackAfterEffect, DestinationRollbackFailed, SinkRejected,
};

// All borrows end with the synchronous callback. The transaction address is
// process-local scope correlation only: never export it or retain it in a
// runtime identity root, sidecar or asynchronous observer.
// Callbacks must not re-enter replay or call queue methods.
struct ReplayTransactionObservation {
  ReplayTransactionRawIdentity raw{};
  ReplayTransactionObservationKind kind = ReplayTransactionObservationKind::Begin;
  ReplayTransactionEffectWitness effect = ReplayTransactionEffectWitness::Transaction;
  const ReplayTransaction* transaction = nullptr;
  const ReplayTransactionState* state = nullptr;
};

struct ReplayTransactionObserverSink {
  void* context = nullptr;
  bool (*observe)(void*, const ReplayTransactionObservation&) noexcept = nullptr;
  void (*failed)(void*, ReplayTransactionObserverFailure) noexcept = nullptr;
  explicit operator bool() const noexcept { return observe && failed; }
};

namespace detail {
struct ReplayTransactionObserverState {
  ReplayTransactionObserverSink sink{};
  ReplayTransactionRawIdentity raw{};
  const ReplayTransaction* transaction = nullptr;
  bool active = false;
  bool effectSeen = false;
  bool destinationSeen = false;
  bool destinationRollbackPending = false;
  bool failed = false;
};
extern thread_local ReplayTransactionObserverState* replayTransactionObserver;
} // namespace detail

// Install only at the cold capture-owned raw boundary. Same-raw/same-sink
// nested scopes alias the outer attempt state; a different raw or sink poisons
// both captures and suppresses callbacks until the rejected scope exits.
// Every actual begin still emits a distinct attempt. Leaving an open attempt
// or an unconfirmed destination rollback poisons capture, never replay.
class ScopedReplayTransactionObserver {
public:
  ScopedReplayTransactionObserver(ReplayTransactionRawIdentity,
      ReplayTransactionObserverSink) noexcept;
  ~ScopedReplayTransactionObserver();
  ScopedReplayTransactionObserver(const ScopedReplayTransactionObserver&) = delete;
  ScopedReplayTransactionObserver& operator=(const ScopedReplayTransactionObserver&) = delete;
private:
  detail::ReplayTransactionObserverState state_{};
  detail::ReplayTransactionObserverState* previous_ = nullptr;
  detail::ReplayTransactionObserverState* installed_ = nullptr;
  bool owner_ = false;
};

// The shipping disabled path is one cached TLS pointer test; observations and
// borrowed packets are constructed only in the cold enabled functions.
inline bool replayTransactionObserverEnabled() noexcept {
  return detail::replayTransactionObserver != nullptr;
}

void observeReplayTransactionBegin(const ReplayTransaction&) noexcept;
void observeReplayTransactionEffect(const ReplayTransaction&,
    ReplayTransactionEffectWitness = ReplayTransactionEffectWitness::Transaction) noexcept;
void observeReplayTransactionDestination(const ReplayTransaction&) noexcept;
void observeReplayTransactionTerminal(const ReplayTransaction&,
    ReplayTransactionObservationKind) noexcept;

// Explicit proof boundary for effects outside ReplayTransaction's reducer.
// Neither helper alters the production transaction, renderer result or route.
// Destination rollback is separate from semantic journal restoration.
inline void noteExternalEffect(const ReplayTransaction& transaction,
    ReplayTransactionEffectWitness witness) noexcept {
  if (replayTransactionObserverEnabled()) observeReplayTransactionEffect(transaction, witness);
}
void noteDestinationRollback(const ReplayTransaction&, bool succeeded) noexcept;

} // namespace dxmt9::d3d9
