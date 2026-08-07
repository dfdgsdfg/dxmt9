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
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"

#include <array>
#include <cstdlib>
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

u32 addReplayRenderPass(FrameGraph& g, std::uint64_t color0,
                        std::uint64_t depth, u32 command_index) {
  PassNode pass{};
  pass.kind = PassKind::Render;
  pass.targets = colorDepth(color0, depth);
  pass.draws.first = static_cast<u32>(g.draws.size());
  pass.draws.count = 1;
  g.draws.push_back(DrawRef{.command_index = command_index});
  pass.commands.first = static_cast<u32>(g.commands.size());
  pass.commands.count = 1;
  g.commands.push_back(CommandRef{
      .command_index = command_index,
      .kind = dxmt9::core::MetalCommandKind::DrawRun,
  });
  g.passes.push_back(pass);
  return static_cast<u32>(g.passes.size() - 1u);
}

ActiveRenderPlanningSeed activeSeed(std::uint64_t color0,
                                    std::uint64_t depth = 0) {
  ActiveRenderPlanningSeed seed{};
  seed.targets = colorDepth(color0, depth);
  if (color0 != 0) {
    seed.write_dependencies[seed.dependency_count++] =
        ResourceHandle{color0};
  }
  if (depth != 0) {
    seed.write_dependencies[seed.dependency_count++] =
        ResourceHandle{depth};
  }
  seed.complete = true;
  return seed;
}

bool sameGraph(const FrameGraph& left, const FrameGraph& right) {
  return left.passes == right.passes &&
      left.resources == right.resources && left.edges == right.edges &&
      left.draws == right.draws && left.commands == right.commands &&
      left.frame_id == right.frame_id &&
      left.flush_boundary == right.flush_boundary;
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
  addReplayRenderPass(g, 0xA000u, 0xD000u, 0u);
  addReplayRenderPass(g, 0x9000u, 0u, 1u);
  addReplayRenderPass(g, 0xA000u, 0xD000u, 2u);
  // No edges: pass1 is independent of 0 and 2.
  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 0, AccessKind::Write);
  access(a, 2, AccessKind::Write);
  ResourceNode& z = addResource(g, 0x9000u);
  access(z, 1, AccessKind::Write);
  runLifetime(g);

  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesced_count == 1, "one coalesce happened");
  check(stats.pass_coalesce_return_candidates == 1u &&
            stats.pass_coalesce_return_merged == 1u &&
            stats.pass_coalesce_return_move_before == 0u &&
            stats.pass_coalesce_return_move_after == 1u &&
            stats.pass_coalesce_return_blocked_cycle == 0u &&
            stats.pass_coalesce_return_candidates ==
                stats.pass_coalesce_return_merged +
                    stats.pass_coalesce_return_blocked_cycle +
                    stats.pass_coalesce_return_second_non_draw +
                    stats.pass_coalesce_return_non_render_intervener +
                    stats.pass_coalesce_return_missing_invariant,
        "movable B is one conserved merged return with move-after");
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
  addReplayRenderPass(g, 0xA000u, 0xD000u, 0u);  // writes A
  addReplayRenderPass(g, 0x9000u, 0u, 1u);       // reads A, writes 0x9000
  addReplayRenderPass(g, 0xA000u, 0xD000u, 2u);  // reads 0x9000, writes A
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
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesced_count == 0, "dependency-wedged pair not coalesced");
  check(stats.pass_coalesce_return_candidates == 1u &&
            stats.pass_coalesce_return_blocked_cycle == 1u &&
            stats.pass_coalesce_return_merged == 0u,
        "dependency-wedged B is one conserved blocked-cycle return");
  check(g.passes.size() == 3, "all three passes survive");
}

void testPassCoalesceReportsReturningNonDrawSemantics() {
  FrameGraph g;
  addReplayRenderPass(g, 0xA000u, 0u, 0u);
  addReplayRenderPass(g, 0xB000u, 0u, 1u);
  addReplayRenderPass(g, 0xA000u, 0u, 2u);
  g.commands.back().kind = dxmt9::core::MetalCommandKind::Clear;

  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(g.passes.size() == 3 && stats.pass_coalesced_count == 0,
        "returning A with non-draw semantics remains unmerged");
  check(stats.pass_coalesce_return_candidates == 1u &&
            stats.pass_coalesce_return_second_non_draw == 1u &&
            stats.pass_coalesce_return_merged == 0u,
        "returning non-draw A is conserved in its semantic blocker bucket");
}

void testPassCoalesceReturnDiagnosticsExcludeAdjacentPair() {
  FrameGraph g;
  addReplayRenderPass(g, 0xA000u, 0u, 0u);
  addReplayRenderPass(g, 0xA000u, 0u, 1u);

  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesced_count == 1u && g.passes.size() == 1u,
        "adjacent A-A retains existing coalescing behavior");
  check(stats.pass_coalesce_return_candidates == 0u &&
            stats.pass_coalesce_return_merged == 0u,
        "adjacent A-A is not classified as an A-B-A return");
}

void testPassCoalesceObservesSemanticIntervener() {
  FrameGraph g;
  addReplayRenderPass(g, 0xA000u, 0u, 0u);
  addReplayRenderPass(g, 0xB000u, 0u, 1u);
  addReplayRenderPass(g, 0xA000u, 0u, 2u);
  g.commands[1].kind = dxmt9::core::MetalCommandKind::Clear;
  g.passes[1].flags.contains_event_query = true;

  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesce_return_merged == 1u &&
            stats.pass_coalesce_return_non_draw_intervener == 1u &&
            stats.pass_coalesce_return_semantic_intervener == 1u,
        "semantic B is observed without changing existing merge behavior");
}

void testPassCoalesceReturnDedupeSurvivesUnrelatedMerge() {
  FrameGraph g;
  addReplayRenderPass(g, 0xA000u, 0u, 0u);
  addReplayRenderPass(g, 0xB000u, 0u, 1u);
  addReplayRenderPass(g, 0xA000u, 0u, 2u);
  addReplayRenderPass(g, 0xC000u, 0u, 3u);
  addReplayRenderPass(g, 0xC000u, 0u, 4u);
  g.edges.push_back(Edge{
      .src_pass = 0,
      .dst_pass = 1,
      .resource = ResourceHandle{0xAB01u},
  });
  g.edges.push_back(Edge{
      .src_pass = 1,
      .dst_pass = 2,
      .resource = ResourceHandle{0xAB02u},
  });

  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesced_count == 1u && g.passes.size() == 4u,
        "unrelated adjacent C-C still coalesces");
  check(stats.pass_coalesce_return_candidates == 1u &&
            stats.pass_coalesce_return_blocked_cycle == 1u,
        "unrelated merge does not recount the stable wedged A-B-A window");
}

void testPassCoalesceReturnDedupeAllowsChangedWindow() {
  FrameGraph g;
  addReplayRenderPass(g, 0xA000u, 0u, 0u);
  addReplayRenderPass(g, 0xB000u, 0u, 1u);
  addReplayRenderPass(g, 0xB000u, 0u, 2u);
  addReplayRenderPass(g, 0xA000u, 0u, 3u);
  g.edges.push_back(Edge{
      .src_pass = 0,
      .dst_pass = 1,
      .resource = ResourceHandle{0xAB11u},
  });
  g.edges.push_back(Edge{
      .src_pass = 1,
      .dst_pass = 3,
      .resource = ResourceHandle{0xAB12u},
  });

  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesced_count == 1u && g.passes.size() == 3u,
        "interior adjacent B-B merge creates a new pass incarnation");
  check(stats.pass_coalesce_return_candidates == 2u &&
            stats.pass_coalesce_return_blocked_cycle == 2u,
        "changed A-[Bmerged]-A window is a distinct diagnostic candidate");
}

void testPassCoalesceMixedSeedCountsSourceOwnedReturn() {
  FrameGraph g;
  addReplayRenderPass(g, 0xA000u, 0u, 0u);
  addReplayRenderPass(g, 0xB000u, 0u, 1u);
  addReplayRenderPass(g, 0xA000u, 0u, 2u);
  check(applyActiveRenderPlanningSeed(g, activeSeed(0xA000u)) ==
            ActiveRenderPlanningSeedResult::Applied,
        "mixed-seed fixture applies active A");

  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesced_count == 2u,
        "seed merges first A and the resulting source return");
  check(stats.pass_coalesce_return_candidates == 1u &&
            stats.pass_coalesce_return_merged == 1u,
        "seed lineage retains the absorbed source A ownership");
}

void testPassCoalesceCommandlessSourceEndsPureSeedExclusion() {
  FrameGraph g;
  PassNode commandlessA{};
  commandlessA.kind = PassKind::Render;
  commandlessA.targets = colorDepth(0xA000u, 0u);
  g.passes.push_back(commandlessA);
  addReplayRenderPass(g, 0xB000u, 0u, 0u);
  addReplayRenderPass(g, 0xA000u, 0u, 1u);
  check(applyActiveRenderPlanningSeed(g, activeSeed(0xA000u)) ==
            ActiveRenderPlanningSeedResult::Applied,
        "commandless-source fixture applies active A");

  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesced_count == 2u,
        "seed merges commandless source A before its later return");
  check(stats.pass_coalesce_return_candidates == 1u &&
            stats.pass_coalesce_return_merged == 1u,
        "commandless source membership ends pure virtual-seed exclusion");
}

void testPassCoalesceReturnDiagnosticsAreReplayNeutral() {
  FrameGraph baseline;
  addReplayRenderPass(baseline, 0xA000u, 0u, 0u);
  addReplayRenderPass(baseline, 0xB000u, 0u, 1u);
  addReplayRenderPass(baseline, 0xA000u, 0u, 2u);
  FrameGraph observed = baseline;
  OptimizerStats baselineStats{};
  OptimizerStats observedStats{};

  runPassCoalesce(baseline, &baselineStats, false);
  runPassCoalesce(observed, &observedStats, true);

  check(sameGraph(baseline, observed),
        "return diagnostics do not change valid graph planning output");
  check(baselineStats.pass_coalesce_return_candidates == 0u &&
            observedStats.pass_coalesce_return_candidates == 1u &&
            observedStats.pass_coalesce_return_merged == 1u,
        "collection flag changes only R7 diagnostic statistics");
}

void testPassCoalesceMalformedRangeHardensToMissingInvariant() {
  FrameGraph g;
  addReplayRenderPass(g, 0xA000u, 0u, 0u);
  addReplayRenderPass(g, 0xB000u, 0u, 1u);
  addReplayRenderPass(g, 0xA000u, 0u, 2u);
  g.passes[2].commands.first =
      static_cast<u32>(g.commands.size() + 1u);
  const FrameGraph malformed = g;

  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(sameGraph(g, malformed),
        "malformed command range conservatively preserves the graph");
  check(stats.pass_coalesce_return_candidates == 1u &&
            stats.pass_coalesce_return_missing_invariant == 1u,
        "malformed return has one conserved missing-invariant terminal");
}

void testSourceLocalReturnCounterSnapshotAudit() {
  using dxmt9::perf::FramegraphSourceLocalPassCoalesceDiagnostic;
  using dxmt9::perf::FramegraphSourceProvenance;
  const auto before =
      dxmt9::perf::test::snapshotFramegraphSourceLocalPassCoalesce();

  dxmt9::perf::recordFramegraphSourceLocalPassCoalesce(
      FramegraphSourceProvenance::Legacy, true,
      FramegraphSourceLocalPassCoalesceDiagnostic{
          .candidates = 4u,
          .merged = 1u,
          .blockedCycle = 1u,
          .secondNonDraw = 1u,
          .missingInvariant = 1u,
          .dependencyKept = 1u,
          .moveBefore = 1u,
          .nonDrawIntervener = 1u,
          .semanticIntervener = 1u,
      });
  dxmt9::perf::recordFramegraphSourceLocalPassCoalesce(
      FramegraphSourceProvenance::Arena, false,
      FramegraphSourceLocalPassCoalesceDiagnostic{
          .candidates = 2u,
          .merged = 1u,
          .nonRenderIntervener = 1u,
          .moveAfter = 1u,
          .commandlessIntervener = 1u,
          .commandlessReturn = 1u,
      });

  const auto after =
      dxmt9::perf::test::snapshotFramegraphSourceLocalPassCoalesce();
  check(after.candidates - before.candidates == 6u &&
            after.merged - before.merged == 2u &&
            after.blockedCycle - before.blockedCycle == 1u &&
            after.secondNonDraw - before.secondNonDraw == 1u &&
            after.nonRenderIntervener - before.nonRenderIntervener == 1u &&
            after.missingInvariant - before.missingInvariant == 1u &&
            after.dependencyKept - before.dependencyKept == 1u &&
            after.moveBefore - before.moveBefore == 1u &&
            after.moveAfter - before.moveAfter == 1u &&
            after.nonDrawIntervener - before.nonDrawIntervener == 1u &&
            after.semanticIntervener - before.semanticIntervener == 1u &&
            after.commandlessIntervener - before.commandlessIntervener == 1u &&
            after.commandlessReturn - before.commandlessReturn == 1u &&
            after.candidates - before.candidates ==
                (after.merged - before.merged) +
                    (after.blockedCycle - before.blockedCycle) +
                    (after.secondNonDraw - before.secondNonDraw) +
                    (after.nonRenderIntervener -
                     before.nonRenderIntervener) +
                    (after.missingInvariant - before.missingInvariant),
        "source-local return counters preserve aggregate classification");
  check(after.legacyCandidates - before.legacyCandidates == 4u &&
            after.arenaCandidates - before.arenaCandidates == 2u &&
            after.unknownCandidates == before.unknownCandidates &&
            after.identityKnownCandidates - before.identityKnownCandidates ==
                4u &&
            after.identityMissingCandidates -
                    before.identityMissingCandidates ==
                2u &&
            after.legacyCandidates - before.legacyCandidates +
                    after.arenaCandidates - before.arenaCandidates +
                    after.unknownCandidates - before.unknownCandidates ==
                after.candidates - before.candidates,
        "source-local counters split representation and identity availability");
}

void testSourceLocalReplayOutcomeCountersConservePopulation() {
  using dxmt9::perf::FramegraphSourceLocalFrontierRollbackReason;
  using dxmt9::perf::FramegraphSourceLocalPassCoalesceDiagnostic;
  using dxmt9::perf::FramegraphSourceLocalReplayOutcome;
  using dxmt9::perf::FramegraphSourceProvenance;
  const auto aggregateBefore =
      dxmt9::perf::test::snapshotFramegraphSourceLocalPassCoalesce();
  const auto outcomeBefore =
      dxmt9::perf::test::snapshotFramegraphSourceLocalReplayOutcome();

  dxmt9::perf::recordFramegraphSourceLocalPassCoalesce(
      FramegraphSourceProvenance::Legacy, true,
      FramegraphSourceLocalPassCoalesceDiagnostic{
          .candidates = 10u,
          .merged = 6u,
          .missingInvariant = 4u,
      });
  dxmt9::perf::recordFramegraphSourceLocalFrontierRollback(
      FramegraphSourceLocalFrontierRollbackReason::InvalidPlan, 1u, 0u);
  dxmt9::perf::recordFramegraphSourceLocalFrontierRollback(
      FramegraphSourceLocalFrontierRollbackReason::LiveSetMismatch, 1u, 1u);
  dxmt9::perf::recordFramegraphSourceLocalFrontierRollback(
      FramegraphSourceLocalFrontierRollbackReason::DuplicateCommand, 1u, 0u);
  dxmt9::perf::recordFramegraphSourceLocalFrontierRollback(
      FramegraphSourceLocalFrontierRollbackReason::MovedHeadUnproved, 1u, 1u);
  dxmt9::perf::recordFramegraphSourceLocalReplayOutcome(
      FramegraphSourceLocalReplayOutcome::FinalInvalid, 1u, 1u);
  dxmt9::perf::recordFramegraphSourceLocalReplayOutcome(
      FramegraphSourceLocalReplayOutcome::FinalNaturalOrder, 2u, 1u);
  dxmt9::perf::recordFramegraphSourceLocalReplayOutcome(
      FramegraphSourceLocalReplayOutcome::FinalReorderedActivated, 3u, 2u);

  const auto aggregateAfter =
      dxmt9::perf::test::snapshotFramegraphSourceLocalPassCoalesce();
  const auto outcomeAfter =
      dxmt9::perf::test::snapshotFramegraphSourceLocalReplayOutcome();
  const auto delta = [](const auto& after, const auto& before) {
    return dxmt9::perf::test::FramegraphSourceLocalReplayOutcomeCount{
        .sources = after.sources - before.sources,
        .candidates = after.candidates - before.candidates,
        .merged = after.merged - before.merged,
    };
  };
  const auto rollback = delta(outcomeAfter.frontierRollback,
                              outcomeBefore.frontierRollback);
  const auto rollbackInvalid = delta(
      outcomeAfter.frontierRollbackInvalidPlan,
      outcomeBefore.frontierRollbackInvalidPlan);
  const auto rollbackLiveSet = delta(
      outcomeAfter.frontierRollbackLiveSetMismatch,
      outcomeBefore.frontierRollbackLiveSetMismatch);
  const auto rollbackDuplicate = delta(
      outcomeAfter.frontierRollbackDuplicateCommand,
      outcomeBefore.frontierRollbackDuplicateCommand);
  const auto rollbackMovedHead = delta(
      outcomeAfter.frontierRollbackMovedHeadUnproved,
      outcomeBefore.frontierRollbackMovedHeadUnproved);
  const auto invalid = delta(outcomeAfter.finalInvalid,
                             outcomeBefore.finalInvalid);
  const auto natural = delta(outcomeAfter.finalNaturalOrder,
                             outcomeBefore.finalNaturalOrder);
  const auto activated = delta(outcomeAfter.finalReorderedActivated,
                               outcomeBefore.finalReorderedActivated);

  check(rollback.sources == 4u && rollback.candidates == 4u &&
            rollback.merged == 2u && invalid.sources == 1u &&
            invalid.candidates == 1u && invalid.merged == 1u &&
            natural.sources == 1u && natural.candidates == 2u &&
            natural.merged == 1u && activated.sources == 1u &&
            activated.candidates == 3u && activated.merged == 2u,
        "replay outcome counters update only their selected terminal bucket");
  check(rollbackInvalid.sources + rollbackLiveSet.sources +
                rollbackDuplicate.sources + rollbackMovedHead.sources ==
            rollback.sources &&
            rollbackInvalid.candidates + rollbackLiveSet.candidates +
                    rollbackDuplicate.candidates +
                    rollbackMovedHead.candidates ==
                rollback.candidates &&
            rollbackInvalid.merged + rollbackLiveSet.merged +
                    rollbackDuplicate.merged + rollbackMovedHead.merged ==
                rollback.merged &&
            rollbackInvalid.sources == 1u &&
            rollbackLiveSet.sources == 1u &&
            rollbackDuplicate.sources == 1u &&
            rollbackMovedHead.sources == 1u,
        "frontier rollback reasons conserve broad rollback on every axis");
  check(rollback.sources + invalid.sources + natural.sources +
                activated.sources ==
            7u &&
            rollback.candidates + invalid.candidates + natural.candidates +
                    activated.candidates ==
                aggregateAfter.candidates - aggregateBefore.candidates &&
            rollback.merged + invalid.merged + natural.merged +
                    activated.merged ==
                aggregateAfter.merged - aggregateBefore.merged,
        "replay outcome source, candidate, and merged axes conserve totals");
}

void testActiveRenderSeedMovesLegalSamePassToHead() {
  FrameGraph g;
  addReplayRenderPass(g, 0xB000u, 0u, 0u);
  addReplayRenderPass(g, 0xA000u, 0u, 1u);
  ResourceNode& b = addResource(g, 0xB000u);
  access(b, 0, AccessKind::Write);
  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 1, AccessKind::Write);

  check(applyActiveRenderPlanningSeed(g, activeSeed(0xA000u)) ==
            ActiveRenderPlanningSeedResult::Applied,
        "complete active A seed applies");
  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesced_count == 1,
        "active A coalesces with current later A");
  check(stats.pass_coalesce_return_candidates == 0u,
        "virtual active seed is excluded from source-owned A-B-A counters");
  check(g.commands.size() == 2 && g.commands[0].command_index == 1u &&
            g.commands[1].command_index == 0u,
        "active A plus current B,A plans current A,B");
  check(activeRenderPlanningSeedProvesReplayHead(g, 1u),
        "merged virtual seed proves the moved current head");
}

void testActiveRenderSeedReportsEmptyInterveningPass() {
  FrameGraph g;
  PassNode empty{};
  empty.kind = PassKind::Render;
  empty.targets = colorDepth(0xB000u, 0u);
  g.passes.push_back(empty);
  addReplayRenderPass(g, 0xA000u, 0u, 0u);
  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 1, AccessKind::Write);

  check(applyActiveRenderPlanningSeed(g, activeSeed(0xA000u)) ==
            ActiveRenderPlanningSeedResult::Applied,
        "empty-intervening seed applies");
  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesce_active_seed_merge_count == 1u &&
            stats.pass_coalesce_active_seed_merge_distance_total == 2u &&
            stats.pass_coalesce_active_seed_merge_distance_max == 2u &&
            stats.pass_coalesce_active_seed_command_before == 0u &&
            stats.pass_coalesce_active_seed_command_after == 0u &&
            stats.pass_coalesce_active_seed_empty_intervening == 1u,
        "active seed attributes a commandless intervening pass exactly");
}

void testActiveRenderSeedDependencyWedgeKeepsNaturalHead() {
  FrameGraph g;
  addReplayRenderPass(g, 0xB000u, 0u, 0u);
  addReplayRenderPass(g, 0xA000u, 0u, 1u);
  ResourceNode& a = addResource(g, 0xA000u);
  access(a, 0, AccessKind::Read);
  access(a, 1, AccessKind::Write);
  ResourceNode& b = addResource(g, 0xB000u);
  access(b, 0, AccessKind::Write);
  access(b, 1, AccessKind::Read);
  g.edges.push_back(Edge{
      .src_pass = 0,
      .dst_pass = 1,
      .resource = ResourceHandle{0xA000u},
  });
  g.edges.push_back(Edge{
      .src_pass = 0,
      .dst_pass = 1,
      .resource = ResourceHandle{0xB000u},
  });

  check(applyActiveRenderPlanningSeed(g, activeSeed(0xA000u)) ==
            ActiveRenderPlanningSeedResult::Applied,
        "dependency-wedge seed applies before conservative planning");
  OptimizerStats stats{};
  runPassCoalesce(g, &stats, true);

  check(stats.pass_coalesced_count == 0,
        "seed-to-B-to-A dependency wedge blocks the head move");
  check(g.commands.size() == 2 && g.commands[0].command_index == 0u &&
            g.commands[1].command_index == 1u,
        "dependency wedge retains natural B,A replay");
  check(!activeRenderPlanningSeedProvesReplayHead(g, 0u),
        "unmerged seed cannot authorize a head change");
}

void testInvalidActiveRenderSeedsAreNoMutationFallbacks() {
  FrameGraph original;
  addReplayRenderPass(original, 0xB000u, 0u, 0u);
  addReplayRenderPass(original, 0xA000u, 0u, 1u);
  auto unchanged = [&](const FrameGraph& graph) {
    return graph.passes == original.passes &&
           graph.resources == original.resources &&
           graph.edges == original.edges && graph.draws == original.draws &&
           graph.commands == original.commands &&
           graph.frame_id == original.frame_id &&
           graph.flush_boundary == original.flush_boundary;
  };

  auto invalid = activeSeed(0xA000u);
  invalid.targets = {};
  FrameGraph graph = original;
  check(applyActiveRenderPlanningSeed(graph, invalid) ==
            ActiveRenderPlanningSeedResult::Invalid && unchanged(graph),
        "invalid target seed leaves natural B,A graph unchanged");

  auto incomplete = activeSeed(0xA000u);
  incomplete.complete = false;
  graph = original;
  check(applyActiveRenderPlanningSeed(graph, incomplete) ==
            ActiveRenderPlanningSeedResult::Incomplete && unchanged(graph),
        "incomplete seed leaves natural B,A graph unchanged");

  auto overflow = activeSeed(0xA000u);
  overflow.dependency_count =
      static_cast<u32>(overflow.write_dependencies.size() + 1u);
  graph = original;
  check(applyActiveRenderPlanningSeed(graph, overflow) ==
            ActiveRenderPlanningSeedResult::Overflow && unchanged(graph),
        "overflow seed leaves natural B,A graph unchanged");
}

void testActiveRenderSeedDceAccounting() {
  FrameGraph unmerged;
  addReplayRenderPass(unmerged, 0xB000u, 0u, 0u);
  ResourceNode& b = addResource(unmerged, 0xB000u);
  access(b, 0, AccessKind::Write);
  check(applyActiveRenderPlanningSeed(unmerged, activeSeed(0xA000u)) ==
            ActiveRenderPlanningSeedResult::Applied,
        "unmerged active-render seed applies");

  const std::array<ResourceHandle, 2> unmergedProof{
      ResourceHandle{0xA000u},
      ResourceHandle{0xB000u},
  };
  OptimizerStats unmergedStats{};
  runDce(unmerged, &unmergedStats, DceLookaheadProof{unmergedProof});
  check(unmerged.passes.size() == 2 &&
            unmerged.passes[0].flags.active_render_seed &&
            !unmerged.passes[0].flags.dead && unmerged.passes[1].flags.dead,
        "DCE excludes only the commandless unmerged seed");
  check(unmergedStats.dce_dropped == 1 &&
            unmergedStats.dce_preserved_unprovable == 0,
        "commandless seed contributes no DCE decision or stats");

  FrameGraph merged;
  addReplayRenderPass(merged, 0xA000u, 0u, 0u);
  ResourceNode& a = addResource(merged, 0xA000u);
  access(a, 0, AccessKind::Write);
  check(applyActiveRenderPlanningSeed(merged, activeSeed(0xA000u)) ==
            ActiveRenderPlanningSeedResult::Applied,
        "mergeable active-render seed applies");
  runPassCoalesce(merged, nullptr);
  check(merged.passes.size() == 1 &&
            merged.passes[0].flags.active_render_seed &&
            merged.passes[0].commands.count == 1,
        "passcoalesce folds a real command into the seed pass");

  const std::array<ResourceHandle, 1> mergedProof{
      ResourceHandle{0xA000u},
  };
  OptimizerStats mergedStats{};
  runDce(merged, &mergedStats, DceLookaheadProof{mergedProof});
  check(merged.passes[0].flags.dead && mergedStats.dce_dropped == 1 &&
            mergedStats.dce_preserved_unprovable == 0,
        "merged real-command seed pass retains normal DCE behavior");
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

void testDceUsesBoundedNextChunkOverwriteProof() {
  FrameGraph current;
  addRenderPass(current, 0xA000u, 0xD000u);
  ResourceNode& color = addResource(current, 0xA000u);
  access(color, 0, AccessKind::Write);
  ResourceNode& depth = addResource(current, 0xD000u);
  access(depth, 0, AccessKind::Write);

  FrameGraph next;
  addRenderPass(next, 0xA000u, 0xD000u);
  ResourceNode& nextColor = addResource(next, 0xA000u);
  access(nextColor, 0, AccessKind::Clear);
  ResourceNode& nextDepth = addResource(next, 0xD000u);
  access(nextDepth, 0, AccessKind::Clear);

  const std::vector<ResourceHandle> overwrites =
      collectDceLookaheadFullOverwrites(next);
  check(overwrites.size() == 2,
        "next-chunk full clears produce two overwrite proofs");

  OptimizerStats stats{};
  runDce(current, &stats, DceLookaheadProof{overwrites});
  check(current.passes[0].flags.dead,
        "persistent color+depth pass drops when every output is next-cleared");
  check(stats.dce_dropped == 1,
        "cross-chunk overwrite proof increments dropped count");

  FrameGraph missingDepth;
  addRenderPass(missingDepth, 0xA000u, 0xD000u);
  ResourceNode& missingColor = addResource(missingDepth, 0xA000u);
  access(missingColor, 0, AccessKind::Write);
  ResourceNode& missingDepthNode = addResource(missingDepth, 0xD000u);
  access(missingDepthNode, 0, AccessKind::Write);
  const std::array<ResourceHandle, 1> colorOnly{ResourceHandle{0xA000u}};
  runDce(missingDepth, nullptr, DceLookaheadProof{colorOnly});
  check(!missingDepth.passes[0].flags.dead,
        "missing proof for one attachment preserves the whole pass");
}

void testDceLookaheadRejectsReadBeforeClear() {
  FrameGraph next;
  addRenderPass(next, 0xB000u, 0u);
  addRenderPass(next, 0xA000u, 0u);
  ResourceNode& resource = addResource(next, 0xA000u);
  access(resource, 0, AccessKind::Read);
  access(resource, 1, AccessKind::Clear);

  const std::vector<ResourceHandle> overwrites =
      collectDceLookaheadFullOverwrites(next);
  check(overwrites.empty(),
        "a next-chunk read before clear cannot prove prior output dead");
}

void testDceLookaheadPredictsOnlyNonemptySafePrefix() {
  FrameGraph graph;
  addRenderPass(graph, 0xA000u, 0u);
  addRenderPass(graph, 0xB000u, 0u);
  PassNode present{};
  present.kind = PassKind::Present;
  graph.passes.push_back(present);

  graph.commands = {
      CommandRef{.command_index = 0,
                 .kind = dxmt9::core::MetalCommandKind::DrawRun},
      CommandRef{.command_index = 1,
                 .kind = dxmt9::core::MetalCommandKind::DrawRun},
      CommandRef{.command_index = 2,
                 .kind = dxmt9::core::MetalCommandKind::Present},
  };
  for (u32 p = 0; p < graph.passes.size(); ++p) {
    graph.passes[p].commands =
        CommandRange{.first = p, .count = 1};
  }

  ResourceNode& retained = addResource(graph, 0xA000u);
  access(retained, 0, AccessKind::Write);
  access(retained, 2, AccessKind::Read);
  ResourceNode& predicted = addResource(graph, 0xB000u);
  access(predicted, 1, AccessKind::Write);

  const std::array<ResourceHandle, 1> prior{ResourceHandle{0xB000u}};
  OptimizerOptions options{};
  options.dce = true;
  const auto prefix = planDceLookaheadReplayPrefix(
      graph, 3u, options, DceLookaheadProof{prior});
  check(prefix == std::vector<u32>{0u},
        "prior proof predicts commands before the first proof-dependent pass");

  check(planDceLookaheadReplayPrefix(
            graph, 3u, options, DceLookaheadProof{})
            .empty(),
        "no prior proof cannot place a speculative ready-FIFO sample boundary");

  FrameGraph first;
  addRenderPass(first, 0xB000u, 0u);
  PassNode firstPresent{};
  firstPresent.kind = PassKind::Present;
  first.passes.push_back(firstPresent);
  first.commands = {
      CommandRef{.command_index = 0,
                 .kind = dxmt9::core::MetalCommandKind::DrawRun},
      CommandRef{.command_index = 1,
                 .kind = dxmt9::core::MetalCommandKind::Present},
  };
  first.passes[0].commands = CommandRange{.first = 0, .count = 1};
  first.passes[1].commands = CommandRange{.first = 1, .count = 1};
  ResourceNode& firstPredicted = addResource(first, 0xB000u);
  access(firstPredicted, 0, AccessKind::Write);
  check(planDceLookaheadReplayPrefix(
            first, 2u, options, DceLookaheadProof{prior})
            .empty(),
        "a proof-dependent first command cannot yield a nonempty prefix");
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

void testActiveSeedWitnessOverflowPublishesNoPartialSet() {
  std::array<dxmt9::encoders::ActiveSeedMergeCommandWitness, 1> storage{};
  ActiveSeedMergeWitnessSink sink{.storage = storage};
  sink.record(7u, 1u);
  check(!sink.overflow && sink.count == 1u && sink.attempted == 1u,
        "bounded witness sink accepts its pre-sized capacity");
  sink.record(3u, 2u);
  check(sink.overflow && sink.attempted == 2u && sink.count == 1u,
        "the next mutation marks overflow without writing out of bounds");
  check(sink.publishable().empty(),
        "overflow fail-closes the complete witness set, not a partial prefix");
}

}  // namespace

int main() {
  setenv("DXMT_PERF_COUNTERS", "1", 1);
  try {
    testLifetime();
    testDefaultOptionsPreserveOrder();
    testLoadStoreClearAndStore();
    testLoadStoreMemorylessDontCare();
    testPassCoalesceSafe();
    testPassCoalesceBlockedByDependency();
    testPassCoalesceReportsReturningNonDrawSemantics();
    testPassCoalesceReturnDiagnosticsExcludeAdjacentPair();
    testPassCoalesceObservesSemanticIntervener();
    testPassCoalesceReturnDedupeSurvivesUnrelatedMerge();
    testPassCoalesceReturnDedupeAllowsChangedWindow();
    testPassCoalesceMixedSeedCountsSourceOwnedReturn();
    testPassCoalesceCommandlessSourceEndsPureSeedExclusion();
    testPassCoalesceReturnDiagnosticsAreReplayNeutral();
    testPassCoalesceMalformedRangeHardensToMissingInvariant();
    testSourceLocalReturnCounterSnapshotAudit();
    testSourceLocalReplayOutcomeCountersConservePopulation();
    testActiveRenderSeedMovesLegalSamePassToHead();
    testActiveRenderSeedReportsEmptyInterveningPass();
    testActiveRenderSeedDependencyWedgeKeepsNaturalHead();
    testInvalidActiveRenderSeedsAreNoMutationFallbacks();
    testActiveRenderSeedDceAccounting();
    testDceOffKeepsAll();
    testDceOnDropsProvablyDead();
    testDceNeverDropsPresentOrQuery();
    testDceUsesBoundedNextChunkOverwriteProof();
    testDceLookaheadRejectsReadBeforeClear();
    testDceLookaheadPredictsOnlyNonemptySafePrefix();
    testReorderPreservesEdges();
    testReorderRespectsAntiDepEdge();
    testPassCoalesceBlockedByAntiDep();
    testReorderIdentityWhenNoEdges();
    testMemorylessClassifier();
    testMemorylessBackbufferSkipped();
    testFullPipelineWithBuilder();
    testActiveSeedWitnessOverflowPublishesNoPartialSet();
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
