#pragma once

#include "device_c_render_tape_identity_v3.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

inline constexpr std::uint32_t kCaptureSourceReplayPlanV3FormatVersion = 1u;
inline constexpr char kCaptureSourceReplayPlanV3DigestDomain[] =
    "dxmt9.capture-source-replay-plan.v3";

enum class CaptureSourceReplayPlanV3Status : std::uint8_t {
  Valid,
  InvalidInput,
  InvalidIdentity,
  BlobIdentityMismatch,
  InvalidPublicationOrder,
  CapacityExceeded,
  AllocationFailed,
};

struct CaptureSourceReplayPlanV3Event {
  std::uint64_t eventOrdinal = 0u;
  std::uint32_t type = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t commandChunkIndex = kRenderTapeIdentityV3NoIndex;
};

enum class CaptureSourceReplayPlanV3ActionKind : std::uint8_t {
  ReplayEvent,
  DispatchRecord,
  ExpectSourcePublication,
  ExpectInitializerPublication,
  SettleCommandEvent,
};

// An expectation qualifies the receipt of an actual publication at its cut.
// It is NOT an unconditional publish/upload request: an ordered coordinator
// can already have published internally. No action submits or completes work.
// DispatchRecord names one original record, even when a disposition witnesses
// several records or commands; the disposition's command extent is not split
// or inferred from record count.
struct CaptureSourceReplayPlanV3Action {
  CaptureSourceReplayPlanV3ActionKind kind =
      CaptureSourceReplayPlanV3ActionKind::ReplayEvent;
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t publicationEdge = 0u;
  std::uint32_t eventIndex = 0u;
  std::uint32_t recordIndex = kRenderTapeIdentityV3NoIndex;
  std::uint32_t dispositionIndex = kRenderTapeIdentityV3NoIndex;
  // Source, initializer, coordinator or settlement index, according to kind.
  // State/GPU DispatchRecord and ReplayEvent have no tableIndex.
  std::uint32_t tableIndex = kRenderTapeIdentityV3NoIndex;
  std::uint32_t cutKind = 0u;
  std::uint32_t cutRecord = 0u;
  friend constexpr bool operator==(const CaptureSourceReplayPlanV3Action&,
                                   const CaptureSourceReplayPlanV3Action&) = default;
};

// Cold, value-owned, fully admitted v3 input. No spans into the caller's tape,
// catalogue or identity bytes survive admission. V2 uses its existing class,
// reader and digest; this type never adapts v3 provenance into v2 source rows.
class CaptureSourceReplayPlanV3 {
public:
  CaptureSourceReplayPlanV3() = default;
  CaptureSourceReplayPlanV3(const CaptureSourceReplayPlanV3&) = delete;
  CaptureSourceReplayPlanV3& operator=(const CaptureSourceReplayPlanV3&) = delete;
  CaptureSourceReplayPlanV3(CaptureSourceReplayPlanV3&& other) noexcept;
  CaptureSourceReplayPlanV3& operator=(CaptureSourceReplayPlanV3&& other) noexcept;

  bool valid() const noexcept {
    return status_ == CaptureSourceReplayPlanV3Status::Valid;
  }
  CaptureSourceReplayPlanV3Status status() const noexcept { return status_; }
  const RenderTapeIdentityV3View& identity() const noexcept { return identity_; }
  std::span<const CaptureSourceReplayPlanV3Event> events() const noexcept {
    return events_;
  }
  std::span<const CaptureSourceReplayPlanV3Action> actions() const noexcept {
    return actions_;
  }
  std::span<const RenderTapeBlob> referencedBlobs() const noexcept {
    return referencedBlobs_;
  }
  const RenderTapeDigest& digest() const noexcept { return digest_; }
  // Exact native/Python binding bytes. Component hashes bind immutable input;
  // the following semantic header/tables use explicit little-endian fields.
  // RGBA oracles remain the enclosing bundle/GPU qualification's obligation.
  std::span<const std::byte> digestCanonicalBytes() const noexcept {
    return digestCanonicalBytes_;
  }

private:
  friend CaptureSourceReplayPlanV3Status buildCaptureSourceReplayPlanV3(
      std::span<const std::byte>, const RenderTapeBlobCatalogue&,
      std::span<const std::byte>, CaptureSourceReplayPlanV3&) noexcept;
  CaptureSourceReplayPlanV3Status status_ =
      CaptureSourceReplayPlanV3Status::InvalidInput;
  RenderTapeIdentityV3View identity_{};
  std::vector<CaptureSourceReplayPlanV3Event> events_{};
  std::vector<CaptureSourceReplayPlanV3Action> actions_{};
  std::vector<RenderTapeBlob> referencedBlobs_{};
  std::vector<std::byte> digestCanonicalBytes_{};
  RenderTapeDigest digest_{};
};

// Full tape and v3 identity validation precede all plan output. Every call
// first invalidates out; failure exposes no valid or partial plan. A previous
// admitted owner must be moved aside explicitly before admitting new input.
// Admission calls no replay, queue, Metal or runtime resource APIs.
[[nodiscard]] CaptureSourceReplayPlanV3Status buildCaptureSourceReplayPlanV3(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> identitySidecar,
    CaptureSourceReplayPlanV3& out) noexcept;

enum class CaptureSourceReplayCursorV3Status : std::uint8_t {
  Uninitialized,
  Ready,
  Complete,
  Rejected,
};

// A value-only traversal guard. A caller must qualify actual dispatch/receipt
// success before acknowledging. This cursor proves order/exactly-once plan
// traversal, not execution, queue submission, GPU completion or pixel results.
// Each operation receives the plan again; no borrowed owner pointer is stored.
class CaptureSourceReplayCursorV3 {
public:
  bool begin(const CaptureSourceReplayPlanV3& plan) noexcept;
  bool peek(const CaptureSourceReplayPlanV3& plan,
            CaptureSourceReplayPlanV3Action& action) noexcept;
  bool acknowledge(const CaptureSourceReplayPlanV3& plan,
                   const CaptureSourceReplayPlanV3Action& action) noexcept;
  CaptureSourceReplayCursorV3Status status() const noexcept { return status_; }
  std::size_t acknowledged() const noexcept { return next_; }

private:
  bool bound(const CaptureSourceReplayPlanV3& plan) noexcept;
  RenderTapeDigest digest_{};
  std::size_t count_ = 0u;
  std::size_t next_ = 0u;
  CaptureSourceReplayCursorV3Status status_ =
      CaptureSourceReplayCursorV3Status::Uninitialized;
};

const char* captureSourceReplayPlanV3StatusName(
    CaptureSourceReplayPlanV3Status status) noexcept;

} // namespace dxmt9::d3d9
