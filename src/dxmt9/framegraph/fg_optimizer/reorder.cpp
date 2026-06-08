// §5.6 Dependency-Respecting Reorder (Task B8, L1). See ../fg_optimizer.hpp.
//
// Topological sort over the dependency edge set with an INTEGER state-change
// cost tiebreaker. No floating point (R-BACK-34.2 / §5.6). Feature-gated.
//
// The sort is Kahn's algorithm:
//   - in_degree[p] counts incoming dependency edges (a producer P_src must run
//     before consumer P_dst),
//   - among all currently-ready passes (in_degree == 0) we pick the one whose
//     transition from the last-emitted pass costs the LEAST state change; ties
//     break on the original pass index so the result is deterministic and, when
//     no reordering is forced, identity (R-BACK-32.2 determinism).
//
// After computing the new order, EVERY pass-index-keyed field is remapped so the
// graph stays internally consistent for the passes that run after reorder
// (loadstore reads AccessLog.pass_index and the renumbered passes):
//   - graph.passes is permuted,
//   - each AccessLog.pass_index is remapped old->new,
//   - each Edge src/dst pass is remapped old->new,
//   - ResourceNode.first/last_use_pass are recomputed (lifetime would also do
//     this, but we keep the graph self-consistent immediately).
//
// EDGE PRESERVATION: because we only emit a pass once all its producers are
// emitted, no consumer precedes a producer. The edge SET is unchanged (same
// (resource, producer, consumer) triples), only its pass indices are remapped.

#include "../fg_optimizer.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace dxmt9::framegraph {

namespace {

// Small fixed integer state-change cost table (§5.6). The "cost" of emitting a
// pass after the previously-emitted one. L1 has no decoded PSO state, so the
// model is intentionally coarse and audited by the spec golden:
//   - switching attachment set (render target / depth) is the dominant cost,
//   - switching pass KIND (render<->blit<->present) is a mid cost,
//   - same kind + same attachments is free.
// All weights are integers; there is NO floating point anywhere in reorder.
struct StateCostWeights {
  u32 attachment_change = 100;
  u32 kind_change = 10;
  u32 same = 0;
};
constexpr StateCostWeights kWeights{};

u32 transitionCost(const PassNode& from, const PassNode& to) {
  u32 cost = kWeights.same;
  if (from.kind != to.kind) {
    cost += kWeights.kind_change;
  }
  if (!(from.targets == to.targets)) {
    cost += kWeights.attachment_change;
  }
  return cost;
}

}  // namespace

void runReorder(FrameGraph& graph) {
  const std::size_t n = graph.passes.size();
  if (n < 2) {
    return;
  }

  // Build adjacency + in-degree from the dependency edges. A self-edge (should
  // not exist per the builder) is ignored so it cannot deadlock the sort.
  std::vector<std::vector<u32>> succ(n);
  std::vector<u32> in_degree(n, 0);
  for (const Edge& e : graph.edges) {
    if (e.src_pass == e.dst_pass || e.src_pass >= n || e.dst_pass >= n) {
      continue;
    }
    succ[e.src_pass].push_back(e.dst_pass);
    ++in_degree[e.dst_pass];
  }

  std::vector<u32> old_to_new(n, 0);
  std::vector<u32> new_order;  // old indices in emission order.
  new_order.reserve(n);
  std::vector<bool> emitted(n, false);

  // last_old is the old index of the previously-emitted pass (for cost), or n
  // when nothing has been emitted yet.
  std::size_t last_old = n;

  for (std::size_t step = 0; step < n; ++step) {
    // Among ready (in_degree==0, not yet emitted) passes, pick min cost; ties
    // break on the smallest ORIGINAL index, giving identity order when no
    // reorder is forced.
    std::size_t best = n;
    u32 best_cost = 0;
    for (std::size_t p = 0; p < n; ++p) {
      if (emitted[p] || in_degree[p] != 0) {
        continue;
      }
      const u32 cost = (last_old == n)
                           ? 0u
                           : transitionCost(graph.passes[last_old], graph.passes[p]);
      if (best == n || cost < best_cost ||
          (cost == best_cost && p < best)) {
        best = p;
        best_cost = cost;
      }
    }
    if (best == n) {
      // Cycle (should not happen for a valid producer->consumer DAG). Bail out
      // without reordering rather than producing a partial order.
      return;
    }
    emitted[best] = true;
    old_to_new[best] = static_cast<u32>(new_order.size());
    new_order.push_back(static_cast<u32>(best));
    for (u32 s : succ[best]) {
      if (in_degree[s] > 0) {
        --in_degree[s];
      }
    }
    last_old = best;
  }

  // Permute the pass array into the new order.
  std::vector<PassNode> reordered;
  reordered.reserve(n);
  for (u32 old_idx : new_order) {
    reordered.push_back(graph.passes[old_idx]);
  }
  graph.passes = std::move(reordered);

  // Remap every pass-index-keyed field old -> new.
  for (ResourceNode& node : graph.resources) {
    for (AccessLog& access : node.accesses) {
      if (access.pass_index < n) {
        access.pass_index = old_to_new[access.pass_index];
      }
    }
    // Recompute lifetime spans from the remapped accesses so the graph is
    // immediately self-consistent (loadstore relies on these).
    if (!node.accesses.empty()) {
      u32 first = node.accesses.front().pass_index;
      u32 last = first;
      for (const AccessLog& access : node.accesses) {
        if (access.pass_index < first) {
          first = access.pass_index;
        }
        if (access.pass_index > last) {
          last = access.pass_index;
        }
      }
      node.first_use_pass = first;
      node.last_use_pass = last;
    }
  }
  for (Edge& e : graph.edges) {
    if (e.src_pass < n) {
      e.src_pass = old_to_new[e.src_pass];
    }
    if (e.dst_pass < n) {
      e.dst_pass = old_to_new[e.dst_pass];
    }
  }
}

}  // namespace dxmt9::framegraph
