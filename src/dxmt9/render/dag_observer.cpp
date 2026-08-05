#include "dag_observer.hpp"

#include "../dxmt9_backend_types.hpp"  // core::ChunkSlot
#include "../dxmt9_format_convert.hpp"
#include "../dxmt9_perf_counters.hpp"
#include "../dxmt9_resource_pool.hpp"
#include "../framegraph/fg_builder.hpp"
#include "../framegraph/fg_debug_export.hpp"
#include "../framegraph/fg_optimizer.hpp"

#include <fstream>
#include <ios>
#include <string>

namespace dxmt9::render {

namespace {

framegraph::ResourceHandle resolveResourceAlias(
    const void* context, framegraph::ResourceHandle handle) noexcept {
  const auto* pool = static_cast<const resources::Pool*>(context);
  if (!pool) {
    return handle;
  }
  const auto* surface = pool->findSurface(handle.value);
  return surface && surface->aliasTexture
             ? framegraph::ResourceHandle{surface->aliasTexture.value}
             : handle;
}

bool depthStencilClearCoversResource(
    const void* context, framegraph::ResourceHandle handle,
    bool clearDepth, bool clearStencil) noexcept {
  const auto* pool = static_cast<const resources::Pool*>(context);
  const auto* surface = pool ? pool->findSurface(handle.value) : nullptr;
  if (!surface) {
    return false;
  }
  const bool hasDepth = convert::formatHasDepthAspect(surface->desc.format);
  const bool hasStencil =
      convert::formatHasStencilAspect(surface->desc.format);
  if (!hasDepth && !hasStencil) {
    return false;
  }
  return (!hasDepth || clearDepth) && (!hasStencil || clearStencil);
}

// Write `<dir>/dag-frame<frameId>-chunk<seqId>-<stage>.json` from a serialized
// snapshot. Matches framegraph::writeDagDump's file naming (Json format) but
// bypasses the static DXMT9_RENDERER_DUMP_DAG cache so the caller-supplied dir
// is honored. Never throws; an unwritable path is silently skipped.
void writeDagJsonToDir(const framegraph::FrameGraph& fg, std::uint64_t frameId,
                       std::uint64_t seqId, const char* stage,
                       const std::string& dir,
                       core::SourcePayloadView payload) {
  std::string path = dir;
  if (!path.empty() && path.back() != '/') {
    path.push_back('/');
  }
  path += "dag-frame";
  path += std::to_string(frameId);
  path += "-chunk";
  path += std::to_string(seqId);
  path += "-";
  path += (stage != nullptr ? stage : "");
  path += ".json";

  // `payload` enables the DEBUG-ONLY per-draw JSON detail for either source
  // representation.
  const std::string contents =
      framegraph::serializeDagJson(fg, seqId, stage, payload);
  std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
  if (out.is_open()) {
    out << contents;  // best-effort: never fail a render on an unwritable dir.
  }
}

// True if `frame` falls inside the inclusive ±radius window centered on
// `target` (R-BACK-39.7). The low end is clamped at 1 because inter-present
// frames are 1-based, so a wide radius can never select frame 0. radius==0
// degenerates to the single-frame test `frame == target`.
bool frameInDumpWindow(std::uint64_t frame, std::uint64_t target,
                       std::uint64_t radius) {
  const std::uint64_t low = (target > radius) ? (target - radius) : 1u;
  const std::uint64_t high = target + radius;
  return frame >= low && frame <= high;
}

// R-BACK-39.2 (Task B11, L1) — record the small frame-graph observe-path
// perf counters. `passesBuilt` is sampled BEFORE runOptimizer (coalesce/dce
// can shrink fg.passes), the rest come from the OptimizerStats runOptimizer
// filled. perf::count* helpers are process-global atomics that no-op unless
// DXMT_PERF_COUNTERS is enabled, so this stays observation-only and keeps the
// Metal command stream byte-identical (R-BACK-40.5). The deferred L2/on-device
// counters (framegraph_icb_*, virtual-attachment misclassification) have no L1
// callsite and are intentionally not recorded here.
void recordFramegraphObserveCounters(std::uint64_t passesBuilt,
                                     const framegraph::OptimizerStats& stats) {
  perf::countFramegraphPassesBuilt(passesBuilt);
  perf::countFramegraphPassesCoalesced(stats.pass_coalesced_count);
  perf::countFramegraphPassesDead(stats.dce_dropped);
  perf::countFramegraphResourcesMemoryless(stats.memoryless_promoted);
  perf::countFramegraphDagDumpWritten();
}

}  // namespace

framegraph::ResourceAliasResolver makeResourceAliasResolver(
    const resources::Pool& pool) noexcept {
  return framegraph::ResourceAliasResolver{
      .context = &pool,
      .resolve = resolveResourceAlias,
      .depth_stencil_clear_covers_resource =
          depthStencilClearCoversResource,
  };
}

void DagObserver::observeAndExportDagToDir(const core::ChunkSlot& slot,
                                           std::uint64_t frameId,
                                           const std::string& dumpDir) const {
  observeAndExportDagToDir(core::SourcePayloadView(slot), slot.seqId, frameId,
                           dumpDir);
}

void DagObserver::observeAndExportDagToDir(core::SourcePayloadView payload,
                                           std::uint64_t seqId,
                                           std::uint64_t frameId,
                                           const std::string& dumpDir) const {
  // Test-seam variant: write JSON to an explicit directory (bypasses the
  // dumpDagDir() static cache). PURE OBSERVATION (R-BACK-39.7 side-effect
  // neutral) — reads only `slot`, writes only dump files plus the
  // observation-only perf counters below; mutates no shared / queue / Metal
  // state, so an enabled dump leaves the Metal stream byte-identical to a
  // disabled run. const / counter-free: the frame id is caller-supplied.
  framegraph::FrameGraph fg = framegraph::buildFrameGraph(payload, frameId);
  // Capture the built pass count before runOptimizer can prune/coalesce.
  const std::uint64_t passesBuilt =
      static_cast<std::uint64_t>(fg.passes.size());
  writeDagJsonToDir(fg, frameId, seqId, "pre-opt", dumpDir, payload);
  framegraph::OptimizerStats stats;
  // ANALYSIS-ONLY post-opt override (R-BACK-39.7): DXMT9_RENDERER_DUMP_DAG_OPTIMIZE
  // can select the optimizer passes the post-opt snapshot runs, independent of
  // the backend's encode options_. The observer never drives the Metal encode,
  // so this cannot change rendered output — it only changes what the post-opt
  // DAG (and its framegraph_* counters) reflects vs the un-optimized pre-opt
  // baseline. With no override the backend's options_ are used (current behavior).
  const framegraph::OptimizerOptions& opt =
      framegraph::dumpDagOptimizeOverride().value_or(options_);
  framegraph::runOptimizer(fg, opt, /*observations=*/nullptr, &stats);
  writeDagJsonToDir(fg, frameId, seqId, "post-opt", dumpDir, payload);
  recordFramegraphObserveCounters(passesBuilt, stats);
}

void DagObserver::observeAndExportDagToDirForFrame(
    const core::ChunkSlot& slot, const std::string& dumpDir,
    std::optional<std::uint64_t> targetFrame, std::uint64_t radius) {
  observeAndExportDagToDirForFrame(core::SourcePayloadView(slot), slot.seqId,
                                   dumpDir, targetFrame, radius);
}

void DagObserver::observeAndExportDagToDirForFrame(
    core::SourcePayloadView payload, std::uint64_t seqId,
    const std::string& dumpDir,
    std::optional<std::uint64_t> targetFrame, std::uint64_t radius) {
  // Testable mirror of observeAndExport's per-frame filter, but writing JSON to
  // an explicit dir (bypassing the dumpDagDir() static cache) and taking the
  // resolved target frame + window radius as arguments (bypassing the
  // DXMT9_RENDERER_DUMP_DAG_FRAME / _RADIUS static caches). NON-const: advances
  // observe_frame_ exactly like the production path.
  //
  // Cheap Present scan FIRST — when this chunk is filtered out we must not build
  // the FrameGraph (the flood-avoiding early-out), but we still have to advance
  // the frame counter when the chunk ends a frame.
  const bool present = framegraph::chunkContainsPresent(payload);

  if (targetFrame.has_value() &&
      !frameInDumpWindow(observe_frame_, *targetFrame, radius)) {
    // Outside the [max(1, N-R), N+R] window: skip the build/serialize entirely.
    // Only the Present scan above ran. Advance the counter when this chunk
    // closes its frame so the window is reached and then left correctly.
    if (present) {
      ++observe_frame_;
    }
    return;
  }

  // Target frame (or unfiltered): build -> dump pre-opt -> optimize -> dump
  // post-opt. frame_id in the filename is the observe frame number; slot.seqId
  // stays the chunk_seq_id (in the JSON body via writeDagJsonToDir).
  framegraph::FrameGraph fg =
      framegraph::buildFrameGraph(payload, observe_frame_);
  const std::uint64_t passesBuilt =
      static_cast<std::uint64_t>(fg.passes.size());
  writeDagJsonToDir(fg, observe_frame_, seqId, "pre-opt", dumpDir, payload);
  framegraph::OptimizerStats stats;
  // ANALYSIS-ONLY post-opt override (R-BACK-39.7) — see observeAndExportDagToDir.
  const framegraph::OptimizerOptions& opt =
      framegraph::dumpDagOptimizeOverride().value_or(options_);
  framegraph::runOptimizer(fg, opt, /*observations=*/nullptr, &stats);
  writeDagJsonToDir(fg, observe_frame_, seqId, "post-opt", dumpDir, payload);
  recordFramegraphObserveCounters(passesBuilt, stats);

  // Advance the frame counter EVEN when this chunk was dumped, so all of frame
  // N's chunks dump and then the next chunk belongs to frame N+1.
  if (present) {
    ++observe_frame_;
  }
}

void DagObserver::observeAndExport(
    const core::ChunkSlot& slot,
    framegraph::ResourceAliasResolver aliasResolver) {
  observeAndExport(core::SourcePayloadView(slot), slot.seqId, aliasResolver);
}

void DagObserver::observeAndExport(
    core::SourcePayloadView payload, std::uint64_t seqId,
    framegraph::ResourceAliasResolver aliasResolver) {
  // Zero-overhead default path: the dump is purely DXMT9_RENDERER_DUMP_DAG-gated
  // (backend-agnostic). With no dump dir there is nothing to observe and nothing
  // to scope by frame, so we return after one cached-optional check — the Metal
  // command stream stays byte-identical to a disabled run on BOTH backends. The
  // frame counter is NOT advanced here: with no dump dir there is nothing to
  // scope by frame.
  if (!framegraph::dumpDagDir().has_value()) {
    return;
  }

  // Cheap Present scan FIRST so a filtered-out chunk never builds the
  // FrameGraph (the flood-avoiding early-out for real apps that emit thousands
  // of chunks/frames; DXMT9_RENDERER_DUMP_DAG_FRAME scopes the dump to one
  // frame).
  const bool present = framegraph::chunkContainsPresent(payload);
  const std::optional<std::uint64_t> target = framegraph::dumpDagFrame();
  const std::uint64_t radius = framegraph::dumpDagFrameRadius();

  if (target.has_value() &&
      !frameInDumpWindow(observe_frame_, *target, radius)) {
    // Outside the [max(1, N-R), N+R] window: skip the build/serialize entirely.
    // Advance the frame counter when this chunk closes its frame so the counter
    // keeps tracking inter-present frames toward (and out of) the window.
    if (present) {
      ++observe_frame_;
    }
    return;
  }

  // Production path: build -> dump pre-opt -> optimize -> dump post-opt via the
  // spec's writeDagDump, which honors DXMT9_RENDERER_DUMP_DAG_FORMATS (json /
  // dot / mermaid) and warns at most once on an unwritable dir (R-BACK-39.7).
  // PURE OBSERVATION — reads only `slot`, writes only dump files. The frame_id
  // stamped into the filename / JSON is the observe frame number (so the
  // filenames read dag-frame<N>-...), while slot.seqId stays the chunk_seq_id.
  framegraph::FrameGraph fg =
      framegraph::buildFrameGraph(payload, observe_frame_, aliasResolver);
  // Capture the built pass count before runOptimizer can prune/coalesce.
  const std::uint64_t passesBuilt =
      static_cast<std::uint64_t>(fg.passes.size());
  framegraph::writeDagDump(fg, observe_frame_, seqId, "pre-opt", payload);

  // Run the resolved optimizer passes (the owning backend's options_). For the
  // traditional backend and L1-strict framegraph these are all-false, so only
  // lifetime + loadstore run — the R-BACK-40.5 parity baseline. No cross-frame
  // memoryless observations are tracked here, so pass nullptr for the
  // observation out-param. R-BACK-39.2 (Task B11): collect an OptimizerStats so
  // the observe-path perf counters reflect the optimizer's decisions.
  //
  // ANALYSIS-ONLY post-opt override (R-BACK-39.7): when
  // DXMT9_RENDERER_DUMP_DAG_OPTIMIZE is set it selects the optimizer passes for
  // the post-opt snapshot instead of the backend's options_. The observer never
  // drives the Metal encode, so this cannot change rendered output; it only
  // changes what the post-opt DAG / framegraph_* counters reflect relative to
  // the un-optimized pre-opt baseline. Unset → use options_ (current behavior).
  framegraph::OptimizerStats stats;
  const framegraph::OptimizerOptions& opt =
      framegraph::dumpDagOptimizeOverride().value_or(options_);
  framegraph::runOptimizer(fg, opt, /*observations=*/nullptr, &stats);
  framegraph::writeDagDump(fg, observe_frame_, seqId, "post-opt", payload);
  recordFramegraphObserveCounters(passesBuilt, stats);

  // Advance on present EVEN when the chunk was dumped, so frame N's multiple
  // chunks all dump before the counter moves to N+1.
  if (present) {
    ++observe_frame_;
  }
}

}  // namespace dxmt9::render
