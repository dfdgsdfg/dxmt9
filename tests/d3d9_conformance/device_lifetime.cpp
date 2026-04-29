/*
 * D3D9 device/resource lifetime conformance checks.
 *
 * Wine provenance: distilled from Wine dlls/d3d9/tests/device.c at 6e073d2:
 * - test_refcount(): GetDirect3D and resource GetDevice AddRef expectations.
 * - test_scene(): BeginScene/EndScene invalid transition cases.
 * - test_private_data(): resource private-data, including D3DSPD_IUNKNOWN ownership.
 */

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int failures = 0;
int skips = 0;

void fail_at(int line, const char* message) {
  std::printf("FAIL:%d: %s\n", line, message);
  ++failures;
}

void check_at(int line, bool condition, const char* message) {
  if (!condition) fail_at(line, message);
}

void check_hr_at(int line, HRESULT actual, HRESULT expected, const char* call) {
  if (actual != expected) {
    std::printf("FAIL:%d: %s returned 0x%08lx, expected 0x%08lx\n",
                line, call, static_cast<unsigned long>(actual), static_cast<unsigned long>(expected));
    ++failures;
  }
}

void check_refcount_at(int line, IUnknown* object, ULONG expected, const char* object_name) {
  object->AddRef();
  ULONG actual = object->Release();
  if (actual != expected) {
    std::printf("FAIL:%d: %s refcount is %lu, expected %lu\n",
                line, object_name, static_cast<unsigned long>(actual), static_cast<unsigned long>(expected));
    ++failures;
  }
}

#define CHECK(condition) check_at(__LINE__, !!(condition), #condition)
#define CHECK_HR(actual, expected) check_hr_at(__LINE__, (actual), (expected), #actual)
#define CHECK_REFCOUNT(object, expected) check_refcount_at(__LINE__, static_cast<IUnknown*>(object), (expected), #object)

IUnknown* dead_unknown() {
  return reinterpret_cast<IUnknown*>(static_cast<std::uintptr_t>(0xdeadbeef));
}

HWND create_window() {
  RECT rect = {0, 0, 64, 64};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowA("static", "dxmt9_d3d9_lifetime", WS_OVERLAPPEDWINDOW,
                       0, 0, rect.right - rect.left, rect.bottom - rect.top,
                       nullptr, nullptr, nullptr, nullptr);
}

IDirect3DDevice9* create_device(IDirect3D9* d3d9, HWND window) {
  D3DPRESENT_PARAMETERS pp{};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = window;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9* device = nullptr;
  const DWORD flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  for (DWORD flags_value : flags) {
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                    flags_value, &pp, &device);
    if (SUCCEEDED(hr)) return device;
  }

  return nullptr;
}

struct Fixture {
  HWND window = nullptr;
  IDirect3D9* d3d9 = nullptr;
  IDirect3DDevice9* device = nullptr;

  bool init(const char* test_name) {
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
    if (device) device->Release();
    if (d3d9) d3d9->Release();
    if (window) DestroyWindow(window);
  }
};

void test_get_direct3d_addref() {
  Fixture fixture;
  if (!fixture.init("get_direct3d_addref")) return;

  CHECK_REFCOUNT(fixture.device, 1);
  CHECK_REFCOUNT(fixture.d3d9, 2);

  IDirect3D9* parent = nullptr;
  HRESULT hr = fixture.device->GetDirect3D(&parent);
  CHECK_HR(hr, D3D_OK);
  CHECK(parent == fixture.d3d9);
  CHECK_REFCOUNT(fixture.device, 1);
  CHECK_REFCOUNT(fixture.d3d9, 3);

  if (parent) parent->Release();
  CHECK_REFCOUNT(fixture.d3d9, 2);
}

void test_resource_get_device_addref() {
  Fixture fixture;
  if (!fixture.init("resource_get_device_addref")) return;

  IDirect3DSurface9* surface = nullptr;
  HRESULT hr = fixture.device->CreateOffscreenPlainSurface(
      4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!surface) return;

  CHECK_REFCOUNT(fixture.device, 2);
  CHECK_REFCOUNT(surface, 1);

  IDirect3DDevice9* from_surface = nullptr;
  hr = surface->GetDevice(&from_surface);
  CHECK_HR(hr, D3D_OK);
  CHECK(from_surface == fixture.device);
  CHECK_REFCOUNT(fixture.device, 3);

  if (from_surface) from_surface->Release();
  CHECK_REFCOUNT(fixture.device, 2);

  surface->Release();
  CHECK_REFCOUNT(fixture.device, 1);
}

void test_private_data_bytes() {
  static const GUID guid =
      {0x0e4c4c8c, 0x5bcb, 0x4e0d, {0xa1, 0x77, 0xc5, 0x65, 0x3c, 0xd6, 0xaf, 0x39}};

  Fixture fixture;
  if (!fixture.init("private_data_bytes")) return;

  IDirect3DSurface9* surface = nullptr;
  HRESULT hr = fixture.device->CreateOffscreenPlainSurface(
      4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!surface) return;

  const unsigned char expected[] = {0x10, 0x20, 0x30, 0x40};
  hr = surface->SetPrivateData(guid, expected, sizeof(expected), 0);
  CHECK_HR(hr, D3D_OK);

  DWORD size = 0;
  hr = surface->GetPrivateData(guid, nullptr, &size);
  CHECK_HR(hr, D3D_OK);
  CHECK(size == sizeof(expected));

  unsigned char actual[sizeof(expected)] = {};
  size = sizeof(actual);
  hr = surface->GetPrivateData(guid, actual, &size);
  CHECK_HR(hr, D3D_OK);
  CHECK(size == sizeof(expected));
  CHECK(std::memcmp(actual, expected, sizeof(expected)) == 0);

  hr = surface->FreePrivateData(guid);
  CHECK_HR(hr, D3D_OK);
  size = 0xdeadbabe;
  hr = surface->GetPrivateData(guid, actual, &size);
  CHECK_HR(hr, D3DERR_NOTFOUND);
  CHECK(size == 0xdeadbabe);

  surface->Release();
}

void test_private_data_iunknown() {
  static const GUID guid =
      {0xfdb37466, 0x428f, 0x4edf, {0xa3, 0x7f, 0x9b, 0x1d, 0xf4, 0x88, 0xc5, 0xfc}};
  static const GUID missing_guid =
      {0x38fb8fd2, 0x23f2, 0x44a1, {0xb7, 0x9a, 0x4b, 0x9b, 0xb8, 0x14, 0x4f, 0x1e}};

  Fixture fixture;
  if (!fixture.init("private_data_iunknown")) return;

  IDirect3DSurface9* surface = nullptr;
  HRESULT hr = fixture.device->CreateOffscreenPlainSurface(
      4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, nullptr);
  CHECK_HR(hr, D3D_OK);
  if (!surface) return;

  CHECK_REFCOUNT(fixture.device, 2);

  hr = surface->SetPrivateData(guid, fixture.device, 0, D3DSPD_IUNKNOWN);
  CHECK_HR(hr, D3DERR_INVALIDCALL);
  hr = surface->SetPrivateData(guid, fixture.device, 5, D3DSPD_IUNKNOWN);
  CHECK_HR(hr, D3DERR_INVALIDCALL);
  hr = surface->SetPrivateData(guid, fixture.device, sizeof(IUnknown*) * 2, D3DSPD_IUNKNOWN);
  CHECK_HR(hr, D3DERR_INVALIDCALL);

  hr = surface->SetPrivateData(guid, fixture.device, sizeof(IUnknown*), D3DSPD_IUNKNOWN);
  CHECK_HR(hr, D3D_OK);
  CHECK_REFCOUNT(fixture.device, 3);

  hr = surface->SetPrivateData(guid, fixture.device, sizeof(IUnknown*) * 2, D3DSPD_IUNKNOWN);
  CHECK_HR(hr, D3DERR_INVALIDCALL);

  IUnknown* got = nullptr;
  DWORD size = sizeof(got);
  hr = surface->GetPrivateData(guid, &got, &size);
  CHECK_HR(hr, D3D_OK);
  CHECK(size == sizeof(IUnknown*));
  CHECK(got == static_cast<IUnknown*>(fixture.device));
  CHECK_REFCOUNT(fixture.device, 4);
  if (got) got->Release();
  CHECK_REFCOUNT(fixture.device, 3);

  size = 1;
  got = dead_unknown();
  hr = surface->GetPrivateData(guid, &got, &size);
  CHECK_HR(hr, D3DERR_MOREDATA);
  CHECK(size == sizeof(IUnknown*));
  CHECK(got == dead_unknown());
  CHECK_REFCOUNT(fixture.device, 3);

  size = 1;
  hr = surface->GetPrivateData(guid, nullptr, &size);
  CHECK_HR(hr, D3D_OK);
  CHECK(size == sizeof(IUnknown*));
  CHECK_REFCOUNT(fixture.device, 3);

  hr = surface->GetPrivateData(missing_guid, nullptr, nullptr);
  CHECK_HR(hr, D3DERR_NOTFOUND);

  size = 0xdeadbabe;
  got = dead_unknown();
  hr = surface->GetPrivateData(missing_guid, &got, &size);
  CHECK_HR(hr, D3DERR_NOTFOUND);
  CHECK(size == 0xdeadbabe);
  CHECK(got == dead_unknown());

  hr = surface->FreePrivateData(guid);
  CHECK_HR(hr, D3D_OK);
  CHECK_REFCOUNT(fixture.device, 2);

  hr = surface->SetPrivateData(guid, fixture.device, sizeof(IUnknown*), D3DSPD_IUNKNOWN);
  CHECK_HR(hr, D3D_OK);
  CHECK_REFCOUNT(fixture.device, 3);

  surface->Release();
  CHECK_REFCOUNT(fixture.device, 1);
}

void test_scene_transitions() {
  Fixture fixture;
  if (!fixture.init("scene_transitions")) return;

  HRESULT hr = fixture.device->EndScene();
  CHECK_HR(hr, D3DERR_INVALIDCALL);

  hr = fixture.device->BeginScene();
  CHECK_HR(hr, D3D_OK);
  hr = fixture.device->EndScene();
  CHECK_HR(hr, D3D_OK);

  hr = fixture.device->EndScene();
  CHECK_HR(hr, D3DERR_INVALIDCALL);

  hr = fixture.device->BeginScene();
  CHECK_HR(hr, D3D_OK);
  hr = fixture.device->BeginScene();
  CHECK_HR(hr, D3DERR_INVALIDCALL);
  hr = fixture.device->EndScene();
  CHECK_HR(hr, D3D_OK);
  hr = fixture.device->EndScene();
  CHECK_HR(hr, D3DERR_INVALIDCALL);
}

}  // namespace

int main() {
  test_get_direct3d_addref();
  test_resource_get_device_addref();
  test_private_data_bytes();
  test_private_data_iunknown();
  test_scene_transitions();

  if (failures) {
    std::printf("%d failure(s), %d skip(s)\n", failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("device_lifetime: passed (%d skip(s))\n", skips);
  return EXIT_SUCCESS;
}
