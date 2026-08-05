#pragma once

#include "dxmt9_source_payload.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dxmt9::core {

enum class SourceEntryEncoderKind : std::uint8_t {
  Empty,
  Render,
  Blit,
  Present,
  Invalid,
};

enum class SourceSemanticBoundaryKind : std::uint8_t {
  None,
  AttachmentChange,
  ResourceHazard,
  RouteChange,
  Clear,
  Blit,
  Readback,
  Present,
  Invalid,
};

enum class RenderRoute : std::uint8_t {
  Unknown,
  Portable,
  Tile,
};

enum SourceSemanticFlags : std::uint32_t {
  SourceSemanticSealed = 1u << 0,
  SourceSemanticHasPresent = 1u << 1,
  SourceSemanticGlobalObservation = 1u << 2,
  SourceSemanticCaptureIsolation = 1u << 3,
  SourceSemanticInitializerRequirement = 1u << 4,
  SourceSemanticEntryStable = 1u << 5,
  SourceSemanticInvalid = 1u << 6,
  SourceSemanticFinalPresentTail = 1u << 7,
};

enum ExactResourceSetFlags : std::uint32_t {
  ExactResourceSetComplete = 1u << 0,
  ExactResourceSetCanonicalized = 1u << 1,
  ExactResourceSetOverflow = 1u << 2,
};

struct RenderAttachmentKey {
  std::array<RenderTargetAttachment, kMaxRenderTargets> color{};
  RenderTargetAttachment depthStencil{};
  std::uint32_t sampleCount = 1;

  friend constexpr bool operator==(const RenderAttachmentKey&,
                                   const RenderAttachmentKey&) = default;
};

// Continuation admission is a fast conservative proof, not the exact replay
// authority. Twelve inline identities cover the common index + stream +
// sampled-texture entry shape while keeping every copied summary cache-sized.
// Wider entries set Overflow/Incomplete and fall back to exact replay.
inline constexpr std::size_t kSourceEntryResourceCapacity = 12;

struct ExactResourceSet {
  std::array<std::uint64_t, kSourceEntryResourceCapacity> handles{};
  std::uint32_t count = 0;
  std::uint32_t flags = ExactResourceSetComplete;

  constexpr bool complete() const noexcept {
    return (flags & ExactResourceSetComplete) != 0 &&
           (flags & ExactResourceSetOverflow) == 0;
  }

  constexpr bool canonicalized() const noexcept {
    return (flags & ExactResourceSetCanonicalized) != 0;
  }

  constexpr bool add(std::uint64_t value) noexcept {
    if (value == 0) {
      return true;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      if (handles[i] == value) {
        return true;
      }
    }
    if (count >= handles.size()) {
      flags &= ~ExactResourceSetComplete;
      flags |= ExactResourceSetOverflow;
      return false;
    }
    handles[count++] = value;
    return true;
  }

  constexpr bool overlaps(const ExactResourceSet& other) const noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
      for (std::uint32_t j = 0; j < other.count; ++j) {
        if (handles[i] == other.handles[j]) {
          return true;
        }
      }
    }
    return false;
  }

  friend constexpr bool operator==(const ExactResourceSet&,
                                   const ExactResourceSet&) = default;
};

enum RenderContinuationKeyFlags : std::uint32_t {
  RenderContinuationKeyValid = 1u << 0,
  RenderContinuationEntryStateComplete = 1u << 1,
  RenderContinuationPreRenderBarrier = 1u << 2,
};

struct RenderContinuationKey {
  RenderAttachmentKey attachments{};
  ExactResourceSet entryReads{};
  RenderRoute route = RenderRoute::Unknown;
  std::uint64_t passActionEpoch = 1;
  std::uint32_t flags = 0;

  constexpr bool valid() const noexcept {
    return (flags & RenderContinuationKeyValid) != 0;
  }

  constexpr bool entryStateComplete() const noexcept {
    return (flags & RenderContinuationEntryStateComplete) != 0;
  }

  friend constexpr bool operator==(const RenderContinuationKey&,
                                   const RenderContinuationKey&) = default;
};

inline constexpr std::uint32_t kNoSourceSemanticBoundary =
    std::numeric_limits<std::uint32_t>::max();

struct SourceSemanticSummary {
  RenderContinuationKey entryRender{};
  std::uint64_t byteCount = 0;
  std::uint32_t commandCount = 0;
  std::uint32_t drawCount = 0;
  std::uint32_t pageCount = 0;
  std::uint32_t firstBoundaryOrdinal = kNoSourceSemanticBoundary;
  SourceEntryEncoderKind entryKind = SourceEntryEncoderKind::Empty;
  SourceSemanticBoundaryKind firstBoundary =
      SourceSemanticBoundaryKind::None;
  std::uint32_t flags = 0;

  constexpr bool sealed() const noexcept {
    return (flags & SourceSemanticSealed) != 0;
  }

  constexpr bool valid() const noexcept {
    return sealed() && (flags & SourceSemanticInvalid) == 0;
  }

  constexpr bool hasPresent() const noexcept {
    return (flags & SourceSemanticHasPresent) != 0;
  }

  constexpr bool hasFinalPresentTail() const noexcept {
    return (flags & SourceSemanticFinalPresentTail) != 0;
  }

  constexpr bool requiresIsolation() const noexcept {
    return (flags & (SourceSemanticGlobalObservation |
                     SourceSemanticCaptureIsolation)) != 0;
  }

  constexpr bool entryStable() const noexcept {
    return (flags & SourceSemanticEntryStable) != 0;
  }

  friend constexpr bool operator==(const SourceSemanticSummary&,
                                   const SourceSemanticSummary&) = default;
};

struct SourceSemanticSummaryContext {
  std::uint64_t byteCount = 0;
  std::uint32_t pageCount = 0;
  RenderRoute firstRenderRoute = RenderRoute::Unknown;
  std::uint64_t passActionEpoch = 1;
  bool sealed = true;
  bool entryStable = true;
  bool resourcesCanonicalized = false;
  bool captureIsolation = false;
  bool globalObservation = false;
  bool initializerRequirement = false;
};

RenderAttachmentKey makeRenderAttachmentKey(
    const FlatDrawStateRecord& hot) noexcept;
ExactResourceSet makeRenderAttachmentWriteSet(
    const FlatDrawStateRecord& hot,
    bool canonicalized = false) noexcept;
ExactResourceSet makeDrawEntryReadSet(
    FlatDrawStateView state,
    bool canonicalized = false) noexcept;
SourceSemanticSummary summarizeSourcePayload(
    SourcePayloadView payload,
    SourceSemanticSummaryContext context = {}) noexcept;
std::size_t measureSourcePayloadLogicalExtent(
    SourcePayloadView payload) noexcept;

static_assert(std::is_trivially_copyable_v<RenderAttachmentKey>);
static_assert(std::is_standard_layout_v<RenderAttachmentKey>);
static_assert(std::is_trivially_copyable_v<ExactResourceSet>);
static_assert(std::is_standard_layout_v<ExactResourceSet>);
static_assert(std::is_trivially_copyable_v<RenderContinuationKey>);
static_assert(std::is_standard_layout_v<RenderContinuationKey>);
static_assert(std::is_trivially_copyable_v<SourceSemanticSummary>);
static_assert(std::is_standard_layout_v<SourceSemanticSummary>);
static_assert(sizeof(ExactResourceSet) <= 112,
              "entry resource proof must remain compact");
static_assert(sizeof(SourceSemanticSummary) <= 256,
              "source admission summary must remain compact");

}  // namespace dxmt9::core
