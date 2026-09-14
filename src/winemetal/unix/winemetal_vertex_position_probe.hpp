#pragma once

#include <cstdint>
#include <string>
#include "winemetal_pipeline_lineage.hpp"

namespace dxmt9::diagnostics {

// Cold probe configuration and draw receipts; never carried by PE/Slot payloads.
struct VertexPositionProbeConfig {
  std::string directory;
  std::string sourceDigest;
  std::uint32_t maxDraws = 0;
  std::uint32_t maxVertices = 0;
  std::uint32_t indexCount = 0;
  bool enabled = false;
  bool skinningInputs = false;
};
const VertexPositionProbeConfig& vertexPositionProbeConfig() noexcept;
bool vertexPositionProbeEnabled() noexcept;

struct VertexPositionProbePipeline {
  std::uint64_t handle = 0;  // Borrowed from the original PSO association.
  MetalPipelineLineage original;
  MetalPipelineLineage instrumented;
};
VertexPositionProbePipeline prepareVertexPositionProbePipeline(std::uint64_t original) noexcept;

struct VertexPositionProbeDraw {
  std::uint64_t commandBuffer = 0, encoder = 0, pipeline = 0, indexBuffer = 0;
  std::uint64_t seq = 0, ordinal = 0, indexCount = 0, indexOffset = 0;
  std::uint64_t captureOwner = 0, sourceOrdinal = 0, rawOrdinal = 0;
  std::uint64_t sourceGeneration = 0, storageGeneration = 0;
  std::uint32_t indexBytes = 0, instances = 0, baseInstance = 0;
  std::int32_t baseVertex = 0;
  bool sourceKnown = false;
  bool triangleList = false;
};

// Always invokes callback exactly once. Preflight rejection calls the original;
// after binding/issuing, unexpected failures are fail-stop, never a replay retry.
using VertexPositionProbeIssue = void (*)(void*, std::uint64_t actualPipeline) noexcept;
void withVertexPositionProbe(const VertexPositionProbeDraw& draw,
                            VertexPositionProbeIssue issue, void* userdata) noexcept;

}  // namespace dxmt9::diagnostics
