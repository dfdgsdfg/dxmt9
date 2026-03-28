#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"

using namespace dxmt9::core;

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message;
    out << " (left != right)";
    fail(out.str());
  }
}

void checkNear(float left, float right, float epsilon, std::string_view message) {
  if (std::fabs(left - right) > epsilon) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

void checkBytes(std::span<const u8> actual, std::span<const u8> expected, std::string_view message) {
  if (actual.size() < expected.size() || !std::equal(expected.begin(), expected.end(), actual.begin())) {
    std::ostringstream out;
    out << message;
    fail(out.str());
  }
}

std::array<u8, 4> bgra(u8 b, u8 g, u8 r, u8 a) {
  return {b, g, r, a};
}

struct RecordingBackend final : BackendDevice {
  BufferHandle createBuffer(const BufferDesc& desc) override {
    createdBuffers.push_back(desc);
    return Handle{nextHandle++};
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    createdTextures.push_back(desc);
    return Handle{nextHandle++};
  }

  SurfaceHandle createSurface(const SurfaceDesc& desc) override {
    createdSurfaces.push_back(desc);
    return Handle{nextHandle++};
  }

  void destroyBuffer(BufferHandle handle) override {
    destroyedBuffers.push_back(handle);
  }

  void destroyTexture(TextureHandle handle) override {
    destroyedTextures.push_back(handle);
  }

  void destroySurface(SurfaceHandle handle) override {
    destroyedSurfaces.push_back(handle);
  }

  void* mapBuffer(BufferHandle handle, u32 flags) override {
    mappedBuffers.push_back({handle, flags});
    return nullptr;
  }

  void unmapBuffer(BufferHandle handle) override {
    unmappedBuffers.push_back(handle);
  }

  void submitDraw(const DrawDesc& desc) override {
    draws.push_back(desc);
  }

  void submitClear(const ClearDesc& desc) override {
    clears.push_back(desc);
  }

  void submitSurfaceCopy(const SurfaceCopyDesc& desc) override {
    surfaceCopies.push_back(desc);
  }

  void submitStretchRect(const StretchRectDesc& desc) override {
    stretchRects.push_back(desc);
  }

  void submitReadback(const ReadbackDesc& desc) override {
    readbacks.push_back(desc);
  }

  void submitColorFill(const ColorFillDesc& desc) override {
    colorFills.push_back(desc);
  }

  void present(const SwapDesc& desc) override {
    presents.push_back(desc);
  }

  u64 nextHandle = 1;
  std::vector<BufferDesc> createdBuffers;
  std::vector<TextureDesc> createdTextures;
  std::vector<SurfaceDesc> createdSurfaces;
  std::vector<BufferHandle> destroyedBuffers;
  std::vector<TextureHandle> destroyedTextures;
  std::vector<SurfaceHandle> destroyedSurfaces;
  std::vector<std::pair<BufferHandle, u32>> mappedBuffers;
  std::vector<BufferHandle> unmappedBuffers;
  std::vector<DrawDesc> draws;
  std::vector<ClearDesc> clears;
  std::vector<SurfaceCopyDesc> surfaceCopies;
  std::vector<StretchRectDesc> stretchRects;
  std::vector<ReadbackDesc> readbacks;
  std::vector<ColorFillDesc> colorFills;
  std::vector<SwapDesc> presents;
};

void testFormatAndCaps() {
  const auto* a8r8g8b8 = findFormatInfo(Format::A8R8G8B8);
  check(a8r8g8b8 != nullptr, "A8R8G8B8 format info missing");
  checkEq(a8r8g8b8->backendFormat, BackendPixelFormat::BGRA8Unorm, "A8R8G8B8 backend format");
  checkEq(a8r8g8b8->support, FormatClass::Required, "A8R8G8B8 support class");
  checkEq(a8r8g8b8->bytesPerPixel, 4u, "A8R8G8B8 bytes per pixel");
  check(a8r8g8b8->renderTarget, "A8R8G8B8 should be renderable");

  const auto* l8 = findFormatInfo(Format::L8);
  check(l8 != nullptr, "L8 format info missing");
  check(!l8->renderTarget, "L8 must not be reported as a render target");

  const auto* r8g8b8 = findFormatInfo(Format::R8G8B8);
  check(r8g8b8 != nullptr, "R8G8B8 format info missing");
  checkEq(r8g8b8->support, FormatClass::Unsupported, "R8G8B8 support class");

  BackendLimits limits{};
  limits.maxTextureSize = 4096;
  limits.maxColorAttachments = 2;
  limits.maxAnisotropy = 8;
  limits.supportsBgr10A2 = false;
  limits.supportsDepth32FloatStencil8 = false;

  Factory factory(limits);
  checkEq(factory.adapterCount(), size_t{1}, "adapter count");
  checkEq(factory.caps(0).maxTextureWidth, 4096u, "max texture width");
  checkEq(factory.caps(0).maxTextureHeight, 4096u, "max texture height");
  checkEq(factory.caps(0).numSimultaneousRTs, 2u, "simultaneous RT count");
  checkEq(factory.caps(0).maxAnisotropy, 8u, "max anisotropy");

  checkEq(factory.checkDeviceFormat(0, Format::A8R8G8B8, UsageTexture), D3D_OK, "A8R8G8B8 texture support");
  checkEq(factory.checkDeviceFormat(0, Format::L8, UsageRenderTarget), D3DERR_NOTAVAILABLE,
          "L8 render-target support");
  checkEq(factory.checkDeviceFormat(0, Format::A2B10G10R10, UsageTexture), D3DERR_NOTAVAILABLE,
          "A2B10G10R10 support gate");
  checkEq(factory.checkDeviceFormat(1, Format::A8R8G8B8, UsageTexture), D3DERR_INVALIDCALL,
          "invalid adapter index");
  checkEq(factory.checkDeviceMultiSampleType(0, Format::A8R8G8B8, MultiSampleType::Four), D3D_OK,
          "4x MSAA support");
  checkEq(factory.checkDeviceMultiSampleType(0, Format::A8R8G8B8, MultiSampleType::Eight), D3DERR_NOTAVAILABLE,
          "8x MSAA support");
}

void testHelpers() {
  const Viewport viewport{0, 0, 800, 600, 0.0f, 1.0f};
  const auto fixup = halfPixelFixup(viewport);
  checkNear(fixup[0], 1.0f / 800.0f, 1.0e-6f, "half-pixel X fixup");
  checkNear(fixup[1], 1.0f / 600.0f, 1.0e-6f, "half-pixel Y fixup");

  const auto zeroFixup = halfPixelFixup(Viewport{});
  checkEq(zeroFixup[0], 0.0f, "zero viewport X fixup");
  checkEq(zeroFixup[1], 0.0f, "zero viewport Y fixup");

  const std::vector<u32> fanIndices{0, 1, 2, 3};
  const auto triangles = decomposeTriangleFanIndices(fanIndices);
  const std::vector<u32> expected{0, 1, 2, 0, 2, 3};
  checkEq(triangles, expected, "triangle fan decomposition");

  const std::vector<u8> upload{0x22, 0x33, 0x44, 0x55};
  const auto converted = convertTextureUpload(Format::A8R8G8B8, 1, 1, upload);
  checkBytes(std::span<const u8>(converted.data(), converted.size()),
             std::span<const u8>(upload.data(), upload.size()), "texture upload conversion");

  check(hashString("dxmt9") != 0, "hashString should not be zero");
}

void testFfpKeys() {
  DeviceState state;
  state.reset();

  state.renderStates[RS_LIGHTING] = 1;
  state.renderStates[RS_SPECULAR_ENABLE] = 1;
  state.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  state.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);
  state.renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  state.renderStates[RS_EMISSIVE_MATERIAL_SOURCE] = 3;
  state.renderStates[RS_AMBIENT_MATERIAL_SOURCE] = 2;
  state.renderStates[RS_DIFFUSE_MATERIAL_SOURCE] = 1;
  state.renderStates[RS_SPECULAR_MATERIAL_SOURCE] = 0;
  state.renderStates[RS_VERTEX_BLEND] = 2;

  state.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  state.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4;
  state.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;

  state.lightEnabled[0] = true;
  state.lights[0].type = LightType::Point;

  const auto vertexKey = makeFfpVertexKey(state);
  check(vertexKey.lightingEnabled, "vertex key lighting");
  check(vertexKey.specularEnabled, "vertex key specular");
  check(vertexKey.lightEnabled[0], "vertex key light enable");
  checkEq(vertexKey.lightType[0], static_cast<u32>(LightType::Point), "vertex key light type");
  checkEq(vertexKey.texCoordGen[0], 4u, "vertex key texcoord");
  checkEq(vertexKey.texTransformFlags[0], 7u, "vertex key transform flags");
  check(vertexKey.hash != 0, "vertex key hash");

  const auto pixelKey = makeFfpPixelKey(state);
  check(pixelKey.alphaTestEnable, "pixel key alpha test");
  checkEq(pixelKey.alphaTestFunc, static_cast<u32>(CompareFunc::GreaterEqual), "pixel key alpha func");
  checkEq(pixelKey.stages[0].colorOp, static_cast<u32>(TextureOp::SelectArg1), "pixel key color op");
  checkEq(pixelKey.stages[0].alphaOp, static_cast<u32>(TextureOp::Modulate), "pixel key alpha op");
  checkEq(pixelKey.stages[0].texCoordIndex, 4u, "pixel key texcoord");
  check(pixelKey.hash != 0, "pixel key hash");
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
  checkEq(stateBlock->snapshot().vertexDecl.fvf, 0x1122u, "state block fvf snapshot");

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
  checkEq(device->state().scissorEnabled, true, "restored scissor enabled");
  checkEq(device->state().scissorRect.left, 16, "restored scissor left");
  checkEq(device->state().transforms.at(XFORM_VIEW).m[0], 2.0f, "restored transform");

  std::array<u8, 4> vertexPayload{1, 2, 3, 4};
  checkEq(device->drawPrimitiveUP(PrimitiveType::TriangleList, 1,
                                  std::span<const u8>(vertexPayload.data(), vertexPayload.size())),
          D3D_OK, "draw primitive up");
  checkEq(backend->draws.size(), size_t{1}, "first draw count");

  const auto& draw0 = backend->draws[0];
  checkEq(draw0.primitiveType, PrimitiveType::TriangleList, "draw0 primitive type");
  checkEq(draw0.indexBuffer, dynamicBuffer->handle(), "draw0 index buffer");
  checkEq(draw0.indexType, IndexType::UInt32, "draw0 index type");
  checkEq(draw0.vertexDecl.fvf, 0x1122u, "draw0 fvf");
  checkEq(draw0.vertexDecl.elements.size(), size_t{1}, "draw0 vertex decl size");
  checkEq(draw0.vertexDecl.streams[0].buffer, dynamicBuffer, "draw0 stream buffer");
  checkEq(draw0.vertexDecl.streams[0].offset, 8u, "draw0 stream offset");
  checkEq(draw0.vertexDecl.streams[0].stride, 16u, "draw0 stream stride");
  checkEq(draw0.textures[0].handle, texture->handle(), "draw0 texture handle");
  checkEq(draw0.textures[0].stageStates.at(TSS_COLOR_OP), static_cast<u32>(TextureOp::SelectArg1),
          "draw0 color op");
  checkEq(draw0.textures[0].stageStates.at(TSS_ALPHA_OP), static_cast<u32>(TextureOp::Modulate),
          "draw0 alpha op");
  checkEq(draw0.samplers[0].states.at(SAMP_MAX_ANISOTROPY), 4u, "draw0 max anisotropy");
  checkEq(draw0.samplers[0].states.at(SAMP_MIN_FILTER), 2u, "draw0 min filter");
  checkEq(draw0.rs.values.at(RS_LIGHTING), 1u, "draw0 lighting state");
  checkEq(draw0.rs.values.at(RS_ALPHA_TEST_ENABLE), 1u, "draw0 alpha test state");
  checkEq(draw0.clipPlaneMask, 1u, "draw0 clip plane mask");
  checkNear(draw0.clipPlanes[0][0], 0.5f, 1.0e-6f, "draw0 clip plane x");
  checkNear(draw0.clipPlanes[0][1], 0.0f, 1.0e-6f, "draw0 clip plane y");
  checkNear(draw0.clipPlanes[0][2], 0.0f, 1.0e-6f, "draw0 clip plane z");
  checkNear(draw0.clipPlanes[0][3], -1.0f, 1.0e-6f, "draw0 clip plane w");
  checkEq(draw0.rts.color[0].handle, primaryChain->backBuffer()->handle(), "draw0 render target");
  checkEq(draw0.rts.color[0].sampleCount, 4u, "draw0 render target sample count");
  checkEq(draw0.rts.depthStencil.handle, primaryChain->depthStencilSurface()->handle(),
          "draw0 depth stencil target");
  checkEq(draw0.viewport.viewport.width, 640u, "draw0 viewport width");
  checkEq(draw0.viewport.scissorEnabled, true, "draw0 scissor enabled");
  checkEq(draw0.viewport.scissor.left, 16, "draw0 scissor left");
  check(draw0.vertexShader.kind == ShaderRef::Kind::FixedFunctionVertex, "draw0 vertex shader kind");
  check(draw0.pixelShader.kind == ShaderRef::Kind::FixedFunctionPixel, "draw0 pixel shader kind");
  check(draw0.vertexShader.vertexKey.has_value(), "draw0 vertex shader key");
  check(draw0.pixelShader.pixelKey.has_value(), "draw0 pixel shader key");
  check(draw0.vertexShader.vertexKey->lightingEnabled, "draw0 vertex key lighting");
  check(draw0.pixelShader.pixelKey->alphaTestEnable, "draw0 pixel key alpha test");
  checkEq(draw0.userVertexData.size(), vertexPayload.size(), "draw0 user vertex payload");

  std::array<u32, 4> fanIndices{0, 1, 2, 3};
  std::array<u8, sizeof(fanIndices)> fanIndexBytes{};
  std::memcpy(fanIndexBytes.data(), fanIndices.data(), fanIndexBytes.size());
  checkEq(device->drawIndexedPrimitiveUP(PrimitiveType::TriangleFan, 2,
                                          std::span<const u8>(vertexPayload.data(), vertexPayload.size()),
                                          std::span<const u8>(fanIndexBytes.data(), fanIndexBytes.size()),
                                          IndexType::UInt32),
          D3D_OK, "draw indexed primitive up");
  checkEq(backend->draws.size(), size_t{2}, "second draw count");
  const auto& draw1 = backend->draws[1];
  checkEq(draw1.primitiveType, PrimitiveType::TriangleList, "fan decomposed to triangle list");
  checkEq(draw1.primitiveCount, 2u, "fan primitive count");
  checkEq(draw1.indexType, IndexType::UInt32, "fan draw index type");
  checkEq(draw1.userIndexData.size(), sizeof(u32) * 6u, "fan user index payload");

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
  checkEq(backend->presents[0].multiSampleType, MultiSampleType::Four, "present sample type");
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
  check(!backend->presents[1].displaySyncEnabled, "immediate present after reset");
  checkEq(device->swapChain()->backBuffer()->desc().width, 800u, "swapchain width after reset");
  checkEq(device->swapChain()->backBuffer()->desc().height, 600u, "swapchain height after reset");
}

void testComWrappers() {
  using namespace dxmt9::com;

  auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  check(d3d != nullptr, "Direct3DCreate9");
  checkEq(d3d->AddRef(), 2u, "factory addref");
  checkEq(d3d->Release(), 1u, "factory release after addref");

  void* unknown = nullptr;
  check(d3d->QueryInterface(InterfaceId::IUnknown, &unknown), "factory query interface");
  auto* queriedFactory = static_cast<IDirect3D9*>(unknown);
  check(queriedFactory != nullptr, "factory query result");
  checkEq(queriedFactory->Release(), 1u, "factory qi release");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;

  auto* device = d3d->CreateDevice(0, params);
  check(device != nullptr, "wrapper device create");
  check(device->coreDevice().swapChain() != nullptr, "wrapper core device swap chain");
  checkEq(device->AddRef(), 2u, "device addref");
  checkEq(device->Release(), 1u, "device release after addref");

  void* deviceUnknown = nullptr;
  check(device->QueryInterface(InterfaceId::Direct3DDevice9, &deviceUnknown), "device query interface");
  auto* queriedDevice = static_cast<IDirect3DDevice9*>(deviceUnknown);
  check(queriedDevice != nullptr, "device query result");
  checkEq(queriedDevice->Release(), 1u, "device qi release");
  checkEq(device->Release(), 0u, "device release");
  checkEq(d3d->Release(), 0u, "factory release");
}

}  // namespace

int main() {
  try {
    testFormatAndCaps();
    testHelpers();
    testFfpKeys();
    testDeviceCoreFlow();
    testComWrappers();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
