#pragma once

#include "dxmt9_prepared_encode_plan.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace dxmt9::encoders {

enum class PreparedShadowScreen : std::uint8_t {
  Candidate, QueryScope, Empty, NoParallelWork,
};

// Preparation economics only, never execution authority. A single DrawRun
// CAN have multiple children; do not reject it by command count alone.
constexpr PreparedShadowScreen screenPreparedEncodeShadow(
    bool queryScoped, std::size_t commandCount, core::MetalCommandKind singleKind,
    std::size_t singleDrawCount) noexcept {
  if (queryScoped) return PreparedShadowScreen::QueryScope;
  if (!commandCount) return PreparedShadowScreen::Empty;
  if (commandCount == 1 &&
      (singleKind != core::MetalCommandKind::DrawRun || singleDrawCount < 2))
    return PreparedShadowScreen::NoParallelWork;
  return PreparedShadowScreen::Candidate;
}

// Shared synchronous source projection. The caller retains the source and
// resolver for this call; canonical identity alone grants no async authority.
// The serial consumer accepts only rows preserving the raw read sequence.
template <typename Resolve, typename Visit>
bool visitPreparedEncodeShadow(
    core::SourcePayloadView source, PreparedSourceKey key,
    Resolve&& resolve, Visit&& visit, std::uint32_t& unresolved) {
  if (!source.valid() || key.firstCommand > source.commandCount() ||
      key.commandCount > source.commandCount() - key.firstCommand)
    return false;
  for (std::uint32_t i = 0; i < key.commandCount; ++i) {
    const auto ordinal = key.firstCommand + i;
    const auto command = source.commandAt(ordinal).command;
    core::ExactResourceSet reads{}, writes{};
    core::RenderAttachmentKey attachments{};
    auto resolution = PreparedAccessResolution::RequiresEncodeResolution;
    bool rawReadsPreserved = false;
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
      const auto rawReads = core::makeDrawEntryReadSet(command.drawState);
      if (canonicalize(rawReads, reads) &&
          canonicalize(core::makeRenderAttachmentWriteSet(*command.drawState.hot), writes))
        resolution = PreparedAccessResolution::CanonicalStable;
      rawReadsPreserved = rawReads.count == reads.count &&
          std::equal(rawReads.handles.begin(), rawReads.handles.begin() + rawReads.count,
                     reads.handles.begin());
    }
    if (resolution == PreparedAccessResolution::RequiresEncodeResolution) {
      ++unresolved;
      reads = {};
      writes = {};
    }
    if (!visit(ordinal, command.kind, reads, writes, attachments, resolution,
               rawReadsPreserved))
      return false;
  }
  return true;
}

template <typename Resolve>
bool appendPreparedEncodeShadow(PreparedEncodePlanBuilder& builder,
    core::SourcePayloadView source, PreparedSourceKey key,
    Resolve&& resolve, std::uint32_t& unresolved) noexcept {
  return visitPreparedEncodeShadow(source, key, resolve,
      [&](auto ordinal, auto kind, const auto& reads, const auto& writes,
          auto attachments, auto resolution, bool rawReadsPreserved) noexcept {
        return builder.append(ordinal, kind, reads, writes, attachments, resolution,
                              rawReadsPreserved);
      }, unresolved);
}

// Cold diagnostic only: count the complete projected draw-access union, without
// the plan's 256-identity ceiling. Unresolved rows are explicit, not zero-access
// proof. No partial count is published on allocation or source failure.
struct PreparedShadowCardinality {
  enum class Status : std::uint8_t { Exact, InvalidSource, Allocation };
  Status status = Status::InvalidSource;
  std::uint64_t identities = 0, accesses = 0;
  std::uint32_t commands = 0, unresolved = 0;
};

template <typename Resolve>
PreparedShadowCardinality measurePreparedShadowCardinality(
    core::SourcePayloadView source, PreparedSourceKey key, Resolve&& resolve) noexcept {
  PreparedShadowCardinality result;
  if (!key.valid()) return result;
  try {
    std::vector<std::uint64_t> handles;
    const bool complete = visitPreparedEncodeShadow(source, key, resolve,
        [&](auto, auto, const auto& reads, const auto& writes, auto, auto, bool) {
          ++result.commands;
          for (const auto* set : {&reads, &writes})
            for (std::uint32_t i = 0; i < set->count; ++i)
              handles.push_back(set->handles[i]);
          return true;
        }, result.unresolved);
    if (!complete) return {};
    result.accesses = handles.size();
    std::sort(handles.begin(), handles.end());
    result.identities = std::unique(handles.begin(), handles.end()) - handles.begin();
    result.status = PreparedShadowCardinality::Status::Exact;
    return result;
  } catch (const std::bad_alloc&) {
    return {.status = PreparedShadowCardinality::Status::Allocation};
  } catch (const std::length_error&) {
    return {.status = PreparedShadowCardinality::Status::Allocation};
  }
}

}  // namespace dxmt9::encoders
