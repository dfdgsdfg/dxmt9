#include "core_spec_fixtures.hpp"

#include <memory>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

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

void testSwapChainPresentOverridesCallerSourceWithOwningBackBuffer() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters primaryParams{};
  primaryParams.backBufferWidth = 320;
  primaryParams.backBufferHeight = 240;
  primaryParams.backBufferFormat = Format::A8R8G8B8;
  primaryParams.windowed = true;
  primaryParams.presentationInterval = PresentInterval::Immediate;
  primaryParams.deviceWindow = Handle{0x1000u};

  auto device = factory.createDevice(0, primaryParams);
  check(device != nullptr, "swapchain present primary device");

  PresentParameters secondaryParams = primaryParams;
  secondaryParams.backBufferWidth = 128;
  secondaryParams.backBufferHeight = 96;
  secondaryParams.deviceWindow = Handle{0x2000u};
  auto secondary = device->createAdditionalSwapChain(secondaryParams);
  check(secondary != nullptr, "secondary swapchain created");
  check(secondary->backBuffer() != nullptr, "secondary swapchain backbuffer");

  SwapDesc callerDesc{};
  callerDesc.window = Handle{0x2000u};
  callerDesc.width = 128;
  callerDesc.height = 96;
  callerDesc.sourceSurface = Handle{0xdeadbeefu};
  checkEq(secondary->present(device->backend(), callerDesc), D3D_OK,
          "secondary swapchain present");

  checkEq(backend->presents.size(), size_t{1},
          "secondary swapchain present submitted once");
  checkEq(backend->presents[0].sourceSurface, secondary->backBuffer()->handle(),
          "swapchain present uses owning backbuffer as source");
  checkEq(backend->presents[0].window, callerDesc.window,
          "swapchain present preserves destination window");
}

}  // namespace

int main() {
  try {
    testDeviceCoreFlow();
    testFullscreenAndDeviceLost();
    testSwapChainPresentOverridesCallerSourceWithOwningBackBuffer();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
