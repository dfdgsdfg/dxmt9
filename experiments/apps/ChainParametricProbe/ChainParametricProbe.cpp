// ChainParametricProbe — sub-CB chain sensitivity probe.
//
// V1 audit item (e), boundary B3+B4. Wave-1 sibling of EncodeReplayProbe;
// extends the same boundary-isolated shape with multiple render passes
// per chunk, parameterised by CHAIN_LENGTH. The U1 SFIV A/B
// (docs/sfiv-benchmark-measurement.md) showed that R-BACK-2.33 cap=4 holds
// and reduces GPU per-CB time p99 by 44%, but failed to recover fps because
// SFIV's 1920x1080 4-RT envelope (~33 MB tile flush per CB) dominated.
//
// The cost model in docs/research/g-axis-tuning.md predicts a workload
// class where pipelining wins: low-RP-density / small-RT / heavy-GPU per
// RP. This probe deliberately constructs that opposite balance:
//   * 256x256 R8G8B8A8, no MSAA, single colour attachment per RT
//     (~256 KB tile-flush envelope per CB instead of SFIV's ~33 MB).
//   * CHAIN_LENGTH distinct render targets, each switched + cleared +
//     drawn into. Each switch is a real R-BACK-2.6 RenderTargetChange
//     split, not a hazard or final split. The encode thread therefore
//     sees CHAIN_LENGTH flushRender(non-Final) calls per iteration, which
//     is what splitMidChunkUnderCap() actually counts under
//     DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass.
//   * ~50 DrawIndexedPrimitive calls per pass to push GPU per-pass time
//     above tile-flush cost.
//
// Mode: single mode. Tunables (read once at process start):
//   CHAIN_LENGTH           default 4   clamp 1..16   (RT count = chain length)
//   CHAIN_DRAWS_PER_PASS   default 50  clamp 1..1024
//   CHAIN_ITERATIONS       default 1000 clamp 1..1000000
//
// The cap policy / cap value (DXMT9_MID_CHUNK_COMMIT_POLICY,
// DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS) are NOT consumed by this
// probe — it only produces the chain shape. The W4 A/B harness sweeps
// those knobs externally.
//
// NO Present(). Boundary isolation requires keeping B6 out of the
// measurement; the recorder's auto-flush hands chunks to the encode thread.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int kRtSize = 256;
constexpr int kWindowSize = 64;  // tiny window; never drawn into
constexpr int kChainLengthMin = 1;
constexpr int kChainLengthMax = 16;
constexpr int kDrawsPerPassMin = 1;
constexpr int kDrawsPerPassMax = 1024;
constexpr int kIterationsMin = 1;
constexpr int kIterationsMax = 1000000;
constexpr DWORD kVertexFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
constexpr char kWindowClass[] = "dxmt9_chain_parametric_window";

FILE* g_trace = nullptr;

struct Vertex {
  float x;
  float y;
  float z;
  float rhw;
  DWORD color;
};

struct ChainTarget {
  IDirect3DTexture9* texture = nullptr;
  IDirect3DSurface9* surface = nullptr;
};

struct TimingTotals {
  std::int64_t windowTicks = 0;
  std::int64_t deviceTicks = 0;
  std::int64_t resourceTicks = 0;
  std::int64_t iterationTicks = 0;
  std::int64_t clearTicks = 0;
  std::int64_t drawLoopTicks = 0;
  std::int64_t cleanupTicks = 0;
  std::int64_t totalTicks = 0;
};

struct AppState {
  HWND hwnd = nullptr;
  IDirect3D9* d3d = nullptr;
  IDirect3DDevice9* device = nullptr;
  IDirect3DVertexBuffer9* vertexBuffer = nullptr;
  IDirect3DIndexBuffer9* indexBuffer = nullptr;
  std::vector<ChainTarget> chain;
  IDirect3DSurface9* depthStencil = nullptr;
  IDirect3DSurface9* backBuffer = nullptr;
  D3DPRESENT_PARAMETERS pp{};
  int chainLength = 4;
  int drawsPerPass = 50;
  int iterations = 1000;
  int iter = 0;
  bool quit = false;
  LARGE_INTEGER qpcFrequency{};
  TimingTotals timings{};
};

std::int64_t qpc_now() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}

struct ScopedTicks {
  explicit ScopedTicks(std::int64_t& t) : ticks(t), start(qpc_now()) {}
  ~ScopedTicks() { ticks += qpc_now() - start; }
  std::int64_t& ticks;
  std::int64_t start = 0;
};

double ticks_to_ms(const AppState& app, std::int64_t ticks) {
  if (app.qpcFrequency.QuadPart <= 0) {
    return 0.0;
  }
  return static_cast<double>(ticks) * 1000.0 /
         static_cast<double>(app.qpcFrequency.QuadPart);
}

double per_iter_ms(const AppState& app, std::int64_t ticks) {
  if (app.iter <= 0) {
    return 0.0;
  }
  return ticks_to_ms(app, ticks) / static_cast<double>(app.iter);
}

template <typename T>
void safe_release(T*& object) {
  if (object) {
    object->Release();
    object = nullptr;
  }
}

void trace_log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stdout, fmt, args);
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
  va_end(args);

  if (!g_trace) {
    return;
  }
  va_start(args, fmt);
  std::vfprintf(g_trace, fmt, args);
  std::fprintf(g_trace, "\n");
  std::fflush(g_trace);
  va_end(args);
}

void print_hresult(const char* label, HRESULT hr) {
  std::fprintf(stderr, "FAIL: %s hr=0x%08lx\n",
               label, static_cast<unsigned long>(hr));
}

bool exe_directory(char* buffer, size_t buffer_size) {
  if (buffer_size == 0) {
    return false;
  }
  DWORD written = GetModuleFileNameA(nullptr, buffer,
                                     static_cast<DWORD>(buffer_size));
  if (written == 0 || written >= buffer_size) {
    return false;
  }
  for (DWORD i = written; i > 0; --i) {
    if (buffer[i - 1] == '\\' || buffer[i - 1] == '/') {
      buffer[i - 1] = '\0';
      return true;
    }
  }
  return false;
}

bool asset_path(const char* name, char* buffer, size_t buffer_size) {
  char dir[MAX_PATH]{};
  if (!exe_directory(dir, sizeof(dir))) {
    return false;
  }
  int written = std::snprintf(buffer, buffer_size, "%s\\%s", dir, name);
  return written > 0 && static_cast<size_t>(written) < buffer_size;
}

int env_int(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (!value || !*value) {
    return fallback;
  }
  char* end = nullptr;
  long parsed = std::strtol(value, &end, 10);
  if (end == value) {
    return fallback;
  }
  return static_cast<int>(parsed);
}

int env_int_clamped(const char* name, int fallback, int lo, int hi) {
  int value = env_int(name, fallback);
  if (value < lo) {
    value = lo;
  }
  if (value > hi) {
    value = hi;
  }
  return value;
}

bool create_window(AppState& app) {
  // A tiny hidden window is required to obtain an HWND for the device.
  // The window is never drawn into; the device renders only into the
  // chain of offscreen RTs.
  WNDCLASSEXA wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
    if (msg == WM_CLOSE) {
      DestroyWindow(hwnd);
      return 0;
    }
    if (msg == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
  };
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = kWindowClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    std::fprintf(stderr, "FAIL: RegisterClassExA err=%lu\n", GetLastError());
    return false;
  }
  app.hwnd = CreateWindowExA(
      0, kWindowClass, "DXMT9 ChainParametric", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, kWindowSize, kWindowSize,
      nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if (!app.hwnd) {
    std::fprintf(stderr, "FAIL: CreateWindowExA err=%lu\n", GetLastError());
    return false;
  }
  // Intentionally do NOT ShowWindow — we never composite.
  return true;
}

bool create_device(AppState& app) {
  app.d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!app.d3d) {
    std::fprintf(stderr, "FAIL: Direct3DCreate9\n");
    return false;
  }

  app.pp.Windowed = TRUE;
  app.pp.BackBufferWidth = kWindowSize;
  app.pp.BackBufferHeight = kWindowSize;
  app.pp.BackBufferCount = 1;
  app.pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  app.pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  app.pp.hDeviceWindow = app.hwnd;
  app.pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  HRESULT hr = app.d3d->CreateDevice(
      0, D3DDEVTYPE_HAL, app.hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      &app.pp, &app.device);
  if (FAILED(hr)) {
    hr = app.d3d->CreateDevice(
        0, D3DDEVTYPE_HAL, app.hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &app.pp, &app.device);
  }
  if (FAILED(hr)) {
    print_hresult("CreateDevice", hr);
    return false;
  }

  hr = app.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &app.backBuffer);
  if (FAILED(hr)) {
    print_hresult("GetBackBuffer", hr);
    return false;
  }
  return true;
}

bool create_chain(AppState& app) {
  // CHAIN_LENGTH distinct 256x256 R8G8B8A8 render targets. Each switch
  // between them is a true R-BACK-2.6 RenderTargetChange split, which is
  // what we want the encode thread to count as a non-Final flushRender().
  app.chain.assign(static_cast<size_t>(app.chainLength), ChainTarget{});
  for (int i = 0; i < app.chainLength; ++i) {
    HRESULT hr = app.device->CreateTexture(
        kRtSize, kRtSize, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &app.chain[static_cast<size_t>(i)].texture, nullptr);
    if (FAILED(hr)) {
      print_hresult("CreateTexture(chain RT)", hr);
      return false;
    }
    hr = app.chain[static_cast<size_t>(i)].texture->GetSurfaceLevel(
        0, &app.chain[static_cast<size_t>(i)].surface);
    if (FAILED(hr)) {
      print_hresult("GetSurfaceLevel(chain RT)", hr);
      return false;
    }
  }

  // Single shared depth-stencil; the spec doesn't require depth variation
  // and reusing one DS keeps the RT switch from being collapsed because of
  // a DS mismatch.
  HRESULT hr = app.device->CreateDepthStencilSurface(
      kRtSize, kRtSize, D3DFMT_D32, D3DMULTISAMPLE_NONE, 0, TRUE,
      &app.depthStencil, nullptr);
  if (FAILED(hr)) {
    // D3DFMT_D32 may be unavailable on some HALs; fall back to D24X8.
    hr = app.device->CreateDepthStencilSurface(
        kRtSize, kRtSize, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE,
        &app.depthStencil, nullptr);
  }
  if (FAILED(hr)) {
    print_hresult("CreateDepthStencilSurface", hr);
    return false;
  }
  hr = app.device->SetDepthStencilSurface(app.depthStencil);
  if (FAILED(hr)) {
    print_hresult("SetDepthStencilSurface", hr);
    return false;
  }

  // Bind the first chain RT as the initial render target so the device
  // is in a known state when the iteration loop begins.
  hr = app.device->SetRenderTarget(0, app.chain[0].surface);
  if (FAILED(hr)) {
    print_hresult("SetRenderTarget(chain[0])", hr);
    return false;
  }
  return true;
}

bool create_geometry(AppState& app) {
  // One small triangle, drawn via DrawIndexedPrimitive. Determinism: every
  // draw call uses the exact same VB+IB contents, the same FVF, and the
  // same parameters every iteration of every render pass.
  static const Vertex kVerts[3] = {
      {16.0f, 16.0f,  0.5f, 1.0f, 0xff80c0ffu},
      {64.0f, 16.0f,  0.5f, 1.0f, 0xff80ff80u},
      {16.0f, 64.0f,  0.5f, 1.0f, 0xffff8080u},
  };
  static const std::uint16_t kIndices[3] = {0u, 1u, 2u};

  HRESULT hr = app.device->CreateVertexBuffer(
      sizeof(kVerts), 0, kVertexFvf, D3DPOOL_MANAGED,
      &app.vertexBuffer, nullptr);
  if (FAILED(hr)) {
    print_hresult("CreateVertexBuffer", hr);
    return false;
  }
  void* mapped = nullptr;
  hr = app.vertexBuffer->Lock(0, 0, &mapped, 0);
  if (FAILED(hr)) {
    print_hresult("VertexBuffer::Lock", hr);
    return false;
  }
  std::memcpy(mapped, kVerts, sizeof(kVerts));
  app.vertexBuffer->Unlock();

  hr = app.device->CreateIndexBuffer(
      sizeof(kIndices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
      &app.indexBuffer, nullptr);
  if (FAILED(hr)) {
    print_hresult("CreateIndexBuffer", hr);
    return false;
  }
  hr = app.indexBuffer->Lock(0, 0, &mapped, 0);
  if (FAILED(hr)) {
    print_hresult("IndexBuffer::Lock", hr);
    return false;
  }
  std::memcpy(mapped, kIndices, sizeof(kIndices));
  app.indexBuffer->Unlock();
  return true;
}

bool create_resources(AppState& app) {
  if (!create_chain(app)) {
    return false;
  }
  if (!create_geometry(app)) {
    return false;
  }

  // Minimal FFP state. Identical every iteration. ZENABLE stays off
  // because the depth-stencil is bound only to keep the RT-switch from
  // collapsing on a DS mismatch — we don't want depth tests to perturb
  // GPU per-pass cost.
  app.device->SetRenderState(D3DRS_LIGHTING, FALSE);
  app.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  app.device->SetRenderState(D3DRS_ZENABLE, FALSE);
  app.device->SetVertexShader(nullptr);
  app.device->SetPixelShader(nullptr);
  app.device->SetFVF(kVertexFvf);
  app.device->SetStreamSource(0, app.vertexBuffer, 0, sizeof(Vertex));
  app.device->SetIndices(app.indexBuffer);
  return true;
}

void run_iteration(AppState& app) {
  ScopedTicks iter_timer(app.timings.iterationTicks);

  for (int p = 0; p < app.chainLength; ++p) {
    HRESULT hr = app.device->SetRenderTarget(
        0, app.chain[static_cast<size_t>(p)].surface);
    if (FAILED(hr)) {
      print_hresult("SetRenderTarget(chain pass)", hr);
      app.quit = true;
      return;
    }

    {
      ScopedTicks clear_timer(app.timings.clearTicks);
      hr = app.device->Clear(0, nullptr, D3DCLEAR_TARGET,
                             D3DCOLOR_XRGB(8, 16, 24), 1.0f, 0);
      if (FAILED(hr)) {
        print_hresult("Clear", hr);
        app.quit = true;
        return;
      }
    }

    if (FAILED(app.device->BeginScene())) {
      std::fprintf(stderr, "FAIL: BeginScene\n");
      app.quit = true;
      return;
    }
    {
      ScopedTicks draw_timer(app.timings.drawLoopTicks);
      for (int i = 0; i < app.drawsPerPass; ++i) {
        // 3 vertices, 1 triangle — same parameters every call.
        hr = app.device->DrawIndexedPrimitive(
            D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
        if (FAILED(hr)) {
          print_hresult("DrawIndexedPrimitive", hr);
          app.quit = true;
          app.device->EndScene();
          return;
        }
      }
    }
    app.device->EndScene();
  }

  // Deliberately NO Present(): coupling B6 (drawable acquisition) into
  // the measurement is what we are trying to avoid. The PE recorder's
  // own auto-flush will commit chunks across the run.
  ++app.iter;
  if ((app.iter % 100) == 0) {
    trace_log("OK: completed iteration %d", app.iter);
  }
  if (app.iter >= app.iterations) {
    app.quit = true;
  }
}

void cleanup(AppState& app) {
  if (app.device) {
    app.device->SetIndices(nullptr);
    app.device->SetStreamSource(0, nullptr, 0, 0);
    app.device->SetDepthStencilSurface(nullptr);
    app.device->SetRenderTarget(0, app.backBuffer);
  }
  safe_release(app.indexBuffer);
  safe_release(app.vertexBuffer);
  safe_release(app.depthStencil);
  for (auto& slot : app.chain) {
    safe_release(slot.surface);
    safe_release(slot.texture);
  }
  app.chain.clear();
  safe_release(app.backBuffer);
  safe_release(app.device);
  safe_release(app.d3d);
  if (app.hwnd) {
    DestroyWindow(app.hwnd);
    app.hwnd = nullptr;
  }
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  AppState app{};
  QueryPerformanceFrequency(&app.qpcFrequency);
  const std::int64_t total_started = qpc_now();
  auto cleanup_with_timing = [&] {
    ScopedTicks cleanup_timer(app.timings.cleanupTicks);
    cleanup(app);
  };

  app.chainLength = env_int_clamped(
      "CHAIN_LENGTH", 4, kChainLengthMin, kChainLengthMax);
  app.drawsPerPass = env_int_clamped(
      "CHAIN_DRAWS_PER_PASS", 50, kDrawsPerPassMin, kDrawsPerPassMax);
  app.iterations = env_int_clamped(
      "CHAIN_ITERATIONS", 1000, kIterationsMin, kIterationsMax);

  char trace_path[MAX_PATH]{};
  if (asset_path("ChainParametricProbe.trace.txt",
                 trace_path, sizeof(trace_path))) {
    DeleteFileA(trace_path);
    g_trace = std::fopen(trace_path, "wb");
  }

  trace_log("OK: ChainParametricProbe startup chain_length=%d "
            "draws_per_pass=%d iterations=%d",
            app.chainLength, app.drawsPerPass, app.iterations);
  {
    ScopedTicks timer(app.timings.windowTicks);
    if (!create_window(app)) {
      cleanup_with_timing();
      return 1;
    }
  }
  {
    ScopedTicks timer(app.timings.deviceTicks);
    if (!create_device(app)) {
      cleanup_with_timing();
      return 1;
    }
  }
  {
    ScopedTicks timer(app.timings.resourceTicks);
    if (!create_resources(app)) {
      cleanup_with_timing();
      return 1;
    }
  }
  trace_log("OK: ChainParametricProbe device ready");

  MSG msg{};
  while (!app.quit) {
    run_iteration(app);
    if (app.quit) {
      break;
    }
    // Drain any queued window messages without blocking.
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
      if (msg.message == WM_QUIT) {
        app.quit = true;
        break;
      }
    }
  }

  trace_log("OK: ChainParametricProbe finished at iteration %d", app.iter);
  cleanup_with_timing();
  app.timings.totalTicks = qpc_now() - total_started;
  trace_log(
      "[perf-probe] iterations=%d chain_length=%d draws_per_pass=%d "
      "total_ms=%.3f window_ms=%.3f device_ms=%.3f resources_ms=%.3f "
      "iteration_ms=%.3f iteration_avg_ms=%.3f clear_ms=%.3f "
      "draw_loop_ms=%.3f draw_loop_avg_ms=%.3f cleanup_ms=%.3f",
      app.iter, app.chainLength, app.drawsPerPass,
      ticks_to_ms(app, app.timings.totalTicks),
      ticks_to_ms(app, app.timings.windowTicks),
      ticks_to_ms(app, app.timings.deviceTicks),
      ticks_to_ms(app, app.timings.resourceTicks),
      ticks_to_ms(app, app.timings.iterationTicks),
      per_iter_ms(app, app.timings.iterationTicks),
      ticks_to_ms(app, app.timings.clearTicks),
      ticks_to_ms(app, app.timings.drawLoopTicks),
      per_iter_ms(app, app.timings.drawLoopTicks),
      ticks_to_ms(app, app.timings.cleanupTicks));
  if (g_trace) {
    std::fclose(g_trace);
    g_trace = nullptr;
  }
  return app.iter >= app.iterations ? 0 : 1;
}
