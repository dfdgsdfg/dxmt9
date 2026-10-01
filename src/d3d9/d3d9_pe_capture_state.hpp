#pragma once

#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_first_access_locator.hpp"
#include "device_c_render_tape_identity.hpp"
#include "d3d9_pe_render_tape_commit_receipt.hpp"
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
  dxmt9::d3d9::RenderTapeQueryCycleState query{};
  // Volume and VolumeTexture wrappers share the owning texture's per-mip
  // lock intent. No live pBits or wrapper pointer enters capture state.
  std::vector<dxmt9::d3d9::RenderTapeVolumeLockState> volumeLocks{};
};

struct RenderTapeArmObjectSnapshot {
  std::size_t objectIndex = 0u;
  std::uint64_t armOrdinal = 0u;
  D9CWireObjectIdentity identity{};
  std::vector<std::byte> descriptor{};
  std::vector<std::vector<std::byte>> content{};
};

enum class RenderTapeArmContentTransferDisposition : std::uint8_t {
  Borrowed,
  OwnedSnapshot,
  AlreadyAdmitted,
  InvalidSnapshot,
};

// A call-local capability, not a retained payload view. Only a selected,
// current, generation-qualified arm owner can surrender bytes. Descriptors,
// snapshot records and unselected payloads remain owned by the arm boundary.
struct RenderTapeArmContentTransferSelection {
  RenderTapeArmContentTransferDisposition disposition =
      RenderTapeArmContentTransferDisposition::Borrowed;
  std::span<std::vector<std::byte>> content{};

  std::vector<std::byte>* payload(std::size_t subresource) const noexcept {
    return disposition == RenderTapeArmContentTransferDisposition::OwnedSnapshot &&
                   subresource < content.size()
        ? &content[subresource] : nullptr;
  }
};

inline RenderTapeArmContentTransferSelection selectRenderTapeArmContentTransfer(
    RenderTapeArmObjectSnapshot* snapshot,
    const D9CWireObjectIdentity& identity, std::uint64_t activeOrdinal,
    const dxmt9::d3d9::RenderTapeArmObjectSnapshotOverlaySelection& selected,
    bool admitted) noexcept {
  using Disposition = RenderTapeArmContentTransferDisposition;
  using Source = dxmt9::d3d9::RenderTapeArmSnapshotOverlaySource;
  if (admitted)
    return {.disposition = Disposition::AlreadyAdmitted};
  if (selected.source != Source::CurrentArm)
    return {.disposition = selected.source == Source::StaleArm
                ? Disposition::InvalidSnapshot : Disposition::Borrowed};
  if (!snapshot || identity.generation == 0u || identity.objectId == 0u ||
      identity.kind > D9C_CHUNK_HANDLE_KIND_QUERY ||
      snapshot->identity.kind != identity.kind ||
      snapshot->identity.generation != identity.generation ||
      snapshot->identity.objectId != identity.objectId ||
      !dxmt9::d3d9::renderTapeArmSnapshotEpochMatches(
          snapshot->armOrdinal, activeOrdinal) ||
      selected.descriptor.data() != snapshot->descriptor.data() ||
      selected.descriptor.size() != snapshot->descriptor.size() ||
      selected.content.data() != snapshot->content.data() ||
      selected.content.size() != snapshot->content.size())
    return {.disposition = Disposition::InvalidSnapshot};
  return {.disposition = Disposition::OwnedSnapshot, .content = snapshot->content};
}

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
  // Non-null only during a cold capture-only child command.  Commit consults
  // this already-owned state pointer to collect any CapacityPost acceptance.
  PeRenderTapeCommitReceiptCollector *renderTapeCommitCollector = nullptr;
};

inline std::unique_ptr<PeCaptureState> makePeCaptureState(
    bool enabled, dxmt9::d3d9::RenderTapeCaptureLimits limits,
    std::uint32_t profile, std::uint32_t armPresentSkip) {
  if (!enabled)
    return nullptr;
  return std::make_unique<PeCaptureState>(limits, profile, armPresentSkip);
}
