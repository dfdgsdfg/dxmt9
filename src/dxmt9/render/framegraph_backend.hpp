#pragma once

// FrameGraphBackend — modern-renderer backend (R-BACK-40.5).
//
// `progressive` + passcoalesce is the promoted default. It builds an optimized
// source-command permutation and sends every record back through the same v2
// encodeChunk switch. Explicit `strict` remains a pure source-order delegate
// that is byte-identical to the traditional path. Other modern renderer
// features remain staged.

#include "backend_interface.hpp"
#include "dag_observer.hpp"

#include "../framegraph/fg_optimizer.hpp"  // framegraph::OptimizerOptions

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dxmt9::render {

// Compat profile governs which implemented optimizer tokens may affect Metal
// emission. Unset resolves to Progressive; unknown profiles resolve to Strict.
enum class RendererCompatProfile {
  Strict,
  Progressive,
};

// Resolved DXMT9_RENDERER_FEATURES set. Only passcoalesce has a production
// progressive lane; the remaining spec tokens are rejected until implemented.
struct RendererFeatureSet {
  bool passcoalesce = false;

  bool empty() const { return !passcoalesce; }
};

RendererCompatProfile resolveRendererCompatProfile(const char* env);

// Pure resolver: parse a DXMT9_RENDERER_FEATURES-style env string under the
// given compat profile. Strict rejects every token and always returns an empty
// feature set. Progressive enables passcoalesce when the env is unset, accepts
// explicit implemented tokens, and ignores unsupported tokens with one
// warning. An empty string or "0" explicitly disables every feature. Testable
// without touching the environment.
RendererFeatureSet resolveRendererFeatures(const char* env,
                                           RendererCompatProfile profile);

class FrameGraphBackend final : public IRenderBackend {
 public:
  FrameGraphBackend();
  ~FrameGraphBackend() override = default;

  // Strict delegates verbatim to encoders::encodeChunk. Progressive
  // passcoalesce may supply a complete source-command permutation to that same
  // encoder after DAG proof; every planning/session mismatch falls back to
  // source order. The shared DagObserver remains a separate debug side-channel.
  std::optional<core::metalqueue::QueueSubmissionRecord> onChunkReady(
      encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      const core::ChunkSlot& slot,
      encoders::EncodeChunkOptions options = {}) override;

  BackendMode mode() const override { return BackendMode::FrameGraph; }

  // Resolved optimizer options for this backend. Strict keeps every option
  // false (parity baseline). Exposed for unit testing the option resolution.
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
  RendererCompatProfile profile_ = RendererCompatProfile::Progressive;
  RendererFeatureSet features_;
  // R-BACK-40.5: strict keeps this all-false. Progressive defaults only
  // passcoalesce on. Memoryless stays gated behind the explicit relaxation
  // contract (R-BACK-40.4) and has no production lane yet.
  framegraph::OptimizerOptions options_{};
  // Shared observe + DAG-export side-channel, constructed with options_ so the
  // observed post-opt DAG reflects this backend's resolved passes.
  DagObserver observer_;
};

}  // namespace dxmt9::render
