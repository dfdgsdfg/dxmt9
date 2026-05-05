#include "dxmt9/core.hpp"
#include "../src/dxmt9/dxmt9_backend_types.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

using namespace dxmt9::core;

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

template <typename T>
void checkEq(const T& actual, const T& expected, std::string_view message) {
  if (!(actual == expected)) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void checkNear(float actual, float expected, std::string_view message) {
  if (std::fabs(actual - expected) > 0.0001f) {
    std::cerr << "FAIL: " << message << " actual=" << actual << " expected=" << expected << '\n';
    std::exit(1);
  }
}

Matrix4x4 taggedMatrix(float base) {
  Matrix4x4 matrix{};
  for (size_t i = 0; i < matrix.m.size(); ++i) {
    matrix.m[i] = base + static_cast<float>(i);
  }
  return matrix;
}

std::shared_ptr<Buffer> makeBuffer(u64 handle, u64 size, u32 usage) {
  BufferDesc desc{};
  desc.size = size;
  desc.usage = usage;
  return std::make_shared<Buffer>(std::shared_ptr<Device>{}, BufferHandle{handle}, desc);
}

std::shared_ptr<Texture> makeTexture(u64 handle, Format format, u32 levels = 1,
                                     u32 usage = UsageTexture) {
  TextureDesc desc{};
  desc.width = 64;
  desc.height = 32;
  desc.levels = levels;
  desc.format = format;
  desc.usage = usage;
  return std::make_shared<Texture>(std::shared_ptr<Device>{}, TextureHandle{handle}, desc);
}

std::shared_ptr<Surface> makeSurface(u64 handle, Format format, MultiSampleType samples,
                                     bool renderTarget, bool depthStencil) {
  SurfaceDesc desc{};
  desc.width = 128;
  desc.height = 64;
  desc.format = format;
  desc.usage = renderTarget ? UsageRenderTarget : UsageDepthStencil;
  desc.renderTarget = renderTarget;
  desc.depthStencil = depthStencil;
  desc.multiSampleType = samples;
  return std::make_shared<Surface>(std::shared_ptr<Device>{}, SurfaceHandle{handle}, desc);
}

ShaderRef makeBytecodeShader(u64 hash, std::vector<u8> bytes) {
  ShaderRef shader{};
  shader.kind = ShaderRef::Kind::Bytecode;
  shader.hash = hash;
  shader.bytecode.hash = hash ^ 0x9e3779b97f4a7c15ull;
  shader.bytecode.bytes = std::move(bytes);
  return shader;
}

void testStateDrawTransform() {
  DeviceState state;
  state.reset();

  state.viewport = {10, 20, 640, 480, 0.25f, 0.75f};
  state.scissorRect = {11, 22, 333, 444};
  state.renderStates[RS_SCISSOR_TEST_ENABLE] = 1;
  state.renderStates[RS_CLIP_PLANE_ENABLE] = 0x3u;
  state.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  state.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);

  state.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[0][TSS_TEXCOORD_INDEX] = 5;
  state.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;
  state.samplerStates[0][SAMP_ADDRESS_U] = 3;
  state.samplerStates[0][SAMP_MAX_ANISOTROPY] = 8;

  state.transforms[XFORM_TEXTURE_BASE] = taggedMatrix(100.0f);
  state.clipPlanes[0] = {1.0f, 2.0f, 3.0f, 4.0f};
  state.clipPlanes[1] = {5.0f, 6.0f, 7.0f, 8.0f};
  state.clipPlanes[2] = {9.0f, 10.0f, 11.0f, 12.0f};

  const DrawCallArgs args{PrimitiveType::TriangleFan, 2, 17, -3, 29, IndexType::UInt32};
  const DrawDesc desc = makeDrawDescFromState(state, args);

  checkEq(desc.primitiveType, PrimitiveType::TriangleList, "triangle fan normalizes to triangle list");
  checkEq(desc.primitiveCount, 2u, "primitive count copied");
  checkEq(desc.startVertex, 17u, "start vertex copied");
  checkEq(desc.baseVertexIndex, -3, "base vertex copied");
  checkEq(desc.startIndex, 29u, "start index copied");
  checkEq(desc.indexType, IndexType::UInt32, "index type copied");

  checkEq(desc.viewport.viewport, state.viewport, "viewport copied");
  checkEq(desc.viewport.scissor, state.scissorRect, "scissor copied");
  check(desc.viewport.scissorEnabled, "scissor enable derived from render state");

  checkEq(desc.rs.values.at(RS_ALPHA_TEST_ENABLE), 1u, "render state copied");
  checkEq(desc.textures[0].stageStates.at(TSS_COLOR_OP), static_cast<u32>(TextureOp::SelectArg1),
          "texture-stage state copied");
  checkEq(desc.textures[0].stageStates.at(TSS_TEXCOORD_INDEX), 5u, "texture-stage texcoord copied");
  checkEq(desc.samplers[0].states.at(SAMP_ADDRESS_U), 3u, "sampler state copied");
  checkEq(desc.samplers[0].states.at(SAMP_MAX_ANISOTROPY), 8u, "sampler anisotropy copied");

  checkEq(desc.vertexShader.kind, ShaderRef::Kind::FixedFunctionVertex, "fixed-function vertex shader selected");
  check(desc.vertexShader.vertexKey.has_value(), "fixed-function vertex key generated");
  checkEq(desc.vertexShader.hash, makeFfpVertexKey(state).hash, "fixed-function vertex hash matches state");
  checkEq(desc.pixelShader.kind, ShaderRef::Kind::FixedFunctionPixel, "fixed-function pixel shader selected");
  check(desc.pixelShader.pixelKey.has_value(), "fixed-function pixel key generated");
  checkEq(desc.pixelShader.hash, makeFfpPixelKey(state).hash, "fixed-function pixel hash matches state");

  checkEq(desc.clipPlaneMask, 0x3u, "clip plane mask copied");
  for (size_t i = 0; i < 4; ++i) {
    checkNear(desc.clipPlanes[0][i], state.clipPlanes[0][i], "enabled clip plane 0 propagated");
    checkNear(desc.clipPlanes[1][i], state.clipPlanes[1][i], "enabled clip plane 1 propagated");
    checkNear(desc.clipPlanes[2][i], 0.0f, "disabled clip plane cleared");
  }
  checkEq(desc.textureTransforms[0], state.transforms.at(XFORM_TEXTURE_BASE), "texture transform propagated");
}

void testConstantsAndShaderRefs() {
  DeviceState state;
  state.reset();

  state.vsConst.float4[7] = {1.25f, -2.5f, 3.75f, 4.5f};
  state.vsConst.float4[kMaxVertexConstants - 1] = {-10.0f, 11.0f, -12.0f, 13.0f};
  state.vsConst.int4[4] = {101, -202, 303, -404};
  state.vsConst.bools[3] = true;
  state.vsConst.bools[kMaxBoolConstants - 1] = true;

  state.psConst.float4[2] = {5.5f, 6.5f, 7.5f, 8.5f};
  state.psConst.float4[kMaxPixelConstants - 1] = {-1.0f, -2.0f, -3.0f, -4.0f};
  state.psConst.int4[5] = {-11, 22, -33, 44};
  state.psConst.bools[1] = true;
  state.psConst.bools[kMaxBoolConstants - 2] = true;

  state.vertexShader = makeBytecodeShader(0x1002003004005006ull, {0x01, 0x02, 0x03, 0x04});
  state.pixelShader = makeBytecodeShader(0x6005004003002001ull, {0xfe, 0xed, 0xfa, 0xce, 0x09});

  const DrawDesc desc = makeDrawDescFromState(state, {});

  checkEq(desc.vsConst, state.vsConst, "vertex shader constants copied");
  checkEq(desc.psConst, state.psConst, "pixel shader constants copied");
  checkEq(desc.vertexShader, state.vertexShader, "bytecode vertex shader ref copied");
  checkEq(desc.pixelShader, state.pixelShader, "bytecode pixel shader ref copied");
  checkEq(desc.vertexShader.kind, ShaderRef::Kind::Bytecode, "bytecode vertex shader preserved");
  checkEq(desc.pixelShader.kind, ShaderRef::Kind::Bytecode, "bytecode pixel shader preserved");
}

void testResourceBindingsAndAttachments() {
  DeviceState state;
  state.reset();

  const auto stream0 = makeBuffer(0x1010, 4096, UsageVertexBuffer);
  const auto stream3 = makeBuffer(0x3030, 8192, UsageVertexBuffer);
  const auto staleStream = makeBuffer(0x9999, 512, UsageVertexBuffer);
  const auto indexBuffer = makeBuffer(0x4040, 2048, UsageIndexBuffer);
  const auto texture0 = makeTexture(0x5050, Format::A8R8G8B8);
  const auto texture7 = makeTexture(0x5757, Format::DXT1);
  const auto texture15 = makeTexture(0x5f5f, Format::A16B16G16R16F);

  state.streamBuffers[0] = stream0;
  state.streamOffsets[0] = 12;
  state.streamStrides[0] = 32;
  state.streamBuffers[3] = stream3;
  state.streamOffsets[3] = 128;
  state.streamStrides[3] = 48;
  state.streamOffsets[5] = 256;
  state.streamStrides[5] = 64;
  state.indexBuffer = indexBuffer;

  state.vertexDecl.streams[0].buffer = staleStream;
  state.vertexDecl.streams[0].offset = 777;
  state.vertexDecl.streams[0].stride = 888;

  state.textures[0] = texture0;
  state.textures[kMaxTextureStages - 1] = texture7;
  state.textures[kMaxTextures - 1] = texture15;
  state.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg2);
  state.textureStageStates[kMaxTextureStages - 1][TSS_TEXTURE_TYPE] = 3;

  const auto rt0 = makeSurface(0x6000, Format::A8R8G8B8, MultiSampleType::None, true, false);
  const auto rt3 = makeSurface(0x6003, Format::A16B16G16R16F, MultiSampleType::Four, true, false);
  const auto depth = makeSurface(0x6d00, Format::D24S8, MultiSampleType::Two, false, true);
  const auto rtTexture = makeTexture(0x7000, Format::A8R8G8B8, 4, UsageTexture | UsageRenderTarget);
  const auto rtMip = std::make_shared<Surface>(std::shared_ptr<Device>{}, SurfaceHandle{0x6001},
                                               rtTexture, 2);

  state.renderTargets[0] = {rt0->handle(), rt0->level(), rt0->multiSampleCount()};
  state.renderTargets[1] = {rtMip->handle(), rtMip->level(), rtMip->multiSampleCount()};
  state.renderTargets[3] = {rt3->handle(), rt3->level(), rt3->multiSampleCount()};
  state.depthStencil = {depth->handle(), depth->level(), depth->multiSampleCount()};

  const DrawCallArgs args{PrimitiveType::TriangleList, 4, 6, -2, 9, IndexType::UInt32};
  const DrawDesc desc = makeDrawDescFromState(state, args);

  checkEq(desc.indexBuffer, indexBuffer->handle(), "index buffer handle copied");
  checkEq(desc.indexType, IndexType::UInt32, "index type copied with index buffer");

  checkEq(desc.vertexDecl.streams[0].buffer, stream0, "stream 0 buffer copied from device streams");
  checkEq(desc.vertexDecl.streams[0].offset, 12u, "stream 0 offset copied");
  checkEq(desc.vertexDecl.streams[0].stride, 32u, "stream 0 stride copied");
  checkEq(desc.vertexDecl.streams[3].buffer, stream3, "stream 3 buffer copied");
  checkEq(desc.vertexDecl.streams[3].offset, 128u, "stream 3 offset copied");
  checkEq(desc.vertexDecl.streams[3].stride, 48u, "stream 3 stride copied");
  check(!desc.vertexDecl.streams[5].buffer, "null stream buffer preserved");
  checkEq(desc.vertexDecl.streams[5].offset, 256u, "null stream offset copied");
  checkEq(desc.vertexDecl.streams[5].stride, 64u, "null stream stride copied");

  checkEq(desc.textures[0].handle, texture0->handle(), "texture 0 handle copied");
  checkEq(desc.textures[kMaxTextureStages - 1].handle, texture7->handle(),
          "last fixed-function texture handle copied");
  checkEq(desc.textures[kMaxTextures - 1].handle, texture15->handle(), "extra sampler texture handle copied");
  checkEq(desc.textures[0].stageStates.at(TSS_COLOR_OP), static_cast<u32>(TextureOp::SelectArg2),
          "texture stage 0 states copied");
  checkEq(desc.textures[kMaxTextureStages - 1].stageStates.at(TSS_TEXTURE_TYPE), 3u,
          "last texture stage states copied");
  check(desc.textures[kMaxTextures - 1].stageStates.empty(), "non-stage texture states cleared");

  checkEq(desc.rts.color[0], state.renderTargets[0], "render target 0 attachment copied");
  checkEq(desc.rts.color[1], state.renderTargets[1], "texture-level render target attachment copied");
  checkEq(desc.rts.color[2], RenderTargetAttachment{}, "unbound render target attachment remains empty");
  checkEq(desc.rts.color[3], state.renderTargets[3], "multisampled render target attachment copied");
  checkEq(desc.rts.depthStencil, state.depthStencil, "depth-stencil attachment copied");
}

void testVertexDeclFvfAndStreamBindings() {
  DeviceState state;
  state.reset();

  const auto stream2 = makeBuffer(0x2222, 1024, UsageVertexBuffer);
  const auto stream4 = makeBuffer(0x4444, 1024, UsageVertexBuffer);

  state.fvf = 0x11223344u;
  state.vertexDecl.fvf = state.fvf;
  state.vertexDecl.elements = {
      VertexElement{0, 0, 2, 0, 0, 0},
      VertexElement{2, 12, 3, 0, 5, 1},
      VertexElement{4, 24, 4, 1, 3, 0},
  };
  state.streamBuffers[2] = stream2;
  state.streamOffsets[2] = 20;
  state.streamStrides[2] = 28;
  state.streamBuffers[4] = stream4;
  state.streamOffsets[4] = 40;
  state.streamStrides[4] = 56;

  const DrawDesc desc = makeDrawDescFromState(state, {});

  checkEq(desc.vertexDecl.fvf, 0x11223344u, "FVF copied through vertex declaration snapshot");
  checkEq(desc.vertexDecl.elements, state.vertexDecl.elements, "vertex declaration elements copied");
  checkEq(desc.vertexDecl.streams[2].buffer, stream2, "decl stream 2 binding copied");
  checkEq(desc.vertexDecl.streams[2].offset, 20u, "decl stream 2 offset copied");
  checkEq(desc.vertexDecl.streams[2].stride, 28u, "decl stream 2 stride copied");
  checkEq(desc.vertexDecl.streams[4].buffer, stream4, "decl stream 4 binding copied");
  checkEq(desc.vertexDecl.streams[4].offset, 40u, "decl stream 4 offset copied");
  checkEq(desc.vertexDecl.streams[4].stride, 56u, "decl stream 4 stride copied");
}

void testFlatDrawStateKey() {
  DeviceState state;
  state.reset();

  const auto stream0 = makeBuffer(0x8100, 4096, UsageVertexBuffer);
  const auto indexBuffer = makeBuffer(0x8200, 2048, UsageIndexBuffer);
  const auto texture0 = makeTexture(0x8300, Format::A8R8G8B8);
  const auto rt0 = makeSurface(0x8400, Format::A8R8G8B8, MultiSampleType::None, true, false);
  const auto depth = makeSurface(0x8500, Format::D24S8, MultiSampleType::None, false, true);

  state.streamBuffers[0] = stream0;
  state.streamOffsets[0] = 16;
  state.streamStrides[0] = 32;
  state.indexBuffer = indexBuffer;
  state.textures[0] = texture0;
  state.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::Modulate);
  state.samplerStates[0][SAMP_ADDRESS_U] = 1;
  state.renderStates[RS_Z_ENABLE] = 1;
  state.renderStates[RS_CULL_MODE] = static_cast<u32>(CullMode::Ccw);
  state.renderTargets[0] = {rt0->handle(), rt0->level(), rt0->multiSampleCount()};
  state.depthStencil = {depth->handle(), depth->level(), depth->multiSampleCount()};
  state.vertexDecl.fvf = 0x120u;
  state.vertexDecl.elements = {
      VertexElement{0, 0, 2, 0, 0, 0},
      VertexElement{0, 12, 3, 0, 5, 0},
  };
  state.vsConst.float4[0] = {1.0f, 2.0f, 3.0f, 4.0f};
  state.transforms[XFORM_WORLD_BASE] = taggedMatrix(10.0f);

  const DrawDesc first = makeDrawDescFromState(
      state, {PrimitiveType::TriangleList, 2, 0, 0, 0, IndexType::UInt16});
  const DrawDesc sameStateDifferentDraw = makeDrawDescFromState(
      state, {PrimitiveType::LineList, 7, 33, -4, 19, IndexType::UInt32});
  const FlatDrawStateKey firstKey = makeFlatDrawStateKey(first);

  checkEq(firstKey, makeFlatDrawStateKey(first),
          "identical draw state summaries compare equal");
  checkEq(firstKey, makeFlatDrawStateKey(sameStateDifferentDraw),
          "draw parameters do not disturb flat base draw state");

  DrawDesc withUserPayload = first;
  withUserPayload.userVertexData = {0xde, 0xad, 0xbe, 0xef};
  withUserPayload.userIndexData = {0x01, 0x02};
  checkEq(firstKey, makeFlatDrawStateKey(withUserPayload),
          "UP payload is excluded from flat base draw state");

  DrawParam drawParamA{};
  drawParamA.primitiveType = PrimitiveType::TriangleList;
  drawParamA.primitiveCount = 2;
  drawParamA.indexed = false;

  DrawParam drawParamB{};
  drawParamB.primitiveType = PrimitiveType::LineList;
  drawParamB.primitiveCount = 7;
  drawParamB.startVertex = 33;
  drawParamB.baseVertexIndex = -4;
  drawParamB.startIndex = 19;
  drawParamB.indexType = IndexType::UInt32;
  drawParamB.indexed = true;
  drawParamB.userVertexData = {0xde, 0xad, 0xbe, 0xef};
  drawParamB.userIndexData = {0x01, 0x02, 0x03, 0x04};

  DrawRunDesc run{};
  run.state = makeCanonicalDrawState(first);
  run.draws = {drawParamA, drawParamB};

  const auto drawDescForParam = [&run](const DrawParam& param) {
    DrawDesc desc = run.state.desc;
    desc.primitiveType = param.primitiveType;
    desc.primitiveCount = param.primitiveCount;
    desc.startVertex = param.startVertex;
    desc.baseVertexIndex = param.baseVertexIndex;
    desc.startIndex = param.startIndex;
    desc.indexType = param.indexType;
    desc.userVertexData = param.userVertexData;
    desc.userIndexData = param.userIndexData;
    return desc;
  };

  checkEq(run.state.key, makeFlatDrawStateKey(run.state.desc),
          "draw-run stores the flat base-state decision key");
  checkEq(run.state.key, makeFlatDrawStateKey(drawDescForParam(run.draws[0])),
          "draw-run key ignores first draw param fields");
  checkEq(run.state.key, makeFlatDrawStateKey(drawDescForParam(run.draws[1])),
          "draw-run key ignores varying draw param fields and UP payloads");
  const FlatDrawStateKey storedRunKey = run.state.key;
  run.draws[1].primitiveCount = 11;
  run.draws[1].startVertex = 64;
  run.draws[1].userVertexData = {0xaa, 0xbb, 0xcc};
  run.draws[1].userIndexData = {0x10, 0x11};
  checkEq(run.state.key, storedRunKey,
          "draw-run key remains separate from mutable draw params");
  checkEq(run.state.key, makeFlatDrawStateKey(drawDescForParam(run.draws[1])),
          "mutated draw params and UP payloads still do not alter the run key");

  DrawRunDesc packedRun{};
  packedRun.state = makeCanonicalDrawState(first);
  packedRun.payloadArena = {0xde, 0xad, 0xbe, 0xef, 0x10, 0x11};
  DrawParam packedParam = drawParamB;
  packedParam.userVertexData.clear();
  packedParam.userIndexData.clear();
  packedParam.userVertexRange = {0, 4};
  packedParam.userIndexRange = {4, 2};
  packedRun.draws = {packedParam};
  checkEq(packedRun.state.key, firstKey,
          "draw-run payload arena does not disturb the base-state key");
  checkEq(packedRun.draws[0].userVertexRange.offset, 0u,
          "draw-run stores vertex UP payload as an arena offset");
  checkEq(packedRun.draws[0].userIndexRange.size, 2u,
          "draw-run stores index UP payload as an arena size");
  const FlatDrawStateKey packedRunKey = packedRun.state.key;
  packedRun.payloadArena[0] = 0xaa;
  packedRun.draws[0].userVertexRange.size = 3;
  checkEq(packedRun.state.key, packedRunKey,
          "payload arena mutations remain separate from the draw-run key");

  ChunkSlot slot{};
  slot.appendDraw(withUserPayload);
  const auto drawView = slot.commandAt(0);
  check(drawView.drawRecord != nullptr, "slot draw command exposes compact draw record");
  check(drawView.drawState != nullptr, "slot draw command indexes canonical draw state");
  check(drawView.drawParam != nullptr, "slot draw command indexes draw param table");
  checkEq(drawView.drawState->key, firstKey,
          "slot draw state stores the canonical flat key");
  checkEq(drawView.drawParam->userVertexRange.offset, 0u,
          "slot draw payload stores vertex bytes in slot arena");
  checkEq(drawView.drawParam->userIndexRange.offset, 4u,
          "slot draw payload stores index bytes after vertex bytes");
  checkEq(slot.drawPayloadArena.size(), std::size_t{6},
          "slot draw payload arena owns single-draw UP bytes");

  packedRun.payloadArena[0] = 0xde;
  packedRun.draws[0].userVertexRange = {0, 4};
  slot.appendDrawRun(std::move(packedRun));
  const auto runView = slot.commandAt(1);
  check(runView.drawRunRecord != nullptr, "slot draw-run command exposes compact run record");
  check(runView.drawState != nullptr, "slot draw-run indexes canonical draw state");
  checkEq(runView.drawParams.size(), std::size_t{1},
          "slot draw-run indexes shared draw param table");
  checkEq(runView.drawParams[0].userVertexRange.offset, 6u,
          "slot draw-run vertex payload range is rebased into slot arena");
  checkEq(runView.drawParams[0].userIndexRange.offset, 10u,
          "slot draw-run index payload range is rebased into slot arena");
  checkEq(slot.drawPayloadArena.size(), std::size_t{12},
          "slot payload arena owns draw and draw-run bytes together");

  DrawDesc sameMapsDifferentInsertion = first;
  sameMapsDifferentInsertion.rs.values.clear();
  sameMapsDifferentInsertion.rs.values[RS_SRC_BLEND] = static_cast<u32>(BlendFactor::One);
  sameMapsDifferentInsertion.rs.values[RS_DEST_BLEND] = static_cast<u32>(BlendFactor::Zero);
  DrawDesc sameMapsOriginalInsertion = first;
  sameMapsOriginalInsertion.rs.values.clear();
  sameMapsOriginalInsertion.rs.values[RS_DEST_BLEND] = static_cast<u32>(BlendFactor::Zero);
  sameMapsOriginalInsertion.rs.values[RS_SRC_BLEND] = static_cast<u32>(BlendFactor::One);
  checkEq(makeFlatDrawStateKey(sameMapsOriginalInsertion),
          makeFlatDrawStateKey(sameMapsDifferentInsertion),
          "flat base draw state map hashes are insertion-order independent");

  DeviceState changedTexture = state;
  changedTexture.textures[0] = makeTexture(0x8301, Format::A8R8G8B8);
  check(!(makeFlatDrawStateKey(first) ==
          makeFlatDrawStateKey(makeDrawDescFromState(changedTexture, {}))),
        "texture handle changes flat base draw state");

  DeviceState changedRenderState = state;
  changedRenderState.renderStates[RS_CULL_MODE] = static_cast<u32>(CullMode::None);
  DrawRunDesc changedRun{};
  changedRun.state = makeCanonicalDrawState(makeDrawDescFromState(changedRenderState, {}));
  check(!(firstKey == changedRun.state.key),
        "render-state changes flat base draw state");
  check(!(run.state.key == changedRun.state.key),
        "draw-run key changes when base state changes");

  DeviceState changedStream = state;
  changedStream.streamStrides[0] = 48;
  check(!(makeFlatDrawStateKey(first) ==
          makeFlatDrawStateKey(makeDrawDescFromState(changedStream, {}))),
        "stream binding changes flat base draw state");

  DeviceState changedConstants = state;
  changedConstants.vsConst.float4[0][2] = 99.0f;
  check(!(makeFlatDrawStateKey(first) ==
          makeFlatDrawStateKey(makeDrawDescFromState(changedConstants, {}))),
        "shader constant changes flat base draw state");
}

}  // namespace

int main() {
  testStateDrawTransform();
  testConstantsAndShaderRefs();
  testResourceBindingsAndAttachments();
  testVertexDeclFvfAndStreamBindings();
  testFlatDrawStateKey();
  return 0;
}
