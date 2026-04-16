#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <atomic>

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

namespace {

struct ShaderValidatorImpl {
  IDirect3DShaderValidator9 iface;
  std::atomic<ULONG> refs{1};
};

HRESULT WINAPI shader_validator_query_interface(IDirect3DShaderValidator9 *iface,
                                                REFIID iid, void **out) {
  if (!out) return E_POINTER;
  if (IsEqualGUID(iid, IID_IUnknown)) {
    *out = iface;
    auto *impl = reinterpret_cast<ShaderValidatorImpl *>(iface);
    ++impl->refs;
    return S_OK;
  }
  *out = nullptr;
  return E_NOINTERFACE;
}

ULONG WINAPI shader_validator_add_ref(IDirect3DShaderValidator9 *iface) {
  auto *impl = reinterpret_cast<ShaderValidatorImpl *>(iface);
  return ++impl->refs;
}

ULONG WINAPI shader_validator_release(IDirect3DShaderValidator9 *iface) {
  auto *impl = reinterpret_cast<ShaderValidatorImpl *>(iface);
  const ULONG refs = --impl->refs;
  if (!refs) delete impl;
  return refs;
}

HRESULT WINAPI shader_validator_begin(IDirect3DShaderValidator9 *, shader_validator_cb,
                                      void *, DWORD_PTR) {
  return S_OK;
}

HRESULT WINAPI shader_validator_instruction(IDirect3DShaderValidator9 *,
                                            const char *, int,
                                            const DWORD *, DWORD) {
  return S_OK;
}

HRESULT WINAPI shader_validator_end(IDirect3DShaderValidator9 *) {
  return S_OK;
}

const IDirect3DShaderValidator9Vtbl shader_validator_vtbl = {
  shader_validator_query_interface,
  shader_validator_add_ref,
  shader_validator_release,
  shader_validator_begin,
  shader_validator_instruction,
  shader_validator_end,
};

}  // namespace

extern "C" IDirect3DShaderValidator9 *WINAPI
dxmt9_pe_create_shader_validator(void) {
  auto *validator = new ShaderValidatorImpl{};
  validator->iface.lpVtbl = &shader_validator_vtbl;
  return &validator->iface;
}
