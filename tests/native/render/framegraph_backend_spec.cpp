// LIGHT spec for FrameGraphBackend (Task A5, R-BACK-40.5).
//
// Asserts the L0 contract without building a device/ChunkSlot fixture
// (behavioral equivalence with the traditional path is Task A8's job):
//   - FrameGraphBackend is constructible
//   - mode() == BackendMode::FrameGraph
//   - usable through an IRenderBackend& reference
//   - the pure profile/feature resolvers select the promoted passcoalesce-only
//     default and retain explicit strict/empty rollback paths.
// Deliberately does NOT call onChunkReady.

#include "../../../src/dxmt9/render/framegraph_backend.hpp"
#include "../framegraph/arena_payload_fixture.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dxmt9::render::BackendMode;
using dxmt9::render::FrameGraphBackend;
using dxmt9::render::IRenderBackend;
using dxmt9::render::RendererCompatProfile;
using dxmt9::render::resolveRendererCompatProfile;
using dxmt9::render::resolveRendererFeatures;
using dxmt9::render::replayPlanPreservesHeadStableFrontier;
using dxmt9::render::selectFrameGraphLookahead;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void testConstructibleAndMode() {
  FrameGraphBackend backend;
  check(backend.mode() == BackendMode::FrameGraph,
        "mode() returns BackendMode::FrameGraph");
}

void testUsableThroughInterface() {
  FrameGraphBackend backend;
  IRenderBackend& iface = backend;
  check(iface.mode() == BackendMode::FrameGraph,
        "IRenderBackend& mode() returns FrameGraph");
  // Lifecycle hooks inherit no-op defaults; calling them must be safe.
  iface.onDeviceCreated();
  iface.onFrameBegin(0);
  iface.onFrameEnd();
  iface.onDeviceDestroyed();
}

void testStrictResolverEmptyForAllTokens() {
  check(resolveRendererFeatures(nullptr, RendererCompatProfile::Strict).empty(),
        "strict null env yields empty feature set");
  check(resolveRendererFeatures("", RendererCompatProfile::Strict).empty(),
        "strict empty env yields empty feature set");
  check(resolveRendererFeatures("   , ;  ",
                                RendererCompatProfile::Strict)
            .empty(),
        "strict separator-only env yields empty feature set");
}

void testResolverEmptyForGarbageTokens() {
  // Garbage / unknown tokens are rejected under strict and the set stays empty.
  check(resolveRendererFeatures("garbage", RendererCompatProfile::Strict)
            .empty(),
        "garbage env yields empty feature set");
  check(resolveRendererFeatures("mesh,icb,not-a-feature",
                                RendererCompatProfile::Strict)
            .empty(),
        "multi-token env yields empty feature set");
}

void testProgressivePasscoalesceResolution() {
  check(resolveRendererCompatProfile(nullptr) ==
            RendererCompatProfile::Progressive,
        "unset compat profile resolves to progressive");
  check(resolveRendererCompatProfile("progressive") ==
            RendererCompatProfile::Progressive,
        "progressive compat profile is recognized");
  check(resolveRendererCompatProfile("strict") ==
            RendererCompatProfile::Strict,
        "strict compat profile is recognized");
  check(resolveRendererCompatProfile("unknown") ==
            RendererCompatProfile::Strict,
        "unknown compat profile resolves to strict");
  check(resolveRendererFeatures("passcoalesce",
                                RendererCompatProfile::Strict)
            .empty(),
        "strict rejects passcoalesce");
  const auto defaults =
      resolveRendererFeatures(nullptr, RendererCompatProfile::Progressive);
  check(defaults.passcoalesce,
        "unset progressive features enable promoted passcoalesce");
  check(!defaults.dce,
        "unset progressive features keep bounded DCE opt-in");
  check(resolveRendererFeatures("", RendererCompatProfile::Progressive).empty(),
        "empty progressive features explicitly disable passcoalesce");
  check(resolveRendererFeatures("0", RendererCompatProfile::Progressive).empty(),
        "zero progressive features explicitly disable passcoalesce");
  const auto progressive = resolveRendererFeatures(
      "passcoalesce,dce,unknown", RendererCompatProfile::Progressive);
  check(progressive.passcoalesce,
        "progressive accepts the implemented passcoalesce feature");
  check(progressive.dce,
        "progressive accepts the opt-in bounded DCE feature");
  check(resolveRendererFeatures("dce", RendererCompatProfile::Strict).empty(),
        "strict rejects the bounded DCE token");
}

dxmt9::framegraph::ReplayCommandPlan replayPlan(
    std::initializer_list<std::uint32_t> commands) {
  return dxmt9::framegraph::ReplayCommandPlan{
      .command_indices = std::vector<std::uint32_t>(commands),
      .valid = true,
  };
}

void testHeadStableFrontierSelection() {
  const auto natural = replayPlan({0, 1, 2});
  check(replayPlanPreservesHeadStableFrontier(replayPlan({0, 2, 1}),
                                               natural),
        "same live subset and natural head permits coalesced replay");
  check(!replayPlanPreservesHeadStableFrontier(replayPlan({1, 0, 2}),
                                                natural),
        "moved live head requires no-coalesce fallback");
  check(!replayPlanPreservesHeadStableFrontier(replayPlan({0, 2}), natural),
        "changed DCE live subset requires no-coalesce fallback");

  const auto empty = replayPlan({});
  check(replayPlanPreservesHeadStableFrontier(empty, empty),
        "empty proven-live subset has a stable frontier");
  auto invalid = empty;
  invalid.valid = false;
  check(!replayPlanPreservesHeadStableFrontier(invalid, empty),
        "invalid optimized plan requires fallback");
}

dxmt9::core::metalqueue::ResolvedPublishedSource resolvedSource(
    std::size_t slotIndex, std::uint64_t seqId,
    dxmt9::core::SourcePayloadView payload,
    dxmt9::core::CpuReadyTape::SourceRef source) {
  const std::size_t usedBytes =
      dxmt9::core::measureSourcePayloadLogicalExtent(payload);
  const dxmt9::core::CpuReadySourceMetadata metadata{
      .rawOrdinal = seqId,
      .sourceOrdinal = seqId,
      .seqId = seqId,
      .buildGeneration = source.id.generation,
      .usedBytes = usedBytes,
      .pageCount = source.storage.pageCount,
  };
  return dxmt9::core::metalqueue::ResolvedPublishedSource{
      .source = source,
      .slotIndex = slotIndex,
      .seqId = seqId,
      .metadata = metadata,
      .semantic = dxmt9::core::summarizeSourcePayload(
          payload,
          dxmt9::core::SourceSemanticSummaryContext{
              .byteCount = usedBytes,
              .pageCount = source.storage.pageCount,
          }),
      .payload = payload,
      .sourceId = source.id,
      .storage = source.storage,
      .slot = nullptr,
      .commandBegin = 0,
      .commandCount = payload.commandCount(),
  };
}

void testGenerationCheckedArenaLookahead() {
  dxmt9::core::ChunkSlot arenaSource;
  arenaSource.appendClear({});
  dxmt9::tests::framegraph::ArenaPayloadFixture arena(arenaSource);
  check(arena.valid(), "Arena lookahead fixture publishes");

  dxmt9::core::ChunkSlot nextSource;
  nextSource.appendClear({});
  const dxmt9::core::CpuReadyTape::SourceRef currentRef{
      .id = {.index = 2, .generation = 17},
      .storage = {.firstPage = 4, .pageCount = 1, .generation = 31},
  };
  const dxmt9::core::CpuReadyTape::SourceRef nextRef{
      .id = {.index = 3, .generation = 18},
      .storage = {.firstPage = 5, .pageCount = 1, .generation = 32},
  };
  std::array selected{
      resolvedSource(7, 40, arena.view(), currentRef),
      resolvedSource(8, 41, dxmt9::core::SourcePayloadView(nextSource),
                     nextRef),
  };

  check(selectFrameGraphLookahead(selected, 7, arena.view(), 40,
                                  currentRef) == &selected[1],
        "Arena lookahead resolves without ChunkSlot pointer identity");

  auto stale = selected;
  ++stale[1].storage.generation;
  check(selectFrameGraphLookahead(stale, 7, arena.view(), 40,
                                  currentRef) == nullptr,
        "stale next-source storage generation is rejected");

  auto partial = selected;
  partial[1].commandCount = 0;
  check(selectFrameGraphLookahead(partial, 7, arena.view(), 40,
                                  currentRef) == nullptr,
        "partial next-source range is not a whole-source DCE proof");

  auto nonConsecutive = selected;
  nonConsecutive[1].seqId = 42;
  check(selectFrameGraphLookahead(nonConsecutive, 7, arena.view(), 40,
                                  currentRef) == nullptr,
        "non-consecutive successor is rejected");
}

}  // namespace

int main() {
  try {
    testConstructibleAndMode();
    testUsableThroughInterface();
    testStrictResolverEmptyForAllTokens();
    testResolverEmptyForGarbageTokens();
    testProgressivePasscoalesceResolution();
    testHeadStableFrontierSelection();
    testGenerationCheckedArenaLookahead();
  } catch (const std::exception& e) {
    std::cerr << "framegraph_backend_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "framegraph_backend_spec passed\n";
  return 0;
}
