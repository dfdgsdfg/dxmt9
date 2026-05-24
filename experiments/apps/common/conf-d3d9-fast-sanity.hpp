#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dxmt9::fastsanity {

template <typename T>
class ComPtr {
public:
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
    other.ptr_ = nullptr;
  }

  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }

  ~ComPtr() {
    reset();
  }

  T* ptr() const { return ptr_; }
  T** put() {
    reset();
    return &ptr_;
  }

  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

  void reset(T* value = nullptr) {
    if (ptr_) {
      ptr_->Release();
    }
    ptr_ = value;
  }

private:
  T* ptr_ = nullptr;
};

struct CaptureConfig {
  bool enabled = false;
  char path[MAX_PATH]{};
};

inline void logf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stdout, fmt, args);
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
  va_end(args);
}

inline void log_hresult(const char* label, HRESULT hr) {
  std::fprintf(stderr, "FAIL: %s hr=0x%08lx\n", label, static_cast<unsigned long>(hr));
  std::fflush(stderr);
}

inline CaptureConfig loadCaptureConfig() {
  CaptureConfig capture;
  const char* path = std::getenv("DXMT_EXPERIMENT_CAPTURE_PATH");
  if (path && path[0] != '\0') {
    capture.enabled = true;
    std::snprintf(capture.path, sizeof(capture.path), "%s", path);
  }
  return capture;
}

inline LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcA(hwnd, message, wparam, lparam);
  }
}

inline HWND createWindow(HINSTANCE instance, const char* className, const char* title, int width, int height) {
  WNDCLASSEXA wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = className;
  RegisterClassExA(&wc);

  HWND hwnd = CreateWindowExA(0,
                              className,
                              title,
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              width,
                              height,
                              nullptr,
                              nullptr,
                              instance,
                              nullptr);
  if (hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
  }
  return hwnd;
}

inline void destroyWindow(HINSTANCE instance, const char* className, HWND hwnd) {
  if (hwnd) {
    DestroyWindow(hwnd);
  }
  UnregisterClassA(className, instance);
}

inline void fillPresentParams(HWND hwnd, UINT width, UINT height, D3DPRESENT_PARAMETERS& params) {
  std::memset(&params, 0, sizeof(params));
  params.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
  params.BackBufferCount = 1;
  params.BackBufferFormat = D3DFMT_X8R8G8B8;
  params.BackBufferWidth = width;
  params.BackBufferHeight = height;
  params.EnableAutoDepthStencil = FALSE;
  params.Flags = 0;
  params.FullScreen_RefreshRateInHz = 0;
  params.hDeviceWindow = hwnd;
  params.MultiSampleQuality = 0;
  params.MultiSampleType = D3DMULTISAMPLE_NONE;
  params.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.Windowed = TRUE;
}

inline bool createDeviceEx(HWND hwnd,
                           UINT width,
                           UINT height,
                           ComPtr<IDirect3D9Ex>& d3d,
                           ComPtr<IDirect3DDevice9Ex>& device,
                           D3DPRESENT_PARAMETERS& params) {
  HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, d3d.put());
  if (FAILED(hr)) {
    log_hresult("Direct3DCreate9Ex", hr);
    return false;
  }

  fillPresentParams(hwnd, width, height, params);
  hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT,
                           D3DDEVTYPE_HAL,
                           hwnd,
                           D3DCREATE_HARDWARE_VERTEXPROCESSING,
                           &params,
                           nullptr,
                           device.put());
  if (FAILED(hr)) {
    log_hresult("CreateDeviceEx", hr);
    return false;
  }
  return true;
}

inline bool saveSurfaceToBmp(IDirect3DSurface9* surface, const CaptureConfig& capture) {
  if (!capture.enabled || !surface) {
    return true;
  }
  const HRESULT hr = D3DXSaveSurfaceToFileA(capture.path, D3DXIFF_BMP, surface, nullptr, nullptr);
  if (FAILED(hr)) {
    log_hresult("D3DXSaveSurfaceToFileA", hr);
    return false;
  }
  logf("OK: captured -> %s", capture.path);
  return true;
}

inline bool copyRenderTargetToSysmem(IDirect3DDevice9* device,
                                     IDirect3DSurface9* source,
                                     ComPtr<IDirect3DSurface9>& staging) {
  if (!device || !source) {
    return false;
  }
  D3DSURFACE_DESC desc{};
  source->GetDesc(&desc);
  HRESULT hr = device->CreateOffscreenPlainSurface(desc.Width,
                                                   desc.Height,
                                                   desc.Format,
                                                   D3DPOOL_SYSTEMMEM,
                                                   staging.put(),
                                                   nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateOffscreenPlainSurface", hr);
    return false;
  }
  hr = device->GetRenderTargetData(source, staging.ptr());
  if (FAILED(hr)) {
    log_hresult("GetRenderTargetData", hr);
    staging.reset();
    return false;
  }
  return true;
}

inline bool captureBackbuffer(IDirect3DDevice9* device, const CaptureConfig& capture) {
  if (!capture.enabled) {
    return true;
  }
  ComPtr<IDirect3DSurface9> backbuffer;
  HRESULT hr = device->GetRenderTarget(0, backbuffer.put());
  if (FAILED(hr)) {
    log_hresult("GetRenderTarget(backbuffer)", hr);
    return false;
  }
  ComPtr<IDirect3DSurface9> staging;
  if (!copyRenderTargetToSysmem(device, backbuffer.ptr(), staging)) {
    return false;
  }
  return saveSurfaceToBmp(staging.ptr(), capture);
}

inline bool readSurfacePixel(IDirect3DSurface9* surface, UINT x, UINT y, D3DCOLOR& color) {
  if (!surface) {
    return false;
  }
  D3DLOCKED_RECT lock{};
  HRESULT hr = surface->LockRect(&lock, nullptr, D3DLOCK_READONLY);
  if (FAILED(hr)) {
    log_hresult("LockRect(read pixel)", hr);
    return false;
  }
  D3DSURFACE_DESC desc{};
  surface->GetDesc(&desc);
  if (x >= desc.Width || y >= desc.Height) {
    surface->UnlockRect();
    return false;
  }
  const auto* row = reinterpret_cast<const D3DCOLOR*>(
      static_cast<const unsigned char*>(lock.pBits) + static_cast<size_t>(y) * lock.Pitch);
  color = row[x];
  surface->UnlockRect();
  return true;
}

inline bool readBackbufferPixel(IDirect3DDevice9* device, UINT x, UINT y, D3DCOLOR& color) {
  ComPtr<IDirect3DSurface9> backbuffer;
  HRESULT hr = device->GetRenderTarget(0, backbuffer.put());
  if (FAILED(hr)) {
    log_hresult("GetRenderTarget(backbuffer pixel)", hr);
    return false;
  }
  ComPtr<IDirect3DSurface9> staging;
  if (!copyRenderTargetToSysmem(device, backbuffer.ptr(), staging)) {
    return false;
  }
  return readSurfacePixel(staging.ptr(), x, y, color);
}

inline unsigned char channelR(D3DCOLOR color) {
  return static_cast<unsigned char>((color >> 16) & 0xffu);
}

inline unsigned char channelG(D3DCOLOR color) {
  return static_cast<unsigned char>((color >> 8) & 0xffu);
}

inline unsigned char channelB(D3DCOLOR color) {
  return static_cast<unsigned char>(color & 0xffu);
}

inline bool channelNear(unsigned char actual, unsigned char expected, unsigned char tolerance) {
  const int delta = static_cast<int>(actual) - static_cast<int>(expected);
  return delta >= -static_cast<int>(tolerance) && delta <= static_cast<int>(tolerance);
}

inline bool colorNear(D3DCOLOR color,
                      unsigned char r,
                      unsigned char g,
                      unsigned char b,
                      unsigned char tolerance) {
  return channelNear(channelR(color), r, tolerance) &&
         channelNear(channelG(color), g, tolerance) &&
         channelNear(channelB(color), b, tolerance);
}

}  // namespace dxmt9::fastsanity
