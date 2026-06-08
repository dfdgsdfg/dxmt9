// §5.1 Dead-Pass Elimination, chunk-conservative (Task B3, L1).
// See ../fg_optimizer.hpp.
//
// DEFAULT OFF (OptimizerOptions.dce defaults false). When the dce feature token
// is absent, no pass is ever dropped — every pass linearizes, which is the
// parity baseline. This function only runs when the orchestrator gates it on.
//
// The Frame Graph window is one CommandChunk (R-BACK-32.1). A pass whose written
// output is unread inside the chunk MAY still be read by a future chunk, so DCE
// cannot prove cross-chunk dead-ness from the per-chunk DAG alone. A pass is
// dead ONLY when ALL conservative gates hold (design §5.1):
//   - it writes a resource that nothing in the chunk reads after the write,
//   - none of its written resources has CPU readback (readback_seen == 0),
//   - kind != Present,
//   - no occlusion / event query record (R-BACK-38.2),
//   - not lock-bearing (a lock forces a flush boundary and side effects),
//   - not a user-requested debug marker pass,
//   - CROSS-CHUNK SAFETY: at least one written resource is memoryless-eligible
//     (ResidencyClass::Memoryless — prior-frame observation proved no cross-chunk
//     read of its stored contents) OR is provably fully overwritten later in the
//     SAME chunk (a same-chunk Clear of the same handle in a later pass).
// Otherwise the pass stays alive (counted as preserved-unprovable).
//
// Dead passes are MARKED (PassNode.flags.dead = true) and stay in the array for
// counter reporting; the linearizer excludes them. We never reindex here.
//
// DETERMINISM (R-BACK-32.2): pure scan, no clock/thread/RNG. The real perf
// counters land in B11; for now decisions surface through OptimizerStats.

#include "../fg_optimizer.hpp"

#include <cstddef>

namespace dxmt9::framegraph {

namespace {

// Does any access read `handle` strictly after pass `write_pass` within the
// chunk? Reads at or before the write do not keep the write alive.
bool readAfterWriteInChunk(const ResourceNode& node, u32 write_pass) {
  for (const AccessLog& access : node.accesses) {
    const auto kind = static_cast<AccessKind>(access.access_kind);
    if ((kind == AccessKind::Read || kind == AccessKind::ReadWrite) &&
        access.pass_index > write_pass) {
      return true;
    }
  }
  return false;
}

// Is `handle` provably fully overwritten by a later same-chunk Clear (a
// same-handle Clear access in a pass after `write_pass`)? This is the
// conservative full-overwrite proof from §5.1 cross-chunk safety (b).
bool fullyOverwrittenLaterInChunk(const ResourceNode& node, u32 write_pass) {
  for (const AccessLog& access : node.accesses) {
    if (static_cast<AccessKind>(access.access_kind) == AccessKind::Clear &&
        access.pass_index > write_pass) {
      return true;
    }
  }
  return false;
}

}  // namespace

void runDce(FrameGraph& graph, OptimizerStats* stats) {
  for (std::size_t p = 0; p < graph.passes.size(); ++p) {
    PassNode& pass = graph.passes[p];
    const u32 pass_index = static_cast<u32>(p);

    // Hard protections — never drop these regardless of read shape.
    if (pass.kind == PassKind::Present || pass.flags.contains_occlusion_query ||
        pass.flags.contains_event_query || pass.flags.contains_lock ||
        pass.flags.debug_marker || pass.flags.dead) {
      continue;
    }
    // Only render/blit passes write resources; a pass with no writes has no
    // dead output to eliminate (and a Sync pass is structurally load-bearing).
    if (pass.kind == PassKind::Present || pass.kind == PassKind::Sync) {
      continue;
    }

    // Collect this pass's written resources. A pass is a drop candidate only if
    // it writes at least one resource and EVERY written resource is dead.
    bool wrote_anything = false;
    bool all_writes_dead = true;
    bool any_cross_chunk_safe = false;
    bool readback_on_any_write = false;

    for (const ResourceNode& node : graph.resources) {
      // Did THIS pass write the resource?
      bool pass_wrote = false;
      for (const AccessLog& access : node.accesses) {
        const auto kind = static_cast<AccessKind>(access.access_kind);
        if (access.pass_index == pass_index &&
            (kind == AccessKind::Write || kind == AccessKind::ReadWrite ||
             kind == AccessKind::Clear)) {
          pass_wrote = true;
          break;
        }
      }
      if (!pass_wrote) {
        continue;
      }
      wrote_anything = true;

      if (node.classifier_flags.readback_seen) {
        readback_on_any_write = true;
      }
      if (readAfterWriteInChunk(node, pass_index)) {
        all_writes_dead = false;
      }
      // Cross-chunk safety (a) memoryless-eligible, or (b) fully overwritten
      // later in this chunk.
      if (node.residency == ResidencyClass::Memoryless ||
          fullyOverwrittenLaterInChunk(node, pass_index)) {
        any_cross_chunk_safe = true;
      }
    }

    if (!wrote_anything) {
      continue;
    }

    const bool droppable = all_writes_dead && !readback_on_any_write &&
                           any_cross_chunk_safe;
    if (droppable) {
      pass.flags.dead = true;
      if (stats) {
        ++stats->dce_dropped;
      }
    } else {
      if (stats) {
        ++stats->dce_preserved_unprovable;
      }
    }
  }
}

}  // namespace dxmt9::framegraph
