#include "core_spec_fixtures.hpp"

#include <cstdlib>
#include <memory>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

std::shared_ptr<Device> makeDevice() {
  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxColorAttachments = kMaxRenderTargets;
  limits.supportsDepth24Stencil8 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits, backend);
  PresentParameters params{};
  params.backBufferWidth = 64;
  params.backBufferHeight = 64;
  params.backBufferFormat = Format::A8R8G8B8;
  params.backBufferCount = 1;
  params.windowed = true;
  params.deviceWindow = Handle{0x51};
  params.enableAutoDepthStencil = true;
  params.autoDepthStencilFormat = Format::D24S8;

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "stateblock test device creation");
  return device;
}

std::shared_ptr<Texture> makeTexture(
    const std::shared_ptr<Device>& device,
    u32 width,
    u32 height,
    TextureType type = TextureType::TwoD) {
  auto texture = device->createTexture(TextureDesc{
      .width = width,
      .height = height,
      .depth = 1,
      .levels = 1,
      .format = Format::A8R8G8B8,
      .type = type,
      .pool = Pool::Default,
      .usage = UsageTexture,
  });
  check(texture != nullptr, "stateblock texture creation");
  return texture;
}

std::shared_ptr<Surface> makeRenderTarget(
    const std::shared_ptr<Device>& device,
    u32 width,
    u32 height,
    MultiSampleType samples = MultiSampleType::None) {
  auto surface = device->createSurface(SurfaceDesc{
      .width = width,
      .height = height,
      .format = Format::A8R8G8B8,
      .pool = Pool::Default,
      .usage = UsageRenderTarget,
      .renderTarget = true,
      .depthStencil = false,
      .multiSampleType = samples,
  });
  check(surface != nullptr, "stateblock render-target creation");
  return surface;
}

std::shared_ptr<Surface> makeDepthStencil(
    const std::shared_ptr<Device>& device,
    u32 width,
    u32 height,
    MultiSampleType samples = MultiSampleType::None) {
  auto surface = device->createSurface(SurfaceDesc{
      .width = width,
      .height = height,
      .format = Format::D24S8,
      .pool = Pool::Default,
      .usage = UsageDepthStencil,
      .renderTarget = false,
      .depthStencil = true,
      .multiSampleType = samples,
  });
  check(surface != nullptr, "stateblock depth-stencil creation");
  return surface;
}

RenderTargetAttachment attachmentOf(const std::shared_ptr<Surface>& surface) {
  return RenderTargetAttachment{
      surface->handle(),
      surface->level(),
      surface->multiSampleCount(),
  };
}

void testAllStateBlockRestoresTextureSamplerTssAndRenderTargets() {
  auto device = makeDevice();
  auto texture0 = makeTexture(device, 16, 16);
  auto texture7 = makeTexture(device, 8, 8, TextureType::Cube);
  auto replacementTexture = makeTexture(device, 32, 32);
  auto rt0 = makeRenderTarget(device, 64, 64);
  auto rt2 = makeRenderTarget(device, 32, 32, MultiSampleType::Four);
  auto replacementRt = makeRenderTarget(device, 16, 16);
  auto depth = makeDepthStencil(device, 64, 64);
  auto replacementDepth = makeDepthStencil(device, 16, 16);

  checkEq(device->setTexture(0, texture0), D3D_OK, "set snapshot texture 0");
  checkEq(device->setTexture(7, texture7), D3D_OK, "set snapshot texture 7");
  checkEq(device->setTextureStageState(0, TSS_COLOR_OP,
                                       static_cast<u32>(TextureOp::SelectArg2)),
          D3D_OK, "set snapshot TSS stage 0");
  checkEq(device->setTextureStageState(7, TSS_TEXCOORD_INDEX, 13u),
          D3D_OK, "set snapshot TSS stage 7");
  checkEq(device->setSamplerState(0, SAMP_ADDRESS_U, 3u),
          D3D_OK, "set snapshot sampler 0");
  checkEq(device->setSamplerState(15, SAMP_MAX_ANISOTROPY, 12u),
          D3D_OK, "set snapshot sampler 15");
  checkEq(device->setRenderTarget(0, rt0), D3D_OK, "set snapshot RT0");
  checkEq(device->setRenderTarget(2, rt2), D3D_OK, "set snapshot RT2");
  checkEq(device->setDepthStencilSurface(depth), D3D_OK, "set snapshot DS");
  checkEq(device->setStreamSourceFreq(0, kStreamSourceIndexedData | 3u),
          D3D_OK, "set snapshot stream0 instance count");
  checkEq(device->setStreamSourceFreq(1, kStreamSourceInstanceData | 2u),
          D3D_OK, "set snapshot stream1 instance divisor");

  auto block = device->createStateBlock(StateBlockType::All);
  check(block != nullptr, "all stateblock capture");

  checkEq(device->setTexture(0, replacementTexture), D3D_OK, "mutate texture 0");
  checkEq(device->setTexture(7, nullptr), D3D_OK, "mutate texture 7");
  checkEq(device->setTextureStageState(0, TSS_COLOR_OP,
                                       static_cast<u32>(TextureOp::Modulate4x)),
          D3D_OK, "mutate TSS stage 0");
  checkEq(device->setTextureStageState(7, TSS_TEXCOORD_INDEX, 2u),
          D3D_OK, "mutate TSS stage 7");
  checkEq(device->setSamplerState(0, SAMP_ADDRESS_U, 1u),
          D3D_OK, "mutate sampler 0");
  checkEq(device->setSamplerState(15, SAMP_MAX_ANISOTROPY, 1u),
          D3D_OK, "mutate sampler 15");
  checkEq(device->setRenderTarget(0, replacementRt), D3D_OK, "mutate RT0");
  checkEq(device->setRenderTarget(2, nullptr), D3D_OK, "mutate RT2");
  checkEq(device->setDepthStencilSurface(replacementDepth), D3D_OK, "mutate DS");
  checkEq(device->setStreamSourceFreq(0, 1u), D3D_OK,
          "mutate stream0 instance count");
  checkEq(device->setStreamSourceFreq(1, 1u), D3D_OK,
          "mutate stream1 instance divisor");

  checkEq(device->applyStateBlock(*block), D3D_OK, "apply all stateblock");
  const auto& state = device->state();
  check(state.textures[0] == texture0, "all stateblock restores texture slot 0");
  check(state.textures[7] == texture7, "all stateblock restores texture slot 7");
  checkEq(device->getTextureStageState(0, TSS_COLOR_OP),
          static_cast<u32>(TextureOp::SelectArg2),
          "all stateblock restores TSS stage 0 value");
  checkEq(device->getTextureStageState(7, TSS_TEXCOORD_INDEX), 13u,
          "all stateblock restores high-stage TSS value");
  checkEq(device->getSamplerState(0, SAMP_ADDRESS_U), 3u,
          "all stateblock restores sampler 0 value");
  checkEq(device->getSamplerState(15, SAMP_MAX_ANISOTROPY), 12u,
          "all stateblock restores sampler 15 value");
  checkEq(state.renderTargets[0], attachmentOf(rt0),
          "all stateblock restores RT0 handle/level/sample payload");
  checkEq(state.renderTargets[2], attachmentOf(rt2),
          "all stateblock restores sparse RT2 handle/level/sample payload");
  checkEq(state.depthStencil, attachmentOf(depth),
          "all stateblock restores depth-stencil payload");
  checkEq(state.streamFrequencies[0], kStreamSourceIndexedData | 3u,
          "all stateblock restores stream0 instance count");
  checkEq(state.streamFrequencies[1], kStreamSourceInstanceData | 2u,
          "all stateblock restores stream1 instance divisor");
}

void testRecordedDeltaStateBlockAppliesOnlyChangedResourcePayloads() {
  auto device = makeDevice();
  auto textureBefore = makeTexture(device, 16, 16);
  auto textureAfter = makeTexture(device, 32, 32);
  auto textureCurrent = makeTexture(device, 64, 64);
  auto rtBefore = makeRenderTarget(device, 64, 64);
  auto rtAfter = makeRenderTarget(device, 32, 32);
  auto rtCurrent = makeRenderTarget(device, 16, 16);

  auto before = device->state();
  before.textures[3] = textureBefore;
  before.renderTargets[1] = attachmentOf(rtBefore);
  before.textureStageStates[2].set(TSS_COLOR_ARG1, 0x10u);
  before.samplerStates[4].set(SAMP_MAG_FILTER, 1u);

  auto after = before;
  after.textures[3] = textureAfter;
  after.renderTargets[1] = attachmentOf(rtAfter);
  after.textureStageStates[2].set(TSS_COLOR_ARG1, 0x20u);

  StateBlock block;
  block.captureDelta(before, after);

  auto& current = device->mutableState();
  current.textures[3] = textureCurrent;
  current.renderTargets[1] = attachmentOf(rtCurrent);
  current.samplerStates[4].set(SAMP_MAG_FILTER, 9u);
  current.textureStageStates[2].set(TSS_COLOR_ARG1, 0x99u);

  block.apply(*device);
  const auto& state = device->state();
  check(state.textures[3] == textureAfter,
        "recorded stateblock delta applies changed texture binding");
  checkEq(state.renderTargets[1], attachmentOf(rtAfter),
          "recorded stateblock delta applies changed RT payload");
  checkEq(device->getTextureStageState(2, TSS_COLOR_ARG1), 0x20u,
          "recorded stateblock delta applies changed TSS payload");
  checkEq(device->getSamplerState(4, SAMP_MAG_FILTER), 9u,
          "recorded stateblock delta leaves unchanged sampler payload alone");
}

void testRecordedDeltaStateBlockCapturesShaderConstantPayloads() {
  auto device = makeDevice();

  DeviceState before;
  before.reset();
  before.vsConst.float4[1] = {1.0f, 2.0f, 3.0f, 4.0f};
  before.vsConst.int4[2] = {10, 20, 30, 40};
  before.psConst.float4[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
  before.psConst.bools[5] = true;

  auto recorded = before;
  recorded.vsConst.float4[1] = {5.0f, 6.0f, 7.0f, 8.0f};
  recorded.vsConst.int4[2] = {-10, -20, -30, -40};
  recorded.vsConst.bools[3] = true;
  recorded.psConst.float4[4] = {9.0f, 10.0f, 11.0f, 12.0f};
  recorded.psConst.int4[1] = {100, 200, 300, 400};
  recorded.psConst.bools[5] = false;

  StateBlock block;
  block.captureDelta(before, recorded);

  auto current = before;
  current.vsConst.float4[1] = {21.0f, 22.0f, 23.0f, 24.0f};
  current.vsConst.float4[6] = {91.0f, 92.0f, 93.0f, 94.0f};
  current.vsConst.int4[2] = {-101, -202, -303, -404};
  current.vsConst.bools[3] = true;
  current.psConst.float4[4] = {31.0f, 32.0f, 33.0f, 34.0f};
  current.psConst.int4[1] = {501, 502, 503, 504};
  current.psConst.bools[5] = false;
  current.psConst.bools[7] = true;
  block.capture(current);

  auto& target = device->mutableState();
  target.vsConst.float4[1] = {-1.0f, -1.0f, -1.0f, -1.0f};
  target.vsConst.float4[6] = {41.0f, 42.0f, 43.0f, 44.0f};
  target.vsConst.int4[2] = {1, 1, 1, 1};
  target.vsConst.bools[3] = false;
  target.psConst.float4[4] = {-9.0f, -9.0f, -9.0f, -9.0f};
  target.psConst.int4[1] = {2, 2, 2, 2};
  target.psConst.bools[5] = true;
  target.psConst.bools[7] = false;

  block.apply(*device);
  const auto& state = device->state();
  checkNear(state.vsConst.float4[1][0], 21.0f, 1.0e-6f,
            "recorded stateblock captures current VS float constant");
  checkNear(state.vsConst.float4[1][3], 24.0f, 1.0e-6f,
            "recorded stateblock captures full VS float constant register");
  checkNear(state.vsConst.float4[6][0], 41.0f, 1.0e-6f,
            "recorded stateblock leaves unrecorded VS float register alone");
  checkEq(state.vsConst.int4[2][2], -303,
          "recorded stateblock captures current VS int constant");
  checkEq(state.vsConst.bools[3], true,
          "recorded stateblock captures current VS bool constant");
  checkNear(state.psConst.float4[4][2], 33.0f, 1.0e-6f,
            "recorded stateblock captures current PS float constant");
  checkEq(state.psConst.int4[1][3], 504,
          "recorded stateblock captures current PS int constant");
  checkEq(state.psConst.bools[5], false,
          "recorded stateblock captures false PS bool constant");
  checkEq(state.psConst.bools[7], false,
          "recorded stateblock leaves unrecorded PS bool register alone");
}

}  // namespace

int main() {
  try {
    testAllStateBlockRestoresTextureSamplerTssAndRenderTargets();
    testRecordedDeltaStateBlockAppliesOnlyChangedResourcePayloads();
    testRecordedDeltaStateBlockCapturesShaderConstantPayloads();
  } catch (const TestFailure& e) {
    std::cerr << "core_stateblock_restore_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
