#pragma once

#include "device_c_render_tape.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

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
  std::uint64_t maxBlobBytes = 64u * 1024u * 1024u;
};

// PE-side injection point for the complete shadow checkpoint. The producer
// owns no live COM/Metal objects through this value-only handoff; the session
// copies every field before the producer's call returns.
struct RenderTapeCaptureObjectSeed {
  D9CWireObjectIdentity identity{};
  std::uint32_t descriptorKind = 0u;
  std::vector<std::byte> descriptor{};
  std::uint64_t immutableBytes = 0u;
  RenderTapeDigest immutableDigest{};
};

struct RenderTapeCaptureMutationSeed {
  D9CWireObjectIdentity identity{};
  RenderTapeMutationKind kind = RenderTapeMutationKind::Upload;
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
};

struct RenderTapeCaptureBootstrapSeed {
  std::vector<std::byte> bootstrapOverlay{};
  std::vector<RenderTapeCaptureBlob> blobs{};
  std::vector<RenderTapeCaptureObjectSeed> objects{};
  std::vector<RenderTapeCaptureMutationSeed> mutations{};
  std::vector<RenderTapeOracleAttachment> oracleAttachments{};
};

class RenderTapeCaptureSession {
public:
  explicit RenderTapeCaptureSession(
      bool enabled, RenderTapeCaptureLimits limits = {});
  ~RenderTapeCaptureSession() = default;

  RenderTapeCaptureSession(const RenderTapeCaptureSession&) = delete;
  RenderTapeCaptureSession& operator=(const RenderTapeCaptureSession&) = delete;

  // The supplied overlay must be the complete PE shadow checkpoint for the
  // next interval. It is copied before this call returns. Blob entries must
  // be verified by the capture owner before they are registered.
  RenderTapeCaptureStatus arm(std::span<const std::byte> bootstrapOverlay,
                              std::span<const RenderTapeBlob> blobs = {});

  RenderTapeCaptureStatus armWithBlobs(
      std::span<const std::byte> bootstrapOverlay,
      std::span<const RenderTapeCaptureBlob> blobs);

  // Starts exactly one future Present interval. A second interval is rejected
  // and a successful Present is the only path to Sealed.
  RenderTapeCaptureStatus beginPresentInterval();

  RenderTapeCaptureStatus registerVerifiedBlob(
      std::span<const std::byte, kRenderTapeDigestSize> digest,
      std::uint64_t size);

  RenderTapeCaptureStatus registerBlobBytes(std::span<const std::byte> bytes,
                                            RenderTapeDigest* digest = nullptr);

  RenderTapeCaptureStatus resourceMutationBytes(
      const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
      std::uint32_t subresource, std::uint64_t byteOffset,
      std::span<const std::byte> bytes);

  RenderTapeCaptureStatus objectDefine(
      const D9CWireObjectIdentity& identity, std::uint32_t descriptorKind,
      std::span<const std::byte> descriptor, std::uint64_t immutableBytes,
      RenderTapeDigest immutableDigest);
  RenderTapeCaptureStatus objectDestroy(
      const D9CWireObjectIdentity& identity);
  RenderTapeCaptureStatus resourceMutation(
      const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
      std::uint32_t subresource, std::uint64_t byteOffset,
      std::uint64_t byteSize,
      std::span<const std::byte, kRenderTapeDigestSize> digest);
  RenderTapeCaptureStatus commandChunk(const CommandChunkEnvelope& envelope,
                                       std::span<const std::byte> chunk);
  RenderTapeCaptureStatus orderedControl(
      const RenderTapeOrderedControlHeader& fixed,
      std::span<const std::byte> controlPayload);

  // CompletePresent performs final structural validation before publishing
  // the artifact. Any failure aborts the session and publishes no bytes.
  RenderTapeCaptureStatus completePresent(
      std::uint64_t presentOrdinal, std::uint64_t completionOrdinal,
      RenderTapeDigestValidity digestValidity, RenderTapeDigest expectedDigest,
      std::span<const std::byte> oracleAttachments = {});

  void abort() noexcept;

  RenderTapeCaptureState state() const noexcept { return state_; }
  std::uint32_t eventCount() const noexcept {
    return static_cast<std::uint32_t>(eventCount_);
  }
  std::uint64_t bufferedBytes() const noexcept { return eventBytes_; }
  std::uint64_t ownedBlobBytes() const noexcept { return blobBytes_; }
  const std::vector<std::byte>& sealedArtifact() const noexcept {
    return sealedArtifact_;
  }
  const RenderTapePublicationBundle& publicationBundle() const noexcept {
    return publicationBundle_;
  }
  bool enabled() const noexcept { return enabled_; }
  RenderTapeValidationStatus validationStatus() const noexcept {
    return validationStatus_;
  }

private:
  struct ObjectSlot {
    D9CWireObjectIdentity identity{};
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

  static RenderTapeDigest sha256(std::span<const std::byte> bytes);

  bool enabled_ = false;
  RenderTapeCaptureLimits limits_{};
  RenderTapeCaptureState state_ = RenderTapeCaptureState::Disabled;
  std::size_t eventCount_ = 0u;
  std::uint64_t eventBytes_ = 0u;
  std::uint64_t blobBytes_ = 0u;
  bool presentChunkSeen_ = false;
  RenderTapeValidationStatus validationStatus_ =
      RenderTapeValidationStatus::Valid;
  RenderTapeBlobCatalogue catalogue_{};
  std::vector<RenderTapePublishedBlob> publishedBlobs_{};
  std::vector<ObjectSlot> objects_{};
  RenderTapeBuilder builder_{};
  std::vector<std::byte> sealedArtifact_{};
  RenderTapePublicationBundle publicationBundle_{};
};

} // namespace dxmt9::d3d9
