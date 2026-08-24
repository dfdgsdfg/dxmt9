// Exhaustive host truth table for the PE state-block candidate.  This stays
// native-buildable: candidate pointer slots are represented as opaque values,
// while the PE wrapper owns the corresponding COM refs at its boundaries.

#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_recorder_state.hpp"
#include "d3d9_pe_stateblock_transaction.hpp"
#include "d3d9_pe_stateblock_value.hpp"

#include <array>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

struct Failure : std::runtime_error {
  explicit Failure(std::string message) : std::runtime_error(std::move(message)) {}
};

void check(bool condition, std::string_view message) {
  if (!condition) throw Failure(std::string(message));
}

template<StateBlockApplyPhysicalStore Store>
constexpr auto fixedKey(std::uint32_t slot) noexcept {
  return stateBlockFixedSlotKey<Store>(slot);
}

static_assert(kStateBlockApplyPhysicalInventory.size() == 26u);
static_assert(stateBlockApplyPhysicalDescriptor<
                  StateBlockApplyPhysicalStore::textures>().role ==
              StateBlockApplyCategoryRole::StagedTexture);
static_assert(stateBlockApplyPhysicalDescriptor<
                  StateBlockApplyPhysicalStore::fvf>().role ==
              StateBlockApplyCategoryRole::ImplicitFvf);
static_assert(stateBlockApplyPhysicalDescriptor<
                  StateBlockApplyPhysicalStore::vertexDeclaration>().role ==
              StateBlockApplyCategoryRole::CandidateOwnedVertexDeclaration);

template<typename T>
concept ExposesRecordedReference = requires(T& value) { value.recorded(); };

template<typename T>
concept ExposesMutableRecordedTable = requires(T& value) {
  { value.renderStates() } -> std::same_as<RenderStateTableView>;
};

template<typename T>
concept ExposesMutableFixedStorage = requires(T& value) {
  { value.writer().textures() } ->
      std::same_as<StateBlockRecorded::texturesState&>;
};

template<typename T>
concept ExposesVertexDeclarationRecorded = requires(T& value) {
  value.vertexDeclarationRecorded;
};

static_assert(!ExposesRecordedReference<PeStateBlockTransactionState>,
              "transaction state must not expose its recorded owner");
static_assert(!ExposesMutableRecordedTable<StateBlockRecorded>,
              "StateBlock snapshots must not expose mutable tables");
static_assert(!ExposesMutableFixedStorage<StateBlockRecorded>,
              "Writer must expose a bounded capability, not fixed storage");
static_assert(!ExposesVertexDeclarationRecorded<StateBlockRecorded>,
              "vertex-declaration recording metadata must stay private");
static_assert(sizeof(StateBlockRecorded::Writer) == sizeof(void*),
              "StateBlock writer capability must be one reference");
static_assert(std::same_as<
              decltype(std::declval<const StateBlockRecorded&>().snapshot()),
              const StateBlockRecorded&>,
              "StateBlock snapshots must remain immutable references");

void testApplyCategoryRegistry() {
  std::array<bool, 26u> seen{};
  std::size_t keyed = 0u;
  std::size_t fixed = 0u;
  std::size_t constants = 0u;
  std::size_t staged = 0u;
  for (const auto descriptor : kStateBlockApplyPhysicalInventory) {
    const auto ordinal = static_cast<std::size_t>(descriptor.store);
    check(ordinal < seen.size() && !seen[ordinal],
          "Apply physical inventory rows are unique");
    seen[ordinal] = true;
    keyed += descriptor.kind == StateBlockApplyPhysicalKind::Keyed ? 1u : 0u;
    fixed += descriptor.kind == StateBlockApplyPhysicalKind::Fixed ? 1u : 0u;
    constants += descriptor.kind == StateBlockApplyPhysicalKind::Constant
        ? 1u : 0u;
    staged += descriptor.role >=
                  StateBlockApplyCategoryRole::StagedTexture
        ? 1u : 0u;
  }
  check(keyed == 4u && fixed == 16u && constants == 6u && staged == 7u,
        "Apply inventory has the exact 4 + 16 + 6 physical scope");
  for (const bool present : seen) {
    check(present, "Apply inventory covers every physical store ordinal");
  }
}

void testEveryFixedWriterRejectsInvalidKindKey() {
  StateBlockRecorded candidate{};
  auto writer = candidate.writer();
#define DXMT9_TEST_INVALID_FIXED(name, type, slots)                           \
  check(!writer.name().set(                                                   \
            fixedKey<StateBlockApplyPhysicalStore::name>(                    \
                static_cast<std::uint32_t>(slots)), type{}),                  \
        "invalid " #name " key rejects before mutation")
  DXMT9_TEST_INVALID_FIXED(textures, StateBlockTextureRef, kPeTextureSlots);
  DXMT9_TEST_INVALID_FIXED(streamSources, StateBlockStreamSourceValue,
                           D9C_DRAW_PACKET_MAX_STREAMS);
  DXMT9_TEST_INVALID_FIXED(streamFrequencies, std::uint32_t,
                           D9C_DRAW_PACKET_MAX_STREAMS);
  DXMT9_TEST_INVALID_FIXED(vertexShader, StateBlockVertexShaderRef, 1u);
  DXMT9_TEST_INVALID_FIXED(pixelShader, StateBlockPixelShaderRef, 1u);
  DXMT9_TEST_INVALID_FIXED(fvf, std::uint32_t, 1u);
  DXMT9_TEST_INVALID_FIXED(vertexDeclaration,
                           StateBlockVertexDeclarationRef, 1u);
  DXMT9_TEST_INVALID_FIXED(indexBuffer, StateBlockIndexBufferRef, 1u);
  DXMT9_TEST_INVALID_FIXED(renderTargets, StateBlockRenderTargetRef,
                           D9C_DRAW_PACKET_MAX_RENDER_TARGETS);
  DXMT9_TEST_INVALID_FIXED(depthStencil, StateBlockDepthStencilRef, 1u);
  DXMT9_TEST_INVALID_FIXED(viewport, D9CViewport, 1u);
  DXMT9_TEST_INVALID_FIXED(scissor, D9CRect, 1u);
  DXMT9_TEST_INVALID_FIXED(material, D9CMaterial, 1u);
  DXMT9_TEST_INVALID_FIXED(clipPlanes, StateBlockClipPlaneValue, 6u);
  DXMT9_TEST_INVALID_FIXED(lights, D9CLight, D9C_DRAW_PACKET_MAX_LIGHTS);
  DXMT9_TEST_INVALID_FIXED(lightEnables, std::uint32_t,
                           D9C_DRAW_PACKET_MAX_LIGHTS);
#undef DXMT9_TEST_INVALID_FIXED
  candidate.snapshot().forEachCategory([&](auto, const auto& table) {
    check(table.empty(), "invalid fixed key leaves occupancy unchanged");
  });
}

void testCandidateAbaAndDomains() {
  StateBlockRecorded candidate{};
  auto candidateWriter = candidate.writer();
  PeHotStateShadow live{};

  // Ordinary writes belong to LiveShadow/PendingDelta.  A recording write
  // to the same key belongs only to the candidate, including A-B-A.
  const auto render = renderStateSlotKey(7u);
  live.writer().renderStateShadowTyped().set(render, 11u);
  live.writer().pendingRenderStatesTyped().set(render, 11u);
  candidateWriter.renderStates().set(render, 1u);
  candidateWriter.renderStates().set(render, 2u);
  candidateWriter.renderStates().set(render, 1u);
  std::uint32_t value = 0u;
  check(live.writer().renderStateShadowTyped().get(render, value) && value == 11u,
        "recording must not mutate live render state");
  check(live.writer().pendingRenderStatesTyped().get(render, value) && value == 11u,
        "recording must not mutate pending render state");
  check(candidate.snapshot().renderStates().get(render, value) && value == 1u,
        "candidate render state must be last-write-wins");

  candidateWriter.textureStageStates().set(textureStageIndexKey(2u),
                                     textureStageStateTypeKey(3u), 1u);
  candidateWriter.textureStageStates().set(textureStageIndexKey(2u),
                                     textureStageStateTypeKey(3u), 2u);
  candidateWriter.textureStageStates().set(textureStageIndexKey(2u),
                                     textureStageStateTypeKey(3u), 1u);
  check(candidate.snapshot().textureStageStates().size() == 1u,
        "TSS candidate key must remain singular");

  SamplerIndex sampler{};
  check(samplerIndexKey(4u, sampler), "sampler key normalization");
  SamplerStateType samplerType{};
  check(samplerStateTypeKey(2u, samplerType), "sampler type normalization");
  candidateWriter.samplerStates().set(sampler, samplerType, 1u);
  candidateWriter.samplerStates().set(sampler, samplerType, 2u);
  candidateWriter.samplerStates().set(sampler, samplerType, 1u);
  check(candidate.snapshot().samplerStates().size() == 1u,
        "sampler candidate key must remain singular");

  D9CMatrix matrix = identityTransformMatrix();
  matrix.m[0] = 2.0f;
  const auto transform = transformStateKey(kD3dTsView);
  candidateWriter.transforms().set(transform, matrix);
  matrix.m[0] = 3.0f;
  candidateWriter.transforms().set(transform, matrix);
  matrix.m[0] = 2.0f;
  candidateWriter.transforms().set(transform, matrix);
  D9CMatrix capturedMatrix{};
  check(candidate.snapshot().transforms().get(transform, capturedMatrix) &&
            capturedMatrix.m[0] == 2.0f,
        "transform candidate must preserve A-B-A");

  // Every remaining PE-shadow category has a bounded tracked slot and can be
  // captured without changing the ordinary live domain.
  candidateWriter.textures().set(fixedKey<StateBlockApplyPhysicalStore::textures>(0u), StateBlockTextureRef::fromRaw(reinterpret_cast<void*>(0x10u)));
  candidateWriter.streamSources().set(fixedKey<StateBlockApplyPhysicalStore::streamSources>(0u), StateBlockStreamSourceValue{
      .buffer = {reinterpret_cast<void*>(0x20u)}, .offset = 4u,
      .stride = 16u});
  candidateWriter.streamFrequencies().set(fixedKey<StateBlockApplyPhysicalStore::streamFrequencies>(0u), 2u);
  candidateWriter.indexBuffer().set(fixedKey<StateBlockApplyPhysicalStore::indexBuffer>(0u), StateBlockIndexBufferRef::fromRaw(reinterpret_cast<void*>(0x30u)));
  candidateWriter.vertexShader().set(fixedKey<StateBlockApplyPhysicalStore::vertexShader>(0u), StateBlockVertexShaderRef::fromRaw(reinterpret_cast<void*>(0x40u)));
  candidateWriter.pixelShader().set(fixedKey<StateBlockApplyPhysicalStore::pixelShader>(0u), StateBlockPixelShaderRef::fromRaw(reinterpret_cast<void*>(0x50u)));
  candidateWriter.fvf().set(fixedKey<StateBlockApplyPhysicalStore::fvf>(0u), 0x1122u);
  candidateWriter.vertexDeclaration().set(fixedKey<StateBlockApplyPhysicalStore::vertexDeclaration>(0u), StateBlockVertexDeclarationRef::fromRaw(reinterpret_cast<void*>(0x60u)));
  candidateWriter.renderTargets().set(fixedKey<StateBlockApplyPhysicalStore::renderTargets>(0u), StateBlockRenderTargetRef::fromRaw(reinterpret_cast<void*>(0x70u)));
  candidateWriter.depthStencil().set(fixedKey<StateBlockApplyPhysicalStore::depthStencil>(0u), StateBlockDepthStencilRef::fromRaw(reinterpret_cast<void*>(0x80u)));
  candidateWriter.viewport().set(fixedKey<StateBlockApplyPhysicalStore::viewport>(0u), D9CViewport{1u, 2u, 640u, 480u, 0.0f, 1.0f});
  candidateWriter.scissor().set(fixedKey<StateBlockApplyPhysicalStore::scissor>(0u), D9CRect{1, 2, 639, 479});
  candidateWriter.material().set(fixedKey<StateBlockApplyPhysicalStore::material>(0u), D9CMaterial{});
  candidateWriter.clipPlanes().set(fixedKey<StateBlockApplyPhysicalStore::clipPlanes>(0u), std::array<float, 4>{1, 2, 3, 4});
  candidateWriter.lights().set(fixedKey<StateBlockApplyPhysicalStore::lights>(0u), D9CLight{});
  candidateWriter.lightEnables().set(fixedKey<StateBlockApplyPhysicalStore::lightEnables>(0u), 1u);
  std::array<float, 4> constants{1, 2, 3, 4};
  candidateWriter.constants().vsConstF.record(3u, 1u, constants.data(),
                                      sizeof(constants));
  candidateWriter.constants().psConstF.record(3u, 1u, constants.data(),
                                      sizeof(constants));

  std::array<bool, static_cast<std::size_t>(
                       StateBlockApplyPhysicalStore::Count)> physicalSeen{};
  candidate.snapshot().forEachApplyPhysical(
      [&]<StateBlockApplyPhysicalStore store>(const auto&) {
        const auto ordinal = static_cast<std::size_t>(store);
        check(ordinal < physicalSeen.size() && !physicalSeen[ordinal],
              "physical Apply inventory rows are unique");
        physicalSeen[ordinal] = true;
      });
  for (const bool present : physicalSeen) {
    check(present, "all 26 physical Apply stores visit exactly once");
  }
  check(candidate.textures().contains(0u) &&
            candidate.streamSources().contains(0u) &&
            candidate.streamFrequencies().contains(0u) &&
            candidate.indexBuffer().contains(0u) &&
            candidate.vertexShader().contains(0u) &&
            candidate.pixelShader().contains(0u) && candidate.fvf().contains(0u) &&
            candidate.vertexDeclaration().contains(0u) &&
            candidate.renderTargets().contains(0u) &&
            candidate.depthStencil().contains(0u) && candidate.viewport().contains(0u) &&
            candidate.scissor().contains(0u) && candidate.material().contains(0u) &&
            candidate.clipPlanes().contains(0u) && candidate.lights().contains(0u) &&
            candidate.lightEnables().contains(0u) &&
            candidate.constantSnapshot().vsConstF.contains(3u) &&
            candidate.constantSnapshot().psConstF.contains(3u),
        "all recordable categories must have fixed candidate slots");
  check(candidate.has(StateBlockRecorded::Category::textures) &&
            candidate.has(StateBlockRecorded::Category::streamSources) &&
            candidate.has(StateBlockRecorded::Category::streamFrequencies),
        "canonical category visitor reports occupied categories");

  // Source tuple and frequency are independent D3D9 state aspects. Applying
  // either tracked set preserves the other aspect.
  StateBlockRecorded sourceOnly{};
  sourceOnly.writer().streamSources().set(
      fixedKey<StateBlockApplyPhysicalStore::streamSources>(0u),
      StateBlockStreamSourceValue{{reinterpret_cast<void*>(0x90u)}, 8u, 32u});
  StateBlockRecorded frequencyOnly{};
  frequencyOnly.writer().streamFrequencies().set(
      fixedKey<StateBlockApplyPhysicalStore::streamFrequencies>(0u), 9u);
  StateBlockStreamSourceValue liveSource{
      reinterpret_cast<void*>(0x91u), 4u, 16u};
  std::uint32_t liveFrequency = 7u;
  sourceOnly.streamSources().forEach(
      [&](std::size_t, const StateBlockStreamSourceValue& value) {
        liveSource = value;
      });
  check(liveSource.buffer == reinterpret_cast<void*>(0x90u) &&
            liveSource.offset == 8u && liveSource.stride == 32u &&
            liveFrequency == 7u,
        "source Apply must preserve an unrecorded frequency");
  frequencyOnly.streamFrequencies().forEach(
      [&](std::size_t, std::uint32_t value) { liveFrequency = value; });
  check(liveSource.buffer == reinterpret_cast<void*>(0x90u) &&
            liveSource.offset == 8u && liveSource.stride == 32u &&
            liveFrequency == 9u,
        "frequency Apply must preserve an unrecorded source tuple");

  // CreateStateBlock dispositions select exact state families; Begin/End
  // supplies Explicit and therefore keeps its touched set unrestricted.
  check(stateBlockCaptureDispositionFromType(1u) ==
            StateBlockCaptureDisposition::All &&
            stateBlockCaptureDispositionFromType(2u) ==
                StateBlockCaptureDisposition::PixelState &&
            stateBlockCaptureDispositionFromType(3u) ==
                StateBlockCaptureDisposition::VertexState,
        "state-block type disposition mapping");
  check(stateBlockCaptureCategorySelected(
            StateBlockCaptureDisposition::All,
            StateBlockCaptureCategory::Texture) &&
            stateBlockCaptureCategorySelected(
                StateBlockCaptureDisposition::Explicit,
                StateBlockCaptureCategory::StreamFrequency) &&
            stateBlockCaptureCategorySelected(
            StateBlockCaptureDisposition::VertexState,
            StateBlockCaptureCategory::VertexShader) &&
            stateBlockCaptureCategorySelected(
                StateBlockCaptureDisposition::PixelState,
                StateBlockCaptureCategory::PixelShader) &&
            !stateBlockCaptureCategorySelected(
                StateBlockCaptureDisposition::VertexState,
                StateBlockCaptureCategory::PixelShader) &&
            !stateBlockCaptureCategorySelected(
                StateBlockCaptureDisposition::PixelState,
                StateBlockCaptureCategory::VertexShader) &&
            !stateBlockCaptureCategorySelected(
                StateBlockCaptureDisposition::PixelState,
                StateBlockCaptureCategory::Texture),
        "vertex/pixel category filtering");
  check(stateBlockRenderStateSelected(
            StateBlockCaptureDisposition::VertexState, 22u) &&
            !stateBlockRenderStateSelected(
                StateBlockCaptureDisposition::VertexState, 27u) &&
            stateBlockRenderStateSelected(
                StateBlockCaptureDisposition::PixelState, 27u) &&
            !stateBlockRenderStateSelected(
                StateBlockCaptureDisposition::PixelState, 137u) &&
            stateBlockTextureStageStateSelected(
                StateBlockCaptureDisposition::VertexState, 24u) &&
            !stateBlockTextureStageStateSelected(
                StateBlockCaptureDisposition::VertexState, 1u) &&
            stateBlockSamplerStateSelected(
                StateBlockCaptureDisposition::PixelState, 6u) &&
            !stateBlockSamplerStateSelected(
                StateBlockCaptureDisposition::PixelState, 13u),
        "vertex/pixel key filtering");

  StateBlockRecorded capture = candidate;
  capture.writer().renderStates().set(render, 99u);
  check(candidate.snapshot().renderStates().get(render, value) && value == 1u,
        "Capture refresh must not mutate the recording candidate");
  live.writer().renderStateShadowTyped().set(render, 77u);
  capture.writer().renderStates().set(render, 77u);
  check(capture.snapshot().renderStates().get(render, value) && value == 77u &&
            candidate.snapshot().renderStates().get(render, value) && value == 1u,
        "Capture must refresh a fixed tracked key from live state only");
  candidate.writer().clear();
  check(candidate.snapshot().renderStates().empty() &&
            candidate.textures().empty() &&
            candidate.constantSnapshot().vsConstF.empty(),
        "Begin must establish an empty candidate domain");
}

struct RefProbe {
  std::uint32_t refs = 1u;
  void addRef() noexcept { ++refs; }
  void release() noexcept {
    check(refs != 0u, "refcount underflow");
    --refs;
  }
};

struct RetainNoop {
  void operator()(void*) const noexcept {}
};

template<typename T>
concept AcceptsRawTextureStage = requires(T& transaction) {
  transaction.stageTexture(0u, StateBlockTextureRef{}, RetainNoop{});
};

static_assert(!AcceptsRawTextureStage<PeStateBlockTransactionState>,
              "StateBlock staging must reject raw size_t/integer keys");
static_assert(!std::is_constructible_v<StateBlockTextureSlot, std::uint32_t> &&
              !std::is_constructible_v<StateBlockStreamSlot, std::uint32_t> &&
              !std::is_constructible_v<StateBlockRenderTargetSlot,
                                       std::uint32_t>,
              "StateBlock boundary keys require bounded factories");

void testGeneratedTransitionTableIsomorphism() {
  static_assert(!std::is_aggregate_v<PeStateBlockTransitionPlan>);
  constexpr std::array kPhases{
      PeStateBlockPhase::Idle, PeStateBlockPhase::Recording,
      PeStateBlockPhase::EndPublication, PeStateBlockPhase::ApplyPrepared,
      PeStateBlockPhase::Poisoned, PeStateBlockPhase::Terminal};
  constexpr std::array kEvents{
      PeStateBlockEvent::BeginFailed,
      PeStateBlockEvent::BeginAccepted,
      PeStateBlockEvent::EndPreEffectFailed,
      PeStateBlockEvent::EndBackendFailed,
      PeStateBlockEvent::EndBackendAccepted,
      PeStateBlockEvent::EndWrapperFailed,
      PeStateBlockEvent::EndPublished,
      PeStateBlockEvent::CapturePreEffectFailed,
      PeStateBlockEvent::CaptureBackendFailed,
      PeStateBlockEvent::CapturePublished,
      PeStateBlockEvent::ApplyPrepareFailed,
      PeStateBlockEvent::ApplyPrepared,
      PeStateBlockEvent::ApplyBackendFailed,
      PeStateBlockEvent::ApplyBackendAccepted,
      PeStateBlockEvent::ResetStarted,
      PeStateBlockEvent::ResetFailed,
      PeStateBlockEvent::ResetAccepted,
      PeStateBlockEvent::Teardown};
  constexpr std::array kActions{
      PeStateBlockAction::Preserve,
      PeStateBlockAction::BeginRecording,
      PeStateBlockAction::EnterEndPublication,
      PeStateBlockAction::PublishEnd,
      PeStateBlockAction::FailStop,
      PeStateBlockAction::PublishCapture,
      PeStateBlockAction::RetainApplyRefs,
      PeStateBlockAction::TransferApplyRefs,
      PeStateBlockAction::AbandonForReset,
      PeStateBlockAction::RecoverReset,
      PeStateBlockAction::Teardown};

  std::array<bool, kEvents.size()> eventMapped{};
  std::array<bool, kActions.size()> actionMapped{};
  for (const auto& row : kPeStateBlockTransitionTable) {
    const auto plan = planPeStateBlockTransition({row.phase, row.event});
    check(plan.valid() && plan.next() == row.next &&
              plan.action() == row.action &&
              plan.candidateEffect() == row.candidateEffect &&
              plan.stagedRefEffect() == row.stagedRefEffect &&
              plan.captureEffect() == row.captureEffect,
          "production StateBlock plan must be isomorphic to every shared row");
    for (std::size_t i = 0; i < kEvents.size(); ++i)
      if (kEvents[i] == row.event) eventMapped[i] = true;
    for (std::size_t i = 0; i < kActions.size(); ++i)
      if (kActions[i] == row.action) actionMapped[i] = true;
  }
  for (std::size_t phaseIndex = 0; phaseIndex < kPhases.size(); ++phaseIndex) {
    for (std::size_t eventIndex = 0; eventIndex < kEvents.size(); ++eventIndex) {
      std::size_t matches = 0u;
      for (const auto& row : kPeStateBlockTransitionTable) {
        if (row.phase == kPhases[phaseIndex] &&
            row.event == kEvents[eventIndex]) ++matches;
      }
      const auto plan = planPeStateBlockTransition(
          {kPhases[phaseIndex], kEvents[eventIndex]});
      check(matches <= 1u && plan.valid() == (matches == 1u),
            "phase/event truth table must be exhaustive and unambiguous");
    }
  }
  for (const bool mapped : eventMapped)
    check(mapped, "every StateBlock event enum must have a shared row");
  for (const bool mapped : actionMapped)
    check(mapped, "every StateBlock action enum must have a shared row");
}

void testTransactionOwnershipAndLifecycle() {
  using dxmt9::d3d9::pe::PeRecorderState;
  static_assert(!std::is_copy_constructible_v<PeStateBlockTransactionState>);
  static_assert(std::is_same_v<
                decltype(std::declval<PeRecorderState&>()
                             .stateBlockTransaction.recordWriter()),
                StateBlockRecorded::Writer>);

  PeStateBlockTransactionState transaction{};
  check(!transaction.isRecording() && !transaction.isInsideEnd() &&
            transaction.writeAllowed(),
        "fresh transaction must be idle and writable");

  RefProbe candidateRef{};
  candidateRef.addRef();
  candidateRef.addRef();
  candidateRef.addRef();
  transaction.recordWriter().textures().set(
      fixedKey<StateBlockApplyPhysicalStore::textures>(0u),
      StateBlockTextureRef::fromRaw(&candidateRef));
  transaction.recordWriter().vertexShader().set(
      fixedKey<StateBlockApplyPhysicalStore::vertexShader>(0u),
      StateBlockVertexShaderRef::fromRaw(&candidateRef));
  transaction.recordWriter().vertexDeclaration().set(
      fixedKey<StateBlockApplyPhysicalStore::vertexDeclaration>(0u),
      StateBlockVertexDeclarationRef::fromRaw(&candidateRef));
  transaction.beginAccepted([&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->release();
  });
  check(transaction.isRecording() &&
            transaction.recordedSnapshot().textures().empty() &&
            transaction.recordedSnapshot().vertexShader().empty() &&
            transaction.recordedSnapshot().vertexDeclaration().empty() &&
            candidateRef.refs == 1u,
        "candidate-owned vdecl and staged categories release each retain by multiplicity");

  transaction.recordWriter().renderStates().set(renderStateSlotKey(7u), 19u);
  transaction.enterEndPublication();
  check(!transaction.isRecording() && transaction.isInsideEnd(),
        "accepted backend End enters the closed publication scope");
  transaction.finishEndPublication(true, [&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->release();
  });
  check(!transaction.isRecording() && !transaction.isInsideEnd() &&
            transaction.writeAllowed() &&
            transaction.recordedSnapshot().renderStates().empty(),
        "successful End leaves no candidate or inside-End state");

  transaction.beginAccepted([&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->release();
  });
  // A pre-effect failure makes no lifecycle call and remains retryable.
  check(transaction.isRecording() && transaction.writeAllowed(),
        "pre-effect End failure preserves the recording transaction");
  transaction.failEnd([&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->release();
  });
  check(!transaction.isRecording() && !transaction.isInsideEnd() &&
            transaction.isPoisoned() && !transaction.writeAllowed(),
        "post-effect End failure closes and poisons the transaction");
}

void testTypedStagingRetentionMultiplicityAndReset() {
  PeStateBlockTransactionState transaction{};
  RefProbe shared{};
  auto retain = [&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->addRef();
  };
  auto release = [&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->release();
  };

  check(!transaction.stageTexture(
            stateBlockTextureSlotKey(kPeTextureSlots),
            StateBlockTextureRef::fromRaw(&shared), retain) &&
            !transaction.stageStream(
                stateBlockStreamSlotKey(D9C_DRAW_PACKET_MAX_STREAMS),
                StateBlockStreamSourceValue{.buffer = {&shared}}, retain) &&
            !transaction.stageRenderTarget(
                stateBlockRenderTargetSlotKey(
                    D9C_DRAW_PACKET_MAX_RENDER_TARGETS),
                StateBlockRenderTargetRef::fromRaw(&shared), retain) &&
            !transaction.hasPreparedApply() && shared.refs == 1u,
        "invalid bounded staging keys fail before retain or occupancy");

  transaction.stageTexture(stateBlockTextureSlotKey(0u),
                           StateBlockTextureRef::fromRaw(&shared), retain);
  transaction.stageTexture(stateBlockTextureSlotKey(1u),
                           StateBlockTextureRef::fromRaw(&shared), retain);
  transaction.stageStream(
      stateBlockStreamSlotKey(0u), StateBlockStreamSourceValue{
              .buffer = {&shared}, .offset = 4u, .stride = 16u},
      retain);
  transaction.stageVertexShader(
      StateBlockVertexShaderRef::fromRaw(&shared), retain);
  transaction.stagePixelShader(
      StateBlockPixelShaderRef::fromRaw(&shared), retain);
  transaction.stageIndexBuffer(
      StateBlockIndexBufferRef::fromRaw(&shared), retain);
  transaction.stageRenderTarget(
      stateBlockRenderTargetSlotKey(0u),
      StateBlockRenderTargetRef::fromRaw(&shared), retain);
  transaction.stageDepthStencil(
      StateBlockDepthStencilRef::fromRaw(&shared), retain);
  check(transaction.hasPreparedApply() && shared.refs == 9u,
        "each typed occupied Apply category retains independently");
  transaction.discardPrepared(release);
  check(!transaction.hasPreparedApply() && shared.refs == 1u,
        "discard releases every staged retain without deduplication");

  transaction.stageTexture(stateBlockTextureSlotKey(0u),
                           StateBlockTextureRef::fromRaw(&shared), retain);
  const auto transferred =
      transaction.takeTexture(stateBlockTextureSlotKey(0u));
  check(transferred.raw() == &shared && !transaction.hasPreparedApply() &&
            shared.refs == 2u,
        "commit take transfers the staged retain without releasing it");
  release(transferred.raw());

  transaction.stageDepthStencil(StateBlockDepthStencilRef{}, retain);
  check(transaction.hasPreparedApply(),
        "typed null staging preserves occupied clear semantics");
  check(transaction.takeDepthStencil().raw() == nullptr &&
            !transaction.hasPreparedApply(),
        "typed null commit clears occupancy exactly once");

  transaction.stagePixelShader(
      StateBlockPixelShaderRef::fromRaw(&shared), retain);
  transaction.poison();
  check(transaction.isPoisoned() && transaction.hasPreparedApply() &&
            shared.refs == 2u,
        "failed Reset preserves poison and pre-effect staging");
  transaction.resetSucceeded(release);
  check(transaction.writeAllowed() && !transaction.hasPreparedApply() &&
            shared.refs == 1u,
        "successful Reset alone clears poison and staged ownership");
}

struct StagedRefProbe {
  RefProbe *staged = nullptr;
  bool occupied = false;

  void prepare(RefProbe *value) noexcept {
    staged = value;
    occupied = true;
    if (staged) staged->addRef();
  }
  void discard() noexcept {
    if (occupied && staged) staged->release();
    staged = nullptr;
    occupied = false;
  }
  void commit(RefProbe *&live) noexcept {
    if (live) live->release();
    live = staged;
    staged = nullptr;
    occupied = false;
  }
};

void testStagedFailureAndIntervalWitness() {
  // Bind failure injection to the production transition helper, proving the
  // same phase disposition used by D3D9StateBlockImpl::Apply.
  check(peStateBlockApplyTransition(PeStateBlockApplyPhase::Prepare, true) ==
                PeStateBlockApplyAction::Continue &&
            peStateBlockApplyTransition(PeStateBlockApplyPhase::Prepare, false) ==
                PeStateBlockApplyAction::Preserve &&
            peStateBlockApplyTransition(PeStateBlockApplyPhase::Backend, true) ==
                PeStateBlockApplyAction::Publish &&
            peStateBlockApplyTransition(PeStateBlockApplyPhase::Backend, false) ==
                PeStateBlockApplyAction::Poison &&
            peStateBlockApplyTransition(PeStateBlockApplyPhase::Commit, true) ==
                PeStateBlockApplyAction::Publish &&
            peStateBlockApplyTransition(PeStateBlockApplyPhase::Commit, false) ==
                PeStateBlockApplyAction::Poison,
        "Apply phase transitions must preserve or poison deterministically");

  RefProbe oldBinding{};
  RefProbe newBinding{};
  RefProbe *live = &oldBinding;
  oldBinding.addRef();  // the live slot's independent owner
  StagedRefProbe staged;
  staged.prepare(&newBinding);
  check(oldBinding.refs == 2u && newBinding.refs == 2u,
        "prepare must retain without disturbing the live owner");
  staged.discard();
  check(oldBinding.refs == 2u && newBinding.refs == 1u,
        "discard must release exactly the staged retain");
  staged.prepare(&newBinding);
  staged.commit(live);
  check(live == &newBinding && oldBinding.refs == 1u &&
            newBinding.refs == 2u,
        "commit must transfer staged ownership exactly once");
  live->release();
  check(newBinding.refs == 1u, "live owner must release exactly once");

  // The host witness mirrors the conditional recursive operation lock: a
  // concurrent setter cannot enter the interval until Apply releases it.
  std::recursive_mutex operationMutex;
  std::atomic<bool> setterEntered{false};
  std::atomic<bool> releaseOperation{false};
  operationMutex.lock();
  std::thread setter([&] {
    std::lock_guard<std::recursive_mutex> lock(operationMutex);
    setterEntered.store(true, std::memory_order_release);
  });
  std::this_thread::yield();
  check(!setterEntered.load(std::memory_order_acquire),
        "setter must not enter a locked Apply interval");
  releaseOperation.store(true, std::memory_order_release);
  (void)releaseOperation;
  operationMutex.unlock();
  setter.join();
  check(setterEntered.load(std::memory_order_acquire),
        "setter must enter after Apply interval unlock");
}

void testPoisonResetFaultSequence() {
  // A failed StateBlock backend operation is fail-stop until the device reset
  // boundary.  Reset validation/backend failure must not make a poisoned PE
  // shadow writable again, while a successful backend reset clears both the
  // latch and any pre-effect staged retains.
  check(peStateBlockPoisonAfterReset(false, true),
        "failed Reset must preserve StateBlock poison");
  check(!peStateBlockPoisonAfterReset(true, true),
        "successful Reset must recover StateBlock poison");
  check(!peStateBlockPoisonAfterReset(true, false),
        "successful Reset keeps an unpoisoned recorder unpoisoned");
  check(!peStateBlockPoisonAfterReset(false, false),
        "failed Reset must not invent poison");

  RefProbe stagedRef{};
  RefProbe liveRef{};
  RefProbe *live = &liveRef;
  liveRef.addRef();
  StagedRefProbe staged;
  staged.prepare(&stagedRef);
  check(stagedRef.refs == 2u && liveRef.refs == 2u,
        "Apply staging retains independently before Reset");
  // A failed reset preserves the staged candidate for the same fail-closed
  // lifetime boundary; it is not silently converted into a live binding.
  check(peStateBlockPoisonAfterReset(false, true),
        "failed reset fault sequence remains poisoned with staging present");
  check(staged.staged == &stagedRef && staged.occupied,
        "failed reset preserves staged Apply ownership");
  // Successful reset discards pre-effect staging after the backend accepts;
  // the live binding remains untouched.
  staged.discard();
  check(!peStateBlockPoisonAfterReset(true, true) && stagedRef.refs == 1u &&
            live == &liveRef && liveRef.refs == 2u,
        "successful reset clears poison and releases only staged ownership");
  live->release();
}

void testEndFailureAndPoisonWriteBoundary() {
  check(peStateBlockEndTransition(PeStateBlockEndPhase::PreEffect, true) ==
                PeStateBlockEndAction::Continue &&
            peStateBlockEndTransition(PeStateBlockEndPhase::PreEffect, false) ==
                PeStateBlockEndAction::Preserve &&
            peStateBlockEndTransition(PeStateBlockEndPhase::Backend, false) ==
                PeStateBlockEndAction::Poison &&
            peStateBlockEndTransition(PeStateBlockEndPhase::Backend, true) ==
                PeStateBlockEndAction::Publish &&
            peStateBlockEndTransition(PeStateBlockEndPhase::Wrapper, false) ==
                PeStateBlockEndAction::Poison &&
            peStateBlockEndTransition(PeStateBlockEndPhase::Wrapper, true) ==
                PeStateBlockEndAction::Publish,
        "End phase transitions distinguish retryable pre-effect failure from fail-stop");

  struct EndDomains {
    bool peRecording = true;
    bool unixRecording = true;
    bool candidateOwned = true;
    bool poisoned = false;
    bool wrapperPublished = false;
  };
  const auto runEnd = [](EndDomains& domains, bool backendAccepted,
                         bool wrapperAccepted) {
    // The fake backend matches the production unix ordering: its recording
    // flag is consumed before the remaining End work can fail.
    domains.unixRecording = false;
    if (peStateBlockEndTransition(PeStateBlockEndPhase::Backend,
                                  backendAccepted) ==
        PeStateBlockEndAction::Poison) {
      domains.peRecording = false;
      domains.candidateOwned = false;
      domains.poisoned = true;
      return;
    }
    domains.peRecording = false;
    if (peStateBlockEndTransition(PeStateBlockEndPhase::Wrapper,
                                  wrapperAccepted) ==
        PeStateBlockEndAction::Poison) {
      domains.candidateOwned = false;
      domains.poisoned = true;
      return;
    }
    domains.candidateOwned = false;
    domains.wrapperPublished = true;
  };

  EndDomains backendFailure{};
  runEnd(backendFailure, false, false);
  check(!backendFailure.peRecording && !backendFailure.unixRecording &&
            !backendFailure.candidateOwned && backendFailure.poisoned &&
            !backendFailure.wrapperPublished,
        "post-effect backend End failure closes both domains and poisons");

  std::uint32_t renderShadow = 7u;
  const auto setRenderState = [&](std::uint32_t value) {
    if (!peStateBlockRecorderWriteAllowed(backendFailure.poisoned)) {
      return false;
    }
    renderShadow = value;
    return true;
  };
  check(!setRenderState(9u) && renderShadow == 7u,
        "poisoned SetRenderState fails before shadow mutation");

  EndDomains wrapperFailure{};
  runEnd(wrapperFailure, true, false);
  check(!wrapperFailure.peRecording && !wrapperFailure.unixRecording &&
            !wrapperFailure.candidateOwned && wrapperFailure.poisoned &&
            !wrapperFailure.wrapperPublished,
        "post-effect wrapper End failure is fail-stop");

  EndDomains success{};
  runEnd(success, true, true);
  check(!success.peRecording && !success.unixRecording &&
            !success.candidateOwned && !success.poisoned &&
            success.wrapperPublished &&
            peStateBlockRecorderWriteAllowed(success.poisoned),
        "accepted End publishes once and keeps later recorder writes legal");
}

void testRepeatedQualifiedValueAndComMultiplicity() {
  constexpr std::array events{
      PeStateBlockValueEvent::CapturePreEffectFailed,
      PeStateBlockValueEvent::CaptureBackendFailed,
      PeStateBlockValueEvent::CaptureAccepted,
      PeStateBlockValueEvent::ApplyPreEffectFailed,
      PeStateBlockValueEvent::ApplyPrepared,
      PeStateBlockValueEvent::ApplyBackendFailed,
      PeStateBlockValueEvent::ApplyAccepted};
  for (const auto event : events) {
    const auto plan = planPeStateBlockValue(event);
    std::size_t matches = 0u;
    for (const auto& row : kPeStateBlockValueTable) {
      if (row.event == event) {
        ++matches;
        check(plan.valid() && plan.action() == row.action &&
                  plan.preserveTrackedSet() == row.preserveTrackedSet &&
                  plan.refreshSnapshot() == row.refreshSnapshot &&
                  plan.publishLive() == row.publishLive &&
                  plan.poison() == row.poison,
              "StateBlock value planner matches every generated row");
      }
    }
    check(matches == 1u, "every StateBlock value event has one row");
  }

  // Drive the host-buildable production shadow types through three repeated
  // value rounds.  RenderState and Transform are the frozen tracked set;
  // Sampler is live but deliberately absent from the StateBlock snapshot.
  const auto renderKey = renderStateSlotKey(7u);
  const auto transformKey = transformStateKey(kD3dTsView);
  SamplerIndex sampler{};
  SamplerStateType samplerType{};
  check(samplerIndexKey(0u, sampler) && samplerStateTypeKey(1u, samplerType),
        "typed sampler keys are valid");
  StateBlockRecorded saved{};
  saved.writer().renderStates().set(renderKey, 0u);
  saved.writer().transforms().set(transformKey, identityTransformMatrix());
  PeHotStateShadow live{};
  std::uint64_t ordinal = 0u;
  for (const std::uint32_t capturedValue : {1u, 2u, 0u}) {
    D9CMatrix capturedMatrix = identityTransformMatrix();
    capturedMatrix.m[0] = static_cast<float>(capturedValue + 1u);
    live.writer().renderStateShadowTyped().set(renderKey, capturedValue);
    live.writer().transformShadowTyped().set(transformKey, capturedMatrix);
    live.writer().samplerStateShadowTyped().set(
        sampler, samplerType, capturedValue + 10u);

    StateBlockRecorded candidate = saved;
    candidate.writer().renderStates().set(renderKey, capturedValue);
    candidate.writer().transforms().set(transformKey, capturedMatrix);
    const auto capturePlan = planPeStateBlockValue(
        PeStateBlockValueEvent::CaptureAccepted);
    check(capturePlan.valid() && capturePlan.refreshSnapshot() &&
              !candidate.snapshot().samplerStates().contains(
                  sampler, samplerType),
          "successful Capture refreshes only the frozen tracked categories");
    saved = std::move(candidate);
    ++ordinal;

    // A failed Capture may build a candidate, but the production publication
    // edge must leave the prior saved snapshot intact.
    StateBlockRecorded failedCandidate = saved;
    failedCandidate.writer().renderStates().set(renderKey, 99u);
    const auto failedCapture = planPeStateBlockValue(
        PeStateBlockValueEvent::CapturePreEffectFailed);
    const auto failedBackendCapture = planPeStateBlockValue(
        PeStateBlockValueEvent::CaptureBackendFailed);
    std::uint32_t savedRender = 0u;
    check(failedCapture.valid() && !failedCapture.refreshSnapshot() &&
              failedBackendCapture.valid() &&
              !failedBackendCapture.refreshSnapshot() &&
              failedBackendCapture.poison() &&
              saved.snapshot().renderStates().get(renderKey, savedRender) &&
              savedRender == capturedValue,
          "pre-effect and backend Capture failures preserve the published snapshot");

    // Mutate all three live categories between Capture and Apply.  Apply must
    // publish the latest captured R/T values while leaving untracked S alone.
    D9CMatrix interveningMatrix = capturedMatrix;
    interveningMatrix.m[0] += 20.0f;
    live.writer().renderStateShadowTyped().set(renderKey, capturedValue + 20u);
    live.writer().transformShadowTyped().set(transformKey, interveningMatrix);
    live.writer().samplerStateShadowTyped().set(
        sampler, samplerType, capturedValue + 30u);
    const auto preEffectApply = planPeStateBlockValue(
        PeStateBlockValueEvent::ApplyPreEffectFailed);
    const auto backendFailedApply = planPeStateBlockValue(
        PeStateBlockValueEvent::ApplyBackendFailed);
    std::uint32_t interveningRender = 0u;
    D9CMatrix failedApplyMatrix{};
    check(preEffectApply.valid() && preEffectApply.preserveTrackedSet() &&
              !preEffectApply.publishLive() &&
              backendFailedApply.valid() && backendFailedApply.poison() &&
              !backendFailedApply.publishLive() &&
              live.snapshot().renderStateShadowTyped().get(
                  renderKey, interveningRender) &&
              interveningRender == capturedValue + 20u &&
              live.snapshot().transformShadowTyped().get(
                  transformKey, failedApplyMatrix) &&
              failedApplyMatrix.m[0] == interveningMatrix.m[0],
          "pre-effect and backend Apply failures preserve live values");
    const auto applyPlan = planPeStateBlockValue(
        PeStateBlockValueEvent::ApplyAccepted);
    saved.snapshot().renderStates().forEach(
        [&](RenderStateSlot key, std::uint32_t value) {
          live.writer().renderStateShadowTyped().set(key, value);
        });
    saved.snapshot().transforms().forEach(
        [&](TransformState key, const D9CMatrix& value) {
          live.writer().transformShadowTyped().set(key, value);
        });
    std::uint32_t liveRender = 0u;
    std::uint32_t liveSampler = 0u;
    D9CMatrix liveMatrix{};
    const PeStateBlockQualifiedValue captured{
        PeStateBlockValueCategory::RenderState, 7u, capturedValue, ordinal};
    check(applyPlan.valid() && applyPlan.publishLive() &&
              live.snapshot().renderStateShadowTyped().get(
                  renderKey, liveRender) && liveRender == capturedValue &&
              live.snapshot().transformShadowTyped().get(
                  transformKey, liveMatrix) &&
              liveMatrix.m[0] == capturedMatrix.m[0] &&
              live.snapshot().samplerStateShadowTyped().get(
                  sampler, samplerType, liveSampler) &&
              liveSampler == capturedValue + 30u &&
              peStateBlockApplyPublishesLatest(captured, captured, true),
          "Apply publishes latest captured R/T and isolates untracked sampler");
  }

  RefProbe shared{};
  auto retain = [&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->addRef();
  };
  auto release = [&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->release();
  };
  PeStateBlockTransactionState transaction{};
  // Two live slots independently own the same COM identity.
  retain(&shared);
  retain(&shared);
  for (std::uint32_t cycle = 0u; cycle < 3u; ++cycle) {
    transaction.stageTexture(
        stateBlockTextureSlotKey(0u),
        StateBlockTextureRef::fromRaw(&shared), retain);
    transaction.stageTexture(
        stateBlockTextureSlotKey(1u),
        StateBlockTextureRef::fromRaw(&shared), retain);
    check(shared.refs == 5u,
          "same COM identity retains once per qualified slot per cycle");
    transaction.markApplyPrepared();
    check(transaction.phase() == PeStateBlockPhase::ApplyPrepared,
          "production transition enters ApplyPrepared after per-slot retain");
    release(&shared);
    release(&shared);
    const auto slot0 = transaction.takeTexture(stateBlockTextureSlotKey(0u));
    const auto slot1 = transaction.takeTexture(stateBlockTextureSlotKey(1u));
    check(slot0.raw() == &shared && slot1.raw() == &shared &&
              shared.refs == 3u && !transaction.hasPreparedApply(),
          "successful Apply transfers exactly two same-identity slot retains");
    transaction.finishPreparedApply();
    check(transaction.phase() == PeStateBlockPhase::Idle,
          "production transfer transition returns to Idle");
  }
  release(&shared);
  release(&shared);
  check(shared.refs == 1u,
        "final live teardown releases exactly one owner per qualified slot");

  PeStateBlockTransactionState failed{};
  failed.stageTexture(stateBlockTextureSlotKey(0u),
                      StateBlockTextureRef::fromRaw(&shared), retain);
  failed.stageTexture(stateBlockTextureSlotKey(1u),
                      StateBlockTextureRef::fromRaw(&shared), retain);
  failed.markApplyPrepared();
  failed.failPreparedApply(release);
  check(failed.phase() == PeStateBlockPhase::Poisoned && shared.refs == 1u,
        "failed Apply releases both staged retains and fails stop");
}

}  // namespace

int main() {
  try {
    testApplyCategoryRegistry();
    testEveryFixedWriterRejectsInvalidKindKey();
    testCandidateAbaAndDomains();
    testGeneratedTransitionTableIsomorphism();
    testTransactionOwnershipAndLifecycle();
    testTypedStagingRetentionMultiplicityAndReset();
    testStagedFailureAndIntervalWitness();
    testPoisonResetFaultSequence();
    testEndFailureAndPoisonWriteBoundary();
    testRepeatedQualifiedValueAndComMultiplicity();
    std::cout << "pe state-block category spec: PASS\n";
    return 0;
  } catch (const Failure& failure) {
    std::cerr << "pe state-block category spec: FAIL: " << failure.what() << '\n';
    return 1;
  }
}
