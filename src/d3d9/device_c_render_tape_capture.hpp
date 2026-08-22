#pragma once

#include "device_c_render_tape.hpp"
#include "device_c_render_tape_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

struct RenderTapeIdentitySource;
struct RenderTapeIdentityRange;

inline constexpr std::uint64_t kRenderTapeDefaultMaxBlobBytes =
    256u * 1024u * 1024u;
inline constexpr std::uint64_t kRenderTapeHardMaxBlobBytes =
    1u * 1024u * 1024u * 1024u;

// The capture owner is deliberately independent of the PE COM wrappers and
// of the unix provider. Callers hand it complete, value-owned shadow snapshots
// and canonical wire bytes; it never retains a PE pointer or crosses the ABI.
enum class RenderTapeCaptureState : std::uint8_t {
  Disabled,
  Armed,
  Capturing,
  Sealed,
  Aborted,
};

enum class RenderTapeCaptureStatus : std::uint8_t {
  Accepted,
  Complete,
  Disabled,
  InvalidState,
  InvalidInput,
  CapacityExceeded,
  Terminal,
  ValidationFailed,
};

struct RenderTapeCaptureLimits {
  std::uint32_t maxEvents = 4096u;
  std::uint64_t maxEventBytes = 64u * 1024u * 1024u;
  std::uint32_t maxBlobEntries = 4096u;
  // Blob admission remains fail-closed at this capture-only limit.
  std::uint64_t maxBlobBytes = kRenderTapeDefaultMaxBlobBytes;
};

// Value-only handoff for the device-owned production checkpoint and the
// explicit injected test override. The session copies every field before the
// producer's call returns and never retains COM/Metal objects.
struct RenderTapeCaptureObjectSeed {
  D9CWireObjectIdentity identity{};
  std::uint32_t descriptorKind = 0u;
  std::vector<std::byte> descriptor{};
  std::uint64_t immutableBytes = 0u;
  RenderTapeDigest immutableDigest{};
  std::uint64_t expectedContentBytes = 0u;
  std::uint32_t expectedContentCount = 0u;
};

struct RenderTapeCaptureMutationSeed {
  D9CWireObjectIdentity identity{};
  RenderTapeMutationKind kind = RenderTapeMutationKind::Upload;
  RenderTapeBufferMutationDisposition bufferDisposition =
      RenderTapeBufferMutationDisposition::Plain;
  std::uint32_t subresource = 0u;
  std::uint64_t byteOffset = 0u;
  std::uint64_t byteSize = 0u;
  RenderTapeDigest digest{};
};

// A capture-owned blob. The digest is never trusted: armWithBlobs hashes the
// copied bytes and derives the catalogue entry from that result.
struct RenderTapeCaptureBlob {
  std::vector<std::byte> bytes{};
};

struct RenderTapePublishedBlob {
  RenderTapeDigest digest{};
  std::vector<std::byte> bytes{};
};

struct RenderTapePublicationBundle {
  std::vector<std::byte> events{};
  std::vector<RenderTapePublishedBlob> blobs{};
  // Optional authoritative scheduling/pass identity. Capture currently leaves
  // this empty until the bounded production metadata join is available.
  std::vector<std::byte> identity{};
  RenderTapeIdentityEventSettlement identitySettlement{};
  std::vector<D9CRenderTapeIdentitySettlementEntry> identitySettlements{};
  // Captured presentation output is an authoritative comparison sidecar, not
  // a replay input and therefore not part of the immutable blob catalogue.
  std::vector<std::byte> outputOracle{};
  // The pre-Present source attachment is an optional diagnostic sidecar. It
  // is deliberately separate from output.rgba: a mismatch here localizes the
  // fault to draw/resource state before Presenter composition.
  std::vector<std::byte> sourceOracle{};
};

struct RenderTapeCaptureBootstrapSeed {
  std::vector<std::byte> bootstrapOverlay{};
  std::vector<std::byte> gammaRamp{};
  std::vector<RenderTapeCaptureBlob> blobs{};
  std::vector<RenderTapeCaptureObjectSeed> objects{};
  std::vector<RenderTapeCaptureMutationSeed> mutations{};
  std::vector<RenderTapeOracleAttachment> oracleAttachments{};
};

enum class RenderTapeObjectDefineDisposition : std::uint32_t {
  Appended = 0u,
  IdempotentSurfaceAlias,
  InvalidState,
  InvalidIdentity,
  InvalidDescriptor,
  InvalidExpectedContent,
  MissingImmutableBlob,
  ExactIdentityConflict,
  OverlappingLiveGeneration,
  StaleOrEqualGeneration,
  CapacityExceeded,
  AllocationFailed,
};

const char* renderTapeObjectDefineDispositionName(
    RenderTapeObjectDefineDisposition disposition) noexcept;

class RenderTapeCaptureSession {
public:
  explicit RenderTapeCaptureSession(
      bool enabled, RenderTapeCaptureLimits limits = {},
      std::uint32_t profile = kRenderTapeProfileFrame);
  RenderTapeCaptureSession(bool enabled, std::uint32_t profile)
      : RenderTapeCaptureSession(enabled, {}, profile) {}
  ~RenderTapeCaptureSession() = default;

  RenderTapeCaptureSession(const RenderTapeCaptureSession&) = delete;
  RenderTapeCaptureSession& operator=(const RenderTapeCaptureSession&) = delete;

  // The supplied overlay must be the complete PE shadow checkpoint for the
  // next interval. It is copied before this call returns. Blob entries must
  // be verified by the capture owner before they are registered.
  RenderTapeCaptureStatus arm(
      std::span<const std::byte> bootstrapOverlay,
      std::span<const RenderTapeBlob> blobs = {},
      std::span<const std::byte> gammaRamp = {});

  RenderTapeCaptureStatus armWithBlobs(
      std::span<const std::byte> bootstrapOverlay,
      std::span<const RenderTapeCaptureBlob> blobs,
      std::span<const std::byte> gammaRamp = {});

  // Starts the bounded profile selected at construction. Frame captures seal
  // after one successful Present; sequence captures retain the first boundary
  // and seal after the second. A new begin call is always rejected.
  RenderTapeCaptureStatus beginPresentInterval();

  RenderTapeCaptureStatus registerVerifiedBlob(
      std::span<const std::byte, kRenderTapeDigestSize> digest,
      std::uint64_t size);

  RenderTapeCaptureStatus registerBlobBytes(std::span<const std::byte> bytes,
                                            RenderTapeDigest* digest = nullptr);

  RenderTapeCaptureStatus resourceMutationBytes(
      const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
      std::uint32_t subresource, std::uint64_t byteOffset,
      std::span<const std::byte> bytes,
      RenderTapeBufferMutationDisposition bufferDisposition =
          RenderTapeBufferMutationDisposition::Plain);

  RenderTapeCaptureStatus objectDefine(
      const D9CWireObjectIdentity& identity, std::uint32_t descriptorKind,
      std::span<const std::byte> descriptor, std::uint64_t immutableBytes,
      RenderTapeDigest immutableDigest, std::uint64_t expectedContentBytes = 0u,
      std::uint32_t expectedContentCount = 0u,
      RenderTapeObjectDefineDisposition* disposition = nullptr);
  RenderTapeCaptureStatus objectDestroy(
      const D9CWireObjectIdentity& identity);
  RenderTapeCaptureStatus resourceMutation(
      const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
      std::uint32_t subresource, std::uint64_t byteOffset,
      std::uint64_t byteSize,
      std::span<const std::byte, kRenderTapeDigestSize> digest,
      RenderTapeBufferMutationDisposition bufferDisposition =
          RenderTapeBufferMutationDisposition::Plain);
  RenderTapeCaptureStatus commandChunk(const CommandChunkEnvelope& envelope,
                                       std::span<const std::byte> chunk);
  RenderTapeCaptureStatus orderedControl(
      const RenderTapeOrderedControlHeader& fixed,
      std::span<const std::byte> controlPayload);

  // CompletePresent journals each selected profile boundary. Only the final
  // boundary performs structural validation and makes bytes publishable. Any
  // failure aborts the session and publishes no partial artifact.
  RenderTapeCaptureStatus completePresent(
      std::uint64_t presentOrdinal, std::uint64_t completionOrdinal,
      RenderTapeDigestValidity digestValidity, RenderTapeDigest expectedDigest,
      std::span<const std::byte> oracleAttachments = {},
      std::span<const std::byte> outputOracle = {},
      std::span<const std::byte> sourceOracle = {});

  RenderTapeCaptureStatus attachCaptureIdentity(
      std::uint64_t captureToken, std::uint64_t presentOrdinal,
      std::span<const RenderTapeIdentitySource> sources,
      std::span<const RenderTapeIdentityRange> ranges,
      RenderTapeIdentityEventSettlement settlement = {},
      std::span<const D9CRenderTapeIdentitySettlementEntry> settlements = {});
  const RenderTapeIdentityValidationResult& identityValidationResult() const
      noexcept {
    return identityValidationResult_;
  }

  void abort() noexcept;

  RenderTapeCaptureState state() const noexcept { return state_; }
  std::uint32_t eventCount() const noexcept {
    return static_cast<std::uint32_t>(eventCount_);
  }
  std::uint64_t bufferedBytes() const noexcept { return eventBytes_; }
  std::uint64_t ownedBlobBytes() const noexcept { return blobBytes_; }
  // Bounded observability for capture-rejection attribution. A rejected
  // append fuses several predicates into one status, so an owner must be able
  // to name which one failed without raising a capacity to find out.
  std::uint32_t ownedBlobEntries() const noexcept {
    return static_cast<std::uint32_t>(catalogue_.blobs.size());
  }
  const RenderTapeCaptureLimits& limits() const noexcept { return limits_; }
  bool hasLiveObject(const D9CWireObjectIdentity& identity) const noexcept {
    return hasObject(identity, true);
  }
  const std::vector<std::byte>& sealedArtifact() const noexcept {
    return sealedArtifact_;
  }
  const RenderTapePublicationBundle& publicationBundle() const noexcept {
    return publicationBundle_;
  }
  bool enabled() const noexcept { return enabled_; }
  std::uint32_t profile() const noexcept { return builder_.profile(); }
  std::uint32_t presentCompletionCount() const noexcept {
    return presentCompletionCount_;
  }
  bool presentChunkSeen() const noexcept { return presentChunkSeen_; }
  RenderTapeValidationStatus validationStatus() const noexcept {
    return validationStatus_;
  }
  const RenderTapeValidationResult& validationResult() const noexcept {
    return validationResult_;
  }

  static RenderTapeDigest sha256(std::span<const std::byte> bytes);

private:
  struct ObjectSlot {
    D9CWireObjectIdentity identity{};
    RenderTapeLogicalObjectSlot logicalSlot{};
    std::uint32_t descriptorKind = 0u;
    std::vector<std::byte> descriptor{};
    std::uint64_t immutableBytes = 0u;
    RenderTapeDigest immutableDigest{};
    std::uint64_t expectedContentBytes = 0u;
    std::uint32_t expectedContentCount = 0u;
    bool live = false;
  };

  RenderTapeCaptureStatus reserveEvent(std::size_t payloadBytes) noexcept;
  RenderTapeCaptureStatus requireCapturing() const noexcept;
  bool hasVerifiedBlob(const RenderTapeDigest& digest,
                       std::uint64_t size) const noexcept;
  bool hasObject(const D9CWireObjectIdentity& identity,
                 bool liveOnly) const noexcept;
  bool chunkHasPresent(std::span<const std::byte> chunk,
                       const CommandChunkEnvelope& envelope) const noexcept;
  void abortInternal() noexcept;

  bool enabled_ = false;
  RenderTapeCaptureLimits limits_{};
  RenderTapeCaptureState state_ = RenderTapeCaptureState::Disabled;
  std::size_t eventCount_ = 0u;
  std::uint64_t eventBytes_ = 0u;
  std::uint64_t blobBytes_ = 0u;
  bool presentChunkSeen_ = false;
  std::uint32_t presentCompletionCount_ = 0u;
  RenderTapeValidationStatus validationStatus_ =
      RenderTapeValidationStatus::Valid;
  RenderTapeValidationResult validationResult_{
      .status = RenderTapeValidationStatus::Valid};
  RenderTapeIdentityValidationResult identityValidationResult_{};
  RenderTapeBlobCatalogue catalogue_{};
  std::vector<RenderTapePublishedBlob> publishedBlobs_{};
  std::vector<ObjectSlot> objects_{};
  RenderTapeBuilder builder_{};
  std::vector<std::byte> sealedArtifact_{};
  RenderTapePublicationBundle publicationBundle_{};
};

} // namespace dxmt9::d3d9
