#pragma once

#include "device_c_render_tape_identity_v3.hpp"
#include "device_c_render_tape_provenance_validation.hpp"

namespace dxmt9::d3d9 {

inline constexpr std::uint32_t kRenderTapeIdentityV4Version = 4u;
inline constexpr char kRenderTapeIdentityV4Schema[] = "dxmt9.render_tape.identity.v4";
inline constexpr std::uint32_t kRenderTapeIdentityV4NoIndex = UINT32_MAX;
inline constexpr std::uint32_t kRenderTapeIdentityV4MaxRows = kRenderTapeIdentityV3MaxRows;
using RenderTapeIdentityV4CaptureKey = RenderTapeIdentityV3CaptureKey;
using RenderTapeIdentityV4CoordinatorAuthority = TapeCoordinatorAuthority;
enum class RenderTapeIdentityV4Status : std::uint8_t {
  Valid, InvalidTape, InvalidHeader, InvalidLayout, EventsMismatch,
  InvalidSource, InvalidCut, RecordCoverageMismatch, InvalidDisposition,
  InvalidSettlement, Incomplete, CapacityExceeded, AllocationFailed,
  UnsupportedProvenance,
  InvalidCoordinator, InvalidPrefix,
  InvalidTransaction, InvalidReceipt,
};

// V4 retains lifecycle provenance, but publication says nothing about the
// selected Encode plan. Its required topology companion supplies that proof.
struct RenderTapeIdentityV4Header {
  std::uint64_t magic = kRenderTapeIdentityMagic;
  std::uint64_t eventsBytes = 0u;
  std::uint64_t frameId = 0u;
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t captureToken = 0u;
  RenderTapeDigest eventsDigest{};
  std::uint32_t version = kRenderTapeIdentityV4Version;
  std::uint32_t headerSize = sizeof(RenderTapeIdentityV4Header);
  std::uint64_t captureEpoch = 0u;
  std::uint64_t queue = 0u;
  std::uint64_t baselineSequence = 0u;
  std::uint64_t baselineInitializer = 0u;
  std::uint64_t baselineCompletedSequence = 0u;
  std::uint64_t initializerEpoch = 0u;
  std::uint64_t baselineCompletedInitializer = 0u;
  std::uint64_t baselineCompletionEdge = 0u;
  std::uint32_t baselineKind = 0u;
  std::uint32_t authority = static_cast<std::uint32_t>(RenderTapeIdentityAuthority::Capture);
  RenderTapeIdentityV3Table sources{}, initializers{}, commands{}, dispositions{},
      coordinators{}, settlements{}, attempts{};
  std::uint32_t requiredExecutionTopologyVersion = 1u;
  std::uint32_t reserved0 = 0u, reserved1 = 0u, reserved2 = 0u, reserved3 = 0u;
  std::uint32_t coordinatorDomain = static_cast<std::uint32_t>(
      RenderTapeIdentityV4CoordinatorAuthority::SessionRelease);
  std::uint32_t reserved4 = 0u;
};

// Explicit V4 admission sum. OrdinarySerial carries an actual queue-origin
// admission edge and zero release evidence; SessionRelease carries the real
// acknowledged receipt and no invented ordinary admission edge.
struct RenderTapeIdentityV4Coordinator {
  RenderTapeIdentityV3Coordinator provenance{};
  std::uint64_t admissionEdge = 0u;
  std::uint32_t authority = static_cast<std::uint32_t>(
      RenderTapeIdentityV4CoordinatorAuthority::SessionRelease);
  std::uint32_t reserved0 = 0u;
};

// Publication metadata authenticates a destination receipt independently of
// the shared render-source lifetime. Arena receipts anchor the first ticket of
// a batch; neither this row nor that receipt implies one transaction/source.
struct RenderTapeIdentityV4Source {
  RenderTapeIdentityV3Source lifecycle{};
  std::uint64_t rawOrdinal = 0u;
  std::uint64_t publicationBuildGeneration = 0u;
  std::uint32_t publicationControlIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t reserved0 = 0u;
};

// Actual raw replay-ledger acknowledgment, not a replay transaction commit.
struct RenderTapeIdentityV4RawSettlement {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint64_t replayEdge = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t reserved0 = 0u;
};

struct RenderTapeIdentityV4ReplaySourceIdentity {
  // Actual begin/receipt projection identity. Canonical tape events do not
  // encode the ingress producer interval, so this is a local receipt join,
  // not an independently authenticated producer-interval claim.
  std::uint64_t frame = 0u;
  std::uint64_t source = 0u;
  std::uint64_t lastSource = 0u;
  std::uint64_t sequence = 0u;
};

enum class RenderTapeIdentityV4ReceiptKind : std::uint32_t {
  None = 0u, Compatibility = 1u, DirectChunkSlot = 2u, Arena = 3u,
};

// Stable field-for-field production receipt evidence. Compatibility sequence
// is a raw projection identity, not GPU queue submission. commandCount is the
// transaction's staged emission count, never physical DrawRun cardinality.
struct RenderTapeIdentityV4ReplayReceipt {
  RenderTapeIdentityV4ReplaySourceIdentity identity{};
  std::uint64_t queueSequence = 0u;
  std::uint64_t buildGeneration = 0u;
  std::uint64_t sourceGeneration = 0u;
  std::uint64_t storageGeneration = 0u;
  std::uint32_t kind = 0u;
  std::uint32_t controlIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t commandCount = 0u;
  std::uint32_t reserved0 = 0u;
};

enum class RenderTapeIdentityV4AttemptOutcome : std::uint32_t {
  Committed = 1u, RolledBack = 2u, FailedStop = 3u,
};

// Allocated at a real begin, closed by that same actual transaction. The range
// is [begin.firstRecordOrdinal, terminal.nextRecordOrdinal); rollback may have
// observed zero records and contributes no committed coverage. effectEdge is
// the actual first irreversible-effect observation, or zero if none occurred.
struct RenderTapeIdentityV4ReplayAttempt {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint64_t attemptGeneration = 0u;
  std::uint64_t beginEdge = 0u;
  std::uint64_t terminalEdge = 0u;
  std::uint64_t effectEdge = 0u;
  RenderTapeIdentityV4ReplaySourceIdentity identity{};
  RenderTapeIdentityV4ReplayReceipt receipt{};
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t outcome = 0u;
  std::uint32_t stagedCommandCount = 0u;
};

// Neutral immutable physical extent. An omitted DrawRun is still materialized
// here; it is not relabelled state-only or an executed no-pass command.
struct RenderTapeIdentityV4PhysicalCommand {
  std::uint32_t sourceIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t physicalCommandIndex = 0u;
  std::uint32_t kind = 0u;
  std::uint32_t drawCount = 0u;
  std::uint32_t coordinatorIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t reserved0 = 0u;
};

struct RenderTapeIdentityV4View {
  RenderTapeIdentityV4Header header{};
  std::vector<RenderTapeIdentityV4Source> sources{};
  std::vector<RenderTapeIdentityV3Initializer> initializers{};
  std::vector<RenderTapeIdentityV4PhysicalCommand> commands{};
  std::vector<RenderTapeIdentityV3Disposition> dispositions{};
  std::vector<RenderTapeIdentityV4Coordinator> coordinators{};
  std::vector<RenderTapeIdentityV4RawSettlement> settlements{};
  std::vector<RenderTapeIdentityV4ReplayAttempt> attempts{};
};

static_assert(sizeof(RenderTapeIdentityV4Header) == 264u);
static_assert(sizeof(RenderTapeIdentityV4Coordinator) == 208u);
static_assert(offsetof(RenderTapeIdentityV4Coordinator, admissionEdge) == 192u);
static_assert(offsetof(RenderTapeIdentityV4Coordinator, authority) == 200u);
static_assert(offsetof(RenderTapeIdentityV4Header, coordinatorDomain) == 256u);
static_assert(sizeof(RenderTapeIdentityV4Source) == 120u);
static_assert(sizeof(RenderTapeIdentityV4RawSettlement) == 32u);
static_assert(sizeof(RenderTapeIdentityV4ReplaySourceIdentity) == 32u);
static_assert(sizeof(RenderTapeIdentityV4ReplayReceipt) == 80u);
static_assert(sizeof(RenderTapeIdentityV4ReplayAttempt) == 176u);
static_assert(sizeof(RenderTapeIdentityV4PhysicalCommand) == 24u);
static_assert(offsetof(RenderTapeIdentityV4Header, attempts) == 224u);
static_assert(offsetof(RenderTapeIdentityV4Header, reserved3) == 252u);
static_assert(offsetof(RenderTapeIdentityV4Source, rawOrdinal) == 96u);
static_assert(offsetof(RenderTapeIdentityV4Source, publicationControlIndex) == 112u);
static_assert(offsetof(RenderTapeIdentityV4RawSettlement, recordCount) == 24u);
static_assert(offsetof(RenderTapeIdentityV4ReplayReceipt, kind) == 64u);
static_assert(offsetof(RenderTapeIdentityV4ReplayAttempt, identity) == 48u);
static_assert(offsetof(RenderTapeIdentityV4ReplayAttempt, receipt) == 80u);
static_assert(offsetof(RenderTapeIdentityV4ReplayAttempt, firstRecord) == 160u);
static_assert(offsetof(RenderTapeIdentityV4Header, version) ==
              offsetof(RenderTapeIdentityV3Header, version));
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV4Header>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV4Source>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV4RawSettlement>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV4ReplayAttempt>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV4PhysicalCommand>);
static_assert(std::is_trivially_copyable_v<RenderTapeIdentityV4Coordinator>);
static_assert(std::is_standard_layout_v<RenderTapeIdentityV4Coordinator>);

// Valid means provenance only. It never qualifies execution without the
// required independently admitted execution_topology.v1 companion.
// Query/END provenance uses the shared canonical grammar. External poll
// readiness still requires the execution transcript/live-receipt join; a
// valid checkpoint-only poll does not invent a captured END coordinator.
RenderTapeIdentityV4Status validateRenderTapeIdentityV4Rows(
    const RenderTapeIdentityV4View& identity,
    std::span<const RenderTapeIdentityV3EventFact> events,
    std::span<const RenderTapeIdentityV3RecordFact> records,
    bool requireComplete = true) noexcept;
RenderTapeIdentityV4Status validateRenderTapeIdentityV4(
    std::span<const std::byte> tape, const RenderTapeBlobCatalogue& catalogue,
    std::span<const std::byte> sidecar, RenderTapeIdentityV4View* out = nullptr,
    bool requireComplete = true) noexcept;
std::vector<std::byte> buildRenderTapeIdentityV4(
    std::span<const std::byte> tape, const RenderTapeBlobCatalogue& catalogue,
    const RenderTapeIdentityV4View& identity,
    RenderTapeIdentityV4Status* status = nullptr);

} // namespace dxmt9::d3d9
