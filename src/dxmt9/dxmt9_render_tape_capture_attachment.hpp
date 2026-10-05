#pragma once

#include <atomic>
#include <cstdint>

namespace dxmt9::render {

class TapeTopologyCapture;
struct TapeRuntimeObserverSink;
struct TapeRuntimeEdgeClockTestAccess;

// One capture's provenance and topology share this clock. Baseline is a
// reserved fact, not an allocation; no consumer owns an independent counter.
// A revoked/exhausted clock cannot be reset or reused for another capture.
class TapeRuntimeEdgeClock {
public:
  static constexpr std::uint64_t baseline() noexcept { return 1u; }
  std::uint64_t next() noexcept {
    auto edge = next_.load(std::memory_order_acquire);
    while (edge && edge != UINT64_MAX) {
      if (next_.compare_exchange_weak(edge, edge + 1u,
          std::memory_order_acq_rel, std::memory_order_acquire)) return edge;
    }
    if (edge == UINT64_MAX) next_.store(0u, std::memory_order_release);
    return 0u;
  }
  void revoke() noexcept { next_.store(0u, std::memory_order_release); }
  bool active() const noexcept {
    const auto edge = next_.load(std::memory_order_acquire);
    return edge && edge != UINT64_MAX;
  }

private:
  friend struct TapeRuntimeEdgeClockTestAccess;
  std::atomic<std::uint64_t> next_{2u};
};

// Cold typed lifetime root. A V4 owner derives from this and owns its
// provenance/topology ledgers. It may borrow this embedded clock internally,
// but must not retain its endpoint, queue, or a lease of itself: ownership is
// endpoint/lease -> root -> ledgers/clock, with no reverse edge or self-cycle.
class TapeRuntimeCaptureAttachment {
public:
  virtual ~TapeRuntimeCaptureAttachment() = default;
  virtual TapeRuntimeObserverSink sink() noexcept = 0;
  virtual TapeTopologyCapture& topology() noexcept = 0;
  TapeRuntimeEdgeClock& edges() noexcept { return edges_; }

private:
  TapeRuntimeEdgeClock edges_{};
};

} // namespace dxmt9::render
