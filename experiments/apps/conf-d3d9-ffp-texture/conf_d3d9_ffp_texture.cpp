#include "../common/conf-d3d9-fast-sanity.hpp"

#include <cstring>

// Minimal self-validated fixed-function texturing test.
//
// Renders a full-screen pre-transformed (XYZRHW) quad with a solid-RED 4x4
// texture bound to stage 0 and the canonical FFP color pipeline
// (COLOROP=SELECTARG1, COLORARG1=TEXTURE). No vertex/pixel shaders are used,
// so this exercises the fixed-function pixel path end to end: SetTexture +
// texture-stage state -> sample -> backbuffer. The backbuffer is cleared to
// BLUE first, so a missing/ignored texture sample is unambiguous:
//   red   -> PASS (FFP sampled the bound texture)
//   blue  -> FAIL (geometry/texture not drawn)
//   white -> FAIL (FFP emitted diffuse/white instead of the texture)
//
// This is the controlled repro for the 3DMark05 "white scene" symptom, where
// FFP draws appeared to output white diffuse instead of their bound texture.

namespace {

using namespace dxmt9::fastsanity;

constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;
constexpr char kWindowClass[] = "dxmt9_ffp_texture_window";
constexpr char kWindowTitle[] = "conf-d3d9-ffp-texture";

struct Vertex {
  float x, y, z, rhw;
  float u, v;
};

bool runFfpTextureApp(HINSTANCE instance) {
  HWND hwnd = createWindow(instance, kWindowClass, kWindowTitle,
                           static_cast<int>(kWidth), static_cast<int>(kHeight));
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

  // 4x4 solid-red A8R8G8B8 texture. D3D9Ex rejects D3DPOOL_MANAGED, so use a
  // DYNAMIC DEFAULT-pool texture (lockable + sampleable on Ex devices).
  ComPtr<IDirect3DTexture9> tex;
  HRESULT hr = device->CreateTexture(4, 4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                     D3DPOOL_DEFAULT, tex.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateTexture", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  D3DLOCKED_RECT lr{};
  hr = tex->LockRect(0, &lr, nullptr, 0);
  if (FAILED(hr)) {
    log_hresult("Texture LockRect", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  for (UINT y = 0; y < 4; ++y) {
    auto* row = reinterpret_cast<D3DCOLOR*>(
        static_cast<unsigned char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
    for (UINT x = 0; x < 4; ++x) {
      row[x] = D3DCOLOR_ARGB(255, 255, 0, 0);
    }
  }
  tex->UnlockRect(0);

  // Full-screen pre-transformed quad: XYZRHW bypasses vertex processing, so
  // only the FFP pixel pipeline is under test.
  const Vertex verts[4] = {
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
      {static_cast<float>(kWidth), 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      {0.0f, static_cast<float>(kHeight), 0.0f, 1.0f, 0.0f, 1.0f},
      {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f, 1.0f, 1.0f},
  };
  ComPtr<IDirect3DVertexBuffer9> vb;
  hr = device->CreateVertexBuffer(static_cast<UINT>(sizeof(verts)), 0,
                                  D3DFVF_XYZRHW | D3DFVF_TEX1, D3DPOOL_DEFAULT,
                                  vb.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateVertexBuffer", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  void* mapped = nullptr;
  hr = vb->Lock(0, sizeof(verts), &mapped, 0);
  if (FAILED(hr) || !mapped) {
    log_hresult("VertexBuffer Lock", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  std::memcpy(mapped, verts, sizeof(verts));
  vb->Unlock();

  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  device->SetStreamSource(0, vb.ptr(), 0, static_cast<UINT>(sizeof(Vertex)));
  device->SetTexture(0, tex.ptr());
  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

  hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0);
  if (FAILED(hr)) {
    log_hresult("Clear", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  device->BeginScene();
  hr = device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
  device->EndScene();
  if (FAILED(hr)) {
    log_hresult("DrawPrimitive", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  D3DCOLOR pixel = 0;
  const bool readOk = readBackbufferPixel(device.ptr(), kWidth / 2, kHeight / 2, pixel);
  device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
  destroyWindow(instance, kWindowClass, hwnd);
  if (!readOk) {
    return false;
  }

  if (!colorNear(pixel, 255, 0, 0, 16)) {
    logf("FAIL: ffp-texture pixel rgb=(%u,%u,%u) expected red(255,0,0) — FFP "
         "did not sample the bound texture",
         static_cast<unsigned>(channelR(pixel)),
         static_cast<unsigned>(channelG(pixel)),
         static_cast<unsigned>(channelB(pixel)));
    return false;
  }

  logf("PASS: ffp-texture pixel rgb=(%u,%u,%u)",
       static_cast<unsigned>(channelR(pixel)),
       static_cast<unsigned>(channelG(pixel)),
       static_cast<unsigned>(channelB(pixel)));
  return true;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  return runFfpTextureApp(instance) ? 0 : 1;
}
