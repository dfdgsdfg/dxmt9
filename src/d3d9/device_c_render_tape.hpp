#pragma once

#include "device_c_chunk_validate.hpp"
#include "device_c_render_tape_descriptors.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

inline constexpr std::uint64_t kRenderTapeMagic = 0x32505452544d5844ull;
inline constexpr std::uint32_t kRenderTapeVersion = 2u;
inline constexpr std::uint32_t kRenderTapeProfileFrame = 1u;
inline constexpr std::uint32_t kRenderTapeProfileSequence = 2u;
inline constexpr std::uint32_t kRenderTapeBaselineProfileVersion = 1u;
inline constexpr std::uint32_t kRenderTapePayloadAlignment = 8u;
inline constexpr std::uint32_t kRenderTapeDigestSize = 32u;
using RenderTapeDigest = std::array<std::byte, kRenderTapeDigestSize>;

enum class RenderTapeDigestValidity : std::uint32_t {
  NotCaptured = 0u,
  Sha256 = 1u,
};

// Object identity kinds name handle-lifetime domains and are not descriptor
// schema tags. Keep this separate, non-zero vocabulary stable: descriptor
// kind 0 is reserved by the event validator as invalid, while the D3D9
// texture identity kind is legitimately 0.
enum class RenderTapeDescriptorKind : std::uint32_t {
  Invalid = 0u,
  Texture = 1u,
  Surface = 2u,
  Buffer = 3u,
  Shader = 4u,
  VertexDeclaration = 5u,
  Query = 6u,
};

constexpr RenderTapeDescriptorKind renderTapeDescriptorKindForObject(
    std::uint32_t identityKind) noexcept {
  switch (identityKind) {
  case D9C_CHUNK_HANDLE_KIND_TEXTURE:
    return RenderTapeDescriptorKind::Texture;
  case D9C_CHUNK_HANDLE_KIND_SURFACE:
    return RenderTapeDescriptorKind::Surface;
  case D9C_CHUNK_HANDLE_KIND_BUFFER:
    return RenderTapeDescriptorKind::Buffer;
  case D9C_CHUNK_HANDLE_KIND_SHADER:
    return RenderTapeDescriptorKind::Shader;
  case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
    return RenderTapeDescriptorKind::VertexDeclaration;
  case D9C_CHUNK_HANDLE_KIND_QUERY:
    return RenderTapeDescriptorKind::Query;
  default:
    return RenderTapeDescriptorKind::Invalid;
  }
}

// Persistent device-state categories carried by a BootstrapState. These map
// 1:1 to the canonical section kinds (D9C_COMMAND_CHUNK_SECTION_* 1..23) that
// define a backend-visible draw-delta state category. The two trailing section
// kinds (UP_INDEX_DATA / UP_VERTEX_DATA) are per-record draw data, not
// persistent state, and are intentionally excluded from the completeness
// manifest (they can never be promised by a frame bootstrap).
inline constexpr std::uint32_t kRenderTapeStateCategoryCount =
    D9C_COMMAND_CHUNK_SECTION_PS_CONST_B;
// Pointer-free completeness manifest: bit (kind-1) set means the bootstrap
// guarantees a canonical overlay covers that state category. A frame bootstrap
// must declare exactly all known categories; any unknown or missing bit is a
// completeness violation and must fail before replay.
inline constexpr std::uint64_t kRenderTapeRequiredCategoryMask =
    (std::uint64_t{1} << kRenderTapeStateCategoryCount) - std::uint64_t{1};

enum class RenderTapeEventType : std::uint32_t {
  BootstrapState = 1u,
  ObjectDefine = 2u,
  ObjectDestroy = 3u,
  ResourceMutation = 4u,
  CommandChunk = 5u,
  OrderedControl = 6u,
  PresentComplete = 7u,
};

enum class RenderTapeMutationKind : std::uint32_t {
  CpuUnlock = 1u,
  Upload = 2u,
  Palette = 3u,
  MipmapClass = 4u,
};

enum class RenderTapeControlKind : std::uint32_t {
  QueryGetData = 1u,
  CpuRead = 2u,
  FlushWait = 3u,
  Reset = 4u,
  DeviceLost = 5u,
};

enum class RenderTapeControlDisposition : std::uint32_t {
  Completed = 1u,
  Pending = 2u,
  Failed = 3u,
  Terminal = 4u,
};

struct RenderTapeHeader {
  std::uint64_t magic = kRenderTapeMagic;
  std::uint32_t version = kRenderTapeVersion;
  std::uint32_t headerSize = sizeof(RenderTapeHeader);
  std::uint32_t profile = kRenderTapeProfileFrame;
  std::uint32_t eventHeaderSize = 0u;
  std::uint32_t eventTableOffset = 0u;
  std::uint32_t eventCount = 0u;
  std::uint32_t payloadArenaOffset = 0u;
  std::uint32_t payloadArenaSize = 0u;
  std::uint32_t wireVersion = D9C_COMMAND_CHUNK_WIRE_VERSION;
  std::uint32_t presentCount = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
  std::uint32_t reserved2 = 0u;
  std::uint32_t reserved3 = 0u;
};

struct RenderTapeEventHeader {
  std::uint32_t type = 0u;
  std::uint32_t flags = 0u;
  std::uint64_t ordinal = 0u;
  std::uint32_t payloadOffset = 0u;
  std::uint32_t payloadSize = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
};

// BootstrapState: first and unique event. Declares a fixed baseline profile
// version (typed defaults for every persistent state category) plus one or
// more canonical APPLY_STATE-only command chunks used as overlays, plus a
// completeness manifest that must equal every known state category. Absent
// sparse sections in an overlay therefore mean the baseline default rather
// than a live-state dependency.
struct RenderTapeBootstrapHeader {
  std::uint32_t baselineProfileVersion = kRenderTapeBaselineProfileVersion;
  std::uint32_t stateCategoryCount = kRenderTapeStateCategoryCount;
  std::uint32_t overlayCount = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint64_t requiredCategoryMask = kRenderTapeRequiredCategoryMask;
};

// ObjectDefine: generation-qualified creation of an object slot. Carries the
// exact descriptor kind/bytes plus an immutable digest for shader/declaration
// payload identity where applicable. POD, bounded, aligned, pointer-free.
struct RenderTapeObjectDefineHeader {
  D9CWireObjectIdentity identity{};
  std::uint32_t descriptorKind = 0u;
  std::uint32_t descriptorBytes = 0u;
  std::uint32_t payloadValidity = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint64_t immutablePayloadBytes = 0u;
  // Resource closure contract. A non-zero extent is a promise that the
  // bootstrap contains exactly that many bytes for every listed subresource.
  // Non-resource objects keep both fields zero.
  std::uint64_t expectedContentBytes = 0u;
  std::uint32_t expectedContentCount = 0u;
  std::uint32_t reserved1 = 0u;
  RenderTapeDigest immutablePayloadDigest{};
};

// ObjectDestroy: generation-qualified retirement of an object slot.
struct RenderTapeObjectDestroyHeader {
  D9CWireObjectIdentity identity{};
};

// ResourceMutation: typed CPU-visible mutation (Unlock / upload / palette /
// mipmap-class). Names the exact object identity, mutation kind, subresource
// and an overflow-safe byte/region range, plus the SHA-256 digest+size of the
// content stored in an external in-memory blob catalogue. Mutation bytes are
// never inlined into the canonical event component.
struct RenderTapeResourceMutationHeader {
  D9CWireObjectIdentity identity{};
  std::uint32_t kind = 0u;
  std::uint32_t subresource = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint64_t byteOffset = 0u;
  std::uint64_t byteSize = 0u;
  RenderTapeDigest digest{};
};

// CommandChunk: byte-exact canonical D9C chunk; validated with the production
// validator. Already-chunked ordering records must not be duplicated as
// OrderedControl.
struct RenderTapeCommandChunkHeader {
  std::uint32_t wireVersion = D9C_COMMAND_CHUNK_WIRE_VERSION;
  std::uint32_t recordCount = 0u;
  std::uint32_t handleCount = 0u;
  std::uint32_t chunkBytes = 0u;
};

// OrderedControl: kind-specific fixed POD payload for direct synchronous
// observations (QueryGetData / CPU-read / flush-wait / reset / device-lost).
// Records result/disposition/completion and enforces identity and monotone
// completion ordering.
struct RenderTapeOrderedControlHeader {
  D9CWireObjectIdentity identity{};
  std::uint32_t kind = 0u;
  std::uint32_t disposition = 0u;
  std::int32_t resultCode = 0;
  std::uint32_t controlBytes = 0u;    // kind-specific fixed POD bytes
  std::uint64_t completionOrdinal = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
};

struct RenderTapeQueryGetDataControl {
  std::uint64_t dataSize = 0u;
  std::uint64_t seqId = 0u;
};

struct RenderTapeCpuReadControl {
  std::uint32_t copyCount = 0u;
  std::uint32_t bytesRead = 0u;
};

struct RenderTapeFlushWaitControl {
  std::uint64_t waitedSeqId = 0u;
};

struct RenderTapeResetControl {
  std::uint32_t reclaimedGeneration = 0u;
  std::uint32_t terminal = 1u; // successful reset is terminal for a frame tape
};

struct RenderTapeDeviceLostControl {
  std::uint32_t hrCode = 0u;
  std::uint32_t reserved0 = 0u;
};

// PresentComplete closes one Present interval. It is last and unique for the
// frame profile; the bounded sequence profile carries exactly two, with the
// second and final completion terminating the tape.
struct RenderTapePresentCompleteHeader {
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
  std::uint32_t digestValidity = 0u;
  std::uint32_t oracleCount = 0u;
  std::uint32_t oracleBytes = 0u;
  std::uint32_t reserved0 = 0u;
  RenderTapeDigest expectedDigest{};
};

struct RenderTapeOracleAttachment {
  D9CWireObjectIdentity identity{};
  std::uint32_t descriptorKind = 0u;
  std::uint32_t reserved0 = 0u;
};

static_assert(sizeof(RenderTapeHeader) == 64u);
static_assert(alignof(RenderTapeHeader) == 8u);
static_assert(sizeof(RenderTapeEventHeader) == 32u);
static_assert(alignof(RenderTapeEventHeader) == 8u);
static_assert(sizeof(RenderTapeBootstrapHeader) == 24u);
static_assert(alignof(RenderTapeBootstrapHeader) == 8u);
static_assert(sizeof(RenderTapeObjectDefineHeader) == 88u);
static_assert(alignof(RenderTapeObjectDefineHeader) == 8u);
static_assert(sizeof(RenderTapeObjectDestroyHeader) == 16u);
static_assert(alignof(RenderTapeObjectDestroyHeader) == 8u);
static_assert(sizeof(RenderTapeResourceMutationHeader) == 80u);
static_assert(alignof(RenderTapeResourceMutationHeader) == 8u);
static_assert(sizeof(RenderTapeCommandChunkHeader) == 16u);
static_assert(alignof(RenderTapeCommandChunkHeader) == 4u);
static_assert(sizeof(RenderTapeOrderedControlHeader) == 48u);
static_assert(alignof(RenderTapeOrderedControlHeader) == 8u);
static_assert(sizeof(RenderTapePresentCompleteHeader) == 64u);
static_assert(alignof(RenderTapePresentCompleteHeader) == 8u);
static_assert(sizeof(RenderTapeOracleAttachment) == 24u);
static_assert(alignof(RenderTapeOracleAttachment) == 8u);

enum class RenderTapeObjectDefineValidationSubreason : std::uint8_t {
  None,
  InvalidIdentity,
  DescriptorKindMismatch,
  DescriptorBytesZero,
  ReservedFields,
  ExpectedContentPair,
  ExpectedContentUnsupported,
  PayloadValidity,
  ImmutablePayloadRequired,
  ImmutablePayloadBytes,
  ImmutablePayloadDigest,
  DescriptorExtent,
  TextureDescriptorSchema,
  TextureDescriptorDimension,
  TextureDescriptorExtent,
  TextureDescriptorDisposition,
  SurfaceDescriptorStorage,
  SurfaceDescriptorDisposition,
  SurfaceDescriptorParent,
  SurfaceDescriptorExtent,
  SurfaceDescriptorSchema,
  SurfaceParentMismatch,
  DescriptorContentDisposition,
  ParentLifetime,
};

struct RenderTapeObjectDefineValidationDetail {
  RenderTapeObjectDefineValidationSubreason subreason =
      RenderTapeObjectDefineValidationSubreason::None;
  D9CWireObjectIdentity identity{};
  std::uint32_t descriptorKind = 0u;
  std::uint32_t descriptorBytes = 0u;
  std::uint32_t descriptorPayloadBytes = 0u;
  std::uint32_t payloadValidity = 0u;
  std::uint64_t immutablePayloadBytes = 0u;
  std::uint64_t expectedContentBytes = 0u;
  std::uint32_t expectedContentCount = 0u;
  std::uint32_t descriptorSchemaVersion = 0u;
  std::uint32_t descriptorDimension = 0u;
  std::uint32_t descriptorMipLevelCount = 0u;
  std::uint32_t descriptorSubresourceCount = 0u;
  std::uint32_t descriptorStorage = 0u;
  std::uint32_t descriptorDisposition = 0u;
  std::uint32_t descriptorSubresource = 0u;
  std::uint64_t descriptorExtentBytes = 0u;
  std::uint64_t descriptorExpectedExtentBytes = 0u;
  D9CWireObjectIdentity parentTexture{};

  bool valid() const noexcept {
    return subreason != RenderTapeObjectDefineValidationSubreason::None;
  }
};

const char* renderTapeObjectDefineValidationSubreasonName(
    RenderTapeObjectDefineValidationSubreason subreason) noexcept;

RenderTapeObjectDefineValidationDetail
renderTapeClassifyObjectDefineValidation(
    const RenderTapeObjectDefineHeader& fixed,
    std::span<const std::byte> descriptor) noexcept;

enum class RenderTapeValidationStatus : std::uint8_t {
  Valid,
  MissingHeader,
  InvalidHeader,
  InvalidLayout,
  NonZeroReserved,
  InvalidEventType,
  InvalidEventFlags,
  InvalidEventOrdinal,
  InvalidEventRange,
  NonCanonicalEventLayout,
  NonZeroPadding,
  MissingBootstrap,
  BootstrapNotFirst,
  DuplicateBootstrap,
  InvalidBootstrap,
  BootstrapCoverageIncomplete,
  BootstrapForbiddenRecord,
  InvalidBootstrapChunk,
  DuplicateGeneration,
  UnknownIdentity,
  RetainedSlotReuse,
  DescriptorMismatch,
  InvalidObjectDefine,
  InvalidObjectDestroy,
  InvalidMutationKind,
  InvalidMutationRange,
  UnknownBlob,
  BlobSizeMismatch,
  BlobDigestMismatch,
  InvalidCommandChunk,
  InvalidControlKind,
  InvalidControlSize,
  NonMonotoneCompletion,
  ResetNotTerminal,
  InvalidPresentComplete,
  PresentNotLast,
  IncompleteFrame,
  ScratchAllocationFailed,
};

struct RenderTapeValidationResult {
  RenderTapeValidationStatus status = RenderTapeValidationStatus::MissingHeader;
  std::uint32_t failedEventIndex = 0xffffffffu;
  CommandChunkValidationStatus chunkStatus =
      CommandChunkValidationStatus::Valid;
  RenderTapeObjectDefineValidationDetail objectDefine{};

  bool valid() const noexcept {
    return status == RenderTapeValidationStatus::Valid;
  }
};

enum class RenderTapeBootstrapReplayMode : std::uint8_t {
  JournalOnlyDeferredProvider = 1u,
};

struct ImportedRenderTapeEventView {
  RenderTapeEventHeader header{};
  std::span<const std::byte> payload{};
};

struct ImportedRenderTapeView {
  RenderTapeHeader header{};
  std::span<const RenderTapeEventHeader> events{};
  std::span<const std::byte> payloadArena{};

  ImportedRenderTapeEventView event(std::size_t index) const noexcept;
};

// In-memory blob catalogue input to validation. ResourceMutation events name a
// SHA-256 digest + size instead of inlining mutation bytes; validation fails
// before replay when a referenced blob is unknown, size-mismatched, or
// digest-mismatched.
struct RenderTapeBlob {
  RenderTapeDigest digest{};
  std::uint64_t size = 0u;
  std::uint32_t verified = 0u;
  std::uint32_t reserved0 = 0u;
};

enum class RenderTapeBlobLookup : std::uint8_t {
  Exact,
  UnknownDigest,
  SizeMismatch,
  Unverified,
};

struct RenderTapeBlobCatalogue {
  std::vector<RenderTapeBlob> blobs;

  RenderTapeBlobLookup
  lookup(std::span<const std::byte, kRenderTapeDigestSize> digest,
         std::uint64_t size) const noexcept;
};

struct RenderTapeValidationScratch {
  struct ObjectDefinition {
    D9CWireObjectIdentity identity{};
    std::uint32_t descriptorKind = 0u;
    std::uint32_t eventIndex = 0xffffffffu;
  };

  struct LiveSlot {
    D9CWireObjectIdentity identity{};
    RenderTapeLogicalObjectSlot logicalSlot{};
    std::uint32_t descriptorKind = 0u;
    std::uint64_t lastUseOrdinal = 0u;
    bool retired = false;
  };

  struct SeedContentExpectation {
    D9CWireObjectIdentity identity{};
    std::uint64_t expectedBytes = 0u;
    std::uint32_t expectedCount = 0u;
    std::uint64_t recordedBytes = 0u;
    std::uint32_t recordedCount = 0u;
  };

  struct SeedSubresource {
    D9CWireObjectIdentity identity{};
    std::uint32_t subresource = 0u;
  };

  struct ProducedPassObligation {
    D9CWireObjectIdentity identity{};
    std::uint32_t definitionEventIndex = 0xffffffffu;
    bool resolved = false;
  };

  // The definition index is populated before the ordered semantic pass. It
  // lets BootstrapState close handles against definitions journaled after the
  // bootstrap event without borrowing any event payload storage.
  std::vector<ObjectDefinition> objectDefinitions;
  std::vector<LiveSlot> liveObjects;
  // Initial-content accounting is deliberately separate from the ordered
  // generation registry. The latter remains identity/lifetime-only while
  // this bounded side table proves unique subresource seeds close their
  // ObjectDefine extent before the first non-seed event.
  std::vector<SeedContentExpectation> seedContentExpectations;
  std::vector<SeedSubresource> seedSubresources;
  std::vector<ProducedPassObligation> producedPassObligations;
  CommandChunkValidationScratch chunk;
};

RenderTapeValidationResult
validateRenderTape(std::span<const std::byte> blob,
                   const RenderTapeBlobCatalogue& catalogue,
                   ImportedRenderTapeView* out,
                   RenderTapeValidationScratch& scratch) noexcept;

RenderTapeValidationResult
validateRenderTape(std::span<const std::byte> blob,
                   const RenderTapeBlobCatalogue& catalogue,
                   ImportedRenderTapeView* out = nullptr) noexcept;

bool importPrevalidatedRenderTape(std::span<const std::byte> blob,
                                  ImportedRenderTapeView& out) noexcept;

class RenderTapeReplaySink {
public:
  virtual ~RenderTapeReplaySink() = default;

  // BootstrapState is a journal callback only. Provider application is
  // intentionally deferred until the consumer has journaled the later
  // ObjectDefine events and explicitly owns that application boundary.
  virtual bool bootstrap(const RenderTapeBootstrapHeader& fixed,
                         std::span<const std::byte> overlayChunks,
                         RenderTapeBootstrapReplayMode mode) = 0;
  virtual bool objectDefine(const RenderTapeObjectDefineHeader& fixed,
                            std::span<const std::byte> descriptor) = 0;
  virtual bool objectDestroy(const RenderTapeObjectDestroyHeader& fixed) = 0;
  virtual bool
  resourceMutation(const RenderTapeResourceMutationHeader& fixed) = 0;
  virtual bool commandChunk(const CommandChunkEnvelope& envelope,
                            std::span<const std::byte> chunk) = 0;
  virtual bool orderedControl(const RenderTapeOrderedControlHeader& fixed,
                              std::span<const std::byte> controlPayload) = 0;
  virtual bool presentComplete(const RenderTapePresentCompleteHeader& fixed,
                               std::span<const std::byte> oracleAttachments) = 0;
};

struct RenderTapeReplayResult {
  bool complete = false;
  std::uint32_t failedEventIndex = 0xffffffffu;
};

enum class RenderTapeReductionStatus : std::uint8_t {
  Valid,
  InvalidSource,
  InvalidSelection,
  UnsupportedEvent,
  MissingPresentSelection,
  ClosureFailure,
  OutputValidationFailed,
  AllocationFailed,
};

struct RenderTapeReductionResult {
  RenderTapeReductionStatus status = RenderTapeReductionStatus::InvalidSource;
  // On failure this is the source or reduced-tape validation evidence. The
  // output vectors remain empty and bytes is never partially populated.
  RenderTapeValidationResult validation{};
  RenderTapeValidationResult sourceValidation{};
  std::vector<std::byte> bytes;
  std::vector<std::uint32_t> retainedSourceEventIndices;
  std::vector<RenderTapeDigest> referencedBlobDigests;

  bool valid() const noexcept {
    return status == RenderTapeReductionStatus::Valid;
  }
};

RenderTapeReplayResult
replayPrevalidatedRenderTape(const ImportedRenderTapeView& tape,
                             const RenderTapeBlobCatalogue& catalogue,
                             RenderTapeReplaySink& sink) noexcept;

class RenderTapeBuilder {
public:
  explicit RenderTapeBuilder(
      std::uint32_t profile = kRenderTapeProfileFrame) : profile_(profile) {}

  // Copies an already validated event payload verbatim. seal() still rebuilds
  // event ordinals, offsets, padding, and presentCount canonically.
  void appendRawEvent(RenderTapeEventType type,
                      std::span<const std::byte> payload);
  void appendBootstrapState(std::span<const std::byte> overlayChunks);
  void appendObjectDefine(const D9CWireObjectIdentity& identity,
                          std::uint32_t descriptorKind,
                          std::span<const std::byte> descriptor,
                          std::uint64_t immutablePayloadBytes,
                          RenderTapeDigest immutablePayloadDigest,
                          std::uint64_t expectedContentBytes = 0u,
                          std::uint32_t expectedContentCount = 0u);
  void appendObjectDestroy(const D9CWireObjectIdentity& identity);
  void appendResourceMutation(const D9CWireObjectIdentity& identity,
                              RenderTapeMutationKind kind,
                              std::uint32_t subresource,
                              std::uint64_t byteOffset, std::uint64_t byteSize,
                              std::span<const std::byte, kRenderTapeDigestSize>
                                  digest);
  void appendCommandChunk(const CommandChunkEnvelope& envelope,
                          std::span<const std::byte> chunk);
  void appendOrderedControl(const RenderTapeOrderedControlHeader& fixed,
                            std::span<const std::byte> controlPayload);
  void appendPresentComplete(std::uint64_t presentOrdinal,
                             std::uint64_t completionOrdinal,
                             RenderTapeDigestValidity digestValidity,
                             RenderTapeDigest expectedDigest,
                             std::span<const std::byte> oracleAttachments);

  std::vector<std::byte> seal() const;
  std::size_t eventCount() const noexcept { return events_.size(); }
  std::uint32_t profile() const noexcept { return profile_; }

private:
  struct PendingEvent {
    RenderTapeEventType type = RenderTapeEventType::BootstrapState;
    std::vector<std::byte> payload;
  };

  void append(RenderTapeEventType type, std::vector<std::byte> payload);

  std::uint32_t profile_ = kRenderTapeProfileFrame;
  std::vector<PendingEvent> events_;
};

const char* renderTapeProfileName(std::uint32_t profile) noexcept;

const char*
renderTapeValidationStatusName(RenderTapeValidationStatus status) noexcept;

const char* renderTapeReductionStatusName(
    RenderTapeReductionStatus status) noexcept;

// Reduces a validated whole-frame tape to the selected CommandChunk events.
// Input bytes and catalogue entries are borrowed only for the duration of the
// call; successful output owns all returned storage.
RenderTapeReductionResult reduceRenderTape(
    std::span<const std::byte> source,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::uint32_t> selectedCommandEventIndices) noexcept;

} // namespace dxmt9::d3d9
