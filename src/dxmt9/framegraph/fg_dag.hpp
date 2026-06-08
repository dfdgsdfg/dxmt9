#pragma once

// Frame Graph DAG data structures (Task B1, L1).
//
// Spec: specs/d3d9-renderer/design.md §3 (Frame Graph Data Structures).
//   §3.1 PassNode, §3.2 ResourceNode, §3.4 FrameGraph container.
//
// SCOPE — L1 ONLY.
//   L1 has NO mesh / bindless / ICB. The linearizer (Task B9) re-emits draws
//   through the existing traditional path (render::IExternalDrawEmitter),
//   pulling the original draw state out of the source core::ChunkSlot. The
//   graph therefore references draws as a lightweight (command-index, range)
//   pair into the chunk's SoA draw-run arrays rather than carrying any decoded
//   per-draw geometry/binding payload.
//
//   The full mesh DrawDescriptor from design.md §3.3 (vb_pointers, bindless
//   indices, bbox, …) is DEFERRED TO L2 and is intentionally NOT declared here.
//
// DETERMINISM (R-BACK-32.2).
//   Pure data + trivial helpers only. No clock, thread-id, or RNG. Building the
//   same graph twice from the same input yields byte-equal contents.
//
// DATA-ORIENTED SHAPE.
//   PassNode / ResourceNode / Edge / DrawRef are flat POD-friendly records.
//   ResourceNode carries a per-frame std::vector access log; the whole graph is
//   per-frame build scratch (design.md §3.4 "rebuilt every frame"), so the
//   per-frame vectors are acceptable build-time storage, not a hot-path
//   per-draw allocation.

#include "../dxmt9_backend_types.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace dxmt9::framegraph {

// Fixed-width aliases match the surrounding dxmt9 code (declared in
// dxmt9::core, core_constants.hpp:22).
using core::u8;
using core::u32;
using core::u64;

// Real backend handle types reused verbatim (no new handle invention):
//   core::Handle               — opaque u64 resource id (core_constants.hpp:71)
//   core::TextureHandle        — using = core::Handle (core_constants.hpp:109)
//   core::RenderTargetAttachment::handle is core::Handle (core_constants.hpp:872)
using TextureHandle = core::TextureHandle;
using ResourceHandle = core::Handle;

// design.md §3.1 — pass kinds the builder emits.
enum class PassKind : u8 {
  Render,
  Compute,
  Blit,
  Present,
  Sync,
};

// design.md §3.1 AttachmentSet. Color count is bounded by the D3D9 backend
// render-target cap (core::kMaxRenderTargets = 4); the array is sized to the
// spec's 8 so a wider Metal target set never under-sizes the field. depth is
// the depth/stencil attachment handle.
struct AttachmentSet {
  std::array<TextureHandle, 8> color{};
  TextureHandle depth{};
  u32 color_count = 0;

  friend bool operator==(const AttachmentSet&, const AttachmentSet&) = default;
};

// L1 lightweight draw reference (design.md §3.1 DrawRange + L1 scope note).
//
// `first`/`count` index into FrameGraph::draws. Each entry of FrameGraph::draws
// is a DrawRef into the source core::ChunkSlot: `command_index` selects the
// MetalCommandHeader (a DrawRun), and `param_first`/`param_count` are the
// draw-call ordinal sub-range within that draw run (matching
// DrawRunCommandRecord::firstParam / paramCount). The linearizer (B9) hands
// these back to the traditional path, which reconstructs full draw state from
// the chunk slot — no decoded geometry is duplicated into the graph.
struct DrawRange {
  u32 first = 0;
  u32 count = 0;

  friend bool operator==(const DrawRange&, const DrawRange&) = default;
};

struct DrawRef {
  u32 command_index = 0;  // index into ChunkSlot::commandHeaders (a DrawRun)
  u32 param_first = 0;    // first draw-call ordinal within the draw run
  u32 param_count = 0;    // draw-call ordinals covered by this ref

  friend bool operator==(const DrawRef&, const DrawRef&) = default;
};

// design.md §3.5 — load/store action selected by the optimizer (loadstore.cpp,
// §5.5). B1 only declares the storage; the optimizer populates it.
enum class LoadAction : u8 {
  DontCare,
  Load,
  Clear,
};

enum class StoreAction : u8 {
  DontCare,
  Store,
};

struct LoadStorePolicy {
  std::array<LoadAction, 8> color_load{};
  std::array<StoreAction, 8> color_store{};
  LoadAction depth_load = LoadAction::DontCare;
  StoreAction depth_store = StoreAction::DontCare;

  friend bool operator==(const LoadStorePolicy&, const LoadStorePolicy&) = default;
};

// design.md §3.1 PassNode::flags + §5.1 DCE gates that read pass flags.
struct PassFlags {
  bool contains_lock = false;             // forces flush boundary (R-BACK-32.4)
  bool contains_occlusion_query = false;  // DCE-protected (§5.1)
  bool contains_event_query = false;      // DCE-protected (§5.1)
  bool dead = false;                      // set by DCE; dropped from linear order
  bool debug_marker = false;              // SetMarker annotation; optimizer-ignored

  friend bool operator==(const PassFlags&, const PassFlags&) = default;
};

// design.md §3.1 PassNode.
struct PassNode {
  PassKind kind = PassKind::Render;
  AttachmentSet targets{};
  DrawRange draws{};            // indices into FrameGraph::draws
  u64 state_profile = 0;        // dominant PSO/state hash (StateProfile); B2 fills
  LoadStorePolicy load_store{}; // updated by optimizer (loadstore.cpp)
  PassFlags flags{};

  friend bool operator==(const PassNode&, const PassNode&) = default;
};

// design.md §3.2 ResidencyClass. Default Persistent (memoryless promotion is a
// later optimizer decision, §5.3).
enum class ResidencyClass : u8 {
  Persistent,
  MemorylessCandidate,
  Memoryless,
};

// design.md §3.2 AccessLog access_kind / stage enumerations. Kept as u8 fields
// in AccessLog (matching the spec layout) with named constants for readability.
enum class AccessKind : u8 {
  Read,
  Write,
  ReadWrite,
  Preserve,
  Clear,
};

enum class AccessStage : u8 {
  Vertex,
  Fragment,
  Compute,
  Copy,
};

// design.md §3.2 AccessLog.
struct AccessLog {
  u32 pass_index = 0;
  u8 access_kind = static_cast<u8>(AccessKind::Read);
  u8 stage = static_cast<u8>(AccessStage::Fragment);

  friend bool operator==(const AccessLog&, const AccessLog&) = default;
};

// design.md §3.2 ResourceNode classifier_flags bits (lock/readback/cross_frame).
struct ResourceClassifierFlags {
  bool lock_seen = false;
  bool readback_seen = false;
  bool cross_frame_seen = false;

  friend bool operator==(const ResourceClassifierFlags&, const ResourceClassifierFlags&) = default;
};

// design.md §3.2 ResourceNode.
struct ResourceNode {
  ResourceHandle handle{};
  std::vector<AccessLog> accesses;  // chronological; per-frame build scratch
  u32 first_use_pass = 0;
  u32 last_use_pass = 0;
  ResidencyClass residency = ResidencyClass::Persistent;
  ResourceClassifierFlags classifier_flags{};

  friend bool operator==(const ResourceNode&, const ResourceNode&) = default;
};

// design.md §3.4 Edge (src_pass, dst_pass, resource). Producer→consumer
// dependency, inferred in §4.2 from access logs.
struct Edge {
  u32 src_pass = 0;
  u32 dst_pass = 0;
  ResourceHandle resource{};

  friend bool operator==(const Edge&, const Edge&) = default;
};

// design.md §3.4 FrameGraph container. Rebuilt every frame (only the
// memoryless ResourceNode observation pool carries forward, handled elsewhere).
struct FrameGraph {
  std::vector<PassNode> passes;
  std::vector<ResourceNode> resources;
  std::vector<Edge> edges;     // (src_pass, dst_pass, resource)
  std::vector<DrawRef> draws;  // L1 lightweight refs into the source ChunkSlot
  u64 frame_id = 0;
  bool flush_boundary = false; // R-BACK-32.4

  void reset() noexcept {
    passes.clear();
    resources.clear();
    edges.clear();
    draws.clear();
    frame_id = 0;
    flush_boundary = false;
  }
};

// Trivial helpers (no clock / thread / RNG — determinism, R-BACK-32.2).

// Find the ResourceNode index for a handle, or return resources.size() if
// absent. Linear scan: the per-chunk resource set is small build scratch.
inline std::size_t findResourceIndex(const FrameGraph& graph,
                                     ResourceHandle handle) noexcept {
  for (std::size_t i = 0; i < graph.resources.size(); ++i) {
    if (graph.resources[i].handle == handle) {
      return i;
    }
  }
  return graph.resources.size();
}

// Append an access to a resource's chronological log (builder/B2 helper).
inline void recordAccess(ResourceNode& node, u32 pass_index, AccessKind kind,
                         AccessStage stage) {
  node.accesses.push_back(AccessLog{
      .pass_index = pass_index,
      .access_kind = static_cast<u8>(kind),
      .stage = static_cast<u8>(stage),
  });
}

}  // namespace dxmt9::framegraph
