#include "../common/dx9_fast_sanity.hpp"

#include <array>
#include <cstring>

namespace {

constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;
constexpr char kWindowClass[] = "dxmt9_d9vk_triangle_window";
constexpr char kWindowTitle[] = "d9vk-d3d9-triangle";

const char kVertexShaderCode[] = R"(
struct VS_INPUT {
  float3 Position : POSITION;
};

struct VS_OUTPUT {
  float4 Position : POSITION;
};

VS_OUTPUT main(VS_INPUT input) {
  VS_OUTPUT output;
  output.Position = float4(input.Position, 1.0f);
  return output;
}
)";

const char kPixelShaderCode[] = R"(
struct VS_OUTPUT {
  float4 Position : POSITION;
};

sampler2D g_tex : register(s0);

float4 main(VS_OUTPUT input) : COLOR {
  return tex2D(g_tex, float2(0.25f, 0.25f));
}
)";

struct Vertex {
  float x;
  float y;
  float z;
};

bool compileVertexShader(IDirect3DDevice9Ex* device, IDirect3DVertexShader9** outShader) {
  using namespace dxmt9::fastsanity;
  ComPtr<ID3DXBuffer> bytecode;
  ComPtr<ID3DXBuffer> errors;
  HRESULT hr = D3DXCompileShader(kVertexShaderCode,
                                 static_cast<UINT>(std::strlen(kVertexShaderCode)),
                                 nullptr,
                                 nullptr,
                                 "main",
                                 "vs_2_0",
                                 0,
                                 bytecode.put(),
                                 errors.put(),
                                 nullptr);
  if (FAILED(hr)) {
    log_hresult("D3DXCompileShader(vs)", hr);
    if (errors) {
      logf("VS errors: %s", static_cast<const char*>(errors->GetBufferPointer()));
    }
    return false;
  }
  hr = device->CreateVertexShader(static_cast<const DWORD*>(bytecode->GetBufferPointer()), outShader);
  if (FAILED(hr)) {
    log_hresult("CreateVertexShader", hr);
    return false;
  }
  return true;
}

bool compilePixelShader(IDirect3DDevice9Ex* device, IDirect3DPixelShader9** outShader) {
  using namespace dxmt9::fastsanity;
  ComPtr<ID3DXBuffer> bytecode;
  ComPtr<ID3DXBuffer> errors;
  HRESULT hr = D3DXCompileShader(kPixelShaderCode,
                                 static_cast<UINT>(std::strlen(kPixelShaderCode)),
                                 nullptr,
                                 nullptr,
                                 "main",
                                 "ps_2_0",
                                 0,
                                 bytecode.put(),
                                 errors.put(),
                                 nullptr);
  if (FAILED(hr)) {
    log_hresult("D3DXCompileShader(ps)", hr);
    if (errors) {
      logf("PS errors: %s", static_cast<const char*>(errors->GetBufferPointer()));
    }
    return false;
  }
  hr = device->CreatePixelShader(static_cast<const DWORD*>(bytecode->GetBufferPointer()), outShader);
  if (FAILED(hr)) {
    log_hresult("CreatePixelShader", hr);
    return false;
  }
  return true;
}

bool createTextureResources(IDirect3DDevice9Ex* device,
                            dxmt9::fastsanity::ComPtr<IDirect3DTexture9>& defaultTexture,
                            dxmt9::fastsanity::ComPtr<IDirect3DTexture9>& sysmemTexture,
                            dxmt9::fastsanity::ComPtr<IDirect3DSurface9>& offscreenSurface) {
  using namespace dxmt9::fastsanity;
  HRESULT hr = device->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, defaultTexture.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateTexture(default)", hr);
    return false;
  }
  hr = device->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, sysmemTexture.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateTexture(systemmem)", hr);
    return false;
  }
  hr = device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, offscreenSurface.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateOffscreenPlainSurface", hr);
    return false;
  }

  D3DLOCKED_RECT offscreenLock{};
  hr = offscreenSurface->LockRect(&offscreenLock, nullptr, 0);
  if (FAILED(hr)) {
    log_hresult("OffscreenSurface::LockRect", hr);
    return false;
  }
  for (UINT y = 0; y < 64; ++y) {
    auto* row = reinterpret_cast<D3DCOLOR*>(
        static_cast<unsigned char*>(offscreenLock.pBits) + static_cast<size_t>(y) * offscreenLock.Pitch);
    for (UINT x = 0; x < 64; ++x) {
      row[x] = D3DCOLOR_ARGB(255, 40, 210, 40);
    }
  }
  offscreenSurface->UnlockRect();

  D3DLOCKED_RECT textureLock{};
  hr = sysmemTexture->LockRect(0, &textureLock, nullptr, 0);
  if (FAILED(hr)) {
    log_hresult("SysmemTexture::LockRect", hr);
    return false;
  }
  for (UINT y = 0; y < 64; ++y) {
    auto* row = reinterpret_cast<D3DCOLOR*>(
        static_cast<unsigned char*>(textureLock.pBits) + static_cast<size_t>(y) * textureLock.Pitch);
    for (UINT x = 0; x < 64; ++x) {
      row[x] = D3DCOLOR_ARGB(255, 40, 210, 40);
    }
  }
  sysmemTexture->UnlockRect(0);

  hr = device->UpdateTexture(sysmemTexture.ptr(), defaultTexture.ptr());
  if (FAILED(hr)) {
    log_hresult("UpdateTexture", hr);
    return false;
  }
  return true;
}

bool runTriangleApp(HINSTANCE instance) {
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

  ComPtr<IDirect3DSurface9> backbuffer;
  ComPtr<IDirect3DSwapChain9> swapchain;
  device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backbuffer.put());
  device->GetSwapChain(0, swapchain.put());

  ComPtr<IDirect3DVertexShader9> vs;
  ComPtr<IDirect3DPixelShader9> ps;
  if (!compileVertexShader(device.ptr(), vs.put()) || !compilePixelShader(device.ptr(), ps.put())) {
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  ComPtr<IDirect3DTexture9> defaultTexture;
  ComPtr<IDirect3DTexture9> sysmemTexture;
  ComPtr<IDirect3DSurface9> offscreenSurface;
  if (!createTextureResources(device.ptr(), defaultTexture, sysmemTexture, offscreenSurface)) {
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  ComPtr<IDirect3DSurface9> rt;
  ComPtr<IDirect3DSurface9> rt2;
  HRESULT hr = device->CreateRenderTarget(kWidth, kHeight, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, rt.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateRenderTarget(rt)", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  hr = device->CreateRenderTarget(kWidth, kHeight, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, rt2.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateRenderTarget(rt2)", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  ComPtr<IDirect3DSurface9> ds;
  hr = device->CreateDepthStencilSurface(kWidth, kHeight, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, FALSE, ds.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateDepthStencilSurface", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  const std::array<Vertex, 3> vertices = {{
      {0.0f, 0.75f, 0.0f},
      {0.75f, -0.75f, 0.0f},
      {-0.75f, -0.75f, 0.0f},
  }};

  static const D3DVERTEXELEMENT9 kVertexDecl[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };

  ComPtr<IDirect3DVertexDeclaration9> decl;
  hr = device->CreateVertexDeclaration(kVertexDecl, decl.put());
  if (FAILED(hr)) {
    log_hresult("CreateVertexDeclaration", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  ComPtr<IDirect3DVertexBuffer9> vb;
  hr = device->CreateVertexBuffer(static_cast<UINT>(sizeof(vertices)), 0, 0, D3DPOOL_DEFAULT, vb.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateVertexBuffer", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  void* mapped = nullptr;
  hr = vb->Lock(0, 0, &mapped, 0);
  if (FAILED(hr) || !mapped) {
    log_hresult("VertexBuffer::Lock", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  std::memcpy(mapped, vertices.data(), sizeof(vertices));
  vb->Unlock();

  device->SetRenderTarget(0, rt.ptr());
  device->SetDepthStencilSurface(ds.ptr());
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ZWRITEENABLE, 1);
  device->SetRenderState(D3DRS_ZENABLE, 1);
  device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  device->SetVertexDeclaration(decl.ptr());
  device->SetStreamSource(0, vb.ptr(), 0, sizeof(Vertex));
  device->SetVertexShader(vs.ptr());
  device->SetPixelShader(ps.ptr());
  device->SetTexture(0, defaultTexture.ptr());

  hr = device->BeginScene();
  if (FAILED(hr)) {
    log_hresult("BeginScene", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  hr = device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(30, 40, 160), 1.0f, 0);
  if (FAILED(hr)) {
    log_hresult("Clear", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  hr = device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
  if (FAILED(hr)) {
    log_hresult("DrawPrimitive", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  hr = device->EndScene();
  if (FAILED(hr)) {
    log_hresult("EndScene", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  hr = device->StretchRect(rt.ptr(), nullptr, rt2.ptr(), nullptr, D3DTEXF_LINEAR);
  if (FAILED(hr)) {
    log_hresult("StretchRect(rt->rt2)", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  hr = device->StretchRect(rt2.ptr(), nullptr, backbuffer.ptr(), nullptr, D3DTEXF_NONE);
  if (FAILED(hr)) {
    log_hresult("StretchRect(rt2->backbuffer)", hr);
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

  if (!colorNear(pixel, 40, 210, 40, 32)) {
    logf("FAIL: unexpected triangle pixel rgb=(%u,%u,%u)",
         static_cast<unsigned>(channelR(pixel)),
         static_cast<unsigned>(channelG(pixel)),
         static_cast<unsigned>(channelB(pixel)));
    return false;
  }

  logf("PASS: triangle pixel rgb=(%u,%u,%u)",
       static_cast<unsigned>(channelR(pixel)),
       static_cast<unsigned>(channelG(pixel)),
       static_cast<unsigned>(channelB(pixel)));
  return true;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  return runTriangleApp(instance) ? 0 : 1;
}
