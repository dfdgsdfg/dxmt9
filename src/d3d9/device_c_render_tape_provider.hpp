#pragma once

#include "device_c_render_tape.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct D9CDevice;

namespace dxmt9::d3d9 {

struct RenderTapeProviderBlob {
  RenderTapeDigest digest{};
  std::span<const std::byte> bytes{};
};

struct RenderTapeBufferMutationReplayPlan {
  std::uint32_t byteOffset = 0u;
  std::uint32_t byteSize = 0u;
  std::uint32_t lockFlags = 0u;
};

// Pure provider-side conversion used by both replay paths. It keeps the
// Windows D3D9 lock constants out of the unix ABI while pinning exact range
// narrowing and lock-disposition semantics for native tests.
std::uint32_t renderTapeBufferMutationLockFlags(
    RenderTapeBufferMutationDisposition disposition) noexcept;
bool renderTapeBufferMutationReplayPlan(
    const RenderTapeResourceMutationHeader& mutation,
    RenderTapeBufferMutationReplayPlan& out) noexcept;

enum class FrameTapeReplayStatus : std::uint8_t {
  Complete,
  InvalidTape,
  InvalidBlobCatalogue,
  UnsupportedGrammar,
  ObjectCreationFailed,
  MutationFailed,
  BootstrapFailed,
  CommandReplayFailed,
  PresentOutputFailed,
  ReadbackFailed,
  OutputMismatch,
  WarmResetUnsupported,
  WarmResetFailed,
  QueryOracleUnsupported,
  QueryOracleFailed,
  QueryProgressExhausted,
};

enum class RenderTapeQueryReplayFailure : std::uint8_t {
  None,
  UnsupportedResultKind,
  SnapshotUnavailable,
  CycleMismatch,
  PollFailed,
  ResultMismatch,
  ProgressExhausted,
  BackendUnavailable,
};

enum class RenderTapeQueryProgressLimit : std::uint8_t {
  None,
  PollBudget,
  WallBudget,
};

// Cold provider evidence, one row per original GetData call. Queue/sequence
// fields are actual local query receipts; captured provenance stays on the
// immutable control and is never imported as completion authority.
struct RenderTapeQueryReplayRow {
  std::uint32_t eventIndex = 0u;
  std::uint64_t eventOrdinal = 0u;
  D9CWireObjectIdentity identity{};
  std::uint64_t issueCycle = 0u;
  std::uint32_t flags = 0u;
  bool hasOutput = false;
  std::uint64_t dataSize = 0u;
  std::int32_t capturedResult = 0;
  std::int32_t replayResult = 0;
  std::int32_t oracleResult = 0;
  std::uint32_t definedBytes = 0u;
  std::array<std::byte, 8> capturedBytes{};
  std::array<std::byte, 8> replayBytes{};
  std::uint64_t localQueryIdentity = 0u;
  std::uint64_t localIssueOrdinal = 0u;
  std::uint64_t localDeviceEpoch = 0u;
  std::uint64_t localQueue = 0u;
  std::uint64_t localSequence = 0u;
  std::uint64_t localInitializer = 0u;
  std::uint64_t completedSequence = 0u;
  std::uint64_t completedInitializer = 0u;
  std::uint32_t oraclePolls = 0u;
  bool resultRequired = false;
  bool readinessSatisfied = false;
  bool resultMatched = false;
  bool resultSubstituted = false;
  bool supplemental = false;
};

// Initial completed EVENT import is setup, never an original transcript issue
// or a captured GPU result comparison. Captured labels authenticate the cut;
// only the new local END snapshot can establish replay completion authority.
struct RenderTapeQueryCheckpointReplayRow {
  D9CWireObjectIdentity identity{};
  std::uint64_t capturedIssueCycle = 0u;
  std::uint64_t capturedIssueOrdinal = 0u;
  std::uint64_t capturedDeviceEpoch = 0u;
  std::uint64_t localQueryIdentity = 0u;
  std::uint64_t localIssueOrdinal = 0u;
  std::uint64_t localDeviceEpoch = 0u;
  std::uint64_t localQueue = 0u;
  std::uint64_t localSequence = 0u;
  std::uint64_t localInitializer = 0u;
  std::uint64_t completedSequence = 0u;
  std::uint64_t completedInitializer = 0u;
  std::uint32_t setupPolls = 0u;
  bool setupIssued = false;
  bool completed = false;
  bool activated = false;
};

struct RenderTapeQueryReplayEvidence {
  bool present = false;
  bool transcriptComplete = false;
  bool oracleComplete = false;
  bool actualMetal = false;
  // CPU replay may prove readiness/order but cannot compare GPU samples.
  bool gpuResultOracle = false;
  RenderTapeQueryProgressLimit progressLimit = RenderTapeQueryProgressLimit::None;
  std::uint32_t queryDefinitions = 0u;
  std::uint32_t issueRecords = 0u;
  std::uint32_t replayedIssueRecords = 0u;
  std::uint32_t recordedPolls = 0u;
  std::uint32_t replayedPolls = 0u;
  std::uint32_t capturedPending = 0u;
  std::uint32_t replayPending = 0u;
  std::uint32_t capturedReady = 0u;
  std::uint32_t replayReady = 0u;
  std::uint32_t requiredResults = 0u;
  std::uint32_t matchedResults = 0u;
  std::uint32_t readinessRequired = 0u;
  std::uint32_t readinessSatisfied = 0u;
  std::uint32_t substitutedResults = 0u;
  std::uint32_t supplementalPolls = 0u;
  std::uint32_t supplementalDrains = 0u;
  std::uint32_t checkpointDefinitions = 0u;
  std::uint32_t checkpointSetupEnds = 0u;
  std::uint32_t checkpointSetupPublications = 0u;
  std::uint32_t checkpointSetupPolls = 0u;
  std::uint32_t checkpointSetupWaits = 0u;
  std::uint32_t checkpointCompleted = 0u;
  std::uint32_t checkpointActivated = 0u;
  std::uint64_t checkpointSetupWallNanos = 0u;
  std::uint64_t transcriptWallNanos = 0u;
  std::uint64_t oracleWallNanos = 0u;
  std::uint32_t maxOraclePolls = 262144u;
  std::uint64_t maxOracleWallNanos = 5000000000ull;
  RenderTapeQueryReplayFailure failure = RenderTapeQueryReplayFailure::None;
  std::vector<RenderTapeQueryReplayRow> rows{};
  // Detailed setup rows, like transcript rows, are omitted in measurement.
  std::vector<RenderTapeQueryCheckpointReplayRow> checkpoints{};
};

// The prepared harness is intentionally cold-path only. Compatibility
// suppresses both direct routes; ProductionSlot retains production Slot
// admission and fallback. Historical provider replay is unchanged.
enum class RenderTapePreparedRoute : std::uint8_t {
  Compatibility,
  ProductionSlot,
};

enum class RenderTapePreparedReset : std::uint8_t {
  FreshDevice,
  WarmDevice,
};

enum class RenderTapeSourceBoundaryMode : std::uint8_t {
  Chunk,
  CaptureSource,
};

struct RenderTapeBatchCut {
  std::uint64_t eventOrdinal = 0u;
  std::uint32_t beforeRecord = 0u;
};

// Cold causal intervention, never a rendering policy. The caller owns the
// immutable bytes until the synchronous prepared call returns. Only one full
// single-level 2D R32F input is supported, at an authenticated source cut.
struct RenderTapeTextureInputAuditRequest {
  D9CWireObjectIdentity identity{};
  std::uint64_t afterCapturedSeqId = 0u;
  RenderTapeDigest digest{};
  std::span<const std::byte> bytes{};
};

struct RenderTapeTextureInputAuditEvidence {
  bool requested = false;
  bool validated = false;
  bool attempted = false;
  bool uploaded = false;
  bool readbackMatched = false;
  D9CWireObjectIdentity identity{};
  std::uint64_t afterCapturedSeqId = 0u;
  std::uint64_t byteCount = 0u;
  RenderTapeDigest inputDigest{};
  RenderTapeDigest beforeDigest{};
  RenderTapeDigest afterDigest{};
};

struct RenderTapePreparedExecutionOptions {
  RenderTapePreparedRoute route = RenderTapePreparedRoute::Compatibility;
  RenderTapePreparedReset reset = RenderTapePreparedReset::FreshDevice;
  // Cold-harness-only diagnostic. It snapshots only initial mutation seeds
  // after upload and before the first command effect; historical replay and
  // the ordinary prepared path leave this disabled.
  bool auditInitialSeeds = false;
  // Cold diagnostic for the narrowly proved fresh-owner mutation boundary.
  bool auditFreshSeedDrains = false;
  // Cold general-provider-only diagnostic. It stages eligible CompleteSeed
  // textures before logical replay and transfers them at their definitions.
  bool auditTexturePrestage = false;
  // Cold general-provider discriminator. Source cuts are authenticated against
  // the owned tape; they are not imported as this process's queue identities.
  bool auditSourceBoundaries = false;
  // Explicit normal reference policy, distinct from the source-cut diagnostic.
  RenderTapeSourceBoundaryMode sourceBoundaryMode = RenderTapeSourceBoundaryMode::Chunk;
  std::span<const std::byte> sourceBoundaryIdentity{};
  // Required by the authenticated CaptureSource policy.  The digest is a
  // value, rather than a borrowed text buffer, so the policy gate remains
  // valid for the entire prepared execution call.
  RenderTapeDigest expectedSourcePlanDigest{};
  bool hasExpectedSourcePlanDigest = false;
  // Optional diagnostic flushes strictly inside authenticated source ranges.
  // These end a CPU DrawRun batch, not a source publication or Metal pass.
  std::span<const RenderTapeBatchCut> auditBatchCuts{};
  const RenderTapeTextureInputAuditRequest* auditTextureInput = nullptr;
};

struct RenderTapeSourceBoundaryEvidence {
  bool requested = false;
  bool diagnostic = true;
  bool validated = false;
  bool complete = false;
  std::uint32_t sourceCount = 0u;
  std::uint32_t completedSourceCount = 0u;
  std::uint32_t batchCutCount = 0u;
  std::uint32_t completedBatchCutCount = 0u;
};

struct RenderTapeSourceReplayPublicationEvidence {
  std::uint32_t sourceIdIndex = 0u;
  std::uint64_t sourceIdGeneration = 0u;
  std::uint32_t storageFirstPage = 0u;
  std::uint32_t storagePageCount = 0u;
  std::uint64_t storageGeneration = 0u;
  std::uint64_t metadataRawOrdinal = 0u;
  std::uint64_t metadataSourceOrdinal = 0u;
  std::uint64_t metadataSeqId = 0u;
  std::uint64_t metadataBuildGeneration = 0u;
  std::uint64_t metadataUsedBytes = 0u;
  std::uint32_t metadataPageCount = 0u;
  std::uint32_t metadataPaddingPagesBefore = 0u;
  bool metadataStrictAdmission = false;
  std::uint64_t controlIndex = 0u;
  bool hasPresent = false;
};

enum class RenderTapeSourceReplaySettlement : std::uint8_t {
  Empty,
  Published,
  PresentPublished,
};

struct RenderTapeSourceReplayRowEvidence {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t capturedSeqId = 0u;
  std::uint32_t commandChunkIndex = 0u;
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  // These are provider-local settlement facts.  In particular, rawOrdinal
  // and every publication locator are never treated as captured authority.
  std::uint64_t actualRawOrdinal = 0u;
  std::uint32_t rangeIndex = 0u;
  std::uint32_t publicationOffset = 0u;
  std::uint32_t publicationCount = 0u;
  bool settled = false;
  RenderTapeSourceReplaySettlement settlement =
      RenderTapeSourceReplaySettlement::Empty;
};

struct RenderTapeSourceReplayEvidence {
  bool requested = false;
  bool validated = false;
  bool complete = false;
  RenderTapeDigest planDigest{};
  std::uint64_t sourceCount = 0u;
  std::uint64_t completedSourceCount = 0u;
  std::uint64_t publicationCount = 0u;
  std::uint64_t emptySourceCount = 0u;
  std::uint64_t presentSourceCount = 0u;
  bool storageOverflow = false;
  std::vector<RenderTapeSourceReplayRowEvidence> sources{};
  std::vector<RenderTapeSourceReplayPublicationEvidence> publications{};
};

struct RenderTapeFreshSeedDrainEvidence {
  bool requested = false;
  bool diagnostic = true;
  bool attempted = false;
  bool complete = false;
  // Coverage is for the resource-mutation preflush audit, independent of the
  // final pixel oracle. mutationCount includes initial mutations before the
  // first command; inspectedMutations advances only after each mutation is
  // successfully applied.
  std::uint32_t mutationCount = 0u;
  std::uint32_t inspectedMutations = 0u;
  std::uint32_t requiredDrains = 0u;
  std::uint32_t bypassedDrains = 0u;
};

struct RenderTapeTexturePrestageEvidence {
  // Object counts track provider-owned wrapper acquisitions/releases, not
  // every underlying Metal allocation or implicit device/upload resource.
  bool requested = false;
  bool diagnostic = true;
  bool attempted = false;
  bool complete = false;
  std::uint32_t candidateObjects = 0u;
  std::uint32_t candidateSubresources = 0u;
  std::uint64_t candidateBytes = 0u;
  std::uint32_t immediateUploadSubresources = 0u;
  std::uint32_t deferredUploadSubresources = 0u;
  std::uint32_t initializerFlushes = 0u;
  std::uint32_t initializerWaits = 0u;
  std::uint64_t initializerUploadValue = 0u;
  std::uint32_t activatedObjects = 0u;
  std::uint32_t consumedSeeds = 0u;
  std::uint32_t stagedObjectsRemaining = 0u;
  std::uint32_t stagedSubresourcesRemaining = 0u;
  std::uint64_t stagedBytesRemaining = 0u;
  std::uint32_t hiddenObjectsCreated = 0u;
  std::uint32_t hiddenObjectsReleased = 0u;
  std::uint32_t activeObjectsCreated = 0u;
  std::uint32_t activeObjectsReleased = 0u;
  std::uint32_t physicalObjectsCreated = 0u;
  std::uint32_t physicalObjectsReleased = 0u;
  bool physicalConservation = false;
};

// Pure admission predicate shared by the provider and native truth-table
// tests. It only admits a first, full seed of an independently-owned texture
// or buffer after a freshly defined generation and rejects aliases, partials,
// repeats, and any intervening command epoch.
bool renderTapeFreshSeedDrainBypassAllowed(
    const RenderTapeObjectDefineHeader& definition,
    std::span<const std::byte> descriptor,
    const RenderTapeResourceMutationHeader& mutation,
    std::span<const std::byte> mutationBytes,
    bool defineEpochMatchesCommandEpoch,
    bool hasSurfaceAlias,
    bool priorSeedForSubresource) noexcept;

// Pure admission predicate for the cold texture-prestage discriminator. The
// mutation span is the complete tape mutation table, not a replay prefix.
bool renderTapeTexturePrestageCandidate(
    const RenderTapeObjectDefineHeader& definition,
    std::span<const std::byte> descriptor,
    std::span<const RenderTapeResourceMutationHeader> mutations,
    bool hasSurfaceAlias, bool hasSlotGenerationConflict,
    bool invalidEventOrder) noexcept;

// Positions are in the validated immutable event stream, qualified to one
// exact texture owner. Seed-before-definition and intervening commands fail
// closed; native event reducers exercise this same production order check.
bool renderTapeTexturePrestageOrderAllowed(
    std::uint32_t definitionPosition,
    std::span<const std::uint32_t> seedPositions,
    std::span<const std::uint32_t> commandPositions) noexcept;

enum class RenderTapeInitialSeedAuditKind : std::uint8_t {
  Color,
  Depth,
};

enum class RenderTapeInitialSeedAuditFailure : std::uint8_t {
  None,
  Unsupported,
  BlobUnavailable,
  ObjectUnavailable,
  CaptureFailed,
  DigestMismatch,
};

struct RenderTapeInitialSeedAuditEntry {
  RenderTapeInitialSeedAuditKind kind = RenderTapeInitialSeedAuditKind::Color;
  D9CWireObjectIdentity identity{};
  std::uint32_t subresource = 0u;
  RenderTapeDigest expectedDigest{};
  RenderTapeDigest actualDigest{};
  std::uint64_t bytes = 0u;
  bool supported = false;
  bool match = false;
  RenderTapeInitialSeedAuditFailure failure =
      RenderTapeInitialSeedAuditFailure::Unsupported;
};

struct RenderTapeInitialSeedAuditEvidence {
  bool requested = false;
  // The capture flush/readback can perturb queue/resource state. This is a
  // diagnostic result and must never be included in promotion/performance
  // claims or used to waive the normal output oracle.
  bool diagnostic = true;
  bool perturbative = true;
  bool attempted = false;
  bool complete = false;
  std::uint32_t count = 0u;
  std::uint32_t matched = 0u;
  std::uint32_t supportedCount = 0u;
  std::uint32_t mismatchedCount = 0u;
  std::uint32_t unsupportedCount = 0u;
  std::uint32_t captureFailedCount = 0u;
  std::uint64_t wallNanos = 0u;
  std::vector<RenderTapeInitialSeedAuditEntry> entries{};
};

struct RenderTapePreparedTiming {
  std::uint64_t prepareWallNanos = 0u;
  std::uint64_t resetWallNanos = 0u;
  std::uint64_t resourceSetupNanos = 0u;
  std::uint64_t chunkReplayNanos = 0u;
  std::uint64_t readbackDrainNanos = 0u;
  std::uint64_t cleanupNanos = 0u;
  std::uint64_t replayWallNanos = 0u;
};

struct RenderTapePreparedEvidence {
  bool immutableInputOwned = false;
  bool preflightCompleted = false;
  bool planReused = false;
  bool resetPerformed = false;
  bool resetVerified = false;
  std::uint64_t prepareCount = 0u;
  std::uint64_t tapeBytes = 0u;
  std::uint64_t blobBytes = 0u;
  std::uint32_t blobCount = 0u;
  RenderTapePreparedRoute requestedRoute = RenderTapePreparedRoute::Compatibility;
  RenderTapePreparedReset reset = RenderTapePreparedReset::FreshDevice;
  RenderTapePreparedTiming timing{};
};

struct FrameTapeValidityEvidence {
  bool structurallyValid = false;
  bool digestsValid = false;
  bool outputReadback = false;
  bool expectedDigestCaptured = false;
  bool expectedDigestMatched = false;
  bool expectedPixelsCompared = false;
  bool pixelEnvelopeMatched = false;
  bool outputNonDegenerate = false;
  std::uint64_t outputBytes = 0u;
  std::uint64_t allowedDifferingPixels = 0u;
  std::uint64_t differingPixels = 0u;
  std::uint64_t totalRgbDelta = 0u;
  std::uint64_t differingAlphaPixels = 0u;
  std::uint32_t maxRgbDelta = 0u;
  RenderTapeDigest expectedOutputDigest{};
  RenderTapeDigest outputDigest{};
  bool sourceReadback = false;
  std::uint64_t sourceBytes = 0u;
  RenderTapeDigest sourceDigest{};
};

struct FrameTapeReplayRequirements {
  std::uint32_t outputWidth = 0u;
  std::uint32_t outputHeight = 0u;
  std::uint32_t outputFormat = 0u;
};

enum class FrameTapeBootstrapOutputDisposition : std::uint8_t {
  Malformed,
  ImplicitDefault,
  ExplicitExact,
  ExplicitNull,
  WrongIdentity,
  SlotOutOfRange,
  Ambiguous,
};

struct FrameTapeCoverageEvidence {
  std::uint32_t eventCount = 0u;
  std::uint32_t objectDefinitions = 0u;
  std::uint32_t seedMutations = 0u;
  std::uint32_t bootstrapChunks = 0u;
  std::uint32_t commandChunks = 0u;
  std::uint32_t commandRecords = 0u;
  std::uint32_t clearRecords = 0u;
  std::uint32_t drawPrimitiveRecords = 0u;
  std::uint32_t drawIndexedPrimitiveRecords = 0u;
  std::uint32_t drawPrimitiveUpRecords = 0u;
  std::uint32_t stateConstantRecords = 0u;
  std::uint32_t applyStateRecords = 0u;
  std::uint32_t presentRecords = 0u;
  std::uint32_t presentSourceMappings = 0u;
  std::uint32_t presentOutputs = 0u;
  std::uint32_t objectDestroys = 0u;
};

struct FrameTapeConservationEvidence {
  std::uint32_t inputBlobs = 0u;
  std::uint32_t referencedBlobs = 0u;
  std::uint32_t objectsCreated = 0u;
  std::uint32_t objectsReleased = 0u;
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
};

inline constexpr std::uint32_t kRenderTapeMaxReplayIntervals = 2u;

struct RenderTapeIntervalEvidence {
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
  FrameTapeValidityEvidence validity{};
};

struct FrameTapeReplayResult {
  FrameTapeReplayStatus status = FrameTapeReplayStatus::InvalidTape;
  std::uint32_t failedEventIndex = 0xffffffffu;
  std::uint32_t profile = 0u;
  std::uint32_t intervalCount = 0u;
  FrameTapeReplayRequirements requirements{};
  FrameTapeValidityEvidence validity{};
  FrameTapeCoverageEvidence coverage{};
  FrameTapeConservationEvidence conservation{};
  std::array<RenderTapeIntervalEvidence, kRenderTapeMaxReplayIntervals>
      intervals{};
  // Cold replay-harness artifact: tightly packed bytes from the last
  // offscreen Present readback. Production rendering never constructs this
  // result type, and callers that only need the digest may ignore the bytes.
  std::vector<std::byte> outputPixels{};
  // Cold replay-harness artifact: source backbuffer bytes immediately after
  // the Present command drained. This is never a replay input.
  std::vector<std::byte> sourcePixels{};
  // These fields are populated only by the prepared cold harness.  Existing
  // provider replay leaves them value-initialized and pays no timing cost.
  RenderTapePreparedEvidence prepared{};
  // Populated only when the prepared cold harness is explicitly asked to run
  // the perturbative initial-seed diagnostic.
  RenderTapeInitialSeedAuditEvidence initialSeedAudit{};
  RenderTapeFreshSeedDrainEvidence freshSeedDrainAudit{};
  RenderTapeTexturePrestageEvidence texturePrestageAudit{};
  RenderTapeSourceBoundaryEvidence sourceBoundaryAudit{};
  RenderTapeTextureInputAuditEvidence textureInputAudit{};
  RenderTapeSourceReplayEvidence sourceReplay{};
  RenderTapeQueryReplayEvidence queryReplay{};

  bool complete() const noexcept {
    return status == FrameTapeReplayStatus::Complete;
  }
};

// Owns a validated tape, all referenced blob bytes, and the immutable
// preflight plan.  It is move-only so the PIMPL remains stable while the
// underlying spans in the plan continue to refer to owned storage.
class PreparedRenderTape {
 public:
  PreparedRenderTape();
  ~PreparedRenderTape();
  PreparedRenderTape(PreparedRenderTape&&) noexcept;
  PreparedRenderTape& operator=(PreparedRenderTape&&) noexcept;
  PreparedRenderTape(const PreparedRenderTape&) = delete;
  PreparedRenderTape& operator=(const PreparedRenderTape&) = delete;

  const FrameTapeReplayResult& preflightResult() const noexcept;
  const FrameTapeReplayResult& preflight() const noexcept;
  const RenderTapePreparedEvidence& evidence() const noexcept;
  std::uint64_t prepareCount() const noexcept;
  bool valid() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend PreparedRenderTape prepareRenderTapeIdentity(
      std::span<const std::byte>, std::span<const RenderTapeProviderBlob>) noexcept;
  friend FrameTapeReplayResult replayPreparedRenderTapeIdentity(
      D9CDevice*, const PreparedRenderTape&,
      RenderTapePreparedExecutionOptions) noexcept;
};

// Classifies the only accepted bootstrap attachment forms without changing
// the canonical wire bytes. The implicit form is the production default RT0
// created and bound by the replay device itself.
FrameTapeBootstrapOutputDisposition classifyFrameTapeBootstrapOutput(
    std::span<const std::byte> bytes, const CommandChunkEnvelope& envelope,
    const D9CWireObjectIdentity& output,
    std::uint32_t baselineProfileVersion = kRenderTapeBaselineProfileVersion) noexcept;

// Event-ordering predicate used by replay: a CPU-visible mutation or object
// retirement must not overtake command work submitted by an earlier event.
bool renderTapeProviderEventRequiresDrain(
    bool submittedCommandWork, RenderTapeEventType event) noexcept;

// Transactional admission for the one-frame identity grammar. No provider or
// device operation is performed until this returns Complete.
FrameTapeReplayResult preflightFrameTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept;

// Replays one preflighted Clear -> [textured DrawPrimitiveUP ->] Present frame
// through production factories, mutation APIs, DeviceReplaySink and the
// production queue. On Metal devices a typed offscreen PresentOutput is
// installed and read back after the completion waterline. Stub devices still
// exercise the exact replay routing deterministically.
FrameTapeReplayResult replayFrameTapeIdentity(
    D9CDevice* device, std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept;

// Profile-aware entry points additionally admit exactly two textured Present
// intervals separated by one full digest-backed texture mutation. Both
// completions and output digests are conserved independently. The frame
// wrappers above retain their historical strictness and reject a sequence tape
// before provider effects.
FrameTapeReplayResult preflightRenderTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept;

FrameTapeReplayResult replayRenderTapeIdentity(
    D9CDevice* device, std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept;

// Copies the complete input and performs structural plus digest preflight once.
// The returned object remains self-contained after caller-owned spans mutate or
// go out of scope.
PreparedRenderTape prepareRenderTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept;

FrameTapeReplayResult replayPreparedRenderTapeIdentity(
    D9CDevice* device, const PreparedRenderTape& prepared,
    RenderTapePreparedExecutionOptions options = {}) noexcept;

// Applies the frame-tape pixel oracle only after strict digest comparison has
// failed. The expected bytes must be the digest-authenticated production
// capture sidecar; this never weakens tape/blob validation or sequence replay.
bool applyRenderTapePixelOracleEnvelope(
    FrameTapeReplayResult& result,
    std::span<const std::byte> expectedPixels) noexcept;

// Source-oracle relaxation is permitted only when it is byte-for-byte the
// same comparison as an already authenticated and accepted output envelope.
bool renderTapeSourceOracleMatchesOutputEvidence(
    std::span<const std::byte> expectedSource,
    std::span<const std::byte> actualSource,
    std::span<const std::byte> expectedOutput,
    std::span<const std::byte> actualOutput,
    bool outputPixelEnvelopeMatched) noexcept;

const char* frameTapeReplayStatusName(FrameTapeReplayStatus status) noexcept;

} // namespace dxmt9::d3d9
