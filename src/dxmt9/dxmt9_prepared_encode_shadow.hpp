#pragma once

#include "dxmt9_prepared_encode_plan.hpp"

namespace dxmt9::encoders {

// Synchronous diagnostic projection only. The caller retains the source and
// resolver for this call; no canonical identity is authority for async use.
template <typename Resolve>
bool appendPreparedEncodeShadow(PreparedEncodePlanBuilder& builder,
    core::SourcePayloadView source, PreparedSourceKey key,
    Resolve&& resolve, std::uint32_t& unresolved) noexcept {
  if (!source.valid() || key.firstCommand > source.commandCount() ||
      key.commandCount > source.commandCount() - key.firstCommand)
    return false;
  for (std::uint32_t i = 0; i < key.commandCount; ++i) {
    const auto ordinal = key.firstCommand + i;
    const auto command = source.commandAt(ordinal).command;
    core::ExactResourceSet reads{}, writes{};
    core::RenderAttachmentKey attachments{};
    auto resolution = PreparedAccessResolution::RequiresEncodeResolution;
    if (command.kind == core::MetalCommandKind::DrawRun &&
        command.drawState.hot && command.drawState.shaderLayout &&
        command.drawState.debug && command.drawRunRecord &&
        !command.drawParams.empty()) {
      attachments = core::makeRenderAttachmentKey(*command.drawState.hot);
      const auto canonicalize = [&](const core::ExactResourceSet& raw,
                                    core::ExactResourceSet& output) {
        if (!raw.complete()) return false;
        output.flags |= core::ExactResourceSetCanonicalized;
        for (std::uint32_t j = 0; j < raw.count; ++j) {
          std::uint64_t canonical = 0;
          if (!resolve(raw.handles[j], canonical) || !canonical ||
              !output.add(canonical)) return false;
        }
        return true;
      };
      if (canonicalize(core::makeDrawEntryReadSet(command.drawState), reads) &&
          canonicalize(core::makeRenderAttachmentWriteSet(*command.drawState.hot), writes))
        resolution = PreparedAccessResolution::CanonicalStable;
    }
    if (resolution == PreparedAccessResolution::RequiresEncodeResolution) {
      ++unresolved;
      reads = {};
      writes = {};
    }
    if (!builder.append(ordinal, command.kind, reads, writes, attachments, resolution))
      return false;
  }
  return true;
}

}  // namespace dxmt9::encoders
