#pragma once

#include <cstdint>
#include <atomic>

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
// and must never refresh it after a failed CAS.
inline uint64_t captureAdmissionGeneration(
    const std::atomic<uint64_t>& gate) noexcept {
  const uint64_t observed = gate.load(std::memory_order_acquire);
  return admissionClosed(observed) ? 0 : admissionGeneration(observed);
}

inline bool tryAcquireWriter(std::atomic<uint64_t>& gate, TraceState state,
                             uint64_t requested_generation) noexcept {
  uint64_t observed = gate.load(std::memory_order_acquire);
  for (;;) {
    if (!mayAcquireWriter(state, observed, requested_generation)) {
      return false;
    }
    const uint64_t desired = observed + 1;
    if (gate.compare_exchange_weak(observed, desired,
                                   std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
      return true;
    }
  }
}

inline void closeAdmission(std::atomic<uint64_t>& gate) noexcept {
  gate.fetch_or(kAdmissionGateClosed, std::memory_order_acq_rel);
}

inline void openAdmission(std::atomic<uint64_t>& gate,
                          uint64_t generation) noexcept {
  gate.store(makeAdmissionGate(generation, 0, false),
             std::memory_order_release);
}

inline void releaseWriter(std::atomic<uint64_t>& gate) noexcept {
  gate.fetch_sub(1, std::memory_order_release);
}

}  // namespace dxmt9::tracing
