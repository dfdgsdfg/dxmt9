// Project-authored public PE oracle: exact visibility, query reuse, and pixels.
// Run explicitly with the real visibility provider; conservative fallback is
// an intentional negative control, not an alternative passing implementation.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {
int failures = 0;
void check(bool value, int line, const char* expression) {
  if (!value) {
    std::printf("FAIL:%d: %s\n", line, expression);
    ++failures;
  }
}
#define CHECK(x) check(!!(x), __LINE__, #x)
#define OK(x) CHECK((x) == D3D_OK)

template<typename T> struct Com {
  T* ptr = nullptr;
  ~Com() { if (ptr) ptr->Release(); }
  T* operator->() const { return ptr; }
};

void occlusion_query_draw_pixels(IDirect3DDevice9* device, bool edge) {
  Com<IDirect3DSurface9> target, depth, readback, scaled, scaledReadback;
  Com<IDirect3DQuery9> query;
  OK(device->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8,
      D3DMULTISAMPLE_NONE, 0, FALSE, &target.ptr, nullptr));
  OK(device->CreateDepthStencilSurface(16, 16, D3DFMT_D24S8,
      D3DMULTISAMPLE_NONE, 0, FALSE, &depth.ptr, nullptr));
  OK(device->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8,
      D3DPOOL_SYSTEMMEM, &readback.ptr, nullptr));
  OK(device->CreateRenderTarget(8, 8, D3DFMT_A8R8G8B8,
      D3DMULTISAMPLE_NONE, 0, FALSE, &scaled.ptr, nullptr));
  OK(device->CreateOffscreenPlainSurface(8, 8, D3DFMT_A8R8G8B8,
      D3DPOOL_SYSTEMMEM, &scaledReadback.ptr, nullptr));
  OK(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query.ptr));
  if (!target.ptr || !depth.ptr || !readback.ptr || !query.ptr ||
      !scaled.ptr || !scaledReadback.ptr) return;
  OK(device->SetRenderTarget(0, target.ptr));
  OK(device->SetDepthStencilSurface(depth.ptr));
  D3DVIEWPORT9 viewport{0, 0, 16, 16, 0, 1};
  OK(device->SetViewport(&viewport));
  OK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
  OK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
  OK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  OK(device->SetRenderState(D3DRS_ZENABLE, TRUE));
  OK(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
  OK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS));
  OK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
  OK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
  OK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
  OK(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
  OK(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
  OK(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
      0xff000000u, 1, 0));

  auto draw = [&](float z, DWORD color) {
    struct Vertex { float x, y, z, rhw; DWORD color; };
    // Overscan the viewport to separate visibility from half-pixel edge rules.
    const float lo = edge ? -0.5f : -1.0f;
    const float hi = edge ? 15.5f : 17.0f;
    const Vertex vertices[] = {
        {lo, lo, z, 1, color}, {hi, lo, z, 1, color},
        {lo, hi, z, 1, color}, {hi, hi, z, 1, color}};
    OK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
  };
  auto pixels = [&](IDirect3DSurface9* src, IDirect3DSurface9* dst,
                    unsigned extent, DWORD expected) {
    OK(device->GetRenderTargetData(src, dst));
    D3DLOCKED_RECT locked{};
    const HRESULT hr = dst->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    OK(hr);
    if (FAILED(hr)) return;
    unsigned mismatches = 0;
    unsigned hash = 2166136261u;
    for (unsigned y = 0; y < extent; ++y)
      for (unsigned x = 0; x < extent; ++x) {
        DWORD value;
        std::memcpy(&value, static_cast<const char*>(locked.pBits) +
            y * locked.Pitch + x * 4, sizeof(value));
        mismatches += value != expected;
        hash = (hash ^ value) * 16777619u;
      }
    std::printf("PIXELS:extent=%u expected=%08lx mismatches=%u hash=%08x\n",
        extent, static_cast<unsigned long>(expected), mismatches, hash);
    CHECK(mismatches == 0);
    OK(dst->UnlockRect());
  };

  // A predecessor outside BEGIN must retain depth/color across the query cut.
  OK(device->BeginScene());
  draw(0.5f, 0xff00ff00u);
  OK(device->EndScene());
  const float depths[] = {0.25f, 0.75f, 0.125f};
  const DWORD colors[] = {0xff00ff00u, 0xffff0000u, 0xff0000ffu};
  const DWORD expectedCounts[] = {256, 0, 256};
  for (unsigned issue = 0; issue < 3; ++issue) {
    OK(device->BeginScene());
    OK(query->Issue(D3DISSUE_BEGIN));
    draw(depths[issue], colors[issue]);
    OK(query->Issue(D3DISSUE_END));
    OK(device->EndScene());
    DWORD result[2] = {0xdeadbeefu, 0xbaadf00du};
    unsigned pending = 0;
    HRESULT hr;
    const DWORD start = GetTickCount();
    do {
      result[0] = 0xdeadbeefu;
      hr = query->GetData(result, sizeof(DWORD), D3DGETDATA_FLUSH);
      CHECK(result[1] == 0xbaadf00du);
      if (hr != S_FALSE) break;
      CHECK(result[0] == 0xdeadbeefu);
      ++pending;
      Sleep(1);
    } while (GetTickCount() - start < 10000);
    CHECK(hr == S_OK);
    if (hr != S_OK) return;
    CHECK(result[0] == expectedCounts[issue]);
    std::printf("REACHED:occlusion_query_draw_pixels issue=%u samples=%lu expected=%lu pending=%u\n",
        issue, static_cast<unsigned long>(result[0]),
        static_cast<unsigned long>(expectedCounts[issue]), pending);
    const char* delay = std::getenv("DXMT9_PERF_COMPLETION_SIGNAL_DELAY_MS");
    if (delay && std::strtoul(delay, nullptr, 10) >= 100) CHECK(pending > 0);
    DWORD repeated = 0xdeadbeefu;
    OK(query->GetData(&repeated, sizeof(repeated), 0));
    CHECK(repeated == result[0]);
    // Nothing below may supply progress to the polling loop above.
    const DWORD expected = issue == 2 ? colors[2] : colors[0];
    pixels(target.ptr, readback.ptr, 16, expected);
    for (const auto filter : {D3DTEXF_POINT, D3DTEXF_LINEAR}) {
      OK(device->StretchRect(target.ptr, nullptr, scaled.ptr, nullptr, filter));
      pixels(scaled.ptr, scaledReadback.ptr, 8, expected);
    }
  }
}
} // namespace

int main(int argc, char** argv) {
  const bool edge = argc == 2 && std::strcmp(argv[1], "--edge") == 0;
  if (argc != 1 && !edge) return EXIT_FAILURE;
  std::printf("GEOMETRY:%s\n", edge ? "edge" : "overscan");
  HWND window = CreateWindowA("static", "dxmt9_occlusion_oracle", WS_OVERLAPPEDWINDOW,
      0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
  Com<IDirect3D9> d3d;
  Com<IDirect3DDevice9> device;
  d3d.ptr = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp{};
  pp.BackBufferWidth = pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = window;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  if (!window || !d3d.ptr || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device.ptr))) {
    std::printf("SKIP:occlusion_query_draw_pixels: device unavailable\n");
    if (window) DestroyWindow(window);
    return 77;
  }
  occlusion_query_draw_pixels(device.ptr, edge);
  device.ptr->Release();
  device.ptr = nullptr;
  DestroyWindow(window);
  std::printf("d3d9_occlusion: %d failures\n", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
