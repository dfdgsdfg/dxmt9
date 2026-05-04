/*
 * Focused D3D9 resource wrapper conformance scaffold.
 *
 * Wine provenance: distilled from Wine dlls/d3d9/tests/device.c at 6e073d2:
 * - test_surface_get_container(), test_volume_get_container()
 * - test_surface_blocks(), test_volume_locking()
 * - test_lod(), test_getdc(), test_mipmap_gen(), test_format_unknown()
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;
int skips = 0;

void fail_at(int line, const char *message) {
  std::printf("FAIL:%d: %s\n", line, message);
  ++failures;
}

void check_at(int line, bool condition, const char *message) {
  if (!condition) fail_at(line, message);
}

void check_hr_at(int line, HRESULT actual, HRESULT expected, const char *call) {
  if (actual != expected) {
    std::printf("FAIL:%d: %s returned 0x%08lx, expected 0x%08lx\n",
        line, call, static_cast<unsigned long>(actual),
        static_cast<unsigned long>(expected));
    ++failures;
  }
}

void check_hr_any_at(int line, HRESULT actual, HRESULT expected_a,
    HRESULT expected_b, const char *call) {
  if (actual != expected_a && actual != expected_b) {
    std::printf("FAIL:%d: %s returned 0x%08lx, expected 0x%08lx or 0x%08lx\n",
        line, call, static_cast<unsigned long>(actual),
        static_cast<unsigned long>(expected_a),
        static_cast<unsigned long>(expected_b));
    ++failures;
  }
}

#define CHECK(condition) check_at(__LINE__, !!(condition), #condition)
#define CHECK_HR(actual, expected) check_hr_at(__LINE__, (actual), (expected), #actual)
#define CHECK_HR_ANY(actual, expected_a, expected_b) \
  check_hr_any_at(__LINE__, (actual), (expected_a), (expected_b), #actual)

template <typename T>
void release_if(T *object) {
  if (object) object->Release();
}

HWND create_window() {
  RECT rect = {0, 0, 64, 64};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowA("static", "dxmt9_d3d9_resources", WS_OVERLAPPEDWINDOW,
      0, 0, rect.right - rect.left, rect.bottom - rect.top,
      nullptr, nullptr, nullptr, nullptr);
}

IDirect3DDevice9 *create_device(IDirect3D9 *d3d9, HWND window) {
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = window;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  const DWORD flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  for (DWORD flag : flags) {
    IDirect3DDevice9 *device = nullptr;
    if (SUCCEEDED(d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, flag, &pp, &device))) {
      return device;
    }
  }

  pp.AutoDepthStencilFormat = D3DFMT_D16;
  for (DWORD flag : flags) {
    IDirect3DDevice9 *device = nullptr;
    if (SUCCEEDED(d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, flag, &pp, &device))) {
      return device;
    }
  }

  return nullptr;
}

struct Fixture {
  HWND window = nullptr;
  IDirect3D9 *d3d9 = nullptr;
  IDirect3DDevice9 *device = nullptr;

  bool init(const char *test_name) {
    window = create_window();
    if (!window) {
      std::printf("SKIP:%s: failed to create a window\n", test_name);
      ++skips;
      return false;
    }

    d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) {
      std::printf("SKIP:%s: Direct3DCreate9 failed\n", test_name);
      ++skips;
      return false;
    }

    device = create_device(d3d9, window);
    if (!device) {
      std::printf("SKIP:%s: failed to create a D3D9 device\n", test_name);
      ++skips;
      return false;
    }

    return true;
  }

  ~Fixture() {
    release_if(device);
    release_if(d3d9);
    if (window) DestroyWindow(window);
  }
};

void check_surface_container(IDirect3DSurface9 *surface, IDirect3DTexture9 *texture) {
  IUnknown *container = nullptr;
  CHECK_HR(surface->GetContainer(IID_IUnknown, reinterpret_cast<void **>(&container)),
      D3D_OK);
  CHECK(container == static_cast<IUnknown *>(texture));
  release_if(container);

  container = nullptr;
  CHECK_HR(surface->GetContainer(IID_IDirect3DTexture9,
      reinterpret_cast<void **>(&container)), D3D_OK);
  CHECK(container == static_cast<IUnknown *>(texture));
  release_if(container);

  container = reinterpret_cast<IUnknown *>(static_cast<UINT_PTR>(0xdeadbeef));
  CHECK_HR(surface->GetContainer(IID_IDirect3DSurface9,
      reinterpret_cast<void **>(&container)), E_NOINTERFACE);
  CHECK(container == nullptr);
}

void resource_container_level_desc_and_locks() {
  Fixture fixture;
  if (!fixture.init("resource_container_level_desc_and_locks")) return;

  IDirect3DTexture9 *texture = nullptr;
  HRESULT hr = fixture.device->CreateTexture(16, 8, 3, 0, D3DFMT_A8R8G8B8,
      D3DPOOL_MANAGED, &texture, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!texture) return;

  IDirect3DSurface9 *surface = nullptr;
  CHECK_HR(texture->GetSurfaceLevel(0, &surface), D3D_OK);
  if (surface) {
    check_surface_container(surface, texture);
    release_if(surface);
  }

  surface = reinterpret_cast<IDirect3DSurface9 *>(static_cast<UINT_PTR>(0xdeadbeef));
  CHECK_HR(texture->GetSurfaceLevel(3, &surface), D3DERR_INVALIDCALL);
  CHECK(surface == nullptr);

  D3DSURFACE_DESC desc = {};
  CHECK_HR(texture->GetLevelDesc(1, &desc), D3D_OK);
  CHECK(desc.Width == 8);
  CHECK(desc.Height == 4);
  CHECK(desc.Format == D3DFMT_A8R8G8B8);
  CHECK(desc.Pool == D3DPOOL_MANAGED);
  CHECK_HR(texture->GetLevelDesc(3, &desc), D3DERR_INVALIDCALL);

  D3DLOCKED_RECT locked = {};
  CHECK_HR(texture->LockRect(0, &locked, nullptr, 0), D3D_OK);
  CHECK(locked.pBits != nullptr);
  CHECK_HR(texture->LockRect(0, &locked, nullptr, 0), D3DERR_INVALIDCALL);
  CHECK_HR(texture->UnlockRect(0), D3D_OK);
  CHECK_HR(texture->UnlockRect(0), D3DERR_INVALIDCALL);
  release_if(texture);

  texture = reinterpret_cast<IDirect3DTexture9 *>(static_cast<UINT_PTR>(0xdeadbeef));
  hr = fixture.device->CreateTexture(2, 4, 1, 0, D3DFMT_DXT1,
      D3DPOOL_SCRATCH, &texture, nullptr);
  CHECK_HR(hr, D3DERR_INVALIDCALL);
  CHECK(texture == nullptr);

  D3DCAPS9 caps = {};
  CHECK_HR(fixture.device->GetDeviceCaps(&caps), D3D_OK);
  if (!(caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP)) {
    std::printf("SKIP:resource_container_level_desc_and_locks: volume textures not supported\n");
    ++skips;
    return;
  }

  IDirect3DVolumeTexture9 *volume_texture = nullptr;
  hr = fixture.device->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8,
      D3DPOOL_MANAGED, &volume_texture, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!volume_texture) return;

  IDirect3DVolume9 *volume = nullptr;
  CHECK_HR(volume_texture->GetVolumeLevel(0, &volume), D3D_OK);
  if (volume) {
    IUnknown *container = nullptr;
    CHECK_HR(volume->GetContainer(IID_IDirect3DVolumeTexture9,
        reinterpret_cast<void **>(&container)), D3D_OK);
    CHECK(container == static_cast<IUnknown *>(volume_texture));
    release_if(container);

    container = reinterpret_cast<IUnknown *>(static_cast<UINT_PTR>(0xdeadbeef));
    CHECK_HR(volume->GetContainer(IID_IDirect3DVolume9,
        reinterpret_cast<void **>(&container)), E_NOINTERFACE);
    CHECK(container == nullptr);
    release_if(volume);
  }

  D3DLOCKED_BOX box = {};
  CHECK_HR(volume_texture->LockBox(0, &box, nullptr, 0), D3D_OK);
  CHECK(box.pBits != nullptr);
  CHECK_HR(volume_texture->LockBox(0, &box, nullptr, 0), D3DERR_INVALIDCALL);
  CHECK_HR(volume_texture->UnlockBox(0), D3D_OK);
  CHECK_HR(volume_texture->UnlockBox(0), D3DERR_INVALIDCALL);
  release_if(volume_texture);
}

void check_getdc(IDirect3DDevice9 *device) {
  IDirect3DSurface9 *surface = nullptr;
  HRESULT hr = device->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8,
      D3DPOOL_SYSTEMMEM, &surface, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!surface) return;

  HDC dc = nullptr;
  CHECK_HR(surface->GetDC(&dc), D3D_OK);
  CHECK(dc != nullptr);

  HDC dc2 = reinterpret_cast<HDC>(static_cast<UINT_PTR>(0x1234));
  CHECK_HR(surface->GetDC(&dc2), D3DERR_INVALIDCALL);
  CHECK(dc2 == reinterpret_cast<HDC>(static_cast<UINT_PTR>(0x1234)));

  CHECK_HR(surface->ReleaseDC(dc), D3D_OK);
  CHECK_HR(surface->ReleaseDC(dc), D3DERR_INVALIDCALL);
  release_if(surface);
}

void check_lod(IDirect3DDevice9 *device) {
  IDirect3DTexture9 *texture = nullptr;
  HRESULT hr = device->CreateTexture(128, 128, 3, 0, D3DFMT_A8R8G8B8,
      D3DPOOL_DEFAULT, &texture, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (texture) {
    CHECK(texture->SetLOD(1) == 0);
    CHECK(texture->GetLOD() == 0);
    release_if(texture);
  }

  texture = nullptr;
  hr = device->CreateTexture(128, 128, 3, 0, D3DFMT_A8R8G8B8,
      D3DPOOL_MANAGED, &texture, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!texture) return;
  CHECK(texture->SetLOD(2) == 0);
  CHECK(texture->GetLOD() == 2);
  CHECK(texture->SetLOD(1) == 2);
  release_if(texture);
}

void check_autogen(Fixture *fixture) {
  HRESULT hr = fixture->d3d9->CheckDeviceFormat(D3DADAPTER_DEFAULT,
      D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_AUTOGENMIPMAP,
      D3DRTYPE_TEXTURE, D3DFMT_X8R8G8B8);
  if (hr != D3D_OK) {
    CHECK_HR_ANY(hr, D3DOK_NOAUTOGEN, D3DERR_NOTAVAILABLE);
    std::printf("SKIP:resource_getdc_lod_autogen_mipmap: autogen mipmap not supported\n");
    ++skips;
    return;
  }

  IDirect3DTexture9 *texture = nullptr;
  hr = fixture->device->CreateTexture(64, 64, 0, D3DUSAGE_AUTOGENMIPMAP,
      D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!texture) return;

  CHECK(texture->GetLevelCount() == 1);
  CHECK(texture->GetAutoGenFilterType() == D3DTEXF_LINEAR);
  CHECK_HR(texture->SetAutoGenFilterType(D3DTEXF_NONE), D3DERR_INVALIDCALL);
  CHECK_HR(texture->SetAutoGenFilterType(D3DTEXF_ANISOTROPIC), D3D_OK);
  CHECK(texture->GetAutoGenFilterType() == D3DTEXF_ANISOTROPIC);
  release_if(texture);
}

void check_format_unknown(IDirect3DDevice9 *device) {
  IDirect3DSurface9 *surface =
      reinterpret_cast<IDirect3DSurface9 *>(static_cast<UINT_PTR>(0xdeadbeef));
  CHECK_HR(device->CreateRenderTarget(64, 64, D3DFMT_UNKNOWN,
      D3DMULTISAMPLE_NONE, 0, FALSE, &surface, nullptr), D3DERR_INVALIDCALL);
  CHECK(surface == nullptr);

  IDirect3DTexture9 *texture =
      reinterpret_cast<IDirect3DTexture9 *>(static_cast<UINT_PTR>(0xdeadbeef));
  CHECK_HR(device->CreateTexture(64, 64, 1, 0, D3DFMT_UNKNOWN,
      D3DPOOL_DEFAULT, &texture, nullptr), D3DERR_INVALIDCALL);
  CHECK(texture == nullptr);
}

void resource_getdc_lod_autogen_mipmap() {
  Fixture fixture;
  if (!fixture.init("resource_getdc_lod_autogen_mipmap")) return;

  check_getdc(fixture.device);
  check_lod(fixture.device);
  check_autogen(&fixture);
  check_format_unknown(fixture.device);
}

}  // namespace

int main() {
  resource_container_level_desc_and_locks();
  resource_getdc_lod_autogen_mipmap();

  if (failures) {
    std::printf("d3d9_resources_x64: %d failure(s), %d skip(s)\n", failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("d3d9_resources_x64: passed (%d skip(s))\n", skips);
  return skips ? 77 : EXIT_SUCCESS;
}
