#pragma once

#include <cstdint>

// PE-local evidence for the one render-tape chunk whose completion is needed
// by a capture-only child operation.  This header intentionally has no Windows
// or D3D includes so the reducer can be exercised by small native tests.
struct PeRenderTapeCommitReceipt {
  bool bridgeAccepted = false;
  bool conflict = false;
  std::uint64_t captureToken = 0u;
  std::uint64_t eventOrdinal = 0u;

  void reset() noexcept { *this = {}; }
  bool hasAcceptedEvent() const noexcept {
    return bridgeAccepted && !conflict && captureToken != 0u &&
        eventOrdinal != 0u;
  }
};

struct PeRenderTapeCommitReceiptCollector {
  PeRenderTapeCommitReceipt *receipt = nullptr;

  void observe(const PeRenderTapeCommitReceipt &candidate) noexcept {
    if (!receipt || !candidate.bridgeAccepted)
      return;
    if (!receipt->bridgeAccepted) {
      *receipt = candidate;
      return;
    }
    // One isolated operation has one authoritative bridge acceptance. Even
    // a repeated equal ticket must not hide a second executed commit.
    receipt->conflict = true;
  }
};

// Installs a collector only for the cold child operation.  The recorder's
// existing PeCaptureState pointer is consulted by commit code; no TLS lookup
// or process-global collector is involved.
class PeRenderTapeCommitReceiptCollectorScope final {
public:
  PeRenderTapeCommitReceiptCollectorScope(
      PeRenderTapeCommitReceiptCollector *&slot,
      PeRenderTapeCommitReceiptCollector &collector) noexcept
      : slot_(slot), prior_(slot) {
    slot_ = &collector;
  }

  PeRenderTapeCommitReceiptCollectorScope(
      const PeRenderTapeCommitReceiptCollectorScope &) = delete;
  PeRenderTapeCommitReceiptCollectorScope &operator=(
      const PeRenderTapeCommitReceiptCollectorScope &) = delete;

  ~PeRenderTapeCommitReceiptCollectorScope() noexcept { slot_ = prior_; }

private:
  PeRenderTapeCommitReceiptCollector *&slot_;
  PeRenderTapeCommitReceiptCollector *prior_;
};
