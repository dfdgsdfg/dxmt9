#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace dxmt9::test {

// This controller consumes already-published CPU sources. ImmediateFifo is
// explicit controlled acknowledgement, never captured readiness or GPU work.
enum class CpuV4ConsumerCompletionPolicy : std::uint8_t { ImmediateFifo };
enum class CpuV4ConsumerPhase : std::uint8_t { Idle, Running, Stopping, Stopped, Failed };
enum class CpuV4ConsumerSourcePhase : std::uint8_t { Empty, Observed, Submitted, Acknowledged };
enum class CpuV4ConsumerAction : std::uint8_t {
  Start, Observe, Submit, Acknowledge, Retire, CloseProducer, Stop, EmptyWake, Fail,
};
enum class CpuV4ConsumerFailure : std::uint8_t {
  None, InvalidBinding, InvalidLifecycle, Capacity, Deadline, LeaseRevoked,
  Observation, MissingIdentity, Submit, Acknowledge, Retire,
  ProducerOpen, WriterNotEmpty, Allocation, ThreadStart, Backend, Cleanup, Cancelled,
};
enum class CpuV4ConsumerWait : std::uint8_t { Ready, Stopped, Deadline, LeaseRevoked, Failed };

struct CpuV4ConsumerIdentity {
  std::uint64_t queue = 0u, captureToken = 0u, captureEpoch = 0u, sequence = 0u;
  std::uint32_t sourceIndex = 0u;
  std::uint64_t sourceGeneration = 0u;
  std::uint32_t storageFirstPage = 0u, storagePageCount = 0u;
  std::uint64_t storageGeneration = 0u;

  bool valid() const noexcept {
    return queue && captureToken && captureEpoch && sequence &&
        sourceGeneration && storageGeneration;
  }
  friend bool operator==(const CpuV4ConsumerIdentity&, const CpuV4ConsumerIdentity&) = default;
};

struct CpuV4ConsumerBinding {
  std::uint64_t queue = 0u, captureToken = 0u, captureEpoch = 0u;
  std::uint64_t baselineSequence = 0u;

  bool valid() const noexcept { return queue && captureToken && captureEpoch; }
  bool matches(const CpuV4ConsumerIdentity& identity) const noexcept {
    return identity.valid() && identity.queue == queue &&
        identity.captureToken == captureToken && identity.captureEpoch == captureEpoch;
  }
  friend bool operator==(const CpuV4ConsumerBinding&, const CpuV4ConsumerBinding&) = default;
};

struct CpuV4ConsumerLimits {
  // Four lifecycle actions per source: Observe, Submit, Acknowledge and Retire.
  // This is not the separate GeneralCpuSourceCapacity submission/probe budget.
  std::size_t maxSources = 4096u, maxCommands = 65536u, maxLifecycleActions = 16384u;

  bool valid() const noexcept {
    return maxSources && maxSources <= 65536u && maxCommands && maxCommands <= 65536u &&
        maxLifecycleActions && maxLifecycleActions <= 262144u;
  }
  friend bool operator==(const CpuV4ConsumerLimits&, const CpuV4ConsumerLimits&) = default;
};

struct CpuV4ConsumerState {
  CpuV4ConsumerBinding binding{};
  CpuV4ConsumerLimits limits{};
  CpuV4ConsumerPhase phase = CpuV4ConsumerPhase::Idle;
  CpuV4ConsumerSourcePhase sourcePhase = CpuV4ConsumerSourcePhase::Empty;
  CpuV4ConsumerIdentity active{};
  std::uint64_t retiredSequence = 0u;
  std::size_t observed = 0u, submitted = 0u, acknowledged = 0u, retired = 0u;
  std::size_t commands = 0u, lifecycleActions = 0u;
  bool producerClosed = false;
  friend bool operator==(const CpuV4ConsumerState&, const CpuV4ConsumerState&) = default;
};

inline bool cpuV4ConsumerStateValid(const CpuV4ConsumerState& state) noexcept {
  using Phase = CpuV4ConsumerPhase;
  using Source = CpuV4ConsumerSourcePhase;
  if (!state.binding.valid() || !state.limits.valid() ||
      state.observed > state.limits.maxSources || state.commands > state.limits.maxCommands ||
      state.lifecycleActions > state.limits.maxLifecycleActions ||
      state.retired > state.acknowledged || state.acknowledged > state.submitted ||
      state.submitted > state.observed || state.observed - state.retired > 1u ||
      state.commands < state.observed ||
      state.lifecycleActions != state.observed + state.submitted + state.acknowledged + state.retired)
    return false;
  switch (state.sourcePhase) {
    case Source::Empty:
      if (state.active != CpuV4ConsumerIdentity{} || state.observed != state.retired) return false;
      break;
    case Source::Observed:
      if (state.observed != state.retired + 1u || state.submitted != state.retired) return false;
      break;
    case Source::Submitted:
      if (state.observed != state.retired + 1u || state.submitted != state.observed ||
          state.acknowledged != state.retired) return false;
      break;
    case Source::Acknowledged:
      if (state.observed != state.retired + 1u || state.acknowledged != state.observed) return false;
      break;
    default: return false;
  }
  if (state.sourcePhase != Source::Empty &&
      (!state.binding.matches(state.active) || state.active.sequence <= state.retiredSequence)) return false;
  switch (state.phase) {
    case Phase::Idle:
      return !state.observed && !state.commands && !state.lifecycleActions &&
          !state.retiredSequence && !state.producerClosed;
    case Phase::Running:
    case Phase::Stopping:
    case Phase::Stopped:
    case Phase::Failed:
      if (state.phase == Phase::Stopping && !state.producerClosed) return false;
      if (state.phase == Phase::Stopped &&
          (!state.producerClosed || state.sourcePhase != Source::Empty)) return false;
      return state.retiredSequence >= state.binding.baselineSequence &&
          (state.retired ? state.retiredSequence > state.binding.baselineSequence
                         : state.retiredSequence == state.binding.baselineSequence);
    default: return false;
  }
}

// Closed finite-state reducer shared by the threaded controller and exhaustive
// small interleavings. Rejection is transactional; no receipt or frontier is
// advanced merely because a source action was requested.
inline bool cpuV4ConsumerAdvance(CpuV4ConsumerState& state, CpuV4ConsumerAction action,
    CpuV4ConsumerIdentity identity = {}, std::size_t commands = 0u) noexcept {
  using Phase = CpuV4ConsumerPhase;
  using Source = CpuV4ConsumerSourcePhase;
  using Action = CpuV4ConsumerAction;
  if (!cpuV4ConsumerStateValid(state) ||
      state.phase == Phase::Stopped || state.phase == Phase::Failed) return false;
  if (action == Action::Fail) {
    if (state.phase == Phase::Idle) state.retiredSequence = state.binding.baselineSequence;
    state.phase = Phase::Failed;
    return true;
  }
  if (action == Action::Start) {
    if (state.phase != Phase::Idle || !state.binding.valid() || !state.limits.valid()) return false;
    state.phase = Phase::Running;
    state.retiredSequence = state.binding.baselineSequence;
    return true;
  }
  if (state.phase != Phase::Running && state.phase != Phase::Stopping) return false;
  if (action == Action::CloseProducer) {
    if (state.phase != Phase::Running || state.producerClosed) return false;
    state.producerClosed = true;
    return true;
  }
  if (action == Action::Stop) {
    if (state.phase != Phase::Running || !state.producerClosed) return false;
    state.phase = Phase::Stopping;
    return true;
  }
  if (action == Action::EmptyWake) {
    if (state.phase != Phase::Stopping || state.sourcePhase != Source::Empty ||
        state.observed != state.retired) return false;
    state.phase = Phase::Stopped;
    return true;
  }
  if (state.lifecycleActions == state.limits.maxLifecycleActions) return false;
  if (action == Action::Observe) {
    if (state.sourcePhase != Source::Empty || !state.binding.matches(identity) ||
        identity.sequence <= state.retiredSequence || state.observed == state.limits.maxSources ||
        !commands || commands > state.limits.maxCommands - state.commands) return false;
    state.active = identity;
    state.sourcePhase = Source::Observed;
    ++state.observed;
    state.commands += commands;
  } else {
    if (!identity.valid() || identity != state.active) return false;
    if (action == Action::Submit && state.sourcePhase == Source::Observed) {
      state.sourcePhase = Source::Submitted;
      ++state.submitted;
    } else if (action == Action::Acknowledge && state.sourcePhase == Source::Submitted) {
      state.sourcePhase = Source::Acknowledged;
      ++state.acknowledged;
    } else if (action == Action::Retire && state.sourcePhase == Source::Acknowledged) {
      state.sourcePhase = Source::Empty;
      state.retiredSequence = identity.sequence;
      state.active = {};
      ++state.retired;
    } else return false;
  }
  ++state.lifecycleActions;
  return true;
}

struct CpuV4ConsumerCommandObservation {
  std::uint32_t kind = 0u, drawCount = 0u;
  friend bool operator==(const CpuV4ConsumerCommandObservation&,
      const CpuV4ConsumerCommandObservation&) = default;
};

// Owned metadata projection only. The backend copies these values during a
// retained SourcePayload borrow outside the scheduling mutex. No pointers,
// command/state spans, or mutable executor resource maps may survive that visit.
// This intentionally cannot satisfy the older full CPU semantic digest oracle.
struct CpuV4ConsumerObservation {
  CpuV4ConsumerIdentity identity{};
  std::uint64_t rawOrdinal = 0u, sourceOrdinal = 0u;
  std::vector<CpuV4ConsumerCommandObservation> commands{};
};

struct CpuV4ConsumerReceipt {
  std::shared_ptr<const CpuV4ConsumerObservation> observation{};
  bool submitted = false, acknowledged = false, retired = false;
  bool cleanupComplete = false;
};

struct CpuV4ConsumerSnapshot {
  CpuV4ConsumerState state{};
  CpuV4ConsumerFailure failure = CpuV4ConsumerFailure::None;
  CpuV4ConsumerCompletionPolicy policy = CpuV4ConsumerCompletionPolicy::ImmediateFifo;
  std::size_t receipts = 0u;
  bool workerExited = false;
  bool cleanupComplete = false;
  bool cleanupFailed = false;
  bool payloadDigestPresent = false, fullSemanticOracleComplete = false;
  bool actualMetal = false, gpuResultOracle = false, policyQualified = false, timingEligible = false;
  std::shared_ptr<const CpuV4ConsumerObservation> activeObservation{};
};

// Backend owns a weak controlled-CPU lease plus an independent backend pin;
// Main/executor retains the sole capability. The adapter validates that lease
// at every operation, with short authority locks and no callback/queue wait
// under them. Its surface is:
//   binding(), producerClosedAndWriterEmpty(), waitReady(deadline),
//   observeReady(remainingCommands, out) -> CpuV4ConsumerFailure (None on success),
//   submit(identity), acknowledge(identity), retire(identity), requestStop(),
//   poison(failure), cleanup(deadline) noexcept -> bool.
// waitReady uses the queue's publication CV and a local stop predicate. It
// returns Stopped only after producer close, Ready drain and an empty writer.
// Observation uses a generation-pinned borrow outside the queue mutex. Submit
// uses the actual fixed source identity and explicit null-CB test contract;
// acknowledgement and retirement use those exact existing receipts once.
// cleanup is an idempotent bounded proof, based on actual backend receipts,
// that the adapter retains no unhandled claimed source. It releases transient
// borrows safely, but an actual represented/submitted source still unhandled
// makes it return false. Exact fail-stop ownership or quarantine is separate
// backend evidence, never cleanupComplete or fabricated acknowledgement,
// retirement or reclamation. Ready backlog remains owned by the poisoned
// queue. Cleanup remains available through the backend pin after weak authority
// revocation; it must not issue new replay/finish effects. Main joins before
// fixture teardown, which owns final destruction of retained failed sources.
// No backend operation or cleanup is called under the controller mutex.
template <typename Backend>
class CpuV4ControlledConsumer final {
public:
  using Clock = std::chrono::steady_clock;
  static_assert(noexcept(std::declval<Backend&>().cleanup(std::declval<Clock::time_point>())),
      "controlled CPU cleanup must be bounded and noexcept");

  CpuV4ControlledConsumer(Backend backend, CpuV4ConsumerLimits limits, Clock::time_point deadline)
      : backend_(std::move(backend)), deadline_(deadline) {
    state_.binding = backend_.binding();
    state_.limits = limits;
  }
  CpuV4ControlledConsumer(const CpuV4ControlledConsumer&) = delete;
  CpuV4ControlledConsumer& operator=(const CpuV4ControlledConsumer&) = delete;
  ~CpuV4ControlledConsumer() {
    if (worker_.joinable()) {
      const auto value = snapshot();
      if (value.state.phase != CpuV4ConsumerPhase::Stopped &&
          value.state.phase != CpuV4ConsumerPhase::Failed) fail(CpuV4ConsumerFailure::Cancelled);
      worker_.join();
    }
  }

  bool start() {
    CpuV4ConsumerFailure failure = CpuV4ConsumerFailure::None;
    {
      std::lock_guard lock(mutex_);
      if (state_.phase != CpuV4ConsumerPhase::Idle) return false;
      if (!cpuV4ConsumerAdvance(state_, CpuV4ConsumerAction::Start))
        failure = CpuV4ConsumerFailure::InvalidBinding;
      else {
        try { receipts_.reserve(state_.limits.maxSources); }
        catch (...) { failure = CpuV4ConsumerFailure::Allocation; }
      }
    }
    if (failure != CpuV4ConsumerFailure::None) { fail(failure); finishCleanup(); return false; }
    try { worker_ = std::thread([this] { run(); }); }
    catch (...) { fail(CpuV4ConsumerFailure::ThreadStart); finishCleanup(); return false; }
    return true;
  }

  bool closeProducer() {
    if (!backend_.producerClosedAndWriterEmpty()) {
      fail(CpuV4ConsumerFailure::WriterNotEmpty);
      return false;
    }
    std::lock_guard lock(mutex_);
    return cpuV4ConsumerAdvance(state_, CpuV4ConsumerAction::CloseProducer);
  }
  bool requestStop() {
    bool producerClosed = false;
    {
      std::lock_guard lock(mutex_);
      producerClosed = state_.producerClosed;
    }
    if (!producerClosed) { fail(CpuV4ConsumerFailure::ProducerOpen); return false; }
    if (!backend_.producerClosedAndWriterEmpty()) {
      fail(CpuV4ConsumerFailure::WriterNotEmpty);
      return false;
    }
    {
      std::lock_guard lock(mutex_);
      if (!cpuV4ConsumerAdvance(state_, CpuV4ConsumerAction::Stop)) return false;
    }
    backend_.requestStop();
    return true;
  }
  bool stopAndJoin() {
    const auto before = snapshot();
    const bool stopped = before.state.phase == CpuV4ConsumerPhase::Stopped ||
        before.state.phase == CpuV4ConsumerPhase::Stopping ||
        (before.state.phase != CpuV4ConsumerPhase::Failed && requestStop());
    join();
    const auto after = snapshot();
    return stopped && after.state.phase == CpuV4ConsumerPhase::Stopped && after.cleanupComplete;
  }
  void failAndJoin(CpuV4ConsumerFailure reason) {
    fail(reason);
    if (!worker_.joinable()) finishCleanup();
    join();
  }
  void join() { if (worker_.joinable()) worker_.join(); }

  CpuV4ConsumerSnapshot snapshot() const {
    std::lock_guard lock(mutex_);
    CpuV4ConsumerSnapshot value;
    value.state = state_;
    value.failure = failure_;
    value.receipts = receipts_.size();
    value.workerExited = workerExited_;
    value.cleanupComplete = cleanupComplete_;
    value.cleanupFailed = cleanupFailed_;
    value.activeObservation = activeObservation_;
    return value;
  }
  std::vector<CpuV4ConsumerReceipt> takeReceipts() {
    std::lock_guard lock(mutex_);
    std::vector<CpuV4ConsumerReceipt> result(receipts_.begin() + receiptsTaken_, receipts_.end());
    receiptsTaken_ = receipts_.size();
    return result;
  }
  // A target is a qualified local source identity. Only our own retired
  // receipt ledger may satisfy it; a watermark or captured sequence cannot.
  bool waitThrough(const CpuV4ConsumerIdentity& target, Clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    if (!state_.binding.matches(target) || target.sequence <= state_.binding.baselineSequence)
      return false;
    const auto rowForSequence = [&] {
      for (const auto& row : receipts_)
        if (row.observation->identity.sequence == target.sequence) return true;
      return false;
    };
    if (!progressCv_.wait_until(lock, deadline, [&] {
          return rowForSequence() || state_.retiredSequence > target.sequence ||
              state_.phase == CpuV4ConsumerPhase::Failed || workerExited_;
        })) return false;
    if (state_.phase == CpuV4ConsumerPhase::Failed) return false;
    for (const auto& row : receipts_)
      if (row.observation->identity == target && row.submitted && row.acknowledged &&
          row.retired && row.cleanupComplete)
        return true;
    return false;
  }

private:
  void fail(CpuV4ConsumerFailure reason) {
    bool poison = false;
    {
      std::lock_guard lock(mutex_);
      if (state_.phase != CpuV4ConsumerPhase::Stopped && state_.phase != CpuV4ConsumerPhase::Failed) {
        // Invalid configuration must still fail-stop, without pretending that
        // the reducer admitted the corrupt input as a lifecycle transition.
        if (!cpuV4ConsumerAdvance(state_, CpuV4ConsumerAction::Fail))
          state_.phase = CpuV4ConsumerPhase::Failed;
        failure_ = reason;
        poison = true;
      }
    }
    progressCv_.notify_all();
    if (poison) backend_.poison(reason);
  }
  bool preview(CpuV4ConsumerAction action, const CpuV4ConsumerIdentity& identity,
      std::size_t commands = 0u) {
    std::lock_guard lock(mutex_);
    auto next = state_;
    return cpuV4ConsumerAdvance(next, action, identity, commands);
  }
  bool commit(CpuV4ConsumerAction action, const CpuV4ConsumerIdentity& identity,
      std::size_t commands = 0u) {
    std::lock_guard lock(mutex_);
    return cpuV4ConsumerAdvance(state_, action, identity, commands);
  }
  bool deadlineExpired() {
    if (Clock::now() < deadline_) return false;
    fail(CpuV4ConsumerFailure::Deadline);
    return true;
  }
  bool finishCleanup() noexcept {
    const bool complete = backend_.cleanup(deadline_);
    {
      std::lock_guard lock(mutex_);
      cleanupComplete_ = complete;
      cleanupFailed_ |= !complete;
    }
    if (!complete) fail(CpuV4ConsumerFailure::Cleanup);
    return complete;
  }
  void run() noexcept {
    bool normalStopPending = false;
    try {
      for (;;) {
        {
          std::lock_guard lock(mutex_);
          if (state_.phase == CpuV4ConsumerPhase::Failed) break;
        }
        if (deadlineExpired()) break;
        const auto wait = backend_.waitReady(deadline_);
        if (wait == CpuV4ConsumerWait::Stopped) {
          if (deadlineExpired()) break;
          normalStopPending = true;
          break;
        }
        if (wait != CpuV4ConsumerWait::Ready) {
          fail(wait == CpuV4ConsumerWait::Deadline ? CpuV4ConsumerFailure::Deadline
              : wait == CpuV4ConsumerWait::LeaseRevoked ? CpuV4ConsumerFailure::LeaseRevoked
                                                      : CpuV4ConsumerFailure::Backend);
          break;
        }
        if (deadlineExpired()) break;
        std::size_t remainingCommands = 0u;
        {
          std::lock_guard lock(mutex_);
          if (cpuV4ConsumerStateValid(state_) &&
              state_.observed < state_.limits.maxSources &&
              state_.lifecycleActions < state_.limits.maxLifecycleActions)
            remainingCommands = state_.limits.maxCommands - state_.commands;
        }
        if (!remainingCommands) { fail(CpuV4ConsumerFailure::Capacity); break; }
        {
          std::lock_guard lock(mutex_);
          cleanupComplete_ = false;
        }
        CpuV4ConsumerObservation observed;
        const auto observationFailure = backend_.observeReady(remainingCommands, observed);
        if (observationFailure != CpuV4ConsumerFailure::None) {
          fail(observationFailure);
          break;
        }
        if (deadlineExpired()) break;
        if (!observed.identity.valid()) { fail(CpuV4ConsumerFailure::MissingIdentity); break; }
        const auto identity = observed.identity;
        if (!commit(CpuV4ConsumerAction::Observe, identity, observed.commands.size())) {
          fail(CpuV4ConsumerFailure::InvalidBinding);
          break;
        }
        auto owned = std::make_shared<const CpuV4ConsumerObservation>(std::move(observed));
        {
          std::lock_guard lock(mutex_);
          activeObservation_ = owned;
        }
        bool succeeded = true;
        for (const auto action : {CpuV4ConsumerAction::Submit, CpuV4ConsumerAction::Acknowledge,
                                 CpuV4ConsumerAction::Retire}) {
          if (deadlineExpired()) { succeeded = false; break; }
          if (!preview(action, identity)) {
            fail(CpuV4ConsumerFailure::Capacity);
            succeeded = false;
            break;
          }
          const bool applied = action == CpuV4ConsumerAction::Submit ? backend_.submit(identity)
              : action == CpuV4ConsumerAction::Acknowledge ? backend_.acknowledge(identity)
                                                         : backend_.retire(identity);
          if (!applied) {
            fail(action == CpuV4ConsumerAction::Submit ? CpuV4ConsumerFailure::Submit
                : action == CpuV4ConsumerAction::Acknowledge ? CpuV4ConsumerFailure::Acknowledge
                                                           : CpuV4ConsumerFailure::Retire);
            succeeded = false;
            break;
          }
          if (deadlineExpired()) { succeeded = false; break; }
          if (!commit(action, identity)) {
            fail(CpuV4ConsumerFailure::InvalidLifecycle);
            succeeded = false;
            break;
          }
        }
        if (!succeeded) break;
        if (!finishCleanup()) break;
        {
          std::lock_guard lock(mutex_);
          if (state_.phase == CpuV4ConsumerPhase::Failed) break;
          receipts_.push_back({std::move(owned), true, true, true, true});
          activeObservation_.reset();
        }
        progressCv_.notify_all();
      }
    } catch (const std::bad_alloc&) { fail(CpuV4ConsumerFailure::Allocation); }
    catch (...) { fail(CpuV4ConsumerFailure::Backend); }
    const bool cleaned = finishCleanup();
    if (normalStopPending && cleaned && !deadlineExpired()) {
      if (!commit(CpuV4ConsumerAction::EmptyWake, {}))
        fail(CpuV4ConsumerFailure::InvalidLifecycle);
    }
    {
      std::lock_guard lock(mutex_);
      workerExited_ = true;
    }
    progressCv_.notify_all();
  }

  Backend backend_;
  Clock::time_point deadline_;
  mutable std::mutex mutex_{};
  std::condition_variable progressCv_{};
  CpuV4ConsumerState state_{};
  CpuV4ConsumerFailure failure_ = CpuV4ConsumerFailure::None;
  bool workerExited_ = false;
  bool cleanupComplete_ = false;
  bool cleanupFailed_ = false;
  std::vector<CpuV4ConsumerReceipt> receipts_{};
  std::size_t receiptsTaken_ = 0u;
  std::shared_ptr<const CpuV4ConsumerObservation> activeObservation_{};
  std::thread worker_{};
};

} // namespace dxmt9::test
