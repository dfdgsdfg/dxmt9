#pragma once

#include "device_c_render_tape_execution_topology_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

inline constexpr std::uint32_t kCaptureSourceReplayPlanV4FormatVersion = 1u;
inline constexpr char kCaptureSourceReplayPlanV4DigestDomain[] =
    "dxmt9.capture-source-replay-plan.v4";

enum class CaptureSourceReplayPlanV4Status : std::uint8_t {
  Valid, InvalidInput, InvalidIdentity, InvalidTopology, BlobIdentityMismatch,
  InvalidPublicationOrder, CapacityExceeded, AllocationFailed,
};

struct CaptureSourceReplayPlanV4Event {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint32_t type = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t commandChunkIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t firstAttemptIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t attemptCount = 0u;
  std::uint32_t rawSettlementIndex = kRenderTapeIdentityV4NoIndex;
};

enum class CaptureSourceReplayPlanV4ActionKind : std::uint8_t {
  ReplayEvent, DispatchRecord, ExpectSourcePublication,
  ExpectInitializerPublication, AcknowledgeRaw,
};

// Publication actions qualify actual receipts at authenticated original cuts;
// they are not flush requests (QUERY_END may already publish internally).
// DispatchRecord preserves original semantic order, not topology execution
// order. Captured attemptIndex is provenance only: a candidate need not repeat
// captured segmentation or pre-effect allocator failures. Rolled-back attempts
// remain evidence in identity().attempts, never imperative replay actions.
// Captured publications are boundary/reference evidence, not universal policy
// equality constraints. An executor must declare its publication mode: exact
// captured-source diagnostics may require these cuts, while policy experiments
// map genuine candidate-local sources/query prefixes to original semantic cuts
// without forcing captured batching counts or physical source partitions.
struct CaptureSourceReplayPlanV4Action {
  CaptureSourceReplayPlanV4ActionKind kind =
      CaptureSourceReplayPlanV4ActionKind::ReplayEvent;
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t evidenceEdge = 0u;
  std::uint32_t eventIndex = 0u;
  std::uint32_t recordIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t dispositionIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t attemptIndex = kRenderTapeIdentityV4NoIndex;
  // Source, initializer, coordinator or raw-settlement index, according to kind.
  std::uint32_t tableIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t cutKind = 0u;
  std::uint32_t cutRecord = 0u;
  friend constexpr bool operator==(const CaptureSourceReplayPlanV4Action&,
      const CaptureSourceReplayPlanV4Action&) = default;
};

// Cold, owned pair admission. No caller tape/catalogue/component span survives
// the build. Shared canonical facts are neutral provenance, not V4-to-V3 wire
// conversion. Valid does not qualify candidate attempts, query readiness,
// actual GPU execution/completion, pixels or timing.
class CaptureSourceReplayPlanV4 {
public:
  CaptureSourceReplayPlanV4() = default;
  CaptureSourceReplayPlanV4(const CaptureSourceReplayPlanV4&) = delete;
  CaptureSourceReplayPlanV4& operator=(const CaptureSourceReplayPlanV4&) = delete;
  CaptureSourceReplayPlanV4(CaptureSourceReplayPlanV4&&) noexcept;
  CaptureSourceReplayPlanV4& operator=(CaptureSourceReplayPlanV4&&) noexcept;

  bool valid() const noexcept { return status_ == CaptureSourceReplayPlanV4Status::Valid; }
  CaptureSourceReplayPlanV4Status status() const noexcept { return status_; }
  const RenderTapeIdentityV4View& identity() const noexcept { return identity_; }
  const RenderTapeExecutionTopologyV1View& topology() const noexcept { return topology_; }
  std::span<const CaptureSourceReplayPlanV4Event> events() const noexcept { return events_; }
  std::span<const RenderTapeIdentityV3RecordFact> records() const noexcept { return records_; }
  std::span<const CaptureSourceReplayPlanV4Action> actions() const noexcept { return actions_; }
  std::span<const RenderTapeBlob> referencedBlobs() const noexcept { return referencedBlobs_; }
  const RenderTapeDigest& digest() const noexcept { return digest_; }
  std::span<const std::byte> digestCanonicalBytes() const noexcept { return digestCanonicalBytes_; }

private:
  friend CaptureSourceReplayPlanV4Status buildCaptureSourceReplayPlanV4(
      std::span<const std::byte>, const RenderTapeBlobCatalogue&,
      std::span<const std::byte>, std::span<const std::byte>,
      CaptureSourceReplayPlanV4&) noexcept;
  CaptureSourceReplayPlanV4Status status_ = CaptureSourceReplayPlanV4Status::InvalidInput;
  RenderTapeIdentityV4View identity_{};
  RenderTapeExecutionTopologyV1View topology_{};
  std::vector<CaptureSourceReplayPlanV4Event> events_{};
  std::vector<RenderTapeIdentityV3RecordFact> records_{};
  std::vector<CaptureSourceReplayPlanV4Action> actions_{};
  std::vector<RenderTapeBlob> referencedBlobs_{};
  std::vector<std::byte> digestCanonicalBytes_{};
  RenderTapeDigest digest_{};
};

// Full canonical tape/provenance AND wire pair/topology admission precedes all
// output. Every call first invalidates out; failure exposes no partial plan.
// The eventual executor must also match the exact input identity against its
// prepared general replay input before effects. This function calls no replay,
// queue, resource or GPU API.
[[nodiscard]] CaptureSourceReplayPlanV4Status buildCaptureSourceReplayPlanV4(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> identitySidecar,
    std::span<const std::byte> topologySidecar,
    CaptureSourceReplayPlanV4& out) noexcept;

enum class CaptureSourceReplayCursorV4Status : std::uint8_t {
  Uninitialized, Ready, Complete, Rejected,
};

// Value-only order/exactly-once guard. Callers qualify actual dispatch, receipts
// and raw ACK before acknowledge; candidate real attempts are independently
// observed and qualified, not copied from captured evidence. No borrowed owner
// pointer is retained, and completion here is not readiness or GPU completion.
// In semantic mode an executor may register an exactly-once bounded deferred
// obligation before acknowledging a publication action. That ACK advances
// traversal only: final qualification must discharge every obligation through
// genuine reference-to-candidate correspondence. It grants no publication or
// lifecycle evidence, and never requests an extra flush.
class CaptureSourceReplayCursorV4 {
public:
  bool begin(const CaptureSourceReplayPlanV4&) noexcept;
  bool peek(const CaptureSourceReplayPlanV4&, CaptureSourceReplayPlanV4Action&) noexcept;
  bool acknowledge(const CaptureSourceReplayPlanV4&,
      const CaptureSourceReplayPlanV4Action&) noexcept;
  CaptureSourceReplayCursorV4Status status() const noexcept { return status_; }
  std::size_t acknowledged() const noexcept { return next_; }

private:
  bool bound(const CaptureSourceReplayPlanV4&) noexcept;
  RenderTapeDigest digest_{};
  std::size_t count_ = 0u, next_ = 0u;
  CaptureSourceReplayCursorV4Status status_ = CaptureSourceReplayCursorV4Status::Uninitialized;
};

const char* captureSourceReplayPlanV4StatusName(CaptureSourceReplayPlanV4Status) noexcept;

} // namespace dxmt9::d3d9
