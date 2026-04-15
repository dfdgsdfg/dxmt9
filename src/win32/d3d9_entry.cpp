/* src/win32/d3d9_entry.cpp — user-facing d3d9.dll entry points.
 *
 * d3d9.dll is intentionally thin: it exposes Direct3DCreate9 / 9Ex and forwards
 * those calls into the internal dxmt9.dll PE bridge. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstring>

extern "C" __declspec(dllimport) IDirect3D9 *WINAPI dxmt9_pe_create9(UINT sdkVersion);
extern "C" __declspec(dllimport) HRESULT WINAPI dxmt9_pe_create9_ex(UINT sdkVersion,
                                                                    IDirect3D9Ex **ppD3D);

typedef HRESULT (WINAPI *shader_validator_cb)(const char *file, int line,
                                              DWORD_PTR arg3, DWORD_PTR message_id,
                                              const char *message, void *context);

typedef struct IDirect3DShaderValidator9 IDirect3DShaderValidator9;

typedef struct IDirect3DShaderValidator9Vtbl {
  HRESULT (WINAPI *QueryInterface)(IDirect3DShaderValidator9 *iface, REFIID iid, void **out);
  ULONG (WINAPI *AddRef)(IDirect3DShaderValidator9 *iface);
  ULONG (WINAPI *Release)(IDirect3DShaderValidator9 *iface);
  HRESULT (WINAPI *Begin)(IDirect3DShaderValidator9 *iface, shader_validator_cb callback,
                          void *context, DWORD_PTR arg3);
  HRESULT (WINAPI *Instruction)(IDirect3DShaderValidator9 *iface, const char *file, int line,
                                const DWORD *code, DWORD code_len);
  HRESULT (WINAPI *End)(IDirect3DShaderValidator9 *iface);
} IDirect3DShaderValidator9Vtbl;

struct IDirect3DShaderValidator9 {
  const IDirect3DShaderValidator9Vtbl *lpVtbl;
};

struct ShaderValidatorImpl {
  IDirect3DShaderValidator9 iface;
  std::atomic<ULONG> refs{1};
};

static HRESULT WINAPI shader_validator_query_interface(IDirect3DShaderValidator9 *iface,
                                                       REFIID iid, void **out) {
  if (!out) return E_POINTER;
  if (IsEqualGUID(iid, IID_IUnknown)) {
    *out = iface;
    auto *impl = reinterpret_cast<ShaderValidatorImpl *>(iface);
    return static_cast<HRESULT>(++impl->refs), S_OK;
  }
  *out = nullptr;
  return E_NOINTERFACE;
}

static ULONG WINAPI shader_validator_add_ref(IDirect3DShaderValidator9 *iface) {
  auto *impl = reinterpret_cast<ShaderValidatorImpl *>(iface);
  return ++impl->refs;
}

static ULONG WINAPI shader_validator_release(IDirect3DShaderValidator9 *iface) {
  auto *impl = reinterpret_cast<ShaderValidatorImpl *>(iface);
  const ULONG refs = --impl->refs;
  if (!refs) delete impl;
  return refs;
}

static HRESULT WINAPI shader_validator_begin(IDirect3DShaderValidator9 *, shader_validator_cb,
                                             void *, DWORD_PTR) {
  return S_OK;
}

static HRESULT WINAPI shader_validator_instruction(IDirect3DShaderValidator9 *,
                                                   const char *, int,
                                                   const DWORD *, DWORD) {
  return S_OK;
}

static HRESULT WINAPI shader_validator_end(IDirect3DShaderValidator9 *) {
  return S_OK;
}

static const IDirect3DShaderValidator9Vtbl shader_validator_vtbl = {
  shader_validator_query_interface,
  shader_validator_add_ref,
  shader_validator_release,
  shader_validator_begin,
  shader_validator_instruction,
  shader_validator_end,
};

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdkVersion) {
  return dxmt9_pe_create9(sdkVersion);
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex **ppD3D) {
  return dxmt9_pe_create9_ex(sdkVersion, ppD3D);
}

extern "C" IDirect3DShaderValidator9 *WINAPI Direct3DShaderValidatorCreate9(void) {
  auto *validator = new ShaderValidatorImpl{};
  validator->iface.lpVtbl = &shader_validator_vtbl;
  return &validator->iface;
}
