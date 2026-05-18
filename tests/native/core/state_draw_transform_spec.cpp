#include "dxmt9/core.hpp"
#include "core_spec_fixtures.hpp"
#include "../../../src/dxmt9/dxmt9_backend_types.hpp"
#include "../../../src/dxmt9/dxmt9_draw_state.hpp"

#include <array>
#include <cmath>
#include <cstring>
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

void checkMatrixNear(const Matrix4x4& actual, const Matrix4x4& expected,
                     std::string_view message) {
  for (size_t i = 0; i < actual.m.size(); ++i) {
    if (std::fabs(actual.m[i] - expected.m[i]) > 0.0001f) {
      std::cerr << "FAIL: " << message << " index=" << i
                << " actual=" << actual.m[i]
                << " expected=" << expected.m[i] << '\n';
      std::exit(1);
    }
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

std::vector<u8> wordsToBytes(std::initializer_list<u32> words) {
  std::vector<u8> bytes(words.size() * sizeof(u32));
  std::size_t offset = 0;
  for (const auto word : words) {
    std::memcpy(bytes.data() + offset, &word, sizeof(word));
    offset += sizeof(word);
  }
  return bytes;
}

u32 makeInstructionToken(u32 opcode, u32 operandCount) {
  return opcode | (operandCount << 24);
}

u32 makeRegisterToken(u32 type, u32 index, bool relative = false) {
  return index | ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11) |
         (relative ? (1u << 13) : 0u);
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

template <std::size_t MaxEntries>
void checkFlatStateValue(const FlatStateSet<MaxEntries>& states, u32 key,
                         u32 expected, std::string_view message) {
  checkEq(flatStateOr(states, key, 0xffffffffu), expected, message);
}

template <typename StateTable>
bool stateTableDirty(const StateTable& table, u32 key) {
  return (table.dirty[StateTable::word(key)] & StateTable::bit(key)) != 0;
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

void testTransformMultiplicationOrderAndBlendSlots() {
  DeviceState state;
  state.reset();

  Matrix4x4 world{};
  world.m = {
      2.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 3.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 4.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
  Matrix4x4 blendWorld1{};
  blendWorld1.m = {
      11.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 13.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 17.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
  Matrix4x4 view{};
  view.m = {
      1.0f, 5.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 6.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 7.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
  Matrix4x4 projection{};
  projection.m = {
      10.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 20.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 30.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 40.0f,
  };

  state.transforms[XFORM_WORLD_BASE] = world;
  state.transforms[XFORM_WORLD_BASE + 1u] = blendWorld1;
  state.transforms[XFORM_VIEW] = view;
  state.transforms[XFORM_PROJECTION] = projection;

  const DrawDesc desc = makeDrawDescFromState(state, {});
  Matrix4x4 expectedWorldViewProj{};
  expectedWorldViewProj.m = {
      20.0f, 200.0f, 0.0f, 0.0f,
      0.0f, 60.0f, 540.0f, 0.0f,
      0.0f, 0.0f, 120.0f, 1120.0f,
      0.0f, 0.0f, 0.0f, 40.0f,
  };
  Matrix4x4 expectedViewProj{};
  expectedViewProj.m = {
      10.0f, 100.0f, 0.0f, 0.0f,
      0.0f, 20.0f, 180.0f, 0.0f,
      0.0f, 0.0f, 30.0f, 280.0f,
      0.0f, 0.0f, 0.0f, 40.0f,
  };
  Matrix4x4 expectedBlendWorld1ViewProj{};
  expectedBlendWorld1ViewProj.m = {
      110.0f, 1100.0f, 0.0f, 0.0f,
      0.0f, 260.0f, 2340.0f, 0.0f,
      0.0f, 0.0f, 510.0f, 4760.0f,
      0.0f, 0.0f, 0.0f, 40.0f,
  };

  checkMatrixNear(desc.worldViewProj, expectedWorldViewProj,
                  "world-view-projection multiply order");
  checkMatrixNear(desc.ffpBlendWorldViewProj[0], expectedWorldViewProj,
                  "blend slot 0 uses WORLD0 * VIEW * PROJECTION");
  checkMatrixNear(desc.ffpBlendWorldViewProj[1], expectedBlendWorld1ViewProj,
                  "blend slot 1 uses WORLD1 * VIEW * PROJECTION");
  checkMatrixNear(desc.ffpBlendWorldViewProj[2], expectedViewProj,
                  "unset blend slot uses identity WORLD2");
}

void testClipPlaneLimitsAtCoreBoundary() {
  auto backend = std::make_shared<dxmt9::core::spec::RecordingBackend>();
  Factory factory(BackendLimits{}, backend);
  PresentParameters params{};
  params.backBufferWidth = 32;
  params.backBufferHeight = 32;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{0x5150};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "clip-plane limit device creation");

  for (u32 i = 0; i < kMaxClipPlanes; ++i) {
    const ClipPlane plane{
        1.0f + static_cast<float>(i),
        2.0f + static_cast<float>(i),
        3.0f + static_cast<float>(i),
        4.0f + static_cast<float>(i),
    };
    checkEq(device->setClipPlane(i, plane), D3D_OK,
            "clip-plane slot within max succeeds");
  }

  const ClipPlane rejectedPlane{9.0f, 8.0f, 7.0f, 6.0f};
  checkEq(device->setClipPlane(kMaxClipPlanes, rejectedPlane),
          D3DERR_INVALIDCALL, "clip-plane slot past max is rejected");

  checkEq(device->setRenderState(RS_CLIP_PLANE_ENABLE, 0xffffffffu), D3D_OK,
          "clip-plane enable mask with high bits is accepted at state boundary");
  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 1), D3D_OK,
          "clip-plane limit draw records");
  checkEq(backend->draws.size(), std::size_t{1},
          "clip-plane limit records one draw");

  const auto& draw = backend->draws.back();
  checkEq(draw.uniforms.clipPlaneMask, 0xffffffffu,
          "clip-plane mask value is preserved for downstream consumers");
  for (u32 i = 0; i < kMaxClipPlanes; ++i) {
    for (size_t component = 0; component < 4; ++component) {
      const float expected = static_cast<float>(component + 1u + i);
      checkNear(draw.uniforms.clipPlanes[i][component], expected,
                "clip-plane value propagates for each supported slot");
    }
  }
}

void testClipPlaneTransformPayloadAndMaskBounds() {
  DeviceState state;
  state.reset();

  Matrix4x4 world{};
  world.m = {
      2.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 3.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 4.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
  Matrix4x4 view{};
  view.m = {
      5.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 7.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 11.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
  Matrix4x4 projection{};
  projection.m = {
      13.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 17.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 19.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
  state.transforms[XFORM_WORLD_BASE] = world;
  state.transforms[XFORM_VIEW] = view;
  state.transforms[XFORM_PROJECTION] = projection;

  state.clipPlanes[0] = {130.0f, 714.0f, 2508.0f, -4.0f};
  state.clipPlanes[1] = {999.0f, 999.0f, 999.0f, 999.0f};
  state.clipPlanes[2] = {-260.0f, 357.0f, 0.0f, 9.0f};
  state.clipPlanes[5] = {0.0f, 0.0f, -836.0f, 8.0f};
  constexpr u32 kClipMask =
      (1u << 0u) | (1u << 2u) | (1u << 5u) | (1u << 9u);
  state.renderStates[RS_CLIP_PLANE_ENABLE] = kClipMask;

  const DrawDesc desc = makeDrawDescFromState(state, {});
  const auto uniforms = makeDrawUniformPayload(desc);
  const auto canonical = makeCanonicalDrawStateFromState(state, {});

  checkEq(desc.clipPlaneMask, kClipMask,
          "draw desc preserves clip-plane mask including unsupported high bits");
  checkEq(uniforms.clipPlaneMask, desc.clipPlaneMask,
          "draw uniform payload preserves clip-plane mask");
  checkEq(canonical.shaderLayout.clipPlaneMask, desc.clipPlaneMask,
          "shader layout preserves clip-plane mask");
  checkEq(canonical.hot.clipPlaneMask, desc.clipPlaneMask,
          "canonical hot state preserves clip-plane mask");

  const std::array<ClipPlane, kMaxClipPlanes> expected{{
      {1.0f, 2.0f, 3.0f, -4.0f},
      {0.0f, 0.0f, 0.0f, 0.0f},
      {-2.0f, 1.0f, 0.0f, 9.0f},
      {0.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, -1.0f, 8.0f},
  }};

  for (size_t plane = 0; plane < expected.size(); ++plane) {
    for (size_t component = 0; component < 4; ++component) {
      checkNear(desc.clipPlanes[plane][component], expected[plane][component],
                "draw desc stores transformed enabled clip planes only");
      checkNear(uniforms.clipPlanes[plane][component],
                expected[plane][component],
                "uniform payload stores transformed enabled clip planes only");
    }
  }

  const auto changed = [&] {
    DeviceState next = state;
    next.clipPlanes[2][1] += 357.0f;
    return makeCanonicalDrawStateFromState(next, {});
  }();
  check(changed.hot.key.clipPlanesHash != canonical.hot.key.clipPlanesHash,
        "transformed clip-plane payload participates in canonical key hash");
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

  const auto canonical = makeCanonicalDrawStateFromState(state, {});
  const auto uniforms = makeDrawUniformPayload(desc);
  checkEq(uniforms.vsConst, state.vsConst,
          "draw uniform payload preserves vertex shader constants");
  checkEq(uniforms.psConst, state.psConst,
          "draw uniform payload preserves pixel shader constants");
  checkEq(canonical.hot.vertexConstantsHash, canonical.hot.key.vertexConstantsHash,
          "hot vertex constant hash matches canonical key");
  checkEq(canonical.hot.pixelConstantsHash, canonical.hot.key.pixelConstantsHash,
          "hot pixel constant hash matches canonical key");
  check(uniforms.hash != 0,
        "draw uniform payload hash records shader constant payload");

  DeviceState changed = state;
  changed.vsConst.float4[7][0] = -99.0f;
  const DrawDesc changedDesc = makeDrawDescFromState(changed, {});
  const auto changedCanonical = makeCanonicalDrawStateFromState(changed, {});
  const auto changedUniforms = makeDrawUniformPayload(changedDesc);
  check(changedCanonical.hot.vertexConstantsHash != canonical.hot.vertexConstantsHash,
        "vertex constant value change affects canonical hot hash");
  checkNear(changedUniforms.vsConst.float4[7][0], -99.0f,
            "changed vertex constant value reaches uniform payload");
}

void testShaderConstantPayloadSurvivesDrawRunCommandView() {
  DeviceState state;
  state.reset();

  state.vertexShader = makeBytecodeShader(0x1234000000005678ull, {0xaa, 0xbb, 0xcc});
  state.pixelShader = makeBytecodeShader(0x8765000000004321ull, {0x10, 0x20, 0x30, 0x40});
  state.vsConst.float4[0] = {1.0f, -2.0f, 3.5f, -4.5f};
  state.vsConst.float4[kMaxVertexConstants - 1] = {255.0f, 128.0f, 64.0f, 32.0f};
  state.vsConst.int4[2] = {7, -8, 9, -10};
  state.vsConst.bools[0] = true;
  state.vsConst.bools[kMaxBoolConstants - 1] = true;
  state.psConst.float4[3] = {-11.0f, 12.25f, -13.5f, 14.75f};
  state.psConst.float4[kMaxPixelConstants - 1] = {0.125f, 0.25f, 0.5f, 1.0f};
  state.psConst.int4[1] = {-101, 202, -303, 404};
  state.psConst.bools[4] = true;
  state.psConst.bools[kMaxBoolConstants - 2] = true;

  const DrawDesc desc = makeDrawDescFromState(
      state, {PrimitiveType::TriangleList, 2u, 4u, -3, 6u, IndexType::UInt16});
  const FlatDrawStateKey expectedKey = makeFlatDrawStateKey(desc);
  auto canonical = makeCanonicalDrawStateForTest(desc);
  auto uniforms = makeDrawUniformPayload(desc);

  DrawParam param = makeDrawParamForTest(desc);
  ChunkSlot slot{};
  slot.appendDrawRun(canonical, uniforms,
                     std::span<const DrawParam>(&param, 1u),
                     std::span<const DrawParamPayloadView>{});

  canonical.hot.vertexConstantsHash = 0u;
  canonical.shaderLayout.vertexShader.hash = 0u;
  uniforms.vsConst.float4[0] = {};
  uniforms.psConst.float4[3] = {};

  const auto command = slot.commandAt(0);
  checkEq(command.kind, MetalCommandKind::DrawRun,
          "shader constant command view reports draw-run kind");
  check(command.drawRunRecord != nullptr,
        "shader constant command view resolves draw-run record");
  check(command.drawState.hasShaderContext(),
        "shader constant command view carries shader layout context");
  check(command.drawState.hasUniformPayload(),
        "shader constant command view carries copied uniform payload");
  check(command.drawUniformPayload != nullptr,
        "shader constant command view resolves interned uniform payload");
  checkEq(command.drawParams.size(), std::size_t{1},
          "shader constant command view keeps draw param count");

  checkEq(command.drawState.shaderContext().vertexShader, state.vertexShader,
          "draw-run command view preserves vertex shader ref");
  checkEq(command.drawState.shaderContext().pixelShader, state.pixelShader,
          "draw-run command view preserves pixel shader ref");
  checkEq(command.drawState.hot->key.vertexShaderHash, expectedKey.vertexShaderHash,
          "draw-run command key preserves vertex shader hash");
  checkEq(command.drawState.hot->key.pixelShaderHash, expectedKey.pixelShaderHash,
          "draw-run command key preserves pixel shader hash");
  checkEq(command.drawState.hot->vertexConstantsHash,
          command.drawState.hot->key.vertexConstantsHash,
          "draw-run command hot state keeps vertex constant hash");
  checkEq(command.drawState.hot->pixelConstantsHash,
          command.drawState.hot->key.pixelConstantsHash,
          "draw-run command hot state keeps pixel constant hash");
  checkEq(command.drawRunRecord->uniformHandle.hash,
          command.drawUniformPayload->hash,
          "draw-run command uniform handle hashes the copied payload");

  const auto vsConsts = dxmt9::state::buildVsConsts(command.drawState);
  const auto psConsts = dxmt9::state::buildPsConsts(command.drawState);
  checkNear(vsConsts.vsFloatConst[0][0], 1.0f,
            "encoder VS constants read copied c0.x");
  checkNear(vsConsts.vsFloatConst[0][1], -2.0f,
            "encoder VS constants read copied c0.y");
  checkNear(vsConsts.vsFloatConst[kMaxVertexConstants - 1][0], 255.0f,
            "encoder VS constants read copied max float register");
  checkEq(vsConsts.vsIntConst[2][1], -8,
          "encoder VS constants read copied int register");
  checkEq(vsConsts.vsBoolConst[0], 1u,
          "encoder VS constants read copied first bool register");
  checkEq(vsConsts.vsBoolConst[kMaxBoolConstants - 1], 1u,
          "encoder VS constants read copied last bool register");

  checkNear(psConsts.psFloatConst[3][2], -13.5f,
            "encoder PS constants read copied c3.z");
  checkNear(psConsts.psFloatConst[kMaxPixelConstants - 1][3], 1.0f,
            "encoder PS constants read copied max float register");
  checkEq(psConsts.psIntConst[1][2], -303,
          "encoder PS constants read copied int register");
  checkEq(psConsts.psBoolConst[4], 1u,
          "encoder PS constants read copied bool register");
  checkEq(psConsts.psBoolConst[kMaxBoolConstants - 2], 1u,
          "encoder PS constants read copied high bool register");
}

void testShaderLayoutCarriesConstantUsageMetadata() {
  DrawDesc desc{};
  desc.vertexShader = makeBytecodeShader(
      0x5151u,
      wordsToBytes({
          0xfffe0300u,
          makeInstructionToken(81u, 5u),
          makeRegisterToken(2u, 0u),
          0u, 0u, 0u, 0u,
          makeInstructionToken(20u, 3u),
          makeRegisterToken(4u, 0u),
          makeRegisterToken(1u, 0u),
          makeRegisterToken(2u, 4u),
          makeInstructionToken(1u, 2u),
          makeRegisterToken(0u, 0u),
          makeRegisterToken(2u, 2u, true),
          makeRegisterToken(3u, 0u),
          0x0000ffffu,
      }));
  desc.pixelShader = makeBytecodeShader(
      0x6262u,
      wordsToBytes({
          0xffff0300u,
          makeInstructionToken(48u, 5u),
          makeRegisterToken(7u, 2u),
          1u, 2u, 3u, 4u,
          makeInstructionToken(47u, 2u),
          makeRegisterToken(14u, 4u),
          1u,
          0x0000ffffu,
      }));

  const auto layout = makeDrawShaderLayoutContext(desc);
  check(!layout.vertexConstantUsage.unknown,
        "VS bytecode scan publishes known constant usage metadata");
  checkEq(layout.vertexConstantUsage.floatCount, std::uint16_t{8},
          "VS matrix operand expands float usage through the matrix row span");
  check(layout.vertexConstantUsage.indexedFloat,
        "VS relative constant source marks indexed float usage");
  check(!layout.pixelConstantUsage.unknown,
        "PS bytecode scan publishes known constant usage metadata");
  checkEq(layout.pixelConstantUsage.intCount, std::uint16_t{3},
          "PS DEFI destination contributes integer constant metadata");
  checkEq(layout.pixelConstantUsage.boolCount, std::uint16_t{5},
          "PS DEFB destination contributes boolean constant metadata");

  DrawDesc fixedFunction{};
  const auto fixedLayout = makeDrawShaderLayoutContext(fixedFunction);
  check(fixedLayout.vertexConstantUsage.unknown,
        "fixed-function VS keeps conservative unknown constant usage");
  check(fixedLayout.pixelConstantUsage.unknown,
        "fixed-function PS keeps conservative unknown constant usage");
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
  const auto vertexTexture0 = makeTexture(0x5810, Format::R32F);
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
  state.textures[kVertexTextureSampler0] = vertexTexture0;
  state.textures[kMaxTextures - 1] = texture15;
  state.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg2);
  state.textureStageStates[kMaxTextureStages - 1][TSS_TEXTURE_TYPE] = 3;
  state.samplerStates[kVertexTextureSampler0][SAMP_MIN_FILTER] = 2;
  state.samplerStates[kVertexTextureSampler0][SAMP_ADDRESS_U] = 3;

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
  checkEq(desc.textures[kVertexTextureSampler0].handle, vertexTexture0->handle(),
          "first vertex texture handle copied");
  checkEq(desc.textures[kMaxTextures - 1].handle, texture15->handle(), "extra sampler texture handle copied");
  checkEq(desc.textures[0].stageStates.at(TSS_COLOR_OP), static_cast<u32>(TextureOp::SelectArg2),
          "texture stage 0 states copied");
  checkEq(desc.textures[kMaxTextureStages - 1].stageStates.at(TSS_TEXTURE_TYPE), 3u,
          "last texture stage states copied");
  check(desc.textures[kVertexTextureSampler0].stageStates.empty(),
        "vertex texture sampler has no fixed-function texture-stage state");
  check(desc.textures[kMaxTextures - 1].stageStates.empty(), "non-stage texture states cleared");
  checkEq(desc.samplers[kVertexTextureSampler0].states.at(SAMP_MIN_FILTER), 2u,
          "first vertex sampler min filter copied");
  checkEq(desc.samplers[kVertexTextureSampler0].states.at(SAMP_ADDRESS_U), 3u,
          "first vertex sampler address mode copied");

  checkEq(desc.rts.color[0], state.renderTargets[0], "render target 0 attachment copied");
  checkEq(desc.rts.color[1], state.renderTargets[1], "texture-level render target attachment copied");
  checkEq(desc.rts.color[2], RenderTargetAttachment{}, "unbound render target attachment remains empty");
  checkEq(desc.rts.color[3], state.renderTargets[3], "multisampled render target attachment copied");
  checkEq(desc.rts.depthStencil, state.depthStencil, "depth-stencil attachment copied");

  const auto canonical = makeCanonicalDrawStateFromState(state, args);
  checkEq(canonical.hot.textures[kVertexTextureSampler0], vertexTexture0->handle(),
          "canonical hot state preserves first vertex texture handle");
  check((canonical.hot.textureMask & (1u << kVertexTextureSampler0)) != 0,
        "canonical hot state marks first vertex texture slot");
  check((canonical.hot.key.textureMask & (1u << kVertexTextureSampler0)) != 0,
        "canonical key marks first vertex texture slot");
  checkEq(flatStateOr(canonical.hot.samplerStates[kVertexTextureSampler0], SAMP_ADDRESS_U, 0u),
          3u, "canonical hot state stores first vertex sampler state");
  check((canonical.hot.key.samplerStateMask & (1u << kVertexTextureSampler0)) != 0,
        "canonical key marks first vertex sampler state");
  checkEq(canonical.hot.samplerStates[kVertexTextureSampler0].hash,
          canonical.hot.key.samplerStateHashes[kVertexTextureSampler0],
          "first vertex sampler hash is part of the canonical key");
  checkEq(canonical.hot.indexBuffer, indexBuffer->handle(),
          "canonical hot state preserves index buffer handle");
  checkEq(canonical.hot.colorAttachments[0], state.renderTargets[0],
          "canonical hot state preserves RT0 attachment");
  checkEq(canonical.hot.colorAttachments[1], state.renderTargets[1],
          "canonical hot state preserves mip-level RT attachment");
  checkEq(canonical.hot.colorAttachments[3], state.renderTargets[3],
          "canonical hot state preserves multisample RT attachment");
  checkEq(canonical.hot.depthStencil, state.depthStencil,
          "canonical hot state preserves depth-stencil attachment");
  checkEq(canonical.hot.renderTargetMask, (1u << 0) | (1u << 1) | (1u << 3),
          "canonical hot state records sparse render-target mask");
  checkEq(canonical.hot.key.colorAttachments[1].level, 2u,
          "canonical key keeps render-target mip level");
  checkEq(canonical.hot.key.colorAttachments[3].sampleCount, 4u,
          "canonical key keeps render-target sample count");
  checkEq(canonical.hot.key.depthStencil.sampleCount, 2u,
          "canonical key keeps depth-stencil sample count");
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

  const auto canonical = makeCanonicalDrawStateFromState(state, {});
  checkEq(canonical.shaderLayout.vertexDecl.elements, state.vertexDecl.elements,
          "canonical shader layout preserves sparse vertex declaration elements");
  checkEq(canonical.hot.key.vertexElementCount, 3u,
          "canonical key records vertex declaration element count");
  checkEq(canonical.hot.key.fvf, 0x11223344u,
          "canonical key records FVF value");
  check(canonical.hot.key.vertexDeclHash != 0,
        "canonical key records vertex declaration hash");
  checkEq(canonical.hot.streamMask, (1u << 2) | (1u << 4),
          "canonical hot state records sparse stream mask");
  checkEq(canonical.hot.streamBuffers[2], stream2->handle(),
          "canonical hot state preserves stream 2 handle");
  checkEq(canonical.hot.streamOffsets[4], 40u,
          "canonical hot state preserves stream 4 offset");
  checkEq(canonical.hot.streamStrides[4], 56u,
          "canonical hot state preserves stream 4 stride");
}

void testTextureStageArgumentCanonicalValues() {
  DeviceState state;
  state.reset();

  constexpr u32 kD3DTA_CURRENT = 1u;
  constexpr u32 kD3DTA_TEXTURE = 2u;
  constexpr u32 kD3DTA_TEMP = 5u;
  constexpr u32 kD3DTA_CONSTANT = 6u;
  constexpr u32 kD3DTA_COMPLEMENT = 0x10u;
  constexpr u32 kD3DTA_ALPHAREPLICATE = 0x20u;
  constexpr u32 kD3DTSS_TCI_CAMERASPACENORMAL = 0x00010000u;

  const u32 colorArg1 = kD3DTA_TEXTURE | kD3DTA_COMPLEMENT;
  const u32 colorArg2 = kD3DTA_CURRENT | kD3DTA_ALPHAREPLICATE;
  const u32 alphaArg1 = kD3DTA_CONSTANT | kD3DTA_ALPHAREPLICATE;
  const u32 alphaArg2 = kD3DTA_TEMP | kD3DTA_COMPLEMENT | kD3DTA_ALPHAREPLICATE;
  const u32 texcoordIndex = kD3DTSS_TCI_CAMERASPACENORMAL | 2u;

  state.textureStageStates[1][TSS_COLOR_OP] = static_cast<u32>(TextureOp::Modulate4x);
  state.textureStageStates[1][TSS_COLOR_ARG1] = colorArg1;
  state.textureStageStates[1][TSS_COLOR_ARG2] = colorArg2;
  state.textureStageStates[1][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::SelectArg2);
  state.textureStageStates[1][TSS_ALPHA_ARG1] = alphaArg1;
  state.textureStageStates[1][TSS_ALPHA_ARG2] = alphaArg2;
  state.textureStageStates[1][TSS_RESULT_ARG] = kD3DTA_TEMP;
  state.textureStageStates[1][TSS_TEXCOORD_INDEX] = texcoordIndex;
  state.textureStageStates[1][TSS_CONSTANT] = 0x80402010u;
  state.textureStageStates[1][TSS_TEXTURE_TYPE] = static_cast<u32>(TextureType::Cube);

  const DrawDesc desc = makeDrawDescFromState(state, {});
  const auto canonical = makeCanonicalDrawStateFromState(state, {});
  const auto& stage = canonical.shaderLayout.pixelShader.pixelKey->stages[1];

  checkEq(desc.textures[1].stageStates.at(TSS_COLOR_ARG1), colorArg1,
          "draw desc preserves D3DTA color arg1 modifier bits");
  checkEq(flatStateOr(canonical.hot.textureStageStates[1], TSS_COLOR_ARG1, 0u),
          colorArg1, "canonical hot state preserves D3DTA color arg1");
  checkEq(flatStateOr(canonical.hot.textureStageStates[1], TSS_COLOR_ARG2, 0u),
          colorArg2, "canonical hot state preserves D3DTA color arg2");
  checkEq(flatStateOr(canonical.hot.textureStageStates[1], TSS_ALPHA_ARG2, 0u),
          alphaArg2, "canonical hot state preserves D3DTA alpha arg2");
  checkEq(flatStateOr(canonical.hot.textureStageStates[1], TSS_RESULT_ARG, 0u),
          kD3DTA_TEMP, "canonical hot state preserves TSS result arg");
  checkEq(flatStateOr(canonical.hot.textureStageStates[1], TSS_TEXCOORD_INDEX, 0u),
          texcoordIndex, "canonical hot state preserves TCI texcoord bits");
  checkEq(flatStateOr(canonical.hot.textureStageStates[1], TSS_CONSTANT, 0u),
          0x80402010u, "canonical hot state preserves D3DTSS_CONSTANT");
  checkEq(flatStateOr(canonical.hot.textureStageStates[1], TSS_TEXTURE_TYPE, 0u),
          static_cast<u32>(TextureType::Cube),
          "canonical hot state keeps internal texture type separate from D3DTSS_CONSTANT");
  checkEq(stage.colorOp, static_cast<u32>(TextureOp::Modulate4x),
          "FFP pixel key preserves stage color op");
  checkEq(stage.colorArg1, colorArg1,
          "FFP pixel key preserves D3DTA color arg1");
  checkEq(stage.colorArg2, colorArg2,
          "FFP pixel key preserves D3DTA color arg2");
  checkEq(stage.alphaArg1, alphaArg1,
          "FFP pixel key preserves D3DTA alpha arg1");
  checkEq(stage.alphaArg2, alphaArg2,
          "FFP pixel key preserves D3DTA alpha arg2");
  checkEq(stage.resultArg, kD3DTA_TEMP,
          "FFP pixel key preserves result arg");
  checkEq(stage.texType, static_cast<u32>(TextureType::Cube),
          "FFP pixel key preserves texture type");
  checkEq(stage.texCoordIndex, texcoordIndex,
          "FFP pixel key preserves full texcoord index value");
}

void testRenderStateIntentPayloadAcrossDrawRunBoundary() {
  DeviceState state;
  state.reset();

  state.renderStates[RS_ALPHABLEND_ENABLE] = 1u;
  state.renderStates[RS_SRC_BLEND] = static_cast<u32>(BlendFactor::SrcAlpha);
  state.renderStates[RS_DEST_BLEND] = static_cast<u32>(BlendFactor::InvSrcAlpha);
  state.renderStates[RS_BLEND_OP] = static_cast<u32>(BlendOp::RevSubtract);
  state.renderStates[RS_SEPARATE_ALPHA_BLEND_ENABLE] = 1u;
  state.renderStates[RS_SRC_BLEND_ALPHA] = static_cast<u32>(BlendFactor::One);
  state.renderStates[RS_DEST_BLEND_ALPHA] = static_cast<u32>(BlendFactor::InvDestAlpha);
  state.renderStates[RS_BLEND_OP_ALPHA] = static_cast<u32>(BlendOp::Max);
  state.renderStates[RS_BLEND_FACTOR] = 0x80402010u;
  state.renderStates[RS_COLOR_WRITE_ENABLE] = 0x5u;
  state.renderStates[RS_SRGB_WRITE_ENABLE] = 1u;
  state.renderStates[RS_Z_ENABLE] = 0u;
  state.renderStates[RS_Z_WRITE_ENABLE] = 0u;
  state.renderStates[RS_Z_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);
  state.renderStates[RS_STENCIL_ENABLE] = 1u;
  state.renderStates[RS_STENCIL_FUNC] = static_cast<u32>(CompareFunc::Less);
  state.renderStates[RS_STENCIL_FAIL] = static_cast<u32>(StencilOp::Replace);
  state.renderStates[RS_STENCIL_ZFAIL] = static_cast<u32>(StencilOp::Incr);
  state.renderStates[RS_STENCIL_PASS] = static_cast<u32>(StencilOp::Decr);
  state.renderStates[RS_STENCIL_REF] = 0x33u;
  state.renderStates[RS_STENCIL_MASK] = 0x0fu;
  state.renderStates[RS_STENCIL_WRITEMASK] = 0xf0u;
  state.renderStates[RS_STENCIL_CCW_FUNC] = static_cast<u32>(CompareFunc::Greater);
  state.renderStates[RS_STENCIL_CCW_FAIL] = static_cast<u32>(StencilOp::Zero);
  state.renderStates[RS_STENCIL_CCW_ZFAIL] = static_cast<u32>(StencilOp::Invert);
  state.renderStates[RS_STENCIL_CCW_PASS] = static_cast<u32>(StencilOp::Keep);
  state.renderStates[RS_ALPHA_TEST_ENABLE] = 1u;
  state.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::NotEqual);
  state.renderStates[RS_ALPHA_REF] = 0x7fu;
  state.renderStates[RS_FOG_ENABLE] = 1u;
  state.renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  state.renderStates[RS_FOG_FROM_VERTEX] = static_cast<u32>(FogMode::Linear);
  state.renderStates[RS_RANGE_FOG] = 1u;
  state.renderStates[RS_TEXTURE_FACTOR] = 0x10204080u;
  state.renderStates[RS_FILL_MODE] = 2u;
  state.renderStates[RS_CULL_MODE] = static_cast<u32>(CullMode::None);

  const DrawCallArgs args{
      PrimitiveType::TriangleList, 2u, 4u, -2, 6u, IndexType::UInt32};
  const DrawDesc desc = makeDrawDescFromState(state, args);
  const auto uniforms = makeDrawUniformPayload(desc);
  const auto canonical = makeCanonicalDrawStateFromState(state, args);
  const auto descCanonical = makeCanonicalDrawStateForTest(desc);
  const auto expectedKey = makeFlatDrawStateKey(desc);

  checkEq(descCanonical.hot.key, expectedKey,
          "fixture canonicalization builds the expected render-state key");
  checkEq(canonical.hot.key, expectedKey,
          "direct canonicalization preserves the same render-state key");
  checkEq(canonical.hot.renderStates.hash, expectedKey.renderStateHash,
          "hot render-state payload hash matches flat key");
  checkEq(canonical.debug.renderStateHash, expectedKey.renderStateHash,
          "debug snapshot carries render-state hash");
  checkEq(makeFlatDrawStateRecord(desc).renderStates.hash, expectedKey.renderStateHash,
          "draw desc render-state table flattens to the same hash");

  checkFlatStateValue(canonical.hot.renderStates, RS_ALPHABLEND_ENABLE, 1u,
                      "hot state preserves alpha blend enable");
  checkFlatStateValue(canonical.hot.renderStates, RS_SRC_BLEND,
                      static_cast<u32>(BlendFactor::SrcAlpha),
                      "hot state preserves source blend factor");
  checkFlatStateValue(canonical.hot.renderStates, RS_DEST_BLEND,
                      static_cast<u32>(BlendFactor::InvSrcAlpha),
                      "hot state preserves destination blend factor");
  checkFlatStateValue(canonical.hot.renderStates, RS_BLEND_OP,
                      static_cast<u32>(BlendOp::RevSubtract),
                      "hot state preserves blend operation");
  checkFlatStateValue(canonical.hot.renderStates, RS_SEPARATE_ALPHA_BLEND_ENABLE, 1u,
                      "hot state preserves separate alpha blend enable");
  checkFlatStateValue(canonical.hot.renderStates, RS_BLEND_OP_ALPHA,
                      static_cast<u32>(BlendOp::Max),
                      "hot state preserves alpha blend operation");
  checkFlatStateValue(canonical.hot.renderStates, RS_COLOR_WRITE_ENABLE, 0x5u,
                      "hot state preserves color-write mask");
  checkFlatStateValue(canonical.hot.renderStates, RS_STENCIL_CCW_ZFAIL,
                      static_cast<u32>(StencilOp::Invert),
                      "hot state preserves back-face stencil operation");
  checkFlatStateValue(canonical.hot.renderStates, RS_ALPHA_REF, 0x7fu,
                      "hot state preserves alpha reference value");
  checkFlatStateValue(canonical.hot.renderStates, RS_TEXTURE_FACTOR, 0x10204080u,
                      "hot state preserves texture factor");

  check(canonical.shaderLayout.pixelShader.pixelKey.has_value(),
        "fixed-function pixel key exists for render-state intent");
  checkEq(canonical.shaderLayout.pixelShader.pixelKey->fogMode, FogMode::Exp2,
          "pixel key prefers table fog mode over vertex fog mode");
  check(canonical.shaderLayout.pixelShader.pixelKey->alphaTestEnable,
        "pixel key carries alpha-test enable");
  checkEq(canonical.shaderLayout.pixelShader.pixelKey->alphaTestFunc,
          static_cast<u32>(CompareFunc::NotEqual),
          "pixel key carries alpha-test function");
  check(canonical.shaderLayout.vertexShader.vertexKey.has_value(),
        "fixed-function vertex key exists for render-state intent");
  checkEq(canonical.shaderLayout.vertexShader.vertexKey->fogMode, FogMode::Exp2,
          "vertex key carries effective fog mode");
  check(canonical.shaderLayout.vertexShader.vertexKey->rangeFog,
        "vertex key carries range-fog enable");

  DrawParam param = makeDrawParamForTest(desc);
  DrawRunDesc run{};
  run.state = canonical;
  check(drawRunAppend(run, param), "render-state draw-run param appends");
  check(drawRunValidate(run), "render-state draw-run validates");
  checkEq(run.state.hot.key.renderStateHash, expectedKey.renderStateHash,
          "draw-run state keeps render-state hash");
  checkFlatStateValue(run.state.hot.renderStates, RS_STENCIL_PASS,
                      static_cast<u32>(StencilOp::Decr),
                      "draw-run hot state keeps stencil pass operation");

  ChunkSlot slot{};
  slot.appendDrawRun(canonical, uniforms, std::span<const DrawParam>(&param, 1),
                     std::span<const DrawParamPayloadView>{});
  const auto view = slot.commandAt(0);
  check(view.drawRunRecord != nullptr,
        "slot command exposes render-state draw-run record");
  checkEq(view.drawState.key().renderStateHash, expectedKey.renderStateHash,
          "slot draw-state view keeps render-state key hash");
  checkEq(view.drawState.debugSnapshot().renderStateHash, expectedKey.renderStateHash,
          "slot debug view keeps render-state key hash");
  checkFlatStateValue(view.drawState.hot->renderStates, RS_BLEND_FACTOR, 0x80402010u,
                      "slot hot state keeps blend factor payload");
  check(view.drawUniformPayload != nullptr,
        "slot resolves the draw uniform payload alongside render state");

  DeviceState changed = state;
  changed.renderStates.set(RS_COLOR_WRITE_ENABLE,
                           state.renderStates.at(RS_COLOR_WRITE_ENABLE) ^ 0x3u);
  const auto changedCanonical = makeCanonicalDrawStateFromState(changed, args);
  check(changedCanonical.hot.key.renderStateHash != canonical.hot.key.renderStateHash,
        "single render-state intent change produces a distinct draw key");
  check(changedCanonical.debug.renderStateHash != canonical.debug.renderStateHash,
        "single render-state intent change produces a distinct debug hash");
}

void testSamplerAndTextureStageDirtyHashPayloadBoundaries() {
  DeviceState state;
  state.reset();

  auto& stage2 = state.textureStageStates[2];
  stage2.clearDirty();
  const u64 initialStageHash = stage2.rollingHash;
  stage2.set(TSS_COLOR_OP, stage2.at(TSS_COLOR_OP));
  checkEq(stage2.rollingHash, initialStageHash,
          "redundant texture-stage set keeps rolling hash");
  check(!stateTableDirty(stage2, TSS_COLOR_OP),
        "redundant texture-stage set does not mark dirty");
  stage2.set(TSS_COLOR_OP, static_cast<u32>(TextureOp::AddSigned2x));
  stage2.set(TSS_ALPHA_ARG2, 0x21u);
  stage2.set(TSS_TEXCOORD_INDEX, 0x00010002u);
  check(stateTableDirty(stage2, TSS_COLOR_OP),
        "changed texture-stage op marks dirty");
  check(stateTableDirty(stage2, TSS_ALPHA_ARG2),
        "changed texture-stage arg marks dirty");

  auto& sampler3 = state.samplerStates[3];
  sampler3.clearDirty();
  const u64 initialSamplerHash = sampler3.rollingHash;
  sampler3.set(SAMP_ADDRESS_U, sampler3.at(SAMP_ADDRESS_U));
  checkEq(sampler3.rollingHash, initialSamplerHash,
          "redundant sampler set keeps rolling hash");
  check(!stateTableDirty(sampler3, SAMP_ADDRESS_U),
        "redundant sampler set does not mark dirty");
  sampler3.set(SAMP_ADDRESS_U, 3u);
  sampler3.set(SAMP_MAX_ANISOTROPY, 16u);
  sampler3.set(SAMP_SRGB_TEXTURE, 1u);
  check(stateTableDirty(sampler3, SAMP_ADDRESS_U),
        "changed sampler address mode marks dirty");
  check(stateTableDirty(sampler3, SAMP_MAX_ANISOTROPY),
        "changed sampler anisotropy marks dirty");

  const DrawDesc desc = makeDrawDescFromState(state, {});
  const auto canonical = makeCanonicalDrawStateFromState(state, {});
  const auto expectedKey = makeFlatDrawStateKey(desc);

  checkEq(canonical.hot.textureStageStates[2].hash,
          expectedKey.textureStageStateHashes[2],
          "hot TSS payload hash matches flat key slot");
  checkEq(canonical.hot.samplerStates[3].hash,
          expectedKey.samplerStateHashes[3],
          "hot sampler payload hash matches flat key slot");
  checkFlatStateValue(canonical.hot.textureStageStates[2], TSS_COLOR_OP,
                      static_cast<u32>(TextureOp::AddSigned2x),
                      "hot TSS payload preserves color op");
  checkFlatStateValue(canonical.hot.textureStageStates[2], TSS_TEXCOORD_INDEX,
                      0x00010002u,
                      "hot TSS payload preserves generated texcoord selector");
  checkFlatStateValue(canonical.hot.samplerStates[3], SAMP_MAX_ANISOTROPY, 16u,
                      "hot sampler payload preserves anisotropy");
  checkFlatStateValue(canonical.hot.samplerStates[3], SAMP_SRGB_TEXTURE, 1u,
                      "hot sampler payload preserves sRGB sampling bit");
  check((canonical.hot.key.samplerStateMask & (1u << 3u)) != 0,
        "sampler dirty payload contributes to sampler-state mask");
  checkEq(canonical.debug.samplerStateMask, canonical.hot.key.samplerStateMask,
          "debug snapshot carries sampler-state mask");

  DrawRunDesc run{};
  run.state = canonical;
  DrawParam param{};
  param.primitiveCount = 1u;
  check(drawRunAppend(run, param), "sampler/TSS draw-run param appends");
  check(drawRunValidate(run), "sampler/TSS draw-run validates");
  checkEq(run.state.hot.textureStageStates[2].hash,
          canonical.hot.textureStageStates[2].hash,
          "draw-run keeps TSS hash");
  checkEq(run.state.hot.samplerStates[3].hash,
          canonical.hot.samplerStates[3].hash,
          "draw-run keeps sampler hash");

  DeviceState changedStage = state;
  changedStage.textureStageStates[2].set(TSS_ALPHA_ARG2, 0x20u);
  const auto changedStageKey = makeCanonicalDrawStateFromState(changedStage, {}).hot.key;
  check(changedStageKey.textureStageStateHashes[2] !=
            canonical.hot.key.textureStageStateHashes[2],
        "texture-stage payload change alters only its keyed slot");

  DeviceState changedSampler = state;
  changedSampler.samplerStates[3].set(SAMP_MAX_ANISOTROPY, 8u);
  const auto changedSamplerKey = makeCanonicalDrawStateFromState(changedSampler, {}).hot.key;
  check(changedSamplerKey.samplerStateHashes[3] !=
            canonical.hot.key.samplerStateHashes[3],
        "sampler payload change alters only its keyed slot");
}

void testVertexDeclSnapshotSurvivesLaterStateMutation() {
  DeviceState state;
  state.reset();

  const auto stream0 = makeBuffer(0x1200, 4096, UsageVertexBuffer);
  const auto stream1 = makeBuffer(0x1210, 4096, UsageVertexBuffer);

  state.fvf = 0x44556677u;
  state.vertexDecl.fvf = state.fvf;
  state.vertexDecl.elements = {
      VertexElement{0, 0, 2, 0, 0, 0},
      VertexElement{0, 12, 4, 0, 10, 0},
      VertexElement{1, 0, 3, 0, 5, 0},
  };
  state.streamBuffers[0] = stream0;
  state.streamOffsets[0] = 32u;
  state.streamStrides[0] = 24u;
  state.streamBuffers[1] = stream1;
  state.streamOffsets[1] = 96u;
  state.streamStrides[1] = 16u;

  const DrawDesc desc = makeDrawDescFromState(state, {});
  const auto canonical = makeCanonicalDrawStateForTest(desc);

  state.fvf = 0u;
  state.vertexDecl.fvf = 0u;
  state.vertexDecl.elements.clear();
  state.streamBuffers[0].reset();
  state.streamOffsets[0] = 0u;
  state.streamStrides[0] = 0u;
  state.streamBuffers[1].reset();
  state.streamOffsets[1] = 0u;
  state.streamStrides[1] = 0u;

  checkEq(desc.vertexDecl.fvf, 0x44556677u,
          "draw desc keeps vertex decl FVF snapshot");
  checkEq(desc.vertexDecl.elements.size(), std::size_t{3},
          "draw desc keeps vertex declaration element snapshot");
  checkEq(desc.vertexDecl.streams[0].buffer, stream0,
          "draw desc keeps stream 0 buffer snapshot");
  checkEq(desc.vertexDecl.streams[0].offset, 32u,
          "draw desc keeps stream 0 offset snapshot");
  checkEq(desc.vertexDecl.streams[0].stride, 24u,
          "draw desc keeps stream 0 stride snapshot");
  checkEq(desc.vertexDecl.streams[1].buffer, stream1,
          "draw desc keeps stream 1 buffer snapshot");
  checkEq(desc.vertexDecl.streams[1].offset, 96u,
          "draw desc keeps stream 1 offset snapshot");
  checkEq(desc.vertexDecl.streams[1].stride, 16u,
          "draw desc keeps stream 1 stride snapshot");
  checkEq(canonical.shaderLayout.vertexDecl, desc.vertexDecl,
          "canonical shader layout owns the same immutable vertex decl snapshot");
  checkEq(canonical.hot.streamBuffers[0], stream0->handle(),
          "canonical hot state keeps stream 0 handle snapshot");
  checkEq(canonical.hot.streamOffsets[0], 32u,
          "canonical hot state keeps stream 0 offset snapshot");
  checkEq(canonical.hot.streamStrides[0], 24u,
          "canonical hot state keeps stream 0 stride snapshot");
}

void testIndexedDrawRunPolicyDataContract() {
  DeviceState state;
  state.reset();

  const auto stream0 = makeBuffer(0x9100, 4096, UsageVertexBuffer);
  const auto indexBuffer = makeBuffer(0x9200, 2048, UsageIndexBuffer);
  state.streamBuffers[0] = stream0;
  state.streamOffsets[0] = 44u;
  state.streamStrides[0] = 28u;
  state.indexBuffer = indexBuffer;
  state.vertexDecl.elements = {
      VertexElement{0, 0, 2, 0, 0, 0},
      VertexElement{0, 12, 4, 0, 10, 0},
      VertexElement{0, 16, 3, 0, 5, 0},
  };

  const DrawCallArgs args{
      PrimitiveType::TriangleList, 7u, 123u, -5, 11u, IndexType::UInt16};
  const DrawDesc desc = makeDrawDescFromState(state, args);
  DrawParam param = makeDrawParamForTest(desc);
  DrawRunDesc run{};
  run.state = makeCanonicalDrawStateForTest(desc);

  check(drawRunAppend(run, param), "indexed draw-run param appends");
  check(drawRunValidate(run), "indexed draw-run validates");

  const auto view = drawRunView(run);
  checkEq(view.draws.size(), std::size_t{1},
          "indexed draw-run exposes one param");
  check(view.draws[0].indexed,
        "indexed draw-run keeps the direct-indexed policy bit");
  checkEq(view.draws[0].primitiveCount, 7u,
          "indexed draw-run keeps primitive count");
  checkEq(view.draws[0].startVertex, 123u,
          "indexed draw-run keeps D3D start vertex for debug/direct callers");
  checkEq(view.draws[0].baseVertexIndex, -5,
          "indexed draw-run keeps base vertex for direct and expanded policies");
  checkEq(view.draws[0].startIndex, 11u,
          "indexed draw-run keeps start index for direct and expanded policies");
  checkEq(view.draws[0].indexType, IndexType::UInt16,
          "indexed draw-run keeps index type");
  check(view.draws[0].userVertexRange.empty(),
        "bound indexed draw-run has no UP vertex payload");
  check(view.draws[0].userIndexRange.empty(),
        "bound indexed draw-run has no UP index payload");
  checkEq(run.state.hot.indexBuffer, indexBuffer->handle(),
          "direct indexed policy keeps the bound index buffer in hot state");
  checkEq(run.state.hot.streamBuffers[0], stream0->handle(),
          "expanded indexed policy keeps source stream buffer in hot state");
  checkEq(run.state.hot.streamOffsets[0], 44u,
          "expanded indexed policy keeps stream byte offset");
  checkEq(run.state.hot.streamStrides[0], 28u,
          "expanded indexed policy keeps stream stride");
  checkEq(run.state.shaderLayout.vertexDecl.elements, desc.vertexDecl.elements,
          "expanded indexed policy keeps vertex declaration layout");

  DrawDesc upDesc = desc;
  upDesc.indexBuffer = {};
  upDesc.vertexDecl.streams[0].buffer.reset();
  upDesc.vertexDecl.streams[0].offset = 0u;
  upDesc.vertexDecl.streams[0].stride = 28u;
  upDesc.userVertexData = {0x10, 0x11, 0x12, 0x13, 0x14};
  upDesc.userIndexData = {0x02, 0x00, 0x01, 0x00};
  DrawParam upParam = makeDrawParamForTest(upDesc);
  DrawRunDesc upRun{};
  upRun.state = makeCanonicalDrawStateForTest(upDesc);
  const DrawParamPayloadView upPayload{
      .userVertexData = std::span<const u8>(upDesc.userVertexData.data(),
                                            upDesc.userVertexData.size()),
      .userIndexData = std::span<const u8>(upDesc.userIndexData.data(),
                                           upDesc.userIndexData.size()),
  };
  check(drawRunAppend(upRun, upParam, upPayload),
        "indexed UP draw-run param appends");
  check(drawRunValidate(upRun), "indexed UP draw-run validates");

  const auto upView = drawRunView(upRun);
  check(upView.draws[0].indexed,
        "indexed UP draw-run keeps indexed policy bit");
  checkEq(upView.draws[0].baseVertexIndex, -5,
          "indexed UP draw-run keeps base vertex");
  checkEq(upView.draws[0].startIndex, 11u,
          "indexed UP draw-run keeps start index");
  checkEq(upRun.state.hot.indexBuffer, Handle{},
          "indexed UP draw-run does not depend on bound index buffer");
  checkEq(upRun.state.hot.streamBuffers[0], Handle{},
          "indexed UP draw-run does not depend on bound stream buffer");
  checkEq(upRun.state.hot.streamOffsets[0], 0u,
          "indexed UP draw-run uses record-local vertex data from zero");
  checkEq(upRun.state.hot.streamStrides[0], 28u,
          "indexed UP draw-run keeps caller stride");
  checkEq(upView.draws[0].userVertexRange.size,
          static_cast<u32>(upDesc.userVertexData.size()),
          "indexed UP draw-run stores vertex payload range");
  checkEq(upView.draws[0].userIndexRange.size,
          static_cast<u32>(upDesc.userIndexData.size()),
          "indexed UP draw-run stores index payload range");
  checkEq(drawRunPayloadSize(upRun),
          upDesc.userVertexData.size() + upDesc.userIndexData.size(),
          "indexed UP draw-run packs both payloads");
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
  testTransformMultiplicationOrderAndBlendSlots();
  testClipPlaneLimitsAtCoreBoundary();
  testClipPlaneTransformPayloadAndMaskBounds();
  testConstantsAndShaderRefs();
  testShaderConstantPayloadSurvivesDrawRunCommandView();
  testShaderLayoutCarriesConstantUsageMetadata();
  testResourceBindingsAndAttachments();
  testVertexDeclFvfAndStreamBindings();
  testTextureStageArgumentCanonicalValues();
  testRenderStateIntentPayloadAcrossDrawRunBoundary();
  testSamplerAndTextureStageDirtyHashPayloadBoundaries();
  testVertexDeclSnapshotSurvivesLaterStateMutation();
  testIndexedDrawRunPolicyDataContract();
  testFlatDrawStateKey();
  return 0;
}
