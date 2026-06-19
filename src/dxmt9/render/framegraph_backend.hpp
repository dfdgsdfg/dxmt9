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
#include "dag_observer.hpp"

#include "../framegraph/fg_optimizer.hpp"  // framegraph::OptimizerOptions

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

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
  // the side-effect-neutral observe+export side-channel (the shared
  // render::DagObserver below), which builds and dumps the Frame Graph DAG for
  // debugging WITHOUT touching the encode.
  std::optional<core::metalqueue::QueueSubmissionRecord> onChunkReady(
      encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      const core::ChunkSlot& slot,
      encoders::EncodeChunkOptions options = {}) override;

  std::optional<core::metalqueue::QueueSubmissionRecord> onChunkBatchReady(
      encoders::EncodeContext& ctx,
      std::span<core::metalqueue::ReadySlotSnapshot> sources) override;

  BackendMode mode() const override { return BackendMode::FrameGraph; }

  // Resolved optimizer options for this backend (from the L0 feature set). At
  // L0/L1 strict the feature set is empty, so every option is false (parity
  // baseline). Exposed for unit testing the option resolution.
  const framegraph::OptimizerOptions& optimizerOptions() const {
    return options_;
  }

  // The shared DAG observe + export side-channel (R-BACK-39.7 side-effect
  // neutral) owned by this backend, constructed with the resolved feature
  // OptimizerOptions so its pre-opt/post-opt DAG diff reflects the passes this
  // backend would run. Exposed (non-const) so the device-free unit test can
  // drive the observe path directly: onChunkReady is device-gated (needs an
  // EncodeContext / MTLDevice) and the class is `final`. The DAG dump is now
  // backend-agnostic — see render::DagObserver, also owned by TraditionalBackend.
  DagObserver& observer() { return observer_; }
  const DagObserver& observer() const { return observer_; }

 private:
  RendererFeatureSet features_;
  // R-BACK-40.5: at L1 strict the feature set is empty, so these stay all-false
  // (parity baseline). Memoryless stays gated behind an explicit relaxation
  // (R-BACK-40.4) which does not exist yet, so it remains off here too.
  framegraph::OptimizerOptions options_{};
  // Shared observe + DAG-export side-channel, constructed with options_ so the
  // observed post-opt DAG reflects this backend's resolved passes.
  DagObserver observer_{options_};
};

}  // namespace dxmt9::render
