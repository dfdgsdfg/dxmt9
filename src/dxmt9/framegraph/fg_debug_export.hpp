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

struct SnapshotPass {
  u32 index = 0;
  PassKind kind = PassKind::Render;
  std::vector<ResourceHandle> color;  // active color attachments (color_count)
  ResourceHandle depth{};
  bool has_depth = false;
  DrawRange draws{};
  u64 state_profile = 0;
  LoadStorePolicy load_store{};
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
DagSnapshot buildSnapshot(const FrameGraph& fg, std::uint64_t chunk_seq_id,
                          const char* stage);

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
std::string serializeDagJson(const FrameGraph& fg, std::uint64_t chunk_seq_id,
                             const char* stage);

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

// True if `slot` contains a Present command (core::MetalCommandKind::Present in
// its commandHeaders). A chunk that contains a Present is the LAST chunk of its
// inter-present frame, so the observe-path frame counter advances after it.
// Pure/cheap: a single linear scan of slot.commandHeaders, no payload deref.
bool chunkContainsPresent(const core::ChunkSlot& slot);

// Side-effect-neutral file dump (R-BACK-39.7). If the dump dir is set, writes
// `dag-frame<frameId>-chunk<seqId>-<stage>.{json,dot,mermaid}` per selected
// format. Reads only `fg`; writes only files. Never throws or fails a render —
// an unwritable dir logs ONE warning (util Warn, "dxmt9-renderer") and skips.
void writeDagDump(const FrameGraph& fg, std::uint64_t frame_id,
                  std::uint64_t chunk_seq_id, const char* stage);

}  // namespace dxmt9::framegraph
