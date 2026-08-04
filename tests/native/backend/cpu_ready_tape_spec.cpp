#include "../../../src/dxmt9/dxmt9_cpu_ready_tape.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::core::ChunkSlot;
using dxmt9::core::ChunkSlotControl;
using dxmt9::core::CpuReadySourceId;
using dxmt9::core::CpuReadyTape;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void reservationIsBoundedAndStateChecked() {
  CpuReadyTape tape{2};
  const auto first = tape.reserve();
  const auto second = tape.reserve();

  check(first.has_value() && second.has_value(),
        "fixed tape admits sources up to capacity");
  check(!tape.reserve().has_value(),
        "fixed tape rejects reservation beyond capacity");
  check(tape.residentCount() == 2u,
        "resident count includes writing sources");
  check(tape.resolve(first->id, CpuReadyTape::State::Writing) ==
            first->payload,
        "writing locator resolves its payload");
  check(tape.resolve(first->id, CpuReadyTape::State::Ready) == nullptr,
        "locator resolution checks expected lifecycle state");
  check(!tape.transition(first->id, CpuReadyTape::State::Writing,
                         CpuReadyTape::State::GPU),
        "tape rejects lifecycle edge skips");

  first->payload->appendClear({});
  check(tape.transition(first->id, CpuReadyTape::State::Writing,
                        CpuReadyTape::State::Ready),
        "writer publishes a complete source atomically");
  check(!tape.transition(first->id, CpuReadyTape::State::Writing,
                         CpuReadyTape::State::Ready),
        "duplicate publish is rejected");
  check(!tape.reclaim(first->id),
        "ready source cannot be reclaimed before encode ownership");
  check(tape.transition(first->id, CpuReadyTape::State::Ready,
                        CpuReadyTape::State::Encoding),
        "ready source transfers to encode ownership");
  check(tape.transition(first->id, CpuReadyTape::State::Encoding,
                        CpuReadyTape::State::GPU),
        "encoded source transfers to GPU residency");
  check(tape.reclaim(first->id),
        "finish-owned reclaim releases GPU-resident source");
  check(tape.residentCount() == 1u,
        "reclaim releases exactly one resident descriptor");
}

void generationRejectsAbaAfterReuse() {
  CpuReadyTape tape{1};
  const auto original = tape.reserve();
  check(original.has_value(), "test reserves original source");
  const CpuReadySourceId staleId = original->id;
  ChunkSlot* const storage = original->payload;
  check(tape.reclaim(original->id),
        "writing reservation may fail-open reclaim before publication");

  const auto reused = tape.reserve();
  check(reused.has_value(), "reclaimed descriptor can be reused");
  check(reused->payload == storage,
        "fixed tape reuses descriptor storage without allocating");
  check(reused->id.index == staleId.index &&
            reused->id.generation != staleId.generation,
        "reuse advances generation for the same descriptor index");
  check(tape.resolve(staleId, CpuReadyTape::State::Writing) == nullptr,
        "stale locator cannot resolve reused storage");
  check(!tape.transition(staleId, CpuReadyTape::State::Writing,
                         CpuReadyTape::State::Ready),
        "stale locator cannot publish the new source");
  check(!tape.reclaim(staleId),
        "stale locator cannot reclaim the new source");
}

void controlShellDoesNotOwnPayload() {
  CpuReadyTape tape{1};
  const auto reservation = tape.reserve();
  check(reservation.has_value(), "test reserves control payload");

  ChunkSlotControl control;
  control.state = ChunkSlot::State::Writing;
  control.sourceId = reservation->id;
  control.payload = reservation->payload;
  control.payload->appendClear({});

  check(control.commandCount() == 1u,
        "control shell exposes the bound payload view");
  check(tape.resolve(control.sourceId, CpuReadyTape::State::Writing) ==
            reservation->payload,
        "tape remains the physical payload owner");
  check(sizeof(ChunkSlotControl) < sizeof(ChunkSlot),
        "control shell stays materially smaller than payload storage");
}

}  // namespace

int main() {
  try {
    reservationIsBoundedAndStateChecked();
    generationRejectsAbaAfterReuse();
    controlShellDoesNotOwnPayload();
  } catch (const std::exception& error) {
    std::cerr << "cpu_ready_tape_spec failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
