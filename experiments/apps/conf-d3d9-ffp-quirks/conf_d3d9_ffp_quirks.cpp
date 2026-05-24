#include "../common/dx9_fast_sanity.hpp"

namespace {

constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;
constexpr char kWindowClass[] = "dxmt9_d9vk_ffp_quirks_window";
constexpr char kWindowTitle[] = "conf-d3d9-ffp-quirks";

struct TestStats {
  int passed = 0;
  int failed = 0;

  void expectOk(const char* name, HRESULT hr) {
    if (SUCCEEDED(hr)) {
      dxmt9::fastsanity::logf("PASS: %s hr=0x%08lx", name, static_cast<unsigned long>(hr));
      ++passed;
    } else {
      dxmt9::fastsanity::logf("FAIL: %s hr=0x%08lx", name, static_cast<unsigned long>(hr));
      ++failed;
    }
  }

  bool ok() const {
    return failed == 0;
  }
};

void runTextureStageClampTests(IDirect3DDevice9Ex* device, TestStats& stats) {
  stats.expectOk("TSS clamps large stage",
                 device->SetTextureStageState(64u, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
  stats.expectOk("TSS clamps large type",
                 device->SetTextureStageState(0u,
                                              static_cast<D3DTEXTURESTAGESTATETYPE>(0xffffffffu),
                                              D3DTOP_SELECTARG1));
  stats.expectOk("TSS clamps large stage and type",
                 device->SetTextureStageState(999u,
                                              static_cast<D3DTEXTURESTAGESTATETYPE>(999u),
                                              D3DTOP_DISABLE));
  stats.expectOk("TSS accepts normal state after clamp",
                 device->SetTextureStageState(0u, D3DTSS_COLOROP, D3DTOP_DISABLE));
}

bool runFixedFunctionQuirksApp(HINSTANCE instance) {
  using namespace dxmt9::fastsanity;

  const CaptureConfig capture = loadCaptureConfig();
  HWND hwnd = createWindow(instance, kWindowClass, kWindowTitle, static_cast<int>(kWidth), static_cast<int>(kHeight));
  if (!hwnd) {
    logf("FAIL: CreateWindowExA");
    return false;
  }

  ComPtr<IDirect3D9Ex> d3d;
  ComPtr<IDirect3DDevice9Ex> device;
  D3DPRESENT_PARAMETERS params{};
  if (!createDeviceEx(hwnd, kWidth, kHeight, d3d, device, params)) {
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  TestStats stats;
  runTextureStageClampTests(device.ptr(), stats);

  const D3DCOLOR clearColor = stats.ok() ? D3DCOLOR_XRGB(40, 190, 40) : D3DCOLOR_XRGB(190, 40, 40);
  HRESULT hr = device->BeginScene();
  if (FAILED(hr)) {
    log_hresult("BeginScene", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, clearColor, 0.0f, 0);
  if (FAILED(hr)) {
    log_hresult("Clear", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  hr = device->EndScene();
  if (FAILED(hr)) {
    log_hresult("EndScene", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  if (!captureBackbuffer(device.ptr(), capture)) {
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  hr = device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
  if (FAILED(hr)) {
    log_hresult("PresentEx", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  logf("SUMMARY: passed=%d failed=%d", stats.passed, stats.failed);
  destroyWindow(instance, kWindowClass, hwnd);
  return stats.ok();
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  return runFixedFunctionQuirksApp(instance) ? 0 : 1;
}
