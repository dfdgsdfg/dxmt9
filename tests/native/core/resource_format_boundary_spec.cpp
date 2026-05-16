#include "core_spec_fixtures.hpp"

#include <array>
#include <memory>
#include <string_view>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

constexpr u32 kD3DUsageRenderTarget = 0x00000001u;
constexpr u32 kD3DUsageDepthStencil = 0x00000002u;
constexpr u32 kD3DUsageDynamic = 0x00000200u;
constexpr u32 kD3DUsageAutoGenMipmap = 0x00000400u;

constexpr u32 kD3DPoolDefault = 0u;
constexpr u32 kD3DPoolManaged = 1u;
constexpr u32 kD3DPoolSystemMem = 2u;

constexpr u32 kD3DResourceTypeSurface = 1u;
constexpr u32 kD3DResourceTypeTexture = 3u;
constexpr u32 kD3DResourceTypeVolumeTexture = 4u;
constexpr u32 kD3DResourceTypeCubeTexture = 5u;

constexpr u32 kD3DMultiSampleNone = 0u;
constexpr u32 kD3DMultiSampleTwo = 2u;
constexpr u32 kD3DMultiSampleFour = 4u;

constexpr u32 kD3DFmtA8R8G8B8 = 21u;
constexpr u32 kD3DFmtX8R8G8B8 = 22u;
constexpr u32 kD3DFmtA8B8G8R8 = 32u;
constexpr u32 kD3DFmtX8B8G8R8 = 33u;
constexpr u32 kD3DFmtA2R10G10B10 = 35u;
constexpr u32 kD3DFmtA2B10G10R10 = 31u;
constexpr u32 kD3DFmtL8 = 50u;
constexpr u32 kD3DFmtA8L8 = 51u;
constexpr u32 kD3DFmtD24S8 = 75u;
constexpr u32 kD3DFmtD24X8 = 77u;
constexpr u32 kD3DFmtDXT5 = 894720068u;

// Native unit tests can drive the public D9C creation calls into core
// resources and the recording backend. They cannot see the final
// WMTTextureInfo assembled inside resources::Pool because RecordingBackend
// captures only TextureDesc/SurfaceDesc before that Metal-side descriptor
// seam. For those values, assert the exact WMT conversion policy from the
// created descriptor; a future resource-pool observer is needed to prove the
// post-Pool Metal descriptor directly.
//
// D3D9 sRGB read/write is not represented as a resource creation field in
// TextureDesc/SurfaceDesc. Creation can only prove that the resource format is
// sRGB-compatible, so these assertions check the exact sRGB pixel-format
// conversion selected from the created descriptor.

BackendLimits defaultLimits() {
  BackendLimits limits{};
  limits.supportsDepth24Stencil8 = true;
  limits.supportsDepth32FloatStencil8 = true;
  limits.supportsBgr10A2 = true;
  return limits;
}

bool hasCoreUsage(u32 usage, u32 flag) {
  return (usage & flag) != 0u;
}

bool hasWmtUsage(WMTTextureUsage usage, WMTTextureUsage flag) {
  return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0u;
}

void checkSwizzle(WMTTextureSwizzleChannels actual, WMTTextureSwizzle r,
                  WMTTextureSwizzle g, WMTTextureSwizzle b,
                  WMTTextureSwizzle a, std::string_view message) {
  checkEq(actual.r, r, message);
  checkEq(actual.g, g, message);
  checkEq(actual.b, b, message);
  checkEq(actual.a, a, message);
}

void checkBytesPrefix(std::span<const u8> actual, std::span<const u8> expected,
                      std::string_view message) {
  check(actual.size() >= expected.size(), message);
  checkBytes(actual.first(expected.size()), expected, message);
}

struct TextureDeleter {
  void operator()(D9CTexture* texture) const {
    if (texture) {
      dxmt9c_texture_release(texture);
    }
  }
};

struct SurfaceDeleter {
  void operator()(D9CSurface* surface) const {
    if (surface) {
      dxmt9c_surface_release(surface);
    }
  }
};

using UniqueTexture = std::unique_ptr<D9CTexture, TextureDeleter>;
using UniqueSurface = std::unique_ptr<D9CSurface, SurfaceDeleter>;

struct PublicDevice {
  PublicDevice() {
    backend = std::make_shared<RecordingBackend>();
    d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION, backend);
    check(d3d != nullptr, "create public D3D9Ex factory");

    PresentParameters params{};
    params.backBufferWidth = 64;
    params.backBufferHeight = 64;
    params.backBufferFormat = Format::A8R8G8B8;
    params.windowed = true;
    params.presentationInterval = PresentInterval::Immediate;
    params.deviceWindow = Handle{0xb300u};

    device = d3d->CreateDeviceEx(0, params);
    check(device != nullptr, "create public D3D9Ex device");
    cDevice = std::make_unique<D9CDevice>(device);
  }

  PublicDevice(const PublicDevice&) = delete;
  PublicDevice& operator=(const PublicDevice&) = delete;

  ~PublicDevice() {
    cDevice.reset();
    if (d3d) {
      d3d->Release();
    }
  }

  D9CDevice* c() const noexcept {
    return cDevice.get();
  }

  std::shared_ptr<RecordingBackend> backend;
  dxmt9::com::IDirect3D9Ex* d3d = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* device = nullptr;
  std::unique_ptr<D9CDevice> cDevice;
};

D9CSurfaceDesc textureLevelDesc(D9CTexture* texture, u32 level,
                                std::string_view message) {
  D9CSurfaceDesc desc{};
  checkEq(dxmt9c_texture_get_level_desc(texture, level, &desc), D3D_OK,
          message);
  return desc;
}

D9CSurfaceDesc surfaceDesc(D9CSurface* surface, std::string_view message) {
  D9CSurfaceDesc desc{};
  checkEq(dxmt9c_surface_get_desc(surface, &desc), D3D_OK, message);
  return desc;
}

void testPublicTextureCreationPreservesD3DValuesAndUploadPitch() {
  PublicDevice fixture;
  const auto before = fixture.backend->createdTextures.size();

  auto texture = UniqueTexture(dxmt9c_device_create_texture(
      fixture.c(), 13, 5, 3,
      kD3DUsageDynamic | kD3DUsageAutoGenMipmap,
      kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(texture != nullptr, "public A8R8G8B8 texture creation succeeds");
  checkEq(fixture.backend->createdTextures.size(), before + size_t{1},
          "public texture reaches backend creation path");

  const auto& backendDesc = fixture.backend->createdTextures[before];
  checkEq(backendDesc.width, 13u, "backend texture width");
  checkEq(backendDesc.height, 5u, "backend texture height");
  checkEq(backendDesc.depth, 1u, "backend 2D texture depth");
  checkEq(backendDesc.levels, 3u, "backend texture levels");
  checkEq(backendDesc.format, Format::A8R8G8B8, "backend core format");
  checkEq(backendDesc.type, TextureType::TwoD, "backend texture type");
  checkEq(backendDesc.pool, Pool::Managed, "backend pool");
  check(hasCoreUsage(backendDesc.usage, UsageDynamic),
        "backend dynamic usage bit");
  check(hasCoreUsage(backendDesc.usage, UsageAutoGenMipmap),
        "backend autogen usage bit");

  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtA8R8G8B8),
          Format::A8R8G8B8, "D3DFORMAT numeric value maps to core format");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(backendDesc.format),
          kD3DFmtA8R8G8B8, "core format maps back to original D3DFORMAT");

  const auto level0 = textureLevelDesc(texture.get(), 0, "texture level0 desc");
  checkEq(level0.format, kD3DFmtA8R8G8B8, "level0 D3DFORMAT identity");
  checkEq(level0.resourceType, kD3DResourceTypeTexture, "level0 resource type");
  checkEq(level0.usage, kD3DUsageDynamic | kD3DUsageAutoGenMipmap,
          "level0 D3D usage round-trip");
  checkEq(level0.pool, kD3DPoolManaged, "level0 pool round-trip");
  checkEq(level0.multiSampleType, kD3DMultiSampleNone,
          "texture levels are non-MSAA");
  checkEq(level0.width, 13u, "level0 width");
  checkEq(level0.height, 5u, "level0 height");

  const auto level1 = textureLevelDesc(texture.get(), 1, "texture level1 desc");
  checkEq(level1.width, 6u, "level1 width truncates by mip shift");
  checkEq(level1.height, 2u, "level1 height truncates by mip shift");

  texture->obj->fillColor(nullptr, ColorRGBA{1.0f, 0.5f, 0.0f, 0.25f});
  const std::array<u8, 4> bgraPixel{0x00u, 0x80u, 0xffu, 0x40u};
  checkBytesPrefix(texture->obj->levelBytes(0),
                   std::span<const u8>(bgraPixel.data(), bgraPixel.size()),
                   "A8R8G8B8 stores D3D ARGB as BGRA bytes");

  const auto uploadBefore = fixture.backend->textureUploads.size();
  D9CLockedRect lock{};
  checkEq(dxmt9c_texture_lock_rect(texture.get(), 0, &lock, nullptr, 0),
          D3D_OK, "public texture lock succeeds");
  check(lock.bits != nullptr, "public texture lock returns bits");
  checkEq(lock.pitch, 52, "A8R8G8B8 pitch is width * 4 bytes");
  checkEq(dxmt9c_texture_unlock_rect(texture.get(), 0), D3D_OK,
          "public texture unlock succeeds");
  checkEq(fixture.backend->textureUploads.size(), uploadBefore + size_t{1},
          "unlock uploads the created texture level to backend");

  const auto& upload = fixture.backend->textureUploads.back();
  checkEq(upload.width, 13u, "upload width");
  checkEq(upload.height, 5u, "upload height");
  checkEq(upload.level, 0u, "upload level");
  checkEq(upload.pitch, 52u, "upload pitch");
  checkBytesPrefix(std::span<const u8>(upload.bytes.data(), upload.bytes.size()),
                   std::span<const u8>(bgraPixel.data(), bgraPixel.size()),
                   "upload preserves A8R8G8B8 component order");
}

void testComponentOrderAlphaAndSrgbCompatibility() {
  struct Case {
    u32 d3dFormat = 0;
    Format coreFormat = Format::Unknown;
    WMTPixelFormat linear = WMTPixelFormatInvalid;
    WMTPixelFormat srgb = WMTPixelFormatInvalid;
    std::array<u8, 4> expectedBytes{};
    const char* label = "";
  };

  const std::array<Case, 5> cases{{
      {kD3DFmtA8R8G8B8, Format::A8R8G8B8, WMTPixelFormatBGRA8Unorm,
       WMTPixelFormatBGRA8Unorm_sRGB, {0x00u, 0x80u, 0xffu, 0x40u},
       "A8R8G8B8"},
      {kD3DFmtX8R8G8B8, Format::X8R8G8B8, WMTPixelFormatBGRA8Unorm,
       WMTPixelFormatBGRA8Unorm_sRGB, {0x00u, 0x80u, 0xffu, 0xffu},
       "X8R8G8B8"},
      {kD3DFmtA8B8G8R8, Format::A8B8G8R8, WMTPixelFormatRGBA8Unorm,
       WMTPixelFormatRGBA8Unorm_sRGB, {0xffu, 0x80u, 0x00u, 0x40u},
       "A8B8G8R8"},
      {kD3DFmtX8B8G8R8, Format::X8B8G8R8, WMTPixelFormatRGBA8Unorm,
       WMTPixelFormatRGBA8Unorm_sRGB, {0xffu, 0x80u, 0x00u, 0xffu},
       "X8B8G8R8"},
      {kD3DFmtA2B10G10R10, Format::A2B10G10R10, WMTPixelFormatBGR10A2Unorm,
       WMTPixelFormatBGR10A2Unorm, {0xffu, 0x03u, 0x08u, 0x40u},
       "A2B10G10R10"},
  }};

  PublicDevice fixture;
  for (const auto& testCase : cases) {
    const auto before = fixture.backend->createdTextures.size();
    auto texture = UniqueTexture(dxmt9c_device_create_texture(
        fixture.c(), 2, 1, 1, 0, testCase.d3dFormat, kD3DPoolManaged));
    check(texture != nullptr, testCase.label);
    checkEq(fixture.backend->createdTextures.size(), before + size_t{1},
            testCase.label);

    const auto& backendDesc = fixture.backend->createdTextures[before];
    checkEq(backendDesc.format, testCase.coreFormat, testCase.label);
    checkEq(dxmt9::d3d9::devicec::fmtToD3D(backendDesc.format),
            testCase.d3dFormat, testCase.label);
    checkEq(dxmt9::convert::toPixelFormat(backendDesc.format, defaultLimits()),
            testCase.linear, testCase.label);
    checkEq(dxmt9::convert::toPixelFormat(backendDesc.format, defaultLimits(), true),
            testCase.srgb, testCase.label);

    const auto d3dDesc = textureLevelDesc(texture.get(), 0, testCase.label);
    checkEq(d3dDesc.format, testCase.d3dFormat, testCase.label);
    checkEq(d3dDesc.resourceType, kD3DResourceTypeTexture, testCase.label);

    texture->obj->fillColor(nullptr, ColorRGBA{1.0f, 0.5f, 0.0f, 0.25f});
    checkBytesPrefix(texture->obj->levelBytes(0),
                     std::span<const u8>(testCase.expectedBytes.data(),
                                         testCase.expectedBytes.size()),
                     testCase.label);
  }

  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtA2R10G10B10),
          Format::A2R10G10B10, "A2R10G10B10 D3DFORMAT maps to core");
  checkEq(dxmt9::convert::toPixelFormat(Format::A2R10G10B10, defaultLimits()),
          WMTPixelFormatRGB10A2Unorm,
          "A2R10G10B10 keeps RGB10A2 channel order for Metal");
}

void testLuminanceDefaultChannelPolicy() {
  struct Case {
    u32 d3dFormat = 0;
    Format coreFormat = Format::Unknown;
    WMTPixelFormat pixelFormat = WMTPixelFormatInvalid;
    WMTTextureSwizzle alphaSwizzle = WMTTextureSwizzleAlpha;
    std::array<u8, 2> expectedBytes{};
    size_t expectedByteCount = 0;
    const char* label = "";
  };

  const std::array<Case, 2> cases{{
      {kD3DFmtL8, Format::L8, WMTPixelFormatR8Unorm,
       WMTTextureSwizzleOne, {0x55u, 0x00u}, 1u, "L8"},
      {kD3DFmtA8L8, Format::A8L8, WMTPixelFormatRG8Unorm,
       WMTTextureSwizzleGreen, {0x55u, 0x40u}, 2u, "A8L8"},
  }};

  PublicDevice fixture;
  for (const auto& testCase : cases) {
    const auto before = fixture.backend->createdTextures.size();
    auto texture = UniqueTexture(dxmt9c_device_create_texture(
        fixture.c(), 4, 4, 1, 0, testCase.d3dFormat, kD3DPoolManaged));
    check(texture != nullptr, testCase.label);
    checkEq(fixture.backend->createdTextures.size(), before + size_t{1},
            testCase.label);

    const auto& backendDesc = fixture.backend->createdTextures[before];
    checkEq(backendDesc.format, testCase.coreFormat, testCase.label);
    checkEq(dxmt9::convert::toPixelFormat(backendDesc.format, defaultLimits()),
            testCase.pixelFormat, testCase.label);
    check(dxmt9::convert::formatNeedsShaderReadSwizzle(backendDesc.format),
          testCase.label);
    check(hasWmtUsage(dxmt9::convert::toTextureUsage(backendDesc),
                      WMTTextureUsagePixelFormatView),
          testCase.label);
    checkSwizzle(dxmt9::convert::toShaderReadSwizzle(backendDesc.format),
                 WMTTextureSwizzleRed, WMTTextureSwizzleRed,
                 WMTTextureSwizzleRed, testCase.alphaSwizzle,
                 testCase.label);

    texture->obj->fillColor(nullptr, ColorRGBA{1.0f, 0.0f, 0.0f, 0.25f});
    checkBytesPrefix(texture->obj->levelBytes(0),
                     std::span<const u8>(testCase.expectedBytes.data(),
                                         testCase.expectedByteCount),
                     testCase.label);
  }
}

void testCompressedCreationAndBlockRowRounding() {
  PublicDevice fixture;
  const auto before = fixture.backend->createdTextures.size();

  auto texture = UniqueTexture(dxmt9c_device_create_texture(
      fixture.c(), 16, 16, 5, 0, kD3DFmtDXT5, kD3DPoolManaged));
  check(texture != nullptr, "public DXT5 texture creation succeeds");
  checkEq(fixture.backend->createdTextures.size(), before + size_t{1},
          "DXT5 creation reaches backend");
  checkEq(dxmt9c_texture_get_level_count(texture.get()), 5u,
          "DXT5 explicit mip level count");

  const auto& backendDesc = fixture.backend->createdTextures[before];
  checkEq(backendDesc.format, Format::DXT5, "DXT5 backend format");
  checkEq(backendDesc.width, 16u, "DXT5 backend width");
  checkEq(backendDesc.height, 16u, "DXT5 backend height");
  checkEq(backendDesc.levels, 5u, "DXT5 backend levels");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(backendDesc.format),
          kD3DFmtDXT5, "DXT5 D3DFORMAT round-trip");
  checkEq(dxmt9::convert::toPixelFormat(backendDesc.format, defaultLimits()),
          WMTPixelFormatBC3_RGBA, "DXT5 Metal BC3 format");
  checkEq(dxmt9::convert::toPixelFormat(backendDesc.format, defaultLimits(), true),
          WMTPixelFormatBC3_RGBA_sRGB, "DXT5 sRGB-compatible Metal format");

  const auto level0 = textureLevelDesc(texture.get(), 0, "DXT5 level0 desc");
  checkEq(level0.format, kD3DFmtDXT5, "DXT5 level0 format");
  checkEq(level0.width, 16u, "DXT5 level0 width");
  checkEq(level0.height, 16u, "DXT5 level0 height");
  const auto level4 = textureLevelDesc(texture.get(), 4, "DXT5 level4 desc");
  checkEq(level4.width, 1u, "DXT5 level4 width clamps to one texel");
  checkEq(level4.height, 1u, "DXT5 level4 height clamps to one texel");

  checkEq(formatBlockWidth(Format::DXT5), 4u, "DXT5 block width");
  checkEq(formatBlockHeight(Format::DXT5), 4u, "DXT5 block height");
  checkEq(formatBlockBytes(Format::DXT5), 16u, "DXT5 block bytes");
  checkEq(formatRowPitch(Format::DXT5, 16u), 64u,
          "DXT5 level0 row pitch");
  checkEq(formatRowPitch(Format::DXT5, 1u), 16u,
          "DXT5 1x1 mip row pitch rounds to one block");
  checkEq(texture->obj->levelBytes(0).size(), size_t{256},
          "DXT5 level0 storage bytes");
  checkEq(texture->obj->levelBytes(4).size(), size_t{16},
          "DXT5 1x1 mip storage rounds to one 4x4 block");

  const auto uploadBefore = fixture.backend->textureUploads.size();
  D9CLockedRect lock{};
  checkEq(dxmt9c_texture_lock_rect(texture.get(), 4, &lock, nullptr, 0),
          D3D_OK, "DXT5 1x1 mip lock succeeds");
  check(lock.bits != nullptr, "DXT5 1x1 mip lock bits");
  checkEq(lock.pitch, 16, "DXT5 1x1 mip lock pitch");
  checkEq(dxmt9c_texture_unlock_rect(texture.get(), 4), D3D_OK,
          "DXT5 1x1 mip unlock succeeds");
  checkEq(fixture.backend->textureUploads.size(), uploadBefore + size_t{1},
          "DXT5 unlock uploads rounded mip");
  const auto& upload = fixture.backend->textureUploads.back();
  checkEq(upload.width, 1u, "DXT5 upload width");
  checkEq(upload.height, 1u, "DXT5 upload height");
  checkEq(upload.pitch, 16u, "DXT5 upload pitch");
  checkEq(upload.bytes.size(), size_t{16}, "DXT5 upload byte count");

  const auto invalidBefore = fixture.backend->createdTextures.size();
  auto* invalid = dxmt9c_device_create_texture(
      fixture.c(), 18, 16, 1, 0, kD3DFmtDXT5, kD3DPoolManaged);
  check(invalid == nullptr, "public DXT5 rejects non-block-aligned width");
  checkEq(fixture.backend->createdTextures.size(), invalidBefore,
          "invalid compressed texture does not reach backend");
}

void testCubeAndVolumeCreationDescriptors() {
  PublicDevice fixture;

  const auto cubeBefore = fixture.backend->createdTextures.size();
  auto cube = UniqueTexture(dxmt9c_device_create_cube_texture(
      fixture.c(), 8, 4, kD3DUsageRenderTarget,
      kD3DFmtA8R8G8B8, kD3DPoolDefault));
  check(cube != nullptr, "public cube texture creation succeeds");
  checkEq(fixture.backend->createdTextures.size(), cubeBefore + size_t{1},
          "cube creation reaches backend");
  const auto& cubeDesc = fixture.backend->createdTextures[cubeBefore];
  checkEq(cubeDesc.type, TextureType::Cube, "cube backend type");
  checkEq(cubeDesc.width, 8u, "cube backend width");
  checkEq(cubeDesc.height, 8u, "cube backend height");
  checkEq(cubeDesc.levels, 4u, "cube backend levels");
  checkEq(cubeDesc.pool, Pool::Default, "cube backend pool");
  check(hasCoreUsage(cubeDesc.usage, UsageRenderTarget),
        "cube backend render-target usage");

  const auto cubeLevel2 = textureLevelDesc(cube.get(), 2, "cube level2 desc");
  checkEq(cubeLevel2.resourceType, kD3DResourceTypeCubeTexture,
          "cube public resource type");
  checkEq(cubeLevel2.format, kD3DFmtA8R8G8B8, "cube public format");
  checkEq(cubeLevel2.usage, kD3DUsageRenderTarget, "cube public usage");
  checkEq(cubeLevel2.pool, kD3DPoolDefault, "cube public pool");
  checkEq(cubeLevel2.width, 2u, "cube level2 width");
  checkEq(cubeLevel2.height, 2u, "cube level2 height");

  const auto surfaceBefore = fixture.backend->textureSurfaces.size();
  auto cubeSurface = UniqueSurface(dxmt9c_texture_get_surface_level(cube.get(), 5));
  check(cubeSurface != nullptr, "cube face surface creation succeeds");
  checkEq(fixture.backend->textureSurfaces.size(), surfaceBefore + size_t{1},
          "cube face reaches backend surface-for-texture path");
  const auto& cubeSurfaceRecord = fixture.backend->textureSurfaces.back();
  checkEq(cubeSurfaceRecord.texture, cube->obj->handle(),
          "cube surface aliases parent texture handle");
  checkEq(cubeSurfaceRecord.subresource, 5u, "cube surface subresource");
  checkEq(cubeSurfaceRecord.desc.width, 4u, "cube face mip width");
  checkEq(cubeSurfaceRecord.desc.height, 4u, "cube face mip height");
  checkEq(cubeSurfaceRecord.desc.format, Format::A8R8G8B8,
          "cube face surface format");
  check(cubeSurfaceRecord.desc.renderTarget, "cube face render-target flag");

  const auto volumeBefore = fixture.backend->createdTextures.size();
  auto volume = UniqueTexture(dxmt9c_device_create_volume_texture(
      fixture.c(), 8, 4, 3, 2, 0, kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(volume != nullptr, "public volume texture creation succeeds");
  checkEq(fixture.backend->createdTextures.size(), volumeBefore + size_t{1},
          "volume creation reaches backend");
  const auto& volumeDesc = fixture.backend->createdTextures[volumeBefore];
  checkEq(volumeDesc.type, TextureType::Volume, "volume backend type");
  checkEq(volumeDesc.width, 8u, "volume backend width");
  checkEq(volumeDesc.height, 4u, "volume backend height");
  checkEq(volumeDesc.depth, 3u, "volume backend depth");
  checkEq(volumeDesc.levels, 2u, "volume backend levels");
  checkEq(volumeDesc.pool, Pool::Managed, "volume backend pool");

  const auto volumeLevel1 = textureLevelDesc(volume.get(), 1, "volume level1 desc");
  checkEq(volumeLevel1.resourceType, kD3DResourceTypeVolumeTexture,
          "volume public resource type");
  checkEq(volumeLevel1.format, kD3DFmtA8R8G8B8, "volume public format");
  checkEq(volumeLevel1.width, 4u, "volume level1 width");
  checkEq(volumeLevel1.height, 2u, "volume level1 height");
  // D9CSurfaceDesc has no depth field and this native D9C layer exposes no
  // volume lock/box path, so backend TextureDesc::depth is the observable
  // creation boundary for volume depth in this unit.
}

void testSurfaceDescriptorsMultisampleDepthFallbackAndOffscreenPitch() {
  PublicDevice fixture;

  const auto rtBefore = fixture.backend->createdSurfaces.size();
  auto renderTarget = UniqueSurface(dxmt9c_device_create_render_target(
      fixture.c(), 33, 17, kD3DFmtA8B8G8R8,
      kD3DMultiSampleFour, 0, 0, nullptr));
  check(renderTarget != nullptr, "public render-target creation succeeds");
  checkEq(fixture.backend->createdSurfaces.size(), rtBefore + size_t{1},
          "render target reaches backend");
  const auto& rtDesc = fixture.backend->createdSurfaces[rtBefore];
  checkEq(rtDesc.width, 33u, "render target backend width");
  checkEq(rtDesc.height, 17u, "render target backend height");
  checkEq(rtDesc.format, Format::A8B8G8R8, "render target backend format");
  check(rtDesc.renderTarget, "render target backend flag");
  check(!rtDesc.depthStencil, "render target is not depth-stencil");
  checkEq(rtDesc.multiSampleType, MultiSampleType::Four,
          "render target backend multisample type");
  checkEq(renderTarget->obj->multiSampleCount(), 4u,
          "render target sample count");
  check(hasWmtUsage(dxmt9::convert::toTextureUsage(rtDesc),
                    WMTTextureUsageRenderTarget),
        "render target WMT usage includes render target");
  check(hasWmtUsage(dxmt9::convert::toTextureUsage(rtDesc),
                    WMTTextureUsageShaderRead),
        "render target WMT usage includes shader read");
  checkEq(dxmt9::convert::toPixelFormat(rtDesc.format, defaultLimits()),
          WMTPixelFormatRGBA8Unorm, "render target Metal pixel format");
  checkEq(dxmt9::convert::toPixelFormat(rtDesc.format, defaultLimits(), true),
          WMTPixelFormatRGBA8Unorm_sRGB,
          "render target sRGB-compatible pixel format");

  const auto rtPublic = surfaceDesc(renderTarget.get(), "render target public desc");
  checkEq(rtPublic.format, kD3DFmtA8B8G8R8, "render target public format");
  checkEq(rtPublic.resourceType, kD3DResourceTypeSurface,
          "render target resource type");
  checkEq(rtPublic.usage, kD3DUsageRenderTarget,
          "render target public usage");
  checkEq(rtPublic.pool, kD3DPoolDefault, "render target public pool");
  checkEq(rtPublic.multiSampleType, kD3DMultiSampleFour,
          "render target public multisample type");
  checkEq(rtPublic.multiSampleQuality, 1u,
          "render target public multisample quality");
  checkEq(rtPublic.width, 33u, "render target public width");
  checkEq(rtPublic.height, 17u, "render target public height");

  const auto dsBefore = fixture.backend->createdSurfaces.size();
  auto depth = UniqueSurface(dxmt9c_device_create_depth_stencil(
      fixture.c(), 64, 32, kD3DFmtD24S8,
      kD3DMultiSampleTwo, 0, 0, nullptr));
  check(depth != nullptr, "public depth-stencil creation succeeds");
  checkEq(fixture.backend->createdSurfaces.size(), dsBefore + size_t{1},
          "depth-stencil reaches backend");
  const auto& dsDesc = fixture.backend->createdSurfaces[dsBefore];
  checkEq(dsDesc.format, Format::D24S8, "depth-stencil backend format");
  check(!dsDesc.renderTarget, "depth-stencil is not render target");
  check(dsDesc.depthStencil, "depth-stencil backend flag");
  checkEq(dsDesc.multiSampleType, MultiSampleType::Two,
          "depth-stencil backend multisample type");
  checkEq(depth->obj->multiSampleCount(), 2u, "depth-stencil sample count");
  check(dxmt9::convert::formatHasDepthAspect(dsDesc.format),
        "D24S8 has depth aspect");
  check(dxmt9::convert::formatHasStencilAspect(dsDesc.format),
        "D24S8 has stencil aspect");

  BackendLimits nativeDepth = defaultLimits();
  checkEq(dxmt9::convert::toPixelFormat(dsDesc.format, nativeDepth),
          WMTPixelFormatDepth24Unorm_Stencil8,
          "D24S8 native Depth24Stencil8 pixel format");
  BackendLimits noDepth24 = nativeDepth;
  noDepth24.supportsDepth24Stencil8 = false;
  checkEq(dxmt9::convert::toPixelFormat(dsDesc.format, noDepth24),
          WMTPixelFormatDepth32Float_Stencil8,
          "D24S8 fallback Depth32FloatStencil8 pixel format");
  BackendLimits depthOnly = noDepth24;
  depthOnly.supportsDepth32FloatStencil8 = false;
  checkEq(dxmt9::convert::toPixelFormat(dsDesc.format, depthOnly),
          WMTPixelFormatDepth32Float,
          "D24S8 fallback depth-only pixel format");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtD24X8),
          Format::D24X8, "D24X8 D3DFORMAT maps to core");
  check(!dxmt9::convert::formatHasStencilAspect(Format::D24X8),
        "D24X8 has no stencil aspect even when storage fallback may include one");
  checkEq(dxmt9::convert::toPixelFormat(Format::D24X8, noDepth24),
          WMTPixelFormatDepth32Float_Stencil8,
          "D24X8 follows D24S8 fallback storage policy");

  const auto dsPublic = surfaceDesc(depth.get(), "depth-stencil public desc");
  checkEq(dsPublic.format, kD3DFmtD24S8, "depth-stencil public format");
  checkEq(dsPublic.resourceType, kD3DResourceTypeSurface,
          "depth-stencil resource type");
  checkEq(dsPublic.usage, kD3DUsageDepthStencil,
          "depth-stencil public usage");
  checkEq(dsPublic.pool, kD3DPoolDefault, "depth-stencil public pool");
  checkEq(dsPublic.multiSampleType, kD3DMultiSampleTwo,
          "depth-stencil public multisample type");
  checkEq(dsPublic.multiSampleQuality, 1u,
          "depth-stencil public multisample quality");
  checkEq(dsPublic.width, 64u, "depth-stencil public width");
  checkEq(dsPublic.height, 32u, "depth-stencil public height");

  const auto offscreenBefore = fixture.backend->createdSurfaces.size();
  auto offscreen = UniqueSurface(dxmt9c_device_create_offscreen_surface(
      fixture.c(), 7, 3, kD3DFmtA8R8G8B8, kD3DPoolSystemMem, nullptr));
  check(offscreen != nullptr, "public offscreen surface creation succeeds");
  checkEq(fixture.backend->createdSurfaces.size(), offscreenBefore + size_t{1},
          "offscreen surface reaches backend");
  const auto& offscreenDesc = fixture.backend->createdSurfaces[offscreenBefore];
  checkEq(offscreenDesc.format, Format::A8R8G8B8, "offscreen backend format");
  checkEq(offscreenDesc.pool, Pool::SystemMem, "offscreen backend pool");
  check(!offscreenDesc.renderTarget, "offscreen is not render target");
  check(!offscreenDesc.depthStencil, "offscreen is not depth-stencil");

  const auto offscreenPublic = surfaceDesc(offscreen.get(), "offscreen public desc");
  checkEq(offscreenPublic.format, kD3DFmtA8R8G8B8, "offscreen public format");
  checkEq(offscreenPublic.resourceType, kD3DResourceTypeSurface,
          "offscreen resource type");
  checkEq(offscreenPublic.usage, 0u, "offscreen public usage");
  checkEq(offscreenPublic.pool, kD3DPoolSystemMem, "offscreen public pool");
  checkEq(offscreenPublic.multiSampleType, kD3DMultiSampleNone,
          "offscreen public multisample type");
  checkEq(offscreenPublic.multiSampleQuality, 0u,
          "offscreen public multisample quality");
  checkEq(offscreenPublic.width, 7u, "offscreen public width");
  checkEq(offscreenPublic.height, 3u, "offscreen public height");

  D9CLockedRect lock{};
  checkEq(dxmt9c_surface_lock_rect(offscreen.get(), &lock, nullptr, 0),
          D3D_OK, "offscreen surface lock succeeds");
  check(lock.bits != nullptr, "offscreen surface lock bits");
  checkEq(lock.pitch, 28, "offscreen A8R8G8B8 pitch is width * 4");
  checkEq(dxmt9c_surface_unlock_rect(offscreen.get()), D3D_OK,
          "offscreen surface unlock succeeds");
}

}  // namespace

int main() {
  try {
    testPublicTextureCreationPreservesD3DValuesAndUploadPitch();
    testComponentOrderAlphaAndSrgbCompatibility();
    testLuminanceDefaultChannelPolicy();
    testCompressedCreationAndBlockRowRounding();
    testCubeAndVolumeCreationDescriptors();
    testSurfaceDescriptorsMultisampleDepthFallbackAndOffscreenPitch();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
