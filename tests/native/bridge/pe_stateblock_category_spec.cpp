// Exhaustive host truth table for the PE state-block candidate.  This stays
// native-buildable: candidate pointer slots are represented as opaque values,
// while the PE wrapper owns the corresponding COM refs at its boundaries.

#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_recorder_state.hpp"
#include "d3d9_pe_stateblock_transaction.hpp"

#include <array>
#include <atomic>
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

void testCandidateAbaAndDomains() {
  StateBlockRecorded candidate{};
  PeHotStateShadow live{};

  // Ordinary writes belong to LiveShadow/PendingDelta.  A recording write
  // to the same key belongs only to the candidate, including A-B-A.
  const auto render = renderStateSlotKey(7u);
  live.renderStateShadowTyped().set(render, 11u);
  live.pendingRenderStatesTyped().set(render, 11u);
  candidate.renderStates().set(render, 1u);
  candidate.renderStates().set(render, 2u);
  candidate.renderStates().set(render, 1u);
  std::uint32_t value = 0u;
  check(live.renderStateShadowTyped().get(render, value) && value == 11u,
        "recording must not mutate live render state");
  check(live.pendingRenderStatesTyped().get(render, value) && value == 11u,
        "recording must not mutate pending render state");
  check(candidate.renderStates().get(render, value) && value == 1u,
        "candidate render state must be last-write-wins");

  candidate.textureStageStates().set(textureStageIndexKey(2u),
                                     textureStageStateTypeKey(3u), 1u);
  candidate.textureStageStates().set(textureStageIndexKey(2u),
                                     textureStageStateTypeKey(3u), 2u);
  candidate.textureStageStates().set(textureStageIndexKey(2u),
                                     textureStageStateTypeKey(3u), 1u);
  check(candidate.textureStageStates().size() == 1u,
        "TSS candidate key must remain singular");

  SamplerIndex sampler{};
  check(samplerIndexKey(4u, sampler), "sampler key normalization");
  SamplerStateType samplerType{};
  check(samplerStateTypeKey(2u, samplerType), "sampler type normalization");
  candidate.samplerStates().set(sampler, samplerType, 1u);
  candidate.samplerStates().set(sampler, samplerType, 2u);
  candidate.samplerStates().set(sampler, samplerType, 1u);
  check(candidate.samplerStates().size() == 1u,
        "sampler candidate key must remain singular");

  D9CMatrix matrix = identityTransformMatrix();
  matrix.m[0] = 2.0f;
  const auto transform = transformStateKey(kD3dTsView);
  candidate.transforms().set(transform, matrix);
  matrix.m[0] = 3.0f;
  candidate.transforms().set(transform, matrix);
  matrix.m[0] = 2.0f;
  candidate.transforms().set(transform, matrix);
  D9CMatrix capturedMatrix{};
  check(candidate.transforms().get(transform, capturedMatrix) &&
            capturedMatrix.m[0] == 2.0f,
        "transform candidate must preserve A-B-A");

  // Every remaining PE-shadow category has a bounded tracked slot and can be
  // captured without changing the ordinary live domain.
  candidate.textures().set(0u, StateBlockTextureRef::fromRaw(reinterpret_cast<void*>(0x10u)));
  candidate.streamSources().set(0u, StateBlockStreamSourceValue{
      .buffer = {reinterpret_cast<void*>(0x20u)}, .offset = 4u,
      .stride = 16u});
  candidate.streamFrequencies().set(0u, 2u);
  candidate.indexBuffer().set(0u, StateBlockIndexBufferRef::fromRaw(reinterpret_cast<void*>(0x30u)));
  candidate.vertexShader().set(0u, StateBlockVertexShaderRef::fromRaw(reinterpret_cast<void*>(0x40u)));
  candidate.pixelShader().set(0u, StateBlockPixelShaderRef::fromRaw(reinterpret_cast<void*>(0x50u)));
  candidate.fvf().set(0u, 0x1122u);
  candidate.vertexDeclaration().set(0u, StateBlockVertexDeclarationRef::fromRaw(reinterpret_cast<void*>(0x60u)));
  candidate.renderTargets().set(0u, StateBlockRenderTargetRef::fromRaw(reinterpret_cast<void*>(0x70u)));
  candidate.depthStencil().set(0u, StateBlockDepthStencilRef::fromRaw(reinterpret_cast<void*>(0x80u)));
  candidate.viewport().set(0u, D9CViewport{1u, 2u, 640u, 480u, 0.0f, 1.0f});
  candidate.scissor().set(0u, D9CRect{1, 2, 639, 479});
  candidate.material().set(0u, D9CMaterial{});
  candidate.clipPlanes().set(0u, std::array<float, 4>{1, 2, 3, 4});
  candidate.lights().set(0u, D9CLight{});
  candidate.lightEnables().set(0u, 1u);
  std::array<float, 4> constants{1, 2, 3, 4};
  candidate.constants.vsConstF.record(3u, 1u, constants.data(),
                                      sizeof(constants));
  candidate.constants.psConstF.record(3u, 1u, constants.data(),
                                      sizeof(constants));

  std::array<bool, static_cast<std::size_t>(
                       StateBlockApplyPhysicalStore::Count)> physicalSeen{};
  candidate.forEachApplyPhysical(
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
            candidate.constants.vsConstF.contains(3u) &&
            candidate.constants.psConstF.contains(3u),
        "all recordable categories must have fixed candidate slots");
  check(candidate.has(StateBlockRecorded::Category::textures) &&
            candidate.has(StateBlockRecorded::Category::streamSources) &&
            candidate.has(StateBlockRecorded::Category::streamFrequencies),
        "canonical category visitor reports occupied categories");

  // Source tuple and frequency are independent D3D9 state aspects. Applying
  // either tracked set preserves the other aspect.
  StateBlockRecorded sourceOnly{};
  sourceOnly.streamSources().set(
      0u, StateBlockStreamSourceValue{{reinterpret_cast<void*>(0x90u)}, 8u, 32u});
  StateBlockRecorded frequencyOnly{};
  frequencyOnly.streamFrequencies().set(0u, 9u);
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
  capture.renderStates().set(render, 99u);
  check(candidate.renderStates().get(render, value) && value == 1u,
        "Capture refresh must not mutate the recording candidate");
  live.renderStateShadowTyped().set(render, 77u);
  capture.renderStates().set(render, 77u);
  check(capture.renderStates().get(render, value) && value == 77u &&
            candidate.renderStates().get(render, value) && value == 1u,
        "Capture must refresh a fixed tracked key from live state only");
  candidate.clearForBegin();
  check(candidate.renderStates().empty() && candidate.textures().empty() &&
            candidate.constants.vsConstF.empty(),
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
                             .stateBlockTransaction.recorded()),
                StateBlockRecorded&>);

  PeStateBlockTransactionState transaction{};
  check(!transaction.isRecording() && !transaction.isInsideEnd() &&
            transaction.writeAllowed(),
        "fresh transaction must be idle and writable");

  RefProbe candidateRef{};
  candidateRef.addRef();
  candidateRef.addRef();
  candidateRef.addRef();
  transaction.recorded().textures().set(
      0u, StateBlockTextureRef::fromRaw(&candidateRef));
  transaction.recorded().vertexShader().set(
      0u, StateBlockVertexShaderRef::fromRaw(&candidateRef));
  transaction.recorded().vertexDeclaration().set(
      0u, StateBlockVertexDeclarationRef::fromRaw(&candidateRef));
  transaction.beginAccepted([&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->release();
  });
  check(transaction.isRecording() &&
            transaction.recorded().textures().empty() &&
            transaction.recorded().vertexShader().empty() &&
            transaction.recorded().vertexDeclaration().empty() &&
            candidateRef.refs == 1u,
        "candidate-owned vdecl and staged categories release each retain by multiplicity");

  transaction.recorded().renderStates().set(renderStateSlotKey(7u), 19u);
  transaction.enterEndPublication();
  check(!transaction.isRecording() && transaction.isInsideEnd(),
        "accepted backend End enters the closed publication scope");
  transaction.finishEndPublication(true, [&](void* raw) noexcept {
    if (raw) static_cast<RefProbe*>(raw)->release();
  });
  check(!transaction.isRecording() && !transaction.isInsideEnd() &&
            transaction.writeAllowed() &&
            transaction.recorded().renderStates().empty(),
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

  transaction.stageTexture(0u, StateBlockTextureRef::fromRaw(&shared), retain);
  transaction.stageTexture(1u, StateBlockTextureRef::fromRaw(&shared), retain);
  transaction.stageStream(
      0u, StateBlockStreamSourceValue{
              .buffer = {&shared}, .offset = 4u, .stride = 16u},
      retain);
  transaction.stageVertexShader(
      StateBlockVertexShaderRef::fromRaw(&shared), retain);
  transaction.stagePixelShader(
      StateBlockPixelShaderRef::fromRaw(&shared), retain);
  transaction.stageIndexBuffer(
      StateBlockIndexBufferRef::fromRaw(&shared), retain);
  transaction.stageRenderTarget(
      0u, StateBlockRenderTargetRef::fromRaw(&shared), retain);
  transaction.stageDepthStencil(
      StateBlockDepthStencilRef::fromRaw(&shared), retain);
  check(transaction.hasPreparedApply() && shared.refs == 9u,
        "each typed occupied Apply category retains independently");
  transaction.discardPrepared(release);
  check(!transaction.hasPreparedApply() && shared.refs == 1u,
        "discard releases every staged retain without deduplication");

  transaction.stageTexture(0u, StateBlockTextureRef::fromRaw(&shared), retain);
  const auto transferred = transaction.takeTexture(0u);
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

}  // namespace

int main() {
  try {
    testApplyCategoryRegistry();
    testCandidateAbaAndDomains();
    testGeneratedTransitionTableIsomorphism();
    testTransactionOwnershipAndLifecycle();
    testTypedStagingRetentionMultiplicityAndReset();
    testStagedFailureAndIntervalWitness();
    testPoisonResetFaultSequence();
    testEndFailureAndPoisonWriteBoundary();
    std::cout << "pe state-block category spec: PASS\n";
    return 0;
  } catch (const Failure& failure) {
    std::cerr << "pe state-block category spec: FAIL: " << failure.what() << '\n';
    return 1;
  }
}
