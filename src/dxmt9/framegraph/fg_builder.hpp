#pragma once

// Frame Graph builder (Task B2, L1).
//
// Spec: specs/d3d9-renderer/spec.md §4 (Frame Graph Builder):
//   §4.1 chunk-record → builder action, §4.2 dependency edge inference,
//   §4.3 determinism (R-BACK-32.2).
//
// SCOPE — L1 ONLY.
//   A single forward pass over ONE immutable `core::SourcePayloadView`
//   (legacy ChunkSlot or arena-backed storage) produces the
//   `framegraph::FrameGraph` declared in fg_dag.hpp (B1). The graph references
//   draws as lightweight `DrawRef`s into the source payload — no decoded
//   geometry/binding payload is copied (deferred to the L2 DrawDescriptor).
//
//   The builder mirrors the grouping `encoders::encodeChunk`
//   (dxmt9_draw_encoder.mm) performs over the SAME payload so the eventual
//   linearizer (B9) can re-emit draws in the original order through the
//   traditional path. Pass boundaries are decided on the attachment set
//   (color0..N + depth handles) — the same `AttachmentKey` the encoder splits
//   on — plus clear / present / blit / readback command boundaries.
//
// DETERMINISM (R-BACK-32.2 / spec.md §4.3).
//   Reads only the supplied payload and optional retained resource-alias
//   mapping — no clock, thread-id, or RNG. Building the same graph twice from
//   the same inputs yields byte-equal contents (asserted by
//   fg_builder_spec.cpp).

#include "fg_dag.hpp"

#include "../dxmt9_source_payload.hpp"

namespace dxmt9::framegraph {

// Optional resource-identity resolver used by the production unix renderer.
//
// D3D9 render-target/depth writes name a SurfaceHandle, while shader reads of
// that same backing name the owning TextureHandle. Edge inference must compare
// those accesses in one handle space or it can miss RAW/WAR/WAW hazards and let
// passcoalesce move a consumer across its producer. The resolver canonicalizes
// a surface alias to its owning texture; standalone surfaces and texture
// handles resolve to themselves.
//
// The callback shape keeps the device-free builder independent of the Metal
// resource pool. The optional aspect callback also answers whether a
// depth/stencil Clear covers every aspect present in the retained surface
// format. Tests and strict observation may omit either callback; production
// supplies both from immutable retained resource records.
struct ResourceAliasResolver {
  using ResolveFn =
      ResourceHandle (*)(const void* context, ResourceHandle handle) noexcept;
  using DepthStencilClearCoversResourceFn =
      bool (*)(const void* context, ResourceHandle handle,
               bool clearDepth, bool clearStencil) noexcept;

  const void* context = nullptr;
  ResolveFn resolve = nullptr;
  DepthStencilClearCoversResourceFn depth_stencil_clear_covers_resource =
      nullptr;

  ResourceHandle operator()(ResourceHandle handle) const noexcept {
    return resolve ? resolve(context, handle) : handle;
  }

  bool depthStencilClearCoversResource(ResourceHandle handle,
                                       bool clearDepth,
                                       bool clearStencil) const noexcept {
    if (depth_stencil_clear_covers_resource) {
      return depth_stencil_clear_covers_resource(
          context, handle, clearDepth, clearStencil);
    }
    // Without format evidence, only clearing both logical aspects is a
    // conservative full-resource overwrite.
    return clearDepth && clearStencil;
  }
};

// Build a FrameGraph DAG from one immutable published source payload.
//
// `payload` is borrowed for the duration of the call only; the returned graph
// holds source-local command/param indices, NOT pointers into its storage, so
// the graph outlives the borrow safely. `frame_id` is caller-supplied.
FrameGraph buildFrameGraph(core::SourcePayloadView payload,
                           u64 frame_id = 0);
FrameGraph buildFrameGraph(core::SourcePayloadView payload, u64 frame_id,
                           ResourceAliasResolver alias_resolver);

// Compatibility wrappers for callers that still own a legacy ChunkSlot.
FrameGraph buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id = 0);
FrameGraph buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id,
                           ResourceAliasResolver alias_resolver);

// In-place variant: reuse the caller's per-frame graph scratch (clears it
// first). Identical contents to buildFrameGraph; avoids a fresh allocation
// when the backend keeps a long-lived FrameGraph for the encode thread.
void buildFrameGraph(core::SourcePayloadView payload, u64 frame_id,
                     FrameGraph& out);
void buildFrameGraph(core::SourcePayloadView payload, u64 frame_id,
                     ResourceAliasResolver alias_resolver, FrameGraph& out);
void buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id,
                     FrameGraph& out);
void buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id,
                     ResourceAliasResolver alias_resolver, FrameGraph& out);

}  // namespace dxmt9::framegraph
