#include "../common/dx9_fast_sanity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

struct XyzDiffuseVertex {
  float x;
  float y;
  float z;
  DWORD color;
};

struct XyzNormalDiffuseVertex {
  float x;
  float y;
  float z;
  float nx;
  float ny;
  float nz;
  DWORD color;
};

struct XyzrhwDiffuseTexVertex {
  float x;
  float y;
  float z;
  float rhw;
  DWORD color;
  float u;
  float v;
};

struct RhwTexVertex {
  float x;
  float y;
  float z;
  float rhw;
  float u;
  float v;
};

struct RhwTex3Vertex {
  float x;
  float y;
  float z;
  float rhw;
  float u;
  float v;
  float w;
};

struct XyzTexVertex {
  float x;
  float y;
  float z;
  float u;
  float v;
};

struct XyzNormalTexVertex {
  float x;
  float y;
  float z;
  float nx;
  float ny;
  float nz;
};

struct Blend3Vertex {
  float x;
  float y;
  float z;
  float w0;
  float w1;
  float w2;
  DWORD color;
};

struct IndexedBlendVertex {
  float x;
  float y;
  float z;
  float weight;
  DWORD indices;
  DWORD color;
};

struct XyzOnlyVertex {
  float x;
  float y;
  float z;
};

struct XyzUbyte4nColorVertex {
  float x;
  float y;
  float z;
  unsigned char r;
  unsigned char g;
  unsigned char b;
  unsigned char a;
};

struct RhwPSizeVertex {
  float x;
  float y;
  float z;
  float rhw;
  float psize;
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
    } else if (std::strcmp(mode, "ffp-vertex-blend-extended") == 0) {
      runFfpVertexBlendExtended(stats);
    } else if (std::strcmp(mode, "texture-transform") == 0) {
      runTextureTransform(stats);
    } else if (std::strcmp(mode, "generated-texcoords") == 0) {
      runGeneratedTexcoords(stats);
    } else if (std::strcmp(mode, "color-material") == 0) {
      runColorMaterial(stats);
    } else if (std::strcmp(mode, "sysmem-draw-processvertices") == 0) {
      runSysmemDrawProcessVertices(stats);
    } else if (std::strcmp(mode, "dynamic-map-sync") == 0) {
      runDynamicMapSync(stats);
    } else if (std::strcmp(mode, "attached-rt-sampling") == 0) {
      runAttachedRtSampling(stats);
    } else if (std::strcmp(mode, "blit-format-conversion") == 0) {
      runBlitFormatConversion(stats);
    } else if (std::strcmp(mode, "reset-resource-lifecycle") == 0) {
      runResetResourceLifecycle(stats);
    } else if (std::strcmp(mode, "depth-stencil-viewport-scissor") == 0) {
      runDepthStencilViewportScissor(stats);
    } else if (std::strcmp(mode, "mipmap-update-texture") == 0) {
      runMipmapUpdateTexture(stats);
    } else if (std::strcmp(mode, "multisample-resolve") == 0) {
      runMultisampleResolve(stats);
    } else if (std::strcmp(mode, "fog-depthbias") == 0) {
      runFogDepthBias(stats);
    } else if (std::strcmp(mode, "draw-indexed-up-edges") == 0) {
      runDrawIndexedUpEdges(stats);
    } else if (std::strcmp(mode, "shader-edge-visual") == 0) {
      runShaderEdgeVisual(stats);
    } else if (std::strcmp(mode, "d3d9ex-wsi") == 0) {
      runD3d9ExWsi(stats);
    } else if (std::strcmp(mode, "cube-volume-texture-update") == 0) {
      runCubeVolumeTextureUpdate(stats);
    } else if (std::strcmp(mode, "autogen-mipmap") == 0) {
      runAutogenMipmap(stats);
    } else if (std::strcmp(mode, "npot-filter-lod") == 0) {
      runNpotFilterLod(stats);
    } else if (std::strcmp(mode, "managed-reset-texture") == 0) {
      runManagedResetTexture(stats);
    } else if (std::strcmp(mode, "sample-mask") == 0) {
      runSampleMask(stats);
    } else if (std::strcmp(mode, "alpha-to-coverage") == 0) {
      runAlphaToCoverage(stats);
    } else if (std::strcmp(mode, "cube-wrap") == 0) {
      runCubeWrap(stats);
    } else if (std::strcmp(mode, "line-aa-blending") == 0) {
      runLineAaBlending(stats);
    } else if (std::strcmp(mode, "default-attribute-components") == 0) {
      runDefaultAttributeComponents(stats);
    } else if (std::strcmp(mode, "vshader-input-types") == 0) {
      runVshaderInputTypes(stats);
    } else if (std::strcmp(mode, "pointsize") == 0) {
      runPointSize(stats);
    } else if (std::strcmp(mode, "depth-stencil-init") == 0) {
      runDepthStencilInit(stats);
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
    device_->SetRenderState(D3DRS_ZENABLE, FALSE);
    device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device_->SetRenderState(D3DRS_COLORWRITEENABLE,
                            D3DCOLORWRITEENABLE_RED |
                                D3DCOLORWRITEENABLE_GREEN |
                                D3DCOLORWRITEENABLE_BLUE |
                                D3DCOLORWRITEENABLE_ALPHA);
    device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device_->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    device_->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device_->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device_->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    D3DMATRIX identity = identityMatrix();
    device_->SetTransform(D3DTS_WORLD, &identity);
    device_->SetTransform(D3DTS_VIEW, &identity);
    device_->SetTransform(D3DTS_PROJECTION, &identity);
    device_->SetTransform(D3DTS_TEXTURE0, &identity);
    device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
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

  void drawDeclaredVertexBuffer(const void* vertices,
                                UINT byteSize,
                                UINT stride,
                                const D3DVERTEXELEMENT9* elements,
                                const char* label,
                                TestStats& stats) {
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    HRESULT hr = device_->CreateVertexDeclaration(elements, declaration.put());
    stats.expectHr("CreateVertexDeclaration(draw)", hr);
    if (FAILED(hr)) {
      return;
    }

    ComPtr<IDirect3DVertexBuffer9> vb;
    hr = device_->CreateVertexBuffer(byteSize, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, vb.put(), nullptr);
    stats.expectHr("CreateVertexBuffer(declared draw)", hr);
    if (FAILED(hr)) {
      return;
    }
    void* mapped = nullptr;
    hr = vb->Lock(0, byteSize, &mapped, 0);
    stats.expectHr("VertexBuffer.Lock(declared draw)", hr);
    if (FAILED(hr)) {
      return;
    }
    std::memcpy(mapped, vertices, byteSize);
    stats.expectHr("VertexBuffer.Unlock(declared draw)", vb->Unlock());
    stats.expectHr("SetVertexDeclaration(draw)", device_->SetVertexDeclaration(declaration.ptr()));
    stats.expectHr("SetStreamSource(declared draw)", device_->SetStreamSource(0, vb.ptr(), 0, stride));
    stats.expectHr(label, device_->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
  }

  void drawXyzDiffuseQuad(float left,
                          float top,
                          float right,
                          float bottom,
                          DWORD color,
                          TestStats& stats) {
    const XyzDiffuseVertex vertices[4] = {
        {left, bottom, 0.5f, color},
        {left, top, 0.5f, color},
        {right, bottom, 0.5f, color},
        {right, top, 0.5f, color},
    };
    drawVertexBuffer(vertices, sizeof(vertices), sizeof(vertices[0]), D3DFVF_XYZ | D3DFVF_DIFFUSE,
                     "DrawPrimitive(XYZ|DIFFUSE)", stats);
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
    return createRtFmt(width, height, D3DFMT_A8R8G8B8, surface, stats, "CreateRenderTarget");
  }

  bool createRtFmt(UINT width,
                   UINT height,
                   D3DFORMAT format,
                   ComPtr<IDirect3DSurface9>& surface,
                   TestStats& stats,
                   const char* label) {
    const HRESULT hr = device_->CreateRenderTarget(width,
                                                   height,
                                                   format,
                                                   D3DMULTISAMPLE_NONE,
                                                   0,
                                                   FALSE,
                                                   surface.put(),
                                                   nullptr);
    stats.expectHr(label, hr);
    return SUCCEEDED(hr);
  }

  bool createDepthStencil(UINT width, UINT height, ComPtr<IDirect3DSurface9>& surface, TestStats& stats) {
    const HRESULT hr = device_->CreateDepthStencilSurface(width,
                                                          height,
                                                          D3DFMT_D24S8,
                                                          D3DMULTISAMPLE_NONE,
                                                          0,
                                                          TRUE,
                                                          surface.put(),
                                                          nullptr);
    stats.expectHr("CreateDepthStencilSurface", hr);
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
    D3DSURFACE_DESC desc{};
    rt->GetDesc(&desc);
    const HRESULT createHr = device_->CreateOffscreenPlainSurface(desc.Width,
                                                                  desc.Height,
                                                                  desc.Format,
                                                                  D3DPOOL_SYSTEMMEM,
                                                                  staging.put(),
                                                                  nullptr);
    stats.expectHr("CreateOffscreenPlainSurface(rt readback)", createHr);
    if (FAILED(createHr)) {
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

  void expectSurfaceU16(IDirect3DSurface9* surface, UINT x, UINT y, std::uint16_t expected,
                        const char* label, TestStats& stats) {
    D3DLOCKED_RECT locked{};
    HRESULT hr = surface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    stats.expectHr("LockRect(read u16)", hr);
    if (FAILED(hr)) {
      return;
    }
    const auto* row = reinterpret_cast<const std::uint16_t*>(
        static_cast<const unsigned char*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch);
    const std::uint16_t actual = row[x];
    surface->UnlockRect();
    if (actual != expected) {
      logf("DETAIL: %s actual=0x%04x expected=0x%04x", label, actual, expected);
    }
    stats.expect(actual == expected, label);
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

  D3DMATRIX translationMatrix(float x, float y, float z = 0.0f) {
    D3DMATRIX m = identityMatrix();
    m._41 = x;
    m._42 = y;
    m._43 = z;
    return m;
  }

  bool createTexture(UINT width,
                     UINT height,
                     D3DFORMAT format,
                     DWORD usage,
                     D3DPOOL pool,
                     ComPtr<IDirect3DTexture9>& texture,
                     TestStats& stats,
                     const char* label) {
    const HRESULT hr = device_->CreateTexture(width, height, 1, usage, format, pool, texture.put(), nullptr);
    stats.expectHr(label, hr);
    return SUCCEEDED(hr);
  }

  void fillTextureQuadrants(IDirect3DTexture9* texture,
                            D3DCOLOR topLeft,
                            D3DCOLOR topRight,
                            D3DCOLOR bottomLeft,
                            D3DCOLOR bottomRight,
                            TestStats& stats) {
    D3DLOCKED_RECT locked{};
    HRESULT hr = texture->LockRect(0, &locked, nullptr, 0);
    stats.expectHr("Texture.LockRect(fill quadrants)", hr);
    if (FAILED(hr)) {
      return;
    }
    D3DSURFACE_DESC desc{};
    texture->GetLevelDesc(0, &desc);
    for (UINT y = 0; y < desc.Height; ++y) {
      auto* row = reinterpret_cast<D3DCOLOR*>(
          static_cast<unsigned char*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch);
      for (UINT x = 0; x < desc.Width; ++x) {
        const bool right = x >= desc.Width / 2;
        const bool bottom = y >= desc.Height / 2;
        row[x] = bottom ? (right ? bottomRight : bottomLeft) : (right ? topRight : topLeft);
      }
    }
    stats.expectHr("Texture.UnlockRect(fill quadrants)", texture->UnlockRect(0));
  }

  void drawTexturedRhwQuad(float u, float v, TestStats& stats) {
    const RhwTexVertex vertices[4] = {
        {0.0f, 0.0f, 0.5f, 1.0f, u, v},
        {static_cast<float>(kWidth), 0.0f, 0.5f, 1.0f, u, v},
        {0.0f, static_cast<float>(kHeight), 0.5f, 1.0f, u, v},
        {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.5f, 1.0f, u, v},
    };
    drawVertexBuffer(vertices, sizeof(vertices), sizeof(vertices[0]), D3DFVF_XYZRHW | D3DFVF_TEX1,
                     "DrawPrimitive(textured XYZRHW)", stats);
  }

  void drawTexturedRhwQuad3(float u, float v, float w, const char* label, TestStats& stats) {
    const RhwTex3Vertex vertices[4] = {
        {0.0f, 0.0f, 0.5f, 1.0f, u, v, w},
        {static_cast<float>(kWidth), 0.0f, 0.5f, 1.0f, u, v, w},
        {0.0f, static_cast<float>(kHeight), 0.5f, 1.0f, u, v, w},
        {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.5f, 1.0f, u, v, w},
    };
    drawVertexBuffer(vertices, sizeof(vertices), sizeof(vertices[0]),
                     D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0),
                     label, stats);
  }

  void drawTexturedXyzQuad(TestStats& stats) {
    const XyzTexVertex vertices[4] = {
        {-1.0f, -1.0f, 0.5f, 0.0f, 1.0f},
        {1.0f, -1.0f, 0.5f, 1.0f, 1.0f},
        {-1.0f, 1.0f, 0.5f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.5f, 1.0f, 0.0f},
    };
    drawVertexBuffer(vertices, sizeof(vertices), sizeof(vertices[0]), D3DFVF_XYZ | D3DFVF_TEX1,
                     "DrawPrimitive(textured XYZ)", stats);
  }

  void drawTexturedRhwQuadColors(DWORD color, TestStats& stats) {
    const XyzrhwDiffuseTexVertex vertices[4] = {
        {0.0f, 0.0f, 0.5f, 1.0f, color, 0.0f, 0.0f},
        {static_cast<float>(kWidth), 0.0f, 0.5f, 1.0f, color, 1.0f, 0.0f},
        {0.0f, static_cast<float>(kHeight), 0.5f, 1.0f, color, 0.0f, 1.0f},
        {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.5f, 1.0f, color, 1.0f, 1.0f},
    };
    drawVertexBuffer(vertices, sizeof(vertices), sizeof(vertices[0]),
                     D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
                     "DrawPrimitive(textured diffuse XYZRHW)", stats);
  }

  void drawRhwQuadZ(float left,
                    float top,
                    float right,
                    float bottom,
                    float z,
                    DWORD color,
                    const char* label,
                    TestStats& stats) {
    const RhwVertex vertices[4] = {
        {left, top, z, 1.0f, color},
        {right, top, z, 1.0f, color},
        {left, bottom, z, 1.0f, color},
        {right, bottom, z, 1.0f, color},
    };
    drawVertexBuffer(vertices, sizeof(vertices), sizeof(vertices[0]), D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                     label, stats);
  }

  DWORD floatAsDword(float value) {
    DWORD bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
  }

  bool createTextureLevels(UINT width,
                           UINT height,
                           UINT levels,
                           D3DFORMAT format,
                           DWORD usage,
                           D3DPOOL pool,
                           ComPtr<IDirect3DTexture9>& texture,
                           TestStats& stats,
                           const char* label) {
    const HRESULT hr = device_->CreateTexture(width, height, levels, usage, format, pool, texture.put(), nullptr);
    stats.expectHr(label, hr);
    return SUCCEEDED(hr);
  }

  void fillTextureLevel(IDirect3DTexture9* texture, UINT level, D3DCOLOR color, TestStats& stats) {
    D3DLOCKED_RECT locked{};
    HRESULT hr = texture->LockRect(level, &locked, nullptr, 0);
    stats.expectHr("Texture.LockRect(fill level)", hr);
    if (FAILED(hr)) {
      return;
    }
    D3DSURFACE_DESC desc{};
    texture->GetLevelDesc(level, &desc);
    for (UINT y = 0; y < desc.Height; ++y) {
      auto* row = reinterpret_cast<D3DCOLOR*>(
          static_cast<unsigned char*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch);
      for (UINT x = 0; x < desc.Width; ++x) {
        row[x] = color;
      }
    }
    stats.expectHr("Texture.UnlockRect(fill level)", texture->UnlockRect(level));
  }

  void fillCubeFace(IDirect3DCubeTexture9* texture,
                    D3DCUBEMAP_FACES face,
                    UINT level,
                    D3DCOLOR color,
                    TestStats& stats) {
    D3DLOCKED_RECT locked{};
    HRESULT hr = texture->LockRect(face, level, &locked, nullptr, 0);
    stats.expectHr("CubeTexture.LockRect(fill face)", hr);
    if (FAILED(hr)) {
      return;
    }
    D3DSURFACE_DESC desc{};
    texture->GetLevelDesc(level, &desc);
    for (UINT y = 0; y < desc.Height; ++y) {
      auto* row = reinterpret_cast<D3DCOLOR*>(
          static_cast<unsigned char*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch);
      for (UINT x = 0; x < desc.Width; ++x) {
        row[x] = color;
      }
    }
    stats.expectHr("CubeTexture.UnlockRect(fill face)", texture->UnlockRect(face, level));
  }

  void fillVolumeLevel(IDirect3DVolumeTexture9* texture, UINT level, D3DCOLOR color, TestStats& stats) {
    D3DLOCKED_BOX locked{};
    HRESULT hr = texture->LockBox(level, &locked, nullptr, 0);
    stats.expectHr("VolumeTexture.LockBox(fill level)", hr);
    if (FAILED(hr)) {
      return;
    }
    D3DVOLUME_DESC desc{};
    texture->GetLevelDesc(level, &desc);
    for (UINT z = 0; z < desc.Depth; ++z) {
      auto* slice = static_cast<unsigned char*>(locked.pBits) + static_cast<size_t>(z) * locked.SlicePitch;
      for (UINT y = 0; y < desc.Height; ++y) {
        auto* row = reinterpret_cast<D3DCOLOR*>(slice + static_cast<size_t>(y) * locked.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x) {
          row[x] = color;
        }
      }
    }
    stats.expectHr("VolumeTexture.UnlockBox(fill level)", texture->UnlockBox(level));
  }

  bool waitEventQuery(IDirect3DQuery9* query, const char* label, TestStats& stats) {
    if (!query) {
      stats.expect(false, label);
      return false;
    }
    BOOL done = FALSE;
    HRESULT hr = S_FALSE;
    for (int spin = 0; spin < 1000; ++spin) {
      hr = query->GetData(&done, sizeof(done), D3DGETDATA_FLUSH);
      if (hr == S_OK) {
        break;
      }
      Sleep(1);
    }
    stats.expect(hr == S_OK, label);
    return hr == S_OK;
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

  void runFfpVertexBlendExtended(TestStats& stats) {
    resetFixedFunctionState();
    D3DMATRIX left = translationMatrix(-0.8f, 0.0f);
    D3DMATRIX lower = translationMatrix(0.0f, -0.8f);
    D3DMATRIX unused = translationMatrix(0.0f, 0.8f);
    D3DMATRIX right = translationMatrix(0.8f, 0.0f);

    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    stats.expectHr("SetTransform(WORLD0 left)", device_->SetTransform(D3DTS_WORLD, &left));
    stats.expectHr("SetTransform(WORLD1 lower)",
                   device_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLD + 1), &lower));
    stats.expectHr("SetTransform(WORLD2 unused)",
                   device_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLD + 2), &unused));
    stats.expectHr("SetTransform(WORLD3 right)",
                   device_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLD + 3), &right));
    stats.expectHr("SetRenderState(VERTEXBLEND 3WEIGHTS)",
                   device_->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS));

    const Blend3Vertex fvfQuad[4] = {
        {-0.2f, -0.25f, 0.5f, 0.0f, 0.0f, 0.0f, D3DCOLOR_ARGB(255, 240, 240, 0)},
        {0.2f, -0.25f, 0.5f, 0.0f, 0.0f, 0.0f, D3DCOLOR_ARGB(255, 240, 240, 0)},
        {-0.2f, 0.25f, 0.5f, 0.0f, 0.0f, 0.0f, D3DCOLOR_ARGB(255, 240, 240, 0)},
        {0.2f, 0.25f, 0.5f, 0.0f, 0.0f, 0.0f, D3DCOLOR_ARGB(255, 240, 240, 0)},
    };
    drawVertexBuffer(fvfQuad, sizeof(fvfQuad), sizeof(fvfQuad[0]),
                     D3DFVF_XYZB3 | D3DFVF_DIFFUSE, "DrawPrimitive(FVF XYZB3)", stats);
    finishScene(stats);
    readBackbufferNear(20, kHeight / 2, 0, 0, 0, "ffp-vertex-blend-extended FVF source empty", stats);
    readBackbufferNear(140, kHeight / 2, 240, 240, 0, "ffp-vertex-blend-extended FVF implicit matrix", stats);

    resetFixedFunctionState();
    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    stats.expectHr("SetTransform(WORLD0 left indexed)", device_->SetTransform(D3DTS_WORLD, &left));
    stats.expectHr("SetTransform(WORLD1 lower indexed)",
                   device_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLD + 1), &lower));
    stats.expectHr("SetTransform(WORLD2 unused indexed)",
                   device_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLD + 2), &unused));
    stats.expectHr("SetTransform(WORLD3 right indexed)",
                   device_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLD + 3), &right));
    stats.expectHr("SetRenderState(VERTEXBLEND indexed 1WEIGHTS)",
                   device_->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS));
    stats.expectHr("SetRenderState(INDEXEDVERTEXBLENDENABLE true)",
                   device_->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE));
    static const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        {0, 16, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        {0, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()};
    const DWORD indices = 0x00000300u;
    const IndexedBlendVertex indexedQuad[4] = {
        {-0.2f, -0.25f, 0.5f, 0.0f, indices, D3DCOLOR_ARGB(255, 0, 220, 220)},
        {0.2f, -0.25f, 0.5f, 0.0f, indices, D3DCOLOR_ARGB(255, 0, 220, 220)},
        {-0.2f, 0.25f, 0.5f, 0.0f, indices, D3DCOLOR_ARGB(255, 0, 220, 220)},
        {0.2f, 0.25f, 0.5f, 0.0f, indices, D3DCOLOR_ARGB(255, 0, 220, 220)},
    };
    drawDeclaredVertexBuffer(indexedQuad, sizeof(indexedQuad), sizeof(indexedQuad[0]), elements,
                             "DrawPrimitive(indexed vertex blend)", stats);
    finishScene(stats);
    readBackbufferNear(20, kHeight / 2, 0, 0, 0, "ffp-vertex-blend-extended indexed source empty", stats);
    readBackbufferNear(140, kHeight / 2, 0, 220, 220, "ffp-vertex-blend-extended indexed matrix", stats);
  }

  void runTextureTransform(TestStats& stats) {
    resetFixedFunctionState();
    ComPtr<IDirect3DTexture9> texture;
    if (!createTexture(2, 2, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED, texture, stats,
                       "CreateTexture(texture-transform)")) {
      return;
    }
    fillTextureQuadrants(texture.ptr(),
                         D3DCOLOR_XRGB(220, 0, 0),
                         D3DCOLOR_XRGB(0, 220, 0),
                         D3DCOLOR_XRGB(0, 0, 220),
                         D3DCOLOR_XRGB(220, 220, 0),
                         stats);

    D3DMATRIX flip = identityMatrix();
    flip._11 = -1.0f;
    flip._14 = 1.0f;
    flip._22 = -1.0f;
    flip._24 = 1.0f;
    stats.expectHr("SetTexture(0)", device_->SetTexture(0, texture.ptr()));
    stats.expectHr("SetTextureStageState(COLORARG1 TEXTURE)",
                   device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    stats.expectHr("SetTextureStageState(TEXTURETRANSFORMFLAGS COUNT2)",
                   device_->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2));
    stats.expectHr("SetTransform(TEXTURE0 flip)", device_->SetTransform(D3DTS_TEXTURE0, &flip));

    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    drawTexturedXyzQuad(stats);
    finishScene(stats);
    readBackbufferNear(40, 30, 220, 220, 0, "texture-transform top-left flipped to bottom-right", stats);
    readBackbufferNear(120, 30, 0, 0, 220, "texture-transform top-right flipped to bottom-left", stats);
    readBackbufferNear(40, 90, 0, 220, 0, "texture-transform bottom-left flipped to top-right", stats);
    readBackbufferNear(120, 90, 220, 0, 0, "texture-transform bottom-right flipped to top-left", stats);
  }

  void runGeneratedTexcoords(TestStats& stats) {
    resetFixedFunctionState();
    ComPtr<IDirect3DTexture9> texture;
    if (!createTexture(4, 4, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED, texture, stats,
                       "CreateTexture(generated-texcoords)")) {
      return;
    }
    fillTextureQuadrants(texture.ptr(),
                         D3DCOLOR_XRGB(220, 0, 0),
                         D3DCOLOR_XRGB(0, 220, 0),
                         D3DCOLOR_XRGB(0, 0, 220),
                         D3DCOLOR_XRGB(220, 220, 0),
                         stats);

    const XyzNormalTexVertex tri[4] = {
        {-1.0f, -1.0f, 0.5f, 1.0f, 0.0f, 0.0f},
        {1.0f, -1.0f, 0.5f, 1.0f, 0.0f, 0.0f},
        {-1.0f, 1.0f, 0.5f, 1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.5f, 1.0f, 0.0f, 0.0f},
    };
    stats.expectHr("SetTexture(generated-texcoords)", device_->SetTexture(0, texture.ptr()));
    stats.expectHr("SetTextureStageState(COLORARG1 TEXTURE generated)",
                   device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    stats.expectHr("SetTextureStageState(TEXCOORDINDEX CAMERASPACENORMAL)",
                   device_->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL));
    stats.expectHr("SetTextureStageState(TEXTURETRANSFORMFLAGS COUNT2 generated)",
                   device_->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2));

    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    drawVertexBuffer(tri, sizeof(tri), sizeof(tri[0]), D3DFVF_XYZ | D3DFVF_NORMAL,
                     "DrawPrimitive(generated texcoords)", stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                       "generated-texcoords camera-space normal", stats);
  }

  void runColorMaterial(TestStats& stats) {
    resetFixedFunctionState();
    D3DMATERIAL9 material{};
    material.Ambient.r = 0.0f;
    material.Ambient.g = 0.0f;
    material.Ambient.b = 1.0f;
    material.Ambient.a = 1.0f;
    stats.expectHr("SetMaterial(color-material)", device_->SetMaterial(&material));
    stats.expectHr("SetRenderState(LIGHTING true)", device_->SetRenderState(D3DRS_LIGHTING, TRUE));
    stats.expectHr("SetRenderState(AMBIENT white)", device_->SetRenderState(D3DRS_AMBIENT, 0xffffffff));
    stats.expectHr("SetTextureStageState(COLORARG1 DIFFUSE color-material)",
                   device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));

    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    stats.expectHr("SetRenderState(COLORVERTEX true)", device_->SetRenderState(D3DRS_COLORVERTEX, TRUE));
    stats.expectHr("SetRenderState(AMBIENTMATERIALSOURCE COLOR1)",
                   device_->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1));
    drawXyzDiffuseQuad(-0.9f, -0.8f, -0.1f, 0.8f, D3DCOLOR_ARGB(255, 220, 0, 0), stats);

    stats.expectHr("SetRenderState(COLORVERTEX false)", device_->SetRenderState(D3DRS_COLORVERTEX, FALSE));
    stats.expectHr("SetRenderState(AMBIENTMATERIALSOURCE MATERIAL)",
                   device_->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL));
    drawXyzDiffuseQuad(0.1f, -0.8f, 0.9f, 0.8f, D3DCOLOR_ARGB(255, 220, 0, 0), stats);
    finishScene(stats);

    readBackbufferNear(40, kHeight / 2, 220, 0, 0, "color-material diffuse ambient source", stats);
    readBackbufferNear(120, kHeight / 2, 0, 0, 255, "color-material material ambient source", stats);
  }

  void runSysmemDrawProcessVertices(TestStats& stats) {
    resetFixedFunctionState();
    const XyzDiffuseVertex quad[4] = {
        {-0.6f, -0.6f, 0.5f, D3DCOLOR_ARGB(255, 0, 220, 0)},
        {-0.6f, 0.6f, 0.5f, D3DCOLOR_ARGB(255, 0, 220, 0)},
        {0.6f, -0.6f, 0.5f, D3DCOLOR_ARGB(255, 0, 220, 0)},
        {0.6f, 0.6f, 0.5f, D3DCOLOR_ARGB(255, 0, 220, 0)},
    };
    ComPtr<IDirect3DVertexBuffer9> vb;
    HRESULT hr = device_->CreateVertexBuffer(sizeof(quad), 0, D3DFVF_XYZ | D3DFVF_DIFFUSE,
                                             D3DPOOL_SYSTEMMEM, vb.put(), nullptr);
    stats.expectHr("CreateVertexBuffer(SYSTEMMEM)", hr);
    if (FAILED(hr)) {
      return;
    }
    void* mapped = nullptr;
    hr = vb->Lock(0, sizeof(quad), &mapped, 0);
    stats.expectHr("VertexBuffer.Lock(SYSTEMMEM)", hr);
    if (SUCCEEDED(hr)) {
      std::memcpy(mapped, quad, sizeof(quad));
      stats.expectHr("VertexBuffer.Unlock(SYSTEMMEM)", vb->Unlock());
    }

    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    stats.expectHr("SetFVF(SYSTEMMEM draw)", device_->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
    stats.expectHr("SetStreamSource(SYSTEMMEM)", device_->SetStreamSource(0, vb.ptr(), 0, sizeof(quad[0])));
    stats.expectHr("DrawPrimitive(SYSTEMMEM)", device_->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0, "sysmem-draw center", stats);

    ComPtr<IDirect3DVertexBuffer9> processed;
    hr = device_->CreateVertexBuffer(4 * sizeof(RhwVertex), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                                     D3DPOOL_SYSTEMMEM, processed.put(), nullptr);
    stats.expectHr("CreateVertexBuffer(ProcessVertices dst)", hr);
    if (SUCCEEDED(hr)) {
      stats.expectHr("ProcessVertices(SYSTEMMEM->SYSTEMMEM)",
                     device_->ProcessVertices(0, 0, 4, processed.ptr(), nullptr, 0));
      RhwVertex* out = nullptr;
      hr = processed->Lock(0, 4 * sizeof(RhwVertex), reinterpret_cast<void**>(&out), D3DLOCK_READONLY);
      stats.expectHr("VertexBuffer.Lock(ProcessVertices dst)", hr);
      if (SUCCEEDED(hr)) {
        const bool sane = std::isfinite(out[0].x) && std::isfinite(out[0].y) && out[0].rhw != 0.0f;
        stats.expect(sane, "sysmem-draw ProcessVertices produced transformed vertex");
        stats.expectHr("VertexBuffer.Unlock(ProcessVertices dst)", processed->Unlock());
      }
    }
  }

  void fillDynamicQuad(RhwVertex* quad, float left, float top, float right, float bottom, DWORD color) {
    quad[0] = {left, top, 0.5f, 1.0f, color};
    quad[1] = {right, top, 0.5f, 1.0f, color};
    quad[2] = {left, bottom, 0.5f, 1.0f, color};
    quad[3] = {right, bottom, 0.5f, 1.0f, color};
  }

  void runDynamicMapSync(TestStats& stats) {
    resetFixedFunctionState();
    constexpr UINT kGrid = 4;
    constexpr UINT kVertsPerQuad = 4;
    constexpr UINT kVertexCount = kGrid * kGrid * kVertsPerQuad;
    ComPtr<IDirect3DVertexBuffer9> vb;
    HRESULT hr = device_->CreateVertexBuffer(kVertexCount * sizeof(RhwVertex),
                                             D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                             D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                                             D3DPOOL_DEFAULT,
                                             vb.put(),
                                             nullptr);
    stats.expectHr("CreateVertexBuffer(dynamic)", hr);
    if (FAILED(hr)) {
      return;
    }
    clearBackbuffer(D3DCOLOR_XRGB(220, 0, 0), stats);
    stats.expectHr("SetFVF(dynamic)", device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    stats.expectHr("SetStreamSource(dynamic)", device_->SetStreamSource(0, vb.ptr(), 0, sizeof(RhwVertex)));

    for (UINT y = 0; y < kGrid; ++y) {
      for (UINT x = 0; x < kGrid; ++x) {
        const UINT quadIndex = y * kGrid + x;
        void* mapped = nullptr;
        const UINT offset = quadIndex * kVertsPerQuad * sizeof(RhwVertex);
        const DWORD lockFlags = quadIndex == 0 ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE;
        hr = vb->Lock(offset, kVertsPerQuad * sizeof(RhwVertex), &mapped, lockFlags);
        stats.expectHr(quadIndex == 0 ? "VertexBuffer.Lock(DISCARD)" : "VertexBuffer.Lock(NOOVERWRITE)", hr);
        if (FAILED(hr)) {
          return;
        }
        const float left = (static_cast<float>(x) / kGrid) * kWidth;
        const float top = (static_cast<float>(y) / kGrid) * kHeight;
        const float right = (static_cast<float>(x + 1) / kGrid) * kWidth;
        const float bottom = (static_cast<float>(y + 1) / kGrid) * kHeight;
        fillDynamicQuad(static_cast<RhwVertex*>(mapped), left, top, right, bottom, D3DCOLOR_ARGB(255, 0, 220, 0));
        stats.expectHr("VertexBuffer.Unlock(dynamic)", vb->Unlock());
        stats.expectHr("DrawPrimitive(dynamic)", device_->DrawPrimitive(D3DPT_TRIANGLESTRIP, quadIndex * kVertsPerQuad, 2));
      }
    }
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0, "dynamic-map-sync discard/nooverwrite fill", stats);

    resetFixedFunctionState();
    clearBackbuffer(D3DCOLOR_XRGB(220, 0, 0), stats);
    stats.expectHr("SetFVF(dynamic mapped)", device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    stats.expectHr("SetStreamSource(dynamic mapped)", device_->SetStreamSource(0, vb.ptr(), 0, sizeof(RhwVertex)));
    RhwVertex* mapped = nullptr;
    hr = vb->Lock(0, kVertexCount * sizeof(RhwVertex), reinterpret_cast<void**>(&mapped), D3DLOCK_DISCARD);
    stats.expectHr("VertexBuffer.Lock(mapped draw whole buffer)", hr);
    if (SUCCEEDED(hr)) {
      for (UINT y = 0; y < kGrid; ++y) {
        for (UINT x = 0; x < kGrid; ++x) {
          const UINT quadIndex = y * kGrid + x;
          const float left = (static_cast<float>(x) / kGrid) * kWidth;
          const float top = (static_cast<float>(y) / kGrid) * kHeight;
          const float right = (static_cast<float>(x + 1) / kGrid) * kWidth;
          const float bottom = (static_cast<float>(y + 1) / kGrid) * kHeight;
          fillDynamicQuad(mapped + quadIndex * kVertsPerQuad, left, top, right, bottom,
                          D3DCOLOR_ARGB(255, 0, 0, 220));
          stats.expectHr("DrawPrimitive(mapped dynamic)", device_->DrawPrimitive(D3DPT_TRIANGLESTRIP,
                                                                                 quadIndex * kVertsPerQuad,
                                                                                 2));
        }
      }
      stats.expectHr("VertexBuffer.Unlock(mapped draw whole buffer)", vb->Unlock());
    }
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 0, 220, "dynamic-map-sync mapped draw fill", stats);
  }

  void runAttachedRtSampling(TestStats& stats) {
    resetFixedFunctionState();
    DWORD caps = 0;
    HRESULT hr = device_->CreateQuery(D3DQUERYTYPE_EVENT, nullptr);
    if (hr == D3DERR_NOTAVAILABLE) {
      logf("SKIP: attached-rt-sampling requires EVENT query support");
      return;
    }

    D3DCAPS9 deviceCaps{};
    stats.expectHr("GetDeviceCaps(attached RT)", device_->GetDeviceCaps(&deviceCaps));
    caps = deviceCaps.PixelShaderVersion;
    if (caps < D3DPS_VERSION(2, 0)) {
      logf("SKIP: attached-rt-sampling requires ps_2_0");
      return;
    }

    static const DWORD psCode[] = {
        0xffff0200,
        0x05000051, 0xa00f0000, 0x3e800000, 0x3e800000, 0x3e800000, 0x3e800000,
        0x0200001f, 0x80000000, 0xb00f0000,
        0x0200001f, 0x90000000, 0xa00f0800,
        0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800,
        0x03000002, 0x800f0000, 0x80e40000, 0xa0e40000,
        0x02000001, 0x800f0800, 0x80e40000,
        0x0000ffff};

    ComPtr<IDirect3DTexture9> texture;
    if (!createTexture(64, 64, D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, texture, stats,
                       "CreateTexture(attached RT)")) {
      return;
    }
    ComPtr<IDirect3DSurface9> rt;
    stats.expectHr("GetSurfaceLevel(attached RT)", texture->GetSurfaceLevel(0, rt.put()));
    ComPtr<IDirect3DPixelShader9> ps;
    stats.expectHr("CreatePixelShader(attached RT)", device_->CreatePixelShader(psCode, ps.put()));
    ComPtr<IDirect3DQuery9> eventQuery;
    stats.expectHr("CreateQuery(EVENT attached RT)", device_->CreateQuery(D3DQUERYTYPE_EVENT, eventQuery.put()));

    const RhwTexVertex quad[4] = {
        {0.0f, 0.0f, 0.5f, 1.0f, 0.25f, 0.25f},
        {64.0f, 0.0f, 0.5f, 1.0f, 0.25f, 0.25f},
        {0.0f, 64.0f, 0.5f, 1.0f, 0.25f, 0.25f},
        {64.0f, 64.0f, 0.5f, 1.0f, 0.25f, 0.25f},
    };
    stats.expectHr("SetRenderTarget(attached RT)", device_->SetRenderTarget(0, rt.ptr()));
    stats.expectHr("SetTexture(attached RT self)", device_->SetTexture(0, texture.ptr()));
    stats.expectHr("Clear(attached RT)", device_->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(1, 1, 1), 0.0f, 0));
    stats.expectHr("BeginScene(attached RT)", device_->BeginScene());
    stats.expectHr("SetPixelShader(attached RT)", device_->SetPixelShader(ps.ptr()));
    stats.expectHr("SetFVF(attached RT)", device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
    stats.expectHr("EVENT Issue initial", eventQuery->Issue(D3DISSUE_END));
    for (int i = 0; i < 3; ++i) {
      BOOL done = FALSE;
      for (int spin = 0; spin < 1000; ++spin) {
        hr = eventQuery->GetData(&done, sizeof(done), D3DGETDATA_FLUSH);
        if (hr == S_OK) {
          break;
        }
        Sleep(1);
      }
      stats.expect(hr == S_OK, "attached-rt-sampling event wait");
      stats.expectHr("DrawPrimitiveUP(attached RT)",
                     device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
      stats.expectHr("EVENT Issue after draw", eventQuery->Issue(D3DISSUE_END));
    }
    stats.expectHr("EndScene(attached RT)", device_->EndScene());

    ComPtr<IDirect3DSurface9> staging;
    if (copyRtToSysmem(rt.ptr(), staging, stats)) {
      expectSurfacePixel(staging.ptr(), 32, 32, 193, 193, 193, "attached-rt-sampling accumulated sample", stats);
    }
  }

  void runBlitFormatConversion(TestStats& stats) {
    resetFixedFunctionState();
    struct Conversion {
      D3DFORMAT src;
      D3DFORMAT dst;
      D3DCOLOR color;
      D3DCOLOR expect32;
      std::uint16_t expect16;
      bool read16;
      const char* label;
    };
    const Conversion conversions[] = {
        {D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, 0x12345678, 0x00345678, 0, false, "A8R8G8B8->X8R8G8B8"},
        {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, 0x00345678, 0xff345678, 0, false, "X8R8G8B8->A8R8G8B8"},
        {D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DCOLOR_XRGB(0, 255, 0), 0, 0x07e0, true, "A8R8G8B8->R5G6B5"},
    };

    for (const Conversion& conversion : conversions) {
      ComPtr<IDirect3DTexture9> srcTexture;
      ComPtr<IDirect3DTexture9> dstTexture;
      if (!createTexture(4, 4, conversion.src, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
                         srcTexture, stats, "CreateTexture(blit-format src)") ||
          !createTexture(4, 4, conversion.dst, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
                         dstTexture, stats, "CreateTexture(blit-format dst)")) {
        continue;
      }
      ComPtr<IDirect3DSurface9> src;
      ComPtr<IDirect3DSurface9> dst;
      stats.expectHr("GetSurfaceLevel(blit-format src)", srcTexture->GetSurfaceLevel(0, src.put()));
      stats.expectHr("GetSurfaceLevel(blit-format dst)", dstTexture->GetSurfaceLevel(0, dst.put()));
      RECT rect{0, 0, 2, 2};
      stats.expectHr("ColorFill(blit-format)", device_->ColorFill(src.ptr(), &rect, conversion.color));
      stats.expectHr("StretchRect(blit-format)", device_->StretchRect(src.ptr(), nullptr, dst.ptr(), nullptr, D3DTEXF_POINT));
      D3DSURFACE_DESC desc{};
      dst->GetDesc(&desc);
      ComPtr<IDirect3DSurface9> staging;
      const HRESULT hr = device_->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                              D3DPOOL_SYSTEMMEM, staging.put(), nullptr);
      stats.expectHr("CreateOffscreenPlainSurface(blit-format)", hr);
      if (FAILED(hr)) {
        continue;
      }
      stats.expectHr("GetRenderTargetData(blit-format)", device_->GetRenderTargetData(dst.ptr(), staging.ptr()));
      if (conversion.read16) {
        expectSurfaceU16(staging.ptr(), 0, 0, conversion.expect16, conversion.label, stats);
      } else {
        D3DCOLOR actual = 0;
        const bool read = readSurfacePixel(staging.ptr(), 0, 0, actual);
        if (read && (actual & 0x00ffffffu) != (conversion.expect32 & 0x00ffffffu)) {
          logf("DETAIL: %s actual=0x%08lx expected=0x%08lx",
               conversion.label,
               static_cast<unsigned long>(actual),
               static_cast<unsigned long>(conversion.expect32));
        }
        stats.expect(read && ((actual & 0x00ffffffu) == (conversion.expect32 & 0x00ffffffu)),
                     conversion.label);
      }
    }
    clearBackbuffer(D3DCOLOR_XRGB(20, 20, 20), stats);
    finishScene(stats);
  }

  void runResetResourceLifecycle(TestStats& stats) {
    resetFixedFunctionState();
    ComPtr<IDirect3DSurface9> defaultSurface;
    const HRESULT defaultHr = device_->CreateOffscreenPlainSurface(16,
                                                                   16,
                                                                   D3DFMT_A8R8G8B8,
                                                                   D3DPOOL_DEFAULT,
                                                                   defaultSurface.put(),
                                                                   nullptr);
    stats.expectHr("CreateOffscreenPlainSurface(DEFAULT before reset)", defaultHr);

    ComPtr<IDirect3DTexture9> managedTexture;
    if (createTexture(2, 2, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED, managedTexture, stats,
                      "CreateTexture(MANAGED before reset)")) {
      fillTextureQuadrants(managedTexture.ptr(),
                           D3DCOLOR_XRGB(0, 220, 0),
                           D3DCOLOR_XRGB(0, 220, 0),
                           D3DCOLOR_XRGB(0, 220, 0),
                           D3DCOLOR_XRGB(0, 220, 0),
                           stats);
    }

    D3DPRESENT_PARAMETERS resetParams = params_;
    stats.expectHr("ResetEx(with default/managed resources)", device_->ResetEx(&resetParams, nullptr));
    stats.expectHr("TestCooperativeLevel(after ResetEx)", device_->TestCooperativeLevel());
    if (defaultSurface) {
      stats.expectHr("ColorFill(DEFAULT after ResetEx)",
                     device_->ColorFill(defaultSurface.ptr(), nullptr, D3DCOLOR_XRGB(40, 80, 200)));
    }
    if (managedTexture) {
      stats.expectHr("SetTexture(managed after ResetEx)", device_->SetTexture(0, managedTexture.ptr()));
      stats.expectHr("SetTextureStageState(COLORARG1 TEXTURE after ResetEx)",
                     device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
      clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
      drawTexturedRhwQuadColors(D3DCOLOR_ARGB(255, 255, 255, 255), stats);
      finishScene(stats);
      readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                         "reset-resource-lifecycle managed texture survives reset", stats);
    }
  }

  void runDepthStencilViewportScissor(TestStats& stats) {
    resetFixedFunctionState();
    D3DCAPS9 caps{};
    stats.expectHr("GetDeviceCaps(depth/scissor)", device_->GetDeviceCaps(&caps));
    if (!(caps.RasterCaps & D3DPRASTERCAPS_SCISSORTEST)) {
      logf("SKIP: scissor test unsupported");
      return;
    }

    ComPtr<IDirect3DSurface9> depth;
    if (!createDepthStencil(kWidth, kHeight, depth, stats)) {
      return;
    }
    stats.expectHr("SetDepthStencilSurface", device_->SetDepthStencilSurface(depth.ptr()));
    D3DVIEWPORT9 viewport{20, 15, 100, 80, 0.0f, 1.0f};
    RECT scissor{50, 35, 110, 85};
    stats.expectHr("SetViewport(depth/scissor)", device_->SetViewport(&viewport));
    stats.expectHr("SetScissorRect(depth/scissor)", device_->SetScissorRect(&scissor));
    stats.expectHr("SetRenderState(SCISSOR true)", device_->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE));
    stats.expectHr("SetRenderState(ZENABLE true)", device_->SetRenderState(D3DRS_ZENABLE, TRUE));
    stats.expectHr("SetRenderState(ZWRITE true)", device_->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
    stats.expectHr("SetRenderState(ZFUNC LESS)", device_->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS));

    stats.expectHr("BeginScene(depth/scissor)", device_->BeginScene());
    stats.expectHr("Clear(target+z)", device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                                     D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
    drawRhwQuadZ(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.5f,
                 D3DCOLOR_ARGB(255, 0, 220, 0), "DrawPrimitive(scissored depth green)", stats);
    drawRhwQuadZ(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.8f,
                 D3DCOLOR_ARGB(255, 220, 0, 0), "DrawPrimitive(scissored depth red rejected)", stats);
    finishScene(stats);
    stats.expectHr("SetRenderState(SCISSOR false)", device_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
    readBackbufferNear(80, 60, 0, 220, 0, "depth-stencil-viewport-scissor intersection", stats);
    readBackbufferNear(30, 60, 0, 0, 0, "depth-stencil-viewport-scissor outside scissor", stats);
    readBackbufferNear(130, 60, 0, 0, 0, "depth-stencil-viewport-scissor outside viewport", stats);
  }

  void runMipmapUpdateTexture(TestStats& stats) {
    resetFixedFunctionState();
    D3DCAPS9 caps{};
    stats.expectHr("GetDeviceCaps(mipmap)", device_->GetDeviceCaps(&caps));
    if (!(caps.TextureCaps & D3DPTEXTURECAPS_MIPMAP)) {
      logf("SKIP: mipmap textures unsupported");
      return;
    }

    ComPtr<IDirect3DTexture9> src;
    ComPtr<IDirect3DTexture9> dst;
    if (!createTextureLevels(8, 8, 4, D3DFMT_A8R8G8B8, 0, D3DPOOL_SYSTEMMEM, src, stats,
                             "CreateTexture(SYSTEMMEM mip src)") ||
        !createTextureLevels(8, 8, 4, D3DFMT_A8R8G8B8, 0, D3DPOOL_DEFAULT, dst, stats,
                             "CreateTexture(DEFAULT mip dst)")) {
      return;
    }
    fillTextureLevel(src.ptr(), 0, D3DCOLOR_XRGB(0, 220, 0), stats);
    fillTextureLevel(src.ptr(), 1, D3DCOLOR_XRGB(0, 0, 220), stats);
    fillTextureLevel(src.ptr(), 2, D3DCOLOR_XRGB(220, 220, 0), stats);
    fillTextureLevel(src.ptr(), 3, D3DCOLOR_XRGB(220, 0, 0), stats);
    stats.expectHr("UpdateTexture(mipmap src->dst)",
                   device_->UpdateTexture(src.ptr(), dst.ptr()));

    stats.expectHr("SetTexture(mipmap dst)", device_->SetTexture(0, dst.ptr()));
    stats.expectHr("SetTextureStageState(COLORARG1 TEXTURE mipmap)",
                   device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    drawTexturedRhwQuadColors(D3DCOLOR_ARGB(255, 255, 255, 255), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0, "mipmap-update-texture level0 sample", stats);
  }

  void runMultisampleResolve(TestStats& stats) {
    resetFixedFunctionState();
    DWORD quality = 0;
    HRESULT hr = d3d_->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT,
                                                  D3DDEVTYPE_HAL,
                                                  D3DFMT_A8R8G8B8,
                                                  TRUE,
                                                  D3DMULTISAMPLE_2_SAMPLES,
                                                  &quality);
    if (hr == D3DERR_NOTAVAILABLE) {
      logf("SKIP: 2x MSAA A8R8G8B8 unsupported");
      return;
    }
    stats.expectHr("CheckDeviceMultiSampleType(2x)", hr);
    ComPtr<IDirect3DSurface9> msaa;
    ComPtr<IDirect3DSurface9> resolved;
    hr = device_->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, 0, FALSE,
                                     msaa.put(), nullptr);
    stats.expectHr("CreateRenderTarget(MSAA)", hr);
    if (FAILED(hr) || !createRt(64, 64, resolved, stats)) {
      return;
    }
    stats.expectHr("SetRenderTarget(MSAA)", device_->SetRenderTarget(0, msaa.ptr()));
    stats.expectHr("SetDepthStencilSurface(NULL MSAA)", device_->SetDepthStencilSurface(nullptr));
    stats.expectHr("Clear(MSAA RT)", device_->Clear(0, nullptr, D3DCLEAR_TARGET,
                                                    D3DCOLOR_XRGB(30, 180, 220), 0.0f, 0));
    stats.expectHr("StretchRect(MSAA resolve)",
                   device_->StretchRect(msaa.ptr(), nullptr, resolved.ptr(), nullptr, D3DTEXF_POINT));
    ComPtr<IDirect3DSurface9> staging;
    if (copyRtToSysmem(resolved.ptr(), staging, stats)) {
      expectSurfacePixel(staging.ptr(), 32, 32, 30, 180, 220, "multisample-resolve center", stats);
    }
    stats.expectHr("BeginScene(multisample final clear)", device_->BeginScene());
    stats.expectHr("Clear(backbuffer after MSAA)", device_->Clear(0, nullptr, D3DCLEAR_TARGET,
                                                                  D3DCOLOR_XRGB(30, 180, 220), 0.0f, 0));
    finishScene(stats);
  }

  void runFogDepthBias(TestStats& stats) {
    resetFixedFunctionState();
    ComPtr<IDirect3DSurface9> depth;
    if (!createDepthStencil(kWidth, kHeight, depth, stats)) {
      return;
    }
    stats.expectHr("SetDepthStencilSurface(fog/depthbias)", device_->SetDepthStencilSurface(depth.ptr()));
    stats.expectHr("SetRenderState(ZENABLE fog/depthbias)", device_->SetRenderState(D3DRS_ZENABLE, TRUE));
    stats.expectHr("SetRenderState(ZWRITE fog/depthbias)", device_->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
    stats.expectHr("SetRenderState(ZFUNC LESSEQUAL fog/depthbias)", device_->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL));
    stats.expectHr("SetRenderState(FOGENABLE)", device_->SetRenderState(D3DRS_FOGENABLE, TRUE));
    stats.expectHr("SetRenderState(FOGCOLOR)", device_->SetRenderState(D3DRS_FOGCOLOR, D3DCOLOR_XRGB(0, 220, 220)));
    stats.expectHr("SetRenderState(FOGTABLEMODE)", device_->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR));
    stats.expectHr("SetRenderState(FOGSTART)", device_->SetRenderState(D3DRS_FOGSTART, floatAsDword(0.0f)));
    stats.expectHr("SetRenderState(FOGEND)", device_->SetRenderState(D3DRS_FOGEND, floatAsDword(1.0f)));

    stats.expectHr("BeginScene(fog/depthbias)", device_->BeginScene());
    stats.expectHr("Clear(fog/depthbias)", device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                                          D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
    drawRhwQuadZ(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.9f,
                 D3DCOLOR_ARGB(255, 220, 0, 0), "DrawPrimitive(fogged red)", stats);
    stats.expectHr("SetRenderState(FOGENABLE false)", device_->SetRenderState(D3DRS_FOGENABLE, FALSE));
    stats.expectHr("SetRenderState(DEPTHBIAS negative)", device_->SetRenderState(D3DRS_DEPTHBIAS, floatAsDword(-0.0005f)));
    drawRhwQuadZ(40.0f, 30.0f, 120.0f, 90.0f, 0.9f,
                 D3DCOLOR_ARGB(255, 0, 220, 0), "DrawPrimitive(depth-biased green)", stats);
    finishScene(stats);
    readBackbufferNear(20, 60, 0, 220, 220, "fog-depthbias fogged far color", stats);
    readBackbufferNear(80, 60, 0, 220, 0, "fog-depthbias biased center", stats);
  }

  void runDrawIndexedUpEdges(TestStats& stats) {
    resetFixedFunctionState();
    const XyzDiffuseVertex vertices[8] = {
        {-1.0f, -1.0f, 0.5f, D3DCOLOR_ARGB(255, 220, 0, 0)},
        {-1.0f, 1.0f, 0.5f, D3DCOLOR_ARGB(255, 220, 0, 0)},
        {1.0f, -1.0f, 0.5f, D3DCOLOR_ARGB(255, 220, 0, 0)},
        {1.0f, 1.0f, 0.5f, D3DCOLOR_ARGB(255, 220, 0, 0)},
        {-0.8f, -0.8f, 0.5f, D3DCOLOR_ARGB(255, 0, 220, 0)},
        {-0.8f, 0.8f, 0.5f, D3DCOLOR_ARGB(255, 0, 220, 0)},
        {0.8f, -0.8f, 0.5f, D3DCOLOR_ARGB(255, 0, 220, 0)},
        {0.8f, 0.8f, 0.5f, D3DCOLOR_ARGB(255, 0, 220, 0)},
    };
    const WORD indices[4] = {4, 5, 6, 7};

    stats.expectHr("BeginScene(draw-indexed-up)", device_->BeginScene());
    stats.expectHr("Clear(draw-indexed-up)", device_->Clear(0, nullptr, D3DCLEAR_TARGET,
                                                            D3DCOLOR_XRGB(0, 0, 0), 0.0f, 0));
    stats.expectHr("SetFVF(draw-indexed-up)", device_->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
    stats.expectHr("DrawIndexedPrimitiveUP(base offset)",
                   device_->DrawIndexedPrimitiveUP(D3DPT_TRIANGLESTRIP,
                                                   4,
                                                   4,
                                                   2,
                                                   indices,
                                                   D3DFMT_INDEX16,
                                                   vertices,
                                                   sizeof(vertices[0])));
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0, "draw-indexed-up-edges base vertex quad", stats);

    ComPtr<IDirect3DVertexBuffer9> stream;
    UINT offset = 99;
    UINT stride = 99;
    stats.expectHr("GetStreamSource(after DrawIndexedPrimitiveUP)",
                   device_->GetStreamSource(0, stream.put(), &offset, &stride));
    stats.expect(!stream && offset == 0 && stride == 0, "draw-indexed-up-edges stream source reset");
    ComPtr<IDirect3DIndexBuffer9> ib;
    stats.expectHr("GetIndices(after DrawIndexedPrimitiveUP)", device_->GetIndices(ib.put()));
    stats.expect(!ib, "draw-indexed-up-edges index buffer reset");
  }

  void runShaderEdgeVisual(TestStats& stats) {
    resetFixedFunctionState();
    D3DCAPS9 caps{};
    stats.expectHr("GetDeviceCaps(shader edge)", device_->GetDeviceCaps(&caps));
    if (caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) {
      logf("SKIP: shader-edge-visual requires ps_2_0");
      return;
    }
    static const DWORD psGreen[] = {
        0xffff0200,
        0x05000051, 0xa00f0000, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000,
        0x02000001, 0x800f0800, 0xa0e40000,
        0x0000ffff};
    static const DWORD invalidPs[] = {0xffff0200, 0xffffffff};
    ComPtr<IDirect3DPixelShader9> invalid;
    HRESULT hr = device_->CreatePixelShader(invalidPs, invalid.put());
    stats.expect(FAILED(hr), "shader-edge-visual invalid shader rejected");
    ComPtr<IDirect3DPixelShader9> ps;
    stats.expectHr("CreatePixelShader(shader edge green)", device_->CreatePixelShader(psGreen, ps.put()));
    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    stats.expectHr("SetPixelShader(shader edge)", device_->SetPixelShader(ps.ptr()));
    drawRhwQuad(20.0f, 20.0f, 140.0f, 100.0f, D3DCOLOR_ARGB(255, 220, 0, 0), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 255, 0, "shader-edge-visual pixel shader output", stats);
  }

  void runD3d9ExWsi(TestStats& stats) {
    resetFixedFunctionState();
    stats.expectHr("TestCooperativeLevel(d3d9ex)", device_->TestCooperativeLevel());
    stats.expectHr("CheckDeviceState(hwnd)", device_->CheckDeviceState(hwnd_));
    HRESULT nullState = device_->CheckDeviceState(nullptr);
    stats.expect(nullState == D3D_OK || nullState == S_PRESENT_OCCLUDED,
                 "CheckDeviceState(NULL) returns ok or occluded");
    stats.expectHr("SetMaximumFrameLatency(1)", device_->SetMaximumFrameLatency(1));
    UINT latency = 0;
    stats.expectHr("GetMaximumFrameLatency", device_->GetMaximumFrameLatency(&latency));
    stats.expect(latency == 1, "d3d9ex-wsi maximum frame latency roundtrip");

    D3DPRESENT_PARAMETERS resetParams = params_;
    stats.expectHr("ResetEx(d3d9ex-wsi)", device_->ResetEx(&resetParams, nullptr));
    clearBackbuffer(D3DCOLOR_XRGB(80, 120, 200), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 80, 120, 200, "d3d9ex-wsi post-reset clear", stats);
  }

  void runCubeVolumeTextureUpdate(TestStats& stats) {
    resetFixedFunctionState();
    D3DCAPS9 caps{};
    stats.expectHr("GetDeviceCaps(cube/volume update)", device_->GetDeviceCaps(&caps));
    if (caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) {
      logf("SKIP: cube-volume-texture-update requires ps_2_0");
      return;
    }
    if (!(caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) ||
        !(caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP)) {
      logf("SKIP: cube or volume textures unsupported");
      return;
    }

    static const DWORD psCube[] = {
        0xffff0200,
        0x0200001f, 0x98000000, 0xa00f0800,
        0x0200001f, 0x80000000, 0xb00f0000,
        0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800,
        0x02000001, 0x800f0800, 0x80e40000,
        0x0000ffff};
    static const DWORD psVolume[] = {
        0xffff0200,
        0x0200001f, 0xa0000000, 0xa00f0800,
        0x0200001f, 0x80000000, 0xb00f0000,
        0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800,
        0x02000001, 0x800f0800, 0x80e40000,
        0x0000ffff};
    ComPtr<IDirect3DPixelShader9> cubeShader;
    ComPtr<IDirect3DPixelShader9> volumeShader;
    stats.expectHr("CreatePixelShader(cube sampler)", device_->CreatePixelShader(psCube, cubeShader.put()));
    stats.expectHr("CreatePixelShader(volume sampler)", device_->CreatePixelShader(psVolume, volumeShader.put()));

    ComPtr<IDirect3DCubeTexture9> cubeSrc;
    ComPtr<IDirect3DCubeTexture9> cubeDst;
    HRESULT hr = device_->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8,
                                            D3DPOOL_SYSTEMMEM, cubeSrc.put(), nullptr);
    stats.expectHr("CreateCubeTexture(SYSTEMMEM src)", hr);
    hr = device_->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, cubeDst.put(), nullptr);
    stats.expectHr("CreateCubeTexture(DEFAULT dst)", hr);
    if (cubeSrc && cubeDst) {
      const D3DCUBEMAP_FACES faces[] = {
          D3DCUBEMAP_FACE_POSITIVE_X,
          D3DCUBEMAP_FACE_NEGATIVE_X,
          D3DCUBEMAP_FACE_POSITIVE_Y,
          D3DCUBEMAP_FACE_NEGATIVE_Y,
          D3DCUBEMAP_FACE_POSITIVE_Z,
          D3DCUBEMAP_FACE_NEGATIVE_Z,
      };
      for (D3DCUBEMAP_FACES face : faces) {
        fillCubeFace(cubeSrc.ptr(), face, 0,
                     face == D3DCUBEMAP_FACE_POSITIVE_X ? D3DCOLOR_XRGB(0, 220, 0)
                                                        : D3DCOLOR_XRGB(220, 0, 0),
                     stats);
      }
      stats.expectHr("UpdateTexture(cube src->dst)", device_->UpdateTexture(cubeSrc.ptr(), cubeDst.ptr()));
      stats.expectHr("SetTexture(cube dst)", device_->SetTexture(0, cubeDst.ptr()));
      stats.expectHr("SetPixelShader(cube)", device_->SetPixelShader(cubeShader.ptr()));
      clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
      drawTexturedRhwQuad3(1.0f, 0.0f, 0.0f, "DrawPrimitive(cube texture sample)", stats);
      finishScene(stats);
      readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                         "cube-volume-texture-update cube +X sample", stats);
    }

    resetFixedFunctionState();
    ComPtr<IDirect3DVolumeTexture9> volumeSrc;
    ComPtr<IDirect3DVolumeTexture9> volumeDst;
    hr = device_->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8,
                                      D3DPOOL_SYSTEMMEM, volumeSrc.put(), nullptr);
    stats.expectHr("CreateVolumeTexture(SYSTEMMEM src)", hr);
    hr = device_->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8,
                                      D3DPOOL_DEFAULT, volumeDst.put(), nullptr);
    stats.expectHr("CreateVolumeTexture(DEFAULT dst)", hr);
    if (volumeSrc && volumeDst) {
      fillVolumeLevel(volumeSrc.ptr(), 0, D3DCOLOR_XRGB(0, 0, 220), stats);
      stats.expectHr("UpdateTexture(volume src->dst)", device_->UpdateTexture(volumeSrc.ptr(), volumeDst.ptr()));
      stats.expectHr("SetTexture(volume dst)", device_->SetTexture(0, volumeDst.ptr()));
      stats.expectHr("SetPixelShader(volume)", device_->SetPixelShader(volumeShader.ptr()));
      clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
      drawTexturedRhwQuad3(0.5f, 0.5f, 0.5f, "DrawPrimitive(volume texture sample)", stats);
      finishScene(stats);
      readBackbufferNear(kWidth / 2, kHeight / 2, 0, 0, 220,
                         "cube-volume-texture-update volume center sample", stats);
    }
  }

  void runAutogenMipmap(TestStats& stats) {
    resetFixedFunctionState();
    HRESULT support = d3d_->CheckDeviceFormat(D3DADAPTER_DEFAULT,
                                              D3DDEVTYPE_HAL,
                                              D3DFMT_X8R8G8B8,
                                              D3DUSAGE_AUTOGENMIPMAP,
                                              D3DRTYPE_TEXTURE,
                                              D3DFMT_A8R8G8B8);
    if (support == D3DOK_NOAUTOGEN || support == D3DERR_NOTAVAILABLE) {
      logf("SKIP: A8R8G8B8 autogen mipmap unsupported");
      return;
    }
    stats.expectHr("CheckDeviceFormat(AUTOGENMIPMAP)", support);

    ComPtr<IDirect3DTexture9> texture;
    HRESULT hr = device_->CreateTexture(16, 16, 0, D3DUSAGE_AUTOGENMIPMAP,
                                        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                        texture.put(), nullptr);
    stats.expectHr("CreateTexture(AUTOGENMIPMAP managed)", hr);
    if (FAILED(hr)) {
      return;
    }
    stats.expect(texture->GetLevelCount() == 1, "autogen-mipmap exposes only level 0");
    stats.expect(texture->GetAutoGenFilterType() == D3DTEXF_LINEAR,
                 "autogen-mipmap default filter is linear");
    stats.expect(texture->SetAutoGenFilterType(D3DTEXF_NONE) == D3DERR_INVALIDCALL,
                 "autogen-mipmap rejects NONE filter");
    stats.expectHr("SetAutoGenFilterType(POINT)", texture->SetAutoGenFilterType(D3DTEXF_POINT));
    fillTextureLevel(texture.ptr(), 0, D3DCOLOR_XRGB(0, 220, 0), stats);
    texture->GenerateMipSubLevels();
    stats.expect(true, "GenerateMipSubLevels");

    stats.expectHr("SetTexture(autogen)", device_->SetTexture(0, texture.ptr()));
    stats.expectHr("SetSamplerState(MIPFILTER POINT autogen)",
                   device_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT));
    stats.expectHr("SetSamplerState(MAXMIPLEVEL 1 autogen)",
                   device_->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 1));
    stats.expectHr("SetTextureStageState(COLORARG1 TEXTURE autogen)",
                   device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    drawTexturedRhwQuadColors(D3DCOLOR_ARGB(255, 255, 255, 255), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                       "autogen-mipmap generated sublevel sample", stats);
  }

  void runNpotFilterLod(TestStats& stats) {
    resetFixedFunctionState();
    D3DCAPS9 caps{};
    stats.expectHr("GetDeviceCaps(npot filter lod)", device_->GetDeviceCaps(&caps));
    if (!(caps.TextureCaps & D3DPTEXTURECAPS_MIPMAP)) {
      logf("SKIP: mipmap textures unsupported");
      return;
    }

    ComPtr<IDirect3DTexture9> texture;
    HRESULT hr = device_->CreateTexture(3, 5, 3, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_MANAGED, texture.put(), nullptr);
    if (hr == D3DERR_INVALIDCALL && (caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL)) {
      logf("SKIP: conditional NPOT mip texture unsupported");
      return;
    }
    stats.expectHr("CreateTexture(NPOT mip chain)", hr);
    if (FAILED(hr)) {
      return;
    }
    fillTextureLevel(texture.ptr(), 0, D3DCOLOR_XRGB(220, 0, 0), stats);
    fillTextureLevel(texture.ptr(), 1, D3DCOLOR_XRGB(0, 220, 0), stats);
    fillTextureLevel(texture.ptr(), 2, D3DCOLOR_XRGB(0, 0, 220), stats);

    stats.expectHr("SetTexture(NPOT)", device_->SetTexture(0, texture.ptr()));
    stats.expectHr("SetSamplerState(MINFILTER POINT NPOT)",
                   device_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    stats.expectHr("SetSamplerState(MAGFILTER POINT NPOT)",
                   device_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    stats.expectHr("SetSamplerState(MIPFILTER POINT NPOT)",
                   device_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT));
    stats.expectHr("SetSamplerState(MAXMIPLEVEL 1 NPOT)",
                   device_->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 1));
    stats.expectHr("SetTextureStageState(COLORARG1 TEXTURE NPOT)",
                   device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    drawTexturedRhwQuadColors(D3DCOLOR_ARGB(255, 255, 255, 255), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                       "npot-filter-lod maxmip level1 sample", stats);
  }

  void runManagedResetTexture(TestStats& stats) {
    resetFixedFunctionState();
    ComPtr<IDirect3DTexture9> texture;
    if (!createTexture(256, 256, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED, texture, stats,
                       "CreateTexture(MANAGED reset texture)")) {
      return;
    }
    fillTextureLevel(texture.ptr(), 0, D3DCOLOR_XRGB(0, 220, 0), stats);

    stats.expectHr("SetTexture(managed reset before)", device_->SetTexture(0, texture.ptr()));
    stats.expectHr("SetTextureStageState(COLORARG1 TEXTURE managed reset before)",
                   device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    clearBackbuffer(D3DCOLOR_XRGB(220, 0, 0), stats);
    drawTexturedRhwQuadColors(D3DCOLOR_ARGB(255, 255, 255, 255), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                       "managed-reset-texture before reset", stats);

    D3DPRESENT_PARAMETERS resetParams = params_;
    stats.expectHr("ResetEx(managed texture)", device_->ResetEx(&resetParams, nullptr));
    resetFixedFunctionState();
    stats.expectHr("SetTexture(managed reset after)", device_->SetTexture(0, texture.ptr()));
    stats.expectHr("SetTextureStageState(COLORARG1 TEXTURE managed reset after)",
                   device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    clearBackbuffer(D3DCOLOR_XRGB(220, 0, 0), stats);
    drawTexturedRhwQuadColors(D3DCOLOR_ARGB(255, 255, 255, 255), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                       "managed-reset-texture after reset", stats);
  }

  void runSampleMask(TestStats& stats) {
    resetFixedFunctionState();
    HRESULT support = d3d_->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT,
                                                       D3DDEVTYPE_HAL,
                                                       D3DFMT_A8R8G8B8,
                                                       TRUE,
                                                       D3DMULTISAMPLE_2_SAMPLES,
                                                       nullptr);
    if (support == D3DERR_NOTAVAILABLE) {
      logf("SKIP: 2x MSAA A8R8G8B8 unsupported");
      return;
    }
    stats.expectHr("CheckDeviceMultiSampleType(sample mask 2x)", support);

    ComPtr<IDirect3DSurface9> msaa;
    ComPtr<IDirect3DSurface9> resolved;
    HRESULT hr = device_->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, 0, FALSE,
                                             msaa.put(), nullptr);
    stats.expectHr("CreateRenderTarget(sample mask MSAA)", hr);
    if (FAILED(hr) || !createRt(64, 64, resolved, stats)) {
      return;
    }

    stats.expectHr("SetRenderTarget(sample mask MSAA)", device_->SetRenderTarget(0, msaa.ptr()));
    stats.expectHr("SetDepthStencilSurface(NULL sample mask)", device_->SetDepthStencilSurface(nullptr));
    stats.expectHr("BeginScene(sample mask)", device_->BeginScene());
    stats.expectHr("Clear(sample mask red)", device_->Clear(0, nullptr, D3DCLEAR_TARGET,
                                                            D3DCOLOR_XRGB(255, 0, 0), 0.0f, 0));
    stats.expectHr("SetRenderState(MULTISAMPLEMASK 0x1)",
                   device_->SetRenderState(D3DRS_MULTISAMPLEMASK, 0x1));
    drawXyzDiffuseQuad(-1.0f, -1.0f, 1.0f, 1.0f, D3DCOLOR_ARGB(255, 255, 255, 255), stats);
    finishScene(stats);

    stats.expectHr("StretchRect(sample mask resolve)",
                   device_->StretchRect(msaa.ptr(), nullptr, resolved.ptr(), nullptr, D3DTEXF_POINT));
    ComPtr<IDirect3DSurface9> staging;
    if (copyRtToSysmem(resolved.ptr(), staging, stats)) {
      expectSurfacePixel(staging.ptr(), 32, 32, 255, 128, 128, "sample-mask partial resolve", stats);
    }
  }

  void runAlphaToCoverage(TestStats& stats) {
    resetFixedFunctionState();
    HRESULT support = d3d_->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT,
                                                       D3DDEVTYPE_HAL,
                                                       D3DFMT_A8R8G8B8,
                                                       TRUE,
                                                       D3DMULTISAMPLE_2_SAMPLES,
                                                       nullptr);
    if (support == D3DERR_NOTAVAILABLE) {
      logf("SKIP: 2x MSAA A8R8G8B8 unsupported");
      return;
    }
    stats.expectHr("CheckDeviceMultiSampleType(alpha-to-coverage 2x)", support);
    support = d3d_->CheckDeviceFormat(D3DADAPTER_DEFAULT,
                                      D3DDEVTYPE_HAL,
                                      D3DFMT_X8R8G8B8,
                                      0,
                                      D3DRTYPE_SURFACE,
                                      static_cast<D3DFORMAT>(MAKEFOURCC('A', 'T', 'O', 'C')));
    if (support == D3DERR_NOTAVAILABLE) {
      logf("SKIP: ATOC pseudo format unsupported");
      return;
    }
    stats.expectHr("CheckDeviceFormat(ATOC)", support);

    ComPtr<IDirect3DSurface9> msaa;
    ComPtr<IDirect3DSurface9> resolved;
    HRESULT hr = device_->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, 0, FALSE,
                                             msaa.put(), nullptr);
    stats.expectHr("CreateRenderTarget(alpha-to-coverage MSAA)", hr);
    if (FAILED(hr) || !createRt(64, 64, resolved, stats)) {
      return;
    }

    ComPtr<IDirect3DTexture9> texture;
    if (!createTexture(4, 4, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED, texture, stats,
                       "CreateTexture(alpha-to-coverage texture)")) {
      return;
    }
    fillTextureLevel(texture.ptr(), 0, D3DCOLOR_ARGB(64, 96, 128, 0), stats);

    stats.expectHr("SetRenderTarget(alpha-to-coverage MSAA)", device_->SetRenderTarget(0, msaa.ptr()));
    stats.expectHr("SetDepthStencilSurface(NULL alpha-to-coverage)", device_->SetDepthStencilSurface(nullptr));
    stats.expectHr("BeginScene(alpha-to-coverage)", device_->BeginScene());
    stats.expectHr("Clear(alpha-to-coverage red)", device_->Clear(0, nullptr, D3DCLEAR_TARGET,
                                                                  D3DCOLOR_XRGB(255, 0, 0), 0.0f, 0));
    stats.expectHr("SetTexture(alpha-to-coverage)", device_->SetTexture(0, texture.ptr()));
    stats.expectHr("SetTextureStageState(COLORARG1 TEXTURE alpha-to-coverage)",
                   device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    stats.expectHr("SetTextureStageState(ALPHAARG1 TEXTURE alpha-to-coverage)",
                   device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
    stats.expectHr("SetRenderState(ADAPTIVETESS_Y ATOC)",
                   device_->SetRenderState(D3DRS_ADAPTIVETESS_Y, MAKEFOURCC('A', 'T', 'O', 'C')));
    stats.expectHr("SetRenderState(ALPHATESTENABLE ATOC)",
                   device_->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE));
    drawTexturedRhwQuadColors(D3DCOLOR_ARGB(255, 255, 255, 255), stats);
    finishScene(stats);

    stats.expectHr("StretchRect(alpha-to-coverage resolve)",
                   device_->StretchRect(msaa.ptr(), nullptr, resolved.ptr(), nullptr, D3DTEXF_POINT));
    ComPtr<IDirect3DSurface9> staging;
    if (copyRtToSysmem(resolved.ptr(), staging, stats)) {
      expectSurfacePixel(staging.ptr(), 32, 32, 176, 64, 0, "alpha-to-coverage partial resolve", stats);
    }
  }

  void runCubeWrap(TestStats& stats) {
    resetFixedFunctionState();
    D3DCAPS9 caps{};
    stats.expectHr("GetDeviceCaps(cube wrap)", device_->GetDeviceCaps(&caps));
    if (caps.PixelShaderVersion < D3DPS_VERSION(2, 0) ||
        !(caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP)) {
      logf("SKIP: cube-wrap requires ps_2_0 and cubemap support");
      return;
    }

    static const DWORD psCube[] = {
        0xffff0200,
        0x0200001f, 0x98000000, 0xa00f0800,
        0x0200001f, 0x80000000, 0xb00f0000,
        0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800,
        0x02000001, 0x800f0800, 0x80e40000,
        0x0000ffff};
    ComPtr<IDirect3DPixelShader9> shader;
    stats.expectHr("CreatePixelShader(cube wrap)", device_->CreatePixelShader(psCube, shader.put()));

    ComPtr<IDirect3DCubeTexture9> cube;
    HRESULT hr = device_->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8,
                                            D3DPOOL_MANAGED, cube.put(), nullptr);
    stats.expectHr("CreateCubeTexture(cube wrap)", hr);
    if (FAILED(hr)) {
      return;
    }
    const D3DCUBEMAP_FACES faces[] = {
        D3DCUBEMAP_FACE_POSITIVE_X,
        D3DCUBEMAP_FACE_NEGATIVE_X,
        D3DCUBEMAP_FACE_POSITIVE_Y,
        D3DCUBEMAP_FACE_NEGATIVE_Y,
        D3DCUBEMAP_FACE_POSITIVE_Z,
        D3DCUBEMAP_FACE_NEGATIVE_Z,
    };
    for (D3DCUBEMAP_FACES face : faces) {
      fillCubeFace(cube.ptr(), face, 0,
                   face == D3DCUBEMAP_FACE_POSITIVE_X ? D3DCOLOR_XRGB(0, 220, 0)
                                                      : D3DCOLOR_XRGB(220, 0, 0),
                   stats);
    }

    stats.expectHr("SetTexture(cube wrap)", device_->SetTexture(0, cube.ptr()));
    stats.expectHr("SetPixelShader(cube wrap)", device_->SetPixelShader(shader.ptr()));
    const DWORD modes[] = {
        D3DTADDRESS_WRAP,
        D3DTADDRESS_MIRROR,
        D3DTADDRESS_CLAMP,
        D3DTADDRESS_BORDER,
        D3DTADDRESS_MIRRORONCE,
    };
    for (DWORD mode : modes) {
      stats.expectHr("SetSamplerState(ADDRESSU cube wrap)", device_->SetSamplerState(0, D3DSAMP_ADDRESSU, mode));
      stats.expectHr("SetSamplerState(ADDRESSV cube wrap)", device_->SetSamplerState(0, D3DSAMP_ADDRESSV, mode));
      stats.expectHr("SetSamplerState(ADDRESSW cube wrap)", device_->SetSamplerState(0, D3DSAMP_ADDRESSW, mode));
      stats.expectHr("SetSamplerState(BORDERCOLOR cube wrap)",
                     device_->SetSamplerState(0, D3DSAMP_BORDERCOLOR, D3DCOLOR_XRGB(0, 0, 220)));
      clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
      drawTexturedRhwQuad3(1.0f, 0.0f, 0.0f, "DrawPrimitive(cube wrap +X)", stats);
      finishScene(stats);
      readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                         "cube-wrap address mode keeps cube lookup", stats);
    }
  }

  void runLineAaBlending(TestStats& stats) {
    resetFixedFunctionState();
    stats.expectHr("SetRenderState(CLIPPING false)", device_->SetRenderState(D3DRS_CLIPPING, FALSE));
    stats.expectHr("SetRenderState(ALPHABLENDENABLE true)",
                   device_->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
    stats.expectHr("SetRenderState(BLENDOP ADD)", device_->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD));
    stats.expectHr("SetRenderState(SRCBLEND SRCALPHA)",
                   device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
    stats.expectHr("SetRenderState(DESTBLEND DESTALPHA)",
                   device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_DESTALPHA));

    clearBackbuffer(D3DCOLOR_ARGB(204, 255, 0, 0), stats);
    drawXyzDiffuseQuad(-1.0f, -1.0f, 1.0f, 1.0f, D3DCOLOR_ARGB(127, 0, 255, 0), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 204, 127, 0,
                       "line-aa-blending alpha blend green over red", stats);

    clearBackbuffer(D3DCOLOR_ARGB(127, 0, 255, 0), stats);
    drawXyzDiffuseQuad(-1.0f, -1.0f, 1.0f, 1.0f, D3DCOLOR_ARGB(204, 255, 0, 0), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 204, 127, 0,
                       "line-aa-blending alpha blend red over green", stats);

    stats.expectHr("SetRenderState(ALPHABLENDENABLE false)",
                   device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    stats.expectHr("SetRenderState(ANTIALIASEDLINEENABLE true)",
                   device_->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, TRUE));

    clearBackbuffer(D3DCOLOR_ARGB(204, 255, 0, 0), stats);
    drawXyzDiffuseQuad(-1.0f, -1.0f, 1.0f, 1.0f, D3DCOLOR_ARGB(127, 0, 255, 0), stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 255, 0,
                       "line-aa-blending AA line state does not alter triangles", stats);
  }

  void runDefaultAttributeComponents(TestStats& stats) {
    resetFixedFunctionState();
    D3DCAPS9 caps{};
    stats.expectHr("GetDeviceCaps(default attributes)", device_->GetDeviceCaps(&caps));
    if (caps.VertexShaderVersion < D3DVS_VERSION(2, 0) ||
        caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) {
      logf("SKIP: default-attribute-components requires vs_2_0/ps_2_0");
      return;
    }

    static const DWORD vsColorThrough[] = {
        0xfffe0200,
        0x0200001f, 0x80000000, 0x900f0000,
        0x0200001f, 0x8000000a, 0x900f0001,
        0x02000001, 0xc00f0000, 0x90e40000,
        0x02000001, 0xd00f0000, 0x90e40001,
        0x0000ffff};
    static const DWORD psColorThrough[] = {
        0xffff0200,
        0x0200001f, 0x80000000, 0x900f0000,
        0x02000001, 0x800f0800, 0x90e40000,
        0x0000ffff};
    static const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END()};
    const XyzOnlyVertex quad[4] = {
        {-1.0f, -1.0f, 0.5f},
        {1.0f, -1.0f, 0.5f},
        {-1.0f, 1.0f, 0.5f},
        {1.0f, 1.0f, 0.5f},
    };

    ComPtr<IDirect3DVertexDeclaration9> declaration;
    ComPtr<IDirect3DVertexShader9> vs;
    ComPtr<IDirect3DPixelShader9> ps;
    stats.expectHr("CreateVertexDeclaration(default attributes)",
                   device_->CreateVertexDeclaration(elements, declaration.put()));
    stats.expectHr("CreateVertexShader(default attributes)",
                   device_->CreateVertexShader(vsColorThrough, vs.put()));
    stats.expectHr("CreatePixelShader(default attributes)",
                   device_->CreatePixelShader(psColorThrough, ps.put()));
    clearBackbuffer(D3DCOLOR_XRGB(220, 0, 0), stats);
    stats.expectHr("SetVertexDeclaration(default attributes)",
                   device_->SetVertexDeclaration(declaration.ptr()));
    stats.expectHr("SetVertexShader(default attributes)", device_->SetVertexShader(vs.ptr()));
    stats.expectHr("SetPixelShader(default attributes)", device_->SetPixelShader(ps.ptr()));
    stats.expectHr("DrawPrimitiveUP(default missing color)",
                   device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 0, 0,
                       "default-attribute-components missing color defaults to zero", stats);
  }

  void runVshaderInputTypes(TestStats& stats) {
    resetFixedFunctionState();
    D3DCAPS9 caps{};
    stats.expectHr("GetDeviceCaps(vshader input types)", device_->GetDeviceCaps(&caps));
    if (caps.VertexShaderVersion < D3DVS_VERSION(2, 0) ||
        caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) {
      logf("SKIP: vshader-input-types requires vs_2_0/ps_2_0");
      return;
    }

    static const DWORD vsColorThrough[] = {
        0xfffe0200,
        0x0200001f, 0x80000000, 0x900f0000,
        0x0200001f, 0x8000000a, 0x900f0001,
        0x02000001, 0xc00f0000, 0x90e40000,
        0x02000001, 0xd00f0000, 0x90e40001,
        0x0000ffff};
    static const DWORD psColorThrough[] = {
        0xffff0200,
        0x0200001f, 0x80000000, 0x900f0000,
        0x02000001, 0x800f0800, 0x90e40000,
        0x0000ffff};
    static const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()};
    const XyzUbyte4nColorVertex quad[4] = {
        {-1.0f, -1.0f, 0.5f, 0, 220, 0, 255},
        {1.0f, -1.0f, 0.5f, 0, 220, 0, 255},
        {-1.0f, 1.0f, 0.5f, 0, 220, 0, 255},
        {1.0f, 1.0f, 0.5f, 0, 220, 0, 255},
    };

    ComPtr<IDirect3DVertexDeclaration9> declaration;
    ComPtr<IDirect3DVertexShader9> vs;
    ComPtr<IDirect3DPixelShader9> ps;
    stats.expectHr("CreateVertexDeclaration(UBYTE4N color)",
                   device_->CreateVertexDeclaration(elements, declaration.put()));
    stats.expectHr("CreateVertexShader(UBYTE4N color)",
                   device_->CreateVertexShader(vsColorThrough, vs.put()));
    stats.expectHr("CreatePixelShader(UBYTE4N color)",
                   device_->CreatePixelShader(psColorThrough, ps.put()));
    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    stats.expectHr("SetVertexDeclaration(UBYTE4N color)",
                   device_->SetVertexDeclaration(declaration.ptr()));
    stats.expectHr("SetVertexShader(UBYTE4N color)", device_->SetVertexShader(vs.ptr()));
    stats.expectHr("SetPixelShader(UBYTE4N color)", device_->SetPixelShader(ps.ptr()));
    stats.expectHr("DrawPrimitiveUP(UBYTE4N color)",
                   device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                       "vshader-input-types UBYTE4N color", stats);
  }

  void runPointSize(TestStats& stats) {
    resetFixedFunctionState();
    stats.expectHr("SetRenderState(POINTSPRITE false)",
                   device_->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE));
    stats.expectHr("SetRenderState(POINTSCALE false)",
                   device_->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE));

    const RhwPSizeVertex point = {
        static_cast<float>(kWidth / 2),
        static_cast<float>(kHeight / 2),
        0.5f,
        1.0f,
        16.0f,
        D3DCOLOR_ARGB(255, 0, 220, 0),
    };

    clearBackbuffer(D3DCOLOR_XRGB(0, 0, 0), stats);
    stats.expectHr("SetFVF(pointsize)", device_->SetFVF(D3DFVF_XYZRHW | D3DFVF_PSIZE | D3DFVF_DIFFUSE));
    stats.expectHr("DrawPrimitiveUP(pointsize)",
                   device_->DrawPrimitiveUP(D3DPT_POINTLIST, 1, &point, sizeof(point)));
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                       "pointsize center covered", stats);
    readBackbufferNear(kWidth / 2 + 20, kHeight / 2, 0, 0, 0,
                       "pointsize outside point remains clear", stats);
  }

  void runDepthStencilInit(TestStats& stats) {
    resetFixedFunctionState();
    ComPtr<IDirect3DSurface9> depth;
    if (!createDepthStencil(kWidth, kHeight, depth, stats)) {
      return;
    }
    stats.expectHr("SetDepthStencilSurface(depth-stencil-init)",
                   device_->SetDepthStencilSurface(depth.ptr()));
    stats.expectHr("SetRenderState(ZENABLE depth-stencil-init)",
                   device_->SetRenderState(D3DRS_ZENABLE, TRUE));
    stats.expectHr("SetRenderState(ZWRITE depth-stencil-init)",
                   device_->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
    stats.expectHr("SetRenderState(ZFUNC LESS depth-stencil-init)",
                   device_->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS));

    stats.expectHr("BeginScene(depth clear 1)", device_->BeginScene());
    stats.expectHr("Clear(depth 1)",
                   device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                  D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
    drawRhwQuadZ(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.5f,
                 D3DCOLOR_ARGB(255, 0, 220, 0), "DrawPrimitive(depth clear accepts)", stats);
    drawRhwQuadZ(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.8f,
                 D3DCOLOR_ARGB(255, 220, 0, 0), "DrawPrimitive(depth reject farther)", stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 220, 0,
                       "depth-stencil-init clear depth 1 accepts nearer draw", stats);

    stats.expectHr("BeginScene(depth clear 0)", device_->BeginScene());
    stats.expectHr("Clear(depth 0)",
                   device_->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                  D3DCOLOR_XRGB(0, 0, 0), 0.0f, 0));
    drawRhwQuadZ(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.5f,
                 D3DCOLOR_ARGB(255, 0, 0, 220), "DrawPrimitive(depth clear rejects)", stats);
    finishScene(stats);
    readBackbufferNear(kWidth / 2, kHeight / 2, 0, 0, 0,
                       "depth-stencil-init clear depth 0 rejects farther draw", stats);
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
