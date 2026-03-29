/* src/win32/entry.cpp — DllMain and Direct3DCreate9/9Ex Win32 entry points.
 *
 * This file is compiled by llvm-mingw as an ARM64 (or x86_64) Windows PE DLL
 * and imports dxmt9c_* symbols from dxmt9.dll (= libdxmt9.dylib installed in
 * the Wine prefix).  On Apple Silicon, PE and native code share the ARM64
 * address space, so cross-boundary calls work without thunks.
 *
 * The WinemetalApi table is populated by the actual Wine fork before this DLL
 * is loaded.  For standalone testing, stubbed function pointers are used. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d9.h>
#include "dxmt9/device_c.h"
#include "dxmt9/winemetal.h"

/* Forward declarations — implemented in factory.cpp / device.cpp */
IDirect3D9*   CreateFactoryImpl(D9CFactory* f);
IDirect3D9Ex* CreateFactoryExImpl(D9CFactory* f);

/* ── WinemetalApi stub ───────────────────────────────────────────────────────
 * A real Wine fork replaces these with real implementations that call into
 * Wine's window-management layer (HWND→NSView lookup, CAMetalLayer lifecycle,
 * shader compilation thunk).  For testing without Wine, all operations are
 * no-ops that return 0 / nullptr. */

static dxmt9_u64 stub_get_view(dxmt9_u64 hwnd) {
    return hwnd; /* pass-through so CreateDevice doesn't crash on addr=0 */
}
static dxmt9_u64 stub_create_layer(dxmt9_u64, dxmt9_u64,
                                    const WinemetalPresentParams*) { return 0; }
static void      stub_resize_layer(dxmt9_u64, dxmt9_u32, dxmt9_u32) {}
static void      stub_set_sync(dxmt9_u64, bool) {}
static void      stub_destroy_layer(dxmt9_u64) {}
static dxmt9_u64 stub_next_drawable(dxmt9_u64) { return 0; }
static void      stub_present_drawable(dxmt9_u64, dxmt9_u64) {}
static dxmt9_u64 stub_compile_shader(const WinemetalShaderCompileRequest*) { return 0; }
static const char* stub_shader_source(dxmt9_u64) { return nullptr; }
static dxmt9_u64 stub_shader_source_size(dxmt9_u64) { return 0; }
static void      stub_destroy_shader(dxmt9_u64) {}

static const WinemetalApi kStubApi = {
    stub_get_view,
    stub_create_layer,
    stub_resize_layer,
    stub_set_sync,
    stub_destroy_layer,
    stub_next_drawable,
    stub_present_drawable,
    stub_compile_shader,
    stub_shader_source,
    stub_shader_source_size,
    stub_destroy_shader,
};

/* ── DllMain ─────────────────────────────────────────────────────────────── */

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        /* Register stub bridge; Wine fork calls dxmt9_winemetal_set_api()
         * again with the real table after loading. */
        dxmt9_winemetal_set_api(&kStubApi);
    }
    return TRUE;
}

/* ── Win32 entry points ──────────────────────────────────────────────────── */

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT /*sdkVersion*/) {
    D9CFactory* f = dxmt9c_factory_create();
    if (!f) return nullptr;
    return CreateFactoryImpl(f);
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT /*sdkVersion*/,
                                             IDirect3D9Ex** ppD3D) {
    if (!ppD3D) return E_POINTER;
    D9CFactory* f = dxmt9c_factory_create();
    if (!f) return E_OUTOFMEMORY;
    *ppD3D = CreateFactoryExImpl(f);
    return S_OK;
}
