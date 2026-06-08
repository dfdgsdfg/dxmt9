#pragma once

// FrameGraphBackend — modern-renderer backend (Task A5, R-BACK-40.5).
//
// At L0 this is a *pure delegate*: onChunkReady forwards verbatim to
// encoders::encodeChunk, so it is byte-identical to the traditional path.
// The DAG/optimizer reinterpretation lands later in L1 (Task B12). Because
// there are no features at L0, the resolved feature set is always empty and
// the strict compat profile rejects any DXMT9_RENDERER_FEATURES token with a
// single warning.

#include "backend_interface.hpp"

#include "../framegraph/fg_optimizer.hpp"  // framegraph::OptimizerOptions

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace dxmt9::render {

// Compat profile governs how unknown / not-yet-implemented feature tokens are
// handled. At L0 only `Strict` exists in effect: every feature token is
// rejected because no feature behavior is implemented.
enum class RendererCompatProfile { Strict };

// Resolved DXMT9_RENDERER_FEATURES set. At L0 this carries no enabled feature;
// it exists so the (empty) result can be stored as a member and unit-tested.
struct RendererFeatureSet {
  bool empty() const { return true; }
};

// Pure resolver: parse a DXMT9_RENDERER_FEATURES-style env string under the
// given compat profile. At L0 the Strict profile rejects every token (logging
// a single warning) and always returns an empty feature set. Null / empty /
// garbage input all yield an empty set. Testable without touching the
// environment.
RendererFeatureSet resolveRendererFeatures(const char* env,
                                           RendererCompatProfile profile);

class FrameGraphBackend final : public IRenderBackend {
 public:
  FrameGraphBackend();
  ~FrameGraphBackend() override = default;

  // L1 (R-BACK-40.5): the Metal encode stays byte-identical — onChunkReady
  // delegates verbatim to encoders::encodeChunk. The only addition over L0 is
  // the side-effect-neutral observe+export side-channel below, which builds and
  // dumps the Frame Graph DAG for debugging WITHOUT touching the encode.
  std::optional<core::metalqueue::QueueSubmissionRecord> onChunkReady(
      encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      const core::ChunkSlot& slot) override;

  BackendMode mode() const override { return BackendMode::FrameGraph; }

  // Resolved optimizer options for this backend (from the L0 feature set). At
  // L0/L1 strict the feature set is empty, so every option is false (parity
  // baseline). Exposed for unit testing the option resolution.
  const framegraph::OptimizerOptions& optimizerOptions() const {
    return options_;
  }

  // Device-free observe+export side-channel (R-BACK-39.7 side-effect neutral).
  // Early-outs when the dump dir is unset AND no features are enabled (zero
  // overhead on the default render path). Otherwise builds + dumps the DAG via
  // the resolved dump directory. onChunkReady calls this before delegating to
  // encoders::encodeChunk.
  //
  // NON-const because it advances the inter-present frame counter
  // (observe_frame_). It also honors DXMT9_RENDERER_DUMP_DAG_FRAME and
  // DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS: when that filter selects frame N with
  // radius R, every chunk whose observe frame is OUTSIDE the inclusive window
  // [max(1, N-R), N+R] early-outs BEFORE building the FrameGraph (only a cheap
  // Present scan runs), so a real app dumping thousands of chunks/frames touches
  // the disk for the selected window only (one frame when R=0).
  //
  // Public so the device-free unit test can drive the early-out directly:
  // onChunkReady is device-gated (needs an EncodeContext / MTLDevice) and the
  // class is `final`, so a test subclass / friend is not available.
  void maybeObserveAndExportDag(const core::ChunkSlot& slot);

  // Explicit-directory observe+export, factored out so the unit test can drive
  // the build->optimize->serialize->write composition with a temp dir WITHOUT
  // fighting framegraph::dumpDagDir()'s static DXMT9_RENDERER_DUMP_DAG cache
  // (which cannot be reset in-process). Builds the FrameGraph from `slot`,
  // writes the pre-opt DAG JSON, runs the resolved optimizer passes, then writes
  // the post-opt DAG JSON, each as
  // `<dir>/dag-frame<frameId>-chunk<slot.seqId>-<stage>.json`. Reads only `slot`
  // and writes only those files — it must NOT mutate any shared / queue / Metal
  // state, so an enabled dump leaves the Metal stream byte-identical to a
  // disabled run. Never throws; an unwritable path is silently skipped.
  //
  // const / counter-free: the caller supplies frameId, so this is a pure test
  // seam unaffected by the static DXMT9_RENDERER_DUMP_DAG_FRAME env cache.
  void observeAndExportDagToDir(const core::ChunkSlot& slot,
                                std::uint64_t frameId,
                                const std::string& dumpDir) const;

  // Testable frame-filter seam (mirrors maybeObserveAndExportDag's per-frame
  // filter + present-advance logic) but takes the resolved target frame, the
  // window radius, and an explicit dump dir as arguments, so it can be driven
  // across synthetic chunks without depending on the static
  // DXMT9_RENDERER_DUMP_DAG_FRAME / _RADIUS env caches. NON-const: it advances
  // observe_frame_ exactly like the production path. With `targetFrame` set,
  // only chunks whose observe frame falls in the inclusive window
  // [max(1, targetFrame-radius), targetFrame+radius] are dumped; with
  // std::nullopt every chunk is dumped (unfiltered, `radius` ignored). The frame
  // counter advances on a chunk that contains a Present (the last chunk of its
  // frame). Reads only `slot`, writes only dump files + the observe-path perf
  // counters.
  void observeAndExportDagToDirForFrame(
      const core::ChunkSlot& slot, const std::string& dumpDir,
      std::optional<std::uint64_t> targetFrame, std::uint64_t radius = 0);

  // Encode-thread-local inter-present frame counter (1-based). SINGLE WRITER:
  // only the encode thread touches it (through maybeObserveAndExportDag /
  // observeAndExportDagToDirForFrame), so no atomic / lock is needed.
  std::uint64_t observeFrame() const { return observe_frame_; }

 private:
  RendererFeatureSet features_;
  // R-BACK-40.5: at L1 strict the feature set is empty, so these stay all-false
  // (parity baseline). Memoryless stays gated behind an explicit relaxation
  // (R-BACK-40.4) which does not exist yet, so it remains off here too.
  framegraph::OptimizerOptions options_{};
  // Inter-present frame counter (1-based), only mutated on the encode thread
  // (single writer; no atomic needed). Advances AFTER a chunk that contains a
  // Present, so all of frame N's chunks share observe_frame_ == N.
  std::uint64_t observe_frame_ = 1;
};

}  // namespace dxmt9::render
