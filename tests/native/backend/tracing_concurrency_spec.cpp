#include "../../../src/dxmt9/tracing/session_state.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>

int main() {
  using namespace dxmt9::tracing;

  // A producer snapshots generation N, then is descheduled before its count
  // increment. Close, quiesce and reopen as N+1 before it resumes. Its CAS
  // must reject the old token instead of admitting into N+1 storage.
  std::atomic<uint64_t> gate{makeAdmissionGate(41, 0, false)};
  const uint64_t delayedGeneration = captureAdmissionGeneration(gate);
  assert(delayedGeneration == 41);
  assert(mayAcquireWriter(TraceState::Collecting,
                          gate.load(std::memory_order_acquire),
                          delayedGeneration));
  closeAdmission(gate);
  assert(admissionClosed(gate));
  assert(!mayAcquireWriter(TraceState::Collecting,
                           gate.load(std::memory_order_acquire),
                           delayedGeneration));
  assert(admissionWriters(gate.load(std::memory_order_acquire)) == 0);
  openAdmission(gate, 42);
  assert(!tryAcquireWriter(gate, TraceState::Collecting, delayedGeneration));
  assert(admissionWriters(gate.load(std::memory_order_acquire)) == 0);
  assert(tryAcquireWriter(gate, TraceState::Collecting,
                          captureAdmissionGeneration(gate)));
  assert(admissionWriters(gate.load(std::memory_order_acquire)) == 1);
  closeAdmission(gate);
  assert(!tryAcquireWriter(gate, TraceState::Collecting, 42));
  releaseWriter(gate);
  assert(admissionWriters(gate.load(std::memory_order_acquire)) == 0);

  // Exercise the close/admit linearization under simultaneous contenders.
  // Once closeAdmission returns, no new producer may increment the gate. A
  // producer which won first remains counted until its explicit release.
  for (uint64_t generation = 1; generation <= 256; ++generation) {
    std::atomic<uint64_t> raceGate{makeAdmissionGate(generation, 0, false)};
    std::atomic<bool> start{false};
    std::atomic<bool> admitted{false};
    std::thread producer([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      admitted.store(tryAcquireWriter(raceGate, TraceState::Collecting,
                                      generation),
                     std::memory_order_release);
    });
    start.store(true, std::memory_order_release);
    closeAdmission(raceGate);
    producer.join();
    const uint32_t count =
        admissionWriters(raceGate.load(std::memory_order_acquire));
    assert(count == (admitted.load(std::memory_order_acquire) ? 1u : 0u));
    if (count) releaseWriter(raceGate);
    assert(admissionWriters(raceGate.load(std::memory_order_acquire)) == 0);
    assert(!tryAcquireWriter(raceGate, TraceState::Collecting, generation));
  }

  std::cout << "TracingSession atomic admission interleavings passed\n";
}
