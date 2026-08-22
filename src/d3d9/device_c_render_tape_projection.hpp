#pragma once

#include "device_c_render_tape.hpp"
#include "device_c_render_tape_identity.hpp"
#include "device_c_render_tape_state_fold.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

inline constexpr std::uint32_t kRenderTapeProjectionNoIndex =
    std::numeric_limits<std::uint32_t>::max();
inline constexpr char kRenderTapeProjectionSchema[] =
    "dxmt9.render_tape.projection.v1";

// A projection selects one contiguous record interval from one canonical
// CommandChunk event. Event ordinal plus record index is the durable locator;
// Render Tape v2 does not carry an authoritative frame/source/pass mapping.
struct RenderTapeProjectionSelector {
  std::uint64_t commandEventOrdinal = 0u;
  std::uint32_t firstRecordIndex = 0u;
  std::uint32_t recordCount = 0u;
};

struct RenderTapeProjectionLocator {
  std::uint64_t eventOrdinal = 0u;
  std::uint32_t sourceEventIndex = kRenderTapeProjectionNoIndex;
  std::uint32_t recordIndex = kRenderTapeProjectionNoIndex;
  std::uint32_t recordType = 0u;
};

struct RenderTapeProjectionObject {
  D9CWireObjectIdentity identity{};
  std::uint32_t descriptorKind = 0u;
  std::uint32_t descriptorBytes = 0u;
  std::uint32_t definitionEventIndex = kRenderTapeProjectionNoIndex;
  std::uint64_t definitionEventOrdinal = 0u;
  std::uint64_t immutablePayloadBytes = 0u;
  std::uint64_t expectedContentBytes = 0u;
  std::uint32_t expectedContentCount = 0u;
  std::uint32_t initialContentDisposition = 0u;
  D9CWireObjectIdentity aliasParentTexture{};
  std::uint32_t aliasSubresource = 0u;
  std::uint64_t initialContentBytes = 0u;
  std::uint32_t initialContentCount = 0u;
};

enum class RenderTapeProjectionBlobKind : std::uint8_t {
  ImmutablePayload,
  ResourceMutation,
};

// One entry per source reference, rather than one entry per unique digest, so
// source order and duplicate content-addressed references remain observable.
struct RenderTapeProjectionBlobReference {
  RenderTapeProjectionBlobKind kind =
      RenderTapeProjectionBlobKind::ImmutablePayload;
  D9CWireObjectIdentity identity{};
  RenderTapeDigest digest{};
  std::uint64_t size = 0u;
  std::uint32_t sourceEventIndex = kRenderTapeProjectionNoIndex;
  std::uint64_t sourceEventOrdinal = 0u;
  std::uint32_t mutationKind = 0u;
  std::uint32_t subresource = 0u;
  std::uint64_t byteOffset = 0u;
  std::uint32_t initialContent = 0u;
};

enum class RenderTapeProjectionExcludedKind : std::uint8_t {
  OrderedControl,
  ObjectDestroy,
  PresentComplete,
};

struct RenderTapeProjectionExcludedEvent {
  RenderTapeProjectionExcludedKind kind =
      RenderTapeProjectionExcludedKind::OrderedControl;
  std::uint32_t sourceEventIndex = kRenderTapeProjectionNoIndex;
  std::uint64_t eventOrdinal = 0u;
  std::uint32_t eventType = 0u;
};

enum class RenderTapeProjectionStatus : std::uint8_t {
  Valid,
  InvalidSource,
  UnsupportedProfile,
  InvalidSelection,
  NonDrawRecord,
  StateFoldFailed,
  MissingFrameBoundary,
  MissingDefinition,
  MissingInitialContent,
  MissingBlobClosure,
  AllocationFailed,
};

struct RenderTapeProjectionResult {
  RenderTapeProjectionStatus status = RenderTapeProjectionStatus::InvalidSource;
  RenderTapeValidationResult sourceValidation{};
  std::uint32_t failedEventIndex = kRenderTapeProjectionNoIndex;
  std::uint32_t failedRecordIndex = kRenderTapeProjectionNoIndex;
  D9CWireObjectIdentity failedIdentity{};
  RenderTapeDigest sourceDigest{};
  std::uint64_t sourceBytes = 0u;
  RenderTapeProjectionSelector selector{};
  std::uint32_t selectedCommandEventIndex = kRenderTapeProjectionNoIndex;
  std::uint32_t sourceEventCount = 0u;
  std::uint64_t sourceRecordCount = 0u;
  std::uint32_t selectedDrawCount = 0u;
  std::uint64_t excludedRecordCount = 0u;
  RenderTapeProjectionLocator clearLocator{};
  RenderTapeProjectionLocator presentLocator{};
  RenderTapeStateFoldResult stateFold{};
  std::vector<RenderTapeProjectionLocator> selectedLocators;
  std::vector<RenderTapeProjectionObject> objects;
  std::vector<RenderTapeProjectionBlobReference> blobReferences;
  std::vector<RenderTapeProjectionExcludedEvent> excludedEvents;

  bool valid() const noexcept {
    return status == RenderTapeProjectionStatus::Valid;
  }
};

// Cold, side-effect-free planning over a structurally validated v2 frame tape.
// The function invokes no replay sink, provider, Metal owner, or artifact
// writer. Successful output owns every returned value and leaves source wire
// bytes untouched.
RenderTapeProjectionResult projectRenderTapeDrawSlice(
    std::span<const std::byte> source,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    const RenderTapeProjectionSelector& selector) noexcept;

const char* renderTapeProjectionStatusName(
    RenderTapeProjectionStatus status) noexcept;
const char* renderTapeProjectionBlobKindName(
    RenderTapeProjectionBlobKind kind) noexcept;
const char* renderTapeProjectionExcludedKindName(
    RenderTapeProjectionExcludedKind kind) noexcept;

enum class RenderTapeProjectionBundleStatus : std::uint8_t {
  Valid,
  InvalidProjection,
  InvalidIdentity,
  SelectionOutsidePass,
  ClosureFailure,
  ChunkBuildFailure,
  OutputValidationFailed,
  AllocationFailed,
};

struct RenderTapeProjectionBundleResult {
  RenderTapeProjectionBundleStatus status =
      RenderTapeProjectionBundleStatus::InvalidProjection;
  RenderTapeProjectionResult projection{};
  RenderTapeIdentityValidationResult identityValidation{};
  std::uint64_t logicalPassId = 0u;
  std::vector<std::byte> bytes{};
  std::vector<RenderTapeDigest> referencedBlobDigests{};

  bool valid() const noexcept {
    return status == RenderTapeProjectionBundleStatus::Valid;
  }
};

RenderTapeProjectionBundleResult materializeRenderTapeProjectionBundle(
    std::span<const std::byte> source,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> identitySidecar,
    const RenderTapeProjectionSelector& selector,
    RenderTapeDigestValidity outputDigestValidity =
        RenderTapeDigestValidity::NotCaptured,
    RenderTapeDigest outputDigest = {}) noexcept;

const char* renderTapeProjectionBundleStatusName(
    RenderTapeProjectionBundleStatus status) noexcept;

} // namespace dxmt9::d3d9
