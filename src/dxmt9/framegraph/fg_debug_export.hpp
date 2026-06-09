#pragma once

// Frame Graph DAG debug export (Task B10, L1).
//
// Spec: specs/d3d9-renderer/design.md §3.5 (DAG Debug Export),
//       requirements.md R-BACK-39.7.
//
// PURE READ TRANSFORM.
//   Every serializer takes a `const framegraph::FrameGraph&` and returns owned
//   bytes. It holds no Metal, queue, cache, or pool reference and mutates no
//   shared state, so an enabled dump leaves the Metal stream byte-identical to a
//   disabled run (R-BACK-39.7 side-effect neutrality, R-BACK-39.1 parity).
//
// ONE SNAPSHOT, MANY FORMATS.
//   The JSON, Graphviz (.dot), and Mermaid renderers all consume the SAME
//   `DagSnapshot` intermediate (built once by `buildSnapshot`). No format walks
//   the live `FrameGraph` independently — the snapshot is the single field-walk
//   over passes/resources/edges, so the three text formats can never disagree
//   about which nodes/edges exist (design.md §3.5, R-BACK-39.7 "one
//   serialization pass — no format re-walks the DAG").
//
// DETERMINISM (R-BACK-32.2).
//   No clock, thread-id, or RNG. Serializing the same DAG twice yields a
//   byte-equal string.

#include "fg_dag.hpp"
#include "fg_optimizer.hpp"  // framegraph::OptimizerOptions

#include "../dxmt9_backend_types.hpp"  // core::ChunkSlot, core::MetalCommandKind

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dxmt9::framegraph {

// ---------------------------------------------------------------------------
// Shared snapshot intermediate.
//
// `buildSnapshot` performs the single field-walk over the FrameGraph; the JSON,
// dot, and mermaid renderers all read from this struct (and never re-walk the
// graph). The snapshot copies the small scalar fields the renderers need and
// flattens the per-resource access log into a renderer-friendly shape, so it is
// itself cheap, owned, and pointer-free.
// ---------------------------------------------------------------------------

struct SnapshotAccess {
  u32 pass_index = 0;
  AccessKind kind = AccessKind::Read;
  AccessStage stage = AccessStage::Fragment;
};

// Optional per-draw D3D9 detail (DEBUG-ONLY; DXMT9_RENDERER_DUMP_DAG_DRAWS).
//
// This is L1-debug only: it carries a BOUNDED, cheaply-resolved per-draw
// summary read out of the source core::ChunkSlot hot state for the JSON dump.
// It is NOT the deferred L2 production `DrawDescriptor` (design.md §3.3), which
// carries per-draw geometry/bindings for the mesh / GPU-driven path and remains
// a separate, deferred production data structure. Field sources (resolved by
// `buildSnapshot` from `slot.drawRunCommandAt(command_index)`):
//   command_index/draw_ordinal — the DrawRef + ordinal within the draw run
//   primitive_type/primitive_count — per-ordinal core::DrawParam (drawParams[])
//   vs_hash/ps_hash — core::DrawDebugSnapshot (drawState.debug); the SAME VS/PS
//                     hashes the 3dmark05 indexed-probe CSV reports
//   texture_mask — core::FlatDrawStateRecord::textureMask (drawState.hot)
//   alpha_blend/z_enable/z_write/z_func/alpha_test/cull — core::flatStateOr on
//                     the hot FlatStateSet renderStates (RS_* ids, matching the
//                     encoder's DrawDebugRecord defaults in dxmt9_draw_encoder.mm)
//   stream0_stride — core::FlatDrawStateRecord::streamStrides[0]
struct SnapshotDraw {
  u32 command_index = 0;
  u32 draw_ordinal = 0;
  u32 primitive_type = 0;  // core::PrimitiveType enum value
  u32 primitive_count = 0;
  u64 vs_hash = 0;
  u64 ps_hash = 0;
  u32 texture_mask = 0;
  u32 alpha_blend = 0;
  u32 z_enable = 0;
  u32 z_write = 0;
  u32 z_func = 0;
  u32 alpha_test = 0;
  u32 cull = 0;
  u32 stream0_stride = 0;
};

struct SnapshotPass {
  u32 index = 0;
  PassKind kind = PassKind::Render;
  std::vector<ResourceHandle> color;  // active color attachments (color_count)
  ResourceHandle depth{};
  bool has_depth = false;
  DrawRange draws{};
  u64 state_profile = 0;
  LoadStorePolicy load_store{};
  // Filled ONLY when buildSnapshot is given a non-null slot AND
  // DXMT9_RENDERER_DUMP_DAG_DRAWS is set. Empty otherwise, so the JSON omits
  // "draws_detail" and existing golden output is unchanged.
  std::vector<SnapshotDraw> draws_detail;
};

struct SnapshotResource {
  ResourceHandle handle{};
  ResidencyClass residency = ResidencyClass::Persistent;
  u32 first_use_pass = 0;
  u32 last_use_pass = 0;
  std::vector<SnapshotAccess> accesses;  // chronological
};

struct SnapshotEdge {
  u32 src_pass = 0;
  u32 dst_pass = 0;
  ResourceHandle resource{};
};

struct DagSnapshot {
  u64 frame_id = 0;
  u64 chunk_seq_id = 0;
  std::string stage;
  std::vector<SnapshotPass> passes;
  std::vector<SnapshotResource> resources;
  std::vector<SnapshotEdge> edges;
};

// Single field-walk over the FrameGraph. `chunk_seq_id` / `stage` are
// caller-supplied (the FrameGraph carries frame_id but not the chunk seq id,
// which onChunkReady sources from its ImportContext — design.md §3.5).
//
// `slot` is the source ChunkSlot the FrameGraph's DrawRefs index into. It is
// OPTIONAL: when non-null AND DXMT9_RENDERER_DUMP_DAG_DRAWS is set, each
// SnapshotPass gets a per-draw `draws_detail` resolved from the slot's hot
// state (DEBUG-ONLY; encode-neutral — pure read). When `slot` is null or the
// flag is off, no draw detail is resolved (zero extra cost) and the JSON is
// byte-identical to the historical output. The pure FrameGraph-only overload
// passes slot=nullptr, so the device-free golden serializers are unaffected.
DagSnapshot buildSnapshot(const FrameGraph& fg, std::uint64_t chunk_seq_id,
                          const char* stage,
                          const core::ChunkSlot* slot = nullptr);

// ---------------------------------------------------------------------------
// Format serializers. The const-FrameGraph overloads build a snapshot then
// delegate to the snapshot overloads, so callers that already hold a snapshot
// (the file-writing path) serialize all formats from one walk.
// ---------------------------------------------------------------------------

std::string serializeDagJson(const DagSnapshot& snapshot);
std::string serializeDagMermaid(const DagSnapshot& snapshot);
std::string serializeDagDot(const DagSnapshot& snapshot);

// design.md §3.5 JSON object (frame_id, chunk_seq_id, stage, passes[],
// resources[] with chronological accesses[], edges[]). Handle values use hex.
// The optional `slot` enables the DEBUG-ONLY per-draw `draws_detail` extension
// (see buildSnapshot). Defaulted nullptr keeps the existing call sites and
// golden output unchanged.
std::string serializeDagJson(const FrameGraph& fg, std::uint64_t chunk_seq_id,
                             const char* stage,
                             const core::ChunkSlot* slot = nullptr);

// `flowchart TD`: one node per pass, one labeled edge per Edge (the re-entry
// case shows as two edges sharing a resource into a later pass).
std::string serializeDagMermaid(const FrameGraph& fg);

// Graphviz `digraph` equivalent of the mermaid flowchart.
std::string serializeDagDot(const FrameGraph& fg);

// ---------------------------------------------------------------------------
// File-writing side (format selection + env). The actual call sites
// (pre-opt + post-opt inside FrameGraphBackend::onChunkReady) are wired in B12.
// ---------------------------------------------------------------------------

enum class DumpFormat : std::uint8_t {
  Json,
  Dot,
  Mermaid,
};

// Pure resolver for DXMT9_RENDERER_DUMP_DAG_FORMATS (comma list). Default
// (nullptr / empty) is {Json}. Recognizes json/dot/mermaid (case-sensitive
// lowercase per spec example); unknown tokens are ignored. Duplicates are
// de-duplicated, preserving first-seen order.
std::vector<DumpFormat> resolveDumpFormats(const char* env);

// Reads DXMT9_RENDERER_DUMP_DAG once (static-const pattern). Returns the dump
// directory if set to a non-empty value, else std::nullopt.
std::optional<std::string> dumpDagDir();

// Pure resolver for DXMT9_RENDERER_DUMP_DAG_FRAME (1-based inter-present frame
// number). nullptr / empty / "0" / non-numeric all resolve to std::nullopt,
// meaning "no filter — dump every chunk's DAG" (the historical behavior; real
// apps such as 3DMark05 emit thousands of chunks/frames and should set this to
// avoid flooding the dump dir). A positive integer N selects only frame N.
// Testable without touching the environment.
std::optional<std::uint64_t> resolveDumpDagFrame(const char* env);

// Reads DXMT9_RENDERER_DUMP_DAG_FRAME once (static-const pattern; mirrors
// dumpDagDir()). std::nullopt = dump all frames (unfiltered).
std::optional<std::uint64_t> dumpDagFrame();

// Pure resolver for DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS (non-negative window
// radius R around DXMT9_RENDERER_DUMP_DAG_FRAME=N). nullptr / empty / "0" /
// non-numeric all resolve to 0 (single-frame filter — the historical default).
// A positive integer R widens the dumped set to the inclusive window
// [max(1, N-R), N+R] (low end clamped at 1 because frames are 1-based).
// Testable without touching the environment.
std::uint64_t resolveDumpDagFrameRadius(const char* env);

// Reads DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS once (static-const pattern;
// mirrors dumpDagFrame()). 0 = single-frame filter (no widening).
std::uint64_t dumpDagFrameRadius();

// Pure resolver for DXMT9_RENDERER_DUMP_DAG_OPTIMIZE (R-BACK-39.7). This is a
// DEBUG-ONLY ANALYSIS knob that OVERRIDES which optimizer passes the observer
// runs for the POST-OPT DAG snapshot only — it never touches the Metal encode
// (the DagObserver is a pure observation side-channel that does not drive
// encoding), so it cannot change rendered output. It is independent of
// DXMT9_RENDERER_FEATURES / compat_profile (which gate the production encode)
// precisely because it is analysis-only.
//
// nullptr / empty → std::nullopt, meaning "use the backend-provided options"
// (current behavior, pre-opt baseline vs the backend's post-opt). Otherwise the
// comma-separated token list is parsed into an OptimizerOptions with the named
// gated passes set: `passcoalesce`, `reorder`, `dce`, `memoryless`. Unknown
// tokens are ignored. An env that lists only unknown tokens still resolves to a
// (default-constructed, all-off) OptimizerOptions, NOT nullopt — the operator
// explicitly asked for an override, so the post-opt snapshot runs the all-off
// pipeline (lifetime + loadstore only). Testable without touching the
// environment.
std::optional<OptimizerOptions> resolveDumpDagOptimize(const char* env);

// Reads DXMT9_RENDERER_DUMP_DAG_OPTIMIZE once (static-const pattern; mirrors
// dumpDagFrame()). std::nullopt = no override (use the backend's options).
std::optional<OptimizerOptions> dumpDagOptimizeOverride();

// Pure resolver for DXMT9_RENDERER_DUMP_DAG_DRAWS (repo env-flag semantics:
// "set" = a non-empty string that is not "0"). DEBUG-ONLY opt-in for the
// per-draw `draws_detail` JSON extension. Testable without the environment.
bool resolveDumpDagDraws(const char* env);

// Reads DXMT9_RENDERER_DUMP_DAG_DRAWS once (static-const pattern; mirrors
// dumpDagDir()). false = no per-draw detail (the historical JSON shape).
bool dumpDagDraws();

// True if `slot` contains a Present command (core::MetalCommandKind::Present in
// its commandHeaders). A chunk that contains a Present is the LAST chunk of its
// inter-present frame, so the observe-path frame counter advances after it.
// Pure/cheap: a single linear scan of slot.commandHeaders, no payload deref.
bool chunkContainsPresent(const core::ChunkSlot& slot);

// Side-effect-neutral file dump (R-BACK-39.7). If the dump dir is set, writes
// `dag-frame<frameId>-chunk<seqId>-<stage>.{json,dot,mermaid}` per selected
// format. Reads only `fg` (+ optional `slot` for the per-draw JSON detail);
// writes only files. Never throws or fails a render — an unwritable dir logs
// ONE warning (util Warn, "dxmt9-renderer") and skips. `slot` is OPTIONAL: when
// non-null AND DXMT9_RENDERER_DUMP_DAG_DRAWS is set the JSON gains the
// per-pass `draws_detail` array (DEBUG-ONLY; the dot/mermaid formats are
// unaffected). Defaulted nullptr preserves the historical behavior/output.
void writeDagDump(const FrameGraph& fg, std::uint64_t frame_id,
                  std::uint64_t chunk_seq_id, const char* stage,
                  const core::ChunkSlot* slot = nullptr);

}  // namespace dxmt9::framegraph
