#include "../common/dx9_fast_sanity.hpp"

namespace {

constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;
constexpr char kWindowClass[] = "dxmt9_d9vk_clear_window";
constexpr char kWindowTitle[] = "d9vk-d3d9-clear";

bool runClearApp(HINSTANCE instance) {
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
  const bool ok = createDeviceEx(hwnd, kWidth, kHeight, d3d, device, params);
  if (!ok) {
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  HRESULT hr = device->ResetEx(&params, nullptr);
  if (FAILED(hr)) {
    log_hresult("ResetEx", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  hr = device->BeginScene();
  if (FAILED(hr)) {
    log_hresult("BeginScene", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 0.0f, 0);
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

  D3DCOLOR pixel = 0;
  if (!readBackbufferPixel(device.ptr(), kWidth / 2, kHeight / 2, pixel)) {
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

  destroyWindow(instance, kWindowClass, hwnd);

  if (!colorNear(pixel, 255, 0, 0, 8)) {
    logf("FAIL: unexpected clear pixel rgb=(%u,%u,%u)",
         static_cast<unsigned>(channelR(pixel)),
         static_cast<unsigned>(channelG(pixel)),
         static_cast<unsigned>(channelB(pixel)));
    return false;
  }

  logf("PASS: clear pixel rgb=(%u,%u,%u)",
       static_cast<unsigned>(channelR(pixel)),
       static_cast<unsigned>(channelG(pixel)),
       static_cast<unsigned>(channelB(pixel)));
  return true;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  return runClearApp(instance) ? 0 : 1;
}
