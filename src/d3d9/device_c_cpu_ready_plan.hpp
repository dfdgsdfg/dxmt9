#pragma once

#include "device_c_chunk_validate.hpp"
#include "device_c_replay_offload.hpp"
#include "../dxmt9/dxmt9_source_payload.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

namespace dxmt9::d3d9 {

struct CpuReadyPlanOptions {
  std::size_t pageSize = 4096;
  std::size_t maxOrdinaryPagesPerSegment = 64;
  std::size_t maxSegmentsPerSource =
      core::kMaxArenaSourcePayloadSegments;
  std::size_t maxPagesPerSource =
      std::numeric_limits<std::uint32_t>::max();
  // Compatibility alias used by the current single-block production caller.
  // The effective source bound is min(maxPages, maxPagesPerSource).
  std::size_t maxPages = std::numeric_limits<std::uint32_t>::max();
  // Compatibility default keeps the current one-logical-source contract.
  // Callers opting into source segmentation may group the bounded physical
  // segments into at most this many source plans.
  std::size_t maxSourcesPerChunk = 1;
};

// Future arena construction appends every UP byte range and synthesized
// binding payload independently with this alignment. Planning applies the
// same policy with checked padding, so drawPayloadBytes is a safe upper bound.
inline constexpr std::size_t kArenaDrawPayloadAppendAlignment =
    core::kSourcePayloadByteAlignment;

struct CpuReadySegmentPlan {
  // Exact semantic-replay range for this construction segment. Adjacent
  // segments partition the complete raw record stream without gaps: leading
  // and interstitial state belongs to the following GPU-producing record,
  // while trailing state belongs to the final segment.
  std::uint32_t firstRecordIndex = 0;
  std::uint32_t recordCount = 0;
  bool jumbo = false;
  core::SourcePayloadCapacity capacity{};
  core::SourcePayloadLayout layout{};
};

struct CpuReadySourcePlan {
  // A source plan borrows contiguous entries from CpuReadyPlan::segments;
  // it owns no storage or payload pointers. Record ranges remain raw-chunk
  // relative and therefore preserve state-only prefix/interstitial/tail
  // ownership exactly when a source boundary is introduced.
  std::uint32_t firstSegmentIndex = 0;
  std::uint32_t segmentCount = 0;
  std::uint32_t firstRecordIndex = 0;
  std::uint32_t recordCount = 0;
  bool jumbo = false;
  core::ArenaSourcePayloadLayout arenaLayout{};

  bool valid() const noexcept {
    return segmentCount != 0 && recordCount != 0 &&
           arenaLayout.segmentCount == segmentCount &&
           arenaLayout.valid();
  }
};

struct CpuReadyPlan {
  RawOrdinal rawOrdinal = 0;
  ReplayLane lane = ReplayLane::Legacy;
  ReplayReason reason = ReplayReason::InvalidImportedView;
  bool logicalSource = false;
  core::SourcePayloadCapacity capacity{};
  // Physical segments are bounded by the producer's raw-record cap, not by
  // the per-source Arena-block cap. Keep the vectors off the stack; planning
  // catches reserve failure and returns a pre-replay rejection.
  std::vector<CpuReadySegmentPlan> segments{};
  std::size_t segmentCount = 0;
  std::vector<CpuReadySourcePlan> sources{};
  std::size_t sourceCount = 0;
  std::optional<core::ArenaSourcePayloadLayout> arenaLayout{};
  // A carrier-free ordinary ChunkSlot can represent one raw source even when
  // the Arena planner must reject its bounded page grouping.  This layout is
  // computed only for the narrow non-UP Draw/APPLY_STATE/constants envelope;
  // it is not a queue/Tape reservation or a second source identity.
  core::SourcePayloadCapacity directSlotCapacity{};
  std::optional<core::SourcePayloadLayout> directSlotLayout{};
  bool directSlotSingleSource = false;
  // True when the complete raw stream contains at least one structurally
  // validated Query, Readback, or UpdateTexture. The plan intentionally does
  // not retain a variable-size disposition list: compatibility replay rebuilds
  // each allocation-free descriptor at its exact record index after this
  // whole-raw preflight has succeeded.
  bool containsOrderedControls = false;
  // Temporary compatibility surface for the existing production replay
  // connection. Multi-segment plans intentionally leave this empty; P3 moves
  // production routing to arenaLayout and the segment table.
  std::optional<core::SourcePayloadLayout> layout{};

  bool directArenaCandidate() const noexcept {
    return lane == ReplayLane::DirectArenaCandidate && logicalSource &&
           sourceCount != 0;
  }

  bool directChunkSlotCandidate() const noexcept {
    return directSlotSingleSource && directSlotLayout.has_value();
  }

  bool requiresAdmission() const noexcept { return directArenaCandidate(); }

  // Every non-rejected whole-raw disposition must replay D3D semantics once;
  // StateOnly is explicit because it performs replay without source admission.
  bool replaysSemanticsExactlyOnce() const noexcept {
    return lane != ReplayLane::Reject;
  }
};

enum class DirectChunkSlotReplayDisposition : std::uint8_t {
  Direct = 0,
  DirectOversized,
  DirectWithPresentTail,
  LegacyStateOnly,
  // Production-unreachable from the emission-plan router: it is produced only
  // for EmissionPlanReason::Coverage, and the coverage fold is enforced by
  // construction over a partition that is total for every structurally sound
  // view. It is enforced rather than assumed, so the value stays in the
  // alphabet as the typed report of a broken partition, and the legacy
  // whole-raw classifier above still produces it for a multi-segment plan.
  LegacySegmented,
  LegacyUpDraw,
  // Present is not appendable at its serial index (see
  // EmissionLeaseBlock::PresentOrdering): a non-trailing or duplicate Present
  // fails the whole raw closed to compatibility replay before any effect.
  LegacyPresent,
  LegacyUnsupported,
  LegacyOversized,
  LegacyCaptureOrTrace,
  InlineOrderedControl,
  RejectInvalid,
  Count,
};

// Cheap, allocation-free eligibility-only gate for the ordinary final
// ChunkSlot path. The complete wire validator runs before import; this gate
// intentionally accepts only non-UP Draw records and APPLY_STATE/constant
// records. All other valid record families remain compatibility/coordinator
// owned, while an impossible imported-view mismatch fails closed.
enum class DirectChunkSlotRangeClass : std::uint8_t {
  Eligible,
  Empty,
  Unsupported,
  Malformed,
};

struct DirectChunkSlotRangePlan {
  DirectChunkSlotRangeClass classification =
      DirectChunkSlotRangeClass::Malformed;
  core::SourcePayloadCapacity capacity{};
  std::size_t drawCount = 0;
  std::size_t plannedBytes = 0;

  bool eligible() const noexcept {
    return classification == DirectChunkSlotRangeClass::Eligible &&
           drawCount != 0 && plannedBytes != 0;
  }
};

// First pass over an already imported source. Complete validity is supplied by
// validateCommandChunk before import; this pass performs no layout
// construction, allocation, queue admission, state mutation, or resource
// marking and is therefore safe to use before any replay effect.
DirectChunkSlotRangeClass classifyDirectChunkSlotRange(
    const ImportedChunkView& imported) noexcept;

// Exact final-slot SoA count/reserve plan for a source accepted by the cheap
// classifier.  The returned bytes are the complete destination layout bound
// by TransactionalChunkSlotAssembler; no per-draw capacity boundary is
// introduced by this plan.
std::optional<DirectChunkSlotRangePlan> planDirectChunkSlotRange(
    const ImportedChunkView& imported, std::size_t pageSize) noexcept;

// LEGACY / TEST-ONLY as of R-BACK-2.102. This was the whole-raw promotion
// gate for the ordinary compatibility source; production routing now goes
// through planReplayEmission + emissionPlanDisposition, and no production call
// site remains. It is retained deliberately rather than deleted: it is the
// independent second opinion the emission plan is differentially checked
// against in `dxmt9-cpu-ready-plan-spec` and
// `dxmt9-cpu-ready-production-routing-spec`, and deleting it would delete that
// oracle along with ~30 pinned assertions. Retirement (or promotion of the
// oracle to an explicit test helper) is tracked in
// `specs/backend/encode-scheduling/gap.md`.
//
// It is a pure structural classifier: no queue state, D3D shadow, resource
// mark, or destination storage is touched here.
DirectChunkSlotReplayDisposition classifyDirectChunkSlotReplay(
    const ImportedChunkView& imported, const CpuReadyPlan& plan,
    bool captureOrTrace) noexcept;

// ---------------------------------------------------------------------------
// Source-wide replay emission plan (R-BACK-2.101, R-BACK-2.102).
//
// The whole-raw gate above answers one question: may this *entire* raw
// construct directly? One coordinator command anywhere demotes every draw in
// the raw to compatibility replay, and the answer carries no description of
// what the raw actually contained.
//
// The emission plan replaces that single verdict with a *total* ordered
// partition of the raw into typed segments. Every live record kind lands in
// exactly one segment: maximal direct draw/state islands, single-record
// coordinator locators, single-record ordered-control locators, and maximal
// compatibility ranges. The partition is always computed for a structurally
// sound view, whatever the raw contains, so the taxonomy is exhaustive and
// observable even for shapes no direct lease may own.
//
// Whether one lease may own the raw is a *separate*, typed question
// (`EmissionLeaseBlock`). Segment kinds describe; the lease block decides.
//
// The plan is pure over the imported view: no allocation beyond one caught
// segment reserve, no handle resolution, no queue state, no D3D shadow, no
// resource mark. Nothing downstream may read it unless `coverageExact`.
enum class EmissionSegmentKind : std::uint8_t {
  // Maximal run of island-resident records containing at least one
  // island-eligible draw. Trailing state stays in the island, matching the
  // CpuReadySegmentPlan convention.
  DirectIsland = 0,
  // Island-resident run with no draw. It carries no capacity and must never
  // open a zero-draw island: beginDirectChunkSlotReplay rejects a plan whose
  // capacity.commandHeaders is zero.
  StateOnlyRun,
  // Exactly one coordinator record at its exact serial index.
  CoordinatorLocator,
  // Exactly one Query/Readback/UpdateTexture at its exact serial index.
  OrderedControlLocator,
  // Maximal run of records the direct appender cannot own: UP draws and
  // TriangleFan draws, plus any zero-draw state run that leads into one
  // (leading state belongs to the following GPU-producing record).
  CompatibilityRange,
  Count,
};

// Why plan construction itself succeeded or failed. These are structural
// properties of the view, not policy: none of them can be partitioned around.
enum class EmissionPlanReason : std::uint8_t {
  // A total, exactly-covering partition was produced.
  Complete = 0,
  // The imported view disagrees with its own record table.
  MalformedView,
  // A record type outside the closed 21-kind alphabet.
  UnknownRecord,
  // A checked count/byte accumulation overflowed.
  Overflow,
  // The segment fold did not partition the record range exactly.
  Coverage,
  // Exact layout construction failed for the aggregate capacity.
  Capacity,
  // Segment storage reservation threw.
  Storage,
  Count,
};

// Why a single final-slot lease may not own this raw. Mutually exclusive and
// reported in this precedence order, so a fallback is never "unclassified".
enum class EmissionLeaseBlock : std::uint8_t {
  // One lease may own every segment of this raw.
  None = 0,
  // Plan construction did not complete; there is nothing to own.
  IncompletePlan,
  // A lease span would have had to own a Present that is not its trailing
  // coordinator, or a second Present. Present is not appended at its serial
  // index: `appendActiveDirectChunkSlotPresent` parks it in the build context
  // and `commitDirectChunkSlotReplay` appends it once, last, as the slot's
  // publication boundary. So anything a lease appends after a Present would
  // execute BEFORE it, and a second Present in one transaction is refused
  // outright. Only one Present, as the span's final segment, is expressible;
  // every other Present shape fails the whole raw closed to compatibility
  // replay before any effect. Highest precedence, because it is the one block
  // that exists to prevent a wrong-order emission rather than an unsupported
  // one.
  PresentOrdering,
  // An ordered control requires a CPU-ready session release at its exact
  // position. An open direct transaction holds the writing slot with an
  // unprepared assembler, so no release can happen across it without
  // publishing a half-built private prefix.
  OrderedControl,
  // A UP or TriangleFan draw replays through the compatibility sink, which
  // bypasses the direct appender entirely.
  CompatibilityRange,
  // No island-eligible draw anywhere; there is nothing to construct directly.
  NoIsland,
  Count,
};

struct EmissionSegment {
  EmissionSegmentKind kind = EmissionSegmentKind::StateOnlyRun;
  std::uint32_t firstRecordIndex = 0;
  std::uint32_t recordCount = 0;
  // DirectIsland only. A DirectIsland always has drawCount != 0.
  std::uint32_t drawCount = 0;
  // Coordinator/OrderedControl locators only: the live wire record type at
  // firstRecordIndex.
  std::uint32_t locatorRecordType = 0;
  // Exact per-segment SoA dimensions for DirectIsland and CoordinatorLocator,
  // derived draw dimensions included. The aggregate is NOT the sum of these:
  // see planReplayEmission.
  core::SourcePayloadCapacity capacity{};
};

static_assert(std::is_trivially_copyable_v<EmissionSegment>);
static_assert(std::is_standard_layout_v<EmissionSegment>);

// What terminates a lease span. Ordered controls and compatibility ranges are
// the only cuts; a coordinator locator stays inside the span that owns it.
enum class EmissionSpanCut : std::uint8_t {
  // The span runs to the end of the raw.
  EndOfRaw = 0,
  // A Query/Readback/UpdateTexture follows. The span's lease must commit and
  // settle before the control executes, because the control needs a CPU-ready
  // session release at its exact position and no open direct transaction may
  // straddle that release (R-BACK-2.85).
  OrderedControl,
  // A UP or TriangleFan draw follows. It replays through the compatibility
  // sink, which bypasses the direct appender entirely.
  CompatibilityRange,
  Count,
};

// One executable unit of the partition: a maximal run of DirectIsland,
// StateOnlyRun and CoordinatorLocator segments between cuts. A span holding at
// least one island-eligible draw owns exactly one final-slot lease; a span
// with no draw carries no lease and replays through the ordinary sink, which
// is what it already did before spans existed.
struct EmissionLeaseSpan {
  std::uint32_t firstRecordIndex = 0;
  std::uint32_t recordCount = 0;
  std::uint32_t firstSegmentIndex = 0;
  std::uint32_t segmentCount = 0;
  // 0-based index among this raw's *lease-owning* spans, and the value the
  // queue presents to the CpuReadyTape span witness. Ordinary spans do not
  // consume an ordinal, so lease ordinals stay densely adjacent and a skipped
  // ordinal is a real producer-ordering fault rather than a numbering
  // artifact. Meaningful only when `ownsLease` is true.
  std::uint32_t leaseOrdinal = 0;
  std::uint32_t drawCount = 0;
  std::uint32_t islandCount = 0;
  std::uint32_t coordinatorCount = 0;
  // Present locators inside this span. A lease may express at most one, and
  // only as the span's final segment; see EmissionLeaseBlock::PresentOrdering.
  std::uint32_t presentCount = 0;
  EmissionSpanCut trailingCut = EmissionSpanCut::EndOfRaw;
  // True when this span owns a final-slot lease.
  bool ownsLease = false;
  // Exactly one Present, and it is this span's last segment -- the only
  // Present shape a direct lease can emit in source order.
  bool presentTrailingCoordinator = false;
  // The last lease-owning span of the raw; the queue settles the witness on
  // its commit so nothing may extend the raw afterwards.
  bool finalLeaseSpan = false;
  bool containsTerminalPresent = false;
  // Exact final-slot reservation for this span alone. Derived dimensions are
  // computed once from this span's total draw count, never summed per island.
  core::SourcePayloadCapacity capacity{};
  std::size_t plannedBytes = 0;

  std::uint32_t endRecordIndex() const noexcept {
    return firstRecordIndex + recordCount;
  }
};

static_assert(std::is_trivially_copyable_v<EmissionLeaseSpan>);
static_assert(std::is_standard_layout_v<EmissionLeaseSpan>);

struct ReplayEmissionPlan {
  RawOrdinal rawOrdinal = 0;
  EmissionPlanReason reason = EmissionPlanReason::MalformedView;
  EmissionLeaseBlock leaseBlock = EmissionLeaseBlock::IncompletePlan;
  // Segments partition [0, imported.records.size()) with no gap, overlap,
  // duplicate, or empty entry, for every structurally sound view. Kept off
  // the stack; a reserve failure is a pre-replay rejection.
  std::vector<EmissionSegment> segments{};
  // Executable partition derived from `segments`. Spans partition
  // [0, recordCount) exactly, in order, for every partitioned plan --
  // including the ones no single lease may own.
  std::vector<EmissionLeaseSpan> leaseSpans{};
  std::size_t leaseSpanCount = 0;
  std::size_t islandCount = 0;
  std::size_t stateOnlyRunCount = 0;
  std::size_t coordinatorCount = 0;
  std::size_t orderedControlCount = 0;
  std::size_t compatibilityRangeCount = 0;
  std::size_t compatibilityRecordCount = 0;
  std::size_t directDrawCount = 0;
  std::size_t directIslandRecordCount = 0;
  // Final-slot command headers a lease would append: one per island draw plus
  // one per coordinator locator. Zero unless leaseBlock == None.
  std::size_t slotCommandCount = 0;
  // Reserved once for the whole raw, and populated only when the raw is
  // lease-eligible. Non-additive derived dimensions (constant bytes, uniform
  // lookup buckets) are computed once from the total draw count, never summed
  // per island, because the bucket count is a non-linear function of the draw
  // count and per-island sums under-reserve.
  core::SourcePayloadCapacity aggregateCapacity{};
  std::size_t aggregatePlannedBytes = 0;
  std::size_t presentCount = 0;
  bool containsPresent = false;
  bool coverageExact = false;

  // The partition is sound and may be read for description/observability.
  bool partitioned() const noexcept {
    return reason == EmissionPlanReason::Complete && coverageExact;
  }
  // One final-slot transaction may own every segment of this raw.
  bool leaseEligible() const noexcept {
    return partitioned() && leaseBlock == EmissionLeaseBlock::None &&
           islandCount != 0 && directDrawCount != 0 &&
           aggregatePlannedBytes != 0 && aggregateCapacity.commandHeaders != 0;
  }
  // True when the raw is exactly today's whole-raw direct envelope, i.e. the
  // island architecture selected the same shape the narrow gate would have.
  bool singleIslandNoCoordinator() const noexcept {
    return leaseEligible() && islandCount == 1 && coordinatorCount == 0;
  }
  // The derived span list may be executed by the production span driver.
  // A PresentOrdering block is deliberately NOT executable as spans: the raw
  // must fall back whole, pre-effect, because the wrong-order emission it
  // describes cannot be repaired by routing around one span.
  bool spansExecutable() const noexcept {
    return partitioned() && leaseBlock != EmissionLeaseBlock::PresentOrdering;
  }
  // Number of spans that own a final-slot lease. Zero means the raw has no
  // direct span at all and is entirely ordinary-owned.
  std::size_t leaseOwningSpanCount() const noexcept {
    std::size_t count = 0;
    for (const auto& span : leaseSpans) {
      if (span.ownsLease) ++count;
    }
    return count;
  }
};

// Pure, allocation-bounded, pre-effect. Complete wire validity is supplied by
// validateCommandChunk before import.
ReplayEmissionPlan planReplayEmission(
    const ImportedChunkView& imported, RawOrdinal rawOrdinal,
    std::size_t pageSize) noexcept;

// One typed whole-raw disposition per classified raw, derived from the plan.
// This closes the production-dead classifyDirectChunkSlotReplay gap: a single
// producer maps a real plan onto the existing perf disposition alphabet.
DirectChunkSlotReplayDisposition emissionPlanDisposition(
    const ReplayEmissionPlan& plan, bool captureOrTrace) noexcept;

// Scans one already-validated ImportedChunkView structurally. It does not
// resolve handles, mutate D3D state, invoke semantic replay, or reserve Tape
// storage. The returned lane covers the whole raw chunk.
CpuReadyPlan planCpuReadyChunk(
    const ImportedChunkView& imported,
    RawOrdinal rawOrdinal,
    CpuReadyPlanOptions options = {}) noexcept;

}  // namespace dxmt9::d3d9
