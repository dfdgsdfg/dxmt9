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
constexpr int kSceneTextureSize = 1024;
constexpr int kExitFrame = 180;
constexpr char kWindowClass[] = "dxmt9_waterrt_window";
constexpr char kWindowTitle[] = "WaterRT";

FILE* g_trace = nullptr;

struct AppState {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* scenePixelShader = nullptr;
    IDirect3DPixelShader9* copyPixelShader = nullptr;
    IDirect3DPixelShader9* waterPixelShader = nullptr;
    ID3DXConstantTable* scenePixelConstants = nullptr;
    ID3DXConstantTable* waterPixelConstants = nullptr;
    IDirect3DVertexDeclaration9* vertexDecl = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DTexture9* sourceTexture = nullptr;
    IDirect3DTexture9* sceneTexture = nullptr;
    IDirect3DSurface9* sceneSurface = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    int frame = 0;
    bool quit = false;
    int captureFrame = -1;
    bool captureDone = false;
    bool disableProjective = false;
    bool forceOpaqueWater = false;
    bool forceSolidWater = false;
    char capturePath[MAX_PATH]{};
};

struct QuadVertex {
    float x;
    float y;
    float z;
    float w;
    DWORD color;
    float tx;
    float ty;
    float tz;
    float tw;
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

bool env_flag(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return false;
    }
    return std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0;
}

void init_runtime_options(AppState& app) {
    app.disableProjective = env_flag("WATERRT_DISABLE_PROJECTIVE");
    app.forceOpaqueWater = env_flag("WATERRT_FORCE_OPAQUE");
    app.forceSolidWater = env_flag("WATERRT_FORCE_SOLID_WATER");
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

float clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

bool create_harbor_texture(IDirect3DDevice9* device, IDirect3DTexture9** out_texture) {
    if (!device || !out_texture) {
        return false;
    }

    constexpr UINT kTextureSize = 512;
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
        print_hresult("CreateTexture(source)", hr);
        return false;
    }

    D3DLOCKED_RECT locked{};
    hr = texture->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr)) {
        print_hresult("Texture::LockRect(source)", hr);
        texture->Release();
        return false;
    }

    for (UINT y = 0; y < kTextureSize; ++y) {
        auto* row = reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(locked.pBits) + y * locked.Pitch);
        for (UINT x = 0; x < kTextureSize; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kTextureSize);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kTextureSize);

            float r = 0.82f - 0.46f * v;
            float g = 0.85f - 0.42f * v;
            float b = 0.90f - 0.32f * v;

            const float cloud = 0.08f * std::sin(u * 24.0f) * std::sin(v * 13.0f);
            r += cloud;
            g += cloud;
            b += cloud * 1.2f;

            const float ridge = 0.46f + 0.08f * std::sin(u * 6.5f) + 0.05f * std::sin(u * 14.0f);
            if (v > ridge) {
                float shade = 0.12f + 0.10f * std::sin(u * 42.0f);
                const float roofBand = std::fabs(std::sin(u * 31.0f + v * 14.0f));
                shade += 0.08f * roofBand;
                r = 0.16f + shade * 0.55f;
                g = 0.13f + shade * 0.42f;
                b = 0.10f + shade * 0.28f;
            }

            const float mast = std::fabs(u - 0.18f);
            if (mast < 0.003f && v > 0.40f) {
                r = 0.10f;
                g = 0.08f;
                b = 0.05f;
            }
            const float sailLeft = (u - 0.19f) * 2.6f + 0.48f;
            const float sailRight = 0.67f - (u - 0.19f) * 1.8f;
            if (u > 0.19f && u < 0.28f && v > sailLeft && v < sailRight) {
                r = 0.88f;
                g = 0.79f;
                b = 0.65f;
            }

            const unsigned char cr = static_cast<unsigned char>(clamp01(r) * 255.0f);
            const unsigned char cg = static_cast<unsigned char>(clamp01(g) * 255.0f);
            const unsigned char cb = static_cast<unsigned char>(clamp01(b) * 255.0f);
            row[x] = 0xff000000u |
                     (static_cast<unsigned int>(cr) << 16) |
                     (static_cast<unsigned int>(cg) << 8) |
                     static_cast<unsigned int>(cb);
        }
    }

    texture->UnlockRect(0);
    *out_texture = texture;
    return true;
}

bool create_geometry(AppState& app) {
    static const D3DVERTEXELEMENT9 kVertexDecl[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };

    constexpr DWORD kWhite = 0xffffffffu;
    constexpr float kWaterTop = -0.12f;
    static const QuadVertex kVertices[12] = {
        {-1.0f, -1.0f, 0.0f, 1.0f, kWhite, 0.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 1.0f, kWhite, 0.0f, 0.0f, 0.0f, 1.0f},
        { 1.0f, -1.0f, 0.0f, 1.0f, kWhite, 1.0f, 1.0f, 1.0f, 0.0f},
        { 1.0f, -1.0f, 0.0f, 1.0f, kWhite, 1.0f, 1.0f, 1.0f, 0.0f},
        {-1.0f,  1.0f, 0.0f, 1.0f, kWhite, 0.0f, 0.0f, 0.0f, 1.0f},
        { 1.0f,  1.0f, 0.0f, 1.0f, kWhite, 1.0f, 0.0f, 1.0f, 0.0f},

        {-1.0f, -1.0f,      0.0f, 1.0f, kWhite, 0.0f, 1.0f, 0.04f, 0.16f},
        {-1.0f,  kWaterTop, 0.0f, 1.0f, kWhite, 0.0f, 0.0f, 0.04f, 0.54f},
        { 1.0f, -1.0f,      0.0f, 1.0f, kWhite, 1.0f, 1.0f, 0.96f, 0.16f},
        { 1.0f, -1.0f,      0.0f, 1.0f, kWhite, 1.0f, 1.0f, 0.96f, 0.16f},
        {-1.0f,  kWaterTop, 0.0f, 1.0f, kWhite, 0.0f, 0.0f, 0.04f, 0.54f},
        { 1.0f,  kWaterTop, 0.0f, 1.0f, kWhite, 1.0f, 0.0f, 0.96f, 0.54f},
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
    return true;
}

bool compile_shader_from_file(
    const char* shader_path,
    const char* entry,
    const char* profile,
    ID3DXBuffer** bytecode,
    ID3DXConstantTable** constants) {
    constexpr DWORD kCompileFlags = D3DXSHADER_NO_PRESHADER | D3DXSHADER_SKIPOPTIMIZATION;
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
}

bool create_scene_resources(AppState& app) {
    char shader_path[MAX_PATH]{};
    if (!asset_path("WaterRT.hlsl", shader_path, sizeof(shader_path))) {
        std::fprintf(stderr, "FAIL: unable to resolve WaterRT.hlsl path\n");
        return false;
    }
    trace_log("OK: using shader %s", shader_path);

    ID3DXBuffer* vs_bytecode = nullptr;
    if (!compile_shader_from_file(shader_path, "WaterRTVS", "vs_2_0", &vs_bytecode, nullptr)) {
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

    ID3DXBuffer* scene_ps = nullptr;
    if (!compile_shader_from_file(shader_path, "ScenePS", "ps_2_0", &scene_ps, &app.scenePixelConstants)) {
        return false;
    }
    hr = app.device->CreatePixelShader(
        static_cast<const DWORD*>(scene_ps->GetBufferPointer()),
        &app.scenePixelShader);
    safe_release_t(scene_ps);
    if (FAILED(hr)) {
        print_hresult("CreatePixelShader(scene)", hr);
        return false;
    }

    ID3DXBuffer* copy_ps = nullptr;
    if (!compile_shader_from_file(shader_path, "CopyPS", "ps_2_0", &copy_ps, nullptr)) {
        return false;
    }
    hr = app.device->CreatePixelShader(
        static_cast<const DWORD*>(copy_ps->GetBufferPointer()),
        &app.copyPixelShader);
    safe_release_t(copy_ps);
    if (FAILED(hr)) {
        print_hresult("CreatePixelShader(copy)", hr);
        return false;
    }

    const char* water_entry = "WaterPS";
    if (app.forceSolidWater) {
        water_entry = "WaterPSSolid";
    } else if (app.disableProjective) {
        water_entry = "WaterPSNoProj";
    }
    ID3DXBuffer* water_ps = nullptr;
    if (!compile_shader_from_file(shader_path, water_entry, "ps_2_0", &water_ps, &app.waterPixelConstants)) {
        return false;
    }
    hr = app.device->CreatePixelShader(
        static_cast<const DWORD*>(water_ps->GetBufferPointer()),
        &app.waterPixelShader);
    safe_release_t(water_ps);
    if (FAILED(hr)) {
        print_hresult("CreatePixelShader(water)", hr);
        return false;
    }
    trace_log("OK: water shader entry %s", water_entry);

    if (!create_harbor_texture(app.device, &app.sourceTexture)) {
        return false;
    }

    hr = app.device->CreateTexture(
        kSceneTextureSize,
        kSceneTextureSize,
        1,
        D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &app.sceneTexture,
        nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateTexture(sceneRT)", hr);
        return false;
    }

    hr = app.sceneTexture->GetSurfaceLevel(0, &app.sceneSurface);
    if (FAILED(hr)) {
        print_hresult("GetSurfaceLevel(sceneRT)", hr);
        return false;
    }

    hr = app.device->GetRenderTarget(0, &app.backBuffer);
    if (FAILED(hr)) {
        print_hresult("GetRenderTarget(backBuffer)", hr);
        return false;
    }

    if (!create_geometry(app)) {
        return false;
    }

    for (DWORD sampler = 0; sampler < 1; ++sampler) {
        app.device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        app.device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        app.device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        app.device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        app.device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }

    trace_log("OK: water sample scene resources created");
    return true;
}

void set_viewport(IDirect3DDevice9* device, UINT width, UINT height) {
    D3DVIEWPORT9 vp{};
    vp.X = 0;
    vp.Y = 0;
    vp.Width = width;
    vp.Height = height;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    device->SetViewport(&vp);
}

void render_scene_texture(AppState& app, float time_sec) {
    (void) time_sec;
    const D3DXVECTOR4 scene_water_tint(0.12f, 0.36f, 0.70f, 0.58f);
    app.device->SetRenderTarget(0, app.sceneSurface);
    set_viewport(app.device, kSceneTextureSize, kSceneTextureSize);
    app.device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(208, 216, 228), 1.0f, 0);

    if (FAILED(app.device->BeginScene())) {
        std::fprintf(stderr, "FAIL: BeginScene(scene)\n");
        app.quit = true;
        return;
    }

    app.device->SetVertexDeclaration(app.vertexDecl);
    app.device->SetStreamSource(0, app.vertexBuffer, 0, sizeof(QuadVertex));
    app.device->SetVertexShader(app.vertexShader);
    app.device->SetPixelShader(app.scenePixelShader);
    app.device->SetTexture(0, app.sourceTexture);

    if (app.scenePixelConstants) {
        app.scenePixelConstants->SetFloat(app.device, "g_WaveScale", 1.0f);
        app.scenePixelConstants->SetVector(app.device, "g_WaterTint", &scene_water_tint);
    }

    HRESULT hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive(scene)", hr);
        app.quit = true;
    }

    app.device->EndScene();
}

void render_frame(AppState& app) {
    const float time_sec = static_cast<float>(app.frame) / 60.0f;
    const D3DXVECTOR4 water_tint(0.14f, 0.34f, 0.62f, 0.54f);
    if (app.frame == 0) {
        trace_log("OK: frame0 start");
    }

    render_scene_texture(app, time_sec);
    if (app.quit) {
        return;
    }

    app.device->SetRenderTarget(0, app.backBuffer);
    set_viewport(app.device, kWidth, kHeight);
    app.device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(6, 10, 18), 1.0f, 0);

    if (FAILED(app.device->BeginScene())) {
        std::fprintf(stderr, "FAIL: BeginScene(backbuffer)\n");
        app.quit = true;
        return;
    }

    app.device->SetVertexDeclaration(app.vertexDecl);
    app.device->SetStreamSource(0, app.vertexBuffer, 0, sizeof(QuadVertex));
    app.device->SetVertexShader(app.vertexShader);
    app.device->SetTexture(0, app.sceneTexture);

    app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    app.device->SetPixelShader(app.copyPixelShader);
    HRESULT hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive(copy)", hr);
        app.quit = true;
    }

    app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, (app.forceOpaqueWater || app.forceSolidWater) ? FALSE : TRUE);
    app.device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    app.device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    app.device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    app.device->SetPixelShader(app.waterPixelShader);
    if (app.waterPixelConstants) {
        app.waterPixelConstants->SetFloat(app.device, "g_WaveScale", 1.0f);
        app.waterPixelConstants->SetVector(app.device, "g_WaterTint", &water_tint);
    }
    hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 6, 2);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive(water)", hr);
        app.quit = true;
    }

    app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
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
    safe_release_t(app.backBuffer);
    safe_release_t(app.sceneSurface);
    safe_release_t(app.sceneTexture);
    safe_release_t(app.sourceTexture);
    safe_release_t(app.vertexBuffer);
    safe_release_t(app.vertexDecl);
    safe_release_t(app.waterPixelConstants);
    safe_release_t(app.scenePixelConstants);
    safe_release_t(app.waterPixelShader);
    safe_release_t(app.copyPixelShader);
    safe_release_t(app.scenePixelShader);
    safe_release_t(app.vertexShader);
    safe_release_t(app.device);
    safe_release_t(app.d3d);
    if (app.hwnd) {
        DestroyWindow(app.hwnd);
        app.hwnd = nullptr;
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    AppState app{};
    g_trace = nullptr;

    char trace_path[MAX_PATH]{};
    if (asset_path("WaterRT.trace.txt", trace_path, sizeof(trace_path))) {
        g_trace = std::fopen(trace_path, "w");
    }

    init_capture(app);
    init_runtime_options(app);

    if (!create_window(app) || !create_device(app) || !create_scene_resources(app)) {
        cleanup(app);
        if (g_trace) {
            std::fclose(g_trace);
        }
        return 1;
    }

    MSG msg{};
    while (!app.quit) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                app.quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (app.quit) {
            break;
        }
        render_frame(app);
    }

    cleanup(app);
    if (g_trace) {
        std::fclose(g_trace);
    }
    return 0;
}
