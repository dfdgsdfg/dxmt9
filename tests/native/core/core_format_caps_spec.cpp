#include "core_spec_fixtures.hpp"

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

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

  const auto* dxt1 = findFormatInfo(Format::DXT1);
  check(dxt1 != nullptr, "DXT1 format info missing");
  checkEq(dxt1->backendFormat, BackendPixelFormat::BC1_RGBA, "DXT1 backend format");
  checkEq(dxmt9::convert::toPixelFormat(Format::DXT1, BackendLimits{}),
          WMTPixelFormatBC1_RGBA, "DXT1 WMT format");
  checkEq(dxmt9::convert::toCullMode(static_cast<u32>(CullMode::Cw)),
          WMTCullModeFront, "D3DCULL_CW culls clockwise front faces");
  checkEq(dxmt9::convert::toCullMode(static_cast<u32>(CullMode::Ccw)),
          WMTCullModeBack, "D3DCULL_CCW culls counter-clockwise back faces");

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

  checkEq(factory.checkDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, true), D3D_OK,
          "HAL windowed device type");
  checkEq(factory.checkDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, false), D3D_OK,
          "HAL fullscreen device type");
  checkEq(factory.checkDeviceType(0, DeviceType::Ref, Format::A8R8G8B8, Format::A8R8G8B8, true),
          D3DERR_NOTAVAILABLE, "non-HAL device type");
  const auto modes = factory.enumAdapterModes(0, Format::A8R8G8B8);
  check(!modes.empty(), "adapter modes");
  checkEq(modes.front().width, 640u, "first adapter mode width");
  checkEq(modes.front().height, 480u, "first adapter mode height");
  checkEq(modes.front().format, Format::A8R8G8B8, "first adapter mode format");
  const auto displayMode = factory.getAdapterDisplayMode(0);
  checkEq(displayMode.width, 1920u, "adapter display width");
  checkEq(displayMode.height, 1080u, "adapter display height");
  checkEq(displayMode.format, Format::A8R8G8B8, "adapter display format");
  const auto identifier = factory.getAdapterIdentifier(0);
  checkEq(identifier.description, std::string("NVIDIA GeForce 6800"), "adapter description");
  checkEq(identifier.monitor, 1u, "adapter monitor");
  checkEq(factory.getAdapterMonitor(0), 1u, "adapter monitor lookup");

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

void testSigned3DcAndUnsupportedFormatCaps() {
  BackendLimits limits{};
  limits.supportsBgr10A2 = true;
  limits.supportsDepth24Stencil8 = true;
  limits.supportsDepth32FloatStencil8 = true;
  Factory factory(limits);

  const auto* v8u8 = findFormatInfo(Format::V8U8);
  check(v8u8 != nullptr, "V8U8 format info missing");
  checkEq(v8u8->backendFormat, BackendPixelFormat::RG8Snorm,
          "V8U8 backend format");
  checkEq(v8u8->support, FormatClass::Required, "V8U8 support class");
  checkEq(v8u8->bytesPerPixel, 2u, "V8U8 bytes per pixel");
  checkEq(dxmt9::convert::toPixelFormat(Format::V8U8, limits),
          WMTPixelFormatRG8Snorm, "V8U8 WMT signed-normal pixel format");
  checkEq(factory.checkDeviceFormat(0, Format::V8U8, UsageTexture), D3D_OK,
          "V8U8 texture caps");

  const auto* q8w8v8u8 = findFormatInfo(Format::Q8W8V8U8);
  check(q8w8v8u8 != nullptr, "Q8W8V8U8 format info missing");
  checkEq(q8w8v8u8->backendFormat, BackendPixelFormat::RGBA8Snorm,
          "Q8W8V8U8 backend format");
  checkEq(q8w8v8u8->bytesPerPixel, 4u, "Q8W8V8U8 bytes per pixel");
  checkEq(dxmt9::convert::toPixelFormat(Format::Q8W8V8U8, limits),
          WMTPixelFormatRGBA8Snorm,
          "Q8W8V8U8 WMT signed-normal pixel format");

  const auto* v16u16 = findFormatInfo(Format::V16U16);
  check(v16u16 != nullptr, "V16U16 format info missing");
  checkEq(v16u16->backendFormat, BackendPixelFormat::RG16Snorm,
          "V16U16 backend format");
  checkEq(v16u16->bytesPerPixel, 4u, "V16U16 bytes per pixel");
  checkEq(dxmt9::convert::toPixelFormat(Format::V16U16, limits),
          WMTPixelFormatRG16Snorm,
          "V16U16 WMT signed-normal pixel format");

  const auto* ati2 = findFormatInfo(Format::ATI2);
  check(ati2 != nullptr, "ATI2 format info missing");
  checkEq(ati2->backendFormat, BackendPixelFormat::BC5_RGUnorm,
          "ATI2 backend format");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(843666497u), Format::ATI2,
          "ATI2 FOURCC maps to core format");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::BC5), 843666497u,
          "BC5 maps to ATI2 D3D FOURCC");
  checkEq(dxmt9::convert::toPixelFormat(Format::ATI2, limits),
          WMTPixelFormatBC5_RGUnorm, "ATI2 WMT BC5 pixel format");
  checkEq(formatBlockBytes(Format::ATI2), 16u, "ATI2 block bytes");
  checkEq(formatRowPitch(Format::ATI2, 5u), 32u,
          "ATI2 row pitch rounds to 4-wide blocks");
  checkEq(formatByteSize(Format::ATI2, 5u, 7u), std::size_t{64},
          "ATI2 byte size rounds to 4x4 blocks");
  checkEq(factory.checkDeviceFormat(0, Format::ATI2, UsageTexture), D3D_OK,
          "ATI2 texture caps");
  checkEq(factory.checkDeviceFormat(0, Format::ATI2, UsageRenderTarget),
          D3DERR_NOTAVAILABLE, "ATI2 render-target caps rejected");

  const auto* cxv8u8 = findFormatInfo(Format::CxV8U8);
  check(cxv8u8 != nullptr, "CxV8U8 format info missing");
  checkEq(cxv8u8->support, FormatClass::Unsupported,
          "CxV8U8 unsupported vendor format class");
  checkEq(cxv8u8->backendFormat, BackendPixelFormat::Unknown,
          "CxV8U8 has no backend format");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(117u), Format::CxV8U8,
          "CxV8U8 D3DFORMAT maps to explicit unsupported core value");
  checkEq(factory.checkDeviceFormat(0, Format::CxV8U8, UsageTexture),
          D3DERR_NOTAVAILABLE, "CxV8U8 texture caps rejected");
  checkEq(factory.checkDeviceFormat(0, Format::Unknown, UsageTexture),
          D3DERR_NOTAVAILABLE, "unknown/null-like format caps rejected");
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

void testDeviceCPresentIntervalMapping() {
  constexpr u32 kD3DPRESENT_INTERVAL_DEFAULT = 0x00000000u;
  constexpr u32 kD3DPRESENT_INTERVAL_ONE = 0x00000001u;
  constexpr u32 kD3DPRESENT_INTERVAL_TWO = 0x00000002u;
  constexpr u32 kD3DPRESENT_INTERVAL_IMMEDIATE = 0x80000000u;

  using dxmt9::d3d9::devicec::presentIntervalFromD3D;
  using dxmt9::d3d9::devicec::presentIntervalToD3D;

  checkEq(presentIntervalFromD3D(kD3DPRESENT_INTERVAL_DEFAULT), PresentInterval::Default,
          "default present interval bridge");
  checkEq(presentIntervalFromD3D(kD3DPRESENT_INTERVAL_ONE), PresentInterval::Default,
          "one present interval bridge");
  checkEq(presentIntervalFromD3D(kD3DPRESENT_INTERVAL_TWO), PresentInterval::Two,
          "two present interval bridge");
  checkEq(presentIntervalFromD3D(kD3DPRESENT_INTERVAL_IMMEDIATE), PresentInterval::Immediate,
          "immediate present interval bridge");

  checkEq(presentIntervalToD3D(PresentInterval::Immediate), kD3DPRESENT_INTERVAL_IMMEDIATE,
          "immediate present interval bridge return");
  checkEq(presentIntervalToD3D(PresentInterval::Default), kD3DPRESENT_INTERVAL_ONE,
          "default present interval bridge return");
  checkEq(presentIntervalToD3D(PresentInterval::Two), kD3DPRESENT_INTERVAL_TWO,
          "two present interval bridge return");

  D9CPresentParams cParams{};
  cParams.presentationInterval = kD3DPRESENT_INTERVAL_IMMEDIATE;
  checkEq(dxmt9::d3d9::devicec::ppFromC(cParams).presentationInterval, PresentInterval::Immediate,
          "present params preserve immediate interval");
}

void testRingArenaExhaustionFallsBack() {
  dxmt9::scratch::RingArena arena{64};

  auto* first = arena.allocateBytes(48, 16, 1);
  check(first != nullptr, "ring arena first allocation");
  auto* second = arena.allocateBytes(32, 16, 2);
  check(second == nullptr, "ring arena exhaustion returns null");

  arena.reclaim(1);
  auto* third = arena.allocateBytes(32, 16, 3);
  check(third != nullptr, "ring arena reuses reclaimed storage");

  arena.reclaim(3);
  auto* fourth = arena.allocateBytes(64, 16, 4);
  check(fourth != nullptr, "ring arena resets after full reclaim");
}

}  // namespace

int main() {
  try {
    testFormatAndCaps();
    testSigned3DcAndUnsupportedFormatCaps();
    testHelpers();
    testDeviceCPresentIntervalMapping();
    testRingArenaExhaustionFallsBack();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
