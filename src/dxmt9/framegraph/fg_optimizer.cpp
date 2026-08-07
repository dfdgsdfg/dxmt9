// Frame Graph optimizer orchestrator (Task B3-B8, L1). See fg_optimizer.hpp.
//
// Runs the enabled passes in the FIXED R-BACK-32.5 order:
//   lifetime -> passcoalesce -> memoryless -> dce -> reorder -> loadstore
// lifetime and loadstore always run; the middle four are feature-gated.
//
// PARITY (R-BACK-32.6): with default OptimizerOptions{} the only passes that run
// are lifetime + loadstore. Neither reorders passes or draws, so the linearizer
// reproduces the original command order byte-for-byte.

#include "fg_optimizer.hpp"

namespace dxmt9::framegraph {

void runOptimizer(FrameGraph& graph, const OptimizerOptions& options,
                  std::vector<MemorylessObservation>* observations,
                  OptimizerStats* stats,
                  DceLookaheadProof dce_lookahead,
                  ActiveSeedMergeWitnessSink* activeSeedWitnesses) {
  // 1. lifetime — always (input to memoryless/reorder; cheap and order-neutral).
  runLifetime(graph);

  // 2. passcoalesce — gated. Merges matching-attachment pass pairs.
  if (options.passcoalesce) {
    runPassCoalesce(graph, stats,
                    options.collect_passcoalesce_return_diagnostics,
                    activeSeedWitnesses);
  }

  // 3. memoryless — gated. Classifier + residency decision only (the alias
  //    acquisition is the device-gated TransientAttachmentPool boundary).
  if (options.memoryless) {
    static const std::vector<MemorylessObservation> kEmptyObservations;
    const std::vector<MemorylessObservation>& obs =
        observations ? *observations : kEmptyObservations;
    markMemorylessCandidates(graph, obs, options.memoryless_observation_frames,
                             stats);
  }

  // 4. dce — gated; default OFF. Runs after memoryless so it can use
  //    memoryless-eligibility as a cross-chunk safety gate.
  if (options.dce) {
    runDce(graph, stats, dce_lookahead);
  }

  // 5. reorder — gated. Dependency-respecting topological reorder; may change
  //    which pass is first/last access of an attachment.
  if (options.reorder) {
    runReorder(graph);
  }

  // 6. loadstore — always, AFTER reorder. Actions reflect the FINAL pass order.
  runLoadStore(graph);
}

}  // namespace dxmt9::framegraph
