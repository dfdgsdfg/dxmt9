#include "device_c_replay_projection.hpp"

#include <algorithm>

namespace dxmt9::d3d9 {

namespace {

std::size_t constantLimit(DeviceStateUndoJournal::ConstantKind kind) noexcept {
  switch (kind) {
  case DeviceStateUndoJournal::ConstantKind::VertexFloat:
    return core::kMaxVertexConstants;
  case DeviceStateUndoJournal::ConstantKind::VertexInt:
  case DeviceStateUndoJournal::ConstantKind::PixelInt:
    return core::kMaxIntegerConstants;
  case DeviceStateUndoJournal::ConstantKind::VertexBool:
  case DeviceStateUndoJournal::ConstantKind::PixelBool:
    return core::kMaxBoolConstants;
  case DeviceStateUndoJournal::ConstantKind::PixelFloat:
    return core::kMaxPixelConstants;
  }
  return 0;
}

}  // namespace

void DeviceStateUndoJournal::clear() noexcept {
  entryCount_ = 0;
  overflowed_ = false;
  constantArenaUsed_ = 0;
  constantCount_ = 0;
  constantCaptured_ = {};
  viewportCaptured_ = false;
  scissorCaptured_ = false;
  materialCaptured_ = false;
  lightCaptured_.fill(false);
  lightEnabledCaptured_.fill(false);
  for (auto& stream : streams_) {
    stream.buffer.reset();
    stream.captured = false;
  }
  indexCaptured_ = false;
  indexBuffer_.reset();
  vertexDeclarationCaptured_ = false;
  vertexDeclaration_ = {};
  fvfCaptured_ = false;
  vertexShaderCaptured_ = false;
  pixelShaderCaptured_ = false;
  vertexShader_ = {};
  pixelShader_ = {};
  for (std::size_t i = 0; i < textureCaptured_.size(); ++i) {
    if (textureCaptured_[i]) textures_[i].reset();
    textureCaptured_[i] = false;
  }
  for (std::size_t i = 0; i < renderTargetCaptured_.size(); ++i) {
    if (renderTargetCaptured_[i]) renderTargets_[i] = {};
    renderTargetCaptured_[i] = false;
  }
  depthStencilCaptured_ = false;
  depthStencil_ = {};
  clipPlaneCaptured_.fill(false);
  inSceneCaptured_ = false;
  const auto clearTable = [](auto& table) noexcept {
    for (auto& entry : table.entries) {
      entry.captured = false;
    }
    table.wordCaptured.fill(false);
    table.count = 0;
    table.rollingHash = 0;
    table.metadataCaptured = false;
  };
  clearTable(renderStates_);
  for (auto& table : textureStageStates_) clearTable(table);
  for (auto& table : samplerStates_) clearTable(table);
  for (auto& entry : transforms_.entries) entry.captured = false;
  transforms_.wordCaptured.fill(false);
  transforms_.count = 0;
  transforms_.rollingHash = 0;
  transforms_.metadataCaptured = false;
}

bool DeviceStateUndoJournal::reserveEntry() noexcept {
  if (entryCount_ == kConstantUndoCapacity +
                         core::kMaxStateSlots +
                         core::kMaxTextureStages * core::kMaxTextureStageStates +
                         core::kMaxSamplers * core::kMaxSamplerStates +
                         core::kMaxTransformSlots + 128u) {
    overflowed_ = true;
    return false;
  }
  ++entryCount_;
  return true;
}

template <std::size_t MaxEntries>
bool DeviceStateUndoJournal::captureTableEntry(
    TableUndo<MaxEntries>& undo,
    const core::StateValueTable<MaxEntries>& table,
    core::u32 key) noexcept {
  if (!table.validKey(key)) return false;
  auto& entry = undo.entries[key];
  if (entry.captured) return true;
  if (!reserveEntry()) return false;
  if (!undo.metadataCaptured) {
    undo.count = table.count;
    undo.rollingHash = table.rollingHash;
    undo.metadataCaptured = true;
  }
  entry.value = table.values[key];
  const auto word = table.word(key);
  if (!undo.wordCaptured[word]) {
    undo.occupied[word] = table.occupied[word];
    undo.dirty[word] = table.dirty[word];
    undo.wordCaptured[word] = true;
  }
  entry.captured = true;
  return true;
}

bool DeviceStateUndoJournal::captureViewport(
    const core::DeviceState& state) noexcept {
  if (viewportCaptured_) return true;
  if (!reserveEntry()) return false;
  viewport_ = state.viewport;
  viewportCaptured_ = true;
  return true;
}

bool DeviceStateUndoJournal::captureScissor(
    const core::DeviceState& state) noexcept {
  if (scissorCaptured_) return true;
  if (!reserveEntry()) return false;
  scissor_ = state.scissorRect;
  scissorEnabled_ = state.scissorEnabled;
  scissorCaptured_ = true;
  return true;
}

bool DeviceStateUndoJournal::captureMaterial(
    const core::DeviceState& state) noexcept {
  if (materialCaptured_) return true;
  if (!reserveEntry()) return false;
  material_ = state.material;
  materialCaptured_ = true;
  return true;
}

bool DeviceStateUndoJournal::captureLight(const core::DeviceState& state,
                                          core::u32 index) noexcept {
  if (index >= core::kMaxLights) return false;
  if (lightCaptured_[index]) return true;
  if (!reserveEntry()) return false;
  lights_[index] = state.lights[index];
  lightCaptured_[index] = true;
  return true;
}

bool DeviceStateUndoJournal::captureLightEnabled(
    const core::DeviceState& state, core::u32 index) noexcept {
  if (index >= core::kMaxLights) return false;
  if (lightEnabledCaptured_[index]) return true;
  if (!reserveEntry()) return false;
  lightEnabled_[index] = state.lightEnabled[index];
  lightEnabledCaptured_[index] = true;
  return true;
}

bool DeviceStateUndoJournal::captureStream(const core::DeviceState& state,
                                           core::u32 index) noexcept {
  if (index >= core::kMaxStreams) return false;
  auto& undo = streams_[index];
  if (undo.captured) return true;
  if (!reserveEntry()) return false;
  undo.buffer = state.streamBuffers[index];
  undo.offset = state.streamOffsets[index];
  undo.stride = state.streamStrides[index];
  undo.frequency = state.streamFrequencies[index];
  undo.captured = true;
  return true;
}

bool DeviceStateUndoJournal::captureIndex(
    const core::DeviceState& state) noexcept {
  if (indexCaptured_) return true;
  if (!reserveEntry()) return false;
  indexBuffer_ = state.indexBuffer;
  indexType_ = state.indexType;
  indexCaptured_ = true;
  return true;
}

bool DeviceStateUndoJournal::captureVertexDeclaration(
    const core::DeviceState& state) noexcept {
  if (vertexDeclarationCaptured_) return true;
  if (!reserveEntry()) return false;
  vertexDeclaration_ = state.vertexDecl;
  vertexDeclarationCaptured_ = true;
  return true;
}

bool DeviceStateUndoJournal::captureFvf(const core::DeviceState& state) noexcept {
  if (fvfCaptured_) return true;
  if (!reserveEntry()) return false;
  fvf_ = state.fvf;
  fvfCaptured_ = true;
  return true;
}

bool DeviceStateUndoJournal::captureShader(const core::DeviceState& state,
                                           bool vertex) noexcept {
  bool& captured = vertex ? vertexShaderCaptured_ : pixelShaderCaptured_;
  if (captured) return true;
  if (!reserveEntry()) return false;
  if (vertex) vertexShader_ = state.vertexShader;
  else pixelShader_ = state.pixelShader;
  captured = true;
  return true;
}

bool DeviceStateUndoJournal::captureTexture(const core::DeviceState& state,
                                            core::u32 index) noexcept {
  if (index >= core::kMaxTextures) return false;
  if (textureCaptured_[index]) return true;
  if (!reserveEntry()) return false;
  textures_[index] = state.textures[index];
  textureCaptured_[index] = true;
  return true;
}

bool DeviceStateUndoJournal::captureRenderTarget(
    const core::DeviceState& state, core::u32 index) noexcept {
  if (index >= core::kMaxRenderTargets) return false;
  if (renderTargetCaptured_[index]) return true;
  if (!reserveEntry()) return false;
  renderTargets_[index] = state.renderTargets[index];
  renderTargetCaptured_[index] = true;
  return true;
}

bool DeviceStateUndoJournal::captureDepthStencil(
    const core::DeviceState& state) noexcept {
  if (depthStencilCaptured_) return true;
  if (!reserveEntry()) return false;
  depthStencil_ = state.depthStencil;
  depthStencilCaptured_ = true;
  return true;
}

bool DeviceStateUndoJournal::captureClipPlane(
    const core::DeviceState& state, core::u32 index) noexcept {
  if (index >= core::kMaxClipPlanes) return false;
  if (clipPlaneCaptured_[index]) return true;
  if (!reserveEntry()) return false;
  clipPlanes_[index] = state.clipPlanes[index];
  clipPlaneCaptured_[index] = true;
  return true;
}

bool DeviceStateUndoJournal::captureInScene(
    const core::DeviceState& state) noexcept {
  if (inSceneCaptured_) return true;
  if (!reserveEntry()) return false;
  inScene_ = state.inScene;
  inSceneCaptured_ = true;
  return true;
}

bool DeviceStateUndoJournal::captureRenderState(
    const core::DeviceState& state, core::u32 key) noexcept {
  return captureTableEntry(renderStates_, state.renderStates, key);
}

bool DeviceStateUndoJournal::captureTextureStageState(
    const core::DeviceState& state, core::u32 stage, core::u32 key) noexcept {
  if (stage >= core::kMaxTextureStages) return false;
  return captureTableEntry(textureStageStates_[stage],
                           state.textureStageStates[stage], key);
}

bool DeviceStateUndoJournal::captureSamplerState(
    const core::DeviceState& state, core::u32 sampler, core::u32 key) noexcept {
  if (sampler >= core::kMaxSamplers) return false;
  return captureTableEntry(samplerStates_[sampler],
                           state.samplerStates[sampler], key);
}

bool DeviceStateUndoJournal::captureTransform(
    const core::DeviceState& state, core::u32 key) noexcept {
  if (!state.transforms.validKey(key)) return false;
  auto& entry = transforms_.entries[key];
  if (entry.captured) return true;
  if (!reserveEntry()) return false;
  if (!transforms_.metadataCaptured) {
    transforms_.count = state.transforms.count;
    transforms_.rollingHash = state.transforms.rollingHash;
    transforms_.metadataCaptured = true;
  }
  entry.value = state.transforms.values[key];
  const auto word = state.transforms.word(key);
  if (!transforms_.wordCaptured[word]) {
    transforms_.occupied[word] = state.transforms.occupied[word];
    transforms_.dirty[word] = state.transforms.dirty[word];
    transforms_.wordCaptured[word] = true;
  }
  entry.captured = true;
  return true;
}

bool DeviceStateUndoJournal::captureConstant(
    const core::DeviceState& state, ConstantKind kind, core::u32 index) noexcept {
  if (index >= constantLimit(kind)) return false;
  const auto kindIndex = static_cast<std::size_t>(kind);
  const auto word = index / 64u;
  const auto bit = std::uint64_t{1} << (index % 64u);
  if ((constantCaptured_[kindIndex][word] & bit) != 0u) return true;
  std::size_t size = 0;
  const void* source = nullptr;
  switch (kind) {
  case ConstantKind::VertexFloat: source = &state.vsConst.float4[index]; size = sizeof(state.vsConst.float4[index]); break;
  case ConstantKind::VertexInt: source = &state.vsConst.int4[index]; size = sizeof(state.vsConst.int4[index]); break;
  case ConstantKind::VertexBool: source = &state.vsConst.bools[index]; size = sizeof(state.vsConst.bools[index]); break;
  case ConstantKind::PixelFloat: source = &state.psConst.float4[index]; size = sizeof(state.psConst.float4[index]); break;
  case ConstantKind::PixelInt: source = &state.psConst.int4[index]; size = sizeof(state.psConst.int4[index]); break;
  case ConstantKind::PixelBool: source = &state.psConst.bools[index]; size = sizeof(state.psConst.bools[index]); break;
  }
  if (constantCount_ == constants_.size() ||
      size > constantArena_.size() - constantArenaUsed_ || !reserveEntry()) {
    overflowed_ = true;
    return false;
  }
  auto& entry = constants_[constantCount_++];
  entry.kind = kind;
  entry.index = index;
  entry.arenaOffset = static_cast<std::uint32_t>(constantArenaUsed_);
  entry.byteSize = static_cast<std::uint16_t>(size);
  std::memcpy(constantArena_.data() + constantArenaUsed_, source, size);
  constantArenaUsed_ += size;
  constantCaptured_[kindIndex][word] |= bit;
  return true;
}

bool DeviceStateUndoJournal::captureConstantRange(
    const core::DeviceState& state, ConstantKind kind, core::u32 start,
    core::u32 count) noexcept {
  if (start > constantLimit(kind) || count > constantLimit(kind) - start) {
    return false;
  }
  for (core::u32 index = start; index < start + count; ++index) {
    if (!captureConstant(state, kind, index)) return false;
  }
  return true;
}

void DeviceStateUndoJournal::restore(core::DeviceState& state) noexcept {
  for (std::size_t i = constantCount_; i != 0; --i) {
    const auto& entry = constants_[i - 1u];
    void* destination = nullptr;
    switch (entry.kind) {
    case ConstantKind::VertexFloat: destination = &state.vsConst.float4[entry.index]; break;
    case ConstantKind::VertexInt: destination = &state.vsConst.int4[entry.index]; break;
    case ConstantKind::VertexBool: destination = &state.vsConst.bools[entry.index]; break;
    case ConstantKind::PixelFloat: destination = &state.psConst.float4[entry.index]; break;
    case ConstantKind::PixelInt: destination = &state.psConst.int4[entry.index]; break;
    case ConstantKind::PixelBool: destination = &state.psConst.bools[entry.index]; break;
    }
    std::memcpy(destination, constantArena_.data() + entry.arenaOffset,
                entry.byteSize);
  }
  if (viewportCaptured_) state.viewport = viewport_;
  if (scissorCaptured_) {
    state.scissorRect = scissor_;
    state.scissorEnabled = scissorEnabled_;
  }
  if (materialCaptured_) state.material = material_;
  for (std::size_t i = 0; i < core::kMaxLights; ++i) {
    if (lightCaptured_[i]) state.lights[i] = lights_[i];
    if (lightEnabledCaptured_[i]) state.lightEnabled[i] = lightEnabled_[i];
  }
  for (std::size_t i = 0; i < core::kMaxStreams; ++i) {
    if (!streams_[i].captured) continue;
    state.streamBuffers[i] = streams_[i].buffer;
    state.streamOffsets[i] = streams_[i].offset;
    state.streamStrides[i] = streams_[i].stride;
    state.streamFrequencies[i] = streams_[i].frequency;
  }
  if (indexCaptured_) {
    state.indexBuffer = indexBuffer_;
    state.indexType = indexType_;
  }
  if (vertexDeclarationCaptured_) state.vertexDecl = vertexDeclaration_;
  if (fvfCaptured_) state.fvf = fvf_;
  if (vertexShaderCaptured_) state.vertexShader = vertexShader_;
  if (pixelShaderCaptured_) state.pixelShader = pixelShader_;
  for (std::size_t i = 0; i < core::kMaxTextures; ++i) {
    if (textureCaptured_[i]) state.textures[i] = textures_[i];
  }
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    if (renderTargetCaptured_[i]) state.renderTargets[i] = renderTargets_[i];
  }
  if (depthStencilCaptured_) state.depthStencil = depthStencil_;
  for (std::size_t i = 0; i < core::kMaxClipPlanes; ++i) {
    if (clipPlaneCaptured_[i]) state.clipPlanes[i] = clipPlanes_[i];
  }
  if (inSceneCaptured_) state.inScene = inScene_;

  auto restoreTable = [](auto& table, const auto& undo) noexcept {
    if (!undo.metadataCaptured) return;
    for (std::size_t key = 0; key < undo.entries.size(); ++key) {
      const auto& entry = undo.entries[key];
      if (!entry.captured) continue;
      table.values[key] = entry.value;
    }
    for (std::size_t word = 0; word < undo.wordCaptured.size(); ++word) {
      if (!undo.wordCaptured[word]) continue;
      table.occupied[word] = undo.occupied[word];
      table.dirty[word] = undo.dirty[word];
    }
    table.count = undo.count;
    table.rollingHash = undo.rollingHash;
  };
  restoreTable(state.renderStates, renderStates_);
  for (std::size_t i = 0; i < core::kMaxTextureStages; ++i) {
    restoreTable(state.textureStageStates[i], textureStageStates_[i]);
  }
  for (std::size_t i = 0; i < core::kMaxSamplers; ++i) {
    restoreTable(state.samplerStates[i], samplerStates_[i]);
  }
  if (transforms_.metadataCaptured) {
    for (std::size_t key = 0; key < transforms_.entries.size(); ++key) {
      const auto& entry = transforms_.entries[key];
      if (!entry.captured) continue;
      state.transforms.values[key] = entry.value;
    }
    for (std::size_t word = 0;
         word < transforms_.wordCaptured.size(); ++word) {
      if (!transforms_.wordCaptured[word]) continue;
      state.transforms.occupied[word] = transforms_.occupied[word];
      state.transforms.dirty[word] = transforms_.dirty[word];
    }
    state.transforms.count = transforms_.count;
    state.transforms.rollingHash = transforms_.rollingHash;
  }
}

bool ReplayTransaction::advance(ReplayTransactionEvent event,
                                ReplayStateProjection projection,
                                ReplayStagedEmission emission,
                                ReplayDestinationReceipt destination) noexcept {
  const auto result = advanceReplayTransaction(
      state_, event, projection, emission, destination);
  if (!result.accepted) return false;
  state_ = result.state;
  return true;
}

bool ReplayTransaction::project(ReplayStateProjection projection) noexcept {
  return advance(ReplayTransactionEvent::ProjectState, projection);
}

bool ReplayTransaction::stage(ReplayStagedEmission emission) noexcept {
  return advance(ReplayTransactionEvent::StageEmission, {}, emission);
}

bool ReplayTransaction::startIrreversibleEffect() noexcept {
  return advance(ReplayTransactionEvent::StartIrreversibleEffect);
}

bool ReplayTransaction::receiveDestination(
    ReplayDestinationReceipt receipt) noexcept {
  return advance(ReplayTransactionEvent::ReceiveDestination, {}, {}, receipt);
}

bool ReplayTransaction::commit() noexcept {
  if (!advance(ReplayTransactionEvent::Commit)) return false;
  journal_.clear();
  progress_.reset();
  return true;
}

bool ReplayTransaction::rollback(core::Device& device) noexcept {
  if (!advance(ReplayTransactionEvent::Rollback)) return false;
  journal_.restore(device.mutableState());
  journal_.clear();
  if (progress_) device.restoreReplayProgress(*progress_);
  progress_.reset();
  return true;
}

bool ReplayTransaction::failStop() noexcept {
  if (!advance(ReplayTransactionEvent::FailStop)) return false;
  journal_.clear();
  progress_.reset();
  return true;
}

}  // namespace dxmt9::d3d9
