#include "render_tape_cpu_snapshot_proof.hpp"
#include "render_tape_cpu_binding_selection.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

int main() {
  using namespace dxmt9;
  try {
    const auto check = [](bool value) {
      if (!value) throw std::runtime_error("CPU snapshot proof invariant");
    };
    for (bool enabled : {false, true})
      for (bool buffer : {false, true})
        for (bool inlineBytes : {false, true}) {
          check(test::cpuTapeObservesIndexBuffer(enabled, buffer, inlineBytes) ==
                (enabled && buffer && !inlineBytes));
          check(test::cpuTapeObservesStreamBuffer(0u, enabled ? 1u : 0u, buffer, inlineBytes) ==
                (enabled && buffer && !inlineBytes));
          check(test::cpuTapeObservesStreamBuffer(1u, enabled ? 2u : 0u, buffer, inlineBytes) ==
                (enabled && buffer));
        }
    check(!test::cpuTapeObservesStreamBuffer(32u, ~0u, true, false));
    std::array<std::uint8_t, 4> actual{1, 2, 3, 4};
    auto expected = actual;
    core::DrawBufferBindingSnapshot snapshot{};
    snapshot.metalHandle = 1u;
    snapshot.contentsAddress = reinterpret_cast<std::uintptr_t>(actual.data());
    snapshot.byteSize = actual.size();
    snapshot.contentRevision = 1u;
    test::CpuTapeSnapshotProofCache proofs;
    std::uint64_t a = 0u, repeated = 0u, b = 0u, restored = 0u;
    check(proofs.prove(core::Handle{1}, snapshot, expected, a));
    check(proofs.prove(core::Handle{1}, snapshot, expected, repeated));
    check(a == repeated && proofs.size() == 1u);
    // Same address/revision may be reused after completion. A mutation must
    // invalidate first, and then the old expected bytes cannot certify B.
    proofs.invalidate();
    actual[0] = 9u;
    check(!proofs.prove(core::Handle{1}, snapshot, expected, repeated));
    expected = actual;
    check(proofs.prove(core::Handle{1}, snapshot, expected, b) && a != b);
    proofs.invalidate();
    actual[0] = expected[0] = 1u;
    check(proofs.prove(core::Handle{1}, snapshot, expected, restored) && a == restored);
    proofs.invalidate();
    snapshot.contentsAddress = 0u;
    check(!proofs.prove(core::Handle{1}, snapshot, expected, repeated));
    snapshot.contentsAddress = reinterpret_cast<std::uintptr_t>(actual.data());
    ++snapshot.byteSize;
    check(!proofs.prove(core::Handle{1}, snapshot, expected, repeated));
    --snapshot.byteSize;
    for (std::size_t i = 0u; i < proofs.kCapacity; ++i)
      check(proofs.prove(core::Handle{i + 1u}, snapshot, expected, repeated));
    check(!proofs.prove(core::Handle{proofs.kCapacity + 1u}, snapshot, expected, repeated));

    test::CpuTapeSnapshotProofCache liveProofs;
    test::CpuTapeLiveBufferView live{
        .metalHandle = 1u,
        .contentsAddress = reinterpret_cast<std::uintptr_t>(actual.data()),
        .byteSize = actual.size(),
        .contentRevision = 1u,
    };
    std::uint64_t liveA = 0u, liveB = 0u, liveRestored = 0u;
    expected = actual;
    check(liveProofs.proveLive(core::Handle{1}, live, expected, liveA));
    liveProofs.invalidate();
    actual[0] = 9u;
    check(!liveProofs.proveLive(core::Handle{1}, live, expected, liveB));
    expected = actual;
    ++live.contentRevision;
    check(liveProofs.proveLive(core::Handle{1}, live, expected, liveB) && liveA != liveB);
    liveProofs.invalidate();
    actual[0] = expected[0] = 1u;
    ++live.contentRevision;
    check(liveProofs.proveLive(core::Handle{1}, live, expected, liveRestored) &&
          liveA == liveRestored);

    const std::array routes{
        test::CpuTapeBufferObservationRoute::Reject,
        test::CpuTapeBufferObservationRoute::Live,
        test::CpuTapeBufferObservationRoute::Reject,
        test::CpuTapeBufferObservationRoute::Reject,
        test::CpuTapeBufferObservationRoute::Reject,
        test::CpuTapeBufferObservationRoute::Reject,
        test::CpuTapeBufferObservationRoute::Reject,
        test::CpuTapeBufferObservationRoute::Captured,
    };
    std::size_t routeIndex = 0u;
    for (const bool versioned : {false, true}) {
      for (const bool snapshotPresent : {false, true}) {
        for (const bool backingValid : {false, true}) {
          check(test::cpuTapeBufferObservationRoute(
                    versioned, snapshotPresent, backingValid) ==
                routes[routeIndex++]);
        }
      }
    }
    std::cout << "CPU snapshot proof epoch, live A/B/A, mismatch, capacity and routes: OK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
