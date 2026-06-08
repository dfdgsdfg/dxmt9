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

#include <cstddef>
#include <optional>

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

  // L0: pure delegate — identical to the traditional path.
  std::optional<core::metalqueue::QueueSubmissionRecord> onChunkReady(
      encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      const core::ChunkSlot& slot) override;

  BackendMode mode() const override { return BackendMode::FrameGraph; }

 private:
  RendererFeatureSet features_;
};

}  // namespace dxmt9::render
