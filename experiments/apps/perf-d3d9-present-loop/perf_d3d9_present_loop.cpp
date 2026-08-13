// PresentLoopProbe — V1 audit (f) drawable-acquire isolation probe (B6).
//
// Runs N standard Present() cycles. By default there is no draw work — just
// Present(). The dominant cost per iteration is the GPU-presenter
// boundary, so the existing `present_*` counter family (acquire/token/
// boundary waits, preacquire stats, present_encoded) reflects only B6
// instead of being entangled with encode CPU + GPU draw work the way
// the existing perf probes are.
//
// PRESENT_LOOP_TEXTURED=1 opts into the small frame-tape fixture: each frame
// is Clear + one fixed-function DrawPrimitiveUP with inline XYZRHW/TEX1
// vertices, one fully initialized non-uniform 4x4 A8R8G8B8 texture, and the
// same ordinary Present() call. The default mode remains clear-only for
// existing present-pacing measurements.
// PRESENT_LOOP_SEQUENCE=1 uses the same draw shape and performs one complete
// texture replacement between captured Present intervals 1 and 2.
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
constexpr UINT kTextureSize = 4;
constexpr DWORD kTexturedFvf =
    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
constexpr char kWindowClass[] = "dxmt9_perf_probe_present_loop_window";
constexpr char kWindowTitle[] = "DXMT9 PresentLoop";

// Every texel is explicit so capture/replay probes have a stable, non-uniform
// seed rather than a procedural or partially initialized upload.
constexpr DWORD kTextureSeed[kTextureSize][kTextureSize] = {
    {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu},
    {0xFFFF8000u, 0xFF8000FFu, 0xFF00FFFFu, 0xFFFFFF00u},
    {0xFF400040u, 0xFF408040u, 0xFF404080u, 0xFF804000u},
    {0xFF101010u, 0xFF707070u, 0xFFB0B0B0u, 0xFFE0E0E0u},
};

struct TexturedVertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD diffuse;
    float u;
    float v;
};

FILE* g_trace = nullptr;

struct AppState {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* texture = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    int iterations = kDefaultIterations;
    int frame = 0;
    bool textured = false;
    bool sequence = false;
    bool quit = false;
    LARGE_INTEGER qpcFrequency{};
    std::int64_t windowTicks = 0;
    std::int64_t deviceTicks = 0;
    std::int64_t clearTicks = 0;
    std::int64_t drawTicks = 0;
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

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value && (std::strcmp(value, "1") == 0 ||
                     std::strcmp(value, "true") == 0 ||
                     std::strcmp(value, "yes") == 0);
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

    if (!app.textured) {
        return true;
    }

    hr = app.device->CreateTexture(
        kTextureSize, kTextureSize, 1, D3DUSAGE_DYNAMIC,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &app.texture, nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateTexture", hr);
        return false;
    }

    D3DLOCKED_RECT locked{};
    hr = app.texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr)) {
        print_hresult("Texture LockRect", hr);
        return false;
    }
    for (UINT y = 0; y < kTextureSize; ++y) {
        auto* row = reinterpret_cast<DWORD*>(
            static_cast<unsigned char*>(locked.pBits) +
            static_cast<size_t>(y) * static_cast<size_t>(locked.Pitch));
        std::memcpy(row, kTextureSeed[y], sizeof(kTextureSeed[y]));
    }
    hr = app.texture->UnlockRect(0);
    if (FAILED(hr)) {
        print_hresult("Texture UnlockRect", hr);
        return false;
    }

    app.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    app.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    app.device->SetRenderState(D3DRS_ZENABLE, FALSE);
    app.device->SetFVF(kTexturedFvf);
    app.device->SetTexture(0, app.texture);
    app.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    app.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    app.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    app.device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    app.device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    app.device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    return true;
}

bool draw_textured_quad(AppState& app) {
    const TexturedVertex vertices[3] = {
        {-0.5f, -0.5f, 0.5f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
        {static_cast<float>(kBackBufferWidth * 2) - 0.5f, -0.5f, 0.5f,
         1.0f, 0xffffffffu, 2.0f, 0.0f},
        {-0.5f, static_cast<float>(kBackBufferHeight * 2) - 0.5f, 0.5f,
         1.0f, 0xffffffffu, 0.0f, 2.0f},
    };

    HRESULT hr = app.device->BeginScene();
    if (FAILED(hr)) {
        print_hresult("BeginScene", hr);
        return false;
    }
    hr = app.device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices,
                                      sizeof(vertices[0]));
    const HRESULT end_hr = app.device->EndScene();
    if (FAILED(hr)) {
        print_hresult("DrawPrimitiveUP", hr);
        return false;
    }
    if (FAILED(end_hr)) {
        print_hresult("EndScene", end_hr);
        return false;
    }
    return true;
}

bool mutate_sequence_texture(AppState& app) {
    D3DLOCKED_RECT locked{};
    HRESULT hr = app.texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr)) {
        print_hresult("Sequence texture LockRect", hr);
        return false;
    }
    for (UINT y = 0; y < kTextureSize; ++y) {
        auto* row = reinterpret_cast<DWORD*>(
            static_cast<unsigned char*>(locked.pBits) +
            static_cast<size_t>(y) * static_cast<size_t>(locked.Pitch));
        for (UINT x = 0; x < kTextureSize; ++x) {
            row[x] = kTextureSeed[y][x] ^ 0x00FFFFFFu;
        }
    }
    hr = app.texture->UnlockRect(0);
    if (FAILED(hr)) {
        print_hresult("Sequence texture UnlockRect", hr);
        return false;
    }
    return true;
}

void run_iteration(AppState& app) {
    // Present 0 is the production arm boundary and Present 1 closes the
    // first captured interval. Mutate exactly once before any command in
    // interval 2 so the sequence journal preserves the boundary mutation
    // ahead of that interval's Clear -> Draw -> Present stream.
    if (app.sequence && app.frame == 2 &&
        !mutate_sequence_texture(app)) {
        app.quit = true;
        return;
    }

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

    if (app.textured) {
        ScopedTicks draw_timer(app.drawTicks);
        if (!draw_textured_quad(app)) {
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
    safe_release(app.texture);
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
    app.sequence = env_enabled("PRESENT_LOOP_SEQUENCE");
    app.textured = env_enabled("PRESENT_LOOP_TEXTURED") || app.sequence;
    if (app.iterations < 1) {
        app.iterations = 1;
    }
    if (app.sequence && app.iterations < 3) {
        app.iterations = 3;
    }

    char trace_path[MAX_PATH]{};
    if (asset_path("PresentLoopProbe.trace.txt", trace_path, sizeof(trace_path))) {
        DeleteFileA(trace_path);
        g_trace = std::fopen(trace_path, "wb");
    }

    trace_log("OK: PresentLoopProbe startup iterations=%d backbuffer=%dx%d mode=%s",
              app.iterations, kBackBufferWidth, kBackBufferHeight,
              app.sequence ? "sequence-texture-mutation" :
              (app.textured ? "textured-drawprimitiveup" : "clear-only"));

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
              "device_ms=%.3f clear_ms=%.3f clear_avg_ms=%.3f draw_ms=%.3f "
              "draw_avg_ms=%.3f present_ms=%.3f present_avg_ms=%.3f "
              "message_ms=%.3f cleanup_ms=%.3f mode=%s",
              app.frame,
              ticks_to_ms(app, app.totalTicks),
              ticks_to_ms(app, app.windowTicks),
              ticks_to_ms(app, app.deviceTicks),
              ticks_to_ms(app, app.clearTicks),
              per_frame_ms(app, app.clearTicks),
              ticks_to_ms(app, app.drawTicks),
              per_frame_ms(app, app.drawTicks),
              ticks_to_ms(app, app.presentTicks),
              per_frame_ms(app, app.presentTicks),
              ticks_to_ms(app, app.messageTicks),
              ticks_to_ms(app, app.cleanupTicks),
              app.sequence ? "sequence-texture-mutation" :
              (app.textured ? "textured-drawprimitiveup" : "clear-only"));
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    return app.frame >= app.iterations ? 0 : 1;
}
