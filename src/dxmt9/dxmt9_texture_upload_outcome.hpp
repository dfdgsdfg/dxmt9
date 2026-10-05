#pragma once

#include "dxmt9/core_constants.hpp"

#include <cstdint>

namespace dxmt9::resources {

enum class TextureUploadStageOutcome : std::uint8_t {
  NoUpload,
  ImmediateShared,
  DeferredStaging,
  Failed,
};

// Matches Pool's flattened cube-face/mip bounds without multiplying a
// potentially overflowing mip count. This does not decode or retarget a mip.
constexpr bool textureUploadSubresourceValid(core::TextureType type,
    std::uint32_t levels, std::uint32_t subresource) noexcept {
  const auto mipLevels = levels ? levels : 1u;
  return type == core::TextureType::Cube
      ? subresource / mipLevels < 6u : subresource < mipLevels;
}

struct TextureUploadStageFacts {
  bool destinationPresent = false;
  bool physicalBackingPresent = false;
  bool subresourceValid = false;
  bool sourceNonempty = false;
  bool stagingRequired = false;
  bool stagingProduced = false;
};

// Pool's nullopt is not itself success: Shared writes return it after
// replaceRegion, but missing/invalid resources and failed staging allocation
// return it before an upload. The caller supplies facts from the real backing
// under the same queue lock and invokes the producer exactly once.
// This classifies the producer effect, not byte-layout validity, pixel
// equivalence, or GPU completion; those remain separate admission/oracles.
constexpr TextureUploadStageOutcome textureUploadStageOutcome(
    TextureUploadStageFacts facts) noexcept {
  if (!facts.destinationPresent || !facts.physicalBackingPresent ||
      !facts.subresourceValid) return TextureUploadStageOutcome::Failed;
  if (!facts.sourceNonempty) {
    return facts.stagingProduced ? TextureUploadStageOutcome::Failed
                                : TextureUploadStageOutcome::NoUpload;
  }
  if (facts.stagingProduced) {
    return facts.stagingRequired ? TextureUploadStageOutcome::DeferredStaging
                                 : TextureUploadStageOutcome::Failed;
  }
  return facts.stagingRequired ? TextureUploadStageOutcome::Failed
                               : TextureUploadStageOutcome::ImmediateShared;
}

} // namespace dxmt9::resources
