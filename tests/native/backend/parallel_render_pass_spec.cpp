#include "../../../src/dxmt9/dxmt9_parallel_render_pass.hpp"
#include "../../../src/dxmt9/dxmt9_parallel_render_pass_metal.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
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
          completeProofs(),
      std::uint32_t drawQuantum =
          dxmt9::encoders::kProductionPartitionDrawThreshold) const {
    return dxmt9::encoders::produceSealedParallelPassSnapshots({
          .stream = &stream,
          .ranges = plan.view(),
          .proofs = proofs,
          .planValidated = true,
          .sourceStartsPass = sourceStartsPass,
          .sourceEndsPass = sourceEndsPass,
          .drawQuantum = drawQuantum,
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

void parallelPassDrawQuantumEnvKnobResolvesClampsAndScalesEligibility() {
  using dxmt9::encoders::classifyParallelPassEconomics;
  using dxmt9::encoders::kProductionPartitionDrawThreshold;
  using dxmt9::encoders::ParallelPassEconomicsSummary;
  using dxmt9::encoders::resolveParallelPassDrawQuantum;
  using dxmt9::encoders::subdivideParallelPassDraws;

  // (a) env parsing/clamping table.
  const auto expect = [](const char* env, std::uint32_t quantum,
                         bool clamped, std::string_view message) {
    const auto resolved = resolveParallelPassDrawQuantum(env);
    check(resolved.quantum == quantum && resolved.clamped == clamped,
          message);
  };
  expect(nullptr, kProductionPartitionDrawThreshold, false,
        "unset env resolves to the production default");
  expect("", kProductionPartitionDrawThreshold, false,
        "empty env resolves to the production default");
  expect("0", kProductionPartitionDrawThreshold, false,
        "zero env resolves to the production default");
  expect("not-a-number", kProductionPartitionDrawThreshold, false,
        "unparseable env resolves to the production default");
  expect("4", 4u, false, "4 is the minimum and needs no clamping");
  expect("2", 4u, true, "2 clamps up to the minimum of 4");
  expect("1024", 1024u, false, "1024 is the maximum and needs no clamping");
  expect("5000", 1024u, true, "5000 clamps down to the maximum of 1024");

  // (b) a pass shape ineligible at quantum 64 becomes eligible at quantum 16
  // with correctly scaled child sizes.
  check(!subdivideParallelPassDraws(40u).valid,
        "40 draws is ineligible at the default quantum of 64");
  const auto scaled = subdivideParallelPassDraws(40u, 16u);
  check(scaled.valid && scaled.childCount == 2u &&
            scaled.drawCounts[0] == 20u && scaled.drawCounts[1] == 20u &&
            scaled.drawBegins[0] == 0u && scaled.drawBegins[1] == 20u,
        "40 draws becomes eligible at quantum 16 with two 20-draw children");

  // The full producer pipeline (PassObservationFixture::finalize) requires a
  // serial-planner-eligible DrawRun (>=64 draws, an independent axis owned
  // by dxmt9_encode_partition.hpp/.cpp and never touched by this knob), so
  // this end-to-end check uses 96 draws: ineligible for the parallel builder
  // at the default quantum of 64 (96 < 128), eligible at quantum 16 with six
  // 16-draw children (96 / 16 == 6, evenly).
  check(!subdivideParallelPassDraws(96u).valid,
        "96 draws is ineligible for the parallel builder at quantum 64");
  const auto scaled96 = subdivideParallelPassDraws(96u, 16u);
  check(scaled96.valid && scaled96.childCount == 6u,
        "96 draws splits into six children at quantum 16");
  for (std::uint32_t child = 0u; child < scaled96.childCount; ++child) {
    check(scaled96.drawCounts[child] == 16u,
          "96 draws splits evenly into six 16-draw children");
  }

  const auto below = draws(96u);
  const auto belowPayloads = payloads(below.size());
  PassObservationFixture fixture;
  fixture.slot.appendDrawRun({}, {}, below, belowPayloads);
  fixture.slot.appendPresent({}, {});
  fixture.finalize(4170u);
  dxmt9::encoders::SealedParallelPassSnapshotBatch defaultOutput{};
  const auto defaultResult = fixture.observe(defaultOutput);
  check(defaultResult.eligibleCount == 0u && defaultOutput.count == 0u,
        "96-draw pass stays on the serial fallback at the default quantum");
  dxmt9::encoders::SealedParallelPassSnapshotBatch scaledOutput{};
  const auto scaledResult =
      fixture.observe(scaledOutput, true, true, completeProofs(), 16u);
  check(scaledResult.eligibleCount == 1u && scaledOutput.count == 1u &&
            scaledOutput.passes[0].childCount == 6u,
        "96-draw pass becomes eligible with six children at quantum 16");
  for (std::uint32_t child = 0u; child < scaledOutput.passes[0].childCount;
       ++child) {
    check(scaledOutput.passes[0].childDrawCounts[child] == 16u,
          "each scaled child carries exactly 16 draws");
  }

  // (c) default-quantum behavior stays byte-identical to the explicit
  // kProductionPartitionDrawThreshold argument.
  check(subdivideParallelPassDraws(128u) ==
            subdivideParallelPassDraws(
                128u, kProductionPartitionDrawThreshold),
        "omitting the quantum argument matches the explicit default");
  const ParallelPassEconomicsSummary summary{
      .totalDraws = 128u,
      .stage1Draws = 128u,
      .psoBoundaryTransitions = 1u,
      .uniformBoundaryTransitions = 1u,
      .childCount = 2u,
      .minimumChildDraws = 64u,
      .maximumChildDraws = 64u,
      .valid = true,
  };
  check(classifyParallelPassEconomics(summary).accepted ==
            classifyParallelPassEconomics(
                summary, kProductionPartitionDrawThreshold).accepted &&
            classifyParallelPassEconomics(summary).reject ==
                classifyParallelPassEconomics(
                    summary, kProductionPartitionDrawThreshold).reject,
        "economics classifier default quantum matches the explicit "
        "production threshold");
}

void sealedPassDrawBucketHistogramConservesAgainstSealedCount() {
  using Bucket = dxmt9::encoders::SealedPassDrawBucket;
  using dxmt9::encoders::classifySealedPassDrawBucket;

  check(classifySealedPassDrawBucket(0u) == Bucket::Under8 &&
            classifySealedPassDrawBucket(7u) == Bucket::Under8 &&
            classifySealedPassDrawBucket(8u) == Bucket::From8To15 &&
            classifySealedPassDrawBucket(15u) == Bucket::From8To15 &&
            classifySealedPassDrawBucket(16u) == Bucket::From16To31 &&
            classifySealedPassDrawBucket(31u) == Bucket::From16To31 &&
            classifySealedPassDrawBucket(32u) == Bucket::From32To63 &&
            classifySealedPassDrawBucket(63u) == Bucket::From32To63 &&
            classifySealedPassDrawBucket(64u) == Bucket::From64To127 &&
            classifySealedPassDrawBucket(127u) == Bucket::From64To127 &&
            classifySealedPassDrawBucket(128u) == Bucket::From128To255 &&
            classifySealedPassDrawBucket(255u) == Bucket::From128To255 &&
            classifySealedPassDrawBucket(256u) == Bucket::From256Plus &&
            classifySealedPassDrawBucket(1000000u) == Bucket::From256Plus,
        "bucket boundaries classify exactly as documented");

  // Build a source with several sealed-but-rejected passes of different draw
  // sizes (each < 128, so each stays on the serial fallback at the default
  // quantum) and confirm the histogram sum equals sealedCount regardless of
  // eligibility.
  const std::array<std::size_t, 4> sizes{5u, 10u, 20u, 100u};
  PassObservationFixture fixture;
  for (const auto size : sizes) {
    fixture.slot.appendClear({});
    const auto rows = draws(size);
    const auto rowPayloads = payloads(rows.size());
    fixture.slot.appendDrawRun({}, {}, rows, rowPayloads);
  }
  fixture.slot.appendPresent({}, {});
  fixture.finalize(4171u);
  dxmt9::encoders::SealedParallelPassSnapshotBatch output{};
  const auto result = fixture.observe(output);
  check(result.sealedCount == sizes.size(),
        "every drawrun-then-clear interval seals independently");
  std::uint32_t bucketSum = 0u;
  for (const auto count : result.sealedDrawBuckets) {
    bucketSum += count;
  }
  check(bucketSum == result.sealedCount,
        "sealed draw-size bucket sum equals sealedCount");
  check(result.sealedDrawBuckets[static_cast<std::size_t>(
            Bucket::From8To15)] == 1u &&
            result.sealedDrawBuckets[static_cast<std::size_t>(
                Bucket::From16To31)] == 1u &&
            result.sealedDrawBuckets[static_cast<std::size_t>(
                Bucket::Under8)] == 1u &&
            result.sealedDrawBuckets[static_cast<std::size_t>(
                Bucket::From64To127)] == 1u,
        "each fixture pass lands in its expected bucket");
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
  check(result.eligibleCount == 1u && result.candidateCount == 1u &&
            result.sealedCount == 1u && result.coordinatorBoundaries == 1u &&
            result.coordinatorSplits == 0u && output.count == 1u &&
            output.passes[0].sealingCommand.valid &&
            output.passes[0].sealingCommand.kind ==
                dxmt9::core::MetalCommandKind::Readback &&
            output.passes[0].replayOrdinalEnd == 1u,
        "a readback terminates the pass interval at its serial position "
        "instead of rejecting the whole source");

  // A zero coordinator seed epoch is the pass-action epoch's invalid sentinel,
  // and the certificate's epoch witness refuses it outright. The producer must
  // therefore fail the whole source closed with the typed epoch reason rather
  // than publish later passes whose nonzero stamps no re-derivation can
  // reproduce (that shape was 100% of the SFIV certificate-invalid population).
  auto zeroSeed = completeProofs(0u);
  result = fragment.observe(output, true, true, zeroSeed);
  check(result.considered && result.candidateCount == 0u &&
            result.sealedCount == 0u && result.eligibleCount == 0u &&
            output.count == 0u &&
            result.fallback ==
                dxmt9::encoders::SealedParallelPassSnapshotFallback::PassActionEpoch &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::PassActionEpoch)] == 1u,
        "a zero coordinator seed epoch fails the whole source closed before "
        "any candidate is observed");

  auto productionLike = completeProofs(17u);
  productionLike.coordinator.flags = 0u;
  result = fragment.observe(output, true, true, productionLike);
  check(result.candidateCount == 1u && result.sealedCount == 1u &&
            result.eligibleCount == 0u && output.count == 0u &&
            result.rejectionCounts[static_cast<std::size_t>(
                dxmt9::encoders::SealedParallelPassSnapshotFallback::PassActionEpoch)] == 0u &&
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

// DXMT9_PARALLEL_PASS_IMBALANCE_BOUND: pins (a) the resolver's
// coupled-by-default / clamped-when-explicit table across multiple fallback
// quantums, (b) that decoupling the bound changes ONLY the UnbalancedChild
// decision (an imbalance rejected at the eligibility quantum is accepted
// once an explicit wider bound is supplied, with every other decision field
// identical), and (c) that the default (no-argument-3) call to
// classifyParallelPassEconomics stays byte-identical to explicitly passing
// the coupled sentinel.
void parallelPassImbalanceBoundDecouplesFromEligibilityQuantum() {
  using Decision = dxmt9::encoders::ParallelPassEconomicsDecision;
  using Reason = dxmt9::encoders::ParallelPassEconomicsRejectReason;
  using Summary = dxmt9::encoders::ParallelPassEconomicsSummary;
  using dxmt9::encoders::classifyParallelPassEconomics;
  using dxmt9::encoders::kParallelPassImbalanceBoundMax;
  using dxmt9::encoders::kParallelPassImbalanceBoundMin;
  using dxmt9::encoders::kProductionPartitionDrawThreshold;
  using dxmt9::encoders::resolveParallelPassImbalanceBound;

  // (a) resolver table — unset/0/garbage couple to the caller's fallback
  // quantum for at least two different quantum values; explicit values
  // clamp into [kParallelPassImbalanceBoundMin, kParallelPassImbalanceBoundMax].
  for (const std::uint32_t fallback : {32u, 64u, 128u}) {
    const auto expect = [&](const char* env, std::uint32_t bound,
                            bool clamped, std::string_view message) {
      const auto resolved = resolveParallelPassImbalanceBound(env, fallback);
      check(resolved.bound == bound && resolved.clamped == clamped, message);
    };
    expect(nullptr, fallback, false,
          "unset env couples to the caller's fallback quantum");
    expect("", fallback, false,
          "empty env couples to the caller's fallback quantum");
    expect("0", fallback, false,
          "zero env couples to the caller's fallback quantum");
    expect("not-a-number", fallback, false,
          "unparseable env couples to the caller's fallback quantum");
  }
  check(resolveParallelPassImbalanceBound("4", 64u).bound == 4u &&
            !resolveParallelPassImbalanceBound("4", 64u).clamped,
        "in-range explicit value 4 (the minimum) is preserved unclamped");
  const auto clampedLow = resolveParallelPassImbalanceBound("2", 64u);
  check(clampedLow.bound == kParallelPassImbalanceBoundMin &&
            clampedLow.clamped,
        "explicit value 2 clamps up to the minimum bound of 4");
  const auto atMax = resolveParallelPassImbalanceBound("4096", 64u);
  check(atMax.bound == 4096u && !atMax.clamped,
        "in-range explicit value 4096 (the maximum) is preserved unclamped");
  const auto clampedHigh = resolveParallelPassImbalanceBound("9999", 64u);
  check(clampedHigh.bound == kParallelPassImbalanceBoundMax &&
            clampedHigh.clamped,
        "explicit value 9999 clamps down to the maximum bound of 4096");

  // (b) economics decoupling. Reuse the existing 64/192-child shape (128-draw
  // imbalance, total 256, quantum 64) that
  // economicsClassifierIsPureBoundedAndEnforcedBeforeEffects pins as
  // UnbalancedChild at the coupled default: it must still reject
  // UnbalancedChild when the eligibility quantum (64) is used as the bound
  // (today's coupled behavior, sentinel imbalanceBound=0), and accept once
  // an explicit wider bound (128) is supplied, with every other decision
  // field identical.
  const Summary shape{
      .totalDraws = 256u,
      .stage1Draws = 256u,
      .stage2bDraws = 0u,
      .forcedStage1Draws = 0u,
      .psoBoundaryTransitions = 1u,
      .uniformBoundaryTransitions = 1u,
      .childCount = 2u,
      .minimumChildDraws = 64u,
      .maximumChildDraws = 192u,
      .valid = true,
  };
  const auto coupled =
      classifyParallelPassEconomics(shape, kProductionPartitionDrawThreshold);
  check(coupled.reject == Reason::UnbalancedChild && !coupled.accepted,
        "the 128-draw imbalance rejects UnbalancedChild at the coupled "
        "default (sentinel imbalanceBound=0)");
  const auto decoupledNarrow = classifyParallelPassEconomics(
      shape, kProductionPartitionDrawThreshold, 64u);
  check(decoupledNarrow.reject == Reason::UnbalancedChild &&
            !decoupledNarrow.accepted,
        "an explicit imbalance bound equal to the quantum (64) still "
        "rejects UnbalancedChild");
  const auto decoupledWide = classifyParallelPassEconomics(
      shape, kProductionPartitionDrawThreshold, 128u);
  check(decoupledWide.accepted && decoupledWide.reject == Reason::None,
        "the same 128-draw imbalance is accepted once the explicit bound "
        "widens to 128, independent of the 64-draw eligibility quantum");

  // (c) default-coupling byte identity — the two-argument call (drawQuantum
  // only) is byte-identical to explicitly passing the coupling sentinel
  // (imbalanceBound=0) as the third argument.
  const auto twoArgument =
      classifyParallelPassEconomics(shape, kProductionPartitionDrawThreshold);
  const auto explicitSentinel = classifyParallelPassEconomics(
      shape, kProductionPartitionDrawThreshold, 0u);
  check(twoArgument == explicitSentinel,
        "the two-argument call is byte-identical to an explicit "
        "imbalanceBound=0 coupling sentinel");

  // dispatchParallelPassEconomics/observeParallelPassEconomicsCountersIfEnabled
  // thread the same argument and reach the same decision as the direct
  // classifier call.
  std::uint32_t parallelEffects = 0u;
  const auto dispatched = dxmt9::encoders::dispatchParallelPassEconomics(
      shape, [&] { ++parallelEffects; }, [] {},
      kProductionPartitionDrawThreshold, 128u);
  check(dispatched.accepted && parallelEffects == 1u,
        "dispatchParallelPassEconomics threads the explicit imbalance bound "
        "and reaches acceptance");
  std::uint32_t observations = 0u;
  check(dxmt9::encoders::observeParallelPassEconomicsCountersIfEnabled(
            true, shape,
            [&](const Summary&, const Decision& decision) {
              ++observations;
              check(decision.accepted,
                    "observer sees the same widened-bound acceptance");
            },
            kProductionPartitionDrawThreshold, 128u) &&
            observations == 1u,
        "observeParallelPassEconomicsCountersIfEnabled threads the explicit "
        "imbalance bound");
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
    const std::uint32_t rows = fixture.resolvedCommandCountOverride != 0u
        ? fixture.resolvedCommandCountOverride
        : 1u;
    coverage.commands.open(child.replayOrdinalBegin,
                           fixture.snapshot.childrenCoverCompleteCommands);
    for (std::uint32_t row = 0u; row < rows; ++row) {
      if (!coverage.commands.append({
              .replayOrdinal = child.replayOrdinalBegin + row,
              .commandIndex = child.range.entry.commandIndex + row,
              .drawParamBegin =
                  static_cast<std::uint32_t>(fixture.snapshot.drawCount) * row,
              .drawParamCount =
                  static_cast<std::uint32_t>(fixture.snapshot.drawCount),
          })) {
        return false;
      }
    }
    coverage.reads = fixture.resolvedReads;
    coverage.writes = fixture.resolvedWrites;
    coverage.attachments = fixture.snapshot.attachments;
    coverage.route = dxmt9::core::RenderRoute::Portable;
    coverage.passActionEpoch = fixture.snapshot.passActionEpoch;
    return true;
  }

  // A small programmable epoch stream. The default is the single DrawRun the
  // rest of the fixture describes, seeded so the derived epoch equals the
  // snapshot's own. Tests that need a later pass script the boundaries ahead
  // of it and retarget the interval.
  struct EpochStreamEntry {
    dxmt9::core::MetalCommandKind kind =
        dxmt9::core::MetalCommandKind::DrawRun;
    bool clearRectsEmpty = false;
    bool foreignAttachments = false;
  };
  std::array<EpochStreamEntry, 8> epochStream{};
  std::uint32_t epochStreamCount = 1u;
  std::uint64_t epochSeed = 7u;
  mutable std::uint32_t epochReads = 0u;

  static bool readEpochFact(
      const void* context, const dxmt9::core::CpuReadyTape::SourceRef& source,
      std::uint64_t seqId, std::uint32_t replayOrdinal,
      dxmt9::encoders::ParallelPassActionEpochFact& fact) noexcept {
    const auto& fixture = *static_cast<const SemanticPlanFixture*>(context);
    ++fixture.epochReads;
    if (source != fixture.expectedSource || seqId != fixture.snapshot.seqId ||
        replayOrdinal >= fixture.epochStreamCount) {
      return false;
    }
    const auto& entry = fixture.epochStream[replayOrdinal];
    fact = {};
    fact.kind = entry.kind;
    fact.clearRectsEmpty = entry.clearRectsEmpty;
    fact.attachments = fixture.snapshot.attachments;
    if (entry.foreignAttachments) {
      fact.attachments.sampleCount =
          static_cast<decltype(fact.attachments.sampleCount)>(
              fixture.snapshot.attachments.sampleCount + 3u);
    }
    return true;
  }

  dxmt9::encoders::ParallelPassActionEpochWitness epochWitness()
      const noexcept {
    return {.context = this,
            .read = readEpochFact,
            .seedEpoch = epochSeed,
            .replayOrdinalCount = epochStreamCount};
  }

  // Move the whole sealed interval to `begin`, keeping the single-command
  // draw-subrange shape the fixture is built around.
  void retargetPassInterval(std::uint32_t begin) noexcept {
    snapshot.replayOrdinalBegin = begin;
    snapshot.replayOrdinalEnd = begin + 1u;
    snapshot.firstDraw.replayOrdinal = begin;
    for (std::uint32_t i = 0u; i < count; ++i) {
      snapshot.ranges[i].replayOrdinalBegin = begin;
      snapshot.childReplayOrdinalBegins[i] = begin;
      children[i].range.replayOrdinalBegin = begin;
      children[i].replayOrdinalBegin = begin;
    }
  }

  void setPassActionEpoch(std::uint64_t epoch) noexcept {
    snapshot.passActionEpoch = epoch;
    snapshot.coordinatorProof.firstPassActionEpoch = epoch;
    for (std::uint32_t i = 0u; i < count; ++i) {
      snapshot.firstDraws[i].entryRender.passActionEpoch = epoch;
      children[i].firstDraw.entryRender.passActionEpoch = epoch;
    }
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
      fixture.coverageResolver(), fixture.epochWitness());
  check(valid.accepted() && fixture.authorityCalls == 1u &&
            fixture.resolverCalls == fixture.count,
        "coherent sealed snapshot produces one owner lookup and one exact resolver call per child");

  auto expectInvalid = [&](auto mutate, std::string_view message) {
    SemanticPlanFixture candidate = fixture;
    mutate(candidate);
    check(!validateParallelPassSemanticPlan(
                candidate.snapshot, candidate.view(),
                candidate.authorityResolver(), candidate.coverageResolver(),
                candidate.epochWitness())
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
  // Row storage no longer bounds a child, so an oversized resolved command
  // count is now a claim mismatch against the child plan rather than a
  // capacity failure. It must still fail closed.
  expectInvalid([](auto& f) { f.resolvedCommandCountOverride = 17u; },
                "resolved command count above the child's claim fails closed");
  expectInvalid([](auto& f) { f.resolvedCommandCountOverride = 2u; },
                "an extra resolved command in a draw-subrange child fails "
                "closed");

  SemanticPlanFixture overlap(3u);
  overlap.snapshot.attachmentWrites.handles[0] = 0x11u;
  overlap.resolvedWrites.handles[0] = 0x11u;
  check(!validateParallelPassSemanticPlan(
              overlap.snapshot, overlap.view(), overlap.authorityResolver(),
              overlap.coverageResolver(), overlap.epochWitness())
              .accepted(),
        "exact read/write overlap fails closed");

  for (std::uint32_t count = 2u;
       count <= kParallelRenderPassChildCapacity; ++count) {
    SemanticPlanFixture bounded(count);
    check(validateParallelPassSemanticPlan(
              bounded.snapshot, bounded.view(), bounded.authorityResolver(),
              bounded.coverageResolver(), bounded.epochWitness()).accepted(),
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
                                 high.coverageResolver(), high.epochWitness(), 2u},
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(),
                                 tiedFew.cost(400), tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), tiedFew.epochWitness(), 1u},
      ParallelPassCandidateInput{&low.snapshot, low.view(), low.cost(300),
                                 low.authorityResolver(), low.coverageResolver(),
                                 low.epochWitness(), 0u},
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
                                 high.coverageResolver(), high.epochWitness(), 2u},
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(),
                                 tiedFew.cost(400), tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), tiedFew.epochWitness(), 1u},
      ParallelPassCandidateInput{&low.snapshot, low.view(), low.cost(300),
                                 low.authorityResolver(), low.coverageResolver(),
                                 low.epochWitness(), 0u}};
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
                                 negativeEvidence.coverageResolver(), negativeEvidence.epochWitness(), 0u}};
  check(!selectParallelPassCandidate(negativeEvidenceInput).selected,
        "adding coordinator negative evidence cannot introduce selection");

  auto negative = tiedFew.cost(0);
  std::array<ParallelPassCandidateInput, 1> negativeInput{
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(), negative,
                                 tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), tiedFew.epochWitness(), 0u}};
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
                                 malformed.coverageResolver(), malformed.epochWitness(), 0u}};
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
                                 tiedFew.coverageResolver(), tiedFew.epochWitness(), 0u}};
  check(!selectParallelPassCandidate(hugeInput).selected,
        "high-domain economics and checked min/max products select serial");
  hugeEconomics.economics.totalDraws = UINT64_MAX;
  hugeInput[0].cost = hugeEconomics;
  check(!selectParallelPassCandidate(hugeInput).selected,
        "UINT64_MAX economics total selects serial before ranking");

  std::array<ParallelPassCandidateInput, 2> duplicateOrdinals{
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(),
                                 tiedFew.cost(400), tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), tiedFew.epochWitness(), 0u},
      ParallelPassCandidateInput{&tiedFew.snapshot, tiedFew.view(),
                                 tiedFew.cost(400), tiedFew.authorityResolver(),
                                 tiedFew.coverageResolver(), tiedFew.epochWitness(), 0u}};
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
                                 lexHigh.coverageResolver(), lexHigh.epochWitness(), 1u},
      ParallelPassCandidateInput{&lexLow.snapshot, lexLow.view(),
                                 lexLow.cost(400), lexLow.authorityResolver(),
                                 lexLow.coverageResolver(), lexLow.epochWitness(), 0u}};
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
            metadataBase.epochWitness(), 1u},
        ParallelPassCandidateInput{
            &metadataVariant.snapshot, metadataVariant.view(),
            metadataVariant.cost(400), metadataVariant.authorityResolver(),
            metadataVariant.coverageResolver(), metadataVariant.epochWitness(), 0u}};
    const auto metadataSelection =
        selectParallelPassCandidate(metadataCandidates);
    check(metadataSelection.selected && metadataSelection.candidateOrdinal == 1u,
          "same-benefit equal-child tie key includes every entry metadata field");
  }
}

struct ProducerCoverageContext {
  const PassObservationFixture* fixture = nullptr;
  std::uint32_t mutateNonzeroWholeRowField = 0u;
  std::uint64_t seedEpoch = 7u;
  // The owner must re-observe with the same boundary facts the coordinator
  // published, or a carried source would be re-derived as a fresh one.
  bool sourceStartsPass = true;
  bool sourceEndsPass = true;
  mutable std::uint32_t epochReads = 0u;
};

// The producer fixture folds the same stream the producer itself walked, so
// the certificate's re-derivation is exercised against real replay order.
bool resolveProducerEpochFact(
    const void* context, const dxmt9::core::CpuReadyTape::SourceRef& source,
    std::uint64_t seqId, std::uint32_t replayOrdinal,
    dxmt9::encoders::ParallelPassActionEpochFact& fact) noexcept {
  const auto& state = *static_cast<const ProducerCoverageContext*>(context);
  ++state.epochReads;
  if (!state.fixture || source != state.fixture->stream.source.source ||
      seqId != state.fixture->stream.source.seqId) {
    return false;
  }
  return dxmt9::encoders::readParallelPassActionEpochFact(
      state.fixture->stream, replayOrdinal, fact);
}

dxmt9::encoders::ParallelPassActionEpochWitness producerEpochWitness(
    const ProducerCoverageContext& context) noexcept {
  return {.context = &context,
          .read = resolveProducerEpochFact,
          .seedEpoch = context.seedEpoch,
          .replayOrdinalCount = static_cast<std::uint32_t>(
              context.fixture->stream.replayOrdinalCount())};
}

// An owner that echoes exactly the snapshot it was handed. It removes the
// snapshot authority as a source of rejection so a test can show which
// remaining check catches a self-consistent forgery.
bool resolveEchoingAuthority(
    const void* context, const dxmt9::core::CpuReadyTape::SourceRef& source,
    std::uint64_t seqId, std::uint32_t replayOrdinalBegin,
    std::uint32_t replayOrdinalEnd,
    dxmt9::encoders::SealedParallelPassSnapshot& authoritative) noexcept {
  const auto& snapshot =
      *static_cast<const dxmt9::encoders::SealedParallelPassSnapshot*>(context);
  if (source != snapshot.source || seqId != snapshot.seqId ||
      replayOrdinalBegin != snapshot.replayOrdinalBegin ||
      replayOrdinalEnd != snapshot.replayOrdinalEnd) {
    return false;
  }
  authoritative = snapshot;
  return true;
}

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
  if (!state.fixture
           ->observe(rebuilt, state.sourceStartsPass, state.sourceEndsPass,
                     completeProofs(state.seedEpoch))
           .considered) {
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
    coverage.commands.open(child.replayOrdinalBegin, true);
    std::uint32_t firstCommandIndex = 0u;
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
      dxmt9::encoders::ParallelPassResolvedCoverage::Command row{
          .replayOrdinal = child.replayOrdinalBegin + offset,
          .commandIndex = commandIndex,
          .drawParamBegin = command.command.drawRunRecord->firstParam,
          .drawParamCount = static_cast<std::uint32_t>(
              command.command.drawParams.size()),
      };
      if (offset == 0u) {
        firstCommandIndex = commandIndex;
      } else if (offset == 1u) {
        switch (state.mutateNonzeroWholeRowField) {
        case 1u:
          row.commandIndex = firstCommandIndex;
          break;
        case 2u:
          ++row.drawParamBegin;
          break;
        case 3u:
          ++row.drawParamCount;
          break;
        default:
          break;
        }
      }
      if (!coverage.commands.append(row)) {
        return false;
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
  coverage.commands.open(child.replayOrdinalBegin, false);
  if (!coverage.commands.append({
          .replayOrdinal = child.replayOrdinalBegin,
          .commandIndex = child.range.entry.commandIndex,
          .drawParamBegin =
              authoritativeCommand.command.drawRunRecord->firstParam,
          .drawParamCount = static_cast<std::uint32_t>(
              authoritativeCommand.command.drawParams.size()),
      })) {
    return false;
  }
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
  const auto epochWitness = producerEpochWitness(context);
  const auto validation = validateParallelPassSemanticPlan(
      snapshot, {children.data(), snapshot.childCount}, authority, resolver,
      epochWitness);
  check(validation.accepted(),
        "producer snapshot passes exact synchronous re-resolution");

  auto malformed = snapshot;
  auto malformedChildren = children;
  malformed.ranges[0].drawEntryCount--;
  malformed.firstDraws[0].provenance = malformed.ranges[0].entry;
  malformedChildren[0].range = malformed.ranges[0];
  malformedChildren[0].firstDraw.provenance = malformed.ranges[0].entry;
  check(!validateParallelPassSemanticPlan(
              malformed, {malformedChildren.data(), malformed.childCount},
              authority, resolver, epochWitness)
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
              authority, resolver, epochWitness)
              .accepted(),
        "producer shifted DrawRun range is rejected by owner authority");
  malformed = snapshot;
  malformed.source.storage.generation++;
  check(!validateParallelPassSemanticPlan(
              malformed, {children.data(), malformed.childCount}, authority,
              resolver, epochWitness)
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
              resolver, epochWitness)
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
      resolver, epochWitness, 0u}};
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
  const auto wholeEpochWitness = producerEpochWitness(wholeContext);
  const auto wholeValidation = validateParallelPassSemanticPlan(
      wholeSnapshot, {wholeChildren.data(), wholeSnapshot.childCount},
      wholeAuthority, wholeResolver, wholeEpochWitness);
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
                wholeAuthority, wholeResolver, wholeEpochWitness)
                .accepted(),
          "each nonzero whole-command row identity/range mutation fails closed");
  }
  wholeContext.mutateNonzeroWholeRowField = 0u;
  wholeChildren[1].replayOrdinalCount++;
  check(!validateParallelPassSemanticPlan(
              wholeSnapshot,
              {wholeChildren.data(), wholeSnapshot.childCount}, wholeAuthority,
              wholeResolver, wholeEpochWitness)
              .accepted(),
        "whole-command duplicate/partial replay span fails closed");
}

// R-BACK-2.70: a non-child coordinator command stays at its serial position and
// segments the source into pass intervals. It never becomes a child range, it
// never removes an unrelated interval's eligibility, and it fails a pass closed
// whenever the same attachment would otherwise have continued across it.
void coordinatorCommandsSegmentPassIntervals() {
  using Kind = dxmt9::core::MetalCommandKind;
  using Fallback = dxmt9::encoders::SealedParallelPassSnapshotFallback;
  using Role = dxmt9::encoders::ParallelPassCommandRole;
  const auto drawValues = draws(128u);
  const auto drawPayloads = payloads(drawValues.size());
  dxmt9::encoders::SealedParallelPassSnapshotBatch output{};

  check(dxmt9::encoders::classifyParallelPassCommandRole(Kind::DrawRun) ==
                Role::Draw &&
            dxmt9::encoders::classifyParallelPassCommandRole(Kind::Clear) ==
                Role::ClearBoundary &&
            dxmt9::encoders::classifyParallelPassCommandRole(Kind::Present) ==
                Role::PresentBoundary &&
            dxmt9::encoders::classifyParallelPassCommandRole(
                Kind::StretchRect) == Role::CoordinatorBoundary &&
            dxmt9::encoders::classifyParallelPassCommandRole(
                Kind::SurfaceCopy) == Role::CoordinatorBoundary &&
            dxmt9::encoders::classifyParallelPassCommandRole(Kind::Readback) ==
                Role::CoordinatorBoundary &&
            dxmt9::encoders::classifyParallelPassCommandRole(Kind::ColorFill) ==
                Role::CoordinatorBoundary &&
            dxmt9::encoders::classifyParallelPassCommandRole(
                Kind::DepthResolve) == Role::CoordinatorBoundary,
        "every source command kind has one total pass-interval role");
  check(!dxmt9::encoders::parallelPassSealingKindAccepted(Kind::DrawRun) &&
            dxmt9::encoders::parallelPassSealingKindAccepted(Kind::Clear) &&
            dxmt9::encoders::parallelPassSealingKindAccepted(Kind::Present) &&
            dxmt9::encoders::parallelPassSealingKindAccepted(
                Kind::StretchRect),
        "only coordinator-owned commands may seal a pass; a draw may not");

  // SFIV shape: Clear -> DrawRuns -> StretchRect -> Present. The StretchRect
  // used to reject the whole source before candidates, sealing, or economics.
  PassObservationFixture sfiv;
  sfiv.slot.appendClear({});
  sfiv.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  sfiv.slot.appendStretchRect({});
  sfiv.slot.appendPresent({}, {});
  sfiv.finalize(430u);
  auto result = sfiv.observe(output);
  check(result.candidateCount == 1u && result.sealedCount == 1u &&
            result.eligibleCount == 1u && result.childCount == 2u &&
            result.drawCount == 128u && result.coordinatorBoundaries == 1u &&
            result.coordinatorSplits == 0u && output.count == 1u,
        "Clear -> DrawRun -> StretchRect -> Present seals one eligible pass");
  const auto& sfivPass = output.passes[0];
  check(sfivPass.leadingClear.valid &&
            sfivPass.leadingClear.kind == Kind::Clear &&
            sfivPass.sealingCommand.valid &&
            sfivPass.sealingCommand.kind == Kind::StretchRect &&
            sfivPass.sealingCommand.replayOrdinal ==
                sfivPass.replayOrdinalEnd &&
            sfivPass.replayOrdinalBegin == 1u &&
            sfivPass.replayOrdinalEnd == 2u,
        "the StretchRect is a source-qualified sealing locator at its own "
        "serial ordinal and is excluded from the pass interval");
  for (std::size_t i = 0; i < sfivPass.childCount; ++i) {
    check(sfivPass.ranges[i].kind ==
              dxmt9::encoders::EncodePartitionRangeKind::DrawRunEntries &&
              sfivPass.ranges[i].replayOrdinalBegin ==
                  sfivPass.replayOrdinalBegin &&
              sfivPass.childReplayOrdinalBegins[i] <
                  sfivPass.replayOrdinalEnd,
          "no child range covers the coordinator command's ordinal");
  }

  // Same attachment resumes across the coordinator command: both fragments of
  // what would otherwise be one logical pass fail closed.
  PassObservationFixture split;
  split.slot.appendClear({});
  split.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  split.slot.appendStretchRect({});
  split.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  split.slot.appendPresent({}, {});
  split.finalize(431u);
  result = split.observe(output);
  check(result.candidateCount == 2u && result.eligibleCount == 0u &&
            output.count == 0u && result.coordinatorBoundaries == 1u &&
            result.coordinatorSplits == 1u &&
            result.rejectionCounts[static_cast<std::size_t>(
                Fallback::CoordinatorCommand)] == 2u,
        "a coordinator command inside one logical pass fails both fragments "
        "closed instead of resuming the pass across it");

  // A different attachment after the coordinator command is a genuine pass
  // change, so both intervals stay independently eligible.
  dxmt9::core::CanonicalDrawState stateA{};
  dxmt9::core::CanonicalDrawState stateB{};
  stateA.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x91u};
  stateB.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x92u};
  PassObservationFixture distinct;
  distinct.slot.appendDrawRun(stateA, {}, drawValues, drawPayloads);
  distinct.slot.appendStretchRect({});
  distinct.slot.appendDrawRun(stateB, {}, drawValues, drawPayloads);
  distinct.slot.appendPresent({}, {});
  distinct.finalize(432u);
  result = distinct.observe(output);
  check(result.candidateCount == 2u && result.eligibleCount == 2u &&
            output.count == 2u && result.coordinatorBoundaries == 1u &&
            result.coordinatorSplits == 0u &&
            output.passes[0].sealingCommand.kind == Kind::StretchRect &&
            output.passes[1].sealingCommand.kind == Kind::Present &&
            output.passes[0].replayOrdinalEnd <=
                output.passes[1].replayOrdinalBegin,
        "distinct attachments across a coordinator command stay two "
        "independent sealed passes");

  // Regression pin: a stream with no coordinator helper behaves exactly as it
  // did before pass-interval extraction.
  PassObservationFixture pure;
  pure.slot.appendClear({});
  pure.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  pure.slot.appendClear({});
  pure.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  pure.slot.appendPresent({}, {});
  pure.finalize(433u);
  result = pure.observe(output);
  check(result.candidateCount == 2u && result.sealedCount == 2u &&
            result.eligibleCount == 2u && result.childCount == 4u &&
            result.drawCount == 256u && output.count == 2u &&
            result.coordinatorBoundaries == 0u &&
            result.coordinatorSplits == 0u &&
            output.passes[0].passActionEpoch == 7u &&
            output.passes[1].passActionEpoch == 8u,
        "a coordinator-free source keeps its exact pre-extraction shape");

  // A coordinator command between two passes never blocks a later interval.
  PassObservationFixture leading;
  leading.slot.appendStretchRect({});
  leading.slot.appendClear({});
  leading.slot.appendDrawRun({}, {}, drawValues, drawPayloads);
  leading.slot.appendPresent({}, {});
  leading.finalize(434u);
  result = leading.observe(output, /*sourceStartsPass=*/false);
  check(result.candidateCount == 1u && result.eligibleCount == 1u &&
            output.count == 1u && result.coordinatorBoundaries == 1u &&
            result.coordinatorSplits == 0u &&
            output.passes[0].replayOrdinalBegin == 2u,
        "a leading coordinator command proves the start of the pass that "
        "follows it even on a carried source");
}

// Counts every backend call the executor makes. `effects` covers exactly the
// calls at or after `beginPassActions()`, which is the Metal effect edge.
struct AdapterEffectRecorder {
  std::uint32_t prepared = 0;
  std::uint32_t created = 0;
  std::uint32_t effects = 0;

  bool prepareParent() noexcept { ++prepared; return true; }
  bool createChild(const ParallelPassChildPlan&) noexcept {
    ++created;
    return true;
  }
  void abandonPrepared() noexcept {}
  bool beginPassActions() noexcept { ++effects; return true; }
  bool replayLogicalCommands(
      std::span<const ParallelPassChildPlan>) noexcept {
    ++effects;
    return true;
  }
  bool emitChild(const ParallelPassChildPlan&) noexcept {
    ++effects;
    return true;
  }
  bool endChild(std::uint32_t) noexcept { ++effects; return true; }
  bool joinChild(std::uint32_t) noexcept { ++effects; return true; }
  bool endPassActions() noexcept { ++effects; return true; }
  bool endParent() noexcept { ++effects; return true; }
  bool publishSidecars() noexcept { ++effects; return true; }
  bool publishCompletion() noexcept { ++effects; return true; }
  void failStop(ParallelPassFailurePhase, std::uint32_t) noexcept {}
};

struct AdapterLaneResult {
  dxmt9::encoders::ParallelPassAdapterDecision decision{};
  dxmt9::encoders::ParallelPassAdapterAccounting accounting{};
  std::uint32_t preparedParents = 0;
  std::uint32_t createdChildren = 0;
  std::uint32_t parallelEffects = 0;
  std::uint64_t serialDraws = 0;
  bool executed = false;
};

// Mirrors the production ordering: certificate gate, then selection, then —
// and only then — Metal effects. Everything else replays serially.
AdapterLaneResult runAdapterLane(
    const SemanticPlanFixture& fixture,
    const dxmt9::encoders::ParallelPassCandidateCost& cost) {
  using namespace dxmt9::encoders;
  AdapterLaneResult out{};
  out.decision = runParallelPassProofCoreAdapter(
      fixture.snapshot, fixture.view(), cost, fixture.authorityResolver(),
      fixture.coverageResolver(), fixture.epochWitness());
  out.accounting = accountParallelPassAdapter(out.decision);
  if (!out.decision.selected()) {
    for (const auto& child : fixture.view()) {
      out.serialDraws += child.range.drawEntryCount;
    }
    return out;
  }
  AdapterEffectRecorder backend{};
  std::array<std::uint32_t, kParallelRenderPassChildCapacity> order{};
  for (std::uint32_t i = 0u; i < fixture.count; ++i) {
    order[i] = i;
  }
  const auto execution = executeParallelRenderPass(
      fixture.view(),
      std::span<const std::uint32_t>(order.data(), fixture.count), backend);
  out.executed = execution.status == ParallelPassExecutionStatus::Completed;
  out.preparedParents = backend.prepared;
  out.createdChildren = backend.created;
  out.parallelEffects = backend.effects;
  return out;
}

// R-BACK-2.70 / R-VERIF-2.17: the streaming coverage fold replaces a fixed
// 16-row array with O(1) exact accumulators. The pin is adversarial: a
// stored-row reference reproducing the pre-change predicate set is run beside
// the fold over a generated domain of gap, overlap, duplicate, out-of-order,
// empty, arithmetic, subrange, and exactly-boundary row sets, and the two
// verdicts are compared. The fold must never accept what the reference
// rejects; the one permitted divergence is the deliberate fail-closed
// ordering strengthening.
void streamingCoverageFoldMatchesStoredRowReference() {
  using namespace dxmt9::encoders;
  using Row = ParallelPassCoverageFold::Command;
  using Failure = ParallelPassCoverageFoldFailure;

  struct ReferenceVerdict {
    Failure failure = Failure::NotStarted;
    std::uint32_t failureCommand = UINT32_MAX;
    std::uint64_t drawTotal = 0u;
    std::uint32_t commandCount = 0u;
    bool accepted = false;
  };

  // Stored-row reference. It keeps every row and re-checks the predicate set
  // the array form enforced, including the O(n) duplicate scan over prior
  // rows. The pre-change code collapsed all of these into one `||` chain and
  // a single rejection, so evaluation order never affected its verdict; the
  // reference uses the fold's order only so the reported reason class is
  // comparable.
  const auto reference = [](std::span<const Row> rows, std::uint32_t begin,
                            bool wholeCommands) {
    ReferenceVerdict out{};
    if (rows.empty()) {
      return out;
    }
    std::vector<Row> stored;
    std::uint32_t previousDrawParamEnd = 0u;
    for (std::uint32_t k = 0u; k < rows.size(); ++k) {
      const auto& row = rows[k];
      const auto fail = [&](Failure reason) {
        out.failure = reason;
        out.failureCommand = k;
        return out;
      };
      if (!wholeCommands && k != 0u) {
        return fail(Failure::SubrangeCapacity);
      }
      if (static_cast<std::uint64_t>(begin) + k !=
          static_cast<std::uint64_t>(row.replayOrdinal)) {
        return fail(Failure::CommandOrder);
      }
      if (row.drawParamCount == 0u) {
        return fail(Failure::EmptyCommand);
      }
      if (row.drawParamBegin > UINT32_MAX - row.drawParamCount) {
        return fail(Failure::CommandArithmetic);
      }
      if (k != 0u &&
          std::any_of(stored.begin(), stored.end(), [&](const Row& prior) {
            return prior.commandIndex == row.commandIndex;
          })) {
        return fail(Failure::CommandOverlap);
      }
      if (wholeCommands && k != 0u &&
          row.drawParamBegin != previousDrawParamEnd) {
        return fail(Failure::CommandContiguity);
      }
      if (out.drawTotal > UINT64_MAX - row.drawParamCount) {
        return fail(Failure::DrawArithmetic);
      }
      stored.push_back(row);
      previousDrawParamEnd = row.drawParamBegin + row.drawParamCount;
      out.drawTotal += row.drawParamCount;
      ++out.commandCount;
    }
    out.failure = Failure::None;
    out.accepted = true;
    return out;
  };

  constexpr std::uint32_t kBegin = 7u;
  const auto baseRow = [](std::uint32_t k) {
    return Row{
        .replayOrdinal = kBegin + k,
        .commandIndex = 20u + 2u * k,
        .drawParamBegin = 100u + 5u * k,
        .drawParamCount = 5u,
    };
  };

  std::uint32_t compared = 0u;
  std::uint32_t divergences = 0u;
  std::uint32_t acceptedCases = 0u;
  for (std::uint32_t rowCount = 1u; rowCount <= 4u; ++rowCount) {
    for (const bool wholeCommands : {false, true}) {
      for (std::uint32_t target = 0u; target < rowCount; ++target) {
        for (std::uint32_t mutation = 0u; mutation <= 9u; ++mutation) {
          std::vector<Row> rows;
          for (std::uint32_t k = 0u; k < rowCount; ++k) {
            Row row = baseRow(k);
            if (k == target) {
              switch (mutation) {
              case 1u: ++row.replayOrdinal; break;                  // gap
              case 2u: --row.replayOrdinal; break;                  // reorder
              case 3u: row.commandIndex = baseRow(0u).commandIndex; break;
              case 4u: row.commandIndex = 20u - 3u; break;          // descend
              case 5u: ++row.drawParamBegin; break;                 // gap
              case 6u: --row.drawParamBegin; break;                 // overlap
              case 7u: row.drawParamCount = 0u; break;              // empty
              case 8u:
                row.drawParamBegin = UINT32_MAX - 1u;
                row.drawParamCount = 5u;
                break;                                              // overflow
              case 9u: ++row.drawParamCount; break;                 // tail
              default: break;
              }
            }
            rows.push_back(row);
          }

          const auto expected = reference(rows, kBegin, wholeCommands);
          ParallelPassCoverageFold fold{};
          fold.open(kBegin, wholeCommands);
          for (const auto& row : rows) {
            fold.append(row);
          }
          ++compared;

          check(!fold.valid() || expected.accepted,
                "the streaming fold never accepts a row set the stored-row "
                "reference rejects");
          if (expected.accepted != fold.valid()) {
            bool strictlyIncreasing = true;
            for (std::size_t k = 1u; k < rows.size(); ++k) {
              strictlyIncreasing = strictlyIncreasing &&
                  rows[k].commandIndex > rows[k - 1u].commandIndex;
            }
            check(expected.accepted && !fold.valid() && !strictlyIncreasing &&
                      fold.failure() == Failure::CommandOverlap,
                  "the only reference/streaming divergence is the deliberate "
                  "fail-closed command-order strengthening");
            ++divergences;
            continue;
          }
          if (fold.valid()) {
            check(fold.drawTotal() == expected.drawTotal &&
                      fold.commandCount() == expected.commandCount,
                  "an accepted fold reports the reference's exact totals");
            ++acceptedCases;
            continue;
          }
          check(fold.failure() == expected.failure &&
                    fold.failureCommand() == expected.failureCommand,
                "a rejected fold reports the reference's failure class and "
                "first-failure locator");
        }
      }
    }
  }
  check(compared >= 200u && acceptedCases > 0u && divergences > 0u,
        "the generated domain exercises accepts, rejects, and the ordering "
        "strengthening");

  // Exactly-boundary and beyond: the former 16-row storage is not a limit.
  for (const std::uint32_t rowCount : {15u, 16u, 17u, 52u}) {
    ParallelPassCoverageFold fold{};
    fold.open(kBegin, true);
    for (std::uint32_t k = 0u; k < rowCount; ++k) {
      check(fold.append(baseRow(k)), "every whole-command row appends");
    }
    check(fold.valid() && fold.commandCount() == rowCount &&
              fold.drawTotal() == static_cast<std::uint64_t>(rowCount) * 5u &&
              fold.first() == baseRow(0u),
          "the fold accumulates exactly beyond any fixed row capacity");
  }

  // An empty child is still not coverage, and a failure locator survives
  // later appends.
  ParallelPassCoverageFold empty{};
  empty.open(kBegin, true);
  check(!empty.valid() && empty.failure() == Failure::NotStarted,
        "a child with no resolved command is not covered");
  ParallelPassCoverageFold sticky{};
  sticky.open(kBegin, true);
  check(sticky.append(baseRow(0u)), "first row appends");
  Row gap = baseRow(1u);
  ++gap.drawParamBegin;
  check(!sticky.append(gap), "a contiguity gap is rejected");
  check(!sticky.append(baseRow(2u)) && !sticky.valid() &&
            sticky.failure() == Failure::CommandContiguity &&
            sticky.failureCommand() == 1u,
        "the first failure and its locator survive later appends");
}

// R-BACK-2.70: a whole-command child owning far more commands than the former
// 16-row coverage storage now certifies through the production validator.
void wideWholeCommandChildrenCertify() {
  using namespace dxmt9::encoders;
  PassObservationFixture wide;
  for (std::uint32_t command = 0u; command < 60u; ++command) {
    wide.slot.appendDrawRun({}, {}, draws(3u), payloads(3u));
  }
  // The production partition planner reports an explicit plan only when some
  // DrawRun is large enough to subdivide; the wide small-command run is what
  // the coverage fold is being exercised on.
  wide.slot.appendDrawRun({}, {}, draws(128u), payloads(128u));
  wide.slot.appendPresent({}, {});
  wide.finalize(640u);
  SealedParallelPassSnapshotBatch output{};
  const auto observed = wide.observe(output);
  check(observed.eligibleCount == 1u && output.count == 1u &&
            output.passes[0].childrenCoverCompleteCommands,
        "a many-command source still produces one whole-command candidate");

  const auto& snapshot = output.passes[0];
  std::uint32_t widestChild = 0u;
  for (std::uint32_t i = 0u; i < snapshot.childCount; ++i) {
    widestChild = std::max(widestChild, snapshot.childReplayOrdinalCounts[i]);
  }
  check(widestChild > kParallelRenderPassChildCapacity,
        "the fixture's widest child owns more commands than the former row "
        "capacity");

  std::array<ParallelPassChildPlan, kParallelRenderPassChildCapacity>
      children{};
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
        .coversCompleteCommands = true,
        .forceFullFirstDrawBinding = true,
    };
  }
  ProducerCoverageContext context{.fixture = &wide};
  const ParallelPassSnapshotAuthority authority{
      .context = &context, .resolve = resolveProducerAuthority};
  const ParallelPassCoverageResolver resolver{
      .context = &context, .resolve = resolveProducerCoverage};
  const auto witness = producerEpochWitness(context);
  check(validateParallelPassSemanticPlan(
            snapshot, {children.data(), snapshot.childCount}, authority,
            resolver, witness)
            .accepted(),
        "a child owning more commands than the former row capacity certifies");

  // The per-row predicates are still enforced at that width.
  for (std::uint32_t field = 1u; field <= 3u; ++field) {
    context.mutateNonzeroWholeRowField = field;
    check(!validateParallelPassSemanticPlan(
                snapshot, {children.data(), snapshot.childCount}, authority,
                resolver, witness)
               .accepted(),
          "wide whole-command rows still fail closed on identity/range "
          "mutation");
  }
  context.mutateNonzeroWholeRowField = 0u;
}

// R-BACK-2.69: the certificate never trusts a stored action epoch. The
// producer issues the coordinator proof per pass, and the certificate
// re-derives that pass's epoch itself by folding the shared classifier over
// the generation-pinned replay stream in replay-ordinal order. The stamp is
// accepted only when it belongs to this pass of this stream.
void passLocalEpochProofIsIndependentlyReDerived() {
  using namespace dxmt9::encoders;
  const auto drawValues = draws(128u);
  const auto drawPayloads = payloads(drawValues.size());
  dxmt9::core::ClearDesc partial{};
  partial.rects.push_back({});

  // The exact GT2 shape the source-wide stamp could not certify: two pass
  // intervals in one source, the second carrying action epoch 3 against a
  // source-wide proof epoch of 1.
  PassObservationFixture twoPasses;
  twoPasses.slot.appendClear({});                                 // 0
  twoPasses.slot.appendDrawRun({}, {}, drawValues, drawPayloads);  // 1
  twoPasses.slot.appendClear(partial);                            // 2
  twoPasses.slot.appendDrawRun({}, {}, drawValues, drawPayloads);  // 3
  twoPasses.slot.appendPresent({}, {});                           // 4
  twoPasses.finalize(620u);
  SealedParallelPassSnapshotBatch output{};
  const auto observed =
      twoPasses.observe(output, true, true, completeProofs(1u));
  check(observed.eligibleCount == 2u && output.count == 2u &&
            output.passes[0].replayOrdinalBegin == 1u &&
            output.passes[1].replayOrdinalBegin == 3u &&
            output.passes[0].passActionEpoch == 1u &&
            output.passes[1].passActionEpoch == 3u,
        "a source's second pass interval carries its own action epoch");
  check(output.passes[0].coordinatorProof.firstPassActionEpoch == 1u &&
            output.passes[1].coordinatorProof.firstPassActionEpoch == 3u &&
            output.passes[0].coordinatorProof.flags ==
                kParallelPassCoordinatorProofComplete &&
            output.passes[1].coordinatorProof.flags ==
                kParallelPassCoordinatorProofComplete,
        "the coordinator proof is issued per pass and keeps its source-wide "
        "facts");

  ProducerCoverageContext context{.fixture = &twoPasses, .seedEpoch = 1u};
  const ParallelPassSnapshotAuthority authority{
      .context = &context, .resolve = resolveProducerAuthority};
  const ParallelPassCoverageResolver resolver{
      .context = &context, .resolve = resolveProducerCoverage};
  const auto witness = producerEpochWitness(context);
  const auto& source = twoPasses.stream.source.source;
  const std::uint64_t seqId = twoPasses.stream.source.seqId;

  // (a) The fold is pure: the same stream yields the same value every time,
  // for every pass, and only a pass-opening DrawRun derives at all.
  for (const std::uint32_t ordinal : {0u, 1u, 2u, 3u, 4u}) {
    const auto first =
        deriveParallelPassActionEpoch(source, seqId, ordinal, witness);
    const auto second =
        deriveParallelPassActionEpoch(source, seqId, ordinal, witness);
    check(first == second,
          "action-epoch re-derivation is deterministic over the same stream");
    const bool startsPass = ordinal == 1u || ordinal == 3u;
    check(first.valid == startsPass,
          "only a pass-opening DrawRun ordinal derives an action epoch");
  }
  check(deriveParallelPassActionEpoch(source, seqId, 1u, witness).epoch == 1u &&
            deriveParallelPassActionEpoch(source, seqId, 3u, witness).epoch ==
                3u,
        "each derived epoch matches the producer's pass-local stamp");
  check(!deriveParallelPassActionEpoch(source, seqId, 5u, witness).valid &&
            !deriveParallelPassActionEpoch(source, seqId + 1u, 3u, witness)
                 .valid,
        "out-of-range ordinals and foreign generations derive nothing");

  const auto planChildren =
      [](const SealedParallelPassSnapshot& snapshot,
         std::array<ParallelPassChildPlan,
                    kParallelRenderPassChildCapacity>& children) {
        for (std::uint32_t i = 0u; i < snapshot.childCount; ++i) {
          children[i] = {
              .range = snapshot.ranges[i],
              .firstDraw = snapshot.firstDraws[i],
              .binding = bindingSnapshot(
                  ParallelPassDirectBindingMode::Stage1Direct,
                  static_cast<std::uint16_t>(i + 1u)),
              .replayOrdinalBegin = snapshot.childReplayOrdinalBegins[i],
              .replayOrdinalCount = snapshot.childReplayOrdinalCounts[i],
              .childOrdinal = i,
              .localShadowOrdinal = i + 1u,
              .coversCompleteCommands =
                  snapshot.childrenCoverCompleteCommands,
              .forceFullFirstDrawBinding = true,
          };
        }
        return std::span<const ParallelPassChildPlan>(children.data(),
                                                      snapshot.childCount);
      };

  // (b) Both passes certify, not only the source's first one.
  std::array<std::array<ParallelPassChildPlan,
                        kParallelRenderPassChildCapacity>, 2> childStorage{};
  for (std::size_t pass = 0u; pass < output.count; ++pass) {
    const auto children =
        planChildren(output.passes[pass], childStorage[pass]);
    const auto validation = validateParallelPassSemanticPlan(
        output.passes[pass], children, authority, resolver, witness);
    check(validation.accepted(),
          "every pass interval of a multi-pass source certifies");
  }

  // (c) A stamp that does not belong to this pass fails closed even when the
  // owner echoes it back and every dependent field agrees with it. Only the
  // independent re-derivation can see this.
  const auto secondPass = output.passes[1];
  std::array<ParallelPassChildPlan, kParallelRenderPassChildCapacity>
      echoChildren{};
  const auto echoPlan = planChildren(secondPass, echoChildren);
  const ParallelPassSnapshotAuthority echoAuthority{
      .context = &secondPass, .resolve = resolveEchoingAuthority};
  check(validateParallelPassSemanticPlan(secondPass, echoPlan, echoAuthority,
                                         resolver, witness)
            .accepted(),
        "the echoing owner accepts the unmutated second pass");

  auto forged = secondPass;
  std::array<ParallelPassChildPlan, kParallelRenderPassChildCapacity>
      forgedChildren{};
  forged.passActionEpoch = 1u;
  forged.coordinatorProof.firstPassActionEpoch = 1u;
  for (std::uint32_t i = 0u; i < forged.childCount; ++i) {
    forged.firstDraws[i].entryRender.passActionEpoch = 1u;
  }
  const auto forgedPlan = planChildren(forged, forgedChildren);
  const ParallelPassSnapshotAuthority forgedAuthority{
      .context = &forged, .resolve = resolveEchoingAuthority};
  const auto forgedResult = validateParallelPassSemanticPlan(
      forged, forgedPlan, forgedAuthority, resolver, witness);
  check(!forgedResult.accepted() &&
            forgedResult.failure ==
                ParallelPassSemanticPlanFailure::PassIdentity,
        "a self-consistent stamp taken from another pass fails pass identity");

  auto tampered = secondPass;
  tampered.coordinatorProof.firstPassActionEpoch =
      secondPass.passActionEpoch + 1u;
  const ParallelPassSnapshotAuthority tamperedAuthority{
      .context = &tampered, .resolve = resolveEchoingAuthority};
  check(!validateParallelPassSemanticPlan(tampered, echoPlan,
                                          tamperedAuthority, resolver, witness)
             .accepted(),
        "a coordinator proof stamped with a foreign epoch fails closed");

  // (d) A stale source generation still fails closed before the fold runs, so
  // the re-derivation never sees a snapshot the owner has already rejected.
  auto stale = secondPass;
  stale.source.storage.generation++;
  context.epochReads = 0u;
  const auto staleResult = validateParallelPassSemanticPlan(
      stale, echoPlan, authority, resolver, witness);
  check(!staleResult.accepted() &&
            staleResult.failure ==
                ParallelPassSemanticPlanFailure::SourceIdentity &&
            context.epochReads == 0u,
        "a stale storage generation fails closed before any epoch fold");
}

// R-BACK-2.69: the two boundary-adjacent pass-identity shapes that a wild
// re-measurement attributed 100% of the SFIV and 1.5% of the GT2
// certificate-invalid population to. Both were false negatives inside the
// `PassIdentity` checkpoint, and both are reproduced here through the real
// producer and the real certificate.
void boundaryAdjacentPassIdentityCertifies() {
  using namespace dxmt9::encoders;
  const auto drawValues = draws(128u);
  const auto drawPayloads = payloads(drawValues.size());
  const auto planChildren =
      [](const SealedParallelPassSnapshot& snapshot,
         std::array<ParallelPassChildPlan,
                    kParallelRenderPassChildCapacity>& children) {
        for (std::uint32_t i = 0u; i < snapshot.childCount; ++i) {
          children[i] = {
              .range = snapshot.ranges[i],
              .firstDraw = snapshot.firstDraws[i],
              .binding = bindingSnapshot(
                  ParallelPassDirectBindingMode::Stage1Direct,
                  static_cast<std::uint16_t>(i + 1u)),
              .replayOrdinalBegin = snapshot.childReplayOrdinalBegins[i],
              .replayOrdinalCount = snapshot.childReplayOrdinalCounts[i],
              .childOrdinal = i,
              .localShadowOrdinal = i + 1u,
              .coversCompleteCommands =
                  snapshot.childrenCoverCompleteCommands,
              .forceFullFirstDrawBinding = true,
          };
        }
        return std::span<const ParallelPassChildPlan>(children.data(),
                                                      snapshot.childCount);
      };

  static_assert(kParallelPassSeedActionEpoch != 0u,
                "the coordinator's published seed is the domain's first epoch, "
                "never its invalid sentinel");

  // The SFIV shape: Clear -> DrawRuns -> StretchRect -> DrawRuns -> Present,
  // with distinct attachments across the helper so both fragments stay
  // independent passes. One pass is sealed *by* the StretchRect and the next
  // one *begins* after it. `carried` selects the coordinator's carried-session
  // boundary facts; under the old session-dependent seed that configuration
  // could not certify a single pass, which is the mechanism behind SFIV's
  // 1,387-of-1,387 `certificate_invalid_pass_identity` residual.
  dxmt9::core::CanonicalDrawState stateA{};
  dxmt9::core::CanonicalDrawState stateB{};
  stateA.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x71u};
  stateB.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x72u};
  const auto certifySfivShape = [&](bool carried, std::uint64_t seqId) {
    PassObservationFixture fixture;
    fixture.slot.appendClear({});                                     // 0
    fixture.slot.appendDrawRun(stateA, {}, drawValues, drawPayloads);  // 1
    fixture.slot.appendStretchRect({});                               // 2
    fixture.slot.appendDrawRun(stateB, {}, drawValues, drawPayloads);  // 3
    fixture.slot.appendPresent({}, {});                               // 4
    fixture.finalize(seqId);
    // The production coordinator seeds every source, carried or not, with the
    // domain's first epoch; `sourceStartsPass` is what carries the carried
    // fact. A zero seed here used to make the certificate's re-derivation
    // structurally impossible for every pass in the source.
    SealedParallelPassSnapshotBatch output{};
    const auto observed = fixture.observe(
        output, !carried, true, completeProofs(kParallelPassSeedActionEpoch));
    check(observed.eligibleCount == 2u && output.count == 2u &&
              output.passes[0].replayOrdinalBegin == 1u &&
              output.passes[0].replayOrdinalEnd == 2u &&
              output.passes[0].sealingCommand.valid &&
              output.passes[0].sealingCommand.kind ==
                  dxmt9::core::MetalCommandKind::StretchRect &&
              output.passes[1].replayOrdinalBegin == 3u &&
              output.passes[1].replayOrdinalEnd == 4u &&
              output.passes[1].sealingCommand.kind ==
                  dxmt9::core::MetalCommandKind::Present,
          "the SFIV shape seals one pass at the StretchRect and opens the "
          "next one after it");

    ProducerCoverageContext context{.fixture = &fixture,
                                    .seedEpoch = kParallelPassSeedActionEpoch,
                                    .sourceStartsPass = !carried};
    const ParallelPassSnapshotAuthority authority{
        .context = &context, .resolve = resolveProducerAuthority};
    const ParallelPassCoverageResolver resolver{
        .context = &context, .resolve = resolveProducerCoverage};
    const auto witness = producerEpochWitness(context);
    check(witness.valid(),
          "the coordinator publishes a usable epoch seed on a carried source");
    std::array<std::array<ParallelPassChildPlan,
                          kParallelRenderPassChildCapacity>, 2> childStorage{};
    for (std::size_t pass = 0u; pass < output.count; ++pass) {
      const auto& snapshot = output.passes[pass];
      const auto children = planChildren(snapshot, childStorage[pass]);
      const auto validation = validateParallelPassSemanticPlan(
          snapshot, children, authority, resolver, witness);
      const auto derived = deriveParallelPassActionEpoch(
          snapshot.source, snapshot.seqId, snapshot.replayOrdinalBegin,
          witness);
      check(validation.accepted() && derived.valid &&
                derived.epoch == snapshot.passActionEpoch,
            "both boundary-adjacent SFIV passes certify against an "
            "independently re-derived action epoch");
    }
    check(output.passes[0].passActionEpoch != output.passes[1].passActionEpoch,
          "the two SFIV pass intervals keep distinct action epochs");

    // The re-derivation still catches a stamp that belongs to the other side
    // of the StretchRect, even with the owner echoing the forgery back and
    // every dependent field agreeing with it.
    const auto postBoundary = output.passes[1];
    auto forged = postBoundary;
    forged.passActionEpoch = output.passes[0].passActionEpoch;
    forged.coordinatorProof.firstPassActionEpoch = forged.passActionEpoch;
    for (std::uint32_t i = 0u; i < forged.childCount; ++i) {
      forged.firstDraws[i].entryRender.passActionEpoch =
          forged.passActionEpoch;
    }
    std::array<ParallelPassChildPlan, kParallelRenderPassChildCapacity>
        forgedChildren{};
    const auto forgedPlan = planChildren(forged, forgedChildren);
    const ParallelPassSnapshotAuthority forgedAuthority{
        .context = &forged, .resolve = resolveEchoingAuthority};
    const auto forgedResult = validateParallelPassSemanticPlan(
        forged, forgedPlan, forgedAuthority, resolver, witness);
    check(!forgedResult.accepted() &&
              forgedResult.failure ==
                  ParallelPassSemanticPlanFailure::PassIdentity,
          "the pre-boundary pass's stamp cannot certify the post-boundary "
          "pass");
  };
  certifySfivShape(/*carried=*/false, 640u);
  certifySfivShape(/*carried=*/true, 641u);

  // The GT2 edge shape: a pass sealed by an attachment change. No coordinator
  // command sits at `replayOrdinalEnd`; the first draw of the next pass does,
  // which is exactly the spelling `parallelPassCloseReason` already maps to
  // `RenderTargetChange`. The certificate used to reject it because a draw is
  // not a coordinator-owned sealing kind.
  dxmt9::core::CanonicalDrawState first{};
  dxmt9::core::CanonicalDrawState second{};
  first.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x81u};
  second.hot.colorAttachments[0].handle = dxmt9::core::Handle{0x82u};
  PassObservationFixture change;
  change.slot.appendDrawRun(first, {}, drawValues, drawPayloads);   // 0
  change.slot.appendDrawRun(second, {}, drawValues, drawPayloads);  // 1
  change.slot.appendPresent({}, {});                                // 2
  change.finalize(642u);
  SealedParallelPassSnapshotBatch changeOutput{};
  const auto changeObserved = change.observe(
      changeOutput, true, true, completeProofs(kParallelPassSeedActionEpoch));
  check(changeObserved.eligibleCount == 2u && changeOutput.count == 2u &&
            changeOutput.passes[0].replayOrdinalBegin == 0u &&
            changeOutput.passes[0].replayOrdinalEnd == 1u &&
            changeOutput.passes[0].sealingCommand.valid &&
            changeOutput.passes[0].sealingCommand.kind ==
                dxmt9::core::MetalCommandKind::DrawRun &&
            !parallelPassSealingKindAccepted(
                changeOutput.passes[0].sealingCommand.kind) &&
            parallelPassAttachmentChangeSealingKind(
                changeOutput.passes[0].sealingCommand.kind),
        "an attachment change seals a pass with the next pass's first draw as "
        "its locator, which is not a coordinator-owned sealing kind");

  ProducerCoverageContext changeContext{
      .fixture = &change, .seedEpoch = kParallelPassSeedActionEpoch};
  const ParallelPassSnapshotAuthority changeAuthority{
      .context = &changeContext, .resolve = resolveProducerAuthority};
  const ParallelPassCoverageResolver changeResolver{
      .context = &changeContext, .resolve = resolveProducerCoverage};
  const auto changeWitness = producerEpochWitness(changeContext);
  std::array<std::array<ParallelPassChildPlan,
                        kParallelRenderPassChildCapacity>, 2> changeChildren{};
  for (std::size_t pass = 0u; pass < changeOutput.count; ++pass) {
    const auto children =
        planChildren(changeOutput.passes[pass], changeChildren[pass]);
    check(validateParallelPassSemanticPlan(changeOutput.passes[pass], children,
                                          changeAuthority, changeResolver,
                                          changeWitness)
              .accepted(),
          "an attachment-change-sealed pass certifies once the sealing ordinal "
          "is proven to open a different pass");
  }

  // The attachment-change seal is admitted only because that extra proof
  // succeeds. A witness that cannot reach the sealing ordinal still derives
  // this pass's own epoch, so only the added next-pass obligation can reject —
  // and it must.
  auto truncated = changeWitness;
  truncated.replayOrdinalCount = changeOutput.passes[0].replayOrdinalEnd;
  check(deriveParallelPassActionEpoch(
            changeOutput.passes[0].source, changeOutput.passes[0].seqId,
            changeOutput.passes[0].replayOrdinalBegin, truncated)
                .epoch == changeOutput.passes[0].passActionEpoch &&
            !deriveParallelPassActionEpoch(
                 changeOutput.passes[0].source, changeOutput.passes[0].seqId,
                 changeOutput.passes[0].replayOrdinalEnd, truncated)
                 .valid,
        "the truncated witness still derives the pass's own epoch but cannot "
        "reach the sealing ordinal");
  const auto truncatedChildren =
      planChildren(changeOutput.passes[0], changeChildren[0]);
  const auto truncatedResult = validateParallelPassSemanticPlan(
      changeOutput.passes[0], truncatedChildren, changeAuthority,
      changeResolver, truncated);
  check(!truncatedResult.accepted() &&
            truncatedResult.failure ==
                ParallelPassSemanticPlanFailure::PassIdentity,
        "an unproven attachment-change sealing ordinal fails pass identity");

  // A coordinator-sealed pass in the same source is unaffected by the new
  // branch: its sealing kind is accepted directly and no second fold runs.
  changeContext.epochReads = 0u;
  const auto presentSealed =
      planChildren(changeOutput.passes[1], changeChildren[1]);
  check(validateParallelPassSemanticPlan(changeOutput.passes[1], presentSealed,
                                        changeAuthority, changeResolver,
                                        changeWitness)
                .accepted() &&
            changeContext.epochReads ==
                changeOutput.passes[1].replayOrdinalBegin + 1u,
        "a coordinator-sealed pass folds the stream exactly once");
}

// R-BACK-2.69/2.74: the production adapter is the only path from a sealed pass
// to Metal child creation, an invalid certificate can never be partially
// consumed, and every rejection returns to exact serial replay before effects.
void proofCoreAdapterGatesEveryProductionCandidate() {
  using namespace dxmt9::encoders;
  using Outcome = ParallelPassAdapterOutcome;

  SemanticPlanFixture accepted(3u);
  ParallelPassCandidateCost cost{};
  check(buildParallelPassCandidateCost(accepted.cost(0).economics, cost) &&
            cost.valid && !cost.overflow &&
            cost.serialWork.raw == 192ll * ParallelPassFixedPoint::kFraction &&
            cost.criticalPath.raw == 64ll * ParallelPassFixedPoint::kFraction &&
            cost.childSetup.raw ==
                3ll * kParallelPassChildSetupDrawEquivalents *
                    ParallelPassFixedPoint::kFraction &&
            cost.imbalance.raw == 0 &&
            cost.perPassOverhead.raw ==
                static_cast<std::int64_t>(
                    kParallelPassPerPassOverheadDrawEquivalents) *
                    ParallelPassFixedPoint::kFraction,
        "the cost record is built from certified integers with checked "
        "fixed-point conversion, including the calibrated per-child setup "
        "and per-pass coordinator overhead terms");

  const auto valid = runAdapterLane(accepted, cost);
  check(valid.decision.outcome == Outcome::Selected &&
            valid.decision.certificate ==
                ParallelPassSemanticPlanFailure::None &&
            valid.decision.selection ==
                ParallelPassCandidateSelectionFailure::None &&
            valid.decision.candidateOrdinal == 0u && valid.executed &&
            valid.preparedParents == 1u &&
            valid.createdChildren == accepted.count &&
            valid.parallelEffects != 0u && valid.serialDraws == 0u &&
            valid.accounting.conserves() &&
            valid.accounting.selected == 1u &&
            valid.accounting.certificateValid == 1u,
        "a certified, positively scored plan selects and executes exactly once");

  // Read the one typed reason field that a given
  // `ParallelPassSemanticPlanFailure` checkpoint is expected to own in the
  // `parallel_pass_adapter_certificate_invalid_*` counter breakdown.
  using Failure = ParallelPassSemanticPlanFailure;
  const auto reasonField =
      [](const dxmt9::perf::ParallelPassAdapterCertificateInvalidSnapshot& s,
         Failure failure) -> std::uint64_t {
    switch (failure) {
    case Failure::MissingSnapshot:
      return s.missingSnapshot;
    case Failure::SourceIdentity:
      return s.sourceIdentity;
    case Failure::PassIdentity:
      return s.passIdentity;
    case Failure::CoordinatorProof:
      return s.coordinatorProof;
    case Failure::AttachmentProof:
      return s.attachmentProof;
    case Failure::ResourceProof:
      return s.resourceProof;
    case Failure::FirstDrawProof:
      return s.firstDrawProof;
    case Failure::ChildCapacity:
      return s.childCapacity;
    case Failure::ChildPlan:
      return s.childPlan;
    case Failure::Coverage:
      return s.coverage;
    case Failure::Arithmetic:
      return s.arithmetic;
    case Failure::None:
    case Failure::Count:
      return 0u;
    }
    return 0u;
  };

  // Read the one typed reason field that a given
  // `ParallelPassCandidateSelectionFailure` checkpoint is expected to own in
  // the `parallel_pass_adapter_selection_*` counter breakdown.
  using SelectionFailure = ParallelPassCandidateSelectionFailure;
  const auto selectionReasonField =
      [](const dxmt9::perf::ParallelPassAdapterSelectionSnapshot& s,
         SelectionFailure failure) -> std::uint64_t {
    switch (failure) {
    case SelectionFailure::Empty:
      return s.empty;
    case SelectionFailure::InvalidPlan:
      return s.invalidPlan;
    case SelectionFailure::InvalidEconomics:
      return s.invalidEconomics;
    case SelectionFailure::NonPositiveBenefit:
      return s.nonPositiveBenefit;
    case SelectionFailure::Arithmetic:
      return s.arithmetic;
    case SelectionFailure::InvalidCandidateOrdinal:
      return s.invalidCandidateOrdinal;
    case SelectionFailure::None:
    case SelectionFailure::Count:
      return 0u;
    }
    return 0u;
  };

  // Drives the real `countParallelPassAdapter` writer for a
  // certificate-valid-but-not-selected decision (the seam the certificate
  // breakdown above cannot see) and proves that exactly the expected typed
  // `parallel_pass_adapter_selection_*` reason counter moves by one while
  // every other typed selection reason stays put.
  const auto expectSelectionFailure =
      [&](const ParallelPassAdapterDecision& decision,
          SelectionFailure expectedFailure, std::string_view message) {
    check(decision.certificateValid() && !decision.selected() &&
              decision.selection == expectedFailure,
          "the decision under test is certificate-valid but not selected, "
          "for the expected reason");
    const auto before = dxmt9::perf::snapshotParallelPassAdapterSelection();
    dxmt9::perf::countParallelPassAdapter(decision);
    const auto after = dxmt9::perf::snapshotParallelPassAdapterSelection();
    check(after.total() == before.total() + 1u &&
              selectionReasonField(after, expectedFailure) ==
                  selectionReasonField(before, expectedFailure) + 1u,
          message);
    for (const auto other :
         {SelectionFailure::Empty, SelectionFailure::InvalidPlan,
          SelectionFailure::InvalidEconomics,
          SelectionFailure::NonPositiveBenefit, SelectionFailure::Arithmetic,
          SelectionFailure::InvalidCandidateOrdinal}) {
      if (other == expectedFailure) {
        continue;
      }
      check(selectionReasonField(after, other) ==
                selectionReasonField(before, other),
            "every other typed selection reason counter is untouched");
    }
  };

  // The authority owner still holds the unmutated snapshot, so every mutated
  // candidate is rejected by the certificate before it can reach the
  // selector. Each fixture also drives the real `countParallelPassAdapter`
  // writer so the typed `parallel_pass_adapter_certificate_invalid_*`
  // breakdown is proven against production code, not just the local
  // in-memory accounting struct: exactly the reason counter matching
  // `expectedFailure` must move by one, every other typed reason must stay
  // put, and the per-reason total must track the aggregate
  // `certificateInvalid` counter one-for-one.
  const auto expectCertificateInvalid = [&](auto mutate, Failure expectedFailure,
                                            std::string_view message) {
    SemanticPlanFixture candidate = accepted;
    mutate(candidate);
    const auto lane = runAdapterLane(candidate, cost);
    check(lane.decision.outcome == Outcome::CertificateInvalid &&
              !lane.decision.certificateValid() && !lane.executed &&
              lane.preparedParents == 0u && lane.createdChildren == 0u &&
              lane.parallelEffects == 0u &&
              lane.serialDraws == 64u * candidate.count &&
              lane.decision.selection ==
                  ParallelPassCandidateSelectionFailure::Empty &&
              lane.decision.certificate == expectedFailure &&
              lane.accounting.conserves() &&
              lane.accounting.certificateInvalid == 1u &&
              lane.accounting.selected == 0u &&
              lane.accounting.serialFallback == 1u,
          message);

    const auto before =
        dxmt9::perf::snapshotParallelPassAdapterCertificateInvalid();
    dxmt9::perf::countParallelPassAdapter(lane.decision);
    const auto after =
        dxmt9::perf::snapshotParallelPassAdapterCertificateInvalid();
    check(after.total() == before.total() + 1u &&
              reasonField(after, expectedFailure) ==
                  reasonField(before, expectedFailure) + 1u,
          "the typed reason counter for the expected checkpoint moves by "
          "exactly one");
    for (const auto other :
         {Failure::MissingSnapshot, Failure::SourceIdentity,
          Failure::PassIdentity, Failure::CoordinatorProof,
          Failure::AttachmentProof, Failure::ResourceProof,
          Failure::FirstDrawProof, Failure::ChildCapacity, Failure::ChildPlan,
          Failure::Coverage, Failure::Arithmetic}) {
      if (other == expectedFailure) {
        continue;
      }
      check(reasonField(after, other) == reasonField(before, other),
            "every other typed reason counter is untouched");
    }
  };
  expectCertificateInvalid([](auto& f) { f.snapshot.passActionEpoch = 8u; },
                           Failure::SourceIdentity,
                           "action-epoch drift yields zero Metal effects and "
                           "exact serial replay, attributed to source "
                           "identity because the authority owner still "
                           "holds the unmutated snapshot");
  expectCertificateInvalid(
      [](auto& f) { f.snapshot.childDrawCounts[1] = 63u; },
      Failure::SourceIdentity,
      "child coverage drift yields zero Metal effects and exact serial "
      "replay, attributed to source identity for the same reason");
  expectCertificateInvalid(
      [](auto& f) {
        f.children[1].binding.mode =
            ParallelPassDirectBindingMode::Stage2DirectCbuf;
      },
      Failure::ChildPlan,
      "mixed child ABI leaves the snapshot untouched, so it clears source "
      "identity and is caught by the child-plan proof instead");
  expectCertificateInvalid(
      [](auto& f) { f.children[1].childOrdinal = 99u; }, Failure::FirstDrawProof,
      "an out-of-order child ordinal leaves the snapshot untouched and "
      "fails the per-child first-draw provenance proof");

  // A certified plan whose economics cannot be scored is still refused before
  // effects, and the selector — not the certificate — is what refuses it.
  ParallelPassCandidateCost overflowed{};
  auto hugeEconomics = accepted.cost(0).economics;
  hugeEconomics.totalDraws = UINT64_MAX;
  check(!buildParallelPassCandidateCost(hugeEconomics, overflowed) &&
            !overflowed.valid && overflowed.overflow,
        "an unrepresentable draw total leaves the cost record invalid");
  const auto unscored = runAdapterLane(accepted, overflowed);
  check(unscored.decision.outcome == Outcome::NotSelected &&
            unscored.decision.certificateValid() &&
            unscored.decision.certificate ==
                ParallelPassSemanticPlanFailure::None &&
            unscored.decision.selection ==
                ParallelPassCandidateSelectionFailure::InvalidEconomics &&
            !unscored.executed && unscored.parallelEffects == 0u &&
            unscored.serialDraws == 192u && unscored.accounting.conserves() &&
            unscored.accounting.certificateValid == 1u &&
            unscored.accounting.selected == 0u,
        "an invalid economics record fails closed at selection, after a valid "
        "certificate, with zero Metal effects");
  expectSelectionFailure(unscored.decision, SelectionFailure::InvalidEconomics,
                         "the typed selection reason counter for invalid "
                         "economics moves by exactly one");

  // A non-positive benefit is a legitimate serial selection, not an error.
  auto nonPositive = cost;
  nonPositive.serialWork = cost.criticalPath;
  const auto rejected = runAdapterLane(accepted, nonPositive);
  check(rejected.decision.outcome == Outcome::NotSelected &&
            rejected.decision.selection ==
                ParallelPassCandidateSelectionFailure::NonPositiveBenefit &&
            !rejected.executed && rejected.parallelEffects == 0u &&
            rejected.accounting.conserves(),
        "a non-positive benefit selects serial without effects");
  expectSelectionFailure(rejected.decision,
                         SelectionFailure::NonPositiveBenefit,
                         "the typed selection reason counter for "
                         "non-positive benefit moves by exactly one");

  // Conservation over the whole observed population.
  ParallelPassAdapterAccounting total{};
  const auto accumulate = [&](const ParallelPassAdapterAccounting& one) {
    total.considered += one.considered;
    total.certificateValid += one.certificateValid;
    total.certificateInvalid += one.certificateInvalid;
    total.selected += one.selected;
    total.serialFallback += one.serialFallback;
  };
  accumulate(valid.accounting);
  accumulate(unscored.accounting);
  accumulate(rejected.accounting);
  // Calibration pin (GT2-shaped large pass still selects): every bounded
  // child count (2..16, each with 64 draws/child, so totalDraws grows with
  // childCount) still clears the calibrated
  // `kParallelPassChildSetupDrawEquivalents` +
  // `kParallelPassPerPassOverheadDrawEquivalents` cost floor and selects
  // through the real `buildParallelPassCandidateCost` /
  // `selectParallelPassCandidate` production path — see
  // `parallelPassCostCalibrationConstantsChangeSelectionDirection` below for
  // the paired small-pass shape that now fails instead.
  for (std::uint32_t children = 2u;
       children <= kParallelRenderPassChildCapacity; ++children) {
    SemanticPlanFixture bounded(children, 20u + children, 700u + children);
    ParallelPassCandidateCost boundedCost{};
    check(buildParallelPassCandidateCost(bounded.cost(0).economics,
                                         boundedCost),
          "every bounded child count produces a representable cost record");
    const auto lane = runAdapterLane(bounded, boundedCost);
    check(lane.decision.selected() && lane.executed &&
              lane.createdChildren == children && lane.accounting.conserves(),
          "every bounded certified plan selects and executes");
    accumulate(lane.accounting);
  }
  check(total.conserves() && total.considered == 3u +
            (kParallelRenderPassChildCapacity - 1u) &&
            total.certificateValid == total.considered &&
            total.certificateInvalid == 0u &&
            total.selected + total.serialFallback == total.considered,
        "adapter outcomes conserve across the observed population");
}

// Calibration pin for `kParallelPassChildSetupDrawEquivalents` /
// `kParallelPassPerPassOverheadDrawEquivalents` (measured 2026-08-18 on GT2,
// see the constants' comment in dxmt9_parallel_render_pass.hpp for the raw
// numbers). Exercises the real `buildParallelPassCandidateCost` production
// function directly against a hand-built economics summary shaped like the
// SFIV selection-attribution finding (small, jagged 2-child passes): this
// keeps the pin exact and independent of `SemanticPlanFixture`, whose
// fixed 64-draws/child shape cannot represent a small pass. The formula
// itself — `benefit = serialWork - (criticalPath + childSetup + imbalance +
// perPassOverhead)` — is the same one `scoreParallelPassCandidate` runs, and
// is reproduced here with the same checked fixed-point helpers so the pin
// fails if either the constants or the formula drift.
void parallelPassCostCalibrationConstantsChangeSelectionDirection() {
  using namespace dxmt9::encoders;

  // Provenance pin: catches an accidental edit to either constant before it
  // reaches the cost formula below.
  static_assert(kParallelPassChildSetupDrawEquivalents == 2u,
                "measured ~1.6 draw-equivalents/child, rounded up "
                "(fail-closed direction) -- see the constant's comment");
  static_assert(kParallelPassPerPassOverheadDrawEquivalents == 32u,
                "measured ~33 draw-equivalents/pass, rounded down to a "
                "power-of-two within measurement noise -- see the "
                "constant's comment");

  // SFIV-like small pass: 2 children, 8 draws each, 16 draws total. The
  // selection-attribution finding was 1,390/1,390 certificate-valid
  // candidates failing NonPositiveBenefit for exactly this jagged
  // small-2-child shape.
  ParallelPassEconomicsSummary small{
      .totalDraws = 16u,
      .stage1Draws = 16u,
      .childCount = 2u,
      .minimumChildDraws = 8u,
      .maximumChildDraws = 8u,
      .valid = true,
  };
  ParallelPassCandidateCost smallCost{};
  check(buildParallelPassCandidateCost(small, smallCost) && smallCost.valid &&
            !smallCost.overflow,
        "the small-pass economics produce a representable cost record");
  check(smallCost.serialWork.raw == 16ll * ParallelPassFixedPoint::kFraction &&
            smallCost.criticalPath.raw ==
                8ll * ParallelPassFixedPoint::kFraction &&
            smallCost.childSetup.raw ==
                2ll * kParallelPassChildSetupDrawEquivalents *
                    ParallelPassFixedPoint::kFraction &&
            smallCost.imbalance.raw == 0 &&
            smallCost.perPassOverhead.raw ==
                static_cast<std::int64_t>(
                    kParallelPassPerPassOverheadDrawEquivalents) *
                    ParallelPassFixedPoint::kFraction,
        "the small-pass cost record matches the calibrated per-child and "
        "per-pass terms exactly");

  // The pre-calibration formula (childSetup = childCount * 1, no per-pass
  // term) would have scored this shape positive: 16 - (8 + 2 + 0) = +6.
  ParallelPassFixedPoint oldChildSetup{};
  check(parallelPassFixedPointFromUnsigned(small.childCount, oldChildSetup),
        "old-formula child setup term converts");
  ParallelPassFixedPoint oldTotalCost{};
  check(parallelPassFixedPointAdd(smallCost.criticalPath, oldChildSetup,
                                  oldTotalCost) &&
            parallelPassFixedPointAdd(oldTotalCost, smallCost.imbalance,
                                      oldTotalCost),
        "old-formula total cost sums");
  ParallelPassFixedPoint oldBenefit{};
  check(parallelPassFixedPointSubtract(smallCost.serialWork, oldTotalCost,
                                       oldBenefit) &&
            oldBenefit.raw == 6ll * ParallelPassFixedPoint::kFraction,
        "under the pre-calibration formula this shape would have scored "
        "a positive benefit (+6 draw-equivalents)");

  // The calibrated formula scores the same shape negative:
  // 16 - (8 + 4 + 0 + 32) = -28.
  ParallelPassFixedPoint newTotalCost{};
  check(parallelPassFixedPointAdd(smallCost.criticalPath, smallCost.childSetup,
                                  newTotalCost) &&
            parallelPassFixedPointAdd(newTotalCost, smallCost.imbalance,
                                      newTotalCost) &&
            parallelPassFixedPointAdd(newTotalCost, smallCost.perPassOverhead,
                                      newTotalCost),
        "calibrated total cost sums");
  ParallelPassFixedPoint newBenefit{};
  check(parallelPassFixedPointSubtract(smallCost.serialWork, newTotalCost,
                                       newBenefit) &&
            newBenefit.raw == -28ll * ParallelPassFixedPoint::kFraction,
        "the calibrated formula scores the same small-pass shape as a "
        "non-positive benefit (-28 draw-equivalents), which is exactly the "
        "sign flip that drives selectParallelPassCandidate's "
        "NonPositiveBenefit fallback for this class of pass");
}

}  // namespace

int main() {
  // The typed certificate-invalid breakdown pin
  // (proofCoreAdapterGatesEveryProductionCandidate) drives the real
  // `dxmt9::perf::countParallelPassAdapter` writer, whose atomics are
  // compiled to a no-op unless perf counters are enabled. `enabled()` caches
  // the env read in a function-local static on first use, so this must be
  // set before any perf counter call in the process.
  setenv("DXMT_PERF_COUNTERS", "1", /*overwrite=*/1);
  try {
    eligibilityAndSelectionAreTypedAndBounded();
    coordinatorProofSnapshotRequiresEveryPreEffectFact();
    wmtParentChildAdapterCreatesAndJoinsMetalEncoders();
    explicitParallelSubdivisionIsEvenAndBounded();
    parallelPassDrawQuantumEnvKnobResolvesClampsAndScalesEligibility();
    sealedPassDrawBucketHistogramConservesAgainstSealedCount();
    wholeCommandSubdivisionIsOrderedBoundedAndFailClosed();
    wholeCommandProducerPreservesCoverageAndFirstLocators();
    passLocalProducerFindsBoundedCompletePasses();
    multiPassAndAttachmentBoundariesStayIndependent();
    activeReplayOrderAndPartialClearDriveExactBoundaries();
    producerRejectsControlsFragmentsHazardsAndBounds();
    coordinatorCommandsSegmentPassIntervals();
    childBoundsAndPerfGateFailClosed();
    fakeChildrenPreserveOwnershipOrderingAndExactlyOnceReplay();
    malformedPlansFailClosedBeforeParentPreparation();
    failuresSeparatePreEffectFallbackFromPostEffectFailStop();
    economicsClassifierIsPureBoundedAndEnforcedBeforeEffects();
    parallelPassImbalanceBoundDecouplesFromEligibilityQuantum();
    semanticPlanMutationAndCoverageProofsFailClosed();
    fixedPointAndCandidateSelectionAreCheckedAndPermutationIndependent();
    producerOutputFeedsSynchronousSemanticValidator();
    streamingCoverageFoldMatchesStoredRowReference();
    wideWholeCommandChildrenCertify();
    passLocalEpochProofIsIndependentlyReDerived();
    boundaryAdjacentPassIdentityCertifies();
    proofCoreAdapterGatesEveryProductionCandidate();
    parallelPassCostCalibrationConstantsChangeSelectionDirection();
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
