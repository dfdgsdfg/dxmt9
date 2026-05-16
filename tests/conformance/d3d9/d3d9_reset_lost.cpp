/*
 * Focused D3D9 reset/lost-device conformance scaffold.
 *
 * Wine behavioral oracle: dlls/d3d9/tests/device.c and
 * dlls/d3d9/tests/d3d9ex.c at 6e073d28dee3af7f4c965daec94644e0f9f92727:
 * - test_reset(), test_lost_device(), test_reset_resources()
 * - test_reset_ex(), d3d9ex test_reset_resources()
 *
 * This intentionally covers a compact reset matrix. Broader mode-switch and
 * foreground-loss cases remain represented by the scaffolded manifest status
 * until current app-local and builtin runtime evidence is recorded.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

#define CHECK(condition) check_at(__LINE__, !!(condition), #condition)
#define CHECK_HR(actual, expected) check_hr_at(__LINE__, (actual), (expected), #actual)

template <typename T>
void release_if(T *object) {
  if (object) object->Release();
}

HWND create_window() {
  RECT rect = {0, 0, 64, 64};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowA("static", "dxmt9_d3d9_reset_lost", WS_OVERLAPPEDWINDOW,
      0, 0, rect.right - rect.left, rect.bottom - rect.top,
      nullptr, nullptr, nullptr, nullptr);
}

D3DPRESENT_PARAMETERS make_present_parameters(HWND window) {
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
  return pp;
}

IDirect3DDevice9 *create_device(IDirect3D9 *d3d9, HWND window) {
  const DWORD flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  for (unsigned int depth_format = 0; depth_format < 2; ++depth_format) {
    D3DPRESENT_PARAMETERS pp = make_present_parameters(window);
    if (depth_format) pp.AutoDepthStencilFormat = D3DFMT_D16;

    for (DWORD flag : flags) {
      IDirect3DDevice9 *device = nullptr;
      HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
          window, flag, &pp, &device);
      if (SUCCEEDED(hr)) return device;
    }
  }

  return nullptr;
}

IDirect3DDevice9Ex *create_device_ex(IDirect3D9Ex *d3d9, HWND window) {
  const DWORD flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  for (unsigned int depth_format = 0; depth_format < 2; ++depth_format) {
    D3DPRESENT_PARAMETERS pp = make_present_parameters(window);
    if (depth_format) pp.AutoDepthStencilFormat = D3DFMT_D16;

    for (DWORD flag : flags) {
      IDirect3DDevice9Ex *device = nullptr;
      HRESULT hr = d3d9->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
          window, flag, &pp, nullptr, &device);
      if (SUCCEEDED(hr)) return device;
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

bool reset_to_windowed(IDirect3DDevice9 *device, HWND window) {
  D3DPRESENT_PARAMETERS pp = make_present_parameters(window);
  HRESULT hr = device->Reset(&pp);
  CHECK_HR(hr, D3D_OK);
  if (FAILED(hr)) return false;

  hr = device->TestCooperativeLevel();
  CHECK_HR(hr, D3D_OK);
  return SUCCEEDED(hr);
}

void reset_default_pool_invalidation() {
  Fixture fixture;
  if (!fixture.init("reset_default_pool_invalidation")) return;

  IDirect3DSurface9 *surface = nullptr;
  HRESULT hr = fixture.device->CreateOffscreenPlainSurface(16, 16,
      D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &surface, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!surface) return;

  D3DPRESENT_PARAMETERS pp = make_present_parameters(fixture.window);
  hr = fixture.device->Reset(&pp);
  CHECK_HR(hr, D3DERR_INVALIDCALL);
  CHECK_HR(fixture.device->TestCooperativeLevel(), D3DERR_DEVICENOTRESET);

  release_if(surface);
  reset_to_windowed(fixture.device, fixture.window);
}

void reset_non_default_pool_survival() {
  Fixture fixture;
  if (!fixture.init("reset_non_default_pool_survival")) return;

  IDirect3DSurface9 *surface = nullptr;
  HRESULT hr = fixture.device->CreateOffscreenPlainSurface(16, 16,
      D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surface, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (surface) {
    D3DLOCKED_RECT locked = {};
    CHECK_HR(surface->LockRect(&locked, nullptr, 0), D3D_OK);
    reset_to_windowed(fixture.device, fixture.window);
    CHECK_HR(surface->UnlockRect(), D3D_OK);
    release_if(surface);
  }

  surface = nullptr;
  hr = fixture.device->CreateOffscreenPlainSurface(16, 16,
      D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (surface) {
    reset_to_windowed(fixture.device, fixture.window);
    release_if(surface);
  }

  IDirect3DTexture9 *texture = nullptr;
  hr = fixture.device->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8,
      D3DPOOL_MANAGED, &texture, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!texture) return;

  reset_to_windowed(fixture.device, fixture.window);

  D3DSURFACE_DESC desc = {};
  CHECK_HR(texture->GetLevelDesc(0, &desc), D3D_OK);
  CHECK(desc.Width == 16);
  CHECK(desc.Height == 16);
  CHECK(desc.Pool == D3DPOOL_MANAGED);
  release_if(texture);
}

void reset_render_target_rebinding() {
  Fixture fixture;
  if (!fixture.init("reset_render_target_rebinding")) return;

  D3DCAPS9 caps = {};
  CHECK_HR(fixture.device->GetDeviceCaps(&caps), D3D_OK);
  unsigned int rt_count = caps.NumSimultaneousRTs;
  if (!rt_count) rt_count = 1;
  if (rt_count > 4) rt_count = 4;

  IDirect3DSurface9 *depth_stencil = nullptr;
  HRESULT hr = fixture.device->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8,
      D3DMULTISAMPLE_NONE, 0, TRUE, &depth_stencil, nullptr);
  if (FAILED(hr)) {
    hr = fixture.device->CreateDepthStencilSurface(64, 64, D3DFMT_D16,
        D3DMULTISAMPLE_NONE, 0, TRUE, &depth_stencil, nullptr);
  }
  if (SUCCEEDED(hr) && depth_stencil) {
    CHECK_HR(fixture.device->SetDepthStencilSurface(depth_stencil), D3D_OK);
    release_if(depth_stencil);
  } else {
    std::printf("SKIP:reset_render_target_rebinding: depth/stencil surface unavailable\n");
    ++skips;
  }

  for (unsigned int i = 0; i < rt_count; ++i) {
    IDirect3DTexture9 *texture = nullptr;
    hr = fixture.device->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr);
    CHECK_HR(hr, D3D_OK);
    if (!texture) return;

    IDirect3DSurface9 *surface = nullptr;
    CHECK_HR(texture->GetSurfaceLevel(0, &surface), D3D_OK);
    release_if(texture);
    if (!surface) return;

    CHECK_HR(fixture.device->SetRenderTarget(i, surface), D3D_OK);
    release_if(surface);
  }

  if (!reset_to_windowed(fixture.device, fixture.window)) return;

  IDirect3DSurface9 *backbuffer = nullptr;
  IDirect3DSurface9 *surface = nullptr;
  CHECK_HR(fixture.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
      &backbuffer), D3D_OK);
  CHECK_HR(fixture.device->GetRenderTarget(0, &surface), D3D_OK);
  CHECK(surface == backbuffer);
  release_if(surface);
  release_if(backbuffer);

  for (unsigned int i = 1; i < rt_count; ++i) {
    surface = nullptr;
    CHECK_HR(fixture.device->GetRenderTarget(i, &surface), D3DERR_NOTFOUND);
    release_if(surface);
  }
}

void get_render_target_returns_user_surface() {
  Fixture fixture;
  if (!fixture.init("get_render_target_returns_user_surface")) return;

  IDirect3DTexture9 *texture = nullptr;
  HRESULT hr = fixture.device->CreateTexture(64, 64, 1,
      D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
      &texture, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!texture) return;

  IDirect3DSurface9 *render_target = nullptr;
  CHECK_HR(texture->GetSurfaceLevel(0, &render_target), D3D_OK);
  release_if(texture);
  if (!render_target) return;

  CHECK_HR(fixture.device->SetRenderTarget(0, render_target), D3D_OK);

  IDirect3DSurface9 *current = nullptr;
  CHECK_HR(fixture.device->GetRenderTarget(0, &current), D3D_OK);
  CHECK(current == render_target);
  release_if(current);

  D3DCAPS9 caps = {};
  CHECK_HR(fixture.device->GetDeviceCaps(&caps), D3D_OK);
  if (caps.NumSimultaneousRTs > 1) {
    IDirect3DTexture9 *texture1 = nullptr;
    hr = fixture.device->CreateTexture(64, 64, 1,
        D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &texture1, nullptr);
    CHECK_HR(hr, D3D_OK);
    if (!texture1) {
      release_if(render_target);
      return;
    }

    IDirect3DSurface9 *render_target1 = nullptr;
    CHECK_HR(texture1->GetSurfaceLevel(0, &render_target1), D3D_OK);
    release_if(texture1);
    if (!render_target1) {
      release_if(render_target);
      return;
    }

    CHECK_HR(fixture.device->SetRenderTarget(1, render_target1), D3D_OK);
    current = nullptr;
    CHECK_HR(fixture.device->GetRenderTarget(1, &current), D3D_OK);
    CHECK(current == render_target1);
    release_if(current);

    CHECK_HR(fixture.device->SetRenderTarget(1, nullptr), D3D_OK);
    current = reinterpret_cast<IDirect3DSurface9 *>(0x1);
    CHECK_HR(fixture.device->GetRenderTarget(1, &current), D3DERR_NOTFOUND);
    CHECK(current == nullptr);

    release_if(render_target1);
  } else {
    std::printf("SKIP:get_render_target_returns_user_surface: slot 1 unavailable\n");
    ++skips;
  }

  release_if(render_target);
}

void get_depth_stencil_returns_user_surface() {
  Fixture fixture;
  if (!fixture.init("get_depth_stencil_returns_user_surface")) return;

  IDirect3DSurface9 *depth_stencil = nullptr;
  HRESULT hr = fixture.device->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8,
      D3DMULTISAMPLE_NONE, 0, TRUE, &depth_stencil, nullptr);
  if (FAILED(hr)) {
    hr = fixture.device->CreateDepthStencilSurface(64, 64, D3DFMT_D16,
        D3DMULTISAMPLE_NONE, 0, TRUE, &depth_stencil, nullptr);
  }
  if (FAILED(hr) || !depth_stencil) {
    std::printf("SKIP:get_depth_stencil_returns_user_surface: depth/stencil surface unavailable\n");
    ++skips;
    return;
  }

  CHECK_HR(fixture.device->SetDepthStencilSurface(depth_stencil), D3D_OK);

  IDirect3DSurface9 *current = nullptr;
  CHECK_HR(fixture.device->GetDepthStencilSurface(&current), D3D_OK);
  CHECK(current == depth_stencil);
  release_if(current);

  CHECK_HR(fixture.device->SetDepthStencilSurface(nullptr), D3D_OK);
  current = reinterpret_cast<IDirect3DSurface9 *>(0x1);
  hr = fixture.device->GetDepthStencilSurface(&current);
  CHECK(hr != D3D_OK);
  CHECK(current == nullptr);

  release_if(depth_stencil);
}

void reset_ex_cooperative_level_smoke() {
  HWND window = create_window();
  if (!window) {
    std::printf("SKIP:reset_ex_cooperative_level_smoke: failed to create a window\n");
    ++skips;
    return;
  }

  IDirect3D9Ex *d3d9 = nullptr;
  HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9);
  if (FAILED(hr) || !d3d9) {
    std::printf("SKIP:reset_ex_cooperative_level_smoke: Direct3DCreate9Ex returned 0x%08lx\n",
        static_cast<unsigned long>(hr));
    ++skips;
    DestroyWindow(window);
    return;
  }

  IDirect3DDevice9Ex *device = create_device_ex(d3d9, window);
  if (!device) {
    std::printf("SKIP:reset_ex_cooperative_level_smoke: failed to create a D3D9Ex device\n");
    ++skips;
    release_if(d3d9);
    DestroyWindow(window);
    return;
  }

  CHECK_HR(device->TestCooperativeLevel(), D3D_OK);

  D3DPRESENT_PARAMETERS pp = make_present_parameters(window);
  hr = device->ResetEx(&pp, nullptr);
  CHECK_HR(hr, D3D_OK);
  CHECK_HR(device->TestCooperativeLevel(), D3D_OK);

  release_if(device);
  release_if(d3d9);
  DestroyWindow(window);
}

void reset_lost_default_pool_rebinding() {
  reset_default_pool_invalidation();
  reset_non_default_pool_survival();
  reset_render_target_rebinding();
  get_render_target_returns_user_surface();
  get_depth_stencil_returns_user_surface();
  reset_ex_cooperative_level_smoke();
}

}  // namespace

int main() {
  reset_lost_default_pool_rebinding();

  if (failures) {
    std::printf("d3d9_reset_lost_x64: %d failure(s), %d skip(s)\n", failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("d3d9_reset_lost_x64: passed (%d skip(s))\n", skips);
  return skips ? 77 : EXIT_SUCCESS;
}
