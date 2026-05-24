#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
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
    FfpOnly,
    MultiRt,
    DepthHeavy,
    Skeletal,
};

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
    std::int64_t renderFrameTicks = 0;
    std::int64_t clearTicks = 0;
    std::int64_t sceneTicks = 0;
    std::int64_t drawLoopTicks = 0;
    std::int64_t presentTicks = 0;
    std::int64_t captureTicks = 0;
    std::int64_t messageTicks = 0;
    std::int64_t cleanupTicks = 0;
    std::int64_t totalTicks = 0;
};

constexpr int kMultiRtCount = 4;
constexpr int kSkeletalConstStart = 16;
constexpr int kSkeletalConstCount = 184;  // 46 bones x 4 vec4

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
    IDirect3DTexture9* defaultTexture = nullptr;
    IDirect3DTexture9* multiRtTextures[kMultiRtCount] = {};
    IDirect3DSurface9* multiRtSurfaces[kMultiRtCount] = {};
    IDirect3DTexture9* depthTexture = nullptr;
    IDirect3DSurface9* depthColorSurface = nullptr;
    IDirect3DSurface9* depthStencilSurface = nullptr;
    IDirect3DSurface9* defaultDepthStencil = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    int frame = 0;
    int exitFrame = 240;
    int drawsPerFrame = 0;
    int captureFrame = -1;
    bool captureDone = false;
    bool quit = false;
    char capturePath[MAX_PATH]{};
    LARGE_INTEGER qpcFrequency{};
    TimingTotals timings{};
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
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(app.qpcFrequency.QuadPart);
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
    if (std::strcmp(value, "ffp-only") == 0) {
        return ProbeMode::FfpOnly;
    }
    if (std::strcmp(value, "multi-rt") == 0) {
        return ProbeMode::MultiRt;
    }
    if (std::strcmp(value, "depth-heavy") == 0) {
        return ProbeMode::DepthHeavy;
    }
    if (std::strcmp(value, "skeletal") == 0) {
        return ProbeMode::Skeletal;
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
        case ProbeMode::FfpOnly:
            app.modeName = "ffp-only";
            app.windowTitle = "DXMT9 FfpOnly";
            app.exitFrame = env_int("DXMT9_PROBE_FRAMES", 120);
            app.drawsPerFrame = env_int("DXMT9_PROBE_DRAWS", 64);
            break;
        case ProbeMode::MultiRt:
            app.modeName = "multi-rt";
            app.windowTitle = "DXMT9 MultiRt";
            app.exitFrame = env_int("DXMT9_PROBE_FRAMES", 120);
            // Drawn in groups of 16 across kMultiRtCount targets per frame.
            app.drawsPerFrame = env_int("DXMT9_PROBE_DRAWS", 64);
            break;
        case ProbeMode::DepthHeavy:
            app.modeName = "depth-heavy";
            app.windowTitle = "DXMT9 DepthHeavy";
            app.exitFrame = env_int("DXMT9_PROBE_FRAMES", 120);
            // Split as 32 depth-pass + 16 color-with-shadow per frame.
            app.drawsPerFrame = env_int("DXMT9_PROBE_DRAWS", 48);
            break;
        case ProbeMode::Skeletal:
            app.modeName = "skeletal";
            app.windowTitle = "DXMT9 Skeletal";
            app.exitFrame = env_int("DXMT9_PROBE_FRAMES", 120);
            app.drawsPerFrame = env_int("DXMT9_PROBE_DRAWS", 64);
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

bool create_default_texture(AppState& app) {
    if (app.mode != ProbeMode::FfpOnly) {
        return true;
    }
    HRESULT hr = app.device->CreateTexture(
        4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        &app.defaultTexture, nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateTexture(ffp default)", hr);
        return false;
    }
    D3DLOCKED_RECT locked{};
    hr = app.defaultTexture->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr)) {
        print_hresult("LockRect(ffp default)", hr);
        return false;
    }
    auto* row = static_cast<unsigned char*>(locked.pBits);
    for (int y = 0; y < 4; ++y) {
        auto* px = reinterpret_cast<DWORD*>(row + y * locked.Pitch);
        for (int x = 0; x < 4; ++x) {
            const DWORD checker = ((x + y) & 1) ? 0xffd0d0d0u : 0xff404040u;
            px[x] = checker;
        }
    }
    app.defaultTexture->UnlockRect(0);
    return true;
}

bool create_multi_rt_targets(AppState& app) {
    if (app.mode != ProbeMode::MultiRt) {
        return true;
    }
    for (int i = 0; i < kMultiRtCount; ++i) {
        HRESULT hr = app.device->CreateTexture(
            kWidth, kHeight, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &app.multiRtTextures[i], nullptr);
        if (FAILED(hr)) {
            print_hresult("CreateTexture(multi-rt)", hr);
            return false;
        }
        hr = app.multiRtTextures[i]->GetSurfaceLevel(0, &app.multiRtSurfaces[i]);
        if (FAILED(hr)) {
            print_hresult("GetSurfaceLevel(multi-rt)", hr);
            return false;
        }
    }
    return true;
}

bool create_depth_resources(AppState& app) {
    if (app.mode != ProbeMode::DepthHeavy) {
        return true;
    }
    HRESULT hr = app.device->CreateTexture(
        kWidth, kHeight, 1, D3DUSAGE_DEPTHSTENCIL,
        D3DFMT_D24S8, D3DPOOL_DEFAULT, &app.depthTexture, nullptr);
    if (FAILED(hr)) {
        // Some drivers only expose D24X8 or deny DEPTHSTENCIL textures; fall
        // back to a plain depth-stencil surface so the probe still exercises
        // depth load/store paths even without depth-as-texture sampling.
        hr = app.device->CreateDepthStencilSurface(
            kWidth, kHeight, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
            &app.depthStencilSurface, nullptr);
        if (FAILED(hr)) {
            print_hresult("CreateDepthStencilSurface(depth-heavy)", hr);
            return false;
        }
    } else {
        hr = app.depthTexture->GetSurfaceLevel(0, &app.depthStencilSurface);
        if (FAILED(hr)) {
            print_hresult("GetSurfaceLevel(depth-heavy)", hr);
            return false;
        }
    }

    hr = app.device->CreateRenderTarget(
        kWidth, kHeight, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
        &app.depthColorSurface, nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateRenderTarget(depth-heavy color)", hr);
        return false;
    }

    app.device->GetDepthStencilSurface(&app.defaultDepthStencil);
    return true;
}

bool create_resources(AppState& app) {
    if (!create_geometry(app)) {
        return false;
    }
    if (!create_offscreen_target(app)) {
        return false;
    }
    if (!create_default_texture(app)) {
        return false;
    }
    if (!create_multi_rt_targets(app)) {
        return false;
    }
    if (!create_depth_resources(app)) {
        return false;
    }

    app.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    app.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    const BOOL z_enable = (app.mode == ProbeMode::DepthHeavy) ? TRUE : FALSE;
    app.device->SetRenderState(D3DRS_ZENABLE, z_enable);
    if (app.mode == ProbeMode::DepthHeavy) {
        app.device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        app.device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    }
    return true;
}

void maybe_capture_surface(AppState& app, int completed_frame, IDirect3DSurface9* source) {
    if (app.captureDone || app.captureFrame <= 0 || completed_frame != app.captureFrame || !source) {
        return;
    }
    ScopedTicks capture_timer(app.timings.captureTicks);

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
    ScopedTicks draw_timer(app.timings.drawLoopTicks);
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

void draw_probe_geometry_range(AppState& app, int begin, int count) {
    if (!app.vertexBuffer || count <= 0) {
        return;
    }
    ScopedTicks draw_timer(app.timings.drawLoopTicks);
    app.device->SetFVF(kVertexFvf);
    app.device->SetStreamSource(0, app.vertexBuffer, 0, sizeof(Vertex));
    const int total = std::max(app.drawsPerFrame, 1);
    for (int i = 0; i < count; ++i) {
        const int slot = (begin + i) % total;
        HRESULT hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, slot * 6, 2);
        if (FAILED(hr)) {
            print_hresult("DrawPrimitive", hr);
            app.quit = true;
            return;
        }
    }
}

void render_ffp_only(AppState& app) {
    // Force fixed-function shader bindings: explicitly null shaders, sample
    // a tiny default texture from stage 0. This separates FFP-only draw cost
    // from VS/PS-bound draws.
    app.device->SetVertexShader(nullptr);
    app.device->SetPixelShader(nullptr);
    if (app.defaultTexture) {
        app.device->SetTexture(0, app.defaultTexture);
        app.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        app.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        app.device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        app.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        app.device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        app.device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        app.device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
    draw_probe_geometry(app);
    if (app.defaultTexture) {
        app.device->SetTexture(0, nullptr);
    }
}

void render_multi_rt(AppState& app) {
    // Bind kMultiRtCount RTs in sequence with 16 draws each.
    constexpr int kDrawsPerRt = 16;
    const int total_rt_changes = std::max(1, app.drawsPerFrame / kDrawsPerRt);
    for (int rt = 0; rt < total_rt_changes; ++rt) {
        const int slot = (app.frame + rt) % kMultiRtCount;
        IDirect3DSurface9* surface = app.multiRtSurfaces[slot];
        if (!surface) {
            continue;
        }
        HRESULT hr = app.device->SetRenderTarget(0, surface);
        if (FAILED(hr)) {
            print_hresult("SetRenderTarget(multi-rt)", hr);
            app.quit = true;
            return;
        }
        const DWORD clear_color = D3DCOLOR_XRGB(
            16 + ((slot * 31) % 80),
            16 + ((slot * 53) % 80),
            16 + ((slot * 79) % 80));
        app.device->Clear(0, nullptr, D3DCLEAR_TARGET, clear_color, 1.0f, 0);
        draw_probe_geometry_range(app, rt * kDrawsPerRt, kDrawsPerRt);
        if (app.quit) {
            return;
        }
    }
    // Restore the backbuffer as RT so Present has something to swap.
    app.device->SetRenderTarget(0, app.backBuffer);
}

void render_depth_heavy(AppState& app) {
    // Phase 1: depth-only pass. Bind a color RT (so the device is happy) but
    // mask out color writes; the depth surface receives all updates.
    if (!app.depthStencilSurface || !app.depthColorSurface) {
        draw_probe_geometry(app);
        return;
    }
    HRESULT hr = app.device->SetRenderTarget(0, app.depthColorSurface);
    if (FAILED(hr)) {
        print_hresult("SetRenderTarget(depth-color)", hr);
        app.quit = true;
        return;
    }
    app.device->SetDepthStencilSurface(app.depthStencilSurface);
    app.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                     D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    constexpr int kDepthDraws = 32;
    constexpr int kColorDraws = 16;
    app.device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    app.device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    draw_probe_geometry_range(app, 0, kDepthDraws);
    if (app.quit) {
        return;
    }

    // Phase 2: bind depth as a sampled texture (if available) and draw to a
    // color RT with depth-write disabled so the depth_store action persists.
    app.device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
    app.device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    if (app.depthTexture) {
        app.device->SetTexture(0, app.depthTexture);
        app.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        app.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        app.device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    }
    draw_probe_geometry_range(app, kDepthDraws, kColorDraws);
    if (app.depthTexture) {
        app.device->SetTexture(0, nullptr);
    }

    // Restore default depth-stencil + backbuffer for Present.
    app.device->SetRenderTarget(0, app.backBuffer);
    app.device->SetDepthStencilSurface(app.defaultDepthStencil);
    app.device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

void render_skeletal(AppState& app) {
    // Simulate per-draw bone-matrix churn by updating
    // vsFloatConst[16..16+kSkeletalConstCount) before each draw. This is the
    // hot path for SetVertexShaderConstantF traffic that R-BACK uniform-pack
    // optimisations target.
    if (!app.vertexBuffer || app.drawsPerFrame <= 0) {
        return;
    }
    ScopedTicks draw_timer(app.timings.drawLoopTicks);
    app.device->SetFVF(kVertexFvf);
    app.device->SetStreamSource(0, app.vertexBuffer, 0, sizeof(Vertex));
    std::vector<float> bone_block(static_cast<size_t>(kSkeletalConstCount) * 4u);
    for (int i = 0; i < app.drawsPerFrame; ++i) {
        const float t = static_cast<float>(app.frame) * 0.0125f
                      + static_cast<float>(i) * 0.0037f;
        for (int c = 0; c < kSkeletalConstCount; ++c) {
            const float phase = t + static_cast<float>(c) * 0.017f;
            bone_block[c * 4 + 0] = phase;
            bone_block[c * 4 + 1] = phase * 0.5f;
            bone_block[c * 4 + 2] = phase * 0.25f;
            bone_block[c * 4 + 3] = 1.0f;
        }
        HRESULT hr = app.device->SetVertexShaderConstantF(
            kSkeletalConstStart, bone_block.data(), kSkeletalConstCount);
        if (FAILED(hr)) {
            print_hresult("SetVertexShaderConstantF(skeletal)", hr);
            app.quit = true;
            return;
        }
        hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, i * 6, 2);
        if (FAILED(hr)) {
            print_hresult("DrawPrimitive(skeletal)", hr);
            app.quit = true;
            return;
        }
    }
}

void render_frame(AppState& app) {
    ScopedTicks render_timer(app.timings.renderFrameTicks);
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
    {
        ScopedTicks clear_timer(app.timings.clearTicks);
        DWORD clear_flags = D3DCLEAR_TARGET;
        if (app.mode == ProbeMode::DepthHeavy) {
            clear_flags |= D3DCLEAR_ZBUFFER;
        }
        app.device->Clear(0, nullptr, clear_flags, clear_color, 1.0f, 0);
    }

    {
        ScopedTicks scene_timer(app.timings.sceneTicks);
        if (FAILED(app.device->BeginScene())) {
            std::fprintf(stderr, "FAIL: BeginScene\n");
            app.quit = true;
            return;
        }
        switch (app.mode) {
            case ProbeMode::FfpOnly:
                render_ffp_only(app);
                break;
            case ProbeMode::MultiRt:
                render_multi_rt(app);
                break;
            case ProbeMode::DepthHeavy:
                render_depth_heavy(app);
                break;
            case ProbeMode::Skeletal:
                render_skeletal(app);
                break;
            case ProbeMode::PresentOnly:
            case ProbeMode::OffscreenHeavy:
            case ProbeMode::ManyDraw:
                draw_probe_geometry(app);
                break;
        }
        app.device->EndScene();
    }

    maybe_capture_surface(app, app.frame + 1, target);

    if (app.mode != ProbeMode::OffscreenHeavy) {
        ScopedTicks present_timer(app.timings.presentTicks);
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
    for (int i = 0; i < kMultiRtCount; ++i) {
        safe_release(app.multiRtSurfaces[i]);
        safe_release(app.multiRtTextures[i]);
    }
    safe_release(app.depthColorSurface);
    safe_release(app.depthStencilSurface);
    safe_release(app.depthTexture);
    safe_release(app.defaultDepthStencil);
    safe_release(app.defaultTexture);
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
    QueryPerformanceFrequency(&app.qpcFrequency);
    const std::int64_t total_started = qpc_now();
    auto cleanup_with_timing = [&] {
        ScopedTicks cleanup_timer(app.timings.cleanupTicks);
        cleanup(app);
    };
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
    trace_log("OK: PerformanceProbe device ready");

    MSG msg{};
    while (!app.quit) {
        render_frame(app);
        if (app.quit) {
            break;
        }
        {
            ScopedTicks message_timer(app.timings.messageTicks);
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

    trace_log("OK: PerformanceProbe finished at frame %d", app.frame);
    cleanup_with_timing();
    app.timings.totalTicks = qpc_now() - total_started;
    trace_log("[perf-probe] frames=%d draws_per_frame=%d total_ms=%.3f window_ms=%.3f "
              "device_ms=%.3f resources_ms=%.3f render_frame_ms=%.3f "
              "render_frame_avg_ms=%.3f clear_ms=%.3f scene_ms=%.3f "
              "draw_loop_ms=%.3f draw_loop_avg_ms=%.3f present_ms=%.3f "
              "present_avg_ms=%.3f capture_ms=%.3f message_ms=%.3f cleanup_ms=%.3f",
              app.frame, app.drawsPerFrame,
              ticks_to_ms(app, app.timings.totalTicks),
              ticks_to_ms(app, app.timings.windowTicks),
              ticks_to_ms(app, app.timings.deviceTicks),
              ticks_to_ms(app, app.timings.resourceTicks),
              ticks_to_ms(app, app.timings.renderFrameTicks),
              per_frame_ms(app, app.timings.renderFrameTicks),
              ticks_to_ms(app, app.timings.clearTicks),
              ticks_to_ms(app, app.timings.sceneTicks),
              ticks_to_ms(app, app.timings.drawLoopTicks),
              per_frame_ms(app, app.timings.drawLoopTicks),
              ticks_to_ms(app, app.timings.presentTicks),
              per_frame_ms(app, app.timings.presentTicks),
              ticks_to_ms(app, app.timings.captureTicks),
              ticks_to_ms(app, app.timings.messageTicks),
              ticks_to_ms(app, app.timings.cleanupTicks));
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    return app.frame >= app.exitFrame ? 0 : 1;
}
