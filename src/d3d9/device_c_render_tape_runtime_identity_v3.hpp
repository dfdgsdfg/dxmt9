#pragma once

#include "device_c_render_tape_runtime_identity_core.hpp"

struct D9CDevice;

namespace dxmt9::d3d9 {

struct ReplayTransactionState;
struct RawCommandChunk;
struct RenderTapeRuntimeIdentityV3TestAccess;

// V3 policy facade over the shared neutral lifecycle core. Publication keeps
// its historical graph/pass assignment; attempt generation and edge ordering
// remain this capture's original V3 rules, never V4-derived rows.
class RenderTapeRuntimeIdentityV3 final : public RenderTapeRuntimeIdentityCore {
public:
  bool begin(RenderTapeIdentityV3CaptureKey key, render::TapeRuntimeBaseline baseline,
      RenderTapeIdentityV3Limits limits = {}) noexcept;
  bool beginReplayAttempt(std::uint64_t token, std::uint64_t eventOrdinal,
      std::uint64_t rawOrdinal, std::uint32_t recordCount) noexcept;
  bool transactionCommitted(const ReplayTransactionState& state) noexcept;
  bool settleReplayAttempt(std::uint64_t rawOrdinal, bool ledgerSucceeded) noexcept;
  bool snapshot(RenderTapeIdentityV3CaptureKey key, RenderTapeIdentityV3View& out,
      bool requireComplete = true) noexcept;

private:
  friend struct RenderTapeRuntimeIdentityV3TestAccess;
  bool publish(const render::TapeRuntimeObservation&) override;
  std::uint32_t sourceIndex(const render::TapeRuntimeObservation&) const override;
  std::uint64_t edge() noexcept override;
  bool beginEventLocked(std::uint64_t token, std::uint64_t eventOrdinal,
      std::uint64_t rawOrdinal, std::uint64_t buildGeneration,
      std::uint32_t recordCount) noexcept;
  std::vector<RenderTapeIdentityV3Command> commands_{};
  std::vector<RenderTapeIdentityV3ReplaySettlement> settlements_{};
  std::uint64_t nextEdge_ = 2u;
  std::uint64_t nextLogicalPass_ = 1u;
  std::uint64_t nextAttemptGeneration_ = 1u;
  bool transactionCommitted_ = false;
};

bool beginRenderTapeRuntimeIdentityV3(D9CDevice* device,
    std::uint64_t token, std::uint64_t epoch) noexcept;
bool finishRenderTapeRuntimeIdentityV3(D9CDevice* device,
    std::uint64_t token, std::uint64_t epoch,
    std::uint64_t presentOrdinal, RenderTapeIdentityV3View& out) noexcept;
void cancelRenderTapeRuntimeIdentityV3(D9CDevice* device,
    std::uint64_t token, std::uint64_t epoch) noexcept;
void detachRenderTapeRuntimeIdentityV3(D9CDevice* device) noexcept;
void observeRenderTapeRuntimeReplaySettlementV3(D9CDevice* device,
    const RawCommandChunk& raw, bool ledgerSucceeded) noexcept;

} // namespace dxmt9::d3d9
