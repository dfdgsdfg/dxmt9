/* tests/integration/wsi_present/main.cpp
 *
 * Minimal Win32 + D3D9 smoke test for the WSI path.
 * Cross-compile:
 *   PATH=~/llvm-mingw/bin:$PATH
 *   x86_64-w64-mingw32-clang++ -o build/wsi_present/wsi_present_x64.exe tests/integration/wsi_present/main.cpp -ld3d9 -luser32 -lgdi32
 * Run:
 *   wine64 build/wsi_present/wsi_present_x64.exe
 *
 * Expected: window appears, cycles red/green/blue for 3 seconds, exits 0.
 * Any crash or blank black window (no colour) indicates a WSI failure.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int main() {
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "dxmt9_wsi_test";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, "dxmt9_wsi_test", "dxmt9 WSI test",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 640, 480,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        fprintf(stderr, "FAIL: CreateWindow failed (%lu)\n", GetLastError());
        return 1;
    }

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        fprintf(stderr, "FAIL: Direct3DCreate9 returned nullptr\n");
        return 1;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth        = 640;
    pp.BackBufferHeight       = 480;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow          = hwnd;
    pp.Windowed               = TRUE;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_DEFAULT;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                   &pp, &dev);
    if (FAILED(hr)) {
        fprintf(stderr, "FAIL: CreateDevice hr=0x%08lx\n", hr);
        d3d->Release();
        return 1;
    }

    printf("OK: device created\n");
    fflush(stdout);

    /* Cycle through red / green / blue, ~60 frames each (~3 seconds total) */
    static const D3DCOLOR colours[] = {
        D3DCOLOR_XRGB(220, 50, 50),
        D3DCOLOR_XRGB(50, 200, 50),
        D3DCOLOR_XRGB(50, 50, 220),
    };
    int frame = 0;

    MSG msg{};
    while (frame < 180) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) goto done;
        }

        dev->Clear(0, nullptr, D3DCLEAR_TARGET,
                   colours[frame / 60], 1.0f, 0);
        dev->BeginScene();
        dev->EndScene();
        hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            fprintf(stderr, "FAIL: Present hr=0x%08lx at frame %d\n", hr, frame);
            fflush(stderr);
            dev->Release();
            d3d->Release();
            return 1;
        }
        ++frame;
        if ((frame % 30) == 0) {
            printf("OK: reached frame %d\n", frame);
            fflush(stdout);
        }
    }

done:
    printf("OK: %d frames presented without error\n", frame);
    fflush(stdout);
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
