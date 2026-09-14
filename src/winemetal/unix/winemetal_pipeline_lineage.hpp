#pragma once

#include <cstdint>

namespace dxmt9::diagnostics {

// Cold observer output only; never part of a draw/queue/PE execution payload.
struct MetalPipelineLineage {
  char receiptSha256[65]{};
  char vertexSha256[65]{};
  char fragmentSha256[65]{};
  bool fragmentAbsent = false;
};

// Pull the immutable creation receipt attached to this actual Metal object.
// Missing, conflicting, unsupported or unpublished lineage remains unknown.
bool copyMetalPipelineLineage(std::uint64_t pipeline,
                             MetalPipelineLineage& output) noexcept;

}  // namespace dxmt9::diagnostics
