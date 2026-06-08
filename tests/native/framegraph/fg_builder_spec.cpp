// Device-free spec for the Frame Graph builder (Task B2, L1).
//
// src/dxmt9/framegraph/fg_builder.cpp builds a framegraph::FrameGraph from one
// core::ChunkSlot via a single forward pass. This spec constructs synthetic
// ChunkSlots directly (populating the SoA vectors / public append helpers — no
// Metal, no device) and asserts the produced DAG's pass count, per-pass
// attachment set + draw range, resource access logs, inferred cross-pass
// edges, and determinism (build twice → equal).
//
// Fixture style mirrors tests/native/backend/render_pass_actions_spec.cpp,
// which also builds ChunkSlot SoA records by hand.

#include "../../../src/dxmt9/framegraph/fg_builder.hpp"
#include "../../../src/dxmt9/framegraph/fg_dag.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::core::ChunkSlot;
using dxmt9::core::ClearDesc;
using dxmt9::core::CommandPayloadIndex;
using dxmt9::core::DrawDebugSnapshot;
using dxmt9::core::DrawRunCommandRecord;
using dxmt9::core::DrawShaderLayoutContext;
using dxmt9::core::FlatDrawStateRecord;
using dxmt9::core::Handle;
using dxmt9::core::MetalCommandHeader;
using dxmt9::core::MetalCommandKind;

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

// --- ChunkSlot SoA fixture helpers ----------------------------------------

// Append a DrawRun with the given color0 + depth attachments, an optional
// sampled (read) texture on stage 0, and a draw-call ordinal count. Matches
// the SoA append render_pass_actions_spec uses, but lets us set paramCount so
// DrawRef.param_count can be asserted.
void appendDrawRun(ChunkSlot& slot, Handle color0, Handle depth,
                   Handle sampled = {}, std::uint32_t paramCount = 1u) {
  FlatDrawStateRecord hot{};
  hot.colorAttachments[0].handle = color0;
  hot.depthStencil.handle = depth;
  if (color0.value != 0) {
    hot.renderTargetMask = 1u;
  }
  if (sampled.value != 0) {
    hot.textures[0] = sampled;
    hot.textureMask = 1u;
  }
  const auto stateIndex = static_cast<std::uint32_t>(slot.drawHotStates.size());
  slot.drawHotStates.push_back(hot);
  slot.drawShaderLayouts.push_back(DrawShaderLayoutContext{});
  slot.drawDebugSnapshots.push_back(DrawDebugSnapshot{});

  const auto recordIndex =
      static_cast<std::uint32_t>(slot.drawRunRecords.size());
  const auto firstParam = static_cast<std::uint32_t>(slot.drawParams.size());
  for (std::uint32_t i = 0; i < paramCount; ++i) {
    slot.drawParams.push_back(dxmt9::core::DrawParam{});
  }
  DrawRunCommandRecord record{};
  record.stateIndex = stateIndex;
  record.firstParam = firstParam;
  record.paramCount = paramCount;
  slot.drawRunRecords.push_back(record);
  slot.commandHeaders.push_back(MetalCommandHeader{
      .kind = MetalCommandKind::DrawRun,
      .payloadIndex = CommandPayloadIndex::fromU32(recordIndex),
  });
}

void appendClearColor(ChunkSlot& slot, Handle color0, Handle depth = {}) {
  ClearDesc desc{};
  desc.colorAttachments[0].handle = color0;
  desc.clearColor = true;
  if (depth.value != 0) {
    desc.depthStencil.handle = depth;
    desc.clearDepth = true;
  }
  slot.appendClear(desc);
}

void appendPresent(ChunkSlot& slot, Handle source) {
  slot.appendPresent(dxmt9::core::SwapDesc{}, source);
}

void appendReadback(ChunkSlot& slot, Handle source) {
  dxmt9::core::ReadbackDesc desc{};
  desc.source = source;
  slot.appendReadback(desc);
}

AccessKind accessKindOf(const AccessLog& log) {
  return static_cast<AccessKind>(log.access_kind);
}

// --- Tests -----------------------------------------------------------------

// clear(rt0/ds) -> 2 draws to rt0/ds -> set rt1 (draw) -> present.
// The fixture's RT change is expressed as a draw whose attachment set differs,
// which is exactly how the imported ChunkSlot carries an RT switch (per-draw
// FlatDrawStateRecord hot state, design.md §2.1 / §4.1).
ChunkSlot buildScenario() {
  ChunkSlot slot;
  const Handle rt0{0xA000u};
  const Handle ds{0xD000u};
  const Handle rt1{0xB000u};

  appendClearColor(slot, rt0, ds);  // command 0
  appendDrawRun(slot, rt0, ds, /*sampled=*/{}, /*paramCount=*/3);  // command 1
  appendDrawRun(slot, rt0, ds, /*sampled=*/{}, /*paramCount=*/2);  // command 2
  appendDrawRun(slot, rt1, {}, /*sampled=*/rt0, /*paramCount=*/1); // command 3
  appendPresent(slot, rt1);  // command 4
  return slot;
}

void testPassStructure() {
  const ChunkSlot slot = buildScenario();
  const FrameGraph graph = buildFrameGraph(slot, /*frame_id=*/60);

  check(graph.frame_id == 60, "frame_id is stamped onto the graph");

  // Pass 0: clear(rt0/ds) opens a render pass; the two matching-target draws
  // continue it. Pass 1: rt1 draw (different attachment set). Pass 2: present.
  check(graph.passes.size() == 3,
        "clear+2 draws (same RT) merge into one pass; RT switch opens a "
        "second; present is the third");

  const PassNode& p0 = graph.passes[0];
  check(p0.kind == PassKind::Render, "pass 0 is a render pass");
  check(p0.targets.color[0] == TextureHandle{0xA000u}, "pass 0 color0 == rt0");
  check(p0.targets.depth == TextureHandle{0xD000u}, "pass 0 depth == ds");
  check(p0.draws.first == 0 && p0.draws.count == 2,
        "pass 0 owns both rt0 draw refs");

  const PassNode& p1 = graph.passes[1];
  check(p1.kind == PassKind::Render, "pass 1 is a render pass");
  check(p1.targets.color[0] == TextureHandle{0xB000u}, "pass 1 color0 == rt1");
  check(p1.targets.depth == TextureHandle{}, "pass 1 has no depth");
  check(p1.draws.first == 2 && p1.draws.count == 1, "pass 1 owns the rt1 draw");

  const PassNode& p2 = graph.passes[2];
  check(p2.kind == PassKind::Present, "pass 2 is a present pass");
  check(graph.flush_boundary, "present marks a flush/frame boundary");
}

void testDrawRefs() {
  const ChunkSlot slot = buildScenario();
  const FrameGraph graph = buildFrameGraph(slot, 0);

  check(graph.draws.size() == 3, "three draw refs for three DrawRun commands");
  check(graph.draws[0].command_index == 1 && graph.draws[0].param_count == 3,
        "draw ref 0 -> command 1, 3 params");
  check(graph.draws[1].command_index == 2 && graph.draws[1].param_count == 2,
        "draw ref 1 -> command 2, 2 params");
  check(graph.draws[2].command_index == 3 && graph.draws[2].param_count == 1,
        "draw ref 2 -> command 3, 1 param");
}

void testResourceAccessLogs() {
  const ChunkSlot slot = buildScenario();
  const FrameGraph graph = buildFrameGraph(slot, 0);

  const std::size_t rt0_idx = findResourceIndex(graph, ResourceHandle{0xA000u});
  const std::size_t ds_idx = findResourceIndex(graph, ResourceHandle{0xD000u});
  const std::size_t rt1_idx = findResourceIndex(graph, ResourceHandle{0xB000u});
  check(rt0_idx != graph.resources.size(), "rt0 resource node exists");
  check(ds_idx != graph.resources.size(), "ds resource node exists");
  check(rt1_idx != graph.resources.size(), "rt1 resource node exists");

  const ResourceNode& rt0 = graph.resources[rt0_idx];
  // rt0: Clear (pass 0), then read as a texture in pass 1.
  check(!rt0.accesses.empty(), "rt0 has access entries");
  check(accessKindOf(rt0.accesses.front()) == AccessKind::Clear,
        "rt0's first access is the clear");
  bool rt0_read_in_pass1 = false;
  for (const auto& a : rt0.accesses) {
    if (accessKindOf(a) == AccessKind::Read && a.pass_index == 1) {
      rt0_read_in_pass1 = true;
    }
  }
  check(rt0_read_in_pass1, "rt0 is read (sampled) in pass 1");
  check(rt0.first_use_pass == 0 && rt0.last_use_pass == 1,
        "rt0 lifetime spans pass 0..1");

  const ResourceNode& rt1 = graph.resources[rt1_idx];
  check(accessKindOf(rt1.accesses.front()) == AccessKind::Write,
        "rt1's first access is a write (attachment)");
}

void testCrossPassEdge() {
  const ChunkSlot slot = buildScenario();
  const FrameGraph graph = buildFrameGraph(slot, 0);

  // rt0 is cleared/written in pass 0 and sampled in pass 1 -> read-after-write
  // edge 0 -> 1 on rt0. No self-edge for the in-pass attachment writes.
  bool found = false;
  for (const auto& e : graph.edges) {
    if (e.src_pass == 0 && e.dst_pass == 1 &&
        e.resource == ResourceHandle{0xA000u}) {
      found = true;
    }
    check(e.src_pass != e.dst_pass, "no self-edges emitted");
  }
  check(found, "RAW edge pass0 -> pass1 on rt0 inferred");
}

void testReadbackClassifier() {
  ChunkSlot slot;
  const Handle rt0{0x111u};
  appendDrawRun(slot, rt0, {});
  appendReadback(slot, rt0);

  const FrameGraph graph = buildFrameGraph(slot, 0);
  const std::size_t rt0_idx = findResourceIndex(graph, ResourceHandle{0x111u});
  check(rt0_idx != graph.resources.size(), "rt0 node exists");
  check(graph.resources[rt0_idx].classifier_flags.readback_seen,
        "readback sets the readback_seen classifier bit on the source");

  // Readback is its own (blit) pass after the render pass.
  check(graph.passes.size() == 2, "render pass + readback blit pass");
  check(graph.passes[1].kind == PassKind::Blit, "readback emits a Blit pass");
}

void testEmptyChunk() {
  ChunkSlot slot;
  const FrameGraph graph = buildFrameGraph(slot, 7);
  check(graph.passes.empty(), "empty chunk -> no passes");
  check(graph.resources.empty(), "empty chunk -> no resources");
  check(graph.draws.empty(), "empty chunk -> no draws");
  check(graph.frame_id == 7, "frame_id still stamped on empty graph");
}

void testDeterminism() {
  const ChunkSlot slot = buildScenario();
  const FrameGraph a = buildFrameGraph(slot, 60);
  const FrameGraph b = buildFrameGraph(slot, 60);
  check(a.passes == b.passes, "deterministic: passes equal across builds");
  check(a.resources == b.resources, "deterministic: resources equal");
  check(a.edges == b.edges, "deterministic: edges equal across builds");
  check(a.draws == b.draws, "deterministic: draws equal across builds");
  check(a.frame_id == b.frame_id && a.flush_boundary == b.flush_boundary,
        "deterministic: frame scalars equal");

  // In-place build must produce identical contents to the value-returning form.
  FrameGraph reused;
  reused.passes.push_back(PassNode{});  // dirty it first
  buildFrameGraph(slot, 60, reused);
  check(reused.passes == a.passes, "in-place build matches value build (reset)");
}

}  // namespace

int main() {
  try {
    testPassStructure();
    testDrawRefs();
    testResourceAccessLogs();
    testCrossPassEdge();
    testReadbackClassifier();
    testEmptyChunk();
    testDeterminism();
  } catch (const TestFailure& failure) {
    std::cerr << "fg_builder_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "fg_builder_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }
  std::cout << "fg_builder_spec passed\n";
  return 0;
}
