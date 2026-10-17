#include "../../../src/dxmt9/tracing/session_state.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
  using dxmt9::tracing::TraceState;
  using dxmt9::tracing::mayAppend;
  using dxmt9::tracing::maySeal;
  using dxmt9::tracing::shouldAdmit;

  constexpr TraceState states[] = {
      TraceState::Idle,       TraceState::Preparing, TraceState::Armed,
      TraceState::Collecting, TraceState::Closing,   TraceState::Draining,
      TraceState::Finalizing, TraceState::Sealed,    TraceState::Failed};
  constexpr uint64_t generations[] = {0, 1, 2};
  constexpr uint32_t writers[] = {0, 1, 2};

  for (TraceState state : states) {
    for (uint64_t current : generations) {
      for (uint64_t requested : generations) {
        const bool expected = state == TraceState::Collecting && current != 0 &&
                              current == requested;
        assert(shouldAdmit(state, current, requested) == expected);
        for (bool sealed : {false, true}) {
          const bool writer_phase = state == TraceState::Collecting ||
                                    state == TraceState::Closing ||
                                    state == TraceState::Draining;
          const bool may_append = writer_phase && !sealed && current != 0 &&
                                  current == requested;
          assert(mayAppend(state, current, requested, sealed) == may_append);
        }
      }
    }
    for (uint32_t active : writers) {
      assert(maySeal(state, active) ==
             (state == TraceState::Draining && active == 0));
    }
  }

  assert(!shouldAdmit(TraceState::Closing, 7, 7));
  assert(mayAppend(TraceState::Closing, 7, 7, false));
  assert(!mayAppend(TraceState::Draining, 7, 7, true));
  assert(!mayAppend(TraceState::Draining, 8, 7, false));
  assert(!maySeal(TraceState::Draining, 1));
  std::cout << "TracingSessionLifecycle predicates passed\n";
}
