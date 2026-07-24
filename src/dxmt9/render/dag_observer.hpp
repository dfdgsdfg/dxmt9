#pragma once

// DagObserver — shared, backend-agnostic Frame Graph DAG observe + export
// side-channel (R-BACK-39.7 side-effect neutral).
//
// The DAG debug dump (DXMT9_RENDERER_DUMP_DAG) is a PURE OBSERVATION channel:
// it builds a framegraph::FrameGraph from one core::ChunkSlot, dumps the
// pre-opt DAG, runs the owning backend's resolved optimizer options, then dumps
// the post-opt DAG. Which backend actually encodes the chunk is irrelevant to
// the dump, so the same observer is owned by BOTH render backends:
//   * FrameGraphBackend constructs it with its resolved feature OptimizerOptions
//     so the pre-opt/post-opt diff reflects the passes it would run.
//   * TraditionalBackend constructs it with default OptimizerOptions{} (no
//     features), so the observed post-opt DAG is the order-preserving parity
//     baseline; its real encode stays byte-identical on encoders::encodeChunk.
//
// SIDE-EFFECT NEUTRAL.
//   observeAndExport reads only `slot` and writes only dump files plus the
//   process-global atomic perf counters (which no-op unless DXMT_PERF_COUNTERS
//   is set). It touches no Metal / queue / cache / pool state, so an enabled
//   dump leaves the Metal command stream byte-identical to a disabled run
//   (R-BACK-40.5 parity). The default path (no DXMT9_RENDERER_DUMP_DAG) early-
//   outs after one cached-optional check.

#include "../framegraph/fg_builder.hpp"    // framegraph::ResourceAliasResolver
#include "../framegraph/fg_optimizer.hpp"  // framegraph::OptimizerOptions

#include <cstdint>
#include <optional>
#include <string>

namespace dxmt9::core {
struct ChunkSlot;
}  // namespace dxmt9::core

namespace dxmt9::resources {
struct Pool;
}  // namespace dxmt9::resources

namespace dxmt9::render {

// Build the hazard-identity resolver shared by the production observer and
// framegraph encode path. Surface aliases canonicalize to their owning texture;
// texture and standalone-surface handles remain unchanged.
framegraph::ResourceAliasResolver makeResourceAliasResolver(
    const resources::Pool& pool) noexcept;

class DagObserver {
 public:
  DagObserver() = default;
  explicit DagObserver(const framegraph::OptimizerOptions& options)
      : options_(options) {}

  // Production observe path. Zero-overhead default: early-outs when the dump dir
  // (DXMT9_RENDERER_DUMP_DAG) is unset — purely dump-dir-gated, so it works on
  // any backend even with no features. Otherwise it does a cheap Present scan,
  // applies the DXMT9_RENDERER_DUMP_DAG_FRAME[_RADIUS] window filter, and on a
  // windowed frame builds -> dumps pre-opt -> runs the owning backend's
  // optimizer options -> dumps post-opt -> records the framegraph_* counters.
  //
  // NON-const: advances the inter-present frame counter (observe_frame_) after a
  // chunk that contains a Present. SINGLE WRITER: only the encode thread calls
  // this, so no atomic / lock is needed.
  void observeAndExport(
      const core::ChunkSlot& slot,
      framegraph::ResourceAliasResolver aliasResolver = {});

  // Explicit-directory observe+export test seam: writes the pre-opt + post-opt
  // DAG JSON to `dumpDir` for the caller-supplied `frameId`, bypassing the
  // framegraph::dumpDagDir() static DXMT9_RENDERER_DUMP_DAG cache. const /
  // counter-free w.r.t. observe_frame_; reads only `slot`, writes only those
  // files plus the observation-only perf counters.
  void observeAndExportDagToDir(const core::ChunkSlot& slot,
                                std::uint64_t frameId,
                                const std::string& dumpDir) const;

  // Frame-filter test seam: mirrors observeAndExport's window filter + present-
  // advance logic but takes an explicit dump dir, target frame, and radius
  // (bypassing the static DXMT9_RENDERER_DUMP_DAG_FRAME / _RADIUS caches).
  // NON-const: advances observe_frame_ exactly like the production path. With
  // `targetFrame` set only chunks whose observe frame falls in the inclusive
  // window [max(1, targetFrame-radius), targetFrame+radius] dump; with
  // std::nullopt every chunk dumps (radius ignored).
  void observeAndExportDagToDirForFrame(
      const core::ChunkSlot& slot, const std::string& dumpDir,
      std::optional<std::uint64_t> targetFrame, std::uint64_t radius = 0);

  // Encode-thread-local inter-present frame counter (1-based). SINGLE WRITER.
  std::uint64_t observeFrame() const { return observe_frame_; }

  const framegraph::OptimizerOptions& options() const { return options_; }

 private:
  // The optimizer options the owning backend would run; drives the pre-opt vs
  // post-opt DAG diff. Default-constructed (all-false parity baseline) when the
  // owner is the traditional path.
  framegraph::OptimizerOptions options_{};
  // Inter-present frame counter (1-based). Only mutated on the encode thread
  // (single writer; no atomic needed). Advances AFTER a chunk that contains a
  // Present, so all of frame N's chunks share observe_frame_ == N.
  std::uint64_t observe_frame_ = 1;
};

}  // namespace dxmt9::render
