#pragma once

// Frame Graph linearizer (Task B9, L1).
//
// Spec: specs/d3d9-renderer/design.md §6 (Linearizer), §14 L1 (R-BACK-39.1
//   byte-identical parity baseline), §15 (IExternalDrawEmitter split).
//
// TWO-PART DESIGN (full encode is device-gated — same constraint A8 found):
//
//   1. planLinearization() — the DEVICE-FREE testable core.
//      Walks the optimized FrameGraph in pass order and emits an ordered POD
//      list of LinearOp (BeginPass / EmitDraw / EmitClear / EndPass / Present /
//      Blit). It touches no Metal device, so the native (device-free) test host
//      can assert the op sequence and diff it against the natural chunk order.
//      This is the artifact the L1 parity-baseline test and the §13 parity
//      harness compare.
//
//   2. executeLinearization() — the DEVICE-GATED executor (wired by B12).
//      Walks the SAME plan and drives the REAL encode through
//      render::IExternalDrawEmitter + encoders::beginRenderPass / endEncoding.
//      It needs a real WMT::Device / CommandBuffer / EncodeContext, which the
//      native test host lacks, so it is NOT exercised by the unit test (see the
//      device-gated boundary note on the declaration). B12 calls it from
//      FrameGraphBackend::onChunkReady.
//
// L1 CORRECTNESS (design.md §14 L1 / R-BACK-39.1):
//   Under default OptimizerOptions{} (no feature passes), the plan MUST
//   reproduce the source chunk's natural pass/draw order — BeginPass /
//   EmitDraw.../ EndPass per render pass, draws in original submission order,
//   Present last. This is the byte-identical parity baseline (the actual
//   byte-exact Metal proof is the device-gated wine conformance leg; the
//   device-free plan proves order).
//
// CLEARS (A3 finding):
//   dxmt9 has no "clear into an open encoder" primitive. The builder folds a
//   clear into a FRESH render pass whose first attachment access is a Clear,
//   which loadstore.cpp turns into LoadAction::Clear. The plan therefore
//   reflects a clear as the BeginPass LoadStorePolicy (LoadAction::Clear) of a
//   render pass — the executor opens that pass with a LoadActionClear
//   beginRenderPass, NOT a mid-pass clear call. A standalone EmitClear op is
//   reserved for a pass that is purely a clear boundary with no draws (e.g. a
//   clear of a target that is never subsequently drawn in the same chunk).
//
// DETERMINISM (R-BACK-32.2): pure walk over the graph; no clock/thread/RNG.

#include "fg_dag.hpp"

#include <cstdint>
#include <vector>

namespace dxmt9::framegraph {

// Kind of a single linearized operation. POD; the executor switches on it.
enum class LinearOpKind : u8 {
  BeginPass,   // open a render pass with `attachments` + `load_store`.
  EmitDraw,    // emit one DrawRef through the traditional draw path.
  EmitClear,   // standalone clear boundary (own LoadActionClear pass, A3).
  EndPass,     // close the current render pass.
  Present,     // present the swap-chain source.
  Blit,        // blit/readback/copy pass.
};

// One ordered operation in the linearization plan. A flat record: BeginPass
// carries the attachment set + load/store; EmitDraw carries the DrawRef to
// hand back to the traditional path; the rest are markers keyed off
// `pass_index`. Storing the DrawRef by value (not an index into the graph)
// keeps the plan self-contained for the parity diff.
struct LinearOp {
  LinearOpKind kind = LinearOpKind::BeginPass;
  u32 pass_index = 0;        // source PassNode index (post-optimizer order).

  // Valid for BeginPass.
  AttachmentSet attachments{};
  LoadStorePolicy load_store{};
  PassKind pass_kind = PassKind::Render;  // BeginPass/Present/Blit pass kind.

  // Valid for EmitDraw.
  DrawRef draw{};

  friend bool operator==(const LinearOp&, const LinearOp&) = default;
};

// The device-free linearization artifact (design.md §6 / §14 L1). The ops are
// in final submission order; for a render pass it is
// BeginPass, EmitDraw*, EndPass; Present / Blit passes are single ops.
struct LinearizationPlan {
  std::vector<LinearOp> ops;

  friend bool operator==(const LinearizationPlan&, const LinearizationPlan&) = default;
};

// Walk the optimized FrameGraph in pass order and produce the ordered op list.
// Skips dead passes (flags.dead, set by DCE). Deterministic. DEVICE-FREE.
//
// Under default OptimizerOptions the produced ops reproduce the chunk's natural
// pass/draw order (parity baseline). With passcoalesce the merged pass yields a
// single BeginPass/EndPass spanning both source passes' draws in submission
// order; with DCE the dead pass's ops are absent.
LinearizationPlan planLinearization(const FrameGraph& graph);

// In-place variant: reuse caller scratch (clears it first). Identical contents.
void planLinearization(const FrameGraph& graph, LinearizationPlan& out);

}  // namespace dxmt9::framegraph

// ---------------------------------------------------------------------------
// DEVICE-GATED EXECUTOR (Task B9 part 2; wired by B12).
//
// executeLinearization() walks the SAME plan planLinearization() produces and
// drives the REAL encode. It is declared in a separate translation unit guard
// because it pulls in the Metal-touching encoder headers (winemetal refs via
// dxmt9_draw_encoder.hpp). The native (device-free) unit test for B9 includes
// ONLY the section above and never calls executeLinearization — there is no
// MTLDevice in the native test host (same constraint A8 documented). B12's
// device-side test / the wine conformance leg cover the executor.
// ---------------------------------------------------------------------------

#include "../render/external_draw_emitter.hpp"  // render::IExternalDrawEmitter
#include "../dxmt9_draw_encoder.hpp"             // encoders::{EncodeContext, beginRenderPass}
#include "../dxmt9_backend_types.hpp"            // core::ChunkSlot

namespace dxmt9::framegraph {

// Drive the real Metal encode from a linearization plan + the source chunk.
//
// DEVICE-GATED — do NOT call from the device-free B9 unit test.
//
// For each op:
//   BeginPass  -> encoders::beginRenderPass(ctx, cb, drawState, clear) opens a
//                 render pass; the plan's LoadStorePolicy selects load/store
//                 (a LoadAction::Clear attachment opens the pass with a Metal
//                 LoadActionClear — the A3 "clear is its own LoadActionClear
//                 pass" path; there is no clear-into-open-encoder).
//   EmitDraw   -> the DrawRef's command_index selects the source DrawRun in
//                 `slot`; slot.drawRunCommandAt(cmd) yields the FlatDrawStateView
//                 + the DrawParam span. The executor iterates the run's
//                 param_first..param_first+param_count DrawParams and calls
//                 emitter.emitDraw(ctx, cb, encoder, drawState, seqId, param)
//                 once per draw-call ordinal, pulling the original draw state
//                 back out of the chunk slot (no decoded geometry is stored in
//                 the graph — L1 DrawRef shape, fg_dag.hpp).
//   EmitClear  -> ends any open encoder (ClearBarrier split) then
//                 emitter.emitClearWithinPass(ctx, cb, clear), which opens its
//                 own LoadActionClear render pass.
//   EndPass    -> encoder.endEncoding() closes the open render pass.
//   Present    -> caller-owned present encode (B12 forwards to the presenter).
//   Blit       -> caller-owned blit/readback encode (B12 forwards to the blit
//                 encoders).
//
// `slot.seqId` is the GPU seq id every encodeDraw / transient reservation keys
// off (mirrors encodeChunk's encodeChunkSeqId). `emitter` is AEC's
// IExternalDrawEmitter; `ctx` / `cb` are caller-owned per §15 / R-BACK-31.8.
void executeLinearization(const FrameGraph& graph,
                          const LinearizationPlan& plan,
                          const core::ChunkSlot& slot,
                          encoders::EncodeContext& ctx,
                          WMT::CommandBuffer& commandBuffer,
                          render::IExternalDrawEmitter& emitter);

}  // namespace dxmt9::framegraph
