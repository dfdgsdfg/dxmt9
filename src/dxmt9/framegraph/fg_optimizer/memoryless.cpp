// §5.3 Memoryless virtual-attachment classifier (Task B6, R-BACK-33, L1).
// See ../fg_optimizer.hpp.
//
// SEMANTIC RELAXATION (R-BACK-40.4) — feature-gated AND only when explicitly
// allowed by OptimizerOptions.memoryless. This pass is excluded from the
// byte-exact parity gate by construction.
//
// DEVICE-GATED BOUNDARY.
//   The TESTABLE deliverable here is the CLASSIFIER + residency decision: it
//   marks an eligible ResourceNode ResidencyClass::Memoryless. It does NOT:
//     - allocate an MTLStorageModeMemoryless alias (TransientAttachmentPool::
//       acquire is device-dependent and cannot run in the native test host),
//     - rewrite PassNode.targets to point at the alias,
//     - touch or reallocate the persistent surface (R-BACK-33.1).
//   The alias acquisition + targets rewrite happen later, in the linearizer /
//   backend, behind the §5.3 TransientAttachmentPool interface declared in the
//   header. Splitting it this way keeps the eligibility logic deterministic and
//   unit-testable without Wine/Metal (R-BACK-32.2).
//
// Per R-BACK-33.2 / design §5.3, a ResourceNode is promoted only when ALL hold:
//   1. its per-surface observation counter reached the threshold
//      (DXMT9_RENDERER_MEMORYLESS_OBSERVATION_FRAMES, default 8) — prior-frame
//      observation gate; the first N frames after device creation never promote,
//   2. the current chunk shows no lock / readback / cross-pass read,
//   3. it is not bound as the swap-chain backbuffer,
//   4. first_use_pass == last_use_pass AFTER passcoalesce (memoryless content
//      does not survive a Metal render-pass boundary).
// Failing any current-chunk gate skips promotion (the backend resets the
// cross-frame counter; this pass only reports the decision).

#include "../fg_optimizer.hpp"

#include <cstddef>

namespace dxmt9::framegraph {

namespace {

// Find the cross-frame observation for a handle, or nullptr if the backend has
// not started observing it yet (treated as not-yet-eligible).
const MemorylessObservation* findObservation(
    const std::vector<MemorylessObservation>& observations,
    ResourceHandle handle) {
  for (const MemorylessObservation& obs : observations) {
    if (obs.handle == handle) {
      return &obs;
    }
  }
  return nullptr;
}

// Current-chunk gate (R-BACK-33.2 step 2): the surface must not have been
// locked, read back, or read across passes within this chunk. lock_seen /
// readback_seen come straight off the builder's classifier flags; the
// cross-pass read is derived from the lifetime span (touched in >1 pass already
// disqualifies via step 4, but an explicit read in a different pass than the
// producing write is the canonical cross-pass-read signal).
bool currentChunkClean(const ResourceNode& node) {
  if (node.classifier_flags.lock_seen || node.classifier_flags.readback_seen) {
    return false;
  }
  // A read in a pass other than the producing-write pass is a cross-pass read.
  // Determine the (single) producer pass; any Read in a different pass fails.
  bool have_producer = false;
  u32 producer_pass = 0;
  for (const AccessLog& access : node.accesses) {
    const auto kind = static_cast<AccessKind>(access.access_kind);
    if (kind == AccessKind::Write || kind == AccessKind::ReadWrite ||
        kind == AccessKind::Clear) {
      have_producer = true;
      producer_pass = access.pass_index;
    }
  }
  if (!have_producer) {
    return true;  // read-only surface in this chunk: no produced content to lose.
  }
  for (const AccessLog& access : node.accesses) {
    const auto kind = static_cast<AccessKind>(access.access_kind);
    if ((kind == AccessKind::Read || kind == AccessKind::ReadWrite) &&
        access.pass_index != producer_pass) {
      return false;  // cross-pass read of produced content.
    }
  }
  return true;
}

}  // namespace

void markMemorylessCandidates(
    FrameGraph& graph, const std::vector<MemorylessObservation>& observations,
    u32 observation_threshold, OptimizerStats* stats) {
  for (ResourceNode& node : graph.resources) {
    // Step 4: must collapse to a single pass (post-passcoalesce lifetime). A
    // surface touched across passes can never be memoryless.
    if (!resourceIsTransient(node)) {
      continue;
    }

    // Step 2: current-chunk gates (lock / readback / cross-pass read).
    if (node.classifier_flags.lock_seen) {
      if (stats) {
        ++stats->memoryless_dropped_via_lock;
      }
      continue;
    }
    if (node.classifier_flags.readback_seen) {
      if (stats) {
        ++stats->memoryless_dropped_via_readback;
      }
      continue;
    }
    if (!currentChunkClean(node)) {
      continue;
    }

    const MemorylessObservation* obs = findObservation(observations, node.handle);

    // Step 3: never promote a swap-chain backbuffer (its content is presented).
    if (obs && obs->bound_as_backbuffer) {
      continue;
    }

    // Step 1: prior-frame observation gate. No observation record, or an
    // under-threshold one, blocks promotion. The first N frames after device
    // creation cannot promote (R-BACK-33.2).
    const u32 frames = obs ? obs->observation_frames : 0u;
    if (frames < observation_threshold) {
      if (stats) {
        ++stats->memoryless_blocked_observation;
      }
      continue;
    }

    // All gates pass: promote the residency decision. The DEVICE-GATED alias
    // acquisition + PassNode.targets rewrite happen later in the linearizer.
    node.residency = ResidencyClass::Memoryless;
    if (stats) {
      ++stats->memoryless_promoted;
    }
  }
}

}  // namespace dxmt9::framegraph
