// Pure-data spec for the Frame Graph DAG debug export (Task B10, L1).
//
// Spec: specs/d3d9-renderer/spec.md §3.5, requirements.md R-BACK-39.7.
//
// Device-free: no Metal, no ChunkSlot encode. Builds a small hand FrameGraph
// (like fg_dag_spec.cpp) including an A->B->A attachment re-entry, then asserts
// the JSON / mermaid serializers and the env-format resolver. File writing is
// NOT exercised against the real filesystem here (writeDagDump is left covered
// by inspection / B12 integration) — see the note in main().

#include "../../../src/dxmt9/framegraph/fg_debug_export.hpp"
#include "../../../src/dxmt9/framegraph/fg_dag.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::framegraph;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

// Count non-overlapping occurrences of `needle` in `haystack`.
std::size_t countOccurrences(const std::string& haystack,
                             std::string_view needle) {
  if (needle.empty()) return 0;
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

// Build a graph with an A -> B -> A attachment re-entry:
//   pass0 (Render rt0,ds), pass1 (Render other), pass2 (Render rt0,ds again).
// rt0 is written by pass0, read by pass2 (the re-entry); edges carry rt0 and ds
// from pass0 into pass2, so the re-entry shows as two edges sharing a later
// consumer pass (spec.md §3.5).
FrameGraph buildReentrySample() {
  FrameGraph graph;
  graph.frame_id = 60;

  const ResourceHandle rt0{0xAA};
  const ResourceHandle ds{0xBB};
  const ResourceHandle rtB{0xCC};

  PassNode p0{};
  p0.kind = PassKind::Render;
  p0.targets.color[0] = TextureHandle{rt0.value};
  p0.targets.depth = TextureHandle{ds.value};
  p0.targets.color_count = 1;
  p0.draws = DrawRange{.first = 0, .count = 187};
  p0.state_profile = 0x1111;
  p0.load_store.color_load[0] = LoadAction::Clear;
  p0.load_store.color_store[0] = StoreAction::Store;
  p0.load_store.depth_load = LoadAction::Clear;
  p0.load_store.depth_store = StoreAction::Store;
  graph.passes.push_back(p0);

  PassNode p1{};
  p1.kind = PassKind::Render;
  p1.targets.color[0] = TextureHandle{rtB.value};
  p1.targets.color_count = 1;
  p1.draws = DrawRange{.first = 187, .count = 20};
  p1.state_profile = 0x2222;
  graph.passes.push_back(p1);

  PassNode p2{};  // re-entry: same attachments as p0
  p2.kind = PassKind::Render;
  p2.targets = p0.targets;
  p2.draws = DrawRange{.first = 240, .count = 20};
  p2.state_profile = 0x3333;
  p2.load_store.color_load[0] = LoadAction::Load;
  p2.load_store.color_store[0] = StoreAction::Store;
  graph.passes.push_back(p2);

  graph.draws.push_back(DrawRef{.command_index = 0, .param_first = 0, .param_count = 187});
  graph.draws.push_back(DrawRef{.command_index = 1, .param_first = 0, .param_count = 20});
  graph.draws.push_back(DrawRef{.command_index = 2, .param_first = 0, .param_count = 20});

  // Resource rt0: written by p0, read by p2 (re-entry).
  ResourceNode nrt0{};
  nrt0.handle = rt0;
  recordAccess(nrt0, 0, AccessKind::Write, AccessStage::Fragment);
  recordAccess(nrt0, 2, AccessKind::Read, AccessStage::Fragment);
  nrt0.first_use_pass = 0;
  nrt0.last_use_pass = 2;
  graph.resources.push_back(std::move(nrt0));

  // Resource ds: written by p0, re-used by p2.
  ResourceNode nds{};
  nds.handle = ds;
  recordAccess(nds, 0, AccessKind::Write, AccessStage::Fragment);
  recordAccess(nds, 2, AccessKind::Write, AccessStage::Fragment);
  nds.first_use_pass = 0;
  nds.last_use_pass = 2;
  graph.resources.push_back(std::move(nds));

  // Resource rtB: pass1 only.
  ResourceNode nrtB{};
  nrtB.handle = rtB;
  recordAccess(nrtB, 1, AccessKind::Write, AccessStage::Fragment);
  nrtB.first_use_pass = 1;
  nrtB.last_use_pass = 1;
  graph.resources.push_back(std::move(nrtB));

  // Re-entry edges: pass0 produces rt0 and ds, both consumed by pass2.
  graph.edges.push_back(Edge{.src_pass = 0, .dst_pass = 2, .resource = rt0});
  graph.edges.push_back(Edge{.src_pass = 0, .dst_pass = 2, .resource = ds});

  return graph;
}

void testJsonShape() {
  const FrameGraph graph = buildReentrySample();
  const std::string json = serializeDagJson(graph, 1422, "post-opt");

  // Top-level framing keys (spec.md §3.5).
  check(contains(json, "\"frame_id\": 60"), "json carries frame_id");
  check(contains(json, "\"chunk_seq_id\": 1422"), "json carries chunk_seq_id");
  check(contains(json, "\"stage\": \"post-opt\""), "json carries stage");
  check(contains(json, "\"passes\""), "json has passes key");
  check(contains(json, "\"resources\""), "json has resources key");
  check(contains(json, "\"edges\""), "json has edges key");

  // Pass fields.
  check(contains(json, "\"kind\": \"Render\""), "json pass kind");
  check(contains(json, "\"draws\": { \"first\": 0, \"count\": 187 }"),
        "json pass draw range");
  check(contains(json, "\"state_profile\": \"0x1111\""), "json pass state profile hex");
  check(contains(json, "\"load_store\""), "json pass load/store");
  check(contains(json, "Clear/Store"), "json load/store pair string");

  // Handle hex formatting.
  check(contains(json, "0xaa"), "json rt0 handle hex");
  check(contains(json, "0xbb"), "json ds handle hex");

  // Resource access log entry (chronological).
  check(contains(json, "\"accesses\""), "json resource access key");
  check(contains(json,
                 "{ \"pass\": 0, \"kind\": \"write\", \"stage\": \"fragment\" }"),
        "json access log write entry");
  check(contains(json,
                 "{ \"pass\": 2, \"kind\": \"read\", \"stage\": \"fragment\" }"),
        "json access log read (re-entry) entry");
  check(contains(json, "\"residency\": \"Persistent\""), "json residency class");

  // Re-entry edges: two edges sharing src_pass 0 -> dst_pass 2.
  check(contains(json,
                 "{ \"src_pass\": 0, \"dst_pass\": 2, \"resource\": \"0xaa\" }"),
        "json re-entry edge rt0");
  check(contains(json,
                 "{ \"src_pass\": 0, \"dst_pass\": 2, \"resource\": \"0xbb\" }"),
        "json re-entry edge ds");
  check(countOccurrences(json, "\"dst_pass\": 2") == 2,
        "json has exactly two edges into pass 2 (re-entry)");
}

void testMermaidShape() {
  const FrameGraph graph = buildReentrySample();
  const std::string mermaid = serializeDagMermaid(graph);

  check(mermaid.rfind("flowchart TD", 0) == 0, "mermaid starts with flowchart TD");

  // One node per pass.
  check(contains(mermaid, "P0[\""), "mermaid node P0");
  check(contains(mermaid, "P1[\""), "mermaid node P1");
  check(contains(mermaid, "P2[\""), "mermaid node P2");
  check(countOccurrences(mermaid, "[\"pass") == 3, "mermaid has exactly 3 pass nodes");

  // Pass label content (kind + draw range).
  check(contains(mermaid, "pass0 Render"), "mermaid pass0 label");
  check(contains(mermaid, "draws 0..187"), "mermaid pass0 draw range label");

  // Re-entry: two edges into P2 sharing different resources.
  check(contains(mermaid, "P0 -->|\"0xaa\"| P2"), "mermaid re-entry edge rt0 into P2");
  check(contains(mermaid, "P0 -->|\"0xbb\"| P2"), "mermaid re-entry edge ds into P2");
  check(countOccurrences(mermaid, "| P2\n") == 2,
        "mermaid has exactly two edges into P2 (re-entry)");
}

void testDotShape() {
  const FrameGraph graph = buildReentrySample();
  const std::string dot = serializeDagDot(graph);

  check(dot.rfind("digraph FrameGraph", 0) == 0, "dot starts with digraph");
  check(contains(dot, "P0 [label="), "dot node P0");
  check(contains(dot, "P0 -> P2 [label=\"0xaa\"]"), "dot re-entry edge rt0");
  check(contains(dot, "P0 -> P2 [label=\"0xbb\"]"), "dot re-entry edge ds");
}

void testSharedSnapshot() {
  // Mermaid and dot must consume the same snapshot field-walk as JSON; verify
  // they agree on node/edge content derived from one buildSnapshot.
  const FrameGraph graph = buildReentrySample();
  const DagSnapshot snap = buildSnapshot(graph, 1422, "post-opt");

  check(snap.passes.size() == 3, "snapshot has 3 passes");
  check(snap.resources.size() == 3, "snapshot has 3 resources");
  check(snap.edges.size() == 2, "snapshot has 2 edges");
  check(snap.passes[0].color.size() == 1, "snapshot pass0 has 1 color attachment");
  check(snap.passes[0].has_depth, "snapshot pass0 has depth");
  check(snap.resources[0].accesses.size() == 2, "snapshot rt0 has 2 accesses");

  // Snapshot overloads and FrameGraph overloads must agree byte-for-byte.
  check(serializeDagJson(snap) == serializeDagJson(graph, 1422, "post-opt"),
        "json snapshot/graph overloads agree");
  check(serializeDagMermaid(snap) == serializeDagMermaid(graph),
        "mermaid snapshot/graph overloads agree");
  check(serializeDagDot(snap) == serializeDagDot(graph),
        "dot snapshot/graph overloads agree");
}

void testResolveDumpFormats() {
  using F = DumpFormat;

  const auto def = resolveDumpFormats(nullptr);
  check(def.size() == 1 && def[0] == F::Json, "default (nullptr) is {json}");

  const auto empty = resolveDumpFormats("");
  check(empty.size() == 1 && empty[0] == F::Json, "empty env is {json}");

  const auto both = resolveDumpFormats("json,mermaid");
  check(both.size() == 2 && both[0] == F::Json && both[1] == F::Mermaid,
        "'json,mermaid' includes both in order");

  const auto withBogus = resolveDumpFormats("json,bogus,dot");
  check(withBogus.size() == 2 && withBogus[0] == F::Json && withBogus[1] == F::Dot,
        "'json,bogus,dot' == {json,dot} (unknown ignored)");

  const auto allBogus = resolveDumpFormats("nope,also-nope");
  check(allBogus.size() == 1 && allBogus[0] == F::Json,
        "all-unknown env falls back to {json}");

  const auto dups = resolveDumpFormats("dot,dot,json,dot");
  check(dups.size() == 2 && dups[0] == F::Dot && dups[1] == F::Json,
        "duplicate tokens de-dup, first-seen order");
}

void testDeterminism() {
  const FrameGraph graph = buildReentrySample();
  check(serializeDagJson(graph, 1422, "post-opt") ==
            serializeDagJson(graph, 1422, "post-opt"),
        "deterministic: json identical across two serializations");
  check(serializeDagMermaid(graph) == serializeDagMermaid(graph),
        "deterministic: mermaid identical across two serializations");
  check(serializeDagDot(graph) == serializeDagDot(graph),
        "deterministic: dot identical across two serializations");
}

}  // namespace

int main() {
  try {
    testJsonShape();
    testMermaidShape();
    testDotShape();
    testSharedSnapshot();
    testResolveDumpFormats();
    testDeterminism();
    // NOTE: writeDagDump is intentionally not exercised against the real
    // filesystem here to avoid polluting the repo. Its file-naming + format
    // dispatch is covered by inspection; the live pre-opt/post-opt call sites
    // land in B12.
  } catch (const TestFailure& failure) {
    std::cerr << "fg_debug_export_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "fg_debug_export_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }
  std::cout << "fg_debug_export_spec passed\n";
  return 0;
}
