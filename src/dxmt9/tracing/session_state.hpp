#pragma once

#include <cstdint>
#include <atomic>
#include <chrono>
#include <thread>

namespace dxmt9::tracing {

// TLA+: TracingSessionLifecycle. These predicates are the executable binding
// for the model's admission, append-after-close, and seal-with-live-writers
// invariants; the native lifecycle spec enumerates their finite state space.
enum class TraceState : uint8_t {
  Idle,
  Preparing,
  Armed,
  Collecting,
  Closing,
  Draining,
  Finalizing,
  Sealed,
  Failed,
};

constexpr bool shouldAdmit(TraceState state, uint64_t current_generation,
                           uint64_t requested_generation) {
  return state == TraceState::Collecting && current_generation != 0 &&
         current_generation == requested_generation;
}

// A writer admitted while Collecting may complete during close/drain, but a
// generation can never append after its storage is sealed or reused.
constexpr bool mayAppend(TraceState state, uint64_t current_generation,
                         uint64_t requested_generation, bool storage_sealed) {
  const bool writer_phase = state == TraceState::Collecting ||
                            state == TraceState::Closing ||
                            state == TraceState::Draining;
  return writer_phase && !storage_sealed && current_generation != 0 &&
         current_generation == requested_generation;
}

// Control acquisition itself is part of the stop deadline. No diagnostic
// caller waits indefinitely behind GPU polling or another cold operation.
template<class Lock, class Clock, class Duration>
bool lockDiagnosticBefore(Lock& lock,
    std::chrono::time_point<Clock, Duration> deadline) noexcept {
  do {
    if (lock.try_lock()) return true;
    std::this_thread::yield();
  } while (Clock::now() < deadline);
  return false;
}

// 0 pending, 1 complete decision, 2 incomplete/expired. The candidate artifact
// must already be durable when completion attempts this one-way transition.
template<class Word>
inline bool decideDiagnosticCompletion(std::atomic<Word>& outcome,
    bool sources_complete, bool candidate_durable, uint64_t completed_ns,
    uint64_t deadline_ns, uint64_t generation = 0) noexcept {
  Word pending = static_cast<Word>(generation << 2);
  const Word decision = static_cast<Word>((generation << 2) |
      (sources_complete && candidate_durable && completed_ns <= deadline_ns ? 1 : 2));
  outcome.compare_exchange_strong(pending, decision, std::memory_order_acq_rel);
  return outcome.load(std::memory_order_acquire) == ((generation << 2) | 1);
}

// A caller timeout changes the outcome, never the lifetime of live owners.


constexpr bool mayRearmDiagnostic(bool controller_done, bool worker_done,
                                   uint32_t active_writers) noexcept {
  return controller_done && worker_done && active_writers == 0;
}

constexpr bool maySeal(TraceState state, uint32_t active_writers) {

  return state == TraceState::Draining && active_writers == 0;
}

// TLA+: TracingSessionLifecycle linearizes writer admission against close and
// generation reuse in one atomic word. A separate state read plus writer
// counter can miss a delayed increment and seal/reuse its storage too early.
constexpr uint64_t kAdmissionGateClosed = uint64_t{1} << 63;
constexpr uint64_t kAdmissionGateGenerationMask = (uint64_t{1} << 31) - 1;
constexpr uint64_t kAdmissionGateWriterMask = (uint64_t{1} << 32) - 1;

constexpr uint64_t makeAdmissionGate(uint64_t generation,
                                     uint32_t writers,
                                     bool closed) noexcept {
  return (closed ? kAdmissionGateClosed : 0) |
      ((generation & kAdmissionGateGenerationMask) << 32) | writers;
}

constexpr uint64_t admissionGeneration(uint64_t gate) noexcept {
  return (gate >> 32) & kAdmissionGateGenerationMask;
}

constexpr uint32_t admissionWriters(uint64_t gate) noexcept {
  return static_cast<uint32_t>(gate & kAdmissionGateWriterMask);
}

constexpr bool admissionClosed(uint64_t gate) noexcept {
  return (gate & kAdmissionGateClosed) != 0;
}

constexpr bool mayAcquireWriter(TraceState state, uint64_t gate,
                                uint64_t requested_generation) noexcept {
  return !admissionClosed(gate) &&
      shouldAdmit(state, admissionGeneration(gate), requested_generation) &&
      admissionWriters(gate) != kAdmissionGateWriterMask;
}

// Production helpers use the same packed word tested by the deterministic
// stale-snapshot/reopen fixture. The caller captures a request generation once
// and must never refresh it after a rejected probe.
inline uint64_t captureAdmissionGeneration(
    const std::atomic<uint64_t>& gate) noexcept {
  const uint64_t observed = gate.load(std::memory_order_acquire);
  return admissionClosed(observed) ? 0 : admissionGeneration(observed);
}

inline bool tryAcquireWriter(std::atomic<uint64_t>& gate, TraceState state,
                             uint64_t requested_generation) noexcept {
  // Supported OS thread/probe populations are strictly below 2^32. Counting
  // the probe itself removes contention failure and any unleased loss update.
  // Reopen CASes only a closed zero-writer word, so it cannot erase a probe
  // whose rejection decrement is still pending.
  const uint64_t observed = gate.fetch_add(1, std::memory_order_acq_rel);
  if (mayAcquireWriter(state, observed, requested_generation)) return true;
  gate.fetch_sub(1, std::memory_order_release);
  return false;
}

inline void closeAdmission(std::atomic<uint64_t>& gate) noexcept {
  gate.fetch_or(kAdmissionGateClosed, std::memory_order_acq_rel);
}

inline bool openAdmission(std::atomic<uint64_t>& gate,
                          uint64_t generation) noexcept {
  uint64_t observed = gate.load(std::memory_order_acquire);
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    if (!admissionClosed(observed) || admissionWriters(observed) != 0) return false;
    if (gate.compare_exchange_strong(observed,
          makeAdmissionGate(generation, 0, false), std::memory_order_acq_rel,
          std::memory_order_acquire)) return true;
  }
  return false;
}

inline void releaseWriter(std::atomic<uint64_t>& gate) noexcept {
  gate.fetch_sub(1, std::memory_order_release);
}

}  // namespace dxmt9::tracing
