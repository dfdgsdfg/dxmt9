#pragma once

// Frame Graph builder (Task B2, L1).
//
// Spec: specs/d3d9-renderer/spec.md §4 (Frame Graph Builder):
//   §4.1 chunk-record → builder action, §4.2 dependency edge inference,
//   §4.3 determinism (R-BACK-32.2).
//
// SCOPE — L1 ONLY.
//   A single forward pass over ONE already-imported `core::ChunkSlot`
//   (spec.md §2.1: ChunkSlot IS the spec's `ChunkView`) produces the
//   `framegraph::FrameGraph` declared in fg_dag.hpp (B1). The graph references
//   draws as lightweight `DrawRef`s into the source slot — no decoded
//   geometry/binding payload is copied (deferred to the L2 DrawDescriptor).
//
//   The builder mirrors the grouping `encoders::encodeChunk`
//   (dxmt9_draw_encoder.mm) performs over the SAME ChunkSlot so the eventual
//   linearizer (B9) can re-emit draws in the original order through the
//   traditional path. Pass boundaries are decided on the attachment set
//   (color0..N + depth handles) — the same `AttachmentKey` the encoder splits
//   on — plus clear / present / blit / readback command boundaries.
//
// DETERMINISM (R-BACK-32.2 / spec.md §4.3).
//   Reads nothing but the supplied ChunkSlot — no clock, thread-id, or RNG.
//   Building the same graph twice from the same slot yields byte-equal
//   contents (asserted by fg_builder_spec.cpp).

#include "fg_dag.hpp"

#include "../dxmt9_backend_types.hpp"

namespace dxmt9::framegraph {

// Build a FrameGraph DAG from one imported chunk slot.
//
// `slot` is borrowed for the duration of the call only; the returned graph
// holds DrawRef indices into the slot's command/param SoA, NOT pointers into
// it, so the graph outlives the borrow safely. `frame_id` is stamped onto the
// graph (the slot carries a seqId, not a frame id; spec.md §3.5 notes the
// frame id is caller-supplied).
FrameGraph buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id = 0);

// In-place variant: reuse the caller's per-frame graph scratch (clears it
// first). Identical contents to buildFrameGraph; avoids a fresh allocation
// when the backend keeps a long-lived FrameGraph for the encode thread.
void buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id, FrameGraph& out);

}  // namespace dxmt9::framegraph
