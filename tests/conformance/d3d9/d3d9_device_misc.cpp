/*
 * Focused D3D9 device utility and creation-flag conformance scaffold.
 *
 * Wine behavioral oracle: dlls/d3d9/tests/device.c and
 * dlls/d3d9/device.c at 6e073d28dee3af7f4c965daec94644e0f9f92727.
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
  return CreateWindowA("static", "dxmt9_d3d9_device_misc", WS_OVERLAPPEDWINDOW,
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

IDirect3DDevice9 *create_device(IDirect3D9 *d3d9, HWND window, DWORD utility_flags) {
  const DWORD vp_flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  for (unsigned int depth_format = 0; depth_format < 2; ++depth_format) {
    D3DPRESENT_PARAMETERS pp = make_present_parameters(window);
    if (depth_format) pp.AutoDepthStencilFormat = D3DFMT_D16;

    for (DWORD vp_flag : vp_flags) {
      IDirect3DDevice9 *device = nullptr;
      HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
          window, vp_flag | utility_flags, &pp, &device);
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

    device = create_device(d3d9, window, 0);
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

void check_creation_parameters(IDirect3DDevice9 *device, HWND window) {
  D3DDEVICE_CREATION_PARAMETERS params = {};
  CHECK_HR(device->GetCreationParameters(&params), D3D_OK);
  CHECK(params.AdapterOrdinal == D3DADAPTER_DEFAULT);
  CHECK(params.DeviceType == D3DDEVTYPE_HAL);
  CHECK(params.hFocusWindow == window);
}

void device_utility_creation_flags() {
  Fixture fixture;
  if (!fixture.init("device_utility_creation_flags")) return;

  CHECK_HR(fixture.device->GetDirect3D(nullptr), D3DERR_INVALIDCALL);
  CHECK_HR(fixture.device->GetDeviceCaps(nullptr), D3DERR_INVALIDCALL);

  IDirect3D9 *parent = nullptr;
  CHECK_HR(fixture.device->GetDirect3D(&parent), D3D_OK);
  CHECK(parent == fixture.d3d9);
  release_if(parent);

  D3DCAPS9 caps = {};
  CHECK_HR(fixture.device->GetDeviceCaps(&caps), D3D_OK);
  CHECK(caps.AdapterOrdinal == D3DADAPTER_DEFAULT);
  CHECK(caps.DeviceType == D3DDEVTYPE_HAL);
  check_creation_parameters(fixture.device, fixture.window);

  CHECK(fixture.device->GetAvailableTextureMem() != 0);
  CHECK_HR(fixture.device->EvictManagedResources(), D3D_OK);

  CHECK_HR(fixture.device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1), D3D_OK);
  DWORD passes = 0xdeadbeef;
  CHECK_HR(fixture.device->ValidateDevice(&passes), D3D_OK);
  CHECK(passes == 1);

  D3DRASTER_STATUS raster_status = {};
  CHECK_HR_ANY(fixture.device->GetRasterStatus(0, &raster_status), D3D_OK, E_FAIL);
  CHECK_HR(fixture.device->GetRasterStatus(1, &raster_status), D3DERR_INVALIDCALL);

  CHECK_HR(fixture.device->SetDialogBoxMode(TRUE), D3D_OK);
  CHECK_HR(fixture.device->SetDialogBoxMode(FALSE), D3D_OK);

  const DWORD utility_flags[] = {
      D3DCREATE_MULTITHREADED,
      D3DCREATE_FPU_PRESERVE,
      D3DCREATE_NOWINDOWCHANGES,
      D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE | D3DCREATE_NOWINDOWCHANGES,
  };

  for (DWORD flags : utility_flags) {
    IDirect3DDevice9 *device = create_device(fixture.d3d9, fixture.window, flags);
    if (!device) {
      std::printf("SKIP:device_utility_creation_flags: failed to create device with utility flags 0x%08lx\n",
          static_cast<unsigned long>(flags));
      ++skips;
      continue;
    }
    check_creation_parameters(device, fixture.window);
    release_if(device);
  }

  D3DPRESENT_PARAMETERS pp = make_present_parameters(fixture.window);
  IDirect3DDevice9 *device =
      reinterpret_cast<IDirect3DDevice9 *>(static_cast<UINT_PTR>(0xdeadbeef));
  HRESULT hr = fixture.d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
      fixture.window,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_SOFTWARE_VERTEXPROCESSING,
      &pp, &device);
  CHECK_HR(hr, D3DERR_INVALIDCALL);
  CHECK(device == nullptr);
}

// gap_d3d9 D.* — regression gates for the remaining silent-S_OK COM stubs.
// These exercise the pure state round-trip / documented returns, not real
// per-primitive clip results or N-Patch tessellation (dxmt9 and Wine both
// treat these as state-only / no-op on the Metal path).
void device_com_stub_returns() {
  Fixture fixture;
  if (!fixture.init("device_com_stub_returns")) return;

  // SetClipStatus / GetClipStatus — pure state round-trip (gap_d3d9 B.8).
  // Null SetClipStatus is rejected with D3DERR_INVALIDCALL (mirrors
  // wined3d_device_set_clip_status's !clip_status guard).
  CHECK_HR(fixture.device->SetClipStatus(nullptr), D3DERR_INVALIDCALL);

  D3DCLIPSTATUS9 cs = {};
  cs.ClipUnion = 0x0000000Fu;
  cs.ClipIntersection = 0x00000003u;
  CHECK_HR(fixture.device->SetClipStatus(&cs), D3D_OK);

  D3DCLIPSTATUS9 out = {};
  out.ClipUnion = 0xdeadbeefu;
  out.ClipIntersection = 0xdeadbeefu;
  CHECK_HR(fixture.device->GetClipStatus(&out), D3D_OK);
  CHECK(out.ClipUnion == cs.ClipUnion);
  CHECK(out.ClipIntersection == cs.ClipIntersection);

  // A second Set/Get with different values must reflect the latest state.
  cs.ClipUnion = 0u;
  cs.ClipIntersection = 0xFFFFFFFFu;
  CHECK_HR(fixture.device->SetClipStatus(&cs), D3D_OK);
  out.ClipUnion = 0xdeadbeefu;
  out.ClipIntersection = 0xdeadbeefu;
  CHECK_HR(fixture.device->GetClipStatus(&out), D3D_OK);
  CHECK(out.ClipUnion == 0u);
  CHECK(out.ClipIntersection == 0xFFFFFFFFu);

  // SetNPatchMode / GetNPatchMode — disabling (0.0f) returns S_OK and the
  // getter reports 0.0f (N-Patch tessellation is a no-op on Metal).
  CHECK_HR(fixture.device->SetNPatchMode(0.0f), D3D_OK);
  CHECK(fixture.device->GetNPatchMode() == 0.0f);

  // DeletePatch — documented S_OK on this path; patch primitives are unused
  // on Metal so deleting any handle is a benign no-op.
  CHECK_HR(fixture.device->DeletePatch(0), D3D_OK);
}

}  // namespace

int main() {
  device_utility_creation_flags();
  device_com_stub_returns();

  if (failures) {
    std::printf("d3d9_device_misc_x64: %d failure(s), %d skip(s)\n", failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("d3d9_device_misc_x64: passed (%d skip(s))\n", skips);
  return skips ? 77 : EXIT_SUCCESS;
}
