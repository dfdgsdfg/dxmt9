#include "core_spec_fixtures.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    const char* existing = std::getenv(name);
    if (existing) {
      oldValue_ = existing;
      hadValue_ = true;
    }
    setenv(name, value, 1);
  }

  ~ScopedEnv() {
    if (hadValue_) {
      setenv(name_.c_str(), oldValue_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::string oldValue_;
  bool hadValue_ = false;
};

bool fileExistsWithBmpHeader(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  char header[2] = {};
  file.read(header, sizeof(header));
  return header[0] == 'B' && header[1] == 'M';
}

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
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
  auto mipTexture = device->createTexture({2, 2, 1, 2, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
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
  check(mipTexture != nullptr, "mipmap texture");
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

  auto mipLevel0 = mipTexture->surfaceLevel(0);
  auto mipLevel1 = mipTexture->surfaceLevel(1);
  check(mipLevel0 != nullptr && mipLevel1 != nullptr, "mipmap texture levels");
  mipLevel0->fillColor(nullptr, {1.0f, 0.0f, 0.0f, 1.0f});
  mipLevel1->fillColor(nullptr, {0.0f, 0.0f, 0.0f, 1.0f});
  checkEq(mipTexture->generateMipSubLevels(), D3D_OK, "generate mip sublevels");
  const std::array<u8, 4> redPixel = bgra(0x00, 0x00, 0xff, 0xff);
  checkBytes(std::span<const u8>(mipTexture->levelBytes(1).data(), 4),
             std::span<const u8>(redPixel.data(), redPixel.size()),
             "generated mip level 1");

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
  light.diffuse = {0.25f, 0.5f, 0.75f, 1.0f};
  light.specular = {0.9f, 0.8f, 0.7f, 1.0f};
  light.ambient = {0.1f, 0.2f, 0.3f, 1.0f};
  light.position = {1.0f, 2.0f, 3.0f};
  light.direction = {0.0f, -1.0f, 0.0f};
  light.range = 42.0f;
  light.falloff = 0.75f;
  light.attenuation0 = 0.5f;
  light.attenuation1 = 0.25f;
  light.attenuation2 = 0.125f;
  light.theta = 0.3f;
  light.phi = 0.6f;
  Light secondaryLight{};
  secondaryLight.type = LightType::Spot;
  secondaryLight.diffuse = {0.4f, 0.3f, 0.2f, 1.0f};
  secondaryLight.specular = {0.2f, 0.3f, 0.4f, 1.0f};
  secondaryLight.ambient = {0.05f, 0.06f, 0.07f, 1.0f};
  secondaryLight.position = {-2.0f, -4.0f, -6.0f};
  secondaryLight.direction = {0.0f, 0.0f, -1.0f};
  secondaryLight.range = 12.0f;
  secondaryLight.falloff = 0.5f;
  secondaryLight.attenuation0 = 1.0f;
  secondaryLight.attenuation1 = 0.1f;
  secondaryLight.attenuation2 = 0.01f;
  secondaryLight.theta = 0.2f;
  secondaryLight.phi = 0.9f;
  Material material{};
  material.emissive = {0.01f, 0.02f, 0.03f, 1.0f};
  material.ambient = {0.11f, 0.12f, 0.13f, 1.0f};
  material.diffuse = {0.21f, 0.22f, 0.23f, 0.8f};
  material.specular = {0.31f, 0.32f, 0.33f, 1.0f};
  material.power = 17.0f;
  checkEq(device->setLight(0, light), D3D_OK, "set light");
  checkEq(device->lightEnable(0, true), D3D_OK, "enable light");
  checkEq(device->setLight(1, secondaryLight), D3D_OK, "set secondary light");
  checkEq(device->lightEnable(1, false), D3D_OK, "disable secondary light");
  checkEq(device->setMaterial(material), D3D_OK, "set material");
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
  checkEq(device->setSamplerState(kVertexTextureSampler0, SAMP_MIN_FILTER, 2), D3D_OK,
          "first vertex sampler state accepted");
  checkEq(device->setTexture(kVertexTextureSampler0, texture), D3D_OK,
          "first vertex texture binding accepted");
  checkEq(device->setTexture(kMaxTextures, texture), D3DERR_INVALIDCALL,
          "texture slot past vertex texture range rejected");
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
  check(stateBlock->snapshot().textures[kVertexTextureSampler0] == texture,
        "state block vertex texture snapshot");
  checkEq(stateBlock->snapshot().fvf, 0x1122u, "state block fvf snapshot");
  checkEq(stateBlock->snapshot().material, material, "state block material snapshot");
  checkEq(stateBlock->snapshot().lights[0], device->state().lights[0],
          "state block primary light snapshot");
  checkEq(stateBlock->snapshot().lights[1], secondaryLight,
          "state block secondary light snapshot");
  check(stateBlock->snapshot().lightEnabled[0], "state block primary light enable snapshot");
  check(!stateBlock->snapshot().lightEnabled[1],
        "state block secondary light disable snapshot");

  checkEq(device->setRenderState(RS_LIGHTING, 0), D3D_OK, "mutate lighting");
  Material mutatedMaterial{};
  mutatedMaterial.diffuse = {0.9f, 0.8f, 0.7f, 0.6f};
  mutatedMaterial.power = 3.0f;
  Light mutatedLight{};
  mutatedLight.type = LightType::Directional;
  mutatedLight.diffuse = {0.6f, 0.5f, 0.4f, 1.0f};
  mutatedLight.direction = {1.0f, 0.0f, 0.0f};
  checkEq(device->setMaterial(mutatedMaterial), D3D_OK, "mutate material");
  checkEq(device->setLight(0, mutatedLight), D3D_OK, "mutate primary light");
  checkEq(device->lightEnable(0, false), D3D_OK, "mutate primary light enable");
  checkEq(device->setLight(1, mutatedLight), D3D_OK, "mutate secondary light");
  checkEq(device->lightEnable(1, true), D3D_OK, "mutate secondary light enable");
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
  checkEq(device->state().material, material, "restored material");
  checkEq(device->state().lights[0], stateBlock->snapshot().lights[0],
          "restored primary light");
  checkEq(device->state().lights[1], secondaryLight, "restored secondary light");
  check(device->state().lightEnabled[0], "restored primary light enable");
  check(!device->state().lightEnabled[1], "restored secondary light disable");

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
  checkEq(texture->setLod(1), 0u, "texture SetLOD previous value");
  checkEq(texture->lod(), 1u, "texture SetLOD stored value");

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
  checkEq(draw0.hot.textures[kVertexTextureSampler0], texture->handle(),
          "draw0 vertex texture handle");
  check((draw0.hot.textureMask & (1u << kVertexTextureSampler0)) != 0,
        "draw0 vertex texture mask bit");
  checkEq(draw0.hot.textureLods[0], 1u, "draw0 texture lod");
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
  checkEq(flatStateOr(draw0.hot.samplerStates[kVertexTextureSampler0], SAMP_MIN_FILTER, 0u), 2u,
          "draw0 vertex sampler min filter");
  checkEq(flatStateOr(draw0.hot.renderStates, RS_LIGHTING, 0u), 1u, "draw0 lighting state");
  checkEq(flatStateOr(draw0.hot.renderStates, RS_ALPHA_TEST_ENABLE, 0u), 1u,
          "draw0 alpha test state");
  checkEq(draw0.uniforms.material, material, "draw0 material payload");
  checkEq(draw0.uniforms.lights[0], device->state().lights[0],
          "draw0 primary light payload");
  checkEq(draw0.uniforms.lights[1], secondaryLight,
          "draw0 secondary light payload");
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
  check(draw0.state.shaderLayout.vertexShader.vertexKey->lightEnabled[0],
        "draw0 vertex key primary light enabled");
  check(!draw0.state.shaderLayout.vertexShader.vertexKey->lightEnabled[1],
        "draw0 vertex key secondary light disabled");
  checkEq(draw0.state.shaderLayout.vertexShader.vertexKey->lightType[0],
          static_cast<u32>(LightType::Point), "draw0 vertex key primary light type");
  checkEq(draw0.state.shaderLayout.vertexShader.vertexKey->lightType[1],
          static_cast<u32>(LightType::Spot), "draw0 vertex key secondary light type");
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

void testQueryFlushPresentResetSequenceBoundaries() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters params{};
  params.backBufferWidth = 64;
  params.backBufferHeight = 48;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{0x1200u};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "query sequence device create");

  auto timestamp = device->createQuery(QueryType::Timestamp);
  check(timestamp != nullptr, "timestamp sequence query");
  checkEq(device->issueQuery(timestamp, false), D3D_OK,
          "timestamp issue records submitted sequence");
  checkEq(device->submittedSequenceId(), 1ull,
          "timestamp issue advances submitted sequence");
  checkEq(device->completedSequenceId(), 0ull,
          "timestamp issue does not complete sequence");

  u64 timestampValue = 0;
  checkEq(device->getQueryData(timestamp, &timestampValue,
                               sizeof(timestampValue), 0),
          S_FALSE, "timestamp unresolved before flush");
  checkEq(backend->flushCount, 0u, "non-flush query poll does not flush");
  checkEq(device->completedSequenceId(), 0ull,
          "non-flush query poll keeps completion cursor");

  checkEq(device->getQueryData(timestamp, &timestampValue,
                               sizeof(timestampValue), QUERY_GETDATA_FLUSH),
          S_OK, "timestamp query resolves after explicit flush");
  checkEq(backend->flushCount, 1u, "query flush crosses backend boundary once");
  checkEq(device->completedSequenceId(), 1ull,
          "query flush completes through submitted sequence");
  checkEq(timestampValue, 1ull, "timestamp value records issued sequence");

  auto event = device->createQuery(QueryType::Event);
  check(event != nullptr, "event sequence query");
  checkEq(device->issueQuery(event, false), D3D_OK,
          "event issue records next submitted sequence");
  checkEq(device->submittedSequenceId(), 2ull,
          "event issue advances submitted sequence");
  checkEq(device->completedSequenceId(), 1ull,
          "event issue leaves completion behind");
  checkEq(device->present(), D3D_OK, "present completes outstanding event");
  checkEq(backend->presents.size(), size_t{1}, "present submitted once");
  checkEq(backend->flushCount, 1u,
          "immediate present does not add a synchronous flush");
  checkEq(device->submittedSequenceId(), 3ull,
          "present itself advances submitted sequence");
  checkEq(device->completedSequenceId(), 3ull,
          "present completes outstanding query and present sequence");
  checkEq(device->getQueryData(event, nullptr, 0, 0), S_OK,
          "event query is ready after present completion");

  PresentParameters resetParams = params;
  resetParams.backBufferWidth = 80;
  resetParams.backBufferHeight = 60;
  resetParams.deviceWindow = Handle{0x1201u};
  checkEq(device->reset(resetParams), D3D_OK,
          "reset drains and recreates swapchain");
  checkEq(backend->flushCount, 2u, "reset flushes before teardown");
  checkEq(device->submittedSequenceId(), 0ull,
          "reset clears submitted sequence cursor");
  checkEq(device->completedSequenceId(), 0ull,
          "reset clears completed sequence cursor");
  checkEq(device->state().viewport.width, 80u,
          "reset sequence test viewport width");
  checkEq(device->state().viewport.height, 60u,
          "reset sequence test viewport height");

  auto postResetTimestamp = device->createQuery(QueryType::Timestamp);
  check(postResetTimestamp != nullptr, "post-reset timestamp query");
  checkEq(device->issueQuery(postResetTimestamp, false), D3D_OK,
          "post-reset timestamp issue");
  checkEq(device->submittedSequenceId(), 1ull,
          "post-reset query starts from fresh submitted sequence");
  checkEq(device->completedSequenceId(), 0ull,
          "post-reset query is not completed by stale pre-reset cursor");
  timestampValue = 0;
  checkEq(device->getQueryData(postResetTimestamp, &timestampValue,
                               sizeof(timestampValue), 0),
          S_FALSE, "post-reset query waits for new completion");
  checkEq(device->present(), D3D_OK, "post-reset present completes query");
  checkEq(backend->presents.size(), size_t{2},
          "post-reset present submitted once");
  checkEq(device->getQueryData(postResetTimestamp, &timestampValue,
                               sizeof(timestampValue), 0),
          S_OK, "post-reset query ready after new present");
  checkEq(timestampValue, 1ull,
          "post-reset timestamp uses reset-local sequence id");
}

void testResetClearsStaleDrawStateBeforeNextBoundaryPacket() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters params{};
  params.backBufferWidth = 128u;
  params.backBufferHeight = 64u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{0x1600u};
  params.enableAutoDepthStencil = true;
  params.autoDepthStencilFormat = Format::D24S8;

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "reset draw-state device create");
  const auto originalBackBuffer = device->swapChain()->backBuffer();
  const auto originalDepth = device->swapChain()->depthStencilSurface();
  check(originalBackBuffer != nullptr, "reset draw-state original backbuffer");
  check(originalDepth != nullptr, "reset draw-state original depth");

  auto staleVertexBuffer =
      device->createBuffer({64u, Pool::Default, UsageVertexBuffer});
  auto staleIndexBuffer =
      device->createBuffer({32u, Pool::Default, UsageIndexBuffer});
  auto staleTexture = device->createTexture(
      {4u, 4u, 1u, 1u, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed,
       UsageTexture});
  auto staleRt1 = device->createSurface(
      {32u, 32u, Format::A8R8G8B8, Pool::Default, UsageRenderTarget, true});
  auto staleDepth = device->createSurface(
      {32u, 32u, Format::D24S8, Pool::Default, UsageDepthStencil, false,
       true});
  check(staleVertexBuffer != nullptr, "reset draw-state stale vb");
  check(staleIndexBuffer != nullptr, "reset draw-state stale ib");
  check(staleTexture != nullptr, "reset draw-state stale texture");
  check(staleRt1 != nullptr, "reset draw-state stale rt1");
  check(staleDepth != nullptr, "reset draw-state stale depth");

  checkEq(device->setStreamSource(0, staleVertexBuffer, 12u, 24u), D3D_OK,
          "reset draw-state bind stream");
  checkEq(device->setIndices(staleIndexBuffer, IndexType::UInt32), D3D_OK,
          "reset draw-state bind index");
  checkEq(device->setTexture(0, staleTexture), D3D_OK,
          "reset draw-state bind texture");
  checkEq(device->setTexture(kVertexTextureSampler0, staleTexture), D3D_OK,
          "reset draw-state bind vertex texture");
  checkEq(device->setRenderTarget(1, staleRt1), D3D_OK,
          "reset draw-state bind rt1");
  checkEq(device->setDepthStencilSurface(staleDepth), D3D_OK,
          "reset draw-state bind custom depth");
  checkEq(device->setRenderState(RS_ALPHA_TEST_ENABLE, 1u), D3D_OK,
          "reset draw-state mutate alpha test");
  checkEq(device->setSamplerState(0, SAMP_MIN_FILTER, 2u), D3D_OK,
          "reset draw-state mutate sampler");
  checkEq(device->setTextureStageState(0, TSS_COLOR_OP,
                                       static_cast<u32>(TextureOp::SelectArg1)),
          D3D_OK, "reset draw-state mutate tss");

  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 1u, 3u), D3D_OK,
          "reset draw-state pre-reset draw");
  checkEq(backend->draws.size(), size_t{1},
          "reset draw-state pre-reset draw recorded");
  const auto& staleDraw = backend->draws.back();
  checkEq(staleDraw.hot.streamBuffers[0], staleVertexBuffer->handle(),
          "pre-reset draw carries stale stream");
  checkEq(staleDraw.hot.indexBuffer, Handle{},
          "pre-reset non-indexed draw strips bound index");
  checkEq(staleDraw.hot.textures[0], staleTexture->handle(),
          "pre-reset draw carries stale texture");
  checkEq(staleDraw.hot.textures[kVertexTextureSampler0],
          staleTexture->handle(),
          "pre-reset draw carries stale vertex texture");
  checkEq(staleDraw.hot.colorAttachments[1].handle, staleRt1->handle(),
          "pre-reset draw carries stale rt1");
  checkEq(staleDraw.hot.depthStencil.handle, staleDepth->handle(),
          "pre-reset draw carries stale depth");

  PresentParameters resetParams = params;
  resetParams.backBufferWidth = 96u;
  resetParams.backBufferHeight = 48u;
  resetParams.deviceWindow = Handle{0x1601u};
  checkEq(device->reset(resetParams), D3D_OK,
          "reset draw-state reset succeeds");
  const auto resetBackBuffer = device->swapChain()->backBuffer();
  const auto resetDepth = device->swapChain()->depthStencilSurface();
  check(resetBackBuffer != nullptr, "reset draw-state replacement backbuffer");
  check(resetDepth != nullptr, "reset draw-state replacement depth");
  check(resetBackBuffer != originalBackBuffer,
        "reset draw-state backbuffer recreated");
  check(resetDepth != originalDepth, "reset draw-state depth recreated");
  check(!staleVertexBuffer->valid(),
        "reset draw-state default stream invalidated");
  check(!staleIndexBuffer->valid(),
        "reset draw-state default index invalidated");
  check(!staleRt1->valid(), "reset draw-state rt1 invalidated");
  check(!staleDepth->valid(), "reset draw-state custom depth invalidated");
  check(staleTexture->valid(), "reset draw-state managed texture survives");

  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 1u, 0u), D3D_OK,
          "reset draw-state post-reset draw");
  checkEq(backend->draws.size(), size_t{2},
          "reset draw-state post-reset draw recorded");
  const auto& resetDraw = backend->draws.back();
  checkEq(resetDraw.hot.streamBuffers[0], Handle{},
          "post-reset draw does not carry stale stream handle");
  checkEq(resetDraw.hot.streamOffsets[0], 0u,
          "post-reset draw clears stale stream offset");
  checkEq(resetDraw.hot.streamStrides[0], 0u,
          "post-reset draw clears stale stream stride");
  checkEq(resetDraw.hot.indexBuffer, Handle{},
          "post-reset draw does not carry stale index handle");
  checkEq(resetDraw.param.indexType, IndexType::UInt16,
          "post-reset draw restores default index type");
  checkEq(resetDraw.hot.textures[0], Handle{},
          "post-reset draw does not carry stale texture");
  checkEq(resetDraw.hot.textures[kVertexTextureSampler0], Handle{},
          "post-reset draw does not carry stale vertex texture");
  checkEq(resetDraw.hot.colorAttachments[0].handle, resetBackBuffer->handle(),
          "post-reset draw carries replacement backbuffer");
  checkEq(resetDraw.hot.colorAttachments[0].sampleCount,
          resetBackBuffer->multiSampleCount(),
          "post-reset draw carries replacement backbuffer samples");
  checkEq(resetDraw.hot.colorAttachments[1].handle, Handle{},
          "post-reset draw clears stale rt1");
  checkEq(resetDraw.hot.depthStencil.handle, resetDepth->handle(),
          "post-reset draw carries replacement depth");
  checkEq(resetDraw.hot.viewport.viewport.width, 96u,
          "post-reset draw carries reset viewport width");
  checkEq(resetDraw.hot.viewport.viewport.height, 48u,
          "post-reset draw carries reset viewport height");
  checkEq(flatStateOr(resetDraw.hot.renderStates, RS_ALPHA_TEST_ENABLE, 99u),
          0u, "post-reset draw restores alpha-test default");
  checkEq(flatStateOr(resetDraw.hot.samplerStates[0], SAMP_MIN_FILTER, 99u),
          1u, "post-reset draw restores sampler default");
  checkEq(flatStateOr(resetDraw.hot.textureStageStates[0], TSS_COLOR_OP, 0u),
          static_cast<u32>(TextureOp::Modulate),
          "post-reset draw restores texture-stage default");
}

void testGetRenderTargetDataUsesRuntimeReadbackPayload() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters params{};
  params.backBufferWidth = 64u;
  params.backBufferHeight = 32u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{0x1700u};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "readback payload device");

  auto source = device->createSurface({
      .width = 3u,
      .height = 2u,
      .format = Format::A8R8G8B8,
      .pool = Pool::Default,
      .usage = UsageRenderTarget,
      .renderTarget = true,
  });
  auto destination = device->createSurface({
      .width = 3u,
      .height = 2u,
      .format = Format::A8R8G8B8,
      .pool = Pool::Scratch,
  });
  check(source != nullptr, "readback payload source surface");
  check(destination != nullptr, "readback payload destination surface");

  backend->surfaceCopies.clear();
  backend->readbacks.clear();
  backend->readbackSurfaceCalls.clear();
  backend->flushCount = 0;
  backend->readbackSurfaceResult = true;
  backend->readbackSurfacePixels.pitch = 16u;
  backend->readbackSurfacePixels.bytes = {
      0x11u, 0x12u, 0x13u, 0xffu,
      0x21u, 0x22u, 0x23u, 0xffu,
      0x31u, 0x32u, 0x33u, 0xffu,
      0xeeu, 0xeeu, 0xeeu, 0xeeu,
      0x41u, 0x42u, 0x43u, 0xffu,
      0x51u, 0x52u, 0x53u, 0xffu,
      0x61u, 0x62u, 0x63u, 0xffu,
      0xddu, 0xddu, 0xddu, 0xddu,
  };

  checkEq(device->getRenderTargetData(source, destination), D3D_OK,
          "runtime readback payload succeeds");

  checkEq(backend->readbacks.size(), size_t{1},
          "runtime readback submits exactly one ReadbackDesc");
  checkEq(backend->readbackSurfaceCalls.size(), size_t{1},
          "runtime readback invokes backend readbackSurface once");
  checkEq(backend->flushCount, 1u,
          "runtime readback flushes before reading backend pixels");
  check(backend->surfaceCopies.empty(),
        "successful runtime readback does not fall back to surface copy");

  const auto& submitted = backend->readbacks[0];
  const auto& consumed = backend->readbackSurfaceCalls[0];
  checkEq(submitted.source, source->handle(),
          "submitted readback source handle");
  checkEq(submitted.destination, destination->handle(),
          "submitted readback destination handle");
  checkEq(submitted.sourceRect, Rect{0, 0, 3, 2},
          "submitted readback covers full source extent");
  checkEq(submitted.sourceLevel, 0u,
          "submitted readback source level");
  checkEq(submitted.sourceSampleCount, 1u,
          "submitted readback source sample count");
  checkEq(submitted.destinationSampleCount, 1u,
          "submitted readback destination sample count");
  checkEq(consumed.source, submitted.source,
          "backend readbackSurface consumes same source handle");
  checkEq(consumed.destination, submitted.destination,
          "backend readbackSurface consumes same destination handle");
  checkEq(consumed.sourceRect, submitted.sourceRect,
          "backend readbackSurface consumes same source rect");

  auto region = destination->lockRect(nullptr, 0);
  check(region.data != nullptr, "runtime readback destination lock");
  checkEq(region.pitch, 12u,
          "destination surface keeps native compact pitch");
  const auto* bytes = static_cast<const u8*>(region.data);
  const std::array<u8, 12> firstRow{
      0x11u, 0x12u, 0x13u, 0xffu,
      0x21u, 0x22u, 0x23u, 0xffu,
      0x31u, 0x32u, 0x33u, 0xffu,
  };
  const std::array<u8, 12> secondRow{
      0x41u, 0x42u, 0x43u, 0xffu,
      0x51u, 0x52u, 0x53u, 0xffu,
      0x61u, 0x62u, 0x63u, 0xffu,
  };
  checkBytes(std::span<const u8>(bytes, firstRow.size()),
             std::span<const u8>(firstRow.data(), firstRow.size()),
             "runtime readback copies first backend row");
  checkBytes(std::span<const u8>(bytes + region.pitch, secondRow.size()),
             std::span<const u8>(secondRow.data(), secondRow.size()),
             "runtime readback honors backend pitch for second row");
  destination->unlockRect();
}

void testGetRenderTargetDataFallsBackWhenRuntimeReadbackUnavailable() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters params{};
  params.backBufferWidth = 32u;
  params.backBufferHeight = 32u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{0x1701u};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "readback fallback device");

  auto source = device->createSurface({
      .width = 2u,
      .height = 2u,
      .format = Format::A8R8G8B8,
      .pool = Pool::Default,
      .usage = UsageRenderTarget,
      .renderTarget = true,
  });
  auto destination = device->createSurface({
      .width = 2u,
      .height = 2u,
      .format = Format::A8R8G8B8,
      .pool = Pool::Scratch,
  });
  check(source != nullptr, "readback fallback source surface");
  check(destination != nullptr, "readback fallback destination surface");

  const std::array<u8, 16> sourcePixels{
      0x10u, 0x11u, 0x12u, 0xffu,
      0x20u, 0x21u, 0x22u, 0xffu,
      0x30u, 0x31u, 0x32u, 0xffu,
      0x40u, 0x41u, 0x42u, 0xffu,
  };
  auto sourceRegion = source->lockRect(nullptr, 0);
  check(sourceRegion.data != nullptr, "readback fallback source lock");
  checkEq(sourceRegion.pitch, 8u, "readback fallback source pitch");
  std::memcpy(sourceRegion.data, sourcePixels.data(), sourcePixels.size());
  source->unlockRect();

  const std::array<u8, 16> runtimePixels{
      0xa0u, 0xa1u, 0xa2u, 0xffu,
      0xb0u, 0xb1u, 0xb2u, 0xffu,
      0xc0u, 0xc1u, 0xc2u, 0xffu,
      0xd0u, 0xd1u, 0xd2u, 0xffu,
  };
  backend->surfaceCopies.clear();
  backend->readbacks.clear();
  backend->readbackSurfaceCalls.clear();
  backend->flushCount = 0;
  backend->readbackSurfaceResult = false;
  backend->readbackSurfacePixels.pitch = 8u;
  backend->readbackSurfacePixels.bytes.assign(runtimePixels.begin(),
                                              runtimePixels.end());

  checkEq(device->getRenderTargetData(source, destination), D3D_OK,
          "readback fallback succeeds");

  checkEq(backend->readbacks.size(), size_t{1},
          "fallback submits readback boundary before copy");
  checkEq(backend->flushCount, 1u,
          "fallback flushes submitted readback before probing backend pixels");
  checkEq(backend->readbackSurfaceCalls.size(), size_t{1},
          "fallback probes runtime readback once");
  checkEq(backend->surfaceCopies.size(), size_t{1},
          "fallback records surface-copy boundary after runtime miss");

  const auto& readback = backend->readbacks[0];
  const auto& consumed = backend->readbackSurfaceCalls[0];
  const auto& copy = backend->surfaceCopies[0];
  checkEq(readback.source, source->handle(),
          "fallback readback source handle");
  checkEq(readback.destination, destination->handle(),
          "fallback readback destination handle");
  checkEq(readback.sourceRect, Rect{0, 0, 2, 2},
          "fallback readback covers source extent");
  checkEq(consumed.source, readback.source,
          "fallback runtime probe sees submitted source");
  checkEq(consumed.destination, readback.destination,
          "fallback runtime probe sees submitted destination");
  checkEq(copy.source, source->handle(),
          "fallback copy source handle");
  checkEq(copy.destination, destination->handle(),
          "fallback copy destination handle");
  checkEq(copy.sourceRect, Rect{0, 0, 2, 2},
          "fallback copy covers source extent");
  checkEq(copy.destinationRect, Rect{0, 0, 2, 2},
          "fallback copy covers destination extent");

  auto destinationRegion = destination->lockRect(nullptr, 0);
  check(destinationRegion.data != nullptr, "readback fallback destination lock");
  checkEq(destinationRegion.pitch, 8u, "readback fallback destination pitch");
  checkBytes(std::span<const u8>(
                 static_cast<const u8*>(destinationRegion.data),
                 sourcePixels.size()),
             std::span<const u8>(sourcePixels.data(), sourcePixels.size()),
             "fallback copies CPU surface bytes instead of stale runtime pixels");
  destination->unlockRect();
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

void testExperimentCaptureFrameListAndRangeWriteInternalFrames() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("dxmt9-capture-" + std::to_string(std::rand()));
  const auto captureDir = root / "internal_frames";
  std::filesystem::remove_all(root);

  ScopedEnv capturePath("DXMT_EXPERIMENT_CAPTURE_PATH",
                        (root / "legacy.bmp").string().c_str());
  ScopedEnv captureDirEnv("DXMT_EXPERIMENT_CAPTURE_DIR",
                          captureDir.string().c_str());
  ScopedEnv captureFrame("DXMT_CAPTURE_FRAME", "1");
  ScopedEnv captureFrames("DXMT_CAPTURE_FRAMES", "2,4");
  ScopedEnv captureRange("DXMT_CAPTURE_RANGE", "3:5:2");

  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters params{};
  params.backBufferWidth = 4;
  params.backBufferHeight = 4;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{0x3000u};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "capture device");
  auto backBuffer = device->swapChain()->backBuffer();
  check(backBuffer != nullptr, "capture backbuffer");

  checkEq(device->fillSurface(backBuffer, nullptr, {1.0f, 0.0f, 0.0f, 1.0f}),
          D3D_OK, "fill capture frame 1");
  checkEq(device->present(), D3D_OK, "present capture frame 1");
  checkEq(device->fillSurface(backBuffer, nullptr, {0.0f, 1.0f, 0.0f, 1.0f}),
          D3D_OK, "fill capture frame 2");
  checkEq(device->present(), D3D_OK, "present capture frame 2");
  checkEq(device->fillSurface(backBuffer, nullptr, {0.0f, 0.0f, 1.0f, 1.0f}),
          D3D_OK, "fill capture frame 3");
  checkEq(device->present(), D3D_OK, "present capture frame 3");

  check(fileExistsWithBmpHeader(root / "legacy.bmp"),
        "legacy DXMT_CAPTURE_FRAME writes single capture path");
  check(fileExistsWithBmpHeader(captureDir / "frame000002.bmp"),
        "DXMT_CAPTURE_FRAMES writes requested frame");
  check(fileExistsWithBmpHeader(captureDir / "frame000003.bmp"),
        "DXMT_CAPTURE_RANGE writes interval frame");
  check(!std::filesystem::exists(captureDir / "frame000001.bmp"),
        "multi-frame dir does not duplicate legacy single-frame capture");

  std::filesystem::remove_all(root);
}

void testResetRestartsExperimentCaptureFrameCounter() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("dxmt9-capture-reset-" + std::to_string(std::rand()));
  const auto capturePath = root / "frame2.bmp";
  std::filesystem::remove_all(root);

  ScopedEnv capturePathEnv("DXMT_EXPERIMENT_CAPTURE_PATH",
                           capturePath.string().c_str());
  ScopedEnv captureDirEnv("DXMT_EXPERIMENT_CAPTURE_DIR", "");
  ScopedEnv captureFrame("DXMT_CAPTURE_FRAME", "2");
  ScopedEnv captureFrames("DXMT_CAPTURE_FRAMES", "");
  ScopedEnv captureRange("DXMT_CAPTURE_RANGE", "");

  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters params{};
  params.backBufferWidth = 4;
  params.backBufferHeight = 4;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{0x3002u};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "capture reset device");
  auto backBuffer = device->swapChain()->backBuffer();
  check(backBuffer != nullptr, "capture reset backbuffer");

  checkEq(device->fillSurface(backBuffer, nullptr, {1.0f, 0.0f, 0.0f, 1.0f}),
          D3D_OK, "fill capture reset frame 1");
  checkEq(device->present(), D3D_OK, "present capture reset frame 1");
  check(!std::filesystem::exists(capturePath),
        "capture reset frame 1 does not satisfy frame 2 request");
  checkEq(backend->readbacks.size(), size_t{0},
          "capture reset frame 1 does not issue readback");

  checkEq(device->fillSurface(backBuffer, nullptr, {0.0f, 1.0f, 0.0f, 1.0f}),
          D3D_OK, "fill capture reset frame 2");
  checkEq(device->present(), D3D_OK, "present capture reset frame 2");
  check(fileExistsWithBmpHeader(capturePath),
        "capture reset pre-reset frame 2 writes capture");
  checkEq(backend->readbacks.size(), size_t{1},
          "capture reset pre-reset frame 2 issues readback");

  std::filesystem::remove(capturePath);
  backend->readbacks.clear();
  backend->readbackSurfaceCalls.clear();
  backend->surfaceCopies.clear();

  PresentParameters resetParams = params;
  resetParams.backBufferWidth = 6;
  resetParams.backBufferHeight = 6;
  checkEq(device->reset(resetParams), D3D_OK,
          "capture reset resets device");
  backBuffer = device->swapChain()->backBuffer();
  check(backBuffer != nullptr, "capture reset replacement backbuffer");

  checkEq(device->fillSurface(backBuffer, nullptr, {0.0f, 0.0f, 1.0f, 1.0f}),
          D3D_OK, "fill capture reset post-reset frame 1");
  checkEq(device->present(), D3D_OK,
          "present capture reset post-reset frame 1");
  check(!std::filesystem::exists(capturePath),
        "capture reset post-reset frame 1 does not reuse stale counter");
  checkEq(backend->readbacks.size(), size_t{0},
          "capture reset post-reset frame 1 does not issue readback");

  checkEq(device->fillSurface(backBuffer, nullptr, {1.0f, 1.0f, 0.0f, 1.0f}),
          D3D_OK, "fill capture reset post-reset frame 2");
  checkEq(device->present(), D3D_OK,
          "present capture reset post-reset frame 2");
  check(fileExistsWithBmpHeader(capturePath),
        "capture reset post-reset frame 2 writes capture again");
  checkEq(backend->readbacks.size(), size_t{1},
          "capture reset post-reset frame 2 issues exactly one readback");

  std::filesystem::remove_all(root);
}

void testExperimentCaptureWriteFailureEmitsSkipSidecar() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("dxmt9-capture-skip-" + std::to_string(std::rand()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto blockedPath = root / "blocked.bmp";
  std::filesystem::create_directories(blockedPath);

  ScopedEnv capturePath("DXMT_EXPERIMENT_CAPTURE_PATH",
                        blockedPath.string().c_str());
  ScopedEnv captureFrame("DXMT_CAPTURE_FRAME", "1");

  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters params{};
  params.backBufferWidth = 4;
  params.backBufferHeight = 4;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{0x3001u};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "capture skip device");
  auto backBuffer = device->swapChain()->backBuffer();
  check(backBuffer != nullptr, "capture skip backbuffer");

  checkEq(device->fillSurface(backBuffer, nullptr, {1.0f, 1.0f, 0.0f, 1.0f}),
          D3D_OK, "fill capture skip frame");
  checkEq(device->present(), D3D_OK, "present capture skip frame");

  const auto sidecar = root / "blocked.bmp.skipped.json";
  check(std::filesystem::exists(sidecar),
        "failed internal capture writes skipped-frame sidecar");
  const auto payload = readTextFile(sidecar);
  check(payload.find("\"schema\": \"dxmt9.render_capture.skip.v1\"") !=
            std::string::npos,
        "skip sidecar records schema");
  check(payload.find("\"reason\": \"artifact-write-failed\"") !=
            std::string::npos,
        "skip sidecar records reason");

  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  try {
    testDeviceCoreFlow();
    testFullscreenAndDeviceLost();
    testQueryFlushPresentResetSequenceBoundaries();
    testResetClearsStaleDrawStateBeforeNextBoundaryPacket();
    testGetRenderTargetDataUsesRuntimeReadbackPayload();
    testGetRenderTargetDataFallsBackWhenRuntimeReadbackUnavailable();
    testSwapChainPresentOverridesCallerSourceWithOwningBackBuffer();
    testExperimentCaptureFrameListAndRangeWriteInternalFrames();
    testResetRestartsExperimentCaptureFrameCounter();
    testExperimentCaptureWriteFailureEmitsSkipSidecar();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
