#include "core_spec_fixtures.hpp"

#include <memory>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

void testRasterStateCoverage() {
  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 4096;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits, backend);
  PresentParameters params{};
  params.backBufferWidth = 16;
  params.backBufferHeight = 16;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{12};
  params.enableAutoDepthStencil = true;
  params.autoDepthStencilFormat = Format::D24S8;
  auto device = factory.createDevice(0, params);
  check(device != nullptr, "raster coverage device");
  auto depthStencil = device->swapChain()->depthStencilSurface();
  check(depthStencil != nullptr, "raster coverage depth stencil");

  const auto fixup = halfPixelFixup(Viewport{0, 0, 16, 16, 0.0f, 1.0f});
  checkNear(fixup[0], 1.0f / 16.0f, 1.0e-6f, "raster half-pixel x");
  checkNear(fixup[1], 1.0f / 16.0f, 1.0e-6f, "raster half-pixel y");

  checkEq(device->setViewport({0, 0, 16, 16, 0.0f, 1.0f}), D3D_OK, "raster viewport");
  checkEq(device->setRenderState(RS_CULL_MODE, static_cast<u32>(CullMode::Ccw)), D3D_OK, "raster cull ccw");
  checkEq(device->setRenderState(RS_Z_ENABLE, 1), D3D_OK, "raster depth enable");
  checkEq(device->setRenderState(RS_Z_WRITE_ENABLE, 1), D3D_OK, "raster depth write");
  checkEq(device->setRenderState(RS_Z_FUNC, static_cast<u32>(CompareFunc::LessEqual)), D3D_OK,
          "raster depth func");
  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 1), D3D_OK, "raster draw ccw");
  check(!backend->draws.empty(), "raster draw recorded");
  const auto& firstDraw = backend->draws.back();
  checkEq(firstDraw.hot.viewport.viewport.width, 16u, "raster draw viewport width");
  checkEq(firstDraw.hot.viewport.viewport.height, 16u, "raster draw viewport height");
  checkEq(flatStateOr(firstDraw.hot.renderStates, RS_CULL_MODE, 0u), static_cast<u32>(CullMode::Ccw),
          "raster cull state ccw");
  checkEq(flatStateOr(firstDraw.hot.renderStates, RS_Z_ENABLE, 0u), 1u, "raster depth enable state");
  checkEq(flatStateOr(firstDraw.hot.renderStates, RS_Z_WRITE_ENABLE, 0u), 1u,
          "raster depth write state");
  checkEq(flatStateOr(firstDraw.hot.renderStates, RS_Z_FUNC, 0u), static_cast<u32>(CompareFunc::LessEqual),
          "raster depth func state");
  checkEq(device->setRenderState(RS_CULL_MODE, static_cast<u32>(CullMode::Cw)), D3D_OK, "raster cull cw");
  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 1), D3D_OK, "raster draw cw");
  const auto& secondDraw = backend->draws.back();
  checkEq(flatStateOr(secondDraw.hot.renderStates, RS_CULL_MODE, 0u), static_cast<u32>(CullMode::Cw),
          "raster cull state cw");
}

void testIndexedDrawPolicyContracts() {
  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 8192;
  limits.maxColorAttachments = 4;

  Factory factory(limits, backend);
  PresentParameters params{};
  params.backBufferWidth = 64;
  params.backBufferHeight = 64;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{1001};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "indexed policy device creation");

  checkEq(device->drawIndexedPrimitive(PrimitiveType::TriangleFan, 1, 0, 0, 0,
                                       IndexType::UInt16),
          D3DERR_INVALIDCALL,
          "indexed fan without an index buffer is rejected before recording");
  std::array<DrawParam, 1> missingIndexFan{};
  missingIndexFan[0].primitiveType = PrimitiveType::TriangleFan;
  missingIndexFan[0].primitiveCount = 1;
  missingIndexFan[0].indexed = true;
  missingIndexFan[0].indexType = IndexType::UInt16;
  checkEq(device->drawPrimitiveRun(std::span<const DrawParam>(
              missingIndexFan.data(), missingIndexFan.size())),
          D3DERR_INVALIDCALL,
          "indexed fan draw-run without an index buffer is rejected before recording");
  check(backend->draws.empty(),
        "invalid indexed fan topology does not reach backend recorder");

  auto vertexBuffer = device->createBuffer({64, Pool::Default, UsageVertexBuffer});
  auto indexBuffer = device->createBuffer({64, Pool::Default, UsageIndexBuffer});
  check(vertexBuffer != nullptr, "indexed policy vertex buffer");
  check(indexBuffer != nullptr, "indexed policy index buffer");
  const std::array<u32, 5> fanSourceIndices{4, 10, 11, 12, 99};
  auto indexRegion = indexBuffer->lock(0, sizeof(fanSourceIndices), 0);
  check(indexRegion.data != nullptr, "indexed policy index buffer lock");
  std::memcpy(indexRegion.data, fanSourceIndices.data(),
              sizeof(fanSourceIndices));
  indexBuffer->unlock();

  checkEq(device->setStreamSource(0, vertexBuffer, 4, 16), D3D_OK,
          "indexed policy stream source");
  checkEq(device->setIndices(indexBuffer, IndexType::UInt32), D3D_OK,
          "indexed policy bound index buffer");

  checkEq(device->drawIndexedPrimitive(PrimitiveType::TriangleList, 2, 5, -2, 3,
                                       IndexType::UInt32),
          D3D_OK, "indexed policy bound indexed draw");
  checkEq(backend->draws.size(), size_t{1}, "indexed policy bound draw count");
  const auto& boundIndexed = backend->draws[0];
  check(boundIndexed.param.indexed, "bound indexed draw remains indexed");
  checkEq(boundIndexed.param.primitiveType, PrimitiveType::TriangleList,
          "bound indexed primitive type");
  checkEq(boundIndexed.param.primitiveCount, 2u, "bound indexed primitive count");
  checkEq(boundIndexed.param.startVertex, 5u, "bound indexed start vertex");
  checkEq(boundIndexed.param.baseVertexIndex, -2, "bound indexed base vertex");
  checkEq(boundIndexed.param.startIndex, 3u, "bound indexed start index");
  checkEq(boundIndexed.param.indexType, IndexType::UInt32,
          "bound indexed draw keeps requested index type");
  checkEq(boundIndexed.hot.indexBuffer, indexBuffer->handle(),
          "bound indexed draw keeps index buffer in base state");
  checkEq(boundIndexed.hot.streamBuffers[0], vertexBuffer->handle(),
          "bound indexed draw keeps stream buffer in base state");
  checkEq(boundIndexed.hot.streamOffsets[0], 4u,
          "bound indexed draw keeps stream offset");
  checkEq(boundIndexed.hot.streamStrides[0], 16u,
          "bound indexed draw keeps stream stride");
  check(boundIndexed.param.userIndexRange.empty(),
        "bound indexed draw does not carry user index payload");

  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 1, 7), D3D_OK,
          "indexed policy non-indexed draw");
  checkEq(backend->draws.size(), size_t{2}, "indexed policy non-indexed draw count");
  const auto& nonIndexed = backend->draws[1];
  check(!nonIndexed.param.indexed, "non-indexed draw param remains non-indexed");
  checkEq(nonIndexed.hot.indexBuffer, Handle{},
          "non-indexed draw strips bound index buffer from base state");
  checkEq(nonIndexed.param.startVertex, 7u, "non-indexed draw keeps start vertex");
  check(nonIndexed.param.userIndexRange.empty(),
        "non-indexed draw does not carry user index payload");

  checkEq(device->drawPrimitive(PrimitiveType::TriangleFan, 2, 7), D3D_OK,
          "indexed policy non-indexed fan draw");
  checkEq(backend->draws.size(), size_t{3}, "indexed policy fan draw count");
  const auto& nonIndexedFan = backend->draws[2];
  check(nonIndexedFan.param.indexed, "non-indexed fan is emitted as indexed");
  checkEq(nonIndexedFan.param.primitiveType, PrimitiveType::TriangleList,
          "non-indexed fan canonical primitive type");
  checkEq(nonIndexedFan.param.primitiveCount, 2u,
          "non-indexed fan primitive count");
  checkEq(nonIndexedFan.param.startVertex, 7u,
          "non-indexed fan keeps source start vertex for diagnostics");
  checkEq(nonIndexedFan.param.baseVertexIndex, 7,
          "non-indexed fan uses start vertex as generated base vertex");
  checkEq(nonIndexedFan.param.startIndex, 0u,
          "non-indexed fan starts generated index payload at zero");
  checkEq(nonIndexedFan.param.indexType, IndexType::UInt16,
          "non-indexed fan uses compact generated indices");
  checkEq(nonIndexedFan.hot.indexBuffer, Handle{},
          "non-indexed fan strips bound index buffer from base state");
  const std::array<u16, 6> expectedNonIndexedFanIndices{0, 1, 2, 0, 2, 3};
  std::array<u8, sizeof(expectedNonIndexedFanIndices)>
      expectedNonIndexedFanIndexBytes{};
  std::memcpy(expectedNonIndexedFanIndexBytes.data(),
              expectedNonIndexedFanIndices.data(),
              expectedNonIndexedFanIndexBytes.size());
  checkEq(nonIndexedFan.param.userIndexRange.size,
          static_cast<u32>(expectedNonIndexedFanIndexBytes.size()),
          "non-indexed fan carries generated index payload");
  checkBytes(payloadSlice(nonIndexedFan, nonIndexedFan.param.userIndexRange,
                          "non-indexed fan user index payload range"),
             std::span<const u8>(expectedNonIndexedFanIndexBytes.data(),
                                 expectedNonIndexedFanIndexBytes.size()),
             "non-indexed fan generated index payload");

  checkEq(device->drawIndexedPrimitive(PrimitiveType::TriangleFan, 2, 0, 3, 1,
                                       IndexType::UInt32),
          D3D_OK, "indexed policy indexed fan draw");
  checkEq(backend->draws.size(), size_t{4}, "indexed policy indexed fan draw count");
  const auto& indexedFan = backend->draws[3];
  check(indexedFan.param.indexed, "indexed fan remains indexed");
  checkEq(indexedFan.param.primitiveType, PrimitiveType::TriangleList,
          "indexed fan canonical primitive type");
  checkEq(indexedFan.param.primitiveCount, 2u,
          "indexed fan primitive count");
  checkEq(indexedFan.param.baseVertexIndex, 3,
          "indexed fan keeps incoming base vertex");
  checkEq(indexedFan.param.startIndex, 0u,
          "indexed fan starts generated index payload at zero");
  checkEq(indexedFan.param.indexType, IndexType::UInt32,
          "indexed fan keeps source index type");
  checkEq(indexedFan.hot.indexBuffer, Handle{},
          "indexed fan strips source index buffer after payload rewrite");
  const std::array<u32, 6> expectedIndexedFanIndices{10, 11, 12, 10, 12, 99};
  std::array<u8, sizeof(expectedIndexedFanIndices)>
      expectedIndexedFanIndexBytes{};
  std::memcpy(expectedIndexedFanIndexBytes.data(),
              expectedIndexedFanIndices.data(),
              expectedIndexedFanIndexBytes.size());
  checkEq(indexedFan.param.userIndexRange.size,
          static_cast<u32>(expectedIndexedFanIndexBytes.size()),
          "indexed fan carries rewritten index payload");
  checkBytes(payloadSlice(indexedFan, indexedFan.param.userIndexRange,
                          "indexed fan user index payload range"),
             std::span<const u8>(expectedIndexedFanIndexBytes.data(),
                                 expectedIndexedFanIndexBytes.size()),
             "indexed fan rewritten index payload");

  std::array<DrawParam, 2> fanRun{};
  fanRun[0].primitiveType = PrimitiveType::TriangleFan;
  fanRun[0].primitiveCount = 2;
  fanRun[0].startVertex = 9;
  fanRun[1].primitiveType = PrimitiveType::TriangleFan;
  fanRun[1].primitiveCount = 2;
  fanRun[1].baseVertexIndex = 5;
  fanRun[1].startIndex = 1;
  fanRun[1].indexType = IndexType::UInt32;
  fanRun[1].indexed = true;
  checkEq(device->drawPrimitiveRun(std::span<const DrawParam>(fanRun.data(),
                                                              fanRun.size())),
          D3D_OK, "indexed policy fan draw run");
  checkEq(backend->draws.size(), size_t{6}, "indexed policy fan draw run count");
  checkEq(backend->drawRuns.back().draws.size(), size_t{2},
          "indexed policy fan run stays coalesced");
  const auto& runNonIndexedFan = backend->draws[4];
  check(runNonIndexedFan.param.indexed,
        "draw run non-indexed fan is emitted as indexed");
  checkEq(runNonIndexedFan.param.primitiveType, PrimitiveType::TriangleList,
          "draw run non-indexed fan canonical primitive type");
  checkEq(runNonIndexedFan.param.baseVertexIndex, 9,
          "draw run non-indexed fan uses start vertex as base vertex");
  checkEq(runNonIndexedFan.param.userIndexRange.size,
          static_cast<u32>(expectedNonIndexedFanIndexBytes.size()),
          "draw run non-indexed fan carries generated index payload");
  checkBytes(payloadSlice(runNonIndexedFan, runNonIndexedFan.param.userIndexRange,
                          "draw run non-indexed fan index payload range"),
             std::span<const u8>(expectedNonIndexedFanIndexBytes.data(),
                                 expectedNonIndexedFanIndexBytes.size()),
             "draw run non-indexed fan generated index payload");

  const auto& runIndexedFan = backend->draws[5];
  check(runIndexedFan.param.indexed, "draw run indexed fan remains indexed");
  checkEq(runIndexedFan.param.primitiveType, PrimitiveType::TriangleList,
          "draw run indexed fan canonical primitive type");
  checkEq(runIndexedFan.param.baseVertexIndex, 5,
          "draw run indexed fan keeps incoming base vertex");
  checkEq(runIndexedFan.param.startIndex, 0u,
          "draw run indexed fan starts generated payload at zero");
  checkEq(runIndexedFan.hot.indexBuffer, Handle{},
          "draw run indexed fan strips source index buffer after payload rewrite");
  checkEq(runIndexedFan.param.userIndexRange.size,
          static_cast<u32>(expectedIndexedFanIndexBytes.size()),
          "draw run indexed fan carries rewritten index payload");
  checkBytes(payloadSlice(runIndexedFan, runIndexedFan.param.userIndexRange,
                          "draw run indexed fan index payload range"),
             std::span<const u8>(expectedIndexedFanIndexBytes.data(),
                                 expectedIndexedFanIndexBytes.size()),
             "draw run indexed fan rewritten index payload");

  const std::array<u8, 24> upVertices{
      0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
      0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
  };
  const std::array<u16, 3> upIndices{2, 1, 0};
  std::array<u8, sizeof(upIndices)> upIndexBytes{};
  std::memcpy(upIndexBytes.data(), upIndices.data(), upIndexBytes.size());

  checkEq(device->drawIndexedPrimitiveUP(
              PrimitiveType::TriangleList, 1,
              std::span<const u8>(upVertices.data(), upVertices.size()),
              std::span<const u8>(upIndexBytes.data(), upIndexBytes.size()),
              IndexType::UInt16, 8),
          D3D_OK, "indexed policy indexed UP draw");
  checkEq(backend->draws.size(), size_t{7}, "indexed policy indexed UP draw count");
  const auto& indexedUp = backend->draws[6];
  check(indexedUp.param.indexed, "indexed UP draw remains indexed");
  checkEq(indexedUp.param.indexType, IndexType::UInt16,
          "indexed UP draw keeps user index type");
  checkEq(indexedUp.hot.indexBuffer, Handle{},
          "indexed UP draw strips bound index buffer from base state");
  checkEq(indexedUp.hot.streamBuffers[0], Handle{},
          "indexed UP draw strips bound stream buffer from base state");
  checkEq(indexedUp.hot.streamOffsets[0], 0u,
          "indexed UP draw uses caller vertex data from offset zero");
  checkEq(indexedUp.hot.streamStrides[0], 8u,
          "indexed UP draw keeps caller vertex stride");
  checkEq(indexedUp.param.userVertexRange.size, static_cast<u32>(upVertices.size()),
          "indexed UP draw carries caller vertex payload");
  checkEq(indexedUp.param.userIndexRange.size, static_cast<u32>(upIndexBytes.size()),
          "indexed UP draw carries caller index payload");
  checkBytes(payloadSlice(indexedUp, indexedUp.param.userIndexRange,
                          "indexed UP user index payload range"),
             std::span<const u8>(upIndexBytes.data(), upIndexBytes.size()),
             "indexed UP draw preserves user index payload");
}

void testCubeTextureSubresourceFlow() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory factory(BackendLimits{}, backend);
  PresentParameters params{};
  params.backBufferWidth = 64;
  params.backBufferHeight = 64;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{7};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "cube test device creation");

  auto cube = device->createTexture(
      {8, 8, 1, 3, Format::A8R8G8B8, TextureType::Cube, Pool::Managed, UsageTexture | UsageRenderTarget});
  check(cube != nullptr, "cube texture creation");
  checkEq(cube->levelCount(), 3u, "cube mip level count");
  checkEq(cube->subresourceCount(), 18u, "cube subresource count");
  checkEq(cube->mipLevelForSubresource(5), 2u, "cube packed subresource mip");

  auto face1Mip2 = cube->surfaceLevel(5);
  check(face1Mip2 != nullptr, "cube face mip surface");
  checkEq(face1Mip2->desc().width, 2u, "cube face mip width");
  checkEq(face1Mip2->desc().height, 2u, "cube face mip height");
  check(!backend->textureSurfaces.empty(), "cube backend texture surface request");
  checkEq(backend->textureSurfaces.back().subresource, 5u, "cube backend packed subresource");
  checkEq(backend->textureSurfaces.back().desc.width, 2u, "cube backend mip width");
  checkEq(backend->textureSurfaces.back().desc.height, 2u, "cube backend mip height");

  face1Mip2->fillColor(nullptr, {1.0f, 1.0f, 0.0f, 1.0f});
  const std::array<u8, 4> yellowPixel = bgra(0x00, 0xff, 0xff, 0xff);
  checkBytes(std::span<const u8>(cube->levelBytes(5).data(), 4),
             std::span<const u8>(yellowPixel.data(), yellowPixel.size()),
             "cube face mip storage");
  check(!backend->textureUploads.empty(), "cube texture upload");
  const auto& upload = backend->textureUploads.back();
  checkEq(upload.level, 5u, "cube upload packed subresource");
  checkEq(upload.width, 2u, "cube upload width");
  checkEq(upload.height, 2u, "cube upload height");

  auto srcCube = device->createTexture(
      {2, 2, 1, 2, Format::A8R8G8B8, TextureType::Cube, Pool::Managed, UsageTexture});
  auto dstCube = device->createTexture(
      {2, 2, 1, 2, Format::A8R8G8B8, TextureType::Cube, Pool::Managed, UsageTexture});
  check(srcCube != nullptr && dstCube != nullptr, "cube update textures");
  auto srcFace4Mip1 = srcCube->surfaceLevel(4u * srcCube->levelCount() + 1u);
  check(srcFace4Mip1 != nullptr, "cube update source face/mip surface");
  srcFace4Mip1->fillColor(nullptr, {0.0f, 1.0f, 0.0f, 1.0f});
  checkEq(device->updateTexture(srcCube, dstCube), D3D_OK, "cube UpdateTexture");
  const std::array<u8, 4> greenPixel = bgra(0x00, 0xff, 0x00, 0xff);
  checkBytes(std::span<const u8>(dstCube->levelBytes(4u * dstCube->levelCount() + 1u).data(), 4),
             std::span<const u8>(greenPixel.data(), greenPixel.size()),
             "cube UpdateTexture copies packed face/mip subresource");

  auto genCube = device->createTexture(
      {2, 2, 1, 2, Format::A8R8G8B8, TextureType::Cube, Pool::Managed, UsageTexture});
  check(genCube != nullptr, "cube generate texture");
  auto genFace3Base = genCube->surfaceLevel(3u * genCube->levelCount());
  check(genFace3Base != nullptr, "cube generate base face surface");
  genFace3Base->fillColor(nullptr, {1.0f, 0.0f, 0.0f, 1.0f});
  checkEq(genCube->generateMipSubLevels(), D3D_OK, "cube GenerateMipSubLevels");
  const std::array<u8, 4> redPixel = bgra(0x00, 0x00, 0xff, 0xff);
  checkBytes(std::span<const u8>(genCube->levelBytes(3u * genCube->levelCount() + 1u).data(), 4),
             std::span<const u8>(redPixel.data(), redPixel.size()),
             "cube GenerateMipSubLevels writes packed face mip");

  auto invalid = cube->surfaceLevel(cube->subresourceCount());
  check(invalid == nullptr, "cube invalid subresource rejected");
}

void testAutogenUpdateTextureRegeneratesMipShadow() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory factory(BackendLimits{}, backend);
  PresentParameters params{};
  params.backBufferWidth = 64;
  params.backBufferHeight = 64;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{8};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "autogen update device creation");

  auto src = device->createTexture(
      {2, 2, 1, 1, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
  auto dst = device->createTexture(
      {2, 2, 1, 2, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed,
       UsageTexture | UsageAutoGenMipmap});
  check(src != nullptr && dst != nullptr, "autogen update textures");

  src->fillColor(0, nullptr, {1.0f, 0.0f, 0.0f, 1.0f});
  dst->fillColor(0, nullptr, {0.0f, 0.0f, 1.0f, 1.0f});
  dst->fillColor(1, nullptr, {0.0f, 0.0f, 0.0f, 1.0f});

  checkEq(device->updateTexture(src, dst), D3D_OK, "autogen UpdateTexture");
  const std::array<u8, 4> redPixel = bgra(0x00, 0x00, 0xff, 0xff);
  checkBytes(std::span<const u8>(dst->levelBytes(1).data(), 4),
             std::span<const u8>(redPixel.data(), redPixel.size()),
             "autogen UpdateTexture regenerates mip shadow from copied base");
}

void testMetalSamplerBorderColorCoverage() {
#if !defined(__APPLE__)
  return;
#else
  if (!getenvFlag("DXMT9_CORE_SPEC_METAL_INTEGRATION")) {
    return;
  }
  BackendLimits limits{};
  limits.maxTextureSize = 1024;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits);
  PresentParameters params{};
  params.backBufferWidth = 64;
  params.backBufferHeight = 64;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{31337};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "metal sampler border device");
  auto backBuffer = device->swapChain()->backBuffer();
  auto readbackSurface = device->createSurface({64, 64, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  auto texture = device->createTexture({1, 1, 1, 1, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
  check(backBuffer != nullptr, "metal sampler border back buffer");
  check(readbackSurface != nullptr, "metal sampler border readback");
  check(texture != nullptr, "metal sampler border texture");

  auto textureLevel0 = texture->surfaceLevel(0);
  check(textureLevel0 != nullptr, "metal sampler border level0");
  textureLevel0->fillColor(nullptr, {1.0f, 0.0f, 0.0f, 1.0f});

  checkEq(device->setRenderTarget(0, backBuffer), D3D_OK, "metal sampler border render target");
  checkEq(device->setViewport({0, 0, 64, 64, 0.0f, 1.0f}), D3D_OK, "metal sampler border viewport");
  checkEq(device->setFVF(kFvfXyzrhw | kFvfTex1), D3D_OK, "metal sampler border fvf");
  checkEq(device->setTexture(0, texture), D3D_OK, "metal sampler border bind texture");
  checkEq(device->setTextureStageState(0, TSS_COLOR_OP, static_cast<u32>(TextureOp::SelectArg1)), D3D_OK,
          "metal sampler border color op");
  checkEq(device->setTextureStageState(0, TSS_ALPHA_OP, static_cast<u32>(TextureOp::SelectArg1)), D3D_OK,
          "metal sampler border alpha op");
  checkEq(device->setSamplerState(0, SAMP_MIN_FILTER, 1), D3D_OK, "metal sampler border min filter");
  checkEq(device->setSamplerState(0, SAMP_MAG_FILTER, 1), D3D_OK, "metal sampler border mag filter");
  checkEq(device->setSamplerState(0, SAMP_ADDRESS_U, kTextureAddressBorder), D3D_OK,
          "metal sampler border address u");
  checkEq(device->setSamplerState(0, SAMP_ADDRESS_V, kTextureAddressBorder), D3D_OK,
          "metal sampler border address v");
  checkEq(device->setSamplerState(0, SAMP_BORDER_COLOR, 0xffffffffu), D3D_OK,
          "metal sampler border color");

  ClearDesc clear{};
  clear.clearColor = true;
  clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
  clear.colorAttachments[0] = {backBuffer->handle(), backBuffer->level(), backBuffer->multiSampleCount()};
  checkEq(device->clear(clear), D3D_OK, "metal sampler border clear");

  const std::array<ScreenSpaceTexturedVertex, 6> quad{
      ScreenSpaceTexturedVertex{0.0f, 0.0f, 0.0f, 1.0f, -0.5f, -0.5f},
      ScreenSpaceTexturedVertex{64.0f, 0.0f, 0.0f, 1.0f, 1.5f, -0.5f},
      ScreenSpaceTexturedVertex{0.0f, 64.0f, 0.0f, 1.0f, -0.5f, 1.5f},
      ScreenSpaceTexturedVertex{64.0f, 0.0f, 0.0f, 1.0f, 1.5f, -0.5f},
      ScreenSpaceTexturedVertex{64.0f, 64.0f, 0.0f, 1.0f, 1.5f, 1.5f},
      ScreenSpaceTexturedVertex{0.0f, 64.0f, 0.0f, 1.0f, -0.5f, 1.5f},
  };
  const auto* quadBytes = reinterpret_cast<const u8*>(quad.data());
  checkEq(device->drawPrimitiveUP(PrimitiveType::TriangleList, 2,
                                  std::span<const u8>(quadBytes, sizeof(quad))),
          D3D_OK, "metal sampler border draw");
  checkEq(device->getRenderTargetData(backBuffer, readbackSurface), D3D_OK, "metal sampler border readback");

  auto region = readbackSurface->lockRect(nullptr, 0);
  check(region.data != nullptr, "metal sampler border lock");
  const auto whitePixel = bgra(0xff, 0xff, 0xff, 0xff);
  const auto redPixel = bgra(0x00, 0x00, 0xff, 0xff);
  checkEq(readPixel(region, 8, 8), whitePixel, "metal sampler border top-left");
  checkEq(readPixel(region, 56, 8), whitePixel, "metal sampler border top-right");
  checkEq(readPixel(region, 32, 32), redPixel, "metal sampler border center");
  checkEq(readPixel(region, 8, 56), whitePixel, "metal sampler border bottom-left");
  checkEq(readPixel(region, 56, 56), whitePixel, "metal sampler border bottom-right");
  readbackSurface->unlockRect();
#endif
}

void testProgrammableTextureOrientationSmoke() {
#if !defined(__APPLE__)
  return;
#else
  if (!getenvFlag("DXMT9_CORE_SPEC_METAL_INTEGRATION")) {
    return;
  }
  BackendLimits limits{};
  limits.maxTextureSize = 1024;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits);
  PresentParameters params{};
  params.backBufferWidth = 8;
  params.backBufferHeight = 8;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{4242};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "programmatic texture orientation device");
  auto backBuffer = device->swapChain()->backBuffer();
  auto readbackSurface = device->createSurface({8, 8, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  auto texture = device->createTexture({2, 2, 1, 1, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
  check(backBuffer != nullptr, "programmatic texture orientation back buffer");
  check(readbackSurface != nullptr, "programmatic texture orientation readback");
  check(texture != nullptr, "programmatic texture orientation texture");

  const auto redPixel = bgra(0x00, 0x00, 0xff, 0xff);
  const auto greenPixel = bgra(0x00, 0xff, 0x00, 0xff);
  const auto bluePixel = bgra(0xff, 0x00, 0x00, 0xff);
  const auto whitePixel = bgra(0xff, 0xff, 0xff, 0xff);
  auto upload = texture->lockRect(0, nullptr, UsageDiscard);
  check(upload.data != nullptr, "programmatic texture orientation texture lock");
  auto* uploadBytes = static_cast<u8*>(upload.data);
  std::memcpy(uploadBytes + 0u * upload.pitch + 0u * 4u, redPixel.data(), redPixel.size());
  std::memcpy(uploadBytes + 0u * upload.pitch + 1u * 4u, greenPixel.data(), greenPixel.size());
  std::memcpy(uploadBytes + 1u * upload.pitch + 0u * 4u, bluePixel.data(), bluePixel.size());
  std::memcpy(uploadBytes + 1u * upload.pitch + 1u * 4u, whitePixel.data(), whitePixel.size());
  texture->unlockRect(0);

  const auto pixelWords = makeTexturedPixelShaderBytecode(0);
  ShaderRef pixelShader{};
  pixelShader.kind = ShaderRef::Kind::Bytecode;
  pixelShader.bytecode.bytes.assign(reinterpret_cast<const u8*>(pixelWords.data()),
                                    reinterpret_cast<const u8*>(pixelWords.data() + pixelWords.size()));
  pixelShader.bytecode.hash = hashBytes(std::as_bytes(std::span<const u32>(pixelWords.data(), pixelWords.size())));

  checkEq(device->setRenderTarget(0, backBuffer), D3D_OK, "programmatic texture orientation render target");
  checkEq(device->setViewport({0, 0, 8, 8, 0.0f, 1.0f}), D3D_OK, "programmatic texture orientation viewport");
  checkEq(device->setFVF(kFvfXyzrhw | kFvfTex1), D3D_OK, "programmatic texture orientation fvf");
  checkEq(device->setTexture(0, texture), D3D_OK, "programmatic texture orientation bind texture");
  checkEq(device->setPixelShader(pixelShader), D3D_OK, "programmatic texture orientation pixel shader");
  checkEq(device->setSamplerState(0, SAMP_MIN_FILTER, 1), D3D_OK, "programmatic texture orientation min filter");
  checkEq(device->setSamplerState(0, SAMP_MAG_FILTER, 1), D3D_OK, "programmatic texture orientation mag filter");
  checkEq(device->setSamplerState(0, SAMP_ADDRESS_U, kTextureAddressClamp), D3D_OK,
          "programmatic texture orientation address u");
  checkEq(device->setSamplerState(0, SAMP_ADDRESS_V, kTextureAddressClamp), D3D_OK,
          "programmatic texture orientation address v");

  ClearDesc clear{};
  clear.clearColor = true;
  clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
  clear.colorAttachments[0] = {backBuffer->handle(), backBuffer->level(), backBuffer->multiSampleCount()};
  checkEq(device->clear(clear), D3D_OK, "programmatic texture orientation clear");

  const std::array<ScreenSpaceTexturedVertex, 6> quad{
      ScreenSpaceTexturedVertex{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
      ScreenSpaceTexturedVertex{8.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      ScreenSpaceTexturedVertex{0.0f, 8.0f, 0.0f, 1.0f, 0.0f, 1.0f},
      ScreenSpaceTexturedVertex{8.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      ScreenSpaceTexturedVertex{8.0f, 8.0f, 0.0f, 1.0f, 1.0f, 1.0f},
      ScreenSpaceTexturedVertex{0.0f, 8.0f, 0.0f, 1.0f, 0.0f, 1.0f},
  };
  const auto* quadBytes = reinterpret_cast<const u8*>(quad.data());
  checkEq(device->drawPrimitiveUP(PrimitiveType::TriangleList, 2,
                                  std::span<const u8>(quadBytes, sizeof(quad)), sizeof(ScreenSpaceTexturedVertex)),
          D3D_OK, "programmatic texture orientation draw");
  checkEq(device->getRenderTargetData(backBuffer, readbackSurface), D3D_OK,
          "programmatic texture orientation readback");

  auto region = readbackSurface->lockRect(nullptr, 0);
  check(region.data != nullptr, "programmatic texture orientation lock");
  checkEq(readPixel(region, 1, 1), redPixel, "programmatic texture orientation top-left");
  checkEq(readPixel(region, 6, 1), greenPixel, "programmatic texture orientation top-right");
  checkEq(readPixel(region, 1, 6), bluePixel, "programmatic texture orientation bottom-left");
  checkEq(readPixel(region, 6, 6), whitePixel, "programmatic texture orientation bottom-right");
  readbackSurface->unlockRect();
#endif
}

void testProgrammablePalettizedTextureDrawSmoke() {
#if !defined(__APPLE__)
  return;
#else
  if (!getenvFlag("DXMT9_CORE_SPEC_METAL_INTEGRATION")) {
    return;
  }

  BackendLimits limits{};
  limits.maxTextureSize = 1024;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits);
  PresentParameters params{};
  params.backBufferWidth = 8;
  params.backBufferHeight = 8;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{5252};

  auto coreDevice = factory.createDevice(0, params);
  check(coreDevice != nullptr, "programmable palettized texture device");
  auto backBuffer = coreDevice->swapChain()->backBuffer();
  auto readbackSurface =
      coreDevice->createSurface({8, 8, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  check(backBuffer != nullptr, "programmable palettized texture back buffer");
  check(readbackSurface != nullptr, "programmable palettized texture readback");

  const auto pixelWords = makeTexturedPixelShaderBytecode(0);
  ShaderRef pixelShader{};
  pixelShader.kind = ShaderRef::Kind::Bytecode;
  pixelShader.bytecode.bytes.assign(reinterpret_cast<const u8*>(pixelWords.data()),
                                    reinterpret_cast<const u8*>(pixelWords.data() + pixelWords.size()));
  pixelShader.bytecode.hash = hashBytes(std::as_bytes(std::span<const u32>(pixelWords.data(), pixelWords.size())));

  checkEq(coreDevice->setRenderTarget(0, backBuffer), D3D_OK,
          "programmable palettized texture render target");
  checkEq(coreDevice->setViewport({0, 0, 8, 8, 0.0f, 1.0f}), D3D_OK,
          "programmable palettized texture viewport");
  checkEq(coreDevice->setFVF(kFvfXyzrhw | kFvfTex1), D3D_OK,
          "programmable palettized texture fvf");
  checkEq(coreDevice->setPixelShader(pixelShader), D3D_OK,
          "programmable palettized texture pixel shader");
  checkEq(coreDevice->setSamplerState(0, SAMP_MIN_FILTER, 1), D3D_OK,
          "programmable palettized texture min filter");
  checkEq(coreDevice->setSamplerState(0, SAMP_MAG_FILTER, 1), D3D_OK,
          "programmable palettized texture mag filter");
  checkEq(coreDevice->setSamplerState(0, SAMP_ADDRESS_U, kTextureAddressClamp), D3D_OK,
          "programmable palettized texture address u");
  checkEq(coreDevice->setSamplerState(0, SAMP_ADDRESS_V, kTextureAddressClamp), D3D_OK,
          "programmable palettized texture address v");

  const std::array<ScreenSpaceTexturedVertex, 6> quad{
      ScreenSpaceTexturedVertex{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
      ScreenSpaceTexturedVertex{8.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      ScreenSpaceTexturedVertex{0.0f, 8.0f, 0.0f, 1.0f, 0.0f, 1.0f},
      ScreenSpaceTexturedVertex{8.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      ScreenSpaceTexturedVertex{8.0f, 8.0f, 0.0f, 1.0f, 1.0f, 1.0f},
      ScreenSpaceTexturedVertex{0.0f, 8.0f, 0.0f, 1.0f, 0.0f, 1.0f},
  };
  const auto* quadBytes = reinterpret_cast<const u8*>(quad.data());

  const auto topLeft = bgra(0x33, 0x22, 0x11, 0xff);
  const auto topRight = bgra(0x66, 0x55, 0x44, 0xff);
  const auto bottomLeft = bgra(0x99, 0x88, 0x77, 0xff);
  const auto bottomRight = bgra(0xcc, 0xbb, 0xaa, 0xff);
  const auto checkPixel = [](const LockedRegion& region, u32 x, u32 y,
                             const std::array<u8, 4>& expected,
                             const std::string& label) {
    const auto actual = readPixel(region, x, y);
    if (actual != expected) {
      std::ostringstream out;
      out << label << " got=0x"
          << std::hex << static_cast<unsigned>(actual[3])
          << static_cast<unsigned>(actual[2])
          << static_cast<unsigned>(actual[1])
          << static_cast<unsigned>(actual[0])
          << " expected=0x"
          << static_cast<unsigned>(expected[3])
          << static_cast<unsigned>(expected[2])
          << static_cast<unsigned>(expected[1])
          << static_cast<unsigned>(expected[0]);
      fail(out.str());
    }
  };
  const auto checkExpanded = [&](const D9CTexture& texture, std::string_view label) {
    const auto levelBytes = texture.obj->levelBytes(0);
    check(levelBytes.size() >= 16, std::string(label) + " expanded bytes");
    checkEq(std::array<u8, 4>{levelBytes[0], levelBytes[1], levelBytes[2], levelBytes[3]},
            topLeft, std::string(label) + " expanded top-left");
    checkEq(std::array<u8, 4>{levelBytes[4], levelBytes[5], levelBytes[6], levelBytes[7]},
            topRight, std::string(label) + " expanded top-right");
    checkEq(std::array<u8, 4>{levelBytes[8], levelBytes[9], levelBytes[10], levelBytes[11]},
            bottomLeft, std::string(label) + " expanded bottom-left");
    checkEq(std::array<u8, 4>{levelBytes[12], levelBytes[13], levelBytes[14], levelBytes[15]},
            bottomRight, std::string(label) + " expanded bottom-right");
  };
  const auto drawAndCheck = [&](const D9CTexture& texture, std::string_view label) {
    checkEq(coreDevice->setTexture(0, texture.obj), D3D_OK,
            std::string(label) + " bind texture");
    ClearDesc clear{};
    clear.clearColor = true;
    clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
    clear.colorAttachments[0] = {backBuffer->handle(), backBuffer->level(), backBuffer->multiSampleCount()};
    checkEq(coreDevice->clear(clear), D3D_OK, std::string(label) + " clear");
    checkEq(coreDevice->drawPrimitiveUP(PrimitiveType::TriangleList, 2,
                                       std::span<const u8>(quadBytes, sizeof(quad)),
                                       sizeof(ScreenSpaceTexturedVertex)),
            D3D_OK, std::string(label) + " draw");
    checkEq(coreDevice->getRenderTargetData(backBuffer, readbackSurface), D3D_OK,
            std::string(label) + " readback");

    auto region = readbackSurface->lockRect(nullptr, 0);
    check(region.data != nullptr, std::string(label) + " lock readback");
    checkPixel(region, 1, 1, topLeft, std::string(label) + " top-left");
    checkPixel(region, 6, 1, topRight, std::string(label) + " top-right");
    checkPixel(region, 1, 6, bottomLeft, std::string(label) + " bottom-left");
    checkPixel(region, 6, 6, bottomRight, std::string(label) + " bottom-right");
    readbackSurface->unlockRect();

    checkEq(coreDevice->setTexture(0, nullptr), D3D_OK,
            std::string(label) + " unbind texture");
  };

  const auto runCase = [&](uint32_t format, std::string_view label) {
    auto textureObject = coreDevice->createTexture(
        {2, 2, 1, 1, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
    check(textureObject != nullptr, std::string(label) + " create texture");

    D9CTexture texture{textureObject, nullptr};
    texture.d3dFormat = format;
    texture.palettized = true;
    texture.p8Levels.resize(1);
    const uint32_t texelBytes = format == 40u ? 2u : 1u;
    texture.p8Levels[0].assign(static_cast<size_t>(2u * 2u * texelBytes), 0);
    auto* texels = texture.p8Levels[0].data();
    if (format == 40u) {
      texels[0] = 1;
      texels[1] = 0xff;
      texels[2] = 2;
      texels[3] = 0xff;
      texels[4] = 3;
      texels[5] = 0xff;
      texels[6] = 4;
      texels[7] = 0xff;
    } else {
      texels[0] = 1;
      texels[1] = 2;
      texels[2] = 3;
      texels[3] = 4;
    }

    std::array<uint32_t, 256> palette{};
    palette[1] = 0xff112233u;
    palette[2] = 0xff445566u;
    palette[3] = 0xff778899u;
    palette[4] = 0xffaabbccu;
    checkEq(dxmt9c_texture_set_palette(&texture, palette.data(),
                                        static_cast<uint32_t>(palette.size())),
            D3D_OK, std::string(label) + " set palette");
    checkExpanded(texture, label);
    drawAndCheck(texture, label);
  };

  const auto runUpdateTextureCase = [&](uint32_t format, std::string_view label) {
    const auto makePalettizedObject = [&](std::string_view createLabel) {
      auto object = coreDevice->createTexture(
          {2, 2, 1, 1, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
      check(object != nullptr, std::string(createLabel) + " create texture");
      return object;
    };
    const auto initPalettized = [&](D9CTexture& texture) {
      texture.d3dFormat = format;
      texture.palettized = true;
      texture.p8Levels.resize(1);
      const uint32_t texelBytes = format == 40u ? 2u : 1u;
      texture.p8Levels[0].assign(static_cast<size_t>(2u * 2u * texelBytes), 0);
    };

    D9CTexture src{makePalettizedObject(std::string(label) + " source"), nullptr};
    D9CTexture dst{makePalettizedObject(std::string(label) + " destination"), nullptr};
    initPalettized(src);
    initPalettized(dst);
    auto* srcTexels = src.p8Levels[0].data();
    if (format == 40u) {
      srcTexels[0] = 1;
      srcTexels[1] = 0xff;
      srcTexels[2] = 2;
      srcTexels[3] = 0xff;
      srcTexels[4] = 3;
      srcTexels[5] = 0xff;
      srcTexels[6] = 4;
      srcTexels[7] = 0xff;
    } else {
      srcTexels[0] = 1;
      srcTexels[1] = 2;
      srcTexels[2] = 3;
      srcTexels[3] = 4;
    }

    std::array<uint32_t, 256> sourcePalette{};
    sourcePalette[1] = 0xff010203u;
    sourcePalette[2] = 0xff040506u;
    sourcePalette[3] = 0xff070809u;
    sourcePalette[4] = 0xff0a0b0cu;
    checkEq(dxmt9c_texture_set_palette(&src, sourcePalette.data(),
                                        static_cast<uint32_t>(sourcePalette.size())),
            D3D_OK, std::string(label) + " set source palette");

    std::array<uint32_t, 256> destinationPalette{};
    destinationPalette[1] = 0xff112233u;
    destinationPalette[2] = 0xff445566u;
    destinationPalette[3] = 0xff778899u;
    destinationPalette[4] = 0xffaabbccu;
    checkEq(dxmt9c_texture_set_palette(&dst, destinationPalette.data(),
                                        static_cast<uint32_t>(destinationPalette.size())),
            D3D_OK, std::string(label) + " set destination palette");

    checkEq(coreDevice->updateTexture(src.obj, dst.obj), D3D_OK,
            std::string(label) + " core UpdateTexture");
    auto updateBarrier = coreDevice->createQuery(QueryType::Event);
    check(updateBarrier != nullptr, std::string(label) + " update barrier query");
    checkEq(coreDevice->getQueryData(updateBarrier, nullptr, 0, QUERY_GETDATA_FLUSH),
            S_OK, std::string(label) + " update barrier flush");
    // Match the PE path ordering: the GPU copy must be flushed before the
    // destination's palettized shadow is re-expanded through its own palette.
    dst.p8Levels[0] = src.p8Levels[0];
    dxmt9c_expand_palettized_subresource(&dst, 0);

    checkExpanded(dst, label);
    drawAndCheck(dst, label);
  };

  runCase(41u, "programmable P8 texture");
  runCase(40u, "programmable A8P8 texture");
  runUpdateTextureCase(41u, "programmable P8 UpdateTexture destination");
  runUpdateTextureCase(40u, "programmable A8P8 UpdateTexture destination");

  checkEq(coreDevice->setPixelShader({}), D3D_OK, "programmable palettized texture reset pixel shader");
#endif
}

}  // namespace

int main() {
  try {
    testRasterStateCoverage();
    testIndexedDrawPolicyContracts();
    testMetalSamplerBorderColorCoverage();
    testCubeTextureSubresourceFlow();
    testAutogenUpdateTextureRegeneratesMipShadow();
    testProgrammableTextureOrientationSmoke();
    testProgrammablePalettizedTextureDrawSmoke();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
