#pragma once

// Private header shared by the draw, chunk, hazard, and diagnostics encoder
// translation units. Keeps the public dxmt9_draw_encoder.hpp surface frozen
// while letting encodeChunk reach hazard helpers and encodeDraw reach the
// geometry-trace recorder.

#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_post_encode_retirement.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace dxmt9::encoders {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

// This is the only locator-bearing -> locator-free attribution conversion.
// Callers must invoke it while the source is synchronously represented for
// encode; the returned value carries no authority to resolve Tape storage.
inline constexpr std::optional<EncodedCommandId>
encodedCommandIdAtSynchronousEncodeSeam(
    core::metalqueue::PublishedCommandRef source) noexcept {
  if (!source.valid()) {
    return std::nullopt;
  }
  return EncodedCommandId{
      .seqId = source.seqId,
      .commandIndex = source.commandIndex,
  };
}

// Hazard tracking primitives shared with encodeChunk. The bloom filter
// is the cheap pre-screen; the exact handle list resolves false
// positives before deciding to split a render encoder. The mix helper
// is inline in the header so HazardBloom::add can keep its verbatim
// call site after T7's split.

inline u64 bloomMix64(u64 value, u64 salt) {
  u64 x = value + salt + 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

struct HazardBloom {
  std::array<u64, 2> bits{};
  void add(u64 value) {
    if (value == 0) return;
    const u64 hash0 = bloomMix64(value, 0x4d595df4d0f33173ull);
    const u64 hash1 = bloomMix64(value, 0x9e3779b97f4a7c15ull);
    bits[0] |= 1ull << (hash0 & 63u);
    bits[1] |= 1ull << (hash1 & 63u);
  }
  bool overlaps(const HazardBloom& other) const {
    return ((bits[0] & other.bits[0]) != 0) || ((bits[1] & other.bits[1]) != 0);
  }
};

struct HazardHandles {
  static constexpr std::size_t kCapacity =
      1u + core::kMaxRenderTargets + core::kMaxStreams + core::kMaxTextures;
  std::array<u64, kCapacity> handles{};
  std::size_t count = 0;

  void add(u64 value) {
    if (value == 0) return;
    for (std::size_t i = 0; i < count; ++i) {
      if (handles[i] == value) return;
    }
    if (count < handles.size()) {
      handles[count++] = value;
    }
  }

  bool overlaps(const HazardHandles& other) const {
    for (std::size_t i = 0; i < count; ++i) {
      for (std::size_t j = 0; j < other.count; ++j) {
        if (handles[i] == other.handles[j]) return true;
      }
    }
    return false;
  }
};

struct HazardProbe {
  HazardBloom bloom;
  HazardHandles exact;

  void add(u64 value) {
    bloom.add(value);
    exact.add(value);
  }

  bool bloomOverlaps(const HazardProbe& other) const {
    return bloom.overlaps(other.bloom);
  }

  bool exactOverlaps(const HazardProbe& other) const {
    return exact.overlaps(other.exact);
  }
};

HazardProbe makeAttachmentHazard(const core::FlatDrawStateRecord& hot);
HazardProbe makeAttachmentHazard(const core::ClearDesc& clear);
HazardProbe makeDrawReadHazard(core::FlatDrawStateView state);

enum class RenderPassEntryDecision : u8 {
  ContinueActive,
  BeginPass,
  SplitRenderTargetChange,
  SplitHazard,
  SplitTileMidPassIneligible,
};

inline RenderPassEntryDecision classifyRenderPassEntry(
    bool hasActiveRender,
    bool attachmentKeyMatches,
    bool exactHazard,
    bool tileMidPassIneligible) noexcept {
  if (!hasActiveRender) {
    return RenderPassEntryDecision::BeginPass;
  }
  if (!attachmentKeyMatches) {
    return RenderPassEntryDecision::SplitRenderTargetChange;
  }
  if (exactHazard) {
    return RenderPassEntryDecision::SplitHazard;
  }
  if (tileMidPassIneligible) {
    return RenderPassEntryDecision::SplitTileMidPassIneligible;
  }
  return RenderPassEntryDecision::ContinueActive;
}

inline perf::EncoderSplitReason renderPassEntrySplitReason(
    RenderPassEntryDecision decision,
    perf::EncoderSplitReason nonSplitFallback) noexcept {
  switch (decision) {
  case RenderPassEntryDecision::SplitRenderTargetChange:
    return perf::EncoderSplitReason::RenderTargetChange;
  case RenderPassEntryDecision::SplitHazard:
    return perf::EncoderSplitReason::Hazard;
  case RenderPassEntryDecision::SplitTileMidPassIneligible:
    return perf::EncoderSplitReason::TileMidPassIneligible;
  case RenderPassEntryDecision::BeginPass:
  case RenderPassEntryDecision::ContinueActive:
    return nonSplitFallback;
  }
  return nonSplitFallback;
}

enum class NaturalFallbackReentryRelation : u8 {
  Excluded,
  SameWindow,
  CrossWindow,
};

enum class ShortReentryDisposition : u8 {
  Ordinary,
  NaturalSameWindow,
  NaturalCrossWindow,
  PlannedComposite,
  EligibilityPresent,
  EligibilityOther,
  PermutationRejected,
  MixedOrInvalid,
  Count,
};

enum class ShortReentrySourceShape : u8 {
  AllSameSource,
  PriorAndInterveningSameCurrentNewer,
  PriorOlderInterveningAndCurrentSame,
  MixedOrInvalid,
  Count,
};

enum class ShortReentryClearOpenTarget : u8 {
  Excluded,
  Exact,
  NaturalCross,
};

// The narrow A | Clear(B)+B | A shape left by the GT2 locality audit. The
// exact close reason and immediate B clear-open bit keep this from becoming a
// broad marginal cross-product: it names only a source-boundary return whose
// prior A was physically ended by the Clear folded into B.
inline ShortReentryClearOpenTarget classifyShortReentryClearOpenTarget(
    std::uint32_t interveningPasses,
    ShortReentryDisposition disposition,
    ShortReentrySourceShape sourceShape,
    bool interveningOpenedWithClear,
    std::optional<perf::EncoderSplitReason> priorCloseReason) noexcept {
  if (interveningPasses != 1u ||
      sourceShape !=
          ShortReentrySourceShape::PriorAndInterveningSameCurrentNewer ||
      !interveningOpenedWithClear || !priorCloseReason.has_value() ||
      *priorCloseReason != perf::EncoderSplitReason::ClearBarrier) {
    return ShortReentryClearOpenTarget::Excluded;
  }
  return disposition == ShortReentryDisposition::NaturalCrossWindow
      ? ShortReentryClearOpenTarget::NaturalCross
      : ShortReentryClearOpenTarget::Exact;
}

// Classifies source ownership across the complete physical
// prior-A | intervening passes | current-A interval. A d2 interval belongs to
// one of the directional buckets only when both intervening passes agree;
// split or non-monotonic source identities remain explicitly mixed.
inline ShortReentrySourceShape classifyShortReentrySourceShape(
    u64 priorSameKeySeq,
    std::span<const u64> interveningSeqs,
    u64 currentSeq) noexcept {
  if (priorSameKeySeq == 0u || currentSeq == 0u ||
      interveningSeqs.empty() || interveningSeqs.size() > 2u ||
      std::any_of(interveningSeqs.begin(), interveningSeqs.end(),
                  [](u64 seq) { return seq == 0u; })) {
    return ShortReentrySourceShape::MixedOrInvalid;
  }

  const bool allInterveningMatchPrior = std::all_of(
      interveningSeqs.begin(), interveningSeqs.end(),
      [priorSameKeySeq](u64 seq) { return seq == priorSameKeySeq; });
  if (allInterveningMatchPrior && currentSeq == priorSameKeySeq) {
    return ShortReentrySourceShape::AllSameSource;
  }
  if (allInterveningMatchPrior && currentSeq > priorSameKeySeq) {
    return ShortReentrySourceShape::PriorAndInterveningSameCurrentNewer;
  }

  const bool allInterveningMatchCurrent = std::all_of(
      interveningSeqs.begin(), interveningSeqs.end(),
      [currentSeq](u64 seq) { return seq == currentSeq; });
  if (allInterveningMatchCurrent && priorSameKeySeq < currentSeq) {
    return ShortReentrySourceShape::PriorOlderInterveningAndCurrentSame;
  }
  return ShortReentrySourceShape::MixedOrInvalid;
}

inline bool canonicalOrdinaryReplayWindow(
    const ReplayWindowProvenance& provenance) noexcept {
  return provenance.disposition == ReplayWindowDisposition::Ordinary &&
      provenance.windowId == 0u && provenance.sourceIndex == 0u &&
      provenance.sourceCount == 0u;
}

// Classifies the complete physical prior-A | intervening passes | current-A
// interval. This is intentionally limited to d1/d2: the frame tracker retains
// every provenance value needed for those intervals without allocation.
inline ShortReentryDisposition classifyShortReentryDisposition(
    const ReplayWindowProvenance& priorSameKey,
    std::span<const ReplayWindowProvenance> intervening,
    const ReplayWindowProvenance& current) noexcept {
  if (intervening.empty() || intervening.size() > 2u) {
    return ShortReentryDisposition::MixedOrInvalid;
  }

  std::array<ReplayWindowProvenance, 4> interval{};
  interval[0] = priorSameKey;
  std::copy(intervening.begin(), intervening.end(), interval.begin() + 1u);
  interval[intervening.size() + 1u] = current;
  const std::size_t intervalSize = intervening.size() + 2u;

  bool allOrdinary = true;
  for (std::size_t i = 0; i < intervalSize; ++i) {
    if (canonicalOrdinaryReplayWindow(interval[i])) {
      continue;
    }
    allOrdinary = false;
    if (!interval[i].valid()) {
      return ShortReentryDisposition::MixedOrInvalid;
    }
  }
  if (allOrdinary) {
    return ShortReentryDisposition::Ordinary;
  }

  const auto isNatural = [](const ReplayWindowProvenance& provenance) {
    return provenance.disposition ==
        ReplayWindowDisposition::NaturalAfterMergeFallback;
  };
  const auto firstNatural = std::find_if(
      interval.begin(), interval.begin() + intervalSize, isNatural);
  if (firstNatural != interval.begin() + intervalSize) {
    const bool allSameNatural = std::all_of(
        interval.begin(), interval.begin() + intervalSize,
        [&](const ReplayWindowProvenance& provenance) {
          return isNatural(provenance) &&
              provenance.windowId == firstNatural->windowId &&
              provenance.sourceCount == firstNatural->sourceCount;
        });
    return allSameNatural ? ShortReentryDisposition::NaturalSameWindow
                          : ShortReentryDisposition::NaturalCrossWindow;
  }

  // Natural has precedence above to preserve the existing exact attribution
  // contract. For the remaining dispositions, Ordinary is transparent: one
  // touched special window names the interval, while multiple special kinds
  // or identities are explicitly mixed.
  const ReplayWindowProvenance* special = nullptr;
  for (std::size_t i = 0; i < intervalSize; ++i) {
    if (canonicalOrdinaryReplayWindow(interval[i])) {
      continue;
    }
    if (!special) {
      special = &interval[i];
      continue;
    }
    if (interval[i].disposition != special->disposition ||
        interval[i].windowId != special->windowId ||
        interval[i].sourceCount != special->sourceCount) {
      return ShortReentryDisposition::MixedOrInvalid;
    }
  }
  if (!special) {
    return ShortReentryDisposition::MixedOrInvalid;
  }
  switch (special->disposition) {
  case ReplayWindowDisposition::PlannedComposite:
    return ShortReentryDisposition::PlannedComposite;
  case ReplayWindowDisposition::EligibilityPresent:
    return ShortReentryDisposition::EligibilityPresent;
  case ReplayWindowDisposition::EligibilityOther:
    return ShortReentryDisposition::EligibilityOther;
  case ReplayWindowDisposition::PermutationRejectedFallback:
    return ShortReentryDisposition::PermutationRejected;
  case ReplayWindowDisposition::Ordinary:
  case ReplayWindowDisposition::NaturalAfterMergeFallback:
    break;
  }
  return ShortReentryDisposition::MixedOrInvalid;
}

enum class ActiveSeedMergeJoinRelation : u8 {
  NotTarget,
  Matched,
  Mismatch,
};

enum class ActiveSeedMergeContinuationRelation : u8 {
  NotTarget,
  Continued,
  Mismatch,
};

// Call-local, allocation-free exact-locator resolver. Replay order may be a
// validated non-monotonic command permutation, so resolution is a bounded
// linear lookup rather than a command-index cursor.
struct ActiveSeedMergeTargetResolver {
  std::span<const ActiveSeedMergeTargetWitness> targets{};
  const ActiveSeedMergeTargetWitness* current = nullptr;
  std::size_t resolved = 0;

  void beginCommand(std::uint32_t sourceIndex,
                    std::uint32_t commandIndex) noexcept {
    current = nullptr;
    for (const auto& target : targets) {
      if (target.retainedSourceIndex == sourceIndex &&
          target.commandIndex == commandIndex) {
        current = &target;
        break;
      }
    }
  }

  const ActiveSeedMergeTargetWitness* currentTarget() const noexcept {
    return current;
  }

  bool consumeCurrent() noexcept {
    if (!current || resolved >= targets.size()) {
      current = nullptr;
      return false;
    }
    ++resolved;
    current = nullptr;
    return true;
  }

  void endCommand() noexcept { current = nullptr; }

  std::size_t unconsumed() const noexcept {
    return targets.size() - std::min(targets.size(), resolved);
  }
};

inline ActiveSeedMergeContinuationRelation
classifyActiveSeedMergeContinuation(
    const ActiveSeedMergeTicketContext& context,
    const ActiveSeedMergeTargetWitness& target,
    const ReplayWindowProvenance& current,
    std::uint32_t currentSourceIndex,
    std::uint32_t currentCommandIndex,
    RenderPassInstanceToken activeInstance) noexcept {
  if (target.retainedSourceIndex != currentSourceIndex ||
      target.commandIndex != currentCommandIndex) {
    return ActiveSeedMergeContinuationRelation::NotTarget;
  }
  if (!context.valid() || !target.valid() || !current.valid() ||
      current.disposition !=
          ReplayWindowDisposition::NaturalAfterMergeFallback ||
      current.windowId != context.windowId ||
      current.sourceCount != context.sourceCount ||
      current.sourceIndex != currentSourceIndex ||
      activeInstance != context.seed) {
    return ActiveSeedMergeContinuationRelation::Mismatch;
  }
  return ActiveSeedMergeContinuationRelation::Continued;
}

inline ActiveSeedMergeJoinRelation classifyActiveSeedMergePassStart(
    const ActiveSeedMergeTicketContext& context,
    const ActiveSeedMergeTargetWitness& target,
    const ReplayWindowProvenance& current,
    std::uint32_t currentSourceIndex,
    std::uint32_t currentCommandIndex,
    std::optional<RenderPassInstanceToken> priorSameKey,
    std::span<const ReplayWindowProvenance> intervening) noexcept {
  if (target.retainedSourceIndex != currentSourceIndex ||
      target.commandIndex != currentCommandIndex) {
    return ActiveSeedMergeJoinRelation::NotTarget;
  }
  if (!context.valid() || !target.valid() ||
      !current.valid() ||
      current.disposition !=
          ReplayWindowDisposition::NaturalAfterMergeFallback ||
      current.windowId != context.windowId ||
      current.sourceCount != context.sourceCount ||
      current.sourceIndex != currentSourceIndex ||
      !priorSameKey.has_value() || *priorSameKey != context.seed ||
      intervening.empty() || intervening.size() > 4u) {
    return ActiveSeedMergeJoinRelation::Mismatch;
  }
  for (const auto& pass : intervening) {
    if (!pass.valid() ||
        pass.disposition !=
            ReplayWindowDisposition::NaturalAfterMergeFallback ||
        pass.windowId != current.windowId ||
        pass.sourceCount != current.sourceCount) {
      return ActiveSeedMergeJoinRelation::Mismatch;
    }
  }
  return ActiveSeedMergeJoinRelation::Matched;
}

inline bool isNaturalFallbackWindow(
    const ReplayWindowProvenance& provenance) noexcept {
  return provenance.valid() &&
      provenance.disposition ==
          ReplayWindowDisposition::NaturalAfterMergeFallback;
}

inline bool sameNaturalFallbackWindow(
    const ReplayWindowProvenance& left,
    const ReplayWindowProvenance& right) noexcept {
  return isNaturalFallbackWindow(left) && isNaturalFallbackWindow(right) &&
      left.windowId == right.windowId &&
      left.sourceCount == right.sourceCount;
}

// Classify the complete prior-A | intervening passes | current-A interval.
// Looking only at current-A would misattribute a window-crossing A-B-A to the
// current source. SameWindow therefore requires every physical pass in the
// interval to carry the same NaturalAfterMerge fallback window identity.
inline NaturalFallbackReentryRelation classifyNaturalFallbackReentry(
    const ReplayWindowProvenance& priorSameKey,
    std::span<const ReplayWindowProvenance> intervening,
    const ReplayWindowProvenance& current) noexcept {
  bool touchesNatural =
      isNaturalFallbackWindow(priorSameKey) ||
      isNaturalFallbackWindow(current);
  bool sameWindow = sameNaturalFallbackWindow(priorSameKey, current);
  for (const auto& pass : intervening) {
    touchesNatural = touchesNatural || isNaturalFallbackWindow(pass);
    sameWindow = sameWindow &&
        sameNaturalFallbackWindow(priorSameKey, pass);
  }
  if (sameWindow) {
    return NaturalFallbackReentryRelation::SameWindow;
  }
  return touchesNatural ? NaturalFallbackReentryRelation::CrossWindow
                        : NaturalFallbackReentryRelation::Excluded;
}

inline NaturalFallbackReentryRelation
classifyNaturalFallbackReentryFromRecentHistory(
    const ReplayWindowProvenance& priorSameKey,
    const std::array<ReplayWindowProvenance, 4>& recent,
    std::uint32_t currentPassIndex,
    std::uint32_t interveningPasses,
    const ReplayWindowProvenance& current) noexcept {
  if (interveningPasses < 1u || interveningPasses > recent.size() ||
      currentPassIndex < interveningPasses) {
    return NaturalFallbackReentryRelation::Excluded;
  }
  std::array<ReplayWindowProvenance, 4> interval{};
  const std::uint32_t firstIntervening =
      currentPassIndex - interveningPasses;
  for (std::uint32_t i = 0; i < interveningPasses; ++i) {
    interval[i] = recent[(firstIntervening + i) % recent.size()];
  }
  return classifyNaturalFallbackReentry(
      priorSameKey,
      std::span<const ReplayWindowProvenance>(interval.data(),
                                               interveningPasses),
      current);
}

struct EncodeChunkReplayRange {
  std::size_t commandBegin = 0;
  std::size_t commandEnd = 0;
  bool valid = true;

  std::size_t commandCount() const noexcept {
    return commandEnd >= commandBegin ? commandEnd - commandBegin : 0;
  }
};

inline EncodeChunkReplayRange encodeChunkReplayRange(
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    const EncodeChunkOptions& options) noexcept {
  const std::size_t slotCommandCount = slot.commandCount();
  if (options.preRegisteredFragment.has_value()) {
    const auto& fragment = *options.preRegisteredFragment;
    if (options.sessionSource.has_value() || !options.session ||
        !options.deferSessionFinalization ||
        fragment.commandCount == 0 ||
        fragment.commandBegin > slotCommandCount ||
        fragment.commandCount > slotCommandCount - fragment.commandBegin) {
      return EncodeChunkReplayRange{.valid = false};
    }
    return EncodeChunkReplayRange{
        .commandBegin = fragment.commandBegin,
        .commandEnd = fragment.commandBegin + fragment.commandCount,
        .valid = true,
    };
  }
  if (!options.sessionSource.has_value()) {
    return EncodeChunkReplayRange{
        .commandBegin = 0,
        .commandEnd = slotCommandCount,
        .valid = true,
    };
  }

  const auto& source = *options.sessionSource;
  if (source.slotIndex != slotIndex || source.seqId != slot.seqId ||
      source.commandBegin > slotCommandCount ||
      source.commandCount > slotCommandCount - source.commandBegin) {
    return EncodeChunkReplayRange{
        .commandBegin = 0,
        .commandEnd = 0,
        .valid = false,
    };
  }
  return EncodeChunkReplayRange{
      .commandBegin = source.commandBegin,
      .commandEnd = source.commandBegin + source.commandCount,
      .valid = true,
  };
}

// Source-kind-neutral twin over the published payload view. The wrapped
// legacy overloads below remain for existing ChunkSlot callers/tests.
inline EncodeChunkReplayRange encodeChunkReplayRange(
    std::size_t slotIndex,
    core::SourcePayloadView payload,
    std::uint64_t sourceSeqId,
    const EncodeChunkOptions& options) noexcept {
  const std::size_t commandCount = payload.commandCount();
  if (options.preRegisteredFragment.has_value()) {
    const auto& fragment = *options.preRegisteredFragment;
    if (options.sessionSource.has_value() || !options.session ||
        !options.deferSessionFinalization ||
        fragment.commandCount == 0 ||
        fragment.commandBegin > commandCount ||
        fragment.commandCount > commandCount - fragment.commandBegin) {
      return EncodeChunkReplayRange{.valid = false};
    }
    return EncodeChunkReplayRange{
        .commandBegin = fragment.commandBegin,
        .commandEnd = fragment.commandBegin + fragment.commandCount,
        .valid = true,
    };
  }
  if (!options.sessionSource.has_value()) {
    return EncodeChunkReplayRange{
        .commandBegin = 0,
        .commandEnd = commandCount,
        .valid = true,
    };
  }

  const auto& source = *options.sessionSource;
  if (source.slotIndex != slotIndex || source.seqId != sourceSeqId ||
      source.commandBegin > commandCount ||
      source.commandCount > commandCount - source.commandBegin) {
    return EncodeChunkReplayRange{
        .commandBegin = 0,
        .commandEnd = 0,
        .valid = false,
    };
  }
  return EncodeChunkReplayRange{
      .commandBegin = source.commandBegin,
      .commandEnd = source.commandBegin + source.commandCount,
      .valid = true,
  };
}

inline bool readySlotSnapshotMatchesCompletionSource(
    const core::metalqueue::ResolvedPublishedSource& snapshot,
    const core::metalqueue::QueueCompletionSource& source,
    std::size_t slotIndex,
    core::SourcePayloadView payload,
    std::uint64_t sourceSeqId) noexcept {
  return snapshot.payload == payload &&
         snapshot.sourceId == source.source.id &&
         snapshot.storage == source.source.storage &&
         snapshot.slotIndex == slotIndex &&
         snapshot.slotIndex == source.slotIndex &&
         snapshot.seqId == sourceSeqId &&
         snapshot.seqId == source.seqId &&
         snapshot.hasPresent == source.hasPresent &&
         snapshot.commandBegin == source.commandBegin &&
         snapshot.commandCount == source.commandCount;
}

inline bool readySlotSnapshotMatchesReplayRange(
    const core::metalqueue::ResolvedPublishedSource& snapshot,
    core::CpuReadyTape::SourceRef source,
    std::size_t slotIndex,
    core::SourcePayloadView payload,
    std::uint64_t sourceSeqId,
    EncodeChunkReplayRange replayRange) noexcept {
  return snapshot.payload == payload &&
         snapshot.sourceId == source.id &&
         snapshot.storage == source.storage &&
         snapshot.slotIndex == slotIndex &&
         snapshot.seqId == sourceSeqId &&
         snapshot.commandBegin == replayRange.commandBegin &&
         snapshot.commandCount == replayRange.commandCount();
}

inline bool readySlotSnapshotMatchesCompletionSource(
    const core::metalqueue::ResolvedPublishedSource& snapshot,
    const core::metalqueue::QueueCompletionSource& source,
    std::size_t slotIndex,
    const core::ChunkSlot& slot) noexcept {
  return snapshot.slot == &slot &&
         snapshot.sourceId == source.source.id &&
         snapshot.storage == source.source.storage &&
         snapshot.slotIndex == slotIndex &&
         snapshot.slotIndex == source.slotIndex &&
         snapshot.seqId == slot.seqId &&
         snapshot.seqId == source.seqId &&
         snapshot.hasPresent == source.hasPresent &&
         snapshot.commandBegin == source.commandBegin &&
         snapshot.commandCount == source.commandCount;
}

inline bool readySlotSnapshotMatchesReplayRange(
    const core::metalqueue::ResolvedPublishedSource& snapshot,
    core::CpuReadyTape::SourceRef source,
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    EncodeChunkReplayRange replayRange) noexcept {
  return snapshot.slot == &slot &&
         snapshot.sourceId == source.id &&
         snapshot.storage == source.storage &&
         snapshot.slotIndex == slotIndex &&
         snapshot.seqId == slot.seqId &&
         snapshot.commandBegin == replayRange.commandBegin &&
         snapshot.commandCount == replayRange.commandCount();
}

// Fixed, synchronous Store-proof view over the current replay-plan suffix
// followed by the already-represented FIFO source suffix. The descriptors own
// no payload; their views remain valid only for the surrounding encode call.
struct RenderPassStoreProofLookaheadPlan {
  std::array<RenderPassStoreProofLookaheadSource,
             core::metalqueue::kMaxReadyPrefixSources>
      sources{};
  std::size_t count = 0;
  bool invalid = false;
  bool storageTruncated = false;

  std::span<const RenderPassStoreProofLookaheadSource> view() const noexcept {
    return std::span<const RenderPassStoreProofLookaheadSource>(sources.data(),
                                                                count);
  }
};

struct RenderPassStoreProofLookaheadInput {
  std::size_t slotIndex = 0;
  core::SourcePayloadView payload{};
  std::uint64_t sourceSeqId = 0;
  EncodeChunkReplayRange replayRange{};
  const core::metalqueue::QueueCompletionSource* sessionSource = nullptr;
  core::CpuReadyTape::SourceRef partitionSource{};
  std::span<const core::metalqueue::ResolvedPublishedSource> retainedSources{};
  bool replayCommandPlanActive = false;
  std::span<const std::uint32_t> replayCommandOrder{};
  std::span<const std::size_t> replayOrdinalByCommandIndex{};
  std::size_t lookaheadStartIndex = 0;
};

inline RenderPassStoreProofLookaheadPlan
makeRenderPassStoreProofLookaheadPlan(
    const RenderPassStoreProofLookaheadInput& input) noexcept {
  RenderPassStoreProofLookaheadPlan result{};
  auto append = [&](core::SourcePayloadView payload,
                    std::size_t firstCommandIndex,
                    std::size_t commandEndIndex,
                    std::span<const std::uint32_t> commandOrder = {}) {
    const std::size_t traversalCount =
        commandOrder.empty() ? payload.commandCount() : commandOrder.size();
    if (!payload.valid() || firstCommandIndex > commandEndIndex ||
        commandEndIndex > traversalCount ||
        result.count >= result.sources.size()) {
      return false;
    }
    result.sources[result.count++] = RenderPassStoreProofLookaheadSource{
        .payload = payload,
        .commandOrder = commandOrder,
        .firstCommandIndex = firstCommandIndex,
        .commandEndIndex = commandEndIndex,
    };
    return true;
  };
  auto appendCurrentSourceSuffix = [&]() {
    if (!input.replayCommandPlanActive) {
      if (!input.payload.valid() ||
          input.lookaheadStartIndex >= input.payload.commandCount()) {
        return false;
      }
      return append(input.payload, input.lookaheadStartIndex + 1u,
                    input.payload.commandCount());
    }
    if (!input.replayRange.valid || !input.payload.valid() ||
        input.lookaheadStartIndex < input.replayRange.commandBegin ||
        input.lookaheadStartIndex >= input.replayRange.commandEnd) {
      return false;
    }
    const std::size_t relative =
        input.lookaheadStartIndex - input.replayRange.commandBegin;
    if (relative >= input.replayOrdinalByCommandIndex.size()) {
      return false;
    }
    const std::size_t replayOrdinal =
        input.replayOrdinalByCommandIndex[relative];
    if (replayOrdinal >= input.replayCommandOrder.size() ||
        input.replayCommandOrder[replayOrdinal] !=
            input.lookaheadStartIndex) {
      return false;
    }
    return append(input.payload, replayOrdinal + 1u,
                  input.replayCommandOrder.size(),
                  input.replayCommandOrder);
  };

  if (input.retainedSources.size() > result.sources.size()) {
    if (input.replayCommandPlanActive) {
      result.invalid = true;
      return result;
    }
    result.storageTruncated = true;
    if (!appendCurrentSourceSuffix()) {
      result.invalid = true;
    }
    return result;
  }

  if (input.retainedSources.empty()) {
    if (!appendCurrentSourceSuffix()) {
      result.invalid = true;
    }
    return result;
  }

  const bool startsAtCurrentSource = input.sessionSource
      ? readySlotSnapshotMatchesCompletionSource(
            input.retainedSources.front(), *input.sessionSource,
            input.slotIndex, input.payload, input.sourceSeqId)
      : readySlotSnapshotMatchesReplayRange(
            input.retainedSources.front(), input.partitionSource,
            input.slotIndex, input.payload, input.sourceSeqId,
            input.replayRange);
  bool retainedSourcesValid = startsAtCurrentSource;
  for (const auto& source : input.retainedSources) {
    if (!source.payload.valid() ||
        source.commandBegin > source.payload.commandCount() ||
        source.commandCount >
            source.payload.commandCount() - source.commandBegin) {
      retainedSourcesValid = false;
      break;
    }
  }
  const auto& firstSource = input.retainedSources.front();
  const std::size_t firstSourceEnd =
      firstSource.commandBegin + firstSource.commandCount;
  retainedSourcesValid = retainedSourcesValid &&
      input.lookaheadStartIndex >= firstSource.commandBegin &&
      input.lookaheadStartIndex < firstSourceEnd;
  if (!retainedSourcesValid) {
    result.invalid = true;
    if (!input.replayCommandPlanActive) {
      appendCurrentSourceSuffix();
    }
    return result;
  }

  if (input.replayCommandPlanActive) {
    // Source zero is represented by the replay-order suffix below. Appending
    // its natural range as well would duplicate the current source and can
    // turn a valid future Clear proof into a false intervening draw.
    if (!appendCurrentSourceSuffix()) {
      result.invalid = true;
      result.count = 0;
      return result;
    }
    for (std::size_t i = 1; i < input.retainedSources.size(); ++i) {
      const auto& source = input.retainedSources[i];
      if (!append(source.payload, source.commandBegin,
                  source.commandBegin + source.commandCount)) {
        result.invalid = true;
        result.count = 0;
        return result;
      }
    }
    return result;
  }

  for (std::size_t i = 0; i < input.retainedSources.size(); ++i) {
    const auto& source = input.retainedSources[i];
    if (!append(source.payload,
                i == 0u ? input.lookaheadStartIndex + 1u
                        : source.commandBegin,
                source.commandBegin + source.commandCount)) {
      result.invalid = true;
      result.count = 0;
      return result;
    }
  }
  return result;
}

// Per-draw view from DrawParam. Constructed once at encodeDraw entry; all
// per-draw field reads inside the function go through this view. Lives in
// the shared internal header because diagnostics consumes it by const-ref.
struct ParamView {
  core::PrimitiveType primitiveType;
  u32 primitiveCount;
  u32 startVertex;
  i32 baseVertexIndex;
  u32 startIndex;
  core::IndexType indexType;
  bool indexed;
  u32 instanceCount;
  std::span<const u8> userVertexData;
  std::span<const u8> userIndexData;
};

inline bool drawBindingOverrideRequiresBaseStateBind(
    const core::DrawBindingOverride& binding,
    const core::DrawShaderLayoutContext* baseShaderLayout) noexcept {
  if (!baseShaderLayout) {
    return false;
  }

  // Stream 0 stride is passed through DrawVolatile, but extra-stream strides
  // are baked into the generated VS input-load source. Rebind base state only
  // when an override can require a different PSO/source variant.
  for (u32 stream = 1; stream < core::kMaxStreams; ++stream) {
    if ((binding.streamMask & (1u << stream)) == 0u) {
      continue;
    }
    if (binding.streams[stream].stride !=
        baseShaderLayout->vertexDecl.streams[stream].stride) {
      return true;
    }
  }
  return false;
}

struct ProgrammableVsExtraStreamBinding {
  u32 stream = 0;
  u32 metalSlot = 0;
  u64 offset = 0;
  u32 stride = 0;
};

struct ProgrammableVsExtraStreamBindingList {
  std::array<ProgrammableVsExtraStreamBinding, core::kMaxStreams - 1u> entries{};
  std::size_t count = 0;

  void push_back(ProgrammableVsExtraStreamBinding binding) {
    if (count < entries.size()) {
      entries[count++] = binding;
    }
  }

  bool empty() const noexcept { return count == 0; }
  std::size_t size() const noexcept { return count; }

  const ProgrammableVsExtraStreamBinding& operator[](std::size_t index) const noexcept {
    return entries[index];
  }

  const ProgrammableVsExtraStreamBinding* begin() const noexcept {
    return entries.data();
  }

  const ProgrammableVsExtraStreamBinding* end() const noexcept {
    return entries.data() + count;
  }
};

struct FragmentTextureSamplerBinding {
  u32 stage = 0;
  core::Handle texture{};
  u32 textureLod = 0;
  u64 samplerStateHash = 0;
  core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
};

struct FragmentTextureSamplerBindingList {
  std::array<FragmentTextureSamplerBinding, core::kMaxSamplers> entries{};
  std::size_t count = 0;

  void push_back(FragmentTextureSamplerBinding binding) {
    if (count < entries.size()) {
      entries[count++] = binding;
    }
  }

  bool empty() const noexcept { return count == 0; }
  std::size_t size() const noexcept { return count; }

  const FragmentTextureSamplerBinding& operator[](std::size_t index) const noexcept {
    return entries[index];
  }

  const FragmentTextureSamplerBinding* begin() const noexcept {
    return entries.data();
  }

  const FragmentTextureSamplerBinding* end() const noexcept {
    return entries.data() + count;
  }
};

struct VertexTextureSamplerBinding {
  u32 stage = 0;
  core::Handle texture{};
  u32 textureLod = 0;
  u64 samplerStateHash = 0;
  core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
};

struct VertexTextureSamplerBindingList {
  std::array<VertexTextureSamplerBinding, core::kMaxVertexTextureSamplers> entries{};
  std::size_t count = 0;

  void push_back(VertexTextureSamplerBinding binding) {
    if (count < entries.size()) {
      entries[count++] = binding;
    }
  }

  bool empty() const noexcept { return count == 0; }
  std::size_t size() const noexcept { return count; }

  const VertexTextureSamplerBinding& operator[](std::size_t index) const noexcept {
    return entries[index];
  }

  const VertexTextureSamplerBinding* begin() const noexcept {
    return entries.data();
  }

  const VertexTextureSamplerBinding* end() const noexcept {
    return entries.data() + count;
  }
};

struct EncoderRasterStatePlan {
  WMTViewport viewport{};
  WMTScissorRect scissor{};
  WMTCullMode cullMode = WMTCullModeNone;
};

struct DrawBindingPacketPlan {
  FragmentTextureSamplerBindingList fragmentTextureSamplers{};
  VertexTextureSamplerBindingList vertexTextureSamplers{};
  ProgrammableVsExtraStreamBindingList extraStreams{};
  EncoderRasterStatePlan raster{};
};

struct DrawBindingPacketTextureSamplerKey {
  u32 stage = 0;
  u64 texture = 0;
  u32 textureLod = 0;
  u64 samplerStateHash = 0;
  core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};

  friend constexpr bool operator==(const DrawBindingPacketTextureSamplerKey& lhs,
                                   const DrawBindingPacketTextureSamplerKey& rhs);
};

struct DrawBindingPacketExtraStreamKey {
  u32 stream = 0;
  u32 metalSlot = 0;
  u64 offset = 0;
  u32 stride = 0;

  friend constexpr bool operator==(const DrawBindingPacketExtraStreamKey&,
                                   const DrawBindingPacketExtraStreamKey&) = default;
};

struct DrawBindingPacketRasterKey {
  std::array<u64, 6> viewportBits{};
  u64 scissorX = 0;
  u64 scissorY = 0;
  u64 scissorWidth = 0;
  u64 scissorHeight = 0;
  u32 cullMode = 0;

  friend constexpr bool operator==(const DrawBindingPacketRasterKey&,
                                   const DrawBindingPacketRasterKey&) = default;
};

struct DrawBindingPacketKey {
  std::array<DrawBindingPacketTextureSamplerKey, core::kMaxSamplers> fragmentTextureSamplers{};
  u32 fragmentTextureSamplerCount = 0;
  std::array<DrawBindingPacketTextureSamplerKey, core::kMaxVertexTextureSamplers> vertexTextureSamplers{};
  u32 vertexTextureSamplerCount = 0;
  std::array<DrawBindingPacketExtraStreamKey, core::kMaxStreams - 1u> extraStreams{};
  u32 extraStreamCount = 0;
  DrawBindingPacketRasterKey raster{};

  friend constexpr bool operator==(const DrawBindingPacketKey& lhs,
                                   const DrawBindingPacketKey& rhs);
};

inline u64 drawBindingPacketHashMix(u64 seed, u64 value) noexcept {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  value ^= value >> 31;
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
  return seed;
}

inline u64 drawBindingPacketDoubleBits(double value) noexcept {
  return std::bit_cast<u64>(value);
}

template <std::size_t MaxEntries>
constexpr bool drawBindingPacketFlatStateSetsEqual(
    const core::FlatStateSet<MaxEntries>& lhs,
    const core::FlatStateSet<MaxEntries>& rhs) noexcept {
  if (lhs.count != rhs.count ||
      lhs.hash != rhs.hash ||
      lhs.overflow != rhs.overflow) {
    return false;
  }
  const u32 count = std::min<u32>(lhs.count, static_cast<u32>(MaxEntries));
  for (u32 i = 0; i < count; ++i) {
    if (lhs.entries[i] != rhs.entries[i]) {
      return false;
    }
  }
  return true;
}

constexpr bool operator==(const DrawBindingPacketTextureSamplerKey& lhs,
                          const DrawBindingPacketTextureSamplerKey& rhs) {
  return lhs.stage == rhs.stage &&
         lhs.texture == rhs.texture &&
         lhs.textureLod == rhs.textureLod &&
         lhs.samplerStateHash == rhs.samplerStateHash &&
         drawBindingPacketFlatStateSetsEqual(lhs.samplerStates, rhs.samplerStates);
}

constexpr bool operator==(const DrawBindingPacketKey& lhs,
                          const DrawBindingPacketKey& rhs) {
  if (lhs.fragmentTextureSamplerCount != rhs.fragmentTextureSamplerCount ||
      lhs.vertexTextureSamplerCount != rhs.vertexTextureSamplerCount ||
      lhs.extraStreamCount != rhs.extraStreamCount ||
      lhs.raster != rhs.raster) {
    return false;
  }
  for (u32 i = 0; i < lhs.fragmentTextureSamplerCount; ++i) {
    if (lhs.fragmentTextureSamplers[i] != rhs.fragmentTextureSamplers[i]) {
      return false;
    }
  }
  for (u32 i = 0; i < lhs.vertexTextureSamplerCount; ++i) {
    if (lhs.vertexTextureSamplers[i] != rhs.vertexTextureSamplers[i]) {
      return false;
    }
  }
  for (u32 i = 0; i < lhs.extraStreamCount; ++i) {
    if (lhs.extraStreams[i] != rhs.extraStreams[i]) {
      return false;
    }
  }
  return true;
}

inline DrawBindingPacketRasterKey makeDrawBindingPacketRasterKey(
    const EncoderRasterStatePlan& raster) noexcept {
  return DrawBindingPacketRasterKey{
      .viewportBits = {
          drawBindingPacketDoubleBits(raster.viewport.originX),
          drawBindingPacketDoubleBits(raster.viewport.originY),
          drawBindingPacketDoubleBits(raster.viewport.width),
          drawBindingPacketDoubleBits(raster.viewport.height),
          drawBindingPacketDoubleBits(raster.viewport.znear),
          drawBindingPacketDoubleBits(raster.viewport.zfar),
      },
      .scissorX = raster.scissor.x,
      .scissorY = raster.scissor.y,
      .scissorWidth = raster.scissor.width,
      .scissorHeight = raster.scissor.height,
      .cullMode = static_cast<u32>(raster.cullMode),
  };
}

inline DrawBindingPacketKey makeDrawBindingPacketKey(
    const DrawBindingPacketPlan& packet) noexcept {
  DrawBindingPacketKey key{};
  key.fragmentTextureSamplerCount = static_cast<u32>(packet.fragmentTextureSamplers.size());
  for (u32 i = 0; i < key.fragmentTextureSamplerCount; ++i) {
    const auto& binding = packet.fragmentTextureSamplers[i];
    key.fragmentTextureSamplers[i] = DrawBindingPacketTextureSamplerKey{
        .stage = binding.stage,
        .texture = binding.texture.value,
        .textureLod = binding.textureLod,
        .samplerStateHash = binding.samplerStateHash,
        .samplerStates = binding.samplerStates,
    };
  }

  key.vertexTextureSamplerCount = static_cast<u32>(packet.vertexTextureSamplers.size());
  for (u32 i = 0; i < key.vertexTextureSamplerCount; ++i) {
    const auto& binding = packet.vertexTextureSamplers[i];
    key.vertexTextureSamplers[i] = DrawBindingPacketTextureSamplerKey{
        .stage = binding.stage,
        .texture = binding.texture.value,
        .textureLod = binding.textureLod,
        .samplerStateHash = binding.samplerStateHash,
        .samplerStates = binding.samplerStates,
    };
  }

  key.extraStreamCount = static_cast<u32>(packet.extraStreams.size());
  for (u32 i = 0; i < key.extraStreamCount; ++i) {
    const auto& binding = packet.extraStreams[i];
    key.extraStreams[i] = DrawBindingPacketExtraStreamKey{
        .stream = binding.stream,
        .metalSlot = binding.metalSlot,
        .offset = binding.offset,
        .stride = binding.stride,
    };
  }

  key.raster = makeDrawBindingPacketRasterKey(packet.raster);
  return key;
}

inline u64 hashDrawBindingPacketKey(const DrawBindingPacketKey& key) noexcept {
  u64 seed = drawBindingPacketHashMix(0x5ad07b1f4c2e9638ull,
                                      key.fragmentTextureSamplerCount);
  for (u32 i = 0; i < key.fragmentTextureSamplerCount; ++i) {
    const auto& binding = key.fragmentTextureSamplers[i];
    seed = drawBindingPacketHashMix(seed, binding.stage);
    seed = drawBindingPacketHashMix(seed, binding.texture);
    seed = drawBindingPacketHashMix(seed, binding.textureLod);
    seed = drawBindingPacketHashMix(seed, binding.samplerStateHash);
  }

  seed = drawBindingPacketHashMix(seed, key.vertexTextureSamplerCount);
  for (u32 i = 0; i < key.vertexTextureSamplerCount; ++i) {
    const auto& binding = key.vertexTextureSamplers[i];
    seed = drawBindingPacketHashMix(seed, binding.stage);
    seed = drawBindingPacketHashMix(seed, binding.texture);
    seed = drawBindingPacketHashMix(seed, binding.textureLod);
    seed = drawBindingPacketHashMix(seed, binding.samplerStateHash);
  }

  seed = drawBindingPacketHashMix(seed, key.extraStreamCount);
  for (u32 i = 0; i < key.extraStreamCount; ++i) {
    const auto& binding = key.extraStreams[i];
    seed = drawBindingPacketHashMix(seed, binding.stream);
    seed = drawBindingPacketHashMix(seed, binding.metalSlot);
    seed = drawBindingPacketHashMix(seed, binding.offset);
    seed = drawBindingPacketHashMix(seed, binding.stride);
  }

  for (const auto bits : key.raster.viewportBits) {
    seed = drawBindingPacketHashMix(seed, bits);
  }
  seed = drawBindingPacketHashMix(seed, key.raster.scissorX);
  seed = drawBindingPacketHashMix(seed, key.raster.scissorY);
  seed = drawBindingPacketHashMix(seed, key.raster.scissorWidth);
  seed = drawBindingPacketHashMix(seed, key.raster.scissorHeight);
  seed = drawBindingPacketHashMix(seed, key.raster.cullMode);
  return seed;
}

inline u64 hashDrawBindingPacketPlan(const DrawBindingPacketPlan& packet) noexcept {
  u64 seed = drawBindingPacketHashMix(
      0x5ad07b1f4c2e9638ull,
      static_cast<u64>(packet.fragmentTextureSamplers.size()));
  for (const auto& binding : packet.fragmentTextureSamplers) {
    seed = drawBindingPacketHashMix(seed, binding.stage);
    seed = drawBindingPacketHashMix(seed, binding.texture.value);
    seed = drawBindingPacketHashMix(seed, binding.textureLod);
    seed = drawBindingPacketHashMix(seed, binding.samplerStateHash);
  }

  seed = drawBindingPacketHashMix(
      seed, static_cast<u64>(packet.vertexTextureSamplers.size()));
  for (const auto& binding : packet.vertexTextureSamplers) {
    seed = drawBindingPacketHashMix(seed, binding.stage);
    seed = drawBindingPacketHashMix(seed, binding.texture.value);
    seed = drawBindingPacketHashMix(seed, binding.textureLod);
    seed = drawBindingPacketHashMix(seed, binding.samplerStateHash);
  }

  seed = drawBindingPacketHashMix(seed, static_cast<u64>(packet.extraStreams.size()));
  for (const auto& binding : packet.extraStreams) {
    seed = drawBindingPacketHashMix(seed, binding.stream);
    seed = drawBindingPacketHashMix(seed, binding.metalSlot);
    seed = drawBindingPacketHashMix(seed, binding.offset);
    seed = drawBindingPacketHashMix(seed, binding.stride);
  }

  const auto raster = makeDrawBindingPacketRasterKey(packet.raster);
  for (const auto bits : raster.viewportBits) {
    seed = drawBindingPacketHashMix(seed, bits);
  }
  seed = drawBindingPacketHashMix(seed, raster.scissorX);
  seed = drawBindingPacketHashMix(seed, raster.scissorY);
  seed = drawBindingPacketHashMix(seed, raster.scissorWidth);
  seed = drawBindingPacketHashMix(seed, raster.scissorHeight);
  seed = drawBindingPacketHashMix(seed, raster.cullMode);
  return seed;
}

inline bool drawBindingPacketTextureSamplerBindingsEqual(
    const FragmentTextureSamplerBinding& lhs,
    const FragmentTextureSamplerBinding& rhs) noexcept {
  return lhs.stage == rhs.stage &&
         lhs.texture == rhs.texture &&
         lhs.textureLod == rhs.textureLod &&
         lhs.samplerStateHash == rhs.samplerStateHash &&
         drawBindingPacketFlatStateSetsEqual(lhs.samplerStates, rhs.samplerStates);
}

inline bool drawBindingPacketTextureSamplerBindingsEqual(
    const VertexTextureSamplerBinding& lhs,
    const VertexTextureSamplerBinding& rhs) noexcept {
  return lhs.stage == rhs.stage &&
         lhs.texture == rhs.texture &&
         lhs.textureLod == rhs.textureLod &&
         lhs.samplerStateHash == rhs.samplerStateHash &&
         drawBindingPacketFlatStateSetsEqual(lhs.samplerStates, rhs.samplerStates);
}

inline bool drawBindingPacketExtraStreamBindingsEqual(
    const ProgrammableVsExtraStreamBinding& lhs,
    const ProgrammableVsExtraStreamBinding& rhs) noexcept {
  return lhs.stream == rhs.stream &&
         lhs.metalSlot == rhs.metalSlot &&
         lhs.offset == rhs.offset &&
         lhs.stride == rhs.stride;
}

inline bool drawBindingPacketPlansEqual(
    const DrawBindingPacketPlan& lhs,
    const DrawBindingPacketPlan& rhs) noexcept {
  if (lhs.fragmentTextureSamplers.size() != rhs.fragmentTextureSamplers.size() ||
      lhs.vertexTextureSamplers.size() != rhs.vertexTextureSamplers.size() ||
      lhs.extraStreams.size() != rhs.extraStreams.size() ||
      makeDrawBindingPacketRasterKey(lhs.raster) != makeDrawBindingPacketRasterKey(rhs.raster)) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.fragmentTextureSamplers.size(); ++i) {
    if (!drawBindingPacketTextureSamplerBindingsEqual(
            lhs.fragmentTextureSamplers[i], rhs.fragmentTextureSamplers[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.vertexTextureSamplers.size(); ++i) {
    if (!drawBindingPacketTextureSamplerBindingsEqual(
            lhs.vertexTextureSamplers[i], rhs.vertexTextureSamplers[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.extraStreams.size(); ++i) {
    if (!drawBindingPacketExtraStreamBindingsEqual(lhs.extraStreams[i], rhs.extraStreams[i])) {
      return false;
    }
  }
  return true;
}

struct DrawBindingPacketKeyHash {
  std::size_t operator()(const DrawBindingPacketKey& key) const noexcept {
    return static_cast<std::size_t>(hashDrawBindingPacketKey(key));
  }
};

struct DrawBindingPacketCacheEntry {
  bool valid = false;
  u64 hash = 0;
  DrawBindingPacketPlan packet{};
};

struct DrawBindingPacketCache {
  static constexpr std::size_t kCapacity = 128;
  std::array<DrawBindingPacketCacheEntry, kCapacity> entries{};
  u64 hits = 0;
  u64 misses = 0;
};

struct DrawBindingPacketCacheStats {
  u64 keyCpuNs = 0;
  u64 hashCpuNs = 0;
  u64 probeCpuNs = 0;
  u64 storeCpuNs = 0;
  u64 hits = 0;
  u64 misses = 0;
  u64 collisions = 0;
};

inline u64 drawBindingPacketNowNs() noexcept {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline const DrawBindingPacketPlan& cacheDrawBindingPacket(
    DrawBindingPacketCache& cache,
    const DrawBindingPacketPlan& packet,
    DrawBindingPacketCacheStats* stats = nullptr) noexcept {
  const u64 hashStart = stats ? drawBindingPacketNowNs() : 0;
  const auto hash = hashDrawBindingPacketPlan(packet);
  const u64 hashEnd = stats ? drawBindingPacketNowNs() : 0;
  if (stats) {
    stats->hashCpuNs += hashEnd - hashStart;
  }

  const u64 probeStart = stats ? drawBindingPacketNowNs() : 0;
  auto& entry = cache.entries[hash % DrawBindingPacketCache::kCapacity];
  const bool entryValid = entry.valid;
  const bool hit = entryValid && entry.hash == hash &&
                   drawBindingPacketPlansEqual(entry.packet, packet);
  const u64 probeEnd = stats ? drawBindingPacketNowNs() : 0;
  if (stats) {
    stats->probeCpuNs += probeEnd - probeStart;
  }

  if (hit) {
    ++cache.hits;
    if (stats) {
      ++stats->hits;
    }
    return entry.packet;
  }

  ++cache.misses;
  if (stats) {
    ++stats->misses;
    if (entryValid) {
      ++stats->collisions;
    }
  }

  const u64 storeStart = stats ? drawBindingPacketNowNs() : 0;
  entry.valid = true;
  entry.hash = hash;
  entry.packet = packet;
  const u64 storeEnd = stats ? drawBindingPacketNowNs() : 0;
  if (stats) {
    stats->storeCpuNs += storeEnd - storeStart;
  }
  return entry.packet;
}

inline bool vertexDeclUsesStream(const core::VertexDeclSnapshot& vertexDecl, u32 stream) {
  for (const auto& element : vertexDecl.elements) {
    if (element.stream == stream) {
      return true;
    }
  }
  return false;
}

inline bool expandIndexedStreamToFlatVertexBytes(std::span<const u8> sourceBytes,
                                                 std::span<const u8> indexBytes,
                                                 core::IndexType indexType,
                                                 u32 startIndex,
                                                 i32 baseVertexIndex,
                                                 u64 vertexCount,
                                                 std::size_t sourceBase,
                                                 std::size_t sourceStride,
                                                 std::vector<u8>& outBytes) {
  if (sourceBytes.empty() || indexBytes.empty() || sourceStride == 0) {
    return false;
  }

  const std::size_t indexSize = indexType == core::IndexType::UInt16 ? sizeof(u16) : sizeof(u32);
  const std::size_t firstIndexByte = static_cast<std::size_t>(startIndex) * indexSize;
  outBytes.assign(static_cast<std::size_t>(vertexCount) * sourceStride, 0);

  for (u64 i = 0; i < vertexCount; ++i) {
    i32 vertexIndex = baseVertexIndex;
    const std::size_t indexOffset = firstIndexByte + static_cast<std::size_t>(i) * indexSize;
    if (indexType == core::IndexType::UInt16) {
      if (indexOffset + sizeof(u16) > indexBytes.size()) {
        return false;
      }
      u16 index = 0;
      std::memcpy(&index, indexBytes.data() + indexOffset, sizeof(index));
      vertexIndex += static_cast<i32>(index);
    } else {
      if (indexOffset + sizeof(u32) > indexBytes.size()) {
        return false;
      }
      u32 index = 0;
      std::memcpy(&index, indexBytes.data() + indexOffset, sizeof(index));
      vertexIndex += static_cast<i32>(index);
    }

    if (vertexIndex < 0) {
      return false;
    }
    const std::size_t sourceOffset =
        sourceBase + static_cast<std::size_t>(vertexIndex) * sourceStride;
    if (sourceOffset + sourceStride > sourceBytes.size()) {
      return false;
    }
    std::memcpy(outBytes.data() + static_cast<std::size_t>(i) * sourceStride,
                sourceBytes.data() + sourceOffset,
                sourceStride);
  }

  return true;
}

inline ProgrammableVsExtraStreamBindingList makeProgrammableVsExtraStreamBindings(
    const core::VertexDeclSnapshot& vertexDecl,
    const core::FlatDrawStateRecord& hot,
    const ParamView& pv) {
  ProgrammableVsExtraStreamBindingList bindings;
  if (!pv.userVertexData.empty()) {
    return bindings;
  }

  for (u32 stream = 1; stream < vertexDecl.streams.size(); ++stream) {
    if (!vertexDeclUsesStream(vertexDecl, stream)) {
      continue;
    }

    const u32 stride = ffp::computeVertexDeclStreamStride(vertexDecl, stream);
    u64 offset = hot.streamOffsets[stream];
    const bool instanced =
        (hot.streamFrequencies[stream] & core::kStreamSourceInstanceData) != 0;
    if (!pv.indexed && !instanced && stride != 0u) {
      offset += static_cast<u64>(pv.startVertex) * static_cast<u64>(stride);
    }
    bindings.push_back(ProgrammableVsExtraStreamBinding{
        .stream = stream,
        .metalSlot = ffp::vertexShaderStreamBufferSlot(stream),
        .offset = offset,
        .stride = stride,
    });
  }
  return bindings;
}

inline FragmentTextureSamplerBindingList makeFragmentTextureSamplerBindings(
    const core::FlatDrawStateRecord& hot,
    const core::ShaderRef* pixelShader = nullptr) {
  FragmentTextureSamplerBindingList bindings;
  const u32 activeMask = pixelShader
      ? drawshader::activeFragmentTextureMaskForShader(*pixelShader, hot.textureMask)
      : (hot.textureMask & ((1u << core::kMaxFragmentSamplers) - 1u));
  for (u32 stage = 0; stage < core::kMaxFragmentSamplers; ++stage) {
    if ((activeMask & (1u << stage)) == 0u) {
      continue;
    }
    const auto textureHandle = hot.textures[stage];
    if (!textureHandle) {
      continue;
    }
    bindings.push_back(FragmentTextureSamplerBinding{
        .stage = stage,
        .texture = textureHandle,
        .textureLod = hot.textureLods[stage],
        .samplerStateHash = hot.key.samplerStateHashes[stage],
        .samplerStates = hot.samplerStates[stage],
    });
  }
  return bindings;
}

template <std::size_t MaxEntries>
inline bool shouldAutoExpandIndexedDraw(const core::FlatStateSet<MaxEntries>& renderStates,
                                        u32 textureMask,
                                        bool fixedFunctionPath,
                                        bool ffpDecodableLayout,
                                        bool texture0R32FCube) {
  const bool alphaBlendEnabled =
      core::flatStateOr(renderStates, core::RS_ALPHABLEND_ENABLE, 0u) != 0u;
  const bool invDestColorAddBlend =
      core::flatStateOr(renderStates, core::RS_SRC_BLEND, 0u) ==
          static_cast<u32>(core::BlendFactor::InvDestColor) &&
      core::flatStateOr(renderStates, core::RS_DEST_BLEND, 0u) ==
          static_cast<u32>(core::BlendFactor::One);
  if (!alphaBlendEnabled || !invDestColorAddBlend || !ffpDecodableLayout) {
    return false;
  }

  if (fixedFunctionPath && (textureMask & 0x3fu) == 0x3fu) {
    return true;
  }

  return texture0R32FCube && (textureMask & 0x1fu) == 0x1fu;
}

template <std::size_t MaxEntries>
inline bool shouldOptimizeOpaqueDepthIndexOrder(
    const core::FlatStateSet<MaxEntries>& renderStates,
    WMTTriangleFillMode fillMode,
    bool depthWriteGloballyDisabled = false,
    bool extendedScope = false) {
  if (fillMode != WMTTriangleFillModeFill) {
    return false;
  }
  const bool alphaBlendEnabled =
      core::flatStateOr(renderStates, core::RS_ALPHABLEND_ENABLE, 0u) != 0u;
  if (alphaBlendEnabled) {
    const bool sourceReplacementBlend =
        extendedScope &&
        core::flatStateOr(
            renderStates, core::RS_SRC_BLEND,
            static_cast<u32>(core::BlendFactor::One)) ==
            static_cast<u32>(core::BlendFactor::One) &&
        core::flatStateOr(
            renderStates, core::RS_DEST_BLEND,
            static_cast<u32>(core::BlendFactor::Zero)) ==
            static_cast<u32>(core::BlendFactor::Zero) &&
        core::flatStateOr(
            renderStates, core::RS_BLEND_OP,
            static_cast<u32>(core::BlendOp::Add)) ==
            static_cast<u32>(core::BlendOp::Add) &&
        core::flatStateOr(
            renderStates, core::RS_SEPARATE_ALPHA_BLEND_ENABLE, 0u) == 0u;
    if (!sourceReplacementBlend) {
      return false;
    }
  }
  if (core::flatStateOr(renderStates, core::RS_ALPHA_TEST_ENABLE, 0u) != 0u) {
    return false;
  }
  if (core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u) {
    return false;
  }
  if (core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u) {
    return false;
  }

  const bool depthEnabled =
      core::flatStateOr(renderStates, core::RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && !depthWriteGloballyDisabled &&
      core::flatStateOr(renderStates, core::RS_Z_WRITE_ENABLE, 0u) != 0u;
  if (!depthWrite) {
    return false;
  }

  switch (static_cast<core::CompareFunc>(core::flatStateOr(
      renderStates,
      core::RS_Z_FUNC,
      static_cast<u32>(core::CompareFunc::LessEqual)))) {
    case core::CompareFunc::Less:
    case core::CompareFunc::LessEqual:
      return true;
    case core::CompareFunc::Greater:
    case core::CompareFunc::GreaterEqual:
      return extendedScope;
    default:
      return false;
  }
}

inline bool drawParamPayloadRangesEqual(
    core::DrawPayloadRange left,
    core::DrawPayloadRange right,
    std::span<const u8> arena) noexcept {
  if (left.size != right.size) {
    return false;
  }
  if (left.size == 0u) {
    return true;
  }
  const auto leftBytes = core::drawPayloadRangeBytes(left, arena);
  const auto rightBytes = core::drawPayloadRangeBytes(right, arena);
  if (leftBytes.size() != left.size || rightBytes.size() != right.size) {
    return false;
  }
  return std::memcmp(leftBytes.data(), rightBytes.data(), leftBytes.size()) == 0;
}

struct CompatibleIndexedDrawMerge {
  core::DrawParam param{};
  std::size_t drawCount = 0;
};

enum class CompatibleIndexedDrawMergeReject : u32 {
  SourceShape = 1u << 0,
  NextShape = 1u << 1,
  IndexType = 1u << 2,
  BaseVertex = 1u << 3,
  StartVertex = 1u << 4,
  Uniform = 1u << 5,
  BindingOverride = 1u << 6,
  BindingSnapshot = 1u << 7,
  NonContiguousIndexRange = 1u << 8,
  PrimitiveCountOverflow = 1u << 9,
};

constexpr std::size_t kCompatibleIndexedDrawMergeRejectCount = 10u;
constexpr std::size_t kCompatibleIndexedDrawMergeRelaxationSetCount = 8u;

enum class CompatibleIndexedDrawMergeRelaxation : u32 {
  BindingPayload = 1u << 0,
  Uniform = 1u << 1,
  NonContiguousIndexRange = 1u << 2,
};

constexpr u32 compatibleIndexedDrawMergeRejectBit(
    CompatibleIndexedDrawMergeReject reject) noexcept {
  return static_cast<u32>(reject);
}

constexpr std::size_t compatibleIndexedDrawMergeRejectIndex(
    CompatibleIndexedDrawMergeReject reject) noexcept {
  return static_cast<std::size_t>(
      std::countr_zero(compatibleIndexedDrawMergeRejectBit(reject)));
}

struct CompatibleIndexedDrawMergeTelemetry {
  u64 pairAttempts = 0u;
  u64 compatiblePairs = 0u;
  u64 multipleRejectPairs = 0u;
  std::array<u64, kCompatibleIndexedDrawMergeRejectCount> rejectPairs{};
  std::array<u64, kCompatibleIndexedDrawMergeRejectCount> onlyRejectPairs{};
  std::array<u64, kCompatibleIndexedDrawMergeRelaxationSetCount>
      exactRelaxationSetPairs{};
  u64 otherRelaxationSetPairs = 0u;

  void record(u32 rejectMask) noexcept {
    ++pairAttempts;
    if (rejectMask == 0u) {
      ++compatiblePairs;
      return;
    }

    for (std::size_t i = 0u; i < rejectPairs.size(); ++i) {
      if ((rejectMask & (1u << i)) != 0u) {
        ++rejectPairs[i];
      }
    }
    if (std::popcount(rejectMask) == 1) {
      const auto index = static_cast<std::size_t>(std::countr_zero(rejectMask));
      ++onlyRejectPairs[index];
    } else {
      ++multipleRejectPairs;
    }

    const u32 bindingPayloadMask =
        compatibleIndexedDrawMergeRejectBit(
            CompatibleIndexedDrawMergeReject::BindingOverride) |
        compatibleIndexedDrawMergeRejectBit(
            CompatibleIndexedDrawMergeReject::BindingSnapshot);
    const u32 uniformMask = compatibleIndexedDrawMergeRejectBit(
        CompatibleIndexedDrawMergeReject::Uniform);
    const u32 nonContiguousMask = compatibleIndexedDrawMergeRejectBit(
        CompatibleIndexedDrawMergeReject::NonContiguousIndexRange);
    const u32 relaxationRejectMask =
        bindingPayloadMask | uniformMask | nonContiguousMask;
    if ((rejectMask & ~relaxationRejectMask) != 0u) {
      ++otherRelaxationSetPairs;
      return;
    }

    u32 relaxationSet = 0u;
    if ((rejectMask & bindingPayloadMask) != 0u) {
      relaxationSet |= static_cast<u32>(
          CompatibleIndexedDrawMergeRelaxation::BindingPayload);
    }
    if ((rejectMask & uniformMask) != 0u) {
      relaxationSet |=
          static_cast<u32>(CompatibleIndexedDrawMergeRelaxation::Uniform);
    }
    if ((rejectMask & nonContiguousMask) != 0u) {
      relaxationSet |= static_cast<u32>(
          CompatibleIndexedDrawMergeRelaxation::NonContiguousIndexRange);
    }
    ++exactRelaxationSetPairs[relaxationSet];
  }
};

inline bool compatibleIndexedDrawMergeShape(
    const core::DrawParam& draw) noexcept {
  return draw.indexed &&
         draw.primitiveType == core::PrimitiveType::TriangleList &&
         draw.instanceCount == 1u && draw.userVertexRange.empty() &&
         draw.userIndexRange.empty();
}

inline u32 classifyCompatibleIndexedDrawMergePair(
    const core::DrawParam& source,
    const core::DrawParam& next,
    std::span<const u8> payloadArena) noexcept {
  u32 rejectMask = 0u;
  if (!compatibleIndexedDrawMergeShape(source)) {
    rejectMask |= compatibleIndexedDrawMergeRejectBit(
        CompatibleIndexedDrawMergeReject::SourceShape);
  }
  if (!compatibleIndexedDrawMergeShape(next)) {
    rejectMask |= compatibleIndexedDrawMergeRejectBit(
        CompatibleIndexedDrawMergeReject::NextShape);
  }
  if (rejectMask != 0u) {
    return rejectMask;
  }

  const auto rejectIf = [&](bool rejected,
                            CompatibleIndexedDrawMergeReject reason) {
    if (rejected) {
      rejectMask |= compatibleIndexedDrawMergeRejectBit(reason);
    }
  };
  rejectIf(next.indexType != source.indexType,
           CompatibleIndexedDrawMergeReject::IndexType);
  rejectIf(next.baseVertexIndex != source.baseVertexIndex,
           CompatibleIndexedDrawMergeReject::BaseVertex);
  rejectIf(next.startVertex != source.startVertex,
           CompatibleIndexedDrawMergeReject::StartVertex);
  rejectIf(next.uniformHandle != source.uniformHandle,
           CompatibleIndexedDrawMergeReject::Uniform);
  rejectIf(!drawParamPayloadRangesEqual(
               source.bindingOverrideRange,
               next.bindingOverrideRange,
               payloadArena),
           CompatibleIndexedDrawMergeReject::BindingOverride);
  rejectIf(!drawParamPayloadRangesEqual(
               source.bindingSnapshotRange,
               next.bindingSnapshotRange,
               payloadArena),
           CompatibleIndexedDrawMergeReject::BindingSnapshot);

  const u64 nextStart =
      static_cast<u64>(source.startIndex) +
      static_cast<u64>(source.primitiveCount) * 3u;
  const u64 combinedPrimitiveCount =
      static_cast<u64>(source.primitiveCount) + next.primitiveCount;
  rejectIf(nextStart != next.startIndex,
           CompatibleIndexedDrawMergeReject::NonContiguousIndexRange);
  rejectIf(combinedPrimitiveCount > std::numeric_limits<u32>::max() / 3u,
           CompatibleIndexedDrawMergeReject::PrimitiveCountOverflow);
  return rejectMask;
}

inline CompatibleIndexedDrawMergeTelemetry
measureCompatibleIndexedDrawMergePairs(
    std::span<const core::DrawParam> draws,
    std::span<const u8> payloadArena) noexcept {
  CompatibleIndexedDrawMergeTelemetry telemetry{};
  for (std::size_t i = 1u; i < draws.size(); ++i) {
    telemetry.record(classifyCompatibleIndexedDrawMergePair(
        draws[i - 1u], draws[i], payloadArena));
  }
  return telemetry;
}

// Collapse only a byte-contiguous source-IB span. This preserves the exact
// submitted index sequence and avoids a transient joined-index allocation.
// InstanceCount is restricted to one because two instanced draws order work as
// A0,A1,B0,B1 while one combined draw would order it A0,B0,A1,B1.
inline CompatibleIndexedDrawMerge makeCompatibleIndexedDrawMerge(
    std::span<const core::DrawParam> draws,
    std::span<const u8> payloadArena) noexcept {
  CompatibleIndexedDrawMerge result{};
  if (draws.empty()) {
    return result;
  }

  result.param = draws.front();
  result.drawCount = 1u;
  if (!compatibleIndexedDrawMergeShape(result.param)) {
    return result;
  }

  for (std::size_t i = 1u; i < draws.size(); ++i) {
    const auto& next = draws[i];
    if (classifyCompatibleIndexedDrawMergePair(
            result.param, next, payloadArena) != 0u) {
      break;
    }

    const u64 combinedPrimitiveCount =
        static_cast<u64>(result.param.primitiveCount) + next.primitiveCount;
    result.param.primitiveCount = static_cast<u32>(combinedPrimitiveCount);
    ++result.drawCount;
  }
  return result;
}

// A versioned MANAGED/DYNAMIC index binding is just as immutable for one
// submitted draw as a non-versioned BufferRecord: the snapshot pins its
// concrete backing until seqId completion and carries the exact content
// revision used to key derived reordered buffers. Require CPU-visible bytes
// because a cache miss must build the reordered candidate from that snapshot.
inline bool isStableIndexCacheSource(
    bool userIndexDataEmpty,
    bool sourceRecordExists,
    bool sourceRecordHasBuffer,
    const core::DrawBufferBindingSnapshot* snapshot) noexcept {
  if (!userIndexDataEmpty || !sourceRecordExists) {
    return false;
  }
  if (!snapshot) {
    return sourceRecordHasBuffer;
  }
  return snapshot->valid() && snapshot->contentsAddress != 0u &&
         snapshot->byteSize != 0u && snapshot->contentRevision != 0u;
}

template <std::size_t MaxEntries>
inline bool shouldOptimizeScreenBlendIndexOrder(
    const core::FlatStateSet<MaxEntries>& renderStates) {
  const bool alphaBlendEnabled =
      core::flatStateOr(renderStates, core::RS_ALPHABLEND_ENABLE, 0u) != 0u;
  const bool screenBlend =
      core::flatStateOr(renderStates, core::RS_SRC_BLEND, 0u) ==
          static_cast<u32>(core::BlendFactor::InvDestColor) &&
      core::flatStateOr(renderStates, core::RS_DEST_BLEND, 0u) ==
          static_cast<u32>(core::BlendFactor::One) &&
      core::flatStateOr(renderStates,
                        core::RS_BLEND_OP,
                        static_cast<u32>(core::BlendOp::Add)) ==
          static_cast<u32>(core::BlendOp::Add);
  const bool separateAlpha =
      core::flatStateOr(renderStates,
                        core::RS_SEPARATE_ALPHA_BLEND_ENABLE,
                        0u) != 0u;
  const bool depthEnabled =
      core::flatStateOr(renderStates, core::RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled &&
      core::flatStateOr(renderStates, core::RS_Z_WRITE_ENABLE, 0u) != 0u;
  const bool alphaTestEnabled =
      core::flatStateOr(renderStates, core::RS_ALPHA_TEST_ENABLE, 0u) != 0u;
  const bool stencilEnabled =
      core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u;
  const bool clipPlaneEnabled =
      core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u;

  return alphaBlendEnabled && screenBlend && !separateAlpha &&
         depthEnabled && !depthWrite && !alphaTestEnabled &&
         !stencilEnabled && !clipPlaneEnabled;
}

inline VertexTextureSamplerBindingList makeVertexTextureSamplerBindings(
    const core::FlatDrawStateRecord& hot) {
  VertexTextureSamplerBindingList bindings;
  for (u32 stage = 0; stage < core::kMaxVertexTextureSamplers; ++stage) {
    const u32 textureSlot = core::kVertexTextureSampler0 + stage;
    if ((hot.textureMask & (1u << textureSlot)) == 0u) {
      continue;
    }
    const auto textureHandle = hot.textures[textureSlot];
    if (!textureHandle) {
      continue;
    }
    bindings.push_back(VertexTextureSamplerBinding{
        .stage = stage,
        .texture = textureHandle,
        .textureLod = hot.textureLods[textureSlot],
        .samplerStateHash = hot.key.samplerStateHashes[textureSlot],
        .samplerStates = hot.samplerStates[textureSlot],
    });
  }
  return bindings;
}

inline EncoderRasterStatePlan makeEncoderRasterStatePlan(
    const core::FlatDrawStateRecord& hot,
    u32 surfaceWidth,
    u32 surfaceHeight,
    bool preTransformed,
    bool scissorDisabled,
    bool cullDisabled,
    const core::ViewportScissor* viewportOverride = nullptr) {
  const auto& viewport = viewportOverride ? *viewportOverride : hot.viewport;
  const auto targetWidth = std::max(1u, surfaceWidth);
  const auto targetHeight = std::max(1u, surfaceHeight);
  double viewportWidth = static_cast<double>(std::max(1u, viewport.viewport.width));
  double viewportHeight = static_cast<double>(std::max(1u, viewport.viewport.height));
  double viewportOriginX = static_cast<double>(viewport.viewport.x);
  double viewportOriginY = static_cast<double>(viewport.viewport.y);
  if (preTransformed) {
    viewportOriginX = 0.0;
    viewportOriginY = 0.0;
    viewportWidth = static_cast<double>(targetWidth);
    viewportHeight = static_cast<double>(targetHeight);
  }

  WMTScissorRect scissor{};
  if (viewport.scissorEnabled && !scissorDisabled) {
    scissor.x = static_cast<std::uint64_t>(std::max(0, viewport.scissor.left));
    scissor.y = static_cast<std::uint64_t>(std::max(0, viewport.scissor.top));
    scissor.width = static_cast<std::uint64_t>(
        std::max(0, viewport.scissor.right - viewport.scissor.left));
    scissor.height = static_cast<std::uint64_t>(
        std::max(0, viewport.scissor.bottom - viewport.scissor.top));
  } else {
    scissor.x = 0;
    scissor.y = 0;
    scissor.width = static_cast<std::uint64_t>(targetWidth);
    scissor.height = static_cast<std::uint64_t>(targetHeight);
  }

  WMTCullMode cullMode = WMTCullModeNone;
  if (!preTransformed && !cullDisabled) {
    cullMode = static_cast<WMTCullMode>(convert::toCullMode(
        core::flatStateOr(hot.renderStates, core::RS_CULL_MODE, 1u)));
  }

  return EncoderRasterStatePlan{
      .viewport = WMTViewport{
          viewportOriginX,
          viewportOriginY,
          viewportWidth,
          viewportHeight,
          static_cast<double>(viewport.viewport.minZ),
          static_cast<double>(viewport.viewport.maxZ),
      },
      .scissor = scissor,
      .cullMode = cullMode,
  };
}

inline DrawBindingPacketPlan makeDrawBindingPacketPlan(
    const core::VertexDeclSnapshot& vertexDecl,
    const core::FlatDrawStateRecord& hot,
    const ParamView& pv,
    u32 surfaceWidth,
    u32 surfaceHeight,
    bool preTransformed,
    bool scissorDisabled,
    bool cullDisabled,
    const core::ShaderRef* pixelShader = nullptr,
    const core::ViewportScissor* viewportOverride = nullptr) {
  return DrawBindingPacketPlan{
      .fragmentTextureSamplers = makeFragmentTextureSamplerBindings(hot, pixelShader),
      .vertexTextureSamplers = makeVertexTextureSamplerBindings(hot),
      .extraStreams = makeProgrammableVsExtraStreamBindings(vertexDecl, hot, pv),
      .raster = makeEncoderRasterStatePlan(
          hot,
          surfaceWidth,
          surfaceHeight,
          preTransformed,
          scissorDisabled,
          cullDisabled,
          viewportOverride),
  };
}

// Geometry-trace recorder (env-gated). Called from encodeDraw for the
// indexed / expanded-indexed / non-indexed paths. Implementation lives
// in dxmt9_draw_encoder_diagnostics.mm.
void recordDrawGeometryDiagnostics(core::FlatDrawStateView drawState,
                                   const ParamView& pv,
                                   u64 seqId,
                                   u64 vertexCount,
                                   u64 vertexBufferOffset,
                                   u32 vertexStreamOffset,
                                   u32 vertexStreamStride,
                                   bool indexed,
                                   bool direct,
                                   bool up,
                                   bool expanded,
                                   bool fixedFunctionPath);

}  // namespace dxmt9::encoders
