#include "dxmt9/dxmt9_render_tape_action_profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dxmt9 {

struct RenderTapeActionProfileHistoryTestAccess {
  static void revision(RenderTapeActionProfileHistory& history,
                       std::uint64_t value) noexcept {
    history.snapshot_.revision = value;
  }
};

} // namespace dxmt9

namespace {

using namespace dxmt9;

std::uint64_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

void checkSnapshot(const RenderTapeActionProfileSnapshot& observed,
                   bool construction, bool current, std::uint64_t changes,
                   bool valid) {
  check(observed.version == 1u &&
            observed.constructionProfile == (construction ? 1u : 0u) &&
            observed.currentProfile == (current ? 1u : 0u) &&
            observed.revision == changes && observed.valid == valid,
        "snapshot reports real immutable construction/current/change history");
}

void testConstructionAndNormalDiagnosticEnable() {
  RenderTapeActionProfileHistory exact;
  check(!exact.snapshot().valid, "uninitialized history is unknown");
  check(!renderTapeActionProfileStableExact(exact.snapshot(), exact.snapshot()),
        "uninitialized endpoint values cannot qualify");
  check(exact.initialize(true), "actual exact construction initializes once");
  const auto construction = exact.snapshot();
  checkSnapshot(construction, true, true, 0u, true);
  check(renderTapeActionProfileStableExact(construction, construction),
        "exact construction with no changes can qualify");
  for (std::uint32_t i = 0u; i != 8u; ++i) exact.set(true);
  checkSnapshot(exact.snapshot(), true, true, 0u, true);
  check(renderTapeActionProfileStableExact(construction, exact.snapshot()),
        "same-value cold setter does not invalidate stable exact policy");

  RenderTapeActionProfileHistory diagnostic;
  check(diagnostic.initialize(false), "diagnostic may construct normal policy");
  checkSnapshot(diagnostic.snapshot(), false, false, 0u, true);
  diagnostic.set(true);
  const auto enabled = diagnostic.snapshot();
  checkSnapshot(enabled, false, true, 1u, true);
  check(!renderTapeActionProfileStableExact(enabled, enabled),
        "normal-then-exact diagnostic is supported but not construction-exact");
  diagnostic.set(true);
  checkSnapshot(diagnostic.snapshot(), false, true, 1u, true);
}

void testAbaBeforeAndDuringTheCapturedInterval() {
  RenderTapeActionProfileHistory history;
  check(history.initialize(true), "ABA fixture constructs exact");
  const auto arm = history.snapshot();
  history.set(false);
  history.set(true);
  const auto freeze = history.snapshot();
  checkSnapshot(freeze, true, true, 2u, true);
  check(arm.currentProfile == freeze.currentProfile &&
            !renderTapeActionProfileStableExact(arm, freeze),
        "equal endpoint values do not hide an interval ABA change");
  check(!renderTapeActionProfileStableExact(freeze, freeze),
        "pre-arm ABA history cannot become construction-through-interval exact");
  checkSnapshot(arm, true, true, 0u, true);
  check(renderTapeActionProfileStableExact(arm, arm),
        "later changes do not mutate an earlier frozen value observation");
}

void testInitializationCannotFabricateOrRecoverHistory() {
  RenderTapeActionProfileHistory normal;
  check(normal.initialize(false), "normal initializes once");
  normal.set(true);
  check(!normal.initialize(true), "ordinary change cannot be relabelled construction");
  checkSnapshot(normal.snapshot(), false, true, 1u, false);
  normal.set(false);
  normal.set(true);
  checkSnapshot(normal.snapshot(), false, true, 3u, false);
  check(!normal.initialize(false), "invalid initialized history cannot recover");
  checkSnapshot(normal.snapshot(), false, true, 3u, false);

  RenderTapeActionProfileHistory missingConstruction;
  missingConstruction.set(true);
  checkSnapshot(missingConstruction.snapshot(), false, true, 1u, false);
  check(!missingConstruction.initialize(true),
        "a setter before initialization permanently loses construction authority");
  check(!renderTapeActionProfileStableExact(missingConstruction.snapshot(),
                                          missingConstruction.snapshot()),
        "missing construction remains unqualified despite current exact policy");
}

void testOverflowIsSaturatingAndPermanentlyInvalid() {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  RenderTapeActionProfileHistory history;
  check(history.initialize(true), "overflow fixture constructs exact");
  RenderTapeActionProfileHistoryTestAccess::revision(history, maximum - 1u);
  history.set(false);
  checkSnapshot(history.snapshot(), true, false, maximum, true);
  history.set(false);
  checkSnapshot(history.snapshot(), true, false, maximum, true);
  history.set(true);
  checkSnapshot(history.snapshot(), true, true, maximum, false);
  history.set(true);
  checkSnapshot(history.snapshot(), true, true, maximum, false);
  history.set(false);
  history.set(true);
  checkSnapshot(history.snapshot(), true, true, maximum, false);
  check(!history.initialize(true), "overflow cannot be cleared by reinitialization");
  check(!renderTapeActionProfileStableExact(history.snapshot(), history.snapshot()),
        "overflowed equal exact endpoint values remain unqualified");
}

void testMalformedOrUnknownSnapshotsFailClosed() {
  RenderTapeActionProfileHistory history;
  check(history.initialize(true), "malformed fixture constructs exact");
  const auto exact = history.snapshot();
  const std::array invalid{
      RenderTapeActionProfileSnapshot{.version = 0u, .constructionProfile = 1u,
                                     .currentProfile = 1u, .valid = true},
      RenderTapeActionProfileSnapshot{.version = 2u, .constructionProfile = 1u,
                                     .currentProfile = 1u, .valid = true},
      RenderTapeActionProfileSnapshot{.constructionProfile = 0u,
                                     .currentProfile = 1u, .valid = true},
      RenderTapeActionProfileSnapshot{.constructionProfile = 2u,
                                     .currentProfile = 1u, .valid = true},
      RenderTapeActionProfileSnapshot{.constructionProfile = 1u,
                                     .currentProfile = 0u, .valid = true},
      RenderTapeActionProfileSnapshot{.constructionProfile = 1u,
                                     .currentProfile = 2u, .valid = true},
      RenderTapeActionProfileSnapshot{.constructionProfile = 1u,
                                     .currentProfile = 1u, .revision = 1u,
                                     .valid = true},
      RenderTapeActionProfileSnapshot{.constructionProfile = 1u,
                                     .currentProfile = 1u, .valid = false},
  };
  for (const auto& bad : invalid) {
    check(!renderTapeActionProfileStableExact(bad, exact) &&
              !renderTapeActionProfileStableExact(exact, bad) &&
              !renderTapeActionProfileStableExact(bad, bad),
          "malformed/unknown arm or freeze cannot grant exact authority");
  }
}

void testExhaustiveSmallTransitionAndObservationTraces() {
  constexpr std::uint32_t depth = 8u;
  for (std::uint32_t initial = 0u; initial != 2u; ++initial) {
    for (std::uint32_t length = 0u; length <= depth; ++length) {
      for (std::uint32_t trace = 0u; trace != (1u << length); ++trace) {
        RenderTapeActionProfileHistory history;
        check(history.initialize(initial != 0u), "bounded trace initializes");
        std::array<RenderTapeActionProfileSnapshot, depth + 1u> observations{};
        std::array<bool, depth + 1u> everyPolicyExact{};
        observations[0u] = history.snapshot();
        everyPolicyExact[0u] = initial != 0u;
        bool current = initial != 0u;
        std::uint64_t changes = 0u;
        for (std::uint32_t step = 0u; step != length; ++step) {
          const bool requested = ((trace >> step) & 1u) != 0u;
          // Independent serial reference: count boundaries in the requested
          // policy trace, not revisions returned by the reducer under test.
          if (requested != current) ++changes;
          current = requested;
          history.set(requested);
          observations[step + 1u] = history.snapshot();
          everyPolicyExact[step + 1u] = everyPolicyExact[step] && requested;
          checkSnapshot(observations[step + 1u], initial != 0u, current,
                        changes, true);
        }
        for (std::uint32_t arm = 0u; arm <= length; ++arm) {
          for (std::uint32_t freeze = arm; freeze <= length; ++freeze) {
            check(renderTapeActionProfileStableExact(observations[arm],
                                                    observations[freeze]) ==
                      everyPolicyExact[freeze],
                  "all bounded arm/freeze traces match serial exact-through-history");
          }
        }
      }
    }
  }
}

} // namespace

int main() {
  try {
    testConstructionAndNormalDiagnosticEnable();
    testAbaBeforeAndDuringTheCapturedInterval();
    testInitializationCannotFabricateOrRecoverHistory();
    testOverflowIsSaturatingAndPermanentlyInvalid();
    testMalformedOrUnknownSnapshotsFailClosed();
    testExhaustiveSmallTransitionAndObservationTraces();
    std::cout << "Render Tape action profile spec passed (" << checks
              << " checks)\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "Render Tape action profile spec failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
