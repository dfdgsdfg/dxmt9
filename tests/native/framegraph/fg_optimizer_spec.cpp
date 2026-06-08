// Device-free spec for the Frame Graph optimizer pipeline (Tasks B3-B8, L1).
//
// src/dxmt9/framegraph/fg_optimizer.{hpp,cpp} + fg_optimizer/*.cpp implement six
// passes in the fixed R-BACK-32.5 order (lifetime -> passcoalesce -> memoryless
// -> dce -> reorder -> loadstore) over an in-memory framegraph::FrameGraph.
//
// This spec hand-builds FrameGraphs (and a couple via buildFrameGraph on a
// synthetic ChunkSlot, mirroring fg_builder_spec's fixture style) and asserts:
//   - lifetime first/last per resource,
//   - default OptimizerOptions{} preserves pass + draw order (parity baseline),
//   - passcoalesce merges a safe pair and refuses a dependency-blocked one,
//   - loadstore: clear->Clear load, last-write->Store, memoryless->DontCare,
//   - dce off keeps all passes; dce on drops a provably-dead pass but never a
//     Present/query pass,
//   - reorder preserves edges (no consumer before producer),
//   - memoryless classifier marks an eligible transient surface and skips
//     locked / readback / backbuffer / under-observed ones.

#include "../../../src/dxmt9/framegraph/fg_builder.hpp"
#include "../../../src/dxmt9/framegraph/fg_dag.hpp"
#include "../../../src/dxmt9/framegraph/fg_optimizer.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::framegraph;
using dxmt9::core::Handle;

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

// --- Hand-built FrameGraph helpers -----------------------------------------

AttachmentSet colorDepth(std::uint64_t color0, std::uint64_t depth) {
  AttachmentSet set{};
  set.color[0] = TextureHandle{color0};
  set.color_count = color0 ? 1u : 0u;
  set.depth = TextureHandle{depth};
  return set;
}

// Append a render PassNode with the given attachments and a single draw ref.
u32 addRenderPass(FrameGraph& g, std::uint64_t color0, std::uint64_t depth) {
  PassNode pass{};
  pass.kind = PassKind::Render;
  pass.targets = colorDepth(color0, depth);
  pass.draws.first = static_cast<u32>(g.draws.size());
  pass.draws.count = 1;
  g.draws.push_back(DrawRef{.command_index = static_cast<u32>(g.passes.size())});
  g.passes.push_back(pass);
  return static_cast<u32>(g.passes.size() - 1u);
}

ResourceNode& addResource(FrameGraph& g, std::uint64_t handle) {
  ResourceNode node{};
  node.handle = ResourceHandle{handle};
  g.resources.push_back(node);
  return g.resources.back();
}

void access(ResourceNode& node, u32 pass, AccessKind kind) {
  recordAccess(node, pass, kind, AccessStage::Fragment);
}

// --- Tests -----------------------------------------------------------------

void testLifetime() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0xD000u);  // pass 0
  addRenderPass(g, 0xB000u, 0u);       // pass 1
  addRenderPass(g, 0xC000u, 0u);       // pass 2

  ResourceNode& rt0 = addResource(g, 0xA000u);
  access(rt0, 0, AccessKind::Clear);
  access(rt0, 2, AccessKind::Read);  // spans pass 0..2

  ResourceNode& rt1 = addResource(g, 0xB000u);
  access(rt1, 1, AccessKind::Write);  // single pass

  runLifetime(g);

  check(g.resources[0].first_use_pass == 0 && g.resources[0].last_use_pass == 2,
        "rt0 lifetime 0..2");
  check(!resourceIsTransient(g.resources[0]), "rt0 is not transient (multi-pass)");
  check(g.resources[1].first_use_pass == 1 && g.resources[1].last_use_pass == 1,
        "rt1 lifetime 1..1");
  check(resourceIsTransient(g.resources[1]), "rt1 is transient (single pass)");
}

// Parity baseline (R-BACK-32.6): default options run only lifetime + loadstore,
// changing neither pass order nor draw order.
void testDefaultOptionsPreserveOrder() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0xD000u);
  addRenderPass(g, 0xB000u, 0u);
  addRenderPass(g, 0xC000u, 0u);
  ResourceNode& rt0 = addResource(g, 0xA000u);
  access(rt0, 0, AccessKind::Write);
  ResourceNode& rt1 = addResource(g, 0xB000u);
  access(rt1, 1, AccessKind::Write);

  const std::vector<PassNode> passes_before = g.passes;
  const std::vector<DrawRef> draws_before = g.draws;

  OptimizerStats stats{};
  runOptimizer(g, OptimizerOptions{}, /*observations=*/nullptr, &stats);

  check(g.passes.size() == passes_before.size(), "default: pass count unchanged");
  for (std::size_t i = 0; i < g.passes.size(); ++i) {
    check(g.passes[i].kind == passes_before[i].kind &&
              g.passes[i].targets == passes_before[i].targets &&
              g.passes[i].draws == passes_before[i].draws,
          "default: pass identity/order unchanged");
  }
  check(g.draws == draws_before, "default: draw order unchanged");
  check(stats.pass_coalesced_count == 0 && stats.dce_dropped == 0 &&
            stats.memoryless_promoted == 0,
        "default: no gated pass ran");
  // loadstore still populated actions (it always runs).
  check(g.passes[0].load_store.color_load[0] == LoadAction::Load,
        "default: loadstore still selects Load for an un-cleared first use");
}

void testLoadStoreClearAndStore() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0xD000u);  // pass 0: clears rt0, last use of rt0
  ResourceNode& rt0 = addResource(g, 0xA000u);
  access(rt0, 0, AccessKind::Clear);  // pass begins with a clear
  ResourceNode& ds = addResource(g, 0xD000u);
  access(ds, 0, AccessKind::Write);  // depth written, not cleared

  runLifetime(g);
  runLoadStore(g);

  check(g.passes[0].load_store.color_load[0] == LoadAction::Clear,
        "color cleared-first -> Clear load");
  check(g.passes[0].load_store.color_store[0] == StoreAction::Store,
        "color last-write (persistent) -> Store");
  check(g.passes[0].load_store.depth_load == LoadAction::Load,
        "depth not cleared -> Load");
  check(g.passes[0].load_store.depth_store == StoreAction::Store,
        "depth persistent last-use -> Store");
}

void testLoadStoreMemorylessDontCare() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0u);
  ResourceNode& rt0 = addResource(g, 0xA000u);
  access(rt0, 0, AccessKind::Write);
  rt0.residency = ResidencyClass::Memoryless;  // promoted by memoryless pass

  runLifetime(g);
  runLoadStore(g);

  check(g.passes[0].load_store.color_store[0] == StoreAction::DontCare,
        "memoryless last-use -> DontCare store");
}

// passcoalesce: two matching-attachment passes with an independent intervening
// pass coalesce; a dependency-blocked intervening pass does NOT.
void testPassCoalesceSafe() {
  FrameGraph g;
  // pass0 rt=A, pass1 rt=0x9000 (independent), pass2 rt=A. pass0/pass2 match.
  addRenderPass(g, 0xA000u, 0xD000u);  // 0
  addRenderPass(g, 0x9000u, 0u);       // 1 (rt=0x9000, independent)
  addRenderPass(g, 0xA000u, 0xD000u);  // 2
  // No edges: pass1 is independent of 0 and 2.
  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 0, AccessKind::Write);
  access(a, 2, AccessKind::Write);
  ResourceNode& z = addResource(g, 0x9000u);
  access(z, 1, AccessKind::Write);
  runLifetime(g);

  OptimizerStats stats{};
  runPassCoalesce(g, &stats);

  check(stats.pass_coalesced_count == 1, "one coalesce happened");
  check(g.passes.size() == 2, "3 passes -> 2 (pass0+pass2 merged)");
  // Merged pass keeps both draws in submission order.
  bool found_merged = false;
  for (const PassNode& p : g.passes) {
    if (p.kind == PassKind::Render && p.targets.color[0] == TextureHandle{0xA000u}) {
      check(p.draws.count == 2, "merged pass owns both draws");
      // Draw refs preserve submission order (cmd 0 then cmd 2).
      check(g.draws[p.draws.first].command_index == 0 &&
                g.draws[p.draws.first + 1].command_index == 2,
            "merged draws keep submission order");
      found_merged = true;
    }
  }
  check(found_merged, "merged rt=A pass present");
}

void testPassCoalesceBlockedByDependency() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0xD000u);  // 0 writes A
  addRenderPass(g, 0x9000u, 0u);       // 1 reads A, writes 0x9000
  addRenderPass(g, 0xA000u, 0xD000u);  // 2 reads 0x9000, writes A
  // Edges: 0->1 (A), 1->2 (0x9000). Pass1 both consumes the pair's product (A)
  // and produces for the pair (0x9000) -> wedged -> NOT coalescable.
  g.edges.push_back(Edge{.src_pass = 0, .dst_pass = 1, .resource = ResourceHandle{0xA000u}});
  g.edges.push_back(Edge{.src_pass = 1, .dst_pass = 2, .resource = ResourceHandle{0x9000u}});
  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 0, AccessKind::Write);
  access(a, 1, AccessKind::Read);
  access(a, 2, AccessKind::Write);
  ResourceNode& m = addResource(g, 0x9000u);
  access(m, 1, AccessKind::Write);
  access(m, 2, AccessKind::Read);
  runLifetime(g);

  OptimizerStats stats{};
  runPassCoalesce(g, &stats);

  check(stats.pass_coalesced_count == 0, "dependency-wedged pair not coalesced");
  check(g.passes.size() == 3, "all three passes survive");
}

void testDceOffKeepsAll() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0u);
  addRenderPass(g, 0xB000u, 0u);
  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 0, AccessKind::Write);  // unread, but dce off
  ResourceNode& b = addResource(g, 0xB000u);
  access(b, 1, AccessKind::Write);

  runOptimizer(g, OptimizerOptions{ /*dce default false*/ });
  for (const PassNode& p : g.passes) {
    check(!p.flags.dead, "dce off: no pass marked dead");
  }
}

void testDceOnDropsProvablyDead() {
  FrameGraph g;
  // pass0 writes A then pass1 re-clears A (full overwrite in same chunk).
  addRenderPass(g, 0xA000u, 0u);  // 0
  addRenderPass(g, 0xA000u, 0u);  // 1 clears A
  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 0, AccessKind::Write);  // dead: overwritten by pass1 clear, unread
  access(a, 1, AccessKind::Clear);
  runLifetime(g);

  OptimizerStats stats{};
  OptimizerOptions opts{};
  opts.dce = true;
  runOptimizer(g, opts, nullptr, &stats);

  check(g.passes[0].flags.dead, "pass0 write is dead (overwritten, unread)");
  check(stats.dce_dropped >= 1, "dce_dropped counted");
}

void testDceNeverDropsPresentOrQuery() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0u);  // 0: writes A, unread
  // Present pass.
  PassNode present{};
  present.kind = PassKind::Present;
  g.passes.push_back(present);  // 1
  // Query-bearing render pass writing an unread, overwritten-later resource.
  addRenderPass(g, 0xC000u, 0u);  // 2
  g.passes[2].flags.contains_occlusion_query = true;
  addRenderPass(g, 0xC000u, 0u);  // 3 clears C (full overwrite)

  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 0, AccessKind::Write);
  ResourceNode& c = addResource(g, 0xC000u);
  access(c, 2, AccessKind::Write);
  access(c, 3, AccessKind::Clear);
  c.residency = ResidencyClass::Memoryless;  // cross-chunk safe
  runLifetime(g);

  OptimizerStats stats{};
  OptimizerOptions opts{};
  opts.dce = true;
  runDce(g, &stats);

  check(!g.passes[1].flags.dead, "Present pass never dropped");
  check(!g.passes[2].flags.dead, "occlusion-query pass never dropped");
}

// reorder: a producer/consumer chain must keep the producer before the consumer
// even if submission order or cost would otherwise move them.
void testReorderPreservesEdges() {
  FrameGraph g;
  // Submission order: 0 (rt A, consumer), 1 (rt B, producer of X read by 0).
  // Edge 1->0 forces pass1 to run BEFORE pass0 after reorder.
  addRenderPass(g, 0xA000u, 0u);  // 0 reads X
  addRenderPass(g, 0xB000u, 0u);  // 1 writes X
  g.edges.push_back(Edge{.src_pass = 1, .dst_pass = 0, .resource = ResourceHandle{0x1234u}});
  ResourceNode& x = addResource(g, 0x1234u);
  access(x, 1, AccessKind::Write);
  access(x, 0, AccessKind::Read);
  runLifetime(g);

  runReorder(g);

  // After reorder the producer (rt B) must come before the consumer (rt A).
  std::size_t pos_a = g.passes.size();
  std::size_t pos_b = g.passes.size();
  for (std::size_t i = 0; i < g.passes.size(); ++i) {
    if (g.passes[i].targets.color[0] == TextureHandle{0xA000u}) pos_a = i;
    if (g.passes[i].targets.color[0] == TextureHandle{0xB000u}) pos_b = i;
  }
  check(pos_b < pos_a, "producer pass (B) reordered before consumer pass (A)");
  // Every edge still has src before dst (no consumer before producer).
  for (const Edge& e : g.edges) {
    check(e.src_pass < e.dst_pass, "edge src precedes dst after reorder");
  }
}

// reorder respects an ANTI-dependency (WAW / WAR) edge that has no RAW edge
// between the two passes. The edge points P_x -> P_y where P_x has the HIGHER
// original index, so the only way P_x can precede P_y in the output is if the
// topo-sort honoured the edge. WITHOUT the edge, both passes are roots and the
// tie-break (smallest original index first) would emit P_y before P_x, flipping
// the order -> the edge is load-bearing.
void testReorderRespectsAntiDepEdge() {
  FrameGraph g;
  // Original submission order: pass0 = P_y (rt A), pass1 = P_x (rt B).
  addRenderPass(g, 0xA000u, 0u);  // 0 = P_y : writes rt A   (later writer / re-entry)
  addRenderPass(g, 0xB000u, 0u);  // 1 = P_x : writes rt B
  // WAW/WAR-style anti-dependency: P_x (pass1) must precede P_y (pass0) on some
  // resource R. No RAW edge exists between them; this is purely the new
  // anti-dependency constraint. src=1 (higher index) -> dst=0 (lower index).
  g.edges.push_back(Edge{.src_pass = 1, .dst_pass = 0,
                         .resource = ResourceHandle{0x5555u}});
  // Resource access log consistent with the edge (R written in pass1, then a
  // later anti-conflicting access in pass0 in the desired final order). The
  // exact AccessKind does not change reorder (it consumes edges, not kinds), but
  // keep the log self-consistent.
  ResourceNode& r = addResource(g, 0x5555u);
  access(r, 1, AccessKind::Write);
  access(r, 0, AccessKind::Write);
  runLifetime(g);

  runReorder(g);

  std::size_t pos_x = g.passes.size();  // rt B
  std::size_t pos_y = g.passes.size();  // rt A
  for (std::size_t i = 0; i < g.passes.size(); ++i) {
    if (g.passes[i].targets.color[0] == TextureHandle{0xB000u}) pos_x = i;
    if (g.passes[i].targets.color[0] == TextureHandle{0xA000u}) pos_y = i;
  }
  check(pos_x != g.passes.size() && pos_y != g.passes.size(),
        "both passes present after reorder");
  check(pos_x < pos_y,
        "anti-dependency edge P_x -> P_y forced P_x before P_y, overriding the "
        "smallest-original-index tie-break that would otherwise emit P_y first");
  // The edge set is preserved and src still precedes dst after remap.
  bool edge_present = false;
  for (const Edge& e : g.edges) {
    check(e.src_pass < e.dst_pass, "anti-dep edge src precedes dst after reorder");
    if (e.resource == ResourceHandle{0x5555u}) {
      edge_present = true;
    }
  }
  check(edge_present, "anti-dependency edge survives reorder remap");
}

// passcoalesce must NOT merge a same-attachment pair when an intervening pass is
// wedged by an anti-dependency: the pair (P_a, P_c) write rt A, and the
// intervening pass P_b BOTH consumes from the pair and produces for the pair via
// anti-dependency edges, so it can move neither before P_a nor after P_c.
void testPassCoalesceBlockedByAntiDep() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0u);  // 0 = P_a : writes rt A
  addRenderPass(g, 0x9000u, 0u);  // 1 = P_b : intervening, different target
  addRenderPass(g, 0xA000u, 0u);  // 2 = P_c : re-writes rt A (matches P_a)
  // Anti-dependency wedge: P_a -> P_b (pair feeds P_b, e.g. WAR on some resource
  // S) AND P_b -> P_c (P_b feeds the pair, e.g. WAW on some resource T). P_b is
  // therefore reachable-from P_a and reaches P_c -> Move::Blocked -> not
  // coalescable. Neither edge is a RAW edge between P_a and P_c.
  g.edges.push_back(Edge{.src_pass = 0, .dst_pass = 1,
                         .resource = ResourceHandle{0x7777u}});
  g.edges.push_back(Edge{.src_pass = 1, .dst_pass = 2,
                         .resource = ResourceHandle{0x8888u}});
  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 0, AccessKind::Write);
  access(a, 2, AccessKind::Write);
  runLifetime(g);

  OptimizerStats stats{};
  runPassCoalesce(g, &stats);

  check(stats.pass_coalesced_count == 0,
        "anti-dependency-wedged intervening pass blocks coalesce");
  check(g.passes.size() == 3, "all three passes survive (no merge)");
}

void testReorderIdentityWhenNoEdges() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0u);
  addRenderPass(g, 0xB000u, 0u);
  addRenderPass(g, 0xC000u, 0u);
  ResourceNode& a = addResource(g, 0xA000u); access(a, 0, AccessKind::Write);
  ResourceNode& b = addResource(g, 0xB000u); access(b, 1, AccessKind::Write);
  ResourceNode& c = addResource(g, 0xC000u); access(c, 2, AccessKind::Write);
  runLifetime(g);
  const std::vector<PassNode> before = g.passes;
  runReorder(g);
  check(g.passes.size() == before.size(), "reorder no-edge: count stable");
  for (std::size_t i = 0; i < g.passes.size(); ++i) {
    check(g.passes[i].targets == before[i].targets,
          "reorder no-edge: identity order (deterministic tiebreak on index)");
  }
}

// memoryless classifier: eligible transient surface promoted after threshold;
// locked / readback / backbuffer / under-observed surfaces skipped.
void testMemorylessClassifier() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0u);  // 0
  // Eligible: single-pass transient, clean classifier flags.
  ResourceNode& eligible = addResource(g, 0xA000u);
  access(eligible, 0, AccessKind::Write);
  // Locked surface (single pass) -> skipped.
  ResourceNode& locked = addResource(g, 0xB000u);
  access(locked, 0, AccessKind::Write);
  locked.classifier_flags.lock_seen = true;
  // Readback surface -> skipped.
  ResourceNode& readback = addResource(g, 0xC000u);
  access(readback, 0, AccessKind::Write);
  readback.classifier_flags.readback_seen = true;
  // Under-observed eligible-shaped surface -> blocked on observation.
  ResourceNode& young = addResource(g, 0xE000u);
  access(young, 0, AccessKind::Write);
  runLifetime(g);

  std::vector<MemorylessObservation> obs;
  obs.push_back(MemorylessObservation{.handle = ResourceHandle{0xA000u},
                                      .observation_frames = 8});  // met threshold
  obs.push_back(MemorylessObservation{.handle = ResourceHandle{0xB000u},
                                      .observation_frames = 8});
  obs.push_back(MemorylessObservation{.handle = ResourceHandle{0xC000u},
                                      .observation_frames = 8});
  obs.push_back(MemorylessObservation{.handle = ResourceHandle{0xE000u},
                                      .observation_frames = 3});  // under threshold

  OptimizerStats stats{};
  markMemorylessCandidates(g, obs, /*threshold=*/8, &stats);

  const auto residency = [&](std::uint64_t h) {
    return g.resources[findResourceIndex(g, ResourceHandle{h})].residency;
  };
  check(residency(0xA000u) == ResidencyClass::Memoryless,
        "eligible transient surface promoted");
  check(residency(0xB000u) == ResidencyClass::Persistent,
        "locked surface not promoted");
  check(residency(0xC000u) == ResidencyClass::Persistent,
        "readback surface not promoted");
  check(residency(0xE000u) == ResidencyClass::Persistent,
        "under-observed surface not promoted");
  check(stats.memoryless_promoted == 1, "one promotion counted");
  check(stats.memoryless_dropped_via_lock == 1, "lock drop counted");
  check(stats.memoryless_dropped_via_readback == 1, "readback drop counted");
  check(stats.memoryless_blocked_observation == 1, "observation block counted");
}

void testMemorylessBackbufferSkipped() {
  FrameGraph g;
  addRenderPass(g, 0xA000u, 0u);
  ResourceNode& bb = addResource(g, 0xA000u);
  access(bb, 0, AccessKind::Write);
  runLifetime(g);

  std::vector<MemorylessObservation> obs;
  obs.push_back(MemorylessObservation{.handle = ResourceHandle{0xA000u},
                                      .observation_frames = 64,
                                      .bound_as_backbuffer = true});
  OptimizerStats stats{};
  markMemorylessCandidates(g, obs, 8, &stats);
  check(g.resources[0].residency == ResidencyClass::Persistent,
        "backbuffer never promoted even when long-observed");
}

// End-to-end ordering: full pipeline on a built ChunkSlot graph still produces a
// valid, edge-consistent result with all features on.
void testFullPipelineWithBuilder() {
  using dxmt9::core::ChunkSlot;
  using dxmt9::core::ClearDesc;
  ChunkSlot slot;
  ClearDesc clear{};
  clear.colorAttachments[0].handle = Handle{0xA000u};
  clear.clearColor = true;
  slot.appendClear(clear);

  FrameGraph g = buildFrameGraph(slot, 1);
  OptimizerOptions opts{};
  opts.passcoalesce = true;
  opts.reorder = true;
  // memoryless + dce left at defaults (off / off) for this end-to-end smoke.
  std::vector<MemorylessObservation> obs;
  runOptimizer(g, opts, &obs, nullptr);

  for (const Edge& e : g.edges) {
    check(e.src_pass < g.passes.size() && e.dst_pass < g.passes.size(),
          "full pipeline: edge endpoints in range");
  }
}

}  // namespace

int main() {
  try {
    testLifetime();
    testDefaultOptionsPreserveOrder();
    testLoadStoreClearAndStore();
    testLoadStoreMemorylessDontCare();
    testPassCoalesceSafe();
    testPassCoalesceBlockedByDependency();
    testDceOffKeepsAll();
    testDceOnDropsProvablyDead();
    testDceNeverDropsPresentOrQuery();
    testReorderPreservesEdges();
    testReorderRespectsAntiDepEdge();
    testPassCoalesceBlockedByAntiDep();
    testReorderIdentityWhenNoEdges();
    testMemorylessClassifier();
    testMemorylessBackbufferSkipped();
    testFullPipelineWithBuilder();
  } catch (const TestFailure& failure) {
    std::cerr << "fg_optimizer_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "fg_optimizer_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }
  std::cout << "fg_optimizer_spec passed\n";
  return 0;
}
