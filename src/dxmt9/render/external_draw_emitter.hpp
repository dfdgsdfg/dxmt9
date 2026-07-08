#pragma once

// spec.md §15 IExternalDrawEmitter — narrow interface that lets an external
// (e.g. modern-renderer) caller drive the existing free-function draw/clear
// encode path without re-implementing it.
//
// Mapping to the REAL code (verified against the free-function encoder):
//   - emitDraw            forwards to encoders::encodeDraw
//                         (decl src/dxmt9/dxmt9_draw_encoder.hpp:360,
//                          def src/dxmt9/dxmt9_draw_encoder.mm:13271).
//                         The interface exposes only the STABLE leading subset
//                         of that signature; the many trailing encodeDraw knobs
//                         (skipBaseStateBind, preUploaded, paramPayloadArena,
//                          dirty, tile/argbuf-hybrid flags, shadows, ...) keep
//                         their defaults and stay an implementation detail.
//   - emitClearWithinPass forwards to encoders::encodeClearPass
//                         (decl src/dxmt9/dxmt9_blit_encoders.hpp:66).
//
// NOTE on the clear path: dxmt9 has no "emit a clear into an already-open
// render encoder" primitive. A clear forces an encoder split
// (perf::EncoderSplitReason::ClearBarrier) and then encodeClearPass opens a
// FRESH render pass with WMTLoadActionClear. So the clear method takes the
// CommandBuffer (not an open RenderCommandEncoder); the caller still owns the
// surrounding beginRenderPass / endEncoding lifecycle for draws. The resources
// pool the clear needs is read from EncodeContext::pool by the implementation.

#include "../dxmt9_draw_encoder.hpp"   // encoders::{EncodeContext, encodeDraw}; WMT refs via ../winemetal/Metal.hpp
#include "../dxmt9_blit_encoders.hpp"  // encoders::encodeClearPass
#include "dxmt9/core.hpp"              // core::{FlatDrawStateView, DrawParam, ClearDesc}

#include <cstdint>

namespace dxmt9::render {

class IExternalDrawEmitter {
 public:
  virtual ~IExternalDrawEmitter() = default;

  // Forwards to encoders::encodeDraw. The caller must have already opened the
  // render pass (encoders::beginRenderPass) on `encoder`. Returns encodeDraw's
  // success bool.
  virtual bool emitDraw(
      encoders::EncodeContext& ctx,
      WMT::CommandBuffer& commandBuffer,
      WMT::RenderCommandEncoder& encoder,
      core::FlatDrawStateView drawState,
      std::uint64_t seqId,
      const core::DrawParam& param) = 0;

  // Forwards to encoders::encodeClearPass, which opens its own LoadActionClear
  // render pass on `commandBuffer`. Any render encoder the caller had open for
  // draws must be ended first (ClearBarrier split) — this method does not take
  // an open encoder for that reason.
  virtual void emitClearWithinPass(
      encoders::EncodeContext& ctx,
      WMT::CommandBuffer& commandBuffer,
      const core::ClearDesc& clear) = 0;
};

}  // namespace dxmt9::render
