#pragma once

// TraditionalBackend (Task A4) — the byte-identical baseline backend.
// It is a stateless forwarding shim: onChunkReady forwards straight to
// encoders::encodeChunk, and the IExternalDrawEmitter half forwards to
// encoders::encodeDraw / encoders::encodeClearPass with all the trailing
// encode knobs left at their defaults. No state, no caching — any divergence
// from the traditional path would defeat the A8 parity harness.

#include "backend_interface.hpp"
#include "external_draw_emitter.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace dxmt9::render {

class TraditionalBackend final : public IRenderBackend,
                                 public IExternalDrawEmitter {
 public:
  // Lifecycle hooks (onDeviceCreated/Destroyed, onFrameBegin/End) keep the
  // IRenderBackend no-op defaults: the traditional path has nothing to set up.

  std::optional<core::metalqueue::QueueSubmissionRecord> onChunkReady(
      encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      const core::ChunkSlot& slot) override;

  BackendMode mode() const override { return BackendMode::Traditional; }

  bool emitDraw(
      encoders::EncodeContext& ctx,
      WMT::CommandBuffer& commandBuffer,
      WMT::RenderCommandEncoder& encoder,
      core::FlatDrawStateView drawState,
      std::uint64_t seqId,
      const core::DrawParam& param) override;

  void emitClearWithinPass(
      encoders::EncodeContext& ctx,
      WMT::CommandBuffer& commandBuffer,
      const core::ClearDesc& clear) override;
};

}  // namespace dxmt9::render
