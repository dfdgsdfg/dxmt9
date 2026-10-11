#pragma once

#include "device_c_render_tape.hpp"
#include "device_c_render_tape_action_witness.hpp"
#include "device_c_render_tape_identity.hpp"
#include "device_c_render_tape_identity_v3.hpp"
#include "device_c_render_tape_execution_topology_v1.hpp"
#include "device_c_render_tape_payload.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dxmt9::d3d9 {

struct RenderTapeIdentitySource;
struct RenderTapeIdentityRange;

inline constexpr std::uint32_t kRenderTapeDefaultMaxEvents = 4096u;
inline constexpr std::uint32_t kRenderTapeHardMaxEvents = 65536u;
inline constexpr std::uint32_t kRenderTapeDefaultMaxBlobEntries = 4096u;
inline constexpr std::uint32_t kRenderTapeHardMaxBlobEntries = 65536u;
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
  std::uint32_t maxEvents = kRenderTapeDefaultMaxEvents;
  std::uint64_t maxEventBytes = 64u * 1024u * 1024u;
  std::uint32_t maxBlobEntries = kRenderTapeDefaultMaxBlobEntries;
  // Blob admission remains fail-closed at this capture-only limit.
  std::uint64_t maxBlobBytes = kRenderTapeDefaultMaxBlobBytes;
};

// Cold arm-failure provenance. Strings always name static storage; recording
// a failure must not allocate or retain an exception's borrowed what() text.
struct RenderTapeCaptureArmFailure {
  const char* reason = "none";
  const char* exceptionKind = "none";
  const char* layer = "none";
  const char* phase = "none";
  std::uint64_t blobIndex = UINT64_MAX;
  std::uint64_t blobBytes = 0u;
  std::uint64_t inputBlobCount = 0u;
  std::uint64_t uniqueBlobCount = 0u;
  std::uint64_t cumulativeBlobBytes = 0u;
  std::uint64_t eventPayloadBytes = 0u;
  std::uint64_t eventCount = 0u;
  std::uint64_t eventBytes = 0u;
};

struct RenderTapeCaptureAbortAccounting {
  bool valid = false;
  std::uint64_t eventCount = 0u;
  std::uint64_t eventBytes = 0u;
  std::uint64_t blobCount = 0u;
  std::uint64_t blobBytes = 0u;
  std::uint64_t objectCount = 0u;
};

// Capture-only failure values survive rollback and terminal payload release.
// Accounting is sampled before the failing operation, never from an aborted
// owner's cleared stores. No payload or exception-owned text is retained.
struct RenderTapeCaptureMutationFailure {
  bool valid = false;
  const char* reason = "none";
  const char* exceptionKind = "none";
  const char* layer = "none";
  const char* phase = "none";
  RenderTapeCaptureStatus status = RenderTapeCaptureStatus::Accepted;
  RenderTapeCaptureState captureState = RenderTapeCaptureState::Disabled;
  bool hasIdentity = false;
  D9CWireObjectIdentity identity{};
  RenderTapeMutationKind kind = RenderTapeMutationKind::Upload;
  RenderTapeBufferMutationDisposition bufferDisposition =
      RenderTapeBufferMutationDisposition::Plain;
  std::uint32_t subresource = 0u;
  std::uint64_t byteOffset = 0u;
  std::uint64_t byteSize = 0u;
  std::uint64_t eventPayloadBytes = 0u;
  std::uint64_t publishedBlobCount = 0u;
  RenderTapeCaptureAbortAccounting accounting{};
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

// A capture input blob. The digest is never trusted: armWithBlobs hashes the
// original bytes and derives the catalogue entry from that result.
struct RenderTapeCaptureBlob {
  RenderTapeCaptureBlob() noexcept = default;
  explicit RenderTapeCaptureBlob(
      std::vector<std::byte>&& input,
      const RenderTapeSnapshotBudgetLease& lease = {}) {
    if (lease.valid() && !lease.boundPayloadMatches(input.data(), input.size()))
      throw std::invalid_argument("render-tape retained snapshot lease mismatch");
    retainedLease = lease;
    bytes = std::move(input);
  }
  explicit RenderTapeCaptureBlob(const std::vector<std::byte>& input) : bytes(input) {}
  explicit RenderTapeCaptureBlob(RenderTapeImmutablePayload input) noexcept
      : immutable(std::move(input)) {}

  RenderTapeCaptureBlob(const RenderTapeCaptureBlob& other) {
    // An uncharged fixture may copy raw bytes. A retained reservation names
    // exactly one allocation, so reject before any hidden deep-copy occurs.
    if (other.retainedLease.valid())
      throw std::invalid_argument("cannot copy a retained render-tape snapshot");
    bytes = other.bytes;
    immutable = other.immutable;
  }
  RenderTapeCaptureBlob(RenderTapeCaptureBlob&&) noexcept = default;
  RenderTapeCaptureBlob& operator=(const RenderTapeCaptureBlob& other) {
    if (this == &other) return *this;
    RenderTapeCaptureBlob replacement(other);
    swap(replacement);
    return *this;
  }
  RenderTapeCaptureBlob& operator=(RenderTapeCaptureBlob&& other) noexcept {
    if (this == &other) return *this;
    // Swapping preserves both allocation/receipt pairs until retired's
    // member destruction frees the old bytes before its old reservation.
    RenderTapeCaptureBlob retired;
    retired.swap(*this);
    swap(other);
    return *this;
  }
  void swap(RenderTapeCaptureBlob& other) noexcept {
    std::swap(retainedLease, other.retainedLease);
    bytes.swap(other.bytes);
    std::swap(immutable, other.immutable);
  }

  // A GPU-B reservation belongs to the original raw allocation, never its
  // content digest. Keep it alive until after the vector's destruction.
  RenderTapeSnapshotBudgetLease retainedLease{};
  std::vector<std::byte> bytes{};
  // Optional already-frozen CPU shadow version. Both representations at once
  // are invalid; raw arm snapshot vectors retain the consuming path above.
  RenderTapeImmutablePayload immutable{};
  RenderTapeByteView readBytes() const noexcept {
    return immutable.hasBacking() ? immutable.readBytes()
        : RenderTapeByteView(std::span<const std::byte>(bytes));
  }
};

struct RenderTapePublishedBlob {
  RenderTapeDigest digest{};
  RenderTapeImmutablePayload bytes{};
};

struct RenderTapePublicationBundle {
  std::vector<std::byte> events{};
  std::vector<RenderTapePublishedBlob> blobs{};
  // Optional versioned capture identity. V4 carries neutral provenance, not
  // execution placement; only an explicit validated metadata join fills it.
  std::vector<std::byte> identity{};
  // Mandatory owned companion for identity.v4, empty for earlier identities.
  // It binds the complete identity bytes and events, not provider execution
  // or independent GPU/query-result qualification.
  std::vector<std::byte> executionTopology{};
  // Queue-owned action-policy observation joined atomically with the V4 pair.
  // Canonical absence keeps old captures unknown, never exact-policy qualified.
  RenderTapeActionWitnessV1 actionWitness{};
  std::vector<std::byte> attachmentHistory{};
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

  // Consumes the vector at entry, including on failure. Unique payloads move
  // into session storage only after independent hashing and unchanged budget
  // admission; duplicates/unadmitted inputs are released without publication.
  RenderTapeCaptureStatus armWithOwnedBlobs(
      std::span<const std::byte> bootstrapOverlay,
      std::vector<RenderTapeCaptureBlob>&& blobs,
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
  RenderTapeCaptureStatus registerBlobBytes(RenderTapeByteView bytes,
                                            RenderTapeDigest* digest = nullptr);

  // Consumes at entry on success or failure. Admission independently hashes
  // the original bytes and uses the borrowed API's exact dedup/budgets. Any
  // rejection aborts an enabled owner: consumed bytes cannot be retried in
  // the same interval as if the snapshot were still complete.
  // A supplied receipt must already bind these exact bytes. The caller may
  // reset its receipt after return: the vector is then consumed/freed, and
  // any adopted immutable backing retains the once-charged reservation.
  RenderTapeCaptureStatus registerOwnedBlobBytes(
      std::vector<std::byte>&& bytes, RenderTapeDigest* digest = nullptr,
      const RenderTapeSnapshotBudgetLease& retainedLease = {});

  RenderTapeCaptureStatus registerSharedBlobBytes(
      const RenderTapeImmutablePayload& bytes, RenderTapeDigest* digest = nullptr);
  RenderTapeCaptureStatus registerShadowBlobBytes(
      RenderTapeCpuShadowContent& bytes, RenderTapeDigest* digest = nullptr);

  RenderTapeCaptureStatus resourceMutationBytes(
      const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
      std::uint32_t subresource, std::uint64_t byteOffset,
      std::span<const std::byte> bytes,
      RenderTapeBufferMutationDisposition bufferDisposition =
          RenderTapeBufferMutationDisposition::Plain);

  // Same consuming/fail-stop contract, including event validation/reserve/
  // append rejection after successful blob admission.
  RenderTapeCaptureStatus resourceMutationOwnedBytes(
      const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
      std::uint32_t subresource, std::uint64_t byteOffset,
      std::vector<std::byte>&& bytes,
      RenderTapeBufferMutationDisposition bufferDisposition =
          RenderTapeBufferMutationDisposition::Plain,
      const RenderTapeSnapshotBudgetLease& retainedLease = {});

  RenderTapeCaptureStatus resourceMutationShadowBytes(
      const D9CWireObjectIdentity& identity, RenderTapeMutationKind kind,
      std::uint32_t subresource, std::uint64_t byteOffset,
      RenderTapeCpuShadowContent& bytes,
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

  // The key comes from the capture's trusted start receipt, never from the
  // candidate sidecar. Complete v3 bytes retain their own source/coordinator/
  // settlement grammar; the v2 settlement mirrors are not populated.
  RenderTapeCaptureStatus attachCaptureIdentityV3(
      RenderTapeIdentityV3CaptureKey expectedCapture,
      std::uint64_t presentOrdinal,
      std::span<const std::byte> completeSidecar);
  // A Unix value snapshot has no tape digest until this final capture join.
  // Serialization uses this owner's tape/catalogue before the byte admission.
  RenderTapeCaptureStatus attachCaptureIdentityV3(
      RenderTapeIdentityV3CaptureKey expectedCapture,
      std::uint64_t presentOrdinal,
      const RenderTapeIdentityV3View& snapshot);
  RenderTapeIdentityV3Status identityV3ValidationStatus() const noexcept {
    return identityV3ValidationStatus_;
  }

  // V4 provenance and its required accepted-topology companion are attached
  // together against this owner's exact sealed Tape and verified catalogue.
  // The capture start key and queue are trusted independently of both inputs.
  // No half pair is publishable; rejection aborts the complete capture owner.
  RenderTapeCaptureStatus attachCaptureIdentityV4(
      RenderTapeIdentityV4CaptureKey expectedCapture,
      std::uint64_t expectedQueue, std::uint64_t presentOrdinal,
      std::span<const std::byte> completeIdentity,
      std::span<const std::byte> completeTopology,
      const RenderTapeActionWitnessV1& actionWitness = {},
      std::span<const std::byte> attachmentHistory = {});
  RenderTapeCaptureStatus attachCaptureIdentityV4(
      RenderTapeIdentityV4CaptureKey expectedCapture,
      std::uint64_t expectedQueue, std::uint64_t presentOrdinal,
      const RenderTapeIdentityV4View& identitySnapshot,
      const RenderTapeExecutionTopologyV1View& topologySnapshot,
      const RenderTapeActionWitnessV1& actionWitness = {},
      std::span<const std::byte> attachmentHistory = {});
  RenderTapeIdentityV4Status identityV4ValidationStatus() const noexcept {
    return identityV4ValidationStatus_;
  }
  RenderTapeExecutionTopologyV1Status topologyV1ValidationStatus() const noexcept {
    return topologyV1ValidationStatus_;
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
  std::span<const RenderTapePublishedBlob> ownedPublishedBlobs() const noexcept {
    return publishedBlobs_;
  }
  const RenderTapeCaptureLimits& limits() const noexcept { return limits_; }
  const std::shared_ptr<RenderTapeScratchArena>& payloadArena() const noexcept { return payloadArena_; }
  const RenderTapeCaptureArmFailure& armFailure() const noexcept {
    return armFailure_;
  }
  const RenderTapeCaptureMutationFailure& mutationFailure() const noexcept {
    return mutationFailure_;
  }
  const RenderTapeCaptureAbortAccounting& lastAbortAccounting() const noexcept {
    return lastAbortAccounting_;
  }
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
  static RenderTapeDigest sha256(RenderTapeByteView bytes);

private:
  RenderTapeCaptureStatus registerBlobStorage(
      RenderTapeByteView bytes, RenderTapeDigest* digest,
      std::vector<std::byte>* transferredBytes,
      const RenderTapeImmutablePayload* sharedBytes = nullptr,
      const RenderTapeSnapshotBudgetLease& retainedLease = {});
  RenderTapeCaptureStatus armWithBlobStorage(
      std::span<const std::byte> bootstrapOverlay,
      std::span<const RenderTapeCaptureBlob> blobs,
      std::span<const std::byte> gammaRamp,
      std::vector<RenderTapeCaptureBlob>* transferredBlobs);

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
  RenderTapeCaptureMutationFailure mutationFailureContext(
      const char* layer) const noexcept;
  void abortInternal() noexcept;

  bool enabled_ = false;
  RenderTapeCaptureLimits limits_{};
  std::shared_ptr<RenderTapeScratchArena> payloadArena_{};
  RenderTapeCaptureArmFailure armFailure_{};
  RenderTapeCaptureMutationFailure mutationFailure_{};
  RenderTapeCaptureAbortAccounting lastAbortAccounting_{};
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
  RenderTapeIdentityV3Status identityV3ValidationStatus_ =
      RenderTapeIdentityV3Status::InvalidHeader;
  RenderTapeIdentityV4Status identityV4ValidationStatus_ =
      RenderTapeIdentityV4Status::InvalidHeader;
  RenderTapeExecutionTopologyV1Status topologyV1ValidationStatus_ =
      RenderTapeExecutionTopologyV1Status::InvalidHeader;
  RenderTapeBlobCatalogue catalogue_{};
  std::vector<RenderTapePublishedBlob> publishedBlobs_{};
  std::vector<ObjectSlot> objects_{};
  RenderTapeBuilder builder_{};
  std::vector<std::byte> sealedArtifact_{};
  RenderTapePublicationBundle publicationBundle_{};
};

} // namespace dxmt9::d3d9
