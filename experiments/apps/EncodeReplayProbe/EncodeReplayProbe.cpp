// EncodeReplayProbe — boundary-isolated encode-thread throughput probe.
//
// Goal: measure dxmt9 boundary B3 (CommandQueue) + B4 (encode thread) in
// isolation from B6 (Presenter) and from drawable acquisition. The probe
// renders the SAME small offscreen workload N times with no Present, so
// the only per-iteration variance is encode/queue noise. The PE recorder
// batches records into chunks and the recorder's capacity-driven auto-flush
// hands chunks to the encode thread, producing measurable
// `encode_chunk_calls` and `encode_chunk_cpu_ms` counters without coupling
// to drawable acquisition or compositor pacing.
//
// Approach A from docs/research/boundary-benchmarks.md section (c): the
// disk-based chunk serialization called out in Approach B does not exist
// today (chunk_record_import_spec / chunk_record_replay_spec construct
// chunks in memory, not from a file format), so this probe instead
// guarantees iteration determinism by replaying an identical record
// stream from inside a single process.
//
// Mode: single mode (no DXMT9_PROBE_MODE switching). Tunables:
//   ENCODE_REPLAY_DRAWS_PER_FRAME   default 100
//   ENCODE_REPLAY_ITERATIONS        default 1000
// Read once at process start.

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
constexpr DWORD kVertexFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
constexpr char kWindowClass[] = "dxmt9_encode_replay_window";

FILE* g_trace = nullptr;

struct Vertex {
  float x;
  float y;
  float z;
  float rhw;
  DWORD color;
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
  IDirect3DTexture9* offscreenTexture = nullptr;
  IDirect3DSurface9* offscreenSurface = nullptr;
  IDirect3DSurface9* backBuffer = nullptr;
  D3DPRESENT_PARAMETERS pp{};
  int drawsPerIter = 100;
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

bool create_window(AppState& app) {
  // A tiny hidden window is required to obtain an HWND for the device.
  // The window is never drawn into; the device renders to an offscreen RT.
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
      0, kWindowClass, "DXMT9 EncodeReplay", WS_OVERLAPPEDWINDOW,
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

bool create_offscreen_target(AppState& app) {
  // 256x256 R8G8B8A8 RT, no MSAA. Bound for the entire run so the encode
  // thread sees a stable single render-pass envelope iteration after
  // iteration.
  HRESULT hr = app.device->CreateTexture(
      kRtSize, kRtSize, 1, D3DUSAGE_RENDERTARGET,
      D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &app.offscreenTexture, nullptr);
  if (FAILED(hr)) {
    print_hresult("CreateTexture(offscreen RT)", hr);
    return false;
  }
  hr = app.offscreenTexture->GetSurfaceLevel(0, &app.offscreenSurface);
  if (FAILED(hr)) {
    print_hresult("GetSurfaceLevel(offscreen RT)", hr);
    return false;
  }
  hr = app.device->SetRenderTarget(0, app.offscreenSurface);
  if (FAILED(hr)) {
    print_hresult("SetRenderTarget(offscreen RT)", hr);
    return false;
  }
  return true;
}

bool create_geometry(AppState& app) {
  // One small triangle, drawn via DrawIndexedPrimitive. The whole point of
  // this probe is determinism: every draw call uses the exact same VB+IB
  // contents, the same FVF, and the same parameters every iteration.
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
  if (!create_offscreen_target(app)) {
    return false;
  }
  if (!create_geometry(app)) {
    return false;
  }

  // Minimal FFP state. Identical every iteration.
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

  // Identical clear color across iterations — boundary-isolation requires
  // a deterministic record stream.
  {
    ScopedTicks clear_timer(app.timings.clearTicks);
    HRESULT hr = app.device->Clear(0, nullptr, D3DCLEAR_TARGET,
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
    for (int i = 0; i < app.drawsPerIter; ++i) {
      // 3 vertices, 1 triangle — same parameters every call.
      HRESULT hr = app.device->DrawIndexedPrimitive(
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

  // Deliberately NO Present(): coupling B6 (drawable acquisition) into the
  // measurement is what we are trying to avoid. The PE recorder's own
  // capacity-driven auto-flush will commit chunks across the run, so the
  // encode thread receives work without any drawable-acquire pacing.
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
    app.device->SetRenderTarget(0, app.backBuffer);
  }
  safe_release(app.indexBuffer);
  safe_release(app.vertexBuffer);
  safe_release(app.offscreenSurface);
  safe_release(app.offscreenTexture);
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

  app.drawsPerIter = std::max(env_int("ENCODE_REPLAY_DRAWS_PER_FRAME", 100), 1);
  app.iterations = std::max(env_int("ENCODE_REPLAY_ITERATIONS", 1000), 1);

  char trace_path[MAX_PATH]{};
  if (asset_path("EncodeReplayProbe.trace.txt",
                 trace_path, sizeof(trace_path))) {
    DeleteFileA(trace_path);
    g_trace = std::fopen(trace_path, "wb");
  }

  trace_log("OK: EncodeReplayProbe startup draws_per_iter=%d iterations=%d",
            app.drawsPerIter, app.iterations);
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
  trace_log("OK: EncodeReplayProbe device ready");

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

  trace_log("OK: EncodeReplayProbe finished at iteration %d", app.iter);
  cleanup_with_timing();
  app.timings.totalTicks = qpc_now() - total_started;
  trace_log(
      "[perf-probe] iterations=%d draws_per_iter=%d total_ms=%.3f "
      "window_ms=%.3f device_ms=%.3f resources_ms=%.3f "
      "iteration_ms=%.3f iteration_avg_ms=%.3f clear_ms=%.3f "
      "draw_loop_ms=%.3f draw_loop_avg_ms=%.3f cleanup_ms=%.3f",
      app.iter, app.drawsPerIter,
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
