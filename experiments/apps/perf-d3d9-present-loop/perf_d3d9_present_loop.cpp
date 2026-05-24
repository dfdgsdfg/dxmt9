// PresentLoopProbe — V1 audit (f) drawable-acquire isolation probe (B6).
//
// Runs N empty Present() cycles with no draw work — just Clear() and
// Present(). The dominant cost per iteration is the GPU-presenter
// boundary, so the existing `present_*` counter family (acquire/token/
// boundary waits, preacquire stats, present_encoded) reflects only B6
// instead of being entangled with encode CPU + GPU draw work the way
// the existing perf probes are.
//
// Iteration count is read once from PRESENT_LOOP_ITERATIONS (default 1000).
// Backbuffer is intentionally tiny (256x256) so the per-frame Clear is
// nearly free and any wall-time variance attributes to drawable acquire
// or compositor pacing.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kBackBufferWidth = 256;
constexpr int kBackBufferHeight = 256;
constexpr int kDefaultIterations = 1000;
constexpr char kWindowClass[] = "dxmt9_perf_probe_present_loop_window";
constexpr char kWindowTitle[] = "DXMT9 PresentLoop";

FILE* g_trace = nullptr;

struct AppState {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    int iterations = kDefaultIterations;
    int frame = 0;
    bool quit = false;
    LARGE_INTEGER qpcFrequency{};
    std::int64_t windowTicks = 0;
    std::int64_t deviceTicks = 0;
    std::int64_t clearTicks = 0;
    std::int64_t presentTicks = 0;
    std::int64_t messageTicks = 0;
    std::int64_t cleanupTicks = 0;
    std::int64_t totalTicks = 0;
};

std::int64_t qpc_now() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

struct ScopedTicks {
    explicit ScopedTicks(std::int64_t& ticks) : ticks(ticks), start(qpc_now()) {}
    ~ScopedTicks() {
        ticks += qpc_now() - start;
    }

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

double per_frame_ms(const AppState& app, std::int64_t ticks) {
    if (app.frame <= 0) {
        return 0.0;
    }
    return ticks_to_ms(app, ticks) / static_cast<double>(app.frame);
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
    DWORD written =
        GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(buffer_size));
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

    RECT rect{0, 0, kBackBufferWidth, kBackBufferHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    app.hwnd = CreateWindowExA(
        0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
        rect.bottom - rect.top, nullptr, nullptr,
        GetModuleHandleA(nullptr), nullptr);
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
    app.pp.BackBufferWidth = kBackBufferWidth;
    app.pp.BackBufferHeight = kBackBufferHeight;
    app.pp.BackBufferCount = 1;
    app.pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    app.pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    app.pp.hDeviceWindow = app.hwnd;
    app.pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = app.d3d->CreateDevice(
        0, D3DDEVTYPE_HAL, app.hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &app.pp, &app.device);
    if (FAILED(hr)) {
        hr = app.d3d->CreateDevice(
            0, D3DDEVTYPE_HAL, app.hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &app.pp, &app.device);
    }
    if (FAILED(hr)) {
        print_hresult("CreateDevice", hr);
        return false;
    }
    return true;
}

void run_iteration(AppState& app) {
    const DWORD clear_color = D3DCOLOR_XRGB(
        12 + ((app.frame * 3) % 36),
        20 + ((app.frame * 5) % 48),
        32 + ((app.frame * 7) % 64));
    {
        ScopedTicks clear_timer(app.clearTicks);
        HRESULT hr = app.device->Clear(0, nullptr, D3DCLEAR_TARGET,
                                       clear_color, 1.0f, 0);
        if (FAILED(hr)) {
            print_hresult("Clear", hr);
            app.quit = true;
            return;
        }
    }

    {
        ScopedTicks present_timer(app.presentTicks);
        HRESULT hr = app.device->Present(nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            print_hresult("Present", hr);
            app.quit = true;
            return;
        }
    }

    ++app.frame;
    if ((app.frame % 200) == 0) {
        trace_log("OK: PresentLoopProbe iter %d/%d", app.frame, app.iterations);
    }
    if (app.frame >= app.iterations) {
        app.quit = true;
    }
}

void cleanup(AppState& app) {
    safe_release(app.device);
    safe_release(app.d3d);
    if (app.hwnd) {
        DestroyWindow(app.hwnd);
        app.hwnd = nullptr;
    }
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    AppState app{};
    QueryPerformanceFrequency(&app.qpcFrequency);
    const std::int64_t total_started = qpc_now();
    auto cleanup_with_timing = [&] {
        ScopedTicks cleanup_timer(app.cleanupTicks);
        cleanup(app);
    };

    app.iterations = env_int("PRESENT_LOOP_ITERATIONS", kDefaultIterations);
    if (app.iterations < 1) {
        app.iterations = 1;
    }

    char trace_path[MAX_PATH]{};
    if (asset_path("PresentLoopProbe.trace.txt", trace_path, sizeof(trace_path))) {
        DeleteFileA(trace_path);
        g_trace = std::fopen(trace_path, "wb");
    }

    trace_log("OK: PresentLoopProbe startup iterations=%d backbuffer=%dx%d",
              app.iterations, kBackBufferWidth, kBackBufferHeight);

    {
        ScopedTicks timer(app.windowTicks);
        if (!create_window(app)) {
            cleanup_with_timing();
            return 1;
        }
    }
    {
        ScopedTicks timer(app.deviceTicks);
        if (!create_device(app)) {
            cleanup_with_timing();
            return 1;
        }
    }
    trace_log("OK: PresentLoopProbe device ready");

    MSG msg{};
    while (!app.quit) {
        run_iteration(app);
        if (app.quit) {
            break;
        }
        {
            ScopedTicks message_timer(app.messageTicks);
            int messages_processed = 0;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
                if (msg.message == WM_QUIT) {
                    app.quit = true;
                    break;
                }
                if (++messages_processed >= 32) {
                    break;
                }
            }
        }
    }

    trace_log("OK: PresentLoopProbe finished at iter %d", app.frame);
    cleanup_with_timing();
    app.totalTicks = qpc_now() - total_started;
    trace_log("[present-loop-probe] iterations=%d total_ms=%.3f window_ms=%.3f "
              "device_ms=%.3f clear_ms=%.3f clear_avg_ms=%.3f present_ms=%.3f "
              "present_avg_ms=%.3f message_ms=%.3f cleanup_ms=%.3f",
              app.frame,
              ticks_to_ms(app, app.totalTicks),
              ticks_to_ms(app, app.windowTicks),
              ticks_to_ms(app, app.deviceTicks),
              ticks_to_ms(app, app.clearTicks),
              per_frame_ms(app, app.clearTicks),
              ticks_to_ms(app, app.presentTicks),
              per_frame_ms(app, app.presentTicks),
              ticks_to_ms(app, app.messageTicks),
              ticks_to_ms(app, app.cleanupTicks));
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    return app.frame >= app.iterations ? 0 : 1;
}
