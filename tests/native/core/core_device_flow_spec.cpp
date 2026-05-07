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

  auto vertexBuffer = device->createBuffer({64, Pool::Default, UsageVertexBuffer});
  auto indexBuffer = device->createBuffer({64, Pool::Default, UsageIndexBuffer});
  check(vertexBuffer != nullptr, "indexed policy vertex buffer");
  check(indexBuffer != nullptr, "indexed policy index buffer");

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
  checkEq(backend->draws.size(), size_t{3}, "indexed policy indexed UP draw count");
  const auto& indexedUp = backend->draws[2];
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

void testDeviceCoreFlow() {
  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 8192;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits, backend);
  PresentParameters params{};
  params.backBufferWidth = 640;
  params.backBufferHeight = 480;
  params.backBufferFormat = Format::A8R8G8B8;
  params.backBufferCount = 2;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Default;
  params.deviceWindow = Handle{99};
  params.enableAutoDepthStencil = true;
  params.autoDepthStencilFormat = Format::D24S8;
  params.multiSampleType = MultiSampleType::Four;

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "device creation");
  check(device->swapChain() != nullptr, "default swap chain");
  const auto primaryChain = device->swapChain();
  const auto backBuffer = primaryChain->backBuffer();
  const auto depthStencil = primaryChain->depthStencilSurface();
  check(backBuffer != nullptr, "back buffer");
  check(depthStencil != nullptr, "depth stencil");
  check(primaryChain->displaySyncEnabled(), "display sync");
  checkEq(backBuffer->desc().width, 640u, "back buffer width");
  checkEq(backBuffer->desc().height, 480u, "back buffer height");
  checkEq(backBuffer->desc().format, Format::A8R8G8B8, "back buffer format");
  checkEq(backBuffer->multiSampleCount(), 4u, "back buffer sample count");
  checkEq(device->state().renderTargets[0].handle, backBuffer->handle(), "primary render target binding");
  checkEq(device->state().depthStencil.handle, depthStencil->handle(), "depth stencil binding");

  auto dynamicBuffer = device->createBuffer({16, Pool::Default, UsageVertexBuffer | UsageDynamic});
  auto managedBuffer = device->createBuffer({16, Pool::Managed, UsageVertexBuffer});
  auto texture = device->createTexture({4, 4, 1, 2, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
  auto srcTexture = device->createTexture({2, 2, 1, 2, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
  auto dstTexture = device->createTexture({2, 2, 1, 2, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
  auto dxt5Texture =
      device->createTexture({1276, 164, 1, 1, Format::DXT5, TextureType::TwoD, Pool::Managed, UsageTexture});
  auto systemSurface = device->createSurface({2, 2, Format::A8R8G8B8, Pool::SystemMem, 0, false, false});
  auto scratchSurface = device->createSurface({2, 2, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  auto copySurface = device->createSurface({2, 2, Format::A8R8G8B8, Pool::Default, 0, false, false});
  auto stretchSurface = device->createSurface({4, 4, Format::A8R8G8B8, Pool::Default, 0, false, false});
  auto readbackSurface = device->createSurface({2, 2, Format::A8R8G8B8, Pool::Scratch, 0, false, false});

  check(dynamicBuffer != nullptr, "default buffer");
  check(managedBuffer != nullptr, "managed buffer");
  check(texture != nullptr, "managed texture");
  check(srcTexture != nullptr, "source texture");
  check(dstTexture != nullptr, "destination texture");
  check(dxt5Texture != nullptr, "dxt5 texture");
  check(systemSurface != nullptr, "systemmem surface");
  check(scratchSurface != nullptr, "scratch surface");
  check(copySurface != nullptr, "copy surface");
  check(stretchSurface != nullptr, "stretch surface");
  check(readbackSurface != nullptr, "readback surface");

  auto bufferRegion = dynamicBuffer->lock(0, 16, UsageDiscard);
  check(bufferRegion.data != nullptr, "buffer lock");
  const std::array<u8, 16> bufferBytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  std::memcpy(bufferRegion.data, bufferBytes.data(), bufferBytes.size());
  dynamicBuffer->unlock();
  checkBytes(dynamicBuffer->bytes(), std::span<const u8>(bufferBytes.data(), bufferBytes.size()),
             "buffer upload");

  auto managedRegion = managedBuffer->lock(0, 16, 0);
  check(managedRegion.data != nullptr, "managed buffer lock");
  const std::array<u8, 16> managedBytes{16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  std::memcpy(managedRegion.data, managedBytes.data(), managedBytes.size());
  managedBuffer->unlock();
  checkEq(backend->mappedBuffers.size(), size_t{2}, "backend buffer map count");
  checkEq(backend->unmappedBuffers.size(), size_t{2}, "backend buffer unmap count");

  const auto textureLevel1 = texture->surfaceLevel(1);
  check(textureLevel1 != nullptr, "texture level surface");
  textureLevel1->fillColor(nullptr, {0.0f, 1.0f, 0.0f, 1.0f});
  const std::array<u8, 4> greenPixel = bgra(0x00, 0xff, 0x00, 0xff);
  checkBytes(std::span<const u8>(texture->levelBytes(1).data(), 4),
             std::span<const u8>(greenPixel.data(), greenPixel.size()),
             "texture-backed surface fill");

  auto srcLevel0 = srcTexture->surfaceLevel(0);
  auto srcLevel1 = srcTexture->surfaceLevel(1);
  auto dstLevel0 = dstTexture->surfaceLevel(0);
  auto dstLevel1 = dstTexture->surfaceLevel(1);
  check(srcLevel0 != nullptr && srcLevel1 != nullptr && dstLevel0 != nullptr && dstLevel1 != nullptr,
        "texture levels");
  srcLevel0->fillColor(nullptr, {1.0f, 0.0f, 0.0f, 1.0f});
  srcLevel1->fillColor(nullptr, {0.0f, 0.0f, 1.0f, 1.0f});
  checkEq(device->updateTexture(srcTexture, dstTexture), D3D_OK, "update texture");
  checkBytes(dstTexture->levelBytes(0), srcTexture->levelBytes(0), "texture level 0 copy");
  checkBytes(dstTexture->levelBytes(1), srcTexture->levelBytes(1), "texture level 1 copy");

  checkEq(formatRowPitch(Format::DXT5, 1276), 5104u, "dxt5 row pitch");
  checkEq(formatRowCount(Format::DXT5, 164), 41u, "dxt5 row count");
  checkEq(formatByteSize(Format::DXT5, 1276, 164), size_t{209264}, "dxt5 byte size");
  auto dxt5Region = dxt5Texture->lockRect(0, nullptr, 0);
  check(dxt5Region.data != nullptr, "dxt5 texture lock");
  checkEq(dxt5Region.pitch, 5104u, "dxt5 texture pitch");
  auto* dxt5Bytes = static_cast<u8*>(dxt5Region.data);
  dxt5Bytes[0] = 0x11u;
  dxt5Bytes[formatByteSize(Format::DXT5, 1276, 164) - 1u] = 0xeeu;
  dxt5Texture->unlockRect(0);
  check(!backend->textureUploads.empty(), "dxt5 texture upload");
  const auto& dxt5Upload = backend->textureUploads.back();
  checkEq(dxt5Upload.pitch, 5104u, "dxt5 upload pitch");
  checkEq(dxt5Upload.bytes.size(), size_t{209264}, "dxt5 upload size");

  checkEq(device->fillSurface(systemSurface, nullptr, {0.0f, 0.0f, 1.0f, 1.0f}), D3D_OK, "systemmem fill");
  checkEq(device->fillSurface(scratchSurface, nullptr, {1.0f, 1.0f, 0.0f, 1.0f}), D3D_OK, "scratch fill");
  checkEq(device->updateSurface(systemSurface, copySurface), D3D_OK, "update surface");
  checkEq(device->stretchRect(systemSurface, nullptr, stretchSurface, nullptr, true), D3D_OK,
          "stretch rect");
  checkEq(device->getRenderTargetData(copySurface, readbackSurface), D3D_OK, "readback");
  auto copyRegion = copySurface->lockRect(nullptr, 0);
  check(copyRegion.data != nullptr, "copy surface lock");
  const std::array<u8, 4> bluePixel = bgra(0xff, 0x00, 0x00, 0xff);
  checkBytes(std::span<const u8>(static_cast<const u8*>(copyRegion.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()), "surface copy");
  copySurface->unlockRect();

  auto stretchRegion = stretchSurface->lockRect(nullptr, 0);
  check(stretchRegion.data != nullptr, "stretch surface lock");
  checkBytes(std::span<const u8>(static_cast<const u8*>(stretchRegion.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()), "stretched surface copy");
  stretchSurface->unlockRect();

  auto readbackRegion = readbackSurface->lockRect(nullptr, 0);
  check(readbackRegion.data != nullptr, "readback surface lock");
  checkBytes(std::span<const u8>(static_cast<const u8*>(readbackRegion.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()), "readback surface copy");
  readbackSurface->unlockRect();

  auto systemRegion = systemSurface->lockRect(nullptr, 0);
  check(systemRegion.data != nullptr, "system surface lock");
  checkBytes(std::span<const u8>(static_cast<const u8*>(systemRegion.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()),
             "system surface fill");
  systemSurface->unlockRect();

  auto scratchRegion = scratchSurface->lockRect(nullptr, 0);
  check(scratchRegion.data != nullptr, "scratch surface lock");
  const std::array<u8, 4> yellowPixel = bgra(0x00, 0xff, 0xff, 0xff);
  checkBytes(std::span<const u8>(static_cast<const u8*>(scratchRegion.data), 4),
             std::span<const u8>(yellowPixel.data(), yellowPixel.size()),
             "scratch surface fill");
  scratchSurface->unlockRect();

  checkEq(backend->colorFills.size(), size_t{2}, "backend color fill count");
  checkEq(backend->surfaceCopies.size(), size_t{4}, "backend surface copy count");
  checkEq(backend->stretchRects.size(), size_t{1}, "backend stretch rect count");
  checkEq(backend->readbacks.size(), size_t{1}, "backend readback count");

  Light light{};
  light.type = LightType::Point;
  light.diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
  light.position = {1.0f, 2.0f, 3.0f};
  checkEq(device->setLight(0, light), D3D_OK, "set light");
  checkEq(device->lightEnable(0, true), D3D_OK, "enable light");
  checkEq(device->setRenderState(RS_LIGHTING, 1), D3D_OK, "lighting state");
  checkEq(device->setRenderState(RS_ALPHA_TEST_ENABLE, 1), D3D_OK, "alpha test state");
  checkEq(device->setRenderState(RS_ALPHA_FUNC, static_cast<u32>(CompareFunc::GreaterEqual)), D3D_OK,
          "alpha function state");
  checkEq(device->setRenderState(RS_CLIP_PLANE_ENABLE, 1), D3D_OK, "clip plane enable");
  const ClipPlane clipPlane{1.0f, 0.0f, 0.0f, -1.0f};
  checkEq(device->setClipPlane(0, clipPlane), D3D_OK, "clip plane state");
  checkEq(device->setTextureStageState(0, TSS_COLOR_OP, static_cast<u32>(TextureOp::SelectArg1)), D3D_OK,
          "texture stage color op");
  checkEq(device->setTextureStageState(0, TSS_ALPHA_OP, static_cast<u32>(TextureOp::Modulate)), D3D_OK,
          "texture stage alpha op");
  checkEq(device->setSamplerState(0, SAMP_MAX_ANISOTROPY, 4), D3D_OK, "sampler state");
  checkEq(device->setSamplerState(0, SAMP_MIN_FILTER, 2), D3D_OK, "sampler min filter");
  checkEq(device->setSamplerState(0, SAMP_ADDRESS_U, kTextureAddressBorder), D3D_OK, "sampler address u");
  checkEq(device->setSamplerState(0, SAMP_ADDRESS_V, kTextureAddressBorder), D3D_OK, "sampler address v");
  checkEq(device->setSamplerState(0, SAMP_BORDER_COLOR, 0xffffffffu), D3D_OK, "sampler border color");
  checkEq(device->setTexture(0, texture), D3D_OK, "texture binding");
  checkEq(device->setStreamSource(0, dynamicBuffer, 8, 16), D3D_OK, "stream source");
  checkEq(device->setIndices(dynamicBuffer, IndexType::UInt32), D3D_OK, "index buffer");
  checkEq(device->setFVF(0x1122u), D3D_OK, "fvf");
  checkEq(device->setVertexDeclaration({VertexElement{0, 0, 0, 0, 0, 0}}), D3D_OK, "vertex decl");

  Matrix4x4 view{};
  view.m = {2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f};
  checkEq(device->setTransform(XFORM_VIEW, view), D3D_OK, "view transform");
  checkEq(device->setViewport({0, 0, 640, 480, 0.0f, 1.0f}), D3D_OK, "viewport");
  checkEq(device->setScissorRect({16, 24, 320, 240}), D3D_OK, "scissor");

  auto stateBlock = device->createStateBlock();
  check(stateBlock != nullptr, "state block");
  check(stateBlock->snapshot().textures[0] == texture, "state block texture snapshot");
  checkEq(stateBlock->snapshot().fvf, 0x1122u, "state block fvf snapshot");

  checkEq(device->setRenderState(RS_LIGHTING, 0), D3D_OK, "mutate lighting");
  checkEq(device->setTexture(0, nullptr), D3D_OK, "unbind texture");
  checkEq(device->setStreamSource(0, nullptr, 0, 0), D3D_OK, "unbind stream");
  checkEq(device->setIndices(nullptr), D3D_OK, "unbind indices");
  checkEq(device->setViewport({0, 0, 320, 240, 0.0f, 1.0f}), D3D_OK, "mutate viewport");
  checkEq(device->setScissorRect({0, 0, 160, 120}), D3D_OK, "mutate scissor");

  checkEq(device->applyStateBlock(*stateBlock), D3D_OK, "apply state block");
  checkEq(device->getRenderState(RS_LIGHTING), 1u, "restored lighting");
  checkEq(device->state().textures[0], texture, "restored texture binding");
  checkEq(device->state().streamBuffers[0], dynamicBuffer, "restored stream source");
  checkEq(device->state().indexBuffer, dynamicBuffer, "restored index buffer");
  checkEq(device->state().viewport.width, 640u, "restored viewport width");
  checkEq(device->state().scissorEnabled, false, "restored scissor enabled");
  checkEq(device->state().scissorRect.left, 16, "restored scissor left");
  checkEq(device->state().transforms.at(XFORM_VIEW).m[0], 2.0f, "restored transform");

  auto pixelStateBlock = device->createStateBlock(StateBlockType::PixelState);
  check(pixelStateBlock != nullptr, "pixel state block");
  checkEq(device->setRenderState(RS_ALPHA_TEST_ENABLE, 0), D3D_OK, "mutate pixel alpha test");
  checkEq(device->setRenderState(RS_LIGHTING, 0), D3D_OK, "mutate vertex lighting before pixel apply");
  checkEq(device->setTextureStageState(0, TSS_COLOR_OP, static_cast<u32>(TextureOp::Disable)), D3D_OK,
          "mutate pixel texture stage");
  checkEq(device->setSamplerState(0, SAMP_MAX_ANISOTROPY, 1), D3D_OK, "mutate pixel sampler");
  checkEq(device->setTexture(0, nullptr), D3D_OK, "mutate texture before pixel apply");
  checkEq(device->setFVF(0x3344u), D3D_OK, "mutate fvf before pixel apply");
  checkEq(device->applyStateBlock(*pixelStateBlock), D3D_OK, "apply pixel state block");
  checkEq(device->getRenderState(RS_ALPHA_TEST_ENABLE), 1u, "pixel block restored alpha test");
  checkEq(device->getRenderState(RS_LIGHTING), 0u, "pixel block left lighting alone");
  checkEq(device->getTextureStageState(0, TSS_COLOR_OP), static_cast<u32>(TextureOp::SelectArg1),
          "pixel block restored texture stage");
  checkEq(device->getSamplerState(0, SAMP_MAX_ANISOTROPY), 4u, "pixel block restored sampler");
  checkEq(device->state().textures[0], nullptr, "pixel block left texture binding alone");
  checkEq(device->state().fvf, 0x3344u, "pixel block left fvf alone");

  checkEq(device->applyStateBlock(*stateBlock), D3D_OK, "restore state before vertex block");
  auto vertexStateBlock = device->createStateBlock(StateBlockType::VertexState);
  check(vertexStateBlock != nullptr, "vertex state block");
  checkEq(device->setRenderState(RS_LIGHTING, 0), D3D_OK, "mutate vertex lighting");
  checkEq(device->setRenderState(RS_ALPHA_TEST_ENABLE, 0), D3D_OK, "mutate alpha before vertex apply");
  checkEq(device->setTextureStageState(0, TSS_COLOR_OP, static_cast<u32>(TextureOp::Disable)), D3D_OK,
          "mutate texture stage before vertex apply");
  checkEq(device->setFVF(0x5566u), D3D_OK, "mutate fvf before vertex apply");
  checkEq(device->setTexture(0, nullptr), D3D_OK, "mutate texture before vertex apply");
  checkEq(device->applyStateBlock(*vertexStateBlock), D3D_OK, "apply vertex state block");
  checkEq(device->getRenderState(RS_LIGHTING), 1u, "vertex block restored lighting");
  checkEq(device->getRenderState(RS_ALPHA_TEST_ENABLE), 0u, "vertex block left alpha test alone");
  checkEq(device->getTextureStageState(0, TSS_COLOR_OP), static_cast<u32>(TextureOp::Disable),
          "vertex block left texture stage alone");
  checkEq(device->state().fvf, 0x1122u, "vertex block restored fvf");
  checkEq(device->state().textures[0], nullptr, "vertex block left texture binding alone");

  checkEq(device->applyStateBlock(*stateBlock), D3D_OK, "restore state after typed blocks");

  std::array<u8, 4> vertexPayload{1, 2, 3, 4};
  checkEq(device->drawPrimitiveUP(PrimitiveType::TriangleList, 1,
                                  std::span<const u8>(vertexPayload.data(), vertexPayload.size())),
          D3D_OK, "draw primitive up");
  checkEq(backend->draws.size(), size_t{1}, "first draw count");

  const auto& draw0 = backend->draws[0];
  checkEq(draw0.param.primitiveType, PrimitiveType::TriangleList, "draw0 primitive type");
  check(!draw0.param.indexed, "draw0 non-indexed UP draw param");
  checkEq(draw0.hot.indexBuffer, Handle{}, "draw0 non-indexed UP draw ignores bound index buffer");
  checkEq(draw0.param.indexType, IndexType::UInt32, "draw0 index type");
  checkEq(draw0.state.shaderLayout.vertexDecl.fvf, 0x1122u, "draw0 fvf");
  checkEq(draw0.state.shaderLayout.vertexDecl.elements.size(), size_t{1}, "draw0 vertex decl size");
  checkEq(draw0.hot.streamBuffers[0], Handle{}, "draw0 stream buffer");
  checkEq(draw0.hot.streamOffsets[0], 0u, "draw0 stream offset");
  checkEq(draw0.hot.streamStrides[0], 16u, "draw0 stream stride");
  checkEq(draw0.hot.textures[0], texture->handle(), "draw0 texture handle");
  checkEq(flatStateOr(draw0.hot.textureStageStates[0], TSS_COLOR_OP, 0u),
          static_cast<u32>(TextureOp::SelectArg1),
          "draw0 color op");
  checkEq(flatStateOr(draw0.hot.textureStageStates[0], TSS_ALPHA_OP, 0u),
          static_cast<u32>(TextureOp::Modulate),
          "draw0 alpha op");
  checkEq(flatStateOr(draw0.hot.samplerStates[0], SAMP_MAX_ANISOTROPY, 0u), 4u,
          "draw0 max anisotropy");
  checkEq(flatStateOr(draw0.hot.samplerStates[0], SAMP_MIN_FILTER, 0u), 2u, "draw0 min filter");
  checkEq(flatStateOr(draw0.hot.samplerStates[0], SAMP_ADDRESS_U, 0u), kTextureAddressBorder,
          "draw0 address u");
  checkEq(flatStateOr(draw0.hot.samplerStates[0], SAMP_ADDRESS_V, 0u), kTextureAddressBorder,
          "draw0 address v");
  checkEq(flatStateOr(draw0.hot.samplerStates[0], SAMP_BORDER_COLOR, 0u), 0xffffffffu,
          "draw0 border color");
  checkEq(flatStateOr(draw0.hot.renderStates, RS_LIGHTING, 0u), 1u, "draw0 lighting state");
  checkEq(flatStateOr(draw0.hot.renderStates, RS_ALPHA_TEST_ENABLE, 0u), 1u,
          "draw0 alpha test state");
  checkEq(draw0.uniforms.clipPlaneMask, 1u, "draw0 clip plane mask");
  checkNear(draw0.uniforms.clipPlanes[0][0], 0.5f, 1.0e-6f, "draw0 clip plane x");
  checkNear(draw0.uniforms.clipPlanes[0][1], 0.0f, 1.0e-6f, "draw0 clip plane y");
  checkNear(draw0.uniforms.clipPlanes[0][2], 0.0f, 1.0e-6f, "draw0 clip plane z");
  checkNear(draw0.uniforms.clipPlanes[0][3], -1.0f, 1.0e-6f, "draw0 clip plane w");
  checkEq(draw0.hot.colorAttachments[0].handle, primaryChain->backBuffer()->handle(),
          "draw0 render target");
  checkEq(draw0.hot.colorAttachments[0].sampleCount, 4u, "draw0 render target sample count");
  checkEq(draw0.hot.depthStencil.handle, primaryChain->depthStencilSurface()->handle(),
          "draw0 depth stencil target");
  checkEq(draw0.hot.viewport.viewport.width, 640u, "draw0 viewport width");
  checkEq(draw0.hot.viewport.scissorEnabled, false, "draw0 scissor enabled");
  checkEq(draw0.hot.viewport.scissor.left, 16, "draw0 scissor left");
  check(draw0.state.shaderLayout.vertexShader.kind == ShaderRef::Kind::FixedFunctionVertex,
        "draw0 vertex shader kind");
  check(draw0.state.shaderLayout.pixelShader.kind == ShaderRef::Kind::FixedFunctionPixel,
        "draw0 pixel shader kind");
  check(draw0.state.shaderLayout.vertexShader.vertexKey.has_value(), "draw0 vertex shader key");
  check(draw0.state.shaderLayout.pixelShader.pixelKey.has_value(), "draw0 pixel shader key");
  check(draw0.state.shaderLayout.vertexShader.vertexKey->lightingEnabled, "draw0 vertex key lighting");
  check(draw0.state.shaderLayout.pixelShader.pixelKey->alphaTestEnable, "draw0 pixel key alpha test");
  checkEq(draw0.param.userVertexRange.size, static_cast<u32>(vertexPayload.size()),
          "draw0 user vertex payload size");
  check(draw0.param.userIndexRange.empty(), "draw0 has no user index payload");
  checkBytes(payloadSlice(draw0, draw0.param.userVertexRange, "draw0 user vertex payload range"),
             std::span<const u8>(vertexPayload.data(), vertexPayload.size()),
             "draw0 user vertex payload");

  std::array<u32, 4> fanIndices{0, 1, 2, 3};
  std::array<u8, sizeof(fanIndices)> fanIndexBytes{};
  std::memcpy(fanIndexBytes.data(), fanIndices.data(), fanIndexBytes.size());
  checkEq(device->drawIndexedPrimitiveUP(PrimitiveType::TriangleFan, 2,
                                          std::span<const u8>(vertexPayload.data(), vertexPayload.size()),
                                          std::span<const u8>(fanIndexBytes.data(), fanIndexBytes.size()),
                                          IndexType::UInt32, 4),
          D3D_OK, "draw indexed primitive up");
  checkEq(backend->draws.size(), size_t{2}, "second draw count");
  const auto& draw1 = backend->draws[1];
  checkEq(draw1.param.primitiveType, PrimitiveType::TriangleList, "fan decomposed to triangle list");
  checkEq(draw1.param.primitiveCount, 2u, "fan primitive count");
  checkEq(draw1.param.indexType, IndexType::UInt32, "fan draw index type");
  check(draw1.param.indexed, "fan draw indexed param");
  checkEq(draw1.param.userVertexRange.size, static_cast<u32>(vertexPayload.size()),
          "fan user vertex payload size");
  checkEq(draw1.param.userIndexRange.size, static_cast<u32>(sizeof(u32) * 6u),
          "fan user index payload size");
  const std::array<u32, 6> expectedFanIndices{0, 1, 2, 0, 2, 3};
  std::array<u8, sizeof(expectedFanIndices)> expectedFanIndexBytes{};
  std::memcpy(expectedFanIndexBytes.data(), expectedFanIndices.data(), expectedFanIndexBytes.size());
  checkBytes(payloadSlice(draw1, draw1.param.userIndexRange, "fan user index payload range"),
             std::span<const u8>(expectedFanIndexBytes.data(), expectedFanIndexBytes.size()),
             "fan user index payload");
  checkEq(draw1.hot.streamBuffers[0], Handle{}, "fan up stream buffer");
  checkEq(draw1.hot.streamOffsets[0], 0u, "fan up stream offset");
  checkEq(draw1.hot.streamStrides[0], 4u, "fan up stream stride");

  std::array<u8, 4> fanVertexPayload{1, 2, 3, 4};
  checkEq(device->drawPrimitiveUP(PrimitiveType::TriangleFan, 2,
                                  std::span<const u8>(fanVertexPayload.data(), fanVertexPayload.size()), 1),
          D3D_OK, "draw primitive up fan");
  checkEq(backend->draws.size(), size_t{3}, "third draw count");
  const auto& draw2 = backend->draws[2];
  const std::array<u8, 6> expectedFanVertices{1, 2, 3, 1, 3, 4};
  checkEq(draw2.param.primitiveType, PrimitiveType::TriangleList, "up fan decomposed to triangle list");
  checkEq(draw2.param.primitiveCount, 2u, "up fan primitive count");
  checkEq(draw2.param.userVertexRange.size, static_cast<u32>(expectedFanVertices.size()),
          "up fan vertex payload size");
  check(draw2.param.userIndexRange.empty(), "up fan has no user index payload");
  checkBytes(payloadSlice(draw2, draw2.param.userVertexRange, "up fan vertex payload range"),
             std::span<const u8>(expectedFanVertices.data(), expectedFanVertices.size()),
             "up fan vertex payload");
  checkEq(draw2.hot.streamStrides[0], 1u, "up fan stride");

  std::array<u16, 4> fanIndices16{7, 8, 9, 10};
  std::array<u8, sizeof(fanIndices16)> fanIndexBytes16{};
  std::memcpy(fanIndexBytes16.data(), fanIndices16.data(), fanIndexBytes16.size());
  checkEq(device->drawIndexedPrimitiveUP(PrimitiveType::TriangleFan, 2,
                                          std::span<const u8>(vertexPayload.data(), vertexPayload.size()),
                                          std::span<const u8>(fanIndexBytes16.data(), fanIndexBytes16.size()),
                                          IndexType::UInt16, 4),
          D3D_OK, "draw indexed primitive up fan 16");
  checkEq(backend->draws.size(), size_t{4}, "fourth draw count");
  const auto& draw3 = backend->draws[3];
  checkEq(draw3.param.primitiveType, PrimitiveType::TriangleList, "fan16 decomposed to triangle list");
  checkEq(draw3.param.primitiveCount, 2u, "fan16 primitive count");
  checkEq(draw3.param.indexType, IndexType::UInt16, "fan16 draw index type");
  checkEq(draw3.param.userIndexRange.size, static_cast<u32>(sizeof(u16) * 6u),
          "fan16 user index payload size");
  const std::array<u16, 6> expectedFanIndices16{7, 8, 9, 7, 9, 10};
  std::array<u8, sizeof(expectedFanIndices16)> expectedFanIndexBytes16{};
  std::memcpy(expectedFanIndexBytes16.data(), expectedFanIndices16.data(),
              expectedFanIndexBytes16.size());
  checkBytes(payloadSlice(draw3, draw3.param.userIndexRange, "fan16 user index payload range"),
             std::span<const u8>(expectedFanIndexBytes16.data(), expectedFanIndexBytes16.size()),
             "fan16 user index payload");

  auto occlusion = device->createQuery(QueryType::Occlusion);
  auto timestamp = device->createQuery(QueryType::Timestamp);
  check(occlusion != nullptr, "occlusion query");
  check(timestamp != nullptr, "timestamp query");
  checkEq(device->issueQuery(occlusion, true), D3D_OK, "occlusion begin");
  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 2), D3D_OK, "occlusion draw");
  checkEq(device->issueQuery(occlusion, false), D3D_OK, "occlusion end");
  checkEq(device->issueQuery(timestamp, false), D3D_OK, "timestamp issue");

  u32 occlusionCount = 0;
  checkEq(device->getQueryData(occlusion, &occlusionCount, sizeof(occlusionCount), 0), S_FALSE,
          "occlusion not ready before present");
  checkEq(device->present(), D3D_OK, "present");
  checkEq(backend->presents.size(), size_t{1}, "present count");
  checkEq(backend->presents[0].window, params.deviceWindow, "present window");
  checkEq(backend->presents[0].width, 640u, "present width");
  checkEq(backend->presents[0].height, 480u, "present height");
  checkEq(backend->presents[0].format, Format::A8R8G8B8, "present format");
  checkEq(backend->presents[0].backBufferCount, 2u, "present back buffer count");
  checkEq(backend->presents[0].multiSampleType, MultiSampleType::Four, "present sample type");
  checkEq(backend->presents[0].sourceSurface, backBuffer->handle(), "present source backbuffer");
  check(backend->presents[0].displaySyncEnabled, "present sync");

  checkEq(device->getQueryData(occlusion, &occlusionCount, sizeof(occlusionCount), QUERY_GETDATA_FLUSH), S_OK,
          "occlusion ready after present");
  checkEq(occlusionCount, 2u, "occlusion count");

  u64 timestampValue = 0;
  checkEq(device->getQueryData(timestamp, &timestampValue, sizeof(timestampValue), 0), S_OK,
          "timestamp query");
  check(timestampValue != 0, "timestamp should be non-zero");

  auto freq = device->createQuery(QueryType::TimestampFreq);
  auto disjoint = device->createQuery(QueryType::TimestampDisjoint);
  u64 freqValue = 0;
  u64 disjointValue = 1234;
  checkEq(device->getQueryData(freq, &freqValue, sizeof(freqValue), 0), S_OK, "timestamp freq");
  checkEq(device->getQueryData(disjoint, &disjointValue, sizeof(disjointValue), 0), S_OK,
          "timestamp disjoint");
  checkEq(freqValue, 1000000000ull, "timestamp frequency");
  checkEq(disjointValue, 0ull, "timestamp disjoint value");

  const auto oldBackBuffer = primaryChain->backBuffer();
  const auto oldDepthStencil = primaryChain->depthStencilSurface();
  checkEq(device->reset(PresentParameters{
                                .backBufferWidth = 800,
                                .backBufferHeight = 600,
                                .backBufferFormat = Format::A8R8G8B8,
                                .backBufferCount = 1,
                                .windowed = true,
                                .presentationInterval = PresentInterval::Immediate,
                                .deviceWindow = Handle{77},
                                .enableAutoDepthStencil = true,
                                .autoDepthStencilFormat = Format::D24S8,
                                .discardSwapEffect = true,
                                .multiSampleType = MultiSampleType::Two,
                            }),
          D3D_OK, "device reset");

  check(!oldBackBuffer->valid(), "default pool back buffer invalidated by reset");
  check(!oldDepthStencil->valid(), "default pool depth stencil invalidated by reset");
  check(primaryChain->backBuffer() != oldBackBuffer, "swap chain back buffer recreated");
  check(primaryChain->depthStencilSurface() != oldDepthStencil, "swap chain depth recreated");
  check(primaryChain->backBuffer()->valid(), "replacement back buffer valid");
  check(primaryChain->depthStencilSurface()->valid(), "replacement depth valid");
  checkEq(primaryChain->backBuffer()->desc().width, 800u, "resized back buffer width");
  checkEq(primaryChain->backBuffer()->desc().height, 600u, "resized back buffer height");
  checkEq(primaryChain->backBuffer()->multiSampleCount(), 2u, "resized back buffer sample count");
  checkEq(device->state().viewport.width, 800u, "reset viewport width");
  checkEq(device->state().viewport.height, 600u, "reset viewport height");
  checkEq(device->state().renderTargets[0].handle, primaryChain->backBuffer()->handle(),
          "reset render target rebound");
  checkEq(device->state().depthStencil.handle, primaryChain->depthStencilSurface()->handle(),
          "reset depth stencil rebound");
  check(!dynamicBuffer->valid(), "default pool buffer invalidated");
  check(managedBuffer->valid(), "managed buffer survives reset");
  check(!copySurface->valid(), "default pool surface invalidated");
  check(backend->destroyedBuffers.size() >= size_t{1}, "backend buffers destroyed on reset");
  check(backend->destroyedSurfaces.size() >= size_t{4}, "backend surfaces destroyed on reset");
  check(systemSurface->valid(), "systemmem surface survives reset");
  check(scratchSurface->valid(), "scratch surface survives reset");
  check(texture->valid(), "managed texture survives reset");
  check(dstTexture->valid(), "managed texture copy survives reset");

  auto managedAfter = managedBuffer->bytes();
  checkBytes(managedAfter, std::span<const u8>(managedBytes.data(), managedBytes.size()),
             "managed buffer contents survive reset");
  checkBytes(texture->levelBytes(1), std::span<const u8>(greenPixel.data(), greenPixel.size()),
             "managed texture contents survive reset");

  auto systemAfter = systemSurface->lockRect(nullptr, 0);
  check(systemAfter.data != nullptr, "system surface lock after reset");
  checkBytes(std::span<const u8>(static_cast<const u8*>(systemAfter.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()),
             "system surface contents survive reset");
  systemSurface->unlockRect();

  auto scratchAfter = scratchSurface->lockRect(nullptr, 0);
  check(scratchAfter.data != nullptr, "scratch surface lock after reset");
  checkBytes(std::span<const u8>(static_cast<const u8*>(scratchAfter.data), 4),
             std::span<const u8>(yellowPixel.data(), yellowPixel.size()),
             "scratch surface contents survive reset");
  scratchSurface->unlockRect();

  checkEq(device->present(), D3D_OK, "present after reset");
  checkEq(backend->presents.size(), size_t{2}, "present count after reset");
  checkEq(backend->presents[1].width, 800u, "present width after reset");
  checkEq(backend->presents[1].height, 600u, "present height after reset");
  checkEq(backend->presents[1].sourceSurface, device->swapChain()->backBuffer()->handle(),
          "present source after reset");
  check(!backend->presents[1].displaySyncEnabled, "immediate present after reset");
  checkEq(device->swapChain()->backBuffer()->desc().width, 800u, "swapchain width after reset");
  checkEq(device->swapChain()->backBuffer()->desc().height, 600u, "swapchain height after reset");
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

  auto invalid = cube->surfaceLevel(cube->subresourceCount());
  check(invalid == nullptr, "cube invalid subresource rejected");
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

void testComWrappers() {
  using namespace dxmt9::com;

  auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  check(d3d != nullptr, "Direct3DCreate9");
  checkEq(d3d->AddRef(), 2u, "factory addref");
  checkEq(d3d->Release(), 1u, "factory release after addref");
  checkEq(d3d->GetAdapterCount(), size_t{1}, "factory adapter count");
  checkEq(d3d->CheckDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, true), D3D_OK,
          "factory device type");
  checkEq(d3d->CheckDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, false), D3D_OK,
          "factory fullscreen device type");
  check(!d3d->EnumAdapterModes(0, Format::R8G8B8).size(), "unsupported adapter modes");
  checkEq(d3d->GetAdapterDisplayMode(0).width, 1920u, "factory adapter mode width");

  void* unknown = nullptr;
  check(d3d->QueryInterface(InterfaceId::IUnknown, &unknown), "factory query interface");
  auto* queriedFactory = static_cast<IDirect3D9*>(unknown);
  check(queriedFactory != nullptr, "factory query result");
  checkEq(queriedFactory->Release(), 1u, "factory qi release");
  void* exUnknown = nullptr;
  check(!d3d->QueryInterface(InterfaceId::Direct3D9Ex, &exUnknown), "base factory must not expose ex");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;

  auto* device = d3d->CreateDevice(0, params);
  check(device != nullptr, "wrapper device create");
  check(device->coreDevice().swapChain() != nullptr, "wrapper core device swap chain");
  checkEq(device->GetDeviceCaps().maxTextureWidth, 16384u, "wrapper caps");
  checkEq(device->GetDeviceCaps().declTypes, 0x000003ffu, "wrapper vertex declaration type caps");
  checkEq(device->TestCooperativeLevel(), D3D_OK, "wrapper cooperative level");
  checkEq(device->GetSwapChainCount(), size_t{1}, "wrapper swap chain count");
  auto* primarySwapChain = device->GetSwapChain();
  check(primarySwapChain != nullptr, "wrapper primary swap chain");
  check(primarySwapChain->backBuffer() != nullptr, "wrapper primary back buffer");
  checkEq(primarySwapChain->Present(), D3D_OK, "wrapper primary swap present");
  checkEq(primarySwapChain->Release(), 0u, "wrapper primary swap release");
  auto* extraSwapChain = device->CreateAdditionalSwapChain(params);
  check(extraSwapChain != nullptr, "wrapper additional swap chain");
  check(extraSwapChain->backBuffer() != nullptr, "wrapper additional back buffer");
  checkEq(extraSwapChain->presentParameters().backBufferWidth, 320u, "wrapper additional params");
  checkEq(extraSwapChain->Present(), D3D_OK, "wrapper additional swap present");
  checkEq(device->GetSwapChainCount(), size_t{2}, "wrapper swap chain count after create");
  checkEq(extraSwapChain->Release(), 0u, "wrapper additional swap release");
  checkEq(device->AddRef(), 2u, "device addref");
  checkEq(device->Release(), 1u, "device release after addref");

  void* deviceUnknown = nullptr;
  check(device->QueryInterface(InterfaceId::Direct3DDevice9, &deviceUnknown), "device query interface");
  auto* queriedDevice = static_cast<IDirect3DDevice9*>(deviceUnknown);
  check(queriedDevice != nullptr, "device query result");
  checkEq(queriedDevice->Release(), 1u, "device qi release");
  check(!device->QueryInterface(InterfaceId::Direct3DDevice9Ex, &deviceUnknown),
        "base device must not expose ex");
  checkEq(device->Release(), 0u, "device release");
  checkEq(d3d->Release(), 0u, "factory release");
}

void testComWrappersEx() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 8192;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "Direct3DCreate9Ex");
  checkEq(d3d->AddRef(), 2u, "factory ex addref");
  checkEq(d3d->Release(), 1u, "factory ex release after addref");

  void* exUnknown = nullptr;
  check(d3d->QueryInterface(InterfaceId::Direct3D9Ex, &exUnknown), "factory ex query interface");
  auto* queriedFactory = static_cast<IDirect3D9Ex*>(exUnknown);
  check(queriedFactory != nullptr, "factory ex query result");
  checkEq(queriedFactory->Release(), 1u, "factory ex qi release");

  checkEq(d3d->GetAdapterModeCountEx(0, nullptr), d3d->EnumAdapterModes(0, Format::A8R8G8B8).size(),
          "ex adapter mode count");
  DisplayModeFilter filter{};
  filter.format = Format::A8R8G8B8;
  DisplayModeEx mode{};
  check(d3d->EnumAdapterModesEx(0, &filter, 0, &mode), "ex enum adapter modes");
  checkEq(mode.scanLineOrdering, DisplayScanLineOrdering::Progressive, "ex scanline ordering");
  DisplayModeEx currentMode{};
  DisplayRotation rotation = DisplayRotation::Rotate90;
  check(d3d->GetAdapterDisplayModeEx(0, &currentMode, &rotation), "ex adapter display mode");
  checkEq(rotation, DisplayRotation::Identity, "ex rotation");
  Luid luid0{};
  Luid luid1{};
  check(d3d->GetAdapterLUID(0, &luid0), "ex adapter luid");
  check(d3d->GetAdapterLUID(0, &luid1), "ex adapter luid stable");
  checkEq(luid0.lowPart, luid1.lowPart, "luid low stable");
  checkEq(luid0.highPart, luid1.highPart, "luid high stable");
  check(luid0.lowPart != 0 || luid0.highPart != 0, "luid non-zero");

  PresentParameters params{};
  params.windowed = false;
  params.backBufferWidth = 1024;
  params.backBufferHeight = 768;
  params.backBufferFormat = Format::Unknown;
  params.presentationInterval = PresentInterval::Default;
  params.deviceWindow = Handle{202};
  DisplayModeEx fullscreenMode{};
  fullscreenMode.width = 1024;
  fullscreenMode.height = 768;
  fullscreenMode.format = Format::A8R8G8B8;

  auto* device = d3d->CreateDeviceEx(0, params, &fullscreenMode);
  check(device != nullptr, "wrapper ex device create");
  check(device->coreDevice().swapChain() != nullptr, "wrapper ex core device swap chain");
  void* deviceUnknown = nullptr;
  check(device->QueryInterface(InterfaceId::Direct3DDevice9Ex, &deviceUnknown), "device ex query interface");
  auto* queriedDevice = static_cast<IDirect3DDevice9Ex*>(deviceUnknown);
  check(queriedDevice != nullptr, "device ex query result");
  checkEq(queriedDevice->Release(), 1u, "device ex qi release");

  {
    device->AddRef();
    D9CDevice cDevice(device);
    check(dxmt9c_device_create_state_block(&cDevice, 0) == nullptr, "reject invalid state block type");
    auto* cStateBlock = dxmt9c_device_create_state_block(&cDevice, 1);
    check(cStateBlock != nullptr, "create c state block");
    checkEq(dxmt9c_device_begin_state_block(&cDevice), D3D_OK, "begin c state block");
    check(dxmt9c_device_create_state_block(&cDevice, 1) == nullptr, "reject create state block while recording");
    checkEq(dxmt9c_device_begin_state_block(&cDevice), D3DERR_INVALIDCALL, "reject nested state block recording");
    checkEq(dxmt9c_stateblock_capture(cStateBlock), D3DERR_INVALIDCALL, "reject state block capture while recording");
    checkEq(dxmt9c_stateblock_apply(cStateBlock), D3DERR_INVALIDCALL, "reject state block apply while recording");
    D9CStateBlock* recordedStateBlock = nullptr;
    checkEq(dxmt9c_device_end_state_block(&cDevice, &recordedStateBlock), D3D_OK, "end c state block");
    check(recordedStateBlock != nullptr, "recorded c state block");
    checkEq(dxmt9c_stateblock_release(recordedStateBlock), 0u, "release recorded c state block");
    checkEq(dxmt9c_stateblock_release(cStateBlock), 0u, "release c state block");

    auto* dxt5Texture = dxmt9c_device_create_texture(
        &cDevice, 16, 16, 1, 0, dxmt9::d3d9::devicec::fmtToD3D(Format::DXT5), 1);
    check(dxt5Texture != nullptr, "create c dxt5 texture");
    D9CLockedRect locked{};
    D9CRect partialRect{4, 4, 12, 12};
    checkEq(dxmt9c_texture_lock_rect(dxt5Texture, 0, &locked, &partialRect, 0), D3D_OK,
            "lock c dxt5 partial rect");
    check(locked.bits != nullptr, "c dxt5 partial lock bits");
    checkEq(locked.pitch, static_cast<int32_t>(formatRowPitch(Format::DXT5, 16)),
            "c dxt5 partial lock exposes level pitch");
    auto* lockedBytes = static_cast<u8*>(locked.bits);
    lockedBytes[0] = 0x7au;
    lockedBytes[static_cast<size_t>(locked.pitch)] = 0x7bu;
    checkEq(dxmt9c_texture_unlock_rect(dxt5Texture, 0), D3D_OK, "unlock c dxt5 partial rect");
    const auto dxt5Bytes = dxt5Texture->obj->levelBytes(0);
    checkEq(dxt5Bytes[80], static_cast<u8>(0x7a), "c dxt5 partial first block copied");
    checkEq(dxt5Bytes[144], static_cast<u8>(0x7b), "c dxt5 partial second block copied");

    auto* dxt5Surface = dxmt9c_texture_get_surface_level(dxt5Texture, 0);
    check(dxt5Surface != nullptr, "create c dxt5 surface level");
    D9CLockedRect surfaceLocked{};
    checkEq(dxmt9c_surface_lock_rect(dxt5Surface, &surfaceLocked, &partialRect, 0), D3D_OK,
            "lock c dxt5 surface partial rect");
    check(surfaceLocked.bits != nullptr, "c dxt5 surface partial lock bits");
    checkEq(surfaceLocked.pitch, static_cast<int32_t>(formatRowPitch(Format::DXT5, 16)),
            "c dxt5 surface partial lock exposes level pitch");
    auto* surfaceLockedBytes = static_cast<u8*>(surfaceLocked.bits);
    surfaceLockedBytes[2] = 0x4au;
    surfaceLockedBytes[static_cast<size_t>(surfaceLocked.pitch) + 2u] = 0x4bu;
    checkEq(dxmt9c_surface_unlock_rect(dxt5Surface), D3D_OK, "unlock c dxt5 surface partial rect");
    const auto dxt5SurfaceBytes = dxt5Texture->obj->levelBytes(0);
    checkEq(dxt5SurfaceBytes[82], static_cast<u8>(0x4a), "c dxt5 surface partial first block copied");
    checkEq(dxt5SurfaceBytes[146], static_cast<u8>(0x4b), "c dxt5 surface partial second block copied");
    checkEq(dxmt9c_surface_release(dxt5Surface), 0u, "release c dxt5 surface level");
    checkEq(dxmt9c_texture_release(dxt5Texture), 0u, "release c dxt5 texture");

    constexpr uint32_t d3dLockDiscard = 0x00002000u;
    auto* discardTexture = dxmt9c_device_create_texture(
        &cDevice, 4, 4, 1, 0, dxmt9::d3d9::devicec::fmtToD3D(Format::A8R8G8B8), 1);
    check(discardTexture != nullptr, "create c discard texture");
    D9CLockedRect discardLocked{};
    checkEq(dxmt9c_texture_lock_rect(discardTexture, 0, &discardLocked, nullptr, 0), D3D_OK,
            "lock c discard texture initial");
    auto* discardBytes = static_cast<u8*>(discardLocked.bits);
    discardBytes[0] = 0xabu;
    checkEq(dxmt9c_texture_unlock_rect(discardTexture, 0), D3D_OK, "unlock c discard texture initial");
    checkEq(dxmt9c_texture_lock_rect(discardTexture, 0, &discardLocked, nullptr, d3dLockDiscard), D3D_OK,
            "lock c texture with d3d discard flag");
    discardBytes = static_cast<u8*>(discardLocked.bits);
    checkEq(discardBytes[0], static_cast<u8>(0x00), "d3d discard lock maps to core discard");
    checkEq(dxmt9c_texture_unlock_rect(discardTexture, 0), D3D_OK, "unlock c discard texture");
    checkEq(dxmt9c_texture_release(discardTexture), 0u, "release c discard texture");
  }

  checkEq(device->GetMaximumFrameLatency(), 4u, "default max frame latency");
  checkEq(device->SetMaximumFrameLatency(0), D3D_OK, "set frame latency default");
  checkEq(device->GetMaximumFrameLatency(), 4u, "zero latency maps to default");
  checkEq(backend->maxFrameLatencyCalls.back(), 4u, "backend received default frame latency");
  checkEq(device->SetMaximumFrameLatency(30), D3D_OK, "set max valid frame latency");
  checkEq(device->GetMaximumFrameLatency(), 30u, "stored max valid frame latency");
  checkEq(backend->maxFrameLatencyCalls.back(), 30u, "backend received max valid frame latency");
  checkEq(device->SetMaximumFrameLatency(31), D3DERR_INVALIDCALL, "reject invalid frame latency");
  checkEq(device->GetMaximumFrameLatency(), 30u, "invalid frame latency leaves previous value");
  checkEq(backend->maxFrameLatencyCalls.back(), 30u, "backend not updated for invalid frame latency");

  checkEq(device->CheckDeviceState(params.deviceWindow), D3D_OK, "initial device state");
  backend->triggerPresentationOccluded(true);
  checkEq(device->CheckDeviceState(params.deviceWindow), S_PRESENT_OCCLUDED, "occluded device state");
  backend->triggerDeviceLost(true);
  checkEq(device->CheckDeviceState(params.deviceWindow), D3DERR_DEVICELOST, "lost beats occluded");
  checkEq(device->ResetEx(params, &fullscreenMode), D3D_OK, "device ex reset");
  checkEq(device->CheckDeviceState(params.deviceWindow), D3D_OK, "device recovered after reset");
  checkEq(device->coreDevice().presentParameters().backBufferWidth, 1024u, "reset ex width");
  checkEq(device->coreDevice().presentParameters().backBufferHeight, 768u, "reset ex height");
  const auto displayModeEx = device->GetDisplayModeEx();
  checkEq(displayModeEx.width, 1024u, "device ex display mode width");
  checkEq(displayModeEx.height, 768u, "device ex display mode height");
  checkEq(displayModeEx.scanLineOrdering, DisplayScanLineOrdering::Progressive,
          "device ex display mode scanline");

  checkEq(device->WaitForVBlank(0), D3D_OK, "wait for vblank");
  checkEq(backend->waitForVBlankCalls.size(), size_t{1}, "backend wait for vblank call");
  checkEq(device->CheckResourceResidency(), S_OK, "check resource residency");
  i32 priority = 123;
  checkEq(device->GetGPUThreadPriority(&priority), D3D_OK, "get gpu priority");
  checkEq(priority, 0, "gpu priority zero");
  checkEq(device->SetGPUThreadPriority(7), D3D_OK, "set gpu priority");
  checkEq(device->SetConvolutionMonoKernel(), E_NOTIMPL, "mono kernel not impl");
  checkEq(device->ComposeRects(), E_NOTIMPL, "compose rects not impl");

  Handle sharedHandle{123};
  auto rt = device->CreateRenderTargetEx({128, 64, Format::A8R8G8B8, Pool::Default, UsageRenderTarget, true,
                                          false, MultiSampleType::Four},
                                         &sharedHandle);
  check(rt != nullptr, "render target ex");
  checkEq(sharedHandle.value, 0u, "render target shared handle cleared");
  check(rt->desc().renderTarget, "render target flagged");

  sharedHandle = Handle{123};
  auto offscreen = device->CreateOffscreenPlainSurfaceEx({32, 32, Format::A8R8G8B8, Pool::SystemMem, 0, false,
                                                          false, MultiSampleType::None},
                                                         &sharedHandle);
  check(offscreen != nullptr, "offscreen ex");
  checkEq(sharedHandle.value, 0u, "offscreen shared handle cleared");

  sharedHandle = Handle{123};
  auto depth = device->CreateDepthStencilSurfaceEx({64, 64, Format::D24S8, Pool::Default, UsageDepthStencil, false,
                                                    true, MultiSampleType::Two},
                                                   &sharedHandle);
  check(depth != nullptr, "depth ex");
  checkEq(sharedHandle.value, 0u, "depth shared handle cleared");
  check(depth->desc().depthStencil, "depth flagged");

  checkEq(device->Release(), 0u, "device ex release");
  checkEq(d3d->Release(), 0u, "factory ex release");
}

void testFullscreenAndDeviceLost() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters params{};
  params.windowed = false;
  params.backBufferWidth = 0;
  params.backBufferHeight = 0;
  params.backBufferFormat = Format::Unknown;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{101};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "fullscreen device create");
  checkEq(device->presentParameters().windowed, false, "fullscreen mode stored");
  checkEq(device->presentParameters().backBufferWidth, 1920u, "fullscreen default width stored");
  checkEq(device->presentParameters().backBufferHeight, 1080u, "fullscreen default height stored");
  checkEq(device->swapChain()->backBuffer()->desc().width, 1920u, "fullscreen back buffer width");
  checkEq(device->swapChain()->backBuffer()->desc().height, 1080u, "fullscreen back buffer height");
  checkEq(device->swapChain()->backBuffer()->desc().format, Format::A8R8G8B8, "fullscreen back buffer format");
  checkEq(device->testCooperativeLevel(), D3D_OK, "fullscreen cooperative level");

  backend->triggerDeviceLost(true);
  checkEq(device->testCooperativeLevel(), D3DERR_DEVICELOST, "triggered lost state");
  checkEq(device->present(), D3DERR_DEVICELOST, "present while lost");
  checkEq(device->reset(params), D3D_OK, "reset after device lost");
  checkEq(device->testCooperativeLevel(), D3D_OK, "recovered cooperative level");
  checkEq(device->swapChain()->backBuffer()->desc().width, 1920u, "fullscreen reset width");
  checkEq(device->swapChain()->backBuffer()->desc().height, 1080u, "fullscreen reset height");
}

}  // namespace

int main() {
  try {
    testRasterStateCoverage();
    testIndexedDrawPolicyContracts();
    testDeviceCoreFlow();
    testCubeTextureSubresourceFlow();
    testMetalSamplerBorderColorCoverage();
    testProgrammableTextureOrientationSmoke();
    testComWrappers();
    testComWrappersEx();
    testFullscreenAndDeviceLost();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
