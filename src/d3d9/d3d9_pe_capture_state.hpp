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
struct RenderTapeLiveObject {
  enum class Role : std::uint8_t { Ordinary, PresentOutput };

  D9CWireObjectIdentity identity{};
  std::vector<std::byte> descriptor{};
  std::vector<std::byte> immutablePayload{};
  std::uint32_t contentCount = 0u;
  std::vector<std::vector<std::byte>> content{};
  dxmt9::d3d9::RenderTapeSurfaceAliasLifetime lifetime{};
  D9CWireObjectIdentity aliasParentTexture{};
  Role role = Role::Ordinary;
};

struct RenderTapeArmObjectSnapshot {
  std::size_t objectIndex = 0u;
  std::uint64_t armOrdinal = 0u;
  D9CWireObjectIdentity identity{};
  std::vector<std::byte> descriptor{};
  std::vector<std::vector<std::byte>> content{};
};

enum class RenderTapeObjectRegistration : std::uint8_t {
  Rejected,
  Existing,
  New,
};

struct RenderTapeLiveRegistry {
  std::vector<RenderTapeLiveObject> objects{};
  std::vector<D9CWireObjectIdentity> knownDead{};
  bool invalid = false;
  const char *invalidReason = nullptr;
  std::uint32_t invalidKind = 0u;
  std::uint32_t invalidGeneration = 0u;
  std::uint64_t invalidObjectId = 0u;
  std::uint32_t invalidSubresource = std::numeric_limits<std::uint32_t>::max();
  dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic invalidLayout{};
  dxmt9::d3d9::RenderTapePresentOutputRole presentOutputRole{};
  std::vector<std::byte> presentOutputPriorDescriptor{};
  std::uint32_t presentOutputPriorContentCount = 0u;
  std::vector<std::vector<std::byte>> presentOutputPriorContent{};
};

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
