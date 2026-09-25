#pragma once

#include "fg_optimizer.hpp"
#include <string>
#include <string_view>

namespace dxmt9::framegraph {

// Cold, device-free optimizer input; not a Render Tape or resource-lifetime token.
struct OptimizerFixture {
  FrameGraph graph;
  u64 sequence = 0;
  u64 process = 0;
  u64 backend = 0;
  u64 observed_ns = 0;
  bool return_diagnostics = false;
};

std::string encodeOptimizerFixture(const OptimizerFixture& fixture);
OptimizerFixture decodeOptimizerFixture(std::string_view bytes);
bool equalOptimizerGraph(const FrameGraph& a, const FrameGraph& b);
bool optimizerFixtureEnabled() noexcept;

// Executes the production optimizer exactly once. Only default pass-coalescing
// inputs without lookahead are exported; unsupported inputs still execute.
void runOptimizerWithFixture(FrameGraph& graph, const OptimizerOptions& options,
                             OptimizerStats* stats, DceLookaheadProof lookahead,
                             u64 sequence, const void* backend);

}  // namespace dxmt9::framegraph
