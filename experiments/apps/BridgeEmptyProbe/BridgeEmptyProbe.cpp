// V1 boundary B2 (audit item (b)) — bridge-ABI throughput probe.
//
// docs/research/boundary-benchmarks.md identifies B2 (PE -> unix bridge) as
// covered today only by the chunk_admit / chunk_reject opcode counts. A bigger
// marshalling struct or extra importer validation would not surface in any
// existing perf counter. This probe issues a tight loop of small Clear() calls
// — the cheapest record graph that still triggers a chunk commit — so the
// resulting bridge_commit_latency_*_ns counters reflect pure PE -> unix
// crossing cost, isolated from encode and GPU work.
//
// Workload shape:
//   - default device + small back buffer (no offscreen RTs, no textures, no VB)
//   - in a tight loop: SetRenderTarget(backbuffer) + Clear()
//   - no BeginScene/EndScene to keep the per-iteration record graph minimal
//   - no Present() inside the hot loop — Present cost is a separate boundary
//   - one Present() at exit so the swap chain is well-behaved
//
// Knobs (read once, R-BACK-2.31 determinism analogue):
//   BRIDGE_EMPTY_ITERATIONS  - inner loop count, default 100000
//   DXMT_EXPERIMENT_CAPTURE_PATH / DXMT_CAPTURE_FRAME — kept off by default

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kWidth = 256;
constexpr int kHeight = 256;
constexpr char kWindowClass[] = "dxmt9_bridge_empty_probe_window";

FILE* g_trace = nullptr;

struct AppState {
  HWND hwnd = nullptr;
  IDirect3D9* d3d = nullptr;
  IDirect3DDevice9* device = nullptr;
  IDirect3DSurface9* backBuffer = nullptr;
  D3DPRESENT_PARAMETERS pp{};
  std::int64_t iterations = 100000;
  bool quit = false;
  LARGE_INTEGER qpcFrequency{};
};

std::int64_t qpc_now() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}

double ticks_to_ms(const AppState& app, std::int64_t ticks) {
  if (app.qpcFrequency.QuadPart <= 0) {
    return 0.0;
  }
  return static_cast<double>(ticks) * 1000.0 /
         static_cast<double>(app.qpcFrequency.QuadPart);
}

double ticks_to_ns(const AppState& app, std::int64_t ticks) {
  if (app.qpcFrequency.QuadPart <= 0) {
    return 0.0;
  }
  return static_cast<double>(ticks) * 1.0e9 /
         static_cast<double>(app.qpcFrequency.QuadPart);
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
  std::fprintf(stderr, "FAIL: %s hr=0x%08lx\n", label,
               static_cast<unsigned long>(hr));
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

// Read-once env var (R-BACK-2.31 determinism analogue): the value of
// BRIDGE_EMPTY_ITERATIONS is captured exactly once at startup. Subsequent
// process state (mid-run env mutation, child processes, ...) cannot change
// the loop count, so two runs at the same env value are reproducible.
std::int64_t read_iterations_once() {
  static const std::int64_t value = [] {
    const char* raw = std::getenv("BRIDGE_EMPTY_ITERATIONS");
    if (!raw || !*raw) {
      return static_cast<std::int64_t>(100000);
    }
    char* end = nullptr;
    long long parsed = std::strtoll(raw, &end, 10);
    if (end == raw || parsed <= 0) {
      return static_cast<std::int64_t>(100000);
    }
    return static_cast<std::int64_t>(parsed);
  }();
  return value;
}

bool create_window(AppState& app) {
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

  RECT rect{0, 0, kWidth, kHeight};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  app.hwnd = CreateWindowExA(
      0, kWindowClass, "DXMT9 BridgeEmpty", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
      rect.bottom - rect.top, nullptr, nullptr, GetModuleHandleA(nullptr),
      nullptr);
  if (!app.hwnd) {
    std::fprintf(stderr, "FAIL: CreateWindowExA err=%lu\n", GetLastError());
    return false;
  }
  ShowWindow(app.hwnd, SW_SHOW);
  UpdateWindow(app.hwnd);
  return true;
}

bool create_device(AppState& app) {
  app.d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!app.d3d) {
    std::fprintf(stderr, "FAIL: Direct3DCreate9\n");
    return false;
  }

  app.pp.Windowed = TRUE;
  app.pp.BackBufferWidth = kWidth;
  app.pp.BackBufferHeight = kHeight;
  app.pp.BackBufferCount = 1;
  app.pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  app.pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  app.pp.hDeviceWindow = app.hwnd;
  app.pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  HRESULT hr = app.d3d->CreateDevice(
      0, D3DDEVTYPE_HAL, app.hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING,
      &app.pp, &app.device);
  if (FAILED(hr)) {
    hr = app.d3d->CreateDevice(
        0, D3DDEVTYPE_HAL, app.hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
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

void cleanup(AppState& app) {
  safe_release(app.backBuffer);
  safe_release(app.device);
  safe_release(app.d3d);
  if (app.hwnd) {
    DestroyWindow(app.hwnd);
    app.hwnd = nullptr;
  }
}

void pump_messages(AppState& app) {
  MSG msg{};
  int processed = 0;
  while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
    if (msg.message == WM_QUIT) {
      app.quit = true;
      break;
    }
    if (++processed >= 32) {
      break;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  AppState app{};
  QueryPerformanceFrequency(&app.qpcFrequency);
  app.iterations = read_iterations_once();

  char trace_path[MAX_PATH]{};
  if (asset_path("BridgeEmptyProbe.trace.txt", trace_path, sizeof(trace_path))) {
    DeleteFileA(trace_path);
    g_trace = std::fopen(trace_path, "wb");
  }

  trace_log("OK: BridgeEmptyProbe startup iterations=%lld",
            static_cast<long long>(app.iterations));

  if (!create_window(app)) {
    cleanup(app);
    return 1;
  }
  if (!create_device(app)) {
    cleanup(app);
    return 1;
  }
  trace_log("OK: BridgeEmptyProbe device ready");

  // Hot loop: smallest record graph that still triggers commit_chunk.
  // SetRenderTarget(backbuffer) + Clear() commits one chunk per iteration on
  // the d3d9 frontend. No BeginScene/EndScene, no draw, no Present — those
  // would inflate the chunk record count and dilute the bridge-only signal
  // we are trying to measure.
  const std::int64_t loop_started = qpc_now();
  std::int64_t completed = 0;
  for (std::int64_t i = 0; i < app.iterations && !app.quit; ++i) {
    HRESULT hr = app.device->SetRenderTarget(0, app.backBuffer);
    if (FAILED(hr)) {
      print_hresult("SetRenderTarget(backbuffer)", hr);
      app.quit = true;
      break;
    }
    const DWORD clear_color = 0xff000000u | static_cast<DWORD>(i & 0xffu);
    hr = app.device->Clear(0, nullptr, D3DCLEAR_TARGET, clear_color, 1.0f, 0);
    if (FAILED(hr)) {
      print_hresult("Clear", hr);
      app.quit = true;
      break;
    }
    ++completed;
    // Drain the message queue every 4096 iterations so the window stays
    // responsive on slow runs (BRIDGE_EMPTY_ITERATIONS up to ~10M is
    // plausible for sub-microsecond bridge crossings on Apple Silicon).
    if ((i & 0xfffll) == 0xfffll) {
      pump_messages(app);
    }
  }
  const std::int64_t loop_ticks = qpc_now() - loop_started;

  // One Present() at the end so the swap chain frame is sane and any
  // deferred-record gather flushes through the full path. Not part of the
  // measured window.
  HRESULT hr = app.device->Present(nullptr, nullptr, nullptr, nullptr);
  if (FAILED(hr)) {
    print_hresult("Present(final)", hr);
  }

  const double total_ms = ticks_to_ms(app, loop_ticks);
  const double per_iter_ns = completed > 0
                                 ? ticks_to_ns(app, loop_ticks) /
                                       static_cast<double>(completed)
                                 : 0.0;

  trace_log("OK: BridgeEmptyProbe finished iterations=%lld completed=%lld",
            static_cast<long long>(app.iterations),
            static_cast<long long>(completed));
  // Wall-clock latency line consumed by run_dx9_present_policy_ab.py and the
  // boundary-audit suite. Format mirrors PerformanceProbe's [perf-probe] tag
  // so a single regex captures both.
  trace_log("[perf-probe] probe=bridge-empty iterations=%lld completed=%lld "
            "loop_ms=%.3f loop_ns=%.0f per_iter_ns=%.3f",
            static_cast<long long>(app.iterations),
            static_cast<long long>(completed),
            total_ms,
            ticks_to_ns(app, loop_ticks),
            per_iter_ns);

  cleanup(app);
  if (g_trace) {
    std::fclose(g_trace);
    g_trace = nullptr;
  }
  return completed >= app.iterations ? 0 : 1;
}
