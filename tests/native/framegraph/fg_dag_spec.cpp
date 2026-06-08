// Pure-data spec for the Frame Graph DAG structures declared in
// src/dxmt9/framegraph/fg_dag.hpp (Task B1, L1).
//
// Device-free: no Metal, no ChunkSlot encode. Asserts the structural shape,
// default values (ResidencyClass::Persistent), DrawRange/AttachmentSet
// behavior, trivial-copyability of the POD-shaped records, and a determinism
// sanity check (building the same graph twice yields equal contents).

#include "../../../src/dxmt9/framegraph/fg_dag.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using namespace dxmt9::framegraph;  // brings in u8/u32/u64 aliases too

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

// Build a small graph: 2 render passes + 1 present pass, two resources with
// access logs, and one producer->consumer edge. Deterministic input only.
FrameGraph buildSample() {
  FrameGraph graph;
  graph.frame_id = 60;
  graph.flush_boundary = false;

  const ResourceHandle rt0{0xAA};
  const ResourceHandle ds{0xBB};

  // Pass 0: render to rt0+ds, draws 0..2 of FrameGraph::draws.
  PassNode p0{};
  p0.kind = PassKind::Render;
  p0.targets.color[0] = TextureHandle{rt0.value};
  p0.targets.depth = TextureHandle{ds.value};
  p0.targets.color_count = 1;
  p0.draws = DrawRange{.first = 0, .count = 2};
  p0.state_profile = 0x1111;
  graph.passes.push_back(p0);

  // Pass 1: render to the same attachments, draws 2..3.
  PassNode p1{};
  p1.kind = PassKind::Render;
  p1.targets = p0.targets;  // re-entry into rt0/ds
  p1.draws = DrawRange{.first = 2, .count = 1};
  p1.state_profile = 0x2222;
  graph.passes.push_back(p1);

  // Pass 2: present.
  PassNode p2{};
  p2.kind = PassKind::Present;
  graph.passes.push_back(p2);

  // Draws: 3 lightweight refs into the source chunk slot.
  graph.draws.push_back(DrawRef{.command_index = 0, .param_first = 0, .param_count = 1});
  graph.draws.push_back(DrawRef{.command_index = 0, .param_first = 1, .param_count = 1});
  graph.draws.push_back(DrawRef{.command_index = 1, .param_first = 0, .param_count = 1});

  // Resource rt0: written by pass 0, read by pass 1.
  ResourceNode nrt0{};
  nrt0.handle = rt0;
  recordAccess(nrt0, 0, AccessKind::Write, AccessStage::Fragment);
  recordAccess(nrt0, 1, AccessKind::Read, AccessStage::Fragment);
  nrt0.first_use_pass = 0;
  nrt0.last_use_pass = 1;
  graph.resources.push_back(std::move(nrt0));

  // Resource ds: written by pass 0, write-only.
  ResourceNode nds{};
  nds.handle = ds;
  recordAccess(nds, 0, AccessKind::Write, AccessStage::Fragment);
  nds.first_use_pass = 0;
  nds.last_use_pass = 0;
  graph.resources.push_back(std::move(nds));

  // Edge: pass 0 produces rt0, pass 1 consumes it.
  graph.edges.push_back(Edge{.src_pass = 0, .dst_pass = 1, .resource = rt0});

  return graph;
}

void testDefaults() {
  PassNode pass{};
  check(pass.kind == PassKind::Render, "PassNode defaults to Render kind");
  check(pass.state_profile == 0, "PassNode state_profile defaults to 0");
  check(pass.draws == DrawRange{}, "PassNode draws default to empty range");
  check(!pass.flags.dead, "PassNode is not dead by default");

  ResourceNode node{};
  check(node.residency == ResidencyClass::Persistent,
        "ResourceNode defaults to Persistent residency");
  check(node.accesses.empty(), "ResourceNode access log defaults empty");
  check(!node.classifier_flags.lock_seen, "classifier flags default clear");

  FrameGraph graph{};
  check(graph.passes.empty() && graph.resources.empty() && graph.edges.empty() &&
            graph.draws.empty(),
        "FrameGraph defaults to empty containers");
  check(graph.frame_id == 0 && !graph.flush_boundary,
        "FrameGraph frame_id/flush_boundary default to 0/false");
}

void testDrawRangeAndAttachmentSet() {
  DrawRange r{.first = 5, .count = 3};
  check(r.first == 5 && r.count == 3, "DrawRange holds first/count");
  check(!(r == DrawRange{}), "DrawRange compares unequal to default");

  AttachmentSet a{};
  a.color[0] = TextureHandle{0x10};
  a.color[1] = TextureHandle{0x20};
  a.color_count = 2;
  a.depth = TextureHandle{0x30};
  check(a.color_count == 2, "AttachmentSet tracks color_count");
  check(a.color[0].value == 0x10 && a.color[1].value == 0x20,
        "AttachmentSet stores color handles");
  check(a.depth.value == 0x30, "AttachmentSet stores depth handle");

  AttachmentSet b = a;
  check(a == b, "AttachmentSet copy compares equal");
  b.color_count = 1;
  check(!(a == b), "AttachmentSet detects color_count divergence");
}

void testGraphContents() {
  const FrameGraph graph = buildSample();

  check(graph.passes.size() == 3, "sample graph has 3 passes");
  check(graph.resources.size() == 2, "sample graph has 2 resources");
  check(graph.edges.size() == 1, "sample graph has 1 edge");
  check(graph.draws.size() == 3, "sample graph has 3 draw refs");
  check(graph.frame_id == 60, "sample graph frame_id preserved");

  check(graph.passes[0].kind == PassKind::Render, "pass 0 is Render");
  check(graph.passes[2].kind == PassKind::Present, "pass 2 is Present");
  check(graph.passes[0].draws == DrawRange{0, 2}, "pass 0 draw range");
  check(graph.passes[1].draws == DrawRange{2, 1}, "pass 1 draw range");

  // L1 lightweight draw ref shape.
  check(graph.draws[2].command_index == 1 && graph.draws[2].param_first == 0,
        "draw ref 2 points at command 1");

  // Resource lookup + access log.
  const ResourceHandle rt0{0xAA};
  const std::size_t idx = findResourceIndex(graph, rt0);
  check(idx == 0, "findResourceIndex resolves rt0 to index 0");
  check(graph.resources[idx].accesses.size() == 2, "rt0 has 2 accesses");
  check(graph.resources[idx].accesses[0].access_kind ==
            static_cast<u8>(AccessKind::Write),
        "rt0 first access is a write");
  check(graph.resources[idx].accesses[1].pass_index == 1,
        "rt0 second access is from pass 1");

  check(findResourceIndex(graph, ResourceHandle{0xDEAD}) == graph.resources.size(),
        "findResourceIndex returns size() for unknown handle");

  // Edge wiring.
  check(graph.edges[0].src_pass == 0 && graph.edges[0].dst_pass == 1,
        "edge connects pass 0 -> pass 1");
  check(graph.edges[0].resource == rt0, "edge carries the rt0 resource");
}

void testTriviallyCopyable() {
  // The POD-shaped records (no std::vector members) must stay trivially
  // copyable so they can live in arenas / be memcpy-relocated.
  static_assert(std::is_trivially_copyable_v<PassNode>,
                "PassNode must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<AttachmentSet>,
                "AttachmentSet must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<DrawRange>,
                "DrawRange must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<DrawRef>,
                "DrawRef must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<Edge>, "Edge must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<AccessLog>,
                "AccessLog must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<LoadStorePolicy>,
                "LoadStorePolicy must be trivially copyable");
  // ResourceNode owns a vector, so it is intentionally NOT trivially copyable.
  static_assert(!std::is_trivially_copyable_v<ResourceNode>,
                "ResourceNode owns an access-log vector (not trivially copyable)");
}

void testDeterminism() {
  const FrameGraph a = buildSample();
  const FrameGraph b = buildSample();

  check(a.passes == b.passes, "deterministic: passes equal across builds");
  check(a.resources == b.resources, "deterministic: resources equal across builds");
  check(a.edges == b.edges, "deterministic: edges equal across builds");
  check(a.draws == b.draws, "deterministic: draws equal across builds");
  check(a.frame_id == b.frame_id && a.flush_boundary == b.flush_boundary,
        "deterministic: frame scalars equal across builds");
}

void testReset() {
  FrameGraph graph = buildSample();
  graph.reset();
  check(graph.passes.empty() && graph.resources.empty() && graph.edges.empty() &&
            graph.draws.empty(),
        "reset clears all containers");
  check(graph.frame_id == 0 && !graph.flush_boundary,
        "reset clears frame scalars");
}

}  // namespace

int main() {
  try {
    testDefaults();
    testDrawRangeAndAttachmentSet();
    testGraphContents();
    testTriviallyCopyable();
    testDeterminism();
    testReset();
  } catch (const TestFailure& failure) {
    std::cerr << "fg_dag_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "fg_dag_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }
  std::cout << "fg_dag_spec passed\n";
  return 0;
}
