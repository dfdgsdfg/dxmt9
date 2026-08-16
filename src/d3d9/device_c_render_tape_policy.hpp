#pragma once

#include "device_c_render_tape_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

inline constexpr std::uint32_t kRenderTapePolicyMaxChildren = 16u;
inline constexpr std::size_t kRenderTapePolicyMaxCandidates = 65536u;

// This analyzer is deliberately not a production planner.  It consumes only
// authenticated tape/identity bytes and reports structural opportunities; the
// proof core still needs owner-issued snapshots before a candidate can encode.
enum class RenderTapePolicyExploreStatus : std::uint8_t {
  Valid,
  InvalidTape,
  InvalidIdentity,
  UnsupportedProfile,
  CandidateLimitExceeded,
  AllocationFailed,
};

enum class RenderTapePolicyRejectionReason : std::uint8_t {
  CoordinatorRecord,
  NonDrawRecord,
  TooFewDrawRecords,
  ArithmeticOverflow,
};

struct RenderTapePolicyChildRange {
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t drawCount = 0u;
  std::uint32_t primitiveTotal = 0u;
};

struct RenderTapePolicyCandidate {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t seqId = 0u;
  std::uint64_t logicalPassId = 0u;
  std::uint32_t dagPassIndex = 0u;
  std::uint32_t passKind = 0u;
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t drawTotal = 0u;
  std::uint32_t primitiveTotal = 0u;
  std::uint32_t minChildDraws = 0u;
  std::uint32_t maxChildDraws = 0u;
  std::uint32_t imbalance = 0u;
  std::uint32_t vertexShaderFacts = 0u;
  std::uint32_t pixelShaderFacts = 0u;
  std::uint32_t vertexInputFacts = 0u;
  std::uint32_t uniformSectionFacts = 0u;
  std::uint32_t pipelineInputSectionFacts = 0u;
  bool structuralOnly = true;
  bool proofCoreValidated = false;
  std::vector<RenderTapePolicyChildRange> children{};
};

struct RenderTapePolicyRejection {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t seqId = 0u;
  std::uint64_t logicalPassId = 0u;
  std::uint32_t dagPassIndex = 0u;
  std::uint32_t passKind = 0u;
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  RenderTapePolicyRejectionReason reason =
      RenderTapePolicyRejectionReason::NonDrawRecord;
};

struct RenderTapePolicyExploreResult {
  RenderTapePolicyExploreStatus status =
      RenderTapePolicyExploreStatus::InvalidTape;
  RenderTapeIdentityValidationResult identityValidation{};
  bool authenticatedInput = false;
  bool structuralOnly = true;
  bool proofCoreValidated = false;
  std::vector<RenderTapePolicyCandidate> candidates{};
  std::vector<RenderTapePolicyRejection> rejections{};

  bool valid() const noexcept {
    return status == RenderTapePolicyExploreStatus::Valid;
  }
};

RenderTapePolicyExploreResult exploreRenderTapeParallelPolicy(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> identitySidecar) noexcept;

std::uint32_t renderTapePolicyCanonicalCandidateCount(
    std::uint32_t recordCount) noexcept;
bool renderTapePolicyCandidateBudgetAccepts(
    std::size_t currentCount, std::uint32_t recordCount) noexcept;

const char* renderTapePolicyExploreStatusName(
    RenderTapePolicyExploreStatus status) noexcept;
const char* renderTapePolicyRejectionReasonName(
    RenderTapePolicyRejectionReason reason) noexcept;

} // namespace dxmt9::d3d9
