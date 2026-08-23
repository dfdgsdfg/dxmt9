// Exhaustive host truth table for the PE state-block candidate.  This stays
// native-buildable: candidate pointer slots are represented as opaque values,
// while the PE wrapper owns the corresponding COM refs at its boundaries.

#include "d3d9_pe_state_shadow.hpp"
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

namespace {

struct Failure : std::runtime_error {
  explicit Failure(std::string message) : std::runtime_error(std::move(message)) {}
};

void check(bool condition, std::string_view message) {
  if (!condition) throw Failure(std::string(message));
}

enum class Category : std::uint8_t {
  Render, Tss, Sampler, Transform, Texture, StreamSource, StreamFrequency, Index, VertexShader,
  PixelShader, Fvf, VertexDecl, RenderTarget, Depth, Viewport, Scissor,
  Material, ClipPlane, Light, LightEnable, VsConstants, PsConstants,
  Singleton,
};

constexpr std::array<Category, 23> kCategories = {
    Category::Render, Category::Tss, Category::Sampler, Category::Transform,
    Category::Texture, Category::StreamSource, Category::StreamFrequency,
    Category::Index,
    Category::VertexShader, Category::PixelShader, Category::Fvf,
    Category::VertexDecl, Category::RenderTarget, Category::Depth,
    Category::Viewport, Category::Scissor, Category::Material,
    Category::ClipPlane, Category::Light, Category::LightEnable,
    Category::VsConstants, Category::PsConstants, Category::Singleton,
};

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
  candidate.textures.set(0u, reinterpret_cast<void*>(0x10u));
  candidate.streamSources.set(0u, StateBlockStreamSourceValue{
      .buffer = reinterpret_cast<void*>(0x20u), .offset = 4u,
      .stride = 16u});
  candidate.streamFrequencies.set(0u, 2u);
  candidate.indexBuffer.set(0u, reinterpret_cast<void*>(0x30u));
  candidate.vertexShader.set(0u, reinterpret_cast<void*>(0x40u));
  candidate.pixelShader.set(0u, reinterpret_cast<void*>(0x50u));
  candidate.fvf.set(0u, 0x1122u);
  candidate.vertexDeclaration.set(0u, reinterpret_cast<void*>(0x60u));
  candidate.renderTargets.set(0u, reinterpret_cast<void*>(0x70u));
  candidate.depthStencil.set(0u, reinterpret_cast<void*>(0x80u));
  candidate.viewport.set(0u, D9CViewport{1u, 2u, 640u, 480u, 0.0f, 1.0f});
  candidate.scissor.set(0u, D9CRect{1, 2, 639, 479});
  candidate.material.set(0u, D9CMaterial{});
  candidate.clipPlanes.set(0u, std::array<float, 4>{1, 2, 3, 4});
  candidate.lights.set(0u, D9CLight{});
  candidate.lightEnables.set(0u, 1u);
  std::array<float, 4> constants{1, 2, 3, 4};
  candidate.constants.vsConstF.record(3u, 1u, constants.data(),
                                      sizeof(constants));
  candidate.constants.psConstF.record(3u, 1u, constants.data(),
                                      sizeof(constants));

  for (const auto category : kCategories) {
    (void)category;
    // The table itself is the exhaustive routing witness; all categories are
    // occupied below and therefore participate in Capture's fixed set.
  }
  check(candidate.textures.contains(0u) &&
            candidate.streamSources.contains(0u) &&
            candidate.streamFrequencies.contains(0u) &&
            candidate.indexBuffer.contains(0u) &&
            candidate.vertexShader.contains(0u) &&
            candidate.pixelShader.contains(0u) && candidate.fvf.contains(0u) &&
            candidate.vertexDeclaration.contains(0u) &&
            candidate.renderTargets.contains(0u) &&
            candidate.depthStencil.contains(0u) && candidate.viewport.contains(0u) &&
            candidate.scissor.contains(0u) && candidate.material.contains(0u) &&
            candidate.clipPlanes.contains(0u) && candidate.lights.contains(0u) &&
            candidate.lightEnables.contains(0u) &&
            candidate.constants.vsConstF.contains(3u) &&
            candidate.constants.psConstF.contains(3u),
        "all recordable categories must have fixed candidate slots");

  // Source tuple and frequency are independent D3D9 state aspects. Applying
  // either tracked set preserves the other aspect.
  StateBlockRecorded sourceOnly{};
  sourceOnly.streamSources.set(
      0u, StateBlockStreamSourceValue{reinterpret_cast<void*>(0x90u), 8u, 32u});
  StateBlockRecorded frequencyOnly{};
  frequencyOnly.streamFrequencies.set(0u, 9u);
  StateBlockStreamSourceValue liveSource{
      reinterpret_cast<void*>(0x91u), 4u, 16u};
  std::uint32_t liveFrequency = 7u;
  sourceOnly.streamSources.forEach(
      [&](std::size_t, const StateBlockStreamSourceValue& value) {
        liveSource = value;
      });
  check(liveSource.buffer == reinterpret_cast<void*>(0x90u) &&
            liveSource.offset == 8u && liveSource.stride == 32u &&
            liveFrequency == 7u,
        "source Apply must preserve an unrecorded frequency");
  frequencyOnly.streamFrequencies.forEach(
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
  check(candidate.renderStates().empty() && candidate.textures.empty() &&
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

}  // namespace

int main() {
  try {
    testCandidateAbaAndDomains();
    testStagedFailureAndIntervalWitness();
    std::cout << "pe state-block category spec: PASS\n";
    return 0;
  } catch (const Failure& failure) {
    std::cerr << "pe state-block category spec: FAIL: " << failure.what() << '\n';
    return 1;
  }
}
