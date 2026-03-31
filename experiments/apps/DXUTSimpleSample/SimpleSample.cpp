#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kExitFrame = 180;
constexpr char kWindowClass[] = "dxmt9_simple_sample_window";
constexpr char kWindowTitle[] = "SimpleSample";

FILE* g_trace = nullptr;

struct AppState {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader = nullptr;
    ID3DXConstantTable* vertexConstants = nullptr;
    ID3DXConstantTable* pixelConstants = nullptr;
    IDirect3DTexture9* baseTexture = nullptr;
    IDirect3DTexture9* overlayTexture = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    int frame = 0;
    bool quit = false;
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

bool create_texture_pattern(IDirect3DDevice9* device, IDirect3DTexture9** out_texture, bool overlay) {
    if (!device || !out_texture) {
        return false;
    }

    constexpr UINT kTextureSize = 256;
    IDirect3DTexture9* texture = nullptr;
    HRESULT hr = device->CreateTexture(
        kTextureSize,
        kTextureSize,
        1,
        0,
        D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,
        &texture,
        nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateTexture", hr);
        return false;
    }

    D3DLOCKED_RECT locked{};
    hr = texture->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr)) {
        print_hresult("Texture::LockRect", hr);
        texture->Release();
        return false;
    }

    for (UINT y = 0; y < kTextureSize; ++y) {
        auto* row = reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(locked.pBits) + y * locked.Pitch);
        for (UINT x = 0; x < kTextureSize; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kTextureSize);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kTextureSize);
            const float cx = u - 0.5f;
            const float cy = v - 0.5f;
            const float radial = std::sqrt(cx * cx + cy * cy);
            const float stripe = 0.5f + 0.5f * std::sin((u * 18.0f + v * 11.0f) * 6.28318f);

            unsigned char a = 255;
            unsigned char r = 0;
            unsigned char g = 0;
            unsigned char b = 0;
            if (!overlay) {
                r = static_cast<unsigned char>(35.0f + 170.0f * (1.0f - radial));
                g = static_cast<unsigned char>(70.0f + 140.0f * stripe);
                b = static_cast<unsigned char>(120.0f + 95.0f * (1.0f - stripe));
            } else {
                const float glow = radial < 0.46f ? (1.0f - radial / 0.46f) : 0.0f;
                a = static_cast<unsigned char>(220.0f * glow);
                r = static_cast<unsigned char>(180.0f + 60.0f * stripe);
                g = static_cast<unsigned char>(120.0f + 90.0f * glow);
                b = static_cast<unsigned char>(40.0f + 40.0f * stripe);
            }
            row[x] = (static_cast<unsigned int>(a) << 24) |
                     (static_cast<unsigned int>(r) << 16) |
                     (static_cast<unsigned int>(g) << 8) |
                     static_cast<unsigned int>(b);
        }
    }

    texture->UnlockRect(0);
    *out_texture = texture;
    return true;
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
    app.pp.EnableAutoDepthStencil = TRUE;
    app.pp.AutoDepthStencilFormat = D3DFMT_D24S8;
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

    app.device->SetRenderState(D3DRS_ZENABLE, TRUE);
    app.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    app.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    return true;
}

bool create_scene_resources(AppState& app) {
    char shader_path[MAX_PATH]{};
    if (!asset_path("SimpleSample.hlsl", shader_path, sizeof(shader_path))) {
        std::fprintf(stderr, "FAIL: unable to resolve SimpleSample.hlsl path\n");
        return false;
    }
    trace_log("OK: using shader %s", shader_path);

    constexpr DWORD kCompileFlags = D3DXSHADER_NO_PRESHADER | D3DXSHADER_SKIPOPTIMIZATION;

    auto compile_shader = [&](const char* entry,
                              const char* profile,
                              ID3DXBuffer** bytecode,
                              ID3DXConstantTable** constants) -> bool {
        trace_log("OK: compiling %s %s", profile, entry);
        ID3DXBuffer* errors = nullptr;
        HRESULT hr = D3DXCompileShaderFromFileA(
            shader_path,
            nullptr,
            nullptr,
            entry,
            profile,
            kCompileFlags,
            bytecode,
            &errors,
            constants);
        if (FAILED(hr)) {
            if (errors) {
                std::fprintf(stderr, "FAIL: D3DXCompileShaderFromFileA(%s/%s) %s\n",
                             profile, entry, static_cast<const char*>(errors->GetBufferPointer()));
            } else {
                print_hresult("D3DXCompileShaderFromFileA", hr);
            }
            safe_release_t(errors);
            return false;
        }
        safe_release_t(errors);
        return true;
    };

    ID3DXBuffer* vs_bytecode = nullptr;
    if (!compile_shader("SimpleSampleVS", "vs_2_0", &vs_bytecode, &app.vertexConstants)) {
        return false;
    }
    HRESULT hr = app.device->CreateVertexShader(
        static_cast<const DWORD*>(vs_bytecode->GetBufferPointer()),
        &app.vertexShader);
    safe_release_t(vs_bytecode);
    if (FAILED(hr)) {
        print_hresult("CreateVertexShader", hr);
        return false;
    }

    ID3DXBuffer* ps_bytecode = nullptr;
    if (!compile_shader("SimpleSamplePS", "ps_2_0", &ps_bytecode, &app.pixelConstants)) {
        return false;
    }
    hr = app.device->CreatePixelShader(
        static_cast<const DWORD*>(ps_bytecode->GetBufferPointer()),
        &app.pixelShader);
    safe_release_t(ps_bytecode);
    if (FAILED(hr)) {
        print_hresult("CreatePixelShader", hr);
        return false;
    }
    trace_log("OK: shaders compiled");

    if (!create_texture_pattern(app.device, &app.baseTexture, false)) {
        return false;
    }
    if (!create_texture_pattern(app.device, &app.overlayTexture, true)) {
        return false;
    }
    trace_log("OK: textures created");

    app.device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    app.device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    return true;
}

void render_frame(AppState& app) {
    const float time_sec = static_cast<float>(app.frame) / 60.0f;
    if (app.frame == 0) {
        trace_log("OK: frame0 start");
    }

    D3DXVECTOR4 tint_a(0.12f, 0.56f, 0.92f, 1.0f);
    D3DXVECTOR4 tint_b(0.98f, 0.70f, 0.22f, 1.0f);
    D3DXVECTOR4 overlay_color(0.98f, 0.72f, 0.34f, 0.58f);
    D3DXMATRIX identity;
    D3DXMatrixIdentity(&identity);

    app.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                      D3DCOLOR_XRGB(8, 14, 24), 1.0f, 0);

    if (FAILED(app.device->BeginScene())) {
        std::fprintf(stderr, "FAIL: BeginScene\n");
        app.quit = true;
        return;
    }
    if (app.frame == 0) {
        trace_log("OK: frame0 scene begun");
    }

    app.device->SetVertexShader(app.vertexShader);
    if (app.vertexConstants) {
        app.vertexConstants->SetMatrix(app.device, "g_mWorldViewProjection", &identity);
        app.vertexConstants->SetVector(app.device, "g_TintA", &tint_a);
        app.vertexConstants->SetVector(app.device, "g_TintB", &tint_b);
        app.vertexConstants->SetFloat(app.device, "g_fTime", time_sec);
    }

    app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    app.device->SetTexture(0, app.baseTexture);
    app.device->SetPixelShader(app.pixelShader);
    if (app.pixelConstants) {
        app.pixelConstants->SetVector(app.device, "g_TintA", &tint_a);
        app.pixelConstants->SetVector(app.device, "g_TintB", &tint_b);
        app.pixelConstants->SetVector(app.device, "g_OverlayColor", &overlay_color);
        app.pixelConstants->SetFloat(app.device, "g_fTime", time_sec);
        app.pixelConstants->SetFloat(app.device, "g_PassMix", 0.0f);
        app.pixelConstants->SetFloat(app.device, "g_OverlayAlpha", 1.0f);
    }
    HRESULT hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive(base)", hr);
        app.quit = true;
    }

    app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    app.device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    app.device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    app.device->SetTexture(0, app.overlayTexture);
    app.device->SetPixelShader(app.pixelShader);
    if (app.pixelConstants) {
        app.pixelConstants->SetVector(app.device, "g_TintA", &tint_a);
        app.pixelConstants->SetVector(app.device, "g_TintB", &tint_b);
        app.pixelConstants->SetVector(app.device, "g_OverlayColor", &overlay_color);
        app.pixelConstants->SetFloat(app.device, "g_fTime", time_sec);
        app.pixelConstants->SetFloat(app.device, "g_PassMix", 1.0f);
        app.pixelConstants->SetFloat(app.device, "g_OverlayAlpha", overlay_color.w);
    }
    hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive(overlay)", hr);
        app.quit = true;
    }
    app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    if (app.frame == 0 && !app.quit) {
        trace_log("OK: frame0 drew two-pass scene");
    }

    app.device->EndScene();
    hr = app.device->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
        print_hresult("Present", hr);
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
    safe_release_t(app.overlayTexture);
    safe_release_t(app.baseTexture);
    safe_release_t(app.pixelConstants);
    safe_release_t(app.vertexConstants);
    safe_release_t(app.pixelShader);
    safe_release_t(app.vertexShader);
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
    char trace_path[MAX_PATH]{};
    if (asset_path("SimpleSample.trace.txt", trace_path, sizeof(trace_path))) {
        DeleteFileA(trace_path);
        g_trace = std::fopen(trace_path, "wb");
    }

    trace_log("OK: SimpleSample startup");
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
    trace_log("OK: SimpleSample device created");

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

    trace_log("OK: SimpleSample finished at frame %d", app.frame);
    cleanup(app);
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    return app.frame >= 150 ? 0 : 1;
}
