#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dxmt9::d3d9 {

inline constexpr std::uint32_t kRenderTapeActionWitnessV1Version = 1u;
inline constexpr char kRenderTapeActionWitnessV1Schema[] =
    "dxmt9.render_tape.action_witness.v1";

enum class RenderTapeActionWitnessEvidenceV1 : std::uint32_t {
  LegacyUnknown = 0u,
  QueueObserved = 1u,
};

enum class RenderTapeActionProfileV1 : std::uint32_t {
  Normal = 0u,
  TapeExact = 1u,
};

// Pointer-free queue observation, not caller/environment attestation. The
// monotonic revisions retain pre-arm and interval changes, including ABA.
// LegacyUnknown is canonical absence of evidence, never a normal-policy claim.
struct RenderTapeActionWitnessV1 {
  std::uint32_t version = kRenderTapeActionWitnessV1Version;
  std::uint32_t evidence = 0u;
  std::uint32_t constructionProfile = 0u;
  std::uint32_t armProfile = 0u;
  std::uint32_t freezeProfile = 0u;
  std::uint32_t reserved = 0u;
  std::uint64_t captureToken = 0u;
  std::uint64_t captureEpoch = 0u;
  std::uint64_t queue = 0u;
  std::uint64_t armRevision = 0u;
  std::uint64_t freezeRevision = 0u;
  friend constexpr bool operator==(const RenderTapeActionWitnessV1&,
      const RenderTapeActionWitnessV1&) = default;
};

static_assert(sizeof(RenderTapeActionWitnessV1) == 64u);
static_assert(offsetof(RenderTapeActionWitnessV1, captureToken) == 24u);
static_assert(offsetof(RenderTapeActionWitnessV1, freezeRevision) == 56u);
static_assert(std::is_trivially_copyable_v<RenderTapeActionWitnessV1>);
static_assert(std::is_standard_layout_v<RenderTapeActionWitnessV1>);

inline constexpr bool renderTapeActionWitnessV1Valid(
    const RenderTapeActionWitnessV1& value) noexcept {
  if (value.version != kRenderTapeActionWitnessV1Version || value.reserved)
    return false;
  if (value.evidence == static_cast<std::uint32_t>(
          RenderTapeActionWitnessEvidenceV1::LegacyUnknown)) {
    return !value.constructionProfile && !value.armProfile && !value.freezeProfile &&
        !value.captureToken && !value.captureEpoch && !value.queue &&
        !value.armRevision && !value.freezeRevision;
  }
  return value.evidence == static_cast<std::uint32_t>(
             RenderTapeActionWitnessEvidenceV1::QueueObserved) &&
      value.captureToken && value.captureEpoch && value.queue &&
      value.constructionProfile <= 1u && value.armProfile <= 1u &&
      value.freezeProfile <= 1u && value.freezeRevision >= value.armRevision &&
      (value.constructionProfile ^ value.armProfile) == (value.armRevision & 1u) &&
      (value.constructionProfile ^ value.freezeProfile) == (value.freezeRevision & 1u) &&
      (value.freezeRevision != value.armRevision || value.armProfile == value.freezeProfile) &&
      (value.armRevision || value.constructionProfile == value.armProfile);
}

// Unknown has no binding authority. Transfer admission may preserve its
// canonical absence, but only QueueObserved can satisfy this predicate.
inline constexpr bool renderTapeActionWitnessV1MatchesBinding(
    const RenderTapeActionWitnessV1& value, std::uint64_t token,
    std::uint64_t epoch, std::uint64_t queue) noexcept {
  return renderTapeActionWitnessV1Valid(value) &&
      value.evidence == static_cast<std::uint32_t>(
          RenderTapeActionWitnessEvidenceV1::QueueObserved) &&
      token && epoch && queue && value.captureToken == token &&
      value.captureEpoch == epoch && value.queue == queue;
}

// Qualification is stricter than structural validity: even a stable post-arm
// profile cannot erase a pre-arm change from the queue's construction policy.
inline constexpr bool renderTapeActionWitnessV1StableExact(
    const RenderTapeActionWitnessV1& value) noexcept {
  return renderTapeActionWitnessV1Valid(value) &&
      value.evidence == static_cast<std::uint32_t>(
          RenderTapeActionWitnessEvidenceV1::QueueObserved) &&
      value.constructionProfile == 1u && value.armProfile == 1u &&
      value.freezeProfile == 1u && !value.armRevision && !value.freezeRevision;
}

} // namespace dxmt9::d3d9
