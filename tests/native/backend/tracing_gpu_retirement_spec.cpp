#include "../../../src/dxmt9/tracing/gpu_retirement_state.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
  using namespace dxmt9::trace::gpu::retirement;
  try {
    // TracingGpuRetirement::TicketMatches, with a second slot to check that
    // equal generations never make distinct storage locations interchangeable.
    std::uint64_t cases = 0;
    for (std::uint64_t session = 0; session <= 2; ++session) {
      for (std::uint32_t slot = 0; slot <= 1; ++slot) {
        for (std::uint64_t generation = 0; generation <= 3; ++generation) {
          for (std::uint64_t ticketSession = 0; ticketSession <= 2; ++ticketSession) {
            for (std::uint32_t ticketSlot = 0; ticketSlot <= 1; ++ticketSlot) {
              for (std::uint64_t ticketGeneration = 0; ticketGeneration <= 3;
                   ++ticketGeneration) {
                const bool modeled = session != 0 && generation != 0 &&
                    session == ticketSession && slot == ticketSlot &&
                    generation == ticketGeneration;
                require(completionTicketMatches(session, slot, generation,
                                                 ticketSession, ticketSlot,
                                                 ticketGeneration) == modeled,
                        "GPU completion ticket/model disagreement");
                ++cases;
              }
            }
          }
        }
      }
    }

    // CanBeginSession projects the state/lease invariants onto the production
    // allFree fact: Initializing, Completing, and Draining are never Free.
    constexpr std::array states{SlotState::Free, SlotState::Initializing,
        SlotState::Reserved, SlotState::Completing, SlotState::Ready,
        SlotState::Draining};
    for (bool open : {false, true}) {
      for (std::uint32_t reservations = 0; reservations <= 2; ++reservations) {
        for (auto state : states) {
          require(canBeginSession(open, reservations, state == SlotState::Free) ==
                      (!open && reservations == 0 && state == SlotState::Free),
                  "GPU session reuse/model disagreement");
          ++cases;
        }
      }
      for (std::uint32_t observers = 0; observers <= 2; ++observers) {
        for (std::uint32_t pending = 0; pending <= 2; ++pending) {
          for (std::uint32_t readers = 0; readers <= 2; ++readers) {
            for (auto state : states) {
              const bool modeled = !open && observers == 0 && pending == 0 &&
                  readers == 0 && state == SlotState::Free;
              require(mayRetireStorage(open, observers, pending,
                                       state == SlotState::Free, readers) == modeled,
                      "GPU storage retirement/model disagreement");
              ++cases;
            }
          }
        }
      }
    }

    // Replay the early-publication counterexample using the production enum.
    // A delayed ticket can still match the previous generation until its
    // replacement is initialized, so the CAS state must reject it first.
    std::atomic<SlotState> state{SlotState::Initializing};
    require(completionTicketMatches(1, 0, 1, 1, 0, 1),
            "counterexample must retain the previous valid ticket");
    SlotState expected = SlotState::Reserved;
    require(!state.compare_exchange_strong(expected, SlotState::Completing),
            "initializing storage must not admit a completion");
    state.store(SlotState::Reserved, std::memory_order_release);
    require(!completionTicketMatches(1, 0, 2, 1, 0, 1),
            "published replacement must reject the old allocation ticket");
    require(!mayRetireStorage(false, 0, 0, true, 1),
            "a quarantined CPU reader must retain storage after close");
    require(!mayRetireStorage(false, 0, 1, true, 0),
            "unconsumed GPU results must retain storage after close");
    std::cout << "TracingGpuRetirement production predicate tuples: " << cases
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
