/*
 * Clean-room D3D9 PE recorder fault fixture.
 *
 * This executable is intentionally one selector per process.  The selector
 * is read from DXMT9_PE_RECORDER_FAULT, the selected API path is exercised,
 * and a REACHED marker is printed only after the expected recovery/accounting
 * checks complete.  The production recorder also emits an exact
 * DXMT9_PE_RECORDER_FAULT_CONSUMED receipt at the selected seam; the runner
 * requires both lines.  It is a dxmt9 policy probe, not copied Wine test code.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

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

void check_hr_at(int line, HRESULT actual, HRESULT expected, const char *call) {
  if (actual != expected) {
    std::printf("FAIL:%d: %s returned 0x%08lx, expected 0x%08lx\n", line,
        call, static_cast<unsigned long>(actual),
        static_cast<unsigned long>(expected));
    ++failures;
  }
}

#define CHECK_HR(actual, expected) check_hr_at(__LINE__, (actual), (expected), #actual)

HWND create_window() {
  RECT rect = {0, 0, 64, 64};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowA("static", "dxmt9_d3d9_recorder_fault",
      WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left,
      rect.bottom - rect.top, nullptr, nullptr, nullptr, nullptr);
}

D3DPRESENT_PARAMETERS present_parameters(HWND window) {
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

IDirect3DDevice9 *create_device(IDirect3D9 *d3d9, HWND window) {
  const DWORD flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };
  for (DWORD behavior : flags) {
    D3DPRESENT_PARAMETERS pp = present_parameters(window);
    IDirect3DDevice9 *device = nullptr;
    if (SUCCEEDED(d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                     window, behavior, &pp, &device))) {
      return device;
    }
  }
  return nullptr;
}

struct Fixture {
  HWND window = nullptr;
  IDirect3D9 *d3d9 = nullptr;
  IDirect3DDevice9 *device = nullptr;

  bool init(const char *name) {
    window = create_window();
    if (!window) {
      std::printf("SKIP:run_fault: window creation unsupported (%s)\n", name);
      ++skips;
      return false;
    }
    d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) {
      std::printf("SKIP:run_fault: Direct3DCreate9 unavailable (%s)\n", name);
      ++skips;
      return false;
    }
    device = create_device(d3d9, window);
    if (!device) {
      std::printf("SKIP:run_fault: D3D9 device creation unsupported (%s)\n", name);
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

bool fault_is(const char *fault, const char *name) {
  const std::size_t length = std::strlen(name);
  return std::strncmp(fault, name, length) == 0 &&
      (fault[length] == '\0' || fault[length] == '=');
}

HRESULT fault_hr(const char *fault, HRESULT default_hr) {
  const char *equals = std::strchr(fault, '=');
  if (!equals || !equals[1]) return default_hr;
  // Decimal retain_acquire values are successful-acquisition budgets, not
  // HRESULT spellings.  Hex remains the explicit way to override the fault
  // result for that selector.
  if (fault_is(fault, "retain_acquire") && equals[1] != '0') {
    return default_hr;
  }
  if (fault_is(fault, "retain_acquire") && equals[1] == '0' &&
      equals[2] != 'x' && equals[2] != 'X') {
    return default_hr;
  }
  return static_cast<HRESULT>(std::strtoul(equals + 1, nullptr, 0));
}

bool known_fault(const char *fault) {
  return fault_is(fault, "capacity_pre_reserve") ||
      fault_is(fault, "retain_acquire") || fault_is(fault, "bridge_pre") ||
      fault_is(fault, "bridge_entered") ||
      fault_is(fault, "capture_disposition") ||
      fault_is(fault, "capture_throw") || fault_is(fault, "reset") ||
      fault_is(fault, "teardown");
}

void mark_reached(const char *fault) {
  std::printf("REACHED:recorder_fault:%s\n", fault);
}

void capacity_pre_reserve(const char *fault) {
  Fixture fixture;
  if (!fixture.init("recorder_capacity_pre_reserve")) return;
  const HRESULT expected = fault_hr(fault, E_OUTOFMEMORY);
  // The runner sets the clean-room byte cap to 50.  A one-rectangle Clear
  // has a 48-byte size hint and a 16-byte payload, so it remains pending;
  // the next Clear crosses the cap and enters the pre-reserve seam.
  D3DRECT rect = {0, 0, 1, 1};
  CHECK_HR(fixture.device->Clear(1, &rect, D3DCLEAR_TARGET, 0, 1.0f, 0),
      D3D_OK);
  CHECK_HR(fixture.device->Clear(1, &rect, D3DCLEAR_TARGET, 0, 1.0f, 0),
      expected);
  // The one-shot pre-effect failure must preserve the first sealed bytes and
  // allow the next call to flush and append normally.
  CHECK_HR(fixture.device->Clear(1, &rect, D3DCLEAR_TARGET, 0, 1.0f, 0),
      D3D_OK);
  mark_reached(fault);
}

void retain_acquire(const char *fault) {
  Fixture fixture;
  if (!fixture.init("recorder_retain_acquire")) return;

  IDirect3DTexture9 *texture = nullptr;
  IDirect3DVertexBuffer9 *vertex_buffer = nullptr;
  CHECK_HR(fixture.device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8,
      D3DPOOL_DEFAULT, &texture, nullptr), D3D_OK);
  CHECK_HR(fixture.device->CreateVertexBuffer(3 * 16, D3DUSAGE_WRITEONLY,
      0, D3DPOOL_DEFAULT, &vertex_buffer, nullptr), D3D_OK);
  if (!texture || !vertex_buffer) {
    if (texture) texture->Release();
    if (vertex_buffer) vertex_buffer->Release();
    return;
  }
  CHECK_HR(fixture.device->SetTexture(0, texture), D3D_OK);
  CHECK_HR(fixture.device->SetStreamSource(0, vertex_buffer, 0, 16), D3D_OK);
  CHECK_HR(fixture.device->SetFVF(D3DFVF_XYZRHW), D3D_OK);
  // The draw record contains both the texture and vertex-buffer identities.
  // retain_acquire=0 fails before either unique acquire; =1 fails after the
  // first and consequently exercises rollback of a partial retain.
  const HRESULT expected = fault_hr(fault, E_OUTOFMEMORY);
  CHECK_HR(fixture.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1), expected);
  CHECK_HR(fixture.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
  texture->Release();
  vertex_buffer->Release();
  mark_reached(fault);
}

HRESULT clear_then_present(Fixture &fixture) {
  CHECK_HR(fixture.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0),
      D3D_OK);
  return fixture.device->Present(nullptr, nullptr, nullptr, nullptr);
}

void reset_recovery(Fixture &fixture) {
  D3DPRESENT_PARAMETERS pp = present_parameters(fixture.window);
  CHECK_HR(fixture.device->Reset(&pp), D3D_OK);
  CHECK_HR(fixture.device->TestCooperativeLevel(), D3D_OK);
}

void bridge_pre(const char *fault) {
  Fixture fixture;
  if (!fixture.init("recorder_bridge_pre")) return;
  const HRESULT expected = fault_hr(fault, E_FAIL);
  CHECK_HR(clear_then_present(fixture), expected);
  // The retry must submit precisely the sealed bytes retained by the failed
  // pre-effect attempt; no second record or resource acquisition is allowed.
  CHECK_HR(fixture.device->Present(nullptr, nullptr, nullptr, nullptr), D3D_OK);
  mark_reached(fault);
}

void bridge_entered(const char *fault) {
  Fixture fixture;
  if (!fixture.init("recorder_bridge_entered")) return;
  const HRESULT expected = fault_hr(fault, E_FAIL);
  CHECK_HR(clear_then_present(fixture), expected);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, FALSE),
      D3DERR_DEVICELOST);
  reset_recovery(fixture);
  mark_reached(fault);
}

bool capture_requested() {
  const char *enabled = std::getenv("DXMT9_RENDER_TAPE_CAPTURE");
  const char *root = std::getenv("DXMT9_RENDER_TAPE_OUTPUT_ROOT");
  if (enabled && *enabled && root && *root) return true;
  std::printf("SKIP:run_fault: render-tape capture is unsupported\n");
  ++skips;
  return false;
}

void capture_fault(const char *fault) {
  if (!capture_requested()) return;
  Fixture fixture;
  if (!fixture.init("recorder_capture_fault")) return;
  const HRESULT expected = fault_hr(fault, E_FAIL);
  // Present #1 commits ordinary work and arms the one-interval capture at
  // the successful present boundary. Present #2 is therefore the active
  // capture commit where both capture selectors are consumed.
  CHECK_HR(clear_then_present(fixture), D3D_OK);
  CHECK_HR(clear_then_present(fixture), D3D_OK);
  (void)expected;  // Capture faults reject/abort capture, not the accepted command.
  mark_reached(fault);
}

void reset_fault(const char *fault) {
  Fixture fixture;
  if (!fixture.init("recorder_reset")) return;
  const HRESULT expected = fault_hr(fault, E_FAIL);
  D3DPRESENT_PARAMETERS pp = present_parameters(fixture.window);
  CHECK_HR(fixture.device->Reset(&pp), expected);
  CHECK_HR(fixture.device->Reset(&pp), D3D_OK);
  CHECK_HR(fixture.device->TestCooperativeLevel(), D3D_OK);
  mark_reached(fault);
}

void teardown_fault(const char *fault) {
  {
    Fixture fixture;
    if (!fixture.init("recorder_teardown")) return;
    CHECK_HR(fixture.device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0),
        D3D_OK);
  }
  // This marker is deliberately after Fixture destruction, so the selected
  // teardown path and its pending ownership drain have completed.
  mark_reached(fault);
}

void run_fault(const char *fault) {
  std::printf("SELECTED:recorder_fault:%s\n", fault);
  if (std::strchr(fault, ',')) {
    fail_at(__LINE__, "multiple recorder selectors are forbidden");
    return;
  }
  if (!known_fault(fault)) {
    fail_at(__LINE__, "unknown recorder selector");
    return;
  }
  if (fault_is(fault, "capacity_pre_reserve")) capacity_pre_reserve(fault);
  else if (fault_is(fault, "retain_acquire")) retain_acquire(fault);
  else if (fault_is(fault, "bridge_pre")) bridge_pre(fault);
  else if (fault_is(fault, "bridge_entered")) bridge_entered(fault);
  else if (fault_is(fault, "capture_disposition") ||
           fault_is(fault, "capture_throw")) capture_fault(fault);
  else if (fault_is(fault, "reset")) reset_fault(fault);
  else teardown_fault(fault);
}

}  // namespace

int main() {
  const char *fault = std::getenv("DXMT9_PE_RECORDER_FAULT");
  if (!fault || !*fault) {
    std::printf("SKIP:run_fault: selector is unset\n");
    ++skips;
  } else {
    run_fault(fault);
  }

  if (failures) {
    std::printf("recorder_fault_matrix: %d failure(s), %d skip(s)\n",
        failures, skips);
    return EXIT_FAILURE;
  }
  if (skips) {
    std::printf("recorder_fault_matrix: skipped (%d)\n", skips);
    return 77;
  }
  std::printf("recorder_fault_matrix: passed\n");
  return EXIT_SUCCESS;
}
