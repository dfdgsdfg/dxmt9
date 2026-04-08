#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kExitFrame = 180;
constexpr char kWindowClass[] = "dxmt9_tutorial07_window";
constexpr char kWindowTitle[] = "Tutorial 07";

FILE* g_trace = nullptr;

struct AppState {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader = nullptr;
    ID3DXConstantTable* vertexConstants = nullptr;
    ID3DXConstantTable* pixelConstants = nullptr;
    IDirect3DVertexDeclaration9* vertexDecl = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DTexture9* texture = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    int frame = 0;
    bool quit = false;
    int captureFrame = -1;
    bool captureDone = false;
    char capturePath[MAX_PATH]{};
};

struct FullscreenVertex {
    float x;
    float y;
    float z;
    float w;
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
    const char* capture_path = std::getenv("DXMT9_EXPERIMENT_CAPTURE_PATH");
    const char* capture_frame = std::getenv("DXMT9_EXPERIMENT_CAPTURE_FRAME");
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

bool create_orbit_texture(IDirect3DDevice9* device, IDirect3DTexture9** out_texture) {
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
            const float radial = cx * cx + cy * cy;
            const float band = std::fmod((u * 17.0f + v * 11.0f), 1.0f);
            const unsigned char r = static_cast<unsigned char>(70.0f + 150.0f * (1.0f - radial));
            const unsigned char g = static_cast<unsigned char>(45.0f + 165.0f * band);
            const unsigned char b = static_cast<unsigned char>(120.0f + 100.0f * (1.0f - band));
            row[x] = 0xff000000u | (static_cast<unsigned int>(r) << 16) |
                     (static_cast<unsigned int>(g) << 8) | static_cast<unsigned int>(b);
        }
    }

    texture->UnlockRect(0);
    *out_texture = texture;
    return true;
}

bool create_fullscreen_triangle(AppState& app) {
    static const D3DVERTEXELEMENT9 kVertexDecl[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const FullscreenVertex kVertices[6] = {
        {-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f},
        { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 0.0f},
    };

    HRESULT hr = app.device->CreateVertexDeclaration(kVertexDecl, &app.vertexDecl);
    if (FAILED(hr)) {
        print_hresult("CreateVertexDeclaration", hr);
        return false;
    }

    hr = app.device->CreateVertexBuffer(sizeof(kVertices), 0, 0, D3DPOOL_MANAGED, &app.vertexBuffer, nullptr);
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
    if (!asset_path("Tutorial07.hlsl", shader_path, sizeof(shader_path))) {
        std::fprintf(stderr, "FAIL: unable to resolve Tutorial07.hlsl path\n");
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
    if (!compile_shader("Tutorial07VS", "vs_2_0", &vs_bytecode, &app.vertexConstants)) {
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
    if (!compile_shader("Tutorial07PS", "ps_2_0", &ps_bytecode, &app.pixelConstants)) {
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

    if (!create_orbit_texture(app.device, &app.texture)) {
        return false;
    }
    trace_log("OK: orbit texture created");

    if (!create_fullscreen_triangle(app)) {
        return false;
    }
    trace_log("OK: fullscreen triangle created");

    app.device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    app.device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    app.device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    app.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    return true;
}

void render_frame(AppState& app) {
    const float time_sec = static_cast<float>(app.frame) / 60.0f;
    if (app.frame == 0) {
        trace_log("OK: frame0 start");
    }

    D3DXVECTOR4 tint_a(0.18f, 0.42f, 0.88f, 1.0f);
    D3DXVECTOR4 tint_b(0.95f, 0.72f, 0.24f, 1.0f);
    D3DXMATRIX identity;
    D3DXMatrixIdentity(&identity);

    app.device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                      D3DCOLOR_XRGB(10, 16, 26), 1.0f, 0);

    if (FAILED(app.device->BeginScene())) {
        std::fprintf(stderr, "FAIL: BeginScene\n");
        app.quit = true;
        return;
    }
    if (app.frame == 0) {
        trace_log("OK: frame0 scene begun");
    }

    app.device->SetVertexShader(app.vertexShader);
    app.device->SetPixelShader(app.pixelShader);
    app.device->SetVertexDeclaration(app.vertexDecl);
    app.device->SetStreamSource(0, app.vertexBuffer, 0, sizeof(FullscreenVertex));
    app.device->SetTexture(0, app.texture);

    if (app.vertexConstants) {
        app.vertexConstants->SetMatrix(app.device, "g_mWorldViewProjection", &identity);
        app.vertexConstants->SetVector(app.device, "g_TintA", &tint_a);
        app.vertexConstants->SetVector(app.device, "g_TintB", &tint_b);
        app.vertexConstants->SetFloat(app.device, "g_fTime", time_sec);
    }
    if (app.pixelConstants) {
        app.pixelConstants->SetVector(app.device, "g_TintA", &tint_a);
        app.pixelConstants->SetVector(app.device, "g_TintB", &tint_b);
        app.pixelConstants->SetFloat(app.device, "g_fTime", time_sec);
    }

    HRESULT hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive", hr);
        app.quit = true;
    }
    if (app.frame == 0 && !app.quit) {
        trace_log("OK: frame0 drew orbit pass");
    }

    app.device->EndScene();
    maybe_capture_backbuffer(app, app.frame + 1);
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
    safe_release_t(app.texture);
    safe_release_t(app.vertexBuffer);
    safe_release_t(app.vertexDecl);
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
    init_capture(app);
    char trace_path[MAX_PATH]{};
    if (asset_path("Tutorial07.trace.txt", trace_path, sizeof(trace_path))) {
        DeleteFileA(trace_path);
        g_trace = std::fopen(trace_path, "wb");
    }

    trace_log("OK: Tutorial07 startup");
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

    trace_log("OK: Tutorial07 device created");

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

    trace_log("OK: Tutorial07 finished at frame %d", app.frame);
    cleanup(app);
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    return app.frame >= 150 ? 0 : 1;
}
