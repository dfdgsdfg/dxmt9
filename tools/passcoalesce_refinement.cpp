#include "dxmt9/framegraph/fg_optimizer_fixture.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace dxmt9::framegraph;
namespace dxmt9::framegraph::reference {
void runPassCoalesce(FrameGraph&, OptimizerStats*, bool, ActiveSeedMergeWitnessSink*);
}
namespace {
void require(bool b) {
  if (!b) throw std::runtime_error("pass-coalescing refinement mismatch");
}
}
void checkOptimizerStats(const OptimizerStats& a, const OptimizerStats& b) {
  require(a.dce_dropped == b.dce_dropped);
  require(a.dce_preserved_unprovable == b.dce_preserved_unprovable);
  require(a.pass_coalesced_count == b.pass_coalesced_count);
  require(a.pass_coalesce_reorder_distance_max == b.pass_coalesce_reorder_distance_max);
  require(a.pass_coalesce_blocked_cycle == b.pass_coalesce_blocked_cycle);
  require(a.pass_coalesce_second_non_draw == b.pass_coalesce_second_non_draw);
  require(a.pass_coalesce_return_candidates == b.pass_coalesce_return_candidates);
  require(a.pass_coalesce_return_merged == b.pass_coalesce_return_merged);
  require(a.pass_coalesce_return_blocked_cycle == b.pass_coalesce_return_blocked_cycle);
  require(a.pass_coalesce_return_second_non_draw == b.pass_coalesce_return_second_non_draw);
  require(a.pass_coalesce_return_non_render_intervener == b.pass_coalesce_return_non_render_intervener);
  require(a.pass_coalesce_return_missing_invariant == b.pass_coalesce_return_missing_invariant);
  require(a.pass_coalesce_return_dependency_kept == b.pass_coalesce_return_dependency_kept);
  require(a.pass_coalesce_return_move_before == b.pass_coalesce_return_move_before);
  require(a.pass_coalesce_return_move_after == b.pass_coalesce_return_move_after);
  require(a.pass_coalesce_return_non_draw_intervener == b.pass_coalesce_return_non_draw_intervener);
  require(a.pass_coalesce_return_semantic_intervener == b.pass_coalesce_return_semantic_intervener);
  require(a.pass_coalesce_return_commandless_intervener == b.pass_coalesce_return_commandless_intervener);
  require(a.pass_coalesce_return_commandless == b.pass_coalesce_return_commandless);
  require(a.pass_coalesce_active_seed_merge_count == b.pass_coalesce_active_seed_merge_count);
  require(a.pass_coalesce_active_seed_merge_distance_total == b.pass_coalesce_active_seed_merge_distance_total);
  require(a.pass_coalesce_active_seed_merge_distance_max == b.pass_coalesce_active_seed_merge_distance_max);
  require(a.pass_coalesce_active_seed_command_before == b.pass_coalesce_active_seed_command_before);
  require(a.pass_coalesce_active_seed_command_after == b.pass_coalesce_active_seed_command_after);
  require(a.pass_coalesce_active_seed_empty_intervening == b.pass_coalesce_active_seed_empty_intervening);
  require(a.memoryless_promoted == b.memoryless_promoted);
  require(a.memoryless_blocked_observation == b.memoryless_blocked_observation);
  require(a.memoryless_dropped_via_lock == b.memoryless_dropped_via_lock);
  require(a.memoryless_dropped_via_readback == b.memoryless_dropped_via_readback);
}
namespace {
void compare(const FrameGraph& input, bool diagnostics) {
  auto expected = input, actual = input;
  OptimizerStats a{}, b{};
  std::array<dxmt9::encoders::ActiveSeedMergeCommandWitness, 8> wa{}, wb{};
  ActiveSeedMergeWitnessSink sa{wa}, sb{wb};
  reference::runPassCoalesce(expected, &a, diagnostics, &sa);
  runPassCoalesce(actual, &b, diagnostics, &sb);
  require(equalOptimizerGraph(expected, actual));
  checkOptimizerStats(a, b);
  require(sa.count == sb.count && sa.attempted == sb.attempted && sa.overflow == sb.overflow);
  for (std::size_t i = 0; i < sa.count; ++i) {
    require(wa[i].flattenedCommandIndex == wb[i].flattenedCommandIndex);
    require(wa[i].mergeOrdinal == wb[i].mergeOrdinal);
    require(wa[i].mergeDistance == wb[i].mergeDistance);
  }
}
}
void checkPassCoalesceRefinement() {
  std::size_t cases = 0;
  // All four-node forward DAGs and attachment partitions over two targets.
  // Also all three-node directed graphs (cycles and self edges included).
  for (bool cyclic : {false, true}) {
    const u32 n = cyclic ? 3 : 4;
    const u32 edgeBits = cyclic ? 9 : 6;
    for (u32 mask = 0; mask < (1u << edgeBits); ++mask) {
      for (u32 targets = 0; targets < (1u << n); ++targets) {
        for (u32 variant = 0; variant < 10; ++variant) {
          FrameGraph graph;
          for (u32 i = 0; i < n; ++i) {
            PassNode p;
            p.targets.color_count = 1;
            p.targets.color[0] = ResourceHandle{1u + ((targets >> i) & 1u)};
            p.commands = {i, 1}; p.draws = {i, 1};
            p.flags.active_render_seed = variant == 1 && i == 0;
            if (i == 1) {
              if (variant == 2) p.kind = PassKind::Sync;
              if (variant == 3) p.flags.dead = true;
              if (variant == 4) p.commands.count = 0;
              if (variant == 5) p.flags.contains_lock = true;
              if (variant == 6) p.flags.contains_event_query = true;
              if (variant == 7) p.flags.contains_occlusion_query = true;
            }
            graph.passes.push_back(p);
            graph.commands.push_back({i, variant == 8 && i == 1
                ? dxmt9::core::MetalCommandKind::Clear : dxmt9::core::MetalCommandKind::DrawRun});
            graph.draws.push_back({i, i, 1});
          }
          u32 bit = 0;
          ResourceNode resource;
          resource.handle = ResourceHandle{1};
          for (u32 i = 0; i < n; ++i) resource.accesses.push_back({i, 1, 1});
          graph.resources.push_back(resource);
          for (u32 a = 0; a < n; ++a) {
            for (u32 b = cyclic ? 0 : a + 1; b < n; ++b, ++bit) {
              if (mask & (1u << bit)) graph.edges.push_back({a, b, ResourceHandle{1}});
            }
          }
          if (variant == 9) {
            graph.edges.push_back({UINT32_MAX, 0, ResourceHandle{1}});
            graph.edges.push_back({0, UINT32_MAX, ResourceHandle{1}});
          }
          for (bool diagnostics : {false, true}) {
            compare(graph, diagnostics);
            ++cases;
          }
        }
      }
    }
  }
  // Cache storage bound falls back to the complete reference traversal.
  // Invalid ranges are tested in an A-B-A window that cannot merge another
  // pair: the reference does not validate unrelated passes during rebuilding.
  for (bool commands : {false, true}) {
    FrameGraph invalid;
    invalid.passes.resize(3);
    for (u32 i = 0; i < 3; ++i) {
      invalid.passes[i].targets.color_count = 1;
      invalid.passes[i].targets.color[0] = ResourceHandle{i == 1 ? 2u : 1u};
    }
    if (commands) invalid.passes[1].commands.count = UINT32_MAX;
    else invalid.passes[1].draws.count = UINT32_MAX;
    compare(invalid, false);
    compare(invalid, true);
    cases += 2;
  }
  FrameGraph oversized;
  oversized.passes.resize(4097);
  for (auto& p : oversized.passes) p.kind = PassKind::Sync;
  for (u32 i : {0u, 1u, 2u}) {
    oversized.passes[i].kind = PassKind::Render;
    oversized.passes[i].targets.color_count = 1;
    oversized.passes[i].targets.color[0] = ResourceHandle{i == 1 ? 2u : 1u};
  }
  oversized.edges = {{0, 1, ResourceHandle{1}}, {1, 2, ResourceHandle{1}}};
  compare(oversized, false);
  for (u32 count : {65u, 129u}) {
    FrameGraph wide;
    wide.passes.resize(count);
    for (u32 i = 0; i < count; ++i) {
      wide.passes[i].targets.color_count = 1;
      wide.passes[i].targets.color[0] = ResourceHandle{i == count - 1 ? 1u : i + 1u};
      if (i != 0) wide.edges.push_back({i - 1, i, ResourceHandle{1}});
    }
    compare(wide, false);
    compare(wide, true);
    wide.edges.clear(); // successful distant merge, then rebuilt query owner
    compare(wide, false);
    compare(wide, true);
    cases += 4;
  }
  FrameGraph edgeBound;
  edgeBound.passes.resize(3);
  for (u32 i = 0; i < 3; ++i) {
    edgeBound.passes[i].targets.color_count = 1;
    edgeBound.passes[i].targets.color[0] = ResourceHandle{i == 1 ? 2u : 1u};
  }
  edgeBound.edges.resize(65537, Edge{0, 1, ResourceHandle{1}});
  edgeBound.edges.push_back({1, 2, ResourceHandle{1}});
  compare(edgeBound, false);
  ++cases;
  std::cout << "pass-coalescing bounded refinement PASS cases=" << cases + 1 << '\n';
}
