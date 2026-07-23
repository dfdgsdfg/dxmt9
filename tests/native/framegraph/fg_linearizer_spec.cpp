// Device-free spec for the Frame Graph linearizer (Task B9, L1).
//
// src/dxmt9/framegraph/fg_linearizer.cpp's planLinearization() walks an
// optimized framegraph::FrameGraph and emits an ordered POD op list (BeginPass
// / EmitDraw / EmitClear / EndPass / Present / Blit). This spec builds synthetic
// ChunkSlots (same SoA fixture style as fg_builder_spec.cpp), runs
// buildFrameGraph -> runOptimizer -> planLinearization, and asserts:
//   - PARITY BASELINE (spec.md §14 L1 / R-BACK-39.1): under default
//     OptimizerOptions{} the plan reproduces the chunk's natural pass/draw
//     order (BeginPass/EmitDraw.../EndPass per pass, draws in submission order,
//     Present last).
//   - passcoalesce ON: two mergeable passes collapse to ONE BeginPass/EndPass
//     spanning both passes' draws in submission order (fewer pass-open ops).
//   - DCE ON marking a pass dead: that pass's ops are ABSENT from the plan.
//   - determinism: plan twice -> identical.
//
// It does NOT call executeLinearization() — that is the device-gated executor
// (no MTLDevice in the native test host; same constraint A8 documented). The
// executor is covered by inspection + the header/.cpp boundary comment and is
// wired/tested device-side by B12.

#include "../../../src/dxmt9/framegraph/fg_builder.hpp"
#include "../../../src/dxmt9/framegraph/fg_dag.hpp"
#include "../../../src/dxmt9/framegraph/fg_linearizer.hpp"
#include "../../../src/dxmt9/framegraph/fg_optimizer.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

// --- ChunkSlot SoA fixture helpers (mirrors fg_builder_spec.cpp) -----------

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

// --- Op-sequence helpers ---------------------------------------------------

std::vector<LinearOpKind> kindsOf(const LinearizationPlan& plan) {
  std::vector<LinearOpKind> kinds;
  kinds.reserve(plan.ops.size());
  for (const auto& op : plan.ops) {
    kinds.push_back(op.kind);
  }
  return kinds;
}

std::size_t countOf(const LinearizationPlan& plan, LinearOpKind kind) {
  std::size_t n = 0;
  for (const auto& op : plan.ops) {
    if (op.kind == kind) {
      ++n;
    }
  }
  return n;
}

// clear(rt0/ds) -> 2 draws to rt0/ds -> rt1 draw (samples rt0) -> present.
ChunkSlot buildScenario() {
  ChunkSlot slot;
  const Handle rt0{0xA000u};
  const Handle ds{0xD000u};
  const Handle rt1{0xB000u};

  appendClearColor(slot, rt0, ds);
  appendDrawRun(slot, rt0, ds, /*sampled=*/{}, /*paramCount=*/3);
  appendDrawRun(slot, rt0, ds, /*sampled=*/{}, /*paramCount=*/2);
  appendDrawRun(slot, rt1, {}, /*sampled=*/rt0, /*paramCount=*/1);
  appendPresent(slot, rt1);
  return slot;
}

// --- Tests -----------------------------------------------------------------

// PARITY BASELINE: default optimizer -> plan reproduces natural pass/draw order.
void testParityBaselineOrder() {
  const ChunkSlot slot = buildScenario();
  FrameGraph graph = buildFrameGraph(slot, /*frame_id=*/60);
  runOptimizer(graph, OptimizerOptions{});  // default: no feature passes.

  const LinearizationPlan plan = planLinearization(graph);

  // Builder: pass0 = clear+2 draws (rt0/ds), pass1 = rt1 draw, pass2 = present.
  // Expected op order:
  //   BeginPass(0) EmitDraw EmitDraw EndPass(0)
  //   BeginPass(1) EmitDraw EndPass(1)
  //   Present(2)
  const std::vector<LinearOpKind> expected = {
      LinearOpKind::BeginPass, LinearOpKind::EmitDraw, LinearOpKind::EmitDraw,
      LinearOpKind::EndPass,   LinearOpKind::BeginPass, LinearOpKind::EmitDraw,
      LinearOpKind::EndPass,   LinearOpKind::Present,
  };
  check(kindsOf(plan) == expected,
        "default optimizer plan reproduces natural pass/draw order (parity "
        "baseline, R-BACK-39.1)");

  // Present is last.
  check(!plan.ops.empty() && plan.ops.back().kind == LinearOpKind::Present,
        "Present is the final op");

  // Draws preserve submission order: pass0 owns draw refs to commands 1 then 2;
  // pass1 owns the rt1 draw (command 3).
  std::vector<std::uint32_t> drawCommandOrder;
  for (const auto& op : plan.ops) {
    if (op.kind == LinearOpKind::EmitDraw) {
      drawCommandOrder.push_back(op.draw.command_index);
    }
  }
  const std::vector<std::uint32_t> expectedDraws = {1u, 2u, 3u};
  check(drawCommandOrder == expectedDraws,
        "EmitDraw ops preserve original draw submission order");

  // The first pass opened with a Clear load (folded clear, A3 path).
  bool firstBeginHasClear = false;
  for (const auto& op : plan.ops) {
    if (op.kind == LinearOpKind::BeginPass) {
      firstBeginHasClear = (op.load_store.color_load[0] == LoadAction::Clear);
      break;
    }
  }
  check(firstBeginHasClear,
        "pass0 BeginPass carries LoadAction::Clear for the folded clear");

  const ReplayCommandPlan replay = planReplayCommands(graph, slot);
  check(replay.valid && !replay.reordered,
        "baseline replay command plan is a valid identity permutation");
  check(replay.command_indices ==
            std::vector<std::uint32_t>({0u, 1u, 2u, 3u, 4u}),
        "baseline replay command plan retains Clear/DrawRun/Present order");
}

// passcoalesce ON: two same-target render passes separated by an independent
// pass coalesce into one BeginPass/EndPass with both passes' draws in order.
void testPassCoalesceFewerBeginOps() {
  // pass0 (rtA) -> pass1 (rtB, independent) -> pass2 (rtA again, no dep on rtB).
  // passcoalesce can move pass1 out and merge pass0+pass2.
  ChunkSlot slot;
  const Handle rtA{0xA000u};
  const Handle rtB{0xB000u};
  appendDrawRun(slot, rtA, {}, /*sampled=*/{}, /*paramCount=*/1);  // cmd0
  appendDrawRun(slot, rtB, {}, /*sampled=*/{}, /*paramCount=*/1);  // cmd1
  appendDrawRun(slot, rtA, {}, /*sampled=*/{}, /*paramCount=*/1);  // cmd2

  // Baseline (no coalesce): 3 render passes -> 3 BeginPass ops.
  FrameGraph base = buildFrameGraph(slot, 0);
  runOptimizer(base, OptimizerOptions{});
  const LinearizationPlan basePlan = planLinearization(base);
  check(countOf(basePlan, LinearOpKind::BeginPass) == 3,
        "baseline: three separate render passes -> three BeginPass ops");

  // Coalesced.
  FrameGraph coalesced = buildFrameGraph(slot, 0);
  OptimizerOptions opts{};
  opts.passcoalesce = true;
  runOptimizer(coalesced, opts);
  const LinearizationPlan coPlan = planLinearization(coalesced);

  check(countOf(coPlan, LinearOpKind::BeginPass) <
            countOf(basePlan, LinearOpKind::BeginPass),
        "passcoalesce produces fewer BeginPass ops than the baseline");

  // Total EmitDraw count is unchanged (no draw dropped/duplicated).
  check(countOf(coPlan, LinearOpKind::EmitDraw) ==
            countOf(basePlan, LinearOpKind::EmitDraw),
        "passcoalesce preserves the total draw count");

  // The merged rtA pass keeps both rtA draws (cmd0 then cmd2) in submission
  // order, contiguously inside one BeginPass/EndPass span.
  bool sawMergedSpan = false;
  for (std::size_t i = 0; i + 3 < coPlan.ops.size(); ++i) {
    if (coPlan.ops[i].kind == LinearOpKind::BeginPass &&
        coPlan.ops[i + 1].kind == LinearOpKind::EmitDraw &&
        coPlan.ops[i + 2].kind == LinearOpKind::EmitDraw &&
        coPlan.ops[i + 3].kind == LinearOpKind::EndPass &&
        coPlan.ops[i + 1].draw.command_index == 0u &&
        coPlan.ops[i + 2].draw.command_index == 2u) {
      sawMergedSpan = true;
      break;
    }
  }
  check(sawMergedSpan,
        "merged rtA pass holds both rtA draws (cmd0, cmd2) in submission order "
        "within one BeginPass/EndPass");

  const ReplayCommandPlan replay = planReplayCommands(coalesced, slot);
  check(replay.valid && replay.reordered,
        "passcoalesce produces a valid reordered v2 replay command plan");
  check(replay.command_indices ==
            std::vector<std::uint32_t>({0u, 2u, 1u}),
        "replay command plan groups the two rtA DrawRuns before rtB");
}

void testPassCoalesceReplayCarriesInterveningClear() {
  ChunkSlot slot;
  const Handle rtA{0xA000u};
  const Handle rtB{0xB000u};
  appendClearColor(slot, rtA);                                // cmd0
  appendDrawRun(slot, rtA, {}, {}, 1);                       // cmd1
  appendClearColor(slot, rtB);                                // cmd2
  appendDrawRun(slot, rtB, {}, {}, 1);                       // cmd3
  appendDrawRun(slot, rtA, {}, {}, 1);                       // cmd4

  FrameGraph graph = buildFrameGraph(slot, 0);
  OptimizerOptions opts{};
  opts.passcoalesce = true;
  OptimizerStats stats{};
  runOptimizer(graph, opts, nullptr, &stats);

  check(stats.pass_coalesced_count == 1,
        "same-target re-entry without a second clear coalesces");
  const ReplayCommandPlan replay = planReplayCommands(graph, slot);
  check(replay.valid && replay.reordered,
        "clear-carry replay plan is valid and reordered");
  check(replay.command_indices ==
            std::vector<std::uint32_t>({0u, 1u, 4u, 2u, 3u}),
        "the intervening clear moves together with its offscreen pass");

  ChunkSlot blocked;
  appendClearColor(blocked, rtA);                             // cmd0
  appendDrawRun(blocked, rtA, {}, {}, 1);                    // cmd1
  appendDrawRun(blocked, rtB, {}, {}, 1);                    // cmd2
  appendClearColor(blocked, rtA);                             // cmd3
  appendDrawRun(blocked, rtA, {}, {}, 1);                    // cmd4
  FrameGraph blockedGraph = buildFrameGraph(blocked, 0);
  OptimizerStats blockedStats{};
  runOptimizer(blockedGraph, opts, nullptr, &blockedStats);
  check(blockedStats.pass_coalesced_count == 0,
        "a clear at the head of the second same-target pass blocks coalescing");
}

// DCE ON marking a pass dead: that pass's ops are absent from the plan.
void testDceDropsDeadPassOps() {
  // pass0 writes rtScratch, never read in-chunk, and is fully overwritten by a
  // later same-handle Clear in pass1 (cross-chunk safety (b)). pass1 clears
  // rtScratch then draws -> visible. DCE should drop pass0.
  ChunkSlot slot;
  const Handle rtScratch{0xC000u};
  appendDrawRun(slot, rtScratch, {}, /*sampled=*/{}, /*paramCount=*/1);  // cmd0 (dead)
  appendClearColor(slot, rtScratch);                                     // clear
  appendDrawRun(slot, rtScratch, {}, /*sampled=*/{}, /*paramCount=*/1);  // live draw

  FrameGraph graph = buildFrameGraph(slot, 0);
  OptimizerOptions opts{};
  opts.dce = true;
  OptimizerStats stats{};
  runOptimizer(graph, opts, /*observations=*/nullptr, &stats);

  // Find the dead pass index (the one DCE marked).
  bool anyDead = false;
  u32 deadPassIndex = 0;
  for (std::size_t p = 0; p < graph.passes.size(); ++p) {
    if (graph.passes[p].flags.dead) {
      anyDead = true;
      deadPassIndex = static_cast<u32>(p);
    }
  }
  check(anyDead && stats.dce_dropped >= 1,
        "DCE marked at least one pass dead in this scenario");

  const LinearizationPlan plan = planLinearization(graph);
  for (const auto& op : plan.ops) {
    check(op.pass_index != deadPassIndex,
          "no plan op references the DCE-dropped pass");
  }

  // The live draw (cmd2) is still emitted.
  bool sawLiveDraw = false;
  for (const auto& op : plan.ops) {
    if (op.kind == LinearOpKind::EmitDraw && op.draw.command_index == 2u) {
      sawLiveDraw = true;
    }
  }
  check(sawLiveDraw, "the live draw survives DCE");
}

void testDeterminism() {
  const ChunkSlot slot = buildScenario();
  FrameGraph graph = buildFrameGraph(slot, 60);
  runOptimizer(graph, OptimizerOptions{});

  const LinearizationPlan a = planLinearization(graph);
  const LinearizationPlan b = planLinearization(graph);
  check(a == b, "deterministic: plan twice -> identical");

  // In-place variant matches the value-returning form.
  LinearizationPlan reused;
  reused.ops.push_back(LinearOp{});  // dirty it first.
  planLinearization(graph, reused);
  check(reused == a, "in-place plan matches value plan (reset)");
}

}  // namespace

int main() {
  try {
    testParityBaselineOrder();
    testPassCoalesceFewerBeginOps();
    testPassCoalesceReplayCarriesInterveningClear();
    testDceDropsDeadPassOps();
    testDeterminism();
  } catch (const TestFailure& failure) {
    std::cerr << "fg_linearizer_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "fg_linearizer_spec unexpected exception: " << ex.what()
              << '\n';
    return 1;
  }
  std::cout << "fg_linearizer_spec passed\n";
  return 0;
}
