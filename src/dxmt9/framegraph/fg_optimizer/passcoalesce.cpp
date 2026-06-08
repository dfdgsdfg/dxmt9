// §5.4 Pass Coalescing (Task B5, R-BACK-34, L1). See ../fg_optimizer.hpp.
//
// Feature-gated. CONSERVATIVE: when in doubt, do NOT merge.
//
// Goal: pairs (P_a, P_b) with matching AttachmentSet are candidates. For the set
// of intervening passes I = {P_x | a < x < b}, the pair is coalesced only when
// every P_x can be moved before P_a OR after P_b WITHOUT breaking a dependency
// edge. The merged pass keeps P_a's and P_b's draws in submission order; the
// intervening passes are pushed before P_a or after P_b. (R-BACK-34.1-34.4.)
//
// SOUNDNESS via reachability over the dependency DAG (Edge = producer->consumer):
//   - P_x can move AFTER P_b iff nothing in [a..b] (P_a, P_b, or another
//     intervening pass that itself stays before b) depends on P_x — i.e. P_x is
//     not a producer reachable-from-required by P_a/P_b. We require: P_x is not
//     reachable FROM P_a along edges, and P_b does not reach back... simplified
//     to: no path P_x ->* P_a and no path P_x ->* P_b (P_x produces nothing a/b
//     consume) — then P_x has no consumer in the pair and can sit after P_b.
//   - P_x can move BEFORE P_a iff P_x does not depend on P_a or P_b or any pass
//     that must stay between them — i.e. no path P_a ->* P_x and no path
//     P_b ->* P_x (nothing the pair produces feeds P_x).
//   If a P_x satisfies NEITHER, the pair is rejected (conservative).
//   A pass that is BOTH a consumer of the pair and a producer for the pair is
//   inherently un-movable, so the pair is rejected.
//
// FLUSH BOUNDARY (R-BACK-34.5): coalescing never crosses graph.flush_boundary.
// At L1 the only flush boundary the builder emits is a trailing Present pass;
// we therefore never coalesce across a Present pass index. A Present between the
// pair makes I contain a non-render pass, which we reject outright.
//
// COST (R-BACK-34.2): integer-only. At L1, true reorder cost is zero (the
// intervening passes keep their relative order, only relocate around the pair),
// and the preserved-tile benefit of merging two same-attachment passes is always
// >= 0, so any safely-reorderable candidate is profitable. No floating point.
//
// DETERMINISM (R-BACK-32.2): scans pairs in ascending (a, b) order, takes the
// first profitable+safe pair, rebuilds, and repeats to a fixpoint. No
// clock/thread/RNG.

#include "../fg_optimizer.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace dxmt9::framegraph {

namespace {

// Reachability over the dependency edges: can we get from `from` to `to`
// following producer->consumer edges? (closed under transitivity). n is small
// per-chunk build scratch, so an O(n*edges) BFS per query is fine.
bool reaches(const FrameGraph& graph, u32 from, u32 to) {
  if (from == to) {
    return true;
  }
  const std::size_t n = graph.passes.size();
  std::vector<bool> seen(n, false);
  std::vector<u32> stack;
  stack.push_back(from);
  seen[from] = true;
  while (!stack.empty()) {
    const u32 cur = stack.back();
    stack.pop_back();
    for (const Edge& e : graph.edges) {
      if (e.src_pass != cur) {
        continue;
      }
      if (e.dst_pass == to) {
        return true;
      }
      if (e.dst_pass < n && !seen[e.dst_pass]) {
        seen[e.dst_pass] = true;
        stack.push_back(e.dst_pass);
      }
    }
  }
  return false;
}

enum class Move { Before, After, Blocked };

// Decide whether intervening pass x (a < x < b) can move before P_a or after
// P_b without breaking an edge. See file header for the rule.
Move classifyIntervening(const FrameGraph& graph, u32 a, u32 b, u32 x) {
  // Does the pair produce something x consumes? (pair ->* x) -> x must stay AFTER
  // the pair start, so it cannot move before P_a.
  const bool pair_feeds_x = reaches(graph, a, x) || reaches(graph, b, x);
  // Does x produce something the pair consumes? (x ->* pair) -> x must stay
  // BEFORE the pair end, so it cannot move after P_b.
  const bool x_feeds_pair = reaches(graph, x, a) || reaches(graph, x, b);

  if (pair_feeds_x && x_feeds_pair) {
    return Move::Blocked;  // wedged inside the pair; cannot coalesce.
  }
  if (!x_feeds_pair) {
    return Move::After;  // no consumer in the pair -> relocate after P_b.
  }
  // x_feeds_pair && !pair_feeds_x: x only produces for the pair -> before P_a.
  return Move::Before;
}

// Attempt to coalesce the first safe+profitable matching pair. Returns true and
// mutates the graph (pass order + draws + edges) if a merge happened.
bool coalesceOnce(FrameGraph& graph, OptimizerStats* stats) {
  const std::size_t n = graph.passes.size();
  for (std::size_t a = 0; a < n; ++a) {
    const PassNode& pa = graph.passes[a];
    if (pa.kind != PassKind::Render || pa.flags.dead) {
      continue;
    }
    for (std::size_t b = a + 1; b < n; ++b) {
      const PassNode& pb = graph.passes[b];
      if (pb.kind != PassKind::Render || pb.flags.dead) {
        continue;
      }
      if (!(pa.targets == pb.targets)) {
        continue;
      }
      // R-BACK-34.5 / L1 flush boundary: reject if any intervening pass is not
      // a plain render pass (Present/Blit/Sync may be flush-bearing).
      bool intervening_ok = true;
      std::vector<u32> before;  // intervening passes that go before P_a.
      std::vector<u32> after;   // intervening passes that go after P_b.
      for (std::size_t x = a + 1; x < b; ++x) {
        const PassNode& px = graph.passes[x];
        if (px.kind != PassKind::Render || px.flags.dead) {
          intervening_ok = false;
          break;
        }
        const Move m = classifyIntervening(graph, static_cast<u32>(a),
                                           static_cast<u32>(b),
                                           static_cast<u32>(x));
        if (m == Move::Blocked) {
          intervening_ok = false;
          break;
        }
        if (m == Move::Before) {
          before.push_back(static_cast<u32>(x));
        } else {
          after.push_back(static_cast<u32>(x));
        }
      }
      if (!intervening_ok) {
        continue;
      }

      // Safe + profitable -> build the new pass order:
      //   [ passes < a, except those moved ]
      //   [ before-movers (kept in relative order) ]
      //   [ merged pass = P_a draws then P_b draws ]
      //   [ after-movers (kept in relative order) ]
      //   [ passes > b ]
      // The intervening set is exactly `before` ++ `after`; passes outside
      // (a, b) keep their positions relative to everything else.
      const u32 reorder_distance = static_cast<u32>(b - a);

      // 1. Assemble the merged draw list: P_a's refs then P_b's refs.
      std::vector<DrawRef> merged_draws;
      merged_draws.reserve(pa.draws.count + pb.draws.count);
      for (u32 i = 0; i < pa.draws.count; ++i) {
        merged_draws.push_back(graph.draws[pa.draws.first + i]);
      }
      for (u32 i = 0; i < pb.draws.count; ++i) {
        merged_draws.push_back(graph.draws[pb.draws.first + i]);
      }

      // 2. Build the new pass order as a list of "sources": each entry is either
      //    an existing pass index to copy verbatim, or the synthetic merged pass.
      constexpr u32 kMergedMarker = 0xFFFFFFFFu;
      std::vector<u32> new_pass_src;
      new_pass_src.reserve(n);
      const auto in_set = [](const std::vector<u32>& v, std::size_t idx) {
        return std::find(v.begin(), v.end(), static_cast<u32>(idx)) != v.end();
      };
      for (std::size_t p = 0; p < n; ++p) {
        if (p == a) {
          // Emit before-movers, then the merged pass, in place of P_a.
          for (u32 m : before) {
            new_pass_src.push_back(m);
          }
          new_pass_src.push_back(kMergedMarker);
          continue;
        }
        if (p == b) {
          // Emit after-movers in place of P_b (P_b is folded into the merge).
          for (u32 m : after) {
            new_pass_src.push_back(m);
          }
          continue;
        }
        if (p > a && p < b && (in_set(before, p) || in_set(after, p))) {
          continue;  // moved; emitted next to the pair.
        }
        new_pass_src.push_back(static_cast<u32>(p));
      }

      // 3. Materialize the new passes + a freshly compacted draws array, so each
      //    pass's draws.first/count is contiguous and valid.
      std::vector<PassNode> new_passes;
      std::vector<DrawRef> new_draws;
      std::vector<u32> old_to_new(n, 0xFFFFFFFFu);
      new_passes.reserve(new_pass_src.size());

      for (u32 src : new_pass_src) {
        const u32 new_idx = static_cast<u32>(new_passes.size());
        if (src == kMergedMarker) {
          PassNode merged = pa;  // inherit P_a's attachments/kind/state.
          merged.draws.first = static_cast<u32>(new_draws.size());
          merged.draws.count = static_cast<u32>(merged_draws.size());
          for (const DrawRef& d : merged_draws) {
            new_draws.push_back(d);
          }
          // The merged pass occupies BOTH old a and old b slots for remap.
          old_to_new[a] = new_idx;
          old_to_new[b] = new_idx;
          new_passes.push_back(merged);
        } else {
          const PassNode& orig = graph.passes[src];
          PassNode copy = orig;
          copy.draws.first = static_cast<u32>(new_draws.size());
          copy.draws.count = orig.draws.count;
          for (u32 i = 0; i < orig.draws.count; ++i) {
            new_draws.push_back(graph.draws[orig.draws.first + i]);
          }
          old_to_new[src] = new_idx;
          new_passes.push_back(copy);
        }
      }

      // 4. Remap pass-index-keyed fields (access logs, edges) old -> new. The
      //    merged pass takes the access entries of both old a and old b.
      for (ResourceNode& node : graph.resources) {
        for (AccessLog& access : node.accesses) {
          if (access.pass_index < n &&
              old_to_new[access.pass_index] != 0xFFFFFFFFu) {
            access.pass_index = old_to_new[access.pass_index];
          }
        }
      }
      std::vector<Edge> new_edges;
      new_edges.reserve(graph.edges.size());
      for (const Edge& e : graph.edges) {
        if (e.src_pass >= n || e.dst_pass >= n) {
          continue;
        }
        const u32 ns = old_to_new[e.src_pass];
        const u32 nd = old_to_new[e.dst_pass];
        if (ns == 0xFFFFFFFFu || nd == 0xFFFFFFFFu || ns == nd) {
          continue;  // dropped self-edge (e.g. a->b internal to the merge).
        }
        // Suppress duplicate edges introduced by the merge.
        bool dup = false;
        for (const Edge& existing : new_edges) {
          if (existing.src_pass == ns && existing.dst_pass == nd &&
              existing.resource == e.resource) {
            dup = true;
            break;
          }
        }
        if (!dup) {
          new_edges.push_back(Edge{.src_pass = ns, .dst_pass = nd,
                                   .resource = e.resource});
        }
      }

      graph.passes = std::move(new_passes);
      graph.draws = std::move(new_draws);
      graph.edges = std::move(new_edges);

      if (stats) {
        ++stats->pass_coalesced_count;
        stats->pass_coalesce_reorder_distance_max =
            std::max(stats->pass_coalesce_reorder_distance_max, reorder_distance);
      }
      return true;
    }
  }
  return false;
}

}  // namespace

void runPassCoalesce(FrameGraph& graph, OptimizerStats* stats) {
  // Iterate to a fixpoint: each successful merge can expose a new adjacent pair.
  // Bounded by pass count (each merge reduces the render-pass count by one).
  std::size_t guard = graph.passes.size();
  while (guard-- > 0 && coalesceOnce(graph, stats)) {
  }
  // Lifetime spans are now stale (pass indices changed); the orchestrator does
  // not re-run lifetime before memoryless, so recompute here to keep first/last
  // authoritative for the memoryless single-pass-collapse gate (§5.3 step 4).
  runLifetime(graph);
}

}  // namespace dxmt9::framegraph
