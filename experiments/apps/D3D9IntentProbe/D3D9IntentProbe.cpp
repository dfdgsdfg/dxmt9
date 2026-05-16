#include "../common/dx9_fast_sanity.hpp"

#include <cmath>

namespace {

using dxmt9::fastsanity::CaptureConfig;
using dxmt9::fastsanity::ComPtr;
using dxmt9::fastsanity::captureBackbuffer;
using dxmt9::fastsanity::colorNear;
using dxmt9::fastsanity::createDeviceEx;
using dxmt9::fastsanity::createWindow;
using dxmt9::fastsanity::destroyWindow;
using dxmt9::fastsanity::loadCaptureConfig;
using dxmt9::fastsanity::log_hresult;
using dxmt9::fastsanity::logf;
using dxmt9::fastsanity::readBackbufferPixel;
using dxmt9::fastsanity::readSurfacePixel;

constexpr UINT kWidth = 160;
constexpr UINT kHeight = 120;
constexpr char kWindowClass[] = "dxmt9_d3d9_intent_probe_window";
constexpr char kWindowTitle[] = "DXMT9 D3D9 Intent Probe";

struct TestStats {
  int passed = 0;
  int failed = 0;

  void expect(bool ok, const char* name) {
    logf("%s: %s", ok ? "PASS" : "FAIL", name);
    ok ? ++passed : ++failed;
  }

  void expectHr(const char* name, HRESULT hr) {
    if (SUCCEEDED(hr)) {
      logf("PASS: %s hr=0x%08lx", name, static_cast<unsigned long>(hr));
      ++passed;
    } else {
      log_hresult(name, hr);
      ++failed;
    }
  }

  bool ok() const {
    return failed == 0;
  }
};

struct RhwVertex {
  float x;
  float y;
  float z;
  float rhw;
  DWORD color;
};

struct BlendVertex {
  float x;
  float y;
  float z;
  float blend;
  DWORD color;
};

class IntentProbe {
public:
  explicit IntentProbe(HINSTANCE instance) : instance_(instance) {}

  bool init() {
    capture_ = loadCaptureConfig();
    hwnd_ = createWindow(instance_, kWindowClass, kWindowTitle, kWidth, kHeight);
    if (!hwnd_) {
      logf("FAIL: CreateWindowExA");
      return false;
    }
    return createDeviceEx(hwnd_, kWidth, kHeight, d3d_, device_, params_);
  }

  ~IntentProbe() {
    device_.reset();
    d3d_.reset();
    destroyWindow(instance_, kWindowClass, hwnd_);
  }

  bool run(const char* mode) {
    TestStats stats;
    if (std::strcmp(mode, "basic-ffp") == 0) {
      runBasicFfp(stats);
    } else if (std::strcmp(mode, "render-state") == 0) {
      runRenderState(stats);
    } else if (std::strcmp(mode, "blit-copy") == 0) {
      runBlitCopy(stats);
    } else if (std::strcmp(mode, "stateblock") == 0) {
      runStateBlock(stats);
    } else if (std::strcmp(mode, "query") == 0) {
      runQuery(stats);
    } else if (std::strcmp(mode, "ffp-vertex-blend") == 0) {
      runFfpVertexBlend(stats);
    } else {
      logf("FAIL: unknown mode '%s'", mode);
      stats.failed++;
    }

    if (stats.ok()) {
      captureBackbuffer(device_.ptr(), capture_);
    }
    const HRESULT presentHr = device_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    stats.expectHr("PresentEx", presentHr);
    logf("SUMMARY: mode=%s passed=%d failed=%d", mode, stats.passed, stats.failed);
    return stats.ok();
  }

private:
  void resetFixedFunctionState() {
    device_->SetVertexShader(nullptr);
    device_->SetPixelShader(nullptr);
    device_->SetTexture(0, nullptr);
    device_->SetRenderState(D3DRS_LIGHTING, FALSE);
    device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_COLORWRITEENABLE,
                            D3DCOLORWRITEENABLE_RED |
                                D3DCOLORWRITEENABLE_GREEN |
                                D3DCOLORWRITEENABLE_BLUE |
                                D3DCOLORWRITEENABLE_ALPHA);
    device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device_->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  }

  void clearBackbuffer(D3DCOLOR color, TestStats& stats) {
    stats.expectHr("BeginScene", device_->BeginScene());
    stats.expectHr("Clear(backbuffer)", device_->Clear(0, nullptr, D3DCLEAR_TARGET, color, 0.0f, 0));
  }

  void finishScene(TestStats& stats) {
    stats.expectHr("EndScene", device_->EndScene());
  }

  void drawRhwQuad(float left, float top, float right, float bottom, DWORD color, TestStats& stats) {
    const RhwVertex vertices[4] = {
        {left, top, 0.5f, 1.0f, color},
        {right, top, 0.5f, 1.0f, color},
        {left, bottom, 0.5f, 1.0f, color},
        {right, bottom, 0.5f, 1.0f, color},
    };
    drawVertexBuffer(vertices, sizeof(vertices), sizeof(vertices[0]), D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                     "XYZRHW|DIFFUSE", stats);
  }

  void drawVertexBuffer(const void* vertices, UINT byteSize, UINT stride, DWORD fvf, const char* label,
                        TestStats& stats) {
    ComPtr<IDirect3DVertexBuffer9> vb;
    HRESULT hr = device_->CreateVertexBuffer(byteSize, D3DUSAGE_WRITEONLY, fvf, D3DPOOL_DEFAULT, vb.put(), nullptr);
    stats.expectHr("CreateVertexBuffer(draw)", hr);
    if (FAILED(hr)) {
      return;
    }
    void* mapped = nullptr;
    hr = vb->Lock(0, byteSize, &mapped, 0);
    stats.expectHr("VertexBuffer.Lock(draw)", hr);
    if (FAILED(hr)) {
      return;
    }
    std::memcpy(mapped, vertices, byteSize);
    stats.expectHr("VertexBuffer.Unlock(draw)", vb->Unlock());
    stats.expectHr("SetFVF(draw)", device_->SetFVF(fvf));
    stats.expectHr("SetStreamSource(draw)", device_->SetStreamSource(0, vb.ptr(), 0, stride));
    stats.expectHr(label, device_->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
  }

  bool readBackbufferNear(UINT x, UINT y, unsigned char r, unsigned char g, unsigned char b,
                          const char* label, TestStats& stats) {
    D3DCOLOR color = 0;
    const bool read = readBackbufferPixel(device_.ptr(), x, y, color);
    const bool matches = read && colorNear(color, r, g, b, 8);
    if (!matches && read) {
      logf("DETAIL: %s actual rgb=(%u,%u,%u) expected rgb=(%u,%u,%u)",
           label,
           dxmt9::fastsanity::channelR(color),
           dxmt9::fastsanity::channelG(color),
           dxmt9::fastsanity::channelB(color),
           r,
           g,
           b);
    }
    stats.expect(matches, label);
    return matches;
  }

  void runBasicFfp(TestStats& stats) {
    resetFixedFunctionState();
    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    drawRhwQuad(20.0f, 20.0f, 140.0f, 100.0f, D3DCOLOR_ARGB(255, 230, 230, 230), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 230, 230, 230, "basic-ffp center readback", stats);
  }

  void runRenderState(TestStats& stats) {
    resetFixedFunctionState();
    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    stats.expectHr("SetRenderState(COLORWRITEENABLE RG)",
                   device_->SetRenderState(D3DRS_COLORWRITEENABLE,
                                           D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN));
    drawRhwQuad(10.0f, 10.0f, 70.0f, 70.0f, D3DCOLOR_ARGB(255, 255, 255, 255), stats);

    stats.expectHr("SetRenderState(COLORWRITEENABLE all)",
                   device_->SetRenderState(D3DRS_COLORWRITEENABLE,
                                           D3DCOLORWRITEENABLE_RED |
                                               D3DCOLORWRITEENABLE_GREEN |
                                               D3DCOLORWRITEENABLE_BLUE |
                                               D3DCOLORWRITEENABLE_ALPHA));
    stats.expectHr("SetRenderState(ALPHATESTENABLE true)",
                   device_->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE));
    stats.expectHr("SetRenderState(ALPHAREF 128)", device_->SetRenderState(D3DRS_ALPHAREF, 128));
    stats.expectHr("SetRenderState(ALPHAFUNC GREATER)",
                   device_->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER));
    drawRhwQuad(90.0f, 10.0f, 150.0f, 50.0f, D3DCOLOR_ARGB(64, 255, 0, 0), stats);
    drawRhwQuad(90.0f, 70.0f, 150.0f, 110.0f, D3DCOLOR_ARGB(255, 0, 220, 0), stats);
    finishScene(stats);

    readBackbufferNear(40, 40, 255, 255, 0, "render-state color-write RG", stats);
    readBackbufferNear(120, 30, 0, 0, 0, "render-state alpha-test reject", stats);
    readBackbufferNear(120, 90, 0, 220, 0, "render-state alpha-test accept", stats);
  }

  void fillLockedSurface(IDirect3DSurface9* surface, D3DCOLOR color, TestStats& stats) {
    D3DLOCKED_RECT locked{};
    const HRESULT lockHr = surface->LockRect(&locked, nullptr, 0);
    stats.expectHr("LockRect(fill surface)", lockHr);
    if (FAILED(lockHr)) {
      return;
    }
    D3DSURFACE_DESC desc{};
    surface->GetDesc(&desc);
    for (UINT y = 0; y < desc.Height; ++y) {
      auto* row = reinterpret_cast<D3DCOLOR*>(
          static_cast<unsigned char*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch);
      for (UINT x = 0; x < desc.Width; ++x) {
        row[x] = color;
      }
    }
    stats.expectHr("UnlockRect(fill surface)", surface->UnlockRect());
  }

  bool createRt(UINT width, UINT height, ComPtr<IDirect3DSurface9>& surface, TestStats& stats) {
    const HRESULT hr = device_->CreateRenderTarget(width,
                                                   height,
                                                   D3DFMT_A8R8G8B8,
                                                   D3DMULTISAMPLE_NONE,
                                                   0,
                                                   FALSE,
                                                   surface.put(),
                                                   nullptr);
    stats.expectHr("CreateRenderTarget", hr);
    return SUCCEEDED(hr);
  }

  bool createSysmemSurface(UINT width, UINT height, ComPtr<IDirect3DSurface9>& surface, TestStats& stats) {
    const HRESULT hr = device_->CreateOffscreenPlainSurface(width,
                                                            height,
                                                            D3DFMT_A8R8G8B8,
                                                            D3DPOOL_SYSTEMMEM,
                                                            surface.put(),
                                                            nullptr);
    stats.expectHr("CreateOffscreenPlainSurface(sysmem)", hr);
    return SUCCEEDED(hr);
  }

  bool copyRtToSysmem(IDirect3DSurface9* rt, ComPtr<IDirect3DSurface9>& staging, TestStats& stats) {
    if (!createSysmemSurface(32, 32, staging, stats)) {
      return false;
    }
    const HRESULT hr = device_->GetRenderTargetData(rt, staging.ptr());
    stats.expectHr("GetRenderTargetData", hr);
    return SUCCEEDED(hr);
  }

  void expectSurfacePixel(IDirect3DSurface9* surface, UINT x, UINT y, unsigned char r, unsigned char g,
                          unsigned char b, const char* label, TestStats& stats) {
    D3DCOLOR color = 0;
    const bool read = readSurfacePixel(surface, x, y, color);
    const bool matches = read && colorNear(color, r, g, b, 8);
    if (!matches && read) {
      logf("DETAIL: %s actual rgb=(%u,%u,%u) expected rgb=(%u,%u,%u)",
           label,
           dxmt9::fastsanity::channelR(color),
           dxmt9::fastsanity::channelG(color),
           dxmt9::fastsanity::channelB(color),
           r,
           g,
           b);
    }
    stats.expect(matches, label);
  }

  void runBlitCopy(TestStats& stats) {
    resetFixedFunctionState();
    ComPtr<IDirect3DSurface9> src;
    ComPtr<IDirect3DSurface9> dst;
    ComPtr<IDirect3DSurface9> staging;

    if (createRt(32, 32, src, stats) && createRt(32, 32, dst, stats)) {
      stats.expectHr("ColorFill(src)", device_->ColorFill(src.ptr(), nullptr, D3DCOLOR_XRGB(30, 60, 210)));
      stats.expectHr("StretchRect(src->dst)", device_->StretchRect(src.ptr(), nullptr, dst.ptr(), nullptr, D3DTEXF_POINT));
      if (copyRtToSysmem(dst.ptr(), staging, stats)) {
        expectSurfacePixel(staging.ptr(), 16, 16, 30, 60, 210, "blit-copy StretchRect pixel", stats);
      }
    }

    ComPtr<IDirect3DSurface9> sysmem;
    ComPtr<IDirect3DSurface9> updated;
    ComPtr<IDirect3DSurface9> updateReadback;
    if (createSysmemSurface(32, 32, sysmem, stats) && createRt(32, 32, updated, stats)) {
      fillLockedSurface(sysmem.ptr(), D3DCOLOR_XRGB(200, 20, 160), stats);
      stats.expectHr("UpdateSurface(sysmem->rt)",
                     device_->UpdateSurface(sysmem.ptr(), nullptr, updated.ptr(), nullptr));
      if (copyRtToSysmem(updated.ptr(), updateReadback, stats)) {
        expectSurfacePixel(updateReadback.ptr(), 8, 8, 200, 20, 160, "blit-copy UpdateSurface pixel", stats);
      }
    }

    clearBackbuffer(D3DCOLOR_XRGB(10, 10, 10), stats);
    finishScene(stats);
  }

  bool matrixNear(const D3DMATRIX& a, const D3DMATRIX& b) {
    const float* af = reinterpret_cast<const float*>(&a);
    const float* bf = reinterpret_cast<const float*>(&b);
    for (int i = 0; i < 16; ++i) {
      if (std::fabs(af[i] - bf[i]) > 0.0001f) {
        return false;
      }
    }
    return true;
  }

  D3DMATRIX identityMatrix() {
    D3DMATRIX m{};
    m._11 = 1.0f;
    m._22 = 1.0f;
    m._33 = 1.0f;
    m._44 = 1.0f;
    return m;
  }

  void runStateBlock(TestStats& stats) {
    resetFixedFunctionState();
    stats.expectHr("SetRenderState(CULLMODE NONE)", device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    ComPtr<IDirect3DStateBlock9> allState;
    stats.expectHr("CreateStateBlock(ALL)", device_->CreateStateBlock(D3DSBT_ALL, allState.put()));
    stats.expectHr("SetRenderState(CULLMODE CW)", device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW));
    if (allState) {
      stats.expectHr("StateBlock.Apply(ALL)", allState->Apply());
    }
    DWORD cull = 0;
    stats.expectHr("GetRenderState(CULLMODE)", device_->GetRenderState(D3DRS_CULLMODE, &cull));
    stats.expect(cull == D3DCULL_NONE, "stateblock restored cull mode");

    D3DMATRIX expected = identityMatrix();
    expected._41 = 2.0f;
    expected._42 = 3.0f;
    ComPtr<IDirect3DStateBlock9> recorded;
    stats.expectHr("BeginStateBlock", device_->BeginStateBlock());
    stats.expectHr("SetTextureStageState(COLOROP DISABLE)",
                   device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE));
    stats.expectHr("SetTransform(WORLD)", device_->SetTransform(D3DTS_WORLD, &expected));
    stats.expectHr("EndStateBlock", device_->EndStateBlock(recorded.put()));

    D3DMATRIX poison = identityMatrix();
    stats.expectHr("SetTextureStageState(COLOROP MODULATE)",
                   device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE));
    stats.expectHr("SetTransform(WORLD poison)", device_->SetTransform(D3DTS_WORLD, &poison));
    if (recorded) {
      stats.expectHr("StateBlock.Apply(recorded)", recorded->Apply());
    }
    DWORD colorOp = 0;
    D3DMATRIX actual{};
    stats.expectHr("GetTextureStageState(COLOROP)",
                   device_->GetTextureStageState(0, D3DTSS_COLOROP, &colorOp));
    stats.expectHr("GetTransform(WORLD)", device_->GetTransform(D3DTS_WORLD, &actual));
    stats.expect(colorOp == D3DTOP_DISABLE, "stateblock restored texture-stage color op");
    stats.expect(matrixNear(actual, expected), "stateblock restored transform matrix");

    clearBackbuffer(D3DCOLOR_XRGB(20, 60, 120), stats);
    finishScene(stats);
  }

  void runQuery(TestStats& stats) {
    HRESULT hr = device_->CreateQuery(D3DQUERYTYPE_EVENT, nullptr);
    stats.expectHr("CreateQuery(EVENT,null)", hr);
    ComPtr<IDirect3DQuery9> eventQuery;
    hr = device_->CreateQuery(D3DQUERYTYPE_EVENT, eventQuery.put());
    stats.expectHr("CreateQuery(EVENT)", hr);
    if (eventQuery) {
      stats.expectHr("EVENT Issue(END)", eventQuery->Issue(D3DISSUE_END));
      BOOL done = FALSE;
      HRESULT dataHr = S_FALSE;
      for (int i = 0; i < 1000; ++i) {
        dataHr = eventQuery->GetData(&done, sizeof(done), D3DGETDATA_FLUSH);
        if (dataHr == S_OK) {
          break;
        }
        Sleep(1);
      }
      stats.expect(dataHr == S_OK, "EVENT query completed");
    }

    hr = device_->CreateQuery(D3DQUERYTYPE_OCCLUSION, nullptr);
    stats.expect(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE,
                 "CreateQuery(OCCLUSION,null) returns supported or not-available");
    hr = device_->CreateQuery(D3DQUERYTYPE_TIMESTAMP, nullptr);
    stats.expect(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE,
                 "CreateQuery(TIMESTAMP,null) returns supported or not-available");

    clearBackbuffer(D3DCOLOR_XRGB(60, 60, 120), stats);
    finishScene(stats);
  }

  void drawBlendedQuad(TestStats& stats) {
    const BlendVertex vertices[4] = {
        {-0.75f, -0.25f, 0.5f, 0.0f, D3DCOLOR_ARGB(255, 0, 220, 0)},
        {-0.25f, -0.25f, 0.5f, 0.0f, D3DCOLOR_ARGB(255, 0, 220, 0)},
        {-0.75f, 0.25f, 0.5f, 0.0f, D3DCOLOR_ARGB(255, 0, 220, 0)},
        {-0.25f, 0.25f, 0.5f, 0.0f, D3DCOLOR_ARGB(255, 0, 220, 0)},
    };
    D3DMATRIX identity = identityMatrix();
    D3DMATRIX translated = identityMatrix();
    translated._41 = 1.0f;
    stats.expectHr("SetTransform(WORLD0)", device_->SetTransform(D3DTS_WORLD, &identity));
    stats.expectHr("SetTransform(WORLD1)",
                   device_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLD + 1), &translated));
    stats.expectHr("SetTransform(VIEW)", device_->SetTransform(D3DTS_VIEW, &identity));
    stats.expectHr("SetTransform(PROJECTION)", device_->SetTransform(D3DTS_PROJECTION, &identity));
    stats.expectHr("SetRenderState(VERTEXBLEND 1WEIGHTS)",
                   device_->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS));
    drawVertexBuffer(vertices, sizeof(vertices), sizeof(vertices[0]), D3DFVF_XYZB1 | D3DFVF_DIFFUSE,
                     "DrawPrimitive(vertex blend)", stats);
  }

  void runFfpVertexBlend(TestStats& stats) {
    resetFixedFunctionState();
    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    drawBlendedQuad(stats);
    finishScene(stats);

    readBackbufferNear(40, kHeight / 2, 0, 0, 0, "ffp-vertex-blend source location empty", stats);
    readBackbufferNear(120, kHeight / 2, 0, 220, 0, "ffp-vertex-blend translated location", stats);
  }

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  CaptureConfig capture_{};
  ComPtr<IDirect3D9Ex> d3d_;
  ComPtr<IDirect3DDevice9Ex> device_;
  D3DPRESENT_PARAMETERS params_{};
};

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR cmdLine, int) {
  const char* mode = "basic-ffp";
  if (cmdLine && cmdLine[0] != '\0') {
    mode = cmdLine;
  }

  IntentProbe probe(instance);
  if (!probe.init()) {
    return 1;
  }
  return probe.run(mode) ? 0 : 1;
}
