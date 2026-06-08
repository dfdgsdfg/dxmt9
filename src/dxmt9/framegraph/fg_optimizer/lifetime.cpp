// §5.2 Resource Lifetime (Task B4, L1). See ../fg_optimizer.hpp.
//
// Recomputes first_use_pass / last_use_pass for every ResourceNode by scanning
// its (chronological) access log. The builder seeds these while walking, but
// later passes (passcoalesce/reorder) renumber passes, so lifetime is the
// authoritative recompute and is ALWAYS run first.
//
// A resource is "transient" when first_use_pass == last_use_pass — exposed via
// resourceIsTransient() in the header rather than a stored bit, because the B1
// ResourceNode (committed) has no transient field and this pass must not widen
// the committed struct.
//
// DETERMINISM (R-BACK-32.2): a pure scan, order-neutral, no clock/thread/RNG.

#include "../fg_optimizer.hpp"

namespace dxmt9::framegraph {

void runLifetime(FrameGraph& graph) {
  for (ResourceNode& node : graph.resources) {
    if (node.accesses.empty()) {
      // No accesses: leave the builder-seeded values (both default 0) so the
      // node degenerates to a single-pass span at pass 0.
      node.first_use_pass = 0;
      node.last_use_pass = 0;
      continue;
    }
    u32 first = node.accesses.front().pass_index;
    u32 last = node.accesses.front().pass_index;
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

}  // namespace dxmt9::framegraph
