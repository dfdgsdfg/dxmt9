#pragma once

// Frame Graph optimizer pipeline (Tasks B3-B8, L1).
//
// Spec: specs/d3d9-renderer/spec.md §5 (Optimizer Pipeline) and
//   requirements.md R-BACK-32.5 (fixed pass order), R-BACK-33 (memoryless),
//   R-BACK-34 (passcoalesce).
//
// Fixed pipeline order (R-BACK-32.5, spec.md §5):
//   lifetime -> passcoalesce -> memoryless -> dce -> reorder -> loadstore
//
// The ordering is load-bearing:
//   - memoryless eligibility (§5.3) needs the single-pass collapse that only
//     exists after passcoalesce mutates the pass graph,
//   - DCE (§5.1) consults memoryless eligibility as a cross-chunk safety gate,
//     so it runs after memoryless,
//   - load/store action selection (§5.5) must run AFTER reorder because reorder
//     can change which pass is the first/last access of an attachment.
//
// ALWAYS-ON vs FEATURE-GATED.
//   lifetime  — always run (input to memoryless/reorder).
//   loadstore — always run (actions must be correct).
//   passcoalesce / memoryless / dce / reorder — feature-gated via OptimizerOptions.
//   dce defaults OFF even when the struct is otherwise default-constructed.
//
// PARITY BASELINE (R-BACK-32.6).
//   With a default-constructed OptimizerOptions (all gated passes off),
//   runOptimizer runs ONLY lifetime + loadstore and MUST NOT change pass order
//   or draw order — so the linearizer (B9) reproduces the original command
//   order, yielding the byte-identical parity baseline.
//
// DETERMINISM (R-BACK-32.2).
//   Pure value transforms over FrameGraph. No clock, thread-id, or RNG. The
//   only device-touching boundary is the memoryless alias acquisition
//   (TransientAttachmentPool::acquire), which is declared here but kept OUT of
//   the unit-tested classifier path — see fg_optimizer/memoryless.cpp.

#include "fg_dag.hpp"
#include "../dxmt9_encode_attribution.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace dxmt9::framegraph {

// Default prior-frame observation window for memoryless promotion
// (DXMT9_RENDERER_MEMORYLESS_OBSERVATION_FRAMES, R-BACK-33.2 default 8). Kept as
// a field on OptimizerOptions so the passes stay env-free / deterministic; the
// backend resolves the env once and stores it here.
inline constexpr u32 kDefaultMemorylessObservationFrames = 8;

// Which optimizer passes are enabled. lifetime + loadstore are unconditional and
// have no toggle here (they must always run for correctness).
struct OptimizerOptions {
  bool passcoalesce = false;
  bool memoryless = false;
  bool dce = false;       // §5.1 — default OFF (chunk-conservative).
  bool reorder = false;
  // Diagnostic-only source-local return classification. Production enables
  // this only while perf counters are active; it never participates in an
  // optimizer decision.
  bool collect_passcoalesce_return_diagnostics = false;

  // Memoryless observation threshold (R-BACK-33.2). A surface must be observed
  // eligible for at least this many prior frames before promotion.
  u32 memoryless_observation_frames = kDefaultMemorylessObservationFrames;
};

// Aggregate counters produced by the optimizer. The real perf::count* counter
// wiring (framegraph_dce_dropped, framegraph_pass_coalesced_count,
// framegraph_virtual_attachment_*) lands in Task B11; for L1 the passes expose
// their decisions through this struct so the unit tests can assert mechanism
// without the counter system.
struct OptimizerStats {
  u32 dce_dropped = 0;             // §5.1 framegraph_dce_dropped
  u32 dce_preserved_unprovable = 0;// §5.1 framegraph_dce_preserved_unprovable
  u32 pass_coalesced_count = 0;    // §5.4 framegraph_pass_coalesced_count
  u32 pass_coalesce_reorder_distance_max = 0; // §5.4 _reorder_distance_max
  u32 pass_coalesce_blocked_cycle = 0;
  u32 pass_coalesce_second_non_draw = 0;
  // Source-owned matching-render-pass return diagnostics. Every non-adjacent
  // matching pair considered by passcoalesce, excluding the separately
  // attributed virtual active-render seed, contributes one terminal bucket:
  // merged, blocked-cycle, second-non-draw, non-render-intervener, or missing-
  // invariant. The remaining fields are orthogonal mechanism counts for
  // classified interveners; dependency-kept is a command-bearing Move::Before
  // producer, semantic means a query/lock/marker pass flag, and non-draw means
  // a command kind other than DrawRun. They never participate in planning.
  u64 pass_coalesce_return_candidates = 0;
  u64 pass_coalesce_return_merged = 0;
  u64 pass_coalesce_return_blocked_cycle = 0;
  u64 pass_coalesce_return_second_non_draw = 0;
  u64 pass_coalesce_return_non_render_intervener = 0;
  u64 pass_coalesce_return_missing_invariant = 0;
  u64 pass_coalesce_return_dependency_kept = 0;
  u64 pass_coalesce_return_move_before = 0;
  u64 pass_coalesce_return_move_after = 0;
  u64 pass_coalesce_return_non_draw_intervener = 0;
  u64 pass_coalesce_return_semantic_intervener = 0;
  u64 pass_coalesce_return_commandless_intervener = 0;
  u64 pass_coalesce_return_commandless = 0;
  // Successful fixpoint merges whose left pass carries the virtual active-
  // render seed. Distance is b-a before each mutation; intervening counts are
  // pass counts partitioned by command-bearing Move and commandless passes.
  u32 pass_coalesce_active_seed_merge_count = 0;
  u64 pass_coalesce_active_seed_merge_distance_total = 0;
  u32 pass_coalesce_active_seed_merge_distance_max = 0;
  u64 pass_coalesce_active_seed_command_before = 0;
  u64 pass_coalesce_active_seed_command_after = 0;
  u64 pass_coalesce_active_seed_empty_intervening = 0;
  u32 memoryless_promoted = 0;     // §5.3 framegraph_virtual_attachment_emitted
  u32 memoryless_blocked_observation = 0; // _promotion_blocked_observation
  u32 memoryless_dropped_via_lock = 0;    // _dropped_via_lock
  u32 memoryless_dropped_via_readback = 0;// _dropped_via_readback
};

// Caller-sized, allocation-free sink consumed only at the exact successful
// active-seed merge point. The multi-source planner sizes storage to the
// initial non-seed pass count before optimizer mutation.
struct ActiveSeedMergeWitnessSink {
  std::span<encoders::ActiveSeedMergeCommandWitness> storage{};
  std::size_t count = 0;
  std::size_t attempted = 0;
  bool overflow = false;

  void record(std::uint32_t flattenedCommandIndex,
              std::uint32_t mergeDistance) noexcept {
    const std::size_t mergeOrdinal = attempted++;
    if (count >= storage.size() ||
        mergeOrdinal > std::numeric_limits<std::uint32_t>::max()) {
      overflow = true;
      return;
    }
    storage[count++] = encoders::ActiveSeedMergeCommandWitness{
        .flattenedCommandIndex = flattenedCommandIndex,
        .mergeOrdinal = static_cast<std::uint32_t>(mergeOrdinal),
        .mergeDistance = mergeDistance,
    };
  }

  void rejectUnrepresentable() noexcept {
    ++attempted;
    overflow = true;
  }

  std::span<const encoders::ActiveSeedMergeCommandWitness>
  publishable() const noexcept {
    return overflow
        ? std::span<const encoders::ActiveSeedMergeCommandWitness>{}
        : std::span<const encoders::ActiveSeedMergeCommandWitness>(
              storage.data(), count);
  }
};

// Bounded cross-chunk DCE evidence. The current chunk still owns an independent
// FrameGraph and no command crosses the chunk boundary; the span only names
// resources whose first access in an already-selected next chunk is a full
// overwrite. The proof is call-local and must not be retained.
struct DceLookaheadProof {
  std::span<const ResourceHandle> fully_overwritten_before_read{};

  bool contains(ResourceHandle handle) const noexcept;
};

// Per-surface state that must survive across frames (the prior-frame observation
// gate, R-BACK-33.2). The FrameGraph itself is rebuilt every frame and cannot
// carry this; the backend owns a small map keyed by ResourceHandle and hands the
// relevant entry to the memoryless classifier. Declared here so the memoryless
// classifier signature is testable device-free.
struct MemorylessObservation {
  ResourceHandle handle{};
  // Consecutive prior frames the surface satisfied every R-BACK-33.2 gate. The
  // backend increments this once per frame the surface is observed eligible and
  // resets it to zero on any failing frame or on a misclassification incident.
  u32 observation_frames = 0;
  // Sticky "this surface is a swap-chain backbuffer" — never promotable (§5.3
  // step 3). The backend sets this when the surface is bound as the backbuffer.
  bool bound_as_backbuffer = false;
};

// ---------------------------------------------------------------------------
// TransientAttachmentPool (spec.md §5.3) — DEVICE-GATED.
//
// The pool hands out MTLStorageModeMemoryless alias textures. acquire() touches
// the Metal device and therefore CANNOT run in the native (device-free) test
// host. The unit-tested deliverable of the memoryless pass is the CLASSIFIER +
// residency decision (markMemorylessCandidates below); the actual alias
// acquisition and PassNode.targets rewrite happen behind this interface in the
// linearizer / backend, not in the pure pass.
//
// Declared here as the §5.3 interface contract; the implementation that calls
// the winemetal bridge lives outside the unit-tested path.
// ---------------------------------------------------------------------------
struct TransientAttachmentKey {
  // L1 keeps this minimal: format/size/usage are carried as opaque integers so
  // this header stays free of Metal/winemetal types. The backend fills them from
  // the persistent surface's descriptor.
  u32 format = 0;
  u32 width = 0;
  u32 height = 0;
  u32 sample_count = 1;
  u32 usage = 0;

  friend bool operator==(const TransientAttachmentKey&,
                         const TransientAttachmentKey&) = default;
};

// Forward-declared device-side pool. Defined in the device/backend layer; the
// pure passes never construct or call it. Kept as an incomplete type here so the
// header has no Metal dependency.
class TransientAttachmentPool;

// ---------------------------------------------------------------------------
// Orchestrator.
// ---------------------------------------------------------------------------

// Run the enabled optimizer passes in the fixed R-BACK-32.5 order on `graph`,
// in place. `observations` carries cross-frame per-surface state for the
// memoryless classifier (may be empty / nullptr-equivalent when memoryless is
// off). `stats` (optional) receives mechanism counters.
void runOptimizer(FrameGraph& graph, const OptimizerOptions& options,
                  std::vector<MemorylessObservation>* observations = nullptr,
                  OptimizerStats* stats = nullptr,
                  DceLookaheadProof dce_lookahead = {},
                  ActiveSeedMergeWitnessSink* activeSeedWitnesses = nullptr);

// ---------------------------------------------------------------------------
// Per-pass entry points (declared for direct unit testing).
// ---------------------------------------------------------------------------

// §5.2 — compute first_use_pass / last_use_pass for every ResourceNode by
// scanning its accesses. ALWAYS run. A resource is transient when
// first_use_pass == last_use_pass; callers can derive that via
// resourceIsTransient() below (no transient bit is added to the B1 ResourceNode).
void runLifetime(FrameGraph& graph);

// Derived transient predicate (§5.2). first==last means the resource is touched
// in a single pass and is therefore a memoryless candidate by lifetime alone.
inline bool resourceIsTransient(const ResourceNode& node) noexcept {
  return node.first_use_pass == node.last_use_pass;
}

// §5.4 / R-BACK-34 — coalesce matching-AttachmentSet pass pairs when every
// intervening pass can move before P_a or after P_b without breaking edges.
// Conservative: when unsure, do not merge. Feature-gated. Preserves draw order.
void runPassCoalesce(FrameGraph& graph, OptimizerStats* stats,
                     bool collectReturnDiagnostics = false,
                     ActiveSeedMergeWitnessSink* activeSeedWitnesses =
                         nullptr);

// §5.3 / R-BACK-33 — classifier + residency decision. Marks eligible
// ResourceNodes ResidencyClass::Memoryless. Device-free: it does NOT acquire an
// alias and does NOT mutate the persistent surface; the alias acquisition is the
// device-gated TransientAttachmentPool boundary. Feature-gated.
void markMemorylessCandidates(FrameGraph& graph,
                              const std::vector<MemorylessObservation>& observations,
                              u32 observation_threshold, OptimizerStats* stats);

// §5.1 — dead-pass elimination. Default OFF. Marks PassNode.flags.dead under all
// conservative gates; dead passes stay in the array but are excluded from
// linearization. Feature-gated.
void runDce(FrameGraph& graph, OptimizerStats* stats,
            DceLookaheadProof lookahead = {});

// Build the conservative proof set for a following, already-selected source.
// A resource is included only when its first chronological access is a full
// Clear and the graph carries no lock/readback observation for that resource.
// Rectangular and aspect-incomplete depth/stencil clears are represented as
// ReadWrite by fg_builder and therefore do not qualify.
std::vector<ResourceHandle> collectDceLookaheadFullOverwrites(
    const FrameGraph& lookahead);

// Plan the optimized command prefix that can be encoded before the next
// chunk's overwrite proof is known. `prior_lookahead` is only a scheduling
// hint learned from an earlier successor: it chooses a ready-FIFO sample point
// but can never authorize a drop. The returned commands precede the first pass
// that would become dead with the hint but stays live without it. An empty
// result means no useful prefix exists.
std::vector<u32> planDceLookaheadReplayPrefix(
    const FrameGraph& graph, std::size_t command_count,
    const OptimizerOptions& options, DceLookaheadProof prior_lookahead);

// §5.6 — dependency-respecting topological reorder with an integer
// state-change-cost tiebreaker. Preserves every edge. Feature-gated.
void runReorder(FrameGraph& graph);

// §5.5 — load/store action selection per PassNode. ALWAYS run, AFTER reorder.
void runLoadStore(FrameGraph& graph);

}  // namespace dxmt9::framegraph
