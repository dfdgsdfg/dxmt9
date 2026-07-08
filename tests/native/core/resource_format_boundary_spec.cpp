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
constexpr u32 kD3DFmtG16R16 = 34u;
constexpr u32 kD3DFmtL8 = 50u;
constexpr u32 kD3DFmtA8L8 = 51u;
constexpr u32 kD3DFmtR16F = 111u;
constexpr u32 kD3DFmtG16R16F = 112u;
constexpr u32 kD3DFmtR32F = 114u;
constexpr u32 kD3DFmtG32R32F = 115u;
constexpr u32 kD3DFmtD24S8 = 75u;
constexpr u32 kD3DFmtD24X8 = 77u;
constexpr u32 kD3DFmtDXT5 = 894720068u;
// FOURCC 'INTZ' = ('I')|('N'<<8)|('T'<<16)|('Z'<<24) = 0x5A544E49.
constexpr u32 kD3DFmtINTZ = 1515474505u;
// FOURCC 'NULL' = ('N')|('U'<<8)|('L'<<16)|('L'<<24) = 0x4C4C554E.
constexpr u32 kD3DFmtNULL = 1280070990u;

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

void testNpotMiptreeLayoutBoundaryValues() {
  PublicDevice fixture;
  const auto before = fixture.backend->createdTextures.size();

  auto texture = UniqueTexture(dxmt9c_device_create_texture(
      fixture.c(), 13, 9, 4, 0, kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(texture != nullptr, "NPOT mip texture creation succeeds");
  checkEq(fixture.backend->createdTextures.size(), before + size_t{1},
          "NPOT mip texture reaches backend creation path");
  checkEq(dxmt9c_texture_get_level_count(texture.get()), 4u,
          "NPOT explicit mip level count");

  const auto& backendDesc = fixture.backend->createdTextures[before];
  checkEq(backendDesc.width, 13u, "NPOT backend width");
  checkEq(backendDesc.height, 9u, "NPOT backend height");
  checkEq(backendDesc.levels, 4u, "NPOT backend level count");

  struct ExpectedLevel {
    u32 width = 0;
    u32 height = 0;
    int32_t pitch = 0;
    size_t byteCount = 0;
  };
  const std::array<ExpectedLevel, 4> levels{{
      {13u, 9u, 52, 468u},
      {6u, 4u, 24, 96u},
      {3u, 2u, 12, 24u},
      {1u, 1u, 4, 4u},
  }};

  for (u32 level = 0; level < levels.size(); ++level) {
    const auto desc = textureLevelDesc(texture.get(), level, "NPOT level desc");
    checkEq(desc.format, kD3DFmtA8R8G8B8, "NPOT level format");
    checkEq(desc.resourceType, kD3DResourceTypeTexture,
            "NPOT level resource type");
    checkEq(desc.width, levels[level].width, "NPOT level width");
    checkEq(desc.height, levels[level].height, "NPOT level height");
    checkEq(texture->obj->levelBytes(level).size(), levels[level].byteCount,
            "NPOT level storage byte count");

    D9CLockedRect lock{};
    checkEq(dxmt9c_texture_lock_rect(texture.get(), level, &lock, nullptr, 0),
            D3D_OK, "NPOT level lock succeeds");
    check(lock.bits != nullptr, "NPOT level lock bits");
    checkEq(lock.pitch, levels[level].pitch, "NPOT level lock pitch");
    checkEq(dxmt9c_texture_unlock_rect(texture.get(), level), D3D_OK,
            "NPOT level unlock succeeds");
  }

  D9CSurfaceDesc invalidDesc{};
  invalidDesc.width = 0xaaaaaaaau;
  checkEq(dxmt9c_texture_get_level_desc(texture.get(), 4, &invalidDesc),
          D3DERR_INVALIDCALL, "NPOT level past end is rejected");
  checkEq(invalidDesc.width, 0u,
          "NPOT invalid level desc is cleared before failure");
}

void testZeroLevelRequestsCreateFullMipChains() {
  PublicDevice fixture;

  const auto textureBefore = fixture.backend->createdTextures.size();
  auto texture = UniqueTexture(dxmt9c_device_create_texture(
      fixture.c(), 17, 9, 0, 0, kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(texture != nullptr, "2D zero-level texture creation succeeds");
  checkEq(fixture.backend->createdTextures.size(), textureBefore + size_t{1},
          "2D zero-level texture reaches backend");
  checkEq(dxmt9c_texture_get_level_count(texture.get()), 5u,
          "2D zero-level request expands to full mip chain");
  checkEq(fixture.backend->createdTextures[textureBefore].levels, 5u,
          "2D backend descriptor receives full mip count");
  const auto textureLevel4 = textureLevelDesc(texture.get(), 4,
                                             "2D zero-level final mip desc");
  checkEq(textureLevel4.width, 1u, "2D final mip width clamps to one");
  checkEq(textureLevel4.height, 1u, "2D final mip height clamps to one");
  checkEq(texture->obj->levelBytes(4).size(), size_t{4},
          "2D final mip owns one BGRA texel");

  const auto cubeBefore = fixture.backend->createdTextures.size();
  auto cube = UniqueTexture(dxmt9c_device_create_cube_texture(
      fixture.c(), 8, 0, 0, kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(cube != nullptr, "cube zero-level texture creation succeeds");
  checkEq(dxmt9c_texture_get_level_count(cube.get()), 4u,
          "cube zero-level request expands to full face mip chain");
  checkEq(fixture.backend->createdTextures[cubeBefore].levels, 4u,
          "cube backend descriptor receives full mip count");
  auto cubeFace5Mip3 = UniqueSurface(dxmt9c_texture_get_surface_level(
      cube.get(), 5u * dxmt9c_texture_get_level_count(cube.get()) + 3u));
  check(cubeFace5Mip3 != nullptr, "cube final face mip surface exists");
  const auto cubeMipDesc = surfaceDesc(cubeFace5Mip3.get(),
                                       "cube final face mip desc");
  checkEq(cubeMipDesc.width, 1u, "cube final face mip width");
  checkEq(cubeMipDesc.height, 1u, "cube final face mip height");

  const auto volumeBefore = fixture.backend->createdTextures.size();
  auto volume = UniqueTexture(dxmt9c_device_create_volume_texture(
      fixture.c(), 9, 5, 3, 0, 0, kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(volume != nullptr, "volume zero-level texture creation succeeds");
  checkEq(dxmt9c_texture_get_level_count(volume.get()), 4u,
          "volume zero-level request expands across max dimension");
  checkEq(fixture.backend->createdTextures[volumeBefore].levels, 4u,
          "volume backend descriptor receives full mip count");
  const auto volumeLevel3 = textureLevelDesc(volume.get(), 3,
                                             "volume final mip desc");
  checkEq(volumeLevel3.width, 1u, "volume final mip width");
  checkEq(volumeLevel3.height, 1u, "volume final mip height");
  checkEq(volume->obj->levelBytes(3).size(), size_t{4},
          "volume final mip clamps depth to one slice");

  auto autogen = UniqueTexture(dxmt9c_device_create_texture(
      fixture.c(), 64, 64, 0, kD3DUsageAutoGenMipmap,
      kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(autogen != nullptr, "autogen zero-level texture creation succeeds");
  checkEq(dxmt9c_texture_get_level_count(autogen.get()), 1u,
          "autogen zero-level request exposes only the app-visible top level");
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

  const std::array<Case, 7> cases{{
      {kD3DFmtL8, Format::L8, WMTPixelFormatR8Unorm,
       WMTTextureSwizzleOne, {0x55u, 0x00u}, 1u, "L8"},
      {kD3DFmtA8L8, Format::A8L8, WMTPixelFormatRG8Unorm,
       WMTTextureSwizzleGreen, {0x55u, 0x40u}, 2u, "A8L8"},
      {kD3DFmtG16R16, Format::G16R16, WMTPixelFormatRG16Unorm,
       WMTTextureSwizzleOne, {}, 0u, "G16R16"},
      {kD3DFmtR16F, Format::R16F, WMTPixelFormatR16Float,
       WMTTextureSwizzleOne, {}, 0u, "R16F"},
      {kD3DFmtG16R16F, Format::G16R16F, WMTPixelFormatRG16Float,
       WMTTextureSwizzleOne, {}, 0u, "G16R16F"},
      {kD3DFmtR32F, Format::R32F, WMTPixelFormatR32Float,
       WMTTextureSwizzleOne, {}, 0u, "R32F"},
      {kD3DFmtG32R32F, Format::G32R32F, WMTPixelFormatRG32Float,
       WMTTextureSwizzleOne, {}, 0u, "G32R32F"},
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
    const bool luminance = backendDesc.format == Format::L8 ||
                           backendDesc.format == Format::L16 ||
                           backendDesc.format == Format::A8L8;
    const auto expectedGreen = luminance
                                   ? WMTTextureSwizzleRed
                                   : ((backendDesc.format == Format::G16R16 ||
                                       backendDesc.format == Format::G16R16F ||
                                       backendDesc.format == Format::G32R32F)
                                          ? WMTTextureSwizzleGreen
                                          : WMTTextureSwizzleOne);
    const auto expectedBlue = (backendDesc.format == Format::L8 ||
                               backendDesc.format == Format::L16 ||
                               backendDesc.format == Format::A8L8)
                                  ? WMTTextureSwizzleRed
                                  : WMTTextureSwizzleOne;
    checkSwizzle(dxmt9::convert::toShaderReadSwizzle(backendDesc.format),
                 WMTTextureSwizzleRed, expectedGreen,
                 expectedBlue, testCase.alphaSwizzle,
                 testCase.label);

    if (testCase.expectedByteCount != 0u) {
      texture->obj->fillColor(nullptr, ColorRGBA{1.0f, 0.0f, 0.0f, 0.25f});
      checkBytesPrefix(texture->obj->levelBytes(0),
                       std::span<const u8>(testCase.expectedBytes.data(),
                                           testCase.expectedByteCount),
                       testCase.label);
    }
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
  checkEq(volume->obj->levelBytes(0).size(), size_t{8u * 4u * 3u * 4u},
          "volume CPU shadow stores every slice in level0");
  checkEq(volume->obj->levelBytes(1).size(), size_t{4u * 2u * 1u * 4u},
          "volume CPU shadow mip depth shrinks with level");

  const auto uploadBefore = fixture.backend->textureUploads.size();
  D9CLockedRect lock{};
  checkEq(dxmt9c_texture_lock_rect(volume.get(), 0, &lock, nullptr, 0),
          D3D_OK, "volume texture lock succeeds");
  check(lock.bits != nullptr, "volume texture lock bits");
  checkEq(lock.pitch, 32, "volume row pitch");
  auto* bytes = static_cast<u8*>(lock.bits);
  const size_t slicePitch = size_t{8u * 4u * 4u};
  bytes[0] = 0x11u;
  bytes[slicePitch + 0] = 0x22u;
  bytes[slicePitch * 2u + 0] = 0x33u;
  checkEq(dxmt9c_texture_unlock_rect(volume.get(), 0), D3D_OK,
          "volume texture unlock succeeds");
  checkEq(fixture.backend->textureUploads.size(), uploadBefore + size_t{1},
          "volume unlock uploads one multi-slice subresource");
  const auto& upload = fixture.backend->textureUploads.back();
  checkEq(upload.width, 8u, "volume upload width");
  checkEq(upload.height, 4u, "volume upload height");
  checkEq(upload.depth, 3u, "volume upload depth");
  checkEq(upload.pitch, 32u, "volume upload row pitch");
  checkEq(upload.slicePitch, 128u, "volume upload slice pitch");
  checkEq(upload.bytes.size(), size_t{8u * 4u * 3u * 4u},
          "volume upload byte count");
  checkEq(upload.bytes[0], 0x11u, "volume upload slice0 byte");
  checkEq(upload.bytes[slicePitch], 0x22u, "volume upload slice1 byte");
  checkEq(upload.bytes[slicePitch * 2u], 0x33u,
          "volume upload slice2 byte");
}

void testCubeAndVolumeSubresourceUploadMetadata() {
  PublicDevice fixture;

  const auto cubeBefore = fixture.backend->createdTextures.size();
  auto cube = UniqueTexture(dxmt9c_device_create_cube_texture(
      fixture.c(), 8, 4, 0, kD3DFmtDXT5, kD3DPoolManaged));
  check(cube != nullptr, "compressed cube texture creation succeeds");
  checkEq(fixture.backend->createdTextures.size(), cubeBefore + size_t{1},
          "compressed cube reaches backend");
  const auto& cubeDesc = fixture.backend->createdTextures[cubeBefore];
  checkEq(cubeDesc.type, TextureType::Cube, "compressed cube backend type");
  checkEq(cubeDesc.format, Format::DXT5, "compressed cube backend format");
  checkEq(cubeDesc.width, 8u, "compressed cube backend width");
  checkEq(cubeDesc.height, 8u, "compressed cube backend height");
  checkEq(cubeDesc.levels, 4u, "compressed cube backend level count");

  const auto mip3 = textureLevelDesc(cube.get(), 3,
                                     "compressed cube public mip3 desc");
  checkEq(mip3.resourceType, kD3DResourceTypeCubeTexture,
          "compressed cube public resource type");
  checkEq(mip3.format, kD3DFmtDXT5, "compressed cube public format");
  checkEq(mip3.width, 1u, "compressed cube mip3 width");
  checkEq(mip3.height, 1u, "compressed cube mip3 height");
  checkEq(mip3.depth, 1u, "compressed cube mip3 depth");
  checkEq(cube->obj->levelBytes(3).size(), size_t{16},
          "compressed cube face0 mip3 stores one DXT5 block");

  const u32 cubeLevels = dxmt9c_texture_get_level_count(cube.get());
  const u32 face4Mip3 = 4u * cubeLevels + 3u;
  const auto surfaceBefore = fixture.backend->textureSurfaces.size();
  auto faceSurface =
      UniqueSurface(dxmt9c_texture_get_surface_level(cube.get(), face4Mip3));
  check(faceSurface != nullptr, "compressed cube face4 mip3 surface exists");
  checkEq(fixture.backend->textureSurfaces.size(), surfaceBefore + size_t{1},
          "compressed cube face mip reaches backend surface-for-texture path");
  const auto& faceSurfaceRecord = fixture.backend->textureSurfaces.back();
  checkEq(faceSurfaceRecord.texture, cube->obj->handle(),
          "compressed cube surface aliases parent texture");
  checkEq(faceSurfaceRecord.subresource, face4Mip3,
          "compressed cube surface preserves face-major subresource index");
  checkEq(faceSurfaceRecord.desc.width, 1u,
          "compressed cube surface backend width");
  checkEq(faceSurfaceRecord.desc.height, 1u,
          "compressed cube surface backend height");
  checkEq(faceSurfaceRecord.desc.format, Format::DXT5,
          "compressed cube surface backend format");
  check(!faceSurfaceRecord.desc.renderTarget,
        "compressed cube surface is not render-target");

  const auto facePublic = surfaceDesc(faceSurface.get(),
                                      "compressed cube face mip public desc");
  checkEq(facePublic.resourceType, kD3DResourceTypeSurface,
          "compressed cube face mip public surface type");
  checkEq(facePublic.format, kD3DFmtDXT5,
          "compressed cube face mip public format");
  checkEq(facePublic.width, 1u, "compressed cube face mip public width");
  checkEq(facePublic.height, 1u, "compressed cube face mip public height");

  const auto cubeUploadBefore = fixture.backend->textureUploads.size();
  D9CLockedRect cubeLock{};
  checkEq(dxmt9c_texture_lock_rect(cube.get(), face4Mip3, &cubeLock, nullptr, 0),
          D3D_OK, "compressed cube face mip lock succeeds");
  check(cubeLock.bits != nullptr, "compressed cube face mip lock bits");
  checkEq(cubeLock.pitch, 16, "compressed cube face mip pitch");
  auto* cubeBytes = static_cast<u8*>(cubeLock.bits);
  for (u32 i = 0; i < 16u; ++i) {
    cubeBytes[i] = static_cast<u8>(0xa0u + i);
  }
  checkEq(dxmt9c_texture_unlock_rect(cube.get(), face4Mip3), D3D_OK,
          "compressed cube face mip unlock succeeds");
  checkEq(fixture.backend->textureUploads.size(), cubeUploadBefore + size_t{1},
          "compressed cube face mip uploads once");
  const auto& cubeUpload = fixture.backend->textureUploads.back();
  checkEq(cubeUpload.handle, cube->obj->handle(),
          "compressed cube upload handle");
  checkEq(cubeUpload.level, face4Mip3,
          "compressed cube upload preserves face-major subresource");
  checkEq(cubeUpload.width, 1u, "compressed cube upload width");
  checkEq(cubeUpload.height, 1u, "compressed cube upload height");
  checkEq(cubeUpload.depth, 1u, "compressed cube upload depth");
  checkEq(cubeUpload.pitch, 16u, "compressed cube upload pitch");
  checkEq(cubeUpload.slicePitch, 16u, "compressed cube upload slice pitch");
  checkEq(cubeUpload.bytes.size(), size_t{16},
          "compressed cube upload byte count");
  checkEq(cubeUpload.bytes[0], 0xa0u, "compressed cube upload first byte");
  checkEq(cubeUpload.bytes[15], 0xafu, "compressed cube upload last byte");

  const auto volumeBefore = fixture.backend->createdTextures.size();
  auto volume = UniqueTexture(dxmt9c_device_create_volume_texture(
      fixture.c(), 16, 8, 5, 3, 0, kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(volume != nullptr, "deep volume texture creation succeeds");
  checkEq(fixture.backend->createdTextures.size(), volumeBefore + size_t{1},
          "deep volume reaches backend");
  const auto& volumeDesc = fixture.backend->createdTextures[volumeBefore];
  checkEq(volumeDesc.type, TextureType::Volume, "deep volume backend type");
  checkEq(volumeDesc.depth, 5u, "deep volume backend depth");
  checkEq(volumeDesc.levels, 3u, "deep volume backend levels");

  const auto volumeMip1 = textureLevelDesc(volume.get(), 1,
                                           "deep volume mip1 public desc");
  checkEq(volumeMip1.resourceType, kD3DResourceTypeVolumeTexture,
          "deep volume public resource type");
  checkEq(volumeMip1.width, 8u, "deep volume mip1 width");
  checkEq(volumeMip1.height, 4u, "deep volume mip1 height");
  checkEq(volumeMip1.depth, 2u, "deep volume mip1 depth");
  checkEq(volume->obj->levelBytes(1).size(), size_t{8u * 4u * 2u * 4u},
          "deep volume mip1 stores two slices");

  const auto volumeUploadBefore = fixture.backend->textureUploads.size();
  D9CLockedRect volumeLock{};
  checkEq(dxmt9c_texture_lock_rect(volume.get(), 1, &volumeLock, nullptr, 0),
          D3D_OK, "deep volume mip1 lock succeeds");
  check(volumeLock.bits != nullptr, "deep volume mip1 lock bits");
  checkEq(volumeLock.pitch, 32, "deep volume mip1 row pitch");
  auto* volumeBytes = static_cast<u8*>(volumeLock.bits);
  const size_t volumeMip1SlicePitch = size_t{8u * 4u * 4u};
  volumeBytes[0] = 0x44u;
  volumeBytes[volumeMip1SlicePitch] = 0x88u;
  checkEq(dxmt9c_texture_unlock_rect(volume.get(), 1), D3D_OK,
          "deep volume mip1 unlock succeeds");
  checkEq(fixture.backend->textureUploads.size(),
          volumeUploadBefore + size_t{1}, "deep volume mip1 uploads once");
  const auto& volumeUpload = fixture.backend->textureUploads.back();
  checkEq(volumeUpload.handle, volume->obj->handle(),
          "deep volume upload handle");
  checkEq(volumeUpload.level, 1u, "deep volume upload mip level");
  checkEq(volumeUpload.width, 8u, "deep volume upload width");
  checkEq(volumeUpload.height, 4u, "deep volume upload height");
  checkEq(volumeUpload.depth, 2u, "deep volume upload depth");
  checkEq(volumeUpload.pitch, 32u, "deep volume upload row pitch");
  checkEq(volumeUpload.slicePitch, 128u, "deep volume upload slice pitch");
  checkEq(volumeUpload.bytes.size(), size_t{8u * 4u * 2u * 4u},
          "deep volume upload byte count");
  checkEq(volumeUpload.bytes[0], 0x44u, "deep volume upload slice0 byte");
  checkEq(volumeUpload.bytes[volumeMip1SlicePitch], 0x88u,
          "deep volume upload slice1 byte");
}

void testCubeTextureFaceMajorMiptreeSubresourceBoundaries() {
  PublicDevice fixture;

  const auto cubeBefore = fixture.backend->createdTextures.size();
  auto cube = UniqueTexture(dxmt9c_device_create_cube_texture(
      fixture.c(), 16, 5, 0, kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(cube != nullptr, "cube miptree creation succeeds");
  checkEq(fixture.backend->createdTextures.size(), cubeBefore + size_t{1},
          "cube miptree reaches backend");
  const auto& cubeDesc = fixture.backend->createdTextures[cubeBefore];
  checkEq(cubeDesc.type, TextureType::Cube, "cube miptree backend type");
  checkEq(cubeDesc.width, 16u, "cube miptree backend width");
  checkEq(cubeDesc.height, 16u, "cube miptree backend height");
  checkEq(cubeDesc.levels, 5u, "cube miptree backend level count");
  checkEq(dxmt9c_texture_get_level_count(cube.get()), 5u,
          "cube miptree public level count");

  const std::array<u32, 5> mipSizes{{16u, 8u, 4u, 2u, 1u}};
  const u32 levelCount = dxmt9c_texture_get_level_count(cube.get());
  for (u32 face : {0u, 2u, 5u}) {
    for (u32 mip = 0; mip < levelCount; ++mip) {
      const u32 subresource = face * levelCount + mip;
      checkEq(cube->obj->levelBytes(subresource).size(),
              size_t{mipSizes[mip]} * mipSizes[mip] * 4u,
              "cube face-major CPU subresource size");

      const auto surfaceBefore = fixture.backend->textureSurfaces.size();
      auto surface =
          UniqueSurface(dxmt9c_texture_get_surface_level(cube.get(), subresource));
      check(surface != nullptr, "cube face-major surface exists");
      checkEq(fixture.backend->textureSurfaces.size(), surfaceBefore + size_t{1},
              "cube face-major surface reaches backend");
      const auto& surfaceRecord = fixture.backend->textureSurfaces.back();
      checkEq(surfaceRecord.texture, cube->obj->handle(),
              "cube face-major surface aliases parent texture");
      checkEq(surfaceRecord.subresource, subresource,
              "cube face-major backend subresource index");
      checkEq(surfaceRecord.desc.width, mipSizes[mip],
              "cube face-major backend mip width");
      checkEq(surfaceRecord.desc.height, mipSizes[mip],
              "cube face-major backend mip height");
      checkEq(surfaceRecord.desc.format, Format::A8R8G8B8,
              "cube face-major backend format");

      const auto publicDesc =
          surfaceDesc(surface.get(), "cube face-major public desc");
      checkEq(publicDesc.resourceType, kD3DResourceTypeSurface,
              "cube face-major public surface type");
      checkEq(publicDesc.width, mipSizes[mip],
              "cube face-major public mip width");
      checkEq(publicDesc.height, mipSizes[mip],
              "cube face-major public mip height");
    }
  }

  const u32 face2Mip3 = 2u * levelCount + 3u;
  const auto uploadBefore = fixture.backend->textureUploads.size();
  D9CLockedRect lock{};
  checkEq(dxmt9c_texture_lock_rect(cube.get(), face2Mip3, &lock, nullptr, 0),
          D3D_OK, "cube face-major lock succeeds");
  check(lock.bits != nullptr, "cube face-major lock bits");
  checkEq(lock.pitch, 8, "cube face-major mip3 row pitch");
  auto* bytes = static_cast<u8*>(lock.bits);
  bytes[0] = 0x31u;
  bytes[15] = 0x7bu;
  checkEq(dxmt9c_texture_unlock_rect(cube.get(), face2Mip3), D3D_OK,
          "cube face-major unlock succeeds");
  checkEq(fixture.backend->textureUploads.size(), uploadBefore + size_t{1},
          "cube face-major unlock uploads once");
  const auto& upload = fixture.backend->textureUploads.back();
  checkEq(upload.handle, cube->obj->handle(), "cube face-major upload handle");
  checkEq(upload.level, face2Mip3,
          "cube face-major upload preserves subresource");
  checkEq(upload.width, 2u, "cube face-major upload width");
  checkEq(upload.height, 2u, "cube face-major upload height");
  checkEq(upload.depth, 1u, "cube face-major upload depth");
  checkEq(upload.pitch, 8u, "cube face-major upload row pitch");
  checkEq(upload.slicePitch, 16u, "cube face-major upload slice pitch");
  checkEq(upload.bytes[0], 0x31u, "cube face-major upload first byte");
  checkEq(upload.bytes[15], 0x7bu, "cube face-major upload last byte");
  checkEq(cube->obj->levelBytes(3)[0], 0u,
          "cube face0 mip3 remains isolated from face2 upload");

  const u32 invalidSubresource = 6u * levelCount;
  const auto surfaceBefore = fixture.backend->textureSurfaces.size();
  auto invalidSurface =
      UniqueSurface(dxmt9c_texture_get_surface_level(cube.get(), invalidSubresource));
  check(invalidSurface == nullptr, "cube invalid face-major surface rejected");
  checkEq(fixture.backend->textureSurfaces.size(), surfaceBefore,
          "cube invalid face-major surface does not reach backend");
  D9CLockedRect invalidLock{123, reinterpret_cast<void*>(0x1)};
  checkEq(dxmt9c_texture_lock_rect(cube.get(), invalidSubresource, &invalidLock,
                                   nullptr, 0),
          D3DERR_INVALIDCALL, "cube invalid face-major lock rejected");
  checkEq(invalidLock.pitch, 0, "cube invalid lock clears pitch");
  check(invalidLock.bits == nullptr, "cube invalid lock clears bits");
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

// INTZ — FOURCC depth-as-color sampler trick. dxmt9 maps the format
// onto MTLPixelFormatDepth32Float and exposes both ShaderRead and
// RenderTarget usage so the same texture can be a depth-stencil target
// and a sampler source. The format table is asserted directly so the
// mapping survives refactors of the surface-creation seam.
void testIntzDepthSampleableFormatMapping() {
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtINTZ), Format::INTZ,
          "INTZ FOURCC maps to core Format::INTZ");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::INTZ), kD3DFmtINTZ,
          "INTZ core format round-trips back to FOURCC");

  const auto* info = findFormatInfo(Format::INTZ);
  check(info != nullptr, "INTZ has a format-table entry");
  if (info) {
    check(info->depthStencil, "INTZ is a depth-stencil format");
    check(!info->renderTarget, "INTZ is not a color render target");
    check(!info->compressed, "INTZ is not block-compressed");
    checkEq(info->bytesPerPixel, 4u,
            "INTZ is four bytes per pixel (Depth32Float storage)");
    checkEq(info->backendFormat, BackendPixelFormat::Depth32Float,
            "INTZ backend pixel format is Depth32Float");
  }

  const auto limits = defaultLimits();
  checkEq(dxmt9::convert::toPixelFormat(Format::INTZ, limits),
          WMTPixelFormatDepth32Float,
          "INTZ Metal pixel format is Depth32Float");
  check(dxmt9::convert::formatHasDepthAspect(Format::INTZ),
        "INTZ exposes a depth aspect");
  check(!dxmt9::convert::formatHasStencilAspect(Format::INTZ),
        "INTZ has no stencil aspect");

  // INTZ + D3DUSAGE_DEPTHSTENCIL + D3DRTYPE_TEXTURE must be supported.
  check(formatSupportsUsage(Format::INTZ,
                            UsageTexture | UsageDepthStencil, limits),
        "INTZ supports depth-stencil texture usage");
  // INTZ + D3DUSAGE_RENDERTARGET must NOT be supported — INTZ is a
  // depth-only target on the GPU side.
  check(!formatSupportsUsage(Format::INTZ,
                             UsageTexture | UsageRenderTarget, limits),
        "INTZ does not support color render-target usage");

  // The TextureDesc → WMTTextureUsage path must produce both ShaderRead
  // and RenderTarget so a single texture can be sampled and used as a
  // depth target — the whole point of INTZ.
  TextureDesc desc{};
  desc.format = Format::INTZ;
  desc.usage = UsageDepthStencil;
  const auto wmtUsage = dxmt9::convert::toTextureUsage(desc);
  check(hasWmtUsage(wmtUsage, WMTTextureUsageShaderRead),
        "INTZ texture usage includes ShaderRead");
  check(hasWmtUsage(wmtUsage, WMTTextureUsageRenderTarget),
        "INTZ texture usage includes RenderTarget");
}

// D3DFMT_NULL — colorless render target (R-FORMAT-12,
// specs/d3d9/formats/spec.md "NULL render target"). Creating a NULL
// render-target surface must SUCCEED but allocate no GPU color backing; the
// surface is flagged as a null render target so the backend render pass
// can omit the color attachment. Lock/LockRect returns dummy CPU scratch per
// Wine, while readback on a NULL surface must return D3DERR_INVALIDCALL. The
// render-pass color-attachment omission itself is ObjC++/Metal and validated
// at the GPU/runtime level, not here.
void testNullRenderTargetColorlessBehavior() {
  // Classification prerequisite landed in 2f619f0: NULL FOURCC maps to
  // Format::NullRt, renderTarget-capable, placeholder BGRA8Unorm backend.
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtNULL), Format::NullRt,
          "NULL FOURCC maps to core Format::NullRt");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::NullRt), kD3DFmtNULL,
          "NULL core format round-trips back to FOURCC");

  const auto* info = findFormatInfo(Format::NullRt);
  check(info != nullptr, "NULL has a format-table entry");
  if (info) {
    check(info->renderTarget, "NULL is render-target capable");
    check(!info->depthStencil, "NULL is not itself a depth-stencil format");
    check(!info->compressed, "NULL is not block-compressed");
  }

  PublicDevice fixture;

  // (b)/(a) Creation: a NULL render target must succeed and reach the
  // backend tagged renderTarget, just like any other RT surface.
  const auto rtBefore = fixture.backend->createdSurfaces.size();
  auto nullRt = UniqueSurface(dxmt9c_device_create_render_target(
      fixture.c(), 64, 64, kD3DFmtNULL, kD3DMultiSampleNone, 0, 0, nullptr));
  check(nullRt != nullptr, "NULL render-target creation succeeds");
  checkEq(fixture.backend->createdSurfaces.size(), rtBefore + size_t{1},
          "NULL render target reaches backend");
  const auto& nullDesc = fixture.backend->createdSurfaces[rtBefore];
  checkEq(nullDesc.format, Format::NullRt, "NULL backend format");
  check(nullDesc.renderTarget, "NULL backend render-target flag");

  // (a) No GPU color backing: the core surface must report itself as a null
  // render target. It still owns dummy CPU scratch so LockRect can return
  // S_OK with valid pBits/Pitch like Wine's test_surface_format_null.
  check(nullRt->obj->isNullRenderTarget(),
        "core surface reports NULL render-target marker");
  checkEq(nullRt->obj->colorBackingByteSize(), size_t{64u * 64u * 4u},
          "NULL render target allocates dummy lock scratch bytes");

  const auto nullPublic = surfaceDesc(nullRt.get(), "NULL public desc");
  checkEq(nullPublic.format, kD3DFmtNULL, "NULL public format round-trip");
  checkEq(nullPublic.resourceType, kD3DResourceTypeSurface,
          "NULL resource type is SURFACE");
  checkEq(nullPublic.width, 64u, "NULL public width preserved");
  checkEq(nullPublic.height, 64u, "NULL public height preserved");

  // (c) Lock/LockRect on a NULL surface must return dummy scratch.
  D9CLockedRect lock{};
  checkEq(dxmt9c_surface_lock_rect(nullRt.get(), &lock, nullptr, 0),
          D3D_OK, "NULL surface full Lock returns S_OK");
  check(lock.bits != nullptr, "NULL surface lock yields dummy bits");
  checkEq(lock.pitch, 64 * 4, "NULL surface lock pitch uses placeholder BGRA8 bpp");
  checkEq(dxmt9c_surface_unlock_rect(nullRt.get()), D3D_OK,
          "NULL surface full Unlock returns S_OK");

  D9CLockedRect rectLock{};
  const D9CRect subRect{0, 0, 32, 32};
  checkEq(dxmt9c_surface_lock_rect(nullRt.get(), &rectLock, &subRect, 0),
          D3D_OK, "NULL surface LockRect returns S_OK");
  check(rectLock.bits != nullptr, "NULL surface sub-rect lock yields dummy bits");
  checkEq(rectLock.pitch, 64 * 4, "NULL surface sub-rect pitch uses full-row pitch");
  checkEq(dxmt9c_surface_unlock_rect(nullRt.get()), D3D_OK,
          "NULL surface sub-rect Unlock returns S_OK");

  // The core-level lockRect must independently expose the same scratch.
  const auto coreLock = nullRt->obj->lockRect(nullptr, 0);
  check(coreLock.data != nullptr && coreLock.pitch == 64u * 4u,
        "core NULL surface lockRect yields dummy region");
  nullRt->obj->unlockRect();

  // (c) GetRenderTargetData with a NULL source must be rejected. Pair the
  // NULL surface against a normal offscreen system-memory destination.
  auto dest = UniqueSurface(dxmt9c_device_create_offscreen_surface(
      fixture.c(), 64, 64, kD3DFmtA8R8G8B8, kD3DPoolSystemMem, nullptr));
  check(dest != nullptr, "offscreen readback destination creation succeeds");
  checkEq(fixture.device->GetRenderTargetData(nullRt->obj, dest->obj),
          D3DERR_INVALIDCALL,
          "GetRenderTargetData from NULL source returns INVALIDCALL");
}

}  // namespace

int main() {
  try {
    testPublicTextureCreationPreservesD3DValuesAndUploadPitch();
    testNpotMiptreeLayoutBoundaryValues();
    testZeroLevelRequestsCreateFullMipChains();
    testComponentOrderAlphaAndSrgbCompatibility();
    testLuminanceDefaultChannelPolicy();
    testCompressedCreationAndBlockRowRounding();
    testCubeAndVolumeCreationDescriptors();
    testCubeAndVolumeSubresourceUploadMetadata();
    testCubeTextureFaceMajorMiptreeSubresourceBoundaries();
    testSurfaceDescriptorsMultisampleDepthFallbackAndOffscreenPitch();
    testIntzDepthSampleableFormatMapping();
    testNullRenderTargetColorlessBehavior();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
