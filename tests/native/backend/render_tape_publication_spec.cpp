#include "device_c_render_tape_publication.hpp"
#include "render_tape_completion_fixture.hpp"
#include "render_tape_cpu_grammar.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dxmt9::d3d9;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

ImportedRenderTapeView importFixture(const test::CompletionTapeFixture& fixture) {
  ImportedRenderTapeView tape;
  check(importPrevalidatedRenderTape(fixture.tape, tape), "fixture imports");
  return tape;
}

void plannedRangesPreserveCommandAndResourceBoundaries() {
  const auto fixture = test::makeCompletionTapeFixture(
      test::CompletionTapeFixtureMode::CapacityContinuation);
  const auto tape = importFixture(fixture);
  check(preflightRenderTapeCpuGrammar(tape, false, false, true).accepted(),
        "plain multi-record draws are admitted for the grouped CPU experiment");
  check(!preflightRenderTapeCpuGrammar(tape, false, false, false).accepted(),
        "the original per-command grammar is not silently expanded");
  const auto grouped = planRenderTapeCpuPublication(
      tape, RenderTapeCpuPublicationMode::DrawIsland);
  const auto serial = planRenderTapeCpuPublication(
      tape, RenderTapeCpuPublicationMode::PerCommand);
  check(grouped.valid && serial.valid && grouped.count == 3u &&
            serial.count == 4u, "only adjacent draw events merge");
  const auto* island = grouped.rangeForEvent(5u);
  check(island && island == grouped.rangeForEvent(6u) &&
            island->firstEvent == 5u && island->lastEvent == 6u &&
            island->commandEvents == 2u && island->drawRecords == 9u,
        "immutable seven-plus-two range has exact event and draw coverage");
  check(serial.rangeForEvent(5u) != serial.rangeForEvent(6u) &&
            serial.rangeForEvent(5u)->drawRecords == 7u &&
            serial.rangeForEvent(6u)->drawRecords == 2u,
        "per-command reference retains its original publication units");
  check(grouped.rangeForEvent(7u)->commandEvents == 1u &&
            grouped.rangeForEvent(7u)->drawRecords == 0u &&
            grouped.rangeForEvent(8u) == nullptr &&
            grouped.rangeForEvent(9u)->firstEvent == 9u,
        "state-only, destroy and Present are not swallowed by a draw range");

  const auto dynamic = test::makeCompletionTapeFixture(
      test::CompletionTapeFixtureMode::DynamicTwoMutation);
  check(preflightRenderTapeCpuGrammar(
            importFixture(dynamic), true, false, true).reason ==
            RenderTapeCpuGrammarReason::GroupedBufferUnsupported,
        "grouped publication does not admit dynamic rename implicitly");
  const auto managed = test::makeManagedAllocationFailureTapeFixture();
  check(preflightRenderTapeCpuGrammar(
            importFixture(managed), true, false, true).reason ==
            RenderTapeCpuGrammarReason::GroupedBufferUnsupported,
        "grouped publication does not admit Managed mutation implicitly");
  const auto separated = planRenderTapeCpuPublication(
      importFixture(dynamic), RenderTapeCpuPublicationMode::DrawIsland);
  check(separated.valid && separated.rangeForEvent(5u)->lastEvent == 5u &&
            separated.rangeForEvent(7u)->firstEvent == 7u &&
            separated.rangeForEvent(6u) == nullptr,
        "a resource mutation ends a candidate range; it is not a draw edge");
  check(!planRenderTapeCpuPublication(
             tape, static_cast<RenderTapeCpuPublicationMode>(255u)).valid,
        "unknown publication modes fail before effects");
}

void sharedRangeWitnessRejectsEveryWrongDimension() {
  const RenderTapeCpuPublicationRange range{1u, 2u, 2u, 2u};
  std::size_t accepted = 0u;
  std::size_t visited = 0u;
  for (std::uint32_t first = 0u; first < 4u; ++first)
    for (std::uint32_t last = 0u; last < 4u; ++last)
      for (std::uint32_t events = 0u; events < 4u; ++events)
        for (std::uint32_t publish = 0u; publish < 4u; ++publish)
          for (std::uint32_t sources = 0u; sources < 3u; ++sources)
            for (std::uint32_t draws = 0u; draws < 4u; ++draws) {
              const auto actual = std::array{first, last, events, publish,
                                             sources, draws};
              const bool expected = actual ==
                  std::array<std::uint32_t, 6u>{1u, 2u, 2u, 2u, 1u, 2u};
              const bool matched = renderTapeGroupedPublicationMatches(
                  range, first, last, events, publish, sources, draws);
              check(matched == expected, "all six range dimensions are binding");
              accepted += matched;
              ++visited;
            }
  check(visited == 3072u && accepted == 1u,
        "exhaustive range witness has one exact accepted tuple");

  // Independent chronological execution of A, B, Publish. The witness is
  // called with the actually accumulated prefix, not the immutable expected
  // values substituted into its observations.
  std::array order{0u, 1u, 2u};
  std::size_t legal = 0u;
  do {
    std::uint32_t first = 0u, last = 0u, count = 0u;
    bool publishAccepted = false;
    for (const auto action : order) {
      if (action == 2u) {
        publishAccepted = renderTapeGroupedPublicationMatches(
            range, first, last, count, last, 1u, count);
      } else {
        if (count == 0u) first = action + 1u;
        last = action + 1u;
        ++count;
      }
    }
    check(publishAccepted == (order == std::array{0u, 1u, 2u}),
          "early publication and reversed event execution reject");
    legal += publishAccepted;
  } while (std::next_permutation(order.begin(), order.end()));
  check(legal == 1u, "only A then B then publication is legal");
}
}  // namespace

int main() {
  try {
    plannedRangesPreserveCommandAndResourceBoundaries();
    sharedRangeWitnessRejectsEveryWrongDimension();
  } catch (const std::exception& error) {
    std::cerr << "render_tape_publication_spec: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render_tape_publication_spec passed\n";
  return 0;
}
