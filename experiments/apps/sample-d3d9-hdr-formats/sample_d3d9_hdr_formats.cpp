#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kExitFrame = 180;
constexpr char kWindowClass[] = "dxmt9_hdrformats_window";
constexpr char kWindowTitle[] = "HDRFormats";

FILE* g_trace = nullptr;

struct AppState {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* hdrPixelShader = nullptr;
    IDirect3DPixelShader9* tonemapPixelShader = nullptr;
    ID3DXConstantTable* vertexConstants = nullptr;
    ID3DXConstantTable* hdrPixelConstants = nullptr;
    ID3DXConstantTable* tonemapPixelConstants = nullptr;
    IDirect3DTexture9* hdrTexture = nullptr;
    IDirect3DSurface9* hdrSurface = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
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

    app.device->SetRenderState(D3DRS_ZENABLE, FALSE);
    app.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    app.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    return true;
}

bool create_scene_resources(AppState& app) {
    char shader_path[MAX_PATH]{};
    if (!asset_path("sample-d3d9-hdr-formats.hlsl", shader_path, sizeof(shader_path))) {
        std::fprintf(stderr, "FAIL: unable to resolve sample-d3d9-hdr-formats.hlsl path\n");
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
    if (!compile_shader("HDRFormatsVS", "vs_2_0", &vs_bytecode, &app.vertexConstants)) {
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
    if (!compile_shader("HDRScenePS", "ps_3_0", &ps_bytecode, &app.hdrPixelConstants)) {
        return false;
    }
    hr = app.device->CreatePixelShader(
        static_cast<const DWORD*>(ps_bytecode->GetBufferPointer()),
        &app.hdrPixelShader);
    safe_release_t(ps_bytecode);
    if (FAILED(hr)) {
        print_hresult("CreatePixelShader(HDRScenePS)", hr);
        return false;
    }

    if (!compile_shader("HDRToneMapPS", "ps_3_0", &ps_bytecode, &app.tonemapPixelConstants)) {
        return false;
    }
    hr = app.device->CreatePixelShader(
        static_cast<const DWORD*>(ps_bytecode->GetBufferPointer()),
        &app.tonemapPixelShader);
    safe_release_t(ps_bytecode);
    if (FAILED(hr)) {
        print_hresult("CreatePixelShader(HDRToneMapPS)", hr);
        return false;
    }
    trace_log("OK: shaders compiled");

    hr = app.device->CreateTexture(
        kWidth,
        kHeight,
        1,
        D3DUSAGE_RENDERTARGET,
        D3DFMT_A16B16G16R16F,
        D3DPOOL_DEFAULT,
        &app.hdrTexture,
        nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateTexture(A16B16G16R16F)", hr);
        return false;
    }

    hr = app.hdrTexture->GetSurfaceLevel(0, &app.hdrSurface);
    if (FAILED(hr)) {
        print_hresult("GetSurfaceLevel(hdr)", hr);
        return false;
    }

    hr = app.device->GetRenderTarget(0, &app.backBuffer);
    if (FAILED(hr)) {
        print_hresult("GetRenderTarget(backBuffer)", hr);
        return false;
    }
    trace_log("OK: fp16 render target created");

    app.device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    app.device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    app.device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    return true;
}

void render_frame(AppState& app) {
    const float time_sec = static_cast<float>(app.frame) / 60.0f;
    if (app.frame == 0) {
        trace_log("OK: frame0 start");
    }

    D3DXVECTOR4 hdr_color_a(2.8f, 1.2f, 0.4f, 1.0f);
    D3DXVECTOR4 hdr_color_b(0.3f, 1.8f, 3.4f, 1.0f);
    D3DXVECTOR4 tone_bias(0.08f, 0.04f, 0.02f, 1.0f);
    D3DXMATRIX identity;
    D3DXMatrixIdentity(&identity);

    app.device->SetRenderTarget(0, app.hdrSurface);
    app.device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (FAILED(app.device->BeginScene())) {
        std::fprintf(stderr, "FAIL: BeginScene(hdr)\n");
        app.quit = true;
        return;
    }

    app.device->SetVertexShader(app.vertexShader);
    app.device->SetPixelShader(app.hdrPixelShader);
    app.device->SetTexture(0, nullptr);
    if (app.vertexConstants) {
        app.vertexConstants->SetMatrix(app.device, "g_mWorldViewProjection", &identity);
        app.vertexConstants->SetFloat(app.device, "g_fTime", time_sec);
    }
    if (app.hdrPixelConstants) {
        app.hdrPixelConstants->SetVector(app.device, "g_HDRColorA", &hdr_color_a);
        app.hdrPixelConstants->SetVector(app.device, "g_HDRColorB", &hdr_color_b);
        app.hdrPixelConstants->SetFloat(app.device, "g_fTime", time_sec);
    }
    HRESULT hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive(hdr)", hr);
        app.quit = true;
    }
    app.device->EndScene();

    app.device->SetRenderTarget(0, app.backBuffer);
    app.device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(6, 8, 12), 1.0f, 0);
    if (FAILED(app.device->BeginScene())) {
        std::fprintf(stderr, "FAIL: BeginScene(tonemap)\n");
        app.quit = true;
        return;
    }

    app.device->SetVertexShader(app.vertexShader);
    app.device->SetPixelShader(app.tonemapPixelShader);
    app.device->SetTexture(0, app.hdrTexture);
    if (app.tonemapPixelConstants) {
        app.tonemapPixelConstants->SetVector(app.device, "g_ToneBias", &tone_bias);
        app.tonemapPixelConstants->SetFloat(app.device, "g_fTime", time_sec);
    }
    hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive(tonemap)", hr);
        app.quit = true;
    }
    if (app.frame == 0 && !app.quit) {
        trace_log("OK: frame0 rendered hdr + tonemap");
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
    safe_release_t(app.backBuffer);
    safe_release_t(app.hdrSurface);
    safe_release_t(app.hdrTexture);
    safe_release_t(app.tonemapPixelConstants);
    safe_release_t(app.hdrPixelConstants);
    safe_release_t(app.vertexConstants);
    safe_release_t(app.tonemapPixelShader);
    safe_release_t(app.hdrPixelShader);
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
    if (asset_path("HDRFormats.trace.txt", trace_path, sizeof(trace_path))) {
        DeleteFileA(trace_path);
        g_trace = std::fopen(trace_path, "wb");
    }

    trace_log("OK: HDRFormats startup");
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
    trace_log("OK: HDRFormats device created");

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

    trace_log("OK: HDRFormats finished at frame %d", app.frame);
    cleanup(app);
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    return app.frame >= 150 ? 0 : 1;
}
