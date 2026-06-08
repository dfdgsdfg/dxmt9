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
  // overhead on the default render path). Otherwise forwards to
  // observeAndExportDagToDir with the resolved dump directory. onChunkReady
  // calls this before delegating to encoders::encodeChunk.
  //
  // Public so the device-free unit test can drive the early-out directly:
  // onChunkReady is device-gated (needs an EncodeContext / MTLDevice) and the
  // class is `final`, so a test subclass / friend is not available.
  void maybeObserveAndExportDag(const core::ChunkSlot& slot,
                                std::uint64_t frameId) const;

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
  void observeAndExportDagToDir(const core::ChunkSlot& slot,
                                std::uint64_t frameId,
                                const std::string& dumpDir) const;

 private:
  RendererFeatureSet features_;
  // R-BACK-40.5: at L1 strict the feature set is empty, so these stay all-false
  // (parity baseline). Memoryless stays gated behind an explicit relaxation
  // (R-BACK-40.4) which does not exist yet, so it remains off here too.
  framegraph::OptimizerOptions options_{};
};

}  // namespace dxmt9::render
