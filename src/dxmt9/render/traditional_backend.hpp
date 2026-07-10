#pragma once

// TraditionalBackend (Task A4) — the byte-identical baseline backend.
// onChunkReady forwards straight to encoders::encodeChunk, and the
// IExternalDrawEmitter half forwards to encoders::encodeDraw /
// encoders::encodeClearPass with all the trailing encode knobs left at their
// defaults. Any divergence from the traditional encode path would defeat the A8
// parity harness.
//
// The only state it carries is a render::DagObserver: the DAG debug dump
// (DXMT9_RENDERER_DUMP_DAG) is a backend-agnostic, side-effect-neutral
// observation channel (R-BACK-39.7), so it is available on the traditional path
// too — built purely for observation with default OptimizerOptions{} (the
// order-preserving baseline). The default render path (dump dir unset) early-
// outs in observeAndExport, so the Metal command stream stays byte-identical.

#include "backend_interface.hpp"
#include "dag_observer.hpp"
#include "external_draw_emitter.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dxmt9::render {

class TraditionalBackend final : public IRenderBackend,
                                 public IExternalDrawEmitter {
 public:
  // Lifecycle hooks (onDeviceCreated/Destroyed, onFrameBegin/End) keep the
  // IRenderBackend no-op defaults: the traditional path has nothing to set up.

  std::optional<core::metalqueue::QueueSubmissionRecord> onChunkReady(
      encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      const core::ChunkSlot& slot,
      encoders::EncodeChunkOptions options = {}) override;

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

  // The shared backend-agnostic DAG observe + export side-channel, constructed
  // with default OptimizerOptions{} (the traditional path runs no optimizer
  // passes; the observed post-opt DAG is the order-preserving parity baseline).
  // Exposed so the device-free unit test can drive the observe path directly
  // (onChunkReady is device-gated and the class is `final`).
  DagObserver& observer() { return observer_; }
  const DagObserver& observer() const { return observer_; }

 private:
  // Default OptimizerOptions{}: traditional has no features. The observe path
  // early-outs unless DXMT9_RENDERER_DUMP_DAG is set, so a production run pays
  // only one cached-optional check and the Metal stream stays byte-identical.
  DagObserver observer_{framegraph::OptimizerOptions{}};
};

}  // namespace dxmt9::render
