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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

D3DPRESENT_PARAMETERS make_present_parameters(HWND window) {
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = window;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  return pp;
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

void stateblock_reset_recovery() {
  Fixture fixture;
  if (!fixture.init("stateblock_reset_recovery")) return;

  IDirect3DStateBlock9 *before_reset = nullptr;
  CHECK_HR(fixture.device->CreateStateBlock(D3DSBT_ALL, &before_reset), D3D_OK);
  CHECK(before_reset != nullptr);
  if (!before_reset) return;
  CHECK_HR(before_reset->Capture(), D3D_OK);

  D3DPRESENT_PARAMETERS pp = make_present_parameters(fixture.window);
  pp.BackBufferWidth = 32;
  pp.BackBufferHeight = 32;
  CHECK_HR(fixture.device->Reset(&pp), D3D_OK);

  IDirect3DStateBlock9 *after_reset = nullptr;
  CHECK_HR(fixture.device->CreateStateBlock(D3DSBT_ALL, &after_reset), D3D_OK);
  CHECK(after_reset != nullptr);
  if (after_reset) {
    CHECK_HR(after_reset->Capture(), D3D_OK);
    CHECK_HR(after_reset->Apply(), D3D_OK);
    after_reset->Release();
  }
  before_reset->Release();
}

void stateblock_reject_foreign_wrapper() {
  Fixture fixture;
  if (!fixture.init("stateblock_reject_foreign_wrapper")) return;

  IDirect3DDevice9 *foreign_device = create_device(fixture.d3d9, fixture.window);
  CHECK(foreign_device != nullptr);
  if (!foreign_device) return;

  D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
       D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };
  IDirect3DVertexDeclaration9 *foreign_decl = nullptr;
  CHECK_HR(foreign_device->CreateVertexDeclaration(elements, &foreign_decl),
           D3D_OK);
  CHECK(foreign_decl != nullptr);
  if (foreign_decl) {
    IDirect3DStateBlock9 *stateblock = nullptr;
    CHECK_HR(fixture.device->BeginStateBlock(), D3D_OK);
    CHECK_HR(fixture.device->SetVertexDeclaration(foreign_decl),
             D3DERR_INVALIDCALL);
    CHECK_HR(fixture.device->EndStateBlock(&stateblock), D3D_OK);
    if (stateblock) stateblock->Release();
    foreign_decl->Release();
  }
  foreign_device->Release();
}

HRESULT configured_fault_hr(const char *fault) {
  const char *equals = std::strchr(fault, '=');
  if (!equals || !equals[1]) {
    return std::strstr(fault, "alloc_pre")
               ? static_cast<HRESULT>(0x8007000e)
               : static_cast<HRESULT>(0x80004005);
  }
  return static_cast<HRESULT>(std::strtoul(equals + 1, nullptr, 0));
}

bool fault_is(const char *fault, const char *name) {
  const std::size_t name_length = std::strlen(name);
  return std::strncmp(fault, name, name_length) == 0 &&
      (fault[name_length] == '\0' || fault[name_length] == '=');
}

void fault_reset_recovery(Fixture &fixture,
                          IDirect3DVertexBuffer9 *reset_blocker) {
  CHECK(reset_blocker != nullptr);
  CHECK_HR(fixture.device->Reset(nullptr), D3DERR_INVALIDCALL);
  D3DPRESENT_PARAMETERS failed_pp = make_present_parameters(fixture.window);
  CHECK_HR(fixture.device->Reset(&failed_pp), D3DERR_INVALIDCALL);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, FALSE),
      D3DERR_DEVICELOST);
  reset_blocker->Release();
  D3DPRESENT_PARAMETERS pp = make_present_parameters(fixture.window);
  pp.BackBufferWidth = 32;
  pp.BackBufferHeight = 32;
  CHECK_HR(fixture.device->Reset(&pp), D3D_OK);
  IDirect3DStateBlock9 *fresh = nullptr;
  CHECK_HR(fixture.device->CreateStateBlock(D3DSBT_ALL, &fresh), D3D_OK);
  CHECK(fresh != nullptr);
  if (fresh) {
    CHECK_HR(fresh->Capture(), D3D_OK);
    fresh->Release();
  }
}

void stateblock_fault_pre(const char *fault) {
  Fixture fixture;
  if (!fixture.init("stateblock_fault_pre")) return;
  const HRESULT expected = configured_fault_hr(fault);

  if (fault_is(fault, "alloc_pre")) {
    IDirect3DStateBlock9 *stateblock =
        reinterpret_cast<IDirect3DStateBlock9 *>(static_cast<uintptr_t>(1));
    CHECK_HR(fixture.device->CreateStateBlock(D3DSBT_ALL, &stateblock), expected);
    CHECK(stateblock == nullptr);
    CHECK_HR(fixture.device->CreateStateBlock(D3DSBT_ALL, &stateblock), D3D_OK);
    CHECK(stateblock != nullptr);
    if (stateblock) stateblock->Release();
    return;
  }

  if (fault_is(fault, "end_pre")) {
    IDirect3DStateBlock9 *stateblock = nullptr;
    CHECK_HR(fixture.device->BeginStateBlock(), D3D_OK);
    CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, FALSE), D3D_OK);
    CHECK_HR(fixture.device->EndStateBlock(&stateblock), expected);
    CHECK(stateblock == nullptr);
    if (stateblock) stateblock->Release();
    CHECK_HR(fixture.device->EndStateBlock(&stateblock), D3D_OK);
    CHECK(stateblock != nullptr);
    if (stateblock) stateblock->Release();
    return;
  }

  IDirect3DStateBlock9 *stateblock = nullptr;
  CHECK_HR(fixture.device->CreateStateBlock(D3DSBT_ALL, &stateblock), D3D_OK);
  CHECK(stateblock != nullptr);
  if (!stateblock) return;
  if (fault_is(fault, "capture_pre")) {
    CHECK_HR(stateblock->Capture(), expected);
    CHECK_HR(stateblock->Capture(), D3D_OK);
  } else {
    CHECK_HR(stateblock->Capture(), D3D_OK);
    CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, FALSE), D3D_OK);
    CHECK_HR(stateblock->Apply(), expected);
    CHECK_HR(stateblock->Apply(), D3D_OK);
  }
  stateblock->Release();
}

void stateblock_fault_entered(const char *fault) {
  Fixture fixture;
  if (!fixture.init("stateblock_fault_entered")) return;
  const HRESULT expected = configured_fault_hr(fault);
  const bool end_fault = fault_is(fault, "end_entered");
  // The generic bridge seam is consumed by the first Capture in this
  // fixture.  It is intentionally not a preparatory Capture: entered faults
  // are one-shot observations of the actual backend entry point.
  const bool capture_fault = fault_is(fault, "capture_entered") ||
      fault_is(fault, "bridge_entered");

  if (end_fault) {
    IDirect3DVertexBuffer9 *reset_blocker = nullptr;
    CHECK_HR(fixture.device->CreateVertexBuffer(
        64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
        &reset_blocker, nullptr), D3D_OK);
    if (!reset_blocker) return;
    IDirect3DStateBlock9 *stateblock = nullptr;
    CHECK_HR(fixture.device->BeginStateBlock(), D3D_OK);
    CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, FALSE), D3D_OK);
    CHECK_HR(fixture.device->EndStateBlock(&stateblock), expected);
    CHECK(stateblock == nullptr);
    CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, TRUE),
        D3DERR_DEVICELOST);
    fault_reset_recovery(fixture, reset_blocker);
    return;
  }

  IDirect3DVertexBuffer9 *reset_blocker = nullptr;
  CHECK_HR(fixture.device->CreateVertexBuffer(
      64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
      &reset_blocker, nullptr), D3D_OK);
  if (!reset_blocker) return;
  IDirect3DStateBlock9 *stateblock = nullptr;
  CHECK_HR(fixture.device->CreateStateBlock(D3DSBT_ALL, &stateblock), D3D_OK);
  CHECK(stateblock != nullptr);
  if (!stateblock) {
    reset_blocker->Release();
    return;
  }
  CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, FALSE), D3D_OK);
  if (capture_fault) {
    CHECK_HR(stateblock->Capture(), expected);
    // CaptureEntered/BridgeEntered poison after the backend call.  The
    // disposition is effect-unknown, so only fail-stop/reset behavior is
    // asserted here; no snapshot value is inferred.
  } else {
    CHECK_HR(stateblock->Capture(), D3D_OK);
    CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, TRUE), D3D_OK);
    // ApplyEntered enters the backend before reporting its injected HRESULT.
    // Its post-call state is effect-unknown across the unchanged C ABI, so
    // this fixture deliberately records only the HRESULT and fail-stop path.
    CHECK_HR(stateblock->Apply(), expected);
  }
  CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, TRUE),
      D3DERR_DEVICELOST);
  stateblock->Release();
  fault_reset_recovery(fixture, reset_blocker);
}

void stateblock_state_management_matrix() {
  stateblock_type_create_matrix();
  stateblock_capture_apply_render_state();
  stateblock_capture_apply_transform_matrix();
  stateblock_reset_recovery();
  stateblock_reject_foreign_wrapper();
}

}  // namespace

int main() {
  const char *fault = std::getenv("DXMT9_PE_STATEBLOCK_FAULT");
  if (fault && *fault) {
    if (fault_is(fault, "capture_pre") || fault_is(fault, "apply_pre") ||
        fault_is(fault, "end_pre") || fault_is(fault, "alloc_pre")) {
      stateblock_fault_pre(fault);
    } else {
      stateblock_fault_entered(fault);
    }
  } else {
    stateblock_state_management_matrix();
  }

  if (failures) {
    std::printf("d3d9_stateblock_x64: %d failure(s), %d skip(s)\n",
        failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("d3d9_stateblock_x64: passed (%d skip(s))\n", skips);
  return skips ? 77 : EXIT_SUCCESS;
}
