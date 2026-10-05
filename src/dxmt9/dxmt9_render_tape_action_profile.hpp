#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace dxmt9 {

inline constexpr std::uint32_t kRenderTapeActionProfileVersion = 1u;
inline constexpr std::uint32_t kRenderTapeActionProfileNormal = 0u;
inline constexpr std::uint32_t kRenderTapeActionProfileExact = 1u;

// A coherent cold observation of real queue policy, not a declaration inferred
// by a publisher. Invalid/uninitialized history remains unknown. Capture token,
// epoch, queue identity and frozen transport authentication belong to the
// capture binding; this flat value carries no borrowed owner or queue pointer.
struct RenderTapeActionProfileSnapshot {
  std::uint32_t version = kRenderTapeActionProfileVersion;
  std::uint32_t constructionProfile = kRenderTapeActionProfileNormal;
  std::uint32_t currentProfile = kRenderTapeActionProfileNormal;
  std::uint64_t revision = 0u;
  bool valid = false;
};

static_assert(std::is_trivially_copyable_v<RenderTapeActionProfileSnapshot>);
static_assert(std::is_standard_layout_v<RenderTapeActionProfileSnapshot>);

struct RenderTapeActionProfileHistoryTestAccess;

// Pure reducer, externally serialized by the queue's cold policy mutex.
// Initialization is one-shot and separate from changes: constructing exact
// policy starts revision zero, whereas normal -> exact is an observed change.
class RenderTapeActionProfileHistory {
public:
  [[nodiscard]] constexpr bool initialize(bool exact) noexcept {
    if (initialized_) {
      snapshot_.valid = false;
      return false;
    }
    initialized_ = true;
    snapshot_.constructionProfile = snapshot_.currentProfile = profile(exact);
    snapshot_.revision = 0u;
    snapshot_.valid = true;
    return true;
  }

  constexpr void set(bool exact) noexcept {
    // An ordinary setter cannot invent a construction fact or recover lost
    // history. Even this invalid path continues reporting actual current policy.
    if (!initialized_) {
      initialized_ = true;
      snapshot_.valid = false;
    }
    const auto next = profile(exact);
    if (snapshot_.currentProfile == next) return;
    snapshot_.currentProfile = next;
    if (snapshot_.revision == std::numeric_limits<std::uint64_t>::max()) {
      snapshot_.valid = false;
      return;
    }
    ++snapshot_.revision;
  }

  [[nodiscard]] constexpr RenderTapeActionProfileSnapshot snapshot() const noexcept {
    return snapshot_;
  }

private:
  friend struct RenderTapeActionProfileHistoryTestAccess;

  static constexpr std::uint32_t profile(bool exact) noexcept {
    return exact ? kRenderTapeActionProfileExact : kRenderTapeActionProfileNormal;
  }

  RenderTapeActionProfileSnapshot snapshot_{};
  bool initialized_ = false;
};

// Qualification only: diagnostic replay may construct normal then enable exact
// policy, and its snapshot truthfully remains valid but does not meet this gate.
// Revision equality is mandatory even when endpoint booleans match (ABA).
[[nodiscard]] constexpr bool renderTapeActionProfileStableExact(
    const RenderTapeActionProfileSnapshot& arm,
    const RenderTapeActionProfileSnapshot& freeze) noexcept {
  return arm.valid && freeze.valid &&
      arm.version == kRenderTapeActionProfileVersion &&
      freeze.version == kRenderTapeActionProfileVersion &&
      arm.constructionProfile == kRenderTapeActionProfileExact &&
      freeze.constructionProfile == kRenderTapeActionProfileExact &&
      arm.currentProfile == kRenderTapeActionProfileExact &&
      freeze.currentProfile == kRenderTapeActionProfileExact &&
      arm.revision == 0u && freeze.revision == arm.revision;
}

} // namespace dxmt9
