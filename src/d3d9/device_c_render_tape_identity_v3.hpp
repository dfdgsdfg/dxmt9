#pragma once

#include "device_c_render_tape_identity.hpp"
#include "device_c_ordered_control.hpp"
#include "dxmt9/query_end_marker.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace dxmt9::d3d9 {

inline constexpr std::uint32_t kRenderTapeIdentityV3Version = 3u;
inline constexpr char kRenderTapeIdentityV3Schema[] =
    "dxmt9.render_tape.identity.v3";
inline constexpr std::uint32_t kRenderTapeIdentityV3NoIndex = UINT32_MAX;
inline constexpr std::uint32_t kRenderTapeIdentityV3MaxRows = 1048576u;

enum class RenderTapeIdentityV3DispositionKind : std::uint32_t {
  GpuCommand = 1u,
  StateOnly = 2u,
  Coordinator = 3u,
};

enum class RenderTapeIdentityV3Membership : std::uint32_t {
  None = 0u,
  Pass = 1u,
  // A real queue command can exist without a render/compute/blit pass.
  NoPassCommand = 2u,
};

// Stable sidecar alphabet, independently versioned from MetalCommandKind.
enum class RenderTapeIdentityV3CommandKind : std::uint32_t {
  Invalid = 0u,
  DrawRun = 1u,
  Clear = 2u,
  SurfaceCopy = 3u,
  StretchRect = 4u,
  Readback = 5u,
  ColorFill = 6u,
  DepthResolve = 7u,
  GenerateMipmaps = 8u,
  Present = 9u,
};

enum class RenderTapeIdentityV3CutKind : std::uint32_t {
  BeforeEvent = 1u,
  AfterRecord = 2u,
  AfterEvent = 3u,
};

enum class RenderTapeIdentityV3PrefixKind : std::uint32_t {
  None = 0u,
  Unbound = 1u,
  EmptyBaseline = 2u,
  ArmBaseline = 3u,
  RealSource = 4u,
};

enum class RenderTapeIdentityV3Observation : std::uint32_t {
  None = 0u,
  Pending = 1u,
  Ready = 2u,
  Completed = 3u,
};

struct RenderTapeIdentityV3Table {
  std::uint32_t offset = 0u;
  std::uint32_t count = 0u;
  std::uint32_t entrySize = 0u;
};

// This is a separate grammar, not an extension of the v2 header's reserved
// bytes. All offsets are canonical, bounded, little-endian POD values.
struct RenderTapeIdentityV3Header {
  std::uint64_t magic = kRenderTapeIdentityMagic;
  std::uint64_t eventsBytes = 0u;
  std::uint64_t frameId = 0u;
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t captureToken = 0u;
  RenderTapeDigest eventsDigest{};
  std::uint32_t version = kRenderTapeIdentityV3Version;
  std::uint32_t headerSize = sizeof(RenderTapeIdentityV3Header);
  // Preserve the existing common dispatch prefix; v2 bytes stay unchanged.
  std::uint64_t captureEpoch = 0u;
  std::uint64_t queue = 0u;
  std::uint64_t baselineSequence = 0u;
  std::uint64_t baselineInitializer = 0u;
  std::uint64_t baselineCompletedSequence = 0u;
  std::uint64_t initializerEpoch = 0u;
  std::uint64_t baselineCompletedInitializer = 0u;
  // A successful quiescent arm checkpoint, including explicit zero work.
  std::uint64_t baselineCompletionEdge = 0u;
  std::uint32_t baselineKind = 0u;
  std::uint32_t authority =
      static_cast<std::uint32_t>(RenderTapeIdentityAuthority::Capture);
  RenderTapeIdentityV3Table sources{};
  RenderTapeIdentityV3Table initializers{};
  RenderTapeIdentityV3Table commands{};
  RenderTapeIdentityV3Table dispositions{};
  RenderTapeIdentityV3Table coordinators{};
  RenderTapeIdentityV3Table settlements{};
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
  std::uint32_t reserved2 = 0u;
  std::uint32_t reserved3 = 0u;
};

// Exactly one row per successful publication. Writer IDs are capture-local
// association IDs, not reserved source IDs, physical Slots or pointers.
// Only this table owns render-source submission and GPU-completion evidence.
struct RenderTapeIdentityV3Source {
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t seqId = 0u;
  std::uint64_t writerCaptureId = 0u;
  std::uint64_t writerGeneration = 0u;
  std::uint64_t storageGeneration = 0u;
  std::uint64_t publicationEdge = 0u;
  std::uint64_t submissionEdge = 0u;
  std::uint64_t completionEdge = 0u;
  std::uint64_t completedSequence = 0u;
  // The actual triggering position, independent of all covered records.
  std::uint64_t cutEventOrdinal = 0u;
  std::uint32_t commandCount = 0u;
  std::uint32_t cutKind = 0u;
  std::uint32_t cutRecord = 0u;
  std::uint32_t reserved0 = 0u;
};

// Initializer/upload publication is an independent sequence domain. An END
// may publish an initializer without publishing any render source.
struct RenderTapeIdentityV3Initializer {
  std::uint64_t sequence = 0u;
  std::uint64_t epoch = 0u;
  std::uint64_t publicationEdge = 0u;
  std::uint64_t submissionEdge = 0u;
  std::uint64_t completionEdge = 0u;
  std::uint64_t completedSequence = 0u;
  std::uint64_t cutEventOrdinal = 0u;
  std::uint32_t cutKind = 0u;
  std::uint32_t cutRecord = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
};

// One actual physical command, irrespective of how many canonical draw
// records contributed to its DrawRun. Pass membership is owned once here.
struct RenderTapeIdentityV3Command {
  std::uint64_t logicalPassId = 0u;
  std::uint32_t sourceIndex = kRenderTapeIdentityV3NoIndex;
  std::uint32_t physicalCommandIndex = 0u;
  std::uint32_t kind = 0u;
  std::uint32_t drawCount = 0u;
  std::uint32_t membership = 0u;
  std::uint32_t dagPassIndex = kRenderTapeIdentityV3NoIndex;
  std::uint32_t passKind = 0u;
  std::uint32_t coordinatorIndex = kRenderTapeIdentityV3NoIndex;
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
};

struct RenderTapeIdentityV3Disposition {
  std::uint64_t eventOrdinal = 0u;
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t kind = 0u;
  std::uint32_t commandIndex = kRenderTapeIdentityV3NoIndex;
  std::uint32_t firstSubdraw = 0u;
  std::uint32_t subdrawCount = 0u;
  std::uint32_t coordinatorIndex = kRenderTapeIdentityV3NoIndex;
  std::uint32_t reserved0 = 0u;
};

struct RenderTapeIdentityV3Coordinator {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint64_t buildGeneration = 0u;
  std::uint64_t dispatchEdge = 0u;
  // Actual ordered-session release acknowledgment, not GPU completion.
  std::uint64_t releaseOrdinal = 0u;
  std::uint64_t releaseGeneration = 0u;
  D9CWireObjectIdentity queryIdentity{};
  std::uint64_t issueCycle = 0u;
  std::uint64_t issueOrdinal = 0u;
  std::uint64_t deviceEpoch = 0u;
  std::uint64_t writerCaptureId = 0u;
  std::uint64_t writerGeneration = 0u;
  std::uint64_t storageGeneration = 0u;
  std::uint64_t prefixSequence = 0u;
  std::uint64_t prefixInitializer = 0u;
  std::uint64_t prefixInitializerEpoch = 0u;
  std::uint32_t recordIndex = 0u;
  std::uint32_t recordType = 0u;
  std::uint32_t flags = 0u;
  std::uint32_t queryType = 0u;
  // HRESULT supplied by the actual dispatch; negative outcomes fail capture.
  std::int32_t dispatchResult = 0;
  std::uint32_t releaseAction = 0u;
  std::uint32_t releaseWriter = 0u;
  std::uint32_t prefixKind = 0u;
  std::uint32_t sourceIndex = kRenderTapeIdentityV3NoIndex;
  std::uint32_t initializerIndex = kRenderTapeIdentityV3NoIndex;
  // Immutable END cursor. Later commands in the same source do not extend it.
  std::uint32_t commandEnd = 0u;
  std::uint32_t observation = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
};

struct RenderTapeIdentityV3ReplaySettlement {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint64_t buildGeneration = 0u;
  std::uint64_t replayEdge = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t reserved0 = 0u;
};

static_assert(sizeof(RenderTapeIdentityV3Header) == 240u);
static_assert(offsetof(RenderTapeIdentityV3Header, version) ==
              offsetof(RenderTapeIdentityHeader, version));
static_assert(offsetof(RenderTapeIdentityV3Header, headerSize) ==
              offsetof(RenderTapeIdentityHeader, headerSize));
static_assert(sizeof(RenderTapeIdentityV3Source) == 96u);
static_assert(sizeof(RenderTapeIdentityV3Initializer) == 72u);
static_assert(sizeof(RenderTapeIdentityV3Command) == 48u);
static_assert(sizeof(RenderTapeIdentityV3Disposition) == 40u);
static_assert(sizeof(RenderTapeIdentityV3Coordinator) == 192u);
static_assert(sizeof(RenderTapeIdentityV3ReplaySettlement) == 40u);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV3Header>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV3Source>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV3Initializer>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV3Command>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV3Disposition>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV3Coordinator>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV3ReplaySettlement>);

enum class RenderTapeIdentityV3SourcePhase : std::uint8_t {
  Invalid,
  Published,
  Submitted,
  Completed,
};

constexpr RenderTapeIdentityV3SourcePhase renderTapeIdentityV3PublicationPhase(
    std::uint64_t sequence, std::uint64_t publication,
    std::uint64_t submission, std::uint64_t completion,
    std::uint64_t completed) noexcept {
  if (!sequence || !publication) return RenderTapeIdentityV3SourcePhase::Invalid;
  if (!submission)
    return !completion && !completed ? RenderTapeIdentityV3SourcePhase::Published
                                    : RenderTapeIdentityV3SourcePhase::Invalid;
  if (submission <= publication) return RenderTapeIdentityV3SourcePhase::Invalid;
  if (!completion)
    return !completed ? RenderTapeIdentityV3SourcePhase::Submitted
                      : RenderTapeIdentityV3SourcePhase::Invalid;
  return completion > submission && completed >= sequence
      ? RenderTapeIdentityV3SourcePhase::Completed
      : RenderTapeIdentityV3SourcePhase::Invalid;
}

constexpr RenderTapeIdentityV3SourcePhase renderTapeIdentityV3SourcePhase(
    const RenderTapeIdentityV3Source& source) noexcept {
  if (!source.sourceOrdinal || !source.seqId || !source.writerCaptureId ||
      !source.writerGeneration || !source.storageGeneration ||
      !source.publicationEdge || !source.commandCount || source.reserved0)
    return RenderTapeIdentityV3SourcePhase::Invalid;
  return renderTapeIdentityV3PublicationPhase(source.seqId, source.publicationEdge,
      source.submissionEdge, source.completionEdge, source.completedSequence);
}

constexpr RenderTapeIdentityV3SourcePhase renderTapeIdentityV3InitializerPhase(
    const RenderTapeIdentityV3Initializer& initializer) noexcept {
  if (!initializer.epoch || initializer.reserved0 || initializer.reserved1)
    return RenderTapeIdentityV3SourcePhase::Invalid;
  return renderTapeIdentityV3PublicationPhase(initializer.sequence,
      initializer.publicationEdge, initializer.submissionEdge,
      initializer.completionEdge, initializer.completedSequence);
}

// Shared finite-model/code transitions. Repeated acknowledgment rejects;
// replay settlement is deliberately not an input to this state machine.
constexpr bool renderTapeIdentityV3SubmitSource(
    RenderTapeIdentityV3Source& source, std::uint64_t edge) noexcept {
  if (renderTapeIdentityV3SourcePhase(source) !=
          RenderTapeIdentityV3SourcePhase::Published ||
      edge <= source.publicationEdge) return false;
  source.submissionEdge = edge;
  return true;
}

constexpr bool renderTapeIdentityV3CompleteSource(
    RenderTapeIdentityV3Source& source, std::uint64_t edge,
    std::uint64_t completedSequence) noexcept {
  if (renderTapeIdentityV3SourcePhase(source) !=
          RenderTapeIdentityV3SourcePhase::Submitted ||
      edge <= source.submissionEdge || completedSequence < source.seqId)
    return false;
  source.completionEdge = edge;
  source.completedSequence = completedSequence;
  return true;
}

constexpr bool renderTapeIdentityV3SubmitInitializer(
    RenderTapeIdentityV3Initializer& initializer, std::uint64_t edge) noexcept {
  if (renderTapeIdentityV3InitializerPhase(initializer) !=
          RenderTapeIdentityV3SourcePhase::Published ||
      edge <= initializer.publicationEdge) return false;
  initializer.submissionEdge = edge;
  return true;
}

constexpr bool renderTapeIdentityV3CompleteInitializer(
    RenderTapeIdentityV3Initializer& initializer, std::uint64_t edge,
    std::uint64_t completedSequence) noexcept {
  if (renderTapeIdentityV3InitializerPhase(initializer) !=
          RenderTapeIdentityV3SourcePhase::Submitted ||
      edge <= initializer.submissionEdge || completedSequence < initializer.sequence)
    return false;
  initializer.completionEdge = edge;
  initializer.completedSequence = completedSequence;
  return true;
}

// Uses the closed production record registry. This proves only the record's
// replay topology, not unrepresented action/resource semantics.
constexpr bool renderTapeIdentityV3DispositionAdmitted(
    std::uint32_t recordType,
    RenderTapeIdentityV3DispositionKind kind) noexcept {
  const auto* topology = recordTopology(recordType);
  if (!topology) return false;
  if (topology->topology == RecordReplayTopology::DirectStateOnly)
    return kind == RenderTapeIdentityV3DispositionKind::StateOnly;
  if (topology->topology == RecordReplayTopology::OrderedControl)
    return kind == RenderTapeIdentityV3DispositionKind::Coordinator;
  return topology->producesSlotCommand &&
      kind == RenderTapeIdentityV3DispositionKind::GpuCommand;
}

constexpr RenderTapeIdentityV3CommandKind renderTapeIdentityV3RecordCommandKind(
    std::uint32_t type) noexcept {
  const auto* rule = recordTopology(type);
  if (rule && rule->primitivePredicate)
    return RenderTapeIdentityV3CommandKind::DrawRun;
  switch (type) {
  case D9C_COMMAND_RECORD_CLEAR: return RenderTapeIdentityV3CommandKind::Clear;
  case D9C_COMMAND_RECORD_PRESENT: return RenderTapeIdentityV3CommandKind::Present;
  case D9C_COMMAND_RECORD_STRETCH_RECT: return RenderTapeIdentityV3CommandKind::StretchRect;
  case D9C_COMMAND_RECORD_COLOR_FILL: return RenderTapeIdentityV3CommandKind::ColorFill;
  case D9C_COMMAND_RECORD_UPDATE_SURFACE: return RenderTapeIdentityV3CommandKind::SurfaceCopy;
  case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE: return RenderTapeIdentityV3CommandKind::DepthResolve;
  case D9C_COMMAND_RECORD_GENERATE_MIPMAPS: return RenderTapeIdentityV3CommandKind::GenerateMipmaps;
  default: return RenderTapeIdentityV3CommandKind::Invalid;
  }
}

// Current ordered controls have a deliberately narrow generated-command
// alphabet. Query changes queue state; readback uses standalone synchronous
// command buffers, not physical source commands. UpdateTexture can emit one
// SurfaceCopy per subresource and an optional autogen-mipmap command.
constexpr bool renderTapeIdentityV3CoordinatorCommandAdmitted(
    std::uint32_t recordType, RenderTapeIdentityV3CommandKind kind) noexcept {
  return recordType == D9C_COMMAND_RECORD_UPDATE_TEXTURE &&
      (kind == RenderTapeIdentityV3CommandKind::SurfaceCopy ||
       kind == RenderTapeIdentityV3CommandKind::GenerateMipmaps);
}

static_assert([] {
  for (const auto& row : kRecordTopology)
    if (row.producesSlotCommand != (renderTapeIdentityV3RecordCommandKind(row.type) !=
            RenderTapeIdentityV3CommandKind::Invalid)) return false;
  return true;
}(), "v3 command mapping conserves the closed production topology");

struct RenderTapeIdentityV3CaptureKey {
  std::uint64_t token = 0u;
  std::uint64_t epoch = 0u;
  friend constexpr bool operator==(RenderTapeIdentityV3CaptureKey,
                                  RenderTapeIdentityV3CaptureKey) = default;
};

struct RenderTapeIdentityV3CurrentIssue {
  RenderTapeIdentityV3CaptureKey capture{};
  D9CWireObjectIdentity identity{};
  std::uint64_t cycle = 0u;
  std::uint64_t ordinal = 0u;
  std::uint64_t deviceEpoch = 0u;
  bool cancelled = false;
};

constexpr bool renderTapeIdentityV3SameIdentity(D9CWireObjectIdentity left,
                                              D9CWireObjectIdentity right) {
  return left.kind == right.kind && left.generation == right.generation &&
      left.objectId == right.objectId;
}

constexpr bool renderTapeIdentityV3CurrentIssueAdmitted(
    const RenderTapeIdentityV3Header& header,
    const RenderTapeIdentityV3Coordinator& coordinator,
    const RenderTapeIdentityV3CurrentIssue& current) noexcept {
  return current.capture.token == header.captureToken &&
      current.capture.epoch == header.captureEpoch && !current.cancelled &&
      coordinator.queryIdentity.kind == D9C_CHUNK_HANDLE_KIND_QUERY &&
      coordinator.queryIdentity.objectId && coordinator.queryIdentity.generation &&
      renderTapeIdentityV3SameIdentity(coordinator.queryIdentity, current.identity) &&
      coordinator.issueCycle && coordinator.issueCycle == current.cycle &&
      coordinator.issueOrdinal && coordinator.issueOrdinal == current.ordinal &&
      coordinator.deviceEpoch && coordinator.deviceEpoch == current.deviceEpoch &&
      coordinator.flags == 1u && coordinator.dispatchResult >= 0;
}

constexpr bool renderTapeIdentityV3BindPrefixAdmitted(
    const RenderTapeIdentityV3Coordinator& coordinator,
    const RenderTapeIdentityV3Source& source,
    query::WriterIdentity admitted, query::WriterIdentity published) noexcept {
  return coordinator.prefixKind == static_cast<std::uint32_t>(
             RenderTapeIdentityV3PrefixKind::Unbound) &&
      !coordinator.prefixSequence &&
      coordinator.sourceIndex == kRenderTapeIdentityV3NoIndex &&
      coordinator.writerCaptureId == source.writerCaptureId &&
      coordinator.writerGeneration == source.writerGeneration &&
      coordinator.storageGeneration == source.storageGeneration &&
      admitted.generation == coordinator.writerGeneration &&
      admitted.storageGeneration == coordinator.storageGeneration &&
      published.generation == source.writerGeneration &&
      published.storageGeneration == source.storageGeneration &&
      coordinator.commandEnd &&
      coordinator.commandEnd <= source.commandCount &&
      renderTapeIdentityV3SourcePhase(source) !=
          RenderTapeIdentityV3SourcePhase::Invalid &&
      query::canBindEnd(admitted, published, 0u, source.seqId);
}

bool renderTapeIdentityV3ObservationAdmitted(
    const RenderTapeIdentityV3Header& header,
    const RenderTapeIdentityV3Coordinator& coordinator,
    const RenderTapeIdentityV3CurrentIssue& current,
    std::span<const RenderTapeIdentityV3Source> sources,
    std::span<const RenderTapeIdentityV3Initializer> initializers,
    RenderTapeIdentityV3Observation observation) noexcept;

struct RenderTapeIdentityV3View {
  RenderTapeIdentityV3Header header{};
  std::vector<RenderTapeIdentityV3Source> sources{};
  std::vector<RenderTapeIdentityV3Initializer> initializers{};
  std::vector<RenderTapeIdentityV3Command> commands{};
  std::vector<RenderTapeIdentityV3Disposition> dispositions{};
  std::vector<RenderTapeIdentityV3Coordinator> coordinators{};
  std::vector<RenderTapeIdentityV3ReplaySettlement> settlements{};
};

// Owned canonical facts built only after production tape/chunk validation.
// Exposing the pure row validator also lets native finite models state their
// precise abstract alphabet without manufacturing a complete resource tape.
struct RenderTapeIdentityV3EventFact {
  std::uint64_t ordinal = 0u;
  std::uint32_t type = 0u;
  std::uint32_t recordCount = 0u;
};

struct RenderTapeIdentityV3RecordFact {
  std::uint64_t eventOrdinal = 0u;
  std::uint32_t recordIndex = 0u;
  std::uint32_t recordType = 0u;
  std::uint32_t requiredReleaseAction = 0u;
  std::uint32_t queryFlags = 0u;
  D9CWireObjectIdentity queryIdentity{};
  std::uint32_t queryType = 0u;
  std::uint64_t issueCycle = 0u;
  std::uint64_t issueOrdinal = 0u;
};

enum class RenderTapeIdentityV3Status : std::uint8_t {
  Valid,
  InvalidTape,
  InvalidHeader,
  InvalidLayout,
  EventsMismatch,
  InvalidSource,
  InvalidCut,
  RecordCoverageMismatch,
  InvalidDisposition,
  InvalidCoordinator,
  InvalidPrefix,
  InvalidSettlement,
  Incomplete,
  CapacityExceeded,
  AllocationFailed,
};

RenderTapeIdentityV3Status validateRenderTapeIdentityV3Rows(
    const RenderTapeIdentityV3View& identity,
    std::span<const RenderTapeIdentityV3EventFact> events,
    std::span<const RenderTapeIdentityV3RecordFact> records,
    bool requireComplete = true) noexcept;

RenderTapeIdentityV3Status validateRenderTapeIdentityV3(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> sidecar,
    RenderTapeIdentityV3View* out = nullptr,
    bool requireComplete = true) noexcept;

std::vector<std::byte> buildRenderTapeIdentityV3(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    const RenderTapeIdentityV3View& identity,
    RenderTapeIdentityV3Status* status = nullptr);

struct RenderTapeIdentityV3Limits {
  std::uint32_t maxSources = 4096u;
  std::uint32_t maxInitializers = 4096u;
  std::uint32_t maxCommands = 65536u;
  std::uint32_t maxDispositions = 65536u;
  std::uint32_t maxCoordinators = 4096u;
  std::uint32_t maxSettlements = 4096u;
};

// First-tranche cold, single-owner ledger. Observer integration must invoke
// publish/submit/complete only AFTER the corresponding successful real edge.
// No method requests a flush, dispatches a control, or retries replay.
// Borrowed writer ownership is converted into value-only internal identity;
// WriterIdentity never appears in serialized bytes.
class RenderTapeIdentityV3Ledger {
public:
  bool begin(RenderTapeIdentityV3Header header,
             RenderTapeIdentityV3Limits limits = {}) noexcept;
  bool publishSource(RenderTapeIdentityV3CaptureKey key,
      RenderTapeIdentityV3Source source, query::WriterIdentity owner,
      std::uint32_t* sourceIndex = nullptr) noexcept;
  bool publishInitializer(RenderTapeIdentityV3CaptureKey key,
      RenderTapeIdentityV3Initializer initializer,
      std::uint32_t* initializerIndex = nullptr) noexcept;
  bool appendCommand(RenderTapeIdentityV3CaptureKey key,
                     RenderTapeIdentityV3Command command) noexcept;
  bool appendDisposition(RenderTapeIdentityV3CaptureKey key,
      RenderTapeIdentityV3Disposition disposition,
      std::uint32_t canonicalRecordType) noexcept;
  bool dispatchCoordinator(RenderTapeIdentityV3CaptureKey key,
      RenderTapeIdentityV3Coordinator coordinator,
      query::WriterIdentity deferredOwner = {},
      std::uint32_t* coordinatorIndex = nullptr) noexcept;
  bool settleReplay(RenderTapeIdentityV3CaptureKey key,
      RenderTapeIdentityV3ReplaySettlement settlement) noexcept;
  bool submitSource(RenderTapeIdentityV3CaptureKey key,
                    std::uint32_t sourceIndex) noexcept;
  bool completeSource(RenderTapeIdentityV3CaptureKey key,
      std::uint32_t sourceIndex, std::uint64_t completedSequence) noexcept;
  bool submitInitializer(RenderTapeIdentityV3CaptureKey key,
                         std::uint32_t initializerIndex) noexcept;
  bool completeInitializer(RenderTapeIdentityV3CaptureKey key,
      std::uint32_t initializerIndex, std::uint64_t completedInitializer) noexcept;
  bool bindQueryPrefix(RenderTapeIdentityV3CaptureKey key,
      std::uint32_t coordinatorIndex, std::uint32_t sourceIndex) noexcept;
  bool observeQuery(RenderTapeIdentityV3CaptureKey key,
      std::uint32_t coordinatorIndex,
      const RenderTapeIdentityV3CurrentIssue& current,
      RenderTapeIdentityV3Observation observation) noexcept;
  bool snapshot(RenderTapeIdentityV3CaptureKey key,
                RenderTapeIdentityV3View& out) const noexcept;
  bool serialize(RenderTapeIdentityV3CaptureKey key,
      std::span<const std::byte> tape,
      const RenderTapeBlobCatalogue& catalogue,
      std::vector<std::byte>& out) noexcept;
  void fail(RenderTapeIdentityV3CaptureKey key) noexcept;
  bool failed() const noexcept { return failed_; }

private:
  struct WriterAssociation {
    query::WriterIdentity owner{};
    std::uint64_t captureId = 0u;
  };
  bool admit(RenderTapeIdentityV3CaptureKey key) noexcept;
  bool reject() noexcept { failed_ = true; return false; }
  std::uint64_t writerId(query::WriterIdentity owner) noexcept;
  std::uint64_t edge() noexcept;

  RenderTapeIdentityV3View identity_{};
  RenderTapeIdentityV3Limits limits_{};
  std::vector<WriterAssociation> writers_{};
  std::vector<query::WriterIdentity> sourceOwners_{};
  std::vector<query::WriterIdentity> queryOwners_{};
  std::uint64_t nextEdge_ = 0u;
  bool active_ = false;
  bool failed_ = false;
};

} // namespace dxmt9::d3d9
