#pragma once

#include "dxmt9_cpu_ready_tape.hpp"
#include "dxmt9_source_payload.hpp"
#include "dxmt9_session_release.hpp"
#include "dxmt9_render_tape_capture_attachment.hpp"
#include "dxmt9/query_end_marker.hpp"

#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <type_traits>
#include <vector>

namespace dxmt9::render {

enum class TapeRuntimeEdge : std::uint8_t {
  Append,
  Publish,
  Submit,
  Complete,
  InitializerPublish,
  InitializerSubmit,
  InitializerComplete,
  OrderedRelease,
  OrdinaryCoordinatorAdmission,
  QueryEnd,
  Failed,
};

// Positive queue-owned no-session admission at the original coordinator's
// preceding-record cut. This is not a release, publication or completion.
struct TapeOrdinaryCoordinatorAdmission {
  std::uint64_t queue = 0u;
  std::uint64_t captureToken = 0u;
  std::uint64_t captureEpoch = 0u;
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint64_t attemptGeneration = 0u;
  std::uint32_t recordIndex = 0u;
  std::uint32_t recordType = 0u;
};

// Cold synchronous observer. Payload borrows end with the callback, and no
// observer may call a queue method while this callback holds its queue lock.
struct TapeRuntimeObservation {
  TapeRuntimeEdge edge = TapeRuntimeEdge::Failed;
  query::WriterIdentity writer{};
  core::CpuReadyTape::SourceRef source{};
  core::CpuReadySourceMetadata metadata{};
  core::SourcePayloadView payload{};
  std::size_t controlIndex = SIZE_MAX;
  std::uint64_t sequence = 0u;
  std::uint64_t completedSequence = 0u;
  std::uint64_t initializer = 0u;
  std::uint32_t commandIndex = 0u;
  std::uint32_t firstSubdraw = 0u;
  std::uint32_t subdrawCount = 0u;
  core::metalqueue::SessionReleaseSnapshot release{};
  core::metalqueue::SessionReleaseWriter releaseWriter =
      core::metalqueue::SessionReleaseWriter::Publish;
  TapeOrdinaryCoordinatorAdmission ordinaryAdmission{};
};

struct TapeRuntimeObserverSink {
  using Fn = void (*)(void*, const TapeRuntimeObservation&) noexcept;
  void* context = nullptr;
  Fn fn = nullptr;
  explicit operator bool() const noexcept { return fn != nullptr; }
};

constexpr bool tapeRuntimeObserverMatches(TapeRuntimeObserverSink installed,
    TapeRuntimeObserverSink requested) noexcept {
  return installed.fn && requested.fn && installed.context == requested.context &&
      installed.fn == requested.fn;
}

constexpr bool tapeRuntimeUploadCompleted(bool completed, bool errored) noexcept {
  return completed && !errored;
}

constexpr bool tapeRuntimeInitializerCompletionAdmitted(bool pendingUploads,
    bool failed, std::size_t retainedCommandBuffers) noexcept {
  return !pendingUploads && !failed && !retainedCommandBuffers;
}

namespace detail {
struct TapeRuntimeObserverState {
  // Admission linearizes under this arm's mutex; execution never holds it.
  // A retirement first closes admission, then joins already-admitted work.
  // Retained lifetime is independent of this short in-flight duration.
  class Activity {
  public:
    explicit Activity(TapeRuntimeObserverState& state) noexcept : state_(&state) {
      {
        std::lock_guard lock(state.mutex);
        if (!state.accepting.load(std::memory_order_relaxed) || !state.sink ||
            state.inFlight == SIZE_MAX || (state.owner && !state.owner->edges().active())) return;
        owner_ = state.owner;
        sink_ = state.sink;
        ++state.inFlight;
      }
      admitted_ = true;
      previous_ = current_;
      current_ = this;
    }
    ~Activity() {
      if (!admitted_) return;
      current_ = previous_;
      {
        std::lock_guard lock(state_->mutex);
        --state_->inFlight;
      }
      state_->quiescent.notify_all();
      // owner_ is released after the lock above, including a final destructor.
    }
    Activity(const Activity&) = delete;
    Activity& operator=(const Activity&) = delete;
    explicit operator bool() const noexcept { return admitted_; }
    void dispatch(const TapeRuntimeObservation& observation) const noexcept {
      sink_.fn(sink_.context, observation);
    }
    static bool activeOnThisThread(const TapeRuntimeObserverState& state) noexcept {
      for (auto* activity = current_; activity; activity = activity->previous_)
        if (activity->state_ == &state) return true;
      return false;
    }
  private:
    TapeRuntimeObserverState* state_ = nullptr;
    Activity* previous_ = nullptr;
    std::shared_ptr<TapeRuntimeCaptureAttachment> owner_{};
    TapeRuntimeObserverSink sink_{};
    bool admitted_ = false;
    static inline thread_local Activity* current_ = nullptr;
  };

  std::mutex mutex{};
  std::condition_variable quiescent{};
  TapeRuntimeObserverSink originalSink{};
  TapeRuntimeObserverSink sink{};
  std::shared_ptr<TapeRuntimeCaptureAttachment> owner{};
  std::atomic<bool> accepting{true};
  std::size_t inFlight = 0u;
};
} // namespace detail

// Pins only this arm's typed capture owner. No raw owner/sink accessor can
// bypass retirement. visit/dispatch are short diagnostic operations; do not
// enclose a backend Encode that later takes queue locks in an in-flight visit.
// A TLS topology scope may borrow during this lease's retained lifetime, but
// its later receipts/dispatch must pass admission again. Revocation keeps an
// old lease's owner alive without granting access to a replacement capture.
class TapeRuntimeCaptureLease {
public:
  TapeRuntimeCaptureLease() = default;
  bool active() const noexcept {
    return owner_ && state_ && state_->accepting.load(std::memory_order_acquire) &&
        owner_->edges().active();
  }
  explicit operator bool() const noexcept { return active(); }
  bool matches(const TapeRuntimeCaptureLease& other) const noexcept {
    return state_ && owner_ && state_ == other.state_ && owner_ == other.owner_;
  }
  template <typename Fn>
  bool visit(Fn&& fn) const noexcept {
    static_assert(std::is_nothrow_invocable_v<Fn&, TapeRuntimeCaptureAttachment&>);
    if (!owner_ || !state_) return false;
    detail::TapeRuntimeObserverState::Activity activity(*state_);
    if (!activity) return false;
    fn(*owner_);
    return true;
  }
  bool dispatch(const TapeRuntimeObservation& observation) const noexcept {
    if (!owner_ || !state_) return false;
    detail::TapeRuntimeObserverState::Activity activity(*state_);
    if (!activity) return false;
    activity.dispatch(observation);
    return true;
  }
  std::uint64_t nextEdge() const noexcept {
    std::uint64_t edge = 0u;
    visit([&](TapeRuntimeCaptureAttachment& owner) noexcept { edge = owner.edges().next(); });
    return edge;
  }
private:
  friend class TapeRuntimeObserverEndpoint;
  std::shared_ptr<detail::TapeRuntimeObserverState> state_{};
  std::shared_ptr<TapeRuntimeCaptureAttachment> owner_{};
};

// Queue-owned rendezvous outlives all encode/completion threads. Each arm has
// bounded, never-reused State storage, so copied proxies never target a new
// capture. Retirement closes admission and joins callbacks/short visits;
// outstanding leases retain the old owner beyond that join. Self-retirement
// from an admitted operation is rejected (false), never a self-join. Callers
// must not hold locks needed by an admitted callback/visit while joining.
class TapeRuntimeObserverEndpoint {
public:
  bool enabled() const noexcept { return current_.load(std::memory_order_acquire) != nullptr; }
  TapeRuntimeObserverSink proxy() noexcept {
    auto* state = current_.load(std::memory_order_acquire);
    return state ? TapeRuntimeObserverSink{state, dispatch} : TapeRuntimeObserverSink{};
  }
  bool attach(TapeRuntimeObserverSink sink) noexcept {
    return attachState(sink, {});
  }
  bool attachRetained(std::shared_ptr<TapeRuntimeCaptureAttachment> owner) noexcept {
    if (!owner || !owner->edges().active()) return false;
    // The virtual owner callback is outside all endpoint/State locks.
    const auto sink = owner->sink();
    return attachState(sink, std::move(owner));
  }
  TapeRuntimeCaptureLease acquireCapture() noexcept {
    TapeRuntimeCaptureLease lease;
    auto state = currentState();
    if (!state) return lease;
    std::lock_guard lock(state->mutex);
    if (!state->accepting.load(std::memory_order_relaxed) || !state->owner ||
        !state->owner->edges().active()) return lease;
    lease.owner_ = state->owner;
    lease.state_ = std::move(state);
    return lease;
  }
  bool detach(TapeRuntimeObserverSink sink) noexcept {
    auto state = currentState();
    return sink && state && retire(state, sink);
  }
  bool revoke(TapeRuntimeObserverSink sink) noexcept {
    if (!sink) return false;
    std::size_t count = 0u;
    {
      std::lock_guard lock(mutex_);
      count = states_.size();
    }
    bool matched = false;
    for (std::size_t index = 0u; index < count; ++index) {
      std::shared_ptr<State> state;
      {
        std::lock_guard lock(mutex_);
        state = states_[index];
      }
      if (!tapeRuntimeObserverMatches(state->originalSink, sink)) continue;
      if (!retire(state, sink)) return false;
      matched = true;
    }
    return matched;
  }
private:
  friend struct TapeRuntimeObserverEndpointTestAccess;
  using State = detail::TapeRuntimeObserverState;
  bool attachState(TapeRuntimeObserverSink sink,
      std::shared_ptr<TapeRuntimeCaptureAttachment> owner) noexcept {
    if (!sink) return false;
    // candidate and owner are destroyed only after the lock scope unwinds.
    std::shared_ptr<State> candidate;
    try {
      candidate = std::make_shared<State>();
      candidate->originalSink = candidate->sink = sink;
      candidate->owner = std::move(owner);
      std::lock_guard lock(mutex_);
      if (current_.load(std::memory_order_acquire) || states_.size() >= 4096u) return false;
      auto* pointer = candidate.get();
      states_.push_back(candidate);
      current_.store(pointer, std::memory_order_release);
      return true;
    } catch (...) { return false; }
  }
  std::shared_ptr<State> currentState() noexcept {
    const auto* pointer = current_.load(std::memory_order_acquire);
    if (!pointer) return {};
    std::lock_guard lock(mutex_);
    for (const auto& state : states_)
      if (state.get() == pointer) return state;
    return {};
  }
  bool retire(const std::shared_ptr<State>& state, TapeRuntimeObserverSink sink) noexcept {
    if (!tapeRuntimeObserverMatches(state->originalSink, sink) ||
        State::Activity::activeOnThisThread(*state)) return false;
    std::shared_ptr<TapeRuntimeCaptureAttachment> retained;
    {
      std::lock_guard lock(state->mutex);
      state->accepting.store(false, std::memory_order_release);
      state->sink = {};
      if (state->owner) state->owner->edges().revoke();
      retained = std::move(state->owner);
    }
    auto* expected = state.get();
    current_.compare_exchange_strong(expected, nullptr,
        std::memory_order_acq_rel, std::memory_order_acquire);
    {
      std::unique_lock lock(state->mutex);
      state->quiescent.wait(lock, [&] { return !state->inFlight; });
    }
    // retained's final destructor runs outside both locks, after the join.
    return true;
  }
  static void dispatch(void* context,
      const TapeRuntimeObservation& observation) noexcept {
    auto& state = *static_cast<State*>(context);
    State::Activity activity(state);
    if (activity) activity.dispatch(observation);
  }
  std::mutex mutex_{};
  std::atomic<State*> current_{nullptr};
  std::vector<std::shared_ptr<State>> states_{};
};

struct TapeRuntimeBaseline {
  std::uint64_t queue = 0u;
  std::uint64_t sequence = 0u;
  std::uint64_t completedSequence = 0u;
  std::uint64_t initializer = 0u;
  std::uint64_t completedInitializer = 0u;
  bool sessionLaneEnabled = false;
};

} // namespace dxmt9::render
