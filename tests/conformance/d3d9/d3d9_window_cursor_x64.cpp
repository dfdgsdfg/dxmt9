/*
 * Focused D3D9 window/cursor conformance scaffold.
 *
 * Wine provenance: distilled from Wine dlls/d3d9/tests/device.c at
 * 6e073d28dee3af7f4c965daec94644e0f9f92727:
 * - test_cursor(), test_cursor_pos(), test_cursor_clipping()
 * - test_wndproc(), test_wndproc_windowed(), test_window_style()
 * - test_device_window_reset(), test_destroyed_window()
 *
 * This keeps only compact PE probes for cursor API shape, clip ownership,
 * window ownership/reset parameters, wndproc/style preservation in windowed
 * mode, and destroyed-window rendering. Full fullscreen message/style matrix
 * coverage remains scaffolded until host-specific runtime evidence is added.
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

LRESULT CALLBACK test_wndproc(HWND window, UINT message, WPARAM wparam,
    LPARAM lparam) {
  return DefWindowProcA(window, message, wparam, lparam);
}

bool register_window_class(const char *class_name) {
  WNDCLASSA wc = {};
  wc.lpfnWndProc = test_wndproc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = class_name;

  if (RegisterClassA(&wc)) return true;
  if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) return true;

  std::printf("SKIP:%s: RegisterClassA failed, error 0x%08lx\n",
      class_name, static_cast<unsigned long>(GetLastError()));
  ++skips;
  return false;
}

HWND create_test_window(const char *class_name, const char *title, DWORD style) {
  RECT rect = {0, 0, 96, 72};
  AdjustWindowRect(&rect, style, FALSE);
  return CreateWindowA(class_name, title, style, 0, 0,
      rect.right - rect.left, rect.bottom - rect.top,
      nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
}

HWND create_static_window(const char *title) {
  return create_test_window("static", title, WS_OVERLAPPEDWINDOW);
}

D3DPRESENT_PARAMETERS make_present_parameters(HWND device_window) {
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = device_window;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  return pp;
}

IDirect3DDevice9 *create_device(IDirect3D9 *d3d9, HWND focus_window,
    HWND device_window, DWORD utility_flags = 0) {
  const DWORD flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  D3DPRESENT_PARAMETERS pp = make_present_parameters(device_window);
  for (DWORD flag : flags) {
    IDirect3DDevice9 *device = nullptr;
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        focus_window, flag | utility_flags, &pp, &device);
    if (SUCCEEDED(hr)) return device;
  }

  return nullptr;
}

struct Fixture {
  HWND window = nullptr;
  IDirect3D9 *d3d9 = nullptr;
  IDirect3DDevice9 *device = nullptr;

  bool init(const char *test_name) {
    window = create_static_window("dxmt9_d3d9_window_cursor");
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

    device = create_device(d3d9, window, window);
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

RECT virtual_screen_rect() {
  RECT rect = {};
  rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  return rect;
}

void cursor_api_surface_shape() {
  Fixture fixture;
  if (!fixture.init("cursor_api_surface_shape")) return;

  BOOL visible = fixture.device->ShowCursor(TRUE);
  CHECK(visible == FALSE);
  visible = fixture.device->ShowCursor(TRUE);
  CHECK(visible == FALSE);

  CHECK_HR(fixture.device->SetCursorProperties(0, 0, nullptr),
      D3DERR_INVALIDCALL);

  IDirect3DSurface9 *cursor = nullptr;
  HRESULT hr = fixture.device->CreateOffscreenPlainSurface(32, 32,
      D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &cursor, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!cursor) return;

  CHECK_HR(fixture.device->SetCursorProperties(0, 0, cursor), D3D_OK);
  release_if(cursor);

  visible = fixture.device->ShowCursor(TRUE);
  CHECK(visible == FALSE);
  visible = fixture.device->ShowCursor(TRUE);
  CHECK(visible == TRUE);
  fixture.device->ShowCursor(FALSE);

  struct CursorSize {
    UINT width;
    UINT height;
    HRESULT expected;
  };

  const CursorSize sizes[] = {
      {1, 1, D3D_OK},
      {2, 4, D3D_OK},
      {3, 2, D3DERR_INVALIDCALL},
      {2, 3, D3DERR_INVALIDCALL},
      {6, 6, D3DERR_INVALIDCALL},
  };

  for (const CursorSize &size : sizes) {
    cursor = nullptr;
    hr = fixture.device->CreateOffscreenPlainSurface(size.width, size.height,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cursor, nullptr);
    CHECK_HR(hr, D3D_OK);
    if (!cursor) continue;

    CHECK_HR(fixture.device->SetCursorProperties(0, 0, cursor),
        size.expected);
    release_if(cursor);
  }
}

void cursor_position_and_clipping() {
  Fixture fixture;
  if (!fixture.init("cursor_position_and_clipping")) return;

  CHECK(ClipCursor(nullptr));

  RECT expected = virtual_screen_rect();
  RECT actual = {};
  CHECK(GetClipCursor(&actual));
  CHECK(EqualRect(&actual, &expected));

  RECT clip = {};
  CHECK(GetWindowRect(fixture.window, &clip));
  CHECK(ClipCursor(&clip));
  CHECK(GetClipCursor(&actual));
  CHECK(EqualRect(&actual, &clip));

  fixture.device->SetCursorPosition(clip.left + 1, clip.top + 1, 0);

  CHECK(ClipCursor(nullptr));
  CHECK(GetClipCursor(&actual));
  CHECK(EqualRect(&actual, &expected));
}

void check_swapchain_window(IDirect3DDevice9 *device, HWND expected_window) {
  IDirect3DSwapChain9 *swapchain = nullptr;
  CHECK_HR(device->GetSwapChain(0, &swapchain), D3D_OK);
  if (!swapchain) return;

  D3DPRESENT_PARAMETERS pp = {};
  CHECK_HR(swapchain->GetPresentParameters(&pp), D3D_OK);
  CHECK(pp.hDeviceWindow == expected_window);
  release_if(swapchain);
}

void window_ownership_wndproc_style() {
  const char *class_name = "dxmt9_d3d9_window_cursor_wc";
  if (!register_window_class(class_name)) return;

  HWND focus_window = create_test_window(class_name, "dxmt9_focus",
      WS_OVERLAPPEDWINDOW);
  HWND device_window = create_test_window(class_name, "dxmt9_device",
      WS_OVERLAPPEDWINDOW);
  if (!focus_window || !device_window) {
    std::printf("SKIP:window_ownership_wndproc_style: failed to create windows\n");
    ++skips;
    if (device_window) DestroyWindow(device_window);
    if (focus_window) DestroyWindow(focus_window);
    UnregisterClassA(class_name, GetModuleHandleA(nullptr));
    return;
  }

  LONG_PTR expected_proc = reinterpret_cast<LONG_PTR>(test_wndproc);
  LONG focus_style = GetWindowLongA(focus_window, GWL_STYLE);
  LONG focus_exstyle = GetWindowLongA(focus_window, GWL_EXSTYLE);
  LONG device_style = GetWindowLongA(device_window, GWL_STYLE);
  LONG device_exstyle = GetWindowLongA(device_window, GWL_EXSTYLE);

  CHECK(GetWindowLongPtrA(focus_window, GWLP_WNDPROC) == expected_proc);
  CHECK(GetWindowLongPtrA(device_window, GWLP_WNDPROC) == expected_proc);

  IDirect3D9 *d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d9) {
    std::printf("SKIP:window_ownership_wndproc_style: Direct3DCreate9 failed\n");
    ++skips;
    DestroyWindow(device_window);
    DestroyWindow(focus_window);
    UnregisterClassA(class_name, GetModuleHandleA(nullptr));
    return;
  }

  IDirect3DDevice9 *device = create_device(d3d9, focus_window, device_window);
  if (!device) {
    std::printf("SKIP:window_ownership_wndproc_style: failed to create a D3D9 device\n");
    ++skips;
    release_if(d3d9);
    DestroyWindow(device_window);
    DestroyWindow(focus_window);
    UnregisterClassA(class_name, GetModuleHandleA(nullptr));
    return;
  }

  D3DDEVICE_CREATION_PARAMETERS creation = {};
  CHECK_HR(device->GetCreationParameters(&creation), D3D_OK);
  CHECK(creation.hFocusWindow == focus_window);
  check_swapchain_window(device, device_window);

  CHECK(GetWindowLongPtrA(focus_window, GWLP_WNDPROC) == expected_proc);
  CHECK(GetWindowLongPtrA(device_window, GWLP_WNDPROC) == expected_proc);

  D3DPRESENT_PARAMETERS pp = make_present_parameters(focus_window);
  CHECK_HR(device->Reset(&pp), D3D_OK);
  check_swapchain_window(device, focus_window);

  CHECK(GetWindowLongPtrA(focus_window, GWLP_WNDPROC) == expected_proc);
  CHECK(GetWindowLongPtrA(device_window, GWLP_WNDPROC) == expected_proc);
  CHECK(GetWindowLongA(focus_window, GWL_STYLE) == focus_style);
  CHECK(GetWindowLongA(focus_window, GWL_EXSTYLE) == focus_exstyle);
  CHECK(GetWindowLongA(device_window, GWL_STYLE) == device_style);
  CHECK(GetWindowLongA(device_window, GWL_EXSTYLE) == device_exstyle);

  release_if(device);
  release_if(d3d9);
  DestroyWindow(device_window);
  DestroyWindow(focus_window);
  UnregisterClassA(class_name, GetModuleHandleA(nullptr));
}

void destroyed_window_device_survives() {
  HWND window = create_static_window("dxmt9_destroyed_window");
  if (!window) {
    std::printf("SKIP:destroyed_window_device_survives: failed to create a window\n");
    ++skips;
    return;
  }

  IDirect3D9 *d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d9) {
    std::printf("SKIP:destroyed_window_device_survives: Direct3DCreate9 failed\n");
    ++skips;
    DestroyWindow(window);
    return;
  }

  IDirect3DDevice9 *device = create_device(d3d9, window, window);
  release_if(d3d9);
  DestroyWindow(window);

  if (!device) {
    std::printf("SKIP:destroyed_window_device_survives: failed to create a D3D9 device\n");
    ++skips;
    return;
  }

  CHECK_HR(device->BeginScene(), D3D_OK);
  CHECK_HR(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 0.0f, 0),
      D3D_OK);
  CHECK_HR(device->EndScene(), D3D_OK);
  CHECK_HR(device->Present(nullptr, nullptr, nullptr, nullptr), D3D_OK);
  release_if(device);
}

void window_cursor_ownership() {
  cursor_api_surface_shape();
  cursor_position_and_clipping();
  window_ownership_wndproc_style();
  destroyed_window_device_survives();
}

}  // namespace

int main() {
  window_cursor_ownership();

  if (failures) {
    std::printf("d3d9_window_cursor_x64: %d failure(s), %d skip(s)\n", failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("d3d9_window_cursor_x64: passed (%d skip(s))\n", skips);
  return skips ? 77 : EXIT_SUCCESS;
}
