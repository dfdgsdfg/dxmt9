#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kExitFrame = 180;
constexpr char kWindowClass[] = "dxmt9_irrlicht_managed_lights_window";
constexpr char kWindowTitle[] = "Irrlicht Engine Demo";
constexpr UINT kTextureSize = 256;

FILE* g_trace = nullptr;

struct AppState {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DTexture9* texture = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    int frame = 0;
    bool quit = false;
    int captureFrame = -1;
    bool captureDone = false;
    char capturePath[MAX_PATH]{};
};

constexpr DWORD kManagedLightsFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

struct ManagedVertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float u;
    float v;
};

template <typename T>
void safe_release_t(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void print_hresult(const char* label, HRESULT hr) {
    std::fprintf(stderr, "FAIL: %s hr=0x%08lx\n", label, static_cast<unsigned long>(hr));
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

void maybe_capture_backbuffer(AppState& app, int completed_frame) {
    if (app.captureDone || app.captureFrame <= 0 || completed_frame != app.captureFrame) {
        return;
    }

    IDirect3DSurface9* backbuffer = nullptr;
    IDirect3DSurface9* staging = nullptr;
    HRESULT hr = app.device->GetRenderTarget(0, &backbuffer);
    if (FAILED(hr)) {
        print_hresult("GetRenderTarget(capture)", hr);
        return;
    }

    D3DSURFACE_DESC desc{};
    backbuffer->GetDesc(&desc);
    hr = app.device->CreateOffscreenPlainSurface(
        desc.Width,
        desc.Height,
        desc.Format,
        D3DPOOL_SYSTEMMEM,
        &staging,
        nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateOffscreenPlainSurface(capture)", hr);
        safe_release_t(backbuffer);
        return;
    }

    hr = app.device->GetRenderTargetData(backbuffer, staging);
    if (FAILED(hr)) {
        print_hresult("GetRenderTargetData(capture)", hr);
        safe_release_t(staging);
        safe_release_t(backbuffer);
        return;
    }

    hr = D3DXSaveSurfaceToFileA(app.capturePath, D3DXIFF_BMP, staging, nullptr, nullptr);
    if (FAILED(hr)) {
        print_hresult("D3DXSaveSurfaceToFileA(capture)", hr);
    } else {
        trace_log("OK: captured frame %d -> %s", completed_frame, app.capturePath);
        app.captureDone = true;
    }

    safe_release_t(staging);
    safe_release_t(backbuffer);
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
        0,
        kWindowClass,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);
    if (!app.hwnd) {
        std::fprintf(stderr, "FAIL: CreateWindowExA err=%lu\n", GetLastError());
        return false;
    }

    ShowWindow(app.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(app.hwnd);
    return true;
}

bool create_device(AppState& app) {
    app.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!app.d3d) {
        std::fprintf(stderr, "FAIL: Direct3DCreate9 returned nullptr\n");
        return false;
    }

    std::memset(&app.pp, 0, sizeof(app.pp));
    app.pp.BackBufferWidth = kWidth;
    app.pp.BackBufferHeight = kHeight;
    app.pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    app.pp.BackBufferCount = 1;
    app.pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    app.pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    app.pp.hDeviceWindow = app.hwnd;
    app.pp.Windowed = TRUE;
    app.pp.EnableAutoDepthStencil = FALSE;
    app.pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = app.d3d->CreateDevice(
        0,
        D3DDEVTYPE_HAL,
        app.hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &app.pp,
        &app.device);
    if (FAILED(hr)) {
        hr = app.d3d->CreateDevice(
            0,
            D3DDEVTYPE_HAL,
            app.hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING,
            &app.pp,
            &app.device);
    }
    if (FAILED(hr)) {
        print_hresult("CreateDevice", hr);
        return false;
    }

    app.device->SetRenderState(D3DRS_ZENABLE, FALSE);
    app.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    app.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    app.device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    app.device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    app.device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(255, 255, 255));
    return true;
}

void set_material(AppState& app) {
    D3DMATERIAL9 material{};
    material.Diffuse.r = 1.0f;
    material.Diffuse.g = 1.0f;
    material.Diffuse.b = 1.0f;
    material.Diffuse.a = 1.0f;
    material.Ambient = material.Diffuse;
    material.Specular.r = 0.45f;
    material.Specular.g = 0.40f;
    material.Specular.b = 0.35f;
    material.Specular.a = 1.0f;
    material.Emissive.r = 0.05f;
    material.Emissive.g = 0.05f;
    material.Emissive.b = 0.08f;
    material.Emissive.a = 1.0f;
    material.Power = 12.0f;
    app.device->SetMaterial(&material);
}

void set_scene_transforms(AppState& app, float time_sec) {
    D3DXMATRIX world;
    D3DXMatrixRotationY(&world, time_sec * 0.20f);

    D3DXVECTOR3 eye(0.0f, 0.0f, -4.0f);
    D3DXVECTOR3 target(0.0f, 0.0f, 0.0f);
    D3DXVECTOR3 up(0.0f, 1.0f, 0.0f);
    D3DXMATRIX view;
    D3DXMatrixLookAtLH(&view, &eye, &target, &up);

    D3DXMATRIX projection;
    D3DXMatrixPerspectiveFovLH(&projection, D3DX_PI / 3.0f, static_cast<float>(kWidth) / static_cast<float>(kHeight),
                               0.1f, 50.0f);

    app.device->SetTransform(D3DTS_WORLD, &world);
    app.device->SetTransform(D3DTS_VIEW, &view);
    app.device->SetTransform(D3DTS_PROJECTION, &projection);
}

void set_managed_lights(AppState& app, float time_sec) {
    (void)app;
    (void)time_sec;
}

bool create_scene_resources(AppState& app) {
    static const ManagedVertex kVertices[3] = {
        {-1.45f, -1.10f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f},
        { 0.00f,  1.25f, 0.0f, 0.0f, 0.0f, -1.0f, 0.5f, 0.0f},
        { 1.45f, -1.10f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f},
    };

    HRESULT hr = app.device->CreateTexture(
        kTextureSize,
        kTextureSize,
        1,
        0,
        D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,
        &app.texture,
        nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateTexture", hr);
        return false;
    }

    app.device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    app.device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    app.device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    app.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    app.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    app.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    app.device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    app.device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

    hr = app.device->CreateVertexBuffer(sizeof(kVertices), 0, kManagedLightsFvf, D3DPOOL_MANAGED, &app.vertexBuffer, nullptr);
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
    std::memcpy(mapped, kVertices, sizeof(kVertices));
    app.vertexBuffer->Unlock();

    set_material(app);
    return true;
}

void update_texture(AppState& app, float time_sec) {
    if (!app.texture) {
        return;
    }

    D3DLOCKED_RECT locked{};
    HRESULT hr = app.texture->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr)) {
        print_hresult("Texture::LockRect", hr);
        app.quit = true;
        return;
    }

    const struct LightBlob {
        float u;
        float v;
        float radius;
        float r;
        float g;
        float b;
    } blobs[3] = {
        {0.50f + std::cos(time_sec * 0.80f) * 0.24f, 0.52f + std::sin(time_sec * 0.65f) * 0.20f, 0.28f, 1.00f, 0.56f, 0.22f},
        {0.52f + std::cos(time_sec * 0.55f + 2.0f) * 0.22f, 0.46f + std::sin(time_sec * 0.70f + 1.5f) * 0.18f, 0.24f, 0.18f, 0.64f, 1.00f},
        {0.48f + std::cos(time_sec * 0.72f + 4.3f) * 0.18f, 0.54f + std::sin(time_sec * 0.58f + 3.1f) * 0.22f, 0.22f, 0.92f, 0.28f, 0.88f},
    };

    for (UINT y = 0; y < kTextureSize; ++y) {
        auto* row = reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(locked.pBits) + y * locked.Pitch);
        for (UINT x = 0; x < kTextureSize; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kTextureSize);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kTextureSize);
            float r = 0.08f;
            float g = 0.09f;
            float b = 0.12f;

            const float line_u = std::abs(std::fmod(u * 12.0f + time_sec * 0.35f, 1.0f) - 0.5f);
            const float line_v = std::abs(std::fmod(v * 10.0f - time_sec * 0.27f, 1.0f) - 0.5f);
            const float grid = std::max(0.0f, 1.0f - std::min(line_u, line_v) * 10.0f);
            r += grid * 0.04f;
            g += grid * 0.05f;
            b += grid * 0.06f;

            for (const auto& blob : blobs) {
                const float dx = u - blob.u;
                const float dy = v - blob.v;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float glow = std::max(0.0f, 1.0f - dist / blob.radius);
                const float intensity = glow * glow * (1.2f + glow * 0.6f);
                r += blob.r * intensity;
                g += blob.g * intensity;
                b += blob.b * intensity;
            }

            const auto to_channel = [](float value) -> unsigned char {
                value = std::clamp(value, 0.0f, 1.0f);
                return static_cast<unsigned char>(value * 255.0f + 0.5f);
            };

            row[x] = (0xffu << 24) |
                     (static_cast<unsigned int>(to_channel(r)) << 16) |
                     (static_cast<unsigned int>(to_channel(g)) << 8) |
                     static_cast<unsigned int>(to_channel(b));
        }
    }

    app.texture->UnlockRect(0);
}

void render_frame(AppState& app) {
    const float time_sec = static_cast<float>(app.frame) / 60.0f;
    if (app.frame == 0) {
        trace_log("OK: frame0 start");
    }

    update_texture(app, time_sec);
    if (app.quit) {
        return;
    }
    set_scene_transforms(app, time_sec);
    set_managed_lights(app, time_sec);

    app.device->SetVertexShader(nullptr);
    app.device->SetPixelShader(nullptr);
    app.device->SetTexture(0, app.texture);
    app.device->SetFVF(kManagedLightsFvf);
    app.device->SetStreamSource(0, app.vertexBuffer, 0, sizeof(ManagedVertex));

    app.device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(18, 24, 30), 1.0f, 0);
    if (FAILED(app.device->BeginScene())) {
        std::fprintf(stderr, "FAIL: BeginScene\n");
        app.quit = true;
        return;
    }

    const HRESULT hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive", hr);
        app.quit = true;
    }

    if (app.frame == 0 && !app.quit) {
        trace_log("OK: frame0 submitted fixed-function draw");
    }

    maybe_capture_backbuffer(app, app.frame + 1);
    app.device->EndScene();
    if (FAILED(app.device->Present(nullptr, nullptr, nullptr, nullptr))) {
        std::fprintf(stderr, "FAIL: Present\n");
        app.quit = true;
        return;
    }

    ++app.frame;
    if ((app.frame % 60) == 0) {
        trace_log("OK: rendered frame %d", app.frame);
    }
    if (app.frame >= kExitFrame) {
        app.quit = true;
    }
}

void cleanup(AppState& app) {
    safe_release_t(app.vertexBuffer);
    safe_release_t(app.texture);
    safe_release_t(app.device);
    safe_release_t(app.d3d);
    if (app.hwnd) {
        DestroyWindow(app.hwnd);
        app.hwnd = nullptr;
    }
}

}  // namespace


int main() {
    AppState app{};
    init_capture(app);
    char trace_path[MAX_PATH]{};
    if (asset_path("20.ManagedLights.trace.txt", trace_path, sizeof(trace_path))) {
        DeleteFileA(trace_path);
        g_trace = std::fopen(trace_path, "wb");
    }

    trace_log("OK: Irrlicht ManagedLights startup");
    if (!create_window(app)) {
        return 1;
    }
    trace_log("OK: window created");
    if (!create_device(app)) {
        cleanup(app);
        return 1;
    }
    if (!create_scene_resources(app)) {
        cleanup(app);
        return 1;
    }
    trace_log("OK: fixed-function resources created");

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

    trace_log("OK: Irrlicht ManagedLights finished at frame %d", app.frame);
    cleanup(app);
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    return app.frame >= 150 ? 0 : 1;
}
