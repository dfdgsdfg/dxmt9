#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kExitFrame = 180;
constexpr char kWindowClass[] = "dxmt9_multitextureterrain_window";
constexpr char kWindowTitle[] = "MultiTextureTerrain";

struct AppState {
    HWND hwnd = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader = nullptr;
    IDirect3DVertexDeclaration9* vertexDecl = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DTexture9* grassTexture = nullptr;
    IDirect3DTexture9* rockTexture = nullptr;
    IDirect3DTexture9* blendTexture = nullptr;
    int frame = 0;
    bool quit = false;
    int captureFrame = -1;
    bool captureDone = false;
    char capturePath[MAX_PATH]{};
};

struct TerrainVertex {
    float x, y, z, w;
    DWORD color;
    float u0, v0;
};

template <typename T>
void safe_release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void print_hresult(const char* label, HRESULT hr) {
    std::fprintf(stderr, "FAIL: %s hr=0x%08lx\n", label, static_cast<unsigned long>(hr));
}

bool exe_directory(char* buffer, size_t buffer_size) {
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
    const int written = std::snprintf(buffer, buffer_size, "%s\\%s", dir, name);
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
    hr = app.device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateOffscreenPlainSurface(capture)", hr);
        safe_release(backbuffer);
        return;
    }

    hr = app.device->GetRenderTargetData(backbuffer, staging);
    if (FAILED(hr)) {
        print_hresult("GetRenderTargetData(capture)", hr);
        safe_release(staging);
        safe_release(backbuffer);
        return;
    }

    hr = D3DXSaveSurfaceToFileA(app.capturePath, D3DXIFF_BMP, staging, nullptr, nullptr);
    if (FAILED(hr)) {
        print_hresult("D3DXSaveSurfaceToFileA(capture)", hr);
    } else {
        std::printf("OK: captured frame %d -> %s\n", completed_frame, app.capturePath);
        std::fflush(stdout);
        app.captureDone = true;
    }

    safe_release(staging);
    safe_release(backbuffer);
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
        return false;
    }

    RECT rect{0, 0, kWidth, kHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    app.hwnd = CreateWindowExA(
        0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!app.hwnd) {
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

    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = app.hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = app.d3d->CreateDevice(0, D3DDEVTYPE_HAL, app.hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &app.device);
    if (FAILED(hr)) {
        hr = app.d3d->CreateDevice(0, D3DDEVTYPE_HAL, app.hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &app.device);
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

bool compile_shader_from_file(const char* shader_path, const char* entry, const char* profile, ID3DXBuffer** bytecode) {
    constexpr DWORD kCompileFlags = D3DXSHADER_NO_PRESHADER | D3DXSHADER_SKIPOPTIMIZATION;
    ID3DXBuffer* errors = nullptr;
    HRESULT hr = D3DXCompileShaderFromFileA(shader_path, nullptr, nullptr, entry, profile, kCompileFlags, bytecode, &errors, nullptr);
    if (FAILED(hr)) {
        if (errors) {
            std::fprintf(stderr, "FAIL: D3DXCompileShaderFromFileA(%s/%s) %s\n",
                         profile, entry, static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            print_hresult("D3DXCompileShaderFromFileA", hr);
        }
        safe_release(errors);
        return false;
    }
    safe_release(errors);
    return true;
}

bool create_texture(IDirect3DDevice9* device, UINT size, IDirect3DTexture9** out_texture, DWORD (*fill)(UINT, UINT, UINT)) {
    IDirect3DTexture9* texture = nullptr;
    HRESULT hr = device->CreateTexture(size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
    if (FAILED(hr)) {
        print_hresult("CreateTexture", hr);
        return false;
    }

    D3DLOCKED_RECT locked{};
    hr = texture->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr)) {
        print_hresult("Texture::LockRect", hr);
        safe_release(texture);
        return false;
    }

    for (UINT y = 0; y < size; ++y) {
        auto* row = reinterpret_cast<DWORD*>(static_cast<unsigned char*>(locked.pBits) + y * locked.Pitch);
        for (UINT x = 0; x < size; ++x) {
            row[x] = fill(x, y, size);
        }
    }
    texture->UnlockRect(0);
    *out_texture = texture;
    return true;
}

DWORD make_grass(UINT x, UINT y, UINT size) {
    const float u = static_cast<float>(x) / static_cast<float>(size);
    const float v = static_cast<float>(y) / static_cast<float>(size);
    const float band = 0.5f + 0.5f * std::sin((u * 3.0f + v * 2.0f) * 3.1415926f);
    const unsigned r = static_cast<unsigned>(52 + 18 * band);
    const unsigned g = static_cast<unsigned>(120 + 42 * (0.35f + 0.65f * band));
    const unsigned b = static_cast<unsigned>(34 + 16 * (1.0f - band * 0.6f));
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

DWORD make_rock(UINT x, UINT y, UINT size) {
    const float u = static_cast<float>(x) / static_cast<float>(size);
    const float v = static_cast<float>(y) / static_cast<float>(size);
    const float ridge = 0.5f + 0.5f * std::sin((u * 2.4f - v * 1.8f) * 3.1415926f);
    const unsigned shade = static_cast<unsigned>(82 + 40 * ridge);
    const unsigned r = shade;
    const unsigned g = static_cast<unsigned>(shade * 0.92f);
    const unsigned b = static_cast<unsigned>(shade * 0.84f);
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

DWORD make_blend(UINT x, UINT y, UINT size) {
    const float u = static_cast<float>(x) / static_cast<float>(size);
    const float v = static_cast<float>(y) / static_cast<float>(size);
    float ridge = 0.42f + 0.18f * std::sin(u * 5.0f) + 0.08f * std::sin(u * 11.0f);
    float mask = v > ridge ? 0.80f : 0.18f;
    mask += 0.08f * std::sin((u + v) * 8.0f);
    if (mask < 0.0f) mask = 0.0f;
    if (mask > 1.0f) mask = 1.0f;
    const unsigned c = static_cast<unsigned>(mask * 255.0f);
    return 0xff000000u | (c << 16) | (c << 8) | c;
}

bool create_geometry(AppState& app) {
    static const D3DVERTEXELEMENT9 kVertexDecl[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };

    constexpr DWORD kWhite = 0xffffffffu;
    static const TerrainVertex kVertices[6] = {
        {-1.0f, -1.0f, 0.0f, 1.0f, kWhite, 0.0f, 1.0f},
        {-1.0f,  0.10f, 0.0f, 1.0f, kWhite, 0.0f, 0.0f},
        { 1.0f, -1.0f, 0.0f, 1.0f, kWhite, 1.0f, 1.0f},
        { 1.0f, -1.0f, 0.0f, 1.0f, kWhite, 1.0f, 1.0f},
        {-1.0f,  0.10f, 0.0f, 1.0f, kWhite, 0.0f, 0.0f},
        { 1.0f,  0.10f, 0.0f, 1.0f, kWhite, 1.0f, 0.0f},
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

bool create_scene_resources(AppState& app) {
    char shader_path[MAX_PATH]{};
    if (!asset_path("MultiTextureTerrain.hlsl", shader_path, sizeof(shader_path))) {
        return false;
    }

    ID3DXBuffer* vs = nullptr;
    if (!compile_shader_from_file(shader_path, "MultiTextureTerrainVS", "vs_2_0", &vs)) {
        return false;
    }
    HRESULT hr = app.device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()), &app.vertexShader);
    safe_release(vs);
    if (FAILED(hr)) {
        print_hresult("CreateVertexShader", hr);
        return false;
    }

    ID3DXBuffer* ps = nullptr;
    if (!compile_shader_from_file(shader_path, "MultiTextureTerrainPS", "ps_2_0", &ps)) {
        return false;
    }
    hr = app.device->CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()), &app.pixelShader);
    safe_release(ps);
    if (FAILED(hr)) {
        print_hresult("CreatePixelShader", hr);
        return false;
    }

    if (!create_texture(app.device, 512, &app.grassTexture, make_grass) ||
        !create_texture(app.device, 512, &app.rockTexture, make_rock) ||
        !create_texture(app.device, 256, &app.blendTexture, make_blend) ||
        !create_geometry(app)) {
        return false;
    }

    for (DWORD sampler = 0; sampler < 3; ++sampler) {
        app.device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        app.device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        app.device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        app.device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        app.device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    return true;
}

void render_frame(AppState& app) {
    const D3DCOLOR sky = D3DCOLOR_XRGB(122, 156, 194);
    app.device->Clear(0, nullptr, D3DCLEAR_TARGET, sky, 1.0f, 0);
    if (FAILED(app.device->BeginScene())) {
        app.quit = true;
        return;
    }

    app.device->SetVertexDeclaration(app.vertexDecl);
    app.device->SetStreamSource(0, app.vertexBuffer, 0, sizeof(TerrainVertex));
    app.device->SetVertexShader(app.vertexShader);
    app.device->SetPixelShader(app.pixelShader);
    app.device->SetTexture(0, app.grassTexture);
    app.device->SetTexture(1, app.rockTexture);
    app.device->SetTexture(2, app.blendTexture);
    app.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    const HRESULT hr = app.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    if (FAILED(hr)) {
        print_hresult("DrawPrimitive", hr);
        app.quit = true;
    }

    app.device->EndScene();
    maybe_capture_backbuffer(app, app.frame + 1);
    if (FAILED(app.device->Present(nullptr, nullptr, nullptr, nullptr))) {
        app.quit = true;
        return;
    }

    ++app.frame;
    if ((app.frame % 60) == 0) {
        std::printf("OK: rendered frame %d\n", app.frame);
        std::fflush(stdout);
    }
    if (app.frame >= kExitFrame) {
        app.quit = true;
    }
}

void cleanup(AppState& app) {
    safe_release(app.blendTexture);
    safe_release(app.rockTexture);
    safe_release(app.grassTexture);
    safe_release(app.vertexBuffer);
    safe_release(app.vertexDecl);
    safe_release(app.pixelShader);
    safe_release(app.vertexShader);
    safe_release(app.device);
    safe_release(app.d3d);
    if (app.hwnd) {
        DestroyWindow(app.hwnd);
        app.hwnd = nullptr;
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    AppState app{};
    init_capture(app);

    if (!create_window(app) || !create_device(app) || !create_scene_resources(app)) {
        cleanup(app);
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
        if (!app.quit) {
            render_frame(app);
        }
    }

    cleanup(app);
    return 0;
}
