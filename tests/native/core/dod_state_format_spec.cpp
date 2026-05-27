#include "dxmt9/core.hpp"
#include "device_c_common.hpp"
#include "../../../src/dxmt9/dxmt9_format_convert.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

using namespace dxmt9::core;

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

template <typename T, typename U>
void checkEq(const T& actual, const U& expected, std::string_view message) {
  if (!(actual == expected)) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

constexpr u32 fourcc(char a, char b, char c, char d) {
  return static_cast<u32>(static_cast<unsigned char>(a)) |
         (static_cast<u32>(static_cast<unsigned char>(b)) << 8u) |
         (static_cast<u32>(static_cast<unsigned char>(c)) << 16u) |
         (static_cast<u32>(static_cast<unsigned char>(d)) << 24u);
}

BackendLimits defaultLimits() {
  BackendLimits limits{};
  limits.supportsDepth24Stencil8 = true;
  limits.supportsDepth32FloatStencil8 = true;
  limits.supportsBgr10A2 = true;
  return limits;
}

bool hasUsage(WMTTextureUsage usage, WMTTextureUsage flag) {
  return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
}

void checkSwizzle(WMTTextureSwizzleChannels actual, WMTTextureSwizzle r,
                  WMTTextureSwizzle g, WMTTextureSwizzle b,
                  WMTTextureSwizzle a, std::string_view message) {
  checkEq(actual.r, r, message);
  checkEq(actual.g, g, message);
  checkEq(actual.b, b, message);
  checkEq(actual.a, a, message);
}

Factory makeFactory(BackendLimits limits = defaultLimits()) {
  return Factory(limits, std::make_shared<BackendDevice>());
}

void checkInfo(Format format, BackendPixelFormat backend, FormatClass support, u32 expectedBytesPerPixel,
               bool renderTarget, bool depthStencil, bool compressed) {
  const auto* info = findFormatInfo(format);
  check(info != nullptr, "format info is present");
  checkEq(info->format, format, "format info preserves key");
  checkEq(info->backendFormat, backend, "format backend policy");
  checkEq(info->support, support, "format support class");
  checkEq(info->bytesPerPixel, expectedBytesPerPixel, "format byte size policy");
  checkEq(info->renderTarget, renderTarget, "format render-target policy");
  checkEq(info->depthStencil, depthStencil, "format depth-stencil policy");
  checkEq(info->compressed, compressed, "format compressed policy");
  checkEq(classifyFormat(format), support, "classifier mirrors table");
  checkEq(backendPixelFormat(format), backend, "backend format helper mirrors table");
  checkEq(bytesPerPixel(format), expectedBytesPerPixel, "bytes-per-pixel helper mirrors table");
  checkEq(isCompressedFormat(format), compressed, "compressed helper mirrors table");
}

void testFormatTableCoverageAndClassifierPolicy() {
  const auto& table = formatTable();
  constexpr auto kLastFormatIndex = static_cast<std::size_t>(Format::INDEX32);
  checkEq(table.size(), kLastFormatIndex, "format table covers every non-Unknown public enum value");

  std::array<bool, kLastFormatIndex + 1u> seen{};
  for (const auto& info : table) {
    const auto index = static_cast<std::size_t>(info.format);
    check(index > 0u && index <= kLastFormatIndex, "format table entry stays in public enum range");
    check(!seen[index], "format table has no duplicate entries");
    seen[index] = true;
    checkEq(classifyFormat(info.format), info.support, "classifier uses table support class");
    checkEq(backendPixelFormat(info.format), info.backendFormat, "backend helper uses table backend format");
  }
  for (std::size_t index = 1; index <= kLastFormatIndex; ++index) {
    check(seen[index], "format table has no gap for public enum value");
  }

  checkInfo(Format::A8R8G8B8, BackendPixelFormat::BGRA8Unorm, FormatClass::Required,
            4u, true, false, false);
  checkInfo(Format::L8, BackendPixelFormat::R8Unorm, FormatClass::Required,
            1u, false, false, false);
  checkInfo(Format::A2B10G10R10, BackendPixelFormat::BGR10A2Unorm, FormatClass::Optional,
            4u, true, false, false);
  checkInfo(Format::D24S8, BackendPixelFormat::Depth24Unorm_Stencil8, FormatClass::Required,
            4u, false, true, false);
  checkInfo(Format::DXT1, BackendPixelFormat::BC1_RGBA, FormatClass::Required,
            0u, false, false, true);
}

void testFourCcAndPseudoFormatExplicitPolicy() {
  using dxmt9::d3d9::devicec::fmtFromD3D;
  using dxmt9::d3d9::devicec::fmtToD3D;

  checkEq(fourcc('D', 'X', 'T', '1'), 827611204u, "FOURCC helper matches DXT1 wire value");
  checkEq(fmtFromD3D(fourcc('D', 'X', 'T', '1')), Format::DXT1, "DXT1 FOURCC maps explicitly");
  checkEq(fmtFromD3D(fourcc('D', 'X', 'T', '5')), Format::DXT5, "DXT5 FOURCC maps explicitly");
  checkEq(fmtToD3D(Format::DXT1), fourcc('D', 'X', 'T', '1'), "DXT1 maps back to FOURCC");
  checkEq(fmtToD3D(Format::DXT5), fourcc('D', 'X', 'T', '5'), "DXT5 maps back to FOURCC");

  checkInfo(Format::ATI1, BackendPixelFormat::BC4_RUnorm, FormatClass::Required,
            0u, false, false, true);
  checkInfo(Format::BC4, BackendPixelFormat::BC4_RUnorm, FormatClass::Required,
            0u, false, false, true);
  checkInfo(Format::ATI2, BackendPixelFormat::BC5_RGUnorm, FormatClass::Required,
            0u, false, false, true);
  checkInfo(Format::BC5, BackendPixelFormat::BC5_RGUnorm, FormatClass::Required,
            0u, false, false, true);

  checkEq(fmtFromD3D(fourcc('A', 'T', 'I', '1')), Format::ATI1, "ATI1 FOURCC maps explicitly");
  checkEq(fmtFromD3D(fourcc('A', 'T', 'I', '2')), Format::ATI2, "ATI2 FOURCC maps explicitly");
  checkEq(fmtToD3D(Format::ATI1), fourcc('A', 'T', 'I', '1'), "ATI1 maps back to FOURCC");
  checkEq(fmtToD3D(Format::BC4), fourcc('A', 'T', 'I', '1'), "BC4 uses ATI1 D3D wire code");
  checkEq(fmtToD3D(Format::ATI2), fourcc('A', 'T', 'I', '2'), "ATI2 maps back to FOURCC");
  checkEq(fmtToD3D(Format::BC5), fourcc('A', 'T', 'I', '2'), "BC5 uses ATI2 D3D wire code");

  checkEq(formatBlockBytes(Format::DXT1), 8u, "DXT1 block byte policy");
  checkEq(formatBlockBytes(Format::DXT5), 16u, "DXT5 block byte policy");
  checkEq(formatBlockBytes(Format::ATI1), 8u, "ATI1 block byte policy");
  checkEq(formatBlockBytes(Format::BC4), 8u, "BC4 block byte policy");
  checkEq(formatBlockBytes(Format::ATI2), 16u, "ATI2 block byte policy");
  checkEq(formatBlockBytes(Format::BC5), 16u, "BC5 block byte policy");
  checkEq(formatRowPitch(Format::BC4, 5u), 16u, "BC4 row pitch rounds to 4x4 blocks");
  checkEq(formatByteSize(Format::BC5, 5u, 7u), std::size_t{64},
          "BC5 byte size rounds width and height to 4x4 blocks");
}

void testUnsupportedFormatBehavior() {
  const auto unknown = static_cast<Format>(0xffffffffu);

  check(findFormatInfo(Format::Unknown) == nullptr, "Unknown has no format table entry");
  check(findFormatInfo(unknown) == nullptr, "out-of-range format has no format table entry");
  checkEq(classifyFormat(Format::Unknown), FormatClass::Unsupported, "Unknown class is unsupported");
  checkEq(classifyFormat(unknown), FormatClass::Unsupported, "out-of-range class is unsupported");
  checkEq(backendPixelFormat(Format::Unknown), BackendPixelFormat::Unknown, "Unknown backend format");
  checkEq(backendPixelFormat(unknown), BackendPixelFormat::Unknown, "out-of-range backend format");
  checkEq(bytesPerPixel(Format::Unknown), 0u, "Unknown has no bytes per pixel");
  checkEq(formatRowPitch(unknown, 16u), 0u, "out-of-range format has no row pitch");
  checkEq(formatByteSize(unknown, 16u, 16u), std::size_t{0}, "out-of-range format has no byte size");
  checkEq(formatName(unknown), std::string{"Unknown"}, "out-of-range format name is Unknown");
  check(!formatSupportsUsage(Format::Unknown, UsageTexture, defaultLimits()),
        "Unknown rejects texture usage");
  check(!formatSupportsUsage(unknown, UsageTexture, defaultLimits()),
        "out-of-range format rejects texture usage");

  checkInfo(Format::R8G8B8, BackendPixelFormat::Unknown, FormatClass::Unsupported,
            3u, false, false, false);
  checkInfo(Format::CxV8U8, BackendPixelFormat::Unknown, FormatClass::Unsupported,
            0u, false, false, false);
  checkInfo(Format::S8_LOCKABLE, BackendPixelFormat::Unknown, FormatClass::Unsupported,
            1u, false, false, false);
  check(!formatSupportsUsage(Format::R8G8B8, 0u, defaultLimits()),
        "R8G8B8 remains unsupported even without usage flags");
  check(!formatSupportsUsage(Format::S8_LOCKABLE, UsageDepthStencil, defaultLimits()),
        "S8_LOCKABLE remains unsupported for depth-stencil usage");

  auto factory = makeFactory();
  checkEq(factory.checkDeviceFormat(0, Format::Unknown, UsageTexture), D3DERR_NOTAVAILABLE,
          "Factory rejects Unknown texture format");
  checkEq(factory.checkDeviceFormat(0, Format::R8G8B8, UsageTexture), D3DERR_NOTAVAILABLE,
          "Factory rejects tabled unsupported texture format");
  checkEq(factory.checkDeviceFormat(0, unknown, UsageTexture), D3DERR_NOTAVAILABLE,
          "Factory rejects out-of-range texture format");
}

void testYuvAndColorKeyPolicyAreExplicitlyUnsupported() {
  using dxmt9::d3d9::devicec::fmtFromD3D;

  checkEq(fmtFromD3D(fourcc('Y', 'U', 'Y', '2')), Format::Unknown,
          "YUY2 is not mapped to a shader-readable core format");
  checkEq(fmtFromD3D(fourcc('U', 'Y', 'V', 'Y')), Format::Unknown,
          "UYVY is not mapped to a shader-readable core format");
  checkEq(fmtFromD3D(fourcc('Y', 'V', '1', '2')), Format::Unknown,
          "YV12 is not mapped to a shader-readable core format");

  auto factory = makeFactory();
  checkEq(factory.checkDeviceFormat(0, fmtFromD3D(fourcc('Y', 'U', 'Y', '2')), UsageTexture),
          D3DERR_NOTAVAILABLE,
          "YUV texture formats remain unavailable until conversion/layout support exists");
  checkEq(factory.checkDeviceFormat(0, Format::Unknown, UsageTexture), D3DERR_NOTAVAILABLE,
          "color-key tests have no color-key-capable texture format policy in core");
}

void testCompressedRenderTargetFactoryRejection() {
  auto factory = makeFactory();

  for (const auto format : {Format::DXT1, Format::DXT3, Format::DXT5,
                           Format::ATI1, Format::BC4, Format::ATI2, Format::BC5}) {
    checkEq(factory.checkDeviceFormat(0, format, UsageTexture), D3D_OK,
            "Factory accepts compressed texture usage");
    checkEq(factory.checkDeviceFormat(0, format, UsageRenderTarget), D3DERR_NOTAVAILABLE,
            "Factory rejects compressed render-target usage");
    checkEq(factory.checkDeviceFormat(0, format, UsageTexture | UsageRenderTarget), D3DERR_NOTAVAILABLE,
            "Factory rejects compressed texture plus render-target usage");
    checkEq(factory.checkDeviceFormat(0, format, UsageDepthStencil), D3DERR_NOTAVAILABLE,
            "Factory rejects compressed depth-stencil usage");
    checkEq(factory.checkDeviceMultiSampleType(0, format, MultiSampleType::Two), D3DERR_NOTAVAILABLE,
            "Factory rejects compressed format for multisample render/depth target");
  }

  checkEq(factory.checkDeviceFormat(1, Format::DXT1, UsageRenderTarget), D3DERR_INVALIDCALL,
          "Factory reports invalid adapter before compressed format policy");
}

void testD24S8FallbackPolicy() {
  BackendLimits nativeDepth24 = defaultLimits();
  nativeDepth24.supportsDepth24Stencil8 = true;
  nativeDepth24.supportsDepth32FloatStencil8 = true;
  checkEq(dxmt9::convert::toPixelFormat(Format::D24S8, nativeDepth24),
          WMTPixelFormatDepth24Unorm_Stencil8, "D24S8 uses native Depth24Stencil8 when available");
  checkEq(dxmt9::convert::toPixelFormat(Format::D24X8, nativeDepth24),
          WMTPixelFormatDepth24Unorm_Stencil8, "D24X8 uses native Depth24Stencil8 when available");

  BackendLimits fallbackStencil = nativeDepth24;
  fallbackStencil.supportsDepth24Stencil8 = false;
  fallbackStencil.supportsDepth32FloatStencil8 = true;
  checkEq(dxmt9::convert::toPixelFormat(Format::D24S8, fallbackStencil),
          WMTPixelFormatDepth32Float_Stencil8, "D24S8 falls back to Depth32FloatStencil8");
  checkEq(dxmt9::convert::toPixelFormat(Format::D24X8, fallbackStencil),
          WMTPixelFormatDepth32Float_Stencil8, "D24X8 follows the D24S8 fallback path");

  BackendLimits fallbackDepthOnly = fallbackStencil;
  fallbackDepthOnly.supportsDepth32FloatStencil8 = false;
  checkEq(dxmt9::convert::toPixelFormat(Format::D24S8, fallbackDepthOnly),
          WMTPixelFormatDepth32Float, "D24S8 falls back to depth-only when stencil fallback is absent");

  auto factory = makeFactory(fallbackDepthOnly);
  checkEq(factory.checkDeviceFormat(0, Format::D24S8, UsageDepthStencil), D3D_OK,
          "Factory keeps D24S8 depth-stencil support advertised across backend fallback");
  checkEq(factory.checkDeviceFormat(0, Format::D24S8, UsageRenderTarget), D3DERR_NOTAVAILABLE,
          "Factory does not advertise D24S8 as a color render target");
}

void testShaderReadSwizzlePolicyForExpandedShaderReadFormats() {
  check(!dxmt9::convert::formatNeedsShaderReadSwizzle(Format::A8R8G8B8),
        "A8R8G8B8 samples without a shader-read swizzle view");
  check(dxmt9::convert::formatNeedsShaderReadSwizzle(Format::G16R16),
        "G16R16 needs shader-read swizzle");
  check(dxmt9::convert::formatNeedsShaderReadSwizzle(Format::R16F),
        "R16F needs shader-read swizzle");
  check(dxmt9::convert::formatNeedsShaderReadSwizzle(Format::G16R16F),
        "G16R16F needs shader-read swizzle");
  check(dxmt9::convert::formatNeedsShaderReadSwizzle(Format::R32F),
        "R32F needs shader-read swizzle");
  check(dxmt9::convert::formatNeedsShaderReadSwizzle(Format::G32R32F),
        "G32R32F needs shader-read swizzle");
  check(dxmt9::convert::formatNeedsShaderReadSwizzle(Format::L8),
        "L8 needs shader-read swizzle");
  check(dxmt9::convert::formatNeedsShaderReadSwizzle(Format::L16),
        "L16 needs shader-read swizzle");
  check(dxmt9::convert::formatNeedsShaderReadSwizzle(Format::A8L8),
        "A8L8 needs shader-read swizzle");

  checkSwizzle(dxmt9::convert::toShaderReadSwizzle(Format::A8R8G8B8),
               WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
               WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha,
               "ordinary RGBA shader-read swizzle is identity");
  checkSwizzle(dxmt9::convert::toShaderReadSwizzle(Format::R16F),
               WMTTextureSwizzleRed, WMTTextureSwizzleOne,
               WMTTextureSwizzleOne, WMTTextureSwizzleOne,
               "R16F shader-read swizzle fills missing channels with one");
  checkSwizzle(dxmt9::convert::toShaderReadSwizzle(Format::R32F),
               WMTTextureSwizzleRed, WMTTextureSwizzleOne,
               WMTTextureSwizzleOne, WMTTextureSwizzleOne,
               "R32F shader-read swizzle fills missing channels with one");
  checkSwizzle(dxmt9::convert::toShaderReadSwizzle(Format::G16R16),
               WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
               WMTTextureSwizzleOne, WMTTextureSwizzleOne,
               "G16R16 shader-read swizzle fills missing channels with one");
  checkSwizzle(dxmt9::convert::toShaderReadSwizzle(Format::G16R16F),
               WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
               WMTTextureSwizzleOne, WMTTextureSwizzleOne,
               "G16R16F shader-read swizzle fills missing channels with one");
  checkSwizzle(dxmt9::convert::toShaderReadSwizzle(Format::G32R32F),
               WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
               WMTTextureSwizzleOne, WMTTextureSwizzleOne,
               "G32R32F shader-read swizzle fills missing channels with one");
  checkSwizzle(dxmt9::convert::toShaderReadSwizzle(Format::L8),
               WMTTextureSwizzleRed, WMTTextureSwizzleRed,
               WMTTextureSwizzleRed, WMTTextureSwizzleOne,
               "L8 shader-read swizzle expands luminance to RGB and alpha one");
  checkSwizzle(dxmt9::convert::toShaderReadSwizzle(Format::L16),
               WMTTextureSwizzleRed, WMTTextureSwizzleRed,
               WMTTextureSwizzleRed, WMTTextureSwizzleOne,
               "L16 shader-read swizzle expands luminance to RGB and alpha one");
  checkSwizzle(dxmt9::convert::toShaderReadSwizzle(Format::A8L8),
               WMTTextureSwizzleRed, WMTTextureSwizzleRed,
               WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
               "A8L8 shader-read swizzle expands luminance and preserves alpha");

  TextureDesc normalDesc{64, 64, 1, 1, Format::A8R8G8B8, TextureType::TwoD,
                         Pool::Managed, UsageTexture};
  TextureDesc l8Desc = normalDesc;
  l8Desc.format = Format::L8;
  TextureDesc g16r16Desc = normalDesc;
  g16r16Desc.format = Format::G16R16;
  TextureDesc r32fDesc = normalDesc;
  r32fDesc.format = Format::R32F;
  check(!hasUsage(dxmt9::convert::toTextureUsage(normalDesc), WMTTextureUsagePixelFormatView),
        "ordinary textures do not request pixel-format views");
  check(hasUsage(dxmt9::convert::toTextureUsage(l8Desc), WMTTextureUsagePixelFormatView),
        "L8 textures request pixel-format views for shader-read swizzle");
  check(hasUsage(dxmt9::convert::toTextureUsage(g16r16Desc), WMTTextureUsagePixelFormatView),
        "G16R16 textures request pixel-format views for shader-read swizzle");
  check(hasUsage(dxmt9::convert::toTextureUsage(r32fDesc), WMTTextureUsagePixelFormatView),
        "R32F textures request pixel-format views for shader-read swizzle");
  checkEq(dxmt9::convert::toShaderReadViewSliceCount(TextureType::TwoD), std::uint16_t{1},
          "2D shader-read view covers one slice");
  checkEq(dxmt9::convert::toShaderReadViewSliceCount(TextureType::Cube), std::uint16_t{6},
          "cube shader-read view covers all six cube faces");
}

template <typename Table>
void checkNoDirty(const Table& table, std::string_view message) {
  for (const auto word : table.dirty) {
    check(word == 0u, message);
  }
}

template <typename Table>
void checkDirtyBit(const Table& table, u32 key, std::string_view message) {
  check((table.dirty[Table::word(key)] & Table::bit(key)) != 0u, message);
}

template <typename Table>
void testStateValueTableDirtyHashContract(std::string_view label, u32 key, u32 otherKey,
                                          u32 invalidKey) {
  Table table;
  Table same;

  check(table.empty(), label);
  checkEq(table.size(), std::size_t{0}, label);
  checkEq(table.rollingHash, 0ull, label);
  check(!table.contains(key), label);
  checkEq(table.valueOr(key, 99u), 99u, label);
  checkEq(table.at(key), 0u, label);
  checkNoDirty(table, label);

  table.set(key, 7u);
  same.set(key, 7u);
  check(table.contains(key), label);
  checkEq(table.size(), std::size_t{1}, label);
  checkEq(table[key], 7u, label);
  checkEq(table, same, label);
  checkEq(table.rollingHash, same.rollingHash, label);
  check(table.rollingHash != 0u, label);
  checkDirtyBit(table, key, label);

  const u64 insertHash = table.rollingHash;
  table.clearDirty();
  checkNoDirty(table, label);
  table.set(key, 7u);
  checkEq(table.rollingHash, insertHash, label);
  checkNoDirty(table, label);

  table[key] = 11u;
  checkEq(table[key], 11u, label);
  check(table.rollingHash != insertHash, label);
  checkDirtyBit(table, key, label);

  Table orderA;
  Table orderB;
  orderA.set(key, 1u);
  orderA.set(otherKey, 2u);
  orderB.set(otherKey, 2u);
  orderB.set(key, 1u);
  checkEq(orderA, orderB, label);
  checkEq(orderA.rollingHash, orderB.rollingHash, label);

  table.clearDirty();
  table.erase(key);
  check(table.empty(), label);
  check(!table.contains(key), label);
  checkEq(table.valueOr(key, 99u), 99u, label);
  checkEq(table.at(key), 0u, label);
  checkEq(table.rollingHash, 0ull, label);
  checkDirtyBit(table, key, label);

  table.clearDirty();
  table.erase(key);
  checkNoDirty(table, label);

  const Table beforeInvalid = table;
  table.set(invalidKey, 123u);
  table.erase(invalidKey);
  checkEq(table, beforeInvalid, label);
  checkNoDirty(table, label);

  table.set(key, 3u);
  table.clear();
  check(table.empty(), label);
  checkEq(table.rollingHash, 0ull, label);
  checkNoDirty(table, label);
}

Matrix4x4 taggedMatrix(float base) {
  Matrix4x4 matrix{};
  for (std::size_t i = 0; i < matrix.m.size(); ++i) {
    matrix.m[i] = base + static_cast<float>(i);
  }
  return matrix;
}

void testTransformTableDirtyHashContract() {
  TransformTable table;
  TransformTable same;
  const auto fallback = taggedMatrix(1000.0f);
  const auto view = taggedMatrix(10.0f);
  const auto updatedView = taggedMatrix(20.0f);

  check(table.empty(), "transform table starts empty");
  checkEq(table.size(), std::size_t{0}, "transform table size starts at zero");
  checkEq(table.rollingHash, 0ull, "transform table hash starts at zero");
  check(!table.contains(XFORM_VIEW), "transform table does not contain unset key");
  checkEq(table.valueOr(XFORM_VIEW, fallback), fallback, "transform table returns fallback for unset key");
  checkNoDirty(table, "transform table starts without dirty bits");

  table.set(XFORM_VIEW, view);
  same.set(XFORM_VIEW, view);
  check(table.contains(XFORM_VIEW), "transform table contains inserted key");
  checkEq(table.size(), std::size_t{1}, "transform table size increments on insert");
  checkEq(table[XFORM_VIEW], view, "transform table stores inserted matrix");
  checkEq(table, same, "transform table equality ignores dirty metadata");
  checkEq(table.rollingHash, same.rollingHash, "transform table hash is deterministic");
  check(table.rollingHash != 0u, "transform table hash changes on insert");
  checkDirtyBit(table, XFORM_VIEW, "transform table marks dirty on insert");

  const u64 insertHash = table.rollingHash;
  table.clearDirty();
  checkNoDirty(table, "transform table clears dirty metadata");
  table.set(XFORM_VIEW, view);
  checkEq(table.rollingHash, insertHash, "transform table redundant set keeps hash stable");
  checkNoDirty(table, "transform table redundant set does not dirty");

  table[XFORM_VIEW] = updatedView;
  checkEq(table[XFORM_VIEW], updatedView, "transform table proxy assignment updates value");
  check(table.rollingHash != insertHash, "transform table hash changes on update");
  checkDirtyBit(table, XFORM_VIEW, "transform table marks dirty on update");

  TransformTable orderA;
  TransformTable orderB;
  const auto world = taggedMatrix(30.0f);
  const auto texture = taggedMatrix(40.0f);
  orderA.set(XFORM_WORLD_BASE, world);
  orderA.set(XFORM_TEXTURE_BASE + 1u, texture);
  orderB.set(XFORM_TEXTURE_BASE + 1u, texture);
  orderB.set(XFORM_WORLD_BASE, world);
  checkEq(orderA, orderB, "transform table hash is insertion-order independent");
  checkEq(orderA.rollingHash, orderB.rollingHash, "transform table rolling hash is insertion-order independent");

  table.clearDirty();
  table.erase(XFORM_VIEW);
  check(table.empty(), "transform table erase removes only entry");
  check(!table.contains(XFORM_VIEW), "transform table erase clears occupied bit");
  checkEq(table.valueOr(XFORM_VIEW, fallback), fallback, "transform table erase restores fallback");
  checkEq(table[XFORM_VIEW], Matrix4x4{}, "transform table const lookup returns zero matrix after erase");
  checkEq(table.rollingHash, 0ull, "transform table hash returns to zero after erasing only entry");
  checkDirtyBit(table, XFORM_VIEW, "transform table marks dirty on erase");

  table.clearDirty();
  table.erase(XFORM_VIEW);
  checkNoDirty(table, "transform table redundant erase does not dirty");

  const TransformTable beforeInvalid = table;
  table.set(kMaxTransformSlots, taggedMatrix(50.0f));
  table.erase(kMaxTransformSlots);
  checkEq(table, beforeInvalid, "transform table ignores invalid keys");
  checkNoDirty(table, "transform table invalid key does not dirty");

  table.set(XFORM_VIEW, view);
  table.clear();
  check(table.empty(), "transform table clear removes entries");
  checkEq(table.rollingHash, 0ull, "transform table clear resets hash");
  checkNoDirty(table, "transform table clear resets dirty metadata");
}

void testStateValueTablesDirtyHashContracts() {
  testStateValueTableDirtyHashContract<RenderStateTable>(
      "render-state table dirty/hash contract", RS_Z_ENABLE, RS_ALPHA_TEST_ENABLE, kMaxStateSlots);
  testStateValueTableDirtyHashContract<TextureStageStateTable>(
      "texture-stage-state table dirty/hash contract", TSS_COLOR_OP, TSS_TEXCOORD_INDEX,
      kMaxTextureStageStates);
  testStateValueTableDirtyHashContract<SamplerStateTable>(
      "sampler-state table dirty/hash contract", SAMP_ADDRESS_U, SAMP_MAX_ANISOTROPY,
      kMaxSamplerStates);
  testTransformTableDirtyHashContract();
}

}  // namespace

int main() {
  testFormatTableCoverageAndClassifierPolicy();
  testFourCcAndPseudoFormatExplicitPolicy();
  testUnsupportedFormatBehavior();
  testYuvAndColorKeyPolicyAreExplicitlyUnsupported();
  testCompressedRenderTargetFactoryRejection();
  testD24S8FallbackPolicy();
  testShaderReadSwizzlePolicyForExpandedShaderReadFormats();
  testStateValueTablesDirtyHashContracts();

  std::cout << "dod_state_format_spec passed\n";
  return 0;
}
