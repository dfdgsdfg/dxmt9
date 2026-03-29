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
 * Window/layer management is now handled directly inside libdxmt9.dylib via
 * macdrv_get_cocoa_view() — no callbacks needed here.
 *
 * The only optional override is compile_shader: a Wine build with the Apple
 * Metal shader converter can call dxmt9_winemetal_set_api() with a real
 * implementation.  For standalone use all shader functions are null so
 * libdxmt9.dylib falls back to its built-in D3DBC→MSL translator. */
static const WinemetalApi kStubApi = {
    nullptr, /* compile_shader  — use built-in translator */
    nullptr, /* shader_source   — unused without compile_shader */
    nullptr, /* shader_source_size */
    nullptr, /* destroy_shader */
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
