// core_d3d9_generate_mipmaps_spec.cpp — regression guard for the
// IDirect3DBaseTexture9::GenerateMipSubLevels promotion (G2-D audit:
// /tmp/g2_d_unknowns_resolution.md §2/§3/§4).
//
// The audit promoted the three PE-side stubs (Texture, CubeTexture,
// VolumeTexture) to `impl` because they already forward to the C ABI
// entrypoint `dxmt9c_texture_generate_mip_sublevels(D9CTexture*)`. This
// spec covers two contracts:
//
//   1. The C ABI succeeds (D3D_OK) and round-trips through the core
//      `dxmt9::core::Texture::generateMipSubLevels()` for all three
//      texture topologies (2D, cube, volume) that the PE wrappers
//      expose. Failure here means a future refactor broke the wiring
//      the audit relied on.
//   2. The Wine oracle gating in the PE wrapper (no-op without
//      D3DUSAGE_AUTOGENMIPMAP, see
//      `dlls/d3d9/texture.c::d3d9_texture_gen_auto_mipmap`) requires
//      reading `dxmt9c_texture_get_level_desc(t, 0, ...)` and checking
//      the round-tripped `usage` field. We exercise the round-trip
//      here so the gate cannot silently regress to the wrong value.
//
// Wine behavioural oracle:
//   - dlls/d3d9/texture.c::d3d9_texture_2d_GenerateMipSubLevels
//   - dlls/d3d9/texture.c::d3d9_texture_cube_GenerateMipSubLevels
//   - dlls/d3d9/texture.c::d3d9_texture_volume_GenerateMipSubLevels
//   - dlls/d3d9/texture.c::d3d9_texture_gen_auto_mipmap

#include "core_spec_fixtures.hpp"
#include "d3d9_pe_autogen_mipmap.hpp"

#include <memory>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

constexpr u32 kD3DUsageAutoGenMipmap = 0x00000400u;
constexpr u32 kD3DPoolManaged = 1u;
constexpr u32 kD3DFmtA8R8G8B8 = 21u;

void testAutogenDirtyTransitionTruthTable() {
  using dxmt9::d3d9::pe::AutogenMipmapEvent;
  using dxmt9::d3d9::pe::AutogenMipmapState;
  using dxmt9::d3d9::pe::transitionAutogenMipmap;

  const auto disabled = transitionAutogenMipmap(
      AutogenMipmapState{}, AutogenMipmapEvent::RenderTargetWrite);
  check(!disabled.generationRequired(), "disabled AUTOGEN ignores writes");

  const AutogenMipmapState clean{.enabled = true, .dirty = false};
  const auto dirty = transitionAutogenMipmap(
      clean, AutogenMipmapEvent::RenderTargetWrite);
  check(dirty.generationRequired(), "RT write makes AUTOGEN dirty");
  const auto failed = transitionAutogenMipmap(
      dirty, AutogenMipmapEvent::GenerationFailed);
  check(failed.generationRequired(), "failed generation preserves dirty");
  const auto settled = transitionAutogenMipmap(
      failed, AutogenMipmapEvent::GenerationSucceeded);
  check(!settled.generationRequired(), "successful generation settles clean");
}

struct TextureDeleter {
  void operator()(D9CTexture* texture) const {
    if (texture) {
      dxmt9c_texture_release(texture);
    }
  }
};

using UniqueTexture = std::unique_ptr<D9CTexture, TextureDeleter>;

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
    params.deviceWindow = Handle{0xb301u};

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

// 2D texture: GenerateMipSubLevels must return D3D_OK and populate mip
// levels from a filled level0. This is the path used by
// `D3D9TextureImpl::GenerateMipSubLevels()` once the AUTOGENMIPMAP gate
// passes.
void testGenerateMipSubLevels2D() {
  PublicDevice fixture;

  auto texture = UniqueTexture(dxmt9c_device_create_texture(
      fixture.c(), 4, 4, 0,
      kD3DUsageAutoGenMipmap, kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(texture != nullptr, "2D AUTOGENMIPMAP texture creation");
  // levels=0 with AUTOGENMIPMAP collapses to one declared level on the
  // public ABI; the backend keeps the full mip pyramid internally so
  // generate_mip_sublevels has something to fill.
  checkEq(texture->obj->levelCount(), 1u,
          "2D AUTOGENMIPMAP exposes only level 0");
  checkEq(fixture.backend->createdTextures.back().levels, 1u,
          "backend descriptor preserves the public AUTOGEN topology");

  // Round-trip the usage field; the PE wrapper relies on this to gate.
  D9CSurfaceDesc desc{};
  checkEq(dxmt9c_texture_get_level_desc(texture.get(), 0, &desc), D3D_OK,
          "2D AUTOGENMIPMAP get_level_desc");
  check((desc.usage & kD3DUsageAutoGenMipmap) != 0u,
        "2D AUTOGENMIPMAP usage bit round-trips through public API");

  texture->obj->fillColor(nullptr, ColorRGBA{1.0f, 0.0f, 0.0f, 1.0f});
  checkEq(dxmt9c_texture_generate_mip_sublevels(texture.get()), D3D_OK,
          "2D GenerateMipSubLevels succeeds via C ABI");
}

// Cube texture: same C ABI, identical contract per Wine
// `d3d9_texture_cube_GenerateMipSubLevels` sharing
// `d3d9_texture_gen_auto_mipmap`.
void testGenerateMipSubLevelsCube() {
  PublicDevice fixture;

  auto cube = UniqueTexture(dxmt9c_device_create_cube_texture(
      fixture.c(), 4, 0, kD3DUsageAutoGenMipmap,
      kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(cube != nullptr, "cube AUTOGENMIPMAP texture creation");

  D9CSurfaceDesc desc{};
  checkEq(dxmt9c_texture_get_level_desc(cube.get(), 0, &desc), D3D_OK,
          "cube AUTOGENMIPMAP get_level_desc");
  check((desc.usage & kD3DUsageAutoGenMipmap) != 0u,
        "cube AUTOGENMIPMAP usage bit round-trips");

  checkEq(dxmt9c_texture_generate_mip_sublevels(cube.get()), D3D_OK,
          "cube GenerateMipSubLevels succeeds via C ABI");
}

// Volume texture: same C ABI, identical contract per Wine
// `d3d9_texture_volume_GenerateMipSubLevels`.
void testGenerateMipSubLevelsVolume() {
  PublicDevice fixture;

  auto volume = UniqueTexture(dxmt9c_device_create_volume_texture(
      fixture.c(), 4, 4, 2, 0, kD3DUsageAutoGenMipmap,
      kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(volume != nullptr, "volume AUTOGENMIPMAP texture creation");

  D9CSurfaceDesc desc{};
  checkEq(dxmt9c_texture_get_level_desc(volume.get(), 0, &desc), D3D_OK,
          "volume AUTOGENMIPMAP get_level_desc");
  check((desc.usage & kD3DUsageAutoGenMipmap) != 0u,
        "volume AUTOGENMIPMAP usage bit round-trips");

  checkEq(dxmt9c_texture_generate_mip_sublevels(volume.get()), D3D_OK,
          "volume GenerateMipSubLevels succeeds via C ABI");
}

// Null-handle defence: the PE wrapper guarantees `t_` is non-null when
// forwarding, but the C ABI itself must reject null and not crash.
// Matches the early-return in `device_c_resources.cpp:460`.
void testGenerateMipSubLevelsNullHandle() {
  checkEq(dxmt9c_texture_generate_mip_sublevels(nullptr), D3DERR_INVALIDCALL,
          "null D9CTexture* rejected with D3DERR_INVALIDCALL");
}

// Non-AUTOGENMIPMAP texture: confirm the *backend* C ABI itself does
// not gate on AUTOGENMIPMAP (it is the PE wrapper's job to no-op).
// This documents the asymmetry called out in the audit: removing the
// PE-side gate would silently change observable behavior because the
// backend would happily rebuild mips.
void testGenerateMipSubLevelsBackendIsUngated() {
  PublicDevice fixture;

  auto texture = UniqueTexture(dxmt9c_device_create_texture(
      fixture.c(), 4, 4, 0, /*usage*/ 0u,
      kD3DFmtA8R8G8B8, kD3DPoolManaged));
  check(texture != nullptr, "non-AUTOGENMIPMAP texture creation");

  D9CSurfaceDesc desc{};
  checkEq(dxmt9c_texture_get_level_desc(texture.get(), 0, &desc), D3D_OK,
          "non-AUTOGENMIPMAP get_level_desc");
  checkEq(desc.usage & kD3DUsageAutoGenMipmap, 0u,
          "non-AUTOGENMIPMAP usage bit unset");

  // Backend C ABI succeeds regardless of usage; the PE wrapper is what
  // turns this into a no-op for non-AUTOGENMIPMAP textures.
  checkEq(dxmt9c_texture_generate_mip_sublevels(texture.get()), D3D_OK,
          "backend C ABI is intentionally ungated on AUTOGENMIPMAP");
}

}  // namespace

int main() {
  try {
    testAutogenDirtyTransitionTruthTable();
    testGenerateMipSubLevels2D();
    testGenerateMipSubLevelsCube();
    testGenerateMipSubLevelsVolume();
    testGenerateMipSubLevelsNullHandle();
    testGenerateMipSubLevelsBackendIsUngated();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
