#pragma once

#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_first_access_locator.hpp"
#include "device_c_render_tape_identity.hpp"
#include "dxmt9/device_c.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

// These PE-local capture types are kept at global namespace scope because
// D3D9DeviceImpl includes their owner from several translation units. Giving
// them internal linkage would make the class definition violate the ODR.
#include "d3d9_pe_device_tape_types.inc.hpp"

struct PeCaptureState {
  PeCaptureState(dxmt9::d3d9::RenderTapeCaptureLimits limits,
                 std::uint32_t profile, std::uint32_t armPresentSkip)
      : renderTapeCapture(true, limits, profile),
        renderTapeArmPresentSkipRemaining(armPresentSkip) {}

  dxmt9::d3d9::RenderTapeCaptureSession renderTapeCapture;
  RenderTapeLiveRegistry renderTapeRegistry{};
  std::vector<dxmt9::d3d9::RenderTapeOracleAttachment>
      renderTapeCaptureOracle{};
  std::optional<dxmt9::d3d9::RenderTapeDigest> renderTapeExpectedDigest{};
  std::vector<std::byte> renderTapeExpectedPixels{};
  std::vector<std::byte> renderTapeExpectedSourcePixels{};
  std::optional<D9CSurfaceDesc> renderTapeOutputDesc{};
  dxmt9::d3d9::RenderTapeArmBoundaryPhase renderTapeArmBoundaryPhase =
      dxmt9::d3d9::RenderTapeArmBoundaryPhase::Disabled;
  std::uint64_t renderTapeArmSnapshotOrdinal = 0u;
  std::uint64_t renderTapeNextCaptureToken = 0u;
  std::uint64_t renderTapeActiveCaptureToken = 0u;
  std::uint32_t renderTapeArmPresentSkipRemaining = 0u;
  std::vector<RenderTapeArmObjectSnapshot> renderTapeArmSnapshots{};
  std::vector<D9CWireObjectIdentity> renderTapeAdmittedIdentities{};
  dxmt9::d3d9::RenderTapeFirstAccessLedger renderTapeFirstAccessLedger{};
  const char *renderTapeAbortReason = nullptr;
  std::uint64_t renderTapeCompletionOrdinal = 0u;
};

inline std::unique_ptr<PeCaptureState> makePeCaptureState(
    bool enabled, dxmt9::d3d9::RenderTapeCaptureLimits limits,
    std::uint32_t profile, std::uint32_t armPresentSkip) {
  if (!enabled)
    return nullptr;
  return std::make_unique<PeCaptureState>(limits, profile, armPresentSkip);
}
