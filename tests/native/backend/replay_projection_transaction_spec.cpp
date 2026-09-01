#include "device_c_replay_projection.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <type_traits>

// This target is also built in release cross configurations. Keep the truth
// table executable when the standard assert macro is compiled out.
#if defined(NDEBUG)
#undef assert
#define assert(condition)                         \
  do {                                            \
    if (!(condition)) std::abort();               \
  } while (false)
#endif

namespace {

using dxmt9::core::DeviceState;
using dxmt9::core::Matrix4x4;
using dxmt9::d3d9::DeviceStateUndoJournal;
using dxmt9::d3d9::ReplayDestinationReceipt;
using dxmt9::d3d9::ReplayDestinationKind;
using dxmt9::d3d9::ReplaySourceIdentity;
using dxmt9::d3d9::ReplayStateProjection;
using dxmt9::d3d9::ReplayStagedEmission;
using dxmt9::d3d9::ReplayTransactionEvent;
using dxmt9::d3d9::ReplayTransactionPhase;
using dxmt9::d3d9::ReplayTransaction;
using dxmt9::d3d9::ReplayTransactionState;
using dxmt9::d3d9::advanceReplayTransaction;

static_assert(!std::is_default_constructible_v<
              dxmt9::core::Device::ReplayProgressCheckpoint>);
static_assert(std::is_copy_constructible_v<
              dxmt9::core::Device::ReplayProgressCheckpoint>);
static_assert(!std::is_copy_constructible_v<ReplayTransaction>);

void expectStateEqual(const DeviceState& a, const DeviceState& b) {
  assert(a.viewport == b.viewport);
  assert(a.scissorRect == b.scissorRect);
  assert(a.scissorEnabled == b.scissorEnabled);
  assert(a.renderStates == b.renderStates);
  assert(a.textureStageStates == b.textureStageStates);
  assert(a.samplerStates == b.samplerStates);
  assert(a.transforms == b.transforms);
  assert(a.lights == b.lights);
  assert(a.lightEnabled == b.lightEnabled);
  assert(a.material == b.material);
  assert(a.streamBuffers == b.streamBuffers);
  assert(a.streamOffsets == b.streamOffsets);
  assert(a.streamStrides == b.streamStrides);
  assert(a.streamFrequencies == b.streamFrequencies);
  assert(a.indexBuffer == b.indexBuffer);
  assert(a.indexType == b.indexType);
  assert(a.vertexDecl == b.vertexDecl);
  assert(a.fvf == b.fvf);
  assert(a.vertexShader == b.vertexShader);
  assert(a.pixelShader == b.pixelShader);
  assert(a.vsConst == b.vsConst);
  assert(a.psConst == b.psConst);
  assert(a.textures == b.textures);
  assert(a.renderTargets == b.renderTargets);
  assert(a.depthStencil == b.depthStencil);
  assert(a.clipPlanes == b.clipPlanes);
  assert(a.inScene == b.inScene);
}

void testPhaseAlgebra() {
  ReplayTransactionState state{.identity = ReplaySourceIdentity{
      .frame = 7, .source = 3, .lastSource = 3, .sequence = 11}};
  auto step = advanceReplayTransaction(
      state, ReplayTransactionEvent::ProjectState,
      ReplayStateProjection{
          .stateGeneration = 4, .source = 3, .recordOrdinal = 0});
  assert(step.accepted);
  state = step.state;
  step = advanceReplayTransaction(
      state, ReplayTransactionEvent::StageEmission, {},
      ReplayStagedEmission{.commandCount = 2, .byteCount = 48});
  assert(step.accepted && step.state.phase == ReplayTransactionPhase::Staged);
  state = step.state;
  step = advanceReplayTransaction(
      state, ReplayTransactionEvent::StartIrreversibleEffect);
  assert(step.accepted && step.state.irreversible());
  state = step.state;
  assert(!advanceReplayTransaction(state, ReplayTransactionEvent::Rollback).accepted);
  step = advanceReplayTransaction(
      state, ReplayTransactionEvent::ReceiveDestination,
      {}, {}, ReplayDestinationReceipt{
          .kind = ReplayDestinationKind::DirectChunkSlot,
          .identity = state.identity,
          .queueSequence = 19,
          .buildGeneration = 1,
          .sourceGeneration = 1,
          .storageGeneration = 1,
          .controlIndex = 0,
          .commandCount = 2});
  assert(step.accepted);
  step = advanceReplayTransaction(step.state, ReplayTransactionEvent::Commit);
  assert(step.accepted && step.state.phase == ReplayTransactionPhase::Committed);
  assert(!advanceReplayTransaction(step.state,
                                   ReplayTransactionEvent::FailStop).accepted);
}

void testMultiRecordPrivateDestinationCommit() {
  ReplayTransaction transaction;
  transaction.begin({.source = 3, .lastSource = 3, .sequence = 11});
  assert(transaction.project(
      {.stateGeneration = 11, .source = 3, .recordOrdinal = 0}));
  assert(transaction.stage({.commandCount = 1, .byteCount = 16}));
  assert(transaction.project(
      {.stateGeneration = 11, .source = 3, .recordOrdinal = 1}));
  assert(transaction.stage({.commandCount = 1, .byteCount = 24}));
  assert(!transaction.project(
      {.stateGeneration = 11, .source = 3, .recordOrdinal = 3}));
  assert(transaction.receiveDestination(
      {.kind = ReplayDestinationKind::Compatibility,
       .identity = transaction.state().identity,
       .queueSequence = 7,
       .commandCount = 2}));
  assert(transaction.commit());
  assert(transaction.state().stagedCommandCount == 2);
  assert(transaction.state().stagedByteCount == 40);
}

void testGenerationSourceAndReceiptIdentityAreExact() {
  ReplayTransaction transaction;
  transaction.begin({.source = 10, .lastSource = 11, .sequence = 7});
  assert(transaction.project(
      {.stateGeneration = 7, .source = 10, .recordOrdinal = 0}));
  assert(!transaction.project(
      {.stateGeneration = 8, .source = 10, .recordOrdinal = 1}));
  assert(transaction.project(
      {.stateGeneration = 7, .source = 11, .recordOrdinal = 1}));
  assert(!transaction.project(
      {.stateGeneration = 7, .source = 10, .recordOrdinal = 2}));
  assert(transaction.stage({.commandCount = 1, .byteCount = 8}));
  auto wrong = transaction.state().identity;
  wrong.source = 9;
  assert(!transaction.receiveDestination({
      .kind = ReplayDestinationKind::Compatibility,
      .identity = wrong,
      .queueSequence = 1,
      .commandCount = 1,
  }));
  assert(transaction.receiveDestination({
      .kind = ReplayDestinationKind::Compatibility,
      .identity = transaction.state().identity,
      .queueSequence = 1,
      .commandCount = 1,
  }));
  assert(transaction.commit());
}

void testTransactionOwnsJournal() {
  ReplayTransaction transaction;
  transaction.begin(ReplaySourceIdentity{
      .frame = 1, .source = 2, .lastSource = 2, .sequence = 3});
  DeviceState state;
  state.viewport.width = 17;
  assert(transaction.journal().captureViewport(state));
  state.viewport.width = 99;
  assert(transaction.project(
      {.stateGeneration = 1, .source = 2, .recordOrdinal = 0}));
  assert(transaction.stage({.commandCount = 1, .byteCount = 4}));
  transaction.journal().restore(state);
  assert(state.viewport.width == 17);
}

void testSparseJournalRestoresAllCategories() {
  DeviceState baseline;
  baseline.renderStates.set(3, 9);
  baseline.renderStates.clearDirty();
  baseline.textureStageStates[2].set(5, 12);
  baseline.textureStageStates[2].clearDirty();
  baseline.samplerStates[1].set(4, 13);
  baseline.samplerStates[1].clearDirty();
  Matrix4x4 transform{};
  transform.m[0] = 2.0f;
  baseline.transforms.set(17, transform);
  baseline.transforms.clearDirty();
  baseline.vsConst.float4[5][0] = 10.0f;
  baseline.psConst.int4[2][1] = 8;
  baseline.vsConst.bools[3] = true;
  baseline.psConst.bools[4] = true;
  baseline.inScene = true;

  DeviceState state = baseline;
  DeviceStateUndoJournal journal;
  assert(journal.captureViewport(state));
  assert(journal.captureScissor(state));
  assert(journal.captureRenderState(state, 3));
  assert(journal.captureRenderState(state, 4));
  assert(journal.captureTextureStageState(state, 2, 5));
  assert(journal.captureSamplerState(state, 1, 4));
  assert(journal.captureTransform(state, 17));
  assert(journal.captureLight(state, 0));
  assert(journal.captureLightEnabled(state, 0));
  assert(journal.captureMaterial(state));
  assert(journal.captureStream(state, 0));
  assert(journal.captureIndex(state));
  assert(journal.captureVertexDeclaration(state));
  assert(journal.captureFvf(state));
  assert(journal.captureShader(state, true));
  assert(journal.captureShader(state, false));
  assert(journal.captureTexture(state, 0));
  assert(journal.captureRenderTarget(state, 0));
  assert(journal.captureDepthStencil(state));
  assert(journal.captureClipPlane(state, 0));
  assert(journal.captureInScene(state));
  assert(journal.captureConstantRange(
      state, DeviceStateUndoJournal::ConstantKind::VertexFloat, 5, 1));
  assert(journal.captureConstantRange(
      state, DeviceStateUndoJournal::ConstantKind::PixelInt, 2, 1));
  assert(journal.captureConstantRange(
      state, DeviceStateUndoJournal::ConstantKind::VertexBool, 3, 1));
  assert(journal.captureConstantRange(
      state, DeviceStateUndoJournal::ConstantKind::PixelBool, 4, 1));

  state.viewport.width = 99;
  state.scissorEnabled = !state.scissorEnabled;
  state.renderStates.set(3, 101);
  state.renderStates.set(4, 102);
  state.textureStageStates[2].set(5, 103);
  state.samplerStates[1].set(4, 104);
  transform.m[1] = 3.0f;
  state.transforms.set(17, transform);
  state.lights[0].range = 1.0f;
  state.lightEnabled[0] = !state.lightEnabled[0];
  state.material.power = 4.0f;
  state.streamOffsets[0] = 8;
  state.indexType = dxmt9::core::IndexType::UInt32;
  state.fvf = 77;
  state.vsConst.float4[5][0] = 88.0f;
  state.psConst.int4[2][1] = 89;
  state.vsConst.bools[3] = false;
  state.psConst.bools[4] = false;
  state.inScene = false;
  journal.restore(state);
  expectStateEqual(state, baseline);
}

void testSameWordSequentialCaptureRestoresOriginalMetadata() {
  DeviceState baseline;
  baseline.renderStates.set(3, 9);
  baseline.renderStates.clearDirty();
  Matrix4x4 first{};
  first.m[0] = 2.0f;
  baseline.transforms.set(17, first);
  baseline.transforms.clearDirty();

  DeviceState state = baseline;
  DeviceStateUndoJournal journal;
  assert(journal.captureRenderState(state, 3));
  state.renderStates.set(3, 101);
  assert(journal.captureRenderState(state, 4));
  state.renderStates.set(4, 102);

  assert(journal.captureTransform(state, 17));
  Matrix4x4 changed = first;
  changed.m[1] = 3.0f;
  state.transforms.set(17, changed);
  assert(journal.captureTransform(state, 18));
  state.transforms.set(18, changed);

  journal.restore(state);
  expectStateEqual(state, baseline);
}

}  // namespace

int main() {
  testPhaseAlgebra();
  testMultiRecordPrivateDestinationCommit();
  testGenerationSourceAndReceiptIdentityAreExact();
  testTransactionOwnsJournal();
  testSparseJournalRestoresAllCategories();
  testSameWordSequentialCaptureRestoresOriginalMetadata();
  std::cout << "replay projection transaction spec: PASS\n";
  return 0;
}
