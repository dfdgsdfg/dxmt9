#include "../../../src/dxmt9/dxmt9_parallel_render_pass.hpp"
#include "../../../src/dxmt9/dxmt9_parallel_render_pass_metal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using dxmt9::encoders::EncodePartitionRangeSnapshot;
using dxmt9::encoders::ParallelFirstDrawSnapshot;
using dxmt9::encoders::ParallelPassChildPlan;
using dxmt9::encoders::ParallelPassExecutionStatus;
using dxmt9::encoders::ParallelPassFallbackReason;
using dxmt9::encoders::ParallelPassFailurePhase;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

template <typename T>
void check(const T& condition, std::string_view message) {
  if (!static_cast<bool>(condition)) {
    throw TestFailure(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    throw TestFailure(std::string(message));
  }
}

dxmt9::core::CpuReadyTape::SourceRef sourceRef(std::uint64_t seqId) {
  return {
      .id = {.index = 3u, .generation = seqId},
      .storage = {.firstPage = 7u, .pageCount = 1u, .generation = seqId},
  };
}

dxmt9::encoders::ParallelPassBindingSnapshot bindingSnapshot(
    dxmt9::encoders::ParallelPassDirectBindingMode mode,
    std::uint16_t slot) {
  return {
      .firstRenderPso = {.slot = slot, .generation = 1u},
      .firstPayload = {
          .vertexConstants = static_cast<std::uint64_t>(slot) + 1u,
          .pixelConstants = static_cast<std::uint64_t>(slot) + 2u,
          .fixedFunction = static_cast<std::uint64_t>(slot) + 3u,
      },
      .firstPayloadCounts = {
          .vertexFloat = 4u,
          .pixelFloat = 4u,
      },
      .mode = mode,
      .reject = dxmt9::encoders::ParallelPassBindingRejectReason::None,
      .complete = true,
  };
}

struct ProductionPlanFixture {
  dxmt9::core::ChunkSlot slot{};
  dxmt9::encoders::EncodePartitionReplayStream stream{};
  dxmt9::encoders::ProductionEncodePartitionPlanStorage production{};
  std::array<ParallelFirstDrawSnapshot, 3> snapshots{};

  ProductionPlanFixture() {
    slot.seqId = 401u;
    std::vector<dxmt9::core::DrawParam> draws(96u);
    std::vector<dxmt9::core::DrawParamPayloadView> payloads(draws.size());
    slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                       dxmt9::core::DrawUniformPayload{}, draws, payloads);
    stream = dxmt9::encoders::makeEncodePartitionReplayStream(
        3u, slot, 0u, slot.commandCount(), false, {}, {},
        sourceRef(slot.seqId));
    const auto result = dxmt9::encoders::planProductionEncodePartitions(
        stream, production);
    check(result.explicitPlan && production.count == 3u,
          "production planner creates three bounded child ranges");
    check(dxmt9::encoders::validateEncodePartitionRanges(production.view(),
                                                          stream),
          "parallel fixture consumes only the validated production plan");
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
      snapshots[i] = ParallelFirstDrawSnapshot{
          .provenance = production.ranges[i].entry,
          .entryRender = dxmt9::core::RenderContinuationKey{
              .entryReads = dxmt9::core::ExactResourceSet{
                  .flags = dxmt9::core::ExactResourceSetComplete |
                           dxmt9::core::ExactResourceSetCanonicalized,
              },
              .route = dxmt9::core::RenderRoute::Portable,
              .passActionEpoch = 1u,
              .flags = dxmt9::core::RenderContinuationKeyValid |
                       dxmt9::core::RenderContinuationEntryStateComplete,
          },
          .generation = static_cast<std::uint64_t>(i + 1u),
          .complete = true,
      };
    }
  }

  dxmt9::encoders::ParallelPassEligibilityInput input() const {
    return {
        .ranges = production.view(),
        .firstDrawSnapshots = snapshots,
        .passActionEpoch = 1u,
        .explicitPlan = true,
        .planValidated = true,
        .logicalPassSealed = true,
    };
  }
};

void eligibilityAndSelectionAreTypedAndBounded() {
  using BindingMode = dxmt9::encoders::ParallelPassDirectBindingMode;
  using BindingReject = dxmt9::encoders::ParallelPassBindingRejectReason;
  using dxmt9::encoders::classifyParallelPassBindingKey;
  check(classifyParallelPassBindingKey({.psoPresent = true}).accepted(),
        "Stage 1 direct PSO is child-local safe");
  const auto stage2b = classifyParallelPassBindingKey({
      .psoPresent = true,
      .argbufHybrid = true,
      .argbufDirectCbuf = true,
  });
  check(stage2b.accepted() &&
            stage2b.mode == BindingMode::Stage2DirectCbuf,
        "Stage 2b direct-cbuf PSO is child-local safe");
  check(classifyParallelPassBindingKey({}).reject ==
            BindingReject::MissingPso,
        "missing PSO metadata fails closed");
  check(classifyParallelPassBindingKey({
            .psoPresent = true,
            .argbufHybrid = true,
        }).reject == BindingReject::Stage2ArgumentTable,
        "slot-30 Stage 2 table fails closed");
  check(classifyParallelPassBindingKey({
            .psoPresent = true,
            .argbufHybrid = true,
            .argbufResourceArray = true,
        }).reject == BindingReject::ResourceArray,
        "resource-array PSO fails closed before table ownership");
  check(classifyParallelPassBindingKey({
            .psoPresent = true,
            .argbufDirectCbuf = true,
        }).reject == BindingReject::MixedAbi,
        "inconsistent direct-cbuf key bits fail as mixed ABI");
  check(classifyParallelPassBindingKey({
            .psoPresent = true,
            .overrideRebuild = true,
        }).reject == BindingReject::OverrideRebuild,
        "PSO-rebuilding draw override fails closed");

  ProductionPlanFixture fixture;
  dxmt9::encoders::ParallelPassPlanStorage storage{};
  const auto eligible = dxmt9::encoders::planParallelRenderPassChildren(
      fixture.input(), storage);
  check(eligible.considered && eligible.eligible && eligible.childCount == 3u &&
            eligible.fallback == ParallelPassFallbackReason::None,
        "sealed validated draw-only plan is eligible");
  checkEq(storage.count, std::size_t{3},
          "eligible planning stays within fixed child storage");

  const auto unavailable = dxmt9::encoders::decideParallelPassExecution(
      true, eligible, false);
  check(unavailable.considered && unavailable.eligible &&
            !unavailable.selected &&
            unavailable.fallback ==
                ParallelPassFallbackReason::ParallelEncoderUnavailable,
        "unavailable parallel encoder selects typed serial fallback");
  const auto selected = dxmt9::encoders::decideParallelPassExecution(
      true, eligible, true);
  check(selected.selected && selected.fallback ==
                                 ParallelPassFallbackReason::None,
        "a capable fake can select the pure executor surface");
  const auto notRequested = dxmt9::encoders::decideParallelPassExecution(
      false, eligible, true);
  check(!notRequested.considered && !notRequested.selected &&
            notRequested.fallback == ParallelPassFallbackReason::NotRequested,
        "serial provider does not create parallel-pass work");

  auto input = fixture.input();
  input.logicalPassSealed = false;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::PassNotSealed,
        "unsealed source-fragment ownership fails before child work");
  input = fixture.input();
  input.hasQuery = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::Query,
        "query work is coordinator-serial");
  input = fixture.input();
  input.hasClear = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::Clear,
        "clear work is coordinator-serial");
  input = fixture.input();
  input.hasSidecarObservation = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::SidecarObservation,
        "sidecar observation blocks children");
  input = fixture.input();
  input.hasInitializerWait = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::InitializerWait,
        "initializer wait blocks children");
  input = fixture.input();
  input.hasPresent = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::Present,
        "present work blocks children");
  input = fixture.input();
  input.hasUnresolvedHazard = true;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::UnresolvedHazard,
        "unresolved hazards block children");
  input = fixture.input();
  auto missingSnapshots = fixture.snapshots;
  missingSnapshots[1].complete = false;
  input.firstDrawSnapshots = missingSnapshots;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::FirstDrawSnapshotMissing,
        "every child requires a complete first-draw snapshot");
  auto unknownRouteSnapshots = fixture.snapshots;
  unknownRouteSnapshots[1].entryRender.route =
      dxmt9::core::RenderRoute::Unknown;
  input = fixture.input();
  input.firstDrawSnapshots = unknownRouteSnapshots;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::FirstDrawSnapshotMissing,
        "unknown render route is never accepted as eligible");

  auto commandRanges = fixture.production.ranges;
  commandRanges[1].kind =
      dxmt9::encoders::EncodePartitionRangeKind::CommandSegment;
  input = fixture.input();
  input.ranges = std::span<const EncodePartitionRangeSnapshot>(
      commandRanges.data(), fixture.production.count);
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::CoordinatorCommand,
        "a coordinator command cannot become a child range");

  std::array<EncodePartitionRangeSnapshot,
             dxmt9::encoders::kParallelRenderPassChildCapacity + 1u>
      oversizedRanges{};
  std::array<ParallelFirstDrawSnapshot,
             dxmt9::encoders::kParallelRenderPassChildCapacity + 1u>
      oversizedSnapshots{};
  input = fixture.input();
  input.ranges = oversizedRanges;
  input.firstDrawSnapshots = oversizedSnapshots;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::ChildCapacity,
        "child planning fails closed above fixed storage capacity");
}

void coordinatorProofSnapshotRequiresEveryPreEffectFact() {
  using namespace dxmt9::encoders;
  const auto complete = makeParallelPassCoordinatorProofSnapshot({
      .firstPassActionEpoch = 17u,
      .queryAbsent = true,
      .updateTextureAbsent = true,
      .captureInactive = true,
      .initializerIndependent = true,
      .orderedControlAbsent = true,
      .sidecarObservationAbsent = true,
  });
  check(complete.firstPassActionEpoch == 17u &&
            complete.flags == kParallelPassCoordinatorProofComplete,
        "coordinator snapshot owns the complete pre-effect proof");

  constexpr std::array facts{
      ParallelPassQueryAbsent,
      ParallelPassUpdateTextureAbsent,
      ParallelPassCaptureInactive,
      ParallelPassInitializerIndependent,
      ParallelPassOrderedControlAbsent,
      ParallelPassSidecarObservationAbsent,
  };
  for (std::size_t missing = 0; missing < facts.size(); ++missing) {
    ParallelPassCoordinatorProofSnapshotInput input{
        .firstPassActionEpoch = 17u,
        .queryAbsent = true,
        .updateTextureAbsent = true,
        .captureInactive = true,
        .initializerIndependent = true,
        .orderedControlAbsent = true,
        .sidecarObservationAbsent = true,
    };
    switch (missing) {
    case 0: input.queryAbsent = false; break;
    case 1: input.updateTextureAbsent = false; break;
    case 2: input.captureInactive = false; break;
    case 3: input.initializerIndependent = false; break;
    case 4: input.orderedControlAbsent = false; break;
    case 5: input.sidecarObservationAbsent = false; break;
    default: break;
    }
    const auto incomplete = makeParallelPassCoordinatorProofSnapshot(input);
    check(!incomplete.proves(facts[missing]),
          "an unresolved coordinator fact remains explicitly unproven");
    check((incomplete.flags |
           static_cast<std::uint32_t>(facts[missing])) ==
              kParallelPassCoordinatorProofComplete,
          "dropping one fact does not erase unrelated coordinator proofs");
  }
}

struct MetalNoopChildEmitter {
  static bool emit(void*, const ParallelPassChildPlan&,
                   WMT::RenderCommandEncoder) noexcept {
    return true;
  }
};

void wmtParentChildAdapterCreatesAndJoinsMetalEncoders() {
  auto devices = WMT::CopyAllDevices();
  if (!devices || devices.count() == 0u) {
    return;
  }
  WMT::Device device = devices.object(0u);
  auto queue = device.newCommandQueue(4u);
  check(static_cast<bool>(queue), "Metal device creates a command queue");
  WMT::Reference<WMT::CommandBuffer> commandBuffer(queue.commandBuffer());
  check(static_cast<bool>(commandBuffer), "Metal queue creates a command buffer");

  WMTTextureInfo textureInfo{
      .pixel_format = WMTPixelFormatBGRA8Unorm,
      .width = 16u,
      .height = 16u,
      .depth = 1u,
      .array_length = 1u,
      .type = WMTTextureType2D,
      .mipmap_level_count = 1u,
      .sample_count = 1u,
      .usage = WMTTextureUsageRenderTarget,
      .options = WMTResourceStorageModePrivate,
      .reserved = 0u,
      .mach_port = 0u,
      .gpu_resource_id = 0u,
  };
  auto texture = device.newTexture(textureInfo);
  check(static_cast<bool>(texture), "Metal device creates a render target");

  WMTRenderPassInfo passInfo{};
  passInfo.colors[0].texture = texture.handle;
  passInfo.colors[0].load_action = WMTLoadActionDontCare;
  passInfo.colors[0].store_action = WMTStoreActionDontCare;
  passInfo.default_raster_sample_count = 1u;
  passInfo.render_target_width = textureInfo.width;
  passInfo.render_target_height = textureInfo.height;
  ProductionPlanFixture fixture;
  dxmt9::encoders::ParallelPassPlanStorage plan{};
  const auto eligibility = dxmt9::encoders::planParallelRenderPassChildren(
      fixture.input(), plan);
  check(eligibility.eligible,
        "hardware adapter fixture has a validated child plan");
  for (std::size_t i = 0; i < plan.count; ++i) {
    plan.children[i].binding = bindingSnapshot(
        dxmt9::encoders::ParallelPassDirectBindingMode::Stage1Direct,
        static_cast<std::uint16_t>(i));
  }
  dxmt9::encoders::ParallelPassMetalBackend backend(
      commandBuffer, passInfo,
      dxmt9::encoders::ParallelPassMetalCallbacks{
          .emitChild = MetalNoopChildEmitter::emit,
      });
  const std::array<std::uint32_t, 3> completionOrder{0u, 1u, 2u};
  const auto result = dxmt9::encoders::executeParallelRenderPass(
      plan.view(), completionOrder, backend);
  check(result.status == ParallelPassExecutionStatus::Completed &&
            result.crossedEffectBoundary,
        "generic executor drives the real WMT parent/child adapter");
  commandBuffer.commit();
  commandBuffer.waitUntilCompleted();
  check(commandBuffer.status() == WMTCommandBufferStatusCompleted,
        "empty parent/child render pass completes on Metal");
}

bool identityResource(const void*, std::uint64_t raw,
                      std::uint64_t& canonical) noexcept {
  canonical = raw;
  return raw != 0u;
}

dxmt9::core::RenderRoute portableRoute(
    const void*, dxmt9::core::FlatDrawStateView) noexcept {
  return dxmt9::core::RenderRoute::Portable;
}

dxmt9::core::RenderRoute unknownRoute(
    const void*, dxmt9::core::FlatDrawStateView) noexcept {
  return dxmt9::core::RenderRoute::Unknown;
}

dxmt9::core::RenderRoute textureSelectedRoute(
    const void*, dxmt9::core::FlatDrawStateView state) noexcept {
  return state.hot && state.hot->textures[0].value == 0xbeefu
      ? dxmt9::core::RenderRoute::Tile
      : dxmt9::core::RenderRoute::Portable;
}

struct ResourceProofFixture {
  std::uint64_t aliasFrom = 0;
  std::uint64_t aliasTo = 0;
  std::uint64_t fail = 0;
};

bool fixtureResourceProof(const void* context, std::uint64_t raw,
                          std::uint64_t& canonical) noexcept {
  const auto& fixture = *static_cast<const ResourceProofFixture*>(context);
  if (raw == fixture.fail) {
    return false;
  }
  canonical = raw == fixture.aliasFrom ? fixture.aliasTo : raw;
  return canonical != 0u;
}

dxmt9::encoders::ParallelPassStaticProofInput completeProofs(
    std::uint64_t epoch = 7u) {
  return {
      .resources = {.resolve = identityResource},
      .route = {.resolve = portableRoute},
      .coordinator = {
          .firstPassActionEpoch = epoch,
          .flags = dxmt9::encoders::kParallelPassCoordinatorProofComplete,
      },
  };
}

struct PassObservationFixture {
  dxmt9::core::ChunkSlot slot{};
  dxmt9::encoders::EncodePartitionReplayStream stream{};
  dxmt9::encoders::ProductionEncodePartitionPlanStorage plan{};
  std::vector<std::uint32_t> commandOrder{};
  std::vector<std::size_t> replayOrdinalByCommandIndex{};

  void finalize(std::uint64_t seqId, std::size_t slotIndex = 4u,
                std::vector<std::uint32_t> order = {}) {
    slot.seqId = seqId;
    commandOrder = std::move(order);
    replayOrdinalByCommandIndex.resize(slot.commandCount());
    stream = dxmt9::encoders::makeEncodePartitionReplayStream(
        slotIndex, slot, 0u, slot.commandCount(), !commandOrder.empty(),
        commandOrder, replayOrdinalByCommandIndex,
        sourceRef(seqId));
    const auto planned = dxmt9::encoders::planProductionEncodePartitions(
        stream, plan);
    check(planned.explicitPlan &&
              dxmt9::encoders::validateEncodePartitionRanges(
                  plan.view(), stream),
          "pass observation fixture requires one validated explicit plan");
  }

  dxmt9::encoders::SealedParallelPassSnapshotBatchResult observe(
      dxmt9::encoders::SealedParallelPassSnapshotBatch& output,
      bool sourceStartsPass = true,
      bool sourceEndsPass = true,
      dxmt9::encoders::ParallelPassStaticProofInput proofs =
          completeProofs()) const {
    return dxmt9::encoders::produceSealedParallelPassSnapshots({
          .stream = &stream,
          .ranges = plan.view(),
          .proofs = proofs,
          .planValidated = true,
          .sourceStartsPass = sourceStartsPass,
          .sourceEndsPass = sourceEndsPass,
        }, output);
  }
};

std::vector<dxmt9::core::DrawParam> draws(std::size_t count) {
  return std::vector<dxmt9::core::DrawParam>(count);
}

std::vector<dxmt9::core::DrawParamPayloadView> payloads(
    std::size_t count) {
  return std::vector<dxmt9::core::DrawParamPayloadView>(count);
}

void explicitParallelSubdivisionIsEvenAndBounded() {
  using dxmt9::encoders::subdivideParallelPassDraws;

  const auto verify = [](std::uint64_t total,
                         std::span<const std::uint32_t> expected) {
    const auto result = subdivideParallelPassDraws(total);
    check(result.valid && result.childCount == expected.size(),
          "eligible subdivision has the expected bounded child count");
    std::uint64_t cursor = 0u;
    for (std::size_t child = 0u; child < expected.size(); ++child) {
      check(result.drawBegins[child] == cursor &&
                result.drawCounts[child] == expected[child] &&
                result.drawCounts[child] >=
                    dxmt9::encoders::kProductionPartitionDrawThreshold,
            "subdivision is ordered, exact, and never thinner than 64");
      cursor += result.drawCounts[child];
    }
    check(cursor == total, "subdivision covers every draw exactly once");
  };

  check(!subdivideParallelPassDraws(127u).valid,
        "127 draws stays on the serial pre-effect path");
  const std::array<std::uint32_t, 2> two64{64u, 64u};
  verify(128u, two64);
  const std::array<std::uint32_t, 2> split129{64u, 65u};
  verify(129u, split129);
  const std::array<std::uint32_t, 5> split380{76u, 76u, 76u, 76u, 76u};
  verify(380u, split380);
  const std::array<std::uint32_t, 5> split381{76u, 76u, 76u, 76u, 77u};
  verify(381u, split381);
  std::array<std::uint32_t,
             dxmt9::encoders::kParallelRenderPassChildCapacity> split1024{};
  split1024.fill(64u);
  verify(1024u, split1024);
  auto split1025 = split1024;
  split1025.back() = 65u;
  verify(1025u, split1025);
  check(!subdivideParallelPassDraws(UINT64_MAX).valid &&
            !subdivideParallelPassDraws(
                static_cast<std::uint64_t>(UINT32_MAX) + 1u).valid,
        "overflowing draw domains fail closed without partial coverage");

  const auto below = draws(127u);
  const auto belowPayloads = payloads(below.size());
  PassObservationFixture fixture;
  fixture.slot.appendDrawRun({}, {}, below, belowPayloads);
  fixture.slot.appendPresent({}, {});
  fixture.finalize(417u);
  dxmt9::encoders::SealedParallelPassSnapshotBatch output{};
  const auto result = fixture.observe(output);
  check(result.eligibleCount == 0u && output.count == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::
                    NoTwoChildWork)] == 1u,
        "sealed 127-draw pass takes the existing serial fallback");
}

void wholeCommandSubdivisionIsOrderedBoundedAndFailClosed() {
  using Failure =
      dxmt9::encoders::ParallelPassWholeCommandPlanFailure;
  using dxmt9::encoders::subdivideParallelPassWholeCommands;

  const auto verify = [&](std::span<const std::uint64_t> commands,
                          std::span<const std::uint64_t> expectedDraws,
                          std::span<const std::uint32_t> expectedCommands) {
    const std::uint64_t total = std::accumulate(
        commands.begin(), commands.end(), std::uint64_t{0});
    const auto plan = subdivideParallelPassWholeCommands(
        7u, static_cast<std::uint32_t>(commands.size()), total,
        [&](std::uint32_t index) { return commands[index]; });
    check(plan.valid() && plan.childCount == expectedDraws.size() &&
              plan.childCount == expectedCommands.size(),
          "whole-command subdivision emits the expected child count");
    std::uint32_t ordinal = 7u;
    std::uint64_t covered = 0u;
    for (std::size_t child = 0u; child < plan.childCount; ++child) {
      check(plan.replayOrdinalBegins[child] == ordinal &&
                plan.replayOrdinalCounts[child] ==
                    expectedCommands[child] &&
                plan.drawCounts[child] == expectedDraws[child] &&
                plan.drawCounts[child] >=
                    dxmt9::encoders::kProductionPartitionDrawThreshold,
            "whole-command children preserve exact order and minimum work");
      ordinal += plan.replayOrdinalCounts[child];
      covered += plan.drawCounts[child];
    }
    check(ordinal == 7u + commands.size() && covered == total,
          "whole-command subdivision covers every command and draw once");
  };

  const std::array<std::uint64_t, 3> suffix64{64u, 32u, 32u};
  const std::array<std::uint64_t, 2> two64{64u, 64u};
  const std::array<std::uint32_t, 2> oneThenTwo{1u, 2u};
  verify(suffix64, two64, oneThenTwo);
  const std::array<std::uint64_t, 3> jagged{70u, 100u, 30u};
  const std::array<std::uint64_t, 2> jaggedGroups{70u, 130u};
  verify(jagged, jaggedGroups, oneThenTwo);
  const std::array<std::uint64_t, 6> split380{
      64u, 31u, 33u, 64u, 64u, 124u};
  const std::array<std::uint64_t, 5> groups380{
      64u, 64u, 64u, 64u, 124u};
  const std::array<std::uint32_t, 5> commands380{1u, 2u, 1u, 1u, 1u};
  verify(split380, groups380, commands380);
  const std::array<std::uint64_t, 6> split381{
      64u, 31u, 33u, 64u, 64u, 125u};
  const std::array<std::uint64_t, 5> groups381{
      64u, 64u, 64u, 64u, 125u};
  verify(split381, groups381, commands380);

  std::array<std::uint64_t,
             dxmt9::encoders::kParallelRenderPassChildCapacity + 1u>
      capped{};
  capped.fill(64u);
  std::array<std::uint64_t,
             dxmt9::encoders::kParallelRenderPassChildCapacity>
      cappedGroups{};
  cappedGroups.fill(64u);
  cappedGroups.back() = 128u;
  std::array<std::uint32_t,
             dxmt9::encoders::kParallelRenderPassChildCapacity>
      cappedCommands{};
  cappedCommands.fill(1u);
  cappedCommands.back() = 2u;
  verify(capped, cappedGroups, cappedCommands);

  const std::array<std::uint64_t, 2> noSplit{100u, 27u};
  check(subdivideParallelPassWholeCommands(
            0u, 2u, 127u,
            [&](std::uint32_t index) { return noSplit[index]; }).failure ==
            Failure::NoTwoChildWork,
        "genuine no-two-child work is typed separately");
  const std::array<std::uint64_t, 2> malformed{64u, 0u};
  const std::array<std::uint64_t, 2> overflowing{UINT64_MAX, 1u};
  check(subdivideParallelPassWholeCommands(
            0u, 2u, 128u,
            [&](std::uint32_t index) { return malformed[index]; }).failure ==
            Failure::InvalidOrOverflow &&
            subdivideParallelPassWholeCommands(
                UINT32_MAX, 2u, 128u,
                [](std::uint32_t) { return std::uint64_t{64u}; }).failure ==
                Failure::InvalidOrOverflow &&
            subdivideParallelPassWholeCommands(
                0u, 2u, UINT64_MAX,
                [&](std::uint32_t index) { return overflowing[index]; })
                    .failure == Failure::InvalidOrOverflow,
        "malformed and overflowing command domains fail closed");

  const auto verifyRejectedSerialReplay = [&](
      std::span<const std::uint64_t> commands,
      std::uint64_t declaredTotal,
      Failure expected) {
    const auto plan = subdivideParallelPassWholeCommands(
        0u, static_cast<std::uint32_t>(commands.size()), declaredTotal,
        [&](std::uint32_t index) { return commands[index]; });
    std::uint32_t parallelEffects = 0u;
    const std::size_t actualDraws = static_cast<std::size_t>(
        std::accumulate(commands.begin(), commands.end(), std::uint64_t{0}));
    std::vector<std::uint32_t> serialReplay(actualDraws);
    if (plan.valid()) {
      ++parallelEffects;
    } else {
      std::size_t draw = 0u;
      for (const std::uint64_t commandDraws : commands) {
        for (std::uint64_t i = 0u; i < commandDraws; ++i) {
          ++serialReplay[draw++];
        }
      }
    }
    check(plan.failure == expected && parallelEffects == 0u &&
              std::all_of(serialReplay.begin(), serialReplay.end(),
                          [](std::uint32_t count) { return count == 1u; }),
          "each planner rejection emits zero parallel effects then exact serial replay");
  };
  verifyRejectedSerialReplay(noSplit, 127u, Failure::NoTwoChildWork);
  const std::array<std::uint64_t, 2> mismatchedTotal{64u, 63u};
  verifyRejectedSerialReplay(
      mismatchedTotal, 128u, Failure::InvalidOrOverflow);
}

void wholeCommandProducerPreservesCoverageAndFirstLocators() {
  const auto verify = [](std::span<const std::size_t> commandDraws,
                         std::span<const std::uint32_t> expectedBegins,
                         std::span<const std::uint32_t> expectedCounts,
                         std::uint64_t sequence) {
    PassObservationFixture fixture;
    for (const std::size_t count : commandDraws) {
      const auto command = draws(count);
      const auto commandPayloads = payloads(count);
      fixture.slot.appendDrawRun({}, {}, command, commandPayloads);
    }
    fixture.slot.appendPresent({}, {});
    fixture.finalize(sequence);
    dxmt9::encoders::SealedParallelPassSnapshotBatch output{};
    const auto result = fixture.observe(output);
    check(result.eligibleCount == 1u && output.count == 1u &&
              output.passes[0].childCount == expectedBegins.size(),
          "whole-command producer publishes one bounded eligible pass");
    const auto& pass = output.passes[0];
    std::uint32_t ordinal = 0u;
    for (std::size_t child = 0u; child < pass.childCount; ++child) {
      check(pass.childReplayOrdinalBegins[child] == expectedBegins[child] &&
                pass.childReplayOrdinalCounts[child] ==
                    expectedCounts[child] &&
                pass.childReplayOrdinalBegins[child] == ordinal &&
                pass.ranges[child].entry.commandIndex ==
                    expectedBegins[child] &&
                pass.firstDraws[child].provenance ==
                    pass.ranges[child].entry &&
                pass.firstDraws[child].generation == sequence &&
                pass.firstDraws[child].complete,
            "producer retains ordered source-qualified first-command locators");
      ordinal += pass.childReplayOrdinalCounts[child];
    }
    check(ordinal == commandDraws.size() &&
              pass.replayOrdinalBegin == 0u &&
              pass.replayOrdinalEnd == commandDraws.size(),
          "producer child ordinals cover the complete pass exactly once");
  };

  const std::array<std::size_t, 3> suffix64{64u, 32u, 32u};
  const std::array<std::uint32_t, 2> begins{0u, 1u};
  const std::array<std::uint32_t, 2> counts{1u, 2u};
  verify(suffix64, begins, counts, 418u);
  const std::array<std::size_t, 3> jagged{70u, 100u, 30u};
  verify(jagged, begins, counts, 419u);

  PassObservationFixture noTwoChildren;
  const auto first = draws(100u);
  const auto firstPayloads = payloads(first.size());
  const auto suffix = draws(27u);
  const auto suffixPayloads = payloads(suffix.size());
  noTwoChildren.slot.appendDrawRun({}, {}, first, firstPayloads);
  noTwoChildren.slot.appendDrawRun({}, {}, suffix, suffixPayloads);
  noTwoChildren.slot.appendPresent({}, {});
  noTwoChildren.finalize(420u);
  dxmt9::encoders::SealedParallelPassSnapshotBatch output{};
  const auto rejected = noTwoChildren.observe(output);
  check(rejected.eligibleCount == 0u && output.count == 0u &&
            rejected.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::
                    NoTwoChildWork)] == 1u,
        "producer distinguishes genuine no-two-child work before effects");
}

void passLocalProducerFindsBoundedCompletePasses() {
  const auto drawValues = draws(128u);
  const auto drawPayloads = payloads(drawValues.size());
  PassObservationFixture fixture;
  fixture.slot.appendClear({});
  fixture.slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                             dxmt9::core::DrawUniformPayload{}, drawValues,
                             drawPayloads);
  fixture.slot.appendPresent({}, dxmt9::core::Handle{0x71u});
  fixture.finalize(402u);
  const auto originalPlan = fixture.plan;

  dxmt9::encoders::SealedParallelPassSnapshotBatch output{};
  const auto result = fixture.observe(output);
  check(result.considered && result.candidateCount == 1u &&
            result.sealedCount == 1u && result.eligibleCount == 1u &&
            result.childCount == 2u && result.drawCount == 128u &&
            output.count == 1u,
        "Clear -> large DrawRun -> Present yields one eligible pass");
  const auto& pass = output.passes[0];
  check(pass.leadingClear.valid &&
            pass.leadingClear.kind == dxmt9::core::MetalCommandKind::Clear &&
            pass.sealingCommand.valid &&
            pass.sealingCommand.kind ==
                dxmt9::core::MetalCommandKind::Present &&
            pass.childCount == 2u && pass.passActionEpoch == 7u,
        "Clear and Present stay coordinator-owned outside child ranges");
  for (std::size_t i = 0; i < pass.childCount; ++i) {
    check(pass.ranges[i].kind ==
              dxmt9::encoders::EncodePartitionRangeKind::DrawRunEntries &&
              pass.firstDraws[i].provenance == pass.ranges[i].entry &&
              pass.firstDraws[i].entryRender.passActionEpoch ==
                  pass.passActionEpoch &&
              pass.firstDraws[i].entryRender.route ==
                  dxmt9::core::RenderRoute::Portable &&
              pass.firstDraws[i].entryRender.entryReads.canonicalized() &&
              pass.firstDraws[i].complete,
          "children own source-qualified range and first-draw values only");
  }
  check(fixture.plan.count == originalPlan.count &&
            std::equal(fixture.plan.view().begin(), fixture.plan.view().end(),
                       originalPlan.view().begin()) &&
            dxmt9::encoders::validateEncodePartitionRanges(
                fixture.plan.view(), fixture.stream),
        "observation leaves original serial partition coverage unchanged");

  auto eligibility = dxmt9::encoders::classifyParallelPassEligibility({
      .ranges = pass.rangeView(),
      .firstDrawSnapshots = pass.firstDrawView(),
      .passActionEpoch = pass.passActionEpoch,
      .explicitPlan = true,
      .planValidated = true,
      .logicalPassSealed = true,
  });
  check(eligibility.eligible,
        "pass-local snapshot feeds the existing child classifier");
  eligibility = dxmt9::encoders::classifyParallelPassEligibility({
      .ranges = pass.rangeView(),
      .firstDrawSnapshots = pass.firstDrawView(),
      .passActionEpoch = pass.passActionEpoch + 1u,
      .explicitPlan = true,
      .planValidated = true,
      .logicalPassSealed = true,
  });
  check(eligibility.fallback == ParallelPassFallbackReason::FirstDrawSnapshotMissing,
        "pass-action epoch mismatch fails before child effects");
}

void multiPassAndAttachmentBoundariesStayIndependent() {
  const auto drawValues = draws(128u);
  const auto drawPayloads = payloads(drawValues.size());
  PassObservationFixture fixture;
  fixture.slot.appendClear({});
  fixture.slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                             dxmt9::core::DrawUniformPayload{}, drawValues,
                             drawPayloads);
  fixture.slot.appendClear({});
  fixture.slot.appendDrawRun(dxmt9::core::CanonicalDrawState{},
                             dxmt9::core::DrawUniformPayload{}, drawValues,
                             drawPayloads);
  fixture.slot.appendPresent({}, {});
  fixture.finalize(403u);
  dxmt9::encoders::SealedParallelPassSnapshotBatch output{};
  const auto result = fixture.observe(output);
  check(result.candidateCount == 2u && result.eligibleCount == 2u &&
            result.childCount == 4u && result.drawCount == 256u &&
            output.count == 2u &&
            output.passes[0].passActionEpoch == 7u &&
            output.passes[1].passActionEpoch == 8u,
        "multiple complete passes receive independent bounded snapshots");

  dxmt9::core::CanonicalDrawState stateA{};
  dxmt9::core::CanonicalDrawState stateB{};
  stateA.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x81u};
  stateB.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x82u};
  PassObservationFixture attachments;
  attachments.slot.appendDrawRun(stateA, {}, drawValues, drawPayloads);
  attachments.slot.appendDrawRun(stateB, {}, drawValues, drawPayloads);
  attachments.slot.appendPresent({}, {});
  attachments.finalize(404u);
  const auto split = attachments.observe(output);
  check(split.candidateCount == 2u && split.eligibleCount == 2u &&
            output.count == 2u &&
            output.passes[0].attachments != output.passes[1].attachments &&
            output.passes[0].replayOrdinalEnd <=
                output.passes[1].replayOrdinalBegin,
        "attachment change splits candidates without over-sealing");
}

void activeReplayOrderAndPartialClearDriveExactBoundaries() {
  const auto drawValues = draws(128u);
  const auto drawPayloads = payloads(drawValues.size());
  dxmt9::encoders::SealedParallelPassSnapshotBatch output{};

  PassObservationFixture reordered;
  reordered.slot.appendDrawRun({}, {}, drawValues, drawPayloads);  // 0
  reordered.slot.appendPresent({}, {});                            // 1
  reordered.slot.appendClear({});                                  // 2
  reordered.slot.appendDrawRun({}, {}, drawValues, drawPayloads);  // 3
  reordered.slot.appendPresent({}, {});                            // 4
  reordered.finalize(411u, 4u, {2u, 3u, 4u, 0u, 1u});
  const auto activeOrder = reordered.observe(output);
  check(activeOrder.candidateCount == 2u &&
            activeOrder.eligibleCount == 2u && output.count == 2u &&
            output.passes[0].leadingClear.commandIndex == 2u &&
            output.passes[0].firstDraw.commandIndex == 3u &&
            output.passes[0].sealingCommand.commandIndex == 4u &&
            output.passes[1].firstDraw.commandIndex == 0u &&
            output.passes[1].sealingCommand.commandIndex == 1u,
        "candidate locators follow active replay order, not storage order");

  dxmt9::core::ClearDesc partial{};
  partial.rects.push_back({});
  PassObservationFixture partialClear;
  partialClear.slot.appendClear({});
  partialClear.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  partialClear.slot.appendClear(partial);
  partialClear.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  partialClear.slot.appendPresent({}, {});
  partialClear.finalize(412u);
  const auto partialResult = partialClear.observe(output);
  check(partialResult.candidateCount == 2u &&
            partialResult.eligibleCount == 2u && output.count == 2u &&
            output.passes[0].passActionEpoch == 7u &&
            output.passes[1].passActionEpoch == 9u &&
            output.passes[0].leadingClear.valid &&
            !output.passes[1].leadingClear.valid,
        "partial Clear remains coordinator-owned and advances two action "
        "epochs when it closes an active pass");
}

void producerRejectsControlsFragmentsHazardsAndBounds() {
  const auto drawValues = draws(128u);
  const auto drawPayloads = payloads(drawValues.size());
  dxmt9::encoders::SealedParallelPassSnapshotBatch output{};

  PassObservationFixture fragment;
  fragment.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  fragment.finalize(405u);
  auto result = fragment.observe(output, false, true);
  check(result.eligibleCount == 0u && output.count == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::UnsealedStart)] == 1u,
        "leading carried pass fragment fails closed");
  result = fragment.observe(output, true, false);
  check(result.eligibleCount == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::UnsealedEnd)] == 1u,
        "incomplete trailing pass fragment fails closed");

  auto malformed = fragment.plan;
  ++malformed.ranges[1].entry.uniformHandle.generation;
  result = dxmt9::encoders::produceSealedParallelPassSnapshots({
        .stream = &fragment.stream,
        .ranges = std::span<const EncodePartitionRangeSnapshot>(
            malformed.ranges.data(), malformed.count),
        .proofs = completeProofs(1u),
        .planValidated = true,
        .sourceStartsPass = true,
        .sourceEndsPass = true,
      }, output);
  check(result.eligibleCount == 1u && output.count == 1u &&
            output.passes[0].ranges[1].entry.uniformHandle !=
                malformed.ranges[1].entry.uniformHandle,
        "parallel producer derives fresh source-qualified locators instead of "
        "trusting the serial planner carrier");

  dxmt9::core::CanonicalDrawState sampling{};
  sampling.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x91u};
  sampling.hot.textures[0] = sampling.hot.colorAttachments[0].handle;
  PassObservationFixture hazard;
  hazard.slot.appendDrawRun(sampling, {}, drawValues, drawPayloads);
  hazard.slot.appendPresent({}, {});
  hazard.finalize(406u);
  result = hazard.observe(output);
  check(result.eligibleCount == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::ResourceHazard)] == 1u,
        "attachment sampling hazard fails closed");

  dxmt9::core::CanonicalDrawState aliasedSampling{};
  aliasedSampling.hot.colorAttachments[0].handle =
      dxmt9::core::Handle{0x92u};
  aliasedSampling.hot.textures[0] = dxmt9::core::Handle{0xa2u};
  PassObservationFixture aliasHazard;
  aliasHazard.slot.appendDrawRun(
      aliasedSampling, {}, drawValues, drawPayloads);
  aliasHazard.slot.appendPresent({}, {});
  aliasHazard.finalize(413u);
  ResourceProofFixture aliasProof{.aliasFrom = 0x92u, .aliasTo = 0xa2u};
  auto proofs = completeProofs();
  proofs.resources = {
      .context = &aliasProof,
      .resolve = fixtureResourceProof,
  };
  result = aliasHazard.observe(output, true, true, proofs);
  check(result.eligibleCount == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::ResourceHazard)] == 1u,
        "canonical alias identities reveal hazards hidden by raw handles");

  ResourceProofFixture failedProof{.fail = 0x92u};
  proofs.resources.context = &failedProof;
  result = aliasHazard.observe(output, true, true, proofs);
  check(result.eligibleCount == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::ResourceIdentityProof)] == 1u,
        "a proof owner that cannot resolve an identity fails closed");

  dxmt9::core::CanonicalDrawState incomplete{};
  for (std::size_t i = 0; i < incomplete.hot.streamBuffers.size(); ++i) {
    incomplete.hot.streamBuffers[i] =
        dxmt9::core::Handle{static_cast<std::uint64_t>(0x100u + i)};
  }
  PassObservationFixture resourceSet;
  resourceSet.slot.appendDrawRun(incomplete, {}, drawValues, drawPayloads);
  resourceSet.slot.appendPresent({}, {});
  resourceSet.finalize(407u);
  result = resourceSet.observe(output);
  check(result.eligibleCount == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::ResourceSetIncomplete)] == 1u,
        "incomplete exact resource set fails closed");

  PassObservationFixture control;
  control.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  control.slot.appendReadback({});
  control.finalize(408u);
  result = control.observe(output);
  check(result.eligibleCount == 0u && result.candidateCount == 0u &&
            result.fallback ==
                dxmt9::encoders::SealedParallelPassSnapshotFallback::CoordinatorCommand,
        "readback rejects observation before effects");

  auto productionLike = completeProofs(0u);
  productionLike.coordinator.flags = 0u;
  result = fragment.observe(output, true, true, productionLike);
  check(result.candidateCount == 1u && result.sealedCount == 1u &&
            result.eligibleCount == 0u && output.count == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::PassActionEpoch)] == 1u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::QueryState)] == 1u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::UpdateTextureState)] == 1u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::CaptureState)] == 1u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::InitializerState)] == 1u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::OrderedControlState)] == 1u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::SidecarState)] == 1u,
        "unresolved production coordinator facts retain candidate observation "
        "but never claim eligibility");

  proofs = completeProofs();
  proofs.resources = {};
  result = fragment.observe(output, true, true, proofs);
  check(result.eligibleCount == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::ResourceIdentityProof)] == 1u,
        "raw handles without a proof-producing owner are not canonical");

  proofs = completeProofs();
  proofs.route.resolve = unknownRoute;
  result = fragment.observe(output, true, true, proofs);
  check(result.eligibleCount == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::RenderRoute)] == 1u,
        "unknown route proof is rejected before publication");

  dxmt9::core::CanonicalDrawState portable{};
  dxmt9::core::CanonicalDrawState tile = portable;
  tile.hot.textures[0] = dxmt9::core::Handle{0xbeefu};
  PassObservationFixture mixedRoute;
  mixedRoute.slot.appendDrawRun(
      portable, {}, drawValues, drawPayloads);
  mixedRoute.slot.appendDrawRun(tile, {}, drawValues, drawPayloads);
  mixedRoute.slot.appendPresent({}, {});
  mixedRoute.finalize(416u);
  proofs = completeProofs();
  proofs.route.resolve = textureSelectedRoute;
  result = mixedRoute.observe(output, true, true, proofs);
  check(result.eligibleCount == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::RenderRoute)] == 1u,
        "mixed fixed-route proof rejects one logical pass");
}

void childBoundsAndPerfGateFailClosed() {
  using SnapshotFallback =
      dxmt9::encoders::SealedParallelPassSnapshotFallback;
  check(dxmt9::encoders::parallelPassFallbackForSnapshot(
            SnapshotFallback::NoTwoChildWork) ==
            ParallelPassFallbackReason::TooFewChildren &&
            dxmt9::encoders::parallelPassFallbackForSnapshot(
                SnapshotFallback::PlannerInvariant) ==
                ParallelPassFallbackReason::PlanNotValidated &&
            dxmt9::encoders::parallelPassFallbackForSnapshot(
                SnapshotFallback::ChildCapacity) ==
                ParallelPassFallbackReason::ChildCapacity &&
            dxmt9::encoders::parallelPassFallbackForSnapshot(
                SnapshotFallback::PassCapacity) ==
                ParallelPassFallbackReason::ChildCapacity,
        "planner and storage observations retain distinct typed snapshots");
  ProductionPlanFixture fixture;
  auto input = fixture.input();
  input.ranges = input.ranges.first(1u);
  input.firstDrawSnapshots = input.firstDrawSnapshots.first(1u);
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::TooFewChildren,
        "fewer than two children remains serial");
  std::array<EncodePartitionRangeSnapshot,
             dxmt9::encoders::kParallelRenderPassChildCapacity + 1u>
      oversizedRanges{};
  std::array<ParallelFirstDrawSnapshot,
             dxmt9::encoders::kParallelRenderPassChildCapacity + 1u>
      oversizedSnapshots{};
  input.ranges = oversizedRanges;
  input.firstDrawSnapshots = oversizedSnapshots;
  check(dxmt9::encoders::classifyParallelPassEligibility(input).fallback ==
            ParallelPassFallbackReason::ChildCapacity,
        "more than sixteen children fails closed");

  const auto manyDraws = draws(1025u);
  const auto manyPayloads = payloads(manyDraws.size());
  PassObservationFixture childOverflow;
  childOverflow.slot.appendDrawRun({}, {}, manyDraws, manyPayloads);
  childOverflow.slot.appendPresent({}, {});
  childOverflow.finalize(409u);
  dxmt9::encoders::SealedParallelPassSnapshotBatch output{};
  const auto cappedResult = childOverflow.observe(output);
  check(cappedResult.eligibleCount == 1u && output.count == 1u &&
            output.passes[0].childCount ==
                dxmt9::encoders::kParallelRenderPassChildCapacity &&
            output.passes[0].ranges.back().drawEntryCount == 65u,
        "above-cap draw volume remains exactly covered by sixteen children");

  const auto largeDraws = draws(128u);
  const auto largePayloads = payloads(largeDraws.size());
  auto appendPasses = [&](PassObservationFixture& observed,
                          std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      observed.slot.appendDrawRun({}, {}, largeDraws, largePayloads);
      observed.slot.appendPresent({}, {});
    }
  };

  PassObservationFixture exactPassCapacity;
  appendPasses(exactPassCapacity,
               dxmt9::encoders::kParallelRenderPassCandidateCapacity);
  exactPassCapacity.finalize(414u);
  const auto exactCapacity = exactPassCapacity.observe(output);
  check(exactCapacity.candidateCount == 16u &&
            exactCapacity.eligibleCount == 16u && output.count == 16u,
        "exactly sixteen logical passes fill bounded batch storage");

  PassObservationFixture passOverflow;
  appendPasses(passOverflow,
               dxmt9::encoders::kParallelRenderPassCandidateCapacity + 1u);
  passOverflow.finalize(415u);
  const auto tooManyPasses = passOverflow.observe(output);
  check(tooManyPasses.candidateCount == 17u &&
            tooManyPasses.eligibleCount == 16u && output.count == 16u &&
            tooManyPasses.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::PassCapacity)] == 1u,
        "seventeenth logical pass is observed and rejected at fixed capacity");

  const auto smallDraws = draws(64u);
  const auto smallPayloads = payloads(smallDraws.size());
  PassObservationFixture mixed;
  mixed.slot.appendDrawRun({}, {}, largeDraws, largePayloads);
  mixed.slot.appendDrawRun({}, {}, smallDraws, smallPayloads);
  mixed.slot.appendPresent({}, {});
  mixed.finalize(410u);
  const auto mixedResult = mixed.observe(output);
  check(mixedResult.eligibleCount == 1u && output.count == 1u &&
            output.passes[0].childCount == 2u &&
            output.passes[0].childrenCoverCompleteCommands &&
            output.passes[0].childReplayOrdinalBegins[0] == 0u &&
            output.passes[0].childReplayOrdinalCounts[0] == 1u &&
            output.passes[0].childReplayOrdinalBegins[1] == 1u &&
            output.passes[0].childReplayOrdinalCounts[1] == 1u,
        "one logical pass groups ordinary DrawRuns into ordered child "
        "command spans");

}

enum class EventKind : std::uint8_t {
  Prepare,
  Create,
  BeginActions,
  LogicalCommands,
  Emit,
  EndChild,
  Join,
  EndActions,
  EndParent,
  Sidecars,
  Completion,
  Abandon,
  FailStop,
};

struct Event {
  EventKind kind = EventKind::Prepare;
  std::uint32_t child = 0;
};

struct FakeChildBackend {
  std::array<Event, 64> events{};
  std::size_t eventCount = 0;
  std::array<std::uint32_t, 96> drawReplayCount{};
  std::array<std::uint32_t, 3> shadows{};
  std::uint32_t logicalCommandReplayCount = 0;
  std::uint32_t actionBeginCount = 0;
  std::uint32_t actionEndCount = 0;
  std::uint32_t sidecarCount = 0;
  std::uint32_t completionCount = 0;
  std::uint32_t failStopCount = 0;
  std::uint32_t failCreateChild = UINT32_MAX;
  ParallelPassFailurePhase injectedFailure = ParallelPassFailurePhase::None;
  std::uint32_t injectedChild = UINT32_MAX;
  ParallelPassFailurePhase terminalFailure = ParallelPassFailurePhase::None;
  std::uint32_t terminalChild = UINT32_MAX;

  void note(EventKind kind, std::uint32_t child = 0u) noexcept {
    events[eventCount++] = Event{kind, child};
  }

  bool prepareParent() noexcept {
    note(EventKind::Prepare);
    return true;
  }
  bool createChild(const ParallelPassChildPlan& child) noexcept {
    note(EventKind::Create, child.childOrdinal);
    if (child.childOrdinal == failCreateChild) {
      return false;
    }
    check(child.forceFullFirstDrawBinding,
          "child must force complete first-draw native binding");
    check(child.firstDraw.complete && child.firstDraw.generation != 0u,
          "child receives its immutable first-draw snapshot");
    check(child.binding.complete && child.binding.firstRenderPso.valid(),
          "child receives its immutable direct-binding snapshot");
    shadows[child.childOrdinal] = child.localShadowOrdinal;
    return true;
  }
  void abandonPrepared() noexcept { note(EventKind::Abandon); }
  bool shouldFail(ParallelPassFailurePhase phase,
                  std::uint32_t child = UINT32_MAX) const noexcept {
    return injectedFailure == phase &&
        (injectedChild == UINT32_MAX || injectedChild == child);
  }
  bool beginPassActions() noexcept {
    note(EventKind::BeginActions);
    ++actionBeginCount;
    return !shouldFail(ParallelPassFailurePhase::BeginPassActions);
  }
  bool replayLogicalCommands(
      std::span<const ParallelPassChildPlan> children) noexcept {
    note(EventKind::LogicalCommands);
    check(!children.empty(), "logical command replay receives child ranges");
    const auto commandIndex = children.front().range.entry.commandIndex;
    for (const auto& child : children) {
      checkEq(child.range.entry.commandIndex, commandIndex,
              "fixture child ranges belong to one logical DrawRun command");
    }
    ++logicalCommandReplayCount;
    return !shouldFail(ParallelPassFailurePhase::LogicalCommandReplay);
  }
  bool emitChild(const ParallelPassChildPlan& child) noexcept {
    note(EventKind::Emit, child.childOrdinal);
    if (shouldFail(ParallelPassFailurePhase::ChildEmission,
                   child.childOrdinal)) {
      return false;
    }
    const auto first = child.range.entry.drawParamIndex;
    for (std::uint32_t i = 0; i < child.range.drawEntryCount; ++i) {
      ++drawReplayCount[first + i];
    }
    return true;
  }
  bool endChild(std::uint32_t child) noexcept {
    note(EventKind::EndChild, child);
    return !shouldFail(ParallelPassFailurePhase::ChildEnd, child);
  }
  bool joinChild(std::uint32_t child) noexcept {
    note(EventKind::Join, child);
    return !shouldFail(ParallelPassFailurePhase::ChildJoin, child);
  }
  bool endPassActions() noexcept {
    note(EventKind::EndActions);
    ++actionEndCount;
    return !shouldFail(ParallelPassFailurePhase::EndPassActions);
  }
  bool endParent() noexcept {
    note(EventKind::EndParent);
    return !shouldFail(ParallelPassFailurePhase::ParentEnd);
  }
  bool publishSidecars() noexcept {
    note(EventKind::Sidecars);
    ++sidecarCount;
    return !shouldFail(ParallelPassFailurePhase::SidecarPublication);
  }
  bool publishCompletion() noexcept {
    note(EventKind::Completion);
    ++completionCount;
    return !shouldFail(ParallelPassFailurePhase::CompletionPublication);
  }
  void failStop(ParallelPassFailurePhase phase,
                std::uint32_t child) noexcept {
    note(EventKind::FailStop, child);
    ++failStopCount;
    terminalFailure = phase;
    terminalChild = child;
  }
};

dxmt9::encoders::ParallelPassPlanStorage eligiblePlan(
    ProductionPlanFixture& fixture) {
  dxmt9::encoders::ParallelPassPlanStorage storage{};
  check(dxmt9::encoders::planParallelRenderPassChildren(fixture.input(),
                                                         storage)
            .eligible,
        "fake executor fixture is eligible");
  for (std::size_t i = 0; i < storage.count; ++i) {
    storage.children[i].binding = bindingSnapshot(
        dxmt9::encoders::ParallelPassDirectBindingMode::Stage1Direct,
        static_cast<std::uint16_t>(i));
  }
  return storage;
}

void fakeChildrenPreserveOwnershipOrderingAndExactlyOnceReplay() {
  ProductionPlanFixture fixture;
  const auto plan = eligiblePlan(fixture);
  FakeChildBackend backend{};
  const std::array<std::uint32_t, 3> completionOrder{2u, 0u, 1u};
  const auto result = dxmt9::encoders::executeParallelRenderPass(
      plan.view(), completionOrder, backend);
  check(result.status == ParallelPassExecutionStatus::Completed &&
            result.crossedEffectBoundary,
        "fake child execution completes through the selected lane");

  std::array<std::uint32_t, 3> created{};
  std::array<std::uint32_t, 3> joined{};
  std::size_t createCount = 0;
  std::size_t joinCount = 0;
  std::size_t lastJoin = 0;
  std::size_t parentEnd = 0;
  for (std::size_t i = 0; i < backend.eventCount; ++i) {
    const auto event = backend.events[i];
    if (event.kind == EventKind::Create) {
      created[createCount++] = event.child;
    } else if (event.kind == EventKind::Join) {
      joined[joinCount++] = event.child;
      lastJoin = i;
    } else if (event.kind == EventKind::EndParent) {
      parentEnd = i;
    }
  }
  checkEq(created, std::array<std::uint32_t, 3>{0u, 1u, 2u},
          "children are created in draw order");
  checkEq(joined, completionOrder,
          "coordinator accepts arbitrary child completion order");
  check(lastJoin < parentEnd, "all children join before parent end");
  check(backend.shadows[0] != backend.shadows[1] &&
            backend.shadows[0] != backend.shadows[2] &&
            backend.shadows[1] != backend.shadows[2],
        "every child owns a distinct local native shadow");
  for (const auto count : backend.drawReplayCount) {
    checkEq(count, std::uint32_t{1}, "every draw replays exactly once");
  }
  checkEq(backend.logicalCommandReplayCount, std::uint32_t{1},
          "the split DrawRun command is coordinated exactly once");
  check(backend.actionBeginCount == 1u && backend.actionEndCount == 1u &&
            backend.sidecarCount == 1u && backend.completionCount == 1u,
        "coordinator alone owns pass actions, sidecars, and completion");
  checkEq(backend.failStopCount, std::uint32_t{0},
          "successful execution does not invoke terminal cleanup");
}

enum class ChildPlanMalformation : std::uint8_t {
  CommandRange,
  ReplayCount,
  EmptyDrawRange,
  DrawOverflow,
  MissingLocator,
  Overlap,
  SourceMismatch,
  ChildOrdinal,
  ShadowDuplicate,
  ProvenanceMismatch,
  IncompleteSnapshot,
  IncompleteEntryRender,
  UnknownRoute,
  MixedRoute,
  FullBindDisabled,
  BindingPsoMissing,
  BindingStage2Table,
  BindingResourceArray,
  BindingMixedAbi,
  BindingOverrideRebuild,
};

void malformedPlansFailClosedBeforeParentPreparation() {
  ProductionPlanFixture fixture;
  const auto valid = eligiblePlan(fixture);
  const std::array<std::uint32_t, 3> completionOrder{0u, 1u, 2u};
  struct Case {
    ChildPlanMalformation malformation;
    ParallelPassFallbackReason expected;
  };
  const std::array cases{
      Case{ChildPlanMalformation::CommandRange,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::ReplayCount,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::EmptyDrawRange,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::DrawOverflow,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::MissingLocator,
           ParallelPassFallbackReason::ChildRangeInvalid},
      Case{ChildPlanMalformation::Overlap,
           ParallelPassFallbackReason::ChildRangeOrderInvalid},
      Case{ChildPlanMalformation::SourceMismatch,
           ParallelPassFallbackReason::ChildRangeOrderInvalid},
      Case{ChildPlanMalformation::ChildOrdinal,
           ParallelPassFallbackReason::ChildOrdinalInvalid},
      Case{ChildPlanMalformation::ShadowDuplicate,
           ParallelPassFallbackReason::LocalShadowInvalid},
      Case{ChildPlanMalformation::ProvenanceMismatch,
           ParallelPassFallbackReason::FirstDrawProvenanceInvalid},
      Case{ChildPlanMalformation::IncompleteSnapshot,
           ParallelPassFallbackReason::FirstDrawProvenanceInvalid},
      Case{ChildPlanMalformation::IncompleteEntryRender,
           ParallelPassFallbackReason::FirstDrawProvenanceInvalid},
      Case{ChildPlanMalformation::UnknownRoute,
           ParallelPassFallbackReason::FirstDrawProvenanceInvalid},
      Case{ChildPlanMalformation::MixedRoute,
           ParallelPassFallbackReason::FirstDrawProvenanceInvalid},
      Case{ChildPlanMalformation::FullBindDisabled,
           ParallelPassFallbackReason::FullFirstDrawBindingRequired},
      Case{ChildPlanMalformation::BindingPsoMissing,
           ParallelPassFallbackReason::BindingPsoMissing},
      Case{ChildPlanMalformation::BindingStage2Table,
           ParallelPassFallbackReason::BindingStage2ArgumentTable},
      Case{ChildPlanMalformation::BindingResourceArray,
           ParallelPassFallbackReason::BindingResourceArray},
      Case{ChildPlanMalformation::BindingMixedAbi,
           ParallelPassFallbackReason::BindingMixedAbi},
      Case{ChildPlanMalformation::BindingOverrideRebuild,
           ParallelPassFallbackReason::BindingOverrideRebuild},
  };

  for (const auto& testCase : cases) {
    auto malformed = valid;
    switch (testCase.malformation) {
    case ChildPlanMalformation::CommandRange:
      malformed.children[1].range.kind =
          dxmt9::encoders::EncodePartitionRangeKind::CommandSegment;
      break;
    case ChildPlanMalformation::ReplayCount:
      malformed.children[1].range.replayOrdinalCount = 2u;
      break;
    case ChildPlanMalformation::EmptyDrawRange:
      malformed.children[1].range.drawEntryCount = 0u;
      break;
    case ChildPlanMalformation::DrawOverflow:
      malformed.children[1].range.entry.drawParamIndex = UINT32_MAX;
      break;
    case ChildPlanMalformation::MissingLocator:
      malformed.children[1].range.entry.source.tapeSource = {};
      break;
    case ChildPlanMalformation::Overlap:
      --malformed.children[1].range.entry.drawParamIndex;
      malformed.children[1].firstDraw.provenance =
          malformed.children[1].range.entry;
      break;
    case ChildPlanMalformation::SourceMismatch:
      ++malformed.children[1].range.entry.source.seqId;
      malformed.children[1].firstDraw.provenance =
          malformed.children[1].range.entry;
      break;
    case ChildPlanMalformation::ChildOrdinal:
      malformed.children[1].childOrdinal = 0u;
      break;
    case ChildPlanMalformation::ShadowDuplicate:
      malformed.children[1].localShadowOrdinal =
          malformed.children[0].localShadowOrdinal;
      break;
    case ChildPlanMalformation::ProvenanceMismatch:
      ++malformed.children[1].firstDraw.provenance.commandIndex;
      break;
    case ChildPlanMalformation::IncompleteSnapshot:
      malformed.children[1].firstDraw.complete = false;
      break;
    case ChildPlanMalformation::IncompleteEntryRender:
      malformed.children[1].firstDraw.entryRender.flags = 0u;
      break;
    case ChildPlanMalformation::UnknownRoute:
      malformed.children[1].firstDraw.entryRender.route =
          dxmt9::core::RenderRoute::Unknown;
      break;
    case ChildPlanMalformation::MixedRoute:
      malformed.children[1].firstDraw.entryRender.route =
          dxmt9::core::RenderRoute::Tile;
      break;
    case ChildPlanMalformation::FullBindDisabled:
      malformed.children[1].forceFullFirstDrawBinding = false;
      break;
    case ChildPlanMalformation::BindingPsoMissing:
      malformed.children[1].binding = {};
      break;
    case ChildPlanMalformation::BindingStage2Table:
      malformed.children[1].binding.reject =
          dxmt9::encoders::ParallelPassBindingRejectReason::
              Stage2ArgumentTable;
      break;
    case ChildPlanMalformation::BindingResourceArray:
      malformed.children[1].binding.reject =
          dxmt9::encoders::ParallelPassBindingRejectReason::ResourceArray;
      break;
    case ChildPlanMalformation::BindingMixedAbi:
      malformed.children[1].binding.mode =
          dxmt9::encoders::ParallelPassDirectBindingMode::Stage2DirectCbuf;
      break;
    case ChildPlanMalformation::BindingOverrideRebuild:
      malformed.children[1].binding.reject =
          dxmt9::encoders::ParallelPassBindingRejectReason::OverrideRebuild;
      break;
    }
    FakeChildBackend backend{};
    const auto result = dxmt9::encoders::executeParallelRenderPass(
        malformed.view(), completionOrder, backend);
    check(result.status == ParallelPassExecutionStatus::SerialFallback &&
              !result.crossedEffectBoundary && backend.eventCount == 0u &&
              result.failurePhase ==
                  ParallelPassFailurePhase::ChildPlanValidation &&
              result.fallback == testCase.expected,
          "malformed child plan fails closed before parent preparation");
  }
}

void failuresSeparatePreEffectFallbackFromPostEffectFailStop() {
  ProductionPlanFixture fixture;
  const auto plan = eligiblePlan(fixture);
  const std::array<std::uint32_t, 3> completionOrder{0u, 1u, 2u};

  FakeChildBackend preEffect{};
  preEffect.failCreateChild = 1u;
  const auto fallback = dxmt9::encoders::executeParallelRenderPass(
      plan.view(), completionOrder, preEffect);
  check(fallback.status == ParallelPassExecutionStatus::SerialFallback &&
            !fallback.crossedEffectBoundary &&
            fallback.fallback ==
                ParallelPassFallbackReason::ChildCreationFailed,
        "child creation rejection selects serial before effects");
  check(preEffect.actionBeginCount == 0u &&
            preEffect.logicalCommandReplayCount == 0u &&
            preEffect.sidecarCount == 0u && preEffect.completionCount == 0u,
        "pre-effect fallback publishes no parallel-pass ownership effects");
  checkEq(preEffect.failStopCount, std::uint32_t{0},
          "pre-effect serial fallback does not invoke fail-stop cleanup");

  FakeChildBackend invalidJoin{};
  const std::array<std::uint32_t, 3> duplicateCompletion{0u, 0u, 2u};
  const auto invalid = dxmt9::encoders::executeParallelRenderPass(
      plan.view(), duplicateCompletion, invalidJoin);
  check(invalid.status == ParallelPassExecutionStatus::SerialFallback &&
            !invalid.crossedEffectBoundary && invalidJoin.eventCount == 0u &&
            invalid.fallback ==
                ParallelPassFallbackReason::InvalidCompletionOrder,
        "invalid arbitrary completion order falls back before preparation");

  struct EffectFailure {
    ParallelPassFailurePhase phase;
    std::uint32_t child;
  };
  const std::array failures{
      EffectFailure{ParallelPassFailurePhase::BeginPassActions, UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::LogicalCommandReplay,
                    UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::ChildEmission, 0u},
      EffectFailure{ParallelPassFailurePhase::ChildEmission, 1u},
      EffectFailure{ParallelPassFailurePhase::ChildEmission, 2u},
      EffectFailure{ParallelPassFailurePhase::ChildEnd, 0u},
      EffectFailure{ParallelPassFailurePhase::ChildEnd, 1u},
      EffectFailure{ParallelPassFailurePhase::ChildEnd, 2u},
      EffectFailure{ParallelPassFailurePhase::ChildJoin, 0u},
      EffectFailure{ParallelPassFailurePhase::ChildJoin, 1u},
      EffectFailure{ParallelPassFailurePhase::ChildJoin, 2u},
      EffectFailure{ParallelPassFailurePhase::EndPassActions, UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::ParentEnd, UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::SidecarPublication, UINT32_MAX},
      EffectFailure{ParallelPassFailurePhase::CompletionPublication,
                    UINT32_MAX},
  };
  for (const auto& failure : failures) {
    FakeChildBackend postEffect{};
    postEffect.injectedFailure = failure.phase;
    postEffect.injectedChild = failure.child;
    const auto failStop = dxmt9::encoders::executeParallelRenderPass(
        plan.view(), completionOrder, postEffect);
    const std::uint32_t expectedChild =
        failure.child == UINT32_MAX ? 0u : failure.child;
    check(failStop.status == ParallelPassExecutionStatus::FailStop &&
              failStop.crossedEffectBoundary &&
              failStop.failurePhase == failure.phase &&
              failStop.affectedChild == expectedChild &&
              postEffect.failStopCount == 1u &&
              postEffect.terminalFailure == failure.phase &&
              postEffect.terminalChild == expectedChild &&
              postEffect.events[postEffect.eventCount - 1u].kind ==
                  EventKind::FailStop,
          "every post-effect failure invokes terminal fail-stop cleanup once");
  }
}

void economicsClassifierIsPureBoundedAndEnforcedBeforeEffects() {
  using Decision = dxmt9::encoders::ParallelPassEconomicsDecision;
  using Reason = dxmt9::encoders::ParallelPassEconomicsRejectReason;
  using Summary = dxmt9::encoders::ParallelPassEconomicsSummary;
  using dxmt9::encoders::classifyParallelPassEconomics;

  const auto summary = [](std::uint32_t children,
                          std::uint32_t minimum,
                          std::uint32_t maximum,
                          std::uint64_t draws,
                          std::uint64_t stage1,
                          std::uint64_t stage2b,
                          std::uint64_t forced,
                          std::uint64_t psoBoundaryTransitions,
                          std::uint64_t uniformBoundaryTransitions) {
    return Summary{
        .totalDraws = draws,
        .stage1Draws = stage1,
        .stage2bDraws = stage2b,
        .forcedStage1Draws = forced,
        .psoBoundaryTransitions = psoBoundaryTransitions,
        .uniformBoundaryTransitions = uniformBoundaryTransitions,
        .childCount = children,
        .minimumChildDraws = minimum,
        .maximumChildDraws = maximum,
        .valid = true,
    };
  };

  check(classifyParallelPassEconomics(
            summary(2u, 63u, 63u, 126u, 126u, 0u, 0u, 1u, 1u)).reject ==
            Reason::ThinChild,
        "2x63 is rejected at the existing eligibility quantum");
  check(classifyParallelPassEconomics(
            summary(2u, 64u, 64u, 128u, 128u, 0u, 0u, 0u, 0u)).reject ==
            Reason::PsoFirstBindAmplification,
        "stable 2x64 rejects a pure child first-bind reset");
  check(classifyParallelPassEconomics(
            summary(2u, 64u, 64u, 128u, 128u, 0u, 0u, 1u, 1u)).accepted,
        "true PSO and uniform changes at a 2x64 boundary are accepted");
  check(classifyParallelPassEconomics(
            summary(16u, 64u, 64u, 1024u, 0u, 1024u, 0u, 15u, 15u)).accepted,
        "16x64 stays structurally eligible without a worker-count cap");
  check(classifyParallelPassEconomics(
            summary(2u, 64u, 64u, 128u, 0u, 0u, 128u, 1u, 1u)).reject ==
            Reason::ForcedStage1,
        "forced Stage 1 draw volume rejects economics");
  check(classifyParallelPassEconomics(
            summary(4u, 64u, 64u, 256u, 256u, 0u, 0u, 2u, 3u)).reject ==
            Reason::PsoFirstBindAmplification,
        "one missing PSO boundary rejects at its exact bound");
  check(classifyParallelPassEconomics(
            summary(4u, 64u, 64u, 256u, 256u, 0u, 0u, 3u, 2u)).reject ==
            Reason::UniformFirstBindAmplification,
        "one missing uniform boundary rejects at its exact bound");
  check(classifyParallelPassEconomics(
            summary(4u, 64u, 64u, 256u, 256u, 0u, 0u, 3u, 3u)).accepted,
        "all k-1 changed child boundaries meet both first-bind bounds");
  check(classifyParallelPassEconomics(
            summary(2u, 64u, 64u, 128u, 128u, 0u, 0u, 0u, 0u)).reject ==
            Reason::PsoFirstBindAmplification,
        "internal A-B-A churn cannot pay for a stable child boundary");
  check(classifyParallelPassEconomics(
            summary(2u, 64u, 192u, 256u, 256u, 0u, 0u, 1u, 1u)).reject ==
            Reason::UnbalancedChild,
        "64/192 children reject critical-path imbalance over one quantum");
  check(classifyParallelPassEconomics(
            summary(2u, 64u, 128u, 192u, 192u, 0u, 0u, 1u, 1u)).accepted,
        "imbalance within one existing quantum remains eligible");
  auto invalid = summary(
      2u, 64u, 64u, 128u, 127u, 0u, 0u, 1u, 1u);
  check(classifyParallelPassEconomics(invalid).reject ==
            Reason::InvalidOrOverflow,
        "non-conserving ABI counts fail closed");
  invalid = summary(2u, 64u, 64u, 128u, 128u, 0u, 0u, 2u, 1u);
  check(classifyParallelPassEconomics(invalid).reject ==
            Reason::InvalidOrOverflow,
        "more boundary transitions than boundaries fails closed");
  invalid = summary(2u, 64u, 100u, 128u, 128u, 0u, 0u, 1u, 1u);
  check(classifyParallelPassEconomics(invalid).reject ==
            Reason::InvalidOrOverflow,
        "impossible min/max/total summary fails closed");
  invalid = summary(2u, 64u, 64u, 128u, 128u, 0u, 0u, 1u, 1u);
  invalid.overflow = true;
  check(classifyParallelPassEconomics(invalid).reject ==
            Reason::InvalidOrOverflow,
        "overflow fails closed");

  const std::array<Reason, 6> rejectReasons{
      Reason::ForcedStage1,
      Reason::ThinChild,
      Reason::UnbalancedChild,
      Reason::PsoFirstBindAmplification,
      Reason::UniformFirstBindAmplification,
      Reason::InvalidOrOverflow,
  };
  for (const auto reason : rejectReasons) {
    const auto accounting = dxmt9::encoders::accountParallelPassEconomics(
        Decision{.reject = reason, .considered = true, .accepted = false});
    check(accounting.conserves() && accounting.considered == 1u &&
              accounting.accepted == 0u &&
              accounting.serialFallback == 1u &&
              accounting.rejectionCounts[static_cast<std::size_t>(reason)] ==
                  1u,
          "each production rejection contributes one nonoverlapping reason");
  }
  const auto acceptedAccounting =
      dxmt9::encoders::accountParallelPassEconomics(
          Decision{.reject = Reason::None,
                   .considered = true,
                   .accepted = true});
  check(acceptedAccounting.conserves() &&
            acceptedAccounting.considered == 1u &&
            acceptedAccounting.accepted == 1u &&
            acceptedAccounting.serialFallback == 0u,
        "considered equals accepted plus serial fallback");
  const auto malformedAccounting =
      dxmt9::encoders::accountParallelPassEconomics(
          Decision{.reject = Reason::None,
                   .considered = true,
                   .accepted = false});
  check(malformedAccounting.conserves() &&
            malformedAccounting.serialFallback == 1u &&
            malformedAccounting.rejectionCounts[static_cast<std::size_t>(
                Reason::InvalidOrOverflow)] == 1u,
        "malformed accounting decisions fail closed and still conserve");

  const std::array<Summary, 6> rejectedSummaries{
      summary(2u, 64u, 64u, 128u, 0u, 0u, 128u, 1u, 1u),
      summary(2u, 63u, 63u, 126u, 0u, 126u, 0u, 1u, 1u),
      summary(2u, 64u, 192u, 256u, 0u, 256u, 0u, 1u, 1u),
      summary(2u, 64u, 64u, 128u, 0u, 128u, 0u, 0u, 1u),
      summary(2u, 64u, 64u, 128u, 0u, 128u, 0u, 1u, 0u),
      invalid,
  };
  for (const auto& rejectedSummary : rejectedSummaries) {
    std::uint32_t parallelEffects = 0u;
    std::array<std::uint32_t, 256> serialReplayCounts{};
    const auto rejected = dxmt9::encoders::dispatchParallelPassEconomics(
        rejectedSummary, [&] { ++parallelEffects; },
        [&] {
          for (std::size_t draw = 0u;
               draw < rejectedSummary.totalDraws; ++draw) {
            ++serialReplayCounts[draw];
          }
        });
    check(!rejected.accepted && parallelEffects == 0u &&
              std::all_of(
                  serialReplayCounts.begin(),
                  serialReplayCounts.begin() + rejectedSummary.totalDraws,
                  [](std::uint32_t count) { return count == 1u; }),
          "every economics rejection emits zero parallel effects then exact serial replay");
  }

  std::uint32_t observations = 0u;
  check(!dxmt9::encoders::observeParallelPassEconomicsCountersIfEnabled(
            false,
            summary(2u, 64u, 64u, 128u, 128u, 0u, 0u, 1u, 1u),
            [&](const Summary&, const Decision&) { ++observations; }) &&
            observations == 0u,
        "perf-off economics performs zero counter-observation work");
  check(dxmt9::encoders::observeParallelPassEconomicsCountersIfEnabled(
            true,
            summary(2u, 64u, 64u, 128u, 0u, 128u, 0u, 1u, 1u),
            [&](const Summary&, const Decision& decision) {
              ++observations;
              check(decision.accepted,
                    "retained Stage 2b is accepted by production policy");
            }) && observations == 1u,
        "perf-on counter observation reports the enforced decision once");
}

struct SemanticPlanFixture {
  dxmt9::encoders::SealedParallelPassSnapshot snapshot{};
  std::array<ParallelPassChildPlan,
             dxmt9::encoders::kParallelRenderPassChildCapacity>
      children{};
  std::uint32_t count = 0;
  dxmt9::core::CpuReadyTape::SourceRef expectedSource{};
  dxmt9::core::ExactResourceSet resolvedReads{};
  dxmt9::core::ExactResourceSet resolvedWrites{};
  const SemanticPlanFixture* authorityOwner = nullptr;
  mutable std::uint32_t authorityCalls = 0u;
  mutable std::uint32_t resolverCalls = 0u;
  std::uint32_t resolvedCommandCountOverride = 0u;

  static bool resolveAuthority(
      const void* context,
      const dxmt9::core::CpuReadyTape::SourceRef& source,
      std::uint64_t seqId,
      std::uint32_t replayOrdinalBegin,
      std::uint32_t replayOrdinalEnd,
      dxmt9::encoders::SealedParallelPassSnapshot& authoritative) noexcept {
    const auto& fixture = *static_cast<const SemanticPlanFixture*>(context);
    const auto& owner = fixture.authorityOwner ? *fixture.authorityOwner : fixture;
    auto& mutableOwner = *const_cast<SemanticPlanFixture*>(&owner);
    if (source != owner.expectedSource || seqId != owner.snapshot.seqId ||
        replayOrdinalBegin != owner.snapshot.replayOrdinalBegin ||
        replayOrdinalEnd != owner.snapshot.replayOrdinalEnd) {
      return false;
    }
    ++mutableOwner.authorityCalls;
    authoritative = owner.snapshot;
    return true;
  }

  static bool resolveCoverage(
      const void* context,
      const dxmt9::encoders::SealedParallelPassSnapshot& snapshot,
      const ParallelPassChildPlan& child,
      dxmt9::encoders::ParallelPassResolvedCoverage& coverage) noexcept {
    const auto& fixture = *static_cast<const SemanticPlanFixture*>(context);
    auto& mutableFixture = *const_cast<SemanticPlanFixture*>(&fixture);
    ++mutableFixture.resolverCalls;
    if (snapshot.source != fixture.expectedSource ||
        child.childOrdinal >= fixture.count ||
        child.range.entry.drawParamIndex != child.childOrdinal * 64u ||
        child.range.drawEntryCount != 64u) {
      return false;
    }
    coverage.drawCount = 64u;
    coverage.commandCount = 1u;
    coverage.commands[0] = {
        .replayOrdinal = child.replayOrdinalBegin,
        .commandIndex = child.range.entry.commandIndex,
        .drawParamBegin = 0u,
        .drawParamCount = static_cast<std::uint32_t>(fixture.snapshot.drawCount),
    };
    if (fixture.resolvedCommandCountOverride != 0u) {
      coverage.commandCount = fixture.resolvedCommandCountOverride;
    }
    coverage.reads = fixture.resolvedReads;
    coverage.writes = fixture.resolvedWrites;
    coverage.attachments = fixture.snapshot.attachments;
    coverage.route = dxmt9::core::RenderRoute::Portable;
    coverage.passActionEpoch = fixture.snapshot.passActionEpoch;
    return true;
  }

  dxmt9::encoders::ParallelPassCoverageResolver coverageResolver() const noexcept {
    return {.context = this, .resolve = resolveCoverage};
  }

  dxmt9::encoders::ParallelPassSnapshotAuthority authorityResolver()
      const noexcept {
    return {.context = this, .resolve = resolveAuthority};
  }

  explicit SemanticPlanFixture(std::uint32_t childCount,
                               std::uint32_t sourceIndex = 3u,
                               std::uint64_t sourceGeneration = 501u) {
    using namespace dxmt9::encoders;
    count = childCount;
    const auto source = dxmt9::core::CpuReadyTape::SourceRef{
        .id = {.index = sourceIndex, .generation = sourceGeneration},
        .storage = {.firstPage = 9u,
                    .pageCount = 2u,
                    .generation = sourceGeneration + 10u},
    };
    expectedSource = source;
    const std::uint32_t drawsPerChild = 64u;
    snapshot.source = source;
    snapshot.seqId = sourceGeneration;
    snapshot.passActionEpoch = 7u;
    snapshot.coordinatorProof = makeParallelPassCoordinatorProofSnapshot({
        .firstPassActionEpoch = snapshot.passActionEpoch,
        .queryAbsent = true,
        .updateTextureAbsent = true,
        .captureInactive = true,
        .initializerIndependent = true,
        .orderedControlAbsent = true,
        .sidecarObservationAbsent = true,
    });
    snapshot.attachments.sampleCount = 1u;
    snapshot.attachmentWrites.flags =
        dxmt9::core::ExactResourceSetComplete |
        dxmt9::core::ExactResourceSetCanonicalized;
    snapshot.resourceReads.flags =
        dxmt9::core::ExactResourceSetComplete |
        dxmt9::core::ExactResourceSetCanonicalized;
    snapshot.resourceReads.add(0x11u);
    snapshot.attachmentWrites.add(0x22u);
    resolvedReads = snapshot.resourceReads;
    resolvedWrites = snapshot.attachmentWrites;
    snapshot.replayOrdinalBegin = 0u;
    snapshot.replayOrdinalEnd = 1u;
    snapshot.drawCount = static_cast<std::uint64_t>(childCount) *
        drawsPerChild;
    snapshot.childCount = childCount;
    snapshot.sealedAtSourceEnd = true;
    snapshot.firstDraw = {
        .source = {
            .tapeSource = source,
            .slotIndex = source.id.index,
            .seqId = sourceGeneration,
        },
        .replayOrdinal = 0u,
        .commandIndex = 11u,
        .kind = dxmt9::core::MetalCommandKind::DrawRun,
        .valid = true,
    };
    for (std::uint32_t i = 0u; i < childCount; ++i) {
      const auto entry = EncodePartitionRangeSnapshot{
          .kind = EncodePartitionRangeKind::DrawRunEntries,
          .replayOrdinalBegin = 0u,
          .replayOrdinalCount = 1u,
          .drawEntryCount = drawsPerChild,
          .entry = {
              .source = snapshot.firstDraw.source,
              .commandIndex = 11u,
              .drawRunRecordIndex = 2u,
              .stateIndex = 3u,
              .drawParamIndex = i * drawsPerChild,
          },
      };
      const auto firstDraw = ParallelFirstDrawSnapshot{
          .provenance = entry.entry,
          .entryRender = {
              .attachments = snapshot.attachments,
              .entryReads = snapshot.resourceReads,
              .route = dxmt9::core::RenderRoute::Portable,
              .passActionEpoch = snapshot.passActionEpoch,
              .flags = dxmt9::core::RenderContinuationKeyValid |
                       dxmt9::core::RenderContinuationEntryStateComplete,
          },
          .generation = sourceGeneration,
          .complete = true,
      };
      snapshot.ranges[i] = entry;
      snapshot.firstDraws[i] = firstDraw;
      snapshot.childReplayOrdinalBegins[i] = 0u;
      snapshot.childReplayOrdinalCounts[i] = 1u;
      snapshot.childDrawCounts[i] = drawsPerChild;
      children[i] = {
          .range = entry,
          .firstDraw = firstDraw,
          .binding = bindingSnapshot(
              dxmt9::encoders::ParallelPassDirectBindingMode::Stage1Direct,
              static_cast<std::uint16_t>(i + 1u)),
          .replayOrdinalBegin = 0u,
          .replayOrdinalCount = 1u,
          .childOrdinal = i,
          .localShadowOrdinal = i + 1u,
          .coversCompleteCommands = false,
          .forceFullFirstDrawBinding = true,
      };
    }
    authorityOwner = this;
  }

  std::span<const ParallelPassChildPlan> view() const noexcept {
    return {children.data(), count};
  }

  dxmt9::encoders::ParallelPassCandidateCost cost(
      std::int64_t serialWork) const noexcept {
    using namespace dxmt9::encoders;
    ParallelPassCandidateCost result{};
    result.economics = {
        .totalDraws = snapshot.drawCount,
        .stage1Draws = snapshot.drawCount,
        .psoBoundaryTransitions = count - 1u,
        .uniformBoundaryTransitions = count - 1u,
        .childCount = count,
        .minimumChildDraws = 64u,
        .maximumChildDraws = 64u,
        .valid = true,
    };
    result.serialWork.raw = serialWork;
    result.valid = true;
    return result;
  }
};

void semanticPlanMutationAndCoverageProofsFailClosed() {
  using namespace dxmt9::encoders;
  SemanticPlanFixture fixture(3u);
  const auto valid = validateParallelPassSemanticPlan(
      fixture.snapshot, fixture.view(), fixture.authorityResolver(),
      fixture.coverageResolver());
  check(valid.accepted() && fixture.authorityCalls == 1u &&
            fixture.resolverCalls == fixture.count,
        "coherent sealed snapshot produces one owner lookup and one exact resolver call per child");

  auto expectInvalid = [&](auto mutate, std::string_view message) {
    SemanticPlanFixture candidate = fixture;
    mutate(candidate);
    check(!validateParallelPassSemanticPlan(
                candidate.snapshot, candidate.view(),
                candidate.authorityResolver(), candidate.coverageResolver())
                .accepted(),
          message);
  };
  expectInvalid([](auto& f) {
    f.snapshot.coordinatorProof.flags &= ~ParallelPassQueryAbsent;
  }, "coordinator proof mutation fails closed");
  expectInvalid([](auto& f) { f.snapshot.passActionEpoch = 8u; },
                "action epoch mutation fails closed");
  expectInvalid([](auto& f) {
    f.snapshot.sealedAtSourceEnd = false;
    f.snapshot.sealingCommand = {};
  }, "missing sealed-end evidence fails closed");
  expectInvalid([](auto& f) { f.snapshot.seqId = 502u; },
                "snapshot source generation mutation fails closed");
  expectInvalid([](auto& f) { f.snapshot.firstDraws[1].generation++; },
                "first-draw generation mutation fails closed");
  expectInvalid([](auto& f) { f.snapshot.firstDraw.commandIndex++; },
                "first-draw locator command mutation fails closed");
  expectInvalid([](auto& f) {
    f.snapshot.sealedAtSourceEnd = false;
    f.snapshot.sealingCommand = {
        .source = f.snapshot.firstDraw.source,
        .replayOrdinal = f.snapshot.replayOrdinalEnd,
        .commandIndex = f.snapshot.firstDraw.commandIndex + 1u,
        .kind = dxmt9::core::MetalCommandKind::Present,
        .valid = true,
    };
  }, "stale sealing locator mutation fails closed");
  expectInvalid([](auto& f) { f.snapshot.resourceReads.handles[0] = 0x99u; },
                "exact read-set mutation fails closed");
  expectInvalid([](auto& f) { f.snapshot.attachmentWrites.handles[0] = 0x98u; },
                "exact write-set mutation fails closed");
  expectInvalid([](auto& f) {
    f.children[1].binding.mode = ParallelPassDirectBindingMode::Stage2DirectCbuf;
  }, "mixed child binding ABI mutation fails closed");
  expectInvalid([](auto& f) { f.snapshot.ranges[1].drawEntryCount = 63u; },
                "snapshot range mutation fails closed");
  expectInvalid([](auto& f) {
    f.children[1].range.entry.source.tapeSource.storage.generation++;
  }, "child source identity mutation fails closed");
  expectInvalid([](auto& f) { f.snapshot.attachments.sampleCount = 0u; },
                "attachment identity mutation fails closed");
  expectInvalid([](auto& f) {
    f.snapshot.resourceReads.flags &= ~dxmt9::core::ExactResourceSetCanonicalized;
  }, "resource completeness mutation fails closed");
  expectInvalid([](auto& f) { f.snapshot.firstDraws[1].complete = false; },
                "first-draw completeness mutation fails closed");
  expectInvalid([](auto& f) { f.snapshot.childDrawCounts[1] = 63u; },
                "draw coverage mutation fails closed");
  expectInvalid([](auto& f) {
    f.children[1].range.entry.drawParamIndex = 65u;
  }, "draw gap mutation fails closed");
  expectInvalid([](auto& f) { f.children[1].childOrdinal = 0u; },
                "child ordinal mutation fails closed");
  expectInvalid([](auto& f) { f.count = 1u; f.snapshot.childCount = 1u; },
                "capacity mutation fails closed");
  expectInvalid([](auto& f) {
    f.resolvedCommandCountOverride =
        static_cast<std::uint32_t>(
            dxmt9::encoders::ParallelPassResolvedCoverage{}.commands.size()) +
        1u;
  }, "coverage command count above bounded storage fails before indexing");

  SemanticPlanFixture overlap(3u);
  overlap.snapshot.attachmentWrites.handles[0] = 0x11u;
  overlap.resolvedWrites.handles[0] = 0x11u;
  check(!validateParallelPassSemanticPlan(
              overlap.snapshot, overlap.view(), overlap.authorityResolver(),
              overlap.coverageResolver())
              .accepted(),
        "exact read/write overlap fails closed");

  for (std::uint32_t count = 2u;
       count <= kParallelRenderPassChildCapacity; ++count) {
    SemanticPlanFixture bounded(count);
    check(validateParallelPassSemanticPlan(bounded.snapshot,
                                           bounded.view(),
                                           bounded.authorityResolver(),
                                           bounded.coverageResolver()).accepted(),
          "every bounded child count has a coherent proof");
  }
}

void fixedPointAndCandidateSelectionAreCheckedAndPermutationIndependent() {
  using namespace dxmt9::encoders;
  ParallelPassFixedPoint one{}, two{}, result{};
  check(parallelPassFixedPointFromUnsigned(1u, one) &&
            parallelPassFixedPointFromUnsigned(2u, two),
        "fixed-point integer conversion is bounded");
  check(!parallelPassFixedPointFromUnsigned(UINT64_MAX, result) &&
            !parallelPassFixedPointAdd(
                {.raw = ParallelPassFixedPoint::kMaxRaw}, one, result) &&
            !parallelPassFixedPointMultiply(
                {.raw = ParallelPassFixedPoint::kMaxRaw}, two, result),
        "fixed-point conversion and helpers reject UINT64_MAX/high-domain values");
  check(parallelPassFixedPointAdd(one, two, result) && result.raw == 3ll *
            ParallelPassFixedPoint::kFraction,
        "fixed-point addition is exact");
  check(parallelPassFixedPointSubtract(two, one, result) &&
            result.raw == ParallelPassFixedPoint::kFraction,
        "fixed-point subtraction is exact");
  check(parallelPassFixedPointMultiply(one, two, result) &&
            result.raw == 2ll * ParallelPassFixedPoint::kFraction,
        "fixed-point multiplication is exact");
  ParallelPassFixedPoint nearMax{
      .raw = std::numeric_limits<std::int64_t>::max()};
  check(!parallelPassFixedPointAdd(nearMax, one, result) &&
            !parallelPassFixedPointMultiply(nearMax, two, result),
        "fixed-point overflow never wraps");

  SemanticPlanFixture high(3u, 8u, 508u);
  SemanticPlanFixture tiedFew(2u, 5u, 505u);
  SemanticPlanFixture low(3u, 2u, 502u);
  std::array<ParallelPassCandidateInput, 3> candidates{
      ParallelPassCandidateInput{&high.snapshot, high.view(), high.cost(400),
                                 high.authorityResolver(),
                                 high.coverageResolver(), 2u},
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(),
                                 tiedFew.cost(400), tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), 1u},
      ParallelPassCandidateInput{&low.snapshot, low.view(), low.cost(300),
                                 low.authorityResolver(), low.coverageResolver(),
                                 0u},
  };
  const auto selected = selectParallelPassCandidate(candidates);
  check(selected.selected && selected.candidateOrdinal == 1u &&
            selected.score.childCount == 2u,
        "equal benefit prefers fewer safe children");
  const auto original = selected;
  std::swap(candidates[0], candidates[2]);
  const auto permuted = selectParallelPassCandidate(candidates);
  check(permuted.selected && permuted.candidateOrdinal ==
            original.candidateOrdinal && permuted.score == original.score,
        "candidate selection is permutation independent");
  const auto allPermutationBase = std::array{
      ParallelPassCandidateInput{&high.snapshot, high.view(), high.cost(400),
                                 high.authorityResolver(),
                                 high.coverageResolver(), 2u},
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(),
                                 tiedFew.cost(400), tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), 1u},
      ParallelPassCandidateInput{&low.snapshot, low.view(), low.cost(300),
                                 low.authorityResolver(), low.coverageResolver(),
                                 0u}};
  auto allPermutation = allPermutationBase;
  do {
    const auto selection = selectParallelPassCandidate(allPermutation);
    check(selection.selected && selection.candidateOrdinal == 1u,
          "three-candidate selection is invariant for every permutation");
  } while (std::next_permutation(
      allPermutation.begin(), allPermutation.end(),
      [](const auto& left, const auto& right) {
        return left.candidateOrdinal < right.candidateOrdinal;
      }));

  auto negativeEvidence = tiedFew;
  negativeEvidence.snapshot.coordinatorProof.flags &=
      ~ParallelPassQueryAbsent;
  std::array<ParallelPassCandidateInput, 1> negativeEvidenceInput{
      ParallelPassCandidateInput{&negativeEvidence.snapshot,
                                 negativeEvidence.view(),
                                 negativeEvidence.cost(400),
                                 negativeEvidence.authorityResolver(),
                                 negativeEvidence.coverageResolver(), 0u}};
  check(!selectParallelPassCandidate(negativeEvidenceInput).selected,
        "adding coordinator negative evidence cannot introduce selection");

  auto negative = tiedFew.cost(0);
  std::array<ParallelPassCandidateInput, 1> negativeInput{
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(), negative,
                                 tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), 0u}};
  check(!selectParallelPassCandidate(negativeInput).selected,
        "non-positive benefit falls back to serial");
  negative.valid = false;
  negativeInput[0].cost = negative;
  check(!selectParallelPassCandidate(negativeInput).selected,
        "invalid economics falls back to serial");
  negative = tiedFew.cost(400);
  negative.overflow = true;
  negativeInput[0].cost = negative;
  check(!selectParallelPassCandidate(negativeInput).selected,
        "overflow economics falls back to serial");
  auto malformed = tiedFew;
  malformed.snapshot.firstDraws[0].complete = false;
  std::array<ParallelPassCandidateInput, 1> malformedInput{
      ParallelPassCandidateInput{&malformed.snapshot, malformed.view(),
                                 malformed.cost(400),
                                 malformed.authorityResolver(),
                                 malformed.coverageResolver(), 0u}};
  check(!selectParallelPassCandidate(malformedInput).selected,
        "unsafe candidate never reaches economics ranking");

  auto negativeCost = tiedFew.cost(400);
  negativeCost.criticalPath.raw = -1;
  negativeInput[0].cost = negativeCost;
  check(!selectParallelPassCandidate(negativeInput).selected,
        "negative fixed-point costs select serial");
  auto negativeBenefitWithTail = tiedFew.cost(400);
  negativeBenefitWithTail.criticalPath.raw = 500;
  negativeBenefitWithTail.childSetup.raw = 1;
  negativeBenefitWithTail.imbalance.raw = 1;
  negativeInput[0].cost = negativeBenefitWithTail;
  const auto negativeBenefitSelection =
      selectParallelPassCandidate(negativeInput);
  check(!negativeBenefitSelection.selected &&
            negativeBenefitSelection.failure ==
                ParallelPassCandidateSelectionFailure::NonPositiveBenefit,
        "valid negative benefit with nonzero cost tail reaches NonPositiveBenefit");
  negativeCost = tiedFew.cost(400);
  negativeCost.serialWork.raw = ParallelPassFixedPoint::kMaxRaw + 1ll;
  negativeInput[0].cost = negativeCost;
  check(!selectParallelPassCandidate(negativeInput).selected,
        "out-of-domain fixed-point conversion selects serial");
  auto hugeEconomics = tiedFew.cost(400);
  hugeEconomics.economics.totalDraws = tiedFew.snapshot.drawCount;
  hugeEconomics.economics.minimumChildDraws = UINT32_MAX;
  hugeEconomics.economics.maximumChildDraws = UINT32_MAX;
  std::array<ParallelPassCandidateInput, 1> hugeInput{
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(), hugeEconomics,
                                 tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), 0u}};
  check(!selectParallelPassCandidate(hugeInput).selected,
        "high-domain economics and checked min/max products select serial");
  hugeEconomics.economics.totalDraws = UINT64_MAX;
  hugeInput[0].cost = hugeEconomics;
  check(!selectParallelPassCandidate(hugeInput).selected,
        "UINT64_MAX economics total selects serial before ranking");

  std::array<ParallelPassCandidateInput, 2> duplicateOrdinals{
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(),
                                 tiedFew.cost(400), tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), 0u},
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(),
                                 tiedFew.cost(400), tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), 0u}};
  check(!selectParallelPassCandidate(duplicateOrdinals).selected,
        "duplicate candidate ordinals select serial");
  duplicateOrdinals[1].candidateOrdinal = UINT32_MAX;
  check(!selectParallelPassCandidate(duplicateOrdinals).selected,
        "invalid candidate ordinal selects serial");

  SemanticPlanFixture lexLow(2u, 1u, 601u);
  SemanticPlanFixture lexHigh(2u, 9u, 609u);
  std::array<ParallelPassCandidateInput, 2> lexicographic{
      ParallelPassCandidateInput{&lexHigh.snapshot, lexHigh.view(),
                                 lexHigh.cost(400), lexHigh.authorityResolver(),
                                 lexHigh.coverageResolver(), 1u},
      ParallelPassCandidateInput{&lexLow.snapshot, lexLow.view(),
                                 lexLow.cost(400), lexLow.authorityResolver(),
                                 lexLow.coverageResolver(), 0u}};
  const auto lexExpected = selectParallelPassCandidate(lexicographic);
  check(lexExpected.selected && lexExpected.candidateOrdinal == 0u,
        "equal benefit and child count prefer canonical range vector");
  std::array<ParallelPassCandidateInput, 2> lexPermutation = lexicographic;
  do {
    const auto selectedPermutation =
        selectParallelPassCandidate(lexPermutation);
    check(selectedPermutation.selected &&
              selectedPermutation.candidateOrdinal == 0u,
          "lexicographic selection is invariant for every permutation");
  } while (std::next_permutation(lexPermutation.begin(),
                                 lexPermutation.end(),
                                 [](const auto& left, const auto& right) {
                                   return left.candidateOrdinal <
                                       right.candidateOrdinal;
                                 }));

  for (const auto metadataField : {0u, 1u, 2u, 3u, 4u}) {
    SemanticPlanFixture metadataBase(2u, 5u, 705u);
    SemanticPlanFixture metadataVariant(2u, 5u, 705u);
    auto& entry = metadataVariant.snapshot.ranges[0].entry;
    switch (metadataField) {
    case 0u:
      ++entry.drawRunRecordIndex;
      break;
    case 1u:
      ++entry.stateIndex;
      break;
    case 2u:
      entry.uniformHandle = {.index = 101u, .generation = 2u, .hash = 303u};
      break;
    case 3u:
      entry.bindingOverrideBytes = {.offset = 401u, .size = 1u};
      break;
    case 4u:
      entry.bindingSnapshotBytes = {.offset = 502u, .size = 1u};
      break;
    }
    metadataVariant.snapshot.firstDraws[0].provenance = entry;
    metadataVariant.children[0].range.entry = entry;
    metadataVariant.children[0].firstDraw.provenance = entry;
    std::array<ParallelPassCandidateInput, 2> metadataCandidates{
        ParallelPassCandidateInput{
            &metadataBase.snapshot, metadataBase.view(), metadataBase.cost(400),
            metadataBase.authorityResolver(), metadataBase.coverageResolver(),
            1u},
        ParallelPassCandidateInput{
            &metadataVariant.snapshot, metadataVariant.view(),
            metadataVariant.cost(400), metadataVariant.authorityResolver(),
            metadataVariant.coverageResolver(), 0u}};
    const auto metadataSelection =
        selectParallelPassCandidate(metadataCandidates);
    check(metadataSelection.selected && metadataSelection.candidateOrdinal == 1u,
          "same-benefit equal-child tie key includes every entry metadata field");
  }
}

struct ProducerCoverageContext {
  const PassObservationFixture* fixture = nullptr;
  std::uint32_t mutateNonzeroWholeRowField = 0u;
};

bool resolveProducerAuthority(
    const void* context, const dxmt9::core::CpuReadyTape::SourceRef& source,
    std::uint64_t seqId, std::uint32_t replayOrdinalBegin,
    std::uint32_t replayOrdinalEnd,
    dxmt9::encoders::SealedParallelPassSnapshot& authoritative) noexcept {
  const auto& state = *static_cast<const ProducerCoverageContext*>(context);
  if (!state.fixture || source != state.fixture->stream.source.source ||
      seqId != state.fixture->stream.source.seqId) {
    return false;
  }
  dxmt9::encoders::SealedParallelPassSnapshotBatch rebuilt{};
  if (!state.fixture->observe(rebuilt).considered) {
    return false;
  }
  for (const auto& candidate : rebuilt.view()) {
    if (candidate.replayOrdinalBegin == replayOrdinalBegin &&
        candidate.replayOrdinalEnd == replayOrdinalEnd) {
      authoritative = candidate;
      return true;
    }
  }
  return false;
}

bool resolveProducerCoverage(
    const void* context,
    const dxmt9::encoders::SealedParallelPassSnapshot& snapshot,
    const ParallelPassChildPlan& child,
    dxmt9::encoders::ParallelPassResolvedCoverage& coverage) noexcept {
  const auto& state = *static_cast<const ProducerCoverageContext*>(context);
  if (!state.fixture || snapshot.source != state.fixture->stream.source.source ||
      child.range.entry.source !=
          dxmt9::encoders::RetainedEncodeSourceLocator{
              .tapeSource = state.fixture->stream.source.source,
              .slotIndex = static_cast<std::uint32_t>(
                  state.fixture->stream.source.slotIndex),
              .seqId = state.fixture->stream.source.seqId} ||
      (!child.coversCompleteCommands && child.replayOrdinalCount != 1u)) {
    return false;
  }
  if (child.coversCompleteCommands) {
    coverage.reads.flags = dxmt9::core::ExactResourceSetComplete |
        dxmt9::core::ExactResourceSetCanonicalized;
    coverage.writes.flags = dxmt9::core::ExactResourceSetComplete |
        dxmt9::core::ExactResourceSetCanonicalized;
    bool haveAttachments = false;
    for (std::uint32_t offset = 0u; offset < child.replayOrdinalCount;
         ++offset) {
      std::uint32_t commandIndex = 0u;
      if (!state.fixture->stream.commandIndexAt(
              child.replayOrdinalBegin + offset, commandIndex)) {
        return false;
      }
      const auto command = state.fixture->stream.source.payload.commandAt(
          commandIndex);
      if (command.kind() != dxmt9::core::MetalCommandKind::DrawRun ||
          !command.command.drawState.hot || command.command.drawParams.empty()) {
        return false;
      }
      const auto attachments = dxmt9::core::makeRenderAttachmentKey(
          *command.command.drawState.hot);
      if (haveAttachments && attachments != coverage.attachments) {
        return false;
      }
      coverage.attachments = attachments;
      haveAttachments = true;
      const auto reads = dxmt9::core::makeDrawEntryReadSet(
          command.command.drawState);
      const auto writes = dxmt9::core::makeRenderAttachmentWriteSet(
          *command.command.drawState.hot);
      for (std::uint32_t i = 0u; i < reads.count; ++i) {
        if (!coverage.reads.add(reads.handles[i])) {
          return false;
        }
      }
      for (std::uint32_t i = 0u; i < writes.count; ++i) {
        if (!coverage.writes.add(writes.handles[i])) {
          return false;
        }
      }
      coverage.drawCount += command.command.drawParams.size();
      if (coverage.commandCount >= coverage.commands.size()) {
        return false;
      }
      coverage.commands[coverage.commandCount++] = {
          .replayOrdinal = child.replayOrdinalBegin + offset,
          .commandIndex = commandIndex,
          .drawParamBegin = command.command.drawRunRecord->firstParam,
          .drawParamCount = static_cast<std::uint32_t>(
              command.command.drawParams.size()),
      };
    }
    if (state.mutateNonzeroWholeRowField != 0u &&
        coverage.commandCount >= 2u) {
      auto& row = coverage.commands[1];
      switch (state.mutateNonzeroWholeRowField) {
      case 1u:
        row.commandIndex = coverage.commands[0].commandIndex;
        break;
      case 2u:
        ++row.drawParamBegin;
        break;
      case 3u:
        ++row.drawParamCount;
        break;
      }
    }
    coverage.route = dxmt9::core::RenderRoute::Portable;
    coverage.passActionEpoch = child.firstDraw.entryRender.passActionEpoch;
    return coverage.drawCount != 0u;
  }
  const auto resolved = dxmt9::encoders::resolveEncodePartition(
      child.range, state.fixture->stream);
  if (!resolved || !resolved.partition.entry.drawState.hot) {
    return false;
  }
  coverage.drawCount = resolved.partition.drawParams.size();
  const auto authoritativeCommand = state.fixture->stream.source.payload.commandAt(
      child.range.entry.commandIndex);
  if (authoritativeCommand.kind() != dxmt9::core::MetalCommandKind::DrawRun ||
      authoritativeCommand.command.drawParams.size() > UINT32_MAX) {
    return false;
  }
  coverage.commandCount = 1u;
  coverage.commands[0] = {
      .replayOrdinal = child.replayOrdinalBegin,
      .commandIndex = child.range.entry.commandIndex,
      .drawParamBegin = authoritativeCommand.command.drawRunRecord->firstParam,
      .drawParamCount = static_cast<std::uint32_t>(
          authoritativeCommand.command.drawParams.size()),
  };
  coverage.reads = dxmt9::core::makeDrawEntryReadSet(
      resolved.partition.entry.drawState);
  coverage.writes = dxmt9::core::makeRenderAttachmentWriteSet(
      *resolved.partition.entry.drawState.hot);
  coverage.reads.flags |= dxmt9::core::ExactResourceSetCanonicalized;
  coverage.writes.flags |= dxmt9::core::ExactResourceSetCanonicalized;
  coverage.attachments = dxmt9::core::makeRenderAttachmentKey(
      *resolved.partition.entry.drawState.hot);
  coverage.route = dxmt9::core::RenderRoute::Portable;
  coverage.passActionEpoch = child.firstDraw.entryRender.passActionEpoch;
  return true;
}

void producerOutputFeedsSynchronousSemanticValidator() {
  using namespace dxmt9::encoders;
  PassObservationFixture fixture;
  fixture.slot.appendDrawRun({}, {}, draws(128u), payloads(128u));
  fixture.slot.appendPresent({}, {});
  fixture.finalize(601u);
  SealedParallelPassSnapshotBatch output{};
  const auto produced = fixture.observe(output);
  check(produced.eligibleCount == 1u && output.count == 1u,
        "producer emits a candidate for synchronous validation");

  const auto& sourceSnapshot = output.passes[0];
  SealedParallelPassSnapshot snapshot = sourceSnapshot;
  std::array<ParallelPassChildPlan, kParallelRenderPassChildCapacity> children{};
  for (std::uint32_t i = 0u; i < snapshot.childCount; ++i) {
    children[i] = {
        .range = snapshot.ranges[i],
        .firstDraw = snapshot.firstDraws[i],
        .binding = bindingSnapshot(ParallelPassDirectBindingMode::Stage1Direct,
                                   static_cast<std::uint16_t>(i + 1u)),
        .replayOrdinalBegin = snapshot.childReplayOrdinalBegins[i],
        .replayOrdinalCount = snapshot.childReplayOrdinalCounts[i],
        .childOrdinal = i,
        .localShadowOrdinal = i + 1u,
        .coversCompleteCommands = snapshot.childrenCoverCompleteCommands,
        .forceFullFirstDrawBinding = true,
    };
  }
  ProducerCoverageContext context{.fixture = &fixture};
  const ParallelPassSnapshotAuthority authority{
      .context = &context, .resolve = resolveProducerAuthority};
  const ParallelPassCoverageResolver resolver{
      .context = &context, .resolve = resolveProducerCoverage};
  const auto validation = validateParallelPassSemanticPlan(
      snapshot, {children.data(), snapshot.childCount}, authority, resolver);
  check(validation.accepted(),
        "producer snapshot passes exact synchronous re-resolution");

  auto malformed = snapshot;
  auto malformedChildren = children;
  malformed.ranges[0].drawEntryCount--;
  malformed.firstDraws[0].provenance = malformed.ranges[0].entry;
  malformedChildren[0].range = malformed.ranges[0];
  malformedChildren[0].firstDraw.provenance = malformed.ranges[0].entry;
  check(!validateParallelPassSemanticPlan(
              malformed, {malformedChildren.data(), malformed.childCount}, authority,
              resolver)
              .accepted(),
        "producer partial tail is rejected by re-resolution");
  malformed = snapshot;
  malformedChildren = children;
  malformed.ranges[0].entry.drawParamIndex++;
  malformed.firstDraws[0].provenance = malformed.ranges[0].entry;
  malformedChildren[0].range = malformed.ranges[0];
  malformedChildren[0].firstDraw.provenance = malformed.ranges[0].entry;
  check(!validateParallelPassSemanticPlan(
              malformed, {malformedChildren.data(), malformed.childCount},
              authority, resolver)
              .accepted(),
        "producer shifted DrawRun range is rejected by owner authority");
  malformed = snapshot;
  malformed.source.storage.generation++;
  check(!validateParallelPassSemanticPlan(
              malformed, {children.data(), malformed.childCount}, authority,
              resolver)
              .accepted(),
        "producer stale storage generation is rejected by resolver identity");
  malformed = snapshot;
  malformed.sealedAtSourceEnd = false;
  malformed.sealingCommand = {
      .source = malformed.firstDraw.source,
      .replayOrdinal = malformed.replayOrdinalEnd,
      .commandIndex = malformed.firstDraw.commandIndex + 2u,
      .kind = dxmt9::core::MetalCommandKind::Present,
      .valid = true,
  };
  check(!validateParallelPassSemanticPlan(
              malformed, {children.data(), malformed.childCount}, authority,
              resolver)
              .accepted(),
        "producer stale sealing command is rejected by owner re-resolution");

  ParallelPassCandidateCost cost{};
  cost.economics = {
      .totalDraws = snapshot.drawCount,
      .stage1Draws = snapshot.drawCount,
      .psoBoundaryTransitions = snapshot.childCount - 1u,
      .uniformBoundaryTransitions = snapshot.childCount - 1u,
      .childCount = snapshot.childCount,
      .minimumChildDraws = 64u,
      .maximumChildDraws = 64u,
      .valid = true,
  };
  cost.serialWork.raw = 400ll * ParallelPassFixedPoint::kFraction;
  cost.valid = true;
  const std::array candidates{ParallelPassCandidateInput{
      &snapshot, {children.data(), snapshot.childCount}, cost, authority,
      resolver, 0u}};
  check(selectParallelPassCandidate(candidates).selected,
        "producer candidate ranks only after exact validation");

  PassObservationFixture whole;
  whole.slot.appendDrawRun({}, {}, draws(64u), payloads(64u));
  whole.slot.appendDrawRun({}, {}, draws(32u), payloads(32u));
  whole.slot.appendDrawRun({}, {}, draws(32u), payloads(32u));
  whole.slot.appendPresent({}, {});
  whole.finalize(602u);
  SealedParallelPassSnapshotBatch wholeOutput{};
  check(whole.observe(wholeOutput).eligibleCount == 1u &&
            wholeOutput.passes[0].childrenCoverCompleteCommands,
        "producer emits whole-command candidate for re-resolution");
  auto wholeSnapshot = wholeOutput.passes[0];
  std::array<ParallelPassChildPlan, kParallelRenderPassChildCapacity>
      wholeChildren{};
  for (std::uint32_t i = 0u; i < wholeSnapshot.childCount; ++i) {
    wholeChildren[i] = {
        .range = wholeSnapshot.ranges[i],
        .firstDraw = wholeSnapshot.firstDraws[i],
        .binding = bindingSnapshot(ParallelPassDirectBindingMode::Stage1Direct,
                                   static_cast<std::uint16_t>(i + 1u)),
        .replayOrdinalBegin = wholeSnapshot.childReplayOrdinalBegins[i],
        .replayOrdinalCount = wholeSnapshot.childReplayOrdinalCounts[i],
        .childOrdinal = i,
        .localShadowOrdinal = i + 1u,
        .coversCompleteCommands = true,
        .forceFullFirstDrawBinding = true,
    };
  }
  ProducerCoverageContext wholeContext{.fixture = &whole};
  const ParallelPassSnapshotAuthority wholeAuthority{
      .context = &wholeContext, .resolve = resolveProducerAuthority};
  const ParallelPassCoverageResolver wholeResolver{
      .context = &wholeContext, .resolve = resolveProducerCoverage};
  const auto wholeValidation = validateParallelPassSemanticPlan(
      wholeSnapshot, {wholeChildren.data(), wholeSnapshot.childCount},
      wholeAuthority, wholeResolver);
  check(wholeValidation.accepted(),
        "whole-command producer coverage re-resolves every DrawRun");
  check(wholeSnapshot.childCount >= 2u &&
            wholeSnapshot.childReplayOrdinalCounts[1] >= 2u,
        "whole-command fixture has a child containing multiple DrawRuns");
  for (std::uint32_t field = 1u; field <= 3u; ++field) {
    wholeContext.mutateNonzeroWholeRowField = field;
    check(!validateParallelPassSemanticPlan(
                wholeSnapshot,
                {wholeChildren.data(), wholeSnapshot.childCount},
                wholeAuthority, wholeResolver)
                .accepted(),
          "each nonzero whole-command row identity/range mutation fails closed");
  }
  wholeContext.mutateNonzeroWholeRowField = 0u;
  wholeChildren[1].replayOrdinalCount++;
  check(!validateParallelPassSemanticPlan(
              wholeSnapshot,
              {wholeChildren.data(), wholeSnapshot.childCount}, wholeAuthority,
              wholeResolver)
              .accepted(),
        "whole-command duplicate/partial replay span fails closed");
}

}  // namespace

int main() {
  try {
    eligibilityAndSelectionAreTypedAndBounded();
    coordinatorProofSnapshotRequiresEveryPreEffectFact();
    wmtParentChildAdapterCreatesAndJoinsMetalEncoders();
    explicitParallelSubdivisionIsEvenAndBounded();
    wholeCommandSubdivisionIsOrderedBoundedAndFailClosed();
    wholeCommandProducerPreservesCoverageAndFirstLocators();
    passLocalProducerFindsBoundedCompletePasses();
    multiPassAndAttachmentBoundariesStayIndependent();
    activeReplayOrderAndPartialClearDriveExactBoundaries();
    producerRejectsControlsFragmentsHazardsAndBounds();
    childBoundsAndPerfGateFailClosed();
    fakeChildrenPreserveOwnershipOrderingAndExactlyOnceReplay();
    malformedPlansFailClosedBeforeParentPreparation();
    failuresSeparatePreEffectFallbackFromPostEffectFailStop();
    economicsClassifierIsPureBoundedAndEnforcedBeforeEffects();
    semanticPlanMutationAndCoverageProofsFailClosed();
    fixedPointAndCandidateSelectionAreCheckedAndPermutationIndependent();
    producerOutputFeedsSynchronousSemanticValidator();
  } catch (const TestFailure& error) {
    std::cerr << "parallel_render_pass_spec failed: " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "parallel_render_pass_spec unexpected exception: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}
