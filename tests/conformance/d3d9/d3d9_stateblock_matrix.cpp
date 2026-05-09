/*
 * Focused D3D9 stateblock conformance scaffold.
 *
 * Wine behavioral oracle: dlls/d3d9/tests/stateblock.c
 * test_state_management() at 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * This is intentionally small: it only establishes PE coverage for stateblock
 * type creation and basic Capture/Apply behavior. The broader Wine matrix stays
 * represented by the scaffolded manifest status until runtime evidence exists.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cmath>
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

HWND create_window() {
  RECT rect = {0, 0, 64, 64};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowA("static", "dxmt9_d3d9_stateblock", WS_OVERLAPPEDWINDOW,
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
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9 *device = nullptr;
  const DWORD flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  for (DWORD flags_value : flags) {
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        window, flags_value, &pp, &device);
    if (SUCCEEDED(hr)) return device;
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
    if (device) device->Release();
    if (d3d9) d3d9->Release();
    if (window) DestroyWindow(window);
  }
};

D3DMATRIX make_matrix(float x, float y, float z) {
  D3DMATRIX matrix = {};
  matrix._11 = 1.0f;
  matrix._22 = 1.0f;
  matrix._33 = 1.0f;
  matrix._44 = 1.0f;
  matrix._41 = x;
  matrix._42 = y;
  matrix._43 = z;
  return matrix;
}

bool same_matrix(const D3DMATRIX &a, const D3DMATRIX &b) {
  const float *af = reinterpret_cast<const float *>(&a);
  const float *bf = reinterpret_cast<const float *>(&b);

  for (unsigned int i = 0; i < 16; ++i) {
    if (std::fabs(af[i] - bf[i]) > 0.0001f) return false;
  }

  return true;
}

void stateblock_type_create_matrix() {
  Fixture fixture;
  if (!fixture.init("stateblock_type_create_matrix")) return;

  const D3DSTATEBLOCKTYPE types[] = {
      D3DSBT_ALL,
      D3DSBT_VERTEXSTATE,
      D3DSBT_PIXELSTATE,
  };

  for (D3DSTATEBLOCKTYPE type : types) {
    IDirect3DStateBlock9 *stateblock = nullptr;
    CHECK_HR(fixture.device->CreateStateBlock(type, &stateblock), D3D_OK);
    CHECK(stateblock != nullptr);
    if (!stateblock) continue;

    CHECK_HR(stateblock->Capture(), D3D_OK);
    CHECK_HR(stateblock->Apply(), D3D_OK);
    stateblock->Release();
  }
}

void stateblock_capture_apply_render_state() {
  Fixture fixture;
  if (!fixture.init("stateblock_capture_apply_render_state")) return;

  IDirect3DStateBlock9 *stateblock = nullptr;
  DWORD lighting = 0;
  DWORD alpha_blend = 0;

  CHECK_HR(fixture.device->BeginStateBlock(), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, FALSE), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE), D3D_OK);
  CHECK_HR(fixture.device->EndStateBlock(&stateblock), D3D_OK);
  CHECK(stateblock != nullptr);
  if (!stateblock) return;

  CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, TRUE), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), D3D_OK);
  CHECK_HR(stateblock->Apply(), D3D_OK);

  CHECK_HR(fixture.device->GetRenderState(D3DRS_LIGHTING, &lighting), D3D_OK);
  CHECK_HR(fixture.device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha_blend),
      D3D_OK);
  CHECK(lighting == FALSE);
  CHECK(alpha_blend == TRUE);

  stateblock->Release();
}

void stateblock_capture_apply_transform_matrix() {
  Fixture fixture;
  if (!fixture.init("stateblock_capture_apply_transform_matrix")) return;

  const D3DMATRIX captured = make_matrix(1.0f, 2.0f, 3.0f);
  const D3DMATRIX changed = make_matrix(4.0f, 5.0f, 6.0f);
  D3DMATRIX actual = {};
  IDirect3DStateBlock9 *stateblock = nullptr;

  CHECK_HR(fixture.device->SetTransform(D3DTS_WORLD, &captured), D3D_OK);
  CHECK_HR(fixture.device->CreateStateBlock(D3DSBT_ALL, &stateblock), D3D_OK);
  CHECK(stateblock != nullptr);
  if (!stateblock) return;

  CHECK_HR(fixture.device->SetTransform(D3DTS_WORLD, &changed), D3D_OK);
  CHECK_HR(stateblock->Apply(), D3D_OK);
  CHECK_HR(fixture.device->GetTransform(D3DTS_WORLD, &actual), D3D_OK);
  CHECK(same_matrix(actual, captured));

  CHECK_HR(fixture.device->SetTransform(D3DTS_WORLD, &changed), D3D_OK);
  CHECK_HR(stateblock->Capture(), D3D_OK);
  CHECK_HR(fixture.device->SetTransform(D3DTS_WORLD, &captured), D3D_OK);
  CHECK_HR(stateblock->Apply(), D3D_OK);
  CHECK_HR(fixture.device->GetTransform(D3DTS_WORLD, &actual), D3D_OK);
  CHECK(same_matrix(actual, changed));

  stateblock->Release();
}

void stateblock_state_management_matrix() {
  stateblock_type_create_matrix();
  stateblock_capture_apply_render_state();
  stateblock_capture_apply_transform_matrix();
}

}  // namespace

int main() {
  stateblock_state_management_matrix();

  if (failures) {
    std::printf("d3d9_stateblock_x64: %d failure(s), %d skip(s)\n",
        failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("d3d9_stateblock_x64: passed (%d skip(s))\n", skips);
  return skips ? 77 : EXIT_SUCCESS;
}
