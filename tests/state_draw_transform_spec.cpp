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
using namespace dxmt9::core::fixture;

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

CanonicalDrawState makeCanonicalDrawStateForTest(const DrawDesc& desc) {
  auto hot = makeFlatDrawStateRecord(desc);
  auto shaderLayout = makeDrawShaderLayoutContext(desc);
  auto debug = makeDrawDebugSnapshot(desc, hot);
  return CanonicalDrawState{std::move(hot), std::move(shaderLayout), std::move(debug)};
}

DrawParam makeDrawParamForTest(const DrawDesc& desc) {
  DrawParam param{};
  param.primitiveType = desc.primitiveType;
  param.primitiveCount = desc.primitiveCount;
  param.startVertex = desc.startVertex;
  param.baseVertexIndex = desc.baseVertexIndex;
  param.startIndex = desc.startIndex;
  param.indexType = desc.indexType;
  param.indexed = desc.indexBuffer || !desc.userIndexData.empty();
  return param;
}

void testChunkSlotU32GuardBoundaries() {
  const auto u32Max = detail::kChunkSlotU32Max;

  check(detail::chunkSlotCanAppendU32IndexedElement(0),
        "slot u32 guard accepts an empty indexed table");
  check(detail::chunkSlotCanAppendU32IndexedElement(u32Max - 1u),
        "slot u32 guard accepts the last appendable indexed table slot");
  check(!detail::chunkSlotCanAppendU32IndexedElement(u32Max),
        "slot u32 guard rejects an indexed table append past u32 range");

  check(detail::chunkSlotCanAppendU32Range(0, u32Max),
        "slot u32 range guard accepts the largest representable param count");
  check(detail::chunkSlotCanAppendU32Range(u32Max, 0),
        "slot u32 range guard accepts an empty range at the u32 boundary");
  check(!detail::chunkSlotCanAppendU32Range(u32Max, 1),
        "slot u32 range guard rejects range growth past u32 storage");
  check(!detail::chunkSlotCanAppendU32Range(u32Max - 1u, 2),
        "slot u32 range guard rejects overflowing appended param count");

  check(detail::chunkSlotCanAppendCommandPayload(0, 0),
        "slot command payload guard accepts empty command and payload tables");
  check(detail::chunkSlotCanAppendCommandPayload(u32Max - 1u, u32Max - 1u),
        "slot command payload guard accepts last appendable command and payload slots");
  check(!detail::chunkSlotCanAppendCommandPayload(u32Max, 0),
        "slot command payload guard rejects command header append past u32 range");
  check(!detail::chunkSlotCanAppendCommandPayload(0, u32Max),
        "slot command payload guard rejects payload append past u32 range");

  u32 payloadIndex = 0;
  check(detail::chunkSlotTryMakeCommandPayloadIndex(3, 7, payloadIndex),
        "slot command payload helper accepts in-range payload index");
  checkEq(payloadIndex, 7u,
          "slot command payload helper casts payload index after validating range");

  if constexpr (sizeof(std::size_t) > sizeof(u32)) {
    check(!detail::chunkSlotCanAppendU32Range(u32Max + 1u, 0),
          "slot u32 range guard rejects pre-existing counts outside u32 storage");
  }
}

void testChunkSlotSimpleCommandSoAViews() {
  ChunkSlot slot{};

  ClearDesc clear{};
  clear.clearColor = true;
  clear.color = {0.25f, 0.5f, 0.75f, 1.0f};
  clear.rects.push_back(Rect{1, 2, 3, 4});
  slot.appendClear(clear);

  SurfaceCopyDesc surfaceCopy{};
  surfaceCopy.source = Handle{0x1010};
  surfaceCopy.destination = Handle{0x2020};
  surfaceCopy.sourceRect = Rect{5, 6, 7, 8};
  surfaceCopy.destinationRect = Rect{9, 10, 11, 12};
  surfaceCopy.sourceLevel = 2;
  surfaceCopy.destinationLevel = 3;
  slot.appendSurfaceCopy(surfaceCopy);

  StretchRectDesc stretchRect{};
  stretchRect.source = Handle{0x3030};
  stretchRect.destination = Handle{0x4040};
  stretchRect.sourceRect = Rect{13, 14, 15, 16};
  stretchRect.destinationRect = Rect{17, 18, 19, 20};
  stretchRect.linear = true;
  slot.appendStretchRect(stretchRect);

  ReadbackDesc readback{};
  readback.source = Handle{0x5050};
  readback.destination = Handle{0x6060};
  readback.sourceRect = Rect{21, 22, 23, 24};
  readback.sourceLevel = 4;
  slot.appendReadback(readback);

  ColorFillDesc colorFill{};
  colorFill.destination = Handle{0x7070};
  colorFill.rect = Rect{25, 26, 27, 28};
  colorFill.hasRect = true;
  colorFill.color = {1.0f, 0.0f, 0.5f, 1.0f};
  slot.appendColorFill(colorFill);

  SwapDesc present{};
  present.window = Handle{0x8080};
  present.sourceSurface = Handle{0x9090};
  present.width = 800;
  present.height = 600;
  present.interval = PresentInterval::Immediate;
  slot.appendPresent(present, Handle{0xa0a0});

  checkEq(slot.commandCount(), std::size_t{6},
          "slot simple command appends produce one command header each");
  checkEq(slot.clearRecords.size(), std::size_t{1},
          "slot simple command append stores one clear payload");
  checkEq(slot.surfaceCopyRecords.size(), std::size_t{1},
          "slot simple command append stores one surface-copy payload");
  checkEq(slot.stretchRectRecords.size(), std::size_t{1},
          "slot simple command append stores one stretch-rect payload");
  checkEq(slot.readbackRecords.size(), std::size_t{1},
          "slot simple command append stores one readback payload");
  checkEq(slot.colorFillRecords.size(), std::size_t{1},
          "slot simple command append stores one color-fill payload");
  checkEq(slot.presentRecords.size(), std::size_t{1},
          "slot simple command append stores one present payload");

  for (const auto& header : slot.commandHeaders) {
    checkEq(header.payloadIndex, 0u,
            "slot simple command header stores a u32 payload index");
  }

  const auto clearView = slot.commandAt(0);
  checkEq(clearView.kind, MetalCommandKind::Clear, "slot clear view reports command kind");
  check(clearView.clear != nullptr, "slot clear view resolves payload");
  check(clearView.clear->clearColor, "slot clear payload preserves clear-color flag");
  checkEq(clearView.clear->rects[0], Rect{1, 2, 3, 4},
          "slot clear payload preserves rect data");

  const auto surfaceCopyView = slot.commandAt(1);
  checkEq(surfaceCopyView.kind, MetalCommandKind::SurfaceCopy,
          "slot surface-copy view reports command kind");
  check(surfaceCopyView.surfaceCopy != nullptr, "slot surface-copy view resolves payload");
  checkEq(surfaceCopyView.surfaceCopy->source, Handle{0x1010},
          "slot surface-copy payload preserves source handle");
  checkEq(surfaceCopyView.surfaceCopy->destinationLevel, 3u,
          "slot surface-copy payload preserves destination level");

  const auto stretchRectView = slot.commandAt(2);
  checkEq(stretchRectView.kind, MetalCommandKind::StretchRect,
          "slot stretch-rect view reports command kind");
  check(stretchRectView.stretchRect != nullptr, "slot stretch-rect view resolves payload");
  check(stretchRectView.stretchRect->linear,
        "slot stretch-rect payload preserves filter mode");

  const auto readbackView = slot.commandAt(3);
  checkEq(readbackView.kind, MetalCommandKind::Readback,
          "slot readback view reports command kind");
  check(readbackView.readback != nullptr, "slot readback view resolves payload");
  checkEq(readbackView.readback->sourceLevel, 4u,
          "slot readback payload preserves source level");

  const auto colorFillView = slot.commandAt(4);
  checkEq(colorFillView.kind, MetalCommandKind::ColorFill,
          "slot color-fill view reports command kind");
  check(colorFillView.colorFill != nullptr, "slot color-fill view resolves payload");
  checkEq(colorFillView.colorFill->rect, Rect{25, 26, 27, 28},
          "slot color-fill payload preserves rect");

  const auto presentView = slot.commandAt(5);
  checkEq(presentView.kind, MetalCommandKind::Present,
          "slot present view reports command kind");
  check(presentView.present != nullptr, "slot present view resolves payload");
  checkEq(presentView.present->present.window, Handle{0x8080},
          "slot present payload preserves present desc");
  checkEq(presentView.present->presentSource, Handle{0xa0a0},
          "slot present payload preserves explicit source");

  slot.clearCommands();
  check(slot.commandsEmpty(), "slot clearCommands removes command headers");
  check(!slot.lastUniformHandle.valid(),
        "slot clearCommands resets recent uniform handle");
}

void testChunkSlotDirectDrawRunUniformLookup() {
  DrawDesc base{};
  base.primitiveCount = 1;
  const auto uniformA = makeDrawUniformPayload(base);
  DrawDesc changed = base;
  changed.clipPlaneMask = 1u;
  const auto uniformB = makeDrawUniformPayload(changed);
  check(!(uniformA == uniformB),
        "test direct chunk-slot uniform payload variants differ");

  DrawParam draw{};
  draw.primitiveCount = 1;
  const std::array<DrawParam, 1> draws{draw};
  const std::array<u8, 4> vertexPayload{0x10, 0x20, 0x30, 0x40};
  const std::array<DrawParamPayloadView, 1> payloads{
      DrawParamPayloadView{
          .userVertexData = std::span<const u8>(vertexPayload.data(), vertexPayload.size()),
      },
  };

  ChunkSlot slot{};
  slot.appendDrawRun(makeCanonicalDrawStateForTest(base), uniformA,
                     std::span<const DrawParam>(draws.data(), draws.size()),
                     std::span<const DrawParamPayloadView>(payloads.data(), payloads.size()));
  const auto firstView = slot.commandAt(0);
  check(firstView.drawRunRecord != nullptr,
        "slot direct draw-run stores the first uniform payload");
  checkEq(slot.drawUniformPayloads.size(), std::size_t{1},
          "slot direct draw-run appends the first uniform payload");
  const auto uniformAHandle = firstView.drawRunRecord->uniformHandle;
  checkEq(slot.lastUniformHandle, uniformAHandle,
          "slot direct draw-run records the first uniform as recent");

  slot.appendDrawRun(makeCanonicalDrawStateForTest(changed), uniformB,
                     std::span<const DrawParam>(draws.data(), draws.size()),
                     std::span<const DrawParamPayloadView>{});
  const auto secondView = slot.commandAt(1);
  check(secondView.drawRunRecord != nullptr,
        "slot direct draw-run stores the changed uniform payload");
  checkEq(slot.drawUniformPayloads.size(), std::size_t{2},
          "slot direct draw-run appends a changed uniform payload");
  const auto uniformBHandle = secondView.drawRunRecord->uniformHandle;
  checkEq(slot.lastUniformHandle, uniformBHandle,
          "slot direct draw-run records the changed uniform as recent");

  slot.appendDrawRun(makeCanonicalDrawStateForTest(base), uniformA,
                     std::span<const DrawParam>(draws.data(), draws.size()),
                     std::span<const DrawParamPayloadView>{});
  const auto reusedView = slot.commandAt(2);
  check(reusedView.drawRunRecord != nullptr,
        "slot direct reused-uniform draw-run exposes compact run record");
  checkEq(slot.drawUniformPayloads.size(), std::size_t{2},
          "slot indexed uniform lookup reuses a non-recent direct payload");
  checkEq(reusedView.drawRunRecord->uniformHandle, uniformAHandle,
          "slot direct indexed uniform lookup returns the older payload handle");
  checkEq(slot.lastUniformHandle, uniformAHandle,
          "slot direct indexed uniform hit becomes the recent uniform handle");
  checkEq(slot.drawUniformPayloadLookupNext.size(), slot.drawUniformPayloads.size(),
          "slot direct uniform lookup tracks every interned payload");

  slot.clearCommands();
  check(slot.drawUniformPayloadLookupHeads.empty(),
        "slot clearCommands resets uniform lookup heads");
  check(slot.drawUniformPayloadLookupTails.empty(),
        "slot clearCommands resets uniform lookup tails");
  check(slot.drawUniformPayloadLookupNext.empty(),
        "slot clearCommands resets uniform lookup links");
}

void testStateValueTableDirtyHashContract() {
  RenderStateTable a;
  RenderStateTable b;

  a.set(RS_Z_ENABLE, 1u);
  b.set(RS_Z_ENABLE, 1u);
  const u64 firstHash = a.rollingHash;
  checkEq(a, b, "state value table equality ignores dirty metadata");
  check((a.dirty[RenderStateTable::word(RS_Z_ENABLE)] & RenderStateTable::bit(RS_Z_ENABLE)) != 0,
        "state value table marks dirty bit on insert");

  a.clearDirty();
  checkEq(a, b, "state value table remains equal after clearing dirty metadata");
  a.set(RS_Z_ENABLE, 1u);
  checkEq(a.rollingHash, firstHash, "state value table hash is stable for redundant set");
  check(a.dirty[RenderStateTable::word(RS_Z_ENABLE)] == 0u,
        "state value table redundant set does not mark dirty");

  a.set(RS_Z_ENABLE, 0u);
  check(a.rollingHash != firstHash, "state value table hash changes on value update");
  check((a.dirty[RenderStateTable::word(RS_Z_ENABLE)] & RenderStateTable::bit(RS_Z_ENABLE)) != 0,
        "state value table marks dirty bit on value update");

  a.erase(RS_Z_ENABLE);
  check(!a.contains(RS_Z_ENABLE), "state value table erase clears occupancy");
  checkEq(a.size(), std::size_t{0}, "state value table erase updates count");
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
  auto directCanonical = makeCanonicalDrawStateFromState(
      state, {PrimitiveType::TriangleList, 2, 0, 0, 0, IndexType::UInt16});

  checkEq(firstKey, makeFlatDrawStateKey(first),
          "identical draw state summaries compare equal");
  checkEq(directCanonical.hot.key, firstKey,
          "direct state canonicalization builds the same flat key from fixed state tables");
  checkEq(directCanonical.hot.renderStates.hash, firstKey.renderStateHash,
          "direct state canonicalization carries render-state hash in hot storage");
  checkEq(flatStateOr(directCanonical.hot.renderStates, RS_Z_ENABLE, 0u), 1u,
          "direct state canonicalization copies fixed render-state table into hot storage");
  checkEq(directCanonical.shaderLayout.vertexDecl.fvf, first.vertexDecl.fvf,
          "direct state canonicalization carries shader/layout context separately");
  checkEq(directCanonical.hot.textures[0], texture0->handle(),
          "direct state canonicalization carries resource handles in hot storage");
  checkEq(directCanonical.debug.primitiveCount, 2u,
          "direct state canonicalization records draw debug parameters separately");
  checkEq(directCanonical.debug.userVertexBytes, 0u,
          "direct state canonicalization keeps payload data out of canonical state");
  checkEq(firstKey, makeFlatDrawStateKey(sameStateDifferentDraw),
          "draw parameters do not disturb flat base draw state");

  DrawDesc withUserPayload = first;
  withUserPayload.userVertexData = {0xde, 0xad, 0xbe, 0xef};
  withUserPayload.userIndexData = {0x01, 0x02};
  const auto sharedUniformPayload = makeDrawUniformPayload(withUserPayload);
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
  const std::vector<u8> drawParamBVertexData{0xde, 0xad, 0xbe, 0xef};
  const std::vector<u8> drawParamBIndexData{0x01, 0x02, 0x03, 0x04};

  DrawRunDesc run{};
  run.state = makeCanonicalDrawStateForTest(first);
  check(drawRunAppend(run, drawParamA),
        "test draw param A appends into run storage");
  check(drawRunAppend(
            run, drawParamB,
            DrawParamPayloadView{
                .userVertexData = std::span<const u8>(drawParamBVertexData.data(),
                                                      drawParamBVertexData.size()),
                .userIndexData = std::span<const u8>(drawParamBIndexData.data(),
                                                     drawParamBIndexData.size()),
            }),
        "test draw param B payload packs into run arena");
  check(drawRunValidate(run), "test draw run validates after append");

  const auto drawDescForParam = [&first, &run](const DrawParam& param) {
    DrawDesc desc = first;
    desc.primitiveType = param.primitiveType;
    desc.primitiveCount = param.primitiveCount;
    desc.startVertex = param.startVertex;
    desc.baseVertexIndex = param.baseVertexIndex;
    desc.startIndex = param.startIndex;
    desc.indexType = param.indexType;
    const auto vertexBytes = drawRunPayloadBytes(run, param.userVertexRange);
    const auto indexBytes = drawRunPayloadBytes(run, param.userIndexRange);
    desc.userVertexData.assign(vertexBytes.begin(), vertexBytes.end());
    desc.userIndexData.assign(indexBytes.begin(), indexBytes.end());
    return desc;
  };

  checkEq(run.state.hot.key, firstKey,
          "draw-run stores the flat base-state decision key in hot state");
  checkEq(run.state.hot.key, makeFlatDrawStateRecord(first).key,
          "flat draw state record exposes the canonical key");
  checkEq(run.state.shaderLayout.vertexShader.hash, first.vertexShader.hash,
          "canonical shader/layout context carries vertex shader identity");
  checkEq(run.state.shaderLayout.pixelShader.hash, first.pixelShader.hash,
          "canonical shader/layout context carries pixel shader identity");
  checkEq(run.state.shaderLayout.vertexDecl.fvf, first.vertexDecl.fvf,
          "canonical shader/layout context carries vertex layout");
  checkEq(run.state.debug.streamMask, run.state.hot.streamMask,
          "canonical debug snapshot records hot stream mask");
  checkEq(run.state.debug.textureMask, run.state.hot.textureMask,
          "canonical debug snapshot records hot texture mask");
  checkEq(run.state.debug.renderStateHash, run.state.hot.key.renderStateHash,
          "canonical debug snapshot records hot render-state hash");
  checkEq(run.state.hot.streamBuffers[0], stream0->handle(),
          "flat draw state record exposes hot stream buffer handles");
  checkEq(run.state.hot.streamOffsets[0], 16u,
          "flat draw state record exposes hot stream offsets");
  checkEq(run.state.hot.streamStrides[0], 32u,
          "flat draw state record exposes hot stream strides");
  checkEq(run.state.hot.textures[0], texture0->handle(),
          "flat draw state record exposes hot texture handles");
  checkEq(flatStateOr(run.state.hot.renderStates, RS_Z_ENABLE, 0u), 1u,
          "flat draw state record stores render states in fixed hot storage");
  checkEq(run.state.hot.renderStates.hash, run.state.hot.key.renderStateHash,
          "flat render-state storage carries the canonical hash");
  checkEq(flatStateOr(run.state.hot.textureStageStates[0], TSS_COLOR_OP, 0u),
          static_cast<u32>(TextureOp::Modulate),
          "flat draw state record stores TSS in fixed hot storage");
  checkEq(run.state.hot.textureStageStates[0].hash,
          run.state.hot.key.textureStageStateHashes[0],
          "flat TSS storage carries the canonical hash");
  checkEq(flatStateOr(run.state.hot.samplerStates[0], SAMP_ADDRESS_U, 0u), 1u,
          "flat draw state record stores sampler states in fixed hot storage");
  checkEq(run.state.hot.samplerStates[0].hash,
          run.state.hot.key.samplerStateHashes[0],
          "flat sampler storage carries the canonical hash");
  checkEq(run.state.hot.colorAttachments[0].handle, rt0->handle(),
          "flat draw state record exposes hot color attachments");
  checkEq(run.state.hot.depthStencil.handle, depth->handle(),
          "flat draw state record exposes hot depth attachment");
  checkEq(run.state.view().key(), run.state.hot.key,
          "flat draw state view exposes hot flat key");
  check(&run.state.view().shaderContext() == &run.state.shaderLayout,
        "flat draw state view exposes shader/layout context");
  check(&run.state.view().debugSnapshot() == &run.state.debug,
        "flat draw state view exposes debug snapshot");
  checkEq(run.state.hot.key, makeFlatDrawStateKey(drawDescForParam(drawRunView(run).draws[0])),
          "draw-run key ignores first draw param fields");
  checkEq(run.state.hot.key, makeFlatDrawStateKey(drawDescForParam(drawRunView(run).draws[1])),
          "draw-run key ignores varying draw param fields and UP payloads");
  const FlatDrawStateKey storedRunKey = run.state.hot.key;
  DrawParam mutatedDrawParamB = drawParamB;
  mutatedDrawParamB.primitiveCount = 11;
  mutatedDrawParamB.startVertex = 64;
  const std::array<u8, 3> mutatedDrawParamBVertexData{0xaa, 0xbb, 0xcc};
  const std::array<u8, 2> mutatedDrawParamBIndexData{0x10, 0x11};
  drawRunClear(run);
  check(drawRunAppend(run, drawParamA),
        "test draw param A re-appends into run storage");
  check(drawRunAppend(
            run, mutatedDrawParamB,
            DrawParamPayloadView{
                .userVertexData = std::span<const u8>(mutatedDrawParamBVertexData.data(),
                                                      mutatedDrawParamBVertexData.size()),
                .userIndexData = std::span<const u8>(mutatedDrawParamBIndexData.data(),
                                                     mutatedDrawParamBIndexData.size()),
            }),
        "test mutated draw param B payload packs into run arena");
  check(drawRunValidate(run), "mutated test draw run validates after append");
  checkEq(run.state.hot.key, storedRunKey,
          "draw-run key remains separate from mutable draw params");
  checkEq(run.state.hot.key, makeFlatDrawStateKey(drawDescForParam(drawRunView(run).draws[1])),
          "mutated draw params and UP payloads still do not alter the run key");

  auto payloadCanonical = makeCanonicalDrawStateForTest(withUserPayload);
  checkEq(payloadCanonical.debug.userVertexBytes, 4u,
          "canonical debug snapshot keeps stripped vertex payload byte count");
  checkEq(payloadCanonical.debug.userIndexBytes, 2u,
          "canonical debug snapshot keeps stripped index payload byte count");

  DrawRunDesc packedRun{};
  packedRun.state = makeCanonicalDrawStateForTest(first);
  const std::array<u8, 4> packedRunVertexData{0xde, 0xad, 0xbe, 0xef};
  const std::array<u8, 2> packedRunIndexData{0x10, 0x11};
  check(drawRunAppend(
            packedRun, drawParamB,
            DrawParamPayloadView{
                .userVertexData = std::span<const u8>(packedRunVertexData.data(),
                                                      packedRunVertexData.size()),
                .userIndexData = std::span<const u8>(packedRunIndexData.data(),
                                                     packedRunIndexData.size()),
            }),
        "test packed draw-run payload appends into run storage");
  check(drawRunValidate(packedRun), "packed draw-run validates after append");
  checkEq(packedRun.state.hot.key, firstKey,
          "draw-run payload arena does not disturb the base-state key");
  checkEq(drawRunView(packedRun).draws[0].userVertexRange.offset, 0u,
          "draw-run stores vertex UP payload as an arena offset");
  checkEq(drawRunView(packedRun).draws[0].userIndexRange.size, 2u,
          "draw-run stores index UP payload as an arena size");
  const FlatDrawStateKey packedRunKey = packedRun.state.hot.key;
  const std::array<u8, 3> smallerPackedRunVertexData{0xaa, 0xad, 0xbe};
  drawRunClear(packedRun);
  check(drawRunAppend(
            packedRun, drawParamB,
            DrawParamPayloadView{
                .userVertexData = std::span<const u8>(smallerPackedRunVertexData.data(),
                                                      smallerPackedRunVertexData.size()),
                .userIndexData = std::span<const u8>(packedRunIndexData.data(),
                                                     packedRunIndexData.size()),
            }),
        "test mutated packed draw-run payload appends into run storage");
  check(drawRunValidate(packedRun), "mutated packed draw-run validates after append");
  checkEq(packedRun.state.hot.key, packedRunKey,
          "payload arena mutations remain separate from the draw-run key");

  ChunkSlot slot{};
  DrawParam singleParam = makeDrawParamForTest(withUserPayload);
  const DrawParamPayloadView singlePayload{
      .userVertexData = std::span<const u8>(withUserPayload.userVertexData.data(),
                                            withUserPayload.userVertexData.size()),
      .userIndexData = std::span<const u8>(withUserPayload.userIndexData.data(),
                                           withUserPayload.userIndexData.size()),
  };
  slot.appendDrawRun(makeCanonicalDrawStateForTest(withUserPayload), sharedUniformPayload,
                     std::span<const DrawParam>(&singleParam, 1),
                     std::span<const DrawParamPayloadView>(&singlePayload, 1));
  const auto drawView = slot.commandAt(0);
  check(drawView.drawRunRecord != nullptr, "slot single-draw command exposes compact run record");
  check(drawView.drawState.hot != nullptr, "slot draw command indexes hot draw state");
  check(drawView.drawState.shaderLayout != nullptr,
        "slot draw command indexes shader layout state");
  check(drawView.drawState.debug != nullptr,
        "slot draw command indexes debug state");
  checkEq(drawView.drawParams.size(), std::size_t{1},
          "slot single-draw command indexes draw param table");
  checkEq(drawView.drawState.key(), firstKey,
          "slot draw state stores the canonical flat key");
  checkEq(drawView.drawParams[0].userVertexRange.offset, 0u,
          "slot draw payload stores vertex bytes at a record-local offset");
  checkEq(drawView.drawParams[0].userIndexRange.offset, 4u,
          "slot draw payload stores index bytes after vertex bytes in the record payload");
  checkEq(drawView.drawRunRecord->payloadOffset, 0u,
          "slot single-draw record starts at first payload arena byte");
  checkEq(drawView.drawRunRecord->payloadSize, 6u,
          "slot single-draw record stores its payload byte count");
  checkEq(drawView.drawPayloadBytes.size(), std::size_t{6},
          "slot single-draw view exposes a record-local payload span");
  check(drawView.drawPayloadBytes.data() == slot.drawPayloadArena.data(),
        "slot single-draw payload span starts at the record offset");
  checkEq(slot.drawPayloadArena.size(), std::size_t{6},
          "slot draw payload arena owns single-draw UP bytes");
  check(drawView.drawUniformPayload != nullptr,
        "slot single-draw command resolves its uniform payload");
  checkEq(slot.drawUniformPayloads.size(), std::size_t{1},
          "slot single-draw command stores one uniform payload");
  const auto sharedUniformHandle = drawView.drawRunRecord->uniformHandle;
  checkEq(slot.lastUniformHandle, sharedUniformHandle,
          "slot remembers recently appended uniform payload handle");

  auto directPackedState = makeCanonicalDrawStateForTest(first);
  directPackedState.debug.renderStateHash = 99ull;
  const DrawParamPayloadView packedPayload{
      .userVertexData = std::span<const u8>(packedRunVertexData.data(),
                                            packedRunVertexData.size()),
      .userIndexData = std::span<const u8>(packedRunIndexData.data(),
                                           packedRunIndexData.size()),
  };
  slot.appendDrawRun(std::move(directPackedState), sharedUniformPayload,
                     std::span<const DrawParam>(&drawParamB, 1),
                     std::span<const DrawParamPayloadView>(&packedPayload, 1));
  const auto runView = slot.commandAt(1);
  check(runView.drawRunRecord != nullptr, "slot draw-run command exposes compact run record");
  check(runView.drawState.hot != nullptr, "slot draw-run indexes hot draw state");
  check(runView.drawState.debug != nullptr, "slot draw-run indexes debug state");
  checkEq(runView.drawState.key(), packedRunKey,
          "slot draw-run preserves the incoming canonical hot state");
  checkEq(runView.drawState.debugSnapshot().renderStateHash, 99ull,
          "slot draw-run preserves the incoming debug snapshot without recomputing base state");
  checkEq(runView.drawParams.size(), std::size_t{1},
          "slot draw-run indexes shared draw param table");
  checkEq(runView.drawParams[0].userVertexRange.offset, 0u,
          "slot draw-run vertex payload range stays local to the record payload");
  checkEq(runView.drawParams[0].userIndexRange.offset, 4u,
          "slot draw-run index payload range stays local to the record payload");
  checkEq(runView.drawRunRecord->payloadOffset, 6u,
          "slot draw-run record points at its payload arena slice");
  checkEq(runView.drawRunRecord->payloadSize, 6u,
          "slot draw-run record stores its payload byte count");
  checkEq(runView.drawPayloadBytes.size(), std::size_t{6},
          "slot draw-run view exposes only its record payload span");
  check(runView.drawPayloadBytes.data() ==
            slot.drawPayloadArena.data() + runView.drawRunRecord->payloadOffset,
        "slot draw-run payload span starts at the record offset");
  checkEq(slot.drawPayloadArena.size(), std::size_t{12},
          "slot payload arena owns draw and draw-run bytes together");
  checkEq(slot.drawUniformPayloads.size(), std::size_t{1},
          "slot draw-run interns an unchanged uniform payload");
  checkEq(runView.drawRunRecord->uniformHandle, sharedUniformHandle,
          "slot draw-run reuses the existing uniform payload handle");
  checkEq(slot.lastUniformHandle, sharedUniformHandle,
          "slot uniform fast path keeps the recent matching handle");

  DrawDesc changedUniformDesc = withUserPayload;
  changedUniformDesc.clipPlaneMask ^= 0x1u;
  const auto changedUniformPayload = makeDrawUniformPayload(changedUniformDesc);
  slot.appendDrawRun(makeCanonicalDrawStateForTest(first), changedUniformPayload,
                     std::span<const DrawParam>(&drawParamA, 1),
                     std::span<const DrawParamPayloadView>{});
  const auto noPayloadView = slot.commandAt(2);
  check(noPayloadView.drawRunRecord != nullptr,
        "slot no-payload draw-run command exposes compact run record");
  checkEq(noPayloadView.drawParams.size(), std::size_t{1},
          "slot no-payload draw-run indexes its draw param");
  checkEq(noPayloadView.drawRunRecord->payloadOffset, 12u,
          "slot no-payload record keeps the current payload arena offset");
  checkEq(noPayloadView.drawRunRecord->payloadSize, 0u,
          "slot no-payload record stores an empty payload size");
  check(noPayloadView.drawPayloadBytes.empty(),
        "slot no-payload draw-run view exposes an empty payload span");
  check(noPayloadView.drawParams[0].userVertexRange.empty(),
        "slot no-payload draw-run leaves vertex payload range empty");
  check(noPayloadView.drawParams[0].userIndexRange.empty(),
        "slot no-payload draw-run leaves index payload range empty");
  checkEq(slot.drawPayloadArena.size(), std::size_t{12},
          "slot no-payload draw-run does not grow the payload arena");
  checkEq(slot.drawUniformPayloads.size(), std::size_t{2},
          "slot stores a new uniform payload when the payload changes");
  check(!(noPayloadView.drawRunRecord->uniformHandle == sharedUniformHandle),
        "slot no-payload draw-run uses a distinct uniform payload handle");
  const auto changedUniformHandle = noPayloadView.drawRunRecord->uniformHandle;
  checkEq(slot.lastUniformHandle, changedUniformHandle,
          "slot remembers recently appended changed uniform payload handle");

  slot.appendDrawRun(makeCanonicalDrawStateForTest(first), sharedUniformPayload,
                     std::span<const DrawParam>(&drawParamA, 1),
                     std::span<const DrawParamPayloadView>{});
  const auto reusedUniformView = slot.commandAt(3);
  check(reusedUniformView.drawRunRecord != nullptr,
        "slot reused-uniform draw-run command exposes compact run record");
  checkEq(slot.drawUniformPayloads.size(), std::size_t{2},
          "slot indexed uniform hit reuses an older payload handle");
  checkEq(reusedUniformView.drawRunRecord->uniformHandle, sharedUniformHandle,
          "slot reused-uniform draw-run uses the older interned handle");
  checkEq(slot.lastUniformHandle, sharedUniformHandle,
          "slot records the uniform handle found by indexed lookup as most recent");

  slot.appendDrawRun(makeCanonicalDrawStateForTest(first), sharedUniformPayload,
                     std::span<const DrawParam>(&drawParamA, 1),
                     std::span<const DrawParamPayloadView>{});
  const auto fastUniformView = slot.commandAt(4);
  check(fastUniformView.drawRunRecord != nullptr,
        "slot fast-uniform draw-run command exposes compact run record");
  checkEq(slot.drawUniformPayloads.size(), std::size_t{2},
          "slot recent uniform hit does not append another payload");
  checkEq(fastUniformView.drawRunRecord->uniformHandle, sharedUniformHandle,
          "slot recent uniform hit reuses the most recent handle");
  checkEq(slot.lastUniformHandle, sharedUniformHandle,
          "slot preserves the most recent uniform handle after fast hit");

  const auto refreshedDrawView = slot.commandAt(0);
  const auto refreshedRunView = slot.commandAt(1);
  check(refreshedDrawView.drawUniformPayload == refreshedRunView.drawUniformPayload,
        "slot command views resolve interned uniform payloads to the same arena record");

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
  changedRun.state = makeCanonicalDrawStateForTest(makeDrawDescFromState(changedRenderState, {}));
  check(!(firstKey == changedRun.state.hot.key),
        "render-state changes flat base draw state");
  check(!(run.state.hot.key == changedRun.state.hot.key),
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
  testChunkSlotU32GuardBoundaries();
  testChunkSlotSimpleCommandSoAViews();
  testChunkSlotDirectDrawRunUniformLookup();
  testStateValueTableDirtyHashContract();
  testStateDrawTransform();
  testConstantsAndShaderRefs();
  testResourceBindingsAndAttachments();
  testVertexDeclFvfAndStreamBindings();
  testFlatDrawStateKey();
  return 0;
}
