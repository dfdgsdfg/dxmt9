#include "../common/dx9_fast_sanity.hpp"

#include <array>
#include <cstring>

namespace {

constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;
constexpr UINT kBufferSize = 512;
constexpr char kWindowClass[] = "dxmt9_d9vk_buffer_window";
constexpr char kWindowTitle[] = "conf-d3d9-buffer";

constexpr std::array<DWORD, 4> kUsagePermutations = {
    0u,
    D3DUSAGE_DYNAMIC,
    D3DUSAGE_WRITEONLY,
    D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC,
};

constexpr std::array<DWORD, 4> kMapFlagPermutations = {
    0u,
    D3DLOCK_DISCARD,
    D3DLOCK_DONOTWAIT,
    D3DLOCK_NOOVERWRITE,
};

bool testBufferPermutation(IDirect3DDevice9Ex* device, DWORD usage, DWORD flags, const unsigned char* expected) {
  using namespace dxmt9::fastsanity;

  ComPtr<IDirect3DVertexBuffer9> buffer;
  HRESULT hr = device->CreateVertexBuffer(kBufferSize, usage, 0, D3DPOOL_DEFAULT, buffer.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateVertexBuffer", hr);
    logf("FAIL: usage=0x%x flags=0x%x create", static_cast<unsigned>(usage), static_cast<unsigned>(flags));
    return false;
  }

  void* mapped = nullptr;
  hr = buffer->Lock(0, 0, &mapped, flags);
  if (FAILED(hr) || mapped == nullptr) {
    log_hresult("VertexBuffer::Lock(write)", hr);
    logf("FAIL: usage=0x%x flags=0x%x lock-write", static_cast<unsigned>(usage), static_cast<unsigned>(flags));
    return false;
  }
  std::memcpy(mapped, expected, kBufferSize);
  hr = buffer->Unlock();
  if (FAILED(hr)) {
    log_hresult("VertexBuffer::Unlock(write)", hr);
    return false;
  }

  mapped = nullptr;
  hr = buffer->Lock(0, 0, &mapped, 0);
  if (FAILED(hr) || mapped == nullptr) {
    log_hresult("VertexBuffer::Lock(readback)", hr);
    logf("FAIL: usage=0x%x flags=0x%x lock-readback", static_cast<unsigned>(usage), static_cast<unsigned>(flags));
    return false;
  }
  const bool match = std::memcmp(mapped, expected, kBufferSize) == 0;
  hr = buffer->Unlock();
  if (FAILED(hr)) {
    log_hresult("VertexBuffer::Unlock(readback)", hr);
    return false;
  }
  if (!match) {
    logf("FAIL: usage=0x%x flags=0x%x data mismatch", static_cast<unsigned>(usage), static_cast<unsigned>(flags));
    return false;
  }
  logf("PASS: usage=0x%x flags=0x%x", static_cast<unsigned>(usage), static_cast<unsigned>(flags));
  return true;
}

bool runBufferApp(HINSTANCE instance) {
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

  std::array<unsigned char, kBufferSize> data{};
  data.fill(0xfc);

  bool allPassed = true;
  for (DWORD usage : kUsagePermutations) {
    for (DWORD flags : kMapFlagPermutations) {
      allPassed = testBufferPermutation(device.ptr(), usage, flags, data.data()) && allPassed;
    }
  }

  const D3DCOLOR clearColor = allPassed ? D3DCOLOR_XRGB(40, 190, 40) : D3DCOLOR_XRGB(190, 40, 40);
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

  destroyWindow(instance, kWindowClass, hwnd);
  return allPassed;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  return runBufferApp(instance) ? 0 : 1;
}
