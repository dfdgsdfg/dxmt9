// R-BACK-2.15.5: real cached batch construction followed by Metal readback.
// Unlike DrawPrimitiveUP corpus probes, every draw uses one persistent VB and
// submitDirectReplayDrawFromCurrentState's binding-agnostic snapshot cache.
#include "dxmt9/core.hpp"
#include "../../../src/winemetal/Metal.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dxmt9::core;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

template <std::size_t N>
ShaderRef shader(const std::array<u32, N>& words) {
  ShaderRef result{};
  result.kind = ShaderRef::Kind::Bytecode;
  std::vector<u8> bytes(sizeof(words));
  std::memcpy(bytes.data(), words.data(), sizeof(words));
  result.bytecode.bytes = std::move(bytes);
  result.bytecode.hash = hashBytes(std::as_bytes(std::span(words)));
  return result;
}

void run() {
  BackendLimits limits{};
  limits.maxTextureSize = 4096;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;
  Factory factory(limits);
  PresentParameters parameters{};
  parameters.backBufferWidth = parameters.backBufferHeight = 64;
  parameters.backBufferFormat = Format::A8R8G8B8;
  parameters.windowed = true;
  parameters.presentationInterval = PresentInterval::Immediate;
  auto device = factory.createDevice(0, parameters);
  check(bool(device), "create device");
  const auto target = device->swapChain()->backBuffer();
  auto readback = device->createSurface(
      {64, 64, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  check(target && readback, "real render/readback surfaces");
  check(device->setRenderTarget(0, target) == D3D_OK, "bind RT");
  check(device->setViewport({0, 0, 64, 64, 0, 1}) == D3D_OK, "viewport");
  check(device->setRenderState(RS_CULL_MODE, u32(CullMode::None)) == D3D_OK,
        "no culling");
  check(device->setRenderState(RS_Z_ENABLE, 0) == D3D_OK, "no depth");
  check(device->setRenderState(RS_LIGHTING, 0) == D3D_OK, "diffuse FFP");
  check(device->setFVF(0x4002u | 0x040u) == D3D_OK, "XYZW + diffuse FVF");
  struct Vertex { float x, y, z, w; u32 color; };
  const std::array<Vertex, 6> vertices{{
      {-.8f, -.8f, .5f, 1, 0xff00ff00}, {-.8f, .8f, .5f, 1, 0xff00ff00},
      {.8f, -.8f, .5f, 1, 0xff00ff00}, {.8f, -.8f, .5f, 1, 0xff00ff00},
      {-.8f, .8f, .5f, 1, 0xff00ff00}, {.8f, .8f, .5f, 1, 0xff00ff00},
  }};
  auto buffer = device->createBuffer({sizeof(vertices), Pool::Default, UsageVertexBuffer});
  check(bool(buffer), "persistent VB");
  const auto mapping = buffer->lock(0, sizeof(vertices), 0);
  check(mapping.data && mapping.pitch >= sizeof(vertices), "VB lock");
  std::memcpy(mapping.data, vertices.data(), sizeof(vertices));
  buffer->unlock();
  check(device->setStreamSource(0, buffer, 0, sizeof(Vertex)) == D3D_OK, "bind VB");

  // Swap x/y in FFP view. A world-space y plane therefore clips x in FFP,
  // while a programmable VS must continue to consume the clip-space y plane.
  Matrix4x4 view{};
  view.m = {0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  check(device->setTransform(XFORM_VIEW, view) == D3D_OK, "distinguishing view");
  check(device->setClipPlane(0, ClipPlane{0, 1, 0, 0}) == D3D_OK, "positive y plane");
  check(device->setRenderState(RS_CLIPPING, 1) == D3D_OK, "enable clipping");
  check(device->setRenderState(RS_CLIP_PLANE_ENABLE, 1) == D3D_OK, "enable plane");
  // Project-authored pass-through VS/PS from the programmable-space corpus.
  const auto vs = shader(std::array<u32, 20>{
      0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000,
      0x0200001f, 0x8000000a, 0x900f0001, 0x0200001f,
      0x80000000, 0xe00f0000, 0x0200001f, 0x8000000a,
      0xe00f0001, 0x02000001, 0xe00f0000, 0x90e40000,
      0x02000001, 0xe00f0001, 0x90e40001, 0x0000ffff});
  const auto ps = shader(std::array<u32, 8>{
      0xffff0300, 0x0200001f, 0x8000000a, 0x900f0000,
      0x02000001, 0x800f0800, 0x90e40000, 0x0000ffff});

  unsigned ordinal = 0;
  const auto draw = [&](const char* label, unsigned greenMask) {
    ClearDesc clear{};
    clear.clearColor = true;
    clear.color = {1, 0, 0, 1};
    check(device->clear(clear) == D3D_OK, "clear");
    check(device->submitDirectReplayDrawFromCurrentState(
              DrawParam{.primitiveType = PrimitiveType::TriangleList,
                        .primitiveCount = 2}).succeeded(), "cached batch draw");
    check(device->getRenderTargetData(target, readback) == D3D_OK, "GPU readback");
    const auto pixels = readback->lockRect(nullptr, 0);
    check(pixels.data && pixels.pitch >= 64 * 4, "readback lock");
    const auto* bytes = static_cast<const u8*>(pixels.data);
    bool match = true;
    for (unsigned i = 0; i < 4; ++i) {
      const auto offset = (i / 2 ? 48 : 16) * pixels.pitch + (i % 2 ? 48 : 16) * 4;
      const bool green = (greenMask & (1u << i)) != 0;
      const std::array<u8, 4> expected{0, u8(green ? 255 : 0), u8(green ? 0 : 255), 255};
      if (std::memcmp(bytes + offset, expected.data(), 4) != 0) {
        std::fprintf(stderr, "%s probe %u: %u,%u,%u,%u expected %u,%u,%u,%u\n",
                     label, i, bytes[offset], bytes[offset+1], bytes[offset+2], bytes[offset+3],
                     expected[0], expected[1], expected[2], expected[3]);
        match = false;
      }
    }
    // The process wrapper compares ALL active pixel bytes between OFF/ON;
    // padding is deliberately excluded. Known-color probes reject NullBackend.
    std::printf("frame=%u label=%s pixels=", ordinal++, label);
    for (unsigned y = 0; y < 64; ++y)
      for (unsigned x = 0; x < 64 * 4; ++x)
        std::printf("%02x", bytes[y * pixels.pitch + x]);
    std::puts("");
    readback->unlockRect();
    check(match, "deterministic pixel oracle");
  };
  for (bool programmable : {false, true, false}) {
    if (programmable) {
      const std::vector<VertexElement> declaration{
          {0, 0, 3, 0, 0, 0}, {0, 16, 4, 0, 10, 0}};
      check(device->setVertexDeclaration(declaration) == D3D_OK, "float4/color declaration");
    } else {
      check(device->setFVF(0x4002u | 0x040u) == D3D_OK, "FFP FVF restored");
    }
    check(device->setVertexShader(programmable ? vs : ShaderRef{}) == D3D_OK, "VS transition");
    check(device->setPixelShader(programmable ? ps : ShaderRef{}) == D3D_OK, "PS transition");
    const unsigned positive = programmable ? 3u : 10u;
    draw("positive", positive);
    for (u32 alphaRef : {1u, 2u, 1u}) {
      check(device->setRenderState(RS_ALPHA_REF, alphaRef) == D3D_OK, "irrelevant RS change");
      draw("irrelevant-rs", positive);
    }
    check(device->setRenderState(RS_CLIPPING, 0) == D3D_OK, "clipping off");
    draw("clipping-off", 15u);
    check(device->setRenderState(RS_CLIPPING, 1) == D3D_OK, "clipping restored");
    draw("clipping-restored", positive);
    check(device->setRenderState(RS_CLIP_PLANE_ENABLE, 0) == D3D_OK, "mask off");
    draw("mask-off", 15u);
    check(device->setRenderState(RS_CLIP_PLANE_ENABLE, 1) == D3D_OK, "mask restored");
    draw("mask-restored", positive);
    auto saved = device->captureStateBlock();
    check(bool(saved), "capture StateBlock");
    check(device->setClipPlane(0, ClipPlane{0, -1, 0, 0}) == D3D_OK, "negative plane");
    draw("negative", 15u ^ positive);
    check(device->applyStateBlock(*saved) == D3D_OK, "restore StateBlock");
    draw("stateblock-restored", positive);
  }
}
}  // namespace

int main() {
  const auto devices = WMT::CopyAllDevices();
  if (!devices || devices.count() == 0) {
    std::puts("SKIP: no Metal device available");
    return 77;
  }
  try { run(); }
  catch (const std::exception& e) {
    std::fprintf(stderr, "uniform invalidation Metal oracle: %s\n", e.what());
    return 1;
  }
  return 0;
}
