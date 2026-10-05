#pragma once

#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_first_access_locator.hpp"
#include "device_c_render_tape_identity.hpp"
#include "device_c_render_tape_snapshot_budget.hpp"
#include "d3d9_pe_render_tape_commit_receipt.hpp"
#include "dxmt9/device_c.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
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
  std::vector<dxmt9::d3d9::RenderTapeCpuShadowContent> content{};
  dxmt9::d3d9::RenderTapeSurfaceAliasLifetime lifetime{};
  D9CWireObjectIdentity aliasParentTexture{};
  Role role = Role::Ordinary;
  dxmt9::d3d9::RenderTapeQueryCycleState query{};
  // Volume and VolumeTexture wrappers share the owning texture's per-mip
  // lock intent. No live pBits or wrapper pointer enters capture state.
  std::vector<dxmt9::d3d9::RenderTapeVolumeLockState> volumeLocks{};
};

struct RenderTapeArmObjectSnapshot {
  RenderTapeArmObjectSnapshot() noexcept = default;
  RenderTapeArmObjectSnapshot(const RenderTapeArmObjectSnapshot&) = delete;
  RenderTapeArmObjectSnapshot& operator=(const RenderTapeArmObjectSnapshot&) = delete;
  RenderTapeArmObjectSnapshot(RenderTapeArmObjectSnapshot&&) noexcept = default;
  RenderTapeArmObjectSnapshot& operator=(RenderTapeArmObjectSnapshot&& other) noexcept {
    if (this != &other) {
      // Default member assignment would release old receipts before freeing
      // old vectors. Retire the whole owner, preserving destruction order.
      RenderTapeArmObjectSnapshot retired;
      retired.swap(*this);
      swap(other);
    }
    return *this;
  }

  void swap(RenderTapeArmObjectSnapshot& other) noexcept {
    std::swap(objectIndex, other.objectIndex);
    std::swap(armOrdinal, other.armOrdinal);
    std::swap(identity, other.identity);
    descriptor.swap(other.descriptor);
    retainedLeases.swap(other.retainedLeases);
    content.swap(other.content);
  }

  std::size_t objectIndex = 0u;
  std::uint64_t armOrdinal = 0u;
  D9CWireObjectIdentity identity{};
  std::vector<std::byte> descriptor{};
  // Payload vectors are destroyed before their allocation-bound receipts.
  std::vector<dxmt9::d3d9::RenderTapeSnapshotBudgetLease> retainedLeases{};
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
  std::span<dxmt9::d3d9::RenderTapeSnapshotBudgetLease> retainedLeases{};

  std::vector<std::byte>* payload(std::size_t subresource) const noexcept {
    return disposition == RenderTapeArmContentTransferDisposition::OwnedSnapshot &&
                   subresource < content.size()
        ? &content[subresource] : nullptr;
  }

  dxmt9::d3d9::RenderTapeSnapshotBudgetLease* retainedLease(
      std::size_t subresource) const noexcept {
    const auto* bytes = payload(subresource);
    if (!bytes || subresource >= retainedLeases.size()) return nullptr;
    auto& lease = retainedLeases[subresource];
    return lease.valid() &&
                   lease.kind() == dxmt9::d3d9::RenderTapeSnapshotBudgetKind::Retained &&
                   lease.resourceBytes() == bytes->size() &&
                   lease.chargedBytes() == bytes->size() &&
                   lease.boundPayloadMatches(bytes->data(), bytes->size())
        ? &lease : nullptr;
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
      !selected.content.vectorBacked() ||
      selected.content.vectorData() != snapshot->content.data() ||
      selected.content.size() != snapshot->content.size() ||
      snapshot->retainedLeases.size() != snapshot->content.size())
    return {.disposition = Disposition::InvalidSnapshot};
  const RenderTapeArmContentTransferSelection transfer{
      .disposition = Disposition::OwnedSnapshot,
      .content = snapshot->content,
      .retainedLeases = snapshot->retainedLeases};
  for (std::size_t subresource = 0u; subresource < snapshot->content.size(); ++subresource)
    if (!snapshot->content[subresource].empty() && !transfer.retainedLease(subresource))
      return {.disposition = Disposition::InvalidSnapshot};
  return transfer;
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
  std::vector<dxmt9::d3d9::RenderTapeCpuShadowContent> presentOutputPriorContent{};
};

struct PeCaptureState {
  PeCaptureState(dxmt9::d3d9::RenderTapeCaptureLimits limits,
                 std::uint32_t profile, std::uint32_t armPresentSkip)
      : renderTapeCapture(true, limits, profile),
        renderTapeArmPresentSkipRemaining(armPresentSkip) {
    renderTapeSnapshotBudgetInitializationStatus =
        renderTapeSnapshotBudget.initialize(renderTapeCapture.limits().maxBlobBytes);
  }

  // One session budget, never reset at arm/rearm. Shared immutable receipts
  // may outlive the arm vectors and publication may outlive an interval.
  dxmt9::d3d9::RenderTapeSnapshotBudget renderTapeSnapshotBudget{};
  dxmt9::d3d9::RenderTapeSnapshotBudgetStatus renderTapeSnapshotBudgetInitializationStatus =
      dxmt9::d3d9::RenderTapeSnapshotBudgetStatus::NotInitialized;
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
  // Trusted arm receipt, independent of transferred identity/topology bytes.
  std::uint64_t renderTapeActiveCaptureEpoch = 0u;
  std::uint64_t renderTapeActiveCaptureQueue = 0u;
  // One cold external effect may reserve the next canonical journal event.
  // Preparatory materialization must finish before this ordinal is installed.
  std::uint64_t renderTapeExternalEventOrdinal = 0u;
  std::uint32_t renderTapeArmPresentSkipRemaining = 0u;
  std::vector<RenderTapeArmObjectSnapshot> renderTapeArmSnapshots{};
  std::vector<D9CWireObjectIdentity> renderTapeAdmittedIdentities{};
  dxmt9::d3d9::RenderTapeFirstAccessLedger renderTapeFirstAccessLedger{};
  const char *renderTapeAbortReason = nullptr;
  dxmt9::d3d9::RenderTapePayloadLedger renderTapePayloadLedger{};
  std::uint64_t renderTapeCompletionOrdinal = 0u;
  // Non-null only during a cold capture-only child command.  Commit consults
  // this already-owned state pointer to collect any CapacityPost acceptance.
  PeRenderTapeCommitReceiptCollector *renderTapeCommitCollector = nullptr;
};

inline const dxmt9::d3d9::RenderTapePayloadLedger& measureRenderTapePayloadBoundary(
    PeCaptureState& state,
    std::span<const dxmt9::d3d9::RenderTapeCaptureBlob> seed = {}) noexcept {
  auto& ledger = state.renderTapePayloadLedger;
  ledger.reset();
  const auto observeShadow = [&](const auto& shadow) {
    ledger.observe(shadow.backingIdentity(), shadow.size(), shadow.capacity(), shadow.shared());
  };
  const auto observeImmutable = [&](const auto& payload) {
    ledger.observe(payload.backingIdentity(), payload.size(), payload.capacity(), payload.shared());
  };
  for (const auto& object : state.renderTapeRegistry.objects) {
    ledger.observe(object.immutablePayload.data(), object.immutablePayload.size(),
                   object.immutablePayload.capacity(), false);
    for (const auto& content : object.content) observeShadow(content);
  }
  for (const auto& content : state.renderTapeRegistry.presentOutputPriorContent) observeShadow(content);
  for (const auto& snapshot : state.renderTapeArmSnapshots)
    for (const auto& content : snapshot.content)
      ledger.observe(content.data(), content.size(), content.capacity(), false);
  for (const auto& blob : state.renderTapeCapture.ownedPublishedBlobs()) observeImmutable(blob.bytes);
  for (const auto& blob : state.renderTapeCapture.publicationBundle().blobs) observeImmutable(blob.bytes);
  for (const auto& blob : seed) {
    if (blob.immutable.hasBacking()) observeImmutable(blob.immutable);
    ledger.observe(blob.bytes.data(), blob.bytes.size(), blob.bytes.capacity(), false);
  }
  return ledger;
}

template <typename Log>
inline void logRenderTapePayloadBoundary(PeCaptureState& state, const char* boundary,
    Log log, std::span<const dxmt9::d3d9::RenderTapeCaptureBlob> seed = {}) noexcept {
  const auto& ledger = measureRenderTapePayloadBoundary(state, seed);
  log("render_tape_capture payload_boundary boundary=%s logical_bytes=%llu "
      "logical_capacity=%llu logical_owners=%llu unique_backings_lower_bound=%zu "
      "unique_bytes_lower_bound=%llu unique_capacity_lower_bound=%llu "
      "unique_shared_bytes_lower_bound=%llu unique_shared_capacity_lower_bound=%llu "
      "unique_complete=%d logical_totals_valid=%d observer_capacity=%zu "
      "scratch_live_bytes=%llu scratch_high_water_bytes=%llu",
      boundary, static_cast<unsigned long long>(ledger.logicalBytes),
      static_cast<unsigned long long>(ledger.logicalCapacity),
      static_cast<unsigned long long>(ledger.logicalOwners), ledger.backingCount,
      static_cast<unsigned long long>(ledger.uniqueBytes),
      static_cast<unsigned long long>(ledger.uniqueCapacity),
      static_cast<unsigned long long>(ledger.uniqueSharedBytes),
      static_cast<unsigned long long>(ledger.uniqueSharedCapacity),
      ledger.uniqueComplete ? 1 : 0, ledger.totalsValid ? 1 : 0, ledger.kMaxBackings,
      static_cast<unsigned long long>(state.renderTapeCapture.payloadArena()->liveBytes()),
      static_cast<unsigned long long>(state.renderTapeCapture.payloadArena()->highWaterBytes()));
}

inline std::unique_ptr<PeCaptureState> makePeCaptureState(
    bool enabled, dxmt9::d3d9::RenderTapeCaptureLimits limits,
    std::uint32_t profile, std::uint32_t armPresentSkip) {
  if (!enabled)
    return nullptr;
  return std::make_unique<PeCaptureState>(limits, profile, armPresentSkip);
}
