#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr DWORD kVertexFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
constexpr char kWindowClass[] = "dxmt9_perf_probe_window";

FILE* g_trace = nullptr;

enum class ProbeMode {
    PresentOnly,
    OffscreenHeavy,
    ManyDraw,
};

struct Vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
};

struct AppState {
    ProbeMode mode = ProbeMode::PresentOnly;
    const char* modeName = "present-only";
    const char* windowTitle = "DXMT9 PresentOnly";
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DTexture9* offscreenTexture = nullptr;
    IDirect3DSurface9* offscreenSurface = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    int frame = 0;
    int exitFrame = 240;
    int drawsPerFrame = 0;
    int captureFrame = -1;
    bool captureDone = false;
    bool quit = false;
    char capturePath[MAX_PATH]{};
};

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
    std::fprintf(stderr, "FAIL: %s hr=0x%08lx\n", label, static_cast<unsigned long>(hr));
}

bool exe_directory(char* buffer, size_t buffer_size) {
    if (buffer_size == 0) {
        return false;
    }
    DWORD written = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(buffer_size));
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

ProbeMode parse_mode(const char* value) {
    if (!value || !*value || std::strcmp(value, "present-only") == 0) {
        return ProbeMode::PresentOnly;
    }
    if (std::strcmp(value, "offscreen-heavy") == 0) {
        return ProbeMode::OffscreenHeavy;
    }
    if (std::strcmp(value, "many-draw") == 0) {
        return ProbeMode::ManyDraw;
    }
    std::fprintf(stderr, "FAIL: unknown mode '%s'\n", value);
    return ProbeMode::PresentOnly;
}

void configure_mode(AppState& app, ProbeMode mode) {
    app.mode = mode;
    switch (mode) {
        case ProbeMode::PresentOnly:
            app.modeName = "present-only";
            app.windowTitle = "DXMT9 PresentOnly";
            app.exitFrame = env_int("DXMT9_PROBE_FRAMES", 240);
            app.drawsPerFrame = 0;
            break;
        case ProbeMode::OffscreenHeavy:
            app.modeName = "offscreen-heavy";
            app.windowTitle = "DXMT9 OffscreenHeavy";
            app.exitFrame = env_int("DXMT9_PROBE_FRAMES", 120);
            app.drawsPerFrame = env_int("DXMT9_PROBE_DRAWS", 1024);
            break;
        case ProbeMode::ManyDraw:
            app.modeName = "many-draw";
            app.windowTitle = "DXMT9 ManyDraw";
            app.exitFrame = env_int("DXMT9_PROBE_FRAMES", 120);
            app.drawsPerFrame = env_int("DXMT9_PROBE_DRAWS", 512);
            break;
    }
    app.exitFrame = std::max(app.exitFrame, 1);
    app.drawsPerFrame = std::max(app.drawsPerFrame, 0);
}

void init_capture(AppState& app) {
    const char* capture_path = std::getenv("DXMT_EXPERIMENT_CAPTURE_PATH");
    const char* capture_frame = std::getenv("DXMT_CAPTURE_FRAME");
    if (!capture_path || !*capture_path) {
        return;
    }
    std::snprintf(app.capturePath, sizeof(app.capturePath), "%s", capture_path);
    if (capture_frame && *capture_frame) {
        app.captureFrame = std::atoi(capture_frame);
    }
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
        0, kWindowClass, app.windowTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
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

DWORD color_for_index(int index) {
    const unsigned int r = 48u + static_cast<unsigned int>((index * 53) % 176);
    const unsigned int g = 48u + static_cast<unsigned int>((index * 97) % 176);
    const unsigned int b = 48u + static_cast<unsigned int>((index * 193) % 176);
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

bool create_geometry(AppState& app) {
    if (app.drawsPerFrame <= 0) {
        return true;
    }

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(app.drawsPerFrame) * 6u);
    const int columns = 64;
    const float cell_w = static_cast<float>(kWidth) / static_cast<float>(columns);
    const float cell_h = cell_w * 0.70f;
    for (int i = 0; i < app.drawsPerFrame; ++i) {
        const int col = i % columns;
        const int row = (i / columns) % 48;
        const float jitter = static_cast<float>((i * 17) % 11) * 0.37f;
        const float x0 = static_cast<float>(col) * cell_w + jitter;
        const float y0 = static_cast<float>(row) * cell_h + jitter;
        const float x1 = std::min(x0 + cell_w * 0.92f, static_cast<float>(kWidth));
        const float y1 = std::min(y0 + cell_h * 0.92f, static_cast<float>(kHeight));
        const DWORD color = color_for_index(i);
        vertices.push_back(Vertex{x0, y0, 0.5f, 1.0f, color});
        vertices.push_back(Vertex{x1, y0, 0.5f, 1.0f, color});
        vertices.push_back(Vertex{x0, y1, 0.5f, 1.0f, color});
        vertices.push_back(Vertex{x1, y0, 0.5f, 1.0f, color});
        vertices.push_back(Vertex{x1, y1, 0.5f, 1.0f, color});
        vertices.push_back(Vertex{x0, y1, 0.5f, 1.0f, color});
    }

    const UINT byte_size = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    HRESULT hr = app.device->CreateVertexBuffer(
        byte_size, 0, kVertexFvf, D3DPOOL_MANAGED, &app.vertexBuffer, nullptr);
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
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(Vertex));
    app.vertexBuffer->Unlock();
    return true;
}

bool create_offscreen_target(AppState& app) {
    if (app.mode != ProbeMode::OffscreenHeavy) {
        return true;
    }
    HRESULT hr = app.device->CreateTexture(
        kWidth, kHeight, 1, D3DUSAGE_RENDERTARGET,
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
    return true;
}

bool create_resources(AppState& app) {
    if (!create_geometry(app)) {
        return false;
    }
    if (!create_offscreen_target(app)) {
        return false;
    }

    app.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    app.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    app.device->SetRenderState(D3DRS_ZENABLE, FALSE);
    return true;
}

void maybe_capture_surface(AppState& app, int completed_frame, IDirect3DSurface9* source) {
    if (app.captureDone || app.captureFrame <= 0 || completed_frame != app.captureFrame || !source) {
        return;
    }

    D3DSURFACE_DESC desc{};
    source->GetDesc(&desc);
    IDirect3DSurface9* staging = nullptr;
    HRESULT hr = app.device->CreateOffscreenPlainSurface(
        desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateOffscreenPlainSurface(capture)", hr);
        return;
    }

    hr = app.device->GetRenderTargetData(source, staging);
    if (FAILED(hr)) {
        print_hresult("GetRenderTargetData(capture)", hr);
        safe_release(staging);
        return;
    }

    hr = D3DXSaveSurfaceToFileA(app.capturePath, D3DXIFF_BMP, staging, nullptr, nullptr);
    if (FAILED(hr)) {
        print_hresult("D3DXSaveSurfaceToFileA(capture)", hr);
    } else {
        trace_log("OK: captured frame %d -> %s", completed_frame, app.capturePath);
        app.captureDone = true;
    }
    safe_release(staging);
}

void draw_probe_geometry(AppState& app) {
    if (!app.vertexBuffer || app.drawsPerFrame <= 0) {
        return;
    }
    app.device->SetFVF(kVertexFvf);
    app.device->SetStreamSource(0, app.vertexBuffer, 0, sizeof(Vertex));
    for (int i = 0; i < app.drawsPerFrame; ++i) {
        HRESULT hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, i * 6, 2);
        if (FAILED(hr)) {
            print_hresult("DrawPrimitive", hr);
            app.quit = true;
            return;
        }
    }
}

void render_frame(AppState& app) {
    IDirect3DSurface9* target =
        app.mode == ProbeMode::OffscreenHeavy ? app.offscreenSurface : app.backBuffer;
    if (target && app.mode == ProbeMode::OffscreenHeavy) {
        HRESULT hr = app.device->SetRenderTarget(0, target);
        if (FAILED(hr)) {
            print_hresult("SetRenderTarget(offscreen)", hr);
            app.quit = true;
            return;
        }
    }

    const DWORD clear_color = D3DCOLOR_XRGB(
        12 + ((app.frame * 3) % 36),
        20 + ((app.frame * 5) % 48),
        32 + ((app.frame * 7) % 64));
    app.device->Clear(0, nullptr, D3DCLEAR_TARGET, clear_color, 1.0f, 0);

    if (FAILED(app.device->BeginScene())) {
        std::fprintf(stderr, "FAIL: BeginScene\n");
        app.quit = true;
        return;
    }
    draw_probe_geometry(app);
    app.device->EndScene();

    maybe_capture_surface(app, app.frame + 1, target);

    if (app.mode != ProbeMode::OffscreenHeavy) {
        HRESULT hr = app.device->Present(nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            print_hresult("Present", hr);
            app.quit = true;
            return;
        }
    }

    ++app.frame;
    if ((app.frame % 60) == 0) {
        trace_log("OK: rendered frame %d", app.frame);
    }
    if (app.frame >= app.exitFrame) {
        app.quit = true;
    }
}

void cleanup(AppState& app) {
    safe_release(app.offscreenSurface);
    safe_release(app.offscreenTexture);
    safe_release(app.vertexBuffer);
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
    AppState app{};
    const char* mode_arg = argc > 1 ? argv[1] : std::getenv("DXMT9_PROBE_MODE");
    configure_mode(app, parse_mode(mode_arg));
    init_capture(app);

    char trace_name[128]{};
    std::snprintf(trace_name, sizeof(trace_name), "PerformanceProbe-%s.trace.txt", app.modeName);
    char trace_path[MAX_PATH]{};
    if (asset_path(trace_name, trace_path, sizeof(trace_path))) {
        DeleteFileA(trace_path);
        g_trace = std::fopen(trace_path, "wb");
    }

    trace_log("OK: PerformanceProbe startup mode=%s frames=%d draws=%d",
              app.modeName, app.exitFrame, app.drawsPerFrame);
    if (!create_window(app)) {
        cleanup(app);
        return 1;
    }
    if (!create_device(app)) {
        cleanup(app);
        return 1;
    }
    if (!create_resources(app)) {
        cleanup(app);
        return 1;
    }
    trace_log("OK: PerformanceProbe device ready");

    MSG msg{};
    while (!app.quit) {
        render_frame(app);
        if (app.quit) {
            break;
        }
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

    trace_log("OK: PerformanceProbe finished at frame %d", app.frame);
    cleanup(app);
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    return app.frame >= app.exitFrame ? 0 : 1;
}
